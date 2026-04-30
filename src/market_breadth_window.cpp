#include "market_breadth_window.h"

#include "app_constants.h"
#include "i18n.h"
#include "quote_provider.h"

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

#include <cmath>
#include <functional>

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
    if (snapshot.overviewTimeline.isEmpty()) {
        return i18n::t("quote.noData", language);
    }

    const qint64 timestampMs = snapshot.overviewTimeline.last().timestampMs;
    if (timestampMs <= 0) {
        return i18n::t("quote.noData", language);
    }

    const QTimeZone bjZone("Asia/Shanghai");
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

    const qint64 elapsedSecsRaw = sampleTs.secsTo(nowTs);
    const qint64 elapsedSecs = qMax<qint64>(0, elapsedSecsRaw);
    if (elapsedSecs < 60) {
        const qint64 seconds = qMax<qint64>(1, elapsedSecs);
        return i18n::t("popup.marketBreadth.updatedSecondsAgoFmt", language)
            .arg(QString::number(seconds));
    }

    if (elapsedSecs < 3600) {
        const qint64 minutes = qMax<qint64>(1, elapsedSecs / 60);
        return i18n::t("popup.marketBreadth.updatedMinutesAgoFmt", language)
            .arg(QString::number(minutes));
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
#endif

inline constexpr int kPopupScreenMarginPx = 12;

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

} // namespace


