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
    AppConfig cfg = defaults;
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
    cfg.dailyKlineDays = 0;
    ConfigManager::saveConfig(cfg);
    check(ConfigManager::loadConfig().dailyKlineDays == 1, "Daily range must have a lower bound");
    check(ConfigManager::loadConfig().dailyKlineMAPeriods.isEmpty(), "Disabling every MA must persist as empty, not defaults");
    cfg.dailyKlineDays = 900;
    ConfigManager::saveConfig(cfg);
    check(ConfigManager::loadConfig().dailyKlineDays == 800, "Daily range must respect bridge limit");
    check(QFile::remove(path), "Could not clean up isolated test settings");
    return 0;
}
