#include "limit_pool_provider.h"
#include "tdx_bridge.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QNetworkProxy>
#include <QTimer>
#include <QUrlQuery>

#include <cstring>
#include <cmath>

namespace {
void check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}
QString lastBoundInterface;

class Reply : public QNetworkReply {
public:
    explicit Reply(const QNetworkRequest& request, QObject* parent) : QNetworkReply(parent) {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly);
    }
    void abort() override {
        if (isFinished()) return;
        aborted = true;
        setError(OperationCanceledError, "fixture canceled");
        setFinished(true);
        emit finished();
    }
    void complete(const QByteArray& bytes, QNetworkReply::NetworkError error = NoError) {
        if (isFinished()) return;
        body = bytes;
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, error == NoError ? 200 : 502);
        if (error != NoError) setError(error, "fixture network failure");
        setFinished(true);
        emit readyRead();
        emit finished();
    }
    qint64 bytesAvailable() const override { return body.size() - offset + QNetworkReply::bytesAvailable(); }
    bool aborted = false;
protected:
    qint64 readData(char* destination, qint64 maximum) override {
        const qint64 count = qMin(maximum, body.size() - offset);
        if (count <= 0) return -1;
        std::memcpy(destination, body.constData() + offset, size_t(count));
        offset += count;
        return count;
    }
private:
    QByteArray body;
    qint64 offset = 0;
};

class Manager : public QNetworkAccessManager {
public:
    struct Call { QNetworkRequest request; QNetworkProxy proxy; QPointer<Reply> reply; };
    QVector<Call> calls;
    void respond(const QJsonObject& object) { calls.last().reply->complete(QJsonDocument(object).toJson()); }
protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request, QIODevice*) override {
        check(operation == GetOperation, "Limit pools must use GET requests");
        auto* reply = new Reply(request, this);
        calls.push_back({request, proxy(), reply});
        return reply;
    }
};

QJsonObject page(const QDate& date, int number, int total, int limit, const QStringList& names, const QJsonArray& rates = {}) {
    QJsonArray stocks;
    for (int i = 0; i < names.size(); ++i) {
        QJsonObject stock {{"name", names[i]}, {"code", QString::number((number - 1) * limit + i).rightJustified(6, '0')}};
        if (i < rates.size()) stock.insert("change_rate", rates.at(i));
        stocks.append(stock);
    }
    const QJsonObject data {
        {"date", date.toString("yyyyMMdd")}, {"info", stocks},
        {"trade_status", QJsonObject{{"id", "closed"}, {"name", "已收盘"}}},
        {"page", QJsonObject{{"page", number}, {"limit", limit}, {"total", total}}},
    };
    return {{"status_code", 0}, {"status_msg", "success"}, {"data", data}};
}

QStringList stockNames(const LimitPoolSnapshot& snapshot) {
    QStringList names;
    for (const auto& stock : snapshot.stocks) names.append(stock.name);
    return names;
}

