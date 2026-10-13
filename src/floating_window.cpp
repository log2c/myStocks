#include "floating_window.h"
#include "market_breadth_window.h"

#include "app_constants.h"
#include "i18n.h"
#include "quote_provider.h"
#include "timeline_popup.h"
#include "watchlist_utils.h"

#include <QDesktopServices>
#include <QDateTime>
#include <QCursor>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHash>
#include <QHoverEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPushButton>
#include <QPainterPath>
#include <QPalette>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QStyleFactory>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <limits>
#include <utility>

#ifdef WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <Carbon/Carbon.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace {

QColor mixColor(const QColor& from, const QColor& to, qreal t) {
    const qreal clamped = qBound(0.0, t, 1.0);
    return QColor(
        qRound(from.red() + (to.red() - from.red()) * clamped),
        qRound(from.green() + (to.green() - from.green()) * clamped),
        qRound(from.blue() + (to.blue() - from.blue()) * clamped),
        qRound(from.alpha() + (to.alpha() - from.alpha()) * clamped)
    );
}

struct HoverReadingTheme {
    QColor background;
    QColor surface;
    QColor border;
    QColor textPrimary;
};

HoverReadingTheme hoverReadingThemeForMode(const QString& rawMode, bool transparentBackgroundEnabled) {
    const QString mode = normalizeHoverReadingUiMode(rawMode);
    if (transparentBackgroundEnabled) {
#if defined(Q_OS_MACOS)
        if (mode == QLatin1String("light")) {
            return {
                QColor(255, 255, 255, 184),
                QColor(255, 255, 255, 184),
                QColor(255, 255, 255, 89),
                QColor(QStringLiteral("#1F1F1F")),
            };
        }

        return {
            QColor(30, 30, 30, 184),
            QColor(30, 30, 30, 184),
            QColor(255, 255, 255, 20),
            QColor(QStringLiteral("#E6E6E6")),
        };
#elif defined(Q_OS_WIN)
        if (mode == QLatin1String("light")) {
            return {
                QColor(255, 255, 255, 217),
                QColor(255, 255, 255, 217),
                QColor(0, 0, 0, 20),
                QColor(QStringLiteral("#1F1F1F")),
            };
        }

        return {
            QColor(32, 32, 32, 204),
            QColor(32, 32, 32, 204),
            QColor(255, 255, 255, 15),
            QColor(QStringLiteral("#E6E6E6")),
        };
#else
        if (mode == QLatin1String("light")) {
            return {
                QColor(255, 255, 255, 217),
                QColor(255, 255, 255, 217),
                QColor(0, 0, 0, 20),
                QColor(QStringLiteral("#1F1F1F")),
            };
        }

        return {
            QColor(32, 32, 32, 204),
            QColor(32, 32, 32, 204),
            QColor(255, 255, 255, 15),
            QColor(QStringLiteral("#E6E6E6")),
        };
#endif
    }

    if (mode == QLatin1String("light")) {
        return {
            QColor(QStringLiteral("#FFFFFF")),
            QColor(QStringLiteral("#F5F5F5")),
            QColor(QStringLiteral("#D6D6D6")),
            QColor(QStringLiteral("#1F1F1F")),
        };
    }

    return {
        QColor(QStringLiteral("#1E1E1E")),
        QColor(QStringLiteral("#252525")),
        QColor(QStringLiteral("#3A3A3A")),
        QColor(QStringLiteral("#E6E6E6")),
    };
}

QColor hoverReadingTableBackgroundColor(const HoverReadingTheme& theme, const QString& rawMode) {
    if (normalizeHoverReadingUiMode(rawMode) == QLatin1String("light")) {
        return theme.surface;
    }
    return theme.background;
}

qreal configuredWindowOpacity(const AppConfig& cfg) {
    const double effectiveOpacity = cfg.transparentBackgroundEnabled
        ? static_cast<double>(qBound(0, cfg.transparentBackgroundOpacity, 100)) / 100.0
        : qBound(0.0, cfg.opacity, 1.0);
    return qBound(0.0, effectiveOpacity, 1.0);
}

int floatingWindowPaddingPx(const AppConfig& cfg) {
    return qMax(0, qRound(qMax(0.0, cfg.floatingWindowPaddingPx)));
}

QString tableCellPaddingStyle(const AppConfig& cfg) {
    const double rawPadding = qMax(0.0, cfg.floatingWindowPaddingPx);
    return QStringLiteral("padding: 0 %1px;")
        .arg(QString::number(rawPadding, 'f', 1));
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

#if defined(WIN32)
bool isVirtualKeyPressed(int virtualKey) {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}
#elif defined(Q_OS_MACOS)
bool isMacKeyPressed(CGKeyCode keycode) {
    return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, keycode);
}
#endif

bool isActivationKeyPressed(const QString& rawKey) {
    const QString key = normalizeMousePassthroughActivationKey(rawKey);

#if defined(WIN32)
    if (key == QLatin1String("ctrl")) {
        return isVirtualKeyPressed(VK_LCONTROL) || isVirtualKeyPressed(VK_RCONTROL);
    }
    if (key == QLatin1String("shift")) {
        return isVirtualKeyPressed(VK_LSHIFT) || isVirtualKeyPressed(VK_RSHIFT);
    }
    if (key == QLatin1String("alt")) {
        return isVirtualKeyPressed(VK_LMENU) || isVirtualKeyPressed(VK_RMENU);
    }
    return false;
#elif defined(Q_OS_MACOS)
    if (key == QLatin1String("ctrl")) {
        return isMacKeyPressed(kVK_Control) || isMacKeyPressed(kVK_RightControl);
    }
    if (key == QLatin1String("shift")) {
        return isMacKeyPressed(kVK_Shift) || isMacKeyPressed(kVK_RightShift);
    }
    if (key == QLatin1String("alt")) {
        return isMacKeyPressed(kVK_Option) || isMacKeyPressed(kVK_RightOption);
    }
    if (key == QLatin1String("command")) {
        return isMacKeyPressed(kVK_Command) || isMacKeyPressed(kVK_RightCommand);
    }
    return false;
#else
    const Qt::KeyboardModifiers modifiers = QGuiApplication::queryKeyboardModifiers();
    if (key == QLatin1String("ctrl")) {
        return modifiers.testFlag(Qt::ControlModifier);
    }
    if (key == QLatin1String("shift")) {
        return modifiers.testFlag(Qt::ShiftModifier);
    }
    if (key == QLatin1String("alt")) {
        return modifiers.testFlag(Qt::AltModifier);
    }
    return false;
#endif
}

#if defined(Q_OS_MACOS)
void* macWindowHandleForWidget(const QWidget* widget) {
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

void setMacWindowIgnoresMouseEvents(const QWidget* widget, bool ignore) {
    void* nsWindow = macWindowHandleForWidget(widget);
    if (!nsWindow) {
        return;
    }

    auto sendBoolMessage = reinterpret_cast<void (*)(void*, SEL, bool)>(objc_msgSend);
    sendBoolMessage(nsWindow, sel_registerName("setIgnoresMouseEvents:"), ignore);
}
#endif

QString extractSixDigitsSymbol(const QString& rawCode) {
    QString code = rawCode.trimmed().toLower();
    if (code.startsWith(QStringLiteral("sh"))
        || code.startsWith(QStringLiteral("sz"))
        || code.startsWith(QStringLiteral("bj"))) {
        code = code.mid(2);
    } else {
        const int dot = code.indexOf(QLatin1Char('.'));
        if (dot > 0 && dot < code.size() - 1) {
            const QString market = code.left(dot);
            const QString symbol = code.mid(dot + 1);
            if ((market == QLatin1String("0") || market == QLatin1String("1"))
                && symbol.size() == 6
                && watchlist_utils::isDigitsOnly(symbol)) {
                return symbol;
            }
            return {};
        }
    }
    return (code.size() == 6 && watchlist_utils::isDigitsOnly(code)) ? code : QString();
}

QString buildEastmoneyDetailUrl(const QString& rawCode) {
    const QString code = rawCode.trimmed().toLower();
    if (code.isEmpty()) {
        return {};
    }

    QString prefix;
    QString symbol;

    if (code.startsWith(QStringLiteral("sh"))
        || code.startsWith(QStringLiteral("sz"))
        || code.startsWith(QStringLiteral("bj"))) {
        prefix = code.left(2);
        symbol = code.mid(2);
    } else {
        symbol = extractSixDigitsSymbol(rawCode);
        if (symbol.isEmpty()) {
            return {};
        }
        const QChar head = symbol[0];
        if (head == QLatin1Char('6') || head == QLatin1Char('5') || head == QLatin1Char('9')) {
            prefix = QStringLiteral("sh");
        } else {
            prefix = QStringLiteral("sz");
        }
    }

    if (symbol.size() != 6 || !watchlist_utils::isDigitsOnly(symbol)) {
        return {};
    }

    return QStringLiteral("https://quote.eastmoney.com/concept/")
        + prefix + symbol + QStringLiteral(".html");
}

} // namespace


namespace {

class BottomGridTableView : public QTableView {
public:
    explicit BottomGridTableView(QWidget* parent = nullptr)
        : QTableView(parent) {
        m_animationTimerId = startTimer(40);
    }

    void setBottomGridVisible(bool visible) {
        if (m_bottomGridVisible == visible) {
            return;
        }
        m_bottomGridVisible = visible;
        viewport()->update();
    }

    void setBottomGridColor(const QColor& color) {
        if (m_bottomGridColor == color) {
            return;
        }
        m_bottomGridColor = color;
        if (m_bottomGridVisible) {
            viewport()->update();
        }
    }

