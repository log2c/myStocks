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
#include <QUrl>
#include <QUrlQuery>

#include <cmath>

namespace {

QJsonArray extractArray(const QJsonDocument& doc) {
    if (doc.isArray()) {
        return doc.array();
    }

    if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        if (obj.value("data").isArray()) {
            return obj.value("data").toArray();
        }
        if (obj.value("result").isArray()) {
            return obj.value("result").toArray();
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

QString normalizedSymbol(const QString& rawCode) {
    QString raw = rawCode.trimmed().toLower();
    if (raw.isEmpty()) {
        return {};
    }

    QString prefix;
    if (raw.startsWith("sh") || raw.startsWith("sz")) {
        prefix = raw.left(2);
    }

    QString digits;
    digits.reserve(raw.size());
    for (QChar ch : raw) {
        if (ch.isDigit()) {
            digits.append(ch);
        }
    }

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

QVector<QuoteItem> toVector(const QHash<QString, QuoteItem>& buffer) {
    QVector<QuoteItem> out;
    out.reserve(buffer.size());
    for (auto it = buffer.constBegin(); it != buffer.constEnd(); ++it) {
        out.push_back(it.value());
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
        emit error(i18n::t("provider.xtick.token.empty", m_language));
        return;
    }

    m_pendingRequests = 0;
    m_errors.clear();
    m_reqMap.clear();
    m_codeFallback.clear();
    m_buffer.clear();

    QHash<int, QSet<QString>> grouped;

    for (const StockItem& s : stocks) {
        const QString code = normalizeCode(s.code);
        if (code.isEmpty()) {
            continue;
        }

        const int type = inferType(s.code);
        grouped[type].insert(code);
        m_reqMap.insert(makeKey(type, code), s);

        if (!m_codeFallback.contains(code)) {
            m_codeFallback.insert(code, s);
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
    QString digits;
    digits.reserve(raw.size());

    for (QChar ch : raw) {
        if (ch.isDigit()) {
            digits.append(ch);
        }
    }

    if (digits.isEmpty()) {
        return {};
    }
    if (digits.size() > 6) {
        digits = digits.right(6);
    }
    return digits.rightJustified(6, '0');
}

int XTickQuoteProvider::inferType(const QString& rawCode) {
    const QString code = rawCode.trimmed().toLower();
    if (code.startsWith("sz399") || code.startsWith("sh000")) {
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

            for (const QJsonValue& v : arr) {
                if (!v.isObject()) {
                    continue;
                }

                const QJsonObject obj = v.toObject();
                QString normCode = normalizeCode(obj.value("code").toString());

                if (normCode.isEmpty() && obj.value("code").isDouble()) {
                    normCode = QString("%1").arg(
                        static_cast<int>(obj.value("code").toDouble()),
                        6,
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
                } else if (m_codeFallback.contains(normCode)) {
                    stock = m_codeFallback.value(normCode);
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
            emit error(m_errors.join(" | "));
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
    return normalizedSymbol(rawCode);
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
            : QString::fromLocal8Bit(fields.at(0).trimmed()).trimmed();

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
            ? QString::fromLocal8Bit(fields.at(1).trimmed()).trimmed()
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

    for (const StockItem& stock : stocks) {
        const QString secId = toSecId(stock.code);
        if (secId.isEmpty()) {
            continue;
        }

        QUrl url("https://push2.eastmoney.com/api/qt/stock/get");
        QUrlQuery query;
        query.addQueryItem("secid", secId);
        query.addQueryItem("fields", "f57,f58,f43,f60");
        query.addQueryItem("invt", "2");
        query.addQueryItem("fltt", "2");
        url.setQuery(query);

        QNetworkRequest req(url);
        req.setRawHeader("User-Agent", m_userAgent.toUtf8());
        req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

        const network_logger::RequestTrace trace = network_logger::logRequestStart(
            "eastmoney",
            "GET",
            req,
            m_proxy
        );

        QNetworkReply* reply = m_nam.get(req);
        ++m_pendingRequests;

        connect(reply, &QNetworkReply::finished, this, [this, reply, stock, trace]() {
            const QString err = (reply->error() == QNetworkReply::NoError)
                ? QString()
                : reply->errorString();
            const QByteArray body = reply->readAll();

            network_logger::logRequestFinish(trace, reply, body.size(), body);

            reply->deleteLater();
            handleResponse(stock, body, err);
        });
    }

    if (m_pendingRequests == 0) {
        emit quotesReady({});
    }
}

QString EastMoneyQuoteProvider::toSecId(const QString& rawCode) {
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
    const StockItem& stock,
    const QByteArray& body,
    const QString& errorText
) {
    if (!errorText.isEmpty()) {
        m_errors << errorText;
    } else {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError) {
            m_errors << ("json parse error: " + pe.errorString());
        } else if (doc.isObject()) {
            const QJsonObject root = doc.object();
            if (root.value("data").isObject()) {
                const QJsonObject data = root.value("data").toObject();

                const double lastRaw = firstNumber(data, {"f43"});
                double preRaw = firstNumber(data, {"f60"});

                if (!std::isnan(lastRaw)) {
                    if (std::isnan(preRaw) || qFuzzyIsNull(preRaw)) {
                        preRaw = lastRaw;
                    }

                    const double last = lastRaw / 100.0;
                    const double pre = preRaw / 100.0;

                    QuoteItem q;
                    q.code = stock.code;
                    const QString remoteName = data.value("f58").toString().trimmed();
                    q.name = remoteName;

                    q.price = last;
                    q.change = last - pre;
                    q.pct = qFuzzyIsNull(pre) ? 0.0 : (q.change / pre * 100.0);

                    m_buffer.insert(q.code, q);
                }
            }
        }
    }

    --m_pendingRequests;
    if (m_pendingRequests <= 0) {
        if (m_buffer.isEmpty() && !m_errors.isEmpty()) {
            emit error(m_errors.join(" | "));
        }
        emit quotesReady(toVector(m_buffer));
    }
}
