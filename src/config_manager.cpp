#include "config_manager.h"

#include "app_constants.h"
#include "app_logging.h"
#include "i18n.h"
#include "watchlist_utils.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <memory>

namespace {

QString settingsStatusToString(QSettings::Status status) {
    switch (status) {
    case QSettings::NoError:
        return QStringLiteral("no-error");
    case QSettings::AccessError:
        return QStringLiteral("access-error");
    case QSettings::FormatError:
        return QStringLiteral("format-error");
    }
    return QStringLiteral("unknown");
}

QString stableConfigKey(const QString& leafKey) {
    return QString::fromLatin1(app_constants::kStableConfigPrefix) + leafKey;
}

QString legacyConfigKey(const QString& leafKey) {
    return QString::fromLatin1(app_constants::kLegacyConfigPrefix) + leafKey;
}

QVariant readConfigValue(
    const QSettings& settings,
    const QString& leafKey,
    const QVariant& defaultValue = QVariant()
) {
    const QVariant stableValue = settings.value(stableConfigKey(leafKey));
    if (stableValue.isValid()) {
        return stableValue;
    }

    return settings.value(legacyConfigKey(leafKey), defaultValue);
}

void writeConfigValue(QSettings& settings, const QString& leafKey, const QVariant& value) {
    settings.setValue(stableConfigKey(leafKey), value);
}

bool shouldUseCacheBackedSettings() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
}

QString fallbackCacheDirPath() {
#if defined(Q_OS_WIN)
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA").trimmed();
    if (!localAppData.isEmpty()) {
        return QDir::cleanPath(QDir(localAppData).filePath("myStocks/cache"));
    }
    return {};
#elif defined(Q_OS_MACOS)
    return QDir::cleanPath(QDir::homePath() + "/Library/Caches/myStocks");
#else
    return QDir::cleanPath(QDir::homePath() + "/.cache/myStocks");
#endif
}

QString resolvedSettingsCacheDirPath() {
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation).trimmed();
    if (cacheDir.isEmpty()) {
        cacheDir = fallbackCacheDirPath();
    }

    if (cacheDir.isEmpty()) {
        return {};
    }

    return QDir::cleanPath(cacheDir);
}

struct DataYamlDocument {
    QVector<StockItem> stocks;
    QVector<StockGroup> groups;
    bool migratedLegacyCodes = false;
    int groupAllPosition = -1; // -1 means absent from YAML (old format)
};

void assignYamlError(QString* errorMessage, const QString& message) {
    if (errorMessage) {
        *errorMessage = message;
    }
}

std::string toUtf8StdString(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    return std::string(utf8.constData(), static_cast<size_t>(utf8.size()));
}

QString yamlScalarText(const YAML::Node& node) {
    if (!node || !node.IsScalar()) {
        return {};
    }

    return QString::fromUtf8(node.Scalar().c_str()).trimmed();
}

bool parsePositiveYamlDouble(const YAML::Node& node, double* value) {
    if (!node || !node.IsScalar() || !value) {
        return false;
    }

    try {
        const double parsed = node.as<double>();
        if (std::isfinite(parsed) && parsed > 0.0) {
            *value = parsed;
            return true;
        }
    } catch (const YAML::Exception&) {
    }

    return false;
}

bool parseDataYamlContent(
    const QByteArray& yamlContent,
    const QString& sourceName,
    DataYamlDocument* document,
    QString* errorMessage = nullptr
) {
    if (!document) {
        assignYamlError(errorMessage, QStringLiteral("internal error"));
        return false;
    }

    document->stocks.clear();
    document->groups.clear();
    document->migratedLegacyCodes = false;

    if (yamlContent.trimmed().isEmpty()) {
        return true;
    }

    try {
        const YAML::Node root = YAML::Load(
            std::string(yamlContent.constData(), static_cast<size_t>(yamlContent.size()))
        );
        if (!root || root.IsNull()) {
            return true;
        }
        if (!root.IsMap()) {
            const QString message = QStringLiteral("YAML root must be a map");
            qWarning() << "ConfigManager::parseDataYamlContent invalid root" << sourceName << message;
            assignYamlError(errorMessage, message);
            return false;
        }

        const YAML::Node stocksNode = root["stocks"];
        if (stocksNode && stocksNode.IsSequence()) {
            for (const auto& itemNode : stocksNode) {
                if (!itemNode.IsMap()) {
                    continue;
                }

                const QString rawCode = yamlScalarText(itemNode["code"]);
                if (rawCode.isEmpty()) {
                    continue;
                }

                const QString normalizedCode = watchlist_utils::normalizeApiWatchCode(rawCode);
                if (normalizedCode.isEmpty()) {
                    qWarning() << "ConfigManager::parseDataYamlContent skip invalid stock code"
                               << rawCode << "in" << sourceName;
                    continue;
                }
                if (normalizedCode.compare(rawCode, Qt::CaseSensitive) != 0) {
                    document->migratedLegacyCodes = true;
                }

                StockItem stock;
                stock.code = normalizedCode;
                stock.name = yamlScalarText(itemNode["name"]);
                if (stock.name.isEmpty()) {
                    stock.name = normalizedCode;
                }
                parsePositiveYamlDouble(itemNode["cost"], &stock.cost);
                document->stocks.push_back(stock);
            }
        }

        const YAML::Node allPosNode = root["group_all_position"];
        if (allPosNode && allPosNode.IsScalar()) {
            try {
                const int v = allPosNode.as<int>();
                if (v >= 0) {
                    document->groupAllPosition = v;
                }
            } catch (const YAML::Exception&) {}
        }

        const YAML::Node groupsNode = root["groups"];
        if (groupsNode && groupsNode.IsSequence()) {
            for (const auto& groupNode : groupsNode) {
                if (!groupNode.IsMap()) {
                    continue;
                }

                const QString groupName = yamlScalarText(groupNode["name"]);
                if (groupName.isEmpty()) {
                    continue;
                }

                StockGroup group;
                group.name = groupName;

                const YAML::Node groupStocksNode = groupNode["stocks"];
                if (groupStocksNode && groupStocksNode.IsSequence()) {
                    for (const auto& stockCodeNode : groupStocksNode) {
                        const QString rawCode = yamlScalarText(stockCodeNode);
                        if (rawCode.isEmpty()) {
                            continue;
                        }

                        const QString normalizedCode = watchlist_utils::normalizeApiWatchCode(rawCode);
                        if (normalizedCode.isEmpty()) {
                            qWarning() << "ConfigManager::parseDataYamlContent skip invalid group stock code"
                                       << rawCode << "in" << sourceName;
                            continue;
                        }
                        if (normalizedCode.compare(rawCode, Qt::CaseSensitive) != 0) {
                            document->migratedLegacyCodes = true;
                        }
                        group.stockCodes.push_back(normalizedCode);
                    }
                }

                document->groups.push_back(group);
            }
        }
    } catch (const YAML::Exception& exception) {
        const QString message = QStringLiteral("YAML parse error: %1")
            .arg(QString::fromUtf8(exception.what()).trimmed());
        qWarning() << "ConfigManager::parseDataYamlContent failed" << sourceName << message;
        assignYamlError(errorMessage, message);
        return false;
    }

    return true;
}

