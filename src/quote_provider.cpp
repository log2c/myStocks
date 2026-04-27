#include "quote_provider.h"

#include "i18n.h"
#include "network_logger.h"
#include "network_utils.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QStringConverter>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {

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

bool isLikelyEtfCode(const QString& code, const QString& marketPrefix) {
    if (code.size() != 6 || !isDigitsOnly(code)) {
        return false;
    }

    if (marketPrefix == QStringLiteral("sh")) {
        return code.startsWith(QLatin1Char('5'));
    }

    if (marketPrefix == QStringLiteral("sz")) {
        return code.startsWith(QStringLiteral("15"))
            || code.startsWith(QStringLiteral("16"));
    }

    return false;
}

bool isXTickQuoteObject(const QJsonObject& obj) {
    if (!obj.contains(QStringLiteral("code"))) {
        return false;
    }

    return obj.contains(QStringLiteral("lastPrice"))
        || obj.contains(QStringLiteral("price"))
        || obj.contains(QStringLiteral("close"))
        || obj.contains(QStringLiteral("x003"));
}

QString xtickResponseMessage(const QJsonObject& obj) {
    const QStringList keys {
        QStringLiteral("message"),
        QStringLiteral("msg"),
        QStringLiteral("error"),
        QStringLiteral("err"),
    };

    for (const QString& key : keys) {
        const QString message = obj.value(key).toString().trimmed();
        if (!message.isEmpty()) {
            return message;
        }
    }

    return {};
}

QJsonArray extractArray(const QJsonDocument& doc) {
    if (doc.isArray()) {
        return doc.array();
    }

    if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        if (obj.value("data").isArray()) {
            return obj.value("data").toArray();
        }
        if (obj.value("data").isObject()) {
            return QJsonArray {obj.value("data").toObject()};
        }
        if (obj.value("result").isArray()) {
            return obj.value("result").toArray();
        }
        if (obj.value("result").isObject()) {
            return QJsonArray {obj.value("result").toObject()};
        }
        if (isXTickQuoteObject(obj)) {
            return QJsonArray {obj};
        }
    }

    return {};
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

