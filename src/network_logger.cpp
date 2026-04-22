#include "network_logger.h"

#include <QDateTime>
#include <QDebug>

#include <atomic>

namespace {

std::atomic<qint64> g_requestId {1};
inline constexpr qsizetype kMaxBodyPreviewBytes = 2048;

QString compactBodyPreview(const QByteArray& body) {
    if (body.isEmpty()) {
        return "<empty>";
    }

    const bool truncated = body.size() > kMaxBodyPreviewBytes;
    const QByteArray previewBytes = truncated ? body.left(kMaxBodyPreviewBytes) : body;

    QString preview = QString::fromUtf8(previewBytes);
    preview.replace('\r', "\\r");
    preview.replace('\n', "\\n");

    if (truncated) {
        preview += QString(" ...(truncated %1 bytes)").arg(body.size() - previewBytes.size());
    }

    return preview;
}

} // namespace

namespace network_logger {

RequestTrace logRequestStart(
    const QString& source,
    const QString& method,
    const QNetworkRequest& req,
    const QNetworkProxy& proxy
) {
    RequestTrace trace;
    trace.id = g_requestId.fetch_add(1);
    trace.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    trace.source = source;
    trace.method = method;
    trace.url = req.url();

    const QString ua = QString::fromUtf8(req.rawHeader("User-Agent"));
    const QString timeoutText = req.transferTimeout() > 0
        ? QString::number(req.transferTimeout())
        : QString::number(kNetworkRequestTimeoutMs);

    qInfo().noquote() << QString(
        "[NET][%1][%2] start method=%3 url=%4 timeout=%5ms proxy=%6 ua=%7"
    )
        .arg(trace.id)
        .arg(trace.source)
        .arg(trace.method)
        .arg(trace.url.toString())
        .arg(timeoutText)
        .arg(proxyToString(proxy))
        .arg(ua.isEmpty() ? QString("<empty>") : ua);

    return trace;
}

void logRequestFinish(
    const RequestTrace& trace,
    const QNetworkReply* reply,
    qsizetype bodyBytes,
    const QByteArray& body
) {
    const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - trace.startedAtMs;

    QString statusText = "n/a";
    QString errorText = "-";

    if (reply) {
        const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (status.isValid()) {
            statusText = status.toString();
        }

        if (reply->error() != QNetworkReply::NoError) {
            errorText = reply->errorString();
        }
    } else {
        errorText = "reply is null";
    }

    qInfo().noquote() << QString(
        "[NET][%1][%2] finish elapsed=%3ms status=%4 error=%5 bytes=%6"
    )
        .arg(trace.id)
        .arg(trace.source)
        .arg(elapsedMs)
        .arg(statusText)
        .arg(errorText)
        .arg(bodyBytes);

    qDebug().noquote() << QString("[NET][%1][%2] response=%3")
        .arg(trace.id)
        .arg(trace.source)
        .arg(compactBodyPreview(body));
}

QString proxyToString(const QNetworkProxy& proxy) {
    QString type;
    switch (proxy.type()) {
    case QNetworkProxy::NoProxy:
        type = "none";
        break;
    case QNetworkProxy::HttpProxy:
        type = "http";
        break;
    case QNetworkProxy::Socks5Proxy:
        type = "socks5";
        break;
    default:
        type = "other";
        break;
    }

    if (proxy.type() == QNetworkProxy::NoProxy) {
        return type;
    }

    return QString("%1://%2:%3")
        .arg(type)
        .arg(proxy.hostName())
        .arg(proxy.port());
}

} // namespace network_logger
