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

inline QString defaultChrome100UserAgent() {
    return QStringLiteral(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/100.0.4896.75 Safari/537.36"
    );
}

struct AppConfig {
    int pollMs = 3000;
    double opacity = 0.9;
    QString hotkey = "Ctrl+Alt+S";
    QString apiSource = "tencent";
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

    bool logEnabled = true;
    QString logLevel = "info";

    bool transparentBackgroundEnabled = false;
    int transparentBackgroundOpacity = 100;

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

    QMap<int, int> columnWidths;
    QRect windowRect = QRect(120, 120, 760, 280);
};
