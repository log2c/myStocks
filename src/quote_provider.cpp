#include "quote_provider.h"

#include "app_constants.h"
#include "network_logger.h"
#include "network_utils.h"
#include "watchlist_utils.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QDebug>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

struct SharedHotRankCacheEntry {
    QVector<HotRankItem> items;
    QString requestKey;
    qint64 expiresAtMs = 0;
    bool valid = false;
};

SharedHotRankCacheEntry& sharedHotRankCache(bool concept) {
    static SharedHotRankCacheEntry sectorCache;
    static SharedHotRankCacheEntry conceptCache;
    return concept ? conceptCache : sectorCache;
}

SharedHotRankCacheEntry& sharedStockHeatCache(const QString& periodType) {
    static SharedHotRankCacheEntry hourCache;
    static SharedHotRankCacheEntry dayCache;
    return periodType == QLatin1String("day") ? dayCache : hourCache;
}

namespace headers {

inline constexpr auto kUserAgent = "User-Agent";
inline constexpr auto kReferer = "Referer";
inline constexpr auto kAccept = "Accept";
inline constexpr auto kOrigin = "Origin";
inline constexpr auto kConnection = "Connection";
inline constexpr auto kContentType = "Content-Type";

inline constexpr auto kEastMoneyReferer = "https://quote.eastmoney.com/";
inline constexpr auto kTonghuashunOrigin = "https://52etf.site";
inline constexpr auto kTonghuashunReferer = "https://52etf.site/";
inline constexpr auto kJsonContentType = "application/json; charset=UTF-8";

} // namespace headers

bool isDigitsOnly(const QString& text) {
    if (text.isEmpty()) {
        return false;
    }

    for (QChar ch : text) {
        if (!ch.isDigit()) {
            return false;
        }
    }

    return true;
}

double firstNumberFromObject(const QJsonObject& obj, const QStringList& keys) {
    for (const QString& key : keys) {
        const QJsonValue v = obj.value(key);
        if (v.isDouble()) {
            return v.toDouble();
        }
        if (v.isString()) {
            bool ok = false;
            const double d = v.toString().toDouble(&ok);
            if (ok) {
                return d;
            }
        }
    }
    return qQNaN();
}

QString firstNonEmptyStringFromObject(const QJsonObject& obj, const QStringList& keys) {
    for (const QString& key : keys) {
        const QJsonValue v = obj.value(key);
        if (!v.isString()) {
            continue;
        }

        const QString s = v.toString().trimmed();
        if (!s.isEmpty()) {
            return s;
        }
    }

    return {};
}

int intFromJsonValue(const QJsonValue& value, bool* ok = nullptr) {
    auto setOk = [ok](bool valueOk) {
        if (ok) {
            *ok = valueOk;
        }
    };

    if (value.isDouble()) {
        const double number = value.toDouble(qQNaN());
        if (std::isfinite(number)) {
            setOk(true);
            return qRound(number);
        }
        setOk(false);
        return 0;
    }

    if (value.isString()) {
        bool parsed = false;
        const double number = value.toString().trimmed().toDouble(&parsed);
        if (parsed && std::isfinite(number)) {
            setOk(true);
            return qRound(number);
        }
    }

    setOk(false);
    return 0;
}

qint64 int64FromJsonValue(const QJsonValue& value, bool* ok = nullptr) {
    auto setOk = [ok](bool valueOk) {
        if (ok) {
            *ok = valueOk;
        }
    };

    if (value.isDouble()) {
        const double number = value.toDouble(qQNaN());
        if (std::isfinite(number)
            && number >= static_cast<double>(std::numeric_limits<qint64>::min())
            && number <= static_cast<double>(std::numeric_limits<qint64>::max())) {
            setOk(true);
            return static_cast<qint64>(number);
        }
        setOk(false);
        return 0;
    }

    if (value.isString()) {
        bool parsed = false;
        const qint64 number = value.toString().trimmed().toLongLong(&parsed);
        if (parsed) {
            setOk(true);
            return number;
        }
    }

    setOk(false);
    return 0;
}

double normalizeEastMoneyPrice(double raw) {
    if (!std::isfinite(raw)) {
        return qQNaN();
    }

    // EastMoney has used both scaled integers (price*100) and plain decimals.
    // For market quotes in this app, values above 10000 are treated as scaled.
    if (std::fabs(raw) > 10000.0) {
        return raw / 100.0;
    }

    return raw;
}

QString digitsOnly(const QString& raw) {
    QString digits;
    digits.reserve(raw.size());
    for (QChar ch : raw) {
        if (ch.isDigit()) {
            digits.append(ch);
        }
    }
    return digits;
}

QString normalizedSymbol(const QString& rawCode) {
    QString raw = rawCode.trimmed().toLower();
    if (raw.isEmpty()) {
        return {};
    }

    if (raw.startsWith("hk")) {
        QString hkDigits = digitsOnly(raw);
        if (hkDigits.isEmpty()) {
            const QString suffix = raw.mid(2).toUpper();
            if (suffix.isEmpty()) {
                return {};
            }
            return "hk" + suffix;
        }
        if (hkDigits.size() > 5) {
            hkDigits = hkDigits.right(5);
        }
        return "hk" + hkDigits.rightJustified(5, '0');
    }

    QString prefix;
    if (raw.startsWith("sh") || raw.startsWith("sz")) {
        prefix = raw.left(2);
    }

    QString digits = digitsOnly(raw);

    if (digits.isEmpty()) {
        return {};
    }

    if (digits.size() > 6) {
        digits = digits.right(6);
    }
    digits = digits.rightJustified(6, '0');

    if (prefix.isEmpty()) {
        const QChar head = digits[0];
        if (head == '6' || head == '5' || head == '9') {
            prefix = "sh";
        } else {
            prefix = "sz";
        }
    }

    return prefix + digits;
}

QString normalizedSectorCode(const QString& rawCode) {
    QString raw = rawCode.trimmed();
    if (raw.isEmpty()) {
        return {};
    }

    if (raw.startsWith(QStringLiteral("90."), Qt::CaseInsensitive)) {
        raw = raw.mid(3);
    }

    if (raw.startsWith(QStringLiteral("bk"), Qt::CaseInsensitive)) {
        return raw.toUpper();
    }

    return {};
}

QString normalizedFutureSecId(const QString& rawCode) {
    const QString raw = rawCode.trimmed();
    if (raw.isEmpty()) {
        return {};
    }

    const int dot = raw.indexOf(QLatin1Char('.'));
    if (dot <= 0 || dot >= raw.size() - 1) {
        return {};
    }

    const QString market = raw.left(dot).trimmed();
    const QString symbol = raw.mid(dot + 1).trimmed().toUpper();
    if (!isDigitsOnly(market) || symbol.isEmpty()) {
        return {};
    }

    if (market == QStringLiteral("0")
        || market == QStringLiteral("1")
        || market == QStringLiteral("90")
        || market == QStringLiteral("100")
        || market == QStringLiteral("116")
        || market == QStringLiteral("124")
        || market == QStringLiteral("128")) {
        return {};
    }

    bool hasLetter = false;
    for (const QChar ch : symbol) {
        if (ch.isLetter()) {
            hasLetter = true;
            break;
        }
    }
    if (!hasLetter) {
        return {};
    }

    return market + QStringLiteral(".") + symbol;
}

bool isFutureSecIdLike(const QString& rawCode) {
    return !normalizedFutureSecId(rawCode).isEmpty();
}

void markSnapshotUpdatedNow(MarketBreadthSnapshot* snapshot) {
    if (!snapshot) {
        return;
    }
    snapshot->lastUpdatedAtMs = qMax(snapshot->lastUpdatedAtMs, QDateTime::currentMSecsSinceEpoch());
}

QString normalizeHongKongIndexSecId(const QString& rawCode) {
    const QString code = rawCode.trimmed().toLower();
    if (code.isEmpty()) {
        return {};
    }

    if (code == QStringLiteral("hsi")
        || code == QStringLiteral("100.hsi")
        || code == QStringLiteral("124.hsi")) {
        return QStringLiteral("100.HSI");
    }

    if (code == QStringLiteral("hstech")
        || code == QStringLiteral("124.hstech")
        || code == QStringLiteral("100.hstech")) {
        return QStringLiteral("124.HSTECH");
    }

    return {};
}

QVector<QuoteItem> toVector(const QHash<QString, QuoteItem>& buffer) {
    QVector<QuoteItem> out;
    out.reserve(buffer.size());
    for (auto it = buffer.constBegin(); it != buffer.constEnd(); ++it) {
        out.push_back(it.value());
    }
    return out;
}

QString secIdKey(const QString& secId) {
    return secId.trimmed().toLower();
}

QString secIdKeyFromDiffItem(const QJsonObject& item) {
    const QString symbol = item.value(QStringLiteral("f12")).toString().trimmed();
    if (symbol.isEmpty()) {
        return {};
    }

    QString market;
    const QJsonValue marketValue = item.value(QStringLiteral("f13"));
    if (marketValue.isString()) {
        market = marketValue.toString().trimmed();
    } else if (marketValue.isDouble()) {
        market = QString::number(static_cast<int>(marketValue.toDouble()));
    }

    if (market.isEmpty()) {
        return {};
    }

    return secIdKey(market + QStringLiteral(".") + symbol);
}

double chooseRawOrDiv100(double raw, double threshold, double reference) {
    const double direct = raw;
    const double scaled = raw / 100.0;

    if (std::isfinite(reference)) {
        const double directDistance = std::fabs(direct - reference);
        const double scaledDistance = std::fabs(scaled - reference);
        return (scaledDistance + 1e-9 < directDistance) ? scaled : direct;
    }

    return (std::fabs(raw) > threshold) ? scaled : direct;
}

