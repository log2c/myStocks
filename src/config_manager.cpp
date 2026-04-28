#include "config_manager.h"

#include "app_constants.h"
#include "app_logging.h"
#include "i18n.h"
#include "watchlist_utils.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

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

QVector<StockItem> ConfigManager::loadStocksFromYaml(const QString& filePath) {
    QVector<StockItem> out;
    if (filePath.trimmed().isEmpty()) {
        qWarning() << "ConfigManager::loadStocksFromYaml path is empty.";
        return out;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "ConfigManager::loadStocksFromYaml failed to open"
                   << filePath << file.errorString();
        return out;
    }

// #ifdef DEBUG_MODE
//     const QByteArray content = file.readAll();
//     file.seek(0); // Reset to beginning for parsing
//     qDebug() << "Debug mode: data.yaml content:\n" << QString::fromUtf8(content);
// #endif

    QString curCode;
    QString curName;

    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }

        const QRegularExpressionMatch mCode =
            QRegularExpression("^-\\s*code\\s*:\\s*(\\S+)").match(line);
        if (mCode.hasMatch()) {
            if (!curCode.isEmpty()) {
                out.push_back({curCode, curName.isEmpty() ? curCode : curName});
            }
            curCode = mCode.captured(1).trimmed();
            curName.clear();
            continue;
        }

        const QRegularExpressionMatch mName =
            QRegularExpression("^name\\s*:\\s*(.+)$").match(line);
        if (mName.hasMatch()) {
            curName = mName.captured(1).trimmed();
        }
    }

    if (!curCode.isEmpty()) {
        out.push_back({curCode, curName.isEmpty() ? curCode : curName});
    }

    qInfo() << "ConfigManager::loadStocksFromYaml loaded"
            << out.size() << "stocks from" << filePath;

    return out;
}

bool ConfigManager::saveStocksToYaml(const QString& filePath, const QVector<StockItem>& stocks) {
    if (filePath.isEmpty()) {
        qWarning() << "ConfigManager::saveStocksToYaml path is empty.";
        return false;
    }

    qInfo() << "ConfigManager::saveStocksToYaml begin path=" << filePath
            << "count=" << stocks.size();

    QString content;
    content += QStringLiteral("ver: 1\n\n");
    content += QStringLiteral("# stocks\n");
    content += QStringLiteral("stocks:\n");
    for (const StockItem& s : stocks) {
        content += QStringLiteral("  - code: ") + s.code + QStringLiteral("\n");
        content += QStringLiteral("    name: ") + s.name + QStringLiteral("\n");
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "ConfigManager::saveStocksToYaml open failed"
                   << filePath << file.errorString();
        return false;
    }
    const QByteArray bytes = content.toUtf8();
    if (file.write(bytes) != bytes.size()) {
        qWarning() << "ConfigManager::saveStocksToYaml write failed"
                   << filePath << file.errorString();
        file.cancelWriting();
        return false;
    }
    const bool committed = file.commit();
    if (!committed) {
        qWarning() << "ConfigManager::saveStocksToYaml commit failed"
                   << filePath << file.errorString();
        return false;
    }

    qInfo() << "ConfigManager::saveStocksToYaml success path=" << filePath
            << "count=" << stocks.size();
    return true;
}

AppConfig ConfigManager::loadConfig() {
    AppConfig cfg;
    std::unique_ptr<QSettings> settings = createAppSettings();
    QSettings& s = *settings;

    cfg.pollMs = readConfigValue(s, QStringLiteral("pollMs"), cfg.pollMs).toInt();
    cfg.opacity = readConfigValue(s, QStringLiteral("opacity"), cfg.opacity).toDouble();
    cfg.hotkey = readConfigValue(s, QStringLiteral("hotkey"), cfg.hotkey).toString();
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
    cfg.floatingWindowPaddingPx = s.value(
        "ui/floatingWindowPaddingPx",
        cfg.floatingWindowPaddingPx
    ).toDouble();

    cfg.showHeader = s.value("ui/showHeader", cfg.showHeader).toBool();
    cfg.showGrid = s.value("ui/showGrid", cfg.showGrid).toBool();
    cfg.gridColor = s.value("ui/gridColor", cfg.gridColor).value<QColor>();
    cfg.bgColor = s.value("ui/bgColor", cfg.bgColor).value<QColor>();
    cfg.textColor = s.value("ui/textColor", cfg.textColor).value<QColor>();
    cfg.upColor = s.value("ui/upColor", cfg.upColor).value<QColor>();
    cfg.downColor = s.value("ui/downColor", cfg.downColor).value<QColor>();
    cfg.flatColor = s.value("ui/flatColor", cfg.flatColor).value<QColor>();
    cfg.windowRect = s.value("ui/windowRect", cfg.windowRect).toRect();

    for (int i = 0; i < ColCount; ++i) {
        cfg.visibleColumns[i] = s.value(QString("ui/columns/%1").arg(i), true).toBool();

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
    s.setValue("ui/floatingWindowPaddingPx", qMax(0.0, cfg.floatingWindowPaddingPx));
    s.setValue("ui/windowRect", cfg.windowRect);

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
