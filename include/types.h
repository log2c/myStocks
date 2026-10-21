#pragma once

#include <QColor>
#include <QMap>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

enum QuoteColumn {
    ColCode = 0,
    ColName,
    ColPrice,
    ColPct,
    ColChange,
    ColIndicator,
    ColCost,
    ColCostPct,
    ColBreakevenGain,
    ColRiseSpeed, // append to preserve saved column IDs
    ColCount
};

inline constexpr int kFloatingWindowPaddingPx = 6;
inline constexpr int kMarketBreadthPopupDefaultWidthPx = 860;
inline constexpr int kMarketBreadthPopupDefaultHeightPx = 715;

struct StockItem {
    QString code;
    QString name;
    double cost = qQNaN(); // optional cost basis per share
};

struct StockGroup {
    QString name;
    QStringList stockCodes; // normalized codes in this group
};

struct QuoteItem {
    QString code;
    QString name;
    double price = qQNaN();
    double pct = qQNaN();
    double change = qQNaN();
    double cost = qQNaN(); // cost basis
    double riseSpeed = qQNaN();
    double preClose = qQNaN();
    // Pre-open reset rows display yesterday's close with a flat-price visual state.
    // Intraday change, indicators and rise speed remain blank.
    bool resetData = false;
};

struct HotRankItem {
    QString code;
    QString name;
    QString detailFs;
    QString watchCode;
    double pct = qQNaN();
    double change = qQNaN();
    double mainNetInflow = qQNaN();
    double yearPct = qQNaN();
    double heat = qQNaN();
    QStringList tags;
};

struct HotRankDetailItem {
    QString code;
    QString watchCode;
    QString name;
    double price = qQNaN();
    double pct = qQNaN();
    double marketCap = qQNaN();
    double turnover = qQNaN();
    double yearPct = qQNaN();
};

struct IndexQuoteItem {
    QString code;
    QString displayName;
    double price = qQNaN();
    double change = qQNaN();
    double pct = qQNaN();
};

struct MarketBreadthDistributionItem {
    QString bucket;
    int value = 0;
};

struct MarketBreadthTimelinePoint {
    qint64 timestampMs = 0;
    int riseCount = -1;
    int fallCount = -1;
    int limitUpCount = -1;
    int limitDownCount = -1;
};

struct MarketBreadthSnapshot {
    int upCount = 0;
    int flatCount = 0;
    int downCount = 0;
    int limitUpCount = 0;
    int limitDownCount = 0;
    qint64 lastUpdatedAtMs = 0;
    bool overviewValid = false;
    bool breadthValid = false;
    QVector<MarketBreadthDistributionItem> distribution;
    bool distributionValid = false;
    QVector<MarketBreadthTimelinePoint> overviewTimeline;
    double turnover = qQNaN();
    double turnoverPre = qQNaN();
    double turnoverChange = qQNaN();
    double turnoverForecast = qQNaN();
    bool turnoverValid = false;
};

inline QString defaultChrome100UserAgent() {
    return QStringLiteral(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/100.0.4896.75 Safari/537.36"
    );
}

