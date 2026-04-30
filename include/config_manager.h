#pragma once

#include "types.h"

#include <memory>
#include <QString>
#include <QVector>

class QSettings;

class ConfigManager {
public:
    static std::unique_ptr<QSettings> createAppSettings();
    static QVector<StockItem> loadStocksFromYaml(
        const QString& filePath,
        bool* migratedLegacyCodes = nullptr
    );
    static bool saveStocksToYaml(const QString& filePath, const QVector<StockItem>& stocks);
    static QString appSettingsFilePath();
    static AppConfig loadConfig();
    static void saveConfig(const AppConfig& cfg);
};
