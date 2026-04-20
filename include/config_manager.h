#pragma once

#include "types.h"

#include <QString>
#include <QVector>

class ConfigManager {
public:
    static QVector<StockItem> loadStocksFromYaml(const QString& filePath);
    static AppConfig loadConfig();
    static void saveConfig(const AppConfig& cfg);
};