double normalizeEastMoneyPercent(double raw) {
    if (!std::isfinite(raw)) {
        return qQNaN();
    }

    return (std::fabs(raw) > 30.0) ? (raw / 100.0) : raw;
}

QVector<QJsonObject> extractDiffObjects(const QJsonValue& diffValue) {
    QVector<QJsonObject> out;

    if (diffValue.isArray()) {
        const QJsonArray array = diffValue.toArray();
        out.reserve(array.size());
        for (const QJsonValue& value : array) {
            if (value.isObject()) {
                out.push_back(value.toObject());
            }
        }
        return out;
    }

    if (!diffValue.isObject()) {
        return out;
    }

    const QJsonObject object = diffValue.toObject();
    QStringList keys = object.keys();
    std::sort(keys.begin(), keys.end(), [](const QString& lhs, const QString& rhs) {
        bool lhsOk = false;
        bool rhsOk = false;
        const int lhsInt = lhs.toInt(&lhsOk);
        const int rhsInt = rhs.toInt(&rhsOk);
        if (lhsOk && rhsOk) {
            return lhsInt < rhsInt;
        }
        return lhs < rhs;
    });

    out.reserve(keys.size());
    for (const QString& key : keys) {
        const QJsonValue value = object.value(key);
        if (value.isObject()) {
            out.push_back(value.toObject());
        }
    }

    return out;
}

QStringList extractTonghuashunStockTags(const QJsonObject& row) {
    QStringList tags;

    const QJsonValue tagValue = row.value(QStringLiteral("tag"));
    if (!tagValue.isObject()) {
        return tags;
    }

    const QJsonObject tagObject = tagValue.toObject();
    const QJsonArray conceptTags = tagObject.value(QStringLiteral("concept_tag")).toArray();
    for (const QJsonValue& conceptValue : conceptTags) {
        if (!conceptValue.isString()) {
            continue;
        }

        const QString concept = conceptValue.toString().trimmed();
        if (!concept.isEmpty() && !tags.contains(concept)) {
            tags.push_back(concept);
        }
    }

    const QString popularityTag = tagObject.value(QStringLiteral("popularity_tag")).toString().trimmed();
    if (!popularityTag.isEmpty() && !tags.contains(popularityTag)) {
        tags.push_back(popularityTag);
    }

    return tags;
}

const QVector<QPair<QString, QString>>& indexQuoteConfig() {
    static const QVector<QPair<QString, QString>> kConfig = {
        {QStringLiteral("1.000001"),   QStringLiteral("上证指数")},
        {QStringLiteral("0.399001"),   QStringLiteral("深证成指")},
        {QStringLiteral("0.399006"),   QStringLiteral("创业板指")},
        {QStringLiteral("1.000688"),   QStringLiteral("科创50")},
        {QStringLiteral("47.800005"),  QStringLiteral("A股均价")},
        {QStringLiteral("100.HSI"),    QStringLiteral("恒生指数")},
        {QStringLiteral("124.HSTECH"), QStringLiteral("恒生科技")},
        {QStringLiteral("104.CN00Y"),   QStringLiteral("富时中国A50")},
        {QStringLiteral("133.USDCNH"), QStringLiteral("美元/人民币")},
        {QStringLiteral("118.AUTD"),       QStringLiteral("黄金T+D")},
        {QStringLiteral("100.NDX"), QStringLiteral("纳斯达克")},
        {QStringLiteral("100.SPX"),    QStringLiteral("标普500")},
        {QStringLiteral("100.N225"),   QStringLiteral("日经225")},
        {QStringLiteral("100.KS11"),   QStringLiteral("韩国KOSPI")},
    };
    return kConfig;
}

} // namespace

IQuoteProvider::IQuoteProvider(QObject* parent)
    : QObject(parent) {}

IQuoteProvider::~IQuoteProvider() = default;

void IQuoteProvider::setLanguage(const QString& language) {
    Q_UNUSED(language);
}

void IQuoteProvider::applyConfig(const AppConfig& cfg) {
    m_userAgent = network_utils::effectiveUserAgent(cfg);
    m_proxy = network_utils::proxyFromConfig(cfg);
}

EastMoneyQuoteProvider::EastMoneyQuoteProvider(QObject* parent)
    : IQuoteProvider(parent) {}

void EastMoneyQuoteProvider::fetchQuotes(const QVector<StockItem>& stocks) {
    // Ensure only one in-flight polling request at a time: cancel any previous
    // request (e.g. one that is timing out) so its stale handler cannot run and
    // corrupt the shared buffer with an overlapping response.
    if (m_activeReply) {
        QNetworkReply* stale = m_activeReply;
        m_activeReply = nullptr;
        disconnect(stale, nullptr, this, nullptr);
        stale->abort();
        stale->deleteLater();
    }

    m_nam.setProxy(m_proxy);
    m_pendingRequests = 0;
    m_errors.clear();
    m_buffer.clear();

    QStringList secIds;
    secIds.reserve(stocks.size());

    QHash<QString, StockItem> requestMap;
    requestMap.reserve(stocks.size());

    for (const StockItem& stock : stocks) {
        const QString secId = toSecId(stock.code);
        if (secId.isEmpty()) {
            continue;
        }

        const QString key = secIdKey(secId);
        if (requestMap.contains(key)) {
            continue;
        }

        requestMap.insert(key, stock);
        secIds.push_back(secId);
    }

    if (secIds.isEmpty()) {
        emit quotesReady({});
        return;
    }

    QUrl url("https://push2delay.eastmoney.com/api/qt/ulist.np/get");
    QUrlQuery query;
    query.addQueryItem("secids", secIds.join(','));
    query.addQueryItem(
        "fields",
        "f2,f3,f4,f6,f8,f9,f12,f13,f14,f18,f20,f22,f25,f62,f100,f145,f265,f266,f297"
    );
    query.addQueryItem("invt", "2");
    query.addQueryItem("fltt", "2");
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader(headers::kUserAgent, m_userAgent.toUtf8());
    req.setRawHeader(headers::kReferer, headers::kEastMoneyReferer);
    req.setRawHeader(headers::kAccept, "*/*");
    req.setRawHeader(headers::kConnection, "keep-alive");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        "eastmoney",
        "GET",
        req,
        m_proxy
    );

    QNetworkReply* reply = m_nam.get(req);
    m_activeReply = reply;
    ++m_pendingRequests;

    connect(reply, &QNetworkReply::finished, this, [this, reply, requestMap, trace]() {
        if (m_activeReply != reply) {
            // Superseded/aborted by a newer request; ignore this stale response.
            reply->deleteLater();
            return;
        }
        m_activeReply = nullptr;

        const QString err = (reply->error() == QNetworkReply::NoError)
            ? QString()
            : reply->errorString();
        const QByteArray body = reply->readAll();

        network_logger::logRequestFinish(trace, reply, body.size(), body);

        reply->deleteLater();
        handleResponse(requestMap, body, err);
    });
}

QString EastMoneyQuoteProvider::toSecId(const QString& rawCode) {
    const QString sector = normalizedSectorCode(rawCode);
    if (!sector.isEmpty()) {
        return "90." + sector;
    }

    const QString futureSecId = normalizedFutureSecId(rawCode);
    if (!futureSecId.isEmpty()) {
        return futureSecId;
    }

    const QString raw = rawCode.trimmed();
    if (raw.isEmpty()) {
        return {};
    }

    const QString lower = raw.toLower();

    // Accept direct secid-like inputs.
    const int dot = lower.indexOf('.');
    if (dot > 0 && dot < lower.size() - 1) {
        const QString market = lower.left(dot);
        const QString symbolPart = raw.mid(dot + 1).trimmed();

        const QString directFutureSecId = normalizedFutureSecId(raw);
        if (!directFutureSecId.isEmpty()) {
            return directFutureSecId;
        }

        if (market == QStringLiteral("90")) {
            const QString directSector = normalizedSectorCode(symbolPart);
            if (!directSector.isEmpty()) {
                return QStringLiteral("90.") + directSector;
            }
        }

        if (market == QStringLiteral("116")) {
            QString hkDigits = digitsOnly(symbolPart);
            if (hkDigits.isEmpty()) {
                return {};
            }
            if (hkDigits.size() > 5) {
                hkDigits = hkDigits.right(5);
            }
            return QStringLiteral("116.") + hkDigits.rightJustified(5, '0');
        }

        if (market == QStringLiteral("100")
            || market == QStringLiteral("124")
            || market == QStringLiteral("128")) {
            const QString hkIndexSecId = normalizeHongKongIndexSecId(market + QStringLiteral(".") + symbolPart);
            if (!hkIndexSecId.isEmpty()) {
                return hkIndexSecId;
            }

            const QString symbol = symbolPart.toUpper();
            if (!symbol.isEmpty()) {
                return market + QStringLiteral(".") + symbol;
            }
        }

        if (market == QStringLiteral("0") || market == QStringLiteral("1")) {
            QString digits = digitsOnly(symbolPart);
            if (digits.isEmpty()) {
                return {};
            }
            if (digits.size() > 6) {
                digits = digits.right(6);
            }
            return market + QStringLiteral(".") + digits.rightJustified(6, '0');
        }
    }

    const QString hkIndexSecId = normalizeHongKongIndexSecId(lower);
    if (!hkIndexSecId.isEmpty()) {
        return hkIndexSecId;
    }

    if (lower.endsWith(QStringLiteral(".hk"))) {
        QString hkDigits = digitsOnly(lower);
        if (hkDigits.size() > 5) {
            hkDigits = hkDigits.right(5);
        }
        if (!hkDigits.isEmpty()) {
            return QStringLiteral("116.") + hkDigits.rightJustified(5, '0');
        }
    }

    if (lower.startsWith("hk")) {
        QString hkDigits = digitsOnly(lower);
        if (hkDigits.isEmpty()) {
            return {};
        }
        if (hkDigits.size() > 5) {
            hkDigits = hkDigits.right(5);
        }
        return QStringLiteral("116.") + hkDigits.rightJustified(5, '0');
    }

    if (lower.size() == 5 && isDigitsOnly(lower)) {
        return QStringLiteral("116.") + lower;
    }

    const QString symbol = normalizedSymbol(rawCode);
    if (symbol.size() != 8) {
        return {};
    }

    const QString market = (symbol.startsWith("sh")) ? "1" : "0";
    return market + "." + symbol.mid(2);
}

