#include "network_utils.h"
#include "tdx_bridge.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace {
void check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}
struct Reply { QByteArray body; QNetworkReply::NetworkError error; };
Reply get(QNetworkAccessManager& manager, const QUrl& url) {
    QNetworkRequest request(url);
    request.setTransferTimeout(4000);
    auto* reply = manager.get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(5000);
    loop.exec();
    const Reply result{reply->readAll(), reply->error()};
    reply->deleteLater();
    return result;
}
QString loopbackInterface() {
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        if (!iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
#if defined(Q_OS_WIN)
        return iface.humanReadableName();
#else
        return iface.name();
#endif
    }
    return {};
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    AppConfig cfg;
    check(network_utils::proxyFromConfig(cfg).type() == QNetworkProxy::NoProxy,
        "Default automatic interface must preserve system routing");
    cfg.proxyType = "http";
    cfg.proxyHost = "127.0.0.1";
    cfg.proxyPort = 12345;
    check(network_utils::proxyFromConfig(cfg).type() == QNetworkProxy::HttpProxy,
        "Automatic interface must preserve the configured remote proxy");
    cfg = AppConfig();
    cfg.networkInterface = loopbackInterface();
    check(!cfg.networkInterface.isEmpty(), "Need an isolated loopback interface for binding tests");

    QTcpServer http;
    check(http.listen(QHostAddress::LocalHost), "Could not listen for a local HTTP fixture");
    int requests = 0;
    QObject::connect(&http, &QTcpServer::newConnection, [&]() {
        while (http.hasPendingConnections()) {
            auto* socket = http.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket]() {
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) return;
                socket->setProperty("sent", true);
                ++requests;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nbound");
                socket->disconnectFromHost();
            });
        }
    });
    QNetworkAccessManager manager;
    const QNetworkProxy relay = network_utils::proxyFromConfig(cfg);
    check(relay.type() == QNetworkProxy::Socks5Proxy && relay.port() > 1 && !relay.password().isEmpty(),
        "Selected interface must use the authenticated bridge relay");
    manager.setProxy(relay);
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/test").arg(http.serverPort()));
    const Reply bound = get(manager, url);
    check(bound.error == QNetworkReply::NoError && bound.body == "bound" && requests == 1,
        "Qt request failed through the actual interface-bound Go relay");

    // Reconfiguring a manager must not reuse a direct pooled connection when a
    // selected interface becomes unavailable.
    cfg.networkInterface = "mystocks-nonexistent-interface";
    manager.setProxy(network_utils::proxyFromConfig(cfg));
    const Reply unavailable = get(manager, url);
    check(unavailable.error != QNetworkReply::NoError && requests == 1,
        "Unavailable selected interface must fail without falling back to an old/default route");
    cfg.networkInterface.clear();
    manager.setProxy(network_utils::proxyFromConfig(cfg));
    const Reply automatic = get(manager, url);
    check(automatic.error == QNetworkReply::NoError && automatic.body == "bound" && requests == 2,
        "Switching back to automatic routing must recover HTTP requests");

    // Random mode picks a built-in preset once and keeps it for the session.
    cfg = AppConfig();
    check(cfg.userAgentMode == "random", "New configurations must default to a random User-Agent");
    const QStringList presets = builtinUserAgentPresets();
    check(presets.size() == 5, "The random pool must hold five mainstream presets");
    const QString randomAgent = network_utils::effectiveUserAgent(cfg);
    check(presets.contains(randomAgent), "A random User-Agent must come from the built-in presets");
    check(network_utils::effectiveUserAgent(cfg) == randomAgent,
        "The random User-Agent must stay stable for the whole session");
    cfg.userAgentMode = "custom";
    cfg.userAgent = "myStocks-custom-agent";
    check(network_utils::effectiveUserAgent(cfg) == cfg.userAgent,
        "Custom mode must send the configured User-Agent verbatim");
    cfg.userAgentMode = "nonsense";
    check(network_utils::effectiveUserAgent(cfg) == randomAgent,
        "Unknown modes must fall back to random");
    return 0;
}
