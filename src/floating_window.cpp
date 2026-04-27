#include "floating_window.h"

#include <QDateTime>
#include <QCursor>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHeaderView>
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
#include <QPainterPath>
#include <QPalette>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QScreen>
#include <QSet>
#include <QStyledItemDelegate>
#include <QStyleFactory>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

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

QVector<int> normalizedColumnOrder(const QVector<int>& order) {
    QVector<int> out;
    out.reserve(ColCount);

    for (int logical : order) {
        if (logical < 0 || logical >= ColCount || out.contains(logical)) {
            continue;
        }
        out.push_back(logical);
    }

    for (int i = 0; i < ColCount; ++i) {
        if (out.contains(i)) {
            continue;
        }
        out.push_back(i);
    }

    return out;
}

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

inline constexpr double kTimelinePopupWidthScale = 1.5;
inline constexpr double kTimelinePopupHeightScale = 1.5;
inline constexpr int kTimelinePopupMinWidth = 560;
inline constexpr int kTimelinePopupMinHeight = 360;
inline constexpr int kTimelinePopupScreenMarginPx = 12;

struct TimelinePoint {
    QDateTime time;
    double price = qQNaN();
    double avgPrice = qQNaN();
    double volume = qQNaN();
    double amount = qQNaN();
};

bool isDigitsOnly(const QString& value) {
    if (value.isEmpty()) {
        return false;
    }
    for (QChar ch : value) {
        if (!ch.isDigit()) {
            return false;
        }
    }
    return true;
}

const QSet<QString>& predefinedAshareIndexAliases() {
    static const QSet<QString> aliases {
        QStringLiteral("sh000001"),
        QStringLiteral("sz399001"),
        QStringLiteral("sh000300"),
        QStringLiteral("sz399300"),
        QStringLiteral("sh000016"),
        QStringLiteral("sh000905"),
        QStringLiteral("sh000852"),
        QStringLiteral("sz399006"),
        QStringLiteral("sz399673"),
        QStringLiteral("sh000688"),
        QStringLiteral("sh931643"),
        QStringLiteral("sz931643"),
        QStringLiteral("sh932133"),
        QStringLiteral("sz399431"),
        QStringLiteral("sz399975"),
        QStringLiteral("sh000808"),
        QStringLiteral("sh000932"),
        QStringLiteral("sz399808"),
        QStringLiteral("sh980017"),
        QStringLiteral("sz980017"),
        QStringLiteral("399001"),
        QStringLiteral("000300"),
        QStringLiteral("399300"),
        QStringLiteral("000016"),
        QStringLiteral("000905"),
        QStringLiteral("000852"),
        QStringLiteral("399006"),
        QStringLiteral("399673"),
        QStringLiteral("000688"),
        QStringLiteral("931643"),
        QStringLiteral("932133"),
        QStringLiteral("399431"),
        QStringLiteral("399975"),
        QStringLiteral("000808"),
        QStringLiteral("000932"),
        QStringLiteral("399808"),
        QStringLiteral("980017"),
    };
    return aliases;
}

bool isAshareIndexCode(const QString& rawCode) {
    const QString code = rawCode.trimmed().toLower();
    if (code.isEmpty()) {
        return false;
    }
    if (code.startsWith(QStringLiteral("bk")) || code.startsWith(QStringLiteral("90."))) {
        return false;
    }
    return predefinedAshareIndexAliases().contains(code);
}

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
                && isDigitsOnly(symbol)) {
                return symbol;
            }
            return {};
        }
    }
    return (code.size() == 6 && isDigitsOnly(code)) ? code : QString();
}

bool isAshareStockCode(const QString& rawCode) {
    const QString code = rawCode.trimmed().toLower();
    if (code.isEmpty() || isAshareIndexCode(code)) {
        return false;
    }

    if (code.startsWith(QStringLiteral("sh"))
        || code.startsWith(QStringLiteral("sz"))
        || code.startsWith(QStringLiteral("bj"))) {
        const QString symbol = code.mid(2);
        return symbol.size() == 6 && isDigitsOnly(symbol);
    }

    if (code.startsWith(QStringLiteral("0.")) || code.startsWith(QStringLiteral("1."))) {
        const QString symbol = code.mid(2);
        return symbol.size() == 6 && isDigitsOnly(symbol) && !isAshareIndexCode(symbol);
    }

    return code.size() == 6 && isDigitsOnly(code) && !isAshareIndexCode(code);
}

bool isAshareTimelineSupportedCode(const QString& rawCode) {
    return isAshareStockCode(rawCode) || isAshareIndexCode(rawCode);
}

double fixedRangeLimitPctForAshareStock(const QString& rawCode) {
    const QString code = rawCode.trimmed().toLower();
    const QString symbol = extractSixDigitsSymbol(code);
    if (symbol.isEmpty()) {
        return qQNaN();
    }

    if (code.startsWith(QStringLiteral("bj"))
        || symbol.startsWith(QStringLiteral("920"))
        || symbol.startsWith(QLatin1Char('8'))
        || symbol.startsWith(QLatin1Char('4'))) {
        return 30.0;
    }
    if (symbol.startsWith(QStringLiteral("300"))
        || symbol.startsWith(QStringLiteral("301"))
        || symbol.startsWith(QStringLiteral("688"))
        || symbol.startsWith(QStringLiteral("689"))) {
        return 20.0;
    }
    return 10.0;
}