bool loadDataYamlDocument(
    const QString& filePath,
    DataYamlDocument* document,
    QString* errorMessage = nullptr
) {
    if (filePath.trimmed().isEmpty()) {
        const QString message = QStringLiteral("path is empty");
        qWarning() << "ConfigManager::loadDataYamlDocument" << message;
        assignYamlError(errorMessage, message);
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString message = file.errorString();
        qWarning() << "ConfigManager::loadDataYamlDocument failed to open"
                   << filePath << message;
        assignYamlError(errorMessage, message);
        return false;
    }

    return parseDataYamlContent(file.readAll(), filePath, document, errorMessage);
}

bool loadDataYamlDocumentForUpdate(
    const QString& filePath,
    DataYamlDocument* document,
    QString* errorMessage = nullptr
) {
    const QFileInfo info(filePath);
    if (!info.exists() || info.size() <= 0) {
        if (document) {
            document->stocks.clear();
            document->groups.clear();
            document->migratedLegacyCodes = false;
        }
        return true;
    }

    return loadDataYamlDocument(filePath, document, errorMessage);
}

QString emitDataYamlText(
    const QVector<StockItem>& stocks,
    const QVector<StockGroup>& groups,
    int groupAllPosition = -1,
    QString* errorMessage = nullptr
) {
    YAML::Emitter emitter;
    emitter.SetIndent(2);

    emitter << YAML::BeginMap;
    emitter << YAML::Key << "ver" << YAML::Value << 1;
    if (groupAllPosition > 0) {
        emitter << YAML::Key << "group_all_position" << YAML::Value << groupAllPosition;
    }
    emitter << YAML::Key << "stocks" << YAML::Value << YAML::BeginSeq;
    for (const StockItem& stock : stocks) {
        const QString normalizedCode = watchlist_utils::normalizeApiWatchCode(stock.code);
        if (normalizedCode.isEmpty()) {
            continue;
        }

        const QString normalizedName = stock.name.trimmed();

        emitter << YAML::BeginMap;
        emitter << YAML::Key << "code" << YAML::Value << toUtf8StdString(normalizedCode);
        if (!normalizedName.isEmpty()
            && normalizedName.compare(normalizedCode, Qt::CaseSensitive) != 0) {
            emitter << YAML::Key << "name" << YAML::Value << toUtf8StdString(normalizedName);
        }
        if (std::isfinite(stock.cost) && stock.cost > 0.0) {
            emitter << YAML::Key << "cost"
                    << YAML::Value << toUtf8StdString(QString::number(stock.cost, 'f', 6));
        }
        emitter << YAML::EndMap;
    }
    emitter << YAML::EndSeq;

    if (!groups.isEmpty()) {
        emitter << YAML::Key << "groups" << YAML::Value << YAML::BeginSeq;
        for (const StockGroup& group : groups) {
            const QString groupName = group.name.trimmed();
            if (groupName.isEmpty()) {
                continue;
            }

            emitter << YAML::BeginMap;
            emitter << YAML::Key << "name" << YAML::Value << toUtf8StdString(groupName);
            emitter << YAML::Key << "stocks" << YAML::Value << YAML::BeginSeq;
            for (const QString& code : group.stockCodes) {
                const QString normalizedCode = watchlist_utils::normalizeApiWatchCode(code);
                if (!normalizedCode.isEmpty()) {
                    emitter << toUtf8StdString(normalizedCode);
                }
            }
            emitter << YAML::EndSeq;
            emitter << YAML::EndMap;
        }
        emitter << YAML::EndSeq;
    }

    emitter << YAML::EndMap;

    if (!emitter.good()) {
        const QString message = QStringLiteral("YAML emit error: %1")
            .arg(QString::fromUtf8(emitter.GetLastError().c_str()).trimmed());
        qWarning() << "ConfigManager::emitDataYamlText failed" << message;
        assignYamlError(errorMessage, message);
        return {};
    }

    QString text = QString::fromUtf8(emitter.c_str());
    if (!text.endsWith(QLatin1Char('\n'))) {
        text += QLatin1Char('\n');
    }
    return text;
}