double EastMoneyQuoteProvider::firstNumber(const QJsonObject& obj, const QStringList& keys) {
    return firstNumberFromObject(obj, keys);
}

void EastMoneyQuoteProvider::handleResponse(
    const QHash<QString, StockItem>& requestMap,
    const QByteArray& body,
    const QString& errorText
) {
    if (!errorText.isEmpty()) {
        m_errors << QString("eastmoney request failed: %1").arg(errorText);
    } else {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError) {
            m_errors << QString("eastmoney json parse error: %1").arg(pe.errorString());
        } else if (!doc.isObject()) {
            m_errors << QStringLiteral("eastmoney invalid payload");
        } else {
            const QJsonObject root = doc.object();
            const int rc = root.value("rc").toInt(0);
            if (root.contains("rc") && rc != 0) {
                const QString rcMsg = firstNonEmptyStringFromObject(root, {"msg", "message"});
                const QString suffix = rcMsg.isEmpty() ? QString() : (" msg=" + rcMsg);
                m_errors << QString("eastmoney rc=%1%2").arg(rc).arg(suffix);
            } else if (!root.value("data").isObject()) {
                m_errors << QStringLiteral("eastmoney data missing");
            } else {
                const QJsonObject data = root.value("data").toObject();
                const QVector<QJsonObject> diff = extractDiffObjects(data.value("diff"));

                for (const QJsonObject& row : diff) {
                    const QString secKey = secIdKeyFromDiffItem(row);
                    StockItem stock = requestMap.value(secKey);

                    if (stock.code.trimmed().isEmpty()) {
                        const QString rowCode = row.value(QStringLiteral("f12")).toString().trimmed();
                        if (!rowCode.isEmpty()) {
                            const QString rowCodeLower = rowCode.toLower();
                            for (auto it = requestMap.constBegin(); it != requestMap.constEnd(); ++it) {
                                if (it.key().endsWith(QStringLiteral(".") + rowCodeLower)) {
                                    stock = it.value();
                                    break;
                                }
                            }
                        }
                    }

                    if (stock.code.trimmed().isEmpty()) {
                        continue;
                    }

                    const bool isFuture = isFutureSecIdLike(stock.code);
                    const double lastRaw = firstNumber(row, {"f2", "f43"});
                    double preRaw = firstNumber(row, {"f18", "f60"});
                    const double pctRaw = firstNumber(row, {"f3", "f170"});
                    const double changeRaw = firstNumber(row, {"f4", "f169"});

                    if (std::isnan(lastRaw)) {
                        m_errors << QString("%1: eastmoney missing price field").arg(stock.code);
                        continue;
                    }

                    if (std::isnan(preRaw) || qFuzzyIsNull(preRaw)) {
                        preRaw = lastRaw;
                    }

                    const double last = isFuture ? lastRaw : normalizeEastMoneyPrice(lastRaw);
                    const double pre = isFuture ? preRaw : normalizeEastMoneyPrice(preRaw);

                    if (std::isnan(last) || std::isnan(pre)) {
                        m_errors << QString("%1: eastmoney invalid price fields").arg(stock.code);
                        continue;
                    }

                    const double derivedChange = last - pre;
                    const double derivedPct = qFuzzyIsNull(pre)
                        ? 0.0
                        : (derivedChange / pre * 100.0);

                    QuoteItem q;
                    q.code = stock.code;
                    const QString remoteName = firstNonEmptyStringFromObject(row, {"f14", "f58"});
                    q.name = remoteName.isEmpty() ? stock.name : remoteName;
                    q.price = last;
                    q.change = std::isnan(changeRaw)
                        ? derivedChange
                        : (isFuture ? changeRaw : chooseRawOrDiv100(changeRaw, 30.0, derivedChange));
                    q.pct = std::isnan(pctRaw)
                        ? derivedPct
                        : (isFuture ? pctRaw : chooseRawOrDiv100(pctRaw, 30.0, derivedPct));

                    m_buffer.insert(q.code, q);
                }

                if (diff.isEmpty() && m_buffer.isEmpty()) {
                    m_errors << QStringLiteral("eastmoney diff missing or empty");
                }
            }
        }
    }

    --m_pendingRequests;
    if (m_pendingRequests <= 0) {
        if (m_buffer.isEmpty() && !m_errors.isEmpty()) {
            emit error("eastmoney: " + m_errors.join(" | "));
        }
        emit quotesReady(toVector(m_buffer));
    }
}

EastMoneyHotRankProvider::EastMoneyHotRankProvider(QObject* parent)
    : QObject(parent) {}

void EastMoneyHotRankProvider::applyConfig(const AppConfig& cfg) {
    m_userAgent = network_utils::effectiveUserAgent(cfg);
    m_proxy = network_utils::proxyFromConfig(cfg);

    m_hotSectorCacheValid = false;
    m_hotConceptCacheValid = false;
    m_hotSectorCacheExpiresAtMs = 0;
    m_hotConceptCacheExpiresAtMs = 0;
    m_hotSectorCacheRequestKey.clear();
    m_hotConceptCacheRequestKey.clear();

    if (m_hotSectorReply) {
        QNetworkReply* pendingReply = m_hotSectorReply;
        m_hotSectorReply = nullptr;
        pendingReply->abort();
        pendingReply->deleteLater();
    }
    if (m_hotConceptReply) {
        QNetworkReply* pendingReply = m_hotConceptReply;
        m_hotConceptReply = nullptr;
        pendingReply->abort();
        pendingReply->deleteLater();
    }
    m_hotSectorInFlightRequestKey.clear();
    m_hotConceptInFlightRequestKey.clear();
}

void EastMoneyHotRankProvider::fetchHotSectors(
    int limit,
    const QString& sortField,
    const QString& sortOrder,
    bool forceRefresh
) {
    fetchHotList(false, limit, sortField, sortOrder, forceRefresh);
}

void EastMoneyHotRankProvider::fetchHotConcepts(
    int limit,
    const QString& sortField,
    const QString& sortOrder,
    bool forceRefresh
) {
    fetchHotList(true, limit, sortField, sortOrder, forceRefresh);
}

