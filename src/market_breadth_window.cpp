#include "market_breadth_window.h"

#include "app_constants.h"
#include "i18n.h"
#include "quote_provider.h"
#include "timeline_popup.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHash>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QStringList>
#include <QTimer>
#include <QTimeZone>
#include <QWheelEvent>

#include <cmath>
#include <functional>
#include <utility>

#ifdef WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace {

QString formatChineseMarketAmount(double value) {
    if (!std::isfinite(value)) {
        return QStringLiteral("--");
    }

    const double absValue = std::abs(value);
    constexpr double kYi = 100000000.0;
    constexpr double kWanYi = 1000000000000.0;
    if (absValue >= kWanYi) {
        return QStringLiteral("%1万亿").arg(QString::number(absValue / kWanYi, 'f', 2));
    }

    const double yiValue = absValue / kYi;
    const int precision = yiValue >= 100.0 ? 0 : 2;
    return QStringLiteral("%1亿").arg(QString::number(yiValue, 'f', precision));
}

double estimateAshareFullDayTurnover(const MarketBreadthSnapshot& snapshot) {
    if (!std::isfinite(snapshot.turnover) || snapshot.turnover <= 0.0) {
        return qQNaN();
    }

    if (snapshot.overviewTimeline.isEmpty()) {
        return snapshot.turnover;
    }

    const qint64 timestampMs = snapshot.overviewTimeline.last().timestampMs;
    if (timestampMs <= 0) {
        return snapshot.turnover;
    }

    const QTimeZone bjZone("Asia/Shanghai");
    QDateTime sampleTs = QDateTime::fromMSecsSinceEpoch(timestampMs, bjZone);
    if (!sampleTs.isValid()) {
        sampleTs = QDateTime::fromMSecsSinceEpoch(timestampMs);
    }
    if (!sampleTs.isValid()) {
        return snapshot.turnover;
    }

    const QTime now = sampleTs.time();
    const QTime amOpen(9, 30);
    const QTime amClose(11, 30);
    const QTime pmOpen(13, 0);
    const QTime pmClose(15, 0);
    constexpr int kFullMinutes = 240;

    int elapsedMinutes = 0;
    if (now <= amOpen) {
        elapsedMinutes = 0;
    } else if (now <= amClose) {
        elapsedMinutes = amOpen.secsTo(now) / 60;
    } else if (now <= pmOpen) {
        elapsedMinutes = 120;
    } else if (now <= pmClose) {
        elapsedMinutes = 120 + (pmOpen.secsTo(now) / 60);
    } else {
        elapsedMinutes = kFullMinutes;
    }

    const double progress = qBound(0.06, static_cast<double>(elapsedMinutes) / kFullMinutes, 1.0);
    const double estimate = snapshot.turnover / progress;
    return qMax(snapshot.turnover, estimate);
}

QString marketBreadthLastUpdatedText(const MarketBreadthSnapshot& snapshot, const QString& language) {
    const QTimeZone bjZone("Asia/Shanghai");
    const qint64 timestampMs = snapshot.lastUpdatedAtMs;
    if (timestampMs <= 0) {
        return i18n::t("quote.noData", language);
    }

    QDateTime sampleTs = QDateTime::fromMSecsSinceEpoch(timestampMs, bjZone);
    if (!sampleTs.isValid()) {
        sampleTs = QDateTime::fromMSecsSinceEpoch(timestampMs);
    }
    if (!sampleTs.isValid()) {
        return i18n::t("quote.noData", language);
    }

    QDateTime nowTs = QDateTime::currentDateTimeUtc().toTimeZone(bjZone);
    if (!nowTs.isValid()) {
        nowTs = QDateTime::currentDateTime();
    }
    const QString exactFormat = (sampleTs.date() == nowTs.date())
        ? QStringLiteral("HH:mm:ss")
        : QStringLiteral("MM-dd HH:mm:ss");
    return sampleTs.toString(exactFormat);
}

QString marketBreadthTurnoverChangeText(double value, const QString& language) {
    if (!std::isfinite(value)) {
        return i18n::t("quote.noData", language);
    }
    if (value > 0.0) {
        return i18n::t("popup.marketBreadth.expand", language);
    }
    if (value < 0.0) {
        return i18n::t("popup.marketBreadth.shrink", language);
    }
    return i18n::t("popup.marketBreadth.same", language);
}

QColor marketBreadthTurnoverChangeColor(double value, const AppConfig& cfg) {
    if (!std::isfinite(value)) {
        return cfg.textColor;
    }
    if (value > 0.0) {
        return cfg.upColor;
    }
    if (value < 0.0) {
        return cfg.downColor;
    }
    return cfg.flatColor;
}

QFont effectiveFloatingWindowFont(const AppConfig& cfg, const QFont& baseFont) {
    QFont font(baseFont);

    const QString family = cfg.floatingWindowFontFamily.trimmed();
    if (!family.isEmpty()) {
        font.setFamily(family);
    } else {
        const QString defaultFamily = defaultFloatingWindowFontFamily();
        if (!defaultFamily.isEmpty()) {
            font.setFamily(defaultFamily);
        }
    }
    if (cfg.floatingWindowFontSize > 0) {
        font.setPointSize(cfg.floatingWindowFontSize);
    }
    font.setBold(cfg.floatingWindowFontBold);

    return font;
}

#if defined(Q_OS_MACOS)
static void* macWindowHandleForWidget(const QWidget* widget) {
    if (!widget) {
        return nullptr;
    }
    void* nsView = reinterpret_cast<void*>(widget->winId());
    if (!nsView) {
        return nullptr;
    }
    auto sendObjectMessage = reinterpret_cast<void* (*)(void*, SEL)>(objc_msgSend);
    return sendObjectMessage(nsView, sel_registerName("window"));
}

// Make the window appear in Mission Control / Exposé.
// NSWindowCollectionBehaviorManaged (4) | NSWindowCollectionBehaviorMoveToActiveSpace (8)
// | NSWindowCollectionBehaviorParticipatesInCycle (32)
static void setMacWindowCollectionBehaviorManaged(const QWidget* widget) {
    void* nsWindow = macWindowHandleForWidget(widget);
    if (!nsWindow) {
        return;
    }
    constexpr unsigned long kBehavior = 4UL | 8UL | 32UL;
    auto sendULMessage = reinterpret_cast<void (*)(void*, SEL, unsigned long)>(objc_msgSend);
    sendULMessage(nsWindow, sel_registerName("setCollectionBehavior:"), kBehavior);
}

// Switch NSApplication activation policy so the app appears / disappears in the Dock.
// NSApplicationActivationPolicyRegular   = 0
// NSApplicationActivationPolicyAccessory = 1
static void setMacAppActivationPolicy(bool regular) {
    auto getShared = reinterpret_cast<void* (*)(void*, SEL)>(objc_msgSend);
    void* nsApp = getShared(
        reinterpret_cast<void*>(objc_getClass("NSApplication")),
        sel_registerName("sharedApplication")
    );
    if (!nsApp) {
        return;
    }
    auto setPolicy = reinterpret_cast<void (*)(void*, SEL, long)>(objc_msgSend);
    setPolicy(nsApp, sel_registerName("setActivationPolicy:"), regular ? 0L : 1L);
    if (regular) {
        auto activate = reinterpret_cast<void (*)(void*, SEL, BOOL)>(objc_msgSend);
        activate(nsApp, sel_registerName("activateIgnoringOtherApps:"), YES);
    }
}
#endif

inline constexpr int kPopupScreenMarginPx = 12;
inline constexpr int kMarketBreadthResizeHandleSizePx = 22;

QSize marketBreadthMinimumPopupSize() {
    return QSize(kMarketBreadthPopupDefaultWidthPx, kMarketBreadthPopupDefaultHeightPx);
}

qreal marketBreadthPopupAspectRatio() {
    return static_cast<qreal>(kMarketBreadthPopupDefaultWidthPx)
        / static_cast<qreal>(kMarketBreadthPopupDefaultHeightPx);
}

QSize normalizedMarketBreadthPopupSize(const QSize& requestedSize, const QSize& referenceSize) {
    const QSize minSize = marketBreadthMinimumPopupSize();
    const qreal aspectRatio = marketBreadthPopupAspectRatio();

    const QSize safeReference = referenceSize.isValid() ? referenceSize : minSize;
    const int requestedWidth = qMax(minSize.width(), requestedSize.width());
    const int requestedHeight = qMax(minSize.height(), requestedSize.height());
    const qreal widthScale = static_cast<qreal>(requestedWidth)
        / static_cast<qreal>(qMax(1, safeReference.width()));
    const qreal heightScale = static_cast<qreal>(requestedHeight)
        / static_cast<qreal>(qMax(1, safeReference.height()));

    int normalizedWidth = requestedWidth;
    int normalizedHeight = requestedHeight;
    if (std::fabs(widthScale - 1.0) >= std::fabs(heightScale - 1.0)) {
        normalizedHeight = qRound(static_cast<qreal>(normalizedWidth) / aspectRatio);
    } else {
        normalizedWidth = qRound(static_cast<qreal>(normalizedHeight) * aspectRatio);
    }

    if (normalizedWidth < minSize.width()) {
        normalizedWidth = minSize.width();
        normalizedHeight = qRound(static_cast<qreal>(normalizedWidth) / aspectRatio);
    }
    if (normalizedHeight < minSize.height()) {
        normalizedHeight = minSize.height();
        normalizedWidth = qRound(static_cast<qreal>(normalizedHeight) * aspectRatio);
    }

    return QSize(normalizedWidth, normalizedHeight);
}

QRect adjustedPopupRectForScreen(const QRect& requestedRect, const QRect& screenRect) {
    QRect adjusted = requestedRect;
    adjusted.setSize(normalizedMarketBreadthPopupSize(adjusted.size(), adjusted.size()));
    if (!screenRect.isValid()) {
        return adjusted;
    }

    adjusted.setWidth(qMin(adjusted.width(), screenRect.width()));
    adjusted.setHeight(qMin(adjusted.height(), screenRect.height()));

    if (adjusted.left() < screenRect.left() + kPopupScreenMarginPx) {
        adjusted.moveLeft(screenRect.left() + kPopupScreenMarginPx);
    }
    if (adjusted.top() < screenRect.top() + kPopupScreenMarginPx) {
        adjusted.moveTop(screenRect.top() + kPopupScreenMarginPx);
    }
    if (adjusted.right() > screenRect.right() - kPopupScreenMarginPx) {
        adjusted.moveRight(screenRect.right() - kPopupScreenMarginPx);
    }
    if (adjusted.bottom() > screenRect.bottom() - kPopupScreenMarginPx) {
        adjusted.moveBottom(screenRect.bottom() - kPopupScreenMarginPx);
    }
    if (adjusted.left() < screenRect.left() + kPopupScreenMarginPx) {
        adjusted.moveLeft(screenRect.left() + kPopupScreenMarginPx);
    }
    if (adjusted.top() < screenRect.top() + kPopupScreenMarginPx) {
        adjusted.moveTop(screenRect.top() + kPopupScreenMarginPx);
    }

    return adjusted;
}

const QStringList& hardcodedAshareIntradayXAxis() {
    static const QStringList labels = []() {
        QStringList out;
        out.reserve(241);
        QTime t(9, 30);
        const QTime morningEnd(11, 30);
        while (t <= morningEnd) {
            out.push_back(t.toString(QStringLiteral("HH:mm")));
            t = t.addSecs(60);
        }
        t = QTime(13, 1);
        const QTime afternoonEnd(15, 0);
        while (t <= afternoonEnd) {
            out.push_back(t.toString(QStringLiteral("HH:mm")));
            t = t.addSecs(60);
        }
        return out;
    }();
    return labels;
}

const QHash<QString, int>& hardcodedAshareIntradayXAxisIndex() {
    static const QHash<QString, int> index = []() {
        QHash<QString, int> map;
        const QStringList& labels = hardcodedAshareIntradayXAxis();
        map.reserve(labels.size());
        for (int i = 0; i < labels.size(); ++i) {
            map.insert(labels.at(i), i);
        }
        return map;
    }();
    return index;
}

QRect availableScreenRectForReference(const QRect& referenceRect) {
    QRect screenRect;
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (QScreen* screen : screens) {
        if (!screen) {
            continue;
        }
        const QRect available = screen->availableGeometry();
        if (referenceRect.isValid() && available.contains(referenceRect.center())) {
            screenRect = available;
            break;
        }
    }

    if (!screenRect.isValid()) {
        if (QScreen* screen = QGuiApplication::screenAt(QCursor::pos())) {
            screenRect = screen->availableGeometry();
        }
    }
    if (!screenRect.isValid()) {
        if (QScreen* screen = QGuiApplication::primaryScreen()) {
            screenRect = screen->availableGeometry();
        }
    }
    return screenRect;
}

} // namespace

class HotRankConstituentDetailWindow : public QWidget {
public:
    explicit HotRankConstituentDetailWindow(QWidget* parentWindow)
        : QWidget(parentWindow) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setMouseTracking(true);
        setMinimumSize(QSize(650, 500));
        resize(minimumSize());

        m_refreshFeedbackTimer = new QTimer(this);
        m_refreshFeedbackTimer->setInterval(33);
        connect(m_refreshFeedbackTimer, &QTimer::timeout, this, [this]() {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (nowMs >= m_refreshFeedbackUntilMs) {
                m_refreshFeedbackStartedMs = 0;
                m_refreshFeedbackUntilMs = 0;
                m_refreshFeedbackTimer->stop();
            }

            if (m_refreshButtonRect.isValid()) {
                update(m_refreshButtonRect.adjusted(-3, -3, 3, 3));
            } else {
                update();
            }
        });
    }

    void applyConfig(const AppConfig& cfg) {
        m_cfg = cfg;
        ensureProvider();
        if (m_provider) {
            m_provider->applyConfig(m_cfg);
        }
        if (m_timelinePopup) {
            m_timelinePopup->applyConfig(m_cfg);
        }
        setFont(effectiveFloatingWindowFont(cfg, font()));
        update();
    }

    void setLanguage(const QString& language) {
        m_language = i18n::resolveLanguage(language);
        update();
    }

    void setWatchlistCallbacks(
        std::function<bool(const QString&)> containsCallback,
        std::function<bool(const QString&, const QString&, bool)> mutateCallback,
        std::function<void()> reloadCallback
    ) {
        m_containsWatchlistCallback = std::move(containsCallback);
        m_mutateWatchlistCallback = std::move(mutateCallback);
        m_reloadWatchlistCallback = std::move(reloadCallback);
        update();
    }

    void showForItem(const HotRankItem& item, const QRect& referenceRect) {
        const QString fs = !item.detailFs.trimmed().isEmpty()
            ? item.detailFs.trimmed()
            : (item.code.trimmed().isEmpty()
                ? QString()
                : QStringLiteral("b:%1").arg(item.code.trimmed()));
        if (fs.isEmpty()) {
            return;
        }

        ensureProvider();
        m_selectedItem = item;
        m_currentFs = fs;
        m_items.clear();
        m_displayItems.clear();
        m_lastError.clear();
        m_loading = true;
        m_scrollIndex = 0;
        m_closeButtonHovered = false;
        m_closeButtonPressed = false;
        m_refreshButtonHovered = false;
        m_refreshButtonPressed = false;
        m_hoveredHeader = 0;
        m_pressedHeader = 0;
        m_hoveredActionRow = -1;
        m_pressedActionRow = -1;
        m_pressedDataRow = -1;
        m_dragging = false;
        m_actionButtonRects.clear();
        m_dataRowHitAreas.clear();
        m_refreshFeedbackStartedMs = 0;
        m_refreshFeedbackUntilMs = 0;
        if (m_refreshFeedbackTimer) {
            m_refreshFeedbackTimer->stop();
        }
        hideTimelinePopup();
        unsetCursor();

        const QRect screenRect = availableScreenRectForReference(referenceRect);
        QRect targetRect;
        if (m_hasStoredGeometry && geometry().isValid()) {
            targetRect = geometry();
        } else {
            const QSize windowSize = minimumSize();
            targetRect = QRect(QPoint(0, 0), windowSize);
            if (referenceRect.isValid()) {
                targetRect.moveCenter(referenceRect.center());
            } else if (screenRect.isValid()) {
                targetRect.moveCenter(screenRect.center());
            }
        }
        if (screenRect.isValid()) {
            targetRect.setWidth(qMin(targetRect.width(), screenRect.width()));
            targetRect.setHeight(qMin(targetRect.height(), screenRect.height()));
            if (targetRect.left() < screenRect.left() + kPopupScreenMarginPx) {
                targetRect.moveLeft(screenRect.left() + kPopupScreenMarginPx);
            }
            if (targetRect.top() < screenRect.top() + kPopupScreenMarginPx) {
                targetRect.moveTop(screenRect.top() + kPopupScreenMarginPx);
            }
            if (targetRect.right() > screenRect.right() - kPopupScreenMarginPx) {
                targetRect.moveRight(screenRect.right() - kPopupScreenMarginPx);
            }
            if (targetRect.bottom() > screenRect.bottom() - kPopupScreenMarginPx) {
                targetRect.moveBottom(screenRect.bottom() - kPopupScreenMarginPx);
            }
        }

        setGeometry(targetRect);
        m_hasStoredGeometry = true;
        if (!isVisible()) {
            show();
        }
#if defined(Q_OS_MACOS)
        setMacWindowCollectionBehaviorManaged(this);
#endif
        requestData();
        update();
    }

    void hidePopup() {
        flushPendingWatchlistReloadIfNeeded();
        hideTimelinePopup();
        m_closeButtonHovered = false;
        m_closeButtonPressed = false;
        m_refreshButtonHovered = false;
        m_refreshButtonPressed = false;
        m_hoveredHeader = 0;
        m_pressedHeader = 0;
        m_hoveredActionRow = -1;
        m_pressedActionRow = -1;
        m_dragging = false;
        m_actionButtonRects.clear();
        m_refreshFeedbackStartedMs = 0;
        m_refreshFeedbackUntilMs = 0;
        if (m_refreshFeedbackTimer) {
            m_refreshFeedbackTimer->stop();
        }
        unsetCursor();
        hide();
    }

protected:
    void hideEvent(QHideEvent* event) override {
        flushPendingWatchlistReloadIfNeeded();
        hideTimelinePopup();
        QWidget::hideEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!event) {
            QWidget::mouseMoveEvent(event);
            return;
        }

        if (m_dragging && (event->buttons() & Qt::LeftButton)) {
            hideTimelinePopup();
            move(event->globalPosition().toPoint() - m_dragOffset);
            event->accept();
            return;
        }

        const bool closeHovered = m_closeButtonRect.contains(event->pos());
        const bool refreshHovered = m_refreshButtonRect.contains(event->pos());
        const int hoveredHeader = headerAt(event->pos());
        const int hoveredActionRow = actionRowAt(event->pos());
        const int previousHoveredHeader = m_hoveredHeader;
        const int previousHoveredActionRow = m_hoveredActionRow;

        if (closeHovered != m_closeButtonHovered
            || refreshHovered != m_refreshButtonHovered
            || hoveredHeader != m_hoveredHeader
            || hoveredActionRow != m_hoveredActionRow) {
            m_closeButtonHovered = closeHovered;
            m_refreshButtonHovered = refreshHovered;
            m_hoveredHeader = hoveredHeader;
            m_hoveredActionRow = hoveredActionRow;
            if (m_closeButtonRect.isValid()) {
                update(m_closeButtonRect.adjusted(-2, -2, 2, 2));
            }
            if (m_refreshButtonRect.isValid()) {
                update(m_refreshButtonRect.adjusted(-2, -2, 2, 2));
            }
            updateHeaderByIndex(previousHoveredHeader);
            updateHeaderByIndex(m_hoveredHeader);
            updateActionRowByIndex(previousHoveredActionRow);
            updateActionRowByIndex(m_hoveredActionRow);
        }

        const int hoveredDataRow = dataRowAt(event->pos());
        if (closeHovered || refreshHovered || hoveredHeader != 0 || hoveredActionRow >= 0 || hoveredDataRow >= 0) {
            setCursor(Qt::PointingHandCursor);
        } else if (m_dragHandleRect.contains(event->pos())) {
            setCursor(Qt::OpenHandCursor);
        } else {
            unsetCursor();
        }
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        if (m_closeButtonHovered || m_closeButtonPressed
            || m_refreshButtonHovered || m_refreshButtonPressed
            || m_hoveredHeader != 0 || m_pressedHeader != 0
            || m_hoveredActionRow >= 0 || m_pressedActionRow >= 0) {
            const int previousHoveredHeader = m_hoveredHeader;
            const int previousPressedHeader = m_pressedHeader;
            const int previousHoveredActionRow = m_hoveredActionRow;
            const int previousPressedActionRow = m_pressedActionRow;
            m_closeButtonHovered = false;
            m_closeButtonPressed = false;
            m_refreshButtonHovered = false;
            m_refreshButtonPressed = false;
            m_hoveredHeader = 0;
            m_pressedHeader = 0;
            m_hoveredActionRow = -1;
            m_pressedActionRow = -1;
            m_pressedDataRow = -1;
            if (m_closeButtonRect.isValid()) {
                update(m_closeButtonRect.adjusted(-2, -2, 2, 2));
            }
            if (m_refreshButtonRect.isValid()) {
                update(m_refreshButtonRect.adjusted(-2, -2, 2, 2));
            }
            updateHeaderByIndex(previousHoveredHeader);
            updateHeaderByIndex(previousPressedHeader);
            updateActionRowByIndex(previousHoveredActionRow);
            updateActionRowByIndex(previousPressedActionRow);
        }
        unsetCursor();
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (!event) {
            QWidget::mousePressEvent(event);
            return;
        }

        if (event->button() == Qt::LeftButton && m_closeButtonRect.contains(event->pos())) {
            m_closeButtonPressed = true;
            m_closeButtonHovered = true;
            update(m_closeButtonRect.adjusted(-2, -2, 2, 2));
            event->accept();
            return;
        }

        if (event->button() == Qt::LeftButton && m_refreshButtonRect.contains(event->pos())) {
            m_refreshButtonPressed = true;
            m_refreshButtonHovered = true;
            update(m_refreshButtonRect.adjusted(-2, -2, 2, 2));
            event->accept();
            return;
        }

        const int pressedHeader = headerAt(event->pos());
        if (event->button() == Qt::LeftButton && pressedHeader != 0) {
            m_pressedHeader = pressedHeader;
            updateHeaderByIndex(pressedHeader);
            event->accept();
            return;
        }

        const int pressedActionRow = actionRowAt(event->pos());
        if (event->button() == Qt::LeftButton && pressedActionRow >= 0) {
            m_pressedActionRow = pressedActionRow;
            updateActionRowByIndex(pressedActionRow);
            event->accept();
            return;
        }

        const int pressedDataRow = dataRowAt(event->pos());
        if (event->button() == Qt::LeftButton && pressedDataRow >= 0) {
            m_pressedDataRow = pressedDataRow;
            event->accept();
            return;
        }

        if (event->button() == Qt::LeftButton && m_dragHandleRect.contains(event->pos())) {
            m_dragging = true;
            m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }

        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!event) {
            QWidget::mouseReleaseEvent(event);
            return;
        }

        if (event->button() == Qt::LeftButton && m_dragging) {
            m_dragging = false;
            if (m_dragHandleRect.contains(event->pos())) {
                setCursor(Qt::OpenHandCursor);
            } else {
                unsetCursor();
            }
            event->accept();
            return;
        }

        if (event->button() == Qt::LeftButton) {
            const bool shouldClose = m_closeButtonPressed && m_closeButtonRect.contains(event->pos());
            const bool shouldRefresh = m_refreshButtonPressed && m_refreshButtonRect.contains(event->pos());
            const int releasedHeader = headerAt(event->pos());
            const bool shouldSort = m_pressedHeader != 0 && m_pressedHeader == releasedHeader;
            const int previousPressedHeader = m_pressedHeader;
            const int releasedActionRow = actionRowAt(event->pos());
            const bool shouldToggleAction =
                m_pressedActionRow >= 0 && m_pressedActionRow == releasedActionRow;
            const int previousPressedActionRow = m_pressedActionRow;
            const int releasedDataRow = dataRowAt(event->pos());
            const bool shouldShowTimeline =
                m_pressedDataRow >= 0 && m_pressedDataRow == releasedDataRow;
            m_closeButtonPressed = false;
            m_closeButtonHovered = m_closeButtonRect.contains(event->pos());
            m_refreshButtonPressed = false;
            m_refreshButtonHovered = m_refreshButtonRect.contains(event->pos());
            m_pressedHeader = 0;
            m_pressedActionRow = -1;
            m_pressedDataRow = -1;
            if (m_closeButtonRect.isValid()) {
                update(m_closeButtonRect.adjusted(-2, -2, 2, 2));
            }
            if (m_refreshButtonRect.isValid()) {
                update(m_refreshButtonRect.adjusted(-2, -2, 2, 2));
            }
            updateHeaderByIndex(previousPressedHeader);
            updateActionRowByIndex(previousPressedActionRow);
            if (shouldClose) {
                hidePopup();
                event->accept();
                return;
            }
            if (shouldRefresh) {
                startRefreshFeedback();
                requestData(true);
                event->accept();
                return;
            }
            if (shouldSort) {
                hideTimelinePopup();
                applySortByHeaderIndex(releasedHeader);
                event->accept();
                return;
            }
            if (shouldToggleAction) {
                hideTimelinePopup();
                toggleWatchlistAction(releasedActionRow);
                event->accept();
                return;
            }
            if (shouldShowTimeline) {
                showTimelinePopupForRow(releasedDataRow);
                event->accept();
                return;
            }
            hideTimelinePopup();
        }

        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (!event) {
            QWidget::mouseDoubleClickEvent(event);
            return;
        }

        if (event->button() == Qt::LeftButton) {
            event->accept();
            return;
        }

        QWidget::mouseDoubleClickEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override {
        if (!event || !m_viewportRect.contains(event->position().toPoint())) {
            QWidget::wheelEvent(event);
            return;
        }

        if (m_items.isEmpty()) {
            event->accept();
            return;
        }

        constexpr int kRowHeight = 24;
        constexpr int kRowGap = 2;
        const int visibleRows = qMax(
            1,
            (m_viewportRect.height() + kRowGap) / (kRowHeight + kRowGap)
        );
        const int maxScrollIndex = qMax(0, m_items.size() - visibleRows);

        const int deltaY = event->angleDelta().y();
        int step = 0;
        if (deltaY < 0) {
            step = 3;
        } else if (deltaY > 0) {
            step = -3;
        } else if (event->pixelDelta().y() < 0) {
            step = 3;
        } else if (event->pixelDelta().y() > 0) {
            step = -3;
        }

        if (step != 0) {
            const int nextScrollIndex = qBound(0, m_scrollIndex + step, maxScrollIndex);
            if (nextScrollIndex != m_scrollIndex) {
                m_scrollIndex = nextScrollIndex;
                hideTimelinePopup();
                update();
            }
        }

        event->accept();
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);

        QColor panelBackground = m_cfg.transparentBackgroundEnabled
            ? QColor(18, 18, 18, 236)
            : m_cfg.bgColor;
        panelBackground.setAlpha(qMax(panelBackground.alpha(), 232));
        const QColor textColor = m_cfg.textColor;

        painter.setBrush(panelBackground);
        painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 12, 12);

        const QRect content = rect().adjusted(18, 16, -18, -16);
        const QRect topHeaderRect(content.left(), content.top(), content.width(), 28);

        QFont titleFont = font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(qMax(10.5, titleFont.pointSizeF() + 1.4));
        QFont bodyFont = font();
        bodyFont.setBold(false);
        bodyFont.setPointSizeF(qMax(9.2, bodyFont.pointSizeF()));
        QFont headerFont = bodyFont;
        headerFont.setBold(true);

        const auto formatPct = [](double value) {
            if (!std::isfinite(value)) {
                return QStringLiteral("--");
            }
            QString text = QString::number(value, 'f', 2);
            if (value > 0.0) {
                text.prepend('+');
            }
            text.append('%');
            return text;
        };
        const auto formatPrice = [](double value) {
            if (!std::isfinite(value)) {
                return QStringLiteral("--");
            }
            return QString::number(value, 'f', 2);
        };

        const QColor titlePctColor = std::isfinite(m_selectedItem.pct)
            ? (m_selectedItem.pct > 0.0 ? m_cfg.upColor
               : (m_selectedItem.pct < 0.0 ? m_cfg.downColor : m_cfg.flatColor))
            : textColor;
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
        const int closeButtonDiameter = 12;
        m_closeButtonRect = QRect(
            topHeaderRect.left() + 8,
            topHeaderRect.center().y() - closeButtonDiameter / 2,
            closeButtonDiameter,
            closeButtonDiameter
        );
