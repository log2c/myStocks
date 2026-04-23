#include "app_controller.h"

#include "app_logging.h"
#include "config_manager.h"
#include "floating_window.h"
#include "i18n.h"
#include "network_logger.h"
#include "network_utils.h"
#include "quote_model.h"
#include "quote_provider.h"
#include "settings_dialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
#include <QHotkey>
#endif
#include <QMenu>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTimeZone>

AppController::AppController(QObject* parent)
    : QObject(parent) {
    m_cfg = ConfigManager::loadConfig();
    app_logging::setLogConfig(m_cfg.logEnabled, m_cfg.logLevel);
    m_resolvedLanguage = i18n::resolveLanguage(m_cfg.language);

    qInfo() << "AppController init"
            << "apiSource=" << m_cfg.apiSource
            << "pollMs=" << m_cfg.pollMs
            << "language=" << m_resolvedLanguage
            << "logEnabled=" << m_cfg.logEnabled
            << "logLevel=" << m_cfg.logLevel;

    QStringList ignoredYamlIndexes;
    m_stocks = filterYamlStocks(
        ConfigManager::loadStocksFromYaml(findDataYaml()),
        &ignoredYamlIndexes
    );
    if (m_stocks.isEmpty()) {
        m_stocks = {
            {"sh600519", "Kweichow Moutai"},
            {"sz000001", "Ping An Bank"},
            {"sz300750", "CATL"}
        };
        qInfo() << "Stock list is empty; fallback defaults loaded count=" << m_stocks.size();
    }
    m_lastIgnoredYamlIndexCodes = ignoredYamlIndexes;

    loadExtraWatchItems();

    const QVector<StockItem> merged = mergedWatchItems();
    qInfo() << "Initial watch counts"
            << "stocks=" << m_stocks.size()
            << "indexes=" << m_indexes.size()
            << "sectors=" << m_sectors.size()
            << "merged=" << merged.size();

    m_model = new QuoteModel(this);
    m_model->setLanguage(m_resolvedLanguage);
    m_model->setConfig(m_cfg);
    m_model->setStocks(merged);

    m_window = new FloatingWindow(m_model);
    m_window->setGeometry(m_cfg.windowRect);
    m_window->applyConfig(m_cfg);
    // Window starts hidden; user shows it via tray or hotkey

    setupTray();
    if (m_tray) {
        m_tray->showMessage(
            i18n::t("app.name", m_resolvedLanguage),
            i18n::t("app.started", m_resolvedLanguage),
            QSystemTrayIcon::Information,
            2500
        );

        if (!m_lastIgnoredYamlIndexCodes.isEmpty()) {
            m_tray->showMessage(
                i18n::t("app.name", m_resolvedLanguage),
                i18n::t("settings.indexSector.ignoreYamlIndexFmt", m_resolvedLanguage)
                    .arg(m_lastIgnoredYamlIndexCodes.join(QStringLiteral(", "))),
                QSystemTrayIcon::Warning,
                5000
            );
        }
    }
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    setupHotkey();
#endif
    rebuildProvider();

    m_timer = new QTimer(this);
    m_timer->setInterval(qMax(500, m_cfg.pollMs));
    connect(m_timer, &QTimer::timeout, this, [this]() { refreshQuotes(); });
    m_timer->start();

    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        qInfo() << "Application aboutToQuit. Persisting runtime config.";
        if (m_window) {
            m_cfg.windowRect = m_window->geometry();
        }
        ConfigManager::saveConfig(m_cfg);
    });

    refreshQuotes(true);
}