QString decodeMarketName(const QByteArray& raw) {
    if (raw.isEmpty()) {
        return {};
    }

    // Try UTF-8 first (some APIs return UTF-8).
    QStringDecoder utf8Decoder(QStringConverter::Utf8);
    const QString utf8 = utf8Decoder(raw);
    if (!utf8Decoder.hasError()) {
        return utf8;
    }

    // Decode as GBK/GB18030 using platform-native API
    // (QStringConverter lacks built-in GBK support in Qt6 without ICU).
#ifdef Q_OS_WIN
    const int wlen = MultiByteToWideChar(
        936, 0, raw.constData(), raw.size(), nullptr, 0
    );
    if (wlen > 0) {
        QString result(wlen, Qt::Uninitialized);
        MultiByteToWideChar(
            936, 0, raw.constData(), raw.size(),
            reinterpret_cast<wchar_t*>(result.data()), wlen
        );
        return result;
    }
#elif defined(Q_OS_MACOS)
    const CFDataRef cfData = CFDataCreateWithBytesNoCopy(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(raw.constData()),
        raw.size(),
        kCFAllocatorNull
    );
    if (cfData) {
        const CFStringRef cfStr = CFStringCreateFromExternalRepresentation(
            kCFAllocatorDefault, cfData, kCFStringEncodingGB_18030_2000
        );
        CFRelease(cfData);
        if (cfStr) {
            const QString result = QString::fromCFString(cfStr);
            CFRelease(cfStr);
            return result;
        }
    }
#else
    const auto gbEnc = QStringConverter::encodingForName("GB18030");
    if (gbEnc) {
        QStringDecoder gbDecoder(*gbEnc);
        const QString gbText = gbDecoder(raw);
        if (!gbDecoder.hasError()) {
            return gbText;
        }
    }
#endif

    return QString::fromLatin1(raw);
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

MockQuoteProvider::MockQuoteProvider(QObject* parent)
    : IQuoteProvider(parent) {}

void MockQuoteProvider::fetchQuotes(const QVector<StockItem>& stocks) {
    QVector<QuoteItem> out;
    out.reserve(stocks.size());

    for (const StockItem& s : stocks) {
        const double pre = m_lastPrice.value(
            s.code,
            10.0 + QRandomGenerator::global()->bounded(90.0)
        );

        const double pctStep = (QRandomGenerator::global()->generateDouble() * 2.0 - 1.0) * 2.0;
        const double now = qMax(0.01, pre * (1.0 + pctStep / 100.0));

        QuoteItem q;
        q.code = s.code;
        q.name = s.name;
        q.price = now;
        q.change = now - pre;
        q.pct = qFuzzyIsNull(pre) ? 0.0 : (q.change / pre * 100.0);

        out.push_back(q);
        m_lastPrice[s.code] = now;
    }

    emit quotesReady(out);
}

XTickQuoteProvider::XTickQuoteProvider(const QString& token, QObject* parent)
    : IQuoteProvider(parent)
    , m_token(token) {}

void XTickQuoteProvider::setToken(const QString& token) {
    m_token = token;
}

void XTickQuoteProvider::setLanguage(const QString& language) {
    m_language = i18n::resolveLanguage(language);
}

void XTickQuoteProvider::fetchQuotes(const QVector<StockItem>& stocks) {
    m_nam.setProxy(m_proxy);

    if (m_token.trimmed().isEmpty()) {
        const QString message = i18n::t("provider.xtick.token.empty", m_language);
        if (message != m_lastError) {
            emit error(message);
            m_lastError = message;
        }
        return;
    }

    m_pendingRequests = 0;
    m_errors.clear();
    m_reqMap.clear();
    m_codeFallback.clear();
    m_buffer.clear();

    QHash<int, QSet<QString>> grouped;

    for (const StockItem& s : stocks) {
        const int type = inferType(s.code);
        if (type < 0) {
            continue;
        }

        const QString code = normalizeCode(s.code);
        if (code.isEmpty()) {
            continue;
        }

        const QString key = makeKey(type, code);
        grouped[type].insert(code);
        m_reqMap.insert(key, s);

        if (!m_codeFallback.contains(key)) {
            m_codeFallback.insert(key, s);
        }
    }

    if (grouped.isEmpty()) {
        emit quotesReady({});
        return;
    }

    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        const int type = it.key();
        QStringList codes = it.value().values();
        codes.sort();

        QUrl url("http://api.xtick.top/doc/order/time");
        QUrlQuery query;
        query.addQueryItem("type", QString::number(type));
        query.addQueryItem("code", codes.join(','));
        query.addQueryItem("period", "lv1");
        query.addQueryItem("token", m_token);
        url.setQuery(query);

        QNetworkRequest req(url);
        req.setRawHeader("User-Agent", m_userAgent.toUtf8());
        req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

        const network_logger::RequestTrace trace = network_logger::logRequestStart(
            "xtick",
            "GET",
            req,
            m_proxy
        );

        QNetworkReply* reply = m_nam.get(req);
        ++m_pendingRequests;

        connect(reply, &QNetworkReply::finished, this, [this, reply, type, trace]() {
            const QString err = (reply->error() == QNetworkReply::NoError)
                ? QString()
                : reply->errorString();
            const QByteArray body = reply->readAll();

            network_logger::logRequestFinish(trace, reply, body.size(), body);

            reply->deleteLater();
            handleResponse(type, body, err);
        });
    }
}

QString XTickQuoteProvider::normalizeCode(const QString& raw) {
    const QString code = raw.trimmed().toLower();
    if (code.isEmpty()) {
        return {};
    }

    if (code.startsWith(QStringLiteral("hk"))) {
        QString digits = digitsOnly(code);
        if (digits.isEmpty()) {
            return {};
        }
        if (digits.size() > 5) {
            digits = digits.right(5);
        }
        return digits.rightJustified(5, QLatin1Char('0'));
    }

    if (code.startsWith(QStringLiteral("sh"))
        || code.startsWith(QStringLiteral("sz"))
        || code.startsWith(QStringLiteral("bj"))) {
        QString digits = digitsOnly(code);
        if (digits.isEmpty()) {
            return {};
        }
        if (digits.size() > 6) {
            digits = digits.right(6);
        }
        return digits.rightJustified(6, QLatin1Char('0'));
    }

    if (code.size() == 6 && isDigitsOnly(code)) {
        return code;
    }

    return {};
}

