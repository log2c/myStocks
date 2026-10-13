#include "config_manager.h"

#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

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

    AppConfig cfg = defaults;
    cfg.tdxMainAutoSelect = false;
    cfg.tdxMainServer = QStringLiteral("127.0.0.1:17709");
    cfg.tdxMainServerPool = {QStringLiteral("127.0.0.1:17709"), QStringLiteral("main.example:7709")};
    cfg.tdxExAutoSelect = true;
    cfg.tdxExServer = QStringLiteral("ex.example:7727");
    cfg.tdxExServerPool = {QStringLiteral("ex.example:7727")};
    ConfigManager::saveConfig(cfg);
    const AppConfig loaded = ConfigManager::loadConfig();
    check(!loaded.tdxMainAutoSelect && loaded.tdxExAutoSelect, "Independent selection modes were lost");
    check(loaded.tdxMainServer == cfg.tdxMainServer && loaded.tdxExServer == cfg.tdxExServer, "Selected server addresses were lost");
    check(loaded.tdxMainServerPool == cfg.tdxMainServerPool && loaded.tdxExServerPool == cfg.tdxExServerPool, "Custom address pools did not round trip");
    check(QFile::remove(path), "Could not clean up isolated test settings");
    return 0;
}