namespace {

QString defaultDataYamlTemplate() {
    return QStringLiteral(
        "# MyStocks stock list template\n"
        "- code: sh600519\n"
        "  name: Kweichow Moutai\n"
        "- code: sz000001\n"
        "  name: Ping An Bank\n"
        "- code: sz300750\n"
        "  name: CATL\n"
    );
}

bool ensureDataYamlExists(const QString& path) {
    const QFileInfo info(path);
    if (info.exists() && info.size() > 0) {
        return true;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    const QByteArray content = defaultDataYamlTemplate().toUtf8();
    qint64 written = file.write(content);
    return written == content.size();
}

bool isHongKongCode(const QString& rawCode) {
    const QString code = rawCode.trimmed().toLower();
    if (code.isEmpty()) {
        return false;
    }

    if (code.startsWith("hk")
        || code.startsWith(QStringLiteral("116."))
        || code.startsWith(QStringLiteral("128."))) {
        return true;
    }

    static const QSet<QString> hkIndexCodes {
        QStringLiteral("hsi"),
        QStringLiteral("hstech"),
        QStringLiteral("100.hsi"),
        QStringLiteral("124.hstech"),
    };

    if (hkIndexCodes.contains(code)) {
        return true;
    }

    if (code.startsWith(QStringLiteral("100."))
        && hkIndexCodes.contains(code.mid(4))) {
        return true;
    }
    if (code.startsWith(QStringLiteral("124."))
        && hkIndexCodes.contains(code.mid(4))) {
        return true;
    }

    if (code.size() == 5) {
        for (QChar ch : code) {
            if (!ch.isDigit()) {
                return false;
            }
        }
        return true;
    }

    return false;
}

bool isBenignCanceledError(const QString& message) {
    const QString lower = message.trimmed().toLower();
    return lower.contains("operation canceled")
        || lower.contains("operation cancelled")
        || lower.contains("operation cancled");
}

bool isDigitsOnly(const QString& value) {
    if (value.isEmpty()) {
        return false;
    }

    for (QChar ch : value) {
        if (!ch.isDigit()) {
            return false;
        }
    }

    return true;
}

const QSet<QString>& predefinedIndexAliases() {
    static const QSet<QString> aliases {
        QStringLiteral("sh000001"),
        QStringLiteral("sz399001"),
        QStringLiteral("sh000300"),
        QStringLiteral("sz399300"),
        QStringLiteral("sh000016"),
        QStringLiteral("sh000905"),
        QStringLiteral("sh000852"),
        QStringLiteral("sz399006"),
        QStringLiteral("sz399673"),
        QStringLiteral("sh000688"),
        QStringLiteral("sh931643"),
        QStringLiteral("sz931643"),
        QStringLiteral("sh932133"),
        QStringLiteral("sz399431"),
        QStringLiteral("sz399975"),
        QStringLiteral("sh000808"),
        QStringLiteral("sh000932"),
        QStringLiteral("sz399808"),
        QStringLiteral("sh980017"),
        QStringLiteral("sz980017"),
        QStringLiteral("hsi"),
        QStringLiteral("hstech"),
        QStringLiteral("100.hsi"),
        QStringLiteral("124.hstech"),

        // Digits-only aliases from docs/api examples except 000001 (ambiguous with stock).
        QStringLiteral("399001"),
        QStringLiteral("000300"),
        QStringLiteral("399300"),
        QStringLiteral("000016"),
        QStringLiteral("000905"),
        QStringLiteral("000852"),
        QStringLiteral("399006"),
        QStringLiteral("399673"),
        QStringLiteral("000688"),
        QStringLiteral("931643"),
        QStringLiteral("932133"),
        QStringLiteral("399431"),
        QStringLiteral("399975"),
        QStringLiteral("000808"),
        QStringLiteral("000932"),
        QStringLiteral("399808"),
        QStringLiteral("980017"),
    };

    return aliases;
}

bool isPredefinedIndexCode(const QString& rawCode) {
    const QString code = rawCode.trimmed().toLower();
    if (code.isEmpty()) {
        return false;
    }
    if (code.startsWith(QStringLiteral("bk")) || code.startsWith(QStringLiteral("90."))) {
        return false;
    }

    if (predefinedIndexAliases().contains(code)) {
        return true;
    }

    if (isDigitsOnly(code) && code.size() == 6) {
        return predefinedIndexAliases().contains(code);
    }

    return false;
}

QString watchCodeKey(const QString& code) {
    return code.trimmed().toLower();
}

QString normalizeSectorCode(const QString& rawCode) {
    QString code = rawCode.trimmed();
    if (code.isEmpty()) {
        return {};
    }

    if (code.startsWith(QStringLiteral("90."), Qt::CaseInsensitive)) {
        code = code.mid(3);
    }

    if (code.startsWith(QStringLiteral("bk"), Qt::CaseInsensitive)) {
        return code.toUpper();
    }

    return {};
}

QString normalizeHongKongIndexCode(const QString& rawCode) {
    const QString code = rawCode.trimmed().toLower();
    if (code.isEmpty()) {
        return {};
    }

    if (code == QStringLiteral("hsi")
        || code == QStringLiteral("100.hsi")
        || code == QStringLiteral("124.hsi")) {
        return QStringLiteral("100.HSI");
    }

    if (code == QStringLiteral("hstech")
        || code == QStringLiteral("124.hstech")
        || code == QStringLiteral("100.hstech")) {
        return QStringLiteral("124.HSTECH");
    }

    return {};
}

QString encodeWatchItem(const StockItem& item) {
    return item.code.trimmed() + QStringLiteral("\t") + item.name.trimmed();
}

QVector<StockItem> decodeWatchItems(const QStringList& values) {
    QVector<StockItem> out;
    out.reserve(values.size());

    QSet<QString> seen;
    for (const QString& raw : values) {
        const int sep = raw.indexOf(QLatin1Char('\t'));
        const QString code = (sep >= 0 ? raw.left(sep) : raw).trimmed();
        const QString name = (sep >= 0 ? raw.mid(sep + 1) : QString()).trimmed();
        if (code.isEmpty()) {
            continue;
        }

        const QString key = watchCodeKey(code);
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);

        out.push_back({code, name});
    }

    return out;
}

QStringList encodeWatchItems(const QVector<StockItem>& items) {
    QStringList out;
    out.reserve(items.size());
    for (const StockItem& item : items) {
        if (item.code.trimmed().isEmpty()) {
            continue;
        }
        out.push_back(encodeWatchItem(item));
    }
    return out;
}

} // namespace

