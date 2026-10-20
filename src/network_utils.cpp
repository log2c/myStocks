#include "network_utils.h"
#include "tdx_bridge.h"

#include <QDebug>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QRandomGenerator>

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

QString sessionRandomUserAgent() {
    static const QString chosen = []() {
        const QStringList presets = builtinUserAgentPresets();
        if (presets.isEmpty()) {
            return defaultChrome100UserAgent();
        }
        const int index = QRandomGenerator::global()->bounded(presets.size());
        return presets.at(index);
    }();
    return chosen;
}

QString effectiveUserAgent(const AppConfig& cfg) {
    if (normalizeUserAgentMode(cfg.userAgentMode) == QLatin1String("random")) {
        return sessionRandomUserAgent();
    }
    const QString ua = cfg.userAgent.trimmed();
    if (ua.isEmpty()) {
        return defaultChrome100UserAgent();
    }
    return ua;
}

QVector<InterfaceChoice> availableInterfaces() {
    QVector<InterfaceChoice> choices;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
#if defined(Q_OS_WIN)
        // Go's Windows net.Interface.Name is the adapter FriendlyName.
        const QString id = iface.humanReadableName();
#else
        const QString id = iface.name();
#endif
        QStringList addresses;
        for (const auto& entry : iface.addressEntries()) addresses.append(entry.ip().toString());
        QString label = iface.humanReadableName();
        if (label.isEmpty() || label == iface.name()) label = iface.name();
        else label += QStringLiteral(" (%1)").arg(iface.name());
        if (!addresses.isEmpty()) label += QStringLiteral(" · %1").arg(addresses.join(QStringLiteral(", ")));
        choices.append({id, label, iface.flags().testFlag(QNetworkInterface::IsUp)});
    }
    return choices;
}

QNetworkProxy proxyFromConfig(const AppConfig& cfg) {
    if (!cfg.networkInterface.trimmed().isEmpty()) {
        QString error;
        const QJsonObject relay = tdx_bridge::networkProxyBlocking(cfg, &error);
        if (error.isEmpty() && relay.value(QStringLiteral("port")).toInt() > 0) {
            return QNetworkProxy(QNetworkProxy::Socks5Proxy, relay.value(QStringLiteral("host")).toString(),
                static_cast<quint16>(relay.value(QStringLiteral("port")).toInt()),
                relay.value(QStringLiteral("user")).toString(), relay.value(QStringLiteral("password")).toString());
        }
        qWarning() << "Could not create interface relay:" << error;
        // A selected interface must never silently fall back to the default
        // route when the bridge is missing or incompatible.
        return QNetworkProxy(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), 1);
    }
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
