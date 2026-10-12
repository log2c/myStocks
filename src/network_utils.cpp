#include "network_utils.h"

#include "network_logger.h"

#include <QHostAddress>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringConverter>
#include <QUrl>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

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

QString decodeMarketText(const QByteArray& body) {
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString decodedUtf8 = utf8(body);
    if (!utf8.hasError()) {
        return decodedUtf8;
    }

    // Named codecs depend on how Qt was built. Fall back to the OS codec so
    // the official Qt Windows and macOS packages also decode Sina/Tencent.
    QStringDecoder gb18030("GB18030");
    if (gb18030.isValid()) {
        const QString decoded = gb18030(body);
        if (!gb18030.hasError()) {
            return decoded;
        }
    }
#ifdef Q_OS_WIN
    const int length = MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS,
        body.constData(), static_cast<int>(body.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS, body.constData(),
            static_cast<int>(body.size()), text.data(), length)) {
        return {};
    }
    return QString::fromStdWString(text);
#else
    iconv_t converter = iconv_open("UTF-8", "GB18030");
    if (converter == reinterpret_cast<iconv_t>(-1)) {
        return {};
    }
    QByteArray output(body.size() * 4 + 16, '\0');
    char* input = const_cast<char*>(body.constData());
    char* cursor = output.data();
    size_t inputBytes = static_cast<size_t>(body.size());
    size_t outputBytes = static_cast<size_t>(output.size());
    const size_t result = iconv(converter, &input, &inputBytes, &cursor, &outputBytes);
    iconv_close(converter);
    if (result == static_cast<size_t>(-1) || inputBytes != 0) {
        return {};
    }
    output.resize(output.size() - static_cast<qsizetype>(outputBytes));
    return QString::fromUtf8(output);
#endif
}

QStringList publicIpServiceUrls() {
    return {
        QStringLiteral("https://api.ipify.org"),
        QStringLiteral("https://ipinfo.io/ip"),
        QStringLiteral("https://icanhazip.com"),
        QStringLiteral("https://ifconfig.me/ip"),
    };
}

PublicIpResolver::PublicIpResolver(QObject* parent, QNetworkAccessManager* manager)
    : QObject(parent), m_manager(manager ? manager : &m_nam) {}

void PublicIpResolver::applyConfig(const AppConfig& cfg) {
    m_userAgent = effectiveUserAgent(cfg);
    m_manager->setProxy(proxyFromConfig(cfg));
}

void PublicIpResolver::resolve(
    std::function<void(const QString& ip, const QString& error)> completion
) {
    if (m_reply) {
        QNetworkReply* stale = m_reply;
        m_reply = nullptr;
        disconnect(stale, nullptr, this, nullptr);
        stale->abort();
        stale->deleteLater();
    }

    m_services = publicIpServiceUrls();
    m_errors.clear();
    m_serviceIndex = 0;
    m_completion = std::move(completion);
    requestNext();
}

void PublicIpResolver::requestNext() {
    if (m_serviceIndex >= m_services.size()) {
        finish({}, m_errors.join(QStringLiteral(" | ")));
        return;
    }

    const QUrl url(m_services.at(m_serviceIndex++));
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", m_userAgent.toUtf8());
    req.setRawHeader("Accept", "text/plain");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const auto trace = network_logger::logRequestStart(
        QStringLiteral("public-ip"), QStringLiteral("GET"), req, m_manager->proxy());
    QNetworkReply* reply = m_manager->get(req);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, trace]() {
        if (m_reply != reply) {
            reply->deleteLater();
            return;
        }
        m_reply = nullptr;

        const QByteArray bytes = reply->readAll();
        network_logger::logRequestFinish(trace, reply, bytes.size(), {});
        const QString body = QString::fromUtf8(bytes).trimmed();
        const QString networkError = reply->error() == QNetworkReply::NoError
            ? QString()
            : reply->errorString();
        reply->deleteLater();

        QHostAddress address;
        if (networkError.isEmpty() && address.setAddress(body)
            && !address.isNull() && !address.isLoopback()
            && !address.isMulticast() && address != QHostAddress::AnyIPv4
            && address != QHostAddress::AnyIPv6) {
            finish(address.toString(), {});
            return;
        }

        m_errors.push_back(
            networkError.isEmpty()
                ? QStringLiteral("%1 returned an invalid address").arg(url.host())
                : QStringLiteral("%1: %2").arg(url.host(), networkError)
        );
        requestNext();
    });
}

void PublicIpResolver::finish(const QString& ip, const QString& error) {
    auto completion = std::move(m_completion);
    m_completion = {};
    if (completion) {
        completion(ip, error);
    }
}

} // namespace network_utils
