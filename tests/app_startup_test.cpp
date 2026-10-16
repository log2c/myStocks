#include "app_controller.h"
#include "app_constants.h"
#include "config_manager.h"
#include "floating_window.h"
#include "market_breadth_window.h"
#include "quote_model.h"
#include "quote_provider.h"
#include "startup_splash.h"
#include "settings_dialog.h"
#include <QComboBox>
#include <QListWidget>
#include <QScrollBar>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTableView>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include "tdx_bridge.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QTabWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <functional>
#include <cmath>

namespace {
void check(bool condition, const char* message) {
    if (!condition) qFatal("%s", message);
}
struct PendingRoute {
    QPointer<QObject> context;
    AppConfig config;
    QString kind;
    tdx_bridge::Callback callback;
};
QVector<PendingRoute> routes;
int quoteRequests = 0;
int timelineRequests = 0;
int klineRequests = 0;
QJsonObject lastTimelinePayload;
QJsonObject lastKlinePayload;
bool constituentFixture = false;
QJsonObject breadthFixture;
int memberRequests = 0;
struct PendingSearch {
    QPointer<QObject> context;
    QString keyword;
    tdx_bridge::Callback callback;
};
QVector<PendingSearch> searches;
void events(int milliseconds = 10) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
void finishRoute(bool fail = false, bool switched = false) {
    check(!routes.isEmpty(), "Expected a pending route preparation");
    auto route = routes.takeFirst();
    if (route.context) {
        route.callback({{"address", route.kind == "main" ? "main.test:7709" : "ex.test:7727"},
            {"latencyMs", 12.5}, {"previous", "old.test:7709"}, {"switched", switched}},
            fail ? "No available route" : QString());
    }
    events();
}
}

// Replace only the C++ bridge for this orchestration test. Actual TCP/protocol
// validation, manual fallback and proxy behavior are covered by Go fixtures.
namespace tdx_bridge {
void request(QObject* context, const AppConfig& config, QJsonObject payload, Callback callback) {
    const QString operation = payload.value("operation").toString();
    if (operation == "prepare_servers") {
        routes.append({context, config, payload.value("kind").toString(), std::move(callback)});
        return;
    }
    if (operation == "search") {
        searches.append({context, payload.value("keyword").toString(), std::move(callback)});
        return;
    }
    if (operation == "timeline") { ++timelineRequests; lastTimelinePayload = payload; }
    if (operation == "klines") { ++klineRequests; lastKlinePayload = payload; }
    if (operation == "quotes") ++quoteRequests;
    QJsonObject response{{"quotes", QJsonArray()}};
    if (operation == "breadth" && !breadthFixture.isEmpty()) response = breadthFixture;
    if (constituentFixture && operation == "members") {
        ++memberRequests;
        response = {{"items", QJsonArray{
            QJsonObject{{"code", "1.600519"}, {"name", "Stock A"}, {"price", 100}, {"pct", 2}, {"marketCap", 2e12}},
            QJsonObject{{"code", "0.000001"}, {"name", "Stock B"}, {"price", 20}, {"pct", -1}, {"marketCap", 1e12}}}}};
    }
    QPointer<QObject> guarded(context);
    QTimer::singleShot(0, context, [guarded, response, callback = std::move(callback)]() {
        if (guarded) callback(response, {});
    });
}
QJsonObject networkProxyBlocking(const AppConfig&, QString* error) {
    if (error) error->clear();
    return {{"host", "127.0.0.1"}, {"port", 1080}, {"user", "test"}, {"password", "test"}};
}
QJsonObject requestBlocking(const QJsonObject&, QString*) { return {}; }
}