QString AppController::findDataYaml() const {
    // First check user home directory ~/.myStocks/data.yaml
    const QString homeDataPath = QDir(QDir::homePath()).filePath(".myStocks/data.yaml");
    if (QFile::exists(homeDataPath)) {
        return QDir::cleanPath(homeDataPath);
    }

#ifdef DEBUG_MODE
    // In debug mode, read from source directory
    const QString sourceDataPath = QString(SOURCE_DIR) + "/data.yaml";
    if (QFile::exists(sourceDataPath)) {
        qDebug() << "Debug mode: Reading data.yaml from:" << sourceDataPath;
        return QDir::cleanPath(sourceDataPath);
    }
    qDebug() << "Debug mode: data.yaml not found in source directory:" << sourceDataPath;
    // In debug mode, if source data.yaml doesn't exist, return empty (no fallback)
    return {};
#else
    const QString appDataPath = QDir(QCoreApplication::applicationDirPath()).filePath("data.yaml");
    if (ensureDataYamlExists(appDataPath)) {
        return QDir::cleanPath(appDataPath);
    }
    return {};
#endif
}

bool AppController::isSectorCode(const QString& code) {
    return !normalizeSectorCode(code).isEmpty();
}

QVector<StockItem> AppController::filterYamlStocks(
    const QVector<StockItem>& loaded,
    QStringList* ignoredCodes
) const {
    QVector<StockItem> out;
    out.reserve(loaded.size());

    QSet<QString> seen;
    QStringList ignored;
    for (const StockItem& item : loaded) {
        const QString code = item.code.trimmed();
        if (code.isEmpty()) {
            continue;
        }

        if (isPredefinedIndexCode(code)) {
            ignored.push_back(code);
            continue;
        }

        const QString key = watchCodeKey(code);
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);

        out.push_back({code, item.name.trimmed()});
    }

    if (ignoredCodes) {
        ignored.removeDuplicates();
        *ignoredCodes = ignored;
    }

    return out;
}

QVector<StockItem> AppController::mergedWatchItems() const {
    QVector<StockItem> out;
    out.reserve(m_indexes.size() + m_sectors.size() + m_stocks.size());

    QSet<QString> seen;
    const auto appendUnique = [&out, &seen](const StockItem& item) {
        const QString code = item.code.trimmed();
        if (code.isEmpty()) {
            return;
        }
        const QString key = watchCodeKey(code);
        if (seen.contains(key)) {
            return;
        }
        seen.insert(key);
        out.push_back({code, item.name.trimmed()});
    };

    for (const StockItem& item : m_indexes) {
        appendUnique(item);
    }

    for (const StockItem& item : m_sectors) {
        const QString sectorCode = normalizeSectorCode(item.code);
        if (sectorCode.isEmpty()) {
            continue;
        }
        appendUnique({sectorCode, item.name});
    }

    for (const StockItem& item : m_stocks) {
        appendUnique(item);
    }

    return out;
}

void AppController::loadExtraWatchItems() {
    QSettings s("myStocks", "myStocks");

    m_indexes = decodeWatchItems(s.value("watch/indexes").toStringList());

    QVector<StockItem> decodedSectors = decodeWatchItems(s.value("watch/sectors").toStringList());
    m_sectors.clear();
    m_sectors.reserve(decodedSectors.size());

    QSet<QString> sectorSeen;
    for (const StockItem& sector : decodedSectors) {
        const QString code = normalizeSectorCode(sector.code);
        if (code.isEmpty()) {
            continue;
        }

        const QString key = watchCodeKey(code);
        if (sectorSeen.contains(key)) {
            continue;
        }
        sectorSeen.insert(key);

        m_sectors.push_back({code, sector.name.trimmed()});
    }
}

void AppController::saveExtraWatchItems() const {
    QSettings s("myStocks", "myStocks");
    s.setValue("watch/indexes", encodeWatchItems(m_indexes));
    s.setValue("watch/sectors", encodeWatchItems(m_sectors));
    s.sync();
}

void AppController::toggleWindow() {
    if (!m_window) {
        return;
    }

    if (m_window->isVisible()) {
        m_window->hide();
    } else {
        m_window->show();
        if (m_cfg.floatingWindowAlwaysOnTop) {
            m_window->raise();
            m_window->activateWindow();
        }
    }
}

