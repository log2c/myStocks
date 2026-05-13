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
    static QVector<StockGroup> loadGroupsFromYaml(const QString& filePath);
    static bool saveStocksToYaml(const QString& filePath, const QVector<StockItem>& stocks);
    static bool saveGroupsToYaml(const QString& filePath, const QVector<StockGroup>& groups);
    static bool saveDataYaml(
        const QString& filePath,
        const QVector<StockItem>& stocks,
        const QVector<StockGroup>& groups
    );
    static QString appSettingsFilePath();
    static AppConfig loadConfig();
    static void saveConfig(const AppConfig& cfg);
};