QString toTimelineSecId(const QString& rawCode) {
    const QString code = rawCode.trimmed().toLower();
    if (code.isEmpty() || !isAshareTimelineSupportedCode(code)) {
        return {};
    }

    const int dot = code.indexOf(QLatin1Char('.'));
    if (dot > 0 && dot < code.size() - 1) {
        bool marketOk = false;
        const int market = code.left(dot).toInt(&marketOk);
        const QString symbol = code.mid(dot + 1);
        bool symbolOk = !symbol.isEmpty();
        for (QChar ch : symbol) {
            if (!ch.isLetterOrNumber()) {
                symbolOk = false;
                break;
            }
        }
        if (marketOk && symbolOk) {
            if ((market == 0 || market == 1) && symbol.size() == 6 && isDigitsOnly(symbol)) {
                return QString::number(market) + QStringLiteral(".") + symbol.toUpper();
            }
            return {};
        }
    }

    QString digits;
    digits.reserve(6);
    for (QChar ch : code) {
        if (ch.isDigit()) {
            digits.append(ch);
        }
    }

    if (digits.size() > 6) {
        digits = digits.right(6);
    }
    if (digits.size() != 6) {
        return {};
    }

    if (code.startsWith(QStringLiteral("sh"))) {
        return QStringLiteral("1.") + digits;
    }
    if (code.startsWith(QStringLiteral("sz"))) {
        return QStringLiteral("0.") + digits;
    }

    QString market;
    if (isAshareIndexCode(code)) {
        market = digits.startsWith(QStringLiteral("399")) ? QStringLiteral("0") : QStringLiteral("1");
    } else {
        const QChar head = digits[0];
        market = (head == QLatin1Char('6') || head == QLatin1Char('5') || head == QLatin1Char('9'))
            ? QStringLiteral("1")
            : QStringLiteral("0");
    }
    return market + QStringLiteral(".") + digits;
}

bool isAshareTradingTimeNow() {
    const QTimeZone bjZone("Asia/Shanghai");
    if (!bjZone.isValid()) {
        return false;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc().toTimeZone(bjZone);
    const int dayOfWeek = now.date().dayOfWeek();
    if (dayOfWeek < 1 || dayOfWeek > 5) {
        return false;
    }

    const QTime time = now.time();
    const bool morning = time >= QTime(9, 30) && time <= QTime(11, 30);
    const bool afternoon = time >= QTime(13, 0) && time <= QTime(15, 0);
    return morning || afternoon;
}

QString stripJsonp(const QByteArray& body) {
    const QString text = QString::fromUtf8(body).trimmed();
    const int open = text.indexOf(QLatin1Char('('));
    const int close = text.lastIndexOf(QLatin1Char(')'));
    if (open <= 0 || close <= open) {
        return {};
    }
    const QString callback = text.left(open).trimmed();
    if (!callback.startsWith(QStringLiteral("jQuery"))) {
        return {};
    }
    return text.mid(open + 1, close - open - 1).trimmed();
}

bool parseTimelinePayload(
    const QByteArray& body,
    QVector<TimelinePoint>* outPoints,
    double* outPreClose,
    bool keepLastTradingDayOnly
) {
    if (!outPoints || !outPreClose) {
        return false;
    }

    *outPreClose = qQNaN();
    outPoints->clear();

    const QString jsonText = stripJsonp(body);
    if (jsonText.isEmpty()) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    const QJsonObject root = doc.object();
    if (!root.value(QStringLiteral("data")).isObject()) {
        return false;
    }
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    *outPreClose = data.value(QStringLiteral("preClose")).toDouble(qQNaN());

    const QJsonArray trends = data.value(QStringLiteral("trends")).toArray();
    if (trends.isEmpty()) {
        return true;
    }

    QString targetDate;
    if (keepLastTradingDayOnly) {
        for (int i = trends.size() - 1; i >= 0; --i) {
            const QString row = trends.at(i).toString().trimmed();
            if (row.size() >= 10) {
                targetDate = row.left(10);
                break;
            }
        }
    }

    outPoints->reserve(trends.size());
    for (const QJsonValue& value : trends) {
        const QString row = value.toString();
        if (row.isEmpty()) {
            continue;
        }

        const QStringList parts = row.split(QLatin1Char(','));
        if (parts.size() < 7) {
            continue;
        }

        const QString timeText = parts.at(0).trimmed();
        if (keepLastTradingDayOnly && !targetDate.isEmpty() && !timeText.startsWith(targetDate)) {
            continue;
        }

        const QDateTime time = QDateTime::fromString(timeText, QStringLiteral("yyyy-MM-dd HH:mm"));
        bool priceOk = false;
        bool avgPriceOk = false;
        bool volumeOk = false;
        bool amountOk = false;
        const double price = parts.at(2).toDouble(&priceOk);
        double avgPrice = qQNaN();
        if (parts.size() >= 8) {
            avgPrice = parts.at(7).toDouble(&avgPriceOk);
        }
        if (!avgPriceOk && parts.size() >= 4) {
            avgPrice = parts.at(3).toDouble(&avgPriceOk);
        }
        const double volume = parts.at(5).toDouble(&volumeOk);
        const double amount = parts.at(6).toDouble(&amountOk);
        if (!time.isValid() || !priceOk) {
            continue;
        }

        TimelinePoint point;
        point.time = time;
        point.price = price;
        point.avgPrice = avgPriceOk ? avgPrice : qQNaN();
        point.volume = volumeOk ? volume : qQNaN();
        point.amount = amountOk ? amount : qQNaN();
        outPoints->push_back(point);
    }

    return true;
}

} // namespace