    void syncSpecialRowSpans(const QuoteModel* quoteModel) {
        clearSpans();

        if (!quoteModel) {
            return;
        }

        QVector<int> visibleColumns;
        QHeaderView* header = horizontalHeader();
        for (int visual = 0; visual < header->count(); ++visual) {
            const int logical = header->logicalIndex(visual);
            if (logical < 0 || isColumnHidden(logical)) {
                continue;
            }
            visibleColumns.push_back(logical);
        }

        if (visibleColumns.isEmpty()) {
            return;
        }
        for (int row = 0; row < quoteModel->rowCount(); ++row) {
            const QuoteModel::RowKind kind = quoteModel->rowKind(row);
            if (kind == QuoteModel::RowKindQuote) {
                continue;
            }

            if (kind == QuoteModel::RowKindMarketBreadth) {
                setSpan(row, visibleColumns.first(), 1, visibleColumns.size());
                continue;
            }

            if (visibleColumns.size() < 2) {
                continue;
            }
            setSpan(row, visibleColumns.at(1), 1, visibleColumns.size() - 1);
        }
    }

    int animationTick() const {
        return m_animationTick;
    }

    void setHotRankFlipSecs(double secs) {
        m_hotRankFlipSecs = qBound(0.5, secs, 60.0);
    }

    double hotRankFlipSecs() const {
        return m_hotRankFlipSecs;
    }

protected:
    void timerEvent(QTimerEvent* event) override {
        if (event && event->timerId() == m_animationTimerId) {
            ++m_animationTick;
            viewport()->update();
            return;
        }

        QTableView::timerEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        QTableView::paintEvent(event);

        if (!m_bottomGridVisible || !model()) {
            return;
        }

        int lastVisibleRow = -1;
        const int rowCount = model()->rowCount(rootIndex());
        for (int row = rowCount - 1; row >= 0; --row) {
            if (!isRowHidden(row)) {
                lastVisibleRow = row;
                break;
            }
        }

        if (lastVisibleRow <= 0) {
            return;
        }

        const QRect clipRect = event ? event->rect() : viewport()->rect();
        QPainter painter(viewport());
        QPen pen(m_bottomGridColor);
        pen.setCosmetic(true);
        painter.setPen(pen);

        const int left = 0;
        const int right = qMax(0, viewport()->width() - 1);
        for (int row = 0; row < lastVisibleRow; ++row) {
            if (isRowHidden(row)) {
                continue;
            }

            const int y = rowViewportPosition(row) + rowHeight(row) - 1;
            if (y < clipRect.top() || y > clipRect.bottom()) {
                continue;
            }

            painter.drawLine(left, y, right, y);
        }
    }

private:
    bool m_bottomGridVisible = false;
    QColor m_bottomGridColor = QColor(255, 255, 255, 80);
    int m_animationTimerId = 0;
    int m_animationTick = 0;
    double m_hotRankFlipSecs = 2.6;
};

class HotRankFlipDelegate : public QStyledItemDelegate {
public:
    HotRankFlipDelegate(QTableView* table, QuoteModel* model, QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
        , m_table(table)
        , m_model(model) {}

    void paint(
        QPainter* painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index
    ) const override {
        if (!m_table || !m_model || m_model->rowKind(index.row()) == QuoteModel::RowKindQuote) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        const QuoteModel::RowKind rowKind = m_model->rowKind(index.row());

        QVector<int> visibleColumns;
        QHeaderView* header = m_table->horizontalHeader();
        for (int visual = 0; visual < header->count(); ++visual) {
            const int logical = header->logicalIndex(visual);
            if (logical < 0 || m_table->isColumnHidden(logical)) {
                continue;
            }
            visibleColumns.push_back(logical);
        }

        if (visibleColumns.isEmpty()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        const int contentColumn = (rowKind == QuoteModel::RowKindMarketBreadth || visibleColumns.size() < 2)
            ? visibleColumns.first()
            : visibleColumns.at(1);
        if (index.column() != contentColumn) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        if (rowKind == QuoteModel::RowKindMarketBreadth) {
            paintMarketBreadthRow(painter, option, index);
            return;
        }

        QStyleOptionViewItem baseOption(option);
        initStyleOption(&baseOption, index);
        baseOption.text.clear();
        QStyledItemDelegate::paint(painter, baseOption, index);

        const QRect textRect = option.rect.adjusted(8, 0, -8, 0);
        const QVariant foreground = index.data(Qt::ForegroundRole);
        const QColor textColor = foreground.canConvert<QColor>()
            ? qvariant_cast<QColor>(foreground)
            : baseOption.palette.color(QPalette::Text);

        painter->save();
        painter->setPen(textColor);

        const int alignment = index.data(Qt::TextAlignmentRole).toInt();
        if (!m_model->specialRowHasData(index.row())) {
            const QString text = m_model->specialRowText(index.row());
            painter->drawText(
                textRect,
                alignment != 0 ? Qt::Alignment(alignment) : (Qt::AlignCenter),
                text
            );
            painter->restore();
            return;
        }

        const int entryCount = m_model->specialRowEntryCount(index.row());
        if (entryCount <= 0) {
            painter->restore();
            return;
        }

        const int currentIndex = currentEntryIndex(entryCount);
        const QString currentText = m_model->specialRowEntryText(index.row(), currentIndex);
        const QColor currentColor = m_model->specialRowEntryColor(index.row(), currentIndex);
        if (entryCount == 1) {
            painter->setPen(currentColor);
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, currentText);
            painter->restore();
            return;
        }

        const int nextIndex = (currentIndex + 1) % entryCount;
        const QString nextText = m_model->specialRowEntryText(index.row(), nextIndex);
        const QColor nextColor = m_model->specialRowEntryColor(index.row(), nextIndex);
        const int yOffset = qRound(flipProgress() * textRect.height());

        painter->setClipRect(textRect);
        drawAlignedText(*painter, textRect.translated(0, -yOffset), currentText, currentColor);
        drawAlignedText(*painter, textRect.translated(0, textRect.height() - yOffset), nextText, nextColor);

        painter->restore();
    }

private:
    int animationTick() const {
        const auto* table = dynamic_cast<const BottomGridTableView*>(m_table);
        return table ? table->animationTick() : 0;
    }

    static QColor bestTextColorForBackground(const QColor& bg) {
        return bg.lightness() < 140 ? QColor(Qt::white) : QColor(Qt::black);
    }

    void paintMarketBreadthRow(
        QPainter* painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index
    ) const {
        QStyleOptionViewItem baseOption(option);
        initStyleOption(&baseOption, index);
        baseOption.text.clear();
        QStyledItemDelegate::paint(painter, baseOption, index);

        const QRect contentRect = option.rect.adjusted(8, 4, -8, -4);
        if (contentRect.width() <= 10 || contentRect.height() <= 6) {
            return;
        }

        const QString fallbackText = m_model->specialRowText(index.row());
        const QVariant foreground = index.data(Qt::ForegroundRole);
        const QColor textColor = foreground.canConvert<QColor>()
            ? qvariant_cast<QColor>(foreground)
            : option.palette.color(QPalette::Text);

        painter->save();

        if (!m_model->marketBreadthValid()) {
            painter->setPen(textColor);
            painter->drawText(
                contentRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                fallbackText
            );
            painter->restore();
            return;
        }

        const int up = m_model->marketBreadthUpCount();
        const int flat = m_model->marketBreadthFlatCount();
        const int down = m_model->marketBreadthDownCount();
        const int total = up + flat + down;
        if (total <= 0) {
            painter->setPen(textColor);
            painter->drawText(
                contentRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                fallbackText
            );
            painter->restore();
            return;
        }

        const int barHeight = qMax(2, contentRect.height() / 5);
        const int barTop = contentRect.top() + (contentRect.height() - barHeight) / 2;
        QRect barRect(contentRect.left(), barTop, contentRect.width(), barHeight);
        if (barRect.width() < 60) {
            painter->setPen(textColor);
            painter->drawText(
                contentRect,
                Qt::AlignLeft | Qt::AlignVCenter,
                fallbackText
            );
            painter->restore();
            return;
        }

        const auto computeSegmentWidths = [&](const QVector<int>& values) -> QVector<int> {
            QVector<int> widths(values.size(), 0);
            QVector<int> positiveIndexes;
            positiveIndexes.reserve(values.size());

            int reservedWidth = 0;
            int reservedValue = 0;
            for (int i = 0; i < values.size(); ++i) {
                if (values.at(i) <= 0) {
                    continue;
                }
                widths[i] = 1;
                positiveIndexes.push_back(i);
                ++reservedWidth;
                reservedValue += 1;
            }

            const int extraWidth = qMax(0, barRect.width() - reservedWidth);
            const int extraValue = qMax(0, total - reservedValue);
            if (extraWidth <= 0 || extraValue <= 0 || positiveIndexes.isEmpty()) {
                return widths;
            }

            QVector<double> remainders(values.size(), 0.0);
            int assignedExtraWidth = 0;
            for (int index : positiveIndexes) {
                const int weightedValue = qMax(0, values.at(index) - 1);
                if (weightedValue <= 0) {
                    continue;
                }

                const double rawExtra = static_cast<double>(extraWidth) * static_cast<double>(weightedValue)
                    / static_cast<double>(extraValue);
                const int extra = static_cast<int>(std::floor(rawExtra));
                widths[index] += extra;
                remainders[index] = rawExtra - static_cast<double>(extra);
                assignedExtraWidth += extra;
            }

            int remainingExtraWidth = extraWidth - assignedExtraWidth;
            while (remainingExtraWidth > 0) {
                int bestIndex = -1;
                double bestRemainder = -1.0;
                for (int index : positiveIndexes) {
                    if (remainders.at(index) > bestRemainder) {
                        bestRemainder = remainders.at(index);
                        bestIndex = index;
                    }
                }
                if (bestIndex < 0) {
                    break;
                }
                ++widths[bestIndex];
                remainders[bestIndex] = 0.0;
                --remainingExtraWidth;
            }

            return widths;
        };

        const QVector<int> segmentWidths = computeSegmentWidths({up, flat, down});
        const int upWidth = segmentWidths.value(0);
        const int flatWidth = segmentWidths.value(1);
        const int downWidth = segmentWidths.value(2);

        const QColor upColor = m_model->marketBreadthUpColor();
        const QColor flatColor = m_model->marketBreadthFlatColor();
        const QColor downColor = m_model->marketBreadthDownColor();

        const QRect upRect(barRect.left(), barRect.top(), upWidth, barRect.height());
        const QRect flatRect(upRect.right() + 1, barRect.top(), flatWidth, barRect.height());
        const QRect downRect(flatRect.right() + 1, barRect.top(), qMax(0, downWidth), barRect.height());
        QFontMetrics fm(painter->font());
        struct BreadthLabel {
            int segmentIndex = -1;
            int priority = 0;
            QString text;
            QColor color;
            QRect segmentRect;
            QRect rect;
            int preferredX = 0;
            int minX = 0;
            int maxX = 0;
        };

        const int labelPadding = 8;
        const int labelGap = 2;
        const int labelHeight = qMin(contentRect.height(), fm.height());
        const int labelY = contentRect.center().y() - labelHeight / 2;
        const int dominantSegment = up >= down ? 0 : 2;

        const auto makeLabel = [&](int segmentIndex, int value, const QRect& segmentRect, const QColor& color) {
            BreadthLabel label;
            label.segmentIndex = segmentIndex;
            label.priority = (segmentIndex == dominantSegment) ? 2 : 1;
            label.text = QString::number(value);
            label.color = color;
            label.segmentRect = segmentRect;

            const int desiredW = qMax(10, fm.horizontalAdvance(label.text) + labelPadding);
            const int labelWidth = qMin(contentRect.width(), desiredW);
            label.minX = contentRect.left();
            label.maxX = qMax(label.minX, contentRect.right() - labelWidth + 1);

            int preferredX = segmentRect.center().x() - labelWidth / 2;
            if (segmentRect.width() < labelWidth) {
                if (segmentIndex == 0) {
                    preferredX = contentRect.left();
                } else if (segmentIndex == 2) {
                    preferredX = contentRect.right() - labelWidth + 1;
                }
            }
            label.preferredX = qBound(label.minX, preferredX, label.maxX);
            label.rect = QRect(label.preferredX, labelY, labelWidth, labelHeight);
            return label;
        };

        QVector<BreadthLabel> labels;
        if (up > 0) {
            labels.push_back(makeLabel(0, up, upRect, upColor));
        }
        if (down > 0) {
            labels.push_back(makeLabel(2, down, downRect, downColor));
        }

        auto requiredWidth = [&]() -> int {
            if (labels.isEmpty()) {
                return 0;
            }

            int totalWidth = 0;
            for (const BreadthLabel& label : labels) {
                totalWidth += label.rect.width();
            }
            totalWidth += labelGap * (labels.size() - 1);
            return totalWidth;
        };

        while (labels.size() > 1 && requiredWidth() > contentRect.width()) {
            int removeIndex = -1;
            int removePriority = std::numeric_limits<int>::max();
            for (int i = 0; i < labels.size(); ++i) {
                if (labels.at(i).priority < removePriority
                    || (labels.at(i).priority == removePriority
                        && (removeIndex < 0
                            || labels.at(i).segmentIndex > labels.at(removeIndex).segmentIndex))) {
                    removePriority = labels.at(i).priority;
                    removeIndex = i;
                }
            }
            if (removeIndex < 0) {
                break;
            }
            labels.removeAt(removeIndex);
        }

        const auto layoutLabels = [&]() {
            int currentMinX = contentRect.left();
            for (BreadthLabel& label : labels) {
                int x = qMax(label.preferredX, currentMinX);
                x = qMin(x, label.maxX);
                label.rect.moveLeft(x);
                currentMinX = label.rect.right() + 1 + labelGap;
            }

            int currentMaxX = contentRect.right() + 1;
            for (int i = labels.size() - 1; i >= 0; --i) {
                BreadthLabel& label = labels[i];
                int x = qMin(label.rect.left(), currentMaxX - label.rect.width());
                x = qMax(x, label.minX);
                label.rect.moveLeft(x);
                currentMaxX = label.rect.left() - labelGap;
            }

            currentMinX = contentRect.left();
            for (BreadthLabel& label : labels) {
                int x = qMax(label.rect.left(), currentMinX);
                x = qMin(x, label.maxX);
                label.rect.moveLeft(x);
                currentMinX = label.rect.right() + 1 + labelGap;
            }
        };

        layoutLabels();

        const auto fillStripeWithHoles = [painter](const QRect& stripe, const QColor& color, const QVector<QRect>& holes) {
            if (stripe.width() <= 0 || stripe.height() <= 0) {
                return;
            }

            QVector<QRect> cuts;
            cuts.reserve(holes.size());
            for (const QRect& hole : holes) {
                const QRect cut = stripe.intersected(hole);
                if (!cut.isEmpty()) {
                    cuts.push_back(cut);
                }
            }

            if (cuts.isEmpty()) {
                painter->fillRect(stripe, color);
                return;
            }

            std::sort(cuts.begin(), cuts.end(), [](const QRect& lhs, const QRect& rhs) {
                return lhs.left() < rhs.left();
            });

            int currentLeft = stripe.left();
            int currentRight = stripe.right();
            for (const QRect& cut : cuts) {
                if (cut.left() > currentLeft) {
                    painter->fillRect(
                        QRect(currentLeft, stripe.top(), cut.left() - currentLeft, stripe.height()),
                        color
                    );
                }
                currentLeft = qMax(currentLeft, cut.right() + 1);
                if (currentLeft > currentRight) {
                    break;
                }
            }

            if (currentLeft <= currentRight) {
                painter->fillRect(
                    QRect(currentLeft, stripe.top(), currentRight - currentLeft + 1, stripe.height()),
                    color
                );
            }
        };

        QVector<QRect> stripeHoles;
        stripeHoles.reserve(labels.size());
        for (const BreadthLabel& label : labels) {
            stripeHoles.push_back(label.rect);
        }

        fillStripeWithHoles(upRect, upColor, stripeHoles);
        if (flatRect.width() > 0) {
            fillStripeWithHoles(flatRect, flatColor, stripeHoles);
        }
        fillStripeWithHoles(downRect, downColor, stripeHoles);
        for (const BreadthLabel& label : labels) {
            painter->setPen(label.color);
            painter->drawText(label.rect, Qt::AlignCenter, label.text);
        }

        painter->restore();
    }

    int currentEntryIndex(int entryCount) const {
        if (entryCount <= 0) {
            return 0;
        }

        return (animationTick() / flipCycleTicks()) % entryCount;
    }

    qreal flipProgress() const {
        const int holdTicks = flipHoldTicks();
        const int animTicks = flipAnimTicks();
        const int phaseTick = animationTick() % flipCycleTicks();
        if (phaseTick < holdTicks) {
            return 0.0;
        }

        return qBound(
            0.0,
            static_cast<qreal>(phaseTick - holdTicks) / static_cast<qreal>(animTicks),
            1.0
        );
    }

    void drawAlignedText(
        QPainter& painter,
        const QRect& rect,
        const QString& text,
        const QColor& color
    ) const {
        painter.setPen(color);
        painter.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, text);
    }