// Built-in presets for the "random" User-Agent mode: current mainstream Chrome
// and Safari desktop agents. One is picked per app start and kept for the
// session so a single run does not look like it keeps changing browsers.
inline QStringList builtinUserAgentPresets() {
    return {
        QStringLiteral(
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/141.0.0.0 Safari/537.36"),
        QStringLiteral(
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/141.0.0.0 Safari/537.36"),
        QStringLiteral(
            "Mozilla/5.0 (X11; Linux x86_64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/141.0.0.0 Safari/537.36"),
        QStringLiteral(
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            "AppleWebKit/605.1.15 (KHTML, like Gecko) "
            "Version/18.6 Safari/605.1.15"),
        QStringLiteral(
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            "AppleWebKit/605.1.15 (KHTML, like Gecko) "
            "Version/17.6 Safari/605.1.15"),
    };
}

inline QString normalizeUserAgentMode(const QString& rawMode) {
    const QString mode = rawMode.trimmed().toLower();
    if (mode == QLatin1String("custom")) {
        return mode;
    }
    return QStringLiteral("random");
}

inline QString defaultFloatingWindowFontFamily() {
#if defined(Q_OS_MACOS)
    return QStringLiteral("PingFang SC");
#elif defined(Q_OS_WIN)
    return QStringLiteral("Microsoft YaHei");
#else
    return QString();
#endif
}

inline QString normalizeMousePassthroughActivationKey(const QString& rawKey) {
    const QString key = rawKey.trimmed().toLower();
    if (key == QLatin1String("ctrl")
        || key == QLatin1String("shift")
        || key == QLatin1String("alt")) {
        return key;
    }
#if defined(Q_OS_MACOS)
    if (key == QLatin1String("command")) {
        return key;
    }
#endif
    return QStringLiteral("ctrl");
}

inline QString normalizeHoverReadingUiMode(const QString& rawMode) {
    const QString mode = rawMode.trimmed().toLower();
    if (mode == QLatin1String("light") || mode == QLatin1String("dark")) {
        return mode;
    }
    return QStringLiteral("dark");
}

inline QString normalizeHotRankSortField(const QString& rawField) {
    const QString field = rawField.trimmed().toLower();
    if (field == QLatin1String("pct")) {
        return field;
    }
    return QStringLiteral("mainnetinflow");
}

inline QString normalizeHotRankSortOrder(const QString& rawOrder) {
    const QString order = rawOrder.trimmed().toLower();
    if (order == QLatin1String("asc")) {
        return order;
    }
    return QStringLiteral("desc");
}

inline QString normalizeQuoteSource(const QString& source) {
    Q_UNUSED(source);
    return QStringLiteral("tdx");
}

inline QVector<int> normalizeDailyKlineMAPeriods(const QVector<int>& periods) {
    QVector<int> normalized;
    for (int period : {5, 10, 20, 30, 60, 120, 250}) {
        if (periods.contains(period)) normalized.append(period);
    }
    return normalized;
}

struct AppConfig {
    int pollMs = 3000;
    double opacity = 0.9;
    QString hotkey;
    QString marketBreadthHotkey;
    bool startupShowFloatingWindow = true;
    QString language = "auto";

    QString quoteSource = "tdx";
    QString tdxUserId;
    QString tdxToken; // session token, only persisted on explicit opt-in
    bool tdxRememberToken = false;
    bool tdxMainAutoSelect = true;
    bool tdxExAutoSelect = true;
    QString tdxMainServer;
    QString tdxExServer;
    QStringList tdxMainServerPool; // empty uses the SDK's built-in MAC servers
    QStringList tdxExServerPool;
    // "random" picks one of builtinUserAgentPresets() at startup and keeps it
    // for the session; "custom" sends userAgent verbatim.
    QString userAgentMode = "random";
    QString userAgent = defaultChrome100UserAgent();
    QString networkInterface; // empty lets the operating system select a route
    QString proxyType = "none";
    QString proxyHost;
    int proxyPort = 0;
    QString proxyUser;
    QString proxyPassword;

    bool debugIgnoreTradingTime = false;

    bool acceptBetaUpdates = false;
    bool autoCheckUpdates = true;

    bool simpleModeEnabled = false;
    bool blinkReminderEnabled = false;
    bool trayTooltipEnabled = false;
    bool floatingWindowAlwaysOnTop = true;

    bool logEnabled = true;
    QString logLevel = "info";

    bool transparentBackgroundEnabled = true;
    int transparentBackgroundOpacity = 60;
    QString floatingWindowFontFamily = defaultFloatingWindowFontFamily();
    int floatingWindowFontSize = 0;
    bool floatingWindowFontBold = false;

    bool showHeader = false;
    bool showGrid = false;
    QColor gridColor = QColor(255, 255, 255, 80);
    QColor bgColor = QColor(18, 18, 18, 190);
    QColor textColor = QColor(245, 245, 245);
    QColor costLineColor = QColor("#d66823");
    QColor upColor = QColor(255, 64, 64);
    QColor downColor = QColor(60, 200, 100);
    QColor flatColor = QColor(245, 245, 245);

    QMap<int, bool> visibleColumns {
        {ColCode, false},
        {ColName, true},
        {ColPrice, true},
        {ColPct, true},
        {ColChange, false},
        {ColIndicator, false},
        {ColCost, false},
        {ColCostPct, false},
        {ColBreakevenGain, false},
        {ColRiseSpeed, true}
    };

    QVector<int> columnOrder {
        ColCode,
        ColName,
        ColPrice,
        ColPct,
        ColChange,
        ColIndicator,
        ColCost,
        ColCostPct,
        ColBreakevenGain,
        ColRiseSpeed
    };

    // 0 means auto max width based on current column content/header.
    QMap<int, int> columnMaxWidths {
        {ColCode, 0},
        {ColName, 0},
        {ColPrice, 0},
        {ColPct, 0},
        {ColChange, 0},
        {ColIndicator, 0},
        {ColCost, 0},
        {ColCostPct, 0},
        {ColBreakevenGain, 0},
        {ColRiseSpeed, 0}
    };

    bool hoverReadingEnabled = true;
    double hoverReadingDelaySecs = 1.0;
    QString hoverReadingUiMode = "dark";
    bool hoverReadingTransparentBackgroundEnabled = true;
    bool timelineChartEnabled = true;
    int timelineChartRefreshSecs = 10;
    int timelineChartHoverDelayMs = 500;
    int dailyKlineDays = 60;
    QVector<int> dailyKlineMAPeriods{5, 10, 20, 30};
    bool marketBreadthEnabled = true;
    int marketBreadthRefreshSecs = 10;
    bool timelineChartFixedRangeEnabled = false;
    QColor timelineChartBgColor = QColor(14, 18, 24, 238);
    QColor timelineChartGridColor = QColor(255, 255, 255, 45);
    QColor timelineChartPriceLineColor = QColor("#3cb2ef");
    QColor timelineChartAvgLineColor = QColor(229, 194, 104);
    QColor timelineChartTextColor = QColor(236, 240, 245);
    QColor timelineChartUpColor = QColor(255, 89, 94);
    QColor timelineChartDownColor = QColor(78, 186, 112);
    bool hotRankEnabled = false;
    int hotRankPollSecs = 60;
    double hotRankFlipSecs = 2.6;
    bool hotSectorVisible = true;
    int hotSectorCount = 5;
    QString hotSectorSortField = "mainnetinflow";
    QString hotSectorSortOrder = "desc";
    bool hotConceptVisible = true;
    int hotConceptCount = 5;
    QString hotConceptSortField = "mainnetinflow";
    QString hotConceptSortOrder = "desc";
    bool mousePassthroughEnabled = false;
    QString mousePassthroughActivationKey = "ctrl";
    // Applies to every top-level window, not just the floating window.
    bool globalRightClickClose = true;
    bool floatingWindowDoubleClickStockDetail = true;
    double floatingWindowPaddingPx = static_cast<double>(kFloatingWindowPaddingPx);

    // Empty string means use the default app icon (:/icon.png).
    // Otherwise, a Qt resource path like ":/tray_icons/logo.png".
    QString trayIconPath;

    // Group switching hotkey prefix, e.g. "Ctrl+Shift".
    // When non-empty, prefix+F1..F10 switch to groups in visual order.
    QString groupSwitchHotkeyPrefix;

    // Visual position of the "所有" group in the group bar (0-indexed).
    // e.g. 0 = first (default), 1 = second, etc.
    int groupAllPosition = 0;

    // When true, the "所有" group shows only stocks not present in any other group.
    bool allGroupShowUngroupedOnly = true;

    // Config sync via GitHub Gist (syncs data.yaml content only)
    QString gistToken;
    QString gistId;
    QString gistLastSyncTime;      // local clock ISO string of last successful sync
    QString gistRemoteUpdatedAt;   // GitHub API updated_at at last sync (for conflict detection)

    QMap<int, int> columnWidths;
    QRect windowRect = QRect(120, 120, 760, 280);
    QRect marketBreadthWindowRect;
};

// A selected network interface and a remote proxy are mutually exclusive: the
// interface relay already owns routing, so both cannot be active at once. A
// legacy file that stored both keeps the interface, which is the more specific
// binding, and drops the proxy. Proxy host, port and credentials are retained
// so switching back to automatic routing restores them.
inline void normalizeNetworkSelection(AppConfig* cfg) {
    if (!cfg) {
        return;
    }
    cfg->networkInterface = cfg->networkInterface.trimmed();
    cfg->proxyType = cfg->proxyType.trimmed().toLower();
    if (cfg->proxyType == QLatin1String("socks")) {
        cfg->proxyType = QStringLiteral("socks5");
    }
    if (cfg->proxyType != QLatin1String("none")
        && cfg->proxyType != QLatin1String("http")
        && cfg->proxyType != QLatin1String("socks5")) {
        cfg->proxyType = QStringLiteral("none");
    }
    if (cfg->proxyPort < 0 || cfg->proxyPort > 65535) {
        cfg->proxyPort = 0;
    }
    if (!cfg->networkInterface.isEmpty()) {
        cfg->proxyType = QStringLiteral("none");
    }
}