bool writeDataYamlText(
    const QString& filePath,
    const QString& yamlText,
    QString* errorMessage = nullptr
) {
    const QFileInfo info(filePath);
    if (!QDir().mkpath(info.dir().absolutePath())) {
        const QString message = QStringLiteral("failed to create parent directory");
        qWarning() << "ConfigManager::writeDataYamlText" << filePath << message;
        assignYamlError(errorMessage, message);
        return false;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        const QString message = file.errorString();
        qWarning() << "ConfigManager::writeDataYamlText open failed" << filePath << message;
        assignYamlError(errorMessage, message);
        return false;
    }

    const QByteArray bytes = yamlText.toUtf8();
    if (file.write(bytes) != bytes.size()) {
        const QString message = file.errorString();
        qWarning() << "ConfigManager::writeDataYamlText write failed" << filePath << message;
        assignYamlError(errorMessage, message);
        file.cancelWriting();
        return false;
    }

    if (!file.commit()) {
        const QString message = file.errorString();
        qWarning() << "ConfigManager::writeDataYamlText commit failed" << filePath << message;
        assignYamlError(errorMessage, message);
        return false;
    }

    return true;
}

} // namespace

std::unique_ptr<QSettings> ConfigManager::createAppSettings() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    const QString settingsPath = ConfigManager::appSettingsFilePath();
    if (!settingsPath.isEmpty()) {
        return std::make_unique<QSettings>(settingsPath, QSettings::IniFormat);
    }
#endif

    return std::make_unique<QSettings>(
        app_constants::kOrganizationName,
        app_constants::kApplicationName
    );
}

QString ConfigManager::appSettingsFilePath() {
    if (!shouldUseCacheBackedSettings()) {
        return {};
    }

    const QString cacheDir = resolvedSettingsCacheDirPath();
    if (cacheDir.isEmpty()) {
        qWarning() << "ConfigManager::appSettingsFilePath cache dir is empty.";
        return {};
    }

    if (!QDir().mkpath(cacheDir)) {
        qWarning() << "ConfigManager::appSettingsFilePath failed to create dir" << cacheDir;
        return {};
    }

    return QDir(cacheDir).filePath(QString::fromLatin1(app_constants::kSettingsFileName));
}

QVector<StockItem> ConfigManager::loadStocksFromYaml(
    const QString& filePath,
    bool* migratedLegacyCodes
) {
    DataYamlDocument document;
    QString errorMessage;
    if (!loadDataYamlDocument(filePath, &document, &errorMessage)) {
        if (migratedLegacyCodes) {
            *migratedLegacyCodes = false;
        }
        return {};
    }

    if (migratedLegacyCodes) {
        *migratedLegacyCodes = document.migratedLegacyCodes;
    }

    qInfo() << "ConfigManager::loadStocksFromYaml loaded"
            << document.stocks.size() << "stocks from" << filePath
            << "migratedLegacyCodes=" << document.migratedLegacyCodes;

    return document.stocks;
}

bool ConfigManager::saveStocksToYaml(const QString& filePath, const QVector<StockItem>& stocks) {
    DataYamlDocument document;
    QString errorMessage;
    if (!loadDataYamlDocumentForUpdate(filePath, &document, &errorMessage)) {
        qWarning() << "ConfigManager::saveStocksToYaml failed to preserve existing groups"
                   << filePath << errorMessage;
        return false;
    }

    return saveDataYaml(filePath, stocks, document.groups, qMax(0, document.groupAllPosition));
}

QVector<StockGroup> ConfigManager::loadGroupsFromYaml(
    const QString& filePath,
    int* outGroupAllPosition
) {
    DataYamlDocument document;
    QString errorMessage;
    if (!loadDataYamlDocument(filePath, &document, &errorMessage)) {
        return {};
    }

    qInfo() << "ConfigManager::loadGroupsFromYaml loaded"
            << document.groups.size() << "groups from" << filePath
            << "groupAllPosition=" << document.groupAllPosition;
    if (outGroupAllPosition) {
        *outGroupAllPosition = document.groupAllPosition;
    }
    return document.groups;
}

bool ConfigManager::saveGroupsToYaml(
    const QString& filePath,
    const QVector<StockGroup>& groups,
    int groupAllPosition
) {
    DataYamlDocument document;
    QString errorMessage;
    if (!loadDataYamlDocumentForUpdate(filePath, &document, &errorMessage)) {
        qWarning() << "ConfigManager::saveGroupsToYaml failed to preserve existing stocks"
                   << filePath << errorMessage;
        return false;
    }

    return saveDataYaml(filePath, document.stocks, groups, groupAllPosition);
}

