#include "floating_window.h"
#include "market_breadth_window.h"

#include "app_constants.h"
#include "i18n.h"
#include "quote_provider.h"
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

void enforceTimelinePopupWindowLevel(const QWidget* widget) {
    if (!widget) {
        return;
    }

#ifdef WIN32
    const HWND hwnd = reinterpret_cast<HWND>(widget->winId());
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
#elif defined(Q_OS_MACOS)
    void* nsWindow = macWindowHandleForWidget(widget);
    if (nsWindow) {
        const long level = static_cast<long>(CGWindowLevelForKey(kCGPopUpMenuWindowLevelKey));
        auto sendIntegerMessage = reinterpret_cast<void (*)(void*, SEL, long)>(objc_msgSend);
        sendIntegerMessage(nsWindow, sel_registerName("setLevel:"), level);
    }
#endif

    const_cast<QWidget*>(widget)->raise();
}

inline constexpr int kTimelinePopupFixedWidth = 560;
inline constexpr int kTimelinePopupFixedHeight = 360;
inline constexpr int kTimelinePopupScreenMarginPx = 12;

struct TimelinePoint {
    QDateTime time;
    double price = qQNaN();
    double avgPrice = qQNaN();
    double volume = qQNaN();
    double amount = qQNaN();
};

struct TradePeriod {
    QDateTime begin;
    QDateTime end;
};

enum class TimelineMarket {
    Unknown,
    Ashare,
    HongKong,
    Sector,
    Future,
};

struct TimelineSession {
    QTime morningStart;
    QTime morningEnd;
    QTime afternoonStart;
    QTime afternoonEnd;
    QString midLabel;
};

struct TimelineCacheEntry {
    QVector<TimelinePoint> points;
    double preClose = qQNaN();
    QVector<TradePeriod> tradePeriods;
    QDateTime expiresAtUtc;
};