#else
        const QSize closeButtonSize(26, 18);
        m_closeButtonRect = QRect(
            topHeaderRect.right() - closeButtonSize.width() - 4,
            topHeaderRect.center().y() - closeButtonSize.height() / 2,
            closeButtonSize.width(),
            closeButtonSize.height()
        );
#endif
        const int refreshButtonSize = 20;
        QRect titleRect;
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
        m_refreshButtonRect = QRect(
            topHeaderRect.right() - refreshButtonSize,
            topHeaderRect.center().y() - refreshButtonSize / 2,
            refreshButtonSize,
            refreshButtonSize
        );
        titleRect = QRect(
            m_closeButtonRect.right() + 12,
            topHeaderRect.top(),
            qMax(80, m_refreshButtonRect.left() - m_closeButtonRect.right() - 24),
            topHeaderRect.height()
        );
#else
        m_refreshButtonRect = QRect(
            m_closeButtonRect.left() - 6 - refreshButtonSize,
            topHeaderRect.center().y() - refreshButtonSize / 2,
            refreshButtonSize,
            refreshButtonSize
        );
        titleRect = QRect(
            topHeaderRect.left() + 8,
            topHeaderRect.top(),
            qMax(80, m_refreshButtonRect.left() - topHeaderRect.left() - 14),
            topHeaderRect.height()
        );
#endif
        m_dragHandleRect = titleRect;
        const QString titlePctText = formatPct(m_selectedItem.pct);
        const QFontMetrics titleMetrics(titleFont);
        const int titlePctWidth = titleMetrics.horizontalAdvance(titlePctText);
        const int titleGap = 6;
        const int titleNameWidth = qMax(30, titleRect.width() - titlePctWidth - titleGap);
        const QString titleNameText = titleMetrics.elidedText(
            m_selectedItem.name.trimmed(),
            Qt::ElideRight,
            titleNameWidth
        );
        const int drawnTitleNameWidth = qMin(
            titleNameWidth,
            qMax(0, titleMetrics.horizontalAdvance(titleNameText))
        );

        painter.setFont(titleFont);
        painter.setPen(textColor);
        painter.drawText(
            QRect(titleRect.left(), titleRect.top(), titleNameWidth, titleRect.height()),
            Qt::AlignVCenter | Qt::AlignLeft,
            titleNameText
        );
        painter.setPen(titlePctColor);
        painter.drawText(
            QRect(
                titleRect.left() + drawnTitleNameWidth + titleGap,
                titleRect.top(),
                qMax(20, titleRect.right() - (titleRect.left() + drawnTitleNameWidth + titleGap) + 1),
                titleRect.height()
            ),
            Qt::AlignVCenter | Qt::AlignLeft,
            titlePctText
        );

        if (m_refreshButtonRect.isValid()) {
            qint64 nowMs = 0;
            const bool refreshAnimating = isRefreshFeedbackActive(&nowMs);
            const qint64 durationMs = m_refreshFeedbackUntilMs > m_refreshFeedbackStartedMs
                ? (m_refreshFeedbackUntilMs - m_refreshFeedbackStartedMs)
                : 1;
            const qreal refreshAnimProgress = refreshAnimating
                ? qBound(
                    0.0,
                    static_cast<double>(nowMs - m_refreshFeedbackStartedMs) / static_cast<double>(durationMs),
                    1.0
                )
                : 1.0;
            QColor refreshIconColor(
                textColor.red(),
                textColor.green(),
                textColor.blue(),
                refreshAnimating ? 234 : 208
            );
            if (m_refreshButtonHovered) {
                refreshIconColor = QColor(textColor.red(), textColor.green(), textColor.blue(), 236);
            }
            if (m_refreshButtonPressed) {
                refreshIconColor = QColor(textColor.red(), textColor.green(), textColor.blue(), 250);
            }

            if (m_refreshButtonHovered || m_refreshButtonPressed || refreshAnimating) {
                const QColor refreshBg(
                    textColor.red(),
                    textColor.green(),
                    textColor.blue(),
                    m_refreshButtonPressed ? 66 : (refreshAnimating ? 58 : 40)
                );
                painter.setPen(QPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 96), 1.0));
                painter.setBrush(refreshBg);
                painter.drawRoundedRect(m_refreshButtonRect.adjusted(0, 0, -1, -1), 5, 5);
            }

            painter.setBrush(Qt::NoBrush);
            QPen refreshPen(refreshIconColor, 1.35);
            refreshPen.setCapStyle(Qt::RoundCap);
            refreshPen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(refreshPen);

            const QPoint center = m_refreshButtonRect.center();
            const int radius = qMax(4, (qMin(m_refreshButtonRect.width(), m_refreshButtonRect.height()) / 2) - 5);
            const QRect arcRect(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
            constexpr qreal kRefreshArcStartDeg = 38.0;
            constexpr qreal kRefreshArcSpanDeg = 286.0;
            const int rotateDegrees = refreshAnimating
                ? qRound(refreshAnimProgress * 320.0)
                : 0;
            painter.save();
            if (rotateDegrees != 0) {
                painter.translate(center);
                painter.rotate(rotateDegrees);
                painter.translate(-center);
            }
            painter.drawArc(
                arcRect,
                qRound(kRefreshArcStartDeg * 16.0),
                qRound(kRefreshArcSpanDeg * 16.0)
            );

            const auto pointAtAngle = [&](qreal distance, qreal angleDeg) {
                constexpr qreal kPi = 3.14159265358979323846;
                const qreal radians = angleDeg * kPi / 180.0;
                return QPointF(
                    center.x() + distance * std::cos(radians),
                    center.y() - distance * std::sin(radians)
                );
            };

            const qreal arrowTipAngle = kRefreshArcStartDeg;
            const QPointF arrowTip = pointAtAngle(static_cast<qreal>(radius), arrowTipAngle);
            const qreal arrowDirDeg = arrowTipAngle - 90.0;
            constexpr qreal kPi2 = 3.14159265358979323846;
            const qreal armLen = qMax<qreal>(3.0, static_cast<qreal>(radius) * 0.52);
            const qreal backDeg = arrowDirDeg + 180.0;
            constexpr qreal arrowSpreadDeg = 28.0;
            const qreal wA_rad = (backDeg + arrowSpreadDeg) * kPi2 / 180.0;
            const qreal wB_rad = (backDeg - arrowSpreadDeg) * kPi2 / 180.0;
            const QPointF arrowWingA(
                arrowTip.x() + armLen * std::cos(wA_rad),
                arrowTip.y() - armLen * std::sin(wA_rad)
            );
            const QPointF arrowWingB(
                arrowTip.x() + armLen * std::cos(wB_rad),
                arrowTip.y() - armLen * std::sin(wB_rad)
            );
            painter.drawLine(arrowTip, arrowWingA);
            painter.drawLine(arrowTip, arrowWingB);
            painter.restore();
        }

        if (!testAttribute(Qt::WA_TransparentForMouseEvents)) {
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
            QColor closeButtonColor(QStringLiteral("#ff5f57"));
            if (m_closeButtonHovered) {
                closeButtonColor = closeButtonColor.lighter(108);
            }
            if (m_closeButtonPressed) {
                closeButtonColor = closeButtonColor.darker(112);
            }

            painter.setPen(QPen(QColor(0, 0, 0, 80), 1.0));
            painter.setBrush(closeButtonColor);
            painter.drawEllipse(m_closeButtonRect.adjusted(0, 0, -1, -1));

            if (m_closeButtonHovered || m_closeButtonPressed) {
                painter.setPen(QPen(QColor(80, 32, 24, 220), 1.2));
                const int x1 = m_closeButtonRect.left() + 4;
                const int x2 = m_closeButtonRect.right() - 4;
                const int y1 = m_closeButtonRect.top() + 4;
                const int y2 = m_closeButtonRect.bottom() - 4;
                painter.drawLine(QPoint(x1, y1), QPoint(x2, y2));
                painter.drawLine(QPoint(x1, y2), QPoint(x2, y1));
            }
#else
            QColor closeButtonBg(textColor.red(), textColor.green(), textColor.blue(), 30);
            QColor closeButtonBorder(textColor.red(), textColor.green(), textColor.blue(), 88);
            QColor closeIconColor(textColor.red(), textColor.green(), textColor.blue(), 210);
            if (m_closeButtonHovered) {
                closeButtonBg = QColor(232, 17, 35, 220);
                closeButtonBorder = QColor(255, 255, 255, 80);
                closeIconColor = QColor(255, 255, 255, 240);
            }
            if (m_closeButtonPressed) {
                closeButtonBg = QColor(201, 12, 31, 230);
                closeButtonBorder = QColor(255, 255, 255, 96);
                closeIconColor = QColor(255, 255, 255, 250);
            }

            painter.setPen(QPen(closeButtonBorder, 1.0));
            painter.setBrush(closeButtonBg);
            painter.drawRoundedRect(m_closeButtonRect.adjusted(0, 0, -1, -1), 3, 3);

            painter.setPen(QPen(closeIconColor, 1.35));
            const int x1 = m_closeButtonRect.left() + 8;
            const int x2 = m_closeButtonRect.right() - 8;
            const int y1 = m_closeButtonRect.top() + 5;
            const int y2 = m_closeButtonRect.bottom() - 5;
            painter.drawLine(QPoint(x1, y1), QPoint(x2, y2));
            painter.drawLine(QPoint(x1, y2), QPoint(x2, y1));
#endif
        }

        QPen dividerPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 72));
        dividerPen.setWidthF(0.8);
        dividerPen.setCosmetic(true);
        painter.setPen(dividerPen);
        painter.drawLine(content.left(), topHeaderRect.bottom() + 8, content.right(), topHeaderRect.bottom() + 8);

        const QRect tableArea(
            content.left(),
            topHeaderRect.bottom() + 18,
            content.width(),
            qMax(120, content.bottom() - (topHeaderRect.bottom() + 18))
        );
        const QRect headerRect(tableArea.left(), tableArea.top(), tableArea.width(), 24);
        m_viewportRect = QRect(
            tableArea.left(),
            headerRect.bottom() + 1 + 8,
            tableArea.width(),
            qMax(40, tableArea.bottom() - (headerRect.bottom() + 8))
        );
        m_actionButtonRects.clear();
        m_dataRowHitAreas.clear();

        painter.setFont(headerFont);
        painter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 92));
        painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

        const int scrollBarWidth = 6;
        const int columnGap = 8;
        const bool showScrollBar = !m_items.isEmpty();
        const int tableContentWidth = qMax(
            120,
            m_viewportRect.width() - (showScrollBar ? (scrollBarWidth + 8) : 0)
        );

        const QFontMetrics headerMetrics(headerFont);
        const QFontMetrics rowMetrics(bodyFont);
        int priceWidth = qMax(
            54,
            headerMetrics.horizontalAdvance(i18n::t("popup.marketBreadth.hotDetailPrice", m_language)) + 10
        );
        int pctWidth = qMax(
            66,
            headerMetrics.horizontalAdvance(i18n::t("popup.marketBreadth.hotChange", m_language)) + 10
        );
        int marketCapWidth = qMax(
            74,
            headerMetrics.horizontalAdvance(i18n::t("popup.marketBreadth.hotDetailMarketCap", m_language)) + 10
        );
        int turnoverWidth = qMax(
            74,
            headerMetrics.horizontalAdvance(i18n::t("popup.marketBreadth.hotDetailTurnover", m_language)) + 10
        );
        int yearPctWidth = qMax(
            82,
            headerMetrics.horizontalAdvance(i18n::t("popup.marketBreadth.hotYearPct", m_language)) + 10
        );
        int actionWidth = qMax(
            44,
            headerMetrics.horizontalAdvance(i18n::t("popup.marketBreadth.hotDetailAction", m_language)) + 12
        );

        const int probeCount = qMin(m_items.size(), 48);
        for (int index = 0; index < probeCount; ++index) {
            const HotRankDetailItem& item = m_items.at(index);
            priceWidth = qMax(priceWidth, rowMetrics.horizontalAdvance(formatPrice(item.price)) + 10);
            pctWidth = qMax(pctWidth, rowMetrics.horizontalAdvance(formatPct(item.pct)) + 10);
            marketCapWidth = qMax(
                marketCapWidth,
                rowMetrics.horizontalAdvance(formatChineseMarketAmount(item.marketCap)) + 10
            );
            turnoverWidth = qMax(
                turnoverWidth,
                rowMetrics.horizontalAdvance(formatChineseMarketAmount(item.turnover)) + 10
            );
            yearPctWidth = qMax(yearPctWidth, rowMetrics.horizontalAdvance(formatPct(item.yearPct)) + 10);
            const QString actionText = containsWatchlistItem(item) ? QStringLiteral("-") : QStringLiteral("+");
            actionWidth = qMax(actionWidth, rowMetrics.horizontalAdvance(actionText) + 18);
        }

        priceWidth = qMin(priceWidth, 72);
        pctWidth = qMin(pctWidth, 76);
        marketCapWidth = qMin(marketCapWidth, 92);
        turnoverWidth = qMin(turnoverWidth, 92);
        yearPctWidth = qMin(yearPctWidth, 86);
        actionWidth = qMin(actionWidth, 52);

        int nameWidth = tableContentWidth - priceWidth - pctWidth - marketCapWidth - turnoverWidth
            - yearPctWidth - actionWidth - columnGap * 6;
        nameWidth = qBound(72, nameWidth, 110);
        const int consumedWidth = nameWidth + priceWidth + pctWidth + marketCapWidth
            + turnoverWidth + yearPctWidth + actionWidth + columnGap * 6;
        int extraWidth = qMax(0, tableContentWidth - consumedWidth);
        marketCapWidth += qMin(extraWidth / 2, 18);
        turnoverWidth += qMin(extraWidth - qMin(extraWidth / 2, 18), 18);
        nameWidth = qMax(
            72,
            tableContentWidth - priceWidth - pctWidth - marketCapWidth - turnoverWidth
                - yearPctWidth - actionWidth - columnGap * 6
        );

        const int tableLeft = m_viewportRect.left();
        const QRect nameHeaderRect(tableLeft, headerRect.top(), nameWidth, headerRect.height());
        const QRect priceHeaderRect(
            nameHeaderRect.right() + 1 + columnGap,
            headerRect.top(),
            priceWidth,
            headerRect.height()
        );
        const QRect pctHeaderRect(
            priceHeaderRect.right() + 1 + columnGap,
            headerRect.top(),
            pctWidth,
            headerRect.height()
        );
        const QRect marketCapHeaderRect(
            pctHeaderRect.right() + 1 + columnGap,
            headerRect.top(),
            marketCapWidth,
            headerRect.height()
        );
        const QRect turnoverHeaderRect(
            marketCapHeaderRect.right() + 1 + columnGap,
            headerRect.top(),
            turnoverWidth,
            headerRect.height()
        );
        const QRect yearPctHeaderRect(
            turnoverHeaderRect.right() + 1 + columnGap,
            headerRect.top(),
            yearPctWidth,
            headerRect.height()
        );
        const QRect actionHeaderRect(
            yearPctHeaderRect.right() + 1 + columnGap,
            headerRect.top(),
            actionWidth,
            headerRect.height()
        );
        m_priceHeaderRect = priceHeaderRect;
        m_pctHeaderRect = pctHeaderRect;
        m_marketCapHeaderRect = marketCapHeaderRect;
        m_turnoverHeaderRect = turnoverHeaderRect;
        m_yearPctHeaderRect = yearPctHeaderRect;

        const auto sortIndicator = [this](DetailSortField field) {
            if (m_sortField != field) {
                return QString();
            }
            return m_sortDescending ? QStringLiteral(" v") : QStringLiteral(" ^");
        };
        auto headerColor = [&](int headerIndex, DetailSortField field) {
            if (m_sortField == field) {
                return QColor(255, 255, 255, m_hoveredHeader == headerIndex ? 250 : 235);
            }
            return QColor(
                textColor.red(),
                textColor.green(),
                textColor.blue(),
                m_hoveredHeader == headerIndex ? 215 : 190
            );
        };

        painter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 190));
        painter.drawText(
            nameHeaderRect.adjusted(1, 0, -2, 0),
            Qt::AlignVCenter | Qt::AlignLeft,
            i18n::t("popup.marketBreadth.hotName", m_language)
        );
        painter.setPen(headerColor(1, DetailSortField::Price));
        painter.drawText(
            priceHeaderRect,
            Qt::AlignCenter,
            i18n::t("popup.marketBreadth.hotDetailPrice", m_language) + sortIndicator(DetailSortField::Price)
        );
        painter.setPen(headerColor(2, DetailSortField::Pct));
        painter.drawText(
            pctHeaderRect,
            Qt::AlignCenter,
            i18n::t("popup.marketBreadth.hotChange", m_language) + sortIndicator(DetailSortField::Pct)
        );
        painter.setPen(headerColor(3, DetailSortField::MarketCap));
        painter.drawText(
            marketCapHeaderRect,
            Qt::AlignCenter,
            i18n::t("popup.marketBreadth.hotDetailMarketCap", m_language) + sortIndicator(DetailSortField::MarketCap)
        );
        painter.setPen(headerColor(4, DetailSortField::Turnover));
        painter.drawText(
            turnoverHeaderRect,
            Qt::AlignCenter,
            i18n::t("popup.marketBreadth.hotDetailTurnover", m_language) + sortIndicator(DetailSortField::Turnover)
        );
        painter.setPen(headerColor(5, DetailSortField::YearPct));
        painter.drawText(
            yearPctHeaderRect,
            Qt::AlignCenter,
            i18n::t("popup.marketBreadth.hotYearPct", m_language) + sortIndicator(DetailSortField::YearPct)
        );
        painter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 190));
        painter.drawText(
            actionHeaderRect,
            Qt::AlignCenter,
            i18n::t("popup.marketBreadth.hotDetailAction", m_language)
        );

        if (m_loading) {
            painter.setFont(bodyFont);
            painter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 160));
            painter.drawText(
                m_viewportRect,
                Qt::AlignCenter,
                i18n::t("popup.marketBreadth.hotDetailLoading", m_language)
            );
            return;
        }

        if (!m_lastError.isEmpty() && m_items.isEmpty()) {
            painter.setFont(bodyFont);
            painter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 160));
            painter.drawText(m_viewportRect, Qt::AlignCenter, i18n::t("quote.dataError", m_language));
            return;
        }

        if (m_items.isEmpty()) {
            painter.setFont(bodyFont);
            painter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 160));
            painter.drawText(m_viewportRect, Qt::AlignCenter, i18n::t("quote.noData", m_language));
            return;
        }

        m_displayItems = m_items;
        std::stable_sort(m_displayItems.begin(), m_displayItems.end(), [this](const HotRankDetailItem& lhs, const HotRankDetailItem& rhs) {
            auto compareDouble = [this](double a, double b) {
                const bool aFinite = std::isfinite(a);
                const bool bFinite = std::isfinite(b);
                if (aFinite != bFinite) {
                    return aFinite;
                }
                if (!aFinite && !bFinite) {
                    return false;
                }
                if (qFuzzyCompare(a + 1.0, b + 1.0)) {
                    return false;
                }
                return m_sortDescending ? (a > b) : (a < b);
            };

            switch (m_sortField) {
            case DetailSortField::Price:
                return compareDouble(lhs.price, rhs.price);
            case DetailSortField::MarketCap:
                return compareDouble(lhs.marketCap, rhs.marketCap);
            case DetailSortField::Turnover:
                return compareDouble(lhs.turnover, rhs.turnover);
            case DetailSortField::YearPct:
                return compareDouble(lhs.yearPct, rhs.yearPct);
            case DetailSortField::Pct:
            default:
                return compareDouble(lhs.pct, rhs.pct);
            }
        });

        constexpr int kRowHeight = 24;
        constexpr int kRowGap = 2;
        const int visibleRows = qMax(
            1,
            (m_viewportRect.height() + kRowGap) / (kRowHeight + kRowGap)
        );
        m_scrollIndex = qBound(0, m_scrollIndex, qMax(0, m_displayItems.size() - visibleRows));
        const int endIndex = qMin(m_displayItems.size(), m_scrollIndex + visibleRows);

        painter.save();
        painter.setClipRect(m_viewportRect);
        painter.setFont(bodyFont);
        for (int row = m_scrollIndex; row < endIndex; ++row) {
            const int visibleRow = row - m_scrollIndex;
            const QRect rowRect(
                m_viewportRect.left(),
                m_viewportRect.top() + visibleRow * (kRowHeight + kRowGap),
                tableContentWidth,
                kRowHeight
            );
            const QRect nameRect(rowRect.left(), rowRect.top(), nameWidth, rowRect.height());
            const QRect priceRect(priceHeaderRect.left(), rowRect.top(), priceWidth, rowRect.height());
            const QRect pctRect(pctHeaderRect.left(), rowRect.top(), pctWidth, rowRect.height());
            const QRect marketCapRect(
                marketCapHeaderRect.left(),
                rowRect.top(),
                marketCapWidth,
                rowRect.height()
            );
            const QRect turnoverRect(
                turnoverHeaderRect.left(),
                rowRect.top(),
                turnoverWidth,
                rowRect.height()
            );
            const QRect yearPctRect(
                yearPctHeaderRect.left(),
                rowRect.top(),
                yearPctWidth,
                rowRect.height()
            );
            const QRect actionRect(
                actionHeaderRect.left(),
                rowRect.top(),
                actionWidth,
                rowRect.height()
            );
            const HotRankDetailItem& item = m_displayItems.at(row);
            m_dataRowHitAreas.push_back({rowRect, item});
            m_actionButtonRects.push_back(actionRect);
            const bool buttonHovered = (m_actionButtonRects.size() - 1) == m_hoveredActionRow;
            const bool buttonPressed = (m_actionButtonRects.size() - 1) == m_pressedActionRow;

            const QColor pctColor = std::isfinite(item.pct)
                ? (item.pct > 0.0 ? m_cfg.upColor : (item.pct < 0.0 ? m_cfg.downColor : m_cfg.flatColor))
                : textColor;
            const QColor yearPctColor = std::isfinite(item.yearPct)
                ? (item.yearPct > 0.0 ? m_cfg.upColor : (item.yearPct < 0.0 ? m_cfg.downColor : m_cfg.flatColor))
                : textColor;
            const bool tracked = containsWatchlistItem(item);
            const QString actionText = item.watchCode.trimmed().isEmpty()
                ? QStringLiteral("--")
                : (tracked ? QStringLiteral("-") : QStringLiteral("+"));
            QColor actionButtonBg(textColor.red(), textColor.green(), textColor.blue(), 28);
            QColor actionButtonBorder(textColor.red(), textColor.green(), textColor.blue(), 88);
            QColor actionButtonText = tracked ? m_cfg.downColor : m_cfg.upColor;
            if (buttonHovered) {
                actionButtonBg = QColor(textColor.red(), textColor.green(), textColor.blue(), 42);
            }
            if (buttonPressed) {
                actionButtonBg = QColor(textColor.red(), textColor.green(), textColor.blue(), 60);
            }

            painter.setPen(textColor);
            painter.drawText(
                nameRect.adjusted(1, 0, -2, 0),
                Qt::AlignVCenter | Qt::AlignLeft,
                rowMetrics.elidedText(item.name.trimmed(), Qt::ElideRight, qMax(8, nameRect.width() - 2))
            );
            painter.drawText(priceRect, Qt::AlignCenter, formatPrice(item.price));
            painter.setPen(pctColor);
            painter.drawText(pctRect, Qt::AlignCenter, formatPct(item.pct));
            painter.setPen(textColor);
            painter.drawText(
                marketCapRect.adjusted(2, 0, -1, 0),
                Qt::AlignVCenter | Qt::AlignRight,
                rowMetrics.elidedText(
                    formatChineseMarketAmount(item.marketCap),
                    Qt::ElideRight,
                    qMax(8, marketCapRect.width() - 2)
                )
            );
            painter.drawText(
                turnoverRect.adjusted(2, 0, -1, 0),
                Qt::AlignVCenter | Qt::AlignRight,
                rowMetrics.elidedText(
                    formatChineseMarketAmount(item.turnover),
                    Qt::ElideRight,
                    qMax(8, turnoverRect.width() - 2)
                )
            );
            painter.setPen(yearPctColor);
            painter.drawText(yearPctRect, Qt::AlignCenter, formatPct(item.yearPct));

            painter.setPen(QPen(actionButtonBorder, 1.0));
            painter.setBrush(actionButtonBg);
            painter.drawRoundedRect(actionRect.adjusted(7, 3, -7, -3), 4, 4);
            painter.setPen(actionButtonText);
            painter.drawText(actionRect, Qt::AlignCenter, actionText);

            if (row + 1 < endIndex) {
                QPen gridPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 42));
                gridPen.setStyle(Qt::DashLine);
                gridPen.setWidthF(0.6);
                gridPen.setCosmetic(true);
                painter.setPen(gridPen);
                const int lineY = rowRect.bottom() + qMax(1, kRowGap / 2);
                painter.drawLine(rowRect.left(), lineY, rowRect.right(), lineY);
            }
        }
        painter.restore();

        if (m_displayItems.size() > visibleRows) {
            const QRect trackRect(
                m_viewportRect.right() - scrollBarWidth + 1,
                m_viewportRect.top(),
                scrollBarWidth,
                m_viewportRect.height()
            );
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(textColor.red(), textColor.green(), textColor.blue(), 28));
            painter.drawRoundedRect(trackRect, 3, 3);

            const double ratio = static_cast<double>(visibleRows) / static_cast<double>(m_displayItems.size());
            const int thumbHeight = qMax(20, qRound(trackRect.height() * ratio));
            const int maxThumbTop = qMax(0, trackRect.height() - thumbHeight);
            const int maxScrollIndex = qMax(1, m_displayItems.size() - visibleRows);
            const int thumbTop = trackRect.top() + qRound(
                static_cast<double>(m_scrollIndex) / static_cast<double>(maxScrollIndex) * maxThumbTop
            );
            const QRect thumbRect(trackRect.left(), thumbTop, trackRect.width(), thumbHeight);
            painter.setBrush(QColor(textColor.red(), textColor.green(), textColor.blue(), 86));
            painter.drawRoundedRect(thumbRect, 3, 3);
        }
    }