bool ConfigManager::saveDataYaml(
    const QString& filePath,
    const QVector<StockItem>& stocks,
    const QVector<StockGroup>& groups,
    int groupAllPosition
) {
    if (filePath.isEmpty()) {
        qWarning() << "ConfigManager::saveDataYaml path is empty.";
        return false;
    }

    qInfo() << "ConfigManager::saveDataYaml begin path=" << filePath
            << "stocks=" << stocks.size()
            << "groups=" << groups.size()
            << "groupAllPosition=" << groupAllPosition;

    QString errorMessage;
    const QString yamlText = emitDataYamlText(stocks, groups, groupAllPosition, &errorMessage);
    if (!errorMessage.isEmpty()) {
        return false;
    }

    if (!writeDataYamlText(filePath, yamlText, &errorMessage)) {
        return false;
    }

    qInfo() << "ConfigManager::saveDataYaml success path=" << filePath;
    return true;
}

QString ConfigManager::loadDataYamlText(const QString& filePath, QString* errorMessage) {
    DataYamlDocument document;
    if (!loadDataYamlDocument(filePath, &document, errorMessage)) {
        return {};
    }

    return emitDataYamlText(document.stocks, document.groups, document.groupAllPosition, errorMessage);
}

bool ConfigManager::saveDataYamlText(
    const QString& filePath,
    const QString& yamlText,
    QString* errorMessage
) {
    if (filePath.trimmed().isEmpty()) {
        const QString message = QStringLiteral("path is empty");
        qWarning() << "ConfigManager::saveDataYamlText" << message;
        assignYamlError(errorMessage, message);
        return false;
    }

    DataYamlDocument document;
    if (!parseDataYamlContent(yamlText.toUtf8(), filePath, &document, errorMessage)) {
        return false;
    }

    const QString canonicalText = emitDataYamlText(document.stocks, document.groups, document.groupAllPosition, errorMessage);
    if (canonicalText.isNull()) {
        return false;
    }

    return writeDataYamlText(filePath, canonicalText, errorMessage);
}


