#pragma once

#include "types.h"

#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

class Updater : public QObject {
    Q_OBJECT

public:
    struct ReleaseAsset {
        QString name;
        QString browserDownloadUrl;
        qint64 size = 0;
    };

    struct ReleaseInfo {
        QString tagName;
        QString htmlUrl;
        QString body;
        QVector<ReleaseAsset> assets;

        // Returns the best asset for the current platform, or a null asset.
        ReleaseAsset platformAsset() const;
    };

    explicit Updater(QObject* parent = nullptr);

    void setConfig(const AppConfig& cfg);
    void checkForUpdates();
    void downloadAsset(const ReleaseAsset& asset);

signals:
    void updateAvailable(const Updater::ReleaseInfo& info);
    void noUpdateAvailable();
    void checkFailed(const QString& error);
    void downloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void downloadFinished(const QString& filePath);
    void downloadFailed(const QString& error);

private:
    // Returns true if remoteTag represents a newer version than currentVersion.
    static bool isNewerVersion(const QString& currentVersion, const QString& remoteTag);

    AppConfig m_cfg;
    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply* m_checkReply = nullptr;
    QNetworkReply* m_downloadReply = nullptr;
};