private:
    struct DataRowHitArea {
        QRect rect;
        HotRankDetailItem item;
    };

    void ensureProvider() {
        if (m_provider) {
            return;
        }

        m_provider = new EastMoneyHotRankDetailProvider(this);
        connect(
            m_provider,
            &EastMoneyHotRankDetailProvider::dataReady,
            this,
            [this](const QString& fs, const QVector<HotRankDetailItem>& items) {
                if (fs != m_currentFs) {
                    return;
                }
                m_items = items;
                m_displayItems = items;
                m_lastError.clear();
                m_loading = false;
                m_scrollIndex = 0;
                update();
            }
        );
        connect(
            m_provider,
            &EastMoneyHotRankDetailProvider::error,
            this,
            [this](const QString& fs, const QString& message) {
                if (fs != m_currentFs) {
                    return;
                }
                m_displayItems.clear();
                m_lastError = message.trimmed();
                m_loading = false;
                update();
            }
        );
    }

    void requestData(bool forceRefresh = false) {
        ensureProvider();
        if (!m_provider || m_currentFs.isEmpty()) {
            return;
        }
        hideTimelinePopup();
        m_provider->fetch(m_currentFs, 300, forceRefresh);
    }

    int dataRowAt(const QPoint& pos) const {
        for (int index = 0; index < m_dataRowHitAreas.size(); ++index) {
            if (m_dataRowHitAreas.at(index).rect.contains(pos)) {
                return index;
            }
        }
        return -1;
    }

    void ensureTimelinePopup() {
        if (m_timelinePopup) {
            return;
        }
        m_timelinePopup = new SharedTimelineChartPopup(this);
        m_timelinePopup->applyConfig(m_cfg);
    }

    void showTimelinePopupForRow(int rowIndex) {
        if (m_loading || rowIndex < 0 || rowIndex >= m_dataRowHitAreas.size()) {
            hideTimelinePopup();
            return;
        }

        const DataRowHitArea& area = m_dataRowHitAreas.at(rowIndex);
        const QString code = area.item.watchCode.trimmed();
        const QString name = area.item.name.trimmed();
        if (code.isEmpty() || !isTimelinePopupSupportedCode(code)) {
            hideTimelinePopup();
            return;
        }

        ensureTimelinePopup();
        if (!m_timelinePopup) {
            return;
        }
        const QRect globalAnchor(mapToGlobal(area.rect.topLeft()), area.rect.size());
        m_timelinePopup->showForStock(code, name, globalAnchor, width());
    }

    void hideTimelinePopup() {
        if (m_timelinePopup) {
            m_timelinePopup->hidePopup();
        }
    }

    void startRefreshFeedback() {
        constexpr qint64 kRefreshFeedbackDurationMs = 600;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        m_refreshFeedbackStartedMs = nowMs;
        m_refreshFeedbackUntilMs = nowMs + kRefreshFeedbackDurationMs;
        if (m_refreshFeedbackTimer && !m_refreshFeedbackTimer->isActive()) {
            m_refreshFeedbackTimer->start();
        }
        if (m_refreshButtonRect.isValid()) {
            update(m_refreshButtonRect.adjusted(-3, -3, 3, 3));
        }
    }

    bool isRefreshFeedbackActive(qint64* nowMsOut = nullptr) const {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMsOut) {
            *nowMsOut = nowMs;
        }
        return nowMs < m_refreshFeedbackUntilMs;
    }

    int headerAt(const QPoint& pos) const {
        if (m_priceHeaderRect.contains(pos)) {
            return 1;
        }
        if (m_pctHeaderRect.contains(pos)) {
            return 2;
        }
        if (m_marketCapHeaderRect.contains(pos)) {
            return 3;
        }
        if (m_turnoverHeaderRect.contains(pos)) {
            return 4;
        }
        if (m_yearPctHeaderRect.contains(pos)) {
            return 5;
        }
        return 0;
    }

    int actionRowAt(const QPoint& pos) const {
        for (int index = 0; index < m_actionButtonRects.size(); ++index) {
            if (m_actionButtonRects.at(index).contains(pos)) {
                return index;
            }
        }
        return -1;
    }

    void updateHeaderByIndex(int index) {
        switch (index) {
        case 1:
            if (m_priceHeaderRect.isValid()) {
                update(m_priceHeaderRect.adjusted(-2, -2, 2, 2));
            }
            break;
        case 2:
            if (m_pctHeaderRect.isValid()) {
                update(m_pctHeaderRect.adjusted(-2, -2, 2, 2));
            }
            break;
        case 3:
            if (m_marketCapHeaderRect.isValid()) {
                update(m_marketCapHeaderRect.adjusted(-2, -2, 2, 2));
            }
            break;
        case 4:
            if (m_turnoverHeaderRect.isValid()) {
                update(m_turnoverHeaderRect.adjusted(-2, -2, 2, 2));
            }
            break;
        case 5:
            if (m_yearPctHeaderRect.isValid()) {
                update(m_yearPctHeaderRect.adjusted(-2, -2, 2, 2));
            }
            break;
        default:
            break;
        }
    }

    void updateActionRowByIndex(int index) {
        if (index >= 0 && index < m_actionButtonRects.size()) {
            update(m_actionButtonRects.at(index).adjusted(-2, -2, 2, 2));
        }
    }

    void applySortByHeaderIndex(int index) {
        DetailSortField nextField = DetailSortField::Pct;
        switch (index) {
        case 1:
            nextField = DetailSortField::Price;
            break;
        case 2:
            nextField = DetailSortField::Pct;
            break;
        case 3:
            nextField = DetailSortField::MarketCap;
            break;
        case 4:
            nextField = DetailSortField::Turnover;
            break;
        case 5:
            nextField = DetailSortField::YearPct;
            break;
        default:
            return;
        }

        if (m_sortField == nextField) {
            m_sortDescending = !m_sortDescending;
        } else {
            m_sortField = nextField;
            m_sortDescending = true;
        }
        m_scrollIndex = 0;
        update();
    }

    QString watchKeyForItem(const HotRankDetailItem& item) const {
        return item.watchCode.trimmed().toLower();
    }

    bool containsWatchlistItem(const HotRankDetailItem& item) const {
        const QString key = watchKeyForItem(item);
        if (key.isEmpty()) {
            return false;
        }
        if (m_watchlistPresenceOverrides.contains(key)) {
            return m_watchlistPresenceOverrides.value(key);
        }
        return m_containsWatchlistCallback ? m_containsWatchlistCallback(item.watchCode) : false;
    }

    void toggleWatchlistAction(int actionRowIndex) {
        if (actionRowIndex < 0 || actionRowIndex >= m_actionButtonRects.size()) {
            return;
        }
        const int itemIndex = m_scrollIndex + actionRowIndex;
        if (itemIndex < 0 || itemIndex >= m_displayItems.size()) {
            return;
        }
        const HotRankDetailItem& item = m_displayItems.at(itemIndex);
        if (item.watchCode.trimmed().isEmpty() || !m_mutateWatchlistCallback) {
            return;
        }

        const bool tracked = containsWatchlistItem(item);
        const bool add = !tracked;
        if (!m_mutateWatchlistCallback(item.watchCode, item.name, add)) {
            return;
        }

        const QString key = watchKeyForItem(item);
        if (!key.isEmpty()) {
            m_watchlistPresenceOverrides.insert(key, add);
        }
        m_watchlistDirty = true;
        update();
    }

    void flushPendingWatchlistReloadIfNeeded() {
        if (!m_watchlistDirty) {
            return;
        }
        m_watchlistDirty = false;
        if (m_reloadWatchlistCallback) {
            m_reloadWatchlistCallback();
        }
        m_watchlistPresenceOverrides.clear();
    }

    enum class DetailSortField {
        Price = 0,
        Pct = 1,
        MarketCap = 2,
        Turnover = 3,
        YearPct = 4,
    };

    AppConfig m_cfg;
    QString m_language = QStringLiteral("en_US");
    HotRankItem m_selectedItem;
    QString m_currentFs;
    QVector<HotRankDetailItem> m_items;
    QVector<HotRankDetailItem> m_displayItems;
    EastMoneyHotRankDetailProvider* m_provider = nullptr;
    QString m_lastError;
    QRect m_closeButtonRect;
    QRect m_refreshButtonRect;
    QRect m_dragHandleRect;
    QRect m_viewportRect;
    QRect m_priceHeaderRect;
    QRect m_pctHeaderRect;
    QRect m_marketCapHeaderRect;
    QRect m_turnoverHeaderRect;
    QRect m_yearPctHeaderRect;
    bool m_closeButtonHovered = false;
    bool m_closeButtonPressed = false;
    bool m_refreshButtonHovered = false;
    bool m_refreshButtonPressed = false;
    bool m_loading = false;
    bool m_dragging = false;
    bool m_hasStoredGeometry = false;
    bool m_watchlistDirty = false;
    int m_hoveredHeader = 0;
    int m_pressedHeader = 0;
    int m_hoveredActionRow = -1;
    int m_pressedActionRow = -1;
    int m_pressedDataRow = -1;
    int m_scrollIndex = 0;
    QPoint m_dragOffset;
    DetailSortField m_sortField = DetailSortField::Pct;
    bool m_sortDescending = true;
    QVector<QRect> m_actionButtonRects;
    QVector<DataRowHitArea> m_dataRowHitAreas;
    QHash<QString, bool> m_watchlistPresenceOverrides;
    QTimer* m_refreshFeedbackTimer = nullptr;
    qint64 m_refreshFeedbackStartedMs = 0;
    qint64 m_refreshFeedbackUntilMs = 0;
    SharedTimelineChartPopup* m_timelinePopup = nullptr;
    std::function<bool(const QString&)> m_containsWatchlistCallback;
    std::function<bool(const QString&, const QString&, bool)> m_mutateWatchlistCallback;
    std::function<void()> m_reloadWatchlistCallback;
};


MarketBreadthDetailWindow* MarketBreadthDetailWindow::s_visiblePopup = nullptr;

 MarketBreadthDetailWindow::MarketBreadthDetailWindow(QWidget* parent)
 : QWidget(nullptr)
 , m_parentWindow(parent) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    setMinimumSize(marketBreadthMinimumPopupSize());
    resize(minimumSize());

    m_refreshFeedbackTimer = new QTimer(this);
    m_refreshFeedbackTimer->setInterval(33);
    connect(m_refreshFeedbackTimer, &QTimer::timeout, this, [this]() {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs >= m_refreshFeedbackUntilMs) {
            m_refreshFeedbackStartedMs = 0;
            m_refreshFeedbackUntilMs = 0;
            m_refreshFeedbackTimer->stop();
        }

        if (m_refreshButtonRect.isValid()) {
            update(m_refreshButtonRect.adjusted(-3, -3, 3, 3));
        } else {
            update();
        }
    });

    m_autoRefreshTimer = new QTimer(this);
    m_autoRefreshTimer->setInterval(
        static_cast<int>(app_constants::kMarketBreadthPopupAutoRefreshIntervalMs)
    );
    connect(m_autoRefreshTimer, &QTimer::timeout, this, [this]() {
        if (!isVisible()) {
            stopAutoRefreshTimer();
            return;
        }
        triggerPopupRefresh(false);
    });
}


MarketBreadthDetailWindow::~MarketBreadthDetailWindow() {
    if (s_visiblePopup == this) {
        s_visiblePopup = nullptr;
    }
}


void  MarketBreadthDetailWindow::applyConfig(const AppConfig& cfg) {
    m_cfg = cfg;
    ensureHotRankProviders();
    if (m_hotRankProvider) {
        m_hotRankProvider->applyConfig(m_cfg);
    }
    ensureStockHeatProvider();
    if (m_stockHeatProvider) {
        m_stockHeatProvider->applyConfig(m_cfg);
    }
    if (m_hotRankDetailWindow) {
        m_hotRankDetailWindow->applyConfig(m_cfg);
    }
    if (m_indexQuoteProvider) {
        m_indexQuoteProvider->applyConfig(m_cfg);
    }
    if (m_timelinePopup) {
        m_timelinePopup->applyConfig(m_cfg);
    }
    setFont(effectiveFloatingWindowFont(cfg, font()));
    update();
}


void  MarketBreadthDetailWindow::setLanguage(const QString& language) {
    m_language = i18n::resolveLanguage(language);
    if (m_hotRankDetailWindow) {
        m_hotRankDetailWindow->setLanguage(m_language);
    }
    update();
}


void  MarketBreadthDetailWindow::setForceRefreshCallback(std::function<void()> callback) {
    m_forceRefreshCallback = callback;
}

void  MarketBreadthDetailWindow::setHotRankDetailWatchlistCallbacks(
    std::function<bool(const QString&)> containsCallback,
    std::function<bool(const QString&, const QString&, bool)> mutateCallback,
    std::function<void()> reloadCallback
) {
    m_hotRankDetailWatchlistContainsCallback = std::move(containsCallback);
    m_hotRankDetailWatchlistMutateCallback = std::move(mutateCallback);
    m_hotRankDetailWatchlistReloadCallback = std::move(reloadCallback);
    if (m_hotRankDetailWindow) {
        m_hotRankDetailWindow->setWatchlistCallbacks(
            m_hotRankDetailWatchlistContainsCallback,
            m_hotRankDetailWatchlistMutateCallback,
            m_hotRankDetailWatchlistReloadCallback
        );
    }
}


void  MarketBreadthDetailWindow::showCenteredForSnapshot(
 const MarketBreadthSnapshot& snapshot,
 const QVector<HotRankItem>& hotSectors,
 const QVector<HotRankItem>& hotConcepts,
 const QRect& referenceRect
) {
    ensureSingleVisible();
    m_snapshot = snapshot;
    m_hotSectors = hotSectors;
    m_hotConcepts = hotConcepts;
    m_pinnedFromTray = true;
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_closeButtonHovered = false;
    m_closeButtonPressed = false;
    m_refreshButtonHovered = false;
    m_refreshButtonPressed = false;
    m_refreshFeedbackStartedMs = 0;
    m_refreshFeedbackUntilMs = 0;
    if (m_refreshFeedbackTimer) {
        m_refreshFeedbackTimer->stop();
    }
    m_hotSectorTabHovered = false;
    m_hotConceptTabHovered = false;
    m_hotHourTabHovered = false;
    m_hotDayTabHovered = false;
    m_hotPctHeaderHovered = false;
    m_hotInflowHeaderHovered = false;
    m_hotYearPctHeaderHovered = false;
    m_hoveredHotRowIndex = -1;
    m_pressedHotRowIndex = -1;
    m_hoveredHotStockActionIndex = -1;
    m_pressedHotStockActionIndex = -1;
    m_pressedIndexCellIndex = -1;
    m_breadthDistributionTabHovered = false;
    m_breadthTrendTabHovered = false;
    m_pressedHotTab = 0;
    m_pressedHotHeader = 0;
    m_pressedBreadthTab = 0;
    m_dragging = false;
    m_resizing = false;
    m_dragOffset = QPoint();
    m_resizeStartGlobalPos = QPoint();
    m_resizeStartSize = QSize();
    m_hotRankRowHitAreas.clear();
    m_indexCellHitAreas.clear();
    hideTimelinePopup();
    unsetCursor();

    const QRect screenRect = availableScreenRectForReference(referenceRect);

    QRect targetRect;
    if (m_hasStoredGeometry && geometry().isValid()) {
        // Use the screen where the window was previously positioned, not the
        // floating window's screen; fall back to the floating window's screen
        // only when the stored position is off all connected screens.
        QRect storedScreenRect = screenRect;
        if (QScreen* storedScreen = QGuiApplication::screenAt(geometry().center())) {
            storedScreenRect = storedScreen->availableGeometry();
        }
        targetRect = adjustedPopupRectForScreen(geometry(), storedScreenRect);
    } else {
        targetRect = QRect(QPoint(0, 0), minimumSize());
        if (screenRect.isValid()) {
            targetRect.moveCenter(screenRect.center());
        }
        targetRect = adjustedPopupRectForScreen(targetRect, screenRect);
    }

    setGeometry(targetRect);
    m_hasStoredGeometry = true;
    if (!isVisible()) {
        show();
    }
#if defined(Q_OS_MACOS)
    // Switch to Regular policy so the window appears in the Dock.
    setMacAppActivationPolicy(true);
    // Keep the window managed by Mission Control / Expose on macOS.
    setMacWindowCollectionBehaviorManaged(this);
#endif
    startAutoRefreshTimer();
    requestHotRankData(false);
    requestHotRankData(true);
    requestStockHeatData(HotRankTabMode::HourHeat);
    requestStockHeatData(HotRankTabMode::DayHeat);
    requestIndexQuoteData();
    update();
}


void  MarketBreadthDetailWindow::refreshSnapshot(
 const MarketBreadthSnapshot& snapshot,
 const QVector<HotRankItem>& hotSectors,
 const QVector<HotRankItem>& hotConcepts
) {
    m_snapshot = snapshot;
    if (!hotSectors.isEmpty()) {
        m_hotSectors = hotSectors;
    }
    if (!hotConcepts.isEmpty()) {
        m_hotConcepts = hotConcepts;
    }
    requestIndexQuoteData();
    update();
}


bool  MarketBreadthDetailWindow::isPinnedFromTray() const {
    return m_pinnedFromTray;
}


QRect  MarketBreadthDetailWindow::savedWindowRect() const {
    return m_hasStoredGeometry ? geometry() : QRect();
}


void  MarketBreadthDetailWindow::setSavedWindowRect(const QRect& rect) {
    if (!rect.isValid() || rect.width() <= 0 || rect.height() <= 0) {
        return;
    }

    QRect screenRect;
    if (QScreen* screen = QGuiApplication::screenAt(rect.center())) {
        screenRect = screen->availableGeometry();
    } else if (QScreen* screen = QGuiApplication::primaryScreen()) {
        screenRect = screen->availableGeometry();
    }

    setGeometry(adjustedPopupRectForScreen(rect, screenRect));
    m_hasStoredGeometry = true;
}


void  MarketBreadthDetailWindow::hidePopup() {
    m_pinnedFromTray = false;
    m_closeButtonHovered = false;
    m_closeButtonPressed = false;
    m_refreshButtonHovered = false;
    m_refreshButtonPressed = false;
    m_refreshFeedbackStartedMs = 0;
    m_refreshFeedbackUntilMs = 0;
    if (m_refreshFeedbackTimer) {
        m_refreshFeedbackTimer->stop();
    }
    m_hotSectorTabHovered = false;
    m_hotConceptTabHovered = false;
    m_hotHourTabHovered = false;
    m_hotDayTabHovered = false;
    m_hotPctHeaderHovered = false;
    m_hotInflowHeaderHovered = false;
    m_hotYearPctHeaderHovered = false;
    m_hoveredHotRowIndex = -1;
    m_pressedHotRowIndex = -1;
    m_hoveredHotStockActionIndex = -1;
    m_pressedHotStockActionIndex = -1;
    m_pressedIndexCellIndex = -1;
    m_breadthDistributionTabHovered = false;
    m_breadthTrendTabHovered = false;
    m_pressedHotTab = 0;
    m_pressedHotHeader = 0;
    m_pressedBreadthTab = 0;
    m_dragging = false;
    m_resizing = false;
    m_dragOffset = QPoint();
    m_resizeStartGlobalPos = QPoint();
    m_resizeStartSize = QSize();
    unsetCursor();
    m_refreshButtonRect = QRect();
    m_updatedTextRect = QRect();
    m_hotSectorTabRect = QRect();
    m_hotConceptTabRect = QRect();
    m_hotHourTabRect = QRect();
    m_hotDayTabRect = QRect();
    m_hotNameHeaderRect = QRect();
    m_hotPctHeaderRect = QRect();
    m_hotInflowHeaderRect = QRect();
    m_hotYearPctHeaderRect = QRect();
    m_hotRankListViewportRect = QRect();
    m_hotRankRowHitAreas.clear();
    m_hotStockActionButtonRects.clear();
    m_breadthDistributionTabRect = QRect();
    m_breadthTrendTabRect = QRect();
    m_indexCellHitAreas.clear();
    hideTimelinePopup();
    if (m_hotRankDetailWindow) {
        m_hotRankDetailWindow->hidePopup();
    }
    flushPendingHotStockWatchlistReloadIfNeeded();
    stopAutoRefreshTimer();
    hide();
    if (s_visiblePopup == this) {
        s_visiblePopup = nullptr;
    }
}


bool  MarketBreadthDetailWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_hotRankDetailWindow && event->type() == QEvent::Hide) {
        if (!testAttribute(Qt::WA_TransparentForMouseEvents)) {
            // already interactive, nothing to do
        } else if (isVisible()) {
            setAttribute(Qt::WA_TransparentForMouseEvents, false);
        }
    }

    // Hide the timeline popup on any mouse press outside both this window and the popup.
    if (m_timelineAppFilterInstalled && event->type() == QEvent::MouseButtonPress) {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        const QPoint globalPos = mouseEvent->globalPosition().toPoint();
        const bool insideMain = geometry().contains(globalPos);
        const bool insidePopup = m_timelinePopup && m_timelinePopup->isVisible()
            && m_timelinePopup->geometry().contains(globalPos);
        if (!insideMain && !insidePopup) {
            hideTimelinePopup();
        }
    }

    return QWidget::eventFilter(watched, event);
}


void  MarketBreadthDetailWindow::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    unsetCursor();
    stopAutoRefreshTimer();
    hideTimelinePopup();
    flushPendingHotStockWatchlistReloadIfNeeded();
    if (m_hotRankDetailWindow) {
        m_hotRankDetailWindow->hidePopup();
    }
#if defined(Q_OS_MACOS)
    // Restore Accessory policy so the app disappears from the Dock when the window closes.
    setMacAppActivationPolicy(false);
#endif
    if (s_visiblePopup == this) {
        s_visiblePopup = nullptr;
    }
}


void  MarketBreadthDetailWindow::resizeEvent(QResizeEvent* event) {
    if (!event) {
        return;
    }

    if (m_adjustingResize) {
        QWidget::resizeEvent(event);
        return;
    }

    const QSize normalizedSize = normalizedMarketBreadthPopupSize(event->size(), event->oldSize());
    if (normalizedSize != event->size()) {
        m_adjustingResize = true;
        resize(normalizedSize);
        m_adjustingResize = false;
    }

    QWidget::resizeEvent(event);
}


QRect  MarketBreadthDetailWindow::resizeHandleRect() const {
    return QRect(
        qMax(0, width() - kMarketBreadthResizeHandleSizePx),
        qMax(0, height() - kMarketBreadthResizeHandleSizePx),
        kMarketBreadthResizeHandleSizePx,
        kMarketBreadthResizeHandleSizePx
    );
}


void  MarketBreadthDetailWindow::updateCursorForPosition(const QPoint& pos) {
    if (testAttribute(Qt::WA_TransparentForMouseEvents)) {
        unsetCursor();
        return;
    }

    if (m_resizing || resizeHandleRect().contains(pos)) {
        setCursor(Qt::SizeFDiagCursor);
        return;
    }

    if (hotStockActionAt(pos) >= 0) {
        setCursor(Qt::PointingHandCursor);
        return;
    }

    const int hotRowIndex = hotRankRowAt(pos);
    if (hotRowIndex >= 0) {
        if (!isStockHeatTabMode(resolvedHotRankTabMode())) {
            setCursor(Qt::PointingHandCursor);
            return;
        }

        if (hotRowIndex < m_hotRankRowHitAreas.size()) {
            const HotRankItem& item = m_hotRankRowHitAreas.at(hotRowIndex).item;
            const QString code = !item.watchCode.trimmed().isEmpty()
                ? item.watchCode.trimmed()
                : item.code.trimmed();
            if (!code.isEmpty() && isTimelinePopupSupportedCode(code)) {
                setCursor(Qt::PointingHandCursor);
                return;
            }
        }
    }

    for (const IndexCellHitArea& area : m_indexCellHitAreas) {
        if (area.rect.contains(pos) && isTimelinePopupSupportedCode(area.code)) {
            setCursor(Qt::PointingHandCursor);
            return;
        }
    }

    unsetCursor();
}


void  MarketBreadthDetailWindow::applyResizeFromGlobalPos(const QPoint& globalPos) {
    if (!m_resizing || !m_resizeStartSize.isValid()) {
        return;
    }

    const QPoint delta = globalPos - m_resizeStartGlobalPos;
    const qreal widthScale = static_cast<qreal>(m_resizeStartSize.width() + delta.x())
        / static_cast<qreal>(m_resizeStartSize.width());
    const qreal heightScale = static_cast<qreal>(m_resizeStartSize.height() + delta.y())
        / static_cast<qreal>(m_resizeStartSize.height());
    qreal scale = (std::fabs(widthScale - 1.0) >= std::fabs(heightScale - 1.0))
        ? widthScale
        : heightScale;
    if (!std::isfinite(scale)) {
        scale = 1.0;
    }

    const QSize requestedSize(
        qRound(static_cast<qreal>(m_resizeStartSize.width()) * scale),
        qRound(static_cast<qreal>(m_resizeStartSize.height()) * scale)
    );
    resize(normalizedMarketBreadthPopupSize(requestedSize, m_resizeStartSize));
}


