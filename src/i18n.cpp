#include "i18n.h"

#include <QHash>
#include <QLocale>

namespace {

using TranslationMap = QHash<QString, QHash<QString, QString>>;

const TranslationMap& translations() {
    static const TranslationMap kTranslations = {
        {
            "en_US",
            {
                {"app.name", "MyStocks"},
                {"tray.toggle", "Show/Hide floating window"},
                {"tray.settings", "Settings"},
                {"tray.reload", "Reload config"},
                {"tray.quit", "Quit"},
                {"settings.title", "Settings"},
                {"settings.tab.general", "General"},
                {"settings.tab.network", "Network"},
                {"settings.tab.display", "Display"},
                {"settings.tab.other", "Other"},
                {"settings.general.poll", "Poll interval (ms)"},
                {"settings.general.hotkey", "Toggle hotkey"},
                {"settings.general.hotkeyHint", "Use Ctrl/Alt/Shift/Meta + key. Press Backspace/Delete to clear."},
                {"settings.general.apiSource", "API source"},
                {"settings.general.token", "XTick token"},
                {"settings.general.userAgent", "User-Agent"},
                {"settings.general.proxyType", "Proxy type"},
                {"settings.general.proxyHost", "Proxy host"},
                {"settings.general.proxyPort", "Proxy port"},
                {"settings.general.proxyUser", "Proxy username"},
                {"settings.general.proxyPassword", "Proxy password"},
                {"settings.general.debugIgnoreTradingTime", "Debug: ignore market time and always poll"},
                {"settings.other.logEnabled", "Enable file logging"},
                {"settings.other.logLevel", "Log level"},
                {"settings.other.logLevel.debug", "Debug"},
                {"settings.other.logLevel.info", "Info"},
                {"settings.other.logLevel.warn", "Warn"},
                {"settings.other.logLevel.error", "Error"},
                {"settings.other.openLogFolder", "View logs"},
                {"settings.other.writeStockNames", "Write stock names"},
                {"settings.other.writeStockNamesResult", "Successfully wrote %1 stock names."},
                {"settings.other.writeStockNamesUnavailable", "Write stock names is currently unavailable."},
                {"settings.general.transparentBackground", "Enable fully transparent background"},
                {"settings.general.transparentOpacity", "Transparency"},
                {"settings.general.transparentOpacityValue", "%1%"},
                {"settings.general.language", "Language"},
                {"settings.general.background", "Background"},
                {"settings.general.text", "Text"},
                {"settings.general.up", "Up"},
                {"settings.general.down", "Down"},
                {"settings.general.flat", "Flat"},
                {"settings.general.resetColors", "Reset default colors"},
                {"settings.source.mock", "Mock"},
                {"settings.source.xtick", "XTick"},
                {"settings.source.sina", "Sina"},
                {"settings.source.tencent", "Tencent(Support HK stocks)"},
                {"settings.source.eastmoney", "Eastmoney"},
                {"settings.proxy.none", "None"},
                {"settings.proxy.http", "HTTP"},
                {"settings.proxy.socks5", "SOCKS5"},
                {"settings.display.showHeader", "Show table header"},
                {"settings.display.showGrid", "Show grid lines"},
                {"settings.display.gridColor", "Grid color"},
                {"settings.display.simpleMode", "Simple mode"},
                {"settings.display.blinkReminder", "Blink reminder"},
                {"settings.display.columns", "Columns"},
                {"settings.display.columnsHint", "Check to show, uncheck to remove, drag to reorder."},
                {"settings.display.columnVisibleFmt", "Column %1 visible"},
                {"settings.display.columnMaxFmt", "Column %1 max width"},
                {"settings.display.columnMaxNameFmt", "%1 max width"},
                {"settings.display.auto", "Auto"},
                {"settings.display.pxSuffix", " px"},
                {"settings.language.auto", "Auto (follow system)"},
                {"settings.language.zh", "Simplified Chinese"},
                {"settings.language.en", "English"},
                {"settings.color.pick", "Pick color"},
                {"hotkey.register.failed", "Global hotkey registration failed. It may be in use by another app."},
                {"provider.xtick.token.empty", "XTick token is empty"},
                {"reload.config.success", "Reloaded %1 stocks from data.yaml."},
                {"reload.config.failed", "Failed to reload data.yaml or no valid stocks found."}
            }
        },
        {
            "zh_CN",
            {
                {"app.name", "我的股票"},
                {"tray.toggle", "显示/隐藏悬浮窗"},
                {"tray.settings", "设置"},
                {"tray.reload", "重新加载配置"},
                {"tray.quit", "退出"},
                {"settings.title", "设置"},
                {"settings.tab.general", "通用"},
                {"settings.tab.network", "网络"},
                {"settings.tab.display", "显示"},
                {"settings.tab.other", "其它"},
                {"settings.general.poll", "轮询间隔 (毫秒)"},
                {"settings.general.hotkey", "切换热键"},
                {"settings.general.hotkeyHint", "请使用 Ctrl/Alt/Shift/Meta + 按键；按 Backspace/Delete 清空。"},
                {"settings.general.apiSource", "数据源"},
                {"settings.general.token", "XTick 令牌"},
                {"settings.general.userAgent", "User-Agent"},
                {"settings.general.proxyType", "代理类型"},
                {"settings.general.proxyHost", "代理地址"},
                {"settings.general.proxyPort", "代理端口"},
                {"settings.general.proxyUser", "代理用户名"},
                {"settings.general.proxyPassword", "代理密码"},
                {"settings.general.debugIgnoreTradingTime", "调试: 忽略交易时间并持续轮询"},
                {"settings.other.logEnabled", "启用日志记录"},
                {"settings.other.logLevel", "日志等级"},
                {"settings.other.logLevel.debug", "调试"},
                {"settings.other.logLevel.info", "信息"},
                {"settings.other.logLevel.warn", "警告"},
                {"settings.other.logLevel.error", "错误"},
                {"settings.other.openLogFolder", "查看日志"},
                {"settings.other.writeStockNames", "写入股票名称"},
                {"settings.other.writeStockNamesResult", "成功写入 %1 条股票名称。"},
                {"settings.other.writeStockNamesUnavailable", "当前无法执行写入股票名称。"},
                {"settings.general.transparentBackground", "启用全透明背景"},
                {"settings.general.transparentOpacity", "透明度"},
                {"settings.general.transparentOpacityValue", "%1%"},
                {"settings.general.language", "语言"},
                {"settings.general.background", "背景色"},
                {"settings.general.text", "文字色"},
                {"settings.general.up", "上涨色"},
                {"settings.general.down", "下跌色"},
                {"settings.general.flat", "平盘色"},
                {"settings.general.resetColors", "恢复默认配色"},
                {"settings.source.mock", "模拟"},
                {"settings.source.xtick", "XTick"},
                {"settings.source.sina", "新浪"},
                {"settings.source.tencent", "腾讯 (支持港股)"},
                {"settings.source.eastmoney", "东方财富"},
                {"settings.proxy.none", "不使用"},
                {"settings.proxy.http", "HTTP"},
                {"settings.proxy.socks5", "SOCKS5"},
                {"settings.display.showHeader", "显示表头"},
                {"settings.display.showGrid", "显示网格线"},
                {"settings.display.gridColor", "网格线颜色"},
                {"settings.display.simpleMode", "简洁模式"},
                {"settings.display.blinkReminder", "闪烁提醒"},
                {"settings.display.columns", "显示列"},
                {"settings.display.columnsHint", "勾选即显示，取消即移除，拖动可调整顺序。"},
                {"settings.display.columnVisibleFmt", "第 %1 列显示"},
                {"settings.display.columnMaxFmt", "第 %1 列最大宽度"},
                {"settings.display.columnMaxNameFmt", "%1 最大宽度"},
                {"settings.display.auto", "自动"},
                {"settings.display.pxSuffix", " 像素"},
                {"settings.language.auto", "自动 (跟随系统)"},
                {"settings.language.zh", "简体中文"},
                {"settings.language.en", "英文"},
                {"settings.color.pick", "选择颜色"},
                {"hotkey.register.failed", "全局热键注册失败，可能已被其他应用占用。"},
                {"provider.xtick.token.empty", "XTick 令牌为空"},
                {"reload.config.success", "已从 data.yaml 重新加载 %1 条股票。"},
                {"reload.config.failed", "重新加载 data.yaml 失败或没有有效股票。"}
            }
        }
    };

    return kTranslations;
}

QString resolvedLanguageCode(const QString& language) {
    const QString normalized = i18n::normalizeLanguage(language);
    if (normalized != "auto") {
        return normalized;
    }

    const QString system = QLocale::system().name();
    if (system.startsWith("zh", Qt::CaseInsensitive)) {
        return "zh_CN";
    }
    return "en_US";
}

} // namespace

namespace i18n {

QString normalizeLanguage(const QString& language) {
    QString value = language.trimmed();
    if (value.isEmpty()) {
        return "auto";
    }

    value.replace('-', '_');

    if (value.compare("auto", Qt::CaseInsensitive) == 0) {
        return "auto";
    }
    if (value.startsWith("zh", Qt::CaseInsensitive)) {
        return "zh_CN";
    }
    if (value.startsWith("en", Qt::CaseInsensitive)) {
        return "en_US";
    }

    return "auto";
}

QString resolveLanguage(const QString& language) {
    return resolvedLanguageCode(language);
}

QString t(const QString& key, const QString& language) {
    const QString code = resolvedLanguageCode(language);
    const TranslationMap& map = translations();

    const QHash<QString, QString> fallback = map.value("en_US");
    const QString fallbackValue = fallback.value(key, key);
    const QHash<QString, QString> langMap = map.value(code);
    return langMap.value(key, fallbackValue);
}

QStringList columnNames(const QString& language) {
    if (resolvedLanguageCode(language) == "zh_CN") {
        return {"代码", "名称", "最新价", "涨跌幅", "涨跌额", "标识"};
    }
    return {"Code", "Name", "Price", "Change%", "Change", "Signal"};
}

} // namespace i18n