void checkProvider() {
    auto* manager = new Manager;
    TonghuashunLimitPoolProvider provider(nullptr, manager);
    const QDate today = QDateTime::currentDateTimeUtc().toOffsetFromUtc(8 * 3600).date();
    AppConfig cfg;
    cfg.proxyType = "http";
    cfg.proxyHost = "proxy.fixture";
    cfg.proxyPort = 4321;
    cfg.proxyUser = "user";
    cfg.proxyPassword = "password";
    cfg.userAgentMode = "custom";
    cfg.userAgent = "myStocks-limit-pool-test";
    provider.applyConfig(cfg);
    QVector<QPair<bool, LimitPoolSnapshot>> results;
    QVector<QString> errors;
    QObject::connect(&provider, &TonghuashunLimitPoolProvider::dataReady, &provider,
        [&](bool down, const LimitPoolSnapshot& snapshot) { results.push_back({down, snapshot}); });
    QObject::connect(&provider, &TonghuashunLimitPoolProvider::error, &provider,
        [&](bool, const QString& message) { errors.push_back(message); });

    provider.fetch(false, today);
    provider.fetch(false, today);
    check(manager->calls.size() == 1, "An in-flight pool request must not be duplicated");
    const auto& first = manager->calls.first();
    check(first.request.url().scheme() == "https"
        && first.request.url().host() == "data.10jqka.com.cn"
        && first.request.url().path() == "/dataapi/limit_up/limit_up_pool", "Wrong limit-up endpoint");
    check(QUrlQuery(first.request.url()).queryItemValue("page") == "1"
        && QUrlQuery(first.request.url()).queryItemValue("date") == today.toString("yyyyMMdd"),
        "Explicit trading date and pagination must be sent");
    check(QUrlQuery(first.request.url()).queryItemValue("field") == "199112",
        "The pool request must explicitly request the change_rate field");
    check(first.request.rawHeader("User-Agent") == cfg.userAgent.toUtf8()
        && first.request.rawHeader("Referer") == "https://data.10jqka.com.cn/"
        && first.request.transferTimeout() > 0, "Global user agent, referer and timeout must be applied");
    check(first.proxy.type() == QNetworkProxy::HttpProxy && first.proxy.hostName() == cfg.proxyHost
        && first.proxy.port() == cfg.proxyPort && first.proxy.user() == cfg.proxyUser
        && first.proxy.password() == cfg.proxyPassword, "Global HTTP proxy and authentication must be applied");
    manager->respond(page(today, 1, 3, 2, {"天津普林", "东方嘉盛"}, {10.0158, 0.0}));
    check(results.isEmpty() && manager->calls.size() == 2, "Partial stock pages must not be displayed as the complete pool");
    check(QUrlQuery(manager->calls.last().request.url()).queryItemValue("page") == "2"
        && QUrlQuery(manager->calls.last().request.url()).queryItemValue("limit") == "2",
        "Pagination must follow the page size actually returned by the server");
    manager->respond(page(today, 2, 3, 2, {"股票三"}, {-10.041}));
    check(results.size() == 1 && !results.last().first && stockNames(results.last().second) == QStringList({"天津普林", "东方嘉盛", "股票三"})
        && results.last().second.tradeDate == today, "All names and the trading date must survive pagination");
    check(results.last().second.stocks.at(0).code == "000000"
        && results.last().second.stocks.at(2).code == "000002",
        "Stock codes must remain paired with their names across pages for chart navigation");
    check(std::abs(results.last().second.stocks.at(0).changeRate - 10.0158) < 1e-9
        && results.last().second.stocks.at(1).changeRate == 0.0
        && std::abs(results.last().second.stocks.at(2).changeRate + 10.041) < 1e-9,
        "Change rates must preserve their percent units, signs and valid zero across pages");
    check(QUrlQuery(manager->calls.last().request.url()).queryItemValue("field") == "199112",
        "Subsequent pages must also request the percentage field");
    provider.fetch(false, today);
    check(results.size() == 2 && manager->calls.size() == 2, "A complete result must be cached");
    provider.fetch(true, today);
    check(manager->calls.last().request.url().path() == "/dataapi/limit_up/lower_limit_pool", "Wrong limit-down endpoint");
    manager->respond(page(today, 1, 0, 200, {}));
    check(results.last().first && results.last().second.stocks.isEmpty() && errors.isEmpty(), "A valid empty pool must remain an empty list");

    provider.fetch(false, today, true);
    auto oldReply = manager->calls.last().reply;
    cfg.proxyType = "socks5";
    cfg.proxyHost = "socks.fixture";
    provider.applyConfig(cfg);
    check(oldReply->aborted && errors.isEmpty(), "Changing global network settings must cancel old requests without publishing errors");
    const int before = results.size();
    oldReply->complete(QJsonDocument(page(today, 1, 1, 200, {"stale"})).toJson());
    check(results.size() == before, "Canceled responses must not replace the current pool");
    provider.fetch(false, today);
    check(manager->calls.last().proxy.type() == QNetworkProxy::Socks5Proxy
        && manager->calls.last().proxy.hostName() == "socks.fixture", "Reconfigured requests must use the new SOCKS5 proxy");
    manager->respond(page(today, 1, 1, 200, {"新配置"}));
    check(std::isnan(results.last().second.stocks.first().changeRate),
        "A missing change_rate must remain unavailable instead of becoming zero");
    provider.fetch(false, today, true);
    manager->respond(page(today, 1, 2, 200, {"空值", "无效值"}, {QJsonValue(QJsonValue::Null), QJsonValue("N/A")}));
    check(results.last().second.stocks.size() == 2
        && std::isnan(results.last().second.stocks[0].changeRate)
        && std::isnan(results.last().second.stocks[1].changeRate),
        "Null and non-numeric percentages must not discard otherwise valid stock names");
    cfg.networkInterface = "fixture-adapter";
    provider.applyConfig(cfg);
    provider.fetch(true, today);
    check(lastBoundInterface == cfg.networkInterface && manager->calls.last().proxy.type() == QNetworkProxy::Socks5Proxy
        && manager->calls.last().proxy.hostName() == "127.0.0.1" && manager->calls.last().proxy.port() == 1080,
        "Selected network interfaces must use the global interface-bound relay");
    manager->respond(page(today, 1, 1, 200, {"跌停名称"}));

    const auto reject = [&](const QByteArray& payload) {
        const int beforeError = errors.size(), beforeResult = results.size();
        provider.fetch(false, today, true);
        manager->calls.last().reply->complete(payload);
        check(errors.size() == beforeError + 1 && results.size() == beforeResult,
            "Malformed, incomplete or stale data must not be published as a valid pool");
    };
    reject("<html>access denied</html>");
    reject(QJsonDocument(QJsonObject{{"status_code", 1}}).toJson());
    reject(QJsonDocument(page(today.addDays(-1), 1, 1, 200, {"昨天数据"})).toJson());
    reject(QJsonDocument(page(today, 2, 1, 200, {"错误页"})).toJson());
    reject(QJsonDocument(page(today, 1, 3, 200, {})).toJson());
    provider.fetch(false, today, true);
    manager->respond(page(today, 1, 3, 2, {"第一页一", "第一页二"}));
    const int beforeMismatch = results.size(), beforeError = errors.size();
    manager->respond(page(today.addDays(-1), 2, 3, 2, {"旧日期"}));
    check(results.size() == beforeMismatch && errors.size() == beforeError + 1,
        "Different trading dates must not be merged across pages");

    provider.fetch(false, {}, true);
    manager->respond(page(today.addDays(-1), 1, 1, 200, {"最近交易日"}));
    check(results.last().second.tradeDate == today.addDays(-1), "An unspecified date must preserve the latest server trading date");
    provider.fetch(false, today, true);
    const int beforeNetworkError = errors.size();
    manager->calls.last().reply->complete({}, QNetworkReply::ConnectionRefusedError);
    check(errors.size() == beforeNetworkError + 1, "Network errors must reach the pool's error state");
}

