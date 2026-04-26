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
    double pct = qQNaN();
    double mainNetInflow = qQNaN();
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
    QString hotkey = "Ctrl+Alt+S";
    bool startupShowFloatingWindow = true;
    QString apiSource = "eastmoney";
    QString xtickToken;
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

    bool transparentBackgroundEnabled = false;
    int transparentBackgroundOpacity = 100;
    QString floatingWindowFontFamily = defaultFloatingWindowFontFamily();
    int floatingWindowFontSize = 0;
    bool floatingWindowFontBold = false;

    bool showHeader = true;
    bool showGrid = false;
    QColor gridColor = QColor(255, 255, 255, 80);
    QColor bgColor = QColor(18, 18, 18, 190);
    QColor textColor = QColor(245, 245, 245);
    QColor upColor = QColor(255, 64, 64);
    QColor downColor = QColor(60, 200, 100);
    QColor flatColor = QColor(245, 245, 245);

    QMap<int, bool> visibleColumns {
        {ColCode, true},
        {ColName, true},
        {ColPrice, true},
        {ColPct, true},
        {ColChange, true},
        {ColIndicator, true}
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
    double floatingWindowPaddingPx = static_cast<double>(kFloatingWindowPaddingPx);

    QMap<int, int> columnWidths;
    QRect windowRect = QRect(120, 120, 760, 280);
};