MarketBreadthDetailWindow* MarketBreadthDetailWindow::s_visiblePopup = nullptr;

 MarketBreadthDetailWindow::MarketBreadthDetailWindow(QWidget* parent)
 : QWidget(nullptr)
 , m_parentWindow(parent) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    resize(740, 715);

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

    m_lastUpdatedTextTimer = new QTimer(this);
    m_lastUpdatedTextTimer->setInterval(1000);
    connect(m_lastUpdatedTextTimer, &QTimer::timeout, this, [this]() {
        if (!isVisible()) {
            m_lastUpdatedTextTimer->stop();
            return;
        }

        QRect dirtyRect = m_updatedTextRect;
        if (m_refreshButtonRect.isValid()) {
            dirtyRect = dirtyRect.isValid()
                ? dirtyRect.united(m_refreshButtonRect)
                : m_refreshButtonRect;
        }
        if (dirtyRect.isValid()) {
            update(dirtyRect.adjusted(-3, -3, 3, 3));
        } else {
            update();
        }
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
    if (m_indexQuoteProvider) {
        m_indexQuoteProvider->applyConfig(m_cfg);
    }
    setFont(effectiveFloatingWindowFont(cfg, font()));
    update();
}


void  MarketBreadthDetailWindow::setLanguage(const QString& language) {
    m_language = i18n::resolveLanguage(language);
    update();
}


void  MarketBreadthDetailWindow::setForceRefreshCallback(std::function<void()> callback) {
    m_forceRefreshCallback = callback;
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
    m_pressedHotTab = 0;
    m_dragging = false;
    m_dragOffset = QPoint();

    const QSize popupSize(740, 715);
    resize(popupSize);

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

    QRect targetRect(QPoint(0, 0), popupSize);
    if (screenRect.isValid()) {
        targetRect.moveCenter(screenRect.center());
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
    if (!isVisible()) {
        show();
    }
#if defined(Q_OS_MACOS)
    // Override collection behavior set by Qt (WindowStaysOnTopHint causes
    // NSWindowCollectionBehaviorTransient, hiding it from Mission Control).
    setMacWindowCollectionBehaviorManaged(this);
#endif
    startLastUpdatedTextTimer();
    enforceAlwaysOnTop();
    requestHotRankData(false);
    requestHotRankData(true);
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
    startLastUpdatedTextTimer();
    update();
}


bool  MarketBreadthDetailWindow::isPinnedFromTray() const {
    return m_pinnedFromTray;
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
    m_pressedHotTab = 0;
    m_dragging = false;
    m_dragOffset = QPoint();
    m_refreshButtonRect = QRect();
    m_updatedTextRect = QRect();
    m_hotSectorTabRect = QRect();
    m_hotConceptTabRect = QRect();
    stopLastUpdatedTextTimer();
    hide();
    if (s_visiblePopup == this) {
        s_visiblePopup = nullptr;
    }
}


void  MarketBreadthDetailWindow::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    stopLastUpdatedTextTimer();
    if (s_visiblePopup == this) {
        s_visiblePopup = nullptr;
    }
}


void  MarketBreadthDetailWindow::mouseMoveEvent(QMouseEvent* event) {
    if (!event || testAttribute(Qt::WA_TransparentForMouseEvents)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }

    const bool closeHovered = m_closeButtonRect.contains(event->pos());
    const bool refreshHovered = m_refreshButtonRect.contains(event->pos());
    const bool sectorHovered = m_hotSectorTabRect.contains(event->pos());
    const bool conceptHovered = m_hotConceptTabRect.contains(event->pos());
    if (closeHovered != m_closeButtonHovered
        || refreshHovered != m_refreshButtonHovered
        || sectorHovered != m_hotSectorTabHovered
        || conceptHovered != m_hotConceptTabHovered) {
        m_closeButtonHovered = closeHovered;
        m_refreshButtonHovered = refreshHovered;
        m_hotSectorTabHovered = sectorHovered;
        m_hotConceptTabHovered = conceptHovered;
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
    }
    QWidget::mouseMoveEvent(event);
}


void  MarketBreadthDetailWindow::leaveEvent(QEvent* event) {
    if (!testAttribute(Qt::WA_TransparentForMouseEvents)
        && (m_closeButtonHovered || m_closeButtonPressed
            || m_refreshButtonHovered || m_refreshButtonPressed
            || m_hotSectorTabHovered || m_hotConceptTabHovered || m_pressedHotTab != 0)) {
        m_closeButtonHovered = false;
        m_closeButtonPressed = false;
        m_refreshButtonHovered = false;
        m_refreshButtonPressed = false;
        m_hotSectorTabHovered = false;
        m_hotConceptTabHovered = false;
        m_pressedHotTab = 0;
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
    }
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

    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
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

    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
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
            startRefreshFeedback();
            requestHotRankData(false, true);
            requestHotRankData(true, true);
            requestIndexQuoteData(true);
            if (m_forceRefreshCallback) {
                m_forceRefreshCallback();
            }
            event->accept();
            return;
        }

        const bool activateSectorTab = m_pressedHotTab == 1 && m_hotSectorTabRect.contains(event->pos());
        const bool activateConceptTab = m_pressedHotTab == 2 && m_hotConceptTabRect.contains(event->pos());
        m_pressedHotTab = 0;

        if (activateSectorTab) {
            m_hotRankTabMode = HotRankTabMode::Sector;
            requestHotRankData(false);
            update();
            event->accept();
            return;
        }
        if (activateConceptTab) {
            m_hotRankTabMode = HotRankTabMode::Concept;
            requestHotRankData(true);
            update();
            event->accept();
            return;
        }
    }

    QWidget::mouseReleaseEvent(event);
}


