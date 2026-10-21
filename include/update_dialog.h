#pragma once

#include "updater.h"

#include <QDialog>
#include <QString>

class QLabel;
class QProgressBar;
class QPushButton;

// Presents a detected release with three choices: skip, download from GitHub,
// or download through the gh-proxy.com acceleration mirror. The same window
// turns into a progress view while the installer downloads.
class UpdateDialog : public QDialog {
public:
    UpdateDialog(
        const Updater::ReleaseInfo& info,
        const QString& language,
        Updater* updater,
        QWidget* parent = nullptr
    );

    // Path of the installer once a download finished, empty otherwise.
    QString downloadedPath() const { return m_downloadedPath; }

    void reject() override;

private:
    void startDownload(bool accelerate);
    void showFailure(const QString& error);
    void showFinished(const QString& filePath);
    void updateProgress(qint64 received, qint64 total);
    void setChoiceButtonsVisible(bool visible);
    QString trText(const QString& key) const;

    Updater::ReleaseInfo m_info;
    Updater::ReleaseAsset m_asset;
    QString m_language;
    Updater* m_updater = nullptr;

    QLabel* m_messageLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QProgressBar* m_progress = nullptr;
    QPushButton* m_skipButton = nullptr;
    QPushButton* m_updateButton = nullptr;
    QPushButton* m_acceleratedButton = nullptr;
    QPushButton* m_cancelButton = nullptr;
    QPushButton* m_closeButton = nullptr;

    bool m_downloading = false;
    QString m_downloadedPath;
};