bool isAshareIndexCode(const QString& rawCode) {
    return watchlist_utils::isPredefinedAshareIndexCode(rawCode);
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

bool isAshareStockCode(const QString& rawCode) {
    const QString code = rawCode.trimmed().toLower();
    if (code.isEmpty() || isAshareIndexCode(code)) {
        return false;
    }

    if (code.startsWith(QStringLiteral("sh"))
        || code.startsWith(QStringLiteral("sz"))
        || code.startsWith(QStringLiteral("bj"))) {
        const QString symbol = code.mid(2);
        return symbol.size() == 6 && watchlist_utils::isDigitsOnly(symbol);
    }

    if (code.startsWith(QStringLiteral("0.")) || code.startsWith(QStringLiteral("1."))) {
        const QString symbol = code.mid(2);
        return symbol.size() == 6
            && watchlist_utils::isDigitsOnly(symbol)
            && !isAshareIndexCode(symbol);
    }

    return code.size() == 6
        && watchlist_utils::isDigitsOnly(code)
        && !isAshareIndexCode(code);
}

bool isAshareTimelineSupportedCode(const QString& rawCode) {
    return isAshareStockCode(rawCode) || isAshareIndexCode(rawCode);
}

bool isHongKongTimelineSupportedCode(const QString& rawCode) {
    const QString code = rawCode.trimmed();
    if (code.isEmpty()) {
        return false;
    }

    if (!watchlist_utils::normalizeHongKongIndexCode(code).isEmpty()) {
        return true;
    }
    if (watchlist_utils::isHongKongCode(code)) {
        return true;
    }

    const QString lower = code.toLower();
    const int dot = lower.indexOf(QLatin1Char('.'));
    if (dot > 0 && dot < lower.size() - 1) {
        const QString market = lower.left(dot);
        return market == QLatin1String("100")
            || market == QLatin1String("124")
            || market == QLatin1String("128")
            || market == QLatin1String("116");
    }

    return lower.endsWith(QStringLiteral(".hk"));
}

bool isSectorTimelineSupportedCode(const QString& rawCode) {
    return !watchlist_utils::normalizeSectorCode(rawCode).isEmpty();
}

bool isFutureTimelineSupportedCode(const QString& rawCode) {
    return !watchlist_utils::normalizeFutureCode(rawCode).isEmpty();
}

TimelineMarket timelineMarketOfCode(const QString& rawCode) {
    if (isAshareTimelineSupportedCode(rawCode)) {
        return TimelineMarket::Ashare;
    }
    if (isHongKongTimelineSupportedCode(rawCode)) {
        return TimelineMarket::HongKong;
    }
    if (isSectorTimelineSupportedCode(rawCode)) {
        return TimelineMarket::Sector;
    }
    if (isFutureTimelineSupportedCode(rawCode)) {
        return TimelineMarket::Future;
    }
    return TimelineMarket::Unknown;
}

bool isTimelineSupportedCode(const QString& rawCode) {
    return timelineMarketOfCode(rawCode) != TimelineMarket::Unknown;
}

TimelineSession timelineSessionForMarket(TimelineMarket market) {
    if (market == TimelineMarket::Ashare || market == TimelineMarket::Sector) {
        return {
            QTime(9, 30),
            QTime(11, 30),
            QTime(13, 0),
            QTime(15, 0),
            QStringLiteral("11:30/13:00"),
        };
    }

    if (market == TimelineMarket::HongKong) {
        return {
            QTime(9, 30),
            QTime(12, 0),
            QTime(13, 0),
            QTime(16, 0),
            QStringLiteral("12:00/13:00"),
        };
    }

    return {};
}

bool hasValidTimelineSession(const TimelineSession& session) {
    return session.morningStart.isValid()
        && session.morningEnd.isValid()
        && session.afternoonStart.isValid()
        && session.afternoonEnd.isValid()
        && session.morningStart < session.morningEnd
        && session.afternoonStart < session.afternoonEnd;
}

const QStringList& hardcodedAshareIntradayXAxis() {
    static const QStringList labels = []() {
        QStringList out;
        out.reserve(241);

        // Morning session: 09:30-11:30 (inclusive)
        QTime t(9, 30);
        const QTime morningEnd(11, 30);
        while (t <= morningEnd) {
            out.push_back(t.toString(QStringLiteral("HH:mm")));
            t = t.addSecs(60);
        }

        // Afternoon session: 13:01-15:00 (inclusive)
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
    const QString raw = rawCode.trimmed();
    const QString code = raw.toLower();
    const TimelineMarket marketType = timelineMarketOfCode(raw);
    if (code.isEmpty() || marketType == TimelineMarket::Unknown) {
        return {};
    }

    if (marketType == TimelineMarket::Sector) {
        const QString sector = watchlist_utils::normalizeSectorCode(raw);
        return sector.isEmpty() ? QString() : (QStringLiteral("90.") + sector);
    }

    if (marketType == TimelineMarket::Future) {
        return watchlist_utils::normalizeFutureCode(raw);
    }

    const int dot = code.indexOf(QLatin1Char('.'));
    if (dot > 0 && dot < code.size() - 1) {
        const QString market = code.left(dot);
        const QString symbol = raw.mid(dot + 1).trimmed();

        if (marketType == TimelineMarket::Ashare) {
            bool marketOk = false;
            const int marketValue = market.toInt(&marketOk);
            bool symbolOk = !symbol.isEmpty();
            for (QChar ch : symbol) {
                if (!ch.isLetterOrNumber()) {
                    symbolOk = false;
                    break;
                }
            }
            if (marketOk && symbolOk) {
                if ((marketValue == 0 || marketValue == 1)
                    && symbol.size() == 6
                    && watchlist_utils::isDigitsOnly(symbol)) {
                    return QString::number(marketValue)
                        + QStringLiteral(".")
                        + symbol.toUpper();
                }
                return {};
            }
        }

        if (marketType == TimelineMarket::HongKong) {
            QString hkDigits;
            for (QChar ch : symbol) {
                if (ch.isDigit()) {
                    hkDigits.append(ch);
                }
            }

            if (market == QLatin1String("116")) {
                if (hkDigits.isEmpty()) {
                    return {};
                }
                if (hkDigits.size() > 5) {
                    hkDigits = hkDigits.right(5);
                }
                return QStringLiteral("116.") + hkDigits.rightJustified(5, '0');
            }

            if (market == QLatin1String("100")
                || market == QLatin1String("124")
                || market == QLatin1String("128")) {
                const QString hkIndexCode = watchlist_utils::normalizeHongKongIndexCode(
                    market + QStringLiteral(".") + symbol
                );
                if (!hkIndexCode.isEmpty()) {
                    return hkIndexCode;
                }

                const QString upperSymbol = symbol.toUpper();
                if (!upperSymbol.isEmpty()) {
                    return market + QStringLiteral(".") + upperSymbol;
                }
            }

            return {};
        }
    }

    if (marketType == TimelineMarket::HongKong) {
        const QString hkIndexCode = watchlist_utils::normalizeHongKongIndexCode(raw);
        if (!hkIndexCode.isEmpty()) {
            return hkIndexCode;
        }

        const auto hkSecIdFromText = [](const QString& text) -> QString {
            QString digits;
            for (QChar ch : text) {
                if (ch.isDigit()) {
                    digits.append(ch);
                }
            }
            if (digits.isEmpty()) {
                return {};
            }
            if (digits.size() > 5) {
                digits = digits.right(5);
            }
            return QStringLiteral("116.") + digits.rightJustified(5, '0');
        };

        if (code.endsWith(QStringLiteral(".hk"))) {
            return hkSecIdFromText(code);
        }
        if (code.startsWith(QStringLiteral("hk"))) {
            return hkSecIdFromText(code);
        }
        if (code.size() == 5 && watchlist_utils::isDigitsOnly(code)) {
            return QStringLiteral("116.") + code;
        }
        return {};
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

QString timelineRequestCacheKey(const QString& secId, int days, bool keepLastTradingDayOnly) {
    return QStringLiteral("%1|%2|%3")
        .arg(secId, QString::number(days), keepLastTradingDayOnly ? QStringLiteral("1") : QStringLiteral("0"));
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

bool isHongKongTradingTimeNow(const QDate& halfDayDate = QDate()) {
    const QTimeZone bjZone("Asia/Shanghai");
    if (!bjZone.isValid()) {
        return false;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc().toTimeZone(bjZone);
    const int dayOfWeek = now.date().dayOfWeek();
    if (dayOfWeek < 1 || dayOfWeek > 5) {
        return false;
    }

    const bool isHalfDayToday = halfDayDate.isValid() && halfDayDate == now.date();
    const QTime time = now.time();
    const bool morning = time >= QTime(9, 30) && time <= QTime(12, 0);
    const bool afternoon = !isHalfDayToday
        && time >= QTime(13, 0)
        && time <= QTime(16, 0);
    return morning || afternoon;
}

QString stripJsonp(const QByteArray& body) {
    const QString text = QString::fromUtf8(body).trimmed();
    if (text.isEmpty()) {
        return {};
    }
    if (text.startsWith(QLatin1Char('{')) || text.startsWith(QLatin1Char('['))) {
        return text;
    }

    const int open = text.indexOf(QLatin1Char('('));
    const int close = text.lastIndexOf(QLatin1Char(')'));
    if (open <= 0 || close <= open) {
        return {};
    }
    const QString callback = text.left(open).trimmed();
    if (callback.isEmpty()) {
        return {};
    }
    return text.mid(open + 1, close - open - 1).trimmed();
}

bool parseTimelinePayload(
    const QByteArray& body,
    QVector<TimelinePoint>* outPoints,
    double* outPreClose,
    bool keepLastTradingDayOnly,
    QVector<TradePeriod>* outTradePeriods = nullptr
) {
    if (!outPoints || !outPreClose) {
        return false;
    }

    *outPreClose = qQNaN();
    outPoints->clear();
    if (outTradePeriods) {
        outTradePeriods->clear();
    }

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

    if (outTradePeriods) {
        const QJsonValue tpVal = data.value(QStringLiteral("tradePeriods"));
        if (tpVal.isObject()) {
            const QJsonArray periods = tpVal.toObject().value(QStringLiteral("periods")).toArray();
            for (const QJsonValue& pv : periods) {
                if (!pv.isObject()) {
                    continue;
                }
                const QJsonObject p = pv.toObject();
                const qint64 bVal = static_cast<qint64>(p.value(QStringLiteral("b")).toDouble());
                const qint64 eVal = static_cast<qint64>(p.value(QStringLiteral("e")).toDouble());
                const QString bStr = QString::number(bVal);
                const QString eStr = QString::number(eVal);
                if (bStr.size() != 12 || eStr.size() != 12) {
                    continue;
                }
                const QDateTime bDt = QDateTime::fromString(bStr, QStringLiteral("yyyyMMddHHmm"));
                const QDateTime eDt = QDateTime::fromString(eStr, QStringLiteral("yyyyMMddHHmm"));
                if (bDt.isValid() && eDt.isValid() && bDt < eDt) {
                    outTradePeriods->push_back({bDt, eDt});
                }
            }
        }
    }

    const QJsonArray trends = data.value(QStringLiteral("trends")).toArray();
    if (trends.isEmpty()) {
        return true;
    }

    QString targetDate;
    const auto hasDatePrefix = [](const QString& text) {
        return text.size() >= 10
            && text.at(4) == QLatin1Char('-')
            && text.at(7) == QLatin1Char('-');
    };

    if (keepLastTradingDayOnly) {
        for (int i = trends.size() - 1; i >= 0; --i) {
            const QString row = trends.at(i).toString().trimmed();
            if (hasDatePrefix(row)) {
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
        if (parts.size() < 2) {
            continue;
        }

        const QString timeText = parts.at(0).trimmed();
        if (keepLastTradingDayOnly
            && !targetDate.isEmpty()
            && hasDatePrefix(timeText)
            && !timeText.startsWith(targetDate)) {
            continue;
        }

        QDateTime time = QDateTime::fromString(timeText, QStringLiteral("yyyy-MM-dd HH:mm"));
        if (!time.isValid()) {
            const QTime hhmm = QTime::fromString(timeText, QStringLiteral("HH:mm"));
            if (hhmm.isValid()) {
                time = QDateTime(QDate::currentDate(), hhmm);
            }
        }

        bool priceOk = false;
        bool avgPriceOk = false;
        bool volumeOk = false;
        bool amountOk = false;
        double price = qQNaN();
        double avgPrice = qQNaN();
        double volume = qQNaN();
        double amount = qQNaN();

        if (parts.size() >= 8) {
            price = parts.at(2).toDouble(&priceOk);
            volume = parts.at(5).toDouble(&volumeOk);
            amount = parts.at(6).toDouble(&amountOk);
            avgPrice = parts.at(7).toDouble(&avgPriceOk);
            if (!avgPriceOk && parts.size() >= 4) {
                avgPrice = parts.at(3).toDouble(&avgPriceOk);
            }
        } else {
            price = parts.at(1).toDouble(&priceOk);
            if (parts.size() >= 3) {
                volume = parts.at(2).toDouble(&volumeOk);
            }
            if (parts.size() >= 4) {
                avgPrice = parts.at(3).toDouble(&avgPriceOk);
            }
            if (parts.size() >= 5) {
                amount = parts.at(4).toDouble(&amountOk);
            }
        }

        if (!time.isValid() || !priceOk) {
            continue;
        }

        // Some markets (e.g. HK indexes) may return avgPrice=0.000 in the first row.
        // Treat non-positive averages as missing data to avoid distorting chart y-range.
        if (avgPriceOk && (!std::isfinite(avgPrice) || avgPrice <= 0.0)) {
            avgPriceOk = false;
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
        m_resolvedLanguage = i18n::resolveLanguage(m_cfg.language);
        update();
    }

    void setSeries(const QString& title, const QString& code, const QVector<TimelinePoint>& points, double preClose, const QVector<TradePeriod>& tradePeriods = {}, double cost = qQNaN()) {
        m_title = title;
        m_code = code;
        m_points = points;
        m_preClose = preClose;
        m_tradePeriods = tradePeriods;
        m_cost = cost;
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

        const TimelineMarket market = timelineMarketOfCode(m_code);
        const TimelineSession session = timelineSessionForMarket(market);
        const bool hasSessionAxis = hasValidTimelineSession(session);
        const bool hasTradePeriodAxis = !m_tradePeriods.isEmpty();
        const bool isAshareStock = isAshareStockCode(m_code);
        const double ashareLimitPct = isAshareStock
            ? fixedRangeLimitPctForAshareStock(m_code)
            : qQNaN();
        double yMinPct = 0.0;
        double yMaxPct = 0.0;
        if (m_cfg.timelineChartFixedRangeEnabled && std::isfinite(ashareLimitPct) && ashareLimitPct > 0.0) {
            yMinPct = -ashareLimitPct;
            yMaxPct = ashareLimitPct;
        }
        if (!(yMinPct < yMaxPct)) {
            if (qFuzzyCompare(minPct, maxPct)) {
                minPct -= 0.5;
                maxPct += 0.5;
            }
            double absMaxPct = qMax(std::abs(minPct), std::abs(maxPct));
            if (std::isfinite(ashareLimitPct) && ashareLimitPct > 0.0) {
                // Keep A-share timeline bounded by board limit when auto range is used.
                absMaxPct = qMin(absMaxPct, ashareLimitPct);
            }
            const double halfSpanPct = qMax(0.5, absMaxPct * 1.08);
            yMinPct = -halfSpanPct;
            yMaxPct = halfSpanPct;
            if (std::isfinite(ashareLimitPct) && ashareLimitPct > 0.0) {
                yMinPct = qMax(yMinPct, -ashareLimitPct);
                yMaxPct = qMin(yMaxPct, ashareLimitPct);
                if (!(yMinPct < yMaxPct)) {
                    yMinPct = -ashareLimitPct;
                    yMaxPct = ashareLimitPct;
                }
            }
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

        const auto xOfSessionTime = [&](const QTime& t) -> int {
            if (!hasSessionAxis) {
                return plot.left();
            }

            const int xLeft = plot.left();
            const int xMid = xLeft + qRound(static_cast<double>(plot.width()) * 0.5);
            const int xRight = plot.right();

            if (t <= session.morningStart) {
                return xLeft;
            }
            if (t < session.morningEnd) {
                const int morningSpan = qMax(1, session.morningStart.secsTo(session.morningEnd));
                const int elapsed = qBound(0, session.morningStart.secsTo(t), morningSpan);
                return xLeft + qRound(
                    (static_cast<double>(elapsed) / static_cast<double>(morningSpan))
                    * (xMid - xLeft)
                );
            }

            if (t < session.afternoonStart) {
                return xMid;
            }
            if (t < session.afternoonEnd) {
                const int afternoonSpan = qMax(1, session.afternoonStart.secsTo(session.afternoonEnd));
                const int elapsed = qBound(0, session.afternoonStart.secsTo(t), afternoonSpan);
                return xMid + qRound(
                    (static_cast<double>(elapsed) / static_cast<double>(afternoonSpan))
                    * (xRight - xMid)
                );
            }

            return xRight;
        };

        // Compute total trading seconds across all trade periods (gaps are skipped).
        qint64 tradePeriodTotalSecs = 0;
        for (const TradePeriod& p : m_tradePeriods) {
            tradePeriodTotalSecs += qMax(static_cast<qint64>(0), p.begin.secsTo(p.end));
        }

        const auto xOfTradePeriodTime = [&](const QDateTime& dt) -> int {
            if (tradePeriodTotalSecs <= 0) {
                return plot.left();
            }
            qint64 cumSecs = 0;
            for (const TradePeriod& p : m_tradePeriods) {
                const qint64 periodSecs = qMax(static_cast<qint64>(0), p.begin.secsTo(p.end));
                if (dt <= p.begin) {
                    return plot.left() + qRound(
                        static_cast<double>(cumSecs) / static_cast<double>(tradePeriodTotalSecs) * plot.width()
                    );
                }
                if (dt <= p.end) {
                    const qint64 elapsed = qBound(static_cast<qint64>(0), p.begin.secsTo(dt), periodSecs);
                    return plot.left() + qRound(
                        static_cast<double>(cumSecs + elapsed) / static_cast<double>(tradePeriodTotalSecs) * plot.width()
                    );
                }
                cumSecs += periodSecs;
            }
            return plot.right();
        };

        bool hasHongKongAfternoonMarker = false;
        int hongKongAfternoonMarkerX = 0;
        bool hasHongKongMorningMarker = false;
        int hongKongMorningMarkerX = 0;
        if (hasTradePeriodAxis && market == TimelineMarket::HongKong) {
            QDate tradeDate;
            for (auto it = m_points.crbegin(); it != m_points.crend(); ++it) {
                if (it->time.isValid()) {
                    tradeDate = it->time.date();
                    break;
                }
            }
            if (tradeDate.isValid()) {
                const QDateTime afternoonMarkerTime(tradeDate, QTime(15, 0));
                for (const TradePeriod& p : m_tradePeriods) {
                    if (p.begin <= afternoonMarkerTime && afternoonMarkerTime < p.end) {
                        hasHongKongAfternoonMarker = true;
                        hongKongAfternoonMarkerX = xOfTradePeriodTime(afternoonMarkerTime);
                        break;
                    }
                }
                const QDateTime morningMarkerTime(tradeDate, QTime(11, 30));
                for (const TradePeriod& p : m_tradePeriods) {
                    if (p.begin <= morningMarkerTime && morningMarkerTime < p.end) {
                        hasHongKongMorningMarker = true;
                        hongKongMorningMarkerX = xOfTradePeriodTime(morningMarkerTime);
                        break;
                    }
                }
            }
        }

        const QPen gridPen(m_cfg.timelineChartGridColor, 0.3, Qt::SolidLine);
        QPen periodEndPen(m_cfg.timelineChartGridColor, 1.0, Qt::CustomDashLine);
        periodEndPen.setDashPattern({3.0, 2.0});
        painter.setPen(gridPen);
        for (int i = 0; i <= 4; ++i) {
            const int y = plot.top() + (plot.height() * i) / 4;
            painter.drawLine(plot.left(), y, plot.right(), y);
        }
        if (hasTradePeriodAxis) {
            for (int i = 0; i < m_tradePeriods.size(); ++i) {
                const TradePeriod& p = m_tradePeriods.at(i);
                if (i > 0) {
                    painter.setPen(gridPen);
                    const int xB = xOfTradePeriodTime(p.begin);
                    painter.drawLine(xB, plot.top(), xB, plot.bottom());
                }
                painter.setPen(periodEndPen);
                const int xE = xOfTradePeriodTime(p.end);
                painter.drawLine(xE, plot.top(), xE, plot.bottom());
            }
            if (hasHongKongAfternoonMarker) {
                painter.setPen(periodEndPen);
                painter.drawLine(
                    hongKongAfternoonMarkerX,
                    plot.top(),
                    hongKongAfternoonMarkerX,
                    plot.bottom()
                );
            }
            if (hasHongKongMorningMarker) {
                painter.setPen(periodEndPen);
                painter.drawLine(
                    hongKongMorningMarkerX,
                    plot.top(),
                    hongKongMorningMarkerX,
                    plot.bottom()
                );
            }
        } else if (hasSessionAxis) {
            painter.setPen(gridPen);
            const QTime morningQuarter = session.morningStart.addSecs(
                session.morningStart.secsTo(session.morningEnd) / 2
            );
            const QTime afternoonQuarter = session.afternoonStart.addSecs(
                session.afternoonStart.secsTo(session.afternoonEnd) / 2
            );
            const QTime vTimes[] = {
                session.morningStart,
                morningQuarter,
                session.morningEnd,
                afternoonQuarter,
                session.afternoonEnd,
            };
            for (const QTime& t : vTimes) {
                const int x = xOfSessionTime(t);
                painter.drawLine(x, plot.top(), x, plot.bottom());
            }
        } else {
            painter.setPen(gridPen);
            for (int i = 0; i <= 4; ++i) {
                const int x = plot.left() + (plot.width() * i) / 4;
                painter.drawLine(x, plot.top(), x, plot.bottom());
            }
        }

        const int yZero = yOfPct(0.0);
        if (yZero >= plot.top() && yZero <= plot.bottom()) {
            QPen zeroPen(m_cfg.timelineChartGridColor.lighter(150), 1.0, Qt::CustomDashLine);
            zeroPen.setDashPattern({3.0, 2.0});
            painter.setPen(zeroPen);
            painter.drawLine(plot.left(), yZero, plot.right(), yZero);
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
        painter.setPen(QPen(m_cfg.timelineChartTextColor));
        const QDate firstDate = m_tradePeriods.isEmpty()
            ? QDate()
            : m_tradePeriods.first().begin.date();
        const auto tradePeriodTimeLabel = [&](const QDateTime& dt) -> QString {
            return (dt.date() == firstDate)
                ? dt.toString(QStringLiteral("H:mm"))
                : dt.toString(QStringLiteral("M/d H:mm"));
        };
        if (hasTradePeriodAxis) {
            const QString openLabel  = tradePeriodTimeLabel(m_tradePeriods.first().begin);
            const QString closeLabel = tradePeriodTimeLabel(m_tradePeriods.last().end);
            painter.drawText(plot.left() - 6,   xLabelY, 64,  18, Qt::AlignLeft  | Qt::AlignTop, openLabel);
            painter.drawText(plot.right() - 58, xLabelY, 64,  18, Qt::AlignRight | Qt::AlignTop, closeLabel);
            for (int i = 0; i < m_tradePeriods.size() - 1; ++i) {
                const TradePeriod& cur  = m_tradePeriods.at(i);
                const TradePeriod& next = m_tradePeriods.at(i + 1);
                const int xBreak = xOfTradePeriodTime(cur.end);
                const QString breakLabel = QStringLiteral("%1/%2")
                    .arg(tradePeriodTimeLabel(cur.end))
                    .arg(tradePeriodTimeLabel(next.begin));
                painter.drawText(xBreak - 54, xLabelY, 108, 18, Qt::AlignHCenter | Qt::AlignTop, breakLabel);
            }
            if (hasHongKongAfternoonMarker) {
                painter.drawText(
                    hongKongAfternoonMarkerX - 30,
                    xLabelY,
                    60,
                    18,
                    Qt::AlignHCenter | Qt::AlignTop,
                    QStringLiteral("15:00")
                );
            }
            if (hasHongKongMorningMarker) {
                painter.drawText(
                    hongKongMorningMarkerX - 30,
                    xLabelY,
                    60,
                    18,
                    Qt::AlignHCenter | Qt::AlignTop,
                    QStringLiteral("11:30")
                );
            }
        } else if (hasSessionAxis) {
            const int xOpen  = xOfSessionTime(session.morningStart);
            const int xMid   = xOfSessionTime(session.morningEnd);
            const int xClose = xOfSessionTime(session.afternoonEnd);
            const QString openLabel = session.morningStart.toString(QStringLiteral("H:mm"));
            const QString closeLabel = session.afternoonEnd.toString(QStringLiteral("H:mm"));
            painter.drawText(xOpen - 6,   xLabelY, 64, 18, Qt::AlignLeft    | Qt::AlignTop, openLabel);
            painter.drawText(xMid - 54,   xLabelY, 108, 18, Qt::AlignHCenter | Qt::AlignTop, session.midLabel);
            painter.drawText(xClose - 58, xLabelY, 64, 18, Qt::AlignRight   | Qt::AlignTop, closeLabel);
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
        int firstPriceX = 0;
        int lastPriceX = 0;
        for (int i = 0; i < m_points.size(); ++i) {
            const int x = (hasTradePeriodAxis && m_points.at(i).time.isValid())
                ? xOfTradePeriodTime(m_points.at(i).time)
                : (hasSessionAxis && m_points.at(i).time.isValid())
                    ? xOfSessionTime(m_points.at(i).time.time())
                    : xOfIndex(i);
            const double pricePct = toPct(m_points.at(i).price, baseline);
            if (std::isfinite(pricePct)) {
                const int yPrice = yOfPct(pricePct);
                if (!hasPricePath) {
                    pricePath.moveTo(x, yPrice);
                    hasPricePath = true;
                    firstPriceX = x;
                } else {
                    pricePath.lineTo(x, yPrice);
                }
                lastPriceX = x;
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

        const auto colorByChange = [&](double pct) {
            if (!std::isfinite(pct)) {
                return m_cfg.timelineChartTextColor;
            }
            if (pct > 0.0) {
                return m_cfg.timelineChartUpColor;
            }
            if (pct < 0.0) {
                return m_cfg.timelineChartDownColor;
            }
            return m_cfg.timelineChartTextColor;
        };

        bool hasCostProfitText = false;
        QString costProfitPrefix;
        QString costProfitPct;
        int costProfitTotalWidth = 0;
        QColor costProfitColor = m_cfg.timelineChartTextColor;
        if (std::isfinite(m_cost) && m_cost > 0.0 && std::isfinite(lastPoint.price)) {
            const double costProfitPctValue = (lastPoint.price - m_cost) / m_cost * 100.0;
            costProfitPrefix = i18n::t("popup.timeline.profit", m_resolvedLanguage) + QStringLiteral(": ");
            costProfitPct = QStringLiteral("%1%").arg(signedNumber(costProfitPctValue, 2));
            costProfitColor = colorByChange(costProfitPctValue);

            QFont normalFont = painter.font();
            QFont boldFont = normalFont;
            boldFont.setBold(true);
            const QFontMetrics normalFm(normalFont);
            const QFontMetrics boldFm(boldFont);
            costProfitTotalWidth = normalFm.horizontalAdvance(costProfitPrefix)
                + boldFm.horizontalAdvance(costProfitPct);
            hasCostProfitText = costProfitTotalWidth > 0;
        }

        if (hasPricePath) {
            QPainterPath fillPath = pricePath;
            fillPath.lineTo(lastPriceX, plot.bottom());
            fillPath.lineTo(firstPriceX, plot.bottom());
            fillPath.closeSubpath();

            QLinearGradient gradient(0, plot.top(), 0, plot.bottom());
            gradient.setColorAt(0.0, QColor(0, 180, 255, 13));
            gradient.setColorAt(0.5, QColor(0, 180, 255, 46));
            gradient.setColorAt(1.0, QColor(0, 180, 255, 102));

            painter.setPen(Qt::NoPen);
            painter.setBrush(QBrush(gradient));
            painter.drawPath(fillPath);

            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(m_cfg.timelineChartPriceLineColor, 0.8));
            painter.drawPath(pricePath);
        }

        if (hasAvgPath) {
            painter.setPen(QPen(m_cfg.timelineChartAvgLineColor, 0.8));
            painter.drawPath(avgPath);
        }

        // Draw cost line
        if (std::isfinite(m_cost) && m_cost > 0.0 && std::isfinite(baseline)) {
            const double costPct = (m_cost - baseline) / baseline * 100.0;
            const bool withinAshareLimit = !std::isfinite(ashareLimitPct) || ashareLimitPct <= 0.0
                || (costPct >= -ashareLimitPct && costPct <= ashareLimitPct);
            if (withinAshareLimit) {
                const int yCost = yOfPct(costPct);
                if (yCost >= plot.top() && yCost <= plot.bottom()) {
                    QPen costPen(m_cfg.costLineColor, 1.0, Qt::CustomDashLine);
                    costPen.setDashPattern({4.0, 3.0});
                    painter.setPen(costPen);
                    painter.drawLine(plot.left(), yCost, plot.right(), yCost);
                }
            }
        }

        if (std::isfinite(latestPct) && std::isfinite(latestChange)) {
            // Get last valid avg price for display
            double latestAvgPrice = qQNaN();
            for (int i = m_points.size() - 1; i >= 0; --i) {
                if (std::isfinite(m_points.at(i).avgPrice)) {
                    latestAvgPrice = m_points.at(i).avgPrice;
                    break;
                }
            }

            const auto priceStr = [](double v) -> QString {
                return std::isfinite(v) ? QString::number(v, 'f', 2) : QStringLiteral("--");
            };

            const QString s0 = priceStr(lastPoint.price);
            const QString s1 = QStringLiteral("%1%").arg(signedNumber(latestPct, 2));
            const QString s2 = signedNumber(latestChange, 3);
            const QString s3 = std::isfinite(latestAvgPrice) ? priceStr(latestAvgPrice) : QString();

            QFontMetrics fm(painter.font());
            const int gap = fm.horizontalAdvance(QStringLiteral("  "));
            const int w0 = fm.horizontalAdvance(s0);
            const int w1 = fm.horizontalAdvance(s1);
            const int w2 = fm.horizontalAdvance(s2);
            const int w3 = s3.isEmpty() ? 0 : fm.horizontalAdvance(s3);
            const int totalW = w0 + gap + w1 + gap + w2 + (w3 > 0 ? gap + w3 : 0);

            const int titleAdvance = fm.horizontalAdvance(m_title + QStringLiteral("  "));
            const int rightReserve = hasCostProfitText ? (costProfitTotalWidth + gap) : 0;
            const int availableWidth = qMax(0, headerRect.width() - rightReserve);
            const int contentRight = headerRect.right() - rightReserve;
            int x = (titleAdvance + totalW + 4 <= headerRect.width())
                && (titleAdvance + totalW + 4 <= availableWidth)
                ? headerRect.left() + titleAdvance
                : qMax(headerRect.left(), contentRight - totalW + 1);
            const int y = headerRect.top();
            const int h = headerRect.height();

            painter.setPen(QPen(trendColor));
            painter.drawText(x, y, w0, h, Qt::AlignLeft | Qt::AlignVCenter, s0);
            x += w0 + gap;
            painter.drawText(x, y, w1, h, Qt::AlignLeft | Qt::AlignVCenter, s1);
            x += w1 + gap;
            painter.drawText(x, y, w2, h, Qt::AlignLeft | Qt::AlignVCenter, s2);
            if (w3 > 0) {
                x += w2 + gap;
                painter.setPen(QPen(m_cfg.timelineChartAvgLineColor));
                painter.drawText(x, y, w3, h, Qt::AlignLeft | Qt::AlignVCenter, s3);
            }
        }

        if (hasCostProfitText) {
            const int y = headerRect.top();
            const int h = headerRect.height();
            const int startX = headerRect.right() - costProfitTotalWidth + 1;

            QFont normalFont = painter.font();
            QFont boldFont = normalFont;
            boldFont.setBold(true);
            const QFontMetrics normalFm(normalFont);
            const int prefixWidth = normalFm.horizontalAdvance(costProfitPrefix);

            painter.setPen(QPen(costProfitColor));
            painter.setFont(normalFont);
            painter.drawText(startX, y, prefixWidth, h, Qt::AlignLeft | Qt::AlignVCenter, costProfitPrefix);

            painter.setFont(boldFont);
            painter.drawText(
                startX + prefixWidth,
                y,
                costProfitTotalWidth - prefixWidth,
                h,
                Qt::AlignLeft | Qt::AlignVCenter,
                costProfitPct
            );
            painter.setFont(normalFont);
        }
    }

private:
    AppConfig m_cfg;
    QString m_resolvedLanguage = QStringLiteral("zh_CN");
    QString m_title;
    QString m_code;
    QString m_status;
    QVector<TimelinePoint> m_points;
    QVector<TradePeriod> m_tradePeriods;
    double m_preClose = qQNaN();
    double m_cost = qQNaN();
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
            if (!isVisible() || !isCurrentMarketTradingTimeNow()) {
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

        if (isCurrentMarketTradingTimeNow()) {
            startRefreshTimer();
        } else {
            stopRefreshTimer();
        }
    }

    void showForStock(const QString& code, const QString& name, const QRect& anchorRect, int baseWidth, double cost = qQNaN()) {
        Q_UNUSED(baseWidth);
        if (code.trimmed().isEmpty() || !isTimelineSupportedCode(code)) {
            hidePopup();
            return;
        }

        const bool changed = (m_code.compare(code, Qt::CaseInsensitive) != 0);
        m_code = code.trimmed();
        m_name = name.trimmed();
        m_cost = cost;
        if (changed) {
            m_hongKongHalfDayDate = QDate();
        }

        const int popupWidth = kTimelinePopupFixedWidth;
        const int popupHeight = kTimelinePopupFixedHeight;
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
        enforceTimelinePopupWindowLevel(this);

        if (changed) {
            fetchTimeline(true);
        }

        if (isCurrentMarketTradingTimeNow()) {
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
        m_hongKongHalfDayDate = QDate();
        hide();
    }

private:
    bool isCurrentMarketTradingTimeNow() const {
        const TimelineMarket market = timelineMarketOfCode(m_code);
        if (market == TimelineMarket::Ashare || market == TimelineMarket::Sector) {
            return isAshareTradingTimeNow();
        }
        if (market == TimelineMarket::HongKong) {
            return isHongKongTradingTimeNow(m_hongKongHalfDayDate);
        }
        if (market == TimelineMarket::Future) {
            // Futures trading sessions vary by instrument; keep refresh on while popup is visible.
            return true;
        }
        return false;
    }

    void updateHongKongHalfDayState(const QVector<TimelinePoint>& points) {
        m_hongKongHalfDayDate = QDate();
        if (timelineMarketOfCode(m_code) != TimelineMarket::HongKong || points.isEmpty()) {
            return;
        }

        bool hasAfternoon = false;
        QDate tradeDate;
        QTime lastTime;
        for (const TimelinePoint& point : points) {
            if (!point.time.isValid()) {
                continue;
            }
            tradeDate = point.time.date();
            lastTime = point.time.time();
            if (lastTime >= QTime(13, 0)) {
                hasAfternoon = true;
                break;
            }
        }

        if (hasAfternoon || !tradeDate.isValid() || lastTime < QTime(12, 0)) {
            return;
        }

        const QTimeZone bjZone("Asia/Shanghai");
        if (!bjZone.isValid()) {
            return;
        }

        const QDateTime now = QDateTime::currentDateTimeUtc().toTimeZone(bjZone);
        if (!now.isValid() || now.date() != tradeDate) {
            return;
        }

        if (now.time() >= QTime(13, 5)) {
            m_hongKongHalfDayDate = tradeDate;
        }
    }

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

    void cancelPendingReply() {
        if (!m_reply) {
            return;
        }

        QNetworkReply* pendingReply = m_reply;
        m_reply = nullptr;
        pendingReply->abort();
        pendingReply->deleteLater();
    }

    void applyTimelineResult(const QVector<TimelinePoint>& points, double preClose, const QVector<TradePeriod>& tradePeriods = {}) {
        updateHongKongHalfDayState(points);
        if (!isCurrentMarketTradingTimeNow()) {
            stopRefreshTimer();
        }

        const QString title = m_name.isEmpty()
            ? m_code
            : QStringLiteral("%1  %2").arg(m_name, m_code);
        m_chart->setSeries(title, m_code, points, preClose, tradePeriods, m_cost);
    }

    bool tryUseCachedTimeline(const QString& cacheKey, int days, bool fallbackAllowed, int token) {
        auto it = m_timelineCache.find(cacheKey);
        if (it == m_timelineCache.end()) {
            return false;
        }

        const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
        if (!it->expiresAtUtc.isValid() || it->expiresAtUtc <= nowUtc) {
            m_timelineCache.erase(it);
            return false;
        }

        const QVector<TimelinePoint> points = it->points;
        const double preClose = it->preClose;
        const QVector<TradePeriod> tradePeriods = it->tradePeriods;
        if (points.isEmpty() && days == 1 && fallbackAllowed) {
            qDebug() << "[TimelineChart] cache hit" << cacheKey << "fallback to ndays=2";
            requestTimeline(2, false, true, token);
            return true;
        }

        qDebug() << "[TimelineChart] cache hit" << cacheKey;
        applyTimelineResult(points, preClose, tradePeriods);
        return true;
    }

    void cacheTimelineResult(
        const QString& cacheKey,
        const QVector<TimelinePoint>& points,
        double preClose,
        const QVector<TradePeriod>& tradePeriods
    ) {
        TimelineCacheEntry entry;
        entry.points = points;
        entry.preClose = preClose;
        entry.tradePeriods = tradePeriods;
        entry.expiresAtUtc = QDateTime::currentDateTimeUtc().addMSecs(app_constants::kNetworkCacheTtlMs);
        m_timelineCache.insert(cacheKey, entry);
    }

    void requestTimeline(int days, bool fallbackAllowed, bool keepLastTradingDayOnly, int token) {
        const QString secId = toTimelineSecId(m_code);
        if (secId.isEmpty()) {
            m_chart->setStatusText(QStringLiteral("Unsupported code: %1").arg(m_code));
            return;
        }

        const TimelineMarket marketType = timelineMarketOfCode(m_code);

        cancelPendingReply();

        const QString cacheKey = timelineRequestCacheKey(secId, days, keepLastTradingDayOnly);
        if (tryUseCachedTimeline(cacheKey, days, fallbackAllowed, token)) {
            return;
        }

        QUrl url(QStringLiteral("https://push2delay.eastmoney.com/api/qt/stock/trends2/get"));
        QUrlQuery query;
        const QString callback = QStringLiteral("jQuery%1_%2")
            .arg(QDateTime::currentMSecsSinceEpoch() % 1000000)
            .arg(QDateTime::currentMSecsSinceEpoch());
        const QString fields1 = (marketType == TimelineMarket::Ashare)
            ? QStringLiteral("f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13,f14")
            : QStringLiteral("f1,f2,f8,f10,f14");
        const QString fields2 = (marketType == TimelineMarket::Ashare)
            ? QStringLiteral("f51,f52,f53,f54,f55,f56,f57,f58")
            : QStringLiteral("f51,f53,f56,f58");
        query.addQueryItem(QStringLiteral("cb"), callback);
        query.addQueryItem(QStringLiteral("secid"), secId);
        query.addQueryItem(QStringLiteral("ut"), QStringLiteral("fa5fd1943c7b386f172d6893dbfba10b"));
        query.addQueryItem(QStringLiteral("fields1"), fields1);
        query.addQueryItem(QStringLiteral("fields2"), fields2);
        query.addQueryItem(QStringLiteral("iscr"), QStringLiteral("0"));
        query.addQueryItem(QStringLiteral("iscca"), QStringLiteral("0"));
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
        connect(reply, &QNetworkReply::finished, this, [this, reply, token, days, fallbackAllowed, keepLastTradingDayOnly, cacheKey]() {
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
            QVector<TradePeriod> tradePeriods;
            if (!parseTimelinePayload(body, &points, &preClose, keepLastTradingDayOnly, &tradePeriods)) {
                m_chart->setStatusText(QStringLiteral("Timeline parse failed"));
                return;
            }

            cacheTimelineResult(cacheKey, points, preClose, tradePeriods);

            if (points.isEmpty() && days == 1 && fallbackAllowed) {
                requestTimeline(2, false, true, token);
                return;
            }

            applyTimelineResult(points, preClose, tradePeriods);
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
    double m_cost = qQNaN();
    QDate m_hongKongHalfDayDate;
    QHash<QString, TimelineCacheEntry> m_timelineCache;
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

    m_timelinePopup = new TimelineChartPopup(this);
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
    if (code.isEmpty() || !isTimelineSupportedCode(code)) {
        hideTimelinePopup();
        return;
    }

    const QRect visual = view->visualRect(index);
    const QPoint globalTopLeft = view->viewport()->mapToGlobal(visual.topLeft());
    const QRect globalAnchor(globalTopLeft, visual.size());

    m_timelineHoverCode = code;
    m_timelineHoverName = name;
    const double cost = m_model ? m_model->costForCode(code) : qQNaN();
    m_timelinePopup->showForStock(code, name, globalAnchor, width(), cost);
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