void checkStockChartClicks() {
    const int oldDragTime = QApplication::startDragTime();
    QApplication::setStartDragTime(120);
    AppConfig cfg;
    check(cfg.timelineChartHoverDelayMs == 500, "Intraday hover must default to 500ms");
    cfg.timelineChartHoverDelayMs = 140;
    cfg.marketBreadthEnabled = false;
    cfg.hotRankEnabled = false;
    cfg.hoverReadingEnabled = false;
    cfg.mousePassthroughEnabled = false;
    cfg.floatingWindowDoubleClickToHide = true;
    cfg.floatingWindowDoubleClickStockDetail = false;
    QuoteModel model;
    model.setConfig(cfg);
    model.setStocks({{"1.510300", "ETF"}, {"1.600519", "Stock"}});
    FloatingWindow window(&model);
    window.applyConfig(cfg);
    window.move(80, 80);
    window.show();
    events();
    QTableView* view = nullptr;
    for (auto* candidate : window.findChildren<QTableView*>()) {
        if (candidate->isVisible() && !candidate->isRowHidden(0)) view = candidate;
    }
    check(view, "Expected a visible stock table");
    QWidget* popup = nullptr;
    QWidget* daily = nullptr;
    for (auto* top : QApplication::topLevelWidgets()) {
        if (auto* child = top->findChild<QWidget*>("dailyKlineChart")) {
            popup = top;
            daily = child;
        }
    }
    check(popup && daily && !popup->isVisible(), "Stock chart must initially be hidden");
    const auto rowGlobal = [&](int row, int column = ColPrice) {
        return view->viewport()->mapToGlobal(view->visualRect(model.index(row, column)).center());
    };
    const auto mouse = [&](QWidget* target, QEvent::Type type, const QPoint& global,
                           Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(target->mapFromGlobal(global)), QPointF(global),
            button, buttons, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    };
    const auto firstColumn = [&]() {
        for (int visual = 0; visual < view->horizontalHeader()->count(); ++visual) {
            const int logical = view->horizontalHeader()->logicalIndex(visual);
            if (!view->isColumnHidden(logical) && view->columnWidth(logical) > 0) return logical;
        }
        return -1;
    };
    const auto hover = [&](int row, int column = -1) {
        mouse(view->viewport(), QEvent::MouseMove, rowGlobal(row, column < 0 ? firstColumn() : column),
            Qt::NoButton, Qt::NoButton);
    };
    const auto click = [&](int row, Qt::MouseButton button) {
        const QPoint global = rowGlobal(row);
        mouse(view->viewport(), QEvent::MouseButtonPress, global, button, button);
        mouse(button == Qt::LeftButton ? static_cast<QWidget*>(&window) : view->viewport(),
            QEvent::MouseButtonRelease, global, button, Qt::NoButton);
    };
    check(firstColumn() == ColName, "The hidden code column must not count as the first displayed column");
    hover(0, ColPrice);
    events(160);
    check(!popup->isVisible() && timelineRequests == 0, "Non-first visible columns must never trigger hover charts");
    hover(0);
    events(90);
    check(!popup->isVisible() && timelineRequests == 0, "Hover must not trigger before the configured delay");
    hover(0, ColPrice);
    events(80);
    check(!popup->isVisible(), "Leaving the first column must cancel its pending hover");
    hover(0);
    events(90);
    check(!popup->isVisible(), "Reentering the first column must wait for the full delay");
    mouse(view->viewport(), QEvent::MouseMove, rowGlobal(0, firstColumn()) + QPoint(1, 0), Qt::NoButton, Qt::NoButton);
    events(80);
    check(popup->isVisible() && !daily->isVisible() && timelineRequests == 1,
        "Movement within the first cell must preserve the hover timer");
    const int beforeSameRow = timelineRequests;
    hover(0);
    events(160);
    check(timelineRequests == beforeSameRow, "An already displayed hovered stock must not be fetched repeatedly");

    click(0, Qt::LeftButton);
    events(160);
    check(!popup->isVisible(), "Left click alone must no longer trigger an intraday chart");
    hover(0);
    events(80);
    hover(1);
    events(90);
    check(!popup->isVisible(), "Changing rows must restart the hover delay");
    events(80);
    check(popup->isVisible() && !daily->isVisible()
        && lastTimelinePayload.value("code").toString() == "1.600519",
        "Hover must open the latest row after its own full delay");
    click(1, Qt::RightButton);
    events(160);
    check(popup->isVisible() && daily->isVisible() && klineRequests == 1,
        "Right-click must still show daily immediately without Ctrl");
    const int beforePinnedHover = timelineRequests;
    hover(1);
    events(160);
    check(daily->isVisible() && timelineRequests == beforePinnedHover,
        "Hovering the same right-clicked row must not replace pinned daily mode");

    hover(0);
    events(60);
    const QPoint oldPosition = window.pos();
    QPoint global = rowGlobal(0);
    mouse(view->viewport(), QEvent::MouseButtonPress, global, Qt::LeftButton, Qt::LeftButton);
    events(220);
    check(!popup->isVisible(), "Holding a drag press must cancel the pending hover timer");
    mouse(&window, QEvent::MouseMove, global + QPoint(40, 20), Qt::NoButton, Qt::LeftButton);
    events(160);
    check(window.pos() == oldPosition + QPoint(40, 20) && !popup->isVisible(),
        "Dragging must move the window without triggering a chart");
    mouse(&window, QEvent::MouseButtonRelease, global + QPoint(40, 20), Qt::LeftButton, Qt::NoButton);
    events(160);
    check(!popup->isVisible(), "Releasing a drag must not trigger a pending hover or click chart");
    hover(0);
    events(160);
    check(popup->isVisible() && !daily->isVisible(), "Hover may trigger again after dragging has finished");

    // A fast drag returning to its origin must still be treated as a drag.
    global = rowGlobal(0);
    mouse(view->viewport(), QEvent::MouseButtonPress, global, Qt::LeftButton, Qt::LeftButton);
    mouse(&window, QEvent::MouseMove, global + QPoint(40, 0), Qt::NoButton, Qt::LeftButton);
    mouse(&window, QEvent::MouseMove, global, Qt::NoButton, Qt::LeftButton);
    mouse(&window, QEvent::MouseButtonRelease, global, Qt::LeftButton, Qt::NoButton);
    events(160);
    check(!popup->isVisible(), "Out-and-back dragging must not open a chart");
    hover(0);
    events(60);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(view->viewport(), &leave);
    QApplication::sendEvent(&window, &leave);
    events(160);
    check(!popup->isVisible(), "Leaving the row before the delay must cancel the pending chart");
    hover(0);
    events(60);
    click(0, Qt::LeftButton);
    global = rowGlobal(0);
    mouse(view->viewport(), QEvent::MouseButtonDblClick, global, Qt::LeftButton, Qt::LeftButton);
    mouse(&window, QEvent::MouseButtonRelease, global, Qt::LeftButton, Qt::NoButton);
    events(160);
    check(!window.isVisible() && !popup->isVisible(),
        "Double click must hide the window and cancel pending hover");
    window.show();
    events();
    hover(0);
    window.hide();
    events(160);
    check(!popup->isVisible(), "A pending hover must not resurrect a hidden window's chart");

    window.show();
    events();
    hover(0);
    cfg.timelineChartHoverDelayMs = 300;
    window.applyConfig(cfg);
    events(160);
    check(!popup->isVisible(), "Changing settings must cancel hover started with an old delay");
    hover(0);
    events(160);
    check(!popup->isVisible(), "Updated hover delay must take effect");
    events(170);
    check(popup->isVisible(), "Updated delay must eventually trigger intraday");
    cfg.timelineChartEnabled = false;
    window.applyConfig(cfg);
    hover(0);
    events(330);
    click(0, Qt::RightButton);
    check(!popup->isVisible(), "Disabling charts must suppress hover and right-click charts");
    cfg.timelineChartEnabled = true;
    cfg.timelineChartHoverDelayMs = 0;
    window.applyConfig(cfg);
    hover(0);
    events();
    check(popup->isVisible(), "A configured zero delay must trigger at the next event-loop iteration");
    QWidget outside;
    const QPoint outsidePoint = window.frameGeometry().united(popup->frameGeometry()).bottomRight() + QPoint(100, 100);
    mouse(&outside, QEvent::MouseButtonPress, outsidePoint, Qt::LeftButton, Qt::LeftButton);
    check(!popup->isVisible(), "An outside click must still dismiss the chart");
    const auto dismissChart = [&]() {
        const QPoint point = window.frameGeometry().united(popup->frameGeometry()).bottomRight() + QPoint(100, 100);
        mouse(&outside, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        check(!popup->isVisible(), "Expected chart dismissal between column scenarios");
    };
    auto* header = view->horizontalHeader();
    header->moveSection(header->visualIndex(ColPrice), 0);
    hover(0, ColName);
    events();
    check(!popup->isVisible(), "The previously first name column must stop triggering after reordering");
    hover(0, ColPrice);
    events();
    check(popup->isVisible() && !daily->isVisible(), "The reordered price column must become the hover trigger");
    dismissChart();
    view->setColumnHidden(ColPrice, true);
    check(firstColumn() == ColName, "Hidden leading sections must be skipped");
    hover(0, ColName);
    events();
    check(popup->isVisible(), "The next visible column must trigger when the leading column is hidden");
    dismissChart();
    view->setColumnHidden(ColCode, false);
    header->moveSection(header->visualIndex(ColCode), 0);
    hover(0, ColName);
    events();
    check(!popup->isVisible(), "Only the newly shown code column may trigger when moved to the front");
    hover(0, ColCode);
    events();
    check(popup->isVisible(), "A visible leading code column must trigger intraday");
    dismissChart();
    // Right-click remains valid on any visible column, including the name.
    const QPoint namePoint = rowGlobal(1, ColName);
    mouse(view->viewport(), QEvent::MouseButtonPress, namePoint, Qt::RightButton, Qt::RightButton);
    mouse(view->viewport(), QEvent::MouseButtonRelease, namePoint, Qt::RightButton, Qt::NoButton);
    events();
    check(popup->isVisible() && daily->isVisible(), "Right-click daily mode must not be restricted to the first column");
    QApplication::setStartDragTime(oldDragTime);
}

void checkBreadthHeaderClick() {
    const int oldDragTime = QApplication::startDragTime();
    QApplication::setStartDragTime(120);
    QTcpServer proxy;
    check(proxy.listen(QHostAddress::LocalHost), "Could not create an isolated overview proxy");
    AppConfig cfg;
    cfg.marketBreadthEnabled = true;
    cfg.hotRankEnabled = false;
    cfg.timelineChartEnabled = false; // Opening overview must not depend on chart settings.
    cfg.hoverReadingEnabled = false;
    cfg.mousePassthroughEnabled = false;
    cfg.proxyType = "http";
    cfg.proxyHost = "127.0.0.1";
    cfg.proxyPort = proxy.serverPort();
    QuoteModel model;
    model.setConfig(cfg);
    model.setStocks({{"1.600519", "Stock"}});
    MarketBreadthSnapshot snapshot;
    snapshot.upCount = 3000;
    snapshot.flatCount = 100;
    snapshot.downCount = 2000;
    snapshot.breadthValid = true;
    model.setMarketBreadthSnapshot(snapshot);
    FloatingWindow window(&model);
    window.applyConfig(cfg);
    window.move(60, 60);
    window.show();
    events();
    QTableView* topTable = nullptr;
    for (auto* view : window.findChildren<QTableView*>()) {
        if (view->isVisible() && !view->isRowHidden(0)) topTable = view;
    }
    check(topTable && model.rowKind(0) == QuoteModel::RowKindMarketBreadth,
        "Expected the market breadth counts in the floating window's top table");
    MarketBreadthDetailWindow* market = nullptr;
    for (auto* top : QApplication::topLevelWidgets()) {
        if (auto* candidate = dynamic_cast<MarketBreadthDetailWindow*>(top)) market = candidate;
    }
    check(market && !market->isVisible(), "Market overview must start hidden");
    int column = 0;
    while (column < ColCount && topTable->isColumnHidden(column)) ++column;
    const auto breadthPoint = [&](int quarter) {
        const QRect row = topTable->visualRect(model.index(0, column));
        return topTable->viewport()->mapToGlobal(QPoint(row.left() + row.width() * quarter / 4, row.center().y()));
    };
    const auto mouse = [](QWidget* target, QEvent::Type type, const QPoint& global,
                          Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(target->mapFromGlobal(global)), QPointF(global), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    };
    const auto click = [&](int quarter, bool releaseOnViewport = false, Qt::MouseButton button = Qt::LeftButton) {
        const QPoint global = breadthPoint(quarter);
        mouse(topTable->viewport(), QEvent::MouseButtonPress, global, button, button);
        mouse(releaseOnViewport ? topTable->viewport() : static_cast<QWidget*>(&window),
            QEvent::MouseButtonRelease, global, button, Qt::NoButton);
        events();
    };
    click(1);
    check(market->isVisible() && market->isPinnedFromTray(),
        "Single-clicking rise counts must open an independent market overview");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&window, &leave);
    check(market->isVisible(), "The opened overview must remain visible when the cursor leaves the floating window");
    market->move(market->pos() + QPoint(12, 8));
    const QRect positioned = market->geometry();
    click(3, true);
    check(market->isVisible() && market->geometry() == positioned,
        "Clicking fall counts while overview is visible must activate it without toggling or repositioning it");
    market->showMinimized();
    events();
    click(1);
    check(market->isVisible() && !market->isMinimized(), "Clicking counts must restore a minimized overview");
    window.toggleMarketBreadthDetailPopupFromTray();
    check(!market->isVisible(), "The existing tray entry must retain its toggle behavior");
    click(3);
    check(market->isVisible(), "Clicking counts must reopen a closed overview");
    market->hidePopup();
    click(1, true, Qt::RightButton);
    check(!market->isVisible(), "Right-clicking breadth counts must not open overview");
    const QPoint first = breadthPoint(1);
    mouse(topTable->viewport(), QEvent::MouseButtonPress, first, Qt::LeftButton, Qt::LeftButton);
    events(150);
    mouse(&window, QEvent::MouseButtonRelease, first, Qt::LeftButton, Qt::NoButton);
    check(!market->isVisible(), "A long press on the floating window must not be treated as an overview click");
    mouse(topTable->viewport(), QEvent::MouseButtonPress, first, Qt::LeftButton, Qt::LeftButton);
    mouse(&window, QEvent::MouseMove, first + QPoint(40, 0), Qt::NoButton, Qt::LeftButton);
    mouse(&window, QEvent::MouseMove, first, Qt::NoButton, Qt::LeftButton);
    mouse(&window, QEvent::MouseButtonRelease, first, Qt::LeftButton, Qt::NoButton);
    check(!market->isVisible(), "Dragging the floating window out and back must not open overview");
    window.setLoading(true);
    click(1);
    check(!market->isVisible(), "Loading must prevent overview activation");
    window.setLoading(false);
    cfg.marketBreadthEnabled = false;
    model.setConfig(cfg);
    window.applyConfig(cfg);
    window.toggleMarketBreadthDetailPopupFromTray();
    check(market->isVisible(), "The existing tray entry must remain usable when floating breadth counts are disabled");
    window.toggleMarketBreadthDetailPopupFromTray();
    window.hide();
    QApplication::setStartDragTime(oldDragTime);
}