void AppController::openSettings() {
    qInfo() << "Open settings dialog.";

    AppConfig updatedCfg = m_cfg;
    bool accepted = false;
    {
        SettingsDialog dlg(
            m_cfg,
            m_stocks,
            m_indexes,
            m_sectors,
            currentApiNamesByCode(),
            findDataYaml(),
            [this]() {
                const int updatedCount = writeApiNamesToDataYaml();
                QMessageBox::information(
                    m_window,
                    i18n::t("app.name", m_resolvedLanguage),
                    i18n::t("settings.other.writeStockNamesResult", m_resolvedLanguage)
                        .arg(updatedCount)
                );
            },
            m_window
        );
        accepted = (dlg.exec() == QDialog::Accepted);
        if (accepted) {
            updatedCfg = dlg.config();
            m_indexes = dlg.selectedIndexes();
            m_sectors = dlg.selectedSectors();
            saveExtraWatchItems();
        }
    }

    // Always reload stocks from yaml: the stocks tab may have saved changes.
    {
        const QString dataPath = findDataYaml();
        if (!dataPath.isEmpty()) {
            const QVector<StockItem> reloadedRaw = ConfigManager::loadStocksFromYaml(dataPath);
            QStringList ignoredIndexes;
            const QVector<StockItem> reloaded = filterYamlStocks(reloadedRaw, &ignoredIndexes);

            if (!ignoredIndexes.isEmpty()) {
                m_lastIgnoredYamlIndexCodes = ignoredIndexes;
                QMessageBox::information(
                    m_window,
                    i18n::t("app.name", m_resolvedLanguage),
                    i18n::t("settings.indexSector.ignoreYamlIndexFmt", m_resolvedLanguage)
                        .arg(ignoredIndexes.join(QStringLiteral(", ")))
                );
            }

            if (!reloadedRaw.isEmpty() || !ignoredIndexes.isEmpty()) {
                m_stocks = reloaded;
                m_apiNamesByCode.clear();
                if (m_model) {
                    m_model->setStocks(mergedWatchItems());
                }
            }
        }
    }

    if (!accepted) {
        qInfo() << "Settings dialog canceled by user.";
        refreshQuotes(true);
        return;
    }

    qInfo() << "Settings accepted"
            << "apiSource:" << m_cfg.apiSource << "->" << updatedCfg.apiSource
            << "pollMs:" << m_cfg.pollMs << "->" << updatedCfg.pollMs
            << "language:" << m_cfg.language << "->" << updatedCfg.language
            << "logEnabled:" << m_cfg.logEnabled << "->" << updatedCfg.logEnabled
            << "logLevel:" << m_cfg.logLevel << "->" << updatedCfg.logLevel;

    m_cfg = updatedCfg;
    m_resolvedLanguage = i18n::resolveLanguage(m_cfg.language);
    m_probeCheckedAt = QDateTime();

    // Preserve window position before destroying.
    QPoint windowPos(120, 120);
    bool wasVisible = false;
    if (m_window) {
        windowPos = m_window->pos();
        wasVisible = m_window->isVisible();
        delete m_window;
        m_window = nullptr;
    }

    ConfigManager::saveConfig(m_cfg);
    app_logging::setLogConfig(m_cfg.logEnabled, m_cfg.logLevel);
    qInfo() << "Settings persisted and log config applied.";

    m_model->setLanguage(m_resolvedLanguage);
    m_model->setConfig(m_cfg);
    m_model->setStocks(mergedWatchItems());

    // Recreate the floating window so column visibility/order takes effect cleanly.
    m_window = new FloatingWindow(m_model);
    m_window->move(windowPos);
    m_window->applyConfig(m_cfg);
    if (wasVisible) {
        m_window->show();
        if (m_cfg.floatingWindowAlwaysOnTop) {
            m_window->raise();
            m_window->activateWindow();
        }
    }

    if (m_timer) {
        m_timer->setInterval(qMax(500, m_cfg.pollMs));
    }

    setupTray();
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    setupHotkey();
#endif
    rebuildProvider();
    updateTrayTooltip();
    refreshQuotes(true);
}

void AppController::reloadStocksFromYaml() {
    qInfo() << "Manual reload from data.yaml requested.";

    const QString dataPath = findDataYaml();
    if (dataPath.isEmpty()) {
        qWarning() << "Reload stocks failed: data.yaml path not found.";
        if (m_tray) {
            m_tray->showMessage(
                i18n::t("app.name", m_resolvedLanguage),
                i18n::t("reload.config.failed", m_resolvedLanguage),
                QSystemTrayIcon::Warning,
                2500
            );
        }
        return;
    }

    const QVector<StockItem> loadedRaw = ConfigManager::loadStocksFromYaml(dataPath);
    if (loadedRaw.isEmpty()) {
        qWarning() << "Reload stocks failed: no valid stocks in" << dataPath;
        if (m_tray) {
            m_tray->showMessage(
                i18n::t("app.name", m_resolvedLanguage),
                i18n::t("reload.config.failed", m_resolvedLanguage),
                QSystemTrayIcon::Warning,
                2500
            );
        }
        return;
    }

    QStringList ignoredIndexes;
    const QVector<StockItem> loaded = filterYamlStocks(loadedRaw, &ignoredIndexes);

    m_stocks = loaded;
    m_lastIgnoredYamlIndexCodes = ignoredIndexes;
    qInfo() << "Reload stocks success count=" << m_stocks.size() << "path=" << dataPath;
    m_apiNamesByCode.clear();
    m_probeDate = QDate();
    m_probeCheckedAt = QDateTime();
    m_probeTradingDay = true;

    if (m_model) {
        m_model->setStocks(mergedWatchItems());
    }

    // Manual reload should refresh immediately regardless of polling window.
    refreshQuotes(true);

    if (m_window && m_window->isVisible() && m_cfg.floatingWindowAlwaysOnTop) {
        m_window->raise();
    }

    if (m_tray) {
        m_tray->showMessage(
            i18n::t("app.name", m_resolvedLanguage),
            i18n::t("reload.config.success", m_resolvedLanguage).arg(m_stocks.size()),
            QSystemTrayIcon::Information,
            2000
        );

        if (!ignoredIndexes.isEmpty()) {
            m_tray->showMessage(
                i18n::t("app.name", m_resolvedLanguage),
                i18n::t("settings.indexSector.ignoreYamlIndexFmt", m_resolvedLanguage)
                    .arg(ignoredIndexes.join(QStringLiteral(", "))),
                QSystemTrayIcon::Warning,
                5000
            );
        }
    }
}