    int flipCycleTicks() const {
        const auto* table = dynamic_cast<const BottomGridTableView*>(m_table);
        const double secs = table ? table->hotRankFlipSecs() : 2.6;
        return qMax(2, qRound((secs * 1000.0) / 40.0));
    }

    int flipHoldTicks() const {
        const int cycle = flipCycleTicks();
        return qMax(1, cycle * 4 / 5);
    }

    int flipAnimTicks() const {
        return qMax(1, flipCycleTicks() - flipHoldTicks());
    }

private:
    QTableView* m_table = nullptr;
    QuoteModel* m_model = nullptr;
};

} // namespace

FloatingWindow::FloatingWindow(QuoteModel* model, QWidget* parent)
    : QWidget(parent)
    , m_model(model) {
    Qt::WindowFlags flags = Qt::FramelessWindowHint;
#ifdef WIN32
    // Qt::Tool suppresses the taskbar entry on Windows.
    flags |= Qt::Tool;
#else
    // Use a normal top-level window on non-Windows to avoid tool-window stacking quirks.
    flags |= Qt::Window;
#endif
    if (m_cfg.floatingWindowAlwaysOnTop) {
        flags |= Qt::WindowStaysOnTopHint;
    } else {
        flags |= Qt::WindowStaysOnBottomHint;
    }
    setWindowFlags(flags);
    setAttribute(Qt::WA_TranslucentBackground, true);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_panel = new QFrame(this);
    m_panel->setObjectName("panel");
    m_panel->setAttribute(Qt::WA_StyledBackground, true);

    QVBoxLayout* panelLayout = new QVBoxLayout(m_panel);
    panelLayout->setSpacing(0);
    const int initialPadding = floatingWindowPaddingPx(m_cfg);
    panelLayout->setContentsMargins(
        initialPadding,
        initialPadding,
        initialPadding,
        initialPadding
    );

    // Helper lambda for shared QTableView setup
    const auto setupTable = [&](BottomGridTableView* tbl, bool hasHeader) {
        tbl->setModel(m_model);
        tbl->setItemDelegate(new HotRankFlipDelegate(tbl, m_model, tbl));
        tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tbl->setSelectionMode(QAbstractItemView::NoSelection);
        tbl->setShowGrid(false);
        tbl->setAlternatingRowColors(false);
        tbl->setFocusPolicy(Qt::NoFocus);
        tbl->setMouseTracking(true);
        tbl->viewport()->setMouseTracking(true);
        tbl->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tbl->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tbl->setFrameShape(QFrame::NoFrame);
        tbl->viewport()->setObjectName("tableViewport");
        tbl->viewport()->setAttribute(Qt::WA_StyledBackground, true);
        tbl->verticalHeader()->setVisible(false);
        tbl->verticalHeader()->setDefaultSectionSize(26);
        tbl->horizontalHeader()->setVisible(hasHeader);
#ifdef WIN32
        if (QStyle* fusion = QStyleFactory::create("Fusion")) {
            tbl->setStyle(fusion);
            if (hasHeader) {
                tbl->horizontalHeader()->setStyle(fusion);
            }
        }
#endif
    };

    // Index table (top): shows header + index rows
    m_table = new BottomGridTableView(m_panel);
    setupTable(static_cast<BottomGridTableView*>(m_table), true);

    QHeaderView* header = m_table->horizontalHeader();
    header->viewport()->setObjectName("tableHeaderViewport");
    header->viewport()->setAttribute(Qt::WA_StyledBackground, true);
    header->setSectionsMovable(false);
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(0);
    header->setSectionResizeMode(QHeaderView::Fixed);

    // Group bar — shown only when more than one group exists (between index and stock tables)
    m_groupBar = new QWidget(m_panel);
    m_groupBar->setObjectName("groupBar");
    m_groupBar->setAttribute(Qt::WA_StyledBackground, true);
    m_groupBar->setFixedHeight(28);
    m_groupBarLayout = new QHBoxLayout(m_groupBar);
    m_groupBarLayout->setContentsMargins(initialPadding, 2, 0, 2);
    m_groupBarLayout->setSpacing(4);
    m_groupBarLayout->addStretch();
    m_groupBar->hide();

    // Stock table (bottom): shows stock rows (no header)
    m_stockTable = new BottomGridTableView(m_panel);
    setupTable(static_cast<BottomGridTableView*>(m_stockTable), false);

    panelLayout->addWidget(m_table);
    panelLayout->addWidget(m_groupBar);
    panelLayout->addWidget(m_stockTable);
    root->addWidget(m_panel);

    m_panel->installEventFilter(this);
    m_table->installEventFilter(this);
    m_table->viewport()->installEventFilter(this);
    m_table->horizontalHeader()->installEventFilter(this);
    m_stockTable->installEventFilter(this);
    m_stockTable->viewport()->installEventFilter(this);

    connect(m_model, &QAbstractItemModel::dataChanged, this, [this]() {
        adjustWindowSize();
        refreshMarketBreadthDetailPopup();
        if (m_timelinePopup && !m_timelineHoverCode.isEmpty()) {
            m_timelinePopup->updateQuoteForStock(m_timelineHoverCode,
                m_model->costForCode(m_timelineHoverCode),
                m_model->priceForCode(m_timelineHoverCode));
        }
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this]() {
        adjustWindowSize();
        refreshMarketBreadthDetailPopup();
    });
    connect(m_model, &QAbstractItemModel::layoutChanged, this, [this]() {
        adjustWindowSize();
        refreshMarketBreadthDetailPopup();
    });
    connect(m_model, &QAbstractItemModel::rowsInserted, this, [this]() {
        adjustWindowSize();
        refreshMarketBreadthDetailPopup();
    });
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, [this]() {
        adjustWindowSize();
        refreshMarketBreadthDetailPopup();
    });

    m_hoverTimer = new QTimer(this);
    m_hoverTimer->setSingleShot(true);
    connect(m_hoverTimer, &QTimer::timeout, this, [this]() {
        if (!m_cfg.hoverReadingEnabled) {
            return;
        }

        if (!isCursorInsideWindow()) {
            setHoverReadingActive(false, true);
            return;
        }

        if (m_dragging) {
            m_hoverTimer->start(120);
            return;
        }

        setHoverReadingActive(true, true);
    });

    m_mousePassthroughTimer = new QTimer(this);
    m_mousePassthroughTimer->setInterval(30);
    connect(m_mousePassthroughTimer, &QTimer::timeout, this, [this]() {
        refreshMousePassthroughState();
    });

    m_styleAnimation = new QVariantAnimation(this);
    m_styleAnimation->setDuration(180);
    m_styleAnimation->setEasingCurve(QEasingCurve::InOutCubic);
    connect(m_styleAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_hoverReadingProgress = qBound(0.0, value.toReal(), 1.0);
        applyInterpolatedStyle(m_hoverReadingProgress);
    });

    m_timelinePopup = new SharedTimelineChartPopup(this);
    m_timelinePopup->applyConfig(m_cfg);
    m_marketBreadthDetailPopup = new MarketBreadthDetailWindow(this);
    m_marketBreadthDetailPopup->applyConfig(m_cfg);
    m_marketBreadthDetailPopup->setLanguage(m_model ? m_model->language() : QStringLiteral("en_US"));
    m_marketBreadthDetailPopup->setForceRefreshCallback([this]() {
        emit forceRefreshRequested();
    });
}