void  MarketBreadthDetailWindow::mouseMoveEvent(QMouseEvent* event) {
    if (!event || testAttribute(Qt::WA_TransparentForMouseEvents)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (m_resizing && (event->buttons() & Qt::LeftButton)) {
        hideTimelinePopup();
        applyResizeFromGlobalPos(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        hideTimelinePopup();
        move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }

    const bool closeHovered = m_closeButtonRect.contains(event->pos());
    const bool refreshHovered = m_refreshButtonRect.contains(event->pos());
    const bool sectorHovered = m_hotSectorTabRect.contains(event->pos());
    const bool conceptHovered = m_hotConceptTabRect.contains(event->pos());
    const bool hourHovered = m_hotHourTabRect.contains(event->pos());
    const bool dayHovered = m_hotDayTabRect.contains(event->pos());
    const bool pctHeaderHovered = m_hotPctHeaderRect.contains(event->pos());
    const bool inflowHeaderHovered = m_hotInflowHeaderRect.contains(event->pos());
    const bool yearPctHeaderHovered = m_hotYearPctHeaderRect.contains(event->pos());
    const bool breadthDistributionHovered = m_breadthDistributionTabRect.contains(event->pos());
    const bool breadthTrendHovered = m_breadthTrendTabRect.contains(event->pos());
    const int hotRowHovered = hotRankRowAt(event->pos());
    const int hotStockActionHovered = hotStockActionAt(event->pos());
    if (closeHovered != m_closeButtonHovered
        || refreshHovered != m_refreshButtonHovered
        || sectorHovered != m_hotSectorTabHovered
        || conceptHovered != m_hotConceptTabHovered
        || hourHovered != m_hotHourTabHovered
        || dayHovered != m_hotDayTabHovered
        || pctHeaderHovered != m_hotPctHeaderHovered
        || inflowHeaderHovered != m_hotInflowHeaderHovered
        || yearPctHeaderHovered != m_hotYearPctHeaderHovered
        || breadthDistributionHovered != m_breadthDistributionTabHovered
        || breadthTrendHovered != m_breadthTrendTabHovered
        || hotRowHovered != m_hoveredHotRowIndex
        || hotStockActionHovered != m_hoveredHotStockActionIndex) {
        const int previousHotRow = m_hoveredHotRowIndex;
        const int previousHotStockAction = m_hoveredHotStockActionIndex;
        m_closeButtonHovered = closeHovered;
        m_refreshButtonHovered = refreshHovered;
        m_hotSectorTabHovered = sectorHovered;
        m_hotConceptTabHovered = conceptHovered;
        m_hotHourTabHovered = hourHovered;
        m_hotDayTabHovered = dayHovered;
        m_hotPctHeaderHovered = pctHeaderHovered;
        m_hotInflowHeaderHovered = inflowHeaderHovered;
        m_hotYearPctHeaderHovered = yearPctHeaderHovered;
        m_breadthDistributionTabHovered = breadthDistributionHovered;
        m_breadthTrendTabHovered = breadthTrendHovered;
        m_hoveredHotRowIndex = hotRowHovered;
        m_hoveredHotStockActionIndex = hotStockActionHovered;
        update(m_closeButtonRect.adjusted(-2, -2, 2, 2));
        if (m_refreshButtonRect.isValid()) {
            update(m_refreshButtonRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotSectorTabRect.isValid()) {
            update(m_hotSectorTabRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotConceptTabRect.isValid()) {
            update(m_hotConceptTabRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotHourTabRect.isValid()) {
            update(m_hotHourTabRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotDayTabRect.isValid()) {
            update(m_hotDayTabRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotPctHeaderRect.isValid()) {
            update(m_hotPctHeaderRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotInflowHeaderRect.isValid()) {
            update(m_hotInflowHeaderRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotYearPctHeaderRect.isValid()) {
            update(m_hotYearPctHeaderRect.adjusted(-2, -2, 2, 2));
        }
        if (m_breadthDistributionTabRect.isValid()) {
            update(m_breadthDistributionTabRect.adjusted(-2, -2, 2, 2));
        }
        if (m_breadthTrendTabRect.isValid()) {
            update(m_breadthTrendTabRect.adjusted(-2, -2, 2, 2));
        }
        if (previousHotRow >= 0 && previousHotRow < m_hotRankRowHitAreas.size()) {
            update(m_hotRankRowHitAreas.at(previousHotRow).rect.adjusted(-2, -2, 2, 2));
        }
        if (m_hoveredHotRowIndex >= 0 && m_hoveredHotRowIndex < m_hotRankRowHitAreas.size()) {
            update(m_hotRankRowHitAreas.at(m_hoveredHotRowIndex).rect.adjusted(-2, -2, 2, 2));
        }
        if (previousHotStockAction >= 0 && previousHotStockAction < m_hotStockActionButtonRects.size()) {
            update(m_hotStockActionButtonRects.at(previousHotStockAction).adjusted(-2, -2, 2, 2));
        }
        if (m_hoveredHotStockActionIndex >= 0 && m_hoveredHotStockActionIndex < m_hotStockActionButtonRects.size()) {
            update(m_hotStockActionButtonRects.at(m_hoveredHotStockActionIndex).adjusted(-2, -2, 2, 2));
        }
    }
    updateCursorForPosition(event->pos());
    QWidget::mouseMoveEvent(event);
}


void  MarketBreadthDetailWindow::leaveEvent(QEvent* event) {
    if (!testAttribute(Qt::WA_TransparentForMouseEvents)
        && (m_closeButtonHovered || m_closeButtonPressed
            || m_refreshButtonHovered || m_refreshButtonPressed
            || m_hotSectorTabHovered || m_hotConceptTabHovered
            || m_hotHourTabHovered || m_hotDayTabHovered || m_pressedHotTab != 0
            || m_hotPctHeaderHovered || m_hotInflowHeaderHovered
            || m_hotYearPctHeaderHovered || m_pressedHotHeader != 0
            || m_hoveredHotRowIndex >= 0 || m_pressedHotRowIndex >= 0
            || m_hoveredHotStockActionIndex >= 0 || m_pressedHotStockActionIndex >= 0
            || m_breadthDistributionTabHovered || m_breadthTrendTabHovered || m_pressedBreadthTab != 0)) {
        const int previousHotRow = m_hoveredHotRowIndex;
        const int previousPressedHotRow = m_pressedHotRowIndex;
        const int previousHotStockAction = m_hoveredHotStockActionIndex;
        const int previousPressedHotStockAction = m_pressedHotStockActionIndex;
        m_closeButtonHovered = false;
        m_closeButtonPressed = false;
        m_refreshButtonHovered = false;
        m_refreshButtonPressed = false;
        m_hotSectorTabHovered = false;
        m_hotConceptTabHovered = false;
        m_hotHourTabHovered = false;
        m_hotDayTabHovered = false;
        m_hotPctHeaderHovered = false;
        m_hotInflowHeaderHovered = false;
        m_hotYearPctHeaderHovered = false;
        m_hoveredHotRowIndex = -1;
        m_pressedHotRowIndex = -1;
        m_hoveredHotStockActionIndex = -1;
        m_pressedHotStockActionIndex = -1;
        m_pressedIndexCellIndex = -1;
        m_breadthDistributionTabHovered = false;
        m_breadthTrendTabHovered = false;
        m_pressedHotTab = 0;
        m_pressedHotHeader = 0;
        m_pressedBreadthTab = 0;
        update(m_closeButtonRect.adjusted(-2, -2, 2, 2));
        if (m_refreshButtonRect.isValid()) {
            update(m_refreshButtonRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotSectorTabRect.isValid()) {
            update(m_hotSectorTabRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotConceptTabRect.isValid()) {
            update(m_hotConceptTabRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotHourTabRect.isValid()) {
            update(m_hotHourTabRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotDayTabRect.isValid()) {
            update(m_hotDayTabRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotPctHeaderRect.isValid()) {
            update(m_hotPctHeaderRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotInflowHeaderRect.isValid()) {
            update(m_hotInflowHeaderRect.adjusted(-2, -2, 2, 2));
        }
        if (m_hotYearPctHeaderRect.isValid()) {
            update(m_hotYearPctHeaderRect.adjusted(-2, -2, 2, 2));
        }
        if (previousHotRow >= 0 && previousHotRow < m_hotRankRowHitAreas.size()) {
            update(m_hotRankRowHitAreas.at(previousHotRow).rect.adjusted(-2, -2, 2, 2));
        }
        if (previousPressedHotRow >= 0 && previousPressedHotRow < m_hotRankRowHitAreas.size()) {
            update(m_hotRankRowHitAreas.at(previousPressedHotRow).rect.adjusted(-2, -2, 2, 2));
        }
        if (previousHotStockAction >= 0 && previousHotStockAction < m_hotStockActionButtonRects.size()) {
            update(m_hotStockActionButtonRects.at(previousHotStockAction).adjusted(-2, -2, 2, 2));
        }
        if (previousPressedHotStockAction >= 0 && previousPressedHotStockAction < m_hotStockActionButtonRects.size()) {
            update(m_hotStockActionButtonRects.at(previousPressedHotStockAction).adjusted(-2, -2, 2, 2));
        }
        if (m_breadthDistributionTabRect.isValid()) {
            update(m_breadthDistributionTabRect.adjusted(-2, -2, 2, 2));
        }
        if (m_breadthTrendTabRect.isValid()) {
            update(m_breadthTrendTabRect.adjusted(-2, -2, 2, 2));
        }
    }
    unsetCursor();
    QWidget::leaveEvent(event);
}


void  MarketBreadthDetailWindow::mousePressEvent(QMouseEvent* event) {
    if (!event || testAttribute(Qt::WA_TransparentForMouseEvents)) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (event->button() == Qt::LeftButton && m_closeButtonRect.contains(event->pos())) {
        m_closeButtonPressed = true;
        m_closeButtonHovered = true;
        update(m_closeButtonRect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_refreshButtonRect.contains(event->pos())) {
        m_refreshButtonPressed = true;
        m_refreshButtonHovered = true;
        update(m_refreshButtonRect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_hotSectorTabRect.contains(event->pos())) {
        m_pressedHotTab = 1;
        update(m_hotSectorTabRect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_hotConceptTabRect.contains(event->pos())) {
        m_pressedHotTab = 2;
        update(m_hotConceptTabRect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_hotHourTabRect.contains(event->pos())) {
        m_pressedHotTab = 3;
        update(m_hotHourTabRect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_hotDayTabRect.contains(event->pos())) {
        m_pressedHotTab = 4;
        update(m_hotDayTabRect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_hotPctHeaderRect.contains(event->pos())) {
        m_pressedHotHeader = 1;
        update(m_hotPctHeaderRect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_hotInflowHeaderRect.contains(event->pos())) {
        m_pressedHotHeader = 2;
        update(m_hotInflowHeaderRect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_hotYearPctHeaderRect.contains(event->pos())) {
        m_pressedHotHeader = 3;
        update(m_hotYearPctHeaderRect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    const int pressedHotStockAction = hotStockActionAt(event->pos());
    if (event->button() == Qt::LeftButton && pressedHotStockAction >= 0) {
        m_pressedHotStockActionIndex = pressedHotStockAction;
        update(m_hotStockActionButtonRects.at(pressedHotStockAction).adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    const int pressedHotRow = hotRankRowAt(event->pos());
    if (event->button() == Qt::LeftButton && pressedHotRow >= 0) {
        m_pressedHotRowIndex = pressedHotRow;
        update(m_hotRankRowHitAreas.at(pressedHotRow).rect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_breadthDistributionTabRect.contains(event->pos())) {
        m_pressedBreadthTab = 1;
        update(m_breadthDistributionTabRect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_breadthTrendTabRect.contains(event->pos())) {
        m_pressedBreadthTab = 2;
        update(m_breadthTrendTabRect.adjusted(-2, -2, 2, 2));
        event->accept();
        return;
    }

    const int pressedIndexCell = indexCellAt(event->pos());
    if (event->button() == Qt::LeftButton && pressedIndexCell >= 0) {
        m_pressedIndexCellIndex = pressedIndexCell;
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        if (resizeHandleRect().contains(event->pos())) {
            m_resizing = true;
            m_dragging = false;
            m_resizeStartGlobalPos = event->globalPosition().toPoint();
            m_resizeStartSize = size();
            updateCursorForPosition(event->pos());
            event->accept();
            return;
        }
        m_dragging = true;
        m_resizing = false;
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}


void  MarketBreadthDetailWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (!event || testAttribute(Qt::WA_TransparentForMouseEvents)) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (event->button() == Qt::LeftButton && m_resizing) {
        m_resizing = false;
        updateCursorForPosition(event->pos());
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        hideTimelinePopup();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        const bool shouldClose = m_closeButtonPressed && m_closeButtonRect.contains(event->pos());
        m_closeButtonPressed = false;
        m_closeButtonHovered = m_closeButtonRect.contains(event->pos());
        update(m_closeButtonRect.adjusted(-2, -2, 2, 2));
        if (shouldClose) {
            hidePopup();
            event->accept();
            return;
        }

        const bool shouldRefresh = m_refreshButtonPressed && m_refreshButtonRect.contains(event->pos());
        m_refreshButtonPressed = false;
        m_refreshButtonHovered = m_refreshButtonRect.contains(event->pos());
        if (m_refreshButtonRect.isValid()) {
            update(m_refreshButtonRect.adjusted(-2, -2, 2, 2));
        }
        if (shouldRefresh) {
            hideTimelinePopup();
            triggerPopupRefresh(true);
            event->accept();
            return;
        }

        const bool activateSectorTab = m_pressedHotTab == 1 && m_hotSectorTabRect.contains(event->pos());
        const bool activateConceptTab = m_pressedHotTab == 2 && m_hotConceptTabRect.contains(event->pos());
        const bool activateHourTab = m_pressedHotTab == 3 && m_hotHourTabRect.contains(event->pos());
        const bool activateDayTab = m_pressedHotTab == 4 && m_hotDayTabRect.contains(event->pos());
        m_pressedHotTab = 0;

        if (activateSectorTab) {
            hideTimelinePopup();
            m_hotRankTabMode = HotRankTabMode::Sector;
            requestHotRankData(false);
            update();
            event->accept();
            return;
        }
        if (activateConceptTab) {
            hideTimelinePopup();
            m_hotRankTabMode = HotRankTabMode::Concept;
            requestHotRankData(true);
            update();
            event->accept();
            return;
        }
        if (activateHourTab) {
            hideTimelinePopup();
            m_hotRankTabMode = HotRankTabMode::HourHeat;
            requestStockHeatData(HotRankTabMode::HourHeat);
            update();
            event->accept();
            return;
        }
        if (activateDayTab) {
            hideTimelinePopup();
            m_hotRankTabMode = HotRankTabMode::DayHeat;
            requestStockHeatData(HotRankTabMode::DayHeat);
            update();
            event->accept();
            return;
        }

        const bool activatePctHeader = m_pressedHotHeader == 1 && m_hotPctHeaderRect.contains(event->pos());
        const bool activateInflowHeader = m_pressedHotHeader == 2 && m_hotInflowHeaderRect.contains(event->pos());
        const bool activateYearPctHeader =
            m_pressedHotHeader == 3 && m_hotYearPctHeaderRect.contains(event->pos());
        m_pressedHotHeader = 0;

        auto applyHotSort = [this](HotRankSortField field) {
            const HotRankTabMode activeMode = resolvedHotRankTabMode();
            if (m_hotRankSortField == field) {
                m_hotRankSortDescending = !m_hotRankSortDescending;
            } else {
                m_hotRankSortField = field;
                m_hotRankSortDescending = true;
            }
            if (activeMode == HotRankTabMode::Concept) {
                m_hotConceptScrollIndex = 0;
            } else {
                m_hotSectorScrollIndex = 0;
            }
        };
        if (activatePctHeader) {
            hideTimelinePopup();
            applyHotSort(HotRankSortField::Pct);
            update();
            event->accept();
            return;
        }
        if (activateInflowHeader) {
            hideTimelinePopup();
            applyHotSort(HotRankSortField::MainNetInflow);
            update();
            event->accept();
            return;
        }
        if (activateYearPctHeader) {
            hideTimelinePopup();
            applyHotSort(HotRankSortField::YearPct);
            update();
            event->accept();
            return;
        }

        const int releasedHotStockAction = hotStockActionAt(event->pos());
        const bool activateHotStockAction = m_pressedHotStockActionIndex >= 0
            && m_pressedHotStockActionIndex == releasedHotStockAction;
        const int previousPressedHotStockAction = m_pressedHotStockActionIndex;
        m_pressedHotStockActionIndex = -1;
        if (previousPressedHotStockAction >= 0
            && previousPressedHotStockAction < m_hotStockActionButtonRects.size()) {
            update(m_hotStockActionButtonRects.at(previousPressedHotStockAction).adjusted(-2, -2, 2, 2));
        }
        if (activateHotStockAction) {
            hideTimelinePopup();
            toggleHotStockWatchlistAction(releasedHotStockAction);
            event->accept();
            return;
        }

        const int releasedHotRow = hotRankRowAt(event->pos());
        const bool activateHotRow = m_pressedHotRowIndex >= 0 && m_pressedHotRowIndex == releasedHotRow;
        const int previousPressedHotRow = m_pressedHotRowIndex;
        m_pressedHotRowIndex = -1;
        if (previousPressedHotRow >= 0 && previousPressedHotRow < m_hotRankRowHitAreas.size()) {
            update(m_hotRankRowHitAreas.at(previousPressedHotRow).rect.adjusted(-2, -2, 2, 2));
        }
        if (activateHotRow && releasedHotRow >= 0 && releasedHotRow < m_hotRankRowHitAreas.size()) {
            hideTimelinePopup();
            if (isStockHeatTabMode(resolvedHotRankTabMode())) {
                const HotRankItem& item = m_hotRankRowHitAreas.at(releasedHotRow).item;
                const QString code = !item.watchCode.trimmed().isEmpty()
                    ? item.watchCode.trimmed()
                    : item.code.trimmed();
                if (!code.isEmpty() && isTimelinePopupSupportedCode(code)) {
                    ensureTimelinePopup();
                    if (m_timelinePopup) {
                        const QRect globalAnchor(
                            mapToGlobal(m_hotRankRowHitAreas.at(releasedHotRow).rect.topLeft()),
                            m_hotRankRowHitAreas.at(releasedHotRow).rect.size()
                        );
                        m_timelinePopup->showForStock(code, item.name.trimmed(), globalAnchor, width());
                    }
                }
            } else {
                openHotRankDetail(m_hotRankRowHitAreas.at(releasedHotRow).item);
            }
            event->accept();
            return;
        }

        const bool activateDistributionTab =
            m_pressedBreadthTab == 1 && m_breadthDistributionTabRect.contains(event->pos());
        const bool activateTrendTab =
            m_pressedBreadthTab == 2 && m_breadthTrendTabRect.contains(event->pos());
        m_pressedBreadthTab = 0;
        const int releasedIndexCell = indexCellAt(event->pos());
        const bool shouldShowIndexTimeline =
            m_pressedIndexCellIndex >= 0 && m_pressedIndexCellIndex == releasedIndexCell;
        m_pressedIndexCellIndex = -1;

        if (activateDistributionTab) {
            hideTimelinePopup();
            m_breadthChartTabMode = BreadthChartTabMode::Distribution;
            update();
            event->accept();
            return;
        }
        if (activateTrendTab) {
            hideTimelinePopup();
            m_breadthChartTabMode = BreadthChartTabMode::Trend;
            update();
            event->accept();
            return;
        }
        if (shouldShowIndexTimeline) {
            showTimelinePopupForIndexCell(releasedIndexCell);
            event->accept();
            return;
        }
        hideTimelinePopup();
    }

    QWidget::mouseReleaseEvent(event);
}


void  MarketBreadthDetailWindow::wheelEvent(QWheelEvent* event) {
    if (!event || testAttribute(Qt::WA_TransparentForMouseEvents)) {
        QWidget::wheelEvent(event);
        return;
    }

    const QPoint pos = event->position().toPoint();
    if (!m_hotRankListViewportRect.isValid() || !m_hotRankListViewportRect.contains(pos)) {
        QWidget::wheelEvent(event);
        return;
    }

    const HotRankTabMode activeMode = resolvedHotRankTabMode();
    const QVector<HotRankItem>& rankedSource = hotRankItemsForMode(activeMode);
    if (rankedSource.isEmpty()) {
        event->accept();
        return;
    }

    const bool useStockHeatCards = isStockHeatTabMode(activeMode);
    const int rowHeight = useStockHeatCards ? 56 : 22;
    const int rowGap = useStockHeatCards ? 6 : 2;
    const int visibleRows = qMax(1, (m_hotRankListViewportRect.height() + rowGap) / (rowHeight + rowGap));
    const int maxScrollIndex = qMax(0, rankedSource.size() - visibleRows);
    int& scrollIndex = hotRankScrollIndexForMode(activeMode);

    const int deltaY = event->angleDelta().y();
    int step = 0;
    if (deltaY < 0) {
        step = 3;
    } else if (deltaY > 0) {
        step = -3;
    } else if (event->pixelDelta().y() < 0) {
        step = 3;
    } else if (event->pixelDelta().y() > 0) {
        step = -3;
    }

    if (step != 0) {
        const int nextScrollIndex = qBound(0, scrollIndex + step, maxScrollIndex);
        if (nextScrollIndex != scrollIndex) {
            scrollIndex = nextScrollIndex;
            hideTimelinePopup();
            update();
        }
    }

    event->accept();
}


void  MarketBreadthDetailWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!event || testAttribute(Qt::WA_TransparentForMouseEvents)) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        event->accept();
        return;
    }

    QWidget::mouseDoubleClickEvent(event);
}


void  MarketBreadthDetailWindow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    m_updatedTextRect = QRect();

    {
        QPainter popupPainter(this);
        popupPainter.setRenderHint(QPainter::Antialiasing, true);
        popupPainter.setPen(Qt::NoPen);

        QColor panelBackground = m_cfg.transparentBackgroundEnabled
            ? QColor(18, 18, 18, 236)
            : m_cfg.bgColor;
        panelBackground.setAlpha(qMax(panelBackground.alpha(), 232));
        const QColor textColor = m_cfg.textColor;
        const QColor borderColor(textColor.red(), textColor.green(), textColor.blue(), 58);
        QColor cardColor = panelBackground.lighter(112);
        cardColor.setAlpha(qMin(255, panelBackground.alpha() + 8));

        popupPainter.setBrush(panelBackground);
        popupPainter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 12, 12);

        const QRect content = rect().adjusted(18, 16, -18, -16);
        m_hotSectorTabRect = QRect();
        m_hotConceptTabRect = QRect();
        m_hotHourTabRect = QRect();
        m_hotDayTabRect = QRect();
        m_hotNameHeaderRect = QRect();
        m_hotPctHeaderRect = QRect();
        m_hotInflowHeaderRect = QRect();
        m_hotYearPctHeaderRect = QRect();
        m_hotRankListViewportRect = QRect();
        m_hotRankRowHitAreas.clear();
        m_hotStockActionButtonRects.clear();
        m_breadthDistributionTabRect = QRect();
        m_breadthTrendTabRect = QRect();
        m_indexCellHitAreas.clear();

        QFont titleFont = popupPainter.font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(qMax(12.0, titleFont.pointSizeF() + 1.8));

        QFont subtitleFont = popupPainter.font();
        subtitleFont.setBold(true);
        subtitleFont.setPointSizeF(qMax(9.0, subtitleFont.pointSizeF() - 0.2));

        QFont bodyFont = popupPainter.font();
        bodyFont.setBold(false);
        bodyFont.setPointSizeF(qMax(8.8, bodyFont.pointSizeF() - 0.3));

        QFont valueFont = popupPainter.font();
        valueFont.setBold(true);
        valueFont.setPointSizeF(qMax(11.0, valueFont.pointSizeF() + 1.2));

        QFont emphasizedValueFont = valueFont;
        emphasizedValueFont.setPointSizeF(emphasizedValueFont.pointSizeF() + 2.0);

        QFont emphasizedSubtitleFont = subtitleFont;
        emphasizedSubtitleFont.setPointSizeF(emphasizedSubtitleFont.pointSizeF() + 2.0);

        const QString noDataText = i18n::t("quote.noData", m_language);
        const QString upLabel = i18n::t("popup.marketBreadth.up", m_language);
        const QString flatLabel = i18n::t("popup.marketBreadth.flat", m_language);
        const QString downLabel = i18n::t("popup.marketBreadth.down", m_language);
        const QString limitUpLabel = i18n::t("popup.marketBreadth.limitUp", m_language);
        const QString limitDownLabel = i18n::t("popup.marketBreadth.limitDown", m_language);
        const QString turnoverLabel = i18n::t("popup.marketBreadth.turnover", m_language);
        const QString updatedText = i18n::t("popup.marketBreadth.lastUpdatedFmt", m_language)
            .arg(marketBreadthLastUpdatedText(m_snapshot, m_language));

        const QRect headerRect(content.left(), content.top(), content.width(), 28);

#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
        const int closeButtonDiameter = 12;
        m_closeButtonRect = QRect(
            headerRect.left() + 8,
            headerRect.center().y() - closeButtonDiameter / 2,
            closeButtonDiameter,
            closeButtonDiameter
        );
#else
        const QSize closeButtonSize(26, 18);
        m_closeButtonRect = QRect(
            headerRect.right() - closeButtonSize.width() - 4,
            headerRect.center().y() - closeButtonSize.height() / 2,
            closeButtonSize.width(),
            closeButtonSize.height()
        );
#endif

        const QRect titleRect = headerRect.adjusted(40, 0, -40, 0);
        QRect updatedRect = headerRect;
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
        updatedRect.setLeft(m_closeButtonRect.right() + 6);
#else
        updatedRect.setRight(m_closeButtonRect.left() - 6);
#endif

        const int refreshButtonSize = 20;
        const int refreshGap = 6;
        const QFontMetrics updatedMetrics(bodyFont);
        const int updatedTextWidth = qMax(0, updatedMetrics.horizontalAdvance(updatedText));
        const int refreshPreferredLeft = updatedRect.right() - updatedTextWidth - refreshGap - refreshButtonSize + 1;
        const int refreshMinLeft = updatedRect.left();
        const int refreshMaxLeft = qMax(refreshMinLeft, updatedRect.right() - refreshButtonSize + 1);
        const int refreshLeft = qBound(refreshMinLeft, refreshPreferredLeft, refreshMaxLeft);
        m_refreshButtonRect = QRect(
            refreshLeft,
            headerRect.center().y() - refreshButtonSize / 2,
            refreshButtonSize,
            refreshButtonSize
        );

        QRect updatedTextRect = updatedRect;
        updatedTextRect.setLeft(qMin(updatedRect.right(), m_refreshButtonRect.right() + 1 + refreshGap));
        if (updatedTextRect.width() < 28) {
            m_refreshButtonRect = QRect();
            updatedTextRect = updatedRect;
        }
        m_updatedTextRect = updatedTextRect;

        popupPainter.setFont(titleFont);
        popupPainter.setPen(textColor);
        popupPainter.drawText(
            titleRect,
            Qt::AlignHCenter | Qt::AlignVCenter,
            i18n::t("popup.marketBreadth.dialogTitle", m_language)
        );

        popupPainter.setFont(bodyFont);
        popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 190));
        popupPainter.drawText(updatedTextRect, Qt::AlignRight | Qt::AlignVCenter, updatedText);

        if (m_refreshButtonRect.isValid()) {
            qint64 nowMs = 0;
            const bool refreshAnimating = isRefreshFeedbackActive(&nowMs);
            const qint64 durationMs = m_refreshFeedbackUntilMs > m_refreshFeedbackStartedMs
                ? (m_refreshFeedbackUntilMs - m_refreshFeedbackStartedMs)
                : 1;
            const qreal refreshAnimProgress = refreshAnimating
                ? qBound(
                    0.0,
                    static_cast<double>(nowMs - m_refreshFeedbackStartedMs) / static_cast<double>(durationMs),
                    1.0
                )
                : 1.0;
            const bool interactiveHeader = !testAttribute(Qt::WA_TransparentForMouseEvents);
            QColor refreshIconColor(
                textColor.red(),
                textColor.green(),
                textColor.blue(),
                interactiveHeader ? (refreshAnimating ? 234 : 208) : 145
            );
            if (interactiveHeader && m_refreshButtonHovered) {
                refreshIconColor = QColor(textColor.red(), textColor.green(), textColor.blue(), 236);
            }
            if (interactiveHeader && m_refreshButtonPressed) {
                refreshIconColor = QColor(textColor.red(), textColor.green(), textColor.blue(), 250);
            }

            if (interactiveHeader && (m_refreshButtonHovered || m_refreshButtonPressed || refreshAnimating)) {
                const int bgAlpha = m_refreshButtonPressed
                    ? 66
                    : (refreshAnimating ? 58 : 40);
                const QColor refreshBg(textColor.red(), textColor.green(), textColor.blue(), bgAlpha);
                popupPainter.setPen(QPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 96), 1.0));
                popupPainter.setBrush(refreshBg);
                popupPainter.drawRoundedRect(m_refreshButtonRect.adjusted(0, 0, -1, -1), 5, 5);
            }

            popupPainter.setBrush(Qt::NoBrush);
            QPen refreshPen(refreshIconColor, 1.35);
            refreshPen.setCapStyle(Qt::RoundCap);
            refreshPen.setJoinStyle(Qt::RoundJoin);
            popupPainter.setPen(refreshPen);

            const QPoint center = m_refreshButtonRect.center();
            const int radius = qMax(4, (qMin(m_refreshButtonRect.width(), m_refreshButtonRect.height()) / 2) - 5);
            const QRect arcRect(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
            constexpr qreal kRefreshArcStartDeg = 38.0;
            constexpr qreal kRefreshArcSpanDeg = 286.0;
            const int rotateDegrees = refreshAnimating
                ? qRound(refreshAnimProgress * 320.0)
                : 0;
            popupPainter.save();
            if (rotateDegrees != 0) {
                popupPainter.translate(center);
                popupPainter.rotate(rotateDegrees);
                popupPainter.translate(-center);
            }
            popupPainter.drawArc(
                arcRect,
                qRound(kRefreshArcStartDeg * 16.0),
                qRound(kRefreshArcSpanDeg * 16.0)
            );

            const auto pointAtAngle = [&](qreal distance, qreal angleDeg) {
                constexpr qreal kPi = 3.14159265358979323846;
                const qreal radians = angleDeg * kPi / 180.0;
                return QPointF(
                    center.x() + distance * std::cos(radians),
                    center.y() - distance * std::sin(radians)
                );
            };

            const qreal arrowTipAngle = kRefreshArcStartDeg;
            const QPointF arrowTip = pointAtAngle(static_cast<qreal>(radius), arrowTipAngle);
            // Clockwise tangent at the tip = arrowTipAngle - 90°
            const qreal arrowDirDeg = arrowTipAngle - 90.0;
            // Wing directions: opposite of arrow direction ± spread
            constexpr qreal kPi2 = 3.14159265358979323846;
            const qreal armLen = qMax<qreal>(3.0, static_cast<qreal>(radius) * 0.52);
            const qreal backDeg = arrowDirDeg + 180.0;
            constexpr qreal arrowSpreadDeg = 28.0;
            const qreal wA_rad = (backDeg + arrowSpreadDeg) * kPi2 / 180.0;
            const qreal wB_rad = (backDeg - arrowSpreadDeg) * kPi2 / 180.0;
            // Offset from tip (same coord convention: cos for x, -sin for y)
            const QPointF arrowWingA(arrowTip.x() + armLen * std::cos(wA_rad),
                                     arrowTip.y() - armLen * std::sin(wA_rad));
            const QPointF arrowWingB(arrowTip.x() + armLen * std::cos(wB_rad),
                                     arrowTip.y() - armLen * std::sin(wB_rad));
            popupPainter.drawLine(arrowTip, arrowWingA);
            popupPainter.drawLine(arrowTip, arrowWingB);
            popupPainter.restore();
        }

        if (!testAttribute(Qt::WA_TransparentForMouseEvents)) {
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
            QColor closeButtonColor(QStringLiteral("#ff5f57"));
            if (m_closeButtonHovered) {
                closeButtonColor = closeButtonColor.lighter(108);
            }
            if (m_closeButtonPressed) {
                closeButtonColor = closeButtonColor.darker(112);
            }

            popupPainter.setPen(QPen(QColor(0, 0, 0, 80), 1.0));
            popupPainter.setBrush(closeButtonColor);
            popupPainter.drawEllipse(m_closeButtonRect.adjusted(0, 0, -1, -1));

            if (m_closeButtonHovered || m_closeButtonPressed) {
                popupPainter.setPen(QPen(QColor(80, 32, 24, 220), 1.2));
                const int x1 = m_closeButtonRect.left() + 4;
                const int x2 = m_closeButtonRect.right() - 4;
                const int y1 = m_closeButtonRect.top() + 4;
                const int y2 = m_closeButtonRect.bottom() - 4;
                popupPainter.drawLine(QPoint(x1, y1), QPoint(x2, y2));
                popupPainter.drawLine(QPoint(x1, y2), QPoint(x2, y1));
            }
#else
            QColor closeButtonBg(textColor.red(), textColor.green(), textColor.blue(), 30);
            QColor closeButtonBorder(textColor.red(), textColor.green(), textColor.blue(), 88);
            QColor closeIconColor(textColor.red(), textColor.green(), textColor.blue(), 210);
            if (m_closeButtonHovered) {
                closeButtonBg = QColor(232, 17, 35, 220);
                closeButtonBorder = QColor(255, 255, 255, 80);
                closeIconColor = QColor(255, 255, 255, 240);
            }
            if (m_closeButtonPressed) {
                closeButtonBg = QColor(201, 12, 31, 230);
                closeButtonBorder = QColor(255, 255, 255, 96);
                closeIconColor = QColor(255, 255, 255, 250);
            }

            popupPainter.setPen(QPen(closeButtonBorder, 1.0));
            popupPainter.setBrush(closeButtonBg);
            popupPainter.drawRoundedRect(m_closeButtonRect.adjusted(0, 0, -1, -1), 3, 3);

            popupPainter.setPen(QPen(closeIconColor, 1.35));
            const int x1 = m_closeButtonRect.left() + 8;
            const int x2 = m_closeButtonRect.right() - 8;
            const int y1 = m_closeButtonRect.top() + 5;
            const int y2 = m_closeButtonRect.bottom() - 5;
            popupPainter.drawLine(QPoint(x1, y1), QPoint(x2, y2));
            popupPainter.drawLine(QPoint(x1, y2), QPoint(x2, y1));
#endif
        }

        constexpr int kIndexCardHeight = 142;
        constexpr int kIndexSectionGap = 14;
        const QRect bodyRect = content.adjusted(0, 36, 0, -(kIndexSectionGap + kIndexCardHeight));
        const QRect indexCardRect(
            content.left(),
            content.bottom() - kIndexCardHeight + 1,
            content.width(),
            kIndexCardHeight
        );
        const int splitGap = 18;
        const int leftWidth = qMax(120, (bodyRect.width() - splitGap) / 2);
        const QRect leftRect(bodyRect.left(), bodyRect.top(), leftWidth, bodyRect.height());
        const QRect rightRect(
            leftRect.right() + 1 + splitGap,
            bodyRect.top(),
            qMax(120, bodyRect.width() - leftWidth - splitGap),
            bodyRect.height()
        );

        auto drawCard = [&](const QRect& cardRect, const QString& cardTitle, Qt::Alignment titleAlignment) {
            popupPainter.setPen(QPen(borderColor, 1.0));
            popupPainter.setBrush(cardColor);
            popupPainter.drawRoundedRect(cardRect, 10, 10);

            popupPainter.setFont(subtitleFont);
            popupPainter.setPen(textColor);
            popupPainter.drawText(
                cardRect.adjusted(12, 8, -12, -8),
                titleAlignment,
                cardTitle
            );
        };

        auto drawToggleTab = [&](const QRect& rect,
                                 const QString& label,
                                 bool active,
                                 bool hovered,
                                 bool hasData,
                                 const QFont& tabFont) {
            const int borderAlpha = active ? 165 : (hovered ? 145 : 105);
            const QColor border(textColor.red(), textColor.green(), textColor.blue(), borderAlpha);
            const QColor fill = active
                ? QColor(textColor.red(), textColor.green(), textColor.blue(), 42)
                : (hovered
                    ? QColor(textColor.red(), textColor.green(), textColor.blue(), 22)
                    : QColor(0, 0, 0, 0));
            popupPainter.setPen(border);
            popupPainter.setBrush(fill);
            popupPainter.drawRoundedRect(rect, 8, 8);

            popupPainter.setFont(tabFont);
            const QColor labelColor = active
                ? QColor(255, 255, 255, hasData ? 245 : 175)
                : QColor(
                    textColor.red(),
                    textColor.green(),
                    textColor.blue(),
                    hasData ? (hovered ? 195 : 175) : 120
                );
            popupPainter.setPen(labelColor);
            popupPainter.drawText(rect, Qt::AlignCenter, label);
        };

        const int leftGap = 10;
        const int maxSummaryCardHeight = qMax(128, leftRect.height() - leftGap - 96);
        const int summaryCardHeight = qBound(
            128,
            qRound(static_cast<double>(leftRect.height()) * 0.39),
            maxSummaryCardHeight
        );
        const QRect summaryCard(leftRect.left(), leftRect.top(), leftRect.width(), summaryCardHeight);
        const QRect distributionCard(
            leftRect.left(),
            summaryCard.bottom() + 1 + leftGap,
            leftRect.width(),
            qMax(84, leftRect.bottom() - (summaryCard.bottom() + leftGap))
        );
        const QRect hotRankCard(rightRect.left(), rightRect.top(), rightRect.width(), rightRect.height());

        drawCard(
            summaryCard,
            QString(),
            Qt::AlignHCenter | Qt::AlignTop
        );
        drawCard(distributionCard, QString(), Qt::AlignLeft | Qt::AlignTop);
        drawCard(hotRankCard, QString(), Qt::AlignLeft | Qt::AlignTop);

        const QRect summaryInner = summaryCard.adjusted(12, 10, -12, -8);
        const int summarySectionGap = 6;
        const int topSectionHeight = qMax(50, (summaryInner.height() - summarySectionGap) / 2);
        const QRect trendSectionRect(
            summaryInner.left(),
            summaryInner.top(),
            summaryInner.width(),
            topSectionHeight
        );
        const QRect turnoverSectionRect(
            summaryInner.left(),
            trendSectionRect.bottom() + 1 + summarySectionGap,
            summaryInner.width(),
            qMax(56, summaryInner.bottom() - (trendSectionRect.bottom() + summarySectionGap))
        );

        QPen sectionDividerPen(
            QColor(textColor.red(), textColor.green(), textColor.blue(), 72),
            0.75,
            Qt::DashLine
        );
        sectionDividerPen.setDashPattern({7.0, 5.0});
        sectionDividerPen.setCapStyle(Qt::RoundCap);
        sectionDividerPen.setCosmetic(true);
        popupPainter.setPen(sectionDividerPen);
        popupPainter.drawLine(
            summaryInner.left(),
            trendSectionRect.bottom() + summarySectionGap / 2,
            summaryInner.right(),
            trendSectionRect.bottom() + summarySectionGap / 2
        );

        QFont sectionTitleFont = subtitleFont;
        sectionTitleFont.setPointSizeF(qMax(9.2, sectionTitleFont.pointSizeF() + 0.3));
        popupPainter.setFont(sectionTitleFont);
        popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 210));
        popupPainter.drawText(
            QRect(trendSectionRect.left(), trendSectionRect.top(), trendSectionRect.width(), 14),
            Qt::AlignHCenter | Qt::AlignVCenter,
            i18n::t("quote.marketBreadth", m_language)
        );
        popupPainter.drawText(
            QRect(turnoverSectionRect.left(), turnoverSectionRect.top(), turnoverSectionRect.width(), 14),
            Qt::AlignHCenter | Qt::AlignVCenter,
            turnoverLabel
        );

        const QRect trendInner = trendSectionRect.adjusted(0, 13, 0, 0);
        const QStringList trendLabels {
            upLabel,
            flatLabel,
            downLabel,
            limitUpLabel,
            limitDownLabel,
        };
        const QVector<int> trendValues {
            qMax(0, m_snapshot.upCount),
            qMax(0, m_snapshot.flatCount),
            qMax(0, m_snapshot.downCount),
            qMax(0, m_snapshot.limitUpCount),
            qMax(0, m_snapshot.limitDownCount),
        };
        const QVector<QColor> trendColors {
            m_cfg.upColor,
            m_cfg.flatColor,
            m_cfg.downColor,
            QColor(QStringLiteral("#bb07ae")),
            QColor(QStringLiteral("#fc7d02")),
        };

        const int trendCount = trendLabels.size();
        const int trendGap = 5;
        const int trendCellWidth = qMax(
            24,
            (trendInner.width() - trendGap * qMax(0, trendCount - 1)) / qMax(1, trendCount)
        );
        const int trendLabelHeight = 13;
        const int trendValueHeight = 18;
        const int trendRowGap = 1;
        const int trendBlockHeight = trendLabelHeight + trendRowGap + trendValueHeight;
        const int trendBlockTop = trendInner.top() + qMax(0, (trendInner.height() - trendBlockHeight) / 2);
        popupPainter.setFont(subtitleFont);
        for (int i = 0; i < trendCount; ++i) {
            const int x = trendInner.left() + i * (trendCellWidth + trendGap);
            const QRect labelRect(x, trendBlockTop, trendCellWidth, trendLabelHeight);
            popupPainter.setPen(trendColors.value(i, textColor));
            popupPainter.drawText(labelRect, Qt::AlignCenter, trendLabels.at(i));
        }
        popupPainter.setFont(emphasizedValueFont);
        for (int i = 0; i < trendCount; ++i) {
            const int x = trendInner.left() + i * (trendCellWidth + trendGap);
            const QRect valueRect(
                x,
                trendBlockTop + trendLabelHeight + trendRowGap,
                trendCellWidth,
                trendValueHeight
            );
            popupPainter.setPen(trendColors.value(i, textColor));
            popupPainter.drawText(valueRect, Qt::AlignCenter, QString::number(trendValues.value(i)));
        }

        const QRect turnoverInner = turnoverSectionRect.adjusted(0, 13, 0, 0);
        const double estimatedFullDay = estimateAshareFullDayTurnover(m_snapshot);
        const QColor compareColor = marketBreadthTurnoverChangeColor(m_snapshot.turnoverChange, m_cfg);
        const QStringList turnoverLabels {
            i18n::t("popup.marketBreadth.turnoverToday", m_language),
            i18n::t("popup.marketBreadth.turnoverPre", m_language),
            i18n::t("popup.marketBreadth.turnoverDelta", m_language),
            i18n::t("popup.marketBreadth.turnoverForecast", m_language),
        };
        const QStringList turnoverValues {
            m_snapshot.turnoverValid ? formatChineseMarketAmount(m_snapshot.turnover) : noDataText,
            m_snapshot.turnoverValid ? formatChineseMarketAmount(m_snapshot.turnoverPre) : noDataText,
            m_snapshot.turnoverValid ? formatChineseMarketAmount(m_snapshot.turnoverChange) : noDataText,
            (m_snapshot.turnoverValid && std::isfinite(estimatedFullDay))
                ? formatChineseMarketAmount(estimatedFullDay)
                : noDataText,
        };

        const int turnoverCount = turnoverLabels.size();
        const int turnoverGap = 5;
        const int turnoverCellWidth = qMax(
            42,
            (turnoverInner.width() - turnoverGap * qMax(0, turnoverCount - 1)) / qMax(1, turnoverCount)
        );
        const int turnoverLabelHeight = 20;
        const int turnoverValueHeight = 20;
        const int turnoverRowGap = 1;
        const int turnoverBlockHeight = turnoverLabelHeight + turnoverRowGap + turnoverValueHeight;
        const int turnoverBlockTop = turnoverInner.top() + qMax(0, (turnoverInner.height() - turnoverBlockHeight) / 2);

        popupPainter.setFont(bodyFont);
        for (int i = 0; i < turnoverCount; ++i) {
            const int x = turnoverInner.left() + i * (turnoverCellWidth + turnoverGap);
            const QRect labelRect(x, turnoverBlockTop, turnoverCellWidth, turnoverLabelHeight);
            popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 205));
            popupPainter.drawText(labelRect, Qt::AlignCenter | Qt::TextWordWrap, turnoverLabels.at(i));
        }

        popupPainter.setFont(emphasizedSubtitleFont);
        for (int i = 0; i < turnoverCount; ++i) {
            const int x = turnoverInner.left() + i * (turnoverCellWidth + turnoverGap);
            const QRect valueRect(
                x,
                turnoverBlockTop + turnoverLabelHeight + turnoverRowGap,
                turnoverCellWidth,
                turnoverValueHeight
            );
            popupPainter.setPen(i == 2 ? compareColor : textColor);
            popupPainter.drawText(valueRect, Qt::AlignCenter | Qt::TextWordWrap, turnoverValues.at(i));
        }

        const QRect noteInner = hotRankCard.adjusted(12, 10, -12, -12);
        const bool hasSectorData = !m_hotSectorsRanked.isEmpty() || !m_hotSectors.isEmpty();
        const bool hasConceptData = !m_hotConceptsRanked.isEmpty() || !m_hotConcepts.isEmpty();
        const bool hasHourHeatData = !m_hotHourStocks.isEmpty();
        const bool hasDayHeatData = !m_hotDayStocks.isEmpty();
        const HotRankTabMode activeHotMode = resolvedHotRankTabMode();
        const bool useStockHeatCards = isStockHeatTabMode(activeHotMode);
        const QVector<HotRankItem>& rankedSource = hotRankItemsForMode(activeHotMode);

        QFont tabFont = bodyFont;
        tabFont.setBold(true);
        tabFont.setPointSizeF(qMax(8.0, tabFont.pointSizeF() - 0.2));
        const int tabHeight = 24;
        const int tabGap = 6;
        const int tabCount = 4;
        const QRect tabBarRect(noteInner.left(), noteInner.top(), noteInner.width(), tabHeight);
        const int tabWidth = qMax(62, (tabBarRect.width() - tabGap * (tabCount - 1)) / tabCount);
        const int tabsTotalWidth = tabWidth * tabCount + tabGap * (tabCount - 1);
        const int tabsStartX = tabBarRect.left() + qMax(0, (tabBarRect.width() - tabsTotalWidth) / 2);
        const QRect sectorTabRect(tabsStartX, tabBarRect.top(), tabWidth, tabHeight);
        const QRect conceptTabRect(sectorTabRect.right() + 1 + tabGap, tabBarRect.top(), tabWidth, tabHeight);
        const QRect hourTabRect(conceptTabRect.right() + 1 + tabGap, tabBarRect.top(), tabWidth, tabHeight);
        const QRect dayTabRect(hourTabRect.right() + 1 + tabGap, tabBarRect.top(), tabWidth, tabHeight);
        m_hotSectorTabRect = sectorTabRect;
        m_hotConceptTabRect = conceptTabRect;
        m_hotHourTabRect = hourTabRect;
        m_hotDayTabRect = dayTabRect;

        drawToggleTab(
            sectorTabRect,
            i18n::t("quote.hotSector", m_language),
            activeHotMode == HotRankTabMode::Sector,
            m_hotSectorTabHovered,
            hasSectorData,
            tabFont
        );
        drawToggleTab(
            conceptTabRect,
            i18n::t("quote.hotConcept", m_language),
            activeHotMode == HotRankTabMode::Concept,
            m_hotConceptTabHovered,
            hasConceptData,
            tabFont
        );
        drawToggleTab(
            hourTabRect,
            i18n::t("popup.marketBreadth.hotHour", m_language),
            activeHotMode == HotRankTabMode::HourHeat,
            m_hotHourTabHovered,
            hasHourHeatData,
            tabFont
        );
        drawToggleTab(
            dayTabRect,
            i18n::t("popup.marketBreadth.hotDay", m_language),
            activeHotMode == HotRankTabMode::DayHeat,
            m_hotDayTabHovered,
            hasDayHeatData,
            tabFont
        );

        const auto formatHotChange = [](double value) {
            if (!std::isfinite(value)) {
                return QStringLiteral("--");
            }
            QString text = QString::number(value, 'f', 2);
            if (value > 0.0) {
                text.prepend('+');
            }
            text.append('%');
            return text;
        };
        const auto formatHotInflow = [this](double value) {
            if (!std::isfinite(value)) {
                return QStringLiteral("--");
            }
            const bool isChinese = m_language.startsWith(QLatin1String("zh"), Qt::CaseInsensitive);
            double scaled = value;
            QString unit;
            int precision = 2;

            if (isChinese) {
                scaled = value / 100000000.0;
                unit = QStringLiteral("亿");
            } else {
                const double absValue = std::abs(value);
                if (absValue >= 1000000000.0) {
                    scaled = value / 1000000000.0;
                    unit = QStringLiteral("B");
                } else if (absValue >= 1000000.0) {
                    scaled = value / 1000000.0;
                    unit = QStringLiteral("M");
                } else if (absValue >= 1000.0) {
                    scaled = value / 1000.0;
                    unit = QStringLiteral("K");
                } else {
                    precision = 0;
                }
            }

            QString text = QString::number(scaled, 'f', precision);
            if (scaled > 0.0) {
                text.prepend('+');
            }
            text.append(unit);
            return text;
        };
        const auto formatStockHeat = [this](double value) {
            if (!std::isfinite(value)) {
                return QStringLiteral("--");
            }

            const bool isChinese = m_language.startsWith(QLatin1String("zh"), Qt::CaseInsensitive);
            const double absValue = std::abs(value);
            if (isChinese) {
                if (absValue >= 10000.0) {
                    return QStringLiteral("%1万热度").arg(QString::number(value / 10000.0, 'f', 2));
                }
                return QStringLiteral("%1热度").arg(QString::number(value, 'f', 0));
            }

            if (absValue >= 1000.0) {
                return QStringLiteral("%1K heat").arg(QString::number(value / 1000.0, 'f', 1));
            }
            return QStringLiteral("%1 heat").arg(QString::number(value, 'f', 0));
        };

        const int rankTopGap = 8;
        const QRect rankArea(
            noteInner.left(),
            tabBarRect.bottom() + 1 + rankTopGap,
            noteInner.width(),
            qMax(40, noteInner.bottom() - (tabBarRect.bottom() + rankTopGap))
        );

        const int scrollBarWidth = 6;
        if (!useStockHeatCards) {
            const QRect rankHeaderRect(rankArea.left(), rankArea.top(), rankArea.width(), 22);
            const QRect rankViewportRect(
                rankArea.left(),
                rankHeaderRect.bottom() + 1 + 6,
                rankArea.width(),
                qMax(20, rankArea.bottom() - (rankHeaderRect.bottom() + 6))
            );
            m_hotRankListViewportRect = rankViewportRect;

            popupPainter.setFont(bodyFont);
            popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 92));
            popupPainter.drawLine(rankHeaderRect.bottomLeft(), rankHeaderRect.bottomRight());

            const int columnGap = 8;
            const bool showScrollBar = !rankedSource.isEmpty();
            const int tableContentWidth = qMax(
                80,
                rankViewportRect.width() - (showScrollBar ? (scrollBarWidth + 8) : 0)
            );
            const int tableLeft = rankViewportRect.left();

            QFont headerFont = bodyFont;
            headerFont.setBold(true);
            headerFont.setPointSizeF(qMax(8.0, headerFont.pointSizeF() - 0.1));
            const QFontMetrics headerMetrics(headerFont);
            QFont rowFont = bodyFont;
            rowFont.setBold(false);
            rowFont.setPointSizeF(qMax(8.4, rowFont.pointSizeF()));
            const QFontMetrics rowMetrics(rowFont);

            int changeWidth = qMax(
                46,
                headerMetrics.horizontalAdvance(i18n::t("popup.marketBreadth.hotChange", m_language)) + 12
            );
            int inflowWidth = qMax(
                68,
                headerMetrics.horizontalAdvance(i18n::t("popup.marketBreadth.hotNetInflow", m_language)) + 12
            );
            int yearPctWidth = qMax(
                62,
                headerMetrics.horizontalAdvance(i18n::t("popup.marketBreadth.hotYearPct", m_language)) + 12
            );
            const int probeCount = qMin(rankedSource.size(), 48);
            for (int row = 0; row < probeCount; ++row) {
                changeWidth = qMax(
                    changeWidth,
                    rowMetrics.horizontalAdvance(formatHotChange(rankedSource.at(row).pct)) + 12
                );
                inflowWidth = qMax(
                    inflowWidth,
                    rowMetrics.horizontalAdvance(formatHotInflow(rankedSource.at(row).mainNetInflow)) + 12
                );
                yearPctWidth = qMax(
                    yearPctWidth,
                    rowMetrics.horizontalAdvance(formatHotChange(rankedSource.at(row).yearPct)) + 12
                );
            }
            changeWidth = qMin(changeWidth, qMax(52, tableContentWidth / 5));
            inflowWidth = qMin(inflowWidth, qMax(78, tableContentWidth / 3));
            yearPctWidth = qMin(yearPctWidth, qMax(58, tableContentWidth / 5));

            int nameWidth = tableContentWidth - changeWidth - inflowWidth - yearPctWidth - columnGap * 3;
            if (nameWidth < 48) {
                int overflow = 48 - nameWidth;
                const int cutInflow = qMin(overflow, qMax(0, inflowWidth - 78));
                inflowWidth -= cutInflow;
                overflow -= cutInflow;
                const int cutYearPct = qMin(overflow, qMax(0, yearPctWidth - 58));
                yearPctWidth -= cutYearPct;
                overflow -= cutYearPct;
                const int cutChange = qMin(overflow, qMax(0, changeWidth - 52));
                changeWidth -= cutChange;
                overflow -= cutChange;
                nameWidth = qMax(
                    48,
                    tableContentWidth - changeWidth - inflowWidth - yearPctWidth - columnGap * 3
                );
            }
            const QRect nameHeaderRect(tableLeft, rankHeaderRect.top(), nameWidth, rankHeaderRect.height());
            const QRect changeHeaderRect(
                nameHeaderRect.right() + 1 + columnGap,
                rankHeaderRect.top(),
                changeWidth,
                rankHeaderRect.height()
            );
            const QRect inflowHeaderRect(
                changeHeaderRect.right() + 1 + columnGap,
                rankHeaderRect.top(),
                inflowWidth,
                rankHeaderRect.height()
            );
            const QRect yearPctHeaderRect(
                inflowHeaderRect.right() + 1 + columnGap,
                rankHeaderRect.top(),
                yearPctWidth,
                rankHeaderRect.height()
            );
            m_hotNameHeaderRect = nameHeaderRect;
            m_hotPctHeaderRect = changeHeaderRect;
            m_hotInflowHeaderRect = inflowHeaderRect;
            m_hotYearPctHeaderRect = yearPctHeaderRect;

            const auto sortIndicator = [this](HotRankSortField field) {
                if (m_hotRankSortField != field) {
                    return QString();
                }
                return m_hotRankSortDescending ? QStringLiteral(" v") : QStringLiteral(" ^");
            };

            auto headerTextColor = [&](bool hovered, HotRankSortField field) {
                if (m_hotRankSortField == field) {
                    return QColor(255, 255, 255, hovered ? 250 : 235);
                }
                return QColor(
                    textColor.red(),
                    textColor.green(),
                    textColor.blue(),
                    hovered ? 215 : 190
                );
            };

            popupPainter.setFont(headerFont);
            popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 190));
            popupPainter.drawText(
                nameHeaderRect.adjusted(1, 0, -2, 0),
                Qt::AlignVCenter | Qt::AlignLeft,
                i18n::t("popup.marketBreadth.hotName", m_language)
            );
            popupPainter.setPen(headerTextColor(m_hotPctHeaderHovered, HotRankSortField::Pct));
            popupPainter.drawText(
                changeHeaderRect,
                Qt::AlignCenter,
                i18n::t("popup.marketBreadth.hotChange", m_language) + sortIndicator(HotRankSortField::Pct)
            );
            popupPainter.setPen(headerTextColor(m_hotInflowHeaderHovered, HotRankSortField::MainNetInflow));
            popupPainter.drawText(
                inflowHeaderRect.adjusted(2, 0, -1, 0),
                Qt::AlignVCenter | Qt::AlignRight,
                i18n::t("popup.marketBreadth.hotNetInflow", m_language)
                    + sortIndicator(HotRankSortField::MainNetInflow)
            );
            popupPainter.setPen(headerTextColor(m_hotYearPctHeaderHovered, HotRankSortField::YearPct));
            popupPainter.drawText(
                yearPctHeaderRect,
                Qt::AlignCenter,
                i18n::t("popup.marketBreadth.hotYearPct", m_language) + sortIndicator(HotRankSortField::YearPct)
            );

            if (rankedSource.isEmpty()) {
                popupPainter.setFont(bodyFont);
                popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 140));
                popupPainter.drawText(rankViewportRect, Qt::AlignCenter, i18n::t("quote.noData", m_language));
            } else {
                QVector<HotRankItem> displayItems = rankedSource;
                std::stable_sort(
                    displayItems.begin(),
                    displayItems.end(),
                    [this](const HotRankItem& lhs, const HotRankItem& rhs) {
                        auto compareDouble = [this](double a, double b) {
                            const bool aFinite = std::isfinite(a);
                            const bool bFinite = std::isfinite(b);
                            if (aFinite != bFinite) {
                                return aFinite;
                            }
                            if (!aFinite && !bFinite) {
                                return false;
                            }
                            if (qFuzzyCompare(a + 1.0, b + 1.0)) {
                                return false;
                            }
                            return m_hotRankSortDescending ? (a > b) : (a < b);
                        };

                        switch (m_hotRankSortField) {
                        case HotRankSortField::MainNetInflow:
                            return compareDouble(lhs.mainNetInflow, rhs.mainNetInflow);
                        case HotRankSortField::YearPct:
                            return compareDouble(lhs.yearPct, rhs.yearPct);
                        case HotRankSortField::Pct:
                        default:
                            return compareDouble(lhs.pct, rhs.pct);
                        }
                    }
                );

                constexpr int kHotListRowHeight = 22;
                constexpr int kHotListRowGap = 2;
                const int visibleRows = qMax(
                    1,
                    (rankViewportRect.height() + kHotListRowGap) / (kHotListRowHeight + kHotListRowGap)
                );
                int& scrollIndex = hotRankScrollIndexForMode(activeHotMode);
                scrollIndex = qBound(0, scrollIndex, qMax(0, displayItems.size() - visibleRows));
                const int endIndex = qMin(displayItems.size(), scrollIndex + visibleRows);

                popupPainter.save();
                popupPainter.setClipRect(rankViewportRect);
                popupPainter.setFont(rowFont);
                for (int row = scrollIndex; row < endIndex; ++row) {
                    const int visibleRow = row - scrollIndex;
                    const QRect rowRect(
                        rankViewportRect.left(),
                        rankViewportRect.top() + visibleRow * (kHotListRowHeight + kHotListRowGap),
                        tableContentWidth,
                        kHotListRowHeight
                    );
                    const QRect nameRect(rowRect.left(), rowRect.top(), nameWidth, rowRect.height());
                    const QRect changeRect(
                        nameRect.right() + 1 + columnGap,
                        rowRect.top(),
                        changeWidth,
                        rowRect.height()
                    );
                    const QRect inflowRect(
                        changeRect.right() + 1 + columnGap,
                        rowRect.top(),
                        inflowWidth,
                        rowRect.height()
                    );
                    const QRect yearPctRect(
                        inflowRect.right() + 1 + columnGap,
                        rowRect.top(),
                        yearPctWidth,
                        rowRect.height()
                    );
                    const HotRankItem& item = displayItems.at(row);
                    const int hitAreaIndex = m_hotRankRowHitAreas.size();
                    const bool rowHovered = (hitAreaIndex == m_hoveredHotRowIndex);
                    const bool rowPressed = (hitAreaIndex == m_pressedHotRowIndex);
                    m_hotRankRowHitAreas.push_back({rowRect, item});

                    if (rowHovered || rowPressed) {
                        popupPainter.setPen(Qt::NoPen);
                        popupPainter.setBrush(QColor(
                            textColor.red(),
                            textColor.green(),
                            textColor.blue(),
                            rowPressed ? 36 : 24
                        ));
                        popupPainter.drawRoundedRect(rowRect.adjusted(0, 0, -1, 0), 5, 5);
                    }

                    const QColor changeColor = std::isfinite(item.pct)
                        ? (item.pct > 0.0 ? m_cfg.upColor : (item.pct < 0.0 ? m_cfg.downColor : m_cfg.flatColor))
                        : textColor;
                    const QColor inflowColor = std::isfinite(item.mainNetInflow)
                        ? (item.mainNetInflow > 0.0
                            ? m_cfg.upColor
                            : (item.mainNetInflow < 0.0 ? m_cfg.downColor : m_cfg.flatColor))
                        : textColor;
                    const QColor yearPctColor = std::isfinite(item.yearPct)
                        ? (item.yearPct > 0.0 ? m_cfg.upColor : (item.yearPct < 0.0 ? m_cfg.downColor : m_cfg.flatColor))
                        : textColor;

                    popupPainter.setPen(textColor);
                    popupPainter.drawText(
                        nameRect.adjusted(1, 0, -2, 0),
                        Qt::AlignVCenter | Qt::AlignLeft,
                        rowMetrics.elidedText(item.name.trimmed(), Qt::ElideRight, qMax(8, nameRect.width() - 2))
                    );
                    popupPainter.setPen(changeColor);
                    popupPainter.drawText(changeRect, Qt::AlignCenter, formatHotChange(item.pct));
                    popupPainter.setPen(inflowColor);
                    popupPainter.drawText(
                        inflowRect.adjusted(2, 0, -1, 0),
                        Qt::AlignVCenter | Qt::AlignRight,
                        rowMetrics.elidedText(
                            formatHotInflow(item.mainNetInflow),
                            Qt::ElideRight,
                            qMax(8, inflowRect.width() - 2)
                        )
                    );
                    popupPainter.setPen(yearPctColor);
                    popupPainter.drawText(yearPctRect, Qt::AlignCenter, formatHotChange(item.yearPct));

                    if (row + 1 < endIndex) {
                        QPen gridPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 42));
                        gridPen.setStyle(Qt::DashLine);
                        gridPen.setWidthF(0.6);
                        gridPen.setCosmetic(true);
                        popupPainter.setPen(gridPen);
                        const int lineY = rowRect.bottom() + qMax(1, kHotListRowGap / 2);
                        popupPainter.drawLine(rowRect.left(), lineY, rowRect.right(), lineY);
                    }
                }
                popupPainter.restore();

                if (displayItems.size() > visibleRows) {
                    const QRect trackRect(
                        rankViewportRect.right() - scrollBarWidth + 1,
                        rankViewportRect.top(),
                        scrollBarWidth,
                        rankViewportRect.height()
                    );
                    popupPainter.setPen(Qt::NoPen);
                    popupPainter.setBrush(QColor(textColor.red(), textColor.green(), textColor.blue(), 28));
                    popupPainter.drawRoundedRect(trackRect, 3, 3);

                    const double ratio = static_cast<double>(visibleRows) / static_cast<double>(displayItems.size());
                    const int thumbHeight = qMax(20, qRound(trackRect.height() * ratio));
                    const int maxThumbTop = qMax(0, trackRect.height() - thumbHeight);
                    const int maxScrollIndex = qMax(1, displayItems.size() - visibleRows);
                    const int thumbTop = trackRect.top() + qRound(
                        static_cast<double>(scrollIndex) / static_cast<double>(maxScrollIndex) * maxThumbTop
                    );
                    const QRect thumbRect(trackRect.left(), thumbTop, trackRect.width(), thumbHeight);
                    popupPainter.setBrush(QColor(textColor.red(), textColor.green(), textColor.blue(), 86));
                    popupPainter.drawRoundedRect(thumbRect, 3, 3);
                }
            }
        } else {
            const QRect rankViewportRect = rankArea.adjusted(0, 2, 0, 0);
            m_hotRankListViewportRect = rankViewportRect;

            if (rankedSource.isEmpty()) {
                popupPainter.setFont(bodyFont);
                popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 140));
                popupPainter.drawText(rankViewportRect, Qt::AlignCenter, i18n::t("quote.noData", m_language));
            } else {
                QFont titleRowFont = bodyFont;
                titleRowFont.setBold(true);
                titleRowFont.setPointSizeF(qMax(9.2, titleRowFont.pointSizeF() + 0.2));
                QFont tagFont = bodyFont;
                tagFont.setBold(false);
                tagFont.setPointSizeF(qMax(7.6, tagFont.pointSizeF() - 0.8));
                const QFontMetrics titleMetrics(titleRowFont);
                const QFontMetrics valueMetrics(bodyFont);
                const QFontMetrics tagMetrics(tagFont);
                const bool showScrollBar = rankedSource.size() > 1;
                const int listContentWidth = qMax(
                    120,
                    rankViewportRect.width() - (showScrollBar ? (scrollBarWidth + 8) : 0)
                );
                constexpr int kStockHeatCardHeight = 50;
                constexpr int kStockHeatCardGap = 6;
                const int visibleRows = qMax(
                    1,
                    (rankViewportRect.height() + kStockHeatCardGap) / (kStockHeatCardHeight + kStockHeatCardGap)
                );
                int& scrollIndex = hotRankScrollIndexForMode(activeHotMode);
                scrollIndex = qBound(0, scrollIndex, qMax(0, rankedSource.size() - visibleRows));
                const int endIndex = qMin(rankedSource.size(), scrollIndex + visibleRows);

                popupPainter.save();
                popupPainter.setClipRect(rankViewportRect);
                for (int row = scrollIndex; row < endIndex; ++row) {
                    const HotRankItem& item = rankedSource.at(row);
                    const int visibleRow = row - scrollIndex;
                    const QRect cardRect(
                        rankViewportRect.left(),
                        rankViewportRect.top() + visibleRow * (kStockHeatCardHeight + kStockHeatCardGap),
                        listContentWidth,
                        kStockHeatCardHeight
                    );
                    const int hitAreaIndex = m_hotRankRowHitAreas.size();
                    const bool rowHovered = hitAreaIndex == m_hoveredHotRowIndex;
                    const bool rowPressed = hitAreaIndex == m_pressedHotRowIndex;
                    m_hotRankRowHitAreas.push_back({cardRect, item});

                    QColor cardFill = cardColor;
                    cardFill.setAlpha(rowPressed ? 78 : (rowHovered ? 64 : 48));
                    QColor cardBorder(textColor.red(), textColor.green(), textColor.blue(), rowHovered ? 96 : 56);
                    popupPainter.setPen(QPen(cardBorder, 1.0));
                    popupPainter.setBrush(cardFill);
                    popupPainter.drawRoundedRect(cardRect.adjusted(0, 0, -1, -1), 7, 7);

                    const QRect cardInner = cardRect.adjusted(10, 6, -10, -6);
                    const QString heatText = formatStockHeat(item.heat);
                    const QString pctText = formatHotChange(item.pct);
                    const int actionWidth = 28;
                    const QRect actionRect(
                        cardInner.right() - actionWidth + 1,
                        cardInner.top() + 1,
                        actionWidth,
                        18
                    );
                    const int actionIndex = m_hotStockActionButtonRects.size();
                    m_hotStockActionButtonRects.push_back(actionRect);
                    const bool actionHovered = actionIndex == m_hoveredHotStockActionIndex;
                    const bool actionPressed = actionIndex == m_pressedHotStockActionIndex;
                    const int pctWidth = qBound(52, valueMetrics.horizontalAdvance(pctText) + 8, 68);
                    const int heatWidth = qBound(74, valueMetrics.horizontalAdvance(heatText) + 10, 96);
                    const int topGap = 6;
                    const QRect titleRect(
                        cardInner.left(),
                        cardInner.top(),
                        qMax(90, actionRect.left() - cardInner.left() - pctWidth - heatWidth - topGap * 2),
                        18
                    );
                    const QRect heatRect(
                        titleRect.right() + 1 + topGap,
                        cardInner.top(),
                        heatWidth,
                        18
                    );
                    const QRect pctRect(
                        heatRect.right() + 1 + topGap,
                        cardInner.top(),
                        pctWidth,
                        18
                    );
                    const QRect tagsRect(
                        cardInner.left(),
                        titleRect.bottom() + 1 + 6,
                        qMax(80, actionRect.left() - cardInner.left() - 6),
                        18
                    );

                    popupPainter.setFont(titleRowFont);
                    popupPainter.setPen(textColor);
                    popupPainter.drawText(
                        titleRect,
                        Qt::AlignVCenter | Qt::AlignLeft,
                        titleMetrics.elidedText(item.name.trimmed(), Qt::ElideRight, titleRect.width())
                    );

                    popupPainter.setFont(bodyFont);
                    popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 220));
                    popupPainter.drawText(heatRect, Qt::AlignVCenter | Qt::AlignRight, heatText);
                    popupPainter.setPen(
                        std::isfinite(item.pct)
                            ? (item.pct > 0.0 ? m_cfg.upColor : (item.pct < 0.0 ? m_cfg.downColor : m_cfg.flatColor))
                            : textColor
                    );
                    popupPainter.drawText(pctRect, Qt::AlignVCenter | Qt::AlignRight, pctText);

                    const bool tracked = containsHotStockWatchlistItem(item);
                    const QString actionText = item.watchCode.trimmed().isEmpty()
                        ? QStringLiteral("--")
                        : (tracked ? QStringLiteral("-") : QStringLiteral("+"));
                    QColor actionButtonBg(textColor.red(), textColor.green(), textColor.blue(), 28);
                    QColor actionButtonBorder(textColor.red(), textColor.green(), textColor.blue(), 88);
                    QColor actionButtonText = tracked ? m_cfg.downColor : m_cfg.upColor;
                    if (item.watchCode.trimmed().isEmpty()) {
                        actionButtonText = QColor(textColor.red(), textColor.green(), textColor.blue(), 120);
                    }
                    if (actionHovered) {
                        actionButtonBg = QColor(textColor.red(), textColor.green(), textColor.blue(), 42);
                    }
                    if (actionPressed) {
                        actionButtonBg = QColor(textColor.red(), textColor.green(), textColor.blue(), 60);
                    }

                    popupPainter.setPen(QPen(actionButtonBorder, 1.0));
                    popupPainter.setBrush(actionButtonBg);
                    popupPainter.drawRoundedRect(actionRect.adjusted(2, 0, -2, 0), 4, 4);
                    popupPainter.setPen(actionButtonText);
                    popupPainter.drawText(actionRect, Qt::AlignCenter, actionText);

                    popupPainter.setFont(tagFont);
                    int tagX = tagsRect.left();
                    const int tagSpacing = 6;
                    const int maxTags = qMin(item.tags.size(), 3);
                    for (int tagIndex = 0; tagIndex < maxTags; ++tagIndex) {
                        const QString tagText = item.tags.at(tagIndex).trimmed();
                        if (tagText.isEmpty()) {
                            continue;
                        }
                        const int pillWidth = tagMetrics.horizontalAdvance(tagText) + 14;
                        if (tagX + pillWidth > tagsRect.right() + 1) {
                            break;
                        }
                        const QRect pillRect(tagX, tagsRect.top(), pillWidth, tagsRect.height());
                        popupPainter.setPen(QPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 66), 1.0));
                        popupPainter.setBrush(QColor(textColor.red(), textColor.green(), textColor.blue(), 18));
                        popupPainter.drawRoundedRect(pillRect.adjusted(0, 0, -1, -1), 8, 8);
                        popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 188));
                        popupPainter.drawText(pillRect, Qt::AlignCenter, tagText);
                        tagX = pillRect.right() + 1 + tagSpacing;
                    }
                }
                popupPainter.restore();

                if (rankedSource.size() > visibleRows) {
                    const QRect trackRect(
                        rankViewportRect.right() - scrollBarWidth + 1,
                        rankViewportRect.top(),
                        scrollBarWidth,
                        rankViewportRect.height()
                    );
                    popupPainter.setPen(Qt::NoPen);
                    popupPainter.setBrush(QColor(textColor.red(), textColor.green(), textColor.blue(), 28));
                    popupPainter.drawRoundedRect(trackRect, 3, 3);

                    const double ratio = static_cast<double>(visibleRows) / static_cast<double>(rankedSource.size());
                    const int thumbHeight = qMax(20, qRound(trackRect.height() * ratio));
                    const int maxThumbTop = qMax(0, trackRect.height() - thumbHeight);
                    const int maxScrollIndex = qMax(1, rankedSource.size() - visibleRows);
                    const int thumbTop = trackRect.top() + qRound(
                        static_cast<double>(scrollIndex) / static_cast<double>(maxScrollIndex) * maxThumbTop
                    );
                    const QRect thumbRect(trackRect.left(), thumbTop, trackRect.width(), thumbHeight);
                    popupPainter.setBrush(QColor(textColor.red(), textColor.green(), textColor.blue(), 86));
                    popupPainter.drawRoundedRect(thumbRect, 3, 3);
                }
            }
        }

        const QColor axisColor(textColor.red(), textColor.green(), textColor.blue(), 165);
        const QRect breadthInner = distributionCard.adjusted(12, 10, -12, -12);
        const bool hasDistributionData = m_snapshot.distributionValid && !m_snapshot.distribution.isEmpty();
        const bool hasTrendData = m_snapshot.overviewTimeline.size() >= 2;

        QFont breadthTabFont = bodyFont;
        breadthTabFont.setBold(true);
        breadthTabFont.setPointSizeF(qMax(8.0, breadthTabFont.pointSizeF() - 0.2));

        const int breadthTabHeight = 24;
        const int breadthTabGap = 8;
        const QRect breadthTabBarRect(
            breadthInner.left(),
            breadthInner.top(),
            breadthInner.width(),
            breadthTabHeight
        );
        const int breadthTabWidth = qMax(72, (breadthTabBarRect.width() - breadthTabGap) / 2);
        const int breadthTabsTotalWidth = breadthTabWidth * 2 + breadthTabGap;
        const int breadthTabsStartX =
            breadthTabBarRect.left() + qMax(0, (breadthTabBarRect.width() - breadthTabsTotalWidth) / 2);
        const QRect breadthTrendTabRect(
            breadthTabsStartX,
            breadthTabBarRect.top(),
            breadthTabWidth,
            breadthTabHeight
        );
        const QRect breadthDistributionTabRect(
            breadthTrendTabRect.right() + 1 + breadthTabGap,
            breadthTabBarRect.top(),
            breadthTabWidth,
            breadthTabHeight
        );
        m_breadthDistributionTabRect = breadthDistributionTabRect;
        m_breadthTrendTabRect = breadthTrendTabRect;

        drawToggleTab(
            breadthTrendTabRect,
            i18n::t("popup.marketBreadth.trend", m_language),
            m_breadthChartTabMode == BreadthChartTabMode::Trend,
            m_breadthTrendTabHovered,
            hasTrendData,
            breadthTabFont
        );
        drawToggleTab(
            breadthDistributionTabRect,
            i18n::t("popup.marketBreadth.distribution", m_language),
            m_breadthChartTabMode == BreadthChartTabMode::Distribution,
            m_breadthDistributionTabHovered,
            hasDistributionData,
            breadthTabFont
        );

        const int breadthContentTopGap = 8;
        const QRect breadthContentRect(
            breadthInner.left(),
            breadthTabBarRect.bottom() + 1 + breadthContentTopGap,
            breadthInner.width(),
            qMax(40, breadthInner.bottom() - (breadthTabBarRect.bottom() + breadthContentTopGap))
        );

        if (m_breadthChartTabMode == BreadthChartTabMode::Distribution) {
            const QRect distributionInner = breadthContentRect;
            const QRect distributionPlotRect = distributionInner.adjusted(34, 8, -8, -24);
            if (hasDistributionData
                && distributionPlotRect.width() > 24
                && distributionPlotRect.height() > 24) {
                int maxValue = 0;
                for (const MarketBreadthDistributionItem& item : m_snapshot.distribution) {
                    maxValue = qMax(maxValue, item.value);
                }

                if (maxValue > 0) {
                    constexpr int distributionYTickStep = 1000;
                    const int distributionYAxisMax = qMax(
                        distributionYTickStep,
                        ((maxValue + distributionYTickStep - 1) / distributionYTickStep) * distributionYTickStep
                    );

                    const auto distributionYAt = [&](int value) {
                        const double ratio = static_cast<double>(qMax(0, value))
                            / static_cast<double>(distributionYAxisMax);
                        return distributionPlotRect.bottom() - qRound(ratio * distributionPlotRect.height());
                    };

                    popupPainter.setPen(QPen(axisColor, 1.0));
                    popupPainter.drawLine(distributionPlotRect.bottomLeft(), distributionPlotRect.bottomRight());
                    popupPainter.drawLine(distributionPlotRect.bottomLeft(), distributionPlotRect.topLeft());

                    popupPainter.setFont(bodyFont);
                    popupPainter.setPen(axisColor);
                    for (int tickValue = 0; tickValue <= distributionYAxisMax; tickValue += distributionYTickStep) {
                        popupPainter.drawText(
                            QRect(distributionInner.left(), distributionYAt(tickValue) - 6, 30, 12),
                            Qt::AlignRight | Qt::AlignVCenter,
                            QString::number(tickValue)
                        );
                    }

                    const int barCount = m_snapshot.distribution.size();
                    const int gap = barCount > 18 ? 1 : 2;
                    const int totalGap = gap * qMax(0, barCount - 1);
                    const int availableWidth = qMax(1, distributionPlotRect.width() - totalGap);
                    const int baseBarWidth = qMax(1, availableWidth / qMax(1, barCount));
                    const int valueLabelTopPadding = 12;
                    const int barPlotHeight = qMax(1, distributionPlotRect.height() - valueLabelTopPadding);
                    QVector<int> barCenters;
                    QVector<QRect> barRects;
                    QVector<int> barValues;
                    barCenters.reserve(barCount);
                    barRects.reserve(barCount);
                    barValues.reserve(barCount);
                    int x = distributionPlotRect.left();
                    for (int i = 0; i < barCount; ++i) {
                        const int value = qMax(0, m_snapshot.distribution.at(i).value);
                        const int barHeight = qMax(
                            1,
                            qRound(
                                static_cast<double>(value)
                                / static_cast<double>(distributionYAxisMax)
                                * barPlotHeight
                            )
                        );
                        const int isLast = (i == barCount - 1) ? 1 : 0;
                        const int barWidth = isLast
                            ? qMax(1, distributionPlotRect.right() - x + 1)
                            : baseBarWidth;
                        const QRect barRect(
                            x,
                            distributionPlotRect.bottom() - barHeight + 1,
                            barWidth,
                            barHeight
                        );

                        QColor barColor;
                        if (i < barCount / 2) {
                            barColor = m_cfg.upColor;
                        } else if (barCount % 2 == 1 && i == barCount / 2) {
                            barColor = m_cfg.flatColor;
                        } else {
                            barColor = m_cfg.downColor;
                        }
                        barColor.setAlpha(205);
                        popupPainter.setPen(Qt::NoPen);
                        popupPainter.setBrush(barColor);
                        popupPainter.drawRoundedRect(barRect, 1.5, 1.5);
                        barCenters.push_back(barRect.center().x());
                        barRects.push_back(barRect);
                        barValues.push_back(value);

                        x += barWidth + gap;
                    }

                    popupPainter.setPen(axisColor);
                    popupPainter.setFont(bodyFont);
                    const int labelY = distributionPlotRect.bottom() + 1;

                    const int visibleLabelCount = qMin(6, barCount);
                    int lastIndex = -1;
                    for (int i = 0; i < visibleLabelCount; ++i) {
                        const int bucketIndex = (visibleLabelCount == 1)
                            ? 0
                            : qRound(
                                static_cast<double>(i) * static_cast<double>(barCount - 1)
                                / static_cast<double>(visibleLabelCount - 1)
                            );
                        if (bucketIndex == lastIndex) {
                            continue;
                        }
                        lastIndex = bucketIndex;

                        const int centerX = barCenters.value(bucketIndex, distributionPlotRect.left());
                        popupPainter.drawText(
                            QRect(centerX - 24, labelY, 48, 12),
                            Qt::AlignHCenter | Qt::AlignVCenter,
                            m_snapshot.distribution.at(bucketIndex).bucket
                        );
                    }

                    QFont valueLabelFont = bodyFont;
                    qreal valueLabelPointSize = qMax(6.0, valueLabelFont.pointSizeF() - 1.6);
                    if (barCount > 18 || baseBarWidth < 10) {
                        valueLabelPointSize = qMax(5.5, valueLabelPointSize - 0.6);
                    }
                    valueLabelFont.setPointSizeF(valueLabelPointSize);
                    popupPainter.setFont(valueLabelFont);

                    const QFontMetrics valueMetrics(valueLabelFont);
                    const int valueLabelHeight = qMax(10, valueMetrics.height());
                    for (int index = 0; index < barCount; ++index) {
                        const QRect barRect = barRects.at(index);
                        const int value = barValues.at(index);
                        const QString valueText = QString::number(value);
                        const int textWidth =
                            qMax(barRect.width() + 4, valueMetrics.horizontalAdvance(valueText) + 2);
                        QRect valueRect(
                            barRect.center().x() - textWidth / 2,
                            barRect.top() - valueLabelHeight - 1,
                            textWidth,
                            valueLabelHeight
                        );

                        if (valueRect.top() < distributionPlotRect.top()) {
                            valueRect.moveTop(distributionPlotRect.top());
                        }
                        if (valueRect.left() < distributionPlotRect.left() - 2) {
                            valueRect.moveLeft(distributionPlotRect.left() - 2);
                        }
                        if (valueRect.right() > distributionPlotRect.right() + 2) {
                            valueRect.moveRight(distributionPlotRect.right() + 2);
                        }

                        popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 205));
                        popupPainter.drawText(valueRect, Qt::AlignHCenter | Qt::AlignVCenter, valueText);
                    }
                } else {
                    popupPainter.setPen(textColor);
                    popupPainter.setFont(bodyFont);
                    popupPainter.drawText(distributionPlotRect, Qt::AlignCenter, noDataText);
                }
            } else {
                popupPainter.setPen(textColor);
                popupPainter.setFont(bodyFont);
                popupPainter.drawText(distributionInner, Qt::AlignCenter, noDataText);
            }
        } else {
            const QRect timelineInner = breadthContentRect;
            const QRect timelineLegendRect(
                timelineInner.left(),
                timelineInner.top(),
                timelineInner.width(),
                14
            );
            const QRect timelinePlotRect = timelineInner.adjusted(34, 20, -8, -30);

            if (hasTrendData && timelinePlotRect.width() > 24 && timelinePlotRect.height() > 24) {
                int maxCount = 0;
                for (const MarketBreadthTimelinePoint& point : m_snapshot.overviewTimeline) {
                    maxCount = qMax(maxCount, qMax(point.riseCount, point.fallCount));
                }

                if (maxCount > 0) {
                    const QStringList& xAxisLabels = hardcodedAshareIntradayXAxis();
                    const QHash<QString, int>& xAxisIndex = hardcodedAshareIntradayXAxisIndex();
                    const int axisCount = xAxisLabels.size();
                    const QTimeZone bjZone("Asia/Shanghai");
                    constexpr int yTickStep = 1000;
                    const int yAxisMax = qMax(yTickStep, ((maxCount + yTickStep - 1) / yTickStep) * yTickStep);

                    const auto xAt = [&](int axisIndexValue) {
                        if (axisCount <= 1) {
                            return timelinePlotRect.left();
                        }
                        const double t = static_cast<double>(axisIndexValue)
                            / static_cast<double>(axisCount - 1);
                        return timelinePlotRect.left() + qRound(t * timelinePlotRect.width());
                    };
                    const auto yAt = [&](int value) {
                        const double ratio = static_cast<double>(qMax(0, value))
                            / static_cast<double>(yAxisMax);
                        return timelinePlotRect.bottom() - qRound(ratio * timelinePlotRect.height());
                    };

                    auto buildSeriesPoints = [&](int seriesKind) {
                        QVector<QPoint> points;
                        points.reserve(m_snapshot.overviewTimeline.size());
                        for (const MarketBreadthTimelinePoint& timelinePoint : m_snapshot.overviewTimeline) {
                            int value = (seriesKind == 0) ? timelinePoint.riseCount : timelinePoint.fallCount;
                            if (value < 0) {
                                continue;
                            }

                            QDateTime ts = QDateTime::fromMSecsSinceEpoch(timelinePoint.timestampMs, bjZone);
                            if (!ts.isValid()) {
                                ts = QDateTime::fromMSecsSinceEpoch(timelinePoint.timestampMs);
                            }
                            const QString hhmm = ts.toString(QStringLiteral("HH:mm"));
                            const int axisIdx = xAxisIndex.value(hhmm, -1);
                            if (axisIdx < 0) {
                                continue;
                            }

                            points.push_back(QPoint(xAt(axisIdx), yAt(value)));
                        }
                        return points;
                    };

                    const QVector<QPoint> risePoints = buildSeriesPoints(0);
                    const QVector<QPoint> fallPoints = buildSeriesPoints(1);

                    popupPainter.setPen(QPen(axisColor, 1.0));
                    popupPainter.drawLine(timelinePlotRect.bottomLeft(), timelinePlotRect.bottomRight());
                    popupPainter.drawLine(timelinePlotRect.bottomLeft(), timelinePlotRect.topLeft());

                    popupPainter.setFont(bodyFont);
                    popupPainter.setPen(axisColor);
                    for (int tickValue = 0; tickValue <= yAxisMax; tickValue += yTickStep) {
                        popupPainter.drawText(
                            QRect(timelineInner.left(), yAt(tickValue) - 6, 30, 12),
                            Qt::AlignRight | Qt::AlignVCenter,
                            QString::number(tickValue)
                        );
                    }

                    const int xLabelY = timelinePlotRect.bottom() + 2;
                    const int xLabelHeight = 14;
                    const int leftWidth = 52;
                    const int middleWidth = 90;
                    const int rightWidth = 52;
                    const int leftLabelLeft = timelinePlotRect.left() - 2;
                    const int rightLabelLeft = timelinePlotRect.right() - rightWidth + 2;

                    int amCloseIdx = xAxisIndex.value(QStringLiteral("11:30"), -1);
                    int pmOpenIdx = xAxisIndex.value(QStringLiteral("13:00"), -1);
                    if (pmOpenIdx < 0) {
                        pmOpenIdx = xAxisIndex.value(QStringLiteral("13:01"), -1);
                    }

                    int middleAnchorX = timelinePlotRect.center().x();
                    if (amCloseIdx >= 0 && pmOpenIdx >= 0) {
                        middleAnchorX = (xAt(amCloseIdx) + xAt(pmOpenIdx)) / 2;
                    } else if (amCloseIdx >= 0) {
                        middleAnchorX = xAt(amCloseIdx);
                    } else if (pmOpenIdx >= 0) {
                        middleAnchorX = xAt(pmOpenIdx);
                    }

                    const int minMiddleLeft = leftLabelLeft + leftWidth + 4;
                    const int maxMiddleLeft = rightLabelLeft - middleWidth - 4;
                    const int middleLabelLeft = maxMiddleLeft >= minMiddleLeft
                        ? qBound(minMiddleLeft, middleAnchorX - middleWidth / 2, maxMiddleLeft)
                        : (leftLabelLeft + rightLabelLeft - middleWidth) / 2;

                    if (middleAnchorX > timelinePlotRect.left() && middleAnchorX < timelinePlotRect.right()) {
                        QPen middayPen(QColor(axisColor.red(), axisColor.green(), axisColor.blue(), 115), 0.9);
                        middayPen.setStyle(Qt::DashLine);
                        middayPen.setDashPattern({4.0, 4.0});
                        middayPen.setCosmetic(true);
                        popupPainter.setPen(middayPen);
                        popupPainter.drawLine(
                            QPoint(middleAnchorX, timelinePlotRect.top()),
                            QPoint(middleAnchorX, timelinePlotRect.bottom())
                        );
                    }

                    popupPainter.drawText(
                        QRect(leftLabelLeft, xLabelY, leftWidth, xLabelHeight),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        QStringLiteral("9:30")
                    );
                    popupPainter.drawText(
                        QRect(middleLabelLeft, xLabelY, middleWidth, xLabelHeight),
                        Qt::AlignHCenter | Qt::AlignVCenter,
                        QStringLiteral("11:30/13:00")
                    );
                    popupPainter.drawText(
                        QRect(rightLabelLeft, xLabelY, rightWidth, xLabelHeight),
                        Qt::AlignRight | Qt::AlignVCenter,
                        QStringLiteral("15:00")
                    );

                    struct SeriesItem {
                        QString label;
                        QColor color;
                        QVector<QPoint> points;
                    };
                    const QVector<SeriesItem> series {
                        {upLabel, m_cfg.upColor, risePoints},
                        {downLabel, m_cfg.downColor, fallPoints},
                    };

                    const int legendItemWidth = 62;
                    const int legendTotalWidth = legendItemWidth * series.size();
                    int legendX = timelineLegendRect.center().x() - legendTotalWidth / 2;
                    popupPainter.setFont(bodyFont);
                    for (const SeriesItem& item : series) {
                        popupPainter.setPen(QPen(item.color, 1.5));
                        popupPainter.drawLine(
                            legendX,
                            timelineLegendRect.center().y(),
                            legendX + 12,
                            timelineLegendRect.center().y()
                        );
                        popupPainter.setPen(textColor);
                        popupPainter.drawText(
                            QRect(legendX + 14, timelineLegendRect.top(), 52, timelineLegendRect.height()),
                            Qt::AlignLeft | Qt::AlignVCenter,
                            item.label
                        );
                        legendX += 62;
                    }

                    popupPainter.setRenderHint(QPainter::Antialiasing, true);
                    for (const SeriesItem& item : series) {
                        if (item.points.size() < 2) {
                            continue;
                        }
                        popupPainter.setPen(QPen(item.color, 1.3));
                        for (int i = 1; i < item.points.size(); ++i) {
                            popupPainter.drawLine(item.points.at(i - 1), item.points.at(i));
                        }
                    }
                } else {
                    popupPainter.setPen(textColor);
                    popupPainter.setFont(bodyFont);
                    popupPainter.drawText(timelineInner, Qt::AlignCenter, noDataText);
                }
            } else {
                popupPainter.setPen(textColor);
                popupPainter.setFont(bodyFont);
                popupPainter.drawText(timelineInner, Qt::AlignCenter, noDataText);
            }
        }

        // ── Index Quotes Section ─────────────────────────────────────────────
        drawCard(indexCardRect, QString(), Qt::AlignLeft | Qt::AlignTop);

        const QRect indexInner = indexCardRect.adjusted(10, 10, -10, -10);

        constexpr int kIdxRowH = 58;
        constexpr int kIdxRowGap = 6;
        constexpr int kIdxColumns = 7;
        constexpr int kIdxCount = 14;
        constexpr int kIdxCellGap = 6;

        const int rowsTop = indexInner.top();
        const int totalCellGap = (kIdxColumns - 1) * kIdxCellGap;
        const int cellWidth = qMax(60, (indexInner.width() - totalCellGap) / kIdxColumns);

        static const char* const kIdxNames[kIdxCount] = {
            "上证指数", "深证成指", "创业板指", "科创50", "A股均价",
            "恒生指数", "恒生科技",
            "富时中国A50", "美元/人民币", "黄金T+D", "纳斯达克", "标普500", "日经225", "韩国KOSPI"
        };

        QFont idxNameFont = subtitleFont;
        idxNameFont.setPointSizeF(qMax(7.8, idxNameFont.pointSizeF() - 0.6));
        QFont idxPriceFont = bodyFont;
        idxPriceFont.setPointSizeF(qMax(8.0, idxPriceFont.pointSizeF() - 0.3));
        QFont idxChangeFont = bodyFont;
        idxChangeFont.setPointSizeF(qMax(7.4, idxChangeFont.pointSizeF() - 0.5));

        const auto indexPricePrecision = [](const IndexQuoteItem& item) -> int {
            if (item.displayName == QStringLiteral("美元/人民币")) {
                return 4;
            }
            if (item.displayName == QStringLiteral("黄金T+D")) {
                return 2;
            }
            const double absP = std::fabs(item.price);
            if (absP < 1.0) {
                return 4;
            }
            if (absP < 10.0) {
                return 3;
            }
            return 2;
        };

        const auto indexChangePrecision = [](const IndexQuoteItem& item) -> int {
            if (item.displayName == QStringLiteral("美元/人民币")) {
                return 4;
            }
            const double absChg = std::fabs(item.change);
            if (absChg < 1.0) {
                return 4;
            }
            return 2;
        };

        const auto fmtIndexPrice = [indexPricePrecision](const IndexQuoteItem& item) -> QString {
            if (!std::isfinite(item.price)) {
                return QStringLiteral("--");
            }
            return QString::number(item.price, 'f', indexPricePrecision(item));
        };

        const auto fmtIndexChange = [indexChangePrecision](const IndexQuoteItem& item) -> QString {
            if (!std::isfinite(item.change) || !std::isfinite(item.pct)) {
                return QStringLiteral("--");
            }
            QString chgStr = QString::number(item.change, 'f', indexChangePrecision(item));
            if (item.change > 0.0) {
                chgStr.prepend('+');
            }
            QString pctStr = QString::number(item.pct, 'f', 2);
            if (item.pct > 0.0) {
                pctStr.prepend('+');
            }
            pctStr.append('%');
            return chgStr + QStringLiteral(" ") + pctStr;
        };

        const QColor cellBorderColor(
            textColor.red(), textColor.green(), textColor.blue(), 45
        );

        for (int i = 0; i < kIdxCount; ++i) {
            const int row = i / kIdxColumns;
            const int col = i % kIdxColumns;

            const int cellLeft = indexInner.left() + col * (cellWidth + kIdxCellGap);
            const int cellTop = rowsTop + row * (kIdxRowH + kIdxRowGap);
            const QRect cellRect(cellLeft, cellTop, cellWidth, kIdxRowH);

            // Cell border
            popupPainter.setPen(QPen(cellBorderColor, 0.4));
            popupPainter.setBrush(Qt::NoBrush);
            popupPainter.drawRoundedRect(cellRect, 6, 6);

            const QString name = QString::fromUtf8(kIdxNames[i]);

            IndexQuoteItem dummy;
            const IndexQuoteItem& item = (i < m_indexQuotes.size()) ? m_indexQuotes.at(i) : dummy;
            m_indexCellHitAreas.push_back({
                cellRect,
                item.code.trimmed(),
                item.displayName.trimmed().isEmpty() ? name : item.displayName.trimmed()
            });

            QColor itemColor = textColor;
            if (std::isfinite(item.pct)) {
                if (item.pct > 0.0) {
                    itemColor = m_cfg.upColor;
                } else if (item.pct < 0.0) {
                    itemColor = m_cfg.downColor;
                } else {
                    itemColor = m_cfg.flatColor;
                }
            }

            constexpr int kLineH = 14;
            constexpr int kLineGap = 3;
            const int totalH = kLineH * 3 + kLineGap * 2;
            const int lineTop = cellTop + (kIdxRowH - totalH) / 2;

            // Name line
            popupPainter.setFont(idxNameFont);
            popupPainter.setPen(textColor);
            popupPainter.drawText(
                QRect(cellLeft, lineTop, cellWidth, kLineH),
                Qt::AlignHCenter | Qt::AlignVCenter,
                name
            );

            // Price line
            popupPainter.setFont(idxPriceFont);
            popupPainter.setPen(itemColor);
            popupPainter.drawText(
                QRect(cellLeft, lineTop + kLineH + kLineGap, cellWidth, kLineH),
                Qt::AlignHCenter | Qt::AlignVCenter,
                fmtIndexPrice(item)
            );

            // Change line
            popupPainter.setFont(idxChangeFont);
            popupPainter.setPen(itemColor);
            popupPainter.drawText(
                QRect(cellLeft, lineTop + (kLineH + kLineGap) * 2, cellWidth, kLineH),
                Qt::AlignHCenter | Qt::AlignVCenter,
                fmtIndexChange(item)
            );
        }

        if (!testAttribute(Qt::WA_TransparentForMouseEvents)) {
            const QRect gripRect = resizeHandleRect().adjusted(5, 5, -3, -3);
            const QColor gripColor(textColor.red(), textColor.green(), textColor.blue(), 116);
            popupPainter.setPen(QPen(gripColor, 1.1, Qt::SolidLine, Qt::RoundCap));
            popupPainter.drawLine(
                QPoint(gripRect.right() - 12, gripRect.bottom()),
                QPoint(gripRect.right(), gripRect.bottom() - 12)
            );
            popupPainter.drawLine(
                QPoint(gripRect.right() - 8, gripRect.bottom()),
                QPoint(gripRect.right(), gripRect.bottom() - 8)
            );
            popupPainter.drawLine(
                QPoint(gripRect.right() - 4, gripRect.bottom()),
                QPoint(gripRect.right(), gripRect.bottom() - 4)
            );
        }
        // ── End Index Quotes Section ──────────────────────────────────────────

        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    QColor background = m_cfg.transparentBackgroundEnabled
        ? QColor(18, 18, 18, 236)
        : m_cfg.bgColor;
    background.setAlpha(qMax(background.alpha(), 236));
    const QColor textColor = m_cfg.textColor;

    painter.setBrush(background);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 12, 12);

    const QRect content = rect().adjusted(12, 10, -12, -10);
    const int columnGap = 8;
    const int colWidth = (content.width() - columnGap * 2) / 3;
    const int headerY = content.top();
    const int countY = headerY + 18;
    const int sectionTitleY = countY + 28;
    const int sectionValueY = sectionTitleY + 16;
    const int limitStatsY = sectionValueY + 24;
    const int distributionTitleY = limitStatsY + 18;
    const int distributionTopY = distributionTitleY + 16;
    const int distributionBottomY = distributionTopY + 48;
    const int trendMarginY = 8;
    const int trendTopY = distributionBottomY + trendMarginY;
    const int trendBottomY = content.bottom() - 10;

    const QString upLabel = i18n::t("popup.marketBreadth.up", m_language);
    const QString flatLabel = i18n::t("popup.marketBreadth.flat", m_language);
    const QString downLabel = i18n::t("popup.marketBreadth.down", m_language);
    const QString turnoverLabel = i18n::t("popup.marketBreadth.turnover", m_language);
    const QString limitUpLabel = i18n::t("popup.marketBreadth.limitUp", m_language);
    const QString limitDownLabel = i18n::t("popup.marketBreadth.limitDown", m_language);
    const QString distributionLabel = i18n::t("popup.marketBreadth.distribution", m_language);
    const QString comparePrefix = i18n::t(
        "popup.marketBreadth.vsYesterdayFmt",
        m_language
    ).arg(QString());
    const QString compareWord = marketBreadthTurnoverChangeText(m_snapshot.turnoverChange, m_language);
    const QColor compareColor = marketBreadthTurnoverChangeColor(m_snapshot.turnoverChange, m_cfg);

    const QRect upRect(content.left(), headerY, colWidth, 14);
    const QRect flatRect(upRect.right() + 1 + columnGap, headerY, colWidth, 14);
    const QRect downRect(flatRect.right() + 1 + columnGap, headerY, colWidth, 14);

    QFont headerFont = painter.font();
    headerFont.setBold(true);
    headerFont.setPointSizeF(qMax(9.0, headerFont.pointSizeF() - 0.5));
    painter.setFont(headerFont);
    painter.setPen(m_cfg.upColor);
    painter.drawText(upRect, Qt::AlignCenter, upLabel);
    painter.setPen(m_cfg.flatColor);
    painter.drawText(flatRect, Qt::AlignCenter, flatLabel);
    painter.setPen(m_cfg.downColor);
    painter.drawText(downRect, Qt::AlignCenter, downLabel);

    QFont valueFont = painter.font();
    valueFont.setBold(false);
    valueFont.setPointSizeF(qMax(10.0, valueFont.pointSizeF() + 1.0));
    QFont emphasizedValueFont = valueFont;
    emphasizedValueFont.setPointSizeF(emphasizedValueFont.pointSizeF() + 2.0);

    painter.setFont(emphasizedValueFont);
    painter.setPen(m_cfg.upColor);
    painter.drawText(QRect(upRect.left(), countY, upRect.width(), 20), Qt::AlignCenter, QString::number(m_snapshot.upCount));
    painter.setPen(textColor);
    painter.drawText(QRect(flatRect.left(), countY, flatRect.width(), 20), Qt::AlignCenter, QString::number(m_snapshot.flatCount));
    painter.setPen(m_cfg.downColor);
    painter.drawText(QRect(downRect.left(), countY, downRect.width(), 20), Qt::AlignCenter, QString::number(m_snapshot.downCount));

    const int lowerColumnGap = 12;
    const int lowerColWidth = (content.width() - lowerColumnGap) / 2;
    const QRect turnoverTitleRect(content.left(), sectionTitleY, lowerColWidth, 16);
    const QRect compareTitleRect(content.left() + lowerColWidth + lowerColumnGap, sectionTitleY, lowerColWidth, 16);
    const QRect turnoverValueRect(content.left(), sectionValueY, lowerColWidth, 20);
    const QRect compareValueRect(content.left() + lowerColWidth + lowerColumnGap, sectionValueY, lowerColWidth, 20);

    painter.setFont(headerFont);
    painter.setPen(textColor);
    painter.drawText(turnoverTitleRect, Qt::AlignCenter, turnoverLabel);

    QFontMetrics headerMetrics(headerFont);
    const int prefixWidth = headerMetrics.horizontalAdvance(comparePrefix);
    const int wordWidth = headerMetrics.horizontalAdvance(compareWord);
    const int combinedWidth = prefixWidth + wordWidth;
    const int compareCombinedLeft = compareTitleRect.left() + qMax(0, (compareTitleRect.width() - combinedWidth) / 2);

    painter.setPen(textColor);
    painter.drawText(
        QRect(compareCombinedLeft, compareTitleRect.top(), prefixWidth, compareTitleRect.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        comparePrefix
    );
    painter.setPen(compareColor);
    painter.drawText(
        QRect(compareCombinedLeft + prefixWidth, compareTitleRect.top(), wordWidth, compareTitleRect.height()),
        Qt::AlignLeft | Qt::AlignVCenter,
        compareWord
    );

    painter.setFont(emphasizedValueFont);
    painter.setPen(textColor);
    painter.drawText(
        turnoverValueRect,
        Qt::AlignCenter,
        formatChineseMarketAmount(m_snapshot.turnover)
    );
    painter.setPen(compareColor);
    painter.drawText(
        compareValueRect,
        Qt::AlignCenter,
        formatChineseMarketAmount(m_snapshot.turnoverChange)
    );

    painter.setFont(valueFont);

    const QRect limitUpRect(content.left(), limitStatsY, lowerColWidth, 14);
    const QRect limitDownRect(
        content.left() + lowerColWidth + lowerColumnGap,
        limitStatsY,
        lowerColWidth,
        14
    );
    QFont limitFont = painter.font();
    limitFont.setBold(true);
    limitFont.setPointSizeF(qMax(8.0, limitFont.pointSizeF() - 0.8));
    painter.setFont(limitFont);
    painter.setPen(m_cfg.upColor);
    painter.drawText(
        limitUpRect,
        Qt::AlignCenter,
        QStringLiteral("%1 %2").arg(limitUpLabel).arg(m_snapshot.limitUpCount)
    );
    painter.setPen(m_cfg.downColor);
    painter.drawText(
        limitDownRect,
        Qt::AlignCenter,
        QStringLiteral("%1 %2").arg(limitDownLabel).arg(m_snapshot.limitDownCount)
    );

    painter.setFont(headerFont);
    painter.setPen(textColor);
    painter.drawText(
        QRect(content.left(), distributionTitleY, content.width(), 14),
        Qt::AlignLeft | Qt::AlignVCenter,
        distributionLabel
    );

    const QRect distributionChartRect(
        content.left(),
        distributionTopY,
        content.width(),
        qMax(24, distributionBottomY - distributionTopY + 1)
    );

    bool distributionRendered = false;

    if (m_snapshot.distributionValid && !m_snapshot.distribution.isEmpty()) {
        int maxValue = 0;
        for (const MarketBreadthDistributionItem& item : m_snapshot.distribution) {
            maxValue = qMax(maxValue, item.value);
        }

        if (maxValue > 0) {
            painter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 90));
            painter.drawLine(distributionChartRect.bottomLeft(), distributionChartRect.bottomRight());

            const int barCount = m_snapshot.distribution.size();
            const int barGap = (barCount > 18) ? 1 : 2;
            const int middleGap = (barCount > 4) ? 6 : 0;
            const int splitIndex = (barCount % 2 == 0)
                ? (barCount / 2 - 1)
                : (barCount / 2);
            int x = distributionChartRect.left();
            for (int i = 0; i < barCount; ++i) {
                const int remainingBars = barCount - i;
                int remainingGap = barGap * qMax(0, remainingBars - 1);
                if (middleGap > 0 && i <= splitIndex) {
                    remainingGap += middleGap;
                }
                const int remainingWidth = distributionChartRect.right() - x + 1 - remainingGap;
                const int barWidth = qMax(1, remainingWidth / remainingBars);

                const int value = qMax(0, m_snapshot.distribution.at(i).value);
                const int barHeight = qMax(
                    1,
                    qRound(static_cast<double>(value) / static_cast<double>(maxValue) * distributionChartRect.height())
                );
                const QRect barRect(x, distributionChartRect.bottom() - barHeight + 1, barWidth, barHeight);

                QColor barColor;
                if (i < barCount / 2) {
                    barColor = m_cfg.upColor;
                } else if (barCount % 2 == 1 && i == barCount / 2) {
                    barColor = m_cfg.flatColor;
                } else {
                    barColor = m_cfg.downColor;
                }
                barColor.setAlpha(205);

                painter.setPen(Qt::NoPen);
                painter.setBrush(barColor);
                painter.drawRoundedRect(barRect, 1.5, 1.5);

                if (i < barCount - 1) {
                    int gapAfter = barGap;
                    if (middleGap > 0 && i == splitIndex) {
                        gapAfter += middleGap;
                    }
                    x += barWidth + gapAfter;
                }
            }

            QFont axisFont = painter.font();
            axisFont.setBold(false);
            axisFont.setPointSizeF(qMax(8.0, axisFont.pointSizeF() - 1.0));
            painter.setFont(axisFont);
            painter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 180));

            const int middleIndex = m_snapshot.distribution.size() / 2;
            const QString leftLabel = m_snapshot.distribution.first().bucket;
            const QString middleLabel = m_snapshot.distribution.at(middleIndex).bucket;
            const QString rightLabel = m_snapshot.distribution.last().bucket;
            const int labelY = distributionChartRect.bottom() + 2;

            painter.drawText(
                QRect(distributionChartRect.left(), labelY, distributionChartRect.width() / 3, 12),
                Qt::AlignLeft | Qt::AlignVCenter,
                leftLabel
            );
            painter.drawText(
                QRect(
                    distributionChartRect.left() + distributionChartRect.width() / 3,
                    labelY,
                    distributionChartRect.width() / 3,
                    12
                ),
                Qt::AlignHCenter | Qt::AlignVCenter,
                middleLabel
            );
            painter.drawText(
                QRect(
                    distributionChartRect.left() + distributionChartRect.width() * 2 / 3,
                    labelY,
                    distributionChartRect.width() / 3,
                    12
                ),
                Qt::AlignRight | Qt::AlignVCenter,
                rightLabel
            );
            distributionRendered = true;
        }
    }

    if (!distributionRendered) {
        painter.setFont(valueFont);
        painter.setPen(textColor);
        painter.drawText(
            distributionChartRect,
            Qt::AlignCenter,
            i18n::t("quote.noData", m_language)
        );
    }

    const QRect trendChartRect(
        content.left(),
        trendTopY,
        content.width(),
        qMax(32, trendBottomY - trendTopY + 1)
    );

    if (m_snapshot.overviewTimeline.size() >= 2) {
        int maxCount = 0;
        for (const MarketBreadthTimelinePoint& point : m_snapshot.overviewTimeline) {
            maxCount = qMax(maxCount, qMax(point.riseCount, point.fallCount));
            maxCount = qMax(maxCount, qMax(point.limitUpCount, point.limitDownCount));
        }

        if (maxCount > 0) {
            const QRect plotRect = trendChartRect.adjusted(30, 4, -6, -16);
            if (plotRect.width() < 16 || plotRect.height() < 16) {
                painter.setFont(valueFont);
                painter.setPen(textColor);
                painter.drawText(
                    trendChartRect,
                    Qt::AlignCenter,
                    i18n::t("quote.noData", m_language)
                );
                return;
            }

            const QStringList& xAxisLabels = hardcodedAshareIntradayXAxis();
            const QHash<QString, int>& xAxisIndex = hardcodedAshareIntradayXAxisIndex();
            const int axisCount = xAxisLabels.size();
            const QTimeZone bjZone("Asia/Shanghai");

            const auto xAt = [&](int axisIndexValue) {
                if (axisCount <= 1) {
                    return plotRect.left();
                }
                const double t = static_cast<double>(axisIndexValue)
                    / static_cast<double>(axisCount - 1);
                return plotRect.left() + qRound(t * plotRect.width());
            };
            const auto yAt = [&](int value) {
                const double ratio = static_cast<double>(qMax(0, value)) / static_cast<double>(maxCount);
                return plotRect.bottom() - qRound(ratio * plotRect.height());
            };

            struct TrendPoint {
                int axisIdx = -1;
                QPoint pixel;
            };

            auto buildSeriesPoints = [&](const std::function<int(const MarketBreadthTimelinePoint&)>& valueGetter) {
                QVector<TrendPoint> points;
                points.reserve(m_snapshot.overviewTimeline.size());

                for (const MarketBreadthTimelinePoint& timelinePoint : m_snapshot.overviewTimeline) {
                    const int value = valueGetter(timelinePoint);
                    if (value < 0) {
                        continue;
                    }

                    QDateTime ts = QDateTime::fromMSecsSinceEpoch(timelinePoint.timestampMs, bjZone);
                    if (!ts.isValid()) {
                        ts = QDateTime::fromMSecsSinceEpoch(timelinePoint.timestampMs);
                    }
                    const QString hhmm = ts.toString(QStringLiteral("HH:mm"));
                    const int axisIdx = xAxisIndex.value(hhmm, -1);
                    if (axisIdx < 0) {
                        continue;
                    }

                    TrendPoint p;
                    p.axisIdx = axisIdx;
                    p.pixel = QPoint(xAt(axisIdx), yAt(value));
                    points.push_back(p);
                }
                return points;
            };

            const QVector<TrendPoint> risePoints = buildSeriesPoints([](const MarketBreadthTimelinePoint& p) {
                return p.riseCount;
            });
            const QVector<TrendPoint> fallPoints = buildSeriesPoints([](const MarketBreadthTimelinePoint& p) {
                return p.fallCount;
            });
            const QVector<TrendPoint> limitUpPoints = buildSeriesPoints([](const MarketBreadthTimelinePoint& p) {
                return p.limitUpCount;
            });
            const QVector<TrendPoint> limitDownPoints = buildSeriesPoints([](const MarketBreadthTimelinePoint& p) {
                return p.limitDownCount;
            });

            QVector<QVector<TrendPoint>> allSeries {
                risePoints,
                fallPoints,
                limitUpPoints,
                limitDownPoints,
            };

            const QColor axisColor(textColor.red(), textColor.green(), textColor.blue(), 160);
            painter.setPen(QPen(axisColor, 1.0));
            painter.drawLine(plotRect.bottomLeft(), plotRect.bottomRight());
            painter.drawLine(plotRect.bottomLeft(), plotRect.topLeft());

            QFont axisFont = painter.font();
            axisFont.setBold(false);
            axisFont.setPointSizeF(qMax(8.0, axisFont.pointSizeF() - 1.0));
            painter.setFont(axisFont);
            painter.setPen(axisColor);
            painter.drawText(
                QRect(trendChartRect.left(), plotRect.top() - 6, 26, 12),
                Qt::AlignRight | Qt::AlignVCenter,
                QString::number(maxCount)
            );
            painter.drawText(
                QRect(trendChartRect.left(), plotRect.bottom() - 6, 26, 12),
                Qt::AlignRight | Qt::AlignVCenter,
                QStringLiteral("0")
            );

            const int xLabelY = plotRect.bottom() + 2;
            const int leftWidth = 52;
            const int middleWidth = 90;
            const int rightWidth = 52;
            const int leftLabelLeft = plotRect.left() - 2;
            const int rightLabelLeft = plotRect.right() - rightWidth + 2;

            int amCloseIdx = xAxisIndex.value(QStringLiteral("11:30"), -1);
            int pmOpenIdx = xAxisIndex.value(QStringLiteral("13:00"), -1);
            if (pmOpenIdx < 0) {
                pmOpenIdx = xAxisIndex.value(QStringLiteral("13:01"), -1);
            }

            int middleAnchorX = plotRect.center().x();
            if (amCloseIdx >= 0 && pmOpenIdx >= 0) {
                middleAnchorX = (xAt(amCloseIdx) + xAt(pmOpenIdx)) / 2;
            } else if (amCloseIdx >= 0) {
                middleAnchorX = xAt(amCloseIdx);
            } else if (pmOpenIdx >= 0) {
                middleAnchorX = xAt(pmOpenIdx);
            }

            const int minMiddleLeft = leftLabelLeft + leftWidth + 4;
            const int maxMiddleLeft = rightLabelLeft - middleWidth - 4;
            const int middleLabelLeft = maxMiddleLeft >= minMiddleLeft
                ? qBound(minMiddleLeft, middleAnchorX - middleWidth / 2, maxMiddleLeft)
                : (leftLabelLeft + rightLabelLeft - middleWidth) / 2;

            if (middleAnchorX > plotRect.left() && middleAnchorX < plotRect.right()) {
                QPen middayPen(QColor(axisColor.red(), axisColor.green(), axisColor.blue(), 115), 0.9);
                middayPen.setStyle(Qt::DashLine);
                middayPen.setDashPattern({4.0, 4.0});
                middayPen.setCosmetic(true);
                painter.setPen(middayPen);
                painter.drawLine(
                    QPoint(middleAnchorX, plotRect.top()),
                    QPoint(middleAnchorX, plotRect.bottom())
                );
            }

            painter.drawText(
                QRect(leftLabelLeft, xLabelY, leftWidth, 12),
                Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("9:30")
            );
            painter.drawText(
                QRect(middleLabelLeft, xLabelY, middleWidth, 12),
                Qt::AlignHCenter | Qt::AlignVCenter,
                QStringLiteral("11:30/13:00")
            );
            painter.drawText(
                QRect(rightLabelLeft, xLabelY, rightWidth, 12),
                Qt::AlignRight | Qt::AlignVCenter,
                QStringLiteral("15:00")
            );

            struct SeriesInfo {
                QString name;
                QColor color;
                int idx;
            };
            const QVector<SeriesInfo> infos {
                {QStringLiteral("上涨"), m_cfg.upColor, 0},
                {QStringLiteral("下跌"), m_cfg.downColor, 1},
                {QStringLiteral("涨停"), QColor(QStringLiteral("#bb07ae")), 2},
                {QStringLiteral("跌停"), QColor(QStringLiteral("#fc7d02")), 3},
            };

            int legendX = plotRect.left();
            const int legendY = plotRect.top() - 14;
            painter.setFont(axisFont);
            for (const SeriesInfo& info : infos) {
                painter.setPen(QPen(info.color, 1.5));
                painter.drawLine(legendX, legendY + 6, legendX + 12, legendY + 6);
                painter.setPen(textColor);
                painter.drawText(QRect(legendX + 14, legendY, 30, 12), Qt::AlignLeft | Qt::AlignVCenter, info.name);
                legendX += 44;
            }

            auto segmentOverlapsOthers = [&](int seriesIdx, int segIdx) {
                const QVector<TrendPoint>& points = allSeries.at(seriesIdx);
                if (segIdx <= 0 || segIdx >= points.size()) {
                    return false;
                }
                const QPoint a1 = points.at(segIdx - 1).pixel;
                const QPoint a2 = points.at(segIdx).pixel;
                for (int otherIdx = 0; otherIdx < allSeries.size(); ++otherIdx) {
                    if (otherIdx == seriesIdx) {
                        continue;
                    }
                    const QVector<TrendPoint>& other = allSeries.at(otherIdx);
                    for (int j = 1; j < other.size(); ++j) {
                        const QPoint b1 = other.at(j - 1).pixel;
                        const QPoint b2 = other.at(j).pixel;
                        const bool sameDir = (a1 == b1 && a2 == b2);
                        const bool reverseDir = (a1 == b2 && a2 == b1);
                        if (sameDir || reverseDir) {
                            return true;
                        }
                    }
                }
                return false;
            };

            painter.setRenderHint(QPainter::Antialiasing, true);
            for (const SeriesInfo& info : infos) {
                const QVector<TrendPoint>& points = allSeries.at(info.idx);
                if (points.size() < 2) {
                    continue;
                }
                painter.setPen(QPen(info.color, 1.2));
                for (int i = 1; i < points.size(); ++i) {
                    if (segmentOverlapsOthers(info.idx, i)) {
                        continue;
                    }
                    painter.drawLine(points.at(i - 1).pixel, points.at(i).pixel);
                }
            }
            return;
        }
    }

    painter.setFont(valueFont);
    painter.setPen(textColor);
    painter.drawText(
        trendChartRect,
        Qt::AlignCenter,
        i18n::t("quote.noData", m_language)
    );
}


