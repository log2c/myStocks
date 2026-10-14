#include "app_controller.h"
#include "app_constants.h"
#include "config_manager.h"
#include "floating_window.h"
#include "quote_model.h"
#include "startup_splash.h"
#include "settings_dialog.h"
#include <QComboBox>
#include <QListWidget>
#include "tdx_bridge.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <functional>

namespace {
void check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}
struct PendingRoute {
    QPointer<QObject> context;
    AppConfig config;
    QString kind;
    tdx_bridge::Callback callback;
};
QVector<PendingRoute> routes;
int quoteRequests = 0;
void events(int milliseconds = 10) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
void finishRoute(bool fail = false, bool switched = false) {
    check(!routes.isEmpty(), "Expected a pending route preparation");
    auto route = routes.takeFirst();
    if (route.context) {
        route.callback({{"address", route.kind == "main" ? "main.test:7709" : "ex.test:7727"},
            {"latencyMs", 12.5}, {"previous", "old.test:7709"}, {"switched", switched}},
            fail ? "No available route" : QString());
    }
    events();
}
}

// Replace only the C++ bridge for this orchestration test. Actual TCP/protocol
// validation, manual fallback and proxy behavior are covered by Go fixtures.
namespace tdx_bridge {
void request(QObject* context, const AppConfig& config, QJsonObject payload, Callback callback) {
    const QString operation = payload.value("operation").toString();
    if (operation == "prepare_servers") {
        routes.append({context, config, payload.value("kind").toString(), std::move(callback)});
        return;
    }
    if (operation == "quotes") ++quoteRequests;
    QPointer<QObject> guarded(context);
    QTimer::singleShot(0, context, [guarded, callback = std::move(callback)]() {
        if (guarded) callback({{"quotes", QJsonArray()}}, {});
    });
}
QJsonObject networkProxyBlocking(const AppConfig&, QString* error) {
    if (error) error->clear();
    return {{"host", "127.0.0.1"}, {"port", 1080}, {"user", "test"}, {"password", "test"}};
}
QJsonObject requestBlocking(const QJsonObject&, QString*) { return {}; }
}

