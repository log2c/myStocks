#include "config_manager.h"

#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <QTemporaryDir>

namespace {
void check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("myStocksTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tdxConfigTest-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString path = ConfigManager::appSettingsFilePath();
    check(!path.isEmpty() && path.contains(QCoreApplication::applicationName()), "Settings test must use an isolated file");
    {
        QSettings oldSettings(path, QSettings::IniFormat);
        oldSettings.setValue(QStringLiteral("quoteSource"), QStringLiteral("tdx"));
        oldSettings.sync();
    }
    const AppConfig defaults = ConfigManager::loadConfig();
    check(defaults.tdxMainAutoSelect && defaults.tdxExAutoSelect, "Existing settings must migrate to automatic server selection");
    check(defaults.tdxMainServerPool.isEmpty() && defaults.tdxExServerPool.isEmpty(), "Empty pools must retain SDK defaults");

    check(defaults.dailyKlineDays == 60, "Daily candles must default to 60 trading days");
    check(defaults.dailyKlineMAPeriods == QVector<int>({5, 10, 20, 30}), "Default MAs must be 5/10/20/30");
    check(defaults.timelineChartHoverDelayMs == 500, "Existing settings must default hover delay to 500ms");
    check(defaults.globalRightClickClose, "Global right-click close must default to enabled");
    // Both retired keys must still seed the app-wide option, so an existing
    // "off" choice is not silently turned back on.
    {
        QSettings legacy(path, QSettings::IniFormat);
        legacy.setValue(QStringLiteral("ui/floatingWindowDoubleClickToHide"), false);
        legacy.sync();
        check(!ConfigManager::loadConfig().globalRightClickClose,
            "A legacy double-click close choice must migrate to the global option");
        legacy.setValue(QStringLiteral("ui/floatingWindowDoubleClickToHide"), true);
        legacy.sync();
        check(ConfigManager::loadConfig().globalRightClickClose,
            "An enabled legacy double-click close must migrate as enabled");
        legacy.remove(QStringLiteral("ui/floatingWindowDoubleClickToHide"));
        legacy.setValue(QStringLiteral("ui/floatingWindowRightClickToHide"), false);
        legacy.sync();
        check(!ConfigManager::loadConfig().globalRightClickClose,
            "The previous floating-window option must migrate to the global option");
        legacy.remove(QStringLiteral("ui/floatingWindowRightClickToHide"));
        legacy.sync();
    }
    // A settings file from before the mode existed already has a User-Agent and
    // must keep sending it; only a fresh install starts on the random pool.
    check(defaults.userAgentMode == "random",
        "A configuration without a stored User-Agent must default to random");
    {
        QSettings legacy(path, QSettings::IniFormat);
        legacy.setValue(QStringLiteral("cfg/userAgent"), QStringLiteral("legacy-agent/1.0"));
        legacy.sync();
        const AppConfig migrated = ConfigManager::loadConfig();
        check(migrated.userAgentMode == "custom"
                && migrated.userAgent == QStringLiteral("legacy-agent/1.0"),
            "An existing stored User-Agent must migrate to the custom mode");
        legacy.remove(QStringLiteral("cfg/userAgent"));
        legacy.sync();
    }
    AppConfig cfg = defaults;
    cfg.userAgentMode = "random";
    cfg.userAgent = QStringLiteral("legacy-agent/1.0");
    cfg.timelineChartHoverDelayMs = 800;
    cfg.dailyKlineDays = 120;
    cfg.dailyKlineMAPeriods = {250, 10, 10, 99, 60};
    cfg.quoteSource = "eastmoney";
    cfg.networkInterface = "selected-test-interface";
    cfg.proxyType = "http";
    cfg.proxyHost = "127.0.0.1";
    cfg.proxyPort = 17890;
    cfg.pollMs = 4700;
    cfg.tdxMainAutoSelect = false;
    cfg.tdxMainServer = QStringLiteral("127.0.0.1:17709");
    cfg.tdxMainServerPool = {QStringLiteral("127.0.0.1:17709"), QStringLiteral("main.example:7709")};
    cfg.tdxExAutoSelect = true;
    cfg.tdxExServer = QStringLiteral("ex.example:7727");
    cfg.tdxExServerPool = {QStringLiteral("ex.example:7727")};
    ConfigManager::saveConfig(cfg);
    const AppConfig loaded = ConfigManager::loadConfig();
    check(loaded.timelineChartHoverDelayMs == 800, "Hover delay must survive saving and loading");
    check(loaded.userAgentMode == "random" && loaded.userAgent == cfg.userAgent,
        "The User-Agent mode and custom value must round trip independently");
    check(loaded.dailyKlineDays == 120, "Daily candle range did not persist");
    check(loaded.dailyKlineMAPeriods == QVector<int>({10, 60, 250}), "MA periods must persist, normalize and reject unknowns");
    check(!loaded.tdxMainAutoSelect && loaded.tdxExAutoSelect, "Independent selection modes were lost");
    check(loaded.tdxMainServer == cfg.tdxMainServer && loaded.tdxExServer == cfg.tdxExServer, "Selected server addresses were lost");
    check(loaded.tdxMainServerPool == cfg.tdxMainServerPool && loaded.tdxExServerPool == cfg.tdxExServerPool, "Custom address pools did not round trip");
    const AppConfig connection = ConfigManager::loadConnectionConfig();
    check(connection.tdxMainServer == cfg.tdxMainServer && connection.tdxMainServerPool == cfg.tdxMainServerPool,
        "Route bootstrap must use saved selections without loading the full configuration");
    check(connection.networkInterface == cfg.networkInterface && loaded.networkInterface == cfg.networkInterface,
        "Selected network interface must persist and be available before route preparation");
    check(connection.proxyType == cfg.proxyType && connection.proxyPort == cfg.proxyPort,
        "Route bootstrap must use the saved proxy");
    check(connection.pollMs != cfg.pollMs && loaded.pollMs == cfg.pollMs,
        "Full configuration must be deferred until route preparation finishes");
    check(loaded.quoteSource == "tdx", "Disabled EastMoney setting must load as TDX");
    QTemporaryDir temp;
    const QString yaml = temp.filePath("mixed.yaml");
    check(ConfigManager::saveDataYamlText(yaml, "ver: 1\nstocks:\n  - code: 90.BK0420\n    name: Legacy\n  - code: tdx:main:0:000001\n    name: Active\ngroups:\n  - name: Mixed\n    stocks: [90.BK0420, 'tdx:main:0:000001']\n"), "Could not prepare mixed-source data");
    check(ConfigManager::saveDataYaml(yaml, {{"tdx:main:1:600519", "New"}}, {{"Mixed", {"tdx:main:1:600519"}}}),
        "Could not save filtered watchlist");
    const auto stocks = ConfigManager::loadStocksFromYaml(yaml);
    check(stocks.size() == 2 && stocks[0].code == "tdx:main:1:600519" && stocks[1].code == "90.BK0420",
        "Saving TDX edits must preserve disabled legacy rows while allowing active rows to be removed");
    const auto groups = ConfigManager::loadGroupsFromYaml(yaml);
    check(groups.size() == 1 && groups[0].stockCodes.contains("90.BK0420") && !groups[0].stockCodes.contains("tdx:main:0:000001"),
        "Saving groups must preserve disabled members without retaining deleted active members");
    cfg.dailyKlineMAPeriods.clear();
    cfg.timelineChartHoverDelayMs = -100;
    cfg.dailyKlineDays = 0;
    ConfigManager::saveConfig(cfg);
    check(ConfigManager::loadConfig().dailyKlineDays == 1, "Daily range must have a lower bound");
    check(ConfigManager::loadConfig().dailyKlineMAPeriods.isEmpty(), "Disabling every MA must persist as empty, not defaults");
    check(ConfigManager::loadConfig().timelineChartHoverDelayMs == 0, "Hover delay must have a lower bound");
    cfg.timelineChartHoverDelayMs = 20000;
    cfg.dailyKlineDays = 900;
    ConfigManager::saveConfig(cfg);
    check(ConfigManager::loadConfig().dailyKlineDays == 800, "Daily range must respect bridge limit");
    check(ConfigManager::loadConfig().timelineChartHoverDelayMs == 10000, "Hover delay must have an upper bound");
    check(QFile::remove(path), "Could not clean up isolated test settings");
    return 0;
}
