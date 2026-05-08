#pragma once

#include <QColor>
#include <QMap>
#include <QRect>
#include <QString>
#include <QVector>
#include <QtGlobal>

enum QuoteColumn {
    ColCode = 0,
    ColName,
    ColPrice,
    ColPct,
    ColChange,
    ColIndicator,
    ColCount
};

inline constexpr int kFloatingWindowPaddingPx = 6;
inline constexpr int kMarketBreadthPopupDefaultWidthPx = 860;
inline constexpr int kMarketBreadthPopupDefaultHeightPx = 715;

struct StockItem {
    QString code;
    QString name;
};

struct QuoteItem {
    QString code;
    QString name;
    double price = qQNaN();
    double pct = qQNaN();
    double change = qQNaN();
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
    bool turnoverValid = false;
};

inline QString defaultChrome100UserAgent() {
    return QStringLiteral(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/100.0.4896.75 Safari/537.36"
    );
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

struct AppConfig {
    int pollMs = 3000;
    double opacity = 0.9;
    QString hotkey;
    QString marketBreadthHotkey;
    bool startupShowFloatingWindow = true;
    QString language = "auto";

    QString userAgent = defaultChrome100UserAgent();
    QString proxyType = "none";
    QString proxyHost;
    int proxyPort = 0;
    QString proxyUser;
    QString proxyPassword;

    bool debugIgnoreTradingTime = false;

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
    QColor upColor = QColor(255, 64, 64);
    QColor downColor = QColor(60, 200, 100);
    QColor flatColor = QColor(245, 245, 245);

    QMap<int, bool> visibleColumns {
        {ColCode, false},
        {ColName, true},
        {ColPrice, true},
        {ColPct, true},
        {ColChange, false},
        {ColIndicator, false}
    };

    QVector<int> columnOrder {
        ColCode,
        ColName,
        ColPrice,
        ColPct,
        ColChange,
        ColIndicator
    };

    // 0 means auto max width based on current column content/header.
    QMap<int, int> columnMaxWidths {
        {ColCode, 0},
        {ColName, 0},
        {ColPrice, 0},
        {ColPct, 0},
        {ColChange, 0},
        {ColIndicator, 0}
    };

    bool hoverReadingEnabled = true;
    double hoverReadingDelaySecs = 1.0;
    QString hoverReadingUiMode = "dark";
    bool hoverReadingTransparentBackgroundEnabled = true;
    bool timelineChartEnabled = true;
    int timelineChartRefreshSecs = 10;
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
    bool floatingWindowDoubleClickToHide = true;
    double floatingWindowPaddingPx = static_cast<double>(kFloatingWindowPaddingPx);

    // Empty string means use the default app icon (:/icon.png).
    // Otherwise, a Qt resource path like ":/tray_icons/logo.png".
    QString trayIconPath;

    QMap<int, int> columnWidths;
    QRect windowRect = QRect(120, 120, 760, 280);
    QRect marketBreadthWindowRect;
};