class TimelineChartWidget : public QWidget {
public:
    explicit TimelineChartWidget(QWidget* parent = nullptr)
        : QWidget(parent) {
        setMinimumSize(280, 180);
    }

    void setConfig(const AppConfig& cfg) {
        m_cfg = cfg;
        update();
    }

    void setSeries(const QString& title, const QString& code, const QVector<TimelinePoint>& points, double preClose) {
        m_title = title;
        m_code = code;
        m_points = points;
        m_preClose = preClose;
        m_status.clear();
        update();
    }

    void setStatusText(const QString& status) {
        m_status = status;
        if (!status.isEmpty()) {
            m_points.clear();
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), m_cfg.timelineChartBgColor);

        const QRect r = rect().adjusted(12, 12, -12, -12);
        if (r.width() < 80 || r.height() < 80) {
            return;
        }

        const int headerHeight = 22;
        const int leftMargin = 56;
        const int rightMargin = 56;
        const int bottomMargin = 28;
        QRect headerRect(r.left(), r.top(), r.width(), headerHeight);
        QRect plot = QRect(
            r.left() + leftMargin,
            r.top() + headerHeight + 6,
            r.width() - leftMargin - rightMargin,
            r.height() - headerHeight - bottomMargin - 6
        );
        if (plot.width() < 20 || plot.height() < 20) {
            return;
        }

        painter.setPen(QPen(m_cfg.timelineChartTextColor));
        if (!m_title.isEmpty()) {
            painter.drawText(headerRect, Qt::AlignLeft | Qt::AlignVCenter, m_title);
        }

        if (!m_status.isEmpty()) {
            painter.drawText(plot, Qt::AlignCenter, m_status);
            return;
        }

        if (m_points.isEmpty()) {
            painter.drawText(plot, Qt::AlignCenter, QStringLiteral("No timeline data"));
            return;
        }

        const auto toPct = [](double value, double base) -> double {
            if (!std::isfinite(value) || !std::isfinite(base) || qFuzzyIsNull(base)) {
                return qQNaN();
            }
            return (value - base) / base * 100.0;
        };

        const auto signedNumber = [](double value, int prec) -> QString {
            if (!std::isfinite(value)) {
                return QStringLiteral("--");
            }
            const QString num = QString::number(std::abs(value), 'f', prec);
            if (value > 0.0) {
                return QStringLiteral("+%1").arg(num);
            }
            if (value < 0.0) {
                return QStringLiteral("-%1").arg(num);
            }
            return QStringLiteral("0.%1").arg(QStringLiteral("0").repeated(prec));
        };

        double baseline = m_preClose;
        if (!std::isfinite(baseline) || qFuzzyIsNull(baseline)) {
            for (const TimelinePoint& point : m_points) {
                if (std::isfinite(point.price) && !qFuzzyIsNull(point.price)) {
                    baseline = point.price;
                    break;
                }
            }
        }
        if (!std::isfinite(baseline) || qFuzzyIsNull(baseline)) {
            painter.drawText(plot, Qt::AlignCenter, QStringLiteral("No timeline data"));
            return;
        }

        double minPct = std::numeric_limits<double>::max();
        double maxPct = std::numeric_limits<double>::lowest();
        for (const TimelinePoint& point : m_points) {
            const double pricePct = toPct(point.price, baseline);
            if (std::isfinite(pricePct)) {
                minPct = qMin(minPct, pricePct);
                maxPct = qMax(maxPct, pricePct);
            }
            const double avgPct = toPct(point.avgPrice, baseline);
            if (std::isfinite(avgPct)) {
                minPct = qMin(minPct, avgPct);
                maxPct = qMax(maxPct, avgPct);
            }
        }
        minPct = qMin(minPct, 0.0);
        maxPct = qMax(maxPct, 0.0);

        if (!std::isfinite(minPct) || !std::isfinite(maxPct)) {
            painter.drawText(plot, Qt::AlignCenter, QStringLiteral("No timeline data"));
            return;
        }

        const bool isAshareStock = isAshareStockCode(m_code);
        double yMinPct = 0.0;
        double yMaxPct = 0.0;
        if (m_cfg.timelineChartFixedRangeEnabled && isAshareStock) {
            const double limitPct = fixedRangeLimitPctForAshareStock(m_code);
            if (std::isfinite(limitPct) && limitPct > 0.0) {
                yMinPct = -limitPct;
                yMaxPct = limitPct;
            }
        }
        if (!(yMinPct < yMaxPct)) {
            if (qFuzzyCompare(minPct, maxPct)) {
                minPct -= 0.5;
                maxPct += 0.5;
            }
            const double pctRange = qMax(0.000001, maxPct - minPct);
            yMinPct = minPct - pctRange * 0.08;
            yMaxPct = maxPct + pctRange * 0.08;
        }
        const double ySpanPct = qMax(0.000001, yMaxPct - yMinPct);

        const auto yOfPct = [&](double pct) -> int {
            return plot.bottom() - qRound(((pct - yMinPct) / ySpanPct) * static_cast<double>(plot.height()));
        };

