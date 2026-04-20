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
#ifdef WIN32
#include <QHotkey>
#endif
#include <QMenu>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTimeZone>

AppController::AppController(QObject* parent)
    : QObject(parent) {
    m_cfg = ConfigManager::loadConfig();
    app_logging::setLogConfig(m_cfg.logEnabled, m_cfg.logLevel);
    m_resolvedLanguage = i18n::resolveLanguage(m_cfg.language);

    m_stocks = ConfigManager::loadStocksFromYaml(findDataYaml());
    if (m_stocks.isEmpty()) {
        m_stocks = {
            {"sh000001", "SSE Composite"},
            {"sz399001", "SZSE Component"},
            {"sz399006", "ChiNext"}
        };
    }

    m_model = new QuoteModel(this);
    m_model->setLanguage(m_resolvedLanguage);
    m_model->setConfig(m_cfg);
    m_model->setStocks(m_stocks);

    m_window = new FloatingWindow(m_model);
    m_window->setGeometry(m_cfg.windowRect);
    m_window->applyConfig(m_cfg);
    m_window->show();

    setupTray();
#ifdef WIN32
    setupHotkey();
#endif
    rebuildProvider();

    m_timer = new QTimer(this);
    m_timer->setInterval(qMax(500, m_cfg.pollMs));
    connect(m_timer, &QTimer::timeout, this, [this]() { refreshQuotes(); });
    m_timer->start();

    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        qInfo() << "Application aboutToQuit.";
        if (m_window) {
            m_cfg.windowRect = m_window->geometry();
        }
        ConfigManager::saveConfig(m_cfg);
    });

    refreshQuotes();
}

namespace {

QString defaultDataYamlTemplate() {
    return QStringLiteral(
        "# MyStocks stock list template\n"
        "- code: sh000001\n"
        "  name: SSE Composite\n"
        "- code: sz399001\n"
        "  name: SZSE Component\n"
        "- code: sz399006\n"
        "  name: ChiNext\n"
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
    return rawCode.trimmed().toLower().startsWith("hk");
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

void AppController::toggleWindow() {
    if (!m_window) {
        return;
    }

    if (m_window->isVisible()) {
        m_window->hide();
    } else {
        m_window->show();
        m_window->raise();
        m_window->activateWindow();
    }
}

void AppController::openSettings() {
    AppConfig updatedCfg = m_cfg;
    {
        SettingsDialog dlg(
            m_cfg,
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
        if (dlg.exec() != QDialog::Accepted) {
            return;
        }
        updatedCfg = dlg.config();
    }

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

    m_model->setLanguage(m_resolvedLanguage);
    m_model->setConfig(m_cfg);

    // Recreate the floating window so column visibility/order takes effect cleanly.
    m_window = new FloatingWindow(m_model);
    m_window->move(windowPos);
    m_window->applyConfig(m_cfg);
    if (wasVisible) {
        m_window->show();
        m_window->raise();
        m_window->activateWindow();
    }

    if (m_timer) {
        m_timer->setInterval(qMax(500, m_cfg.pollMs));
    }

    setupTray();
#ifdef WIN32
    setupHotkey();
#endif
    rebuildProvider();
    refreshQuotes();
}

void AppController::reloadStocksFromYaml() {
    const QString dataPath = findDataYaml();
    if (dataPath.isEmpty()) {
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

    const QVector<StockItem> loaded = ConfigManager::loadStocksFromYaml(dataPath);
    if (loaded.isEmpty()) {
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

    m_stocks = loaded;
    m_apiNamesByCode.clear();
    m_probeDate = QDate();
    m_probeCheckedAt = QDateTime();
    m_probeTradingDay = true;

    if (m_model) {
        m_model->setStocks(m_stocks);
    }

    if (m_provider) {
        // Manual reload should refresh immediately regardless of polling window.
        m_provider->fetchQuotes(m_stocks);
    }

    if (m_window && m_window->isVisible()) {
        m_window->raise();
    }

    if (m_tray) {
        m_tray->showMessage(
            i18n::t("app.name", m_resolvedLanguage),
            i18n::t("reload.config.success", m_resolvedLanguage).arg(m_stocks.size()),
            QSystemTrayIcon::Information,
            2000
        );
    }
}

void AppController::refreshQuotes() {
    if (!m_provider) {
        return;
    }

    if (!shouldPollNow()) {
        return;
    }

    m_provider->fetchQuotes(m_stocks);
}

void AppController::onProviderError(const QString& message) {
    if (m_tray) {
        m_tray->showMessage(i18n::t("app.name", m_resolvedLanguage), message, QSystemTrayIcon::Warning, 2500);
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

    QIcon icon = QIcon::fromTheme("view-financial");
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

#ifdef WIN32
void AppController::setupHotkey() {
    if (m_hotkey) {
        m_hotkey->setRegistered(false);
        m_hotkey->deleteLater();
        m_hotkey = nullptr;
    }

    if (m_cfg.hotkey.trimmed().isEmpty()) {
        return;
    }

    m_hotkey = new QHotkey(QKeySequence(m_cfg.hotkey), true, this);
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

    m_provider->setLanguage(m_resolvedLanguage);
    m_provider->applyConfig(m_cfg);

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
    connect(m_provider, &IQuoteProvider::error, this, [this](const QString& msg) {
        onProviderError(msg);
    });
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
    for (const StockItem& stock : m_stocks) {
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