void FloatingWindow::setIndexCount(int count) {
    m_indexCount = count;
    adjustWindowSize();
}

void FloatingWindow::unlockWidth() {
    m_maxColumnWidths.clear();
}

void FloatingWindow::applyRowVisibility() {
    if (!m_stockTable || !m_model) {
        return;
    }
    const int total = m_model->rowCount();
    int quotesSeen = 0;
    for (int r = 0; r < total; ++r) {
        const QuoteModel::RowKind kind = m_model->rowKind(r);
        bool isIndex;
        if (kind == QuoteModel::RowKindMarketBreadth) {
            // Market breadth row precedes quotes and always belongs to the top table.
            isIndex = true;
        } else if (kind == QuoteModel::RowKindQuote) {
            isIndex = (quotesSeen < m_indexCount);
            ++quotesSeen;
        } else {
            // Hot sector / hot concept rows trail the quotes — treat as stock rows.
            isIndex = false;
        }
        m_table->setRowHidden(r, !isIndex);
        m_stockTable->setRowHidden(r, isIndex);
    }
}

void FloatingWindow::setGroups(const QVector<StockGroup>& customGroups, int activeGroupIndex, int allGroupPosition) {
    m_customGroups = customGroups;
    m_activeGroupIndex = activeGroupIndex;
    m_allGroupPosition = allGroupPosition;
    rebuildGroupBar();
    adjustWindowSize();
}

void FloatingWindow::setActiveGroupIndex(int index) {
    if (m_activeGroupIndex == index) {
        return;
    }
    m_activeGroupIndex = index;
    applyGroupBarStyle();
}