int XTickQuoteProvider::inferType(const QString& rawCode) {
    const QString raw = rawCode.trimmed().toLower();
    const QString code = normalizeCode(raw);

    if (code.isEmpty()) {
        return -1;
    }

    if (raw.startsWith(QStringLiteral("hk"))) {
        return 3;
    }

    QString marketPrefix;
    if (raw.startsWith(QStringLiteral("sh"))
        || raw.startsWith(QStringLiteral("sz"))
        || raw.startsWith(QStringLiteral("bj"))) {
        marketPrefix = raw.left(2);
    }

    if (marketPrefix == QStringLiteral("sh")
        && (code.startsWith(QStringLiteral("000")) || code.startsWith(QStringLiteral("880")))) {
        return 10;
    }
    if (marketPrefix == QStringLiteral("sz") && code.startsWith(QStringLiteral("399"))) {
        return 10;
    }

    if (isLikelyEtfCode(code, marketPrefix)) {
        return 20;
    }

    if (marketPrefix.isEmpty() && code.startsWith(QStringLiteral("399"))) {
        return 10;
    }

    return 1;
}

QString XTickQuoteProvider::makeKey(int type, const QString& code) {
    return QString::number(type) + ':' + code;
}

void XTickQuoteProvider::handleResponse(int reqType, const QByteArray& body, const QString& errorText) {
    if (!errorText.isEmpty()) {
        m_errors << errorText;
    } else {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError) {
            m_errors << ("json parse error: " + pe.errorString());
        } else {
            const QJsonArray arr = extractArray(doc);

            if (doc.isObject()) {
                const QJsonObject root = doc.object();
                const QString message = xtickResponseMessage(root);

                bool hasStatusCode = false;
                int statusCode = 0;
                const QJsonValue codeValue = root.value(QStringLiteral("code"));
                if (codeValue.isDouble()) {
                    hasStatusCode = true;
                    statusCode = codeValue.toInt();
                } else if (codeValue.isString()) {
                    bool ok = false;
                    const int parsed = codeValue.toString().trimmed().toInt(&ok);
                    if (ok) {
                        hasStatusCode = true;
                        statusCode = parsed;
                    }
                }

                if (hasStatusCode && statusCode != 0 && !isXTickQuoteObject(root)) {
                    const QString suffix = message.isEmpty() ? QString() : (QStringLiteral(" msg=") + message);
                    m_errors << QString("xtick code=%1%2").arg(statusCode).arg(suffix);
                } else if (arr.isEmpty() && !isXTickQuoteObject(root) && !message.isEmpty()) {
                    m_errors << QString("xtick: %1").arg(message);
                }
            }

            for (const QJsonValue& v : arr) {
                if (!v.isObject()) {
                    continue;
                }

                const QJsonObject obj = v.toObject();
                QString normCode;
                if (reqType == 3) {
                    QString hkDigits = digitsOnly(obj.value("code").toString());
                    if (!hkDigits.isEmpty()) {
                        if (hkDigits.size() > 5) {
                            hkDigits = hkDigits.right(5);
                        }
                        normCode = hkDigits.rightJustified(5, QLatin1Char('0'));
                    }
                } else {
                    normCode = normalizeCode(obj.value("code").toString());
                }

                if (normCode.isEmpty() && obj.value("code").isDouble()) {
                    const int width = (reqType == 3) ? 5 : 6;
                    normCode = QString("%1").arg(
                        static_cast<int>(obj.value("code").toDouble()),
                        width,
                        10,
                        QChar('0')
                    );
                }

                if (normCode.isEmpty()) {
                    continue;
                }

                StockItem stock;
                bool found = false;

                const QString key = makeKey(reqType, normCode);
                if (m_reqMap.contains(key)) {
                    stock = m_reqMap.value(key);
                    found = true;
                } else if (m_codeFallback.contains(key)) {
                    stock = m_codeFallback.value(key);
                    found = true;
                }

                if (!found) {
                    continue;
                }

                const double last = firstNumberFromObject(obj, {"lastPrice", "price", "close", "x003"});
                double pre = firstNumberFromObject(obj, {"lastClose", "preClose", "close", "x002"});

                if (std::isnan(last)) {
                    continue;
                }
                if (std::isnan(pre) || qFuzzyIsNull(pre)) {
                    pre = last;
                }

                QuoteItem q;
                q.code = stock.code;
                const QString remoteName = firstNonEmptyStringFromObject(
                    obj,
                    {"name", "stockName", "codeName", "cname", "x001", "n"}
                );
                q.name = remoteName;
                q.price = last;
                q.change = last - pre;
                q.pct = qFuzzyIsNull(pre) ? 0.0 : (q.change / pre * 100.0);

                m_buffer.insert(q.code, q);
            }
        }
    }

    --m_pendingRequests;
    if (m_pendingRequests <= 0) {
        if (m_buffer.isEmpty() && !m_errors.isEmpty()) {
            const QString message = m_errors.join(" | ");
            if (message != m_lastError) {
                emit error(message);
                m_lastError = message;
            }
        } else {
            m_lastError.clear();
        }
        emit quotesReady(toVector(m_buffer));
    }
}