void EastMoneyHotRankProvider::fetchHotList(
    bool concept,
    int limit,
    const QString& sortField,
    const QString& sortOrder,
    bool forceRefresh
) {
    m_nam.setProxy(m_proxy);

    const QString normalizedSortField = normalizeHotRankSortField(sortField);
    const QString normalizedSortOrder = normalizeHotRankSortOrder(sortOrder);
    const QString requestKey = QStringLiteral("%1|%2|%3")
        .arg(qMax(1, limit))
        .arg(normalizedSortField, normalizedSortOrder);

    QVector<HotRankItem>& cachedItems = concept ? m_cachedHotConcepts : m_cachedHotSectors;
    QString& cacheRequestKey = concept ? m_hotConceptCacheRequestKey : m_hotSectorCacheRequestKey;
    qint64& cacheExpiresAtMs = concept ? m_hotConceptCacheExpiresAtMs : m_hotSectorCacheExpiresAtMs;
    bool& cacheValid = concept ? m_hotConceptCacheValid : m_hotSectorCacheValid;
    QNetworkReply*& inFlightReply = concept ? m_hotConceptReply : m_hotSectorReply;
    QString& inFlightRequestKey = concept
        ? m_hotConceptInFlightRequestKey
        : m_hotSectorInFlightRequestKey;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!forceRefresh && cacheValid && cacheRequestKey == requestKey && nowMs < cacheExpiresAtMs) {
        qInfo() << "[HotRank] cache hit type=" << (concept ? "concept" : "sector")
                << "ttl_ms=" << (cacheExpiresAtMs - nowMs);
        if (concept) {
            emit hotConceptsReady(cachedItems);
        } else {
            emit hotSectorsReady(cachedItems);
        }
        return;
    }

    SharedHotRankCacheEntry& sharedCache = sharedHotRankCache(concept);
    if (!forceRefresh
        && sharedCache.valid
        && sharedCache.requestKey == requestKey
        && nowMs < sharedCache.expiresAtMs) {
        cachedItems = sharedCache.items;
        cacheRequestKey = sharedCache.requestKey;
        cacheExpiresAtMs = sharedCache.expiresAtMs;
        cacheValid = true;

        qInfo() << "[HotRank] shared cache hit type=" << (concept ? "concept" : "sector")
                << "ttl_ms=" << (sharedCache.expiresAtMs - nowMs);
        if (concept) {
            emit hotConceptsReady(cachedItems);
        } else {
            emit hotSectorsReady(cachedItems);
        }
        return;
    }

    if (inFlightReply && !forceRefresh) {
        qInfo() << "[HotRank] request already in flight type=" << (concept ? "concept" : "sector")
                << "skip";
        return;
    }

    if (forceRefresh && inFlightReply) {
        inFlightReply->setProperty("myStocksIgnoreAbort", true);
        inFlightReply->abort();
        inFlightReply = nullptr;
        inFlightRequestKey.clear();
    }

    inFlightRequestKey = requestKey;

    const QString fid = normalizedSortField == QLatin1String("pct")
        ? QStringLiteral("f3")
        : QStringLiteral("f62");
    const QString fs = concept
        ? QStringLiteral("m:90+t:3")
        : QStringLiteral("b:MK0878");
    const int effectiveLimit = concept
        ? qMin(100, qMax(1, limit))
        : qMin(2000, qMax(1, limit));

    QUrl url(QStringLiteral("https://push2delay.eastmoney.com/api/qt/clist/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("pn"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("pz"), QString::number(effectiveLimit));
    query.addQueryItem(
        QStringLiteral("po"),
        normalizedSortOrder == QLatin1String("asc") ? QStringLiteral("0") : QStringLiteral("1")
    );
    query.addQueryItem(QStringLiteral("np"), QStringLiteral("3"));
    query.addQueryItem(QStringLiteral("fltt"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("invt"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("fid"), fid);
    query.addQueryItem(QStringLiteral("fs"), fs);
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("f12,f14,f3,f25,f62"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader(headers::kUserAgent, m_userAgent.toUtf8());
    req.setRawHeader(headers::kReferer, headers::kEastMoneyReferer);
    req.setRawHeader(headers::kAccept, "*/*");
    req.setRawHeader(headers::kConnection, "keep-alive");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        concept ? QStringLiteral("eastmoney-hot-concept") : QStringLiteral("eastmoney-hot-sector"),
        QStringLiteral("GET"),
        req,
        m_proxy
    );

    inFlightReply = m_nam.get(req);
    QNetworkReply* reply = inFlightReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, concept, trace, requestKey]() {
        if (concept) {
            if (reply == m_hotConceptReply) {
                m_hotConceptReply = nullptr;
            }
            m_hotConceptInFlightRequestKey.clear();
        } else {
            if (reply == m_hotSectorReply) {
                m_hotSectorReply = nullptr;
            }
            m_hotSectorInFlightRequestKey.clear();
        }

        if (reply->property("myStocksIgnoreAbort").toBool()) {
            reply->deleteLater();
            return;
        }

        const QString err = (reply->error() == QNetworkReply::NoError)
            ? QString()
            : reply->errorString();
        const QByteArray body = reply->readAll();

        network_logger::logRequestFinish(trace, reply, body.size(), body);

        reply->deleteLater();
        handleHotListResponse(concept, body, err, requestKey);
    });
}

void EastMoneyHotRankProvider::handleHotListResponse(
    bool concept,
    const QByteArray& body,
    const QString& errorText,
    const QString& requestKey
) {
    QString errorMessage;
    QVector<HotRankItem> items;

    if (!errorText.isEmpty()) {
        errorMessage = QStringLiteral("eastmoney hot list request failed: %1").arg(errorText);
    } else {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            errorMessage = QStringLiteral("eastmoney hot list json parse error: %1")
                .arg(parseError.errorString());
        } else if (!doc.isObject()) {
            errorMessage = QStringLiteral("eastmoney hot list invalid payload");
        } else {
            const QJsonObject root = doc.object();
            const int rc = root.value(QStringLiteral("rc")).toInt(0);
            if (root.contains(QStringLiteral("rc")) && rc != 0) {
                const QString rcMsg = firstNonEmptyStringFromObject(root, {"msg", "message"});
                const QString suffix = rcMsg.isEmpty() ? QString() : (QStringLiteral(" msg=") + rcMsg);
                errorMessage = QStringLiteral("eastmoney hot list rc=%1%2").arg(rc).arg(suffix);
            } else if (!root.value(QStringLiteral("data")).isObject()) {
                errorMessage = QStringLiteral("eastmoney hot list data missing");
            } else {
                const QJsonObject data = root.value(QStringLiteral("data")).toObject();
                const QVector<QJsonObject> diff = extractDiffObjects(data.value(QStringLiteral("diff")));
                items.reserve(diff.size());

                for (const QJsonObject& row : diff) {
                    HotRankItem item;
                    item.code = row.value(QStringLiteral("f12")).toString().trimmed();
                    item.name = row.value(QStringLiteral("f14")).toString().trimmed();
                    item.detailFs = item.code.isEmpty()
                        ? QString()
                        : QStringLiteral("b:%1").arg(item.code);
                    item.pct = normalizeEastMoneyPercent(
                        firstNumberFromObject(row, {QStringLiteral("f3")})
                    );
                    item.change = firstNumberFromObject(row, {QStringLiteral("f4")});
                    item.mainNetInflow = firstNumberFromObject(row, {QStringLiteral("f62")});
                    item.yearPct = normalizeEastMoneyPercent(
                        firstNumberFromObject(row, {QStringLiteral("f25")})
                    );
                    if (!item.code.isEmpty() && !item.name.isEmpty()) {
                        items.push_back(item);
                    }
                }
            }
        }
    }

    if (!errorMessage.isEmpty()) {
        if (errorMessage != m_lastError) {
            emit error(errorMessage);
            m_lastError = errorMessage;
        }
        return;
    }

    m_lastError.clear();
    QVector<HotRankItem>& cachedItems = concept ? m_cachedHotConcepts : m_cachedHotSectors;
    QString& cacheRequestKey = concept ? m_hotConceptCacheRequestKey : m_hotSectorCacheRequestKey;
    qint64& cacheExpiresAtMs = concept ? m_hotConceptCacheExpiresAtMs : m_hotSectorCacheExpiresAtMs;
    bool& cacheValid = concept ? m_hotConceptCacheValid : m_hotSectorCacheValid;

    cachedItems = items;
    cacheRequestKey = requestKey;
    cacheExpiresAtMs = QDateTime::currentMSecsSinceEpoch() + app_constants::kNetworkCacheTtlMs;
    cacheValid = true;

    SharedHotRankCacheEntry& sharedCache = sharedHotRankCache(concept);
    sharedCache.items = items;
    sharedCache.requestKey = requestKey;
    sharedCache.expiresAtMs = cacheExpiresAtMs;
    sharedCache.valid = true;

    if (concept) {
        emit hotConceptsReady(items);
    } else {
        emit hotSectorsReady(items);
    }
}

EastMoneyHotRankDetailProvider::EastMoneyHotRankDetailProvider(QObject* parent)
    : QObject(parent) {}

void EastMoneyHotRankDetailProvider::applyConfig(const AppConfig& cfg) {
    m_userAgent = network_utils::effectiveUserAgent(cfg);
    m_proxy = network_utils::proxyFromConfig(cfg);
    m_cacheValid = false;
    m_cacheExpiresAtMs = 0;
    m_cachedFs.clear();

    if (m_reply) {
        QNetworkReply* pendingReply = m_reply;
        m_reply = nullptr;
        pendingReply->abort();
        pendingReply->deleteLater();
    }
    m_inFlightFs.clear();
}