        const auto xOfIndex = [&](int index) -> int {
            if (m_points.size() <= 1) {
                return plot.left();
            }
            const double t = static_cast<double>(index) / static_cast<double>(m_points.size() - 1);
            return plot.left() + qRound(t * static_cast<double>(plot.width()));
        };

        const QPen gridPen(m_cfg.timelineChartGridColor, 1.0, Qt::DashLine);
        painter.setPen(gridPen);
        for (int i = 0; i <= 4; ++i) {
            const int y = plot.top() + (plot.height() * i) / 4;
            painter.drawLine(plot.left(), y, plot.right(), y);
        }
        for (int i = 0; i <= 4; ++i) {
            const int x = plot.left() + (plot.width() * i) / 4;
            painter.drawLine(x, plot.top(), x, plot.bottom());
        }

        const int yZero = yOfPct(0.0);
        if (yZero >= plot.top() && yZero <= plot.bottom()) {
            painter.setPen(QPen(m_cfg.timelineChartGridColor.lighter(150), 1.6, Qt::DashLine));
            painter.drawLine(plot.left(), yZero, plot.right(), yZero);

            QFont rightLabelFont = painter.font();
            rightLabelFont.setBold(true);
            painter.setFont(rightLabelFont);
            painter.setPen(QPen(m_cfg.timelineChartTextColor));
            painter.drawText(
                plot.right() + 6,
                yZero - 10,
                rightMargin - 6,
                20,
                Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("0%")
            );

            QFont normalFont = painter.font();
            normalFont.setBold(false);
            painter.setFont(normalFont);
        }

        painter.setPen(QPen(m_cfg.timelineChartTextColor));
        for (int i = 0; i <= 4; ++i) {
            const double pct = yMaxPct - (ySpanPct * static_cast<double>(i) / 4.0);
            const int y = plot.top() + (plot.height() * i) / 4;
            painter.drawText(
                r.left(),
                y - 10,
                leftMargin - 6,
                20,
                Qt::AlignRight | Qt::AlignVCenter,
                QStringLiteral("%1%").arg(QString::number(pct, 'f', 2))
            );
        }

        const int xLabelY = plot.bottom() + 8;
        if (isAshareStock) {
            int morningLastIndex = -1;
            int afternoonFirstIndex = -1;
            for (int i = 0; i < m_points.size(); ++i) {
                if (!m_points.at(i).time.isValid()) {
                    continue;
                }
                const QTime pointTime = m_points.at(i).time.time();
                if (pointTime <= QTime(11, 30)) {
                    morningLastIndex = i;
                }
                if (afternoonFirstIndex < 0 && pointTime >= QTime(13, 0)) {
                    afternoonFirstIndex = i;
                }
            }

            if (morningLastIndex >= 0 && afternoonFirstIndex >= 0 && afternoonFirstIndex > morningLastIndex) {
                const int x1130 = xOfIndex(morningLastIndex);
                const int x1300 = xOfIndex(afternoonFirstIndex);
                const int xSep = (x1130 + x1300) / 2;
                painter.setPen(QPen(m_cfg.timelineChartGridColor.lighter(130), 1.0, Qt::DashLine));
                painter.drawLine(xSep, plot.top(), xSep, plot.bottom());
                painter.setPen(QPen(m_cfg.timelineChartTextColor));
                painter.drawText(
                    xSep - 48,
                    xLabelY,
                    96,
                    18,
                    Qt::AlignHCenter | Qt::AlignTop,
                    QStringLiteral("11:30/13:00")
                );
            } else if (morningLastIndex >= 0) {
                const int x1130 = xOfIndex(morningLastIndex);
                painter.drawText(x1130 - 30, xLabelY, 60, 18, Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("11:30"));
            } else if (afternoonFirstIndex >= 0) {
                const int x1300 = xOfIndex(afternoonFirstIndex);
                painter.drawText(x1300 - 30, xLabelY, 60, 18, Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("13:00"));
            }
            painter.setPen(QPen(m_cfg.timelineChartTextColor));
        } else {
            const int midIndex = m_points.size() / 2;
            const QString leftTime = m_points.first().time.isValid()
                ? m_points.first().time.toString(QStringLiteral("HH:mm"))
                : QStringLiteral("--:--");
            const QString midTime = m_points.at(midIndex).time.isValid()
                ? m_points.at(midIndex).time.toString(QStringLiteral("HH:mm"))
                : QStringLiteral("--:--");
            const QString rightTime = m_points.last().time.isValid()
                ? m_points.last().time.toString(QStringLiteral("HH:mm"))
                : QStringLiteral("--:--");
            painter.drawText(plot.left() - 6, xLabelY, 60, 18, Qt::AlignLeft | Qt::AlignTop, leftTime);
            painter.drawText(xOfIndex(midIndex) - 30, xLabelY, 60, 18, Qt::AlignHCenter | Qt::AlignTop, midTime);
            painter.drawText(plot.right() - 54, xLabelY, 60, 18, Qt::AlignRight | Qt::AlignTop, rightTime);
        }