void  MarketBreadthDetailWindow::triggerPopupRefresh(bool withFeedback) {
    if (withFeedback) {
        startRefreshFeedback();
    }
    requestHotRankData(false, true);
    requestHotRankData(true, true);
    requestStockHeatData(HotRankTabMode::HourHeat, true);
    requestStockHeatData(HotRankTabMode::DayHeat, true);
    requestIndexQuoteData(true);
    if (m_forceRefreshCallback) {
        m_forceRefreshCallback();
    }
}


void  MarketBreadthDetailWindow::startRefreshFeedback() {
    constexpr qint64 kRefreshFeedbackDurationMs = 600;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_refreshFeedbackStartedMs = nowMs;
    m_refreshFeedbackUntilMs = nowMs + kRefreshFeedbackDurationMs;
    if (m_refreshFeedbackTimer && !m_refreshFeedbackTimer->isActive()) {
        m_refreshFeedbackTimer->start();
    }
    if (m_refreshButtonRect.isValid()) {
        update(m_refreshButtonRect.adjusted(-3, -3, 3, 3));
    }
}


bool  MarketBreadthDetailWindow::isRefreshFeedbackActive(qint64* nowMsOut) const {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMsOut) {
        *nowMsOut = nowMs;
    }
    return nowMs < m_refreshFeedbackUntilMs;
}


