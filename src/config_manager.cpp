#include "config_manager.h"

#include "app_logging.h"
#include "i18n.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#include <memory>

namespace {

QVector<int> normalizedColumnOrder(const QVector<int>& order) {
    QVector<int> out;
    out.reserve(ColCount);

    QSet<int> seen;
    for (int logical : order) {
        if (logical < 0 || logical >= ColCount || seen.contains(logical)) {
            continue;
        }
        out.push_back(logical);
        seen.insert(logical);
    }

    for (int i = 0; i < ColCount; ++i) {
        if (seen.contains(i)) {
            continue;
        }
        out.push_back(i);
        seen.insert(i);
    }

    return out;
}

QString normalizeHoverReadingUiMode(const QString& rawMode) {
    const QString mode = rawMode.trimmed().toLower();
    if (mode == QLatin1String("light") || mode == QLatin1String("dark")) {
        return mode;
    }
    return QStringLiteral("dark");
}

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

std::unique_ptr<QSettings> createAppSettings() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    ConfigManager::migrateSettingsToCacheIfNeeded();
    const QString settingsPath = ConfigManager::appSettingsFilePath();
    if (!settingsPath.isEmpty()) {
        return std::make_unique<QSettings>(settingsPath, QSettings::IniFormat);
    }
#endif

    return std::make_unique<QSettings>("myStocks", "myStocks");
}

} // namespace

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

    return QDir(cacheDir).filePath("settings.ini");
}