        QPainterPath pricePath;
        QPainterPath avgPath;
        bool hasPricePath = false;
        bool hasAvgPath = false;
        for (int i = 0; i < m_points.size(); ++i) {
            const int x = xOfIndex(i);
            const double pricePct = toPct(m_points.at(i).price, baseline);
            if (std::isfinite(pricePct)) {
                const int yPrice = yOfPct(pricePct);
                if (!hasPricePath) {
                    pricePath.moveTo(x, yPrice);
                    hasPricePath = true;
                } else {
                    pricePath.lineTo(x, yPrice);
                }
            }

            if (std::isfinite(m_points.at(i).avgPrice)) {
                const double avgPct = toPct(m_points.at(i).avgPrice, baseline);
                if (!std::isfinite(avgPct)) {
                    continue;
                }
                const int yAvg = yOfPct(avgPct);
                if (!hasAvgPath) {
                    avgPath.moveTo(x, yAvg);
                    hasAvgPath = true;
                } else {
                    avgPath.lineTo(x, yAvg);
                }
            }
        }

        const TimelinePoint& lastPoint = m_points.last();
        const double latestPct = toPct(lastPoint.price, baseline);
        const double latestChange = std::isfinite(lastPoint.price)
            ? (lastPoint.price - baseline)
            : qQNaN();

        QColor trendColor = m_cfg.timelineChartPriceLineColor;
        if (std::isfinite(latestPct)) {
            if (latestPct > 0.0) {
                trendColor = m_cfg.timelineChartUpColor;
            } else if (latestPct < 0.0) {
                trendColor = m_cfg.timelineChartDownColor;
            }
        }

        if (hasPricePath) {
            painter.setPen(QPen(trendColor, 1.8));
            painter.drawPath(pricePath);
        }

        if (hasAvgPath) {
            painter.setPen(QPen(m_cfg.timelineChartAvgLineColor, 1.2));
            painter.drawPath(avgPath);
        }

        if (std::isfinite(latestPct) && std::isfinite(latestChange)) {
            const QString changeInfo = QStringLiteral("%1%  %2")
                .arg(signedNumber(latestPct, 2))
                .arg(signedNumber(latestChange, 3));
            painter.setPen(QPen(trendColor));

            QFontMetrics fm(painter.font());
            const int titleAdvance = fm.horizontalAdvance(m_title + QStringLiteral("  "));
            const int minRightSpace = 120;
            if (titleAdvance + minRightSpace < headerRect.width()) {
                painter.drawText(
                    headerRect.left() + titleAdvance,
                    headerRect.top(),
                    headerRect.width() - titleAdvance,
                    headerRect.height(),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    changeInfo
                );
            } else {
                painter.drawText(headerRect, Qt::AlignRight | Qt::AlignVCenter, changeInfo);
            }
        }
    }

private:
    AppConfig m_cfg;
    QString m_title;
    QString m_code;
    QString m_status;
    QVector<TimelinePoint> m_points;
    double m_preClose = qQNaN();
};

class TimelineChartPopup : public QWidget {
public:
    explicit TimelineChartPopup(QWidget* parent = nullptr)
        : QWidget(nullptr)
        , m_parentWindow(parent) {
        Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint;
        setWindowFlags(flags);
        setAttribute(Qt::WA_ShowWithoutActivating, true);

        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        m_chart = new TimelineChartWidget(this);
        layout->addWidget(m_chart);

        m_refreshTimer = new QTimer(this);
        connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
            if (!isVisible() || !isAshareTradingTimeNow()) {
                m_refreshTimer->stop();
                return;
            }
            fetchTimeline(true);
        });
    }

    void applyConfig(const AppConfig& cfg) {
        m_cfg = cfg;
        if (m_chart) {
            m_chart->setConfig(cfg);
        }
        if (!isVisible()) {
            return;
        }

        if (isAshareTradingTimeNow()) {
            startRefreshTimer();
        } else {
            stopRefreshTimer();
        }
    }

    void showForStock(const QString& code, const QString& name, const QRect& anchorRect, int baseWidth) {
        if (code.trimmed().isEmpty() || !isAshareTimelineSupportedCode(code)) {
            hidePopup();
            return;
        }

        const bool changed = (m_code.compare(code, Qt::CaseInsensitive) != 0);
        m_code = code.trimmed();
        m_name = name.trimmed();

        const int popupWidth = qMax(
            kTimelinePopupMinWidth,
            qRound(static_cast<double>(qMax(1, baseWidth)) * kTimelinePopupWidthScale)
        );
        const int popupHeight = qMax(
            kTimelinePopupMinHeight,
            qRound(static_cast<double>(qMax(1, baseWidth)) * kTimelinePopupHeightScale)
        );
        resize(popupWidth, popupHeight);

        QRect targetRect = QRect(anchorRect.topRight() + QPoint(12, 0), size());
        const QList<QScreen*> screens = QGuiApplication::screens();
        QRect screenRect;
        for (QScreen* screen : screens) {
            if (screen && screen->geometry().contains(anchorRect.center())) {
                screenRect = screen->availableGeometry();
                break;
            }
        }
        if (!screenRect.isValid()) {
            if (QScreen* screen = QGuiApplication::primaryScreen()) {
                screenRect = screen->availableGeometry();
            }
        }

        if (screenRect.isValid()) {
            if (targetRect.right() + kTimelinePopupScreenMarginPx > screenRect.right()) {
                targetRect.moveLeft(anchorRect.left() - popupWidth - 12);
            }
            if (targetRect.left() < screenRect.left() + kTimelinePopupScreenMarginPx) {
                targetRect.moveLeft(screenRect.left() + kTimelinePopupScreenMarginPx);
            }
            if (targetRect.bottom() + kTimelinePopupScreenMarginPx > screenRect.bottom()) {
                targetRect.moveTop(screenRect.bottom() - popupHeight - kTimelinePopupScreenMarginPx);
            }
            if (targetRect.top() < screenRect.top() + kTimelinePopupScreenMarginPx) {
                targetRect.moveTop(screenRect.top() + kTimelinePopupScreenMarginPx);
            }
        }
        setGeometry(targetRect);

        if (!isVisible()) {
            show();
            raise();
        }

        if (changed) {
            fetchTimeline(true);
        }

        if (isAshareTradingTimeNow()) {
            startRefreshTimer();
        } else {
            stopRefreshTimer();
        }
    }

    void hidePopup() {
        stopRefreshTimer();
        if (m_reply) {
            QNetworkReply* pendingReply = m_reply;
            m_reply = nullptr;
            pendingReply->abort();
            pendingReply->deleteLater();
        }
        m_code.clear();
        m_name.clear();
        hide();
    }

