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

    QuoteItem indexQuote;
    indexQuote.code = "1.000001";
    indexQuote.riseSpeed = 1.5;
    indexQuote.pct = 0.5;
    model.updateQuotes({indexQuote});
    check(model.data(model.index(3, ColRiseSpeed), Qt::DisplayRole).toString().isEmpty(),
        "Index rows must not display rise speed");

    QVector<QuoteItem> preOpenReset;
    for (const QString& code : {"tdx:main:1:600519", "116.00700", "1.510300", "1.000001", "1.600519"}) {
        QuoteItem reset;
        reset.code = code;
        reset.price = 0.0;
        reset.preClose = 10.0;
        reset.pct = -100.0;
        reset.change = -10.0;
        reset.riseSpeed = -100.0;
        preOpenReset.push_back(reset);
    }
    model.updateQuotes(preOpenReset);
    check(model.data(model.index(0, ColPrice), Qt::DisplayRole).toString() == "10.000",
        "Pre-open reset prices must display yesterday's close");
    check(model.data(model.index(0, ColPrice), Qt::ForegroundRole).value<QColor>() == cfg.flatColor,
        "Pre-open reset prices must use the flat color");
    check(model.data(model.index(0, ColCode), Qt::DisplayRole).toString() == "tdx:main:1:600519",
        "Pre-open reset must retain the quote identity");
    for (int column : {ColPct, ColChange, ColIndicator, ColRiseSpeed}) {
        check(model.data(model.index(0, column), Qt::DisplayRole).toString().isEmpty(),
            "Pre-open reset must hide intraday change fields");
    }
    check(model.trayTooltipText().contains("10.000") && !model.trayTooltipText().contains("-100.00%"),
        "Tray tooltip must use the previous close and hide reset percentages");

    // A live HK/futures row must not prevent an A-share reset from being detected.
    QuoteModel mixed;
    mixed.setConfig(cfg);
    mixed.setStocks({{"1.600519", "Stock", 20.0}, {"116.00700", "Tencent"}});
    QuoteItem reset;
    reset.code = "1.600519";
    reset.price = 0.0;
    reset.preClose = 18.5;
    reset.pct = -100.0;
    reset.change = -18.5;
    quote.code = "116.00700";
    quote.price = 532.5;
    quote.pct = 2.0;
    mixed.updateQuotes({reset, quote});
    check(mixed.data(mixed.index(0, ColPrice), Qt::DisplayRole).toString() == "18.500",
        "Mixed reset batch must use the previous close");
    check(mixed.data(mixed.index(0, ColPct), Qt::DisplayRole).toString().isEmpty(),
        "Mixed reset batch must hide -100 percent");
    check(mixed.data(mixed.index(0, ColCostPct), Qt::DisplayRole).toString() == "-7.50%",
        "Cost return must use the previous close instead of the zero sentinel");
    check(mixed.data(mixed.index(1, ColPct), Qt::DisplayRole).toString() == "+2.00%",
        "Live rows must retain their percentage during another market's reset");
    reset.resetData = true;
    reset.price = 18.5; // normalized bridge payload
    reset.pct = reset.change = qQNaN();
    mixed.updateQuotes({reset});
    check(mixed.data(mixed.index(0, ColPrice), Qt::DisplayRole).toString() == "18.500",
        "Explicit reset flag must preserve the normalized previous close");
    reset.price = 0.0;
    reset.preClose = qQNaN();
    mixed.updateQuotes({reset});
    check(mixed.data(mixed.index(0, ColPrice), Qt::DisplayRole).toString() == "--",
        "Missing previous close must not display a fake zero price");
    check(mixed.data(mixed.index(0, ColCostPct), Qt::DisplayRole).toString().isEmpty(),
        "Missing previous close must not display -100 percent cost return");
    reset.resetData = false;
    reset.price = 21.0;
    reset.pct = 5.0;
    reset.change = 1.0;
    reset.riseSpeed = 0.1;
    mixed.updateQuotes({reset});
    check(mixed.data(mixed.index(0, ColPrice), Qt::DisplayRole).toString() == "21.000"
        && mixed.data(mixed.index(0, ColPct), Qt::DisplayRole).toString() == "+5.00%",
        "Live trading must recover from reset state on the next quote");

    cfg.marketBreadthEnabled = true;
    mixed.setConfig(cfg);
    const int breadthRow = 0;
    check(mixed.rowKind(breadthRow) == QuoteModel::RowKindMarketBreadth,
        "Expected breadth row before quotes");
    auto checkBlankBreadth = [&]() {
        check(!mixed.marketBreadthValid() && !mixed.specialRowHasData(breadthRow),
            "Invalid or empty breadth must not draw counts");
        check(mixed.specialRowText(breadthRow).isEmpty(), "Invalid breadth fallback must be blank");
        for (int column = 0; column < ColCount; ++column) {
            check(mixed.data(mixed.index(breadthRow, column), Qt::DisplayRole).toString().isEmpty(),
                "Invalid breadth must not expose placeholder text");
        }
    };
    checkBlankBreadth();
    MarketBreadthSnapshot snapshot;
    snapshot.breadthValid = true;
    mixed.setMarketBreadthSnapshot(snapshot);
    checkBlankBreadth();
    snapshot.upCount = 3000;
    snapshot.flatCount = 100;
    snapshot.downCount = 2000;
    mixed.setMarketBreadthSnapshot(snapshot);
    check(mixed.marketBreadthValid() && mixed.specialRowHasData(breadthRow),
        "Valid breadth must remain visible");
    snapshot.upCount = -1;
    mixed.setMarketBreadthSnapshot(snapshot);
    checkBlankBreadth();
    mixed.setMarketBreadthSnapshot({});
    checkBlankBreadth();
    return 0;
}