AppConfig ConfigManager::loadConfig() {
    AppConfig cfg;
    std::unique_ptr<QSettings> settings = createAppSettings();
    QSettings& s = *settings;

    cfg.pollMs = readConfigValue(s, QStringLiteral("pollMs"), cfg.pollMs).toInt();
    cfg.opacity = readConfigValue(s, QStringLiteral("opacity"), cfg.opacity).toDouble();
    cfg.hotkey = readConfigValue(s, QStringLiteral("hotkey"), cfg.hotkey).toString();
    cfg.marketBreadthHotkey = readConfigValue(
        s,
        QStringLiteral("marketBreadthHotkey"),
        cfg.marketBreadthHotkey
    ).toString();
    cfg.startupShowFloatingWindow = readConfigValue(
        s,
        QStringLiteral("startupShowFloatingWindow"),
        cfg.startupShowFloatingWindow
    ).toBool();
    cfg.language = i18n::normalizeLanguage(
        readConfigValue(s, QStringLiteral("language"), cfg.language).toString()
    );
    cfg.userAgent = readConfigValue(s, QStringLiteral("userAgent"), cfg.userAgent).toString();
    cfg.proxyType = readConfigValue(s, QStringLiteral("proxyType"), cfg.proxyType).toString();
    cfg.proxyHost = readConfigValue(s, QStringLiteral("proxyHost"), cfg.proxyHost).toString();
    cfg.proxyPort = readConfigValue(s, QStringLiteral("proxyPort"), cfg.proxyPort).toInt();
    cfg.proxyUser = readConfigValue(s, QStringLiteral("proxyUser"), cfg.proxyUser).toString();
    cfg.proxyPassword = readConfigValue(s, QStringLiteral("proxyPassword"), cfg.proxyPassword).toString();
    cfg.debugIgnoreTradingTime = readConfigValue(
        s,
        QStringLiteral("debugIgnoreTradingTime"),
        cfg.debugIgnoreTradingTime
    ).toBool();
    cfg.acceptBetaUpdates = readConfigValue(
        s,
        QStringLiteral("acceptBetaUpdates"),
        cfg.acceptBetaUpdates
    ).toBool();
    cfg.autoCheckUpdates = readConfigValue(
        s,
        QStringLiteral("autoCheckUpdates"),
        cfg.autoCheckUpdates
    ).toBool();
    cfg.simpleModeEnabled = s.value("ui/simpleModeEnabled", cfg.simpleModeEnabled).toBool();
    cfg.blinkReminderEnabled = s.value("ui/blinkReminderEnabled", cfg.blinkReminderEnabled).toBool();
    cfg.trayTooltipEnabled = s.value("ui/trayTooltipEnabled", cfg.trayTooltipEnabled).toBool();
    cfg.floatingWindowAlwaysOnTop = s.value(
        "ui/floatingWindowAlwaysOnTop",
        cfg.floatingWindowAlwaysOnTop
    ).toBool();
    cfg.logEnabled = s.value("log/enabled", cfg.logEnabled).toBool();
    cfg.logLevel = app_logging::normalizeLogLevel(
        s.value("log/level", cfg.logLevel).toString()
    );
    cfg.transparentBackgroundEnabled = s.value(
        "ui/transparentBackgroundEnabled",
        cfg.transparentBackgroundEnabled
    ).toBool();
    cfg.transparentBackgroundOpacity = s.value(
        "ui/transparentBackgroundOpacity",
        cfg.transparentBackgroundOpacity
    ).toInt();
    cfg.floatingWindowFontFamily = s.value(
        "ui/floatingWindowFontFamily",
        cfg.floatingWindowFontFamily
    ).toString().trimmed();
    cfg.floatingWindowFontSize = s.value(
        "ui/floatingWindowFontSize",
        cfg.floatingWindowFontSize
    ).toInt();
    const QVariant floatingWindowFontBoldValue = s.value("ui/floatingWindowFontBold");
    cfg.floatingWindowFontBold = floatingWindowFontBoldValue.isValid()
        ? floatingWindowFontBoldValue.toBool()
        : (s.value("ui/floatingWindowFontWeight", 0).toInt() >= QFont::DemiBold);

    cfg.hoverReadingEnabled = s.value("ui/hoverReadingEnabled", cfg.hoverReadingEnabled).toBool();
    cfg.hoverReadingDelaySecs = s.value("ui/hoverReadingDelaySecs", cfg.hoverReadingDelaySecs).toDouble();
    cfg.hoverReadingUiMode = normalizeHoverReadingUiMode(
        s.value("ui/hoverReadingUiMode", cfg.hoverReadingUiMode).toString()
    );
    cfg.hoverReadingTransparentBackgroundEnabled = s.value(
        "ui/hoverReadingTransparentBackgroundEnabled",
        cfg.hoverReadingTransparentBackgroundEnabled
    ).toBool();
    cfg.timelineChartEnabled = s.value(
        "ui/timelineChartEnabled",
        cfg.timelineChartEnabled
    ).toBool();
    cfg.timelineChartRefreshSecs = s.value(
        "ui/timelineChartRefreshSecs",
        cfg.timelineChartRefreshSecs
    ).toInt();
    cfg.marketBreadthEnabled = s.value(
        "ui/marketBreadthEnabled",
        cfg.marketBreadthEnabled
    ).toBool();
    cfg.marketBreadthRefreshSecs = s.value(
        "ui/marketBreadthRefreshSecs",
        cfg.marketBreadthRefreshSecs
    ).toInt();
    cfg.timelineChartFixedRangeEnabled = s.value(
        "ui/timelineChartFixedRangeEnabled",
        cfg.timelineChartFixedRangeEnabled
    ).toBool();
    cfg.timelineChartBgColor = s.value(
        "ui/timelineChartBgColor",
        cfg.timelineChartBgColor
    ).value<QColor>();
    cfg.timelineChartGridColor = s.value(
        "ui/timelineChartGridColor",
        cfg.timelineChartGridColor
    ).value<QColor>();
    cfg.timelineChartPriceLineColor = s.value(
        "ui/timelineChartPriceLineColor",
        cfg.timelineChartPriceLineColor
    ).value<QColor>();
    cfg.timelineChartAvgLineColor = s.value(
        "ui/timelineChartAvgLineColor",
        cfg.timelineChartAvgLineColor
    ).value<QColor>();
    cfg.timelineChartTextColor = s.value(
        "ui/timelineChartTextColor",
        cfg.timelineChartTextColor
    ).value<QColor>();
    cfg.timelineChartUpColor = s.value(
        "ui/timelineChartUpColor",
        cfg.timelineChartUpColor
    ).value<QColor>();
    cfg.timelineChartDownColor = s.value(
        "ui/timelineChartDownColor",
        cfg.timelineChartDownColor
    ).value<QColor>();
    cfg.hotRankEnabled = s.value("ui/hotRankEnabled", cfg.hotRankEnabled).toBool();
    cfg.hotRankPollSecs = s.value("ui/hotRankPollSecs", cfg.hotRankPollSecs).toInt();
    cfg.hotRankFlipSecs = s.value("ui/hotRankFlipSecs", cfg.hotRankFlipSecs).toDouble();
    cfg.hotSectorVisible = s.value("ui/hotSectorVisible", cfg.hotSectorVisible).toBool();
    cfg.hotSectorCount = s.value("ui/hotSectorCount", cfg.hotSectorCount).toInt();
    cfg.hotSectorSortField = normalizeHotRankSortField(
        s.value("ui/hotSectorSortField", cfg.hotSectorSortField).toString()
    );
    cfg.hotSectorSortOrder = normalizeHotRankSortOrder(
        s.value("ui/hotSectorSortOrder", cfg.hotSectorSortOrder).toString()
    );
    cfg.hotConceptVisible = s.value("ui/hotConceptVisible", cfg.hotConceptVisible).toBool();
    cfg.hotConceptCount = s.value("ui/hotConceptCount", cfg.hotConceptCount).toInt();
    cfg.hotConceptSortField = normalizeHotRankSortField(
        s.value("ui/hotConceptSortField", cfg.hotConceptSortField).toString()
    );
    cfg.hotConceptSortOrder = normalizeHotRankSortOrder(
        s.value("ui/hotConceptSortOrder", cfg.hotConceptSortOrder).toString()
    );
    cfg.mousePassthroughEnabled = s.value(
        "ui/mousePassthroughEnabled",
        cfg.mousePassthroughEnabled
    ).toBool();
    cfg.mousePassthroughActivationKey = normalizeMousePassthroughActivationKey(
        s.value(
            "ui/mousePassthroughActivationKey",
            cfg.mousePassthroughActivationKey
        ).toString()
    );
    cfg.floatingWindowDoubleClickToHide = s.value(
        "ui/floatingWindowDoubleClickToHide",
        cfg.floatingWindowDoubleClickToHide
    ).toBool();
    cfg.floatingWindowDoubleClickStockDetail = s.value(
        "ui/floatingWindowDoubleClickStockDetail",
        cfg.floatingWindowDoubleClickStockDetail
    ).toBool();
    cfg.floatingWindowPaddingPx = s.value(
        "ui/floatingWindowPaddingPx",
        cfg.floatingWindowPaddingPx
    ).toDouble();
    cfg.trayIconPath = s.value("ui/trayIconPath", cfg.trayIconPath).toString();

    cfg.showHeader = s.value("ui/showHeader", cfg.showHeader).toBool();
    cfg.showGrid = s.value("ui/showGrid", cfg.showGrid).toBool();
    cfg.gridColor = s.value("ui/gridColor", cfg.gridColor).value<QColor>();
    cfg.bgColor = s.value("ui/bgColor", cfg.bgColor).value<QColor>();
    cfg.textColor = s.value("ui/textColor", cfg.textColor).value<QColor>();
    cfg.upColor = s.value("ui/upColor", cfg.upColor).value<QColor>();
    cfg.downColor = s.value("ui/downColor", cfg.downColor).value<QColor>();
    cfg.flatColor = s.value("ui/flatColor", cfg.flatColor).value<QColor>();
    cfg.costLineColor = s.value("ui/costLineColor", cfg.costLineColor).value<QColor>();
    cfg.windowRect = s.value("ui/windowRect", cfg.windowRect).toRect();
    cfg.marketBreadthWindowRect = s.value(
        "ui/marketBreadthWindowRect",
        cfg.marketBreadthWindowRect
    ).toRect();
    cfg.groupSwitchHotkeyPrefix = s.value(
        "ui/groupSwitchHotkeyPrefix",
        cfg.groupSwitchHotkeyPrefix
    ).toString();
    cfg.groupAllPosition = s.value("ui/groupAllPosition", 0).toInt();
    cfg.allGroupShowUngroupedOnly = s.value("ui/allGroupShowUngroupedOnly", true).toBool();

    cfg.gistToken = s.value("sync/gistToken", cfg.gistToken).toString();
    cfg.gistId = s.value("sync/gistId", cfg.gistId).toString();
    cfg.gistLastSyncTime = s.value("sync/gistLastSyncTime", cfg.gistLastSyncTime).toString();
    cfg.gistRemoteUpdatedAt = s.value("sync/gistRemoteUpdatedAt", cfg.gistRemoteUpdatedAt).toString();

    for (int i = 0; i < ColCount; ++i) {
        cfg.visibleColumns[i] = s.value(
            QString("ui/columns/%1").arg(i),
            cfg.visibleColumns.value(i, true)
        ).toBool();

        const QVariant w = s.value(QString("ui/columnWidth/%1").arg(i));
        if (w.isValid()) {
            cfg.columnWidths[i] = w.toInt();
        }

        int maxW = s.value(
            QString("ui/columnMaxWidth/%1").arg(i),
            cfg.columnMaxWidths.value(i, 0)
        ).toInt();
        if (maxW < 0) {
            maxW = 0;
        }
        cfg.columnMaxWidths[i] = maxW;
    }

    QVector<int> loadedOrder = cfg.columnOrder;
    const QVariantList rawOrder = s.value("ui/columnOrder").toList();
    if (!rawOrder.isEmpty()) {
        loadedOrder.clear();
        loadedOrder.reserve(rawOrder.size());
        for (const QVariant& value : rawOrder) {
            loadedOrder.push_back(value.toInt());
        }
    }
    cfg.columnOrder = watchlist_utils::normalizedColumnOrder(loadedOrder);

    cfg.pollMs = qMax(500, cfg.pollMs);
    cfg.opacity = qBound(0.2, cfg.opacity, 1.0);
    cfg.userAgent = cfg.userAgent.trimmed();
    if (cfg.userAgent.isEmpty()) {
        cfg.userAgent = defaultChrome100UserAgent();
    }

    cfg.proxyType = cfg.proxyType.trimmed().toLower();
    if (cfg.proxyType == "socks") {
        cfg.proxyType = "socks5";
    }
    if (cfg.proxyType != "none" && cfg.proxyType != "http" && cfg.proxyType != "socks5") {
        cfg.proxyType = "none";
    }
    if (cfg.proxyPort < 0 || cfg.proxyPort > 65535) {
        cfg.proxyPort = 0;
    }

    cfg.transparentBackgroundOpacity = qBound(0, cfg.transparentBackgroundOpacity, 100);
    cfg.floatingWindowFontSize = qBound(0, cfg.floatingWindowFontSize, 72);
    cfg.hoverReadingDelaySecs = qBound(0.1, cfg.hoverReadingDelaySecs, 60.0);
    cfg.timelineChartRefreshSecs = qBound(10, cfg.timelineChartRefreshSecs, 3600);
    cfg.marketBreadthRefreshSecs = qBound(10, cfg.marketBreadthRefreshSecs, 3600);
    cfg.hoverReadingUiMode = normalizeHoverReadingUiMode(cfg.hoverReadingUiMode);
    cfg.hotRankPollSecs = qBound(10, cfg.hotRankPollSecs, 3600);
    cfg.hotRankFlipSecs = qBound(0.5, cfg.hotRankFlipSecs, 60.0);
    cfg.hotSectorCount = qMax(1, cfg.hotSectorCount);
    cfg.hotSectorSortField = normalizeHotRankSortField(cfg.hotSectorSortField);
    cfg.hotSectorSortOrder = normalizeHotRankSortOrder(cfg.hotSectorSortOrder);
    cfg.hotConceptCount = qMax(1, cfg.hotConceptCount);
    cfg.hotConceptSortField = normalizeHotRankSortField(cfg.hotConceptSortField);
    cfg.hotConceptSortOrder = normalizeHotRankSortOrder(cfg.hotConceptSortOrder);
    cfg.mousePassthroughActivationKey = normalizeMousePassthroughActivationKey(
        cfg.mousePassthroughActivationKey
    );
    if (cfg.mousePassthroughEnabled) {
        cfg.floatingWindowDoubleClickToHide = false;
    }
    cfg.floatingWindowPaddingPx = qMax(0.0, cfg.floatingWindowPaddingPx);

        qInfo() << "ConfigManager::loadConfig"
            << "settingsFile=" << s.fileName()
            << "pollMs=" << cfg.pollMs
            << "language=" << cfg.language
            << "logEnabled=" << cfg.logEnabled
            << "logLevel=" << cfg.logLevel
            << "proxyType=" << cfg.proxyType
            << "proxyHostSet=" << !cfg.proxyHost.trimmed().isEmpty()
            << "proxyPort=" << cfg.proxyPort;

    return cfg;
}