private:
    void startRefreshTimer() {
        if (!m_refreshTimer) {
            return;
        }
        const int intervalMs = qBound(10, m_cfg.timelineChartRefreshSecs, 3600) * 1000;
        m_refreshTimer->start(intervalMs);
    }

    void stopRefreshTimer() {
        if (m_refreshTimer) {
            m_refreshTimer->stop();
        }
    }

    void requestTimeline(int days, bool fallbackAllowed, bool keepLastTradingDayOnly, int token) {
        if (m_reply) {
            QNetworkReply* pendingReply = m_reply;
            m_reply = nullptr;
            pendingReply->abort();
            pendingReply->deleteLater();
        }

        const QString secId = toTimelineSecId(m_code);
        if (secId.isEmpty()) {
            m_chart->setStatusText(QStringLiteral("Unsupported code: %1").arg(m_code));
            return;
        }

        QUrl url(QStringLiteral("https://push2his.eastmoney.com/api/qt/stock/trends2/get"));
        QUrlQuery query;
        const QString callback = QStringLiteral("jQuery%1_%2")
            .arg(QDateTime::currentMSecsSinceEpoch() % 1000000)
            .arg(QDateTime::currentMSecsSinceEpoch());
        query.addQueryItem(QStringLiteral("cb"), callback);
        query.addQueryItem(QStringLiteral("secid"), secId);
        query.addQueryItem(QStringLiteral("ut"), QStringLiteral("fa5fd1943c7b386f172d6893dbfba10b"));
        query.addQueryItem(QStringLiteral("fields1"), QStringLiteral("f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13"));
        query.addQueryItem(QStringLiteral("fields2"), QStringLiteral("f51,f52,f53,f54,f55,f56,f57,f58"));
        query.addQueryItem(QStringLiteral("iscr"), QStringLiteral("0"));
        query.addQueryItem(QStringLiteral("ndays"), QString::number(days));
        query.addQueryItem(QStringLiteral("_"), QString::number(QDateTime::currentMSecsSinceEpoch()));
        url.setQuery(query);

        QNetworkRequest req(url);
        const QString userAgent = m_cfg.userAgent.trimmed().isEmpty()
            ? defaultChrome100UserAgent()
            : m_cfg.userAgent.trimmed();
        req.setRawHeader("User-Agent", userAgent.toUtf8());
        req.setRawHeader("Referer", "https://quote.eastmoney.com/");
        req.setTransferTimeout(10000);

        QNetworkReply* reply = m_nam.get(req);
        m_reply = reply;
        connect(reply, &QNetworkReply::finished, this, [this, reply, token, days, fallbackAllowed, keepLastTradingDayOnly]() {
            if (reply == m_reply) {
                m_reply = nullptr;
            }

            if (token != m_requestToken) {
                reply->deleteLater();
                return;
            }

            const QString error = reply->error() == QNetworkReply::NoError
                ? QString()
                : reply->errorString();
            const QByteArray body = reply->readAll();
            reply->deleteLater();

            if (!error.isEmpty()) {
                m_chart->setStatusText(QStringLiteral("Request failed: %1").arg(error));
                return;
            }

            QVector<TimelinePoint> points;
            double preClose = qQNaN();
            if (!parseTimelinePayload(body, &points, &preClose, keepLastTradingDayOnly)) {
                m_chart->setStatusText(QStringLiteral("Timeline parse failed"));
                return;
            }

            if (points.isEmpty() && days == 1 && fallbackAllowed) {
                requestTimeline(2, false, true, token);
                return;
            }

            const QString title = m_name.isEmpty()
                ? m_code
                : QStringLiteral("%1  %2").arg(m_name, m_code);
            m_chart->setSeries(title, m_code, points, preClose);
        });
    }

    void fetchTimeline(bool fallbackAllowed) {
        ++m_requestToken;
        const QString title = m_name.isEmpty()
            ? m_code
            : QStringLiteral("%1  %2").arg(m_name, m_code);
        m_chart->setStatusText(QStringLiteral("Loading %1 ...").arg(title));
        requestTimeline(1, fallbackAllowed, false, m_requestToken);
    }

