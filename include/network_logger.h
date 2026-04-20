#pragma once

#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

namespace network_logger {

inline constexpr int kNetworkRequestTimeoutMs = 10000;

struct RequestTrace {
    qint64 id = 0;
    qint64 startedAtMs = 0;
    QString source;
    QString method;
    QUrl url;
};

RequestTrace logRequestStart(
    const QString& source,
    const QString& method,
    const QNetworkRequest& req,
    const QNetworkProxy& proxy
);

void logRequestFinish(
    const RequestTrace& trace,
    const QNetworkReply* reply,
    qsizetype bodyBytes
);

QString proxyToString(const QNetworkProxy& proxy);

} // namespace network_logger