class AppControllerStartupTest {
public:
    static void run() {
        const QString testRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath("myStocks-test-data");
        const QString yaml = QDir(testRoot).filePath("data.yaml");
        check(ConfigManager::saveDataYamlText(yaml,
            "ver: 1\nstocks:\n  - code: 1.600519\n    name: Disabled\n  - code: tdx:main:0:000001\n    name: Active\n"),
            "Could not create isolated watchlist");
        AppConfig cfg;
        cfg.startupShowFloatingWindow = false;
        cfg.autoCheckUpdates = false;
        cfg.marketBreadthEnabled = false;
        cfg.pollMs = 4800;
        cfg.tdxMainAutoSelect = false;
        cfg.tdxMainServer = "old.test:7709";
        cfg.tdxMainServerPool = {"old.test:7709", "main.test:7709"};
        cfg.tdxExServerPool = {"ex.test:7727"};
        ConfigManager::saveConfig(cfg);
        {
            auto settings = ConfigManager::createAppSettings();
            settings->setValue(app_constants::kWatchIndexesKey, QStringList{"1.000001\tLegacy index"});
        }
        {
            SettingsDialog dialog(cfg, {{"1.600519", "Stock"}, {"1.510300", "ETF"}},
                {{"1.000001", "Legacy index"}}, {}, yaml, {});
            auto* adapters = dialog.findChild<QComboBox*>("networkInterfaceCombo");
            check(adapters && adapters->currentData().toString().isEmpty(), "Network interface selection must default to automatic");
            const auto indexes = dialog.selectedIndexes();
            check(indexes.size() == 1 && indexes.first().code == "1.000001", "Index editor must retain compatible legacy index selection and format");
            cfg.networkInterface = "mystocks-missing-adapter";
        }
        {
            SettingsDialog dialog(cfg, {}, {}, {}, yaml, {});
            auto* adapters = dialog.findChild<QComboBox*>("networkInterfaceCombo");
            check(adapters && adapters->currentData().toString() == cfg.networkInterface,
                "A missing saved interface must stay selected rather than reverting to automatic");
            check(dialog.config().networkInterface == cfg.networkInterface, "Network draft must preserve the selected adapter");
        }
        cfg.networkInterface.clear();
        {
            AppController controller;
            events();
            check(controller.m_loading && controller.m_splash->isVisible(), "Cold startup must show splash");
            check(!controller.m_initialized && !controller.m_model && !controller.m_window,
                "Full initialization must wait for confirmed routes");
            check(controller.m_cfg.pollMs != cfg.pollMs, "Full configuration loaded before route confirmation");
            check(quoteRequests == 0 && routes.size() == 1 && routes.first().kind == "main",
                "Cold startup must probe main market before requesting quotations");
            finishRoute(false, true);
            check(!controller.m_initialized && routes.first().kind == "ex", "Extended route must also finish before config loading");
            finishRoute();
            check(controller.m_initialized && controller.m_cfg.pollMs == cfg.pollMs,
                "Confirmed routes must be followed by full configuration loading");
            check(controller.m_cfg.tdxMainServer == "main.test:7709" && !controller.m_cfg.tdxMainAutoSelect,
                "Manual fallback must persist the replacement without changing selection mode");
            check(!controller.m_window->isVisible() && controller.m_splash->isVisible(),
                "Hidden startup window must remain hidden while splash is active");
            check(controller.m_model->rowCount() == 3, "Compatible legacy stock rows must load during cold startup");
            check(controller.m_indexes.size() == 1 && controller.m_indexes.first().code == "1.000001", "Compatible legacy indexes must be loaded");
            controller.saveExtraWatchItems();
            {
                auto settings = ConfigManager::createAppSettings();
                check(settings->value(app_constants::kWatchIndexesKey).toStringList().contains("1.000001\tLegacy index"),
                    "Saving index edits must retain compatible legacy indexes");
            }
            events(1900);
            check(!controller.m_loading && !controller.m_splash->isVisible() && quoteRequests > 0,
                "Completed startup must close splash and start quotations");

            controller.m_window->move(120, 100);
            controller.m_window->show();
            events();
            const QRect position = controller.m_window->geometry();
            const int beforeReload = quoteRequests;
            controller.reloadStocksFromYaml();
            events();
            check(controller.m_splash->isVisible() && !controller.m_timer->isActive(),
                "Reload must display splash and suspend polling");
            check(controller.m_window->isVisible() && !controller.m_window->isEnabled(),
                "Reload must retain the visible floating window underneath splash");
            check(quoteRequests == beforeReload, "Reload must not request quotations before route confirmation");
            finishRoute();
            finishRoute();
            events(300);
            check(controller.m_window->isVisible() && controller.m_window->geometry() == position,
                "Reload must preserve floating-window visibility and position");
            check(controller.m_window->isEnabled() && !controller.m_splash->isVisible(),
                "Reload completion must restore floating-window interaction");

            controller.m_window->hide();
            controller.reloadStocksFromYaml();
            events();
            finishRoute(true);
            finishRoute(true);
            check(controller.m_loading && controller.m_splash->isVisible() && !controller.m_timer->isActive(),
                "Unusable routes must retain an actionable splash and pause polling");
            check(!controller.m_window->isVisible(), "Failure must not show a hidden floating window");
            controller.loadAfterRoutes();
            events(300);
            check(!controller.m_loading && !controller.m_routesReady && !controller.m_timer->isActive(),
                "Offline load must finish local configuration without starting network polling");
            const int beforeOffline = quoteRequests;
            controller.refreshQuotes(true);
            check(quoteRequests == beforeOffline, "Offline mode must not issue forced quotation requests");
            controller.reloadStocksFromYaml();
            events();
            finishRoute();
            finishRoute();
            events(300);
            check(controller.m_routesReady && controller.m_timer->isActive(), "Reload must recover from offline mode");
        }
        {
            AppController controller;
            events();
            finishRoute(true);
            finishRoute(true);
            check(!controller.m_initialized && !controller.m_model,
                "Cold-start route failure must not prematurely load full configuration");
            for (auto* button : controller.m_splash->findChildren<QPushButton*>()) {
                if (button->text() == "重新检测") button->click();
            }
            events();
            check(routes.size() == 1, "Retry button must begin a fresh probe rather than queue behind failure");
            finishRoute();
            finishRoute();
            events(300);
            check(controller.m_initialized && controller.m_routesReady && !controller.m_loading,
                "Cold startup must recover after a retry");
        }
        {
            AppController controller;
            events();
            controller.reloadStocksFromYaml();
            controller.reloadStocksFromYaml();
            finishRoute();
            finishRoute();
            events(300);
            check(controller.m_initialized && controller.m_loading && routes.size() == 1,
                "Multiple reloads during loading must coalesce into one subsequent reload");
            finishRoute();
            finishRoute();
            events(300);
            check(!controller.m_loading && routes.isEmpty(), "Coalesced reload must complete without duplicate route callbacks");
        }
        // Test-mode paths are unique to this process and never touch user data.
        QDir(testRoot).removeRecursively();
        QFile::remove(ConfigManager::appSettingsFilePath());
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("myStocksTests");
    QCoreApplication::setApplicationName("startupTest-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    AppControllerStartupTest::run();
    return 0;
}
