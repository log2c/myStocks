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
#include <QStandardPaths>
#include <QUrl>

namespace {

// GitHub API endpoint for latest release.
constexpr auto kGitHubApiLatestRelease =
    "https://api.github.com/repos/log2c/myStocks/releases/latest";

// GitHub API endpoint for all releases (includes pre-releases).
constexpr auto kGitHubApiAllReleases =
    "https://api.github.com/repos/log2c/myStocks/releases";

constexpr int kCheckTimeoutMs = 15000;
constexpr int kDownloadTimeoutMs = 300000; // 5 min

// Strip leading 'v' or 'V' from a tag name like "v1.2.3" → "1.2.3".
QString stripVersionPrefix(const QString& tag) {
    const QString t = tag.trimmed();
    if (t.startsWith(QLatin1Char('v')) || t.startsWith(QLatin1Char('V'))) {
        return t.mid(1);
    }
    return t;
}

// Parse "1.2.3" into a comparable tuple of ints.
QVector<int> parseVersion(const QString& ver) {
    QVector<int> parts;
    const QStringList tokens = ver.split(QLatin1Char('.'));
    for (const QString& tok : tokens) {
        bool ok = false;
        const int n = tok.trimmed().toInt(&ok);
        parts.push_back(ok ? n : 0);
    }
    // Normalise to at least 3 components.
    while (parts.size() < 3) {
        parts.push_back(0);
    }
    return parts;
}

// Returns true when remoteVer > localVer (simple numeric comparison).
bool versionGreaterThan(const QString& remoteVer, const QString& localVer) {
    const QVector<int> remote = parseVersion(remoteVer);
    const QVector<int> local = parseVersion(localVer);
    const int n = qMax(remote.size(), local.size());
    for (int i = 0; i < n; ++i) {
        const int r = (i < remote.size()) ? remote[i] : 0;
        const int l = (i < local.size())  ? local[i]  : 0;
        if (r > l) return true;
        if (r < l) return false;
    }
    return false;
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

        if (!versionGreaterThan(remoteVer, currentVer)) {
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

void Updater::downloadAsset(const ReleaseAsset& asset) {
    if (asset.browserDownloadUrl.isEmpty()) {
        emit downloadFailed(QStringLiteral("no download URL"));
        return;
    }

    if (m_downloadReply) {
        m_downloadReply->abort();
        m_downloadReply->deleteLater();
        m_downloadReply = nullptr;
    }

    const QString downloadDir =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(downloadDir);
    const QString destPath = QDir(downloadDir).filePath(asset.name);

    QNetworkRequest req(QUrl(asset.browserDownloadUrl));
    req.setRawHeader("User-Agent", network_utils::effectiveUserAgent(m_cfg).toUtf8());
    req.setTransferTimeout(kDownloadTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    qInfo() << "[Updater] downloading" << asset.browserDownloadUrl << "->" << destPath;
    m_downloadReply = m_nam->get(req);

    connect(m_downloadReply, &QNetworkReply::downloadProgress,
            this, [this](qint64 received, qint64 total) {
        emit downloadProgress(received, total);
    });

    connect(m_downloadReply, &QNetworkReply::finished, this, [this, destPath]() {
        if (!m_downloadReply) {
            return;
        }

        const QNetworkReply::NetworkError netErr = m_downloadReply->error();
        const QByteArray data = m_downloadReply->readAll();
        m_downloadReply->deleteLater();
        m_downloadReply = nullptr;

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

// static
bool Updater::isNewerVersion(const QString& currentVersion, const QString& remoteTag) {
    return versionGreaterThan(stripVersionPrefix(remoteTag), currentVersion);
}