void  MarketBreadthDetailWindow::startAutoRefreshTimer() {
    if (!m_autoRefreshTimer) {
        return;
    }
    if (!m_autoRefreshTimer->isActive()) {
        m_autoRefreshTimer->start();
    }
}


void  MarketBreadthDetailWindow::stopAutoRefreshTimer() {
    if (m_autoRefreshTimer) {
        m_autoRefreshTimer->stop();
    }
}


void  MarketBreadthDetailWindow::ensureSingleVisible() {
    if (s_visiblePopup && s_visiblePopup != this) {
        s_visiblePopup->hidePopup();
    }
    s_visiblePopup = this;
}


void  MarketBreadthDetailWindow::ensureHotRankProviders() {
    if (m_hotRankProvider) {
        return;
    }

    m_hotRankProvider = new EastMoneyHotRankProvider(this);
    m_hotRankProvider->applyConfig(m_cfg);

    connect(
        m_hotRankProvider,
        &EastMoneyHotRankProvider::hotSectorsReady,
        this,
        [this](const QVector<HotRankItem>& items) {
            m_hotSectorsRanked = items;
            update();
        }
    );
    connect(
        m_hotRankProvider,
        &EastMoneyHotRankProvider::hotConceptsReady,
        this,
        [this](const QVector<HotRankItem>& items) {
            m_hotConceptsRanked = items;
            update();
        }
    );
    connect(
        m_hotRankProvider,
        &EastMoneyHotRankProvider::error,
        this,
        [this](const QString& message) {
            const QString trimmed = message.trimmed();
            if (trimmed.isEmpty() || trimmed == m_lastHotRankError) {
                return;
            }
            m_lastHotRankError = trimmed;
            qInfo() << "[MarketBreadthPopup] hot rank request error:" << trimmed;
        }
    );
}