void AppController::refreshQuotes(bool force) {
    if (!m_provider && !m_sectorProvider) {
        return;
    }

    if (!force && !shouldPollNow()) {
        return;
    }

    const QVector<StockItem> merged = mergedWatchItems();

    // EastMoney source now supports mixed batch quotes (index + sector + stock)
    // via ulist endpoint, so avoid splitting into separate requests.
    if (m_cfg.apiSource == QStringLiteral("eastmoney")) {
        QVector<StockItem> allItems;
        allItems.reserve(merged.size());
        for (const StockItem& item : merged) {
            const QString sectorCode = normalizeSectorCode(item.code);
            if (!sectorCode.isEmpty()) {
                allItems.push_back({sectorCode, item.name});
                continue;
            }

            const QString hkIndexCode = normalizeHongKongIndexCode(item.code);
            if (!hkIndexCode.isEmpty()) {
                allItems.push_back({hkIndexCode, item.name});
                continue;
            }

            allItems.push_back(item);
        }

        if (m_provider && !allItems.isEmpty()) {
            m_provider->fetchQuotes(allItems);
        }
        return;
    }

    QVector<StockItem> marketItems;
    QVector<StockItem> sectorItems;
    marketItems.reserve(merged.size());
    sectorItems.reserve(merged.size());

    for (const StockItem& item : merged) {
        const QString sectorCode = normalizeSectorCode(item.code);
        if (!sectorCode.isEmpty()) {
            sectorItems.push_back({sectorCode, item.name});
            continue;
        }

        const QString hkIndexCode = normalizeHongKongIndexCode(item.code);
        if (!hkIndexCode.isEmpty()) {
            // Hang Seng family indexes should be fetched with EastMoney sector batch.
            sectorItems.push_back({hkIndexCode, item.name});
            continue;
        }

        marketItems.push_back(item);
    }

    if (m_provider && !marketItems.isEmpty()) {
        m_provider->fetchQuotes(marketItems);
    }
    if (m_sectorProvider && !sectorItems.isEmpty()) {
        m_sectorProvider->fetchQuotes(sectorItems);
    }
}

void AppController::onProviderError(const QString& message) {
    const QString text = message.trimmed();
    if (text.isEmpty()) {
        return;
    }

    if (isBenignCanceledError(text)) {
        qInfo() << "Suppress benign network canceled error in tray:" << text;
        return;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (m_lastTrayErrorAt.isValid()) {
        // Global cooldown: avoid rapid-fire tray spam when provider/network is unstable.
        if (m_lastTrayErrorAt.msecsTo(now) < 15000) {
            qInfo() << "Suppress tray error due cooldown:" << text;
            return;
        }

        // Same error cooldown: don't repeat identical warnings too frequently.
        if (text == m_lastTrayErrorMessage && m_lastTrayErrorAt.msecsTo(now) < 180000) {
            qInfo() << "Suppress duplicate tray error:" << text;
            return;
        }
    }

    m_lastTrayErrorMessage = text;
    m_lastTrayErrorAt = now;

    if (m_tray) {
        m_tray->showMessage(i18n::t("app.name", m_resolvedLanguage), text, QSystemTrayIcon::Warning, 2500);
    }
}

void AppController::setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    if (m_tray) {
        m_tray->hide();
        m_tray->deleteLater();
        m_tray = nullptr;
    }

    QIcon icon(QStringLiteral(":/icon.png"));
    if (icon.isNull()) {
        icon = QIcon::fromTheme("view-financial");
    }
    if (icon.isNull()) {
        icon = qApp->style()->standardIcon(QStyle::SP_DesktopIcon);
    }

    m_tray = new QSystemTrayIcon(icon, this);
    QMenu* menu = new QMenu;

    menu->addAction(i18n::t("tray.toggle", m_resolvedLanguage), this, [this]() { toggleWindow(); });
    menu->addAction(i18n::t("tray.settings", m_resolvedLanguage), this, [this]() { openSettings(); });
    menu->addAction(i18n::t("tray.reload", m_resolvedLanguage), this, [this]() { reloadStocksFromYaml(); });
    menu->addSeparator();
    menu->addAction(i18n::t("tray.quit", m_resolvedLanguage), qApp, &QCoreApplication::quit);

    m_tray->setContextMenu(menu);
    m_tray->show();

    connect(m_tray, &QSystemTrayIcon::activated, this,
        [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) {
                toggleWindow();
            }
        }
    );
}

void AppController::updateTrayTooltip() {
    if (!m_tray) {
        return;
    }
    if (!m_cfg.trayTooltipEnabled) {
        m_tray->setToolTip(QString());
        return;
    }
    m_tray->setToolTip(m_model->trayTooltipText());
}

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
void AppController::setupHotkey() {
    if (m_hotkey) {
        m_hotkey->setRegistered(false);
        m_hotkey->deleteLater();
        m_hotkey = nullptr;
    }

    if (m_cfg.hotkey.trimmed().isEmpty()) {
        return;
    }

    m_hotkey = new QHotkey(
        QKeySequence::fromString(m_cfg.hotkey, QKeySequence::PortableText),
        true, this
    );
    connect(m_hotkey, &QHotkey::activated, this, [this]() { toggleWindow(); });

    if (!m_hotkey->isRegistered() && m_tray) {
        m_tray->showMessage(
            i18n::t("app.name", m_resolvedLanguage),
            i18n::t("hotkey.register.failed", m_resolvedLanguage),
            QSystemTrayIcon::Warning,
            3000
        );
    }
}
#endif