void ConfigManager::migrateSettingsToCacheIfNeeded() {
    if (!shouldUseCacheBackedSettings()) {
        return;
    }

    static bool checked = false;
    if (checked) {
        return;
    }
    checked = true;

    const QString targetPath = appSettingsFilePath();
    if (targetPath.isEmpty()) {
        return;
    }

    QSettings cacheSettings(targetPath, QSettings::IniFormat);
    if (!cacheSettings.allKeys().isEmpty()) {
        return;
    }

    QSettings legacySettings("myStocks", "myStocks");
    const QStringList legacyKeys = legacySettings.allKeys();
    if (legacyKeys.isEmpty()) {
        return;
    }

    qInfo() << "ConfigManager migrate settings to cache file"
            << targetPath
            << "keys=" << legacyKeys.size();

    for (const QString& key : legacyKeys) {
        cacheSettings.setValue(key, legacySettings.value(key));
    }

    cacheSettings.sync();
    const QSettings::Status status = cacheSettings.status();
    if (status != QSettings::NoError) {
        qWarning() << "ConfigManager migrate settings failed status="
                   << settingsStatusToString(status);
        return;
    }

    qInfo() << "ConfigManager migrate settings success path=" << targetPath;
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

    cfg.pollMs = s.value("general/pollMs", cfg.pollMs).toInt();
    cfg.opacity = s.value("general/opacity", cfg.opacity).toDouble();
    cfg.hotkey = s.value("general/hotkey", cfg.hotkey).toString();
    cfg.startupShowFloatingWindow = s.value(
        "general/startupShowFloatingWindow",
        cfg.startupShowFloatingWindow
    ).toBool();
    cfg.apiSource = s.value("general/apiSource", cfg.apiSource).toString();
    cfg.xtickToken = s.value("general/xtickToken", cfg.xtickToken).toString();
    cfg.language = i18n::normalizeLanguage(
        s.value("general/language", cfg.language).toString()
    );
    cfg.userAgent = s.value("general/userAgent", cfg.userAgent).toString();
    cfg.proxyType = s.value("general/proxyType", cfg.proxyType).toString();
    cfg.proxyHost = s.value("general/proxyHost", cfg.proxyHost).toString();
    cfg.proxyPort = s.value("general/proxyPort", cfg.proxyPort).toInt();
    cfg.proxyUser = s.value("general/proxyUser", cfg.proxyUser).toString();
    cfg.proxyPassword = s.value("general/proxyPassword", cfg.proxyPassword).toString();
    cfg.debugIgnoreTradingTime = s.value(
        "general/debugIgnoreTradingTime",
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

    cfg.hoverReadingEnabled = s.value("ui/hoverReadingEnabled", cfg.hoverReadingEnabled).toBool();
    cfg.hoverReadingDelaySecs = s.value("ui/hoverReadingDelaySecs", cfg.hoverReadingDelaySecs).toDouble();
    cfg.hoverReadingUiMode = normalizeHoverReadingUiMode(
        s.value("ui/hoverReadingUiMode", cfg.hoverReadingUiMode).toString()
    );
    cfg.hoverReadingTransparentBackgroundEnabled = s.value(
        "ui/hoverReadingTransparentBackgroundEnabled",
        cfg.hoverReadingTransparentBackgroundEnabled
    ).toBool();

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
    cfg.columnOrder = normalizedColumnOrder(loadedOrder);

    cfg.pollMs = qMax(500, cfg.pollMs);
    cfg.opacity = qBound(0.2, cfg.opacity, 1.0);
    if (cfg.apiSource != "mock"
        && cfg.apiSource != "xtick"
        && cfg.apiSource != "sina"
        && cfg.apiSource != "tencent"
        && cfg.apiSource != "eastmoney") {
        cfg.apiSource = "tencent";
    }

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
    cfg.hoverReadingDelaySecs = qBound(0.1, cfg.hoverReadingDelaySecs, 60.0);
    cfg.hoverReadingUiMode = normalizeHoverReadingUiMode(cfg.hoverReadingUiMode);

        qInfo() << "ConfigManager::loadConfig"
            << "apiSource=" << cfg.apiSource
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
            << "apiSource=" << cfg.apiSource
            << "pollMs=" << cfg.pollMs
            << "language=" << cfg.language
            << "logEnabled=" << cfg.logEnabled
            << "logLevel=" << cfg.logLevel;

    s.setValue("general/pollMs", cfg.pollMs);
    s.setValue("general/opacity", cfg.opacity);
    s.setValue("general/hotkey", cfg.hotkey);
    s.setValue("general/startupShowFloatingWindow", cfg.startupShowFloatingWindow);
    s.setValue("general/apiSource", cfg.apiSource);
    s.setValue("general/xtickToken", cfg.xtickToken);
    s.setValue("general/language", i18n::normalizeLanguage(cfg.language));
    s.setValue(
        "general/userAgent",
        cfg.userAgent.trimmed().isEmpty() ? defaultChrome100UserAgent() : cfg.userAgent.trimmed()
    );

    QString proxyType = cfg.proxyType.trimmed().toLower();
    if (proxyType == "socks") {
        proxyType = "socks5";
    }
    if (proxyType != "none" && proxyType != "http" && proxyType != "socks5") {
        proxyType = "none";
    }
    s.setValue("general/proxyType", proxyType);
    s.setValue("general/proxyHost", cfg.proxyHost.trimmed());
    s.setValue("general/proxyPort", qBound(0, cfg.proxyPort, 65535));
    s.setValue("general/proxyUser", cfg.proxyUser);
    s.setValue("general/proxyPassword", cfg.proxyPassword);
    s.setValue("general/debugIgnoreTradingTime", cfg.debugIgnoreTradingTime);

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
    s.setValue("ui/hoverReadingEnabled", cfg.hoverReadingEnabled);
    s.setValue("ui/hoverReadingDelaySecs", qBound(0.1, cfg.hoverReadingDelaySecs, 60.0));
    s.setValue("ui/hoverReadingUiMode", normalizeHoverReadingUiMode(cfg.hoverReadingUiMode));
    s.setValue(
        "ui/hoverReadingTransparentBackgroundEnabled",
        cfg.hoverReadingTransparentBackgroundEnabled
    );
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

    const QVector<int> columnOrder = normalizedColumnOrder(cfg.columnOrder);
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