void  MarketBreadthDetailWindow::ensureStockHeatProvider() {
    if (m_stockHeatProvider) {
        return;
    }

    m_stockHeatProvider = new TonghuashunStockHeatProvider(this);
    m_stockHeatProvider->applyConfig(m_cfg);

    connect(
        m_stockHeatProvider,
        &TonghuashunStockHeatProvider::hourHotStocksReady,
        this,
        [this](const QVector<HotRankItem>& items) {
            m_hotHourStocks = items;
            update();
        }
    );
    connect(
        m_stockHeatProvider,
        &TonghuashunStockHeatProvider::dayHotStocksReady,
        this,
        [this](const QVector<HotRankItem>& items) {
            m_hotDayStocks = items;
            update();
        }
    );
    connect(
        m_stockHeatProvider,
        &TonghuashunStockHeatProvider::error,
        this,
        [this](const QString& message) {
            const QString trimmed = message.trimmed();
            if (trimmed.isEmpty() || trimmed == m_lastStockHeatError) {
                return;
            }
            m_lastStockHeatError = trimmed;
            qInfo() << "[MarketBreadthPopup] stock heat request error:" << trimmed;
        }
    );
}

void  MarketBreadthDetailWindow::ensureHotRankDetailWindow() {
    if (m_hotRankDetailWindow) {
        return;
    }

    m_hotRankDetailWindow = new HotRankConstituentDetailWindow(m_parentWindow);
    m_hotRankDetailWindow->installEventFilter(this);
    m_hotRankDetailWindow->applyConfig(m_cfg);
    m_hotRankDetailWindow->setLanguage(m_language);
    m_hotRankDetailWindow->setWatchlistCallbacks(
        m_hotRankDetailWatchlistContainsCallback,
        m_hotRankDetailWatchlistMutateCallback,
        m_hotRankDetailWatchlistReloadCallback
    );
}


