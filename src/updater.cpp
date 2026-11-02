#include "updater.h"

#include "app_logging.h"
#include "network_utils.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QVersionNumber>

namespace {

// GitHub API endpoint for latest release.
constexpr auto kGitHubApiLatestRelease =
    "https://api.github.com/repos/log2c/myStocks/releases/latest";

// GitHub API endpoint for all releases (includes pre-releases).
constexpr auto kGitHubApiAllReleases =
    "https://api.github.com/repos/log2c/myStocks/releases";

constexpr int kCheckTimeoutMs = 15000;
constexpr int kDownloadTimeoutMs = 300000; // 5 min

// Public GitHub download mirror used by the "国内加速" choice. It accepts the
// full github.com URL appended to the prefix.
constexpr auto kAccelerationPrefix = "https://gh-proxy.com/";

// Strip leading 'v' or 'V' from a tag name like "v1.2.3" → "1.2.3".
QString stripVersionPrefix(const QString& tag) {
    const QString t = tag.trimmed();
    if (t.startsWith(QLatin1Char('v')) || t.startsWith(QLatin1Char('V'))) {
        return t.mid(1);
    }
    return t;
}

bool versionGreaterThan(const QString& remoteVer, const QString& localVer) {
    static const QRegularExpression pattern(QStringLiteral(
        "^([0-9]+(?:\\.[0-9]+)*)(?:-([0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*))?"
        "(?:\\+[0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*)?$"));
    const auto remote = pattern.match(stripVersionPrefix(remoteVer));
    const auto local = pattern.match(stripVersionPrefix(localVer));
    if (!remote.hasMatch() || !local.hasMatch()) {
        return false;
    }
    const int coreComparison = QVersionNumber::compare(
        QVersionNumber::fromString(remote.captured(1)).normalized(),
        QVersionNumber::fromString(local.captured(1)).normalized());
    if (coreComparison != 0) {
        return coreComparison > 0;
    }
    const QString remotePrerelease = remote.captured(2);
    const QString localPrerelease = local.captured(2);
    if (remotePrerelease.isEmpty() || localPrerelease.isEmpty()) {
        return remotePrerelease.isEmpty() && !localPrerelease.isEmpty();
    }
    const QStringList remoteParts = remotePrerelease.split(QLatin1Char('.'));
    const QStringList localParts = localPrerelease.split(QLatin1Char('.'));
    static const QRegularExpression numeric(QStringLiteral("^[0-9]+$"));
    for (qsizetype index = 0; index < qMin(remoteParts.size(), localParts.size()); ++index) {
        const QString& remotePart = remoteParts[index];
        const QString& localPart = localParts[index];
        if (remotePart == localPart) {
            continue;
        }
        const bool remoteNumeric = numeric.match(remotePart).hasMatch();
        const bool localNumeric = numeric.match(localPart).hasMatch();
        if (remoteNumeric != localNumeric) {
            return !remoteNumeric;
        }
        if (remoteNumeric && remotePart.size() != localPart.size()) {
            return remotePart.size() > localPart.size();
        }
        return remotePart > localPart;
    }
    return remoteParts.size() > localParts.size();
}

// Pick a platform-appropriate keyword for matching asset filenames.
QString platformKeyword() {
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#else
    return QStringLiteral("linux");
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// ReleaseInfo helpers
// ---------------------------------------------------------------------------

Updater::ReleaseAsset Updater::ReleaseInfo::platformAsset() const {
    const QString keyword = platformKeyword();
    for (const ReleaseAsset& a : assets) {
        if (a.name.toLower().contains(keyword)) {
            return a;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Updater
// ---------------------------------------------------------------------------

Updater::Updater(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this)) {
}

void Updater::setConfig(const AppConfig& cfg) {
    m_cfg = cfg;
    m_nam->setProxy(network_utils::proxyFromConfig(cfg));
}

void Updater::checkForUpdates() {
    if (m_checkReply) {
        m_checkReply->abort();
        m_checkReply->deleteLater();
        m_checkReply = nullptr;
    }

    const bool useBeta = m_cfg.acceptBetaUpdates;
    const QString url = useBeta
        ? QString::fromLatin1(kGitHubApiAllReleases)
        : QString::fromLatin1(kGitHubApiLatestRelease);

    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("User-Agent", network_utils::effectiveUserAgent(m_cfg).toUtf8());
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(kCheckTimeoutMs);

    qInfo() << "[Updater] checking" << url;
    m_checkReply = m_nam->get(req);

    connect(m_checkReply, &QNetworkReply::finished, this, [this, useBeta]() {
        if (!m_checkReply) {
            return;
        }

        const QNetworkReply::NetworkError netErr = m_checkReply->error();
        const QByteArray data = m_checkReply->readAll();
        m_checkReply->deleteLater();
        m_checkReply = nullptr;

        if (netErr != QNetworkReply::NoError) {
            qWarning() << "[Updater] check failed:" << netErr;
            emit checkFailed(QString::number(static_cast<int>(netErr)));
            return;
        }

        QJsonParseError parseErr;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            qWarning() << "[Updater] json parse error:" << parseErr.errorString();
            emit checkFailed(parseErr.errorString());
            return;
        }

        // When using beta endpoint the response is an array; pick the first (newest) element.
        QJsonObject root;
        if (useBeta) {
            if (!doc.isArray() || doc.array().isEmpty()) {
                qInfo() << "[Updater] no releases found";
                emit noUpdateAvailable();
                return;
            }
            const QJsonValue first = doc.array().first();
            if (!first.isObject()) {
                qWarning() << "[Updater] unexpected releases response format";
                emit checkFailed(QStringLiteral("unexpected format"));
                return;
            }
            root = first.toObject();
        } else {
            if (!doc.isObject()) {
                qWarning() << "[Updater] json parse error: not an object";
                emit checkFailed(QStringLiteral("unexpected format"));
                return;
            }
            root = doc.object();
        }
        const QString tagName = root.value(QStringLiteral("tag_name")).toString().trimmed();
        if (tagName.isEmpty()) {
            qInfo() << "[Updater] no release found (empty tag_name)";
            emit noUpdateAvailable();
            return;
        }

        const QString remoteVer = stripVersionPrefix(tagName);
        const QString currentVer = QString::fromLatin1(APP_VERSION_STRING);

        qInfo() << "[Updater] current=" << currentVer << "remote=" << remoteVer;

        if (!isNewerVersion(currentVer, tagName)) {
            emit noUpdateAvailable();
            return;
        }

        ReleaseInfo info;
        info.tagName  = tagName;
        info.htmlUrl  = root.value(QStringLiteral("html_url")).toString();
        info.body     = root.value(QStringLiteral("body")).toString();

        const QJsonArray assetsArr = root.value(QStringLiteral("assets")).toArray();
        for (const QJsonValue& v : assetsArr) {
            if (!v.isObject()) {
                continue;
            }
            const QJsonObject obj = v.toObject();
            ReleaseAsset a;
            a.name               = obj.value(QStringLiteral("name")).toString();
            a.browserDownloadUrl = obj.value(QStringLiteral("browser_download_url")).toString();
            a.size               = static_cast<qint64>(
                obj.value(QStringLiteral("size")).toDouble()
            );
            if (!a.name.isEmpty() && !a.browserDownloadUrl.isEmpty()) {
                info.assets.push_back(a);
            }
        }

        qInfo() << "[Updater] update available tag=" << tagName
                << "assets=" << info.assets.size();
        emit updateAvailable(info);
    });
}

void Updater::downloadAsset(const ReleaseAsset& asset, bool accelerate) {
    if (asset.browserDownloadUrl.isEmpty()) {
        emit downloadFailed(QStringLiteral("no download URL"));
        return;
    }

    if (m_downloadReply) {
        // Replacing an in-flight transfer is not a user cancellation.
        QNetworkReply* previous = m_downloadReply;
        m_downloadReply = nullptr;
        previous->abort();
        previous->deleteLater();
    }

    const QString downloadDir =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(downloadDir);
    const QString destPath = QDir(downloadDir).filePath(asset.name);

    const QString sourceUrl = accelerate
        ? acceleratedUrl(asset.browserDownloadUrl)
        : asset.browserDownloadUrl;

    QNetworkRequest req{QUrl(sourceUrl)};
    req.setRawHeader("User-Agent", network_utils::effectiveUserAgent(m_cfg).toUtf8());
    req.setTransferTimeout(kDownloadTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    qInfo() << "[Updater] downloading" << sourceUrl << "->" << destPath;
    QNetworkReply* reply = m_nam->get(req);
    m_downloadReply = reply;

    connect(reply, &QNetworkReply::downloadProgress,
            this, [this, reply](qint64 received, qint64 total) {
        if (m_downloadReply != reply) {
            return;
        }
        emit downloadProgress(received, total);
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, destPath]() {
        if (m_downloadReply != reply) {
            reply->deleteLater();
            return;
        }
        m_downloadReply = nullptr;

        const QNetworkReply::NetworkError netErr = reply->error();
        const QByteArray data = reply->readAll();
        reply->deleteLater();

        if (netErr != QNetworkReply::NoError) {
            qWarning() << "[Updater] download failed:" << netErr;
            emit downloadFailed(QString::number(static_cast<int>(netErr)));
            return;
        }

        QFile file(destPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "[Updater] cannot write" << destPath;
            emit downloadFailed(file.errorString());
            return;
        }
        file.write(data);
        file.close();

        qInfo() << "[Updater] download complete" << destPath;
        emit downloadFinished(destPath);
    });
}

void Updater::cancelDownload() {
    if (!m_downloadReply) {
        return;
    }
    QNetworkReply* reply = m_downloadReply;
    // Clear the handle first: abort() finishes the reply synchronously and the
    // finished handler must not report an aborted transfer as a failure.
    m_downloadReply = nullptr;
    reply->abort();
    reply->deleteLater();
    emit downloadCanceled();
}

// static
QString Updater::accelerationPrefix() {
    return QString::fromLatin1(kAccelerationPrefix);
}

// static
QString Updater::acceleratedUrl(const QString& url) {
    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(accelerationPrefix(), Qt::CaseInsensitive)) {
        return trimmed;
    }
    // The mirror takes the origin URL verbatim after the prefix.
    return accelerationPrefix() + trimmed;
}

// static
bool Updater::isNewerVersion(const QString& currentVersion, const QString& remoteTag) {
    return versionGreaterThan(stripVersionPrefix(remoteTag), currentVersion);
}