void FloatingWindow::rebuildGroupBar() {
    if (!m_groupBar || !m_groupBarLayout) {
        return;
    }

    // Remove all existing buttons
    while (m_groupBarLayout->count() > 0) {
        QLayoutItem* item = m_groupBarLayout->takeAt(0);
        if (item) {
            if (item->widget()) {
                item->widget()->deleteLater();
            }
            delete item;
        }
    }

    // Total groups = custom groups + "所有" (always 1)
    const int totalGroups = 1 + m_customGroups.size();
    if (totalGroups <= 1) {
        m_groupBar->hide();
        return;
    }

    // Build group name list in visual order, inserting "所有" at m_allGroupPosition
    const int allPos = qBound(0, m_allGroupPosition, m_customGroups.size());
    QStringList groupNames;
    groupNames.reserve(totalGroups);
    for (int vi = 0; vi < totalGroups; ++vi) {
        if (vi == allPos) {
            groupNames.append(QStringLiteral("\u6240\u6709")); // "所有"
        } else {
            const int ci = vi < allPos ? vi : vi - 1;
            const StockGroup& g = m_customGroups.at(ci);
            groupNames.append(g.name.isEmpty() ? QStringLiteral("?") : g.name);
        }
    }

    for (int i = 0; i < groupNames.size(); ++i) {
        QPushButton* btn = new QPushButton(groupNames.at(i), m_groupBar);
        btn->setObjectName(QStringLiteral("groupBtn"));
        btn->setFlat(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
        btn->setMinimumHeight(20);
        btn->setMaximumHeight(24);
        const int idx = i;
        connect(btn, &QPushButton::clicked, this, [this, idx]() {
            if (m_activeGroupIndex != idx) {
                m_activeGroupIndex = idx;
                applyGroupBarStyle();
                emit groupSwitchRequested(idx);
            }
        });
        m_groupBarLayout->addWidget(btn);
    }
    m_groupBarLayout->addStretch();

    applyGroupBarStyle();
    m_groupBar->show();
}

void FloatingWindow::applyGroupBarStyle() {
    if (!m_groupBar || !m_groupBarLayout) {
        return;
    }

    const QColor textColor = m_cfg.textColor;
    const QColor activeColor = m_cfg.upColor.isValid() ? m_cfg.upColor : QColor(255, 200, 60);
    const QString activeStyle = QStringLiteral(
        "QPushButton#groupBtn {"
        "  color: %1;"
        "  background: transparent;"
        "  border: none;"
        "  padding: 0 6px;"
        "  font-size: 12px;"
        "  font-weight: bold;"
        "}"
    ).arg(activeColor.name());
    const QString normalStyle = QStringLiteral(
        "QPushButton#groupBtn {"
        "  color: %1;"
        "  background: transparent;"
        "  border: none;"
        "  padding: 0 6px;"
        "  font-size: 12px;"
        "}"
    ).arg(textColor.name(QColor::HexArgb));

    int btnIdx = 0;
    for (int i = 0; i < m_groupBarLayout->count(); ++i) {
        QLayoutItem* item = m_groupBarLayout->itemAt(i);
        if (!item || !item->widget()) {
            continue;
        }
        QPushButton* btn = qobject_cast<QPushButton*>(item->widget());
        if (!btn) {
            continue;
        }
        btn->setStyleSheet(btnIdx == m_activeGroupIndex ? activeStyle : normalStyle);
        ++btnIdx;
    }
}

FloatingWindow::~FloatingWindow() {
    if (m_timelinePopup) {
        m_timelinePopup->hidePopup();
        delete m_timelinePopup;
        m_timelinePopup = nullptr;
    }

    if (m_marketBreadthDetailPopup) {
        m_marketBreadthDetailPopup->hidePopup();
        delete m_marketBreadthDetailPopup;
        m_marketBreadthDetailPopup = nullptr;
    }
}

bool FloatingWindow::isCursorInsideWindow() const {
    return frameGeometry().contains(QCursor::pos());
}

bool FloatingWindow::isInteractionActivationPressed() const {
    return isActivationKeyPressed(m_cfg.mousePassthroughActivationKey);
}

bool FloatingWindow::shouldCaptureMouseInteraction() const {
    if (m_dragging) {
        return true;
    }
    return isInteractionActivationPressed() && isCursorInsideWindow();
}

bool FloatingWindow::shouldAllowMouseInteraction() const {
    if (!m_cfg.mousePassthroughEnabled) {
        return true;
    }
    return shouldCaptureMouseInteraction();
}

bool FloatingWindow::isDragTriggerButton(Qt::MouseButton button) const {
    if (button == Qt::LeftButton) {
        return true;
    }
#if defined(Q_OS_MACOS)
    if (button == Qt::RightButton
        && m_cfg.mousePassthroughEnabled
        && normalizeMousePassthroughActivationKey(m_cfg.mousePassthroughActivationKey)
            == QLatin1String("ctrl")) {
        return true;
    }
#endif
    return false;
}

bool FloatingWindow::isCurrentDragButtonHeld(Qt::MouseButtons buttons) const {
    if (m_dragButton == Qt::NoButton) {
        return false;
    }
    return buttons.testFlag(m_dragButton);
}

void FloatingWindow::scheduleHoverReadingTimer() {
    if (!m_hoverTimer
        || !m_cfg.hoverReadingEnabled
        || m_hoverReadingActive
        || m_dragging
        || !shouldAllowMouseInteraction()) {
        return;
    }
    if (!isCursorInsideWindow()) {
        return;
    }

    const int ms = static_cast<int>(qBound(0.1, m_cfg.hoverReadingDelaySecs, 60.0) * 1000.0);
    m_hoverTimer->start(qMax(100, ms));
}

void FloatingWindow::updateHoverReadingState(bool animated) {
    if (m_cfg.mousePassthroughEnabled && !shouldAllowMouseInteraction()) {
        if (m_hoverTimer) {
            m_hoverTimer->stop();
        }
        setHoverReadingActive(false, animated);
        hideTimelinePopup();
        hideMarketBreadthDetailPopup();
        return;
    }

    if (!m_cfg.hoverReadingEnabled) {
        if (m_hoverTimer) {
            m_hoverTimer->stop();
        }
        setHoverReadingActive(false, animated);
        if (m_cfg.mousePassthroughEnabled) {
            hideTimelinePopup();
            hideMarketBreadthDetailPopup();
        }
        return;
    }

    if (isCursorInsideWindow()) {
        if (m_cfg.mousePassthroughEnabled) {
            if (m_hoverTimer) {
                m_hoverTimer->stop();
            }
            if (!m_dragging) {
                setHoverReadingActive(true, animated);
            }
            return;
        }
        if (m_hoverReadingActive) {
            if (m_hoverTimer) {
                m_hoverTimer->stop();
            }
            return;
        }
        scheduleHoverReadingTimer();
        return;
    }

    if (m_hoverTimer) {
        m_hoverTimer->stop();
    }
    setHoverReadingActive(false, animated);
    hideTimelinePopup();
    hideMarketBreadthDetailPopup();
}

bool FloatingWindow::canShowTimelinePopup() const {
    if (!m_cfg.timelineChartEnabled || !m_timelinePopup) {
        return false;
    }
    if (!isVisible()) {
        return false;
    }

    if (m_cfg.mousePassthroughEnabled) {
        return m_hoverReadingActive;
    }

    return true;
}

void FloatingWindow::updateHoverPopupsForViewport(const QPoint& viewportPos, QTableView* sourceView) {
    if (!m_table || !m_model) {
        return;
    }

    updateTimelinePopupForHover(viewportPos, sourceView);
}

void FloatingWindow::updateTimelinePopupForHover(const QPoint& viewportPos, QTableView* sourceView) {
    if (!m_table || !m_model || !m_timelinePopup) {
        return;
    }

    if (!canShowTimelinePopup()) {
        hideTimelinePopup();
        return;
    }

    QTableView* view = sourceView ? sourceView : m_table;
    const QModelIndex index = view->indexAt(viewportPos);
    if (!index.isValid() || m_model->rowKind(index.row()) != QuoteModel::RowKindQuote) {
        hideTimelinePopup();
        return;
    }

    if (index.column() != ColCode && index.column() != ColName) {
        hideTimelinePopup();
        return;
    }

    const QString code = m_model->data(m_model->index(index.row(), ColCode), Qt::DisplayRole)
        .toString()
        .trimmed();
    const QString name = m_model->data(m_model->index(index.row(), ColName), Qt::DisplayRole)
        .toString()
        .trimmed();
    if (code.isEmpty() || !isTimelinePopupSupportedCode(code)) {
        hideTimelinePopup();
        return;
    }

    const QRect visual = view->visualRect(index);
    const QPoint globalTopLeft = view->viewport()->mapToGlobal(visual.topLeft());
    const QRect globalAnchor(globalTopLeft, visual.size());

    m_timelineHoverCode = code;
    m_timelineHoverName = name;
    const double cost = m_model ? m_model->costForCode(code) : qQNaN();
    m_timelinePopup->showForStock(code, name, globalAnchor, width(), cost, m_model->priceForCode(code));
}

void FloatingWindow::hideTimelinePopup() {
    m_timelineHoverCode.clear();
    m_timelineHoverName.clear();
    if (m_timelinePopup) {
        m_timelinePopup->hidePopup();
    }
}

void FloatingWindow::toggleMarketBreadthDetailPopupFromTray() {
    if (!m_marketBreadthDetailPopup || !m_model) {
        return;
    }

    if (m_marketBreadthDetailPopup->isVisible()) {
        m_marketBreadthDetailPopup->hidePopup();
        return;
    }

    m_marketBreadthDetailPopup->setLanguage(m_model->language());
    m_marketBreadthDetailPopup->showCenteredForSnapshot(
        m_model->marketBreadthSnapshot(),
        m_model->hotSectors(),
        m_model->hotConcepts(),
        frameGeometry()
    );
}

QRect FloatingWindow::marketBreadthDetailPopupGeometry() const {
    return m_marketBreadthDetailPopup
        ? m_marketBreadthDetailPopup->savedWindowRect()
        : QRect();
}

void FloatingWindow::setMarketBreadthDetailPopupGeometry(const QRect& rect) {
    if (m_marketBreadthDetailPopup) {
        m_marketBreadthDetailPopup->setSavedWindowRect(rect);
    }
}

void FloatingWindow::setMarketBreadthDetailWatchlistCallbacks(
    std::function<bool(const QString&)> containsCallback,
    std::function<bool(const QString&, const QString&, bool)> mutateCallback,
    std::function<void()> reloadCallback
) {
    if (!m_marketBreadthDetailPopup) {
        return;
    }
    m_marketBreadthDetailPopup->setHotRankDetailWatchlistCallbacks(
        std::move(containsCallback),
        std::move(mutateCallback),
        std::move(reloadCallback)
    );
}

void FloatingWindow::refreshMarketBreadthDetailPopup() {
    if (!m_marketBreadthDetailPopup || !m_marketBreadthDetailPopup->isVisible()) {
        return;
    }

    m_marketBreadthDetailPopup->setLanguage(m_model ? m_model->language() : QStringLiteral("en_US"));
    m_marketBreadthDetailPopup->refreshSnapshot(
        m_model ? m_model->marketBreadthSnapshot() : MarketBreadthSnapshot{},
        m_model ? m_model->hotSectors() : QVector<HotRankItem>{},
        m_model ? m_model->hotConcepts() : QVector<HotRankItem>{}
    );
}

void FloatingWindow::hideMarketBreadthDetailPopup() {
    // Independent window: only close if not pinned (opened from tray).
    if (m_marketBreadthDetailPopup && !m_marketBreadthDetailPopup->isPinnedFromTray()) {
        m_marketBreadthDetailPopup->hidePopup();
    }
}

void FloatingWindow::refreshMousePassthroughState(bool force) {
    const bool shouldPassthrough = m_cfg.mousePassthroughEnabled
        && !shouldCaptureMouseInteraction();
    if (!force && shouldPassthrough == m_mousePassthroughActive) {
        return;
    }

    const bool stateChanged = setMousePassthroughActive(shouldPassthrough);

    if (shouldPassthrough) {
        if (m_hoverTimer) {
            m_hoverTimer->stop();
        }
        setHoverReadingActive(false, true);
        hideTimelinePopup();
        hideMarketBreadthDetailPopup();
        return;
    }

    if (stateChanged) {
        QTimer::singleShot(0, this, [this]() {
            if (!m_mousePassthroughActive) {
                updateHoverReadingState(false);
            }
        });
        return;
    }

    updateHoverReadingState(false);
}

bool FloatingWindow::setMousePassthroughActive(bool active) {
    const bool stateChanged = m_mousePassthroughActive != active;
    m_mousePassthroughActive = active;

#if defined(Q_OS_MACOS)
    if (isVisible()) {
        setMacWindowIgnoresMouseEvents(this, active);
    }
    return stateChanged;
#else
    if (!stateChanged
        && windowFlags().testFlag(Qt::WindowTransparentForInput) == active) {
        return false;
    }

    const QRect oldGeometry = geometry();
    const bool wasVisible = isVisible();
    setWindowFlag(Qt::WindowTransparentForInput, active);
    if (wasVisible) {
        show();
        if (geometry() != oldGeometry) {
            setGeometry(oldGeometry);
        }
    }
    return true;
#endif
}

bool FloatingWindow::eventFilter(QObject* watched, QEvent* event) {
    // Consume all wheel/scroll events — the window is sized to fit all rows without scrolling.
    if (event->type() == QEvent::Wheel) {
        return true;
    }

    const bool isFromStockTable = m_stockTable
        && (watched == m_stockTable || watched == m_stockTable->viewport());

    switch (event->type()) {
    case QEvent::Enter:
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
        if (watched == m_panel
            || watched == m_table
            || watched == m_table->viewport()
            || watched == m_table->horizontalHeader()
            || isFromStockTable) {
            if (!shouldAllowMouseInteraction()) {
                if (m_hoverTimer) {
                    m_hoverTimer->stop();
                }
                setHoverReadingActive(false, false);
                return false;
            }
            updateHoverReadingState(false);

            if (watched == m_table->viewport() || (m_stockTable && watched == m_stockTable->viewport())) {
                QTableView* srcView = (m_stockTable && watched == m_stockTable->viewport())
                    ? m_stockTable : m_table;
                QPoint hoverPos;
                if (event->type() == QEvent::MouseMove) {
                    hoverPos = static_cast<QMouseEvent*>(event)->position().toPoint();
                } else if (event->type() == QEvent::HoverMove || event->type() == QEvent::HoverEnter) {
                    hoverPos = static_cast<QHoverEvent*>(event)->position().toPoint();
                } else {
                    hoverPos = srcView->viewport()->mapFromGlobal(QCursor::pos());
                }
                updateHoverPopupsForViewport(hoverPos, srcView);
            }
        }
        break;
    case QEvent::Leave:
    case QEvent::HoverLeave:
        if (watched == m_panel
            || watched == m_table
            || watched == m_table->viewport()
            || watched == m_table->horizontalHeader()
            || isFromStockTable) {
            updateHoverReadingState(true);
            hideTimelinePopup();
            hideMarketBreadthDetailPopup();
        }
        break;
    case QEvent::MouseButtonPress: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (isDragTriggerButton(mouseEvent->button()) && shouldAllowMouseInteraction()) {
            m_dragging = true;
            m_dragButton = mouseEvent->button();
            m_dragOffset = mouseEvent->globalPosition().toPoint() - frameGeometry().topLeft();
            grabMouse();
            hideTimelinePopup();
            hideMarketBreadthDetailPopup();
            refreshMousePassthroughState();
            return true;
        }
        break;
    }
    case QEvent::MouseMove: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (m_dragging && isCurrentDragButtonHeld(mouseEvent->buttons())) {
            move(mouseEvent->globalPosition().toPoint() - m_dragOffset);
            updateHoverReadingState(false);
            return true;
        }

        if (watched == m_panel
            || watched == m_table
            || watched == m_table->viewport()
            || watched == m_table->horizontalHeader()
            || isFromStockTable) {
            if (!shouldAllowMouseInteraction()) {
                if (m_hoverTimer) {
                    m_hoverTimer->stop();
                }
                setHoverReadingActive(false, false);
                return false;
            }
            updateHoverReadingState(false);

            if (watched == m_table->viewport() || (m_stockTable && watched == m_stockTable->viewport())) {
                QTableView* srcView = (m_stockTable && watched == m_stockTable->viewport())
                    ? m_stockTable : m_table;
                updateHoverPopupsForViewport(mouseEvent->position().toPoint(), srcView);
            }
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == m_dragButton) {
            m_dragging = false;
            m_dragButton = Qt::NoButton;
            releaseMouse();
            enforceWindowLevel(false);
            updateHoverReadingState(true);
            refreshMousePassthroughState();
            return true;
        }
        break;
    }
    case QEvent::MouseButtonDblClick: {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton && shouldAllowMouseInteraction()) {
            // Double-click on stock name column: open eastmoney detail page
            if (m_cfg.floatingWindowDoubleClickStockDetail && m_model) {
                QTableView* srcView = nullptr;
                if (watched == m_table->viewport()) {
                    srcView = m_table;
                } else if (m_stockTable && watched == m_stockTable->viewport()) {
                    srcView = m_stockTable;
                }
                if (srcView) {
                    const QModelIndex idx = srcView->indexAt(mouseEvent->position().toPoint());
                    if (idx.isValid()
                        && idx.column() == ColName
                        && m_model->rowKind(idx.row()) == QuoteModel::RowKindQuote) {
                        const QString rawCode = m_model->data(
                            m_model->index(idx.row(), ColCode), Qt::DisplayRole
                        ).toString().trimmed();
                        const QString url = buildEastmoneyDetailUrl(rawCode);
                        if (!url.isEmpty()) {
                            QDesktopServices::openUrl(QUrl(url));
                            hideTimelinePopup();
                            hideMarketBreadthDetailPopup();
                            hide();
                            return true;
                        }
                    }
                }
            }
            // Fall through to close-window behavior
            if (m_cfg.floatingWindowDoubleClickToHide
                && !m_cfg.mousePassthroughEnabled) {
                if (m_dragging) {
                    m_dragging = false;
                    m_dragButton = Qt::NoButton;
                    releaseMouse();
                }
                hide();
                return true;
            }
        }
        break;
    }
    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}

