#include "daily_kline_chart.h"
#include "tdx_bridge.h"
#include "daily_kline_indicators.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QPainter>
#include <QWheelEvent>

class TimelineTestWidget : public QWidget {
public:
    AppConfig chartConfig;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), chartConfig.timelineChartBgColor);
        if (property("dailyKlineVisible").toBool()) return;
        // Stand-in for timeline contents, which must not bleed through daily mode.
        painter.fillRect(QRect(290, 150, 20, 20), Qt::magenta);
    }
};

struct Pending { QJsonObject payload; tdx_bridge::Callback callback; };
QVector<Pending> pending;
namespace tdx_bridge {
void request(QObject*, const AppConfig&, QJsonObject payload, Callback callback) {
    pending.append({payload, std::move(callback)});
}
}
void check(bool condition, const char* message) { if (!condition) qFatal("%s", message); }
void control(QWidget& widget, bool down, bool repeat = false) {
#if defined(Q_OS_MACOS)
    const int physicalKey = Qt::Key_Meta;
    const auto modifier = Qt::MetaModifier;
#else
    const int physicalKey = Qt::Key_Control;
    const auto modifier = Qt::ControlModifier;
#endif
    QKeyEvent key(down ? QEvent::KeyPress : QEvent::KeyRelease, physicalKey,
        down ? modifier : Qt::NoModifier, {}, repeat);
    QApplication::sendEvent(&widget, &key);
}
QJsonObject response() {
    return {{"bars", QJsonArray{QJsonObject{{"time", "2026-10-14 00:00:00"}, {"open", 4.25}, {"high", 4.30},
        {"low", 4.20}, {"close", 4.28}, {"volume", 1234567}},
        QJsonObject{{"time", "2026-10-15 00:00:00"}, {"open", 4.27}, {"high", 4.29},
        {"low", 4.18}, {"close", 4.20}, {"volume", 2345678}}}}};
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    TimelineTestWidget timeline;
    timeline.resize(560, 360);
    auto* daily = new DailyKlineChart(&timeline);
    AppConfig cfg;
    cfg.language = "zh_CN";
    daily->setContext(cfg, "1.510300", "沪深300ETF");
    timeline.show();
    check(daily->isHidden() && pending.isEmpty(), "No Ctrl: timeline must remain visible without daily requests");
#if defined(Q_OS_MACOS)
    QKeyEvent command(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    QApplication::sendEvent(&timeline, &command);
    check(daily->isHidden() && pending.isEmpty(), "macOS Command must not activate physical Ctrl mode");
#endif
    control(timeline, true);
    check(daily->isVisible() && pending.size() == 1, "Ctrl must show daily candles");
    check(pending.first().payload.value("count").toInt() == daily_kline::kMaxHistoryCount
        && pending.first().payload.value("period").toInt() == 4, "Daily period and full history depth must reach bridge");
    control(timeline, true, true);
    check(pending.size() == 1, "Auto-repeat must not create duplicate requests");
    control(timeline, false);
    check(daily->isHidden(), "Releasing Ctrl while loading must immediately return to timeline");
    pending.takeFirst().callback(response(), {});
    check(daily->isHidden() && daily->property("barCount").toInt() == 2,
        "Late reply may cache daily data but must not restore daily mode");
    control(timeline, true);
    check(daily->isVisible() && pending.isEmpty(), "Holding Ctrl again must reuse loaded bars");
    daily->setPinnedPeriod(KlinePeriod::Weekly);
    check(daily->isVisible() && pending.size() == 1
        && pending.first().payload.value("period").toInt() == 5,
        "Weekly mode must request the TongdaXin weekly period");
    pending.takeFirst().callback(response(), {});
    daily->setPinnedPeriod(KlinePeriod::Monthly);
    check(daily->isVisible() && pending.size() == 1
        && pending.first().payload.value("period").toInt() == 6,
        "Monthly mode must request the TongdaXin monthly period");
    pending.takeFirst().callback(response(), {});
    daily->setPinnedPeriod(KlinePeriod::Daily);
    check(daily->isVisible() && pending.size() == 1
        && pending.first().payload.value("period").toInt() == 4,
        "Switching back to daily mode must request daily candles");
    pending.takeFirst().callback(response(), {});
    daily->setPinnedDaily(true);
    control(timeline, false);
    check(daily->isVisible(), "Right-click daily mode must stay visible after Ctrl is released");
    daily->setPinnedDaily(false);
    check(daily->isHidden(), "A left-click mode change must return to intraday without holding Ctrl");
    control(timeline, true);
    timeline.resize(640, 420);
    check(daily->size() == timeline.size(), "Daily overlay must follow popup resize");
    daily->setContext(cfg, "1.510300", "沪深300ETF");
    check(pending.isEmpty(), "Repeated hover updates must not invalidate daily cache");
    cfg.dailyKlineDays = 120;
    daily->setContext(cfg, "1.510300", "沪深300ETF");
    check(pending.size() == 1 && pending.first().payload.value("count").toInt() == daily_kline::kMaxHistoryCount,
        "Changed range must reload the full history depth");
    auto stale = pending.takeFirst();
    daily->setContext(cfg, "1.600519", "贵州茅台");
    check(pending.size() == 1, "Switching stocks while Ctrl is held must load the new stock");
    stale.callback(response(), {});
    check(daily->property("barCount").toInt() == 0, "Old symbol response must not overwrite current chart");
    pending.takeFirst().callback({}, "test failure");
    check(daily->isVisible(), "Daily errors must preserve Ctrl mode");
    control(timeline, false);
    check(daily->isHidden(), "Ctrl release must recover timeline after an error");
    control(timeline, true);
    timeline.hide();
    check(daily->isHidden(), "Hiding popup must reset daily mode");
    timeline.show();
    check(daily->isHidden(), "Reopened popup must not retain released Ctrl state");
    struct Sample { double close, high, low; };
    QVector<Sample> samples;
    for (int i = 1; i <= 309; ++i) samples.append({double(i), double(i + 1), double(i) - 0.5});
    const auto ma250 = daily_kline::movingAverage(samples, 250);
    check(!std::isfinite(ma250[248]) && std::abs(ma250[249] - 125.5) < 1e-8
        && std::abs(ma250.last() - 184.5) < 1e-8,
        "MA250 must use 250 closes including hidden warm-up history");
    samples[0].high = 10000;
    samples[0].low = 0.01;
    const auto extrema = daily_kline::visibleExtrema(samples, 249);
    check(extrema.high == 310 && extrema.low == 249.5 && extrema.highIndex == 308 && extrema.lowIndex == 249,
        "Visible high/low must exclude hidden history and use candle wicks, not closes");
    // Text layout, font padding and edge handling must never move the arrow
    // horizontally off the first, middle or last candle's fractional center.
    const QRectF markerPlot(70, 48, 469, 200);
    for (double candleX : {73.908333333, 304.5, 535.091666667}) {
        const QPointF anchor(candleX, 100);
        for (double width : {30.0, 80.0, 600.0}) {
            const auto marker = daily_kline::priceMarkerLayout(markerPlot, anchor, width);
            check(marker.tip.x() == candleX && marker.tip.y() == anchor.y(),
                "Arrow tip must use candle coordinates rather than a centered text rectangle");
            check(marker.label.left() >= markerPlot.left() && marker.label.right() <= markerPlot.right(),
                "Edge price labels must fit without shifting the arrow");
            check(marker.labelOnRight ? marker.label.left() > candleX : marker.label.right() < candleX,
                "Price text must remain beside its arrow instead of covering the candle");
        }
    }
    for (double pointSize : {9.0, 9.5, 12.0, 14.5, 18.0}) {
        QFont markerFont = daily->font();
        markerFont.setPointSizeF(pointSize);
        const QFontMetricsF metrics(markerFont);
        for (const QString& number : {QStringLiteral("1163.00"), QStringLiteral("645.01"),
                                     QStringLiteral("4.825"), QStringLiteral("10000.00")}) {
            for (double candleX : {73.908333333, 304.5, 535.091666667}) {
                const auto marker = daily_kline::priceMarkerLayout(markerPlot, QPointF(candleX, 100), number, metrics);
                check(marker.label.width() >= metrics.horizontalAdvance(number) + 3.0,
                    "Price rectangles must fit all digits even when integer font metrics round down");
                check(QFontMetrics(markerFont).elidedText(number, Qt::ElideRight, int(marker.label.width())) == number,
                    "300308 price markers must have enough width to avoid Qt elision");
                check(marker.tip.x() == candleX, "Wider price labels must not move the candle's arrow tip");
            }
        }
    }
    const auto singleton = daily_kline::visibleExtrema(QVector<Sample>{{4.2, 4.3, 4.1}}, 0);
    check(singleton.highIndex == 0 && singleton.lowIndex == 0,
        "A single candle must have both price markers");
    const auto ties = daily_kline::visibleExtrema(QVector<Sample>{{4.2, 4.3, 4.1}, {4.2, 4.3, 4.1}}, 0);
    check(ties.highIndex == 0 && ties.lowIndex == 0, "Repeated extrema must have deterministic markers");
    check(daily_kline::defaultView(309, 60).start == 249
        && daily_kline::defaultView(309, 60).count == 60,
        "An untouched chart must show the configured range ending at the newest candle");
    check(daily_kline::defaultView(10, 800).count == 10
        && daily_kline::defaultView(10, 800).start == 0,
        "A short history must fill the chart instead of leaving an empty left edge");
    check(daily_kline::defaultView(309, 0).count == daily_kline::kMinViewCount,
        "A configured range below the zoom-in floor must still show a readable chart");
    const auto panned = daily_kline::panView({249, 60}, 309, -30.0);
    check(panned.start == 219 && panned.count == 60, "Panning left must reveal earlier candles");
    check(daily_kline::panView({249, 60}, 309, 500.0).start == 249,
        "Panning right must stop at the newest candle instead of running past it");
    const auto zoomed = daily_kline::zoomView({249, 60}, 309, 279.0, 0.5);
    check(zoomed.count == 30 && zoomed.start == 264,
        "Zooming in must keep the candle under the pointer in place");
    check(daily_kline::zoomView({0, 60}, 309, 0.0, 0.5).start == 0,
        "Zooming at the left edge must not pull hidden history into view");
    check(daily_kline::zoomView({249, 60}, 309, 279.0, 0.01).count == daily_kline::kMinViewCount,
        "Zooming in must stop at the minimum readable candle count");
    check(daily_kline::zoomView({0, 3}, 10, 1.0, 0.1, 3).count == 3,
        "A configured range below the readable minimum must cap the zoom-in floor instead of overriding it");
    check(daily_kline::zoomView({0, 0}, 0, 0.0, 0.5).count == 0
        && daily_kline::defaultView(0, 60).count == 0,
        "An empty series must produce an empty view instead of a phantom candle");
    check(daily_kline::zoomView({249, 60}, 309, 279.0, 100.0).count == 309,
        "Zooming out must stop at the loaded history");
    check(daily_kline::pinchZoomFactor(0.0) == 1.0
        && daily_kline::pinchZoomFactor(0.1) < 1.0
        && daily_kline::pinchZoomFactor(-0.1) > 1.0
        && daily_kline::pinchZoomFactor(10.0) == std::pow(0.9, 1.0)
        && daily_kline::pinchZoomFactor(-10.0) == std::pow(0.9, -1.0),
        "Trackpad magnification must map to the wheel's zoom direction and rate");
    check(daily_kline::clampView({-20, 5000}, 309).start == 0
        && daily_kline::clampView({-20, 5000}, 309).count == 309,
        "A stale view must be clamped to the loaded bars after a reload");
    const auto refreshed = daily_kline::refreshView({219, 30}, 309, 310, 60, true);
    check(refreshed.start == 220 && refreshed.count == 30,
        "A refresh after panning must shift the view with the new candle instead of resetting it");
    check(daily_kline::refreshView({219, 30}, 309, 310, 60, false).start == 250,
        "An untouched view must stay on the configured range after a refresh");
    check(std::abs(daily_kline::candleWidth(8.0) - 5.6) < 1e-8
        && daily_kline::candleWidth(80.0) == 24.0,
        "Sparse daily histories must use readable but bounded candle widths");
    check(!daily_kline::candleBodyFilled(QStringLiteral("hollow"), 4.1, 4.2)
        && daily_kline::candleBodyFilled(QStringLiteral("hollow"), 4.2, 4.1)
        && daily_kline::candleBodyFilled(QStringLiteral("hollow"), 4.2, 4.2)
        && daily_kline::candleBodyFilled(QStringLiteral("solid"), 4.1, 4.2)
        && daily_kline::candleBodyFilled(QStringLiteral("solid"), 4.2, 4.1)
        && !daily_kline::candleBodyFilled(QStringLiteral("diagonal"), 4.1, 4.2),
        "Hollow candles must only fill falling bodies; solid candles fill both directions");
    // Header statistics: amount formatting, amplitude against the previous
    // close, and market value from the 10k-share count.
    check(daily_kline::formatAmount(2.4e8) == QStringLiteral("2.40亿")
        && daily_kline::formatAmount(3.2e11) == QStringLiteral("3200亿")
        && daily_kline::formatAmount(2.5e12) == QStringLiteral("2.50万亿")
        && daily_kline::formatAmount(12345.0) == QStringLiteral("1.23万")
        && daily_kline::formatAmount(qQNaN()) == QStringLiteral("--"),
        "Amounts must read in 万/亿/万亿 and never invent a value for missing data");
    check(std::abs(daily_kline::amplitudePct(10.8, 10.1, 10.2) - (0.7 / 10.2 * 100.0)) < 1e-9,
        "Amplitude must be the bar's high-low span against the previous close");
    check(!std::isfinite(daily_kline::amplitudePct(10.8, 10.1, 0.0))
        && !std::isfinite(daily_kline::amplitudePct(10.8, 10.1, qQNaN())),
        "Amplitude must stay unknown without a usable previous close");
    check(std::abs(daily_kline::marketValue(10.6, 100000.0) - 10.6e9) < 1.0,
        "Market value must scale the 10k-share count into shares");
    check(!std::isfinite(daily_kline::marketValue(10.6, 0.0))
        && !std::isfinite(daily_kline::marketValue(qQNaN(), 100000.0)),
        "Market value must stay unknown without both a price and a share count");
    samples[1].close = qQNaN();
    const auto missing = daily_kline::movingAverage(samples, 5);
    check(!std::isfinite(missing[4]) && std::isfinite(missing[6]),
        "Invalid closes must suppress only averages whose windows include them");
    cfg.dailyKlineDays = 60;
    cfg.dailyKlineMAPeriods = {250};
    daily->setContext(cfg, "1.510300", "沪深300ETF");
    control(timeline, true);
    check(pending.size() == 1 && pending.first().payload.value("count").toInt() == daily_kline::kMaxHistoryCount,
        "MA250 on a 60-day view must still request the full history depth");
    QJsonArray longHistory;
    for (int i = 0; i < 309; ++i) {
        longHistory.append(QJsonObject{{"time", QDate(2025, 1, 1).addDays(i).toString(Qt::ISODate) + " 00:00:00"},
            {"open", 4.2}, {"high", 4.3}, {"low", 4.1}, {"close", 4.25}, {"volume", 12345}});
    }
    pending.takeFirst().callback({{"bars", longHistory}}, {});
    check(daily->property("barCount").toInt() == 60,
        "Warm-up data must not expand the visible 60-day range");
    cfg.dailyKlineMAPeriods.clear();
    daily->setContext(cfg, "1.510300", "沪深300ETF");
    check(pending.size() == 1 && pending.first().payload.value("count").toInt() == daily_kline::kMaxHistoryCount,
        "Disabling all MAs must preserve the candle chart and its history depth");
    pending.takeFirst().callback(response(), {});
    control(timeline, false);
    // Shared background must preserve both the configured RGB and alpha, with
    // no double tint or opaque override when switching to the child daily view.
    cfg.timelineChartBgColor = QColor(36, 72, 103, 140);
    timeline.chartConfig = cfg;
    daily->setContext(cfg, "1.510300", "沪深300ETF");
    timeline.update();
    const QImage timelineImage = timeline.grab().toImage();
    const QColor commonBackground = timelineImage.pixelColor(2, 2);
    control(timeline, true);
    const QImage dailyImage = timeline.grab().toImage();
    check(dailyImage.pixelColor(2, 2) == commonBackground,
        "Timeline and daily background color and transparency must match");
    check(timeline.property("dailyKlineVisible").toBool(), "Daily mode must suppress timeline contents");
    for (int yy = 60; yy <= 300; ++yy) {
        check(dailyImage.pixelColor(300, yy) == commonBackground,
            "Gap-free daily chart must have no horizontal background grid or timeline ghosting");
    }
    // Hover readout: the cursor's slot picks a candle and leaving clears it.
    const QPoint hoverPoint(480, 150);
    QMouseEvent hoverMove(QEvent::MouseMove, QPointF(hoverPoint),
        QPointF(timeline.mapToGlobal(hoverPoint)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &hoverMove);
    check(daily->property("hoverIndex").toInt() == 1,
        "Hovering the right slot must select the newest visible candle");
    daily->grab();
    check(std::abs(daily->property("hoverOpen").toDouble() - 4.27) < 1e-9
            && std::abs(daily->property("hoverHigh").toDouble() - 4.29) < 1e-9
            && std::abs(daily->property("hoverLow").toDouble() - 4.18) < 1e-9
            && std::abs(daily->property("hoverClose").toDouble() - 4.20) < 1e-9,
        "K-line hover readout must include open, high, low and close");
    QMouseEvent hoverLeave(QEvent::Leave, QPointF(0, 0),
        QPointF(timeline.mapToGlobal(QPoint(0, 0))), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &hoverLeave);
    check(daily->property("hoverIndex").toInt() == -1,
        "Leaving the chart must clear the hover indicator");
    control(timeline, false);
    check(!timeline.property("dailyKlineVisible").toBool()
        && timeline.grab().toImage().pixelColor(300, 160) == QColor(Qt::magenta),
        "Releasing Ctrl must restore timeline drawing");

    // Pan and zoom: drag the plot to reach older candles, wheel-zoom around the
    // pointer, and confirm the untouched range survives a refresh.
    cfg.dailyKlineDays = 60;
    cfg.dailyKlineMAPeriods = {};
    daily->setContext(cfg, "1.600000", "Drag fixture");
    daily->setPinnedDaily(true);
    QJsonArray deepHistory;
    for (int i = 0; i < 300; ++i) {
        const double close = 10.0 + (i % 7) * 0.1;
        deepHistory.append(QJsonObject{
            {"time", QDate(2024, 1, 1).addDays(i).toString(Qt::ISODate) + " 00:00:00"},
            {"open", close - 0.05}, {"high", close + 0.12}, {"low", close - 0.15},
            {"close", close}, {"volume", 1000 + i}});
    }
    pending.takeFirst().callback({{"bars", deepHistory}}, {});
    daily->grab();
    check(daily->property("viewStart").toInt() == 240 && daily->property("viewCount").toInt() == 60,
        "A loaded chart must start on the configured range at the newest candle");
    const QPoint plotCenter(280, 200);
    const auto sendMouse = [&](QEvent::Type type, const QPoint& pos, Qt::MouseButton button,
                               Qt::MouseButtons buttons) {
        QMouseEvent mouseEvent(type, QPointF(pos), QPointF(timeline.mapToGlobal(pos)),
            button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&timeline, &mouseEvent);
    };
    sendMouse(QEvent::MouseButtonPress, plotCenter, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, plotCenter + QPoint(100, 0), Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, plotCenter + QPoint(100, 0), Qt::LeftButton, Qt::NoButton);
    const int draggedStart = daily->property("viewStart").toInt();
    check(draggedStart < 240 && daily->property("viewCount").toInt() == 60,
        "Dragging right must scroll back through earlier candles without changing the zoom");
    sendMouse(QEvent::MouseButtonPress, plotCenter, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, plotCenter - QPoint(5000, 0), Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, plotCenter - QPoint(5000, 0), Qt::LeftButton, Qt::NoButton);
    check(daily->property("viewStart").toInt() == 240,
        "Dragging left past the newest candle must stop at the right edge");
    // A release outside the chart never reaches this filter, so a move without
    // the left button must end the drag instead of panning on every hover.
    sendMouse(QEvent::MouseButtonPress, plotCenter, Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseMove, plotCenter + QPoint(60, 0), Qt::NoButton, Qt::LeftButton);
    const int releasedStart = daily->property("viewStart").toInt();
    sendMouse(QEvent::MouseMove, plotCenter + QPoint(200, 0), Qt::NoButton, Qt::NoButton);
    sendMouse(QEvent::MouseMove, plotCenter + QPoint(400, 0), Qt::NoButton, Qt::NoButton);
    check(daily->property("viewStart").toInt() == releasedStart,
        "Hovering after a release outside the chart must not keep panning");
    sendMouse(QEvent::MouseMove, plotCenter, Qt::NoButton, Qt::NoButton);
    check(timeline.cursor().shape() == Qt::OpenHandCursor,
        "The hand cursor must return once the drag ends");

    // The third argument is the pixel delta, the fourth the angle delta; a
    // trackpad reports pixels only.
    const auto wheel = [&](const QPoint& pos, int delta, int pixel = 0) {
        QWheelEvent wheelEvent(QPointF(pos), QPointF(timeline.mapToGlobal(pos)),
            pixel != 0 ? QPoint(0, pixel) : QPoint(),
            pixel != 0 ? QPoint() : QPoint(0, delta),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&timeline, &wheelEvent);
    };
    wheel(plotCenter, 120);
    const int zoomedCount = daily->property("viewCount").toInt();
    check(zoomedCount < 60 && zoomedCount >= daily_kline::kMinViewCount,
        "Wheel up must zoom in to fewer candles");
    check(daily->property("viewStart").toInt() + zoomedCount <= 300,
        "Zooming in must stay inside the loaded history");
    wheel(plotCenter, -120);
    check(daily->property("viewCount").toInt() > zoomedCount,
        "Wheel down must zoom back out");
    // Trackpads deliver a stream of small pixel deltas; they must accumulate
    // into one zoom step instead of a single pixel nudging the scale.
    const int beforeTrackpad = daily->property("viewCount").toInt();
    wheel(plotCenter, 0, 5);
    wheel(plotCenter, 0, 5);
    check(daily->property("viewCount").toInt() == beforeTrackpad,
        "Small trackpad deltas must accumulate rather than nudge the scale");
    wheel(plotCenter, 0, 5);
    check(daily->property("viewCount").toInt() < beforeTrackpad,
        "Accumulated trackpad deltas must eventually zoom");
    for (int i = 0; i < 40; ++i) wheel(plotCenter, 120);
    check(daily->property("viewCount").toInt() == daily_kline::kMinViewCount,
        "Zooming in must stop at the readable minimum");
    for (int i = 0; i < 80; ++i) wheel(plotCenter, -120);
    check(daily->property("viewCount").toInt() == 300,
        "Zooming out must stop at the loaded history instead of the configured range");
    // A Mac trackpad pinch reaches the chart as a native zoom gesture, with the
    // magnification increment in value().
    const auto pinch = [&](double magnification, const QPoint& pos) {
        QNativeGestureEvent gesture(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
            QPointF(pos), QPointF(pos), QPointF(timeline.mapToGlobal(pos)),
            magnification, QPointF());
        QApplication::sendEvent(&timeline, &gesture);
    };
    const int beforePinch = daily->property("viewCount").toInt();
    pinch(0.12, plotCenter);
    const int pinchedIn = daily->property("viewCount").toInt();
    check(pinchedIn < beforePinch && pinchedIn >= daily_kline::kMinViewCount,
        "Pinching out must zoom in to fewer candles");
    pinch(-0.12, plotCenter);
    check(daily->property("viewCount").toInt() > pinchedIn,
        "Pinching in must zoom back out");
    pinch(0.0, plotCenter);
    check(daily->property("viewCount").toInt() == 300,
        "A zero-magnification gesture must leave the view untouched");
    for (int i = 0; i < 40; ++i) pinch(0.2, plotCenter);
    check(daily->property("viewCount").toInt() == daily_kline::kMinViewCount,
        "Pinching in must honour the readable minimum");
    // Solid candle bodies must visibly fill rising candles that the default
    // hollow style leaves empty.
    const auto countUpPixels = [](const QImage& image) {
        int count = 0;
        const QColor upColor = AppConfig{}.timelineChartUpColor;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y) == upColor) ++count;
            }
        }
        return count;
    };
    cfg.timelineChartCandleStyle = QStringLiteral("hollow");
    daily->setContext(cfg, "1.600000", "Drag fixture");
    const int hollowPixels = countUpPixels(daily->grab().toImage());
    cfg.timelineChartCandleStyle = QStringLiteral("solid");
    daily->setContext(cfg, "1.600000", "Drag fixture");
    const int solidPixels = countUpPixels(daily->grab().toImage());
    check(solidPixels > hollowPixels,
        "Solid candle style must fill rising bodies the hollow style leaves empty");
    cfg.timelineChartCandleStyle = QStringLiteral("hollow");
    daily->setContext(cfg, "1.600000", "Drag fixture");
    daily->setPinnedDaily(false);
    check(daily->isHidden() && timeline.cursor().shape() == Qt::ArrowCursor,
        "Leaving daily mode must restore the normal cursor");

    struct Range { double high, low; };
    const auto up = daily_kline::unfilledGaps(QVector<Range>{{10, 9}, {13, 12}}, 0);
    check(up.size() == 1 && up[0].upward && up[0].lower == 10 && up[0].upper == 12,
        "Upward gap must span previous high to next low");
    const auto upPartial = daily_kline::unfilledGaps(QVector<Range>{{10, 9}, {13, 12}, {13, 11}}, 0);
    check(upPartial.size() == 1 && upPartial[0].lower == 10 && upPartial[0].upper == 11,
        "Partial upward fill must shrink the remaining upper edge");
    check(daily_kline::unfilledGaps(QVector<Range>{{10, 9}, {13, 12}, {13, 10}}, 0).isEmpty(),
        "Touching previous high must fully fill an upward gap");
    const auto down = daily_kline::unfilledGaps(QVector<Range>{{13, 12}, {10, 9}}, 0);
    check(down.size() == 1 && !down[0].upward && down[0].lower == 10 && down[0].upper == 12,
        "Downward gap must span next high to previous low");
    const auto downPartial = daily_kline::unfilledGaps(QVector<Range>{{13, 12}, {10, 9}, {11, 9}}, 0);
    check(downPartial.size() == 1 && downPartial[0].lower == 11 && downPartial[0].upper == 12,
        "Partial downward fill must raise the remaining lower edge");
    check(daily_kline::unfilledGaps(QVector<Range>{{13, 12}, {10, 9}, {12, 9}}, 0).isEmpty(),
        "Touching previous low must fully fill a downward gap");
    check(daily_kline::unfilledGaps(QVector<Range>{{11, 9}, {13, 10}}, 0).isEmpty(),
        "Overlapping wicks must not be marked as a gap merely because the open jumps");
    check(daily_kline::unfilledGaps(QVector<Range>{{10, 9}, {13, 12}, {13, 12}}, 1).isEmpty(),
        "Hidden warm-up history must not introduce gaps outside the visible range");
    const auto multiple = daily_kline::unfilledGaps(QVector<Range>{{10, 9}, {13, 12}, {16, 15}, {16, 14}}, 0);
    check(multiple.size() == 2 && multiple[0].lower == 10 && multiple[0].upper == 12
        && multiple[1].lower == 13 && multiple[1].upper == 14,
        "Multiple unfilled gaps must update independently");
    cfg.dailyKlineDays = 3;
    daily->setContext(cfg, "1.510300", "Gap rendering fixture");
    control(timeline, true);
    QJsonArray gapBars;
    gapBars.append(QJsonObject{{"time", "2026-10-12 00:00:00"}, {"open", 10},
        {"high", 11}, {"low", 9}, {"close", 10}, {"volume", 100}});
    gapBars.append(QJsonObject{{"time", "2026-10-13 00:00:00"}, {"open", 14},
        {"high", 15}, {"low", 13}, {"close", 14}, {"volume", 200}});
    gapBars.append(QJsonObject{{"time", "2026-10-14 00:00:00"}, {"open", 14},
        {"high", 15}, {"low", 13}, {"close", 14}, {"volume", 150}});
    pending.takeFirst().callback({{"bars", gapBars}}, {});
    check(timeline.grab().toImage().pixelColor(300, 180) != commonBackground,
        "An unfilled gap must visibly shade its price interval");
    daily->setContext(cfg, "1.600519", "Filled gap rendering fixture");
    auto filledBar = gapBars.last().toObject();
    filledBar.insert("low", 10);
    gapBars[2] = filledBar;
    pending.takeFirst().callback({{"bars", gapBars}}, {});
    check(timeline.grab().toImage().pixelColor(300, 180) == commonBackground,
        "A filled gap must stop shading the chart");
    control(timeline, false);
    // Optional actual-data screenshot, used for visual verification without networking.
    if (argc > 2) {
        timeline.resize(560, 360);
        cfg.timelineChartBgColor = AppConfig{}.timelineChartBgColor;
        timeline.chartConfig = cfg;
        QFile input(QString::fromLocal8Bit(argv[1]));
        check(input.open(QIODevice::ReadOnly), "Cannot open daily fixture");
        cfg.dailyKlineDays = 60;
        cfg.dailyKlineMAPeriods = {5, 10, 20, 30, 60, 120, 250};
        daily->setContext(cfg, argc > 3 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("1.510300"),
            argc > 4 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("沪深300ETF"));
        control(timeline, true);
        pending.takeFirst().callback(QJsonDocument::fromJson(input.readAll()).object(), {});
        const QString base = QString::fromLocal8Bit(argv[2]);
        timeline.grab().save(base);
        // Same fixture after the pan/zoom interactions, for visual checks.
        const QPoint center(timeline.width() / 2, timeline.height() / 2);
        QWheelEvent zoomOut(QPointF(center), QPointF(timeline.mapToGlobal(center)), QPoint(),
            QPoint(0, -360), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&timeline, &zoomOut);
        timeline.grab().save(base + QStringLiteral("-zoomout.png"));
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(center),
            QPointF(timeline.mapToGlobal(center)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&timeline, &press);
        const QPoint dragged = center + QPoint(timeline.width() / 2, 0);
        QMouseEvent move(QEvent::MouseMove, QPointF(dragged),
            QPointF(timeline.mapToGlobal(dragged)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&timeline, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(dragged),
            QPointF(timeline.mapToGlobal(dragged)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&timeline, &release);
        timeline.grab().save(base + QStringLiteral("-panned.png"));
    }
    return 0;
}
