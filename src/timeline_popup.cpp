#include "timeline_popup.h"

#include "app_constants.h"
#include "watchlist_utils.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
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
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace {

inline constexpr int kTimelinePopupFixedWidth = 560;
inline constexpr int kTimelinePopupFixedHeight = 360;
inline constexpr int kTimelinePopupScreenMarginPx = 12;

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
#endif

static void enforceTimelinePopupWindowLevel(const QWidget* widget) {
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

TimelineSession timelineSessionForMarket(TimelineMarket market) {
    if (market == TimelineMarket::Ashare || market == TimelineMarket::Sector) {
        return {QTime(9, 30), QTime(11, 30), QTime(13, 0), QTime(15, 0), QStringLiteral("11:30/13:00")};
    }
    if (market == TimelineMarket::HongKong) {
        return {QTime(9, 30), QTime(12, 0), QTime(13, 0), QTime(16, 0), QStringLiteral("12:00/13:00")};
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
                    return QString::number(marketValue) + QStringLiteral(".") + symbol.toUpper();
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
    const QDateTime now = QDateTime::currentDateTimeUtc().toTimeZone(bjZone);
    const int dayOfWeek = now.date().dayOfWeek();
    if (dayOfWeek < 1 || dayOfWeek > 5) {
        return false;
    }
    const QTime time = now.time();
    return (time >= QTime(9, 30) && time <= QTime(11, 30))
        || (time >= QTime(13, 0) && time <= QTime(15, 0));
}

bool isHongKongTradingTimeNow(const QDate& halfDayDate = QDate()) {
    const QTimeZone bjZone("Asia/Shanghai");
    const QDateTime now = QDateTime::currentDateTimeUtc().toTimeZone(bjZone);
    const int dayOfWeek = now.date().dayOfWeek();
    if (dayOfWeek < 1 || dayOfWeek > 5) {
        return false;
    }
    const bool isHalfDayToday = halfDayDate.isValid() && halfDayDate == now.date();
    const QTime time = now.time();
    return (time >= QTime(9, 30) && time <= QTime(12, 0))
        || (!isHalfDayToday && time >= QTime(13, 0) && time <= QTime(16, 0));
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

class TimelineChartWidget : public QWidget {
public:
    explicit TimelineChartWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(280, 180);
    }

    void setConfig(const AppConfig& cfg) {
        m_cfg = cfg;
        update();
    }

    void setSeries(
        const QString& title,
        const QString& code,
        const QVector<TimelinePoint>& points,
        double preClose,
        const QVector<TradePeriod>& tradePeriods = {},
        double cost = qQNaN()
    ) {
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
        QRect plot(
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
        const double ashareLimitPct = isAshareStock ? fixedRangeLimitPctForAshareStock(m_code) : qQNaN();
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

        const auto yOfPct = [&](double pct) {
            return plot.bottom() - qRound(((pct - yMinPct) / ySpanPct) * static_cast<double>(plot.height()));
        };
        const auto xOfIndex = [&](int index) {
            if (m_points.size() <= 1) {
                return plot.left();
            }
            const double t = static_cast<double>(index) / static_cast<double>(m_points.size() - 1);
            return plot.left() + qRound(t * static_cast<double>(plot.width()));
        };
        const auto xOfSessionTime = [&](const QTime& t) {
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
                return xLeft + qRound((static_cast<double>(elapsed) / morningSpan) * (xMid - xLeft));
            }
            if (t < session.afternoonStart) {
                return xMid;
            }
            if (t < session.afternoonEnd) {
                const int afternoonSpan = qMax(1, session.afternoonStart.secsTo(session.afternoonEnd));
                const int elapsed = qBound(0, session.afternoonStart.secsTo(t), afternoonSpan);
                return xMid + qRound((static_cast<double>(elapsed) / afternoonSpan) * (xRight - xMid));
            }
            return xRight;
        };

        qint64 tradePeriodTotalSecs = 0;
        for (const TradePeriod& p : m_tradePeriods) {
            tradePeriodTotalSecs += qMax(static_cast<qint64>(0), p.begin.secsTo(p.end));
        }
        const auto xOfTradePeriodTime = [&](const QDateTime& dt) {
            if (tradePeriodTotalSecs <= 0) {
                return plot.left();
            }
            qint64 cumSecs = 0;
            for (const TradePeriod& p : m_tradePeriods) {
                const qint64 periodSecs = qMax(static_cast<qint64>(0), p.begin.secsTo(p.end));
                if (dt <= p.begin) {
                    return plot.left() + qRound(static_cast<double>(cumSecs) / tradePeriodTotalSecs * plot.width());
                }
                if (dt <= p.end) {
                    const qint64 elapsed = qBound(static_cast<qint64>(0), p.begin.secsTo(dt), periodSecs);
                    return plot.left() + qRound(static_cast<double>(cumSecs + elapsed) / tradePeriodTotalSecs * plot.width());
                }
                cumSecs += periodSecs;
            }
            return plot.right();
        };

        bool hasHongKongAfternoonMarker = false;
        int hongKongAfternoonMarkerX = 0;
        if (hasTradePeriodAxis && market == TimelineMarket::HongKong) {
            QDate tradeDate;
            for (auto it = m_points.crbegin(); it != m_points.crend(); ++it) {
                if (it->time.isValid()) {
                    tradeDate = it->time.date();
                    break;
                }
            }
            if (tradeDate.isValid()) {
                const QDateTime markerTime(tradeDate, QTime(15, 0));
                for (const TradePeriod& p : m_tradePeriods) {
                    if (p.begin <= markerTime && markerTime < p.end) {
                        hasHongKongAfternoonMarker = true;
                        hongKongAfternoonMarkerX = xOfTradePeriodTime(markerTime);
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
        } else if (hasSessionAxis) {
            painter.setPen(gridPen);
            const QTime midMorning = session.morningStart.addSecs(session.morningStart.secsTo(session.morningEnd) / 2);
            const QTime midAfternoon = session.afternoonStart.addSecs(session.afternoonStart.secsTo(session.afternoonEnd) / 2);
            const QTime vTimes[] = {session.morningStart, midMorning, session.morningEnd, midAfternoon, session.afternoonEnd};
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
            painter.drawText(r.left(), y - 10, leftMargin - 6, 20, Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("%1%").arg(QString::number(pct, 'f', 2)));
        }

        const int xLabelY = plot.bottom() + 8;
        const QDate firstDate = m_tradePeriods.isEmpty() ? QDate() : m_tradePeriods.first().begin.date();
        const auto tradePeriodTimeLabel = [&](const QDateTime& dt) {
            return (dt.date() == firstDate) ? dt.toString(QStringLiteral("H:mm")) : dt.toString(QStringLiteral("M/d H:mm"));
        };
        if (hasTradePeriodAxis) {
            const QString openLabel = tradePeriodTimeLabel(m_tradePeriods.first().begin);
            const QString closeLabel = tradePeriodTimeLabel(m_tradePeriods.last().end);
            painter.drawText(plot.left() - 6, xLabelY, 64, 18, Qt::AlignLeft | Qt::AlignTop, openLabel);
            painter.drawText(plot.right() - 58, xLabelY, 64, 18, Qt::AlignRight | Qt::AlignTop, closeLabel);
            for (int i = 0; i < m_tradePeriods.size() - 1; ++i) {
                const TradePeriod& cur = m_tradePeriods.at(i);
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
        } else if (hasSessionAxis) {
            const int xOpen = xOfSessionTime(session.morningStart);
            const int xMid = xOfSessionTime(session.morningEnd);
            const int xClose = xOfSessionTime(session.afternoonEnd);
            painter.drawText(xOpen - 6, xLabelY, 64, 18, Qt::AlignLeft | Qt::AlignTop, session.morningStart.toString(QStringLiteral("H:mm")));
            painter.drawText(xMid - 54, xLabelY, 108, 18, Qt::AlignHCenter | Qt::AlignTop, session.midLabel);
            painter.drawText(xClose - 58, xLabelY, 64, 18, Qt::AlignRight | Qt::AlignTop, session.afternoonEnd.toString(QStringLiteral("H:mm")));
        } else {
            const int midIndex = m_points.size() / 2;
            const QString leftTime = m_points.first().time.isValid() ? m_points.first().time.toString(QStringLiteral("HH:mm")) : QStringLiteral("--:--");
            const QString midTime = m_points.at(midIndex).time.isValid() ? m_points.at(midIndex).time.toString(QStringLiteral("HH:mm")) : QStringLiteral("--:--");
            const QString rightTime = m_points.last().time.isValid() ? m_points.last().time.toString(QStringLiteral("HH:mm")) : QStringLiteral("--:--");
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
        const double latestChange = std::isfinite(lastPoint.price) ? (lastPoint.price - baseline) : qQNaN();

        QColor trendColor = m_cfg.timelineChartPriceLineColor;
        if (std::isfinite(latestPct)) {
            if (latestPct > 0.0) {
                trendColor = m_cfg.timelineChartUpColor;
            } else if (latestPct < 0.0) {
                trendColor = m_cfg.timelineChartDownColor;
            }
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

        // Draw cost line if cost is set and within chart range
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
            double latestAvgPrice = qQNaN();
            for (int i = m_points.size() - 1; i >= 0; --i) {
                if (std::isfinite(m_points.at(i).avgPrice)) {
                    latestAvgPrice = m_points.at(i).avgPrice;
                    break;
                }
            }
            const auto priceStr = [](double v) { return std::isfinite(v) ? QString::number(v, 'f', 2) : QStringLiteral("--"); };
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
            int x = (titleAdvance + totalW + 4 <= headerRect.width())
                ? headerRect.left() + titleAdvance
                : qMax(headerRect.left(), headerRect.right() - totalW);
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
    }

private:
    AppConfig m_cfg;
    QString m_title;
    QString m_code;
    QString m_status;
    QVector<TimelinePoint> m_points;
    QVector<TradePeriod> m_tradePeriods;
    double m_preClose = qQNaN();
    double m_cost = qQNaN();
};

} // namespace

bool isTimelinePopupSupportedCode(const QString& rawCode) {
    return timelineMarketOfCode(rawCode) != TimelineMarket::Unknown;
}

class SharedTimelineChartPopupPrivate : public QWidget {
public:
    explicit SharedTimelineChartPopupPrivate(QWidget* parentWindow)
        : QWidget(nullptr)
        , m_parentWindow(parentWindow) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
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
        m_chart->setConfig(cfg);
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
        if (code.trimmed().isEmpty() || !isTimelinePopupSupportedCode(code)) {
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

        QRect targetRect(anchorRect.topRight() + QPoint(12, 0), size());
        QRect screenRect;
        for (QScreen* screen : QGuiApplication::screens()) {
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
        const QDateTime now = QDateTime::currentDateTimeUtc().toTimeZone(bjZone);
        if (!now.isValid() || now.date() != tradeDate) {
            return;
        }
        if (now.time() >= QTime(13, 5)) {
            m_hongKongHalfDayDate = tradeDate;
        }
    }

    void startRefreshTimer() {
        const int intervalMs = qBound(10, m_cfg.timelineChartRefreshSecs, 3600) * 1000;
        m_refreshTimer->start(intervalMs);
    }

    void stopRefreshTimer() {
        m_refreshTimer->stop();
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
        const QString title = m_name.isEmpty() ? m_code : QStringLiteral("%1  %2").arg(m_name, m_code);
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
            requestTimeline(2, false, true, token);
            return true;
        }
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

            const QString error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
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
        const QString title = m_name.isEmpty() ? m_code : QStringLiteral("%1  %2").arg(m_name, m_code);
        m_chart->setStatusText(QStringLiteral("Loading %1 ...").arg(title));
        requestTimeline(1, fallbackAllowed, false, m_requestToken);
    }

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

SharedTimelineChartPopup::SharedTimelineChartPopup(QWidget* parent)
    : QWidget(parent)
    , d(new SharedTimelineChartPopupPrivate(parent)) {}

SharedTimelineChartPopup::~SharedTimelineChartPopup() {
    delete d;
    d = nullptr;
}

void SharedTimelineChartPopup::applyConfig(const AppConfig& cfg) {
    if (d) {
        d->applyConfig(cfg);
    }
}

void SharedTimelineChartPopup::showForStock(
    const QString& code,
    const QString& name,
    const QRect& anchorRect,
    int baseWidth,
    double cost
) {
    if (d) {
        d->showForStock(code, name, anchorRect, baseWidth, cost);
    }
}

void SharedTimelineChartPopup::hidePopup() {
    if (d) {
        d->hidePopup();
    }
}