void EastMoneyHotRankDetailProvider::fetch(const QString& fs, int limit, bool forceRefresh) {
    const QString normalizedFs = fs.trimmed();
    if (normalizedFs.isEmpty()) {
        emit error(fs, QStringLiteral("eastmoney hot detail fs missing"));
        return;
    }

    m_nam.setProxy(m_proxy);

    const int effectiveLimit = qBound(1, limit, 1000);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!forceRefresh
        && m_cacheValid
        && m_cachedFs == normalizedFs
        && nowMs < m_cacheExpiresAtMs) {
        emit dataReady(normalizedFs, m_cachedItems);
        return;
    }

    if (m_reply && !forceRefresh && m_inFlightFs == normalizedFs) {
        return;
    }

    if (forceRefresh && m_reply) {
        m_reply->setProperty("myStocksIgnoreAbort", true);
        m_reply->abort();
        m_reply = nullptr;
        m_inFlightFs.clear();
    }

    m_inFlightFs = normalizedFs;

    QUrl url(QStringLiteral("https://push2delay.eastmoney.com/api/qt/clist/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("pn"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("pz"), QString::number(effectiveLimit));
    query.addQueryItem(QStringLiteral("po"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("np"), QStringLiteral("3"));
    query.addQueryItem(QStringLiteral("fltt"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("invt"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("fid"), QStringLiteral("f3"));
    query.addQueryItem(QStringLiteral("fs"), normalizedFs);
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("f12,f13,f14,f2,f3,f6,f20,f25"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader(headers::kUserAgent, m_userAgent.toUtf8());
    req.setRawHeader(headers::kReferer, headers::kEastMoneyReferer);
    req.setRawHeader(headers::kAccept, "*/*");
    req.setRawHeader(headers::kConnection, "keep-alive");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        QStringLiteral("eastmoney-hot-detail"),
        QStringLiteral("GET"),
        req,
        m_proxy
    );

    m_reply = m_nam.get(req);
    QNetworkReply* reply = m_reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, trace, normalizedFs]() {
        if (reply == m_reply) {
            m_reply = nullptr;
        }
        m_inFlightFs.clear();

        if (reply->property("myStocksIgnoreAbort").toBool()) {
            reply->deleteLater();
            return;
        }

        const QString err = (reply->error() == QNetworkReply::NoError)
            ? QString()
            : reply->errorString();
        const QByteArray body = reply->readAll();

        network_logger::logRequestFinish(trace, reply, body.size(), body);

        reply->deleteLater();
        handleResponse(normalizedFs, body, err);
    });
}

void EastMoneyHotRankDetailProvider::handleResponse(
    const QString& fs,
    const QByteArray& body,
    const QString& errorText
) {
    QString errorMessage;
    QVector<HotRankDetailItem> items;

    if (!errorText.isEmpty()) {
        errorMessage = QStringLiteral("eastmoney hot detail request failed: %1").arg(errorText);
    } else {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            errorMessage = QStringLiteral("eastmoney hot detail json parse error: %1")
                .arg(parseError.errorString());
        } else if (!doc.isObject()) {
            errorMessage = QStringLiteral("eastmoney hot detail invalid payload");
        } else {
            const QJsonObject root = doc.object();
            const int rc = root.value(QStringLiteral("rc")).toInt(0);
            if (root.contains(QStringLiteral("rc")) && rc != 0) {
                const QString rcMsg = firstNonEmptyStringFromObject(root, {"msg", "message"});
                const QString suffix = rcMsg.isEmpty() ? QString() : (QStringLiteral(" msg=") + rcMsg);
                errorMessage = QStringLiteral("eastmoney hot detail rc=%1%2").arg(rc).arg(suffix);
            } else if (!root.value(QStringLiteral("data")).isObject()) {
                errorMessage = QStringLiteral("eastmoney hot detail data missing");
            } else {
                const QJsonObject data = root.value(QStringLiteral("data")).toObject();
                const QVector<QJsonObject> diff = extractDiffObjects(data.value(QStringLiteral("diff")));
                items.reserve(diff.size());

                for (const QJsonObject& row : diff) {
                    HotRankDetailItem item;
                    item.code = row.value(QStringLiteral("f12")).toString().trimmed();
                    const QString market = QString::number(
                        row.value(QStringLiteral("f13")).toInt(-1)
                    );
                    item.watchCode = (item.code.isEmpty() || market == QStringLiteral("-1"))
                        ? QString()
                        : QStringLiteral("%1.%2").arg(market, item.code);
                    item.name = row.value(QStringLiteral("f14")).toString().trimmed();
                    item.price = normalizeEastMoneyPrice(
                        firstNumberFromObject(row, {QStringLiteral("f2")})
                    );
                    item.pct = normalizeEastMoneyPercent(
                        firstNumberFromObject(row, {QStringLiteral("f3")})
                    );
                    item.marketCap = firstNumberFromObject(row, {QStringLiteral("f20")});
                    item.turnover = firstNumberFromObject(row, {QStringLiteral("f6")});
                    item.yearPct = normalizeEastMoneyPercent(
                        firstNumberFromObject(row, {QStringLiteral("f25")})
                    );
                    if (!item.code.isEmpty() && !item.name.isEmpty()) {
                        items.push_back(item);
                    }
                }
            }
        }
    }

    if (!errorMessage.isEmpty()) {
        if (errorMessage != m_lastError) {
            emit error(fs, errorMessage);
            m_lastError = errorMessage;
        }
        return;
    }

    m_lastError.clear();
    m_cachedFs = fs;
    m_cachedItems = items;
    m_cacheExpiresAtMs = QDateTime::currentMSecsSinceEpoch() + app_constants::kNetworkCacheTtlMs;
    m_cacheValid = true;
    emit dataReady(fs, items);
}

AshareMarketBreadthProvider::AshareMarketBreadthProvider(QObject* parent)
    : QObject(parent) {}

void AshareMarketBreadthProvider::applyConfig(const AppConfig& cfg) {
    m_userAgent = network_utils::effectiveUserAgent(cfg);
    m_proxy = network_utils::proxyFromConfig(cfg);
    m_cacheValid = false;
    m_cacheExpiresAtMs = 0;
}

namespace {

QString parseMarketOverviewPayload(const QByteArray& body, MarketBreadthSnapshot* snapshot) {
    if (!snapshot) {
        return QStringLiteral("market overview snapshot missing");
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return QStringLiteral("market overview json parse error: %1")
            .arg(parseError.errorString());
    }
    if (!doc.isObject()) {
        return QStringLiteral("market overview invalid payload");
    }

    const QJsonObject root = doc.object();
    const int statusCode = root.value(QStringLiteral("status_code")).toInt(-1);
    if (statusCode != 0) {
        const QString statusMsg = root.value(QStringLiteral("status_msg")).toString().trimmed();
        return statusMsg.isEmpty()
            ? QStringLiteral("market overview status_code=%1").arg(statusCode)
            : QStringLiteral("market overview status_code=%1 msg=%2").arg(statusCode).arg(statusMsg);
    }
    if (!root.value(QStringLiteral("data")).isObject()) {
        return QStringLiteral("market overview data missing");
    }

    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    if (!data.value(QStringLiteral("charts")).isObject()) {
        return QStringLiteral("market overview charts missing");
    }

    const QJsonObject charts = data.value(QStringLiteral("charts")).toObject();
    const QJsonArray header = charts.value(QStringLiteral("header")).toArray();
    const QJsonArray pointList = charts.value(QStringLiteral("point_list")).toArray();

    int rise = -1;
    int fall = -1;
    int limitUp = -1;
    int limitDown = -1;

    for (const QJsonValue& itemValue : header) {
        if (!itemValue.isObject()) {
            continue;
        }

        const QJsonObject item = itemValue.toObject();
        const QString key = item.value(QStringLiteral("key")).toString().trimmed();
        bool ok = false;
        const int value = qMax(0, intFromJsonValue(item.value(QStringLiteral("val")), &ok));
        if (!ok) {
            continue;
        }

        if (key == QLatin1String("rise")) {
            rise = value;
        } else if (key == QLatin1String("fall")) {
            fall = value;
        } else if (key == QLatin1String("limit_up")) {
            limitUp = value;
        } else if (key == QLatin1String("limit_down")) {
            limitDown = value;
        }
    }

    if (rise < 0 || fall < 0) {
        if (!pointList.isEmpty()) {
            const QJsonArray lastPoint = pointList.last().toArray();
            if (lastPoint.size() >= 3) {
                bool riseOk = false;
                bool fallOk = false;
                const int riseFromPoint = intFromJsonValue(lastPoint.at(1), &riseOk);
                const int fallFromPoint = intFromJsonValue(lastPoint.at(2), &fallOk);
                if (riseOk && fallOk) {
                    rise = qMax(0, riseFromPoint);
                    fall = qMax(0, fallFromPoint);
                }
                if (lastPoint.size() >= 5) {
                    bool upOk = false;
                    bool downOk = false;
                    const int upFromPoint = intFromJsonValue(lastPoint.at(3), &upOk);
                    const int downFromPoint = intFromJsonValue(lastPoint.at(4), &downOk);
                    if (upOk) {
                        limitUp = qMax(0, upFromPoint);
                    }
                    if (downOk) {
                        limitDown = qMax(0, downFromPoint);
                    }
                }
            }
        }
    }

    if (rise < 0 || fall < 0) {
        return QStringLiteral("market overview rise/fall missing");
    }

    snapshot->upCount = rise;
    snapshot->downCount = fall;
    if (limitUp >= 0) {
        snapshot->limitUpCount = limitUp;
    }
    if (limitDown >= 0) {
        snapshot->limitDownCount = limitDown;
    }
    snapshot->overviewValid = true;
    snapshot->breadthValid = true;

    QVector<MarketBreadthTimelinePoint> timeline;
    timeline.reserve(pointList.size());
    for (const QJsonValue& pointValue : pointList) {
        if (!pointValue.isArray()) {
            continue;
        }
        const QJsonArray row = pointValue.toArray();
        if (row.size() < 5) {
            continue;
        }

        bool tsOk = false;
        bool riseOk = false;
        bool fallOk = false;
        bool limitUpOk = false;
        bool limitDownOk = false;

        const qint64 timestampMs = int64FromJsonValue(row.at(0), &tsOk);
        const int riseCount = intFromJsonValue(row.at(1), &riseOk);
        const int fallCount = intFromJsonValue(row.at(2), &fallOk);
        const int limitUpCount = intFromJsonValue(row.at(3), &limitUpOk);
        const int limitDownCount = intFromJsonValue(row.at(4), &limitDownOk);
        if (!(tsOk && riseOk && fallOk && limitUpOk && limitDownOk)) {
            continue;
        }

        MarketBreadthTimelinePoint point;
        point.timestampMs = timestampMs;
        point.riseCount = qMax(0, riseCount);
        point.fallCount = qMax(0, fallCount);
        point.limitUpCount = qMax(0, limitUpCount);
        point.limitDownCount = qMax(0, limitDownCount);
        timeline.push_back(point);
    }

    if (!timeline.isEmpty()) {
        snapshot->overviewTimeline = timeline;
    }
    return {};
}

QString parseMarketDistributionPayload(const QByteArray& body, MarketBreadthSnapshot* snapshot) {
    if (!snapshot) {
        return QStringLiteral("market distribution snapshot missing");
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return QStringLiteral("market distribution json parse error: %1")
            .arg(parseError.errorString());
    }
    if (!doc.isObject()) {
        return QStringLiteral("market distribution invalid payload");
    }

    const QJsonObject root = doc.object();
    const int statusCode = root.value(QStringLiteral("status_code")).toInt(-1);
    if (statusCode != 0) {
        const QString statusMsg = root.value(QStringLiteral("status_msg")).toString().trimmed();
        return statusMsg.isEmpty()
            ? QStringLiteral("market distribution status_code=%1").arg(statusCode)
            : QStringLiteral("market distribution status_code=%1 msg=%2").arg(statusCode).arg(statusMsg);
    }
    if (!root.value(QStringLiteral("data")).isObject()) {
        return QStringLiteral("market distribution data missing");
    }

    const QJsonObject data = root.value(QStringLiteral("data")).toObject();

    bool upOk = false;
    bool flatOk = false;
    bool downOk = false;
    bool limitUpOk = false;
    bool limitDownOk = false;
    const int up = qMax(0, intFromJsonValue(data.value(QStringLiteral("up")), &upOk));
    const int flat = qMax(0, intFromJsonValue(data.value(QStringLiteral("flat")), &flatOk));
    const int down = qMax(0, intFromJsonValue(data.value(QStringLiteral("down")), &downOk));
    const int limitUp = qMax(0, intFromJsonValue(data.value(QStringLiteral("limit_up")), &limitUpOk));
    const int limitDown = qMax(0, intFromJsonValue(data.value(QStringLiteral("limit_down")), &limitDownOk));

    if (!snapshot->overviewValid && upOk && downOk) {
        snapshot->upCount = up;
        snapshot->downCount = down;
    }
    if (flatOk) {
        snapshot->flatCount = flat;
    }
    if (limitUpOk && (snapshot->limitUpCount == 0 || !snapshot->overviewValid)) {
        snapshot->limitUpCount = limitUp;
    }
    if (limitDownOk && (snapshot->limitDownCount == 0 || !snapshot->overviewValid)) {
        snapshot->limitDownCount = limitDown;
    }
    if ((snapshot->overviewValid && flatOk) || (upOk && flatOk && downOk)) {
        snapshot->breadthValid = true;
    }

    const QJsonArray table = data.value(QStringLiteral("table")).toArray();
    QVector<MarketBreadthDistributionItem> distribution;
    distribution.reserve(table.size());

    for (const QJsonValue& itemValue : table) {
        if (!itemValue.isObject()) {
            continue;
        }

        const QJsonObject item = itemValue.toObject();
        const QString bucket = item.value(QStringLiteral("key")).toString().trimmed();
        bool valueOk = false;
        const int value = qMax(0, intFromJsonValue(item.value(QStringLiteral("value")), &valueOk));
        if (bucket.isEmpty() || !valueOk) {
            continue;
        }

        MarketBreadthDistributionItem row;
        row.bucket = bucket;
        row.value = value;
        distribution.push_back(row);
    }

    if (!distribution.isEmpty()) {
        snapshot->distribution = distribution;
        snapshot->distributionValid = true;
    }

    if (!flatOk && distribution.isEmpty()) {
        return QStringLiteral("market distribution data missing");
    }

    return {};
}

QString parseMarketTurnoverPayload(const QByteArray& body, MarketBreadthSnapshot* snapshot) {
    if (!snapshot) {
        return QStringLiteral("market turnover snapshot missing");
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return QStringLiteral("market turnover json parse error: %1")
            .arg(parseError.errorString());
    }
    if (!doc.isObject()) {
        return QStringLiteral("market turnover invalid payload");
    }

    const QJsonObject root = doc.object();
    const int statusCode = root.value(QStringLiteral("status_code")).toInt(-1);
    if (statusCode != 0) {
        const QString statusMsg = root.value(QStringLiteral("status_msg")).toString().trimmed();
        return statusMsg.isEmpty()
            ? QStringLiteral("market turnover status_code=%1").arg(statusCode)
            : QStringLiteral("market turnover status_code=%1 msg=%2").arg(statusCode).arg(statusMsg);
    }
    if (!root.value(QStringLiteral("data")).isObject()) {
        return QStringLiteral("market turnover data missing");
    }

    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    if (!data.value(QStringLiteral("charts")).isObject()) {
        return QStringLiteral("market turnover charts missing");
    }

    const QJsonObject charts = data.value(QStringLiteral("charts")).toObject();
    const QJsonArray header = charts.value(QStringLiteral("header")).toArray();
    if (header.isEmpty()) {
        return QStringLiteral("market turnover header missing");
    }

    bool hasTurnover = false;
    bool hasTurnoverPre = false;
    bool hasTurnoverChange = false;
    for (const QJsonValue& itemValue : header) {
        if (!itemValue.isObject()) {
            continue;
        }

        const QJsonObject item = itemValue.toObject();
        const QString key = item.value(QStringLiteral("key")).toString().trimmed();
        const double value = item.value(QStringLiteral("val")).toDouble(qQNaN());
        if (key == QLatin1String("turnover")) {
            snapshot->turnover = value;
            hasTurnover = std::isfinite(value);
        } else if (key == QLatin1String("turnover_pre")) {
            snapshot->turnoverPre = value;
            hasTurnoverPre = std::isfinite(value);
        } else if (key == QLatin1String("turnover_change")) {
            snapshot->turnoverChange = value;
            hasTurnoverChange = std::isfinite(value);
        }
    }

    if (!hasTurnover || !hasTurnoverPre || !hasTurnoverChange) {
        return QStringLiteral("market turnover header incomplete");
    }

    snapshot->turnoverValid = true;
    return {};
}

void applyTonghuashunCommonHeaders(QNetworkRequest* req, const QString& userAgent) {
    if (!req) {
        return;
    }

    req->setRawHeader(headers::kAccept, "*/*");
    req->setRawHeader(headers::kOrigin, headers::kTonghuashunOrigin);
    req->setRawHeader(headers::kReferer, headers::kTonghuashunReferer);
    req->setRawHeader(headers::kConnection, "keep-alive");
    req->setRawHeader(headers::kContentType, headers::kJsonContentType);
    req->setRawHeader(headers::kUserAgent, userAgent.toUtf8());
    req->setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);
}

} // namespace