void FloatingWindow::applyConfig(const AppConfig& cfg) {
    m_cfg = cfg;
    if (auto* table = static_cast<BottomGridTableView*>(m_table)) {
        table->setHotRankFlipSecs(m_cfg.hotRankFlipSecs);
    }

    if (QLayout* panelLayout = m_panel ? m_panel->layout() : nullptr) {
        const int padding = floatingWindowPaddingPx(m_cfg);
        panelLayout->setContentsMargins(padding, padding, padding, padding);
    }
    // Keep group bar text left-aligned with cell text (same horizontal padding as table cells).
    if (m_groupBarLayout) {
        const int padding = floatingWindowPaddingPx(m_cfg);
        m_groupBarLayout->setContentsMargins(padding, 2, 0, 2);
    }

    const bool topFlagChanged =
        windowFlags().testFlag(Qt::WindowStaysOnTopHint) != m_cfg.floatingWindowAlwaysOnTop;
    const bool bottomFlagChanged =
        windowFlags().testFlag(Qt::WindowStaysOnBottomHint) == m_cfg.floatingWindowAlwaysOnTop;
    const QRect oldGeometry = geometry();
    const bool wasVisible = isVisible();
    if (topFlagChanged) {
        setWindowFlag(Qt::WindowStaysOnTopHint, m_cfg.floatingWindowAlwaysOnTop);
    }
    if (bottomFlagChanged) {
        setWindowFlag(Qt::WindowStaysOnBottomHint, !m_cfg.floatingWindowAlwaysOnTop);
    }
    if (wasVisible && (topFlagChanged || bottomFlagChanged)) {
        show();
        setGeometry(oldGeometry);
    }

    setWindowOpacity(configuredWindowOpacity(m_cfg));

    const QFont baseTableFont = m_table ? m_table->font() : font();
    const QFont tableFont = effectiveFloatingWindowFont(m_cfg, baseTableFont);
    m_panel->setFont(tableFont);
    m_table->setFont(tableFont);
    m_table->viewport()->setFont(tableFont);
    if (m_stockTable) {
        m_stockTable->setFont(tableFont);
        m_stockTable->viewport()->setFont(tableFont);
    }

    const QFont baseHeaderFont = m_table->horizontalHeader()
        ? m_table->horizontalHeader()->font()
        : tableFont;
    const QFont headerFont = effectiveFloatingWindowFont(m_cfg, baseHeaderFont);
    m_table->horizontalHeader()->setFont(headerFont);
    m_table->horizontalHeader()->viewport()->setFont(headerFont);

    // Reset hover reading state when config is reloaded
    if (m_hoverTimer) {
        m_hoverTimer->stop();
    }
    setHoverReadingActive(false, false);
    m_dragging = false;
    m_dragButton = Qt::NoButton;

    // Font or padding may have changed — reset max-width history so columns recompute from scratch.
    m_maxColumnWidths.clear();
    applyStyle();
    applyColumns();
    if (m_timelinePopup) {
        m_timelinePopup->applyConfig(m_cfg);
        if (!m_cfg.timelineChartEnabled) {
            hideTimelinePopup();
        }
    }
    if (m_marketBreadthDetailPopup) {
        m_marketBreadthDetailPopup->applyConfig(m_cfg);
        m_marketBreadthDetailPopup->setLanguage(m_model ? m_model->language() : QStringLiteral("en_US"));
        if (m_cfg.marketBreadthWindowRect.isValid()) {
            m_marketBreadthDetailPopup->setSavedWindowRect(m_cfg.marketBreadthWindowRect);
        }
        if (!m_cfg.marketBreadthEnabled) {
            m_marketBreadthDetailPopup->hidePopup();
            hideMarketBreadthDetailPopup();
        } else {
            refreshMarketBreadthDetailPopup();
        }
    }

    if (m_mousePassthroughTimer) {
        if (m_cfg.mousePassthroughEnabled) {
            m_mousePassthroughTimer->start();
        } else {
            m_mousePassthroughTimer->stop();
        }
    }
    refreshMousePassthroughState(true);

    if (isVisible()) {
        enforceWindowLevel(false);
    }
    updateHoverReadingState(false);
}

void FloatingWindow::mousePressEvent(QMouseEvent* event) {
    if (isDragTriggerButton(event->button()) && shouldAllowMouseInteraction()) {
        m_dragging = true;
        m_dragButton = event->button();
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        grabMouse();
        refreshMousePassthroughState();
    }
    QWidget::mousePressEvent(event);
}

void FloatingWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton
        && m_cfg.floatingWindowDoubleClickToHide
        && !m_cfg.mousePassthroughEnabled
        && shouldAllowMouseInteraction()) {
        if (m_dragging) {
            m_dragging = false;
            m_dragButton = Qt::NoButton;
            releaseMouse();
        }
        hideTimelinePopup();
        hideMarketBreadthDetailPopup();
        hide();
        event->accept();
        return;
    }

    QWidget::mouseDoubleClickEvent(event);
}

void FloatingWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && isCurrentDragButtonHeld(event->buttons())) {
        move(event->globalPosition().toPoint() - m_dragOffset);
        updateHoverReadingState(false);
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void FloatingWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == m_dragButton) {
        m_dragging = false;
        m_dragButton = Qt::NoButton;
        releaseMouse();
        enforceWindowLevel(false);
        updateHoverReadingState(true);
        refreshMousePassthroughState();
    }
    QWidget::mouseReleaseEvent(event);
}

void FloatingWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);

    enforceWindowLevel(true);
    refreshMousePassthroughState(true);
    updateHoverReadingState(false);
}