void checkLive() {
    TonghuashunLimitPoolProvider provider;
    QEventLoop loop;
    int received = 0;
    QString failure;
    QObject::connect(&provider, &TonghuashunLimitPoolProvider::dataReady, &provider,
        [&](bool down, const LimitPoolSnapshot& snapshot) {
            if (!snapshot.stocks.isEmpty()) {
                check(std::isfinite(snapshot.stocks.first().changeRate), "Live percentage field is missing");
                qInfo() << "first stock change_rate=" << snapshot.stocks.first().changeRate;
            }
            qInfo() << (down ? "limit-down" : "limit-up") << snapshot.tradeDate << snapshot.stocks.size() << "stocks";
            if (++received == 2) loop.quit();
        });
    QObject::connect(&provider, &TonghuashunLimitPoolProvider::error, &provider,
        [&](bool, const QString& message) { failure = message; loop.quit(); });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    provider.fetch(false);
    provider.fetch(true);
    loop.exec();
    check(received == 2 && failure.isEmpty(), "Live limit-pool smoke test failed");
}
}

namespace tdx_bridge {
QJsonObject networkProxyBlocking(const AppConfig& cfg, QString* error) {
    lastBoundInterface = cfg.networkInterface;
    if (error) error->clear();
    return {{"host", "127.0.0.1"}, {"port", 1080}, {"user", "bound"}, {"password", "test"}};
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--live")) checkLive();
    else checkProvider();
    return 0;
}