void AppController::rebuildProvider() {
    if (m_provider) {
        disconnect(m_provider, nullptr, this, nullptr);
        m_provider->deleteLater();
        m_provider = nullptr;
    }
    if (m_sectorProvider) {
        disconnect(m_sectorProvider, nullptr, this, nullptr);
        m_sectorProvider->deleteLater();
        m_sectorProvider = nullptr;
    }

    if (m_cfg.apiSource == "xtick") {
        m_provider = new XTickQuoteProvider(m_cfg.xtickToken, this);
    } else if (m_cfg.apiSource == "sina") {
        m_provider = new SinaQuoteProvider(this);
    } else if (m_cfg.apiSource == "tencent") {
        m_provider = new TencentQuoteProvider(this);
    } else if (m_cfg.apiSource == "eastmoney") {
        m_provider = new EastMoneyQuoteProvider(this);
    } else {
        m_provider = new MockQuoteProvider(this);
    }

    qInfo() << "Rebuild provider done. source=" << m_cfg.apiSource;

    m_provider->setLanguage(m_resolvedLanguage);
    m_provider->applyConfig(m_cfg);

    // Sector(BKxxxx) quotes are always fetched from EastMoney so they can work
    // even when the primary provider is Tencent/Sina/XTick.
    // When primary source is EastMoney, m_provider already fetches everything in batch.
    if (m_cfg.apiSource != "eastmoney") {
        m_sectorProvider = new EastMoneyQuoteProvider(this);
        m_sectorProvider->setLanguage(m_resolvedLanguage);
        m_sectorProvider->applyConfig(m_cfg);
    }

    m_apiNamesByCode.clear();
    const QString sourceAtConnect = m_cfg.apiSource;
    connect(m_provider, &IQuoteProvider::quotesReady, this, [this, sourceAtConnect](const QVector<QuoteItem>& quotes) {
        if (sourceAtConnect == "mock") {
            return;
        }

        for (const QuoteItem& q : quotes) {
            const QString code = q.code.trimmed();
            const QString name = q.name.trimmed();
            if (code.isEmpty() || name.isEmpty()) {
                continue;
            }
            m_apiNamesByCode.insert(code, name);
        }
    });

    connect(m_provider, &IQuoteProvider::quotesReady, m_model, &QuoteModel::updateQuotes);
    connect(m_provider, &IQuoteProvider::quotesReady, this, [this](const QVector<QuoteItem>&) {
        updateTrayTooltip();
    });
    connect(m_provider, &IQuoteProvider::error, this, [this](const QString& msg) {
        onProviderError(msg);
    });

    if (m_sectorProvider) {
        connect(m_sectorProvider, &IQuoteProvider::quotesReady, this, [this](const QVector<QuoteItem>& quotes) {
            for (const QuoteItem& q : quotes) {
                const QString code = q.code.trimmed();
                const QString name = q.name.trimmed();
                if (code.isEmpty() || name.isEmpty()) {
                    continue;
                }
                m_apiNamesByCode.insert(code, name);
            }
        });
        connect(m_sectorProvider, &IQuoteProvider::quotesReady, m_model, &QuoteModel::updateQuotes);
        connect(m_sectorProvider, &IQuoteProvider::quotesReady, this, [this](const QVector<QuoteItem>&) {
            updateTrayTooltip();
        });
        connect(m_sectorProvider, &IQuoteProvider::error, this, [this](const QString& msg) {
            onProviderError(msg);
        });
    }
}

QHash<QString, QString> AppController::currentApiNamesByCode() const {
    QHash<QString, QString> out;
    out.reserve(m_apiNamesByCode.size());

    for (auto it = m_apiNamesByCode.constBegin(); it != m_apiNamesByCode.constEnd(); ++it) {
        const QString code = it.key().trimmed();
        const QString name = it.value().trimmed();
        if (code.isEmpty() || name.isEmpty()) {
            continue;
        }
        out.insert(code, name);
    }

    if (out.isEmpty() && m_model) {
        const int rows = m_model->rowCount();
        for (int row = 0; row < rows; ++row) {
            const QString code = m_model->data(
                m_model->index(row, ColCode),
                Qt::DisplayRole
            ).toString().trimmed();
            const QString name = m_model->data(
                m_model->index(row, ColName),
                Qt::DisplayRole
            ).toString().trimmed();

            if (code.isEmpty() || name.isEmpty()) {
                continue;
            }
            if (name.compare(code, Qt::CaseInsensitive) == 0) {
                continue;
            }

            out.insert(code, name);
        }
    }

    return out;
}