void ConfigManager::saveConfig(const AppConfig& cfg) {
    std::unique_ptr<QSettings> settings = createAppSettings();
    QSettings& s = *settings;

        qInfo() << "ConfigManager::saveConfig begin"
            << "settingsFile=" << s.fileName()
            << "pollMs=" << cfg.pollMs
            << "language=" << cfg.language
            << "logEnabled=" << cfg.logEnabled
            << "logLevel=" << cfg.logLevel;

    writeConfigValue(s, QStringLiteral("pollMs"), cfg.pollMs);
    writeConfigValue(s, QStringLiteral("opacity"), cfg.opacity);
    writeConfigValue(s, QStringLiteral("hotkey"), cfg.hotkey);
    writeConfigValue(s, QStringLiteral("marketBreadthHotkey"), cfg.marketBreadthHotkey);
    writeConfigValue(
        s,
        QStringLiteral("startupShowFloatingWindow"),
        cfg.startupShowFloatingWindow
    );
    writeConfigValue(s, QStringLiteral("language"), i18n::normalizeLanguage(cfg.language));
    writeConfigValue(
        s,
        QStringLiteral("userAgent"),
        cfg.userAgent.trimmed().isEmpty() ? defaultChrome100UserAgent() : cfg.userAgent.trimmed()
    );

    QString proxyType = cfg.proxyType.trimmed().toLower();
    if (proxyType == "socks") {
        proxyType = "socks5";
    }
    if (proxyType != "none" && proxyType != "http" && proxyType != "socks5") {
        proxyType = "none";
    }
    writeConfigValue(s, QStringLiteral("proxyType"), proxyType);
    writeConfigValue(s, QStringLiteral("proxyHost"), cfg.proxyHost.trimmed());
    writeConfigValue(s, QStringLiteral("proxyPort"), qBound(0, cfg.proxyPort, 65535));
    writeConfigValue(s, QStringLiteral("proxyUser"), cfg.proxyUser);
    writeConfigValue(s, QStringLiteral("proxyPassword"), cfg.proxyPassword);
    writeConfigValue(
        s,
        QStringLiteral("debugIgnoreTradingTime"),
        cfg.debugIgnoreTradingTime
    );
    writeConfigValue(
        s,
        QStringLiteral("acceptBetaUpdates"),
        cfg.acceptBetaUpdates
    );
    writeConfigValue(
        s,
        QStringLiteral("autoCheckUpdates"),
        cfg.autoCheckUpdates
    );

    // Remove legacy keys that map to [%General] in INI and can shadow values across runs.
    s.remove(QStringLiteral("general"));
    s.remove(QStringLiteral("General"));

    s.setValue("log/enabled", cfg.logEnabled);
    s.setValue("log/level", app_logging::normalizeLogLevel(cfg.logLevel));

    s.setValue("ui/showHeader", cfg.showHeader);
    s.setValue("ui/showGrid", cfg.showGrid);
    s.setValue("ui/gridColor", cfg.gridColor);
    s.setValue("ui/simpleModeEnabled", cfg.simpleModeEnabled);
    s.setValue("ui/blinkReminderEnabled", cfg.blinkReminderEnabled);
    s.setValue("ui/trayTooltipEnabled", cfg.trayTooltipEnabled);
    s.setValue("ui/floatingWindowAlwaysOnTop", cfg.floatingWindowAlwaysOnTop);
    s.setValue("ui/bgColor", cfg.bgColor);
    s.setValue("ui/textColor", cfg.textColor);
    s.setValue("ui/upColor", cfg.upColor);
    s.setValue("ui/downColor", cfg.downColor);
    s.setValue("ui/flatColor", cfg.flatColor);
    s.setValue("ui/costLineColor", cfg.costLineColor);
    s.setValue("ui/transparentBackgroundEnabled", cfg.transparentBackgroundEnabled);
    s.setValue(
        "ui/transparentBackgroundOpacity",
        qBound(0, cfg.transparentBackgroundOpacity, 100)
    );
    s.setValue("ui/floatingWindowFontFamily", cfg.floatingWindowFontFamily.trimmed());
    s.setValue("ui/floatingWindowFontSize", qBound(0, cfg.floatingWindowFontSize, 72));
    s.setValue("ui/floatingWindowFontBold", cfg.floatingWindowFontBold);
    s.remove("ui/floatingWindowFontWeight");
    s.setValue("ui/hoverReadingEnabled", cfg.hoverReadingEnabled);
    s.setValue("ui/hoverReadingDelaySecs", qBound(0.1, cfg.hoverReadingDelaySecs, 60.0));
    s.setValue("ui/hoverReadingUiMode", normalizeHoverReadingUiMode(cfg.hoverReadingUiMode));
    s.setValue(
        "ui/hoverReadingTransparentBackgroundEnabled",
        cfg.hoverReadingTransparentBackgroundEnabled
    );
    s.setValue("ui/timelineChartEnabled", cfg.timelineChartEnabled);
    s.setValue("ui/timelineChartRefreshSecs", qBound(10, cfg.timelineChartRefreshSecs, 3600));
    s.setValue("ui/marketBreadthEnabled", cfg.marketBreadthEnabled);
    s.setValue("ui/marketBreadthRefreshSecs", qBound(10, cfg.marketBreadthRefreshSecs, 3600));
    s.setValue("ui/timelineChartFixedRangeEnabled", cfg.timelineChartFixedRangeEnabled);
    s.setValue("ui/timelineChartBgColor", cfg.timelineChartBgColor);
    s.setValue("ui/timelineChartGridColor", cfg.timelineChartGridColor);
    s.setValue("ui/timelineChartPriceLineColor", cfg.timelineChartPriceLineColor);
    s.setValue("ui/timelineChartAvgLineColor", cfg.timelineChartAvgLineColor);
    s.setValue("ui/timelineChartTextColor", cfg.timelineChartTextColor);
    s.setValue("ui/timelineChartUpColor", cfg.timelineChartUpColor);
    s.setValue("ui/timelineChartDownColor", cfg.timelineChartDownColor);
    s.setValue("ui/hotRankEnabled", cfg.hotRankEnabled);
    s.setValue("ui/hotRankPollSecs", qBound(10, cfg.hotRankPollSecs, 3600));
    s.setValue("ui/hotRankFlipSecs", qBound(0.5, cfg.hotRankFlipSecs, 60.0));
    s.setValue("ui/hotSectorVisible", cfg.hotSectorVisible);
    s.setValue("ui/hotSectorCount", qMax(1, cfg.hotSectorCount));
    s.setValue(
        "ui/hotSectorSortField",
        normalizeHotRankSortField(cfg.hotSectorSortField)
    );
    s.setValue(
        "ui/hotSectorSortOrder",
        normalizeHotRankSortOrder(cfg.hotSectorSortOrder)
    );
    s.setValue("ui/hotConceptVisible", cfg.hotConceptVisible);
    s.setValue("ui/hotConceptCount", qMax(1, cfg.hotConceptCount));
    s.setValue(
        "ui/hotConceptSortField",
        normalizeHotRankSortField(cfg.hotConceptSortField)
    );
    s.setValue(
        "ui/hotConceptSortOrder",
        normalizeHotRankSortOrder(cfg.hotConceptSortOrder)
    );
    s.setValue("ui/mousePassthroughEnabled", cfg.mousePassthroughEnabled);
    s.setValue(
        "ui/mousePassthroughActivationKey",
        normalizeMousePassthroughActivationKey(cfg.mousePassthroughActivationKey)
    );
    s.setValue(
        "ui/floatingWindowDoubleClickToHide",
        cfg.floatingWindowDoubleClickToHide && !cfg.mousePassthroughEnabled
    );
    s.setValue("ui/floatingWindowDoubleClickStockDetail", cfg.floatingWindowDoubleClickStockDetail);
    s.setValue("ui/floatingWindowPaddingPx", qMax(0.0, cfg.floatingWindowPaddingPx));
    s.setValue("ui/trayIconPath", cfg.trayIconPath);
    s.setValue("ui/windowRect", cfg.windowRect);
    s.setValue("ui/marketBreadthWindowRect", cfg.marketBreadthWindowRect);
    s.setValue("ui/groupSwitchHotkeyPrefix", cfg.groupSwitchHotkeyPrefix);
    s.setValue("ui/groupAllPosition", cfg.groupAllPosition);
    s.setValue("ui/allGroupShowUngroupedOnly", cfg.allGroupShowUngroupedOnly);

    s.setValue("sync/gistToken", cfg.gistToken);
    s.setValue("sync/gistId", cfg.gistId);
    s.setValue("sync/gistLastSyncTime", cfg.gistLastSyncTime);
    s.setValue("sync/gistRemoteUpdatedAt", cfg.gistRemoteUpdatedAt);

    for (int i = 0; i < ColCount; ++i) {
        s.setValue(QString("ui/columns/%1").arg(i), cfg.visibleColumns.value(i, true));
        if (cfg.columnWidths.contains(i)) {
            s.setValue(QString("ui/columnWidth/%1").arg(i), cfg.columnWidths.value(i));
        }
        s.setValue(
            QString("ui/columnMaxWidth/%1").arg(i),
            qMax(0, cfg.columnMaxWidths.value(i, 0))
        );
    }

    const QVector<int> columnOrder = watchlist_utils::normalizedColumnOrder(cfg.columnOrder);
    QVariantList orderValues;
    orderValues.reserve(columnOrder.size());
    for (int logical : columnOrder) {
        orderValues.push_back(logical);
    }
    s.setValue("ui/columnOrder", orderValues);

    s.sync();
    const QSettings::Status status = s.status();
    if (status != QSettings::NoError) {
        qWarning() << "ConfigManager::saveConfig sync status=" << settingsStatusToString(status);
        return;
    }

    qInfo() << "ConfigManager::saveConfig success";
}
