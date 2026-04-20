#pragma once

#include "types.h"

#include <QString>
#include <QVector>

class ConfigManager {
public:
    static QVector<StockItem> loadStocksFromYaml(const QString& filePath);
    static bool saveStocksToYaml(const QString& filePath, const QVector<StockItem>& stocks);
    static AppConfig loadConfig();
    static void saveConfig(const AppConfig& cfg);
};
