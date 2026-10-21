#include "update_dialog.h"

#include "i18n.h"

#include <QApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QIcon dialogIcon(QWidget* parent) {
    if (parent && !parent->windowIcon().isNull()) {
        return parent->windowIcon();
    }
    if (qApp && !qApp->windowIcon().isNull()) {
        return qApp->windowIcon();
    }
    return {};
}

} // namespace

UpdateDialog::UpdateDialog(
    const Updater::ReleaseInfo& info,
    const QString& language,
    Updater* updater,
    QWidget* parent
)
    : QDialog(parent)
    , m_info(info)
    , m_asset(info.platformAsset())
    , m_language(language)
    , m_updater(updater) {
    setWindowTitle(trText("update.title"));
    const QIcon icon = dialogIcon(parent);
    if (!icon.isNull()) {
        setWindowIcon(icon);
    }
    setModal(true);
    setMinimumWidth(440);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(12);

    m_messageLabel = new QLabel(this);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setTextFormat(Qt::RichText);
    m_messageLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_messageLabel->setOpenExternalLinks(true);
    QString message = trText("update.message").arg(m_info.tagName.toHtmlEscaped());
    if (!m_info.htmlUrl.isEmpty()) {
        message += QStringLiteral("<br><br><a href=\"%1\">%2</a>")
            .arg(m_info.htmlUrl.toHtmlEscaped(), trText("settings.about.openReleasePage").toHtmlEscaped());
    }
    m_messageLabel->setText(message);
    root->addWidget(m_messageLabel);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setVisible(false);
    root->addWidget(m_statusLabel);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setVisible(false);
    root->addWidget(m_progress);

    QHBoxLayout* actions = new QHBoxLayout();
    actions->setSpacing(8);
    m_skipButton = new QPushButton(trText("update.skip"), this);
    m_updateButton = new QPushButton(trText("update.update"), this);
    m_acceleratedButton = new QPushButton(trText("update.accelerated"), this);
    m_cancelButton = new QPushButton(trText("update.cancel"), this);
    m_closeButton = new QPushButton(trText("update.close"), this);
    m_skipButton->setObjectName(QStringLiteral("updateSkipButton"));
    m_updateButton->setObjectName(QStringLiteral("updateButton"));
    m_acceleratedButton->setObjectName(QStringLiteral("updateAcceleratedButton"));
    m_cancelButton->setObjectName(QStringLiteral("updateCancelButton"));
    m_closeButton->setObjectName(QStringLiteral("updateCloseButton"));
    m_progress->setObjectName(QStringLiteral("updateProgress"));
    m_statusLabel->setObjectName(QStringLiteral("updateStatusLabel"));
    m_cancelButton->setVisible(false);
    m_closeButton->setVisible(false);
    for (QPushButton* button : {m_skipButton, m_updateButton, m_acceleratedButton,
                                m_cancelButton, m_closeButton}) {
        button->setAutoDefault(false);
    }
    m_updateButton->setDefault(true);

    actions->addWidget(m_skipButton);
    actions->addStretch(1);
    actions->addWidget(m_acceleratedButton);
    actions->addWidget(m_updateButton);
    actions->addWidget(m_cancelButton);
    actions->addWidget(m_closeButton);
    root->addLayout(actions);

    const bool hasAsset = !m_asset.browserDownloadUrl.isEmpty();
    m_updateButton->setEnabled(hasAsset);
    m_acceleratedButton->setEnabled(hasAsset);
    m_acceleratedButton->setToolTip(
        trText("update.acceleratedHint").arg(Updater::accelerationPrefix()));

    connect(m_skipButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_updateButton, &QPushButton::clicked, this, [this]() { startDownload(false); });
    connect(m_acceleratedButton, &QPushButton::clicked, this, [this]() { startDownload(true); });
    connect(m_cancelButton, &QPushButton::clicked, this, [this]() {
        if (m_updater) {
            m_updater->cancelDownload();
        }
    });
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);

    if (m_updater) {
        connect(m_updater, &Updater::downloadProgress, this,
            [this](qint64 received, qint64 total) { updateProgress(received, total); });
        connect(m_updater, &Updater::downloadFinished, this,
            [this](const QString& filePath) { showFinished(filePath); });
        connect(m_updater, &Updater::downloadFailed, this,
            [this](const QString& error) { showFailure(error); });
        connect(m_updater, &Updater::downloadCanceled, this, [this]() {
            m_downloading = false;
            m_progress->setVisible(false);
            m_statusLabel->setVisible(false);
            setChoiceButtonsVisible(true);
        });
    }
}

QString UpdateDialog::trText(const QString& key) const {
    return i18n::t(key, m_language);
}

void UpdateDialog::setChoiceButtonsVisible(bool visible) {
    m_skipButton->setVisible(visible);
    m_updateButton->setVisible(visible);
    m_acceleratedButton->setVisible(visible);
    m_cancelButton->setVisible(!visible);
    m_closeButton->setVisible(!visible);
}

void UpdateDialog::startDownload(bool accelerate) {
    if (!m_updater || m_asset.browserDownloadUrl.isEmpty() || m_downloading) {
        return;
    }
    m_downloading = true;
    setChoiceButtonsVisible(false);
    m_statusLabel->setVisible(true);
    m_statusLabel->setText(
        trText(accelerate ? "update.accelerating" : "update.preparing")
            .arg(Updater::accelerationPrefix()));
    m_progress->setValue(0);
    m_progress->setVisible(true);
    m_updater->downloadAsset(m_asset, accelerate);
}

void UpdateDialog::updateProgress(qint64 received, qint64 total) {
    if (!m_downloading) {
        return;
    }
    if (total > 0) {
        const int pct = static_cast<int>(received * 100 / total);
        m_progress->setValue(pct);
        m_statusLabel->setText(trText("update.downloading").arg(pct));
    } else {
        m_statusLabel->setText(trText("update.downloadingUnknown")
            .arg(QLocale().formattedDataSize(received)));
    }
}

void UpdateDialog::showFinished(const QString& filePath) {
    if (!m_downloading) {
        return;
    }
    m_downloading = false;
    m_downloadedPath = filePath;
    m_progress->setValue(100);
    m_statusLabel->setText(trText("update.downloadDone"));
    m_skipButton->setVisible(false);
    m_updateButton->setVisible(false);
    m_acceleratedButton->setVisible(false);
    m_cancelButton->setVisible(false);
    m_closeButton->setVisible(true);
    QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
}

void UpdateDialog::showFailure(const QString& error) {
    m_downloading = false;
    m_progress->setVisible(false);
    m_statusLabel->setVisible(true);
    m_statusLabel->setText(
        trText("update.downloadFailedReason").arg(error));
    setChoiceButtonsVisible(true);
    m_skipButton->setVisible(true);
}

void UpdateDialog::reject() {
    if (m_updater) {
        m_updater->cancelDownload();
    }
    m_downloading = false;
    QDialog::reject();
}