void FloatingWindow::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    hideTimelinePopup();
    hideMarketBreadthDetailPopup();
}

void FloatingWindow::enterEvent(QEnterEvent* event) {
    QWidget::enterEvent(event);
    refreshMousePassthroughState();
    updateHoverReadingState(false);
}

void FloatingWindow::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    refreshMousePassthroughState();
    updateHoverReadingState(true);
    hideTimelinePopup();
    hideMarketBreadthDetailPopup();
}

void FloatingWindow::enforceWindowLevel(bool activate) {
    if (!isVisible()) {
        return;
    }

    const bool alwaysOnTop = m_cfg.floatingWindowAlwaysOnTop;

#ifdef WIN32
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd) {
        if (alwaysOnTop) {
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING
            );
        } else {
            SetWindowPos(
                hwnd,
                HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING
            );
            SetWindowPos(
                hwnd,
                HWND_BOTTOM,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING
            );
        }
    }
#endif

    if (alwaysOnTop) {
        raise();
        if (activate) {
            activateWindow();
        }
    } else {
        lower();
    }
}

void FloatingWindow::applyStyle() {
    auto* table = static_cast<BottomGridTableView*>(m_table);
    const QColor b = m_cfg.transparentBackgroundEnabled
        ? QColor(0, 0, 0, 0)
        : m_cfg.bgColor;
    const QColor t = m_cfg.textColor;
    const QColor g = m_cfg.gridColor;
    const QColor transparentBorder(0, 0, 0, 0);

    const QString css = QString(
        "QFrame#panel{"
        "background-color: rgba(%1,%2,%3,%4);"
        "border-radius: 8px;"
        "border: 1px solid rgba(%5,%6,%7,%8);"
        "}"
        "QTableView{"
        "background-color: rgba(%1,%2,%3,%4);"
        "border: none;"
        "border-radius: 8px;"
        "color: rgb(%9,%10,%11);"
        "gridline-color: rgba(%12,%13,%14,%15);"
        "}"
        "QWidget#tableViewport{"
        "background-color: rgba(%1,%2,%3,%4);"
        "}"
        "QWidget#tableHeaderViewport{"
        "background-color: rgba(%1,%2,%3,%4);"
        "}"
        "QHeaderView::section{"
        "background-color: rgba(%1,%2,%3,%4);"
        "border: none;"
        "%16"
        "color: rgb(%9,%10,%11);"
        "}"
        "QAbstractItemView::item{"
        "%16"
        "}"
    )
        .arg(b.red())
        .arg(b.green())
        .arg(b.blue())
        .arg(b.alpha())
        .arg(transparentBorder.red())
        .arg(transparentBorder.green())
        .arg(transparentBorder.blue())
        .arg(transparentBorder.alpha())
        .arg(t.red())
        .arg(t.green())
        .arg(t.blue())
        .arg(g.red())
        .arg(g.green())
        .arg(g.blue())
        .arg(g.alpha())
        .arg(tableCellPaddingStyle(m_cfg));

    m_panel->setStyleSheet(css);
    table->setBottomGridVisible(m_cfg.showGrid);
    table->setBottomGridColor(g);
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setVisible(m_cfg.showHeader);
    if (auto* st = static_cast<BottomGridTableView*>(m_stockTable)) {
        st->setBottomGridVisible(m_cfg.showGrid);
        st->setBottomGridColor(g);
        m_stockTable->setShowGrid(false);
    }

#ifdef WIN32
    // Sync palette so Fusion style picks up the correct text / base colors.
    QPalette pal = m_table->palette();
    pal.setColor(QPalette::Base, b);
    pal.setColor(QPalette::Text, t);
    pal.setColor(QPalette::WindowText, t);
    m_table->setPalette(pal);
    m_table->viewport()->setPalette(pal);
    m_table->horizontalHeader()->setPalette(pal);
    if (m_stockTable) {
        m_stockTable->setPalette(pal);
        m_stockTable->viewport()->setPalette(pal);
    }
#endif

    applyGroupBarStyle();
}

void FloatingWindow::applyHoverReadingStyle() {
    auto* table = static_cast<BottomGridTableView*>(m_table);
    const bool transparentBg = m_cfg.hoverReadingEnabled
        && m_cfg.hoverReadingTransparentBackgroundEnabled;
    const HoverReadingTheme theme = hoverReadingThemeForMode(
        m_cfg.hoverReadingUiMode,
        transparentBg
    );
    const bool lightMode = normalizeHoverReadingUiMode(m_cfg.hoverReadingUiMode)
        == QLatin1String("light");
    const QColor tableBackground = hoverReadingTableBackgroundColor(
        theme,
        m_cfg.hoverReadingUiMode
    );
    const QColor lightGridColor(QStringLiteral("#d4d4d4"));
    const QColor hoverGridColor = lightMode ? lightGridColor : theme.border;
    const QColor grid = m_cfg.showGrid ? hoverGridColor : QColor(0, 0, 0, 0);
    const QColor transparentBorder(0, 0, 0, 0);
    const QColor tableChromeBackground = transparentBg
        ? QColor(0, 0, 0, 0)
        : tableBackground;

    const QString css = QString(
        "QFrame#panel{"
        "background-color: rgba(%1,%2,%3,%4);"
        "border-radius: 8px;"
        "border: 1px solid rgba(%5,%6,%7,%8);"
        "}"
        "QTableView{"
        "background-color: rgba(%16,%17,%18,%19);"
        "border: 1px solid rgba(%16,%17,%18,%19);"
        "border-radius: 8px;"
        "color: rgb(%9,%10,%11);"
        "gridline-color: rgba(%12,%13,%14,%15);"
        "}"
        "QWidget#tableViewport{"
        "background-color: rgba(%1,%2,%3,%4);"
        "}"
        "QWidget#tableHeaderViewport{"
        "background-color: rgba(%20,%21,%22,%23);"
        "}"
        "QHeaderView::section{"
        "background-color: rgba(%20,%21,%22,%23);"
        "border: none;"
        "%24"
        "color: rgb(%9,%10,%11);"
        "}"
        "QAbstractItemView::item{"
        "%24"
        "}"
    )
        .arg(tableBackground.red())
        .arg(tableBackground.green())
        .arg(tableBackground.blue())
        .arg(tableBackground.alpha())
        .arg(transparentBorder.red())
        .arg(transparentBorder.green())
        .arg(transparentBorder.blue())
        .arg(transparentBorder.alpha())
        .arg(theme.textPrimary.red())
        .arg(theme.textPrimary.green())
        .arg(theme.textPrimary.blue())
        .arg(grid.red())
        .arg(grid.green())
        .arg(grid.blue())
        .arg(grid.alpha())
        .arg(tableChromeBackground.red())
        .arg(tableChromeBackground.green())
        .arg(tableChromeBackground.blue())
        .arg(tableChromeBackground.alpha())
        .arg(theme.surface.red())
        .arg(theme.surface.green())
        .arg(theme.surface.blue())
        .arg(theme.surface.alpha())
        .arg(tableCellPaddingStyle(m_cfg));

    m_panel->setStyleSheet(css);
    table->setBottomGridVisible(m_cfg.showGrid);
    table->setBottomGridColor(grid);
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setVisible(m_cfg.showHeader);
    if (auto* st = static_cast<BottomGridTableView*>(m_stockTable)) {
        st->setBottomGridVisible(m_cfg.showGrid);
        st->setBottomGridColor(grid);
        m_stockTable->setShowGrid(false);
    }

#ifdef WIN32
    QPalette pal = m_table->palette();
    pal.setColor(QPalette::Base, tableBackground);
    pal.setColor(QPalette::Text, theme.textPrimary);
    pal.setColor(QPalette::WindowText, theme.textPrimary);
    m_table->setPalette(pal);
    m_table->viewport()->setPalette(pal);
    m_table->horizontalHeader()->setPalette(pal);
    if (m_stockTable) {
        m_stockTable->setPalette(pal);
        m_stockTable->viewport()->setPalette(pal);
    }
#endif
}

void FloatingWindow::setHoverReadingActive(bool active, bool animated) {
    if (m_hoverReadingActive == active
        && (!m_styleAnimation
            || m_styleAnimation->state() != QAbstractAnimation::Running)) {
        return;
    }

    m_hoverReadingActive = active;
    if (m_model) {
        m_model->setHoverReadingVisualState(m_hoverReadingActive);
    }
    const qreal target = m_hoverReadingActive ? 1.0 : 0.0;

    if (!animated || !m_styleAnimation) {
        if (m_styleAnimation) {
            m_styleAnimation->stop();
        }
        m_hoverReadingProgress = target;
        setWindowOpacity(m_hoverReadingActive ? 1.0 : configuredWindowOpacity(m_cfg));
        if (m_hoverReadingActive) {
            applyHoverReadingStyle();
        } else {
            applyStyle();
        }
        return;
    }

    const qreal start = m_hoverReadingProgress;
    if (qAbs(start - target) < 0.0001) {
        applyInterpolatedStyle(target);
        return;
    }

    m_styleAnimation->stop();
    m_styleAnimation->setStartValue(start);
    m_styleAnimation->setEndValue(target);
    m_styleAnimation->start();
}