SinaQuoteProvider::SinaQuoteProvider(QObject* parent)
    : IQuoteProvider(parent) {}

void SinaQuoteProvider::fetchQuotes(const QVector<StockItem>& stocks) {
    m_nam.setProxy(m_proxy);
    m_symbolMap.clear();

    QStringList symbols;
    symbols.reserve(stocks.size());

    for (const StockItem& s : stocks) {
        const QString symbol = toCnSymbol(s.code);
        if (symbol.isEmpty()) {
            continue;
        }
        symbols.push_back(symbol);
        m_symbolMap.insert(symbol, s);
    }

    if (symbols.isEmpty()) {
        emit quotesReady({});
        return;
    }

    QUrl url(QString("https://hq.sinajs.cn/list=%1").arg(symbols.join(',')));
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", m_userAgent.toUtf8());
    req.setRawHeader("Referer", "https://finance.sina.com.cn");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        "sina",
        "GET",
        req,
        m_proxy
    );

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, trace]() {
        const QString err = (reply->error() == QNetworkReply::NoError)
            ? QString()
            : reply->errorString();
        const QByteArray body = reply->readAll();

        network_logger::logRequestFinish(trace, reply, body.size(), body);

        reply->deleteLater();
        handleResponse(body, err);
    });
}

QString SinaQuoteProvider::toCnSymbol(const QString& rawCode) {
    const QString symbol = normalizedSymbol(rawCode);
    if (symbol.startsWith("hk")) {
        return "rt_" + symbol;
    }
    return symbol;
}

double SinaQuoteProvider::parseFieldNumber(const QList<QByteArray>& fields, int index) {
    if (index < 0 || index >= fields.size()) {
        return qQNaN();
    }
    bool ok = false;
    const double v = QString::fromLatin1(fields.at(index).trimmed()).toDouble(&ok);
    return ok ? v : qQNaN();
}

void SinaQuoteProvider::handleResponse(const QByteArray& body, const QString& errorText) {
    if (!errorText.isEmpty()) {
        emit error(errorText);
        return;
    }

    QHash<QString, QuoteItem> outMap;

    const QList<QByteArray> lines = body.split('\n');
    for (const QByteArray& lineRaw : lines) {
        const QByteArray line = lineRaw.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        static const QByteArray kPrefix("var hq_str_");
        if (!line.startsWith(kPrefix)) {
            continue;
        }

        const int eq = line.indexOf("=\"");
        if (eq <= static_cast<int>(kPrefix.size())) {
            continue;
        }

        const QString symbol = QString::fromLatin1(line.mid(kPrefix.size(), eq - kPrefix.size()));
        const int payloadStart = eq + 2;
        const int payloadEnd = line.lastIndexOf('"');
        if (payloadEnd <= payloadStart) {
            continue;
        }

        const QByteArray payload = line.mid(payloadStart, payloadEnd - payloadStart);
        const QList<QByteArray> fields = payload.split(',');

        const double pre = parseFieldNumber(fields, 2);
        double last = parseFieldNumber(fields, 3);
        if (std::isnan(last)) {
            last = parseFieldNumber(fields, 1);
        }
        if (std::isnan(last)) {
            continue;
        }

        const StockItem stock = m_symbolMap.value(symbol, StockItem{symbol, symbol});
        const QString remoteName = fields.isEmpty()
            ? QString()
            : decodeMarketName(fields.at(0).trimmed()).trimmed();

        QuoteItem q;
        q.code = stock.code;
        q.name = remoteName;
        q.price = last;

        const double preSafe = (std::isnan(pre) || qFuzzyIsNull(pre)) ? last : pre;
        q.change = last - preSafe;
        q.pct = qFuzzyIsNull(preSafe) ? 0.0 : (q.change / preSafe * 100.0);

        outMap.insert(q.code, q);
    }

    emit quotesReady(toVector(outMap));
}