int AppController::writeApiNamesToDataYaml() {
    const QHash<QString, QString> apiNames = currentApiNamesByCode();
    if (apiNames.isEmpty()) {
        qInfo() << "Write stock names skipped: api names are empty.";
        return 0;
    }

    const QString homeDataPath = QDir(QDir::homePath()).filePath(".myStocks/data.yaml");
    const auto nativePath = [](const QString& path) {
        return QDir::toNativeSeparators(path);
    };

    QString sourcePath = findDataYaml();
    if (sourcePath.isEmpty()) {
        sourcePath = homeDataPath;
    }

    qInfo() << "Write stock names resolved source path:" << nativePath(sourcePath);
    qInfo() << "Write stock names resolved home path:" << nativePath(homeDataPath);

    if (sourcePath.isEmpty()) {
        qWarning() << "Write stock names skipped: data.yaml path is empty.";
        return 0;
    }

    QFile inFile(sourcePath);
    if (!inFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (sourcePath != homeDataPath) {
            sourcePath = homeDataPath;
            inFile.setFileName(sourcePath);
        }
        if (!inFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "Write stock names skipped: cannot open" << sourcePath << inFile.errorString();
            return 0;
        }
    }

    const QString original = QString::fromUtf8(inFile.readAll());
    const bool hadTrailingNewline = original.endsWith('\n');
    QStringList lines = original.split('\n', Qt::KeepEmptyParts);

    const QRegularExpression reCode("^([ \\t]*)-\\s*code\\s*:\\s*(\\S+)\\s*$");
    const QRegularExpression reName("^([ \\t]*)name\\s*:\\s*(.*)$");

    bool inStocks = false;
    int updatedCount = 0;

    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);
        const QString trimmed = line.trimmed();

        if (!inStocks) {
            if (trimmed == "stocks:") {
                inStocks = true;
            }
            continue;
        }

        if (trimmed.isEmpty() || trimmed.startsWith('#')) {
            continue;
        }

        if (!line.startsWith(' ') && trimmed.endsWith(':') && trimmed != "stocks:") {
            inStocks = false;
            continue;
        }

        const QRegularExpressionMatch codeMatch = reCode.match(line);
        if (!codeMatch.hasMatch()) {
            continue;
        }

        const QString codeIndent = codeMatch.captured(1);
        const QString code = codeMatch.captured(2).trimmed();
        const QString apiName = apiNames.value(code).trimmed();
        if (apiName.isEmpty()) {
            continue;
        }

        bool hasNameLine = false;
        int insertAt = i + 1;

        for (int j = i + 1; j < lines.size(); ++j) {
            const QString nextLine = lines.at(j);
            const QString nextTrimmed = nextLine.trimmed();

            if (nextTrimmed.isEmpty() || nextTrimmed.startsWith('#')) {
                insertAt = j + 1;
                continue;
            }

            if (reCode.match(nextLine).hasMatch()) {
                insertAt = j;
                break;
            }

            if (!nextLine.startsWith(codeIndent + "  ") && !nextLine.startsWith(codeIndent + "\t")) {
                insertAt = j;
                break;
            }

            const QRegularExpressionMatch nameMatch = reName.match(nextLine);
            if (nameMatch.hasMatch()) {
                const QString oldName = nameMatch.captured(2).trimmed();
                if (oldName != apiName) {
                    lines[j] = nameMatch.captured(1) + "name: " + apiName;
                    ++updatedCount;
                }
                hasNameLine = true;
                break;
            }

            insertAt = j + 1;
        }

        if (!hasNameLine) {
            lines.insert(insertAt, codeIndent + "  name: " + apiName);
            ++updatedCount;
            ++i;
        }
    }

    if (updatedCount <= 0) {
        qInfo() << "Write stock names skipped: no yaml content changes.";
        return 0;
    }

    QString updatedContent = lines.join('\n');
    if (hadTrailingNewline && !updatedContent.endsWith('\n')) {
        updatedContent.append('\n');
    }

    const QByteArray bytes = updatedContent.toUtf8();

    auto writeContentToPath = [&](const QString& targetPath) -> bool {
        qInfo() << "Write stock names target path:" << nativePath(targetPath);
        qInfo() << "Write stock names content bytes:" << bytes.size();
        qInfo().noquote() << "Write stock names content begin\n" + updatedContent + "\nWrite stock names content end";

        QSaveFile outFile(targetPath);
        if (!outFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning() << "Write stock names failed: cannot open" << targetPath << outFile.errorString();
            return false;
        }

        if (outFile.write(bytes) != bytes.size()) {
            qWarning() << "Write stock names failed: write error" << outFile.errorString();
            outFile.cancelWriting();
            return false;
        }

        if (!outFile.commit()) {
            qWarning() << "Write stock names failed: commit error" << outFile.errorString();
            return false;
        }

        return true;
    };

    bool writeOk = writeContentToPath(sourcePath);

    if (!writeOk && sourcePath != homeDataPath) {
        const QString homeDirPath = QFileInfo(homeDataPath).absolutePath();
        if (!QDir().mkpath(homeDirPath)) {
            qWarning() << "Write stock names failed: cannot create home data dir" << homeDirPath;
            return 0;
        }

        if (!writeContentToPath(homeDataPath)) {
            return 0;
        }

        qInfo() << "Write stock names fallback path:" << homeDataPath;
    } else if (!writeOk) {
        return 0;
    }

    for (StockItem& stock : m_stocks) {
        const QString updatedName = apiNames.value(stock.code).trimmed();
        if (updatedName.isEmpty()) {
            continue;
        }
        stock.name = updatedName;
    }

    return updatedCount;
}