void  MarketBreadthDetailWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!event || testAttribute(Qt::WA_TransparentForMouseEvents)) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        hidePopup();
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

        const int leftGap = 12;
        const int maxSummaryCardHeight = qMax(140, leftRect.height() - leftGap - 84);
        const int summaryCardHeight = qBound(
            140,
            qRound(static_cast<double>(leftRect.height()) * 0.45),
            maxSummaryCardHeight
        );
        const QRect summaryCard(leftRect.left(), leftRect.top(), leftRect.width(), summaryCardHeight);
        const QRect noteCard(
            leftRect.left(),
            summaryCard.bottom() + 1 + leftGap,
            leftRect.width(),
            qMax(84, leftRect.bottom() - (summaryCard.bottom() + leftGap))
        );

        drawCard(
            summaryCard,
            QString(),
            Qt::AlignHCenter | Qt::AlignTop
        );
        drawCard(noteCard, QString(), Qt::AlignLeft | Qt::AlignTop);

        const QRect summaryInner = summaryCard.adjusted(12, 12, -12, -12);
        const int summarySectionGap = 8;
        const int topSectionHeight = qMax(56, (summaryInner.height() - summarySectionGap) / 2);
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
        sectionTitleFont.setPointSizeF(qMax(8.2, sectionTitleFont.pointSizeF() - 0.6));
        popupPainter.setFont(sectionTitleFont);
        popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 210));
        popupPainter.drawText(
            QRect(trendSectionRect.left(), trendSectionRect.top(), trendSectionRect.width(), 12),
            Qt::AlignHCenter | Qt::AlignVCenter,
            i18n::t("quote.marketBreadth", m_language)
        );
        popupPainter.drawText(
            QRect(turnoverSectionRect.left(), turnoverSectionRect.top(), turnoverSectionRect.width(), 12),
            Qt::AlignHCenter | Qt::AlignVCenter,
            turnoverLabel
        );

        const QRect trendInner = trendSectionRect.adjusted(0, 14, 0, 0);
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
        const int trendGap = 6;
        const int trendCellWidth = qMax(
            24,
            (trendInner.width() - trendGap * qMax(0, trendCount - 1)) / qMax(1, trendCount)
        );
        const int trendLabelHeight = 14;
        const int trendValueHeight = 20;
        const int trendRowGap = 2;
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

        const QRect turnoverInner = turnoverSectionRect.adjusted(0, 14, 0, 0);
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
        const int turnoverGap = 6;
        const int turnoverCellWidth = qMax(
            42,
            (turnoverInner.width() - turnoverGap * qMax(0, turnoverCount - 1)) / qMax(1, turnoverCount)
        );
        const int turnoverLabelHeight = 22;
        const int turnoverValueHeight = 22;
        const int turnoverRowGap = 2;
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

        const QRect noteInner = noteCard.adjusted(12, 10, -12, -12);
        const bool hasSectorData = !m_hotSectorsRanked.isEmpty() || !m_hotSectors.isEmpty();
        const bool hasConceptData = !m_hotConceptsRanked.isEmpty() || !m_hotConcepts.isEmpty();
        const bool useConceptData = [this, hasSectorData, hasConceptData]() {
            switch (m_hotRankTabMode) {
            case HotRankTabMode::Sector:
                return false;
            case HotRankTabMode::Concept:
                return true;
            case HotRankTabMode::Auto:
            default:
                return !hasSectorData && hasConceptData;
            }
        }();
        const QVector<HotRankItem>& rankedSource = useConceptData
            ? (!m_hotConceptsRanked.isEmpty() ? m_hotConceptsRanked : m_hotConcepts)
            : (!m_hotSectorsRanked.isEmpty() ? m_hotSectorsRanked : m_hotSectors);

        QFont tabFont = bodyFont;
        tabFont.setBold(true);
        tabFont.setPointSizeF(qMax(8.0, tabFont.pointSizeF() - 0.2));
        const int tabHeight = 24;
        const int tabGap = 8;
        const QRect tabBarRect(noteInner.left(), noteInner.top(), noteInner.width(), tabHeight);
        const int tabWidth = qMax(62, (tabBarRect.width() - tabGap) / 2);
        const int tabsTotalWidth = tabWidth * 2 + tabGap;
        const int tabsStartX = tabBarRect.left() + qMax(0, (tabBarRect.width() - tabsTotalWidth) / 2);
        const QRect sectorTabRect(tabsStartX, tabBarRect.top(), tabWidth, tabHeight);
        const QRect conceptTabRect(sectorTabRect.right() + 1 + tabGap, tabBarRect.top(), tabWidth, tabHeight);
        m_hotSectorTabRect = sectorTabRect;
        m_hotConceptTabRect = conceptTabRect;

        auto drawTab = [&](const QRect& rect,
                           const QString& label,
                           bool active,
                           bool hovered,
                           bool hasData) {
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

        drawTab(
            sectorTabRect,
            i18n::t("quote.hotSector", m_language),
            !useConceptData,
            m_hotSectorTabHovered,
            hasSectorData
        );
        drawTab(
            conceptTabRect,
            i18n::t("quote.hotConcept", m_language),
            useConceptData,
            m_hotConceptTabHovered,
            hasConceptData
        );

        QVector<HotRankItem> risingItems;
        QVector<HotRankItem> fallingItems;
        if (useConceptData) {
            risingItems.reserve(rankedSource.size());
            for (const HotRankItem& item : rankedSource) {
                if (!std::isfinite(item.pct) || item.pct <= 0.0) {
                    continue;
                }
                risingItems.push_back(item);
            }
        } else {
            const int displayCount = 4;
            const int totalRanked = rankedSource.size();
            const int topCount = qMin(displayCount, totalRanked);
            const int bottomCount = qMin(displayCount, totalRanked);
            risingItems.reserve(topCount);
            fallingItems.reserve(bottomCount);
            for (int i = 0; i < topCount; ++i) {
                risingItems.push_back(rankedSource.at(i));
            }
            for (int i = 0; i < bottomCount; ++i) {
                fallingItems.push_back(rankedSource.at(totalRanked - 1 - i));
            }
        }

        const auto formatHotPct = [](double pct) {
            if (!std::isfinite(pct)) {
                return QStringLiteral("--");
            }
            QString text = QString::number(pct, 'f', 2);
            if (pct > 0.0) {
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

        const int rankTopGap = 8;
        const QRect rankArea(
            noteInner.left(),
            tabBarRect.bottom() + 1 + rankTopGap,
            noteInner.width(),
            qMax(40, noteInner.bottom() - (tabBarRect.bottom() + rankTopGap))
        );
        const int rankSectionGap = 8;
        const int rankSectionHeight = qMax(60, (rankArea.height() - rankSectionGap) / 2);
        const QRect riseSectionRect(
            rankArea.left(),
            rankArea.top(),
            rankArea.width(),
            rankSectionHeight
        );
        const QRect fallSectionRect(
            rankArea.left(),
            riseSectionRect.bottom() + 1 + rankSectionGap,
            rankArea.width(),
            qMax(60, rankArea.bottom() - (riseSectionRect.bottom() + rankSectionGap))
        );

        auto drawRankColumn = [&](const QRect& columnRect,
                                  const QVector<HotRankItem>& sortedItems,
                                  int maxRows) {
            if (columnRect.width() <= 8 || columnRect.height() <= 8) {
                return;
            }

            const int sectionTitleHeight = 0;
            const int columnHeaderHeight = 0;
            const int innerGap = 3;

            const QRect rowsRect(
                columnRect.left(),
                columnRect.top() + sectionTitleHeight + innerGap + columnHeaderHeight,
                columnRect.width(),
                qMax(0, columnRect.bottom() - (columnRect.top() + sectionTitleHeight + innerGap + columnHeaderHeight))
            );

            const int visibleRows = qMin(qMax(1, maxRows), sortedItems.size());

            if (visibleRows <= 0) {
                popupPainter.setFont(bodyFont);
                popupPainter.setPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 140));
                popupPainter.drawText(rowsRect, Qt::AlignCenter, i18n::t("quote.noData", m_language));
                return;
            }

            const int rowGap = visibleRows > 1 ? 3 : 0;
            const int availableRowsHeight = qMax(1, rowsRect.height() - rowGap * qMax(0, visibleRows - 1));
            const int rowHeight = qMax(1, qMin(20, availableRowsHeight / qMax(1, visibleRows)));
            const int rowStep = rowHeight + rowGap;
            const int usedRowsHeight = rowHeight * visibleRows + rowGap * qMax(0, visibleRows - 1);
            const int rowsTop = rowsRect.top() + qMax(0, (rowsRect.height() - usedRowsHeight) / 2);

            popupPainter.setFont(valueFont);
            const QFontMetrics rowMetrics(valueFont);

            int pctContentWidth = rowMetrics.horizontalAdvance(QStringLiteral("+12.34%")) + 8;
            int inflowContentWidth = 34;
            const int inflowProbeCount = qMin(sortedItems.size(), 48);
            for (int row = 0; row < inflowProbeCount; ++row) {
                inflowContentWidth = qMax(
                    inflowContentWidth,
                    rowMetrics.horizontalAdvance(formatHotInflow(sortedItems.at(row).mainNetInflow)) + 8
                );
            }

            const int minNameWidth = 34;
            int pctWidth = qMax(32, pctContentWidth);
            pctWidth = qMin(pctWidth, qMax(32, columnRect.width() / 3));

            int inflowWidth = qMax(34, inflowContentWidth);
            inflowWidth = qMin(inflowWidth, qMax(34, columnRect.width() - minNameWidth - pctWidth));

            int nameWidth = qMax(minNameWidth, columnRect.width() - pctWidth - inflowWidth);
            int overflow = nameWidth + pctWidth + inflowWidth - columnRect.width();
            if (overflow > 0) {
                const int cutInflow = qMin(overflow, qMax(0, inflowWidth - 34));
                inflowWidth -= cutInflow;
                overflow -= cutInflow;

                const int cutPct = qMin(overflow, qMax(0, pctWidth - 32));
                pctWidth -= cutPct;
                overflow -= cutPct;

                nameWidth = qMax(20, columnRect.width() - pctWidth - inflowWidth);
            }

            popupPainter.setFont(valueFont);
            for (int row = 0; row < visibleRows; ++row) {
                const HotRankItem& item = sortedItems.at(row);
                const QRect rowRect(
                    rowsRect.left(),
                    rowsTop + row * rowStep,
                    rowsRect.width(),
                    rowHeight
                );

                const QRect nameRect(rowRect.left(), rowRect.top(), nameWidth, rowRect.height());
                const QRect pctRect(nameRect.right() + 1, rowRect.top(), pctWidth, rowRect.height());
                const QRect inflowRect(pctRect.right() + 1, rowRect.top(), inflowWidth, rowRect.height());

                const QColor pctColor = item.pct > 0.0
                    ? m_cfg.upColor
                    : (item.pct < 0.0 ? m_cfg.downColor : m_cfg.flatColor);

                popupPainter.setPen(textColor);
                popupPainter.drawText(
                    nameRect.adjusted(1, 0, -2, 0),
                    Qt::AlignVCenter | Qt::AlignLeft,
                    rowMetrics.elidedText(item.name.trimmed(), Qt::ElideRight, qMax(8, nameRect.width() - 2))
                );

                popupPainter.setPen(pctColor);
                popupPainter.drawText(pctRect, Qt::AlignCenter, formatHotPct(item.pct));
                popupPainter.drawText(
                    inflowRect.adjusted(2, 0, -1, 0),
                    Qt::AlignVCenter | Qt::AlignRight,
                    rowMetrics.elidedText(formatHotInflow(item.mainNetInflow), Qt::ElideRight, qMax(8, inflowRect.width() - 2))
                );

                if (row + 1 < visibleRows) {
                    QPen gridPen(QColor(textColor.red(), textColor.green(), textColor.blue(), 50));
                    gridPen.setStyle(Qt::DashLine);
                    gridPen.setWidthF(0.6);
                    gridPen.setCosmetic(true);
                    popupPainter.setPen(gridPen);
                    const int lineY = rowRect.bottom() + qMax(1, rowGap / 2);
                    popupPainter.drawLine(rowRect.left(), lineY, rowRect.right(), lineY);
                }
            }
        };

        if (useConceptData) {
            drawRankColumn(rankArea, risingItems, 8);
        } else {
            drawRankColumn(riseSectionRect, risingItems, 4);
            drawRankColumn(fallSectionRect, fallingItems, 4);
        }

        const int rightGap = 12;
        const int maxDistributionCardHeight = qMax(160, rightRect.height() - rightGap - 120);
        const int distributionCardHeight = qBound(
            160,
            qRound(static_cast<double>(rightRect.height()) * 0.45),
            maxDistributionCardHeight
        );
        const QRect distributionCard(rightRect.left(), rightRect.top(), rightRect.width(), distributionCardHeight);
        const QRect timelineCard(
            rightRect.left(),
            distributionCard.bottom() + 1 + rightGap,
            rightRect.width(),
            qMax(120, rightRect.bottom() - (distributionCard.bottom() + rightGap))
        );

        drawCard(distributionCard, QString(), Qt::AlignLeft | Qt::AlignTop);
        drawCard(timelineCard, QString(), Qt::AlignLeft | Qt::AlignTop);

        const QColor axisColor(textColor.red(), textColor.green(), textColor.blue(), 165);

        const QRect distributionInner = distributionCard.adjusted(12, 36, -12, -12);
        const QRect distributionPlotRect = distributionInner.adjusted(34, 8, -8, -24);
        if (m_snapshot.distributionValid
            && !m_snapshot.distribution.isEmpty()
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
                    const QRect barRect(x, distributionPlotRect.bottom() - barHeight + 1, barWidth, barHeight);

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
                QVector<int> visibleBucketIndices;
                visibleBucketIndices.reserve(visibleLabelCount);
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
                    visibleBucketIndices.push_back(bucketIndex);

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
                    const int textWidth = qMax(barRect.width() + 4, valueMetrics.horizontalAdvance(valueText) + 2);
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

        const QRect timelineInner = timelineCard.adjusted(12, 36, -12, -12);
        const QRect timelineLegendRect(timelineInner.left(), timelineInner.top(), timelineInner.width(), 14);
        const QRect timelinePlotRect = timelineInner.adjusted(34, 20, -8, -30);

        if (m_snapshot.overviewTimeline.size() >= 2
            && timelinePlotRect.width() > 24
            && timelinePlotRect.height() > 24) {
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
                    const double ratio = static_cast<double>(qMax(0, value)) / static_cast<double>(yAxisMax);
                    return timelinePlotRect.bottom() - qRound(ratio * timelinePlotRect.height());
                };

                auto buildSeriesPoints = [&](int seriesKind) {
                    QVector<QPoint> points;
                    points.reserve(m_snapshot.overviewTimeline.size());
                    for (const MarketBreadthTimelinePoint& timelinePoint : m_snapshot.overviewTimeline) {
                        int value = -1;
                        if (seriesKind == 0) {
                            value = timelinePoint.riseCount;
                        } else {
                            value = timelinePoint.fallCount;
                        }
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
                    popupPainter.drawLine(legendX, timelineLegendRect.center().y(), legendX + 12, timelineLegendRect.center().y());
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
            "富时A50", "美元/人民币", "黄金T+D", "纳斯达克", "标普500", "日经225", "韩国KOSPI"
        };

        QFont idxNameFont = subtitleFont;
        idxNameFont.setPointSizeF(qMax(7.8, idxNameFont.pointSizeF() - 0.6));
        QFont idxPriceFont = bodyFont;
        idxPriceFont.setPointSizeF(qMax(8.0, idxPriceFont.pointSizeF() - 0.3));
        QFont idxChangeFont = bodyFont;
        idxChangeFont.setPointSizeF(qMax(7.4, idxChangeFont.pointSizeF() - 0.5));

        const auto fmtIndexPrice = [](const IndexQuoteItem& item) -> QString {
            if (!std::isfinite(item.price)) {
                return QStringLiteral("--");
            }
            const double absP = std::fabs(item.price);
            const int prec = absP < 10.0 ? 4 : (absP < 1000.0 ? 2 : 0);
            return QString::number(item.price, 'f', prec);
        };

        const auto fmtIndexChange = [](const IndexQuoteItem& item) -> QString {
            if (!std::isfinite(item.change) || !std::isfinite(item.pct)) {
                return QStringLiteral("--");
            }
            const double absChg = std::fabs(item.change);
            const int chgPrec = absChg < 0.1 ? 4 : (absChg < 100.0 ? 2 : 0);
            QString chgStr = QString::number(item.change, 'f', chgPrec);
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


void  MarketBreadthDetailWindow::startLastUpdatedTextTimer() {
    if (!isVisible() || !m_lastUpdatedTextTimer) {
        return;
    }
    if (!m_lastUpdatedTextTimer->isActive()) {
        m_lastUpdatedTextTimer->start();
    }
}


void  MarketBreadthDetailWindow::stopLastUpdatedTextTimer() {
    if (m_lastUpdatedTextTimer) {
        m_lastUpdatedTextTimer->stop();
    }
}


void  MarketBreadthDetailWindow::ensureSingleVisible() {
    if (s_visiblePopup && s_visiblePopup != this) {
        s_visiblePopup->hidePopup();
    }
    s_visiblePopup = this;
}


void  MarketBreadthDetailWindow::enforceAlwaysOnTop() {
#ifdef WIN32
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd) {
        SetWindowPos(
            hwnd,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING
        );
    }
#endif
    raise();
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