TencentQuoteProvider::TencentQuoteProvider(QObject* parent)
    : IQuoteProvider(parent) {}

void TencentQuoteProvider::fetchQuotes(const QVector<StockItem>& stocks) {
    m_nam.setProxy(m_proxy);
    m_symbolMap.clear();

    QStringList symbols;
    symbols.reserve(stocks.size());

    for (const StockItem& s : stocks) {
        const QString symbol = toCnSymbol(s.code);
        if (symbol.isEmpty()) {
            continue;
        }
        symbols.push_back(symbol);
        m_symbolMap.insert(symbol, s);
    }

    if (symbols.isEmpty()) {
        emit quotesReady({});
        return;
    }

    QUrl url(QString("https://qt.gtimg.cn/q=%1").arg(symbols.join(',')));
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", m_userAgent.toUtf8());
    req.setRawHeader("Referer", "https://gu.qq.com");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        "tencent",
        "GET",
        req,
        m_proxy
    );

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, trace]() {
        const QString err = (reply->error() == QNetworkReply::NoError)
            ? QString()
            : reply->errorString();
        const QByteArray body = reply->readAll();

        network_logger::logRequestFinish(trace, reply, body.size(), body);

        reply->deleteLater();
        handleResponse(body, err);
    });
}

QString TencentQuoteProvider::toCnSymbol(const QString& rawCode) {
    return normalizedSymbol(rawCode);
}

double TencentQuoteProvider::parseFieldNumber(const QList<QByteArray>& fields, int index) {
    if (index < 0 || index >= fields.size()) {
        return qQNaN();
    }
    bool ok = false;
    const double v = QString::fromLatin1(fields.at(index).trimmed()).toDouble(&ok);
    return ok ? v : qQNaN();
}

void TencentQuoteProvider::handleResponse(const QByteArray& body, const QString& errorText) {
    if (!errorText.isEmpty()) {
        emit error(errorText);
        return;
    }

    QHash<QString, QuoteItem> outMap;

    const QList<QByteArray> lines = body.split(';');
    for (const QByteArray& lineRaw : lines) {
        const QByteArray line = lineRaw.trimmed();
        if (line.isEmpty() || !line.startsWith("v_")) {
            continue;
        }

        const int eq = line.indexOf("=\"");
        if (eq <= 2) {
            continue;
        }

        const QString symbol = QString::fromLatin1(line.mid(2, eq - 2));
        const int payloadStart = eq + 2;
        const int payloadEnd = line.lastIndexOf('"');
        if (payloadEnd <= payloadStart) {
            continue;
        }

        const QByteArray payload = line.mid(payloadStart, payloadEnd - payloadStart);
        const QList<QByteArray> fields = payload.split('~');

        const double last = parseFieldNumber(fields, 3);
        const double pre = parseFieldNumber(fields, 4);
        if (std::isnan(last)) {
            continue;
        }

        const StockItem stock = m_symbolMap.value(symbol, StockItem{symbol, symbol});
        const QString remoteName = (fields.size() > 1)
            ? decodeMarketName(fields.at(1).trimmed()).trimmed()
            : QString();

        QuoteItem q;
        q.code = stock.code;
        q.name = remoteName;
        q.price = last;

        const double preSafe = (std::isnan(pre) || qFuzzyIsNull(pre)) ? last : pre;
        q.change = last - preSafe;
        q.pct = qFuzzyIsNull(preSafe) ? 0.0 : (q.change / preSafe * 100.0);

        outMap.insert(q.code, q);
    }

    emit quotesReady(toVector(outMap));
}

EastMoneyQuoteProvider::EastMoneyQuoteProvider(QObject* parent)
    : IQuoteProvider(parent) {}