void FloatingWindow::applyInterpolatedStyle(qreal hoverProgress) {
    auto* table = static_cast<BottomGridTableView*>(m_table);
    const qreal progress = qBound(0.0, hoverProgress, 1.0);
    const qreal normalOpacity = configuredWindowOpacity(m_cfg);
    setWindowOpacity(normalOpacity + (1.0 - normalOpacity) * progress);

    const QColor normalBg = m_cfg.transparentBackgroundEnabled
        ? QColor(0, 0, 0, 0)
        : m_cfg.bgColor;
    const QColor normalText = m_cfg.textColor;
    const QColor normalGrid = m_cfg.gridColor;
    const QColor normalBorder(0, 0, 0, 0);
    const QColor normalHeaderBg = normalBg;
    const QColor normalTableBg = normalBg;
    const QColor normalTableBorder(0, 0, 0, 0);

    const bool transparentBg = m_cfg.hoverReadingEnabled
        && m_cfg.hoverReadingTransparentBackgroundEnabled;
    const HoverReadingTheme theme = hoverReadingThemeForMode(
        m_cfg.hoverReadingUiMode,
        transparentBg
    );
    const bool lightMode = normalizeHoverReadingUiMode(m_cfg.hoverReadingUiMode)
        == QLatin1String("light");
    const QColor hoverTableBackground = hoverReadingTableBackgroundColor(
        theme,
        m_cfg.hoverReadingUiMode
    );
    const QColor lightGridColor(QStringLiteral("#d4d4d4"));
    const QColor hoverGridColor = lightMode ? lightGridColor : theme.border;
    const QColor hoverGrid = m_cfg.showGrid ? hoverGridColor : QColor(0, 0, 0, 0);
    const QColor hoverTableChromeBackground = transparentBg
        ? QColor(0, 0, 0, 0)
        : hoverTableBackground;

    const QColor bg = mixColor(normalBg, hoverTableBackground, progress);
    const QColor border = mixColor(normalBorder, QColor(0, 0, 0, 0), progress);
    const QColor text = mixColor(normalText, theme.textPrimary, progress);
    const QColor grid = mixColor(normalGrid, hoverGrid, progress);
    const QColor headerBg = mixColor(normalHeaderBg, theme.surface, progress);
    const QColor tableViewportBg = mixColor(normalTableBg, hoverTableBackground, progress);
    const QColor tableChromeBg = mixColor(
        normalTableBg,
        hoverTableChromeBackground,
        progress
    );
    const QColor tableBorder = mixColor(normalTableBorder, QColor(0, 0, 0, 0), progress);

    const QString css = QString(
        "QFrame#panel{"
        "background-color: rgba(%1,%2,%3,%4);"
        "border-radius: 8px;"
        "border: 1px solid rgba(%5,%6,%7,%8);"
        "}"
        "QTableView{"
        "background-color: rgba(%16,%17,%18,%19);"
        "border: 1px solid rgba(%24,%25,%26,%27);"
        "border-radius: 8px;"
        "color: rgb(%9,%10,%11);"
        "gridline-color: rgba(%12,%13,%14,%15);"
        "}"
        "QWidget#tableViewport{"
        "background-color: rgba(%28,%29,%30,%31);"
        "}"
        "QWidget#tableHeaderViewport{"
        "background-color: rgba(%20,%21,%22,%23);"
        "}"
        "QHeaderView::section{"
        "background-color: rgba(%20,%21,%22,%23);"
        "border: none;"
        "%32"
        "color: rgb(%9,%10,%11);"
        "}"
        "QAbstractItemView::item{"
        "%32"
        "}"
    )
        .arg(bg.red())
        .arg(bg.green())
        .arg(bg.blue())
        .arg(bg.alpha())
        .arg(border.red())
        .arg(border.green())
        .arg(border.blue())
        .arg(border.alpha())
        .arg(text.red())
        .arg(text.green())
        .arg(text.blue())
        .arg(grid.red())
        .arg(grid.green())
        .arg(grid.blue())
        .arg(grid.alpha())
        .arg(tableChromeBg.red())
        .arg(tableChromeBg.green())
        .arg(tableChromeBg.blue())
        .arg(tableChromeBg.alpha())
        .arg(headerBg.red())
        .arg(headerBg.green())
        .arg(headerBg.blue())
        .arg(headerBg.alpha())
        .arg(tableBorder.red())
        .arg(tableBorder.green())
        .arg(tableBorder.blue())
        .arg(tableBorder.alpha())
        .arg(tableViewportBg.red())
        .arg(tableViewportBg.green())
        .arg(tableViewportBg.blue())
        .arg(tableViewportBg.alpha())
        .arg(tableCellPaddingStyle(m_cfg));

    m_panel->setStyleSheet(css);
    table->setBottomGridVisible(m_cfg.showGrid);
    table->setBottomGridColor(grid);
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setVisible(m_cfg.showHeader);
    if (auto* st = static_cast<BottomGridTableView*>(m_stockTable)) {
        st->setBottomGridVisible(m_cfg.showGrid);
        st->setBottomGridColor(grid);
        m_stockTable->setShowGrid(false);
    }

#ifdef WIN32
    QPalette pal = m_table->palette();
    pal.setColor(QPalette::Base, tableViewportBg);
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::WindowText, text);
    m_table->setPalette(pal);
    m_table->viewport()->setPalette(pal);
    m_table->horizontalHeader()->setPalette(pal);
    if (m_stockTable) {
        m_stockTable->setPalette(pal);
        m_stockTable->viewport()->setPalette(pal);
    }
#endif
}

void FloatingWindow::applyColumns() {
    const QVector<int> columnOrder = watchlist_utils::normalizedColumnOrder(m_cfg.columnOrder);
    QHeaderView* header = m_table->horizontalHeader();

    // Helper to apply column ordering to a header
    const auto applyOrder = [&](QHeaderView* hdr) {
        QSignalBlocker blocker(hdr);
        hdr->setSectionsMovable(true);
        for (int visualIndex = 0; visualIndex < columnOrder.size(); ++visualIndex) {
            const int logical = columnOrder[visualIndex];
            const int from = hdr->visualIndex(logical);
            if (from >= 0 && from != visualIndex) {
                hdr->moveSection(from, visualIndex);
            }
        }
        hdr->setSectionsMovable(false);
    };

    // Apply column visual order to both tables.
    applyOrder(header);
    if (m_stockTable) {
        applyOrder(m_stockTable->horizontalHeader());
    }

    // Set visibility for each column.
    for (int i = 0; i < ColCount; ++i) {
        const bool visible = m_cfg.visibleColumns.value(i, true);
        m_table->setColumnHidden(i, !visible);
        if (m_stockTable) {
            m_stockTable->setColumnHidden(i, !visible);
        }
    }

    adjustWindowSize();
}

void FloatingWindow::adjustWindowSize() {
    if (auto* table = static_cast<BottomGridTableView*>(m_table)) {
        table->syncSpecialRowSpans(m_model);
    }
    if (auto* table = static_cast<BottomGridTableView*>(m_stockTable)) {
        table->syncSpecialRowSpans(m_model);
    }

    // Re-apply row visibility after any model reset (index rows → m_table, stock rows → m_stockTable)
    applyRowVisibility();

    // Column widths: always compute fresh from content, then take the running maximum per column.
    // This ensures the window never shrinks when switching groups — it only grows or stays the same.
    // The max history is reset by unlockWidth() on reload or position reset.
    int totalWidth = 0;
    for (int i = 0; i < ColCount; ++i) {
        if (m_table->isColumnHidden(i)) {
            m_maxColumnWidths.remove(i);
            continue;
        }
        const int freshW = autoColumnWidthFromContent(i);
        const int w = qMax(freshW, m_maxColumnWidths.value(i, 0));
        m_maxColumnWidths[i] = w;
        m_table->setColumnWidth(i, w);
        if (m_stockTable) {
            m_stockTable->setColumnWidth(i, w);
        }
        totalWidth += w;
    }

    const int rowHeight = m_table->verticalHeader()->defaultSectionSize();
    const int headerHeight = m_table->horizontalHeader()->isVisible()
        ? m_table->horizontalHeader()->sizeHint().height()
        : 0;

    const int totalModelRows = m_model ? m_model->rowCount() : 0;
    // Count special rows that precede quotes (e.g. market breadth) separately so that
    // m_indexCount is applied only to quote rows, keeping all pre-quote special rows in
    // the top table.
    int specialBeforeQuotes = 0;
    int quoteRowCount = 0;
    bool anyQuoteSeen = false;
    for (int r = 0; r < totalModelRows; ++r) {
        if (m_model->rowKind(r) == QuoteModel::RowKindQuote) {
            anyQuoteSeen = true;
            ++quoteRowCount;
        } else if (!anyQuoteSeen) {
            ++specialBeforeQuotes;
        }
    }
    const int indexQuoteRows = qMin(m_indexCount, quoteRowCount);
    const int indexRows = specialBeforeQuotes + indexQuoteRows;
    const int stockRows = qMax(0, totalModelRows - indexRows);

    const int indexTableHeight = headerHeight + indexRows * rowHeight;
    const int stockTableHeight = stockRows * rowHeight;
    const int groupBarH = (m_groupBar && m_groupBar->isVisible()) ? m_groupBar->height() : 0;

    // Fix heights so layout distributes correctly
    m_table->setFixedHeight(qMax(indexTableHeight, headerHeight)); // always at least header
    if (m_stockTable) {
        m_stockTable->setFixedHeight(qMax(stockTableHeight, 0));
    }

    const int padding = floatingWindowPaddingPx(m_cfg);
    const int safeWidth = qMax(totalWidth + padding * 2, 1);
    const int safeHeight = qMax(
        qMax(indexTableHeight, headerHeight) + groupBarH + stockTableHeight + padding * 2,
        1
    );
    setFixedSize(safeWidth, safeHeight);
}

int FloatingWindow::autoColumnWidthFromContent(int column) const {
    const int minWidth = 16;
    int width = minWidth;

    const QString headerText =
        m_model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
    const QFontMetrics headerFm(m_table->horizontalHeader()->font());
    width = qMax(width, headerFm.horizontalAdvance(headerText) + 24);

    const QFontMetrics cellFm(m_table->font());
    const int rows = m_model->rowCount();
    for (int r = 0; r < rows; ++r) {
        if (m_model->rowKind(r) != QuoteModel::RowKindQuote
            && column != m_model->firstVisibleLogicalColumn()) {
            continue;
        }
        const QString text = m_model->data(m_model->index(r, column), Qt::DisplayRole).toString();
        width = qMax(width, cellFm.horizontalAdvance(text) + 24);
    }

    const int maxW = m_cfg.columnMaxWidths.value(column, 0);
    if (maxW > 0) {
        width = qMin(width, qMax(maxW, minWidth));
    }

    return width;
}
