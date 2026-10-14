#include "app_controller.h"
#include "app_logging.h"
#include "config_manager.h"
#include "floating_window.h"
#include "i18n.h"
#include "quote_model.h"
#include "quote_provider.h"
#include "startup_splash.h"
#include "tdx_bridge.h"

#include <QJsonObject>
#include <QSystemTrayIcon>
#include <QTimer>

void AppController::beginLoading(bool coldStart) {
    if (m_loading) {
        m_reloadQueued = true;
        return;
    }
    m_loading = true;
    m_coldStart = coldStart;
    m_routesReady = false;
    m_loadCompleted = false;
    m_routeErrors.clear();
    m_routeWarnings.clear();
    m_routeConfig = ConfigManager::loadConnectionConfig();
    if (!coldStart && !m_cfg.tdxRememberToken) m_routeConfig.tdxToken = m_cfg.tdxToken;
    for (auto* timer : {m_timer, m_hotRankTimer, m_marketBreadthTimer}) {
        if (timer) timer->stop();
    }
    // Old callbacks cannot update the model or queue more quotation requests during probing.
    if (m_provider) {
        disconnect(m_provider, nullptr, nullptr, nullptr);
        m_provider->deleteLater();
        m_provider = nullptr;
    }
    if (m_marketBreadthProvider) {
        disconnect(m_marketBreadthProvider, nullptr, nullptr, nullptr);
        m_marketBreadthProvider->deleteLater();
        m_marketBreadthProvider = nullptr;
    }
    if (m_window) m_window->setLoading(true);
    if (!m_splash) {
        m_splash = new StartupSplash;
        connect(m_splash, &StartupSplash::retryRequested, this, [this]() {
            m_loading = false;
            m_reloadQueued = false;
            beginLoading(m_coldStart);
        });
        connect(m_splash, &StartupSplash::offlineRequested, this, [this]() { loadAfterRoutes(); });
    }
    m_splash->setLanguage(m_resolvedLanguage);
    m_splash->setWarning({});
    m_splash->setStatus(i18n::t(coldStart ? "splash.starting" : "splash.reloading", m_resolvedLanguage),
        i18n::t("splash.probing", m_resolvedLanguage));
    m_splash->setRouteStatus(false, i18n::t("splash.mainWaiting", m_resolvedLanguage));
    m_splash->setRouteStatus(true, i18n::t("splash.exWaiting", m_resolvedLanguage));
    m_splash->showFor(m_window);
    // Let Qt paint the splash before starting the worker.
    QTimer::singleShot(0, this, [this]() { prepareRoute(false); });
}

void AppController::prepareRoute(bool extended) {
    const QString market = i18n::t(extended ? "splash.ex" : "splash.main", m_resolvedLanguage);
    m_splash->setRouteStatus(extended, i18n::t("splash.routeProbing", m_resolvedLanguage).arg(market));
    tdx_bridge::request(this, m_routeConfig,
        {{QStringLiteral("operation"), QStringLiteral("prepare_servers")},
         {QStringLiteral("kind"), extended ? QStringLiteral("ex") : QStringLiteral("main")}},
        [this, extended, market](const QJsonObject& result, const QString& error) {
            if (!error.isEmpty()) {
                const QString message = QStringLiteral("%1: %2").arg(market, error);
                m_routeErrors.append(message);
                m_splash->setRouteStatus(extended, message);
            } else {
                const QString address = result.value(QStringLiteral("address")).toString();
                const QString latency = QString::number(result.value(QStringLiteral("latencyMs")).toDouble(), 'f', 1);
                m_splash->setRouteStatus(extended, i18n::t("splash.routeReady", m_resolvedLanguage)
                    .arg(market, address, latency));
                if (extended) m_routeConfig.tdxExServer = address;
                else m_routeConfig.tdxMainServer = address;
                if (result.value(QStringLiteral("switched")).toBool()) {
                    const QString warning = i18n::t("splash.switched", m_resolvedLanguage)
                        .arg(market, result.value(QStringLiteral("previous")).toString(), address);
                    m_routeWarnings.append(warning);
                    m_splash->setWarning(m_routeWarnings.join(QLatin1Char('\n')));
                }
            }
            if (!extended) {
                prepareRoute(true);
                return;
            }
            if (!m_routeErrors.isEmpty()) {
                m_splash->setStatus(i18n::t("splash.failed", m_resolvedLanguage), {}, 0);
                m_splash->setFailed(m_routeErrors.join(QLatin1Char('\n')));
                return;
            }
            m_routesReady = true;
            m_splash->setStatus(i18n::t("splash.loading", m_resolvedLanguage),
                i18n::t("splash.loadingDetail", m_resolvedLanguage), 80);
            QTimer::singleShot(0, this, [this]() { loadAfterRoutes(); });
        });
}