void AshareMarketBreadthProvider::fetch(bool forceRefresh) {
    m_nam.setProxy(m_proxy);

    if (forceRefresh) {
        m_cacheValid = false;
        m_cacheExpiresAtMs = 0;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!forceRefresh && m_cacheValid && nowMs < m_cacheExpiresAtMs) {
        const qint64 ttlMs = m_cacheExpiresAtMs - nowMs;
        qInfo() << "[MarketBreadth] cache hit endpoint=overview"
                << "ttl_ms=" << ttlMs;
        qInfo() << "[MarketBreadth] cache hit endpoint=distribution"
                << "ttl_ms=" << ttlMs;
        qInfo() << "[MarketBreadth] cache hit endpoint=turnover"
                << "ttl_ms=" << ttlMs;
        emit dataReady(m_cachedSnapshot);
        return;
    }

    if (m_overviewReply || m_distributionReply || m_turnoverReply) {
        if (forceRefresh) {
            if (m_overviewReply) {
                m_overviewReply->setProperty("myStocksIgnoreAbort", true);
                m_overviewReply->abort();
                m_overviewReply = nullptr;
            }
            if (m_distributionReply) {
                m_distributionReply->setProperty("myStocksIgnoreAbort", true);
                m_distributionReply->abort();
                m_distributionReply = nullptr;
            }
            if (m_turnoverReply) {
                m_turnoverReply->setProperty("myStocksIgnoreAbort", true);
                m_turnoverReply->abort();
                m_turnoverReply = nullptr;
            }
        } else {
            qInfo() << "[MarketBreadth] request already in flight, skip restart";
            return;
        }
    }

    ++m_requestToken;
    const int token = m_requestToken;
    m_pendingSnapshot = MarketBreadthSnapshot{};
    m_overviewDone = false;
    m_distributionDone = false;
    m_turnoverDone = false;
    m_pendingErrors.clear();

    QNetworkRequest overviewReq(QUrl(QStringLiteral(
        "https://dq.10jqka.com.cn/fuyao/market_analysis_api/chart/v1/get_chart_data?chart_key=limit_up_minute"
    )));
    applyTonghuashunCommonHeaders(&overviewReq, m_userAgent);
    const network_logger::RequestTrace overviewTrace = network_logger::logRequestStart(
        QStringLiteral("market-overview"),
        QStringLiteral("GET"),
        overviewReq,
        m_proxy
    );

    m_overviewReply = m_nam.get(overviewReq);
    QNetworkReply* overviewReply = m_overviewReply;
    connect(overviewReply, &QNetworkReply::finished, this, [this, overviewReply, token, overviewTrace]() {
        if (overviewReply == m_overviewReply) {
            m_overviewReply = nullptr;
        }

        if (overviewReply->property("myStocksIgnoreAbort").toBool()) {
            overviewReply->deleteLater();
            return;
        }

        const QString networkError = (overviewReply->error() == QNetworkReply::NoError)
            ? QString()
            : overviewReply->errorString();
        const QByteArray body = overviewReply->readAll();

        network_logger::logRequestFinish(overviewTrace, overviewReply, body.size(), body);
        overviewReply->deleteLater();

        if (token != m_requestToken) {
            return;
        }

        QString errorMessage;
        if (!networkError.isEmpty()) {
            errorMessage = QStringLiteral("market overview request failed: %1").arg(networkError);
        } else {
            errorMessage = parseMarketOverviewPayload(body, &m_pendingSnapshot);
            if (errorMessage.isEmpty()) {
                markSnapshotUpdatedNow(&m_pendingSnapshot);
            }
        }
        if (!errorMessage.isEmpty()) {
            m_pendingErrors.push_back(errorMessage);
        }

        m_overviewDone = true;
        finalizeFetch(token);
    });

    QNetworkRequest distributionReq(QUrl(QStringLiteral(
        "https://dq.10jqka.com.cn/fuyao/up_down_distribution/distribution/v2/realtime"
    )));
    applyTonghuashunCommonHeaders(&distributionReq, m_userAgent);
    const network_logger::RequestTrace distributionTrace = network_logger::logRequestStart(
        QStringLiteral("market-distribution"),
        QStringLiteral("GET"),
        distributionReq,
        m_proxy
    );

    m_distributionReply = m_nam.get(distributionReq);
    QNetworkReply* distributionReply = m_distributionReply;
    connect(
        distributionReply,
        &QNetworkReply::finished,
        this,
        [this, distributionReply, token, distributionTrace]() {
            if (distributionReply == m_distributionReply) {
                m_distributionReply = nullptr;
            }

            if (distributionReply->property("myStocksIgnoreAbort").toBool()) {
                distributionReply->deleteLater();
                return;
            }

            const QString networkError = (distributionReply->error() == QNetworkReply::NoError)
                ? QString()
                : distributionReply->errorString();
            const QByteArray body = distributionReply->readAll();

            network_logger::logRequestFinish(distributionTrace, distributionReply, body.size(), body);
            distributionReply->deleteLater();

            if (token != m_requestToken) {
                return;
            }

            QString errorMessage;
            if (!networkError.isEmpty()) {
                errorMessage = QStringLiteral("market distribution request failed: %1").arg(networkError);
            } else {
                errorMessage = parseMarketDistributionPayload(body, &m_pendingSnapshot);
                if (errorMessage.isEmpty()) {
                    markSnapshotUpdatedNow(&m_pendingSnapshot);
                }
            }
            if (!errorMessage.isEmpty()) {
                m_pendingErrors.push_back(errorMessage);
            }

            m_distributionDone = true;
            finalizeFetch(token);
        }
    );

    QNetworkRequest turnoverReq(QUrl(QStringLiteral(
        "https://dq.10jqka.com.cn/fuyao/market_analysis_api/chart/v1/get_chart_data?chart_key=turnover_minute"
    )));
    applyTonghuashunCommonHeaders(&turnoverReq, m_userAgent);
    const network_logger::RequestTrace turnoverTrace = network_logger::logRequestStart(
        QStringLiteral("market-turnover"),
        QStringLiteral("GET"),
        turnoverReq,
        m_proxy
    );

    m_turnoverReply = m_nam.get(turnoverReq);
    QNetworkReply* turnoverReply = m_turnoverReply;
    connect(turnoverReply, &QNetworkReply::finished, this, [this, turnoverReply, token, turnoverTrace]() {
        if (turnoverReply == m_turnoverReply) {
            m_turnoverReply = nullptr;
        }

        if (turnoverReply->property("myStocksIgnoreAbort").toBool()) {
            turnoverReply->deleteLater();
            return;
        }

        const QString networkError = (turnoverReply->error() == QNetworkReply::NoError)
            ? QString()
            : turnoverReply->errorString();
        const QByteArray body = turnoverReply->readAll();

        network_logger::logRequestFinish(turnoverTrace, turnoverReply, body.size(), body);
        turnoverReply->deleteLater();

        if (token != m_requestToken) {
            return;
        }

        QString errorMessage;
        if (!networkError.isEmpty()) {
            errorMessage = QStringLiteral("market turnover request failed: %1").arg(networkError);
        } else {
            errorMessage = parseMarketTurnoverPayload(body, &m_pendingSnapshot);
            if (errorMessage.isEmpty()) {
                markSnapshotUpdatedNow(&m_pendingSnapshot);
            }
        }
        if (!errorMessage.isEmpty()) {
            m_pendingErrors.push_back(errorMessage);
        }

        m_turnoverDone = true;
        finalizeFetch(token);
    });
}