private:
    QWidget* m_parentWindow = nullptr;
    AppConfig m_cfg;
    TimelineChartWidget* m_chart = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QNetworkAccessManager m_nam;
    QNetworkReply* m_reply = nullptr;
    QString m_code;
    QString m_name;
    int m_requestToken = 0;
};

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

        if (visibleColumns.size() < 2) {
            return;
        }

        const int contentColumn = visibleColumns.at(1);
        const int spanWidth = qMax(1, ColCount - contentColumn);
        for (int row = 0; row < quoteModel->rowCount(); ++row) {
            if (quoteModel->rowKind(row) == QuoteModel::RowKindQuote) {
                continue;
            }
            setSpan(row, contentColumn, 1, spanWidth);
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

        QVector<int> visibleColumns;
        QHeaderView* header = m_table->horizontalHeader();
        for (int visual = 0; visual < header->count(); ++visual) {
            const int logical = header->logicalIndex(visual);
            if (logical < 0 || m_table->isColumnHidden(logical)) {
                continue;
            }
            visibleColumns.push_back(logical);
        }

        if (visibleColumns.size() < 2) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        const int contentColumn = visibleColumns.at(1);
        if (index.column() != contentColumn) {
            QStyledItemDelegate::paint(painter, option, index);
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
    const int initialPadding = floatingWindowPaddingPx(m_cfg);
    panelLayout->setContentsMargins(
        initialPadding,
        initialPadding,
        initialPadding,
        initialPadding
    );

    m_table = new BottomGridTableView(m_panel);
    m_table->setModel(m_model);
    m_table->setItemDelegate(new HotRankFlipDelegate(m_table, m_model, m_table));
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(false);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setMouseTracking(true);
    m_table->viewport()->setMouseTracking(true);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->viewport()->setObjectName("tableViewport");
    m_table->viewport()->setAttribute(Qt::WA_StyledBackground, true);

#ifdef WIN32
    // Windows native style (QWindowsVistaStyle) ignores stylesheet color and
    // ForegroundRole. Force Fusion so our palette and stylesheet are respected.
    if (QStyle* fusion = QStyleFactory::create("Fusion")) {
        m_table->setStyle(fusion);
        m_table->horizontalHeader()->setStyle(fusion);
    }
#endif

    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);

    QHeaderView* header = m_table->horizontalHeader();
    header->viewport()->setObjectName("tableHeaderViewport");
    header->viewport()->setAttribute(Qt::WA_StyledBackground, true);
    header->setSectionsMovable(false);
    header->setStretchLastSection(false);
    header->setMinimumSectionSize(0);
    header->setSectionResizeMode(QHeaderView::Fixed);

    panelLayout->addWidget(m_table);
    root->addWidget(m_panel);

    m_panel->installEventFilter(this);
    m_table->installEventFilter(this);
    m_table->viewport()->installEventFilter(this);
    m_table->horizontalHeader()->installEventFilter(this);

    connect(m_model, &QAbstractItemModel::dataChanged, this, [this]() {
        adjustWindowSize();
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this]() {
        adjustWindowSize();
    });
    connect(m_model, &QAbstractItemModel::layoutChanged, this, [this]() {
        adjustWindowSize();
    });
    connect(m_model, &QAbstractItemModel::rowsInserted, this, [this]() {
        adjustWindowSize();
    });
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, [this]() {
        adjustWindowSize();
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

    m_timelinePopup = new TimelineChartPopup(this);
    m_timelinePopup->applyConfig(m_cfg);
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
        return;
    }

    if (!m_cfg.hoverReadingEnabled) {
        if (m_hoverTimer) {
            m_hoverTimer->stop();
        }
        setHoverReadingActive(false, animated);
        if (m_cfg.mousePassthroughEnabled) {
            hideTimelinePopup();
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

void FloatingWindow::updateTimelinePopupForHover(const QPoint& viewportPos) {
    if (!m_table || !m_model || !m_timelinePopup) {
        return;
    }

    if (!canShowTimelinePopup()) {
        hideTimelinePopup();
        return;
    }

    const QModelIndex index = m_table->indexAt(viewportPos);
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
    if (code.isEmpty() || !isAshareTimelineSupportedCode(code)) {
        hideTimelinePopup();
        return;
    }

    const QRect visual = m_table->visualRect(index);
    const QPoint globalTopLeft = m_table->viewport()->mapToGlobal(visual.topLeft());
    const QRect globalAnchor(globalTopLeft, visual.size());

    m_timelineHoverCode = code;
    m_timelineHoverName = name;
    m_timelinePopup->showForStock(code, name, globalAnchor, width());
}

void FloatingWindow::hideTimelinePopup() {
    m_timelineHoverCode.clear();
    m_timelineHoverName.clear();
    if (m_timelinePopup) {
        m_timelinePopup->hidePopup();
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
    switch (event->type()) {
    case QEvent::Enter:
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
        if (watched == m_panel
            || watched == m_table
            || watched == m_table->viewport()
            || watched == m_table->horizontalHeader()) {
            if (!shouldAllowMouseInteraction()) {
                if (m_hoverTimer) {
                    m_hoverTimer->stop();
                }
                setHoverReadingActive(false, false);
                return false;
            }
            updateHoverReadingState(false);

            if (watched == m_table->viewport()) {
                QPoint hoverPos;
                if (event->type() == QEvent::MouseMove) {
                    hoverPos = static_cast<QMouseEvent*>(event)->position().toPoint();
                } else if (event->type() == QEvent::HoverMove || event->type() == QEvent::HoverEnter) {
                    hoverPos = static_cast<QHoverEvent*>(event)->position().toPoint();
                } else {
                    hoverPos = m_table->viewport()->mapFromGlobal(QCursor::pos());
                }
                updateTimelinePopupForHover(hoverPos);
            }
        }
        break;
    case QEvent::Leave:
    case QEvent::HoverLeave:
        if (watched == m_panel
            || watched == m_table
            || watched == m_table->viewport()
            || watched == m_table->horizontalHeader()) {
            updateHoverReadingState(true);
            hideTimelinePopup();
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
            || watched == m_table->horizontalHeader()) {
            if (!shouldAllowMouseInteraction()) {
                if (m_hoverTimer) {
                    m_hoverTimer->stop();
                }
                setHoverReadingActive(false, false);
                return false;
            }
            updateHoverReadingState(false);

            if (watched == m_table->viewport()) {
                updateTimelinePopupForHover(mouseEvent->position().toPoint());
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
        if (mouseEvent->button() == Qt::LeftButton
            && m_cfg.floatingWindowDoubleClickToHide
            && !m_cfg.mousePassthroughEnabled
            && shouldAllowMouseInteraction()) {
            if (m_dragging) {
                m_dragging = false;
                m_dragButton = Qt::NoButton;
                releaseMouse();
            }
            hide();
            return true;
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

    applyStyle();
    applyColumns();
    if (m_timelinePopup) {
        m_timelinePopup->applyConfig(m_cfg);
        if (!m_cfg.timelineChartEnabled) {
            hideTimelinePopup();
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
        "background: transparent;"
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

#ifdef WIN32
    // Sync palette so Fusion style picks up the correct text / base colors.
    QPalette pal = m_table->palette();
    pal.setColor(QPalette::Base, b);
    pal.setColor(QPalette::Text, t);
    pal.setColor(QPalette::WindowText, t);
    m_table->setPalette(pal);
    m_table->viewport()->setPalette(pal);
    m_table->horizontalHeader()->setPalette(pal);
#endif
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

#ifdef WIN32
    QPalette pal = m_table->palette();
    pal.setColor(QPalette::Base, tableBackground);
    pal.setColor(QPalette::Text, theme.textPrimary);
    pal.setColor(QPalette::WindowText, theme.textPrimary);
    m_table->setPalette(pal);
    m_table->viewport()->setPalette(pal);
    m_table->horizontalHeader()->setPalette(pal);
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
    const QColor normalHeaderBg(0, 0, 0, 0);
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

#ifdef WIN32
    QPalette pal = m_table->palette();
    pal.setColor(QPalette::Base, tableViewportBg);
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::WindowText, text);
    m_table->setPalette(pal);
    m_table->viewport()->setPalette(pal);
    m_table->horizontalHeader()->setPalette(pal);
#endif
}

void FloatingWindow::applyColumns() {
    const QVector<int> columnOrder = normalizedColumnOrder(m_cfg.columnOrder);
    QHeaderView* header = m_table->horizontalHeader();

    // Apply column visual order.
    {
        QSignalBlocker blocker(header);
        header->setSectionsMovable(true);
        for (int visualIndex = 0; visualIndex < columnOrder.size(); ++visualIndex) {
            const int logical = columnOrder[visualIndex];
            const int from = header->visualIndex(logical);
            if (from >= 0 && from != visualIndex) {
                header->moveSection(from, visualIndex);
            }
        }
        header->setSectionsMovable(false);
    }

    // Set visibility for each column.
    for (int i = 0; i < ColCount; ++i) {
        const bool visible = m_cfg.visibleColumns.value(i, true);
        m_table->setColumnHidden(i, !visible);
    }

    adjustWindowSize();
}

void FloatingWindow::adjustWindowSize() {
    if (auto* table = static_cast<BottomGridTableView*>(m_table)) {
        table->syncSpecialRowSpans(m_model);
    }

    // Auto-size each visible column to its content.
    for (int i = 0; i < ColCount; ++i) {
        if (m_table->isColumnHidden(i)) {
            continue;
        }
        m_table->setColumnWidth(i, autoColumnWidthFromContent(i));
    }

    QVector<int> visibleColumns;
    QHeaderView* header = m_table->horizontalHeader();
    for (int visual = 0; visual < header->count(); ++visual) {
        const int logical = header->logicalIndex(visual);
        if (logical < 0 || m_table->isColumnHidden(logical)) {
            continue;
        }
        visibleColumns.push_back(logical);
    }

    // Width = sum of visible column widths.
    int totalWidth = 0;
    for (int i = 0; i < ColCount; ++i) {
        if (m_table->isColumnHidden(i)) {
            continue;
        }
        totalWidth += m_table->columnWidth(i);
    }

    // Height = header + rows.
    int totalHeight = 0;
    if (m_table->horizontalHeader()->isVisible()) {
        totalHeight += m_table->horizontalHeader()->sizeHint().height();
    }
    totalHeight += m_model->rowCount() * m_table->verticalHeader()->defaultSectionSize();

    const int padding = floatingWindowPaddingPx(m_cfg);
    const int horizontalPadding = padding * 2;
    const int verticalPadding = padding * 2;
    const int safeWidth = qMax(totalWidth + horizontalPadding, 1);
    const int safeHeight = qMax(totalHeight + verticalPadding, 1);
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
