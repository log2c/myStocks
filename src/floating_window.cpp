#include "floating_window.h"

#include "app_constants.h"
#include "i18n.h"
#include "quote_provider.h"
#include "watchlist_utils.h"

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

    return sampleTs.toString(QStringLiteral("HH:mm"));
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
inline constexpr int kTimelinePopupMinWidth = 560;
inline constexpr int kTimelinePopupFixedHeight = 360;
inline constexpr int kTimelinePopupScreenMarginPx = 12;

struct TimelinePoint {
    QDateTime time;
    double price = qQNaN();
    double avgPrice = qQNaN();
    double volume = qQNaN();
    double amount = qQNaN();
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

        const TimelineMarket market = timelineMarketOfCode(m_code);
        const TimelineSession session = timelineSessionForMarket(market);
        const bool hasSessionAxis = hasValidTimelineSession(session);
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

        const QPen gridPen(m_cfg.timelineChartGridColor, 1.0, Qt::DashLine);
        painter.setPen(gridPen);
        for (int i = 0; i <= 4; ++i) {
            const int y = plot.top() + (plot.height() * i) / 4;
            painter.drawLine(plot.left(), y, plot.right(), y);
        }
        if (hasSessionAxis) {
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
            for (int i = 0; i <= 4; ++i) {
                const int x = plot.left() + (plot.width() * i) / 4;
                painter.drawLine(x, plot.top(), x, plot.bottom());
            }
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
        if (hasSessionAxis) {
            const int xOpen  = xOfSessionTime(session.morningStart);
            const int xMid   = xOfSessionTime(session.morningEnd);
            const int xClose = xOfSessionTime(session.afternoonEnd);
            const QString openLabel = session.morningStart.toString(QStringLiteral("H:mm"));
            const QString closeLabel = session.afternoonEnd.toString(QStringLiteral("H:mm"));
            painter.setPen(QPen(m_cfg.timelineChartTextColor));
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
        for (int i = 0; i < m_points.size(); ++i) {
            const int x = (hasSessionAxis && m_points.at(i).time.isValid())
                ? xOfSessionTime(m_points.at(i).time.time())
                : xOfIndex(i);
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

    void showForStock(const QString& code, const QString& name, const QRect& anchorRect, int baseWidth) {
        if (code.trimmed().isEmpty() || !isTimelineSupportedCode(code)) {
            hidePopup();
            return;
        }

        const bool changed = (m_code.compare(code, Qt::CaseInsensitive) != 0);
        m_code = code.trimmed();
        m_name = name.trimmed();
        if (changed) {
            m_hongKongHalfDayDate = QDate();
        }

        const int popupWidth = qMax(
            kTimelinePopupMinWidth,
            qRound(static_cast<double>(qMax(1, baseWidth)) * kTimelinePopupWidthScale)
        );
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

    void applyTimelineResult(const QVector<TimelinePoint>& points, double preClose) {
        updateHongKongHalfDayState(points);
        if (!isCurrentMarketTradingTimeNow()) {
            stopRefreshTimer();
        }

        const QString title = m_name.isEmpty()
            ? m_code
            : QStringLiteral("%1  %2").arg(m_name, m_code);
        m_chart->setSeries(title, m_code, points, preClose);
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
        if (points.isEmpty() && days == 1 && fallbackAllowed) {
            qDebug() << "[TimelineChart] cache hit" << cacheKey << "fallback to ndays=2";
            requestTimeline(2, false, true, token);
            return true;
        }

        qDebug() << "[TimelineChart] cache hit" << cacheKey;
        applyTimelineResult(points, preClose);
        return true;
    }

    void cacheTimelineResult(
        const QString& cacheKey,
        const QVector<TimelinePoint>& points,
        double preClose
    ) {
        TimelineCacheEntry entry;
        entry.points = points;
        entry.preClose = preClose;
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
            ? QStringLiteral("f1,f2,f3,f4,f5,f6,f7,f8,f9,f10,f11,f12,f13")
            : QStringLiteral("f1,f2,f8,f10");
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
            if (!parseTimelinePayload(body, &points, &preClose, keepLastTradingDayOnly)) {
                m_chart->setStatusText(QStringLiteral("Timeline parse failed"));
                return;
            }

            cacheTimelineResult(cacheKey, points, preClose);

            if (points.isEmpty() && days == 1 && fallbackAllowed) {
                requestTimeline(2, false, true, token);
                return;
            }

            applyTimelineResult(points, preClose);
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
    QDate m_hongKongHalfDayDate;
    QHash<QString, TimelineCacheEntry> m_timelineCache;
    int m_requestToken = 0;
};

class MarketBreadthDetailPopup : public QWidget {
public:
    explicit MarketBreadthDetailPopup(QWidget* parent = nullptr)
        : QWidget(nullptr)
        , m_parentWindow(parent) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setMouseTracking(true);
        resize(740, 550);

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
        ensureHotRankProviders();
        if (m_hotRankRiseProvider) {
            m_hotRankRiseProvider->applyConfig(m_cfg);
        }
        if (m_hotRankFallProvider) {
            m_hotRankFallProvider->applyConfig(m_cfg);
        }
        setFont(effectiveFloatingWindowFont(cfg, font()));
        update();
    }

    void setLanguage(const QString& language) {
        m_language = i18n::resolveLanguage(language);
        update();
    }

    void setForceRefreshCallback(std::function<void()> callback) {
        m_forceRefreshCallback = callback;
    }

    void showForSnapshot(
        const MarketBreadthSnapshot& snapshot,
        const QVector<HotRankItem>& hotSectors,
        const QVector<HotRankItem>& hotConcepts,
        const QRect& anchorRect,
        int baseWidth
    ) {
        Q_UNUSED(baseWidth);

        m_snapshot = snapshot;
        m_hotSectors = hotSectors;
        m_hotConcepts = hotConcepts;
        m_hotRankTabMode = HotRankTabMode::Auto;
        m_pinnedFromTray = false;
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
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

        const int popupWidth = 740;
        const int popupHeight = 550;
        resize(popupWidth, popupHeight);

        QRect targetRect(anchorRect.topRight() + QPoint(12, 0), size());
        QRect screenRect;
        const QList<QScreen*> screens = QGuiApplication::screens();
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
        requestHotRankData(false);
        requestHotRankData(true);
        update();
    }

    void showCenteredForSnapshot(
        const MarketBreadthSnapshot& snapshot,
        const QVector<HotRankItem>& hotSectors,
        const QVector<HotRankItem>& hotConcepts,
        const QRect& referenceRect
    ) {
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

        const QSize popupSize(740, 550);
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
            if (targetRect.left() < screenRect.left() + kTimelinePopupScreenMarginPx) {
                targetRect.moveLeft(screenRect.left() + kTimelinePopupScreenMarginPx);
            }
            if (targetRect.top() < screenRect.top() + kTimelinePopupScreenMarginPx) {
                targetRect.moveTop(screenRect.top() + kTimelinePopupScreenMarginPx);
            }
            if (targetRect.right() > screenRect.right() - kTimelinePopupScreenMarginPx) {
                targetRect.moveRight(screenRect.right() - kTimelinePopupScreenMarginPx);
            }
            if (targetRect.bottom() > screenRect.bottom() - kTimelinePopupScreenMarginPx) {
                targetRect.moveBottom(screenRect.bottom() - kTimelinePopupScreenMarginPx);
            }
        }

        setGeometry(targetRect);
        if (!isVisible()) {
            show();
        }
        raise();
        requestHotRankData(false);
        requestHotRankData(true);
        update();
    }

    void refreshSnapshot(
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
        update();
    }

    bool isPinnedFromTray() const {
        return m_pinnedFromTray;
    }

    void hidePopup() {
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
        m_hotSectorTabRect = QRect();
        m_hotConceptTabRect = QRect();
        hide();
    }

protected:
    void mouseMoveEvent(QMouseEvent* event) override {
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

    void leaveEvent(QEvent* event) override {
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

    void mousePressEvent(QMouseEvent* event) override {
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

    void mouseReleaseEvent(QMouseEvent* event) override {
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
                requestHotRankData(false, true, true);
                requestHotRankData(false, false, true);
                requestHotRankData(true, true, true);
                requestHotRankData(true, false, true);
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

    void mouseDoubleClickEvent(QMouseEvent* event) override {
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

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);

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
                const int rotateDegrees = refreshAnimating
                    ? qRound(refreshAnimProgress * 320.0)
                    : 0;
                popupPainter.drawArc(arcRect, (38 + rotateDegrees) * 16, 286 * 16);

                const QPoint arrowTip(center.x() + radius - 1, center.y() - radius / 2 - 1);
                const int arrowArm = qMax(3, radius / 2 + 1);
                popupPainter.drawLine(arrowTip, arrowTip + QPoint(-arrowArm, -1));
                popupPainter.drawLine(arrowTip, arrowTip + QPoint(-1, arrowArm));
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

            const QRect bodyRect = content.adjusted(0, 36, 0, 0);
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
            const bool hasSectorData = !m_hotSectors.isEmpty();
            const bool hasConceptData = !m_hotConcepts.isEmpty();
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
            const QVector<HotRankItem>& risingSource = useConceptData
                ? (!m_hotConceptsRise.isEmpty() ? m_hotConceptsRise : m_hotConcepts)
                : (!m_hotSectorsRise.isEmpty() ? m_hotSectorsRise : m_hotSectors);
            const QVector<HotRankItem>& fallingSource = useConceptData
                ? (!m_hotConceptsFall.isEmpty() ? m_hotConceptsFall : m_hotConcepts)
                : (!m_hotSectorsFall.isEmpty() ? m_hotSectorsFall : m_hotSectors);

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
            risingItems.reserve(risingSource.size());
            fallingItems.reserve(fallingSource.size());
            for (const HotRankItem& item : risingSource) {
                if (!std::isfinite(item.pct)) {
                    continue;
                }
                if (item.pct > 0.0) {
                    risingItems.push_back(item);
                }
            }
            for (const HotRankItem& item : fallingSource) {
                if (!std::isfinite(item.pct)) {
                    continue;
                }
                if (item.pct < 0.0) {
                    fallingItems.push_back(item);
                }
            }

            std::sort(risingItems.begin(), risingItems.end(), [](const HotRankItem& lhs, const HotRankItem& rhs) {
                return lhs.pct > rhs.pct;
            });
            std::sort(fallingItems.begin(), fallingItems.end(), [](const HotRankItem& lhs, const HotRankItem& rhs) {
                return lhs.pct < rhs.pct;
            });

            if (risingItems.size() > 4) {
                risingItems.resize(4);
            }
            if (fallingItems.size() > 4) {
                fallingItems.resize(4);
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
                                      const QVector<HotRankItem>& sortedItems) {
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

                const int visibleRows = qMin(4, sortedItems.size());

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

            drawRankColumn(
                riseSectionRect,
                risingItems
            );
            drawRankColumn(
                fallSectionRect,
                fallingItems
            );

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

private:
    enum class HotRankTabMode {
        Auto = 0,
        Sector = 1,
        Concept = 2,
    };

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

    void ensureHotRankProviders() {
        if (m_hotRankRiseProvider && m_hotRankFallProvider) {
            return;
        }

        if (!m_hotRankRiseProvider) {
            m_hotRankRiseProvider = new EastMoneyHotRankProvider(this);
            m_hotRankRiseProvider->applyConfig(m_cfg);

            connect(
                m_hotRankRiseProvider,
                &EastMoneyHotRankProvider::hotSectorsReady,
                this,
                [this](const QVector<HotRankItem>& items) {
                    m_hotSectorsRise = items;
                    update();
                }
            );
            connect(
                m_hotRankRiseProvider,
                &EastMoneyHotRankProvider::hotConceptsReady,
                this,
                [this](const QVector<HotRankItem>& items) {
                    m_hotConceptsRise = items;
                    update();
                }
            );
            connect(
                m_hotRankRiseProvider,
                &EastMoneyHotRankProvider::error,
                this,
                [this](const QString& message) {
                    const QString trimmed = message.trimmed();
                    if (trimmed.isEmpty() || trimmed == m_lastHotRankErrorRise) {
                        return;
                    }
                    m_lastHotRankErrorRise = trimmed;
                    qInfo() << "[MarketBreadthPopup] hot rank rise request error:" << trimmed;
                }
            );
        }

        if (!m_hotRankFallProvider) {
            m_hotRankFallProvider = new EastMoneyHotRankProvider(this);
            m_hotRankFallProvider->applyConfig(m_cfg);

            connect(
                m_hotRankFallProvider,
                &EastMoneyHotRankProvider::hotSectorsReady,
                this,
                [this](const QVector<HotRankItem>& items) {
                    m_hotSectorsFall = items;
                    update();
                }
            );
            connect(
                m_hotRankFallProvider,
                &EastMoneyHotRankProvider::hotConceptsReady,
                this,
                [this](const QVector<HotRankItem>& items) {
                    m_hotConceptsFall = items;
                    update();
                }
            );
            connect(
                m_hotRankFallProvider,
                &EastMoneyHotRankProvider::error,
                this,
                [this](const QString& message) {
                    const QString trimmed = message.trimmed();
                    if (trimmed.isEmpty() || trimmed == m_lastHotRankErrorFall) {
                        return;
                    }
                    m_lastHotRankErrorFall = trimmed;
                    qInfo() << "[MarketBreadthPopup] hot rank fall request error:" << trimmed;
                }
            );
        }
    }

    int popupHotRankLimit(bool concept) const {
        Q_UNUSED(concept);
        return 4;
    }

    void requestHotRankData(bool concept) {
        requestHotRankData(concept, true, false);
        requestHotRankData(concept, false, false);
    }

    void requestHotRankData(bool concept, bool rising, bool forceRefresh = false) {
        ensureHotRankProviders();
        EastMoneyHotRankProvider* provider = rising ? m_hotRankRiseProvider : m_hotRankFallProvider;
        if (!provider) {
            return;
        }

        const int limit = popupHotRankLimit(concept);
        const QString order = rising ? QStringLiteral("desc") : QStringLiteral("asc");
        if (concept) {
            provider->fetchHotConcepts(limit, QStringLiteral("pct"), order, forceRefresh);
        } else {
            provider->fetchHotSectors(limit, QStringLiteral("pct"), order, forceRefresh);
        }
    }

    QWidget* m_parentWindow = nullptr;
    AppConfig m_cfg;
    QString m_language = QStringLiteral("en_US");
    MarketBreadthSnapshot m_snapshot;
    QVector<HotRankItem> m_hotSectors;
    QVector<HotRankItem> m_hotConcepts;
    QVector<HotRankItem> m_hotSectorsRise;
    QVector<HotRankItem> m_hotSectorsFall;
    QVector<HotRankItem> m_hotConceptsRise;
    QVector<HotRankItem> m_hotConceptsFall;
    EastMoneyHotRankProvider* m_hotRankRiseProvider = nullptr;
    EastMoneyHotRankProvider* m_hotRankFallProvider = nullptr;
    QString m_lastHotRankErrorRise;
    QString m_lastHotRankErrorFall;
    HotRankTabMode m_hotRankTabMode = HotRankTabMode::Auto;
    bool m_pinnedFromTray = false;
    QRect m_closeButtonRect;
    QRect m_refreshButtonRect;
    QRect m_hotSectorTabRect;
    QRect m_hotConceptTabRect;
    bool m_closeButtonHovered = false;
    bool m_closeButtonPressed = false;
    bool m_refreshButtonHovered = false;
    bool m_refreshButtonPressed = false;
    bool m_hotSectorTabHovered = false;
    bool m_hotConceptTabHovered = false;
    int m_pressedHotTab = 0;
    bool m_dragging = false;
    QPoint m_dragOffset;
    QTimer* m_refreshFeedbackTimer = nullptr;
    qint64 m_refreshFeedbackStartedMs = 0;
    qint64 m_refreshFeedbackUntilMs = 0;
    std::function<void()> m_forceRefreshCallback;
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

    m_marketBreadthDetailShowTimer = new QTimer(this);
    m_marketBreadthDetailShowTimer->setSingleShot(true);
    connect(m_marketBreadthDetailShowTimer, &QTimer::timeout, this, [this]() {
        showMarketBreadthDetailPopup();
    });

    m_marketBreadthDetailHideTimer = new QTimer(this);
    m_marketBreadthDetailHideTimer->setSingleShot(true);
    connect(m_marketBreadthDetailHideTimer, &QTimer::timeout, this, [this]() {
        hideMarketBreadthDetailPopup(true);
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
    m_marketBreadthDetailPopup = new MarketBreadthDetailPopup(this);
    m_marketBreadthDetailPopup->applyConfig(m_cfg);
    m_marketBreadthDetailPopup->setLanguage(m_model ? m_model->language() : QStringLiteral("en_US"));
    m_marketBreadthDetailPopup->setForceRefreshCallback([this]() {
        emit forceRefreshRequested();
    });
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
        hideMarketBreadthDetailPopup(true);
        return;
    }

    if (!m_cfg.hoverReadingEnabled) {
        if (m_hoverTimer) {
            m_hoverTimer->stop();
        }
        setHoverReadingActive(false, animated);
        if (m_cfg.mousePassthroughEnabled) {
            hideTimelinePopup();
            hideMarketBreadthDetailPopup(true);
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
    hideMarketBreadthDetailPopup(true);
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

bool FloatingWindow::canShowMarketBreadthDetailPopup() const {
    if (!m_cfg.marketBreadthEnabled || !m_marketBreadthDetailPopup || !m_model) {
        return false;
    }
    if (!isVisible()) {
        return false;
    }

    const MarketBreadthSnapshot snapshot = m_model->marketBreadthSnapshot();
    if (!snapshot.breadthValid || !snapshot.turnoverValid) {
        return false;
    }
    if ((snapshot.upCount + snapshot.flatCount + snapshot.downCount) <= 0) {
        return false;
    }

    if (m_cfg.mousePassthroughEnabled) {
        return m_hoverReadingActive;
    }

    return true;
}

void FloatingWindow::updateHoverPopupsForViewport(const QPoint& viewportPos) {
    if (!m_table || !m_model) {
        return;
    }

    const QModelIndex index = m_table->indexAt(viewportPos);
    if (index.isValid() && m_model->rowKind(index.row()) == QuoteModel::RowKindMarketBreadth) {
        hideTimelinePopup();
        updateMarketBreadthDetailPopupForHover(viewportPos);
        return;
    }

    hideMarketBreadthDetailPopup();
    updateTimelinePopupForHover(viewportPos);
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
    if (code.isEmpty() || !isTimelineSupportedCode(code)) {
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

void FloatingWindow::toggleMarketBreadthDetailPopupFromTray() {
    if (!m_marketBreadthDetailPopup || !m_model) {
        return;
    }

    if (m_marketBreadthDetailPopup->isVisible() && m_marketBreadthDetailPopup->isPinnedFromTray()) {
        m_marketBreadthDetailPopup->hidePopup();
        return;
    }

    if (m_marketBreadthDetailShowTimer) {
        m_marketBreadthDetailShowTimer->stop();
    }
    if (m_marketBreadthDetailHideTimer) {
        m_marketBreadthDetailHideTimer->stop();
    }

    m_marketBreadthDetailHoverPending = false;
    m_marketBreadthDetailPopup->setLanguage(m_model->language());
    m_marketBreadthDetailPopup->showCenteredForSnapshot(
        m_model->marketBreadthSnapshot(),
        m_model->hotSectors(),
        m_model->hotConcepts(),
        frameGeometry()
    );
}

void FloatingWindow::updateMarketBreadthDetailPopupForHover(const QPoint& viewportPos) {
    if (!m_table || !m_model) {
        return;
    }

    if (m_marketBreadthDetailPopup && m_marketBreadthDetailPopup->isPinnedFromTray()) {
        return;
    }

    const QModelIndex index = m_table->indexAt(viewportPos);
    if (!index.isValid() || m_model->rowKind(index.row()) != QuoteModel::RowKindMarketBreadth) {
        hideMarketBreadthDetailPopup();
        return;
    }

    if (!canShowMarketBreadthDetailPopup()) {
        hideMarketBreadthDetailPopup(true);
        return;
    }

    const QRect visual = m_table->visualRect(index);
    m_marketBreadthDetailAnchorRect = QRect(
        m_table->viewport()->mapToGlobal(visual.topLeft()),
        visual.size()
    );
    m_marketBreadthDetailHoverPending = true;

    if (m_marketBreadthDetailHideTimer) {
        m_marketBreadthDetailHideTimer->stop();
    }

    if (m_marketBreadthDetailPopup && m_marketBreadthDetailPopup->isVisible()) {
        showMarketBreadthDetailPopup();
        return;
    }

    if (m_marketBreadthDetailShowTimer && !m_marketBreadthDetailShowTimer->isActive()) {
        m_marketBreadthDetailShowTimer->start(180);
    }
}

void FloatingWindow::showMarketBreadthDetailPopup() {
    if (!m_marketBreadthDetailPopup || !m_model) {
        return;
    }
    if (m_marketBreadthDetailPopup->isPinnedFromTray()) {
        return;
    }
    if (!m_marketBreadthDetailHoverPending || !canShowMarketBreadthDetailPopup()) {
        hideMarketBreadthDetailPopup(true);
        return;
    }

    m_marketBreadthDetailPopup->setLanguage(m_model->language());
    m_marketBreadthDetailPopup->showForSnapshot(
        m_model->marketBreadthSnapshot(),
        m_model->hotSectors(),
        m_model->hotConcepts(),
        m_marketBreadthDetailAnchorRect,
        width()
    );
}

void FloatingWindow::refreshMarketBreadthDetailPopup() {
    if (!m_marketBreadthDetailPopup || !m_marketBreadthDetailPopup->isVisible()) {
        return;
    }

    if (m_marketBreadthDetailPopup->isPinnedFromTray()) {
        m_marketBreadthDetailPopup->setLanguage(m_model ? m_model->language() : QStringLiteral("en_US"));
        m_marketBreadthDetailPopup->refreshSnapshot(
            m_model ? m_model->marketBreadthSnapshot() : MarketBreadthSnapshot{},
            m_model ? m_model->hotSectors() : QVector<HotRankItem>{},
            m_model ? m_model->hotConcepts() : QVector<HotRankItem>{}
        );
        return;
    }

    if (!m_marketBreadthDetailHoverPending || !canShowMarketBreadthDetailPopup()) {
        hideMarketBreadthDetailPopup(true);
        return;
    }
    showMarketBreadthDetailPopup();
}

void FloatingWindow::hideMarketBreadthDetailPopup(bool immediate) {
    if (m_marketBreadthDetailPopup && m_marketBreadthDetailPopup->isPinnedFromTray()) {
        return;
    }

    m_marketBreadthDetailHoverPending = false;
    if (m_marketBreadthDetailShowTimer) {
        m_marketBreadthDetailShowTimer->stop();
    }
    if (!m_marketBreadthDetailPopup) {
        return;
    }

    if (immediate) {
        if (m_marketBreadthDetailHideTimer) {
            m_marketBreadthDetailHideTimer->stop();
        }
        m_marketBreadthDetailPopup->hidePopup();
        return;
    }

    if (m_marketBreadthDetailPopup->isVisible() && m_marketBreadthDetailHideTimer) {
        m_marketBreadthDetailHideTimer->start(110);
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
        hideMarketBreadthDetailPopup(true);
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
                updateHoverPopupsForViewport(hoverPos);
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
            hideMarketBreadthDetailPopup(true);
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
                updateHoverPopupsForViewport(mouseEvent->position().toPoint());
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
    if (m_marketBreadthDetailPopup) {
        m_marketBreadthDetailPopup->applyConfig(m_cfg);
        m_marketBreadthDetailPopup->setLanguage(m_model ? m_model->language() : QStringLiteral("en_US"));
        if (!m_cfg.marketBreadthEnabled) {
            m_marketBreadthDetailPopup->hidePopup();
            hideMarketBreadthDetailPopup(true);
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
        hideMarketBreadthDetailPopup(true);
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
    hideMarketBreadthDetailPopup(true);
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
    const QVector<int> columnOrder = watchlist_utils::normalizedColumnOrder(m_cfg.columnOrder);
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
