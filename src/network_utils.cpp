#include "network_utils.h"

namespace {

QString normalizedProxyType(const QString& raw) {
    const QString type = raw.trimmed().toLower();
    if (type == "http") {
        return "http";
    }
    if (type == "socks" || type == "socks5") {
        return "socks5";
    }
    return "none";
}

} // namespace

namespace network_utils {

QString effectiveUserAgent(const AppConfig& cfg) {
    const QString ua = cfg.userAgent.trimmed();
    if (ua.isEmpty()) {
        return defaultChrome100UserAgent();
    }
    return ua;
}

QNetworkProxy proxyFromConfig(const AppConfig& cfg) {
    const QString proxyType = normalizedProxyType(cfg.proxyType);
    const QString host = cfg.proxyHost.trimmed();

    if (proxyType == "none" || host.isEmpty() || cfg.proxyPort <= 0 || cfg.proxyPort > 65535) {
        return QNetworkProxy(QNetworkProxy::NoProxy);
    }

    const QNetworkProxy::ProxyType qtType =
        (proxyType == "http") ? QNetworkProxy::HttpProxy : QNetworkProxy::Socks5Proxy;

    return QNetworkProxy(
        qtType,
        host,
        static_cast<quint16>(cfg.proxyPort),
        cfg.proxyUser,
        cfg.proxyPassword
    );
}

bool hasProxy(const AppConfig& cfg) {
    return proxyFromConfig(cfg).type() != QNetworkProxy::NoProxy;
}

} // namespace network_utils
