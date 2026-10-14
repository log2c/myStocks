#include "quote_model.h"
#include "i18n.h"
#include "watchlist_utils.h"
#include <QCoreApplication>
#include <QDebug>

void check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    AppConfig cfg;
    cfg.marketBreadthEnabled = false;
    cfg.hotRankEnabled = false;
    check(cfg.quoteSource == "tdx", "TDX must be the default");
    check(normalizeQuoteSource(" EASTMONEY ") == "tdx", "Disabled EastMoney settings must select TDX");
    check(ColBreakevenGain == 8 && ColRiseSpeed == 9, "Persisted column IDs changed");
    check(i18n::columnNames("zh_CN").size() == ColCount, "Missing translated column");
    check(watchlist_utils::normalizedColumnOrder({0, 1, 2, 3, 4, 5, 6, 7, 8}).last() == ColRiseSpeed, "Old column layout must append new column");
    check(watchlist_utils::normalizeApiWatchCode("TDX:MAIN:001:881319") == "tdx:main:1:881319", "Native TDX board code was rewritten as EastMoney");
    check(watchlist_utils::normalizeSectorCode("tdx:main:1:881319") == "tdx:main:1:881319", "Native board classification failed");
    check(watchlist_utils::normalizeApiWatchCode("tdx:ex:49:02800") == "tdx:ex:49:02800", "Extended market ID was lost");
    check(watchlist_utils::isHongKongCode("tdx:ex:49:02800"), "Native HK instrument classification failed");
    check(watchlist_utils::isCostEditableCode("2.920002"), "Beijing stock cost editing was lost");
    check(watchlist_utils::isCostEditableCode("tdx:main:1:600519"), "Native TDX stocks must support cost editing");
    check(!watchlist_utils::isCostEditableCode("tdx:main:1:000001"), "Native indexes must not accept stock cost editing");
    check(watchlist_utils::isPredefinedAshareIndexCode("tdx:main:0:399006"), "Native A-share index classification was lost");
    check(!watchlist_utils::isTdxCode("tdx:ex:256:00700"), "Invalid market was accepted");
    QuoteModel model;
    model.setConfig(cfg);
    for (const QString& code : {"1.600519", "116.00700", "1.510300", "0.159915", "1.000001", "0.399006", "100.HSI", "124.HSTECH"}) {
        check(watchlist_utils::isTdxCompatibleCode(code), "Legacy stock/ETF/index with an explicit TDX mapping must remain visible");
    }
    for (const QString& code : {"BK0420", "90.BK0420", "100.SPX", "999.UNKNOWN", "tdx:ex:256:00700"}) {
        check(!watchlist_utils::isTdxCompatibleCode(code), "Unsupported code must be skipped");
    }
    model.setStocks({{"90.BK0420", "Unsupported board"}, {"tdx:main:1:600519", "贵州茅台"},
        {"116.00700", "Tencent"}, {"1.510300", "ETF"}, {"1.000001", "Index"}, {"1.600519", "Legacy stock"}});
    check(model.rowCount() == 5, "Only unsupported rows should be filtered, with remaining row mappings preserved");
    QuoteItem quote;
    quote.code = "tdx:main:1:600519";
    quote.pct = 2;
    quote.riseSpeed = -0.25;
    model.updateQuotes({quote});
    const QModelIndex cell = model.index(0, ColRiseSpeed);
    check(model.data(cell, Qt::DisplayRole).toString() == "-0.25%", "Rise speed display units are wrong");
    check(model.data(cell, Qt::ForegroundRole).value<QColor>() == cfg.downColor, "Rise speed must use its own sign");
    quote.riseSpeed = 0;
    model.updateQuotes({quote});
    check(model.data(cell, Qt::DisplayRole).toString().contains("0.00%"), "Zero speed must not become missing");
    quote.riseSpeed = qQNaN();
    model.updateQuotes({quote});
    check(model.data(cell, Qt::DisplayRole).toString() == "--", "Unavailable speed must clear old data");
    quote.code = "116.00700";
    quote.price = 532.5;
    model.updateQuotes({quote});
    check(model.data(model.index(1, ColPrice), Qt::DisplayRole).toString() == "532.500", "Filtering a BK row must not offset legacy quote updates");
    return 0;
}