void AshareMarketBreadthProvider::finalizeFetch(int token) {
    if (token != m_requestToken || !m_overviewDone || !m_distributionDone || !m_turnoverDone) {
        return;
    }

    emit dataReady(m_pendingSnapshot);

    const QString errorMessage = m_pendingErrors.join(QStringLiteral("; "));
    if (!errorMessage.isEmpty()) {
        if (errorMessage != m_lastError) {
            emit error(errorMessage);
            m_lastError = errorMessage;
        }
        return;
    }

    if (m_pendingSnapshot.breadthValid && m_pendingSnapshot.turnoverValid) {
        m_cachedSnapshot = m_pendingSnapshot;
        m_cacheExpiresAtMs = QDateTime::currentMSecsSinceEpoch() + app_constants::kNetworkCacheTtlMs;
        m_cacheValid = true;
    }

    m_lastError.clear();
}


TonghuashunStockHeatProvider::TonghuashunStockHeatProvider(QObject* parent)
    : QObject(parent) {}

void TonghuashunStockHeatProvider::applyConfig(const AppConfig& cfg) {
    m_userAgent = network_utils::effectiveUserAgent(cfg);
    m_proxy = network_utils::proxyFromConfig(cfg);

    m_hourCacheValid = false;
    m_dayCacheValid = false;
    m_hourCacheExpiresAtMs = 0;
    m_dayCacheExpiresAtMs = 0;
    m_hourCacheRequestKey.clear();
    m_dayCacheRequestKey.clear();

    if (m_hourReply) {
        QNetworkReply* pendingReply = m_hourReply;
        m_hourReply = nullptr;
        pendingReply->abort();
        pendingReply->deleteLater();
    }
    if (m_dayReply) {
        QNetworkReply* pendingReply = m_dayReply;
        m_dayReply = nullptr;
        pendingReply->abort();
        pendingReply->deleteLater();
    }
    m_hourInFlightRequestKey.clear();
    m_dayInFlightRequestKey.clear();
}

void TonghuashunStockHeatProvider::fetchHourHotStocks(int limit, bool forceRefresh) {
    fetchHotStocks(QStringLiteral("hour"), limit, forceRefresh);
}

void TonghuashunStockHeatProvider::fetchDayHotStocks(int limit, bool forceRefresh) {
    fetchHotStocks(QStringLiteral("day"), limit, forceRefresh);
}

void TonghuashunStockHeatProvider::fetchHotStocks(
    const QString& periodType,
    int limit,
    bool forceRefresh
) {
    m_nam.setProxy(m_proxy);

    const QString normalizedPeriod = periodType == QLatin1String("day")
        ? QStringLiteral("day")
        : QStringLiteral("hour");
    const int effectiveLimit = qBound(1, limit, 100);
    const QString requestKey = QString::number(effectiveLimit);

    QVector<HotRankItem>& cachedItems = normalizedPeriod == QLatin1String("day")
        ? m_cachedDayItems
        : m_cachedHourItems;
    QString& cacheRequestKey = normalizedPeriod == QLatin1String("day")
        ? m_dayCacheRequestKey
        : m_hourCacheRequestKey;
    qint64& cacheExpiresAtMs = normalizedPeriod == QLatin1String("day")
        ? m_dayCacheExpiresAtMs
        : m_hourCacheExpiresAtMs;
    bool& cacheValid = normalizedPeriod == QLatin1String("day")
        ? m_dayCacheValid
        : m_hourCacheValid;
    QNetworkReply*& inFlightReply = normalizedPeriod == QLatin1String("day")
        ? m_dayReply
        : m_hourReply;
    QString& inFlightRequestKey = normalizedPeriod == QLatin1String("day")
        ? m_dayInFlightRequestKey
        : m_hourInFlightRequestKey;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!forceRefresh && cacheValid && cacheRequestKey == requestKey && nowMs < cacheExpiresAtMs) {
        qInfo() << "[StockHeat] cache hit type=" << normalizedPeriod
                << "ttl_ms=" << (cacheExpiresAtMs - nowMs);
        if (normalizedPeriod == QLatin1String("day")) {
            emit dayHotStocksReady(cachedItems);
        } else {
            emit hourHotStocksReady(cachedItems);
        }
        return;
    }

    SharedHotRankCacheEntry& sharedCache = sharedStockHeatCache(normalizedPeriod);
    if (!forceRefresh
        && sharedCache.valid
        && sharedCache.requestKey == requestKey
        && nowMs < sharedCache.expiresAtMs) {
        cachedItems = sharedCache.items;
        cacheRequestKey = sharedCache.requestKey;
        cacheExpiresAtMs = sharedCache.expiresAtMs;
        cacheValid = true;

        qInfo() << "[StockHeat] shared cache hit type=" << normalizedPeriod
                << "ttl_ms=" << (sharedCache.expiresAtMs - nowMs);
        if (normalizedPeriod == QLatin1String("day")) {
            emit dayHotStocksReady(cachedItems);
        } else {
            emit hourHotStocksReady(cachedItems);
        }
        return;
    }

    if (inFlightReply && !forceRefresh) {
        qInfo() << "[StockHeat] request already in flight type=" << normalizedPeriod
                << "skip";
        return;
    }

    if (forceRefresh && inFlightReply) {
        inFlightReply->setProperty("myStocksIgnoreAbort", true);
        inFlightReply->abort();
        inFlightReply = nullptr;
        inFlightRequestKey.clear();
    }

    inFlightRequestKey = requestKey;

    QUrl url(QStringLiteral("https://dq.10jqka.com.cn/fuyao/hot_list_data/out/hot_list/v1/stock"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("stock_type"), QStringLiteral("a"));
    query.addQueryItem(QStringLiteral("type"), normalizedPeriod);
    query.addQueryItem(QStringLiteral("list_type"), QStringLiteral("normal"));
    url.setQuery(query);

    QNetworkRequest req(url);
    applyTonghuashunCommonHeaders(&req, m_userAgent);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        normalizedPeriod == QLatin1String("day")
            ? QStringLiteral("tonghuashun-stock-heat-day")
            : QStringLiteral("tonghuashun-stock-heat-hour"),
        QStringLiteral("GET"),
        req,
        m_proxy
    );

    inFlightReply = m_nam.get(req);
    QNetworkReply* reply = inFlightReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, normalizedPeriod, trace, requestKey]() {
        if (normalizedPeriod == QLatin1String("day")) {
            if (reply == m_dayReply) {
                m_dayReply = nullptr;
            }
            m_dayInFlightRequestKey.clear();
        } else {
            if (reply == m_hourReply) {
                m_hourReply = nullptr;
            }
            m_hourInFlightRequestKey.clear();
        }

        if (reply->property("myStocksIgnoreAbort").toBool()) {
            reply->deleteLater();
            return;
        }

        const QString err = (reply->error() == QNetworkReply::NoError)
            ? QString()
            : reply->errorString();
        const QByteArray body = reply->readAll();

        network_logger::logRequestFinish(trace, reply, body.size(), body);

        reply->deleteLater();
        handleHotStocksResponse(normalizedPeriod, body, err, requestKey);
    });
}