int  MarketBreadthDetailWindow::popupHotRankLimit(bool concept) const {
    return concept ? 100 : 2000;
}


void  MarketBreadthDetailWindow::requestHotRankData(bool concept, bool forceRefresh) {
    ensureHotRankProviders();
    if (!m_hotRankProvider) {
        return;
    }

    const int limit = popupHotRankLimit(concept);
    if (concept) {
        m_hotRankProvider->fetchHotConcepts(
            limit,
            QStringLiteral("pct"),
            QStringLiteral("desc"),
            forceRefresh
        );
    } else {
        m_hotRankProvider->fetchHotSectors(
            limit,
            QStringLiteral("pct"),
            QStringLiteral("desc"),
            forceRefresh
        );
    }
}

void  MarketBreadthDetailWindow::requestStockHeatData(HotRankTabMode mode, bool forceRefresh) {
    ensureStockHeatProvider();
    if (!m_stockHeatProvider || !isStockHeatTabMode(mode)) {
        return;
    }

    constexpr int kStockHeatLimit = 100;
    if (mode == HotRankTabMode::DayHeat) {
        m_stockHeatProvider->fetchDayHotStocks(kStockHeatLimit, forceRefresh);
    } else {
        m_stockHeatProvider->fetchHourHotStocks(kStockHeatLimit, forceRefresh);
    }
}

MarketBreadthDetailWindow::HotRankTabMode MarketBreadthDetailWindow::resolvedHotRankTabMode() const {
    if (m_hotRankTabMode != HotRankTabMode::Auto) {
        return m_hotRankTabMode;
    }

    if (!m_hotSectorsRanked.isEmpty() || !m_hotSectors.isEmpty()) {
        return HotRankTabMode::Sector;
    }
    if (!m_hotConceptsRanked.isEmpty() || !m_hotConcepts.isEmpty()) {
        return HotRankTabMode::Concept;
    }
    if (!m_hotHourStocks.isEmpty()) {
        return HotRankTabMode::HourHeat;
    }
    if (!m_hotDayStocks.isEmpty()) {
        return HotRankTabMode::DayHeat;
    }
    return HotRankTabMode::Sector;
}

bool  MarketBreadthDetailWindow::isStockHeatTabMode(HotRankTabMode mode) {
    return mode == HotRankTabMode::HourHeat || mode == HotRankTabMode::DayHeat;
}

const QVector<HotRankItem>& MarketBreadthDetailWindow::hotRankItemsForMode(HotRankTabMode mode) const {
    switch (mode) {
    case HotRankTabMode::Concept:
        return !m_hotConceptsRanked.isEmpty() ? m_hotConceptsRanked : m_hotConcepts;
    case HotRankTabMode::HourHeat:
        return m_hotHourStocks;
    case HotRankTabMode::DayHeat:
        return m_hotDayStocks;
    case HotRankTabMode::Sector:
    case HotRankTabMode::Auto:
    default:
        return !m_hotSectorsRanked.isEmpty() ? m_hotSectorsRanked : m_hotSectors;
    }
}

int& MarketBreadthDetailWindow::hotRankScrollIndexForMode(HotRankTabMode mode) {
    switch (mode) {
    case HotRankTabMode::Concept:
        return m_hotConceptScrollIndex;
    case HotRankTabMode::HourHeat:
        return m_hotHourScrollIndex;
    case HotRankTabMode::DayHeat:
        return m_hotDayScrollIndex;
    case HotRankTabMode::Sector:
    case HotRankTabMode::Auto:
    default:
        return m_hotSectorScrollIndex;
    }
}

int  MarketBreadthDetailWindow::hotRankRowAt(const QPoint& pos) const {
    for (int index = 0; index < m_hotRankRowHitAreas.size(); ++index) {
        if (m_hotRankRowHitAreas.at(index).rect.contains(pos)) {
            return index;
        }
    }
    return -1;
}

int  MarketBreadthDetailWindow::hotStockActionAt(const QPoint& pos) const {
    for (int index = 0; index < m_hotStockActionButtonRects.size(); ++index) {
        if (m_hotStockActionButtonRects.at(index).contains(pos)) {
            return index;
        }
    }
    return -1;
}

int  MarketBreadthDetailWindow::indexCellAt(const QPoint& pos) const {
    for (int index = 0; index < m_indexCellHitAreas.size(); ++index) {
        const IndexCellHitArea& area = m_indexCellHitAreas.at(index);
        if (area.rect.contains(pos) && isTimelinePopupSupportedCode(area.code)) {
            return index;
        }
    }
    return -1;
}

void  MarketBreadthDetailWindow::openHotRankDetail(const HotRankItem& item) {
    const QString fs = !item.detailFs.trimmed().isEmpty()
        ? item.detailFs.trimmed()
        : (item.code.trimmed().isEmpty()
            ? QString()
            : QStringLiteral("b:%1").arg(item.code.trimmed()));
    if (fs.isEmpty()) {
        return;
    }

    ensureHotRankDetailWindow();
    if (!m_hotRankDetailWindow) {
        return;
    }
    m_hotRankDetailWindow->applyConfig(m_cfg);
    m_hotRankDetailWindow->setLanguage(m_language);
    m_hotRankDetailWindow->showForItem(item, geometry());
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

bool  MarketBreadthDetailWindow::containsHotStockWatchlistItem(const HotRankItem& item) const {
    const QString key = item.watchCode.trimmed().toLower();
    if (key.isEmpty()) {
        return false;
    }
    if (m_hotStockWatchlistPresenceOverrides.contains(key)) {
        return m_hotStockWatchlistPresenceOverrides.value(key);
    }
    return m_hotRankDetailWatchlistContainsCallback
        ? m_hotRankDetailWatchlistContainsCallback(item.watchCode)
        : false;
}

void  MarketBreadthDetailWindow::toggleHotStockWatchlistAction(int actionIndex) {
    const HotRankTabMode activeMode = resolvedHotRankTabMode();
    if (!isStockHeatTabMode(activeMode) || actionIndex < 0 || !m_hotRankDetailWatchlistMutateCallback) {
        return;
    }

    const QVector<HotRankItem>& items = hotRankItemsForMode(activeMode);
    const int itemIndex = hotRankScrollIndexForMode(activeMode) + actionIndex;
    if (itemIndex < 0 || itemIndex >= items.size()) {
        return;
    }

    const HotRankItem& item = items.at(itemIndex);
    const QString watchCode = item.watchCode.trimmed();
    if (watchCode.isEmpty()) {
        return;
    }

    const bool tracked = containsHotStockWatchlistItem(item);
    const bool add = !tracked;
    if (!m_hotRankDetailWatchlistMutateCallback(watchCode, item.name, add)) {
        return;
    }

    m_hotStockWatchlistPresenceOverrides.insert(watchCode.toLower(), add);
    m_hotStockWatchlistDirty = true;
    update();
}

void  MarketBreadthDetailWindow::flushPendingHotStockWatchlistReloadIfNeeded() {
    if (!m_hotStockWatchlistDirty) {
        return;
    }

    m_hotStockWatchlistDirty = false;
    if (m_hotRankDetailWatchlistReloadCallback) {
        m_hotRankDetailWatchlistReloadCallback();
    }
    m_hotStockWatchlistPresenceOverrides.clear();
}


void  MarketBreadthDetailWindow::ensureIndexQuoteProvider() {
    if (m_indexQuoteProvider) {
        return;
    }

    m_indexQuoteProvider = new EastMoneyIndexQuoteProvider(this);
    m_indexQuoteProvider->applyConfig(m_cfg);

    connect(
        m_indexQuoteProvider,
        &EastMoneyIndexQuoteProvider::dataReady,
        this,
        [this](const QVector<IndexQuoteItem>& items) {
            m_indexQuotes = items;
            update();
        }
    );
    connect(
        m_indexQuoteProvider,
        &EastMoneyIndexQuoteProvider::error,
        this,
        [](const QString& message) {
            qInfo() << "[IndexQuote] error:" << message;
        }
    );
}


void  MarketBreadthDetailWindow::requestIndexQuoteData(bool forceRefresh) {
    ensureIndexQuoteProvider();
    if (m_indexQuoteProvider) {
        m_indexQuoteProvider->fetch(forceRefresh);
    }
}


void  MarketBreadthDetailWindow::ensureTimelinePopup() {
    if (m_timelinePopup) {
        return;
    }

    m_timelinePopup = new SharedTimelineChartPopup(this);
    m_timelinePopup->applyConfig(m_cfg);
}


void  MarketBreadthDetailWindow::showTimelinePopupForIndexCell(int index) {
    if (testAttribute(Qt::WA_TransparentForMouseEvents)
        || index < 0
        || index >= m_indexCellHitAreas.size()) {
        hideTimelinePopup();
        return;
    }

    const IndexCellHitArea& area = m_indexCellHitAreas.at(index);
    const QString code = area.code.trimmed();
    if (code.isEmpty() || !isTimelinePopupSupportedCode(code)) {
        hideTimelinePopup();
        return;
    }

    ensureTimelinePopup();
    if (!m_timelinePopup) {
        return;
    }

    const QString name = area.name.trimmed().isEmpty() ? code : area.name.trimmed();
    const QRect globalAnchor(mapToGlobal(area.rect.topLeft()), area.rect.size());
    m_timelinePopup->showForStock(code, name, globalAnchor, width());

    if (!m_timelineAppFilterInstalled) {
        m_timelineAppFilterInstalled = true;
        QCoreApplication::instance()->installEventFilter(this);
    }
}


void  MarketBreadthDetailWindow::hideTimelinePopup() {
    if (m_timelineAppFilterInstalled) {
        m_timelineAppFilterInstalled = false;
        QCoreApplication::instance()->removeEventFilter(this);
    }
    if (m_timelinePopup) {
        m_timelinePopup->hidePopup();
    }
}
