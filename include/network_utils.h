#pragma once

#include "types.h"

#include <functional>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QPointer>
#include <QString>
#include <QStringList>

class QNetworkReply;

namespace network_utils {

QString effectiveUserAgent(const AppConfig& cfg);
QNetworkProxy proxyFromConfig(const AppConfig& cfg);
bool hasProxy(const AppConfig& cfg);
QString decodeMarketText(const QByteArray& body);
QStringList publicIpServiceUrls();

class PublicIpResolver : public QObject {
public:
    explicit PublicIpResolver(QObject* parent = nullptr, QNetworkAccessManager* manager = nullptr);

    void applyConfig(const AppConfig& cfg);
    void resolve(std::function<void(const QString& ip, const QString& error)> completion);

private:
    void requestNext();
    void finish(const QString& ip, const QString& error);

    QNetworkAccessManager m_nam;
    QNetworkAccessManager* m_manager = nullptr;
    QPointer<QNetworkReply> m_reply;
    QString m_userAgent = defaultChrome100UserAgent();
    QStringList m_services;
    QStringList m_errors;
    int m_serviceIndex = 0;
    std::function<void(const QString&, const QString&)> m_completion;
};

} // namespace network_utils