void TonghuashunStockHeatProvider::handleHotStocksResponse(
    const QString& periodType,
    const QByteArray& body,
    const QString& errorText,
    const QString& requestKey
) {
    QString errorMessage;
    QVector<HotRankItem> items;

    if (!errorText.isEmpty()) {
        errorMessage = QStringLiteral("stock heat request failed: %1").arg(errorText);
    } else {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            errorMessage = QStringLiteral("stock heat json parse error: %1")
                .arg(parseError.errorString());
        } else if (!doc.isObject()) {
            errorMessage = QStringLiteral("stock heat invalid payload");
        } else {
            const QJsonObject root = doc.object();
            const int statusCode = root.value(QStringLiteral("status_code")).toInt(-1);
            if (statusCode != 0) {
                const QString statusMsg = root.value(QStringLiteral("status_msg")).toString().trimmed();
                errorMessage = statusMsg.isEmpty()
                    ? QStringLiteral("stock heat status_code=%1").arg(statusCode)
                    : QStringLiteral("stock heat status_code=%1 msg=%2").arg(statusCode).arg(statusMsg);
            } else if (!root.value(QStringLiteral("data")).isObject()) {
                errorMessage = QStringLiteral("stock heat data missing");
            } else {
                const QJsonObject data = root.value(QStringLiteral("data")).toObject();
                const QJsonArray stockList = data.value(QStringLiteral("stock_list")).toArray();
                items.reserve(qMin(stockList.size(), 100));

                for (const QJsonValue& stockValue : stockList) {
                    if (!stockValue.isObject()) {
                        continue;
                    }

                    const QJsonObject row = stockValue.toObject();
                    HotRankItem item;
                    item.code = row.value(QStringLiteral("code")).toString().trimmed();
                    item.name = row.value(QStringLiteral("name")).toString().trimmed();
                    item.watchCode = watchlist_utils::normalizeApiWatchCode(item.code);
                    item.pct = firstNumberFromObject(row, {QStringLiteral("rise_and_fall")});
                    item.heat = firstNumberFromObject(row, {QStringLiteral("rate")});
                    item.tags = extractTonghuashunStockTags(row);
                    if (!item.code.isEmpty() && !item.name.isEmpty()) {
                        items.push_back(item);
                    }
                    if (items.size() >= requestKey.toInt()) {
                        break;
                    }
                }
            }
        }
    }

    if (!errorMessage.isEmpty()) {
        if (errorMessage != m_lastError) {
            emit error(errorMessage);
            m_lastError = errorMessage;
        }
        return;
    }

    m_lastError.clear();
    QVector<HotRankItem>& cachedItems = periodType == QLatin1String("day")
        ? m_cachedDayItems
        : m_cachedHourItems;
    QString& cacheRequestKey = periodType == QLatin1String("day")
        ? m_dayCacheRequestKey
        : m_hourCacheRequestKey;
    qint64& cacheExpiresAtMs = periodType == QLatin1String("day")
        ? m_dayCacheExpiresAtMs
        : m_hourCacheExpiresAtMs;
    bool& cacheValid = periodType == QLatin1String("day")
        ? m_dayCacheValid
        : m_hourCacheValid;

    cachedItems = items;
    cacheRequestKey = requestKey;
    cacheExpiresAtMs = QDateTime::currentMSecsSinceEpoch() + app_constants::kNetworkCacheTtlMs;
    cacheValid = true;

    SharedHotRankCacheEntry& sharedCache = sharedStockHeatCache(periodType);
    sharedCache.items = items;
    sharedCache.requestKey = requestKey;
    sharedCache.expiresAtMs = cacheExpiresAtMs;
    sharedCache.valid = true;

    if (periodType == QLatin1String("day")) {
        emit dayHotStocksReady(items);
    } else {
        emit hourHotStocksReady(items);
    }
}

EastMoneyIndexQuoteProvider::EastMoneyIndexQuoteProvider(QObject* parent)
    : QObject(parent) {}

void EastMoneyIndexQuoteProvider::applyConfig(const AppConfig& cfg) {
    m_userAgent = network_utils::effectiveUserAgent(cfg);
    m_proxy = network_utils::proxyFromConfig(cfg);
    m_cacheValid = false;
    m_cacheExpiresAtMs = 0;
    if (m_reply) {
        m_reply->setProperty("myStocksIgnoreAbort", true);
        m_reply->abort();
        m_reply = nullptr;
    }
}

void EastMoneyIndexQuoteProvider::fetch(bool forceRefresh) {
    m_nam.setProxy(m_proxy);

    if (forceRefresh) {
        m_cacheValid = false;
        m_cacheExpiresAtMs = 0;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!forceRefresh && m_cacheValid && nowMs < m_cacheExpiresAtMs) {
        qInfo() << "[IndexQuote] cache hit ttl_ms=" << (m_cacheExpiresAtMs - nowMs);
        emit dataReady(m_cachedItems);
        return;
    }

    if (m_reply) {
        if (forceRefresh) {
            m_reply->setProperty("myStocksIgnoreAbort", true);
            m_reply->abort();
            m_reply = nullptr;
        } else {
            qInfo() << "[IndexQuote] request already in flight, skip";
            return;
        }
    }

    const QVector<QPair<QString, QString>>& config = indexQuoteConfig();
    QStringList secIds;
    secIds.reserve(config.size());
    for (const auto& pair : config) {
        secIds.push_back(pair.first);
    }

    QUrl url(QStringLiteral("https://push2delay.eastmoney.com/api/qt/ulist.np/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("fltt"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("f2,f3,f4,f12,f13"));
    query.addQueryItem(QStringLiteral("secids"), secIds.join(QLatin1Char(',')));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader(headers::kUserAgent, m_userAgent.toUtf8());
    req.setRawHeader(headers::kReferer, headers::kEastMoneyReferer);
    req.setRawHeader(headers::kAccept, "*/*");
    req.setRawHeader(headers::kConnection, "keep-alive");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        QStringLiteral("eastmoney-index-quote"),
        QStringLiteral("GET"),
        req,
        m_proxy
    );

    m_reply = m_nam.get(req);
    QNetworkReply* reply = m_reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, trace]() {
        if (reply == m_reply) {
            m_reply = nullptr;
        }
        if (reply->property("myStocksIgnoreAbort").toBool()) {
            reply->deleteLater();
            return;
        }
        const QString err = (reply->error() == QNetworkReply::NoError)
            ? QString()
            : reply->errorString();
        const QByteArray body = reply->readAll();
        network_logger::logRequestFinish(trace, reply, body.size(), body);
        reply->deleteLater();
        handleResponse(body, err);
    });
}

void EastMoneyIndexQuoteProvider::handleResponse(
    const QByteArray& body,
    const QString& errorText
) {
    if (!errorText.isEmpty()) {
        const QString msg = QStringLiteral("index quote request failed: %1").arg(errorText);
        if (msg != m_lastError) {
            emit error(msg);
            m_lastError = msg;
        }
        return;
    }

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError) {
        const QString msg = QStringLiteral("index quote json parse error: %1").arg(pe.errorString());
        if (msg != m_lastError) {
            emit error(msg);
            m_lastError = msg;
        }
        return;
    }
    if (!doc.isObject()) {
        emit error(QStringLiteral("index quote invalid payload"));
        return;
    }

    const QJsonObject root = doc.object();
    if (!root.value(QStringLiteral("data")).isObject()) {
        emit error(QStringLiteral("index quote data missing"));
        return;
    }

    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    const QVector<QJsonObject> diff = extractDiffObjects(data.value(QStringLiteral("diff")));

    struct QuoteData {
        double price = qQNaN();
        double change = qQNaN();
        double pct = qQNaN();
    };
    QHash<QString, QuoteData> lookup;
    lookup.reserve(diff.size());
    for (const QJsonObject& row : diff) {
        const QString key = secIdKeyFromDiffItem(row);
        if (key.isEmpty()) {
            continue;
        }
        QuoteData qd;
        qd.price = firstNumberFromObject(row, {QStringLiteral("f2")});
        qd.pct = normalizeEastMoneyPercent(firstNumberFromObject(row, {QStringLiteral("f3")}));
        qd.change = firstNumberFromObject(row, {QStringLiteral("f4")});
        lookup.insert(key, qd);
    }

    const QVector<QPair<QString, QString>>& config = indexQuoteConfig();
    QVector<IndexQuoteItem> items;
    items.reserve(config.size());
    for (const auto& pair : config) {
        const QString key = secIdKey(pair.first);
        IndexQuoteItem item;
        item.code = pair.first;
        item.displayName = pair.second;
        const QuoteData qd = lookup.value(key);
        item.price = qd.price;
        item.change = qd.change;
        item.pct = qd.pct;
        items.push_back(item);
    }

    m_lastError.clear();
    m_cachedItems = items;
    m_cacheExpiresAtMs = QDateTime::currentMSecsSinceEpoch() + app_constants::kNetworkCacheTtlMs;
    m_cacheValid = true;
    emit dataReady(items);
}