void AppController::copyPreparedRoutes(AppConfig* cfg) const {
    cfg->tdxMainAutoSelect = m_routeConfig.tdxMainAutoSelect;
    cfg->tdxExAutoSelect = m_routeConfig.tdxExAutoSelect;
    cfg->tdxMainServer = m_routeConfig.tdxMainServer;
    cfg->tdxExServer = m_routeConfig.tdxExServer;
    cfg->tdxMainServerPool = m_routeConfig.tdxMainServerPool;
    cfg->tdxExServerPool = m_routeConfig.tdxExServerPool;
    cfg->tdxUserId = m_routeConfig.tdxUserId;
    cfg->tdxToken = m_routeConfig.tdxToken;
    cfg->networkInterface = m_routeConfig.networkInterface;
    cfg->proxyType = m_routeConfig.proxyType;
    cfg->proxyHost = m_routeConfig.proxyHost;
    cfg->proxyPort = m_routeConfig.proxyPort;
    cfg->proxyUser = m_routeConfig.proxyUser;
    cfg->proxyPassword = m_routeConfig.proxyPassword;
    cfg->quoteSource = QStringLiteral("tdx");
}

void AppController::loadAfterRoutes() {
    if (m_loadCompleted) return;
    m_loadCompleted = true;
    m_splash->setStatus(i18n::t("splash.loading", m_resolvedLanguage),
        i18n::t("splash.loadingDetail", m_resolvedLanguage), 85);
    const AppConfig runtime = m_cfg;
    m_cfg = ConfigManager::loadConfig();
    copyPreparedRoutes(&m_cfg);
    m_resolvedLanguage = i18n::resolveLanguage(m_cfg.language);
    app_logging::setLogConfig(m_cfg.logEnabled, m_cfg.logLevel);
    if (!m_initialized) {
        initializeApplication();
        m_initialized = true;
    } else {
        // A reload must preserve the user's current window position and visibility.
        m_cfg.windowRect = runtime.windowRect;
        captureRuntimeWindowGeometries(&m_cfg);
        loadExtraWatchItems();
        loadStocksAfterRoutes();
        m_model->setLanguage(m_resolvedLanguage);
        m_model->setConfig(m_cfg);
        if (m_window) {
            m_window->applyConfig(m_cfg);
            m_window->setLoading(true);
        }
        setupTray();
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        setupHotkey();
        setupGroupHotkeys();
#endif
        rebuildProvider();
        rebuildHotRankProvider();
        if (m_updater) m_updater->setConfig(m_cfg);
    }
    ConfigManager::saveConfig(m_cfg);
    m_splash->showFor(m_window);
    m_splash->setStatus(i18n::t("splash.ready", m_resolvedLanguage),
        i18n::t(m_routesReady ? "splash.readyDetail" : "splash.offlineDetail", m_resolvedLanguage), 100);
    // Leave fallback warnings visible long enough to read before closing.
    QTimer::singleShot(m_routeWarnings.isEmpty() ? 250 : 1800, this, [this]() { completeLoading(); });
}

void AppController::completeLoading() {
    m_splash->hide();
    if (m_window) m_window->setLoading(false);
    m_loading = false;
    if (!m_routeWarnings.isEmpty() && m_tray) {
        m_tray->showMessage(i18n::t("app.name", m_resolvedLanguage), m_routeWarnings.join(QLatin1Char('\n')),
            QSystemTrayIcon::Warning, 7000);
    }
    if (m_reloadQueued) {
        m_reloadQueued = false;
        QTimer::singleShot(0, this, [this]() { beginLoading(false); });
        return;
    }
    if (m_routesReady) {
        m_timer->setInterval(qMax(500, m_cfg.pollMs));
        m_timer->start();
        if (m_cfg.marketBreadthEnabled) {
            m_marketBreadthTimer->setInterval(qMax(10000, m_cfg.marketBreadthRefreshSecs * 1000));
            m_marketBreadthTimer->start();
        }
        refreshQuotes(true);
        refreshHotRanks(true);
        if (m_window && m_window->isVisible()) refreshMarketBreadth(true);
    }
    updateTrayTooltip();
}
