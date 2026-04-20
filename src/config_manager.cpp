#include "config_manager.h"

#include "i18n.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

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

} // namespace

QVector<StockItem> ConfigManager::loadStocksFromYaml(const QString& filePath) {
    QVector<StockItem> out;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }

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

    return out;
}

AppConfig ConfigManager::loadConfig() {
    AppConfig cfg;
    QSettings s("myStocks", "myStocks");

    cfg.pollMs = s.value("general/pollMs", cfg.pollMs).toInt();
    cfg.opacity = s.value("general/opacity", cfg.opacity).toDouble();
    cfg.hotkey = s.value("general/hotkey", cfg.hotkey).toString();
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
    cfg.transparentBackgroundEnabled = s.value(
        "ui/transparentBackgroundEnabled",
        cfg.transparentBackgroundEnabled
    ).toBool();
    cfg.transparentBackgroundOpacity = s.value(
        "ui/transparentBackgroundOpacity",
        cfg.transparentBackgroundOpacity
    ).toInt();

    cfg.showHeader = s.value("ui/showHeader", cfg.showHeader).toBool();
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
    if (cfg.hotkey.trimmed().isEmpty()) {
        cfg.hotkey = "Ctrl+Alt+S";
    }
    if (cfg.apiSource != "mock"
        && cfg.apiSource != "xtick"
        && cfg.apiSource != "sina"
        && cfg.apiSource != "tencent"
        && cfg.apiSource != "eastmoney") {
        cfg.apiSource = "mock";
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

    return cfg;
}

void ConfigManager::saveConfig(const AppConfig& cfg) {
    QSettings s("myStocks", "myStocks");

    s.setValue("general/pollMs", cfg.pollMs);
    s.setValue("general/opacity", cfg.opacity);
    s.setValue("general/hotkey", cfg.hotkey);
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

    s.setValue("ui/showHeader", cfg.showHeader);
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
}