bool AppController::shouldPollNow() {
    if (m_cfg.debugIgnoreTradingTime) {
        return true;
    }

    if (m_cfg.apiSource == "mock") {
        return true;
    }

    const QTimeZone bjZone("Asia/Shanghai");
    if (!bjZone.isValid()) {
        return true;
    }

    const QDateTime bjNow = QDateTime::currentDateTimeUtc().toTimeZone(bjZone);
    if (!isWithinTradingSession(bjNow)) {
        return false;
    }

    const QDate today = bjNow.date();
    if (m_probeDate != today) {
        m_probeDate = today;
        m_probeCheckedAt = QDateTime();
        m_probeTradingDay = true;
    }

    const bool needProbe = !m_probeCheckedAt.isValid()
        || m_probeCheckedAt.secsTo(bjNow) >= 300;

    if (needProbe) {
        m_probeTradingDay = probeTradingDay(today);
        m_probeCheckedAt = bjNow;
    }

    return m_probeTradingDay;
}

bool AppController::hasHongKongStocks() const {
    const QVector<StockItem> merged = mergedWatchItems();
    for (const StockItem& stock : merged) {
        if (isHongKongCode(stock.code)) {
            return true;
        }
    }
    return false;
}

bool AppController::isWithinTradingSession(const QDateTime& bjNow) const {
    if (hasHongKongStocks()) {
        return isWithinHongKongTradingSession(bjNow);
    }

    if (!bjNow.isValid()) {
        return true;
    }

    if (bjNow.date().dayOfWeek() > 5) {
        return false;
    }

    const QTime t = bjNow.time();
    const bool morning = (t >= QTime(9, 30) && t < QTime(11, 30));
    const bool afternoon = (t >= QTime(13, 0) && t < QTime(15, 0));
    return morning || afternoon;
}

bool AppController::isWithinHongKongTradingSession(const QDateTime& bjNow) const {
    if (!bjNow.isValid()) {
        return true;
    }

    if (bjNow.date().dayOfWeek() > 5) {
        return false;
    }

    const QTime t = bjNow.time();
    const bool morning = (t >= QTime(9, 30) && t < QTime(12, 0));
    const bool afternoon = (t >= QTime(13, 0) && t < QTime(16, 0));
    return morning || afternoon;
}

bool AppController::probeTradingDay(const QDate& bjDate) {
    const bool useHongKongProbe = hasHongKongStocks();

    const QNetworkProxy proxy = network_utils::proxyFromConfig(m_cfg);
    m_probeNam.setProxy(proxy);

    const QUrl probeUrl = useHongKongProbe
        ? QUrl("https://qt.gtimg.cn/q=hk00700")
        : QUrl("https://qt.gtimg.cn/q=sh000001");

    QNetworkRequest req(probeUrl);
    req.setRawHeader("User-Agent", network_utils::effectiveUserAgent(m_cfg).toUtf8());
    req.setRawHeader("Referer", useHongKongProbe ? "https://gu.qq.com/hk/" : "https://gu.qq.com");
    req.setTransferTimeout(network_logger::kNetworkRequestTimeoutMs);

    const network_logger::RequestTrace trace = network_logger::logRequestStart(
        useHongKongProbe ? "market-probe-hk" : "market-probe",
        "GET",
        req,
        proxy
    );

    QNetworkReply* reply = m_probeNam.get(req);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);

    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    timeout.start(network_logger::kNetworkRequestTimeoutMs);
    loop.exec();

    if (timeout.isActive()) {
        timeout.stop();
    }

    if (!reply->isFinished()) {
        reply->abort();
    }

    const QByteArray body = reply->readAll();
    network_logger::logRequestFinish(trace, reply, body.size(), body);

    const QString errorText = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    reply->deleteLater();

    if (!errorText.isEmpty()) {
        // Fallback to avoid losing intraday updates when probe endpoint is temporarily unreachable.
        return true;
    }

    const QString tradeDateText = probeTradingDateText(body);
    if (tradeDateText.size() != 8) {
        return true;
    }

    const QDate tradeDate = QDate::fromString(tradeDateText, "yyyyMMdd");
    if (!tradeDate.isValid()) {
        return true;
    }

    return tradeDate == bjDate;
}

QString AppController::probeTradingDateText(const QByteArray& body) const {
    const QString text = QString::fromLatin1(body);
    {
        const QRegularExpression compactRe("(20\\d{2})(\\d{2})(\\d{2})\\d{6}");
        const QRegularExpressionMatch match = compactRe.match(text);
        if (match.hasMatch()) {
            return match.captured(1) + match.captured(2) + match.captured(3);
        }
    }

    {
        const QRegularExpression dashedRe("(20\\d{2})-(\\d{2})-(\\d{2})");
        const QRegularExpressionMatch match = dashedRe.match(text);
        if (match.hasMatch()) {
            return match.captured(1) + match.captured(2) + match.captured(3);
        }
    }

    return {};
}