void EastMoneyQuoteProvider::fetchQuotes(const QVector<StockItem>& stocks) {
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

    QUrl url("https://push2.eastmoney.com/api/qt/ulist.np/get");
    QUrlQuery query;
    query.addQueryItem("secids", secIds.join(','));
    query.addQueryItem("fields", "f12,f13,f14,f2,f3,f4,f18");
    query.addQueryItem("ut", "fa5fd1943c7b386f172d6893dbfba10b");
    query.addQueryItem("invt", "2");
    query.addQueryItem("fltt", "2");
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", m_userAgent.toUtf8());
    req.setRawHeader("Referer", "https://quote.eastmoney.com/");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        "eastmoney",
        "GET",
        req,
        m_proxy
    );

    QNetworkReply* reply = m_nam.get(req);
    ++m_pendingRequests;

    connect(reply, &QNetworkReply::finished, this, [this, reply, requestMap, trace]() {
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
}

void EastMoneyHotRankProvider::fetchHotSectors(
    int limit,
    const QString& sortField,
    const QString& sortOrder
) {
    fetchHotList(false, limit, sortField, sortOrder);
}

void EastMoneyHotRankProvider::fetchHotConcepts(
    int limit,
    const QString& sortField,
    const QString& sortOrder
) {
    fetchHotList(true, limit, sortField, sortOrder);
}

void EastMoneyHotRankProvider::fetchHotList(
    bool concept,
    int limit,
    const QString& sortField,
    const QString& sortOrder
) {
    m_nam.setProxy(m_proxy);

    const QString normalizedSortField = normalizeHotRankSortField(sortField);
    const QString normalizedSortOrder = normalizeHotRankSortOrder(sortOrder);
    const QString fid = normalizedSortField == QLatin1String("pct")
        ? QStringLiteral("f3")
        : QStringLiteral("f62");
    const QString fs = concept
        ? QStringLiteral("m:90+t:3")
        : QStringLiteral("m:90+t:2");

    QUrl url(QStringLiteral("https://push2delay.eastmoney.com/api/qt/clist/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("pn"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("pz"), QString::number(qMax(1, limit)));
    query.addQueryItem(
        QStringLiteral("po"),
        normalizedSortOrder == QLatin1String("asc") ? QStringLiteral("0") : QStringLiteral("1")
    );
    query.addQueryItem(QStringLiteral("np"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("fltt"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("invt"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("fid"), fid);
    query.addQueryItem(QStringLiteral("fs"), fs);
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("f12,f14,f3,f62"));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", m_userAgent.toUtf8());
    req.setRawHeader("Referer", "https://quote.eastmoney.com/");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        concept ? QStringLiteral("eastmoney-hot-concept") : QStringLiteral("eastmoney-hot-sector"),
        QStringLiteral("GET"),
        req,
        m_proxy
    );

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, concept, trace]() {
        const QString err = (reply->error() == QNetworkReply::NoError)
            ? QString()
            : reply->errorString();
        const QByteArray body = reply->readAll();

        network_logger::logRequestFinish(trace, reply, body.size(), body);

        reply->deleteLater();
        handleHotListResponse(concept, body, err);
    });
}

void EastMoneyHotRankProvider::handleHotListResponse(
    bool concept,
    const QByteArray& body,
    const QString& errorText
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
                    item.pct = normalizeEastMoneyPercent(
                        firstNumberFromObject(row, {QStringLiteral("f3")})
                    );
                    item.mainNetInflow = firstNumberFromObject(row, {QStringLiteral("f62")});
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
    if (concept) {
        emit hotConceptsReady(items);
    } else {
        emit hotSectorsReady(items);
    }
}

AshareMarketBreadthProvider::AshareMarketBreadthProvider(QObject* parent)
    : QObject(parent) {}

void AshareMarketBreadthProvider::applyConfig(const AppConfig& cfg) {
    m_userAgent = network_utils::effectiveUserAgent(cfg);
    m_proxy = network_utils::proxyFromConfig(cfg);
}

namespace {

QString parseMarketBreadthPayload(const QByteArray& body, MarketBreadthSnapshot* snapshot) {
    if (!snapshot) {
        return QStringLiteral("market breadth snapshot missing");
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return QStringLiteral("market breadth json parse error: %1")
            .arg(parseError.errorString());
    }
    if (!doc.isObject()) {
        return QStringLiteral("market breadth invalid payload");
    }

    const QJsonObject root = doc.object();
    const int statusCode = root.value(QStringLiteral("status_code")).toInt(-1);
    if (statusCode != 0) {
        const QString statusMsg = root.value(QStringLiteral("status_msg")).toString().trimmed();
        return statusMsg.isEmpty()
            ? QStringLiteral("market breadth status_code=%1").arg(statusCode)
            : QStringLiteral("market breadth status_code=%1 msg=%2").arg(statusCode).arg(statusMsg);
    }
    if (!root.value(QStringLiteral("data")).isObject()) {
        return QStringLiteral("market breadth data missing");
    }

    const QJsonObject data = root.value(QStringLiteral("data")).toObject();
    snapshot->upCount = qMax(0, data.value(QStringLiteral("up")).toInt(0));
    snapshot->flatCount = qMax(0, data.value(QStringLiteral("flat")).toInt(0));
    snapshot->downCount = qMax(0, data.value(QStringLiteral("down")).toInt(0));
    snapshot->breadthValid = true;
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

    req->setRawHeader("Accept", "*/*");
    req->setRawHeader("Origin", "https://52etf.site");
    req->setRawHeader("Referer", "https://52etf.site/");
    req->setRawHeader("Content-Type", "application/json; charset=UTF-8");
    req->setRawHeader("User-Agent", userAgent.toUtf8());
    req->setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);
}

} // namespace

void AshareMarketBreadthProvider::fetch() {
    m_nam.setProxy(m_proxy);

    ++m_requestToken;
    const int token = m_requestToken;
    m_pendingSnapshot = MarketBreadthSnapshot{};
    m_breadthDone = false;
    m_turnoverDone = false;
    m_pendingErrors.clear();

    if (m_breadthReply) {
        QNetworkReply* pendingReply = m_breadthReply;
        m_breadthReply = nullptr;
        pendingReply->abort();
        pendingReply->deleteLater();
    }
    if (m_turnoverReply) {
        QNetworkReply* pendingReply = m_turnoverReply;
        m_turnoverReply = nullptr;
        pendingReply->abort();
        pendingReply->deleteLater();
    }

    QNetworkRequest breadthReq(QUrl(QStringLiteral(
        "https://dq.10jqka.com.cn/fuyao/up_down_distribution/distribution/v2/realtime"
    )));
    applyTonghuashunCommonHeaders(&breadthReq, m_userAgent);
    const network_logger::RequestTrace breadthTrace = network_logger::logRequestStart(
        QStringLiteral("market-breadth"),
        QStringLiteral("GET"),
        breadthReq,
        m_proxy
    );

    m_breadthReply = m_nam.get(breadthReq);
    QNetworkReply* breadthReply = m_breadthReply;
    connect(breadthReply, &QNetworkReply::finished, this, [this, breadthReply, token, breadthTrace]() {
        if (breadthReply == m_breadthReply) {
            m_breadthReply = nullptr;
        }

        const QString networkError = (breadthReply->error() == QNetworkReply::NoError)
            ? QString()
            : breadthReply->errorString();
        const QByteArray body = breadthReply->readAll();

        network_logger::logRequestFinish(breadthTrace, breadthReply, body.size(), body);
        breadthReply->deleteLater();

        if (token != m_requestToken) {
            return;
        }

        QString errorMessage;
        if (!networkError.isEmpty()) {
            errorMessage = QStringLiteral("market breadth request failed: %1").arg(networkError);
        } else {
            errorMessage = parseMarketBreadthPayload(body, &m_pendingSnapshot);
        }
        if (!errorMessage.isEmpty()) {
            m_pendingErrors.push_back(errorMessage);
        }

        m_breadthDone = true;
        finalizeFetch(token);
    });

    QNetworkRequest turnoverReq(QUrl(QStringLiteral(
        "https://dq.10jqka.com.cn/fuyao/market_analysis_api/chart/v1/get_chart_data?chart_key=turnover_minute"
    )));
    applyTonghuashunCommonHeaders(&turnoverReq, m_userAgent);
    const network_logger::RequestTrace turnoverTrace = network_logger::logRequestStart(
        QStringLiteral("market-breadth-turnover"),
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
        }
        if (!errorMessage.isEmpty()) {
            m_pendingErrors.push_back(errorMessage);
        }

        m_turnoverDone = true;
        finalizeFetch(token);
    });
}

void AshareMarketBreadthProvider::finalizeFetch(int token) {
    if (token != m_requestToken || !m_breadthDone || !m_turnoverDone) {
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

    m_lastError.clear();
}