void checkBreadthTimelineAfterClose() {
    QTcpServer turnoverProxy;
    check(turnoverProxy.listen(QHostAddress::LocalHost), "Could not start stalled turnover fixture");
    AppConfig cfg;
    cfg.proxyType = "http";
    cfg.proxyHost = "127.0.0.1";
    cfg.proxyPort = turnoverProxy.serverPort();
    const QDateTime open = QDateTime::fromString("2026-10-15T09:30:00+08:00", Qt::ISODate);
    QJsonArray timeline;
    for (int i = 0; i <= 240; ++i) {
        const int minutes = i >= 120 ? i + 90 : i;
        timeline.append(QJsonObject{{"timestampMs", open.addSecs(minutes * 60).toMSecsSinceEpoch()},
            {"riseCount", 430}, {"fallCount", 5108}, {"limitUpCount", 36}, {"limitDownCount", 26}});
    }
    breadthFixture = {{"upCount", 430}, {"downCount", 5108}, {"flatCount", 62},
        {"limitUpCount", 36}, {"limitDownCount", 26}, {"overviewTimeline", timeline},
        {"lastUpdatedAtMs", open.addSecs(330 * 60).toMSecsSinceEpoch()}};
    QVector<MarketBreadthSnapshot> snapshots;
    AshareMarketBreadthProvider provider;
    provider.applyConfig(cfg);
    QObject::connect(&provider, &AshareMarketBreadthProvider::dataReady, &provider,
        [&](const MarketBreadthSnapshot& snapshot) { snapshots.push_back(snapshot); });
    provider.fetch(true);
    events(50);
    check(!snapshots.isEmpty() && snapshots.first().overviewTimeline.size() == 241
        && snapshots.first().breadthValid && !snapshots.first().turnoverValid,
        "Cold startup must publish complete gotdx breadth history before turnover HTTP completes");
    const auto& points = snapshots.first().overviewTimeline;
    check(points.first().timestampMs == open.toMSecsSinceEpoch()
        && points.last().timestampMs == open.addSecs(330 * 60).toMSecsSinceEpoch()
        && points.last().limitUpCount == 36 && points.last().fallCount == 5108,
        "Provider must retain all four series and the China-time closing endpoint");
    MarketBreadthSnapshot forecastSnapshot;
    forecastSnapshot.turnover = 699413400000.0;
    forecastSnapshot.turnoverForecast = 1493278705303.773;
    check(qAbs(market_breadth::estimateFullDayTurnover(forecastSnapshot) - forecastSnapshot.turnoverForecast) < 0.5,
        "Turnover forecast must use the prediction supplied by the turnover endpoint");
    forecastSnapshot.turnoverForecast = qQNaN();
    check(qAbs(market_breadth::estimateFullDayTurnover(forecastSnapshot) - forecastSnapshot.turnover) < 0.5,
        "Turnover forecast must fall back to current turnover without a timeline or endpoint prediction");
    // Finish the unrelated HTTPS proxy request with an error and keep the valid trend.
    for (int i = 0; i < 20 && !turnoverProxy.hasPendingConnections(); ++i) events(10);
    check(turnoverProxy.hasPendingConnections(), "Turnover fixture must intercept the HTTP proxy request");
    auto* socket = turnoverProxy.nextPendingConnection();
    socket->write("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    socket->disconnectFromHost();
    for (int i = 0; i < 20 && snapshots.size() < 2; ++i) events(10);
    check(snapshots.size() >= 2 && snapshots.last().overviewTimeline.size() == 241,
        "Turnover failure must preserve the gotdx breadth history");
    breadthFixture = {};
}

class LimitPoolTabsTest {
public:
    static void run() {
        const int oldDragTime = QApplication::startDragTime();
        QApplication::setStartDragTime(120);
        QTcpServer proxy;
        check(proxy.listen(QHostAddress::LocalHost), "Could not create an isolated pool proxy");
        AppConfig cfg;
        cfg.proxyType = "http";
        cfg.proxyHost = "127.0.0.1";
        cfg.proxyPort = proxy.serverPort();
        cfg.upColor = QColor("#e14343");
        cfg.downColor = QColor("#25ba71");
        MarketBreadthDetailWindow market;
        market.applyConfig(cfg);
        market.setLanguage("zh_CN");
        market.setAttribute(Qt::WA_TransparentForMouseEvents, false);
        market.show();
        market.grab();
        check(market.m_breadthLimitPoolTabRect.isValid()
            && !market.m_breadthDistributionTabRect.intersects(market.m_breadthLimitPoolTabRect),
            "The combined pool must have one tab alongside trend and distribution");
        const auto mouse = [](QWidget* target, QEvent::Type type, QPoint pos,
                              Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(pos), QPointF(target->mapToGlobal(pos)), button, buttons, Qt::NoModifier);
            QApplication::sendEvent(target, &event);
        };
        const auto tabClick = [&](const QRect& rect) {
            const QPoint center = rect.center();
            mouse(&market, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
            mouse(&market, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
            market.grab();
        };
        const QDate tradeDate = QDate::currentDate();
        LimitPoolSnapshot up;
        up.tradeDate = tradeDate;
        up.stocks = {{"002134", "天津普林", 10.0158}, {"002889", "东方嘉盛", 10.0}, {"920002", "北交股票", 0.0}};
        for (int i = 3; i < 60; ++i) up.stocks.append(LimitPoolStock{"600519", "贵州茅台", i == 59 ? qQNaN() : 10.0});
        const LimitPoolSnapshot down {tradeDate, {{"002647", "仁东控股", -10.041}, {"600693", "东百集团", -10.0082}}};
        tabClick(market.m_breadthLimitPoolTabRect);
        check(market.m_breadthChartTabMode == MarketBreadthDetailWindow::BreadthChartTabMode::LimitPool
            && market.m_limitPoolLoading[0] && market.m_limitPoolLoading[1],
            "The merged tab must load both pools together");
        market.m_limitPoolProvider->dataReady(false, up);
        market.m_limitPoolProvider->dataReady(true, down);
        market.grab();
        auto* table = market.findChild<QTableWidget*>("limitPoolTable");
        check(table && table->isVisible() && table->columnCount() == 2 && table->rowCount() == 60,
            "The pool must render as one scrollable two-column table");
        check(!table->horizontalHeader()->isVisible(), "The pool table must not display a header row");
        for (int column = 0; column < 2; ++column) {
            const auto& stocks = column == 0 ? up.stocks : down.stocks;
            for (int row = 0; row < stocks.size(); ++row) {
                const auto* item = table->item(row, column);
                check(item && item->text() == stocks[row].name
                    && item->textAlignment() == (Qt::AlignLeft | Qt::AlignVCenter)
                    && item->foreground().color() == QColor(Qt::white)
                    && item->toolTip().isEmpty(),
                    "Names must remain left-aligned white text with no date tooltip");
                const double rate = stocks[row].changeRate;
                const QColor expectedColor = !std::isfinite(rate) ? cfg.textColor
                    : rate > 0 ? cfg.upColor : rate < 0 ? cfg.downColor : cfg.flatColor;
                check(item->data(Qt::UserRole + 2).value<QColor>() == expectedColor,
                    "The percentage color must follow its sign, independent of the pool column");
            }
        }
        check(!table->item(3, 1), "A shorter pool must leave the remaining cells empty");
        check(table->item(0, 0)->data(Qt::UserRole + 1).toString() == "+10.02%"
            && table->item(0, 1)->data(Qt::UserRole + 1).toString() == "-10.04%"
            && table->item(2, 0)->data(Qt::UserRole + 1).toString() == "0.00%"
            && table->item(59, 0)->data(Qt::UserRole + 1).toString() == "--",
            "Percentages must use two decimal places without scaling, preserving zero and missing values");
        check(table->toolTip().isEmpty() && table->horizontalHeaderItem(0)->toolTip().isEmpty()
            && table->horizontalHeaderItem(1)->toolTip().isEmpty(), "Date tooltips must be removed from the table and headers");
        const QImage rendered = table->viewport()->grab().toImage();
        const auto hasColor = [&](QRect rect, const QColor& color) {
            rect = rect.intersected(rendered.rect());
            for (int y = rect.top(); y <= rect.bottom(); ++y) {
                for (int x = rect.left(); x <= rect.right(); ++x) {
                    const QColor pixel = rendered.pixelColor(x, y);
                    if (qAbs(pixel.red() - color.red()) < 24 && qAbs(pixel.green() - color.green()) < 24
                        && qAbs(pixel.blue() - color.blue()) < 24) return true;
                }
            }
            return false;
        };
        for (int column = 0; column < 2; ++column) {
            QRect name = table->visualRect(table->model()->index(0, column));
            QRect rate = name;
            name.setRight(name.center().x());
            rate.setLeft(rate.center().x());
            check(hasColor(name, QColor(Qt::white)) && hasColor(rate, column == 0 ? cfg.upColor : cfg.downColor),
                "Rendered cells must place white names on the left and colored percentages on the right");
        }
        const auto cellPosition = [&](int row, int column) {
            return table->visualRect(table->model()->index(row, column)).center();
        };
        const auto cellClick = [&](int row, int column, Qt::MouseButton button) {
            const QPoint pos = cellPosition(row, column);
            mouse(table->viewport(), QEvent::MouseButtonPress, pos, button, button);
            mouse(table->viewport(), QEvent::MouseButtonRelease, pos, button, Qt::NoButton);
            events();
        };
        cellClick(0, 0, Qt::LeftButton);
        QWidget* popup = nullptr;
        for (auto* top : QApplication::topLevelWidgets()) {
            if (top->isVisible() && top->findChild<QWidget*>("dailyKlineChart")) popup = top;
        }
        check(popup && !popup->findChild<QWidget*>("dailyKlineChart")->isVisible()
            && lastTimelinePayload.value("code").toString() == "0.002134",
            "Single-clicking a limit-up stock must open its intraday chart");
        cellClick(0, 1, Qt::LeftButton);
        check(lastTimelinePayload.value("code").toString() == "0.002647",
            "Single-clicking a limit-down stock must open its intraday chart");
        cellClick(0, 0, Qt::RightButton);
        check(popup->findChild<QWidget*>("dailyKlineChart")->isVisible()
            && lastKlinePayload.value("code").toString() == "0.002134"
            && lastKlinePayload.value("period").toInt() == 4,
            "Right-clicking a limit-up stock must open its daily candles");
        cellClick(1, 1, Qt::RightButton);
        check(lastKlinePayload.value("code").toString() == "1.600693",
            "Right-clicking another pool/row must select that exact stock");
        events(80);
        check(popup->findChild<QWidget*>("dailyKlineChart")->isVisible(),
            "Pool daily candles must remain visible without holding Ctrl");
        cellClick(2, 0, Qt::LeftButton);
        check(!popup->findChild<QWidget*>("dailyKlineChart")->isVisible()
            && lastTimelinePayload.value("code").toString() == "tdx:main:2:920002",
            "Beijing stocks must map to the correct market and switch daily back to intraday");
        const int beforeMisclick = timelineRequests + klineRequests;
        cellClick(3, 1, Qt::LeftButton);
        cellClick(3, 1, Qt::RightButton);
        const QPoint first = cellPosition(0, 0), second = cellPosition(0, 1);
        mouse(table->viewport(), QEvent::MouseButtonPress, first, Qt::RightButton, Qt::RightButton);
        mouse(table->viewport(), QEvent::MouseButtonRelease, second, Qt::RightButton, Qt::NoButton);
        mouse(table->viewport(), QEvent::MouseButtonPress, first, Qt::LeftButton, Qt::LeftButton);
        mouse(table->viewport(), QEvent::MouseMove, first + QPoint(40, 0), Qt::NoButton, Qt::LeftButton);
        mouse(table->viewport(), QEvent::MouseMove, first, Qt::NoButton, Qt::LeftButton);
        mouse(table->viewport(), QEvent::MouseButtonRelease, first, Qt::LeftButton, Qt::NoButton);
        mouse(table->viewport(), QEvent::MouseButtonPress, first, Qt::RightButton, Qt::RightButton);
        events(150);
        mouse(table->viewport(), QEvent::MouseButtonRelease, first, Qt::RightButton, Qt::NoButton);
        check(timelineRequests + klineRequests == beforeMisclick,
            "Blank cells, releases on another cell, drags and long presses must not open charts");
        mouse(table->viewport(), QEvent::MouseButtonPress, first, Qt::RightButton, Qt::RightButton);
        up.stocks[0] = {"600036", "招商银行", 10.0};
        market.m_limitPoolProvider->dataReady(false, up);
        mouse(table->viewport(), QEvent::MouseButtonRelease, first, Qt::RightButton, Qt::NoButton);
        check(timelineRequests + klineRequests == beforeMisclick,
            "An async refresh replacing the pressed stock must cancel the chart click");
        check(table->verticalScrollBar()->maximum() > 0, "Long pools must have a usable vertical scrollbar");
        const QPoint wheelPosition = table->viewport()->rect().center();
        QWheelEvent wheel(QPointF(wheelPosition), QPointF(table->viewport()->mapToGlobal(wheelPosition)),
            QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(table->viewport(), &wheel);
        const int scroll = table->verticalScrollBar()->value();
        check(scroll > 0 && !popup->isVisible(), "Scrolling must move both columns and dismiss the old chart anchor");
        tabClick(market.m_breadthTrendTabRect);
        check(!table->isVisible(), "The trend tab must hide the pool table");
        tabClick(market.m_breadthLimitPoolTabRect);
        check(table->isVisible() && table->verticalScrollBar()->value() == scroll,
            "Switching breadth tabs must preserve the pool's scroll position");
        table->verticalScrollBar()->setValue(0);
        const QString screenshot = qEnvironmentVariable("MYSTOCKS_LIMIT_POOL_SCREENSHOT");
        if (!screenshot.isEmpty()) check(market.grab().save(screenshot), "Could not save pool preview");
        up.stocks[0].changeRate = -2.5;
        market.m_limitPoolProvider->dataReady(false, up);
        check(table->item(0, 0)->data(Qt::UserRole + 1).toString() == "-2.50%"
            && table->item(0, 0)->data(Qt::UserRole + 2).value<QColor>() == cfg.downColor
            && table->item(0, 0)->foreground().color() == QColor(Qt::white),
            "A negative percentage in the up pool must use the down color while its name stays white");
        auto positiveDown = down;
        positiveDown.stocks[0].changeRate = 1.25;
        market.m_limitPoolProvider->dataReady(true, positiveDown);
        check(table->item(0, 1)->data(Qt::UserRole + 1).toString() == "+1.25%"
            && table->item(0, 1)->data(Qt::UserRole + 2).value<QColor>() == cfg.upColor,
            "A positive percentage in the down pool must use the up color");
        market.m_limitPoolProvider->dataReady(false, {tradeDate, {}});
        check(table->rowCount() == 2 && table->item(0, 0)->data(Qt::UserRole).toString().isEmpty()
            && table->item(0, 1)->text() == "仁东控股", "An empty pool must clear its names without removing the other pool");
        market.m_limitPoolProvider->error(true, "fixture error");
        check(table->rowCount() == 1 && table->item(0, 1)->data(Qt::UserRole).toString().isEmpty(),
            "Failed pool requests must not leave clickable stale names");
        tabClick(market.m_breadthDistributionTabRect);
        check(!table->isVisible()
            && market.m_breadthChartTabMode == MarketBreadthDetailWindow::BreadthChartTabMode::Distribution,
            "Existing distribution chart must remain available");
        market.hide();
        QApplication::setStartDragTime(oldDragTime);
    }
};

class MarketBreadthConstituentInteractionTest {
public:
    static void run() {
        constituentFixture = true;
        const int oldDragTime = QApplication::startDragTime();
        QApplication::setStartDragTime(120);
        QWidget owner;
        MarketBreadthDetailWindow market(&owner);
        AppConfig cfg;
        cfg.language = "zh_CN";
        market.applyConfig(cfg);
        market.setLanguage("zh_CN");
        int watchlistMutations = 0;
        market.setHotRankDetailWatchlistCallbacks([](const QString&) { return false; },
            [&](const QString&, const QString&, bool) { ++watchlistMutations; return true; }, []() {});
        const auto mouse = [](QWidget* target, QEvent::Type type, QPoint pos,
                              Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, QPointF(pos), QPointF(target->mapToGlobal(pos)), button, buttons, Qt::NoModifier);
            QApplication::sendEvent(target, &event);
        };
        const auto click = [&](QWidget* target, QPoint pos, Qt::MouseButton button) {
            mouse(target, QEvent::MouseButtonPress, pos, button, button);
            mouse(target, QEvent::MouseButtonRelease, pos, button, Qt::NoButton);
            events();
        };
        const auto chartPopup = []() -> QWidget* {
            for (auto* top : QApplication::topLevelWidgets()) {
                if (top->isVisible() && top->findChild<QWidget*>("dailyKlineChart")) return top;
            }
            return nullptr;
        };
        for (const QString& category : {QStringLiteral("热门概念"), QStringLiteral("热门板块")}) {
            HotRankItem item;
            item.code = category == QStringLiteral("热门概念") ? "tdx:main:1:885000" : "tdx:main:1:881319";
            item.detailFs = item.code;
            item.name = category;
            const int beforeMembers = memberRequests;
            market.openHotRankDetail(item);
            events(20);
            auto* detail = owner.findChild<QWidget*>("hotRankConstituentDetailWindow");
            check(detail && detail->isVisible() && memberRequests == beforeMembers + 1,
                "Opening a concept/sector must load its shared constituent popup");
            detail->grab(); // Ensure the custom-painted row hit areas are current.
            const QPoint firstRow(50, 105), secondRow(50, 131);
            click(detail, firstRow, Qt::RightButton);
            auto* popup = chartPopup();
            check(popup && popup->findChild<QWidget*>("dailyKlineChart")->isVisible()
                && lastKlinePayload.value("code").toString() == "1.600519"
                && lastKlinePayload.value("period").toInt() == 4,
                "Right-clicking a concept/sector constituent must show that stock's daily candles");
            events(80);
            check(popup->findChild<QWidget*>("dailyKlineChart")->isVisible(),
                "Constituent daily mode must stay visible without holding Ctrl");
            click(detail, secondRow, Qt::RightButton);
            check(lastKlinePayload.value("code").toString() == "0.000001",
                "Right-clicking another constituent must load the correct stock");
            const int beforeIntraday = timelineRequests;
            click(detail, firstRow, Qt::LeftButton);
            check(popup->isVisible() && !popup->findChild<QWidget*>("dailyKlineChart")->isVisible(),
                "Existing left-click constituent intraday behavior must remain available");
            if (timelineRequests > beforeIntraday) {
                check(lastTimelinePayload.value("code").toString() == "1.600519",
                    "Uncached left-click intraday requests must select the correct constituent");
            }
            const int beforeWatchlist = watchlistMutations;
            click(detail, QPoint(detail->width() - 50, 105), Qt::LeftButton);
            check(watchlistMutations == beforeWatchlist + 1,
                "The existing left-click watchlist button must still work");
            const int beforeMisclick = klineRequests;
            mouse(detail, QEvent::MouseButtonPress, firstRow, Qt::RightButton, Qt::RightButton);
            mouse(detail, QEvent::MouseButtonRelease, secondRow, Qt::RightButton, Qt::NoButton);
            check(klineRequests == beforeMisclick, "Right release on another row must not open a chart");
            mouse(detail, QEvent::MouseButtonPress, firstRow, Qt::RightButton, Qt::RightButton);
            mouse(detail, QEvent::MouseMove, firstRow + QPoint(40, 0), Qt::NoButton, Qt::RightButton);
            mouse(detail, QEvent::MouseMove, firstRow, Qt::NoButton, Qt::RightButton);
            mouse(detail, QEvent::MouseButtonRelease, firstRow, Qt::RightButton, Qt::NoButton);
            check(klineRequests == beforeMisclick, "Dragging out and back must not become a right-click chart");
            mouse(detail, QEvent::MouseButtonPress, firstRow, Qt::RightButton, Qt::RightButton);
            events(150);
            mouse(detail, QEvent::MouseButtonRelease, firstRow, Qt::RightButton, Qt::NoButton);
            check(klineRequests == beforeMisclick, "A long right press must not trigger a chart");
            click(detail, QPoint(detail->width() / 2, 72), Qt::LeftButton);
            check(!popup->isVisible() && memberRequests == beforeMembers + 1,
                "Existing numeric-header sorting must remain local and dismiss the chart");
            detail->hide();
        }
        market.m_indexQuotes = {{"1.000001", "Shanghai", 3000, 10, 0.3},
                                {"0.399001", "Shenzhen", 10000, 20, 0.2}};
        market.setAttribute(Qt::WA_TransparentForMouseEvents, false);
        market.show();
        events();
        market.grab();
        check(market.m_indexCellHitAreas.size() >= 2, "Market panel must paint index chart targets");
        const QPoint firstIndex = market.m_indexCellHitAreas.at(0).rect.center();
        const QPoint secondIndex = market.m_indexCellHitAreas.at(1).rect.center();
        click(&market, firstIndex, Qt::RightButton);
        auto* indexPopup = chartPopup();
        check(indexPopup && indexPopup->findChild<QWidget*>("dailyKlineChart")->isVisible()
            && lastKlinePayload.value("code").toString() == market.m_indexCellHitAreas.at(0).code
            && lastKlinePayload.value("period").toInt() == 4,
            "Right-clicking a market index must show its daily candles");
        click(&market, secondIndex, Qt::RightButton);
        check(lastKlinePayload.value("code").toString() == market.m_indexCellHitAreas.at(1).code,
            "Right-clicking another index must select the correct instrument");
        click(&market, firstIndex, Qt::LeftButton);
        check(!indexPopup->findChild<QWidget*>("dailyKlineChart")->isVisible(),
            "Left-clicking a market index must retain intraday mode");
        const int beforeIndexMisclick = klineRequests;
        mouse(&market, QEvent::MouseButtonPress, firstIndex, Qt::RightButton, Qt::RightButton);
        mouse(&market, QEvent::MouseButtonRelease, secondIndex, Qt::RightButton, Qt::NoButton);
        check(klineRequests == beforeIndexMisclick, "Releasing on a different index must not open daily candles");

        HotRankItem firstHeat, secondHeat;
        firstHeat.code = "600519";
        firstHeat.watchCode = "1.600519";
        firstHeat.name = "Heat A";
        secondHeat.code = "0.000001"; // Fall back to code when watchCode is absent.
        secondHeat.name = "Heat B";
        market.m_hotHourStocks = {firstHeat, secondHeat};
        market.m_hotDayStocks = {secondHeat, firstHeat};
        for (const auto mode : {MarketBreadthDetailWindow::HotRankTabMode::HourHeat,
                                MarketBreadthDetailWindow::HotRankTabMode::DayHeat}) {
            market.m_hotRankTabMode = mode;
            market.grab();
            check(market.m_hotRankRowHitAreas.size() == 2, "Heat tab must paint both stock cards");
            const auto& firstArea = market.m_hotRankRowHitAreas.at(0);
            const QString expectedCode = firstArea.item.watchCode.isEmpty() ? firstArea.item.code : firstArea.item.watchCode;
            const QPoint first = firstArea.rect.center();
            const QPoint second = market.m_hotRankRowHitAreas.at(1).rect.center();
            click(&market, first, Qt::RightButton);
            auto* popup = chartPopup();
            check(popup && popup->findChild<QWidget*>("dailyKlineChart")->isVisible()
                && lastKlinePayload.value("code").toString() == expectedCode,
                "Both heat tabs must open daily candles for the right-clicked stock");
            events(80);
            check(popup->findChild<QWidget*>("dailyKlineChart")->isVisible(),
                "Heat daily candles must remain visible without holding Ctrl");
            click(&market, first, Qt::LeftButton);
            check(!popup->findChild<QWidget*>("dailyKlineChart")->isVisible(),
                "Left-clicking a heat card must retain intraday mode");
            const int beforeMisclick = klineRequests;
            mouse(&market, QEvent::MouseButtonPress, first, Qt::RightButton, Qt::RightButton);
            mouse(&market, QEvent::MouseButtonRelease, second, Qt::RightButton, Qt::NoButton);
            mouse(&market, QEvent::MouseButtonPress, first, Qt::RightButton, Qt::RightButton);
            mouse(&market, QEvent::MouseMove, first + QPoint(40, 0), Qt::NoButton, Qt::RightButton);
            mouse(&market, QEvent::MouseMove, first, Qt::NoButton, Qt::RightButton);
            mouse(&market, QEvent::MouseButtonRelease, first, Qt::RightButton, Qt::NoButton);
            mouse(&market, QEvent::MouseButtonPress, first, Qt::RightButton, Qt::RightButton);
            events(150);
            mouse(&market, QEvent::MouseButtonRelease, first, Qt::RightButton, Qt::NoButton);
            check(klineRequests == beforeMisclick, "Heat mismatched releases, drags and long presses must not open daily candles");
            const int beforeWatchlist = watchlistMutations;
            const int actionIndex = mode == MarketBreadthDetailWindow::HotRankTabMode::HourHeat ? 0 : 1;
            click(&market, market.m_hotStockActionButtonRects.at(actionIndex).center(), Qt::LeftButton);
            check(watchlistMutations == beforeWatchlist + 1, "Heat watchlist actions must remain available");
        }
        market.hide();
        QApplication::setStartDragTime(oldDragTime);
        constituentFixture = false;
    }
};

class AppControllerStartupTest {
public:
    static void run() {
        const QString testRoot = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath("myStocks-test-data");
        const QString yaml = QDir(testRoot).filePath("data.yaml");
        check(ConfigManager::saveDataYamlText(yaml,
            "ver: 1\nstocks:\n  - code: 1.600519\n    name: Disabled\n  - code: tdx:main:0:000001\n    name: Active\n"),
            "Could not create isolated watchlist");
        AppConfig cfg;
        cfg.startupShowFloatingWindow = false;
        cfg.autoCheckUpdates = false;
        cfg.marketBreadthEnabled = false;
        cfg.pollMs = 4800;
        cfg.tdxMainAutoSelect = false;
        cfg.tdxMainServer = "old.test:7709";
        cfg.tdxMainServerPool = {"old.test:7709", "main.test:7709"};
        cfg.tdxExServerPool = {"ex.test:7727"};
        ConfigManager::saveConfig(cfg);
        {
            auto settings = ConfigManager::createAppSettings();
            settings->setValue(app_constants::kWatchIndexesKey, QStringList{"1.000001\tLegacy index"});
        }
        {
            SettingsDialog dialog(cfg, {{"1.600519", "Stock"}, {"1.510300", "ETF"}},
                {{"1.000001", "Legacy index"}}, {}, yaml, {});
            auto* hoverDelay = dialog.findChild<QSpinBox*>("timelineChartHoverDelaySpin");
            check(hoverDelay && hoverDelay->value() == 500, "Hover delay editor must default to 500ms");
            hoverDelay->setValue(800);
            check(dialog.config().timelineChartHoverDelayMs == 800, "Hover delay editor must update configuration");
            auto* dailyDays = dialog.findChild<QSpinBox*>("dailyKlineDaysSpin");
            check(dailyDays && dailyDays->value() == 60, "Daily range setting must default to 60");
            dailyDays->setValue(120);
            check(dialog.config().dailyKlineDays == 120, "Daily range editor must update configuration");
            for (int period : {5, 10, 20, 30, 60, 120, 250}) {
                auto* ma = dialog.findChild<QCheckBox*>(QStringLiteral("dailyKlineMA%1Check").arg(period));
                check(ma && ma->isChecked() == (period <= 30), "MA editor defaults are incorrect");
                ma->setChecked(period == 250);
            }
            check(dialog.config().dailyKlineMAPeriods == QVector<int>({250}), "MA choices must update configuration");
            auto* adapters = dialog.findChild<QComboBox*>("networkInterfaceCombo");
            check(adapters && adapters->currentData().toString().isEmpty(), "Network interface selection must default to automatic");
            const auto indexes = dialog.selectedIndexes();
            check(indexes.size() == 1 && indexes.first().code == "1.000001", "Index editor must retain compatible legacy index selection and format");
            cfg.networkInterface = "mystocks-missing-adapter";
        }
        {
            SettingsDialog dialog(cfg, {}, {}, {}, yaml, {});
            auto* adapters = dialog.findChild<QComboBox*>("networkInterfaceCombo");
            check(adapters && adapters->currentData().toString() == cfg.networkInterface,
                "A missing saved interface must stay selected rather than reverting to automatic");
            check(dialog.config().networkInterface == cfg.networkInterface, "Network draft must preserve the selected adapter");
        }
        cfg.networkInterface.clear();
        {
            SettingsDialog dialog(cfg, {}, {}, {}, yaml, {});
            auto* edit = dialog.findChild<QLineEdit*>("stockSearchEdit");
            auto* button = dialog.findChild<QPushButton*>("stockSearchButton");
            auto* status = dialog.findChild<QLabel*>("stockSearchStatus");
            auto* results = dialog.findChild<QListWidget*>("stockSearchResults");
            check(edit && button && status && results, "Search controls must be available");
            edit->setText("600519");
            button->click();
            check(searches.size() == 1 && !status->isHidden() && !status->text().isEmpty(),
                "Search must show directory loading progress");
            auto pending = searches.takeFirst();
            pending.callback({}, "Directory unavailable");
            check(!status->isHidden() && status->text().contains("Directory unavailable") && results->isHidden(),
                "Search failures must be visible without hovering");
            button->click();
            pending = searches.takeFirst();
            pending.callback({{"items", QJsonArray{QJsonObject{{"code", "1.600519"}, {"name", "贵州茅台"}}}},
                {"warning", "Extended market unavailable"}}, {});
            check(results->count() == 1 && !results->isHidden()
                && results->item(0)->data(Qt::UserRole).toString() == "1.600519"
                && status->text().contains("Extended market unavailable"),
                "Partial catalog must display both available results and a warning");
            button->click();
            pending = searches.takeFirst();
            pending.callback({{"items", QJsonArray()}}, {});
            check(results->isHidden() && !status->isHidden() && !status->text().isEmpty(),
                "An empty search must have a visible no-results message");
            edit->setText("腾讯");
            button->click();
            edit->setText("茅台");
            pending = searches.takeFirst();
            pending.callback({{"items", QJsonArray{QJsonObject{{"code", "tdx:ex:31:00700"}, {"name", "腾讯控股"}}}}}, {});
            check(searches.size() == 1 && searches.first().keyword == "茅台" && results->count() == 0,
                "A stale response must request the latest keyword instead of displaying old results");
            pending = searches.takeFirst();
            pending.callback({{"items", QJsonArray{QJsonObject{{"code", "1.600519"}, {"name", "贵州茅台"}}}}}, {});
            check(results->count() == 1 && results->item(0)->data(Qt::UserRole).toString() == "1.600519"
                && status->isHidden(), "Successful search must show matching results and clear status");
            // Exercise the actual Return key path, including QDialog default-button handling.
            auto* tabs = dialog.findChild<QTabWidget*>();
            for (int i = 0; tabs && i < tabs->count(); ++i) {
                if (tabs->widget(i)->isAncestorOf(edit)) tabs->setCurrentIndex(i);
            }
            bool finished = false;
            QObject::connect(&dialog, &QDialog::finished, &dialog, [&finished](int) { finished = true; });
            dialog.show();
            edit->setFocus();
            events();
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(edit, &enter);
            check(searches.size() == 1 && !finished,
                "Return in the search field must search once and keep settings open");
            pending = searches.takeFirst();
            pending.callback({{"items", QJsonArray()}}, {});
        }
        {
            AppController controller;
            events();
            check(controller.m_loading && controller.m_splash->isVisible(), "Cold startup must show splash");
            check(!controller.m_initialized && !controller.m_model && !controller.m_window,
                "Full initialization must wait for confirmed routes");
            check(controller.m_cfg.pollMs != cfg.pollMs, "Full configuration loaded before route confirmation");
            check(quoteRequests == 0 && routes.size() == 1 && routes.first().kind == "main",
                "Cold startup must probe main market before requesting quotations");
            finishRoute(false, true);
            check(!controller.m_initialized && routes.first().kind == "ex", "Extended route must also finish before config loading");
            finishRoute();
            check(controller.m_initialized && controller.m_cfg.pollMs == cfg.pollMs,
                "Confirmed routes must be followed by full configuration loading");
            check(controller.m_cfg.tdxMainServer == "main.test:7709" && !controller.m_cfg.tdxMainAutoSelect,
                "Manual fallback must persist the replacement without changing selection mode");
            check(!controller.m_window->isVisible() && controller.m_splash->isVisible(),
                "Hidden startup window must remain hidden while splash is active");
            check(controller.m_model->rowCount() == 3, "Compatible legacy stock rows must load during cold startup");
            check(controller.m_indexes.size() == 1 && controller.m_indexes.first().code == "1.000001", "Compatible legacy indexes must be loaded");
            controller.saveExtraWatchItems();
            {
                auto settings = ConfigManager::createAppSettings();
                check(settings->value(app_constants::kWatchIndexesKey).toStringList().contains("1.000001\tLegacy index"),
                    "Saving index edits must retain compatible legacy indexes");
            }
            {
                auto settings = ConfigManager::createAppSettings();
                const auto originalIndexes = settings->value(app_constants::kWatchIndexesKey);
                settings->setValue(app_constants::kWatchIndexesKey, QStringList{
                    "1.000001\tLegacy index", "tdx:main:1:000688\t科创50",
                    "1.000688\t科创50", "tdx:main:1:000680\t科创综指", "90.BK0420\tLegacy board"});
                controller.loadExtraWatchItems();
                check(controller.m_indexes.size() == 2 && controller.m_indexes[0].code == "1.000001"
                    && controller.m_indexes[1].code == "1.000680" && controller.m_indexes[1].name == "科创综指",
                    "Saved STAR 50 selections must migrate in place and deduplicate STAR Composite formats");
                const auto migrated = settings->value(app_constants::kWatchIndexesKey).toStringList();
                check(migrated.contains("1.000680\t科创综指") && migrated.contains("90.BK0420\tLegacy board")
                    && !migrated.join("|").contains("000688"),
                    "Index replacement must persist while preserving unsupported saved selections");
                settings->remove(app_constants::kWatchIndexesKey);
                controller.loadExtraWatchItems();
                check(controller.m_indexes.size() == 4 && controller.m_indexes[2].code == "1.000680"
                    && controller.m_indexes[2].name == "科创综指", "Default top indexes must use STAR Composite");
                settings->setValue(app_constants::kWatchIndexesKey, originalIndexes);
                controller.loadExtraWatchItems();
            }
            events(static_cast<int>(app_constants::kStartupSplashMinimumDisplayMs + 200));
            check(!controller.m_loading && !controller.m_splash->isVisible() && quoteRequests > 0,
                "Completed startup must close splash and start quotations");

            controller.m_window->move(120, 100);
            controller.m_window->show();
            events();
            const QRect position = controller.m_window->geometry();
            const int beforeReload = quoteRequests;
            controller.reloadStocksFromYaml();
            events();
            check(controller.m_splash->isVisible() && !controller.m_timer->isActive(),
                "Reload must display splash and suspend polling");
            check(controller.m_splash->geometry() == StartupSplash::placement(controller.m_splash->size(),
                QGuiApplication::primaryScreen()->availableGeometry()),
                "Reload splash must use the screen corner instead of covering the floating window");
            check(controller.m_window->isVisible() && !controller.m_window->isEnabled(),
                "Reload must retain the visible floating window underneath splash");
            check(quoteRequests == beforeReload, "Reload must not request quotations before route confirmation");
            finishRoute();
            finishRoute();
            events(static_cast<int>(app_constants::kStartupSplashMinimumDisplayMs + 200));
            check(controller.m_window->isVisible() && controller.m_window->geometry() == position,
                "Reload must preserve floating-window visibility and position");
            check(controller.m_window->isEnabled() && !controller.m_splash->isVisible(),
                "Reload completion must restore floating-window interaction");

            controller.m_window->hide();
            controller.reloadStocksFromYaml();
            events();
            finishRoute(true);
            finishRoute(true);
            check(controller.m_loading && controller.m_splash->isVisible() && !controller.m_timer->isActive(),
                "Unusable routes must retain an actionable splash and pause polling");
            check(!controller.m_window->isVisible(), "Failure must not show a hidden floating window");
            controller.loadAfterRoutes();
            events(static_cast<int>(app_constants::kStartupSplashMinimumDisplayMs + 200));
            check(!controller.m_loading && !controller.m_routesReady && !controller.m_timer->isActive(),
                "Offline load must finish local configuration without starting network polling");
            const int beforeOffline = quoteRequests;
            controller.refreshQuotes(true);
            check(quoteRequests == beforeOffline, "Offline mode must not issue forced quotation requests");
            controller.reloadStocksFromYaml();
            events();
            finishRoute();
            finishRoute();
            events(static_cast<int>(app_constants::kStartupSplashMinimumDisplayMs + 200));
            check(controller.m_routesReady && controller.m_timer->isActive(), "Reload must recover from offline mode");
        }
        {
            AppController controller;
            events();
            finishRoute(true);
            finishRoute(true);
            check(!controller.m_initialized && !controller.m_model,
                "Cold-start route failure must not prematurely load full configuration");
            for (auto* button : controller.m_splash->findChildren<QPushButton*>()) {
                if (button->text() == "重新检测") button->click();
            }
            events();
            check(routes.size() == 1, "Retry button must begin a fresh probe rather than queue behind failure");
            finishRoute();
            finishRoute();
            events(static_cast<int>(app_constants::kStartupSplashMinimumDisplayMs + 200));
            check(controller.m_initialized && controller.m_routesReady && !controller.m_loading,
                "Cold startup must recover after a retry");
        }
        {
            AppController controller;
            events();
            controller.reloadStocksFromYaml();
            controller.reloadStocksFromYaml();
            finishRoute();
            finishRoute();
            events(static_cast<int>(app_constants::kStartupSplashMinimumDisplayMs + 200));
            check(controller.m_initialized && controller.m_loading && routes.size() == 1,
                "Multiple reloads during loading must coalesce into one subsequent reload");
            finishRoute();
            finishRoute();
            events(static_cast<int>(app_constants::kStartupSplashMinimumDisplayMs + 200));
            check(!controller.m_loading && routes.isEmpty(), "Coalesced reload must complete without duplicate route callbacks");
        }
        // Test-mode paths are unique to this process and never touch user data.
        QDir(testRoot).removeRecursively();
        QFile::remove(ConfigManager::appSettingsFilePath());
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("myStocksTests");
    QCoreApplication::setApplicationName("startupTest-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (app.arguments().contains("--stock-chart-clicks")) {
        checkStockChartClicks();
        return 0;
    }
    if (app.arguments().contains("--limit-pool-tabs")) {
        LimitPoolTabsTest::run();
        return 0;
    }
    if (app.arguments().contains("--breadth-header-click")) {
        checkBreadthHeaderClick();
        return 0;
    }
    if (app.arguments().contains("--breadth-timeline")) {
        checkBreadthTimelineAfterClose();
        return 0;
    }
    if (app.arguments().contains("--constituent-chart-clicks")) {
        MarketBreadthConstituentInteractionTest::run();
        return 0;
    }
    AppControllerStartupTest::run();
    return 0;
}
