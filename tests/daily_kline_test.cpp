#include "daily_kline_chart.h"
#include "tdx_bridge.h"
#include "daily_kline_indicators.h"
#include <QApplication>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QPainter>

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
    check(pending.first().payload.value("count").toInt() == 89
        && pending.first().payload.value("period").toInt() == 4, "Daily period and default range must reach bridge");
    control(timeline, true, true);
    check(pending.size() == 1, "Auto-repeat must not create duplicate requests");
    control(timeline, false);
    check(daily->isHidden(), "Releasing Ctrl while loading must immediately return to timeline");
    pending.takeFirst().callback(response(), {});
    check(daily->isHidden() && daily->property("barCount").toInt() == 2,
        "Late reply may cache daily data but must not restore daily mode");
    control(timeline, true);
    check(daily->isVisible() && pending.isEmpty(), "Holding Ctrl again must reuse loaded bars");
    timeline.resize(640, 420);
    check(daily->size() == timeline.size(), "Daily overlay must follow popup resize");
    daily->setContext(cfg, "1.510300", "沪深300ETF");
    check(pending.isEmpty(), "Repeated hover updates must not invalidate daily cache");
    cfg.dailyKlineDays = 120;
    daily->setContext(cfg, "1.510300", "沪深300ETF");
    check(pending.size() == 1 && pending.first().payload.value("count").toInt() == 149,
        "Changed range must request new bar count");
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
    const auto singleton = daily_kline::visibleExtrema(QVector<Sample>{{4.2, 4.3, 4.1}}, 0);
    check(singleton.highIndex == 0 && singleton.lowIndex == 0,
        "A single candle must have both price markers");
    const auto ties = daily_kline::visibleExtrema(QVector<Sample>{{4.2, 4.3, 4.1}, {4.2, 4.3, 4.1}}, 0);
    check(ties.highIndex == 0 && ties.lowIndex == 0, "Repeated extrema must have deterministic markers");
    check(daily_kline::historyCount(800, {250}) == 1049 && daily_kline::historyCount(60, {}) == 60,
        "History request must include only the enabled MA warm-up");
    samples[1].close = qQNaN();
    const auto missing = daily_kline::movingAverage(samples, 5);
    check(!std::isfinite(missing[4]) && std::isfinite(missing[6]),
        "Invalid closes must suppress only averages whose windows include them");
    cfg.dailyKlineDays = 60;
    cfg.dailyKlineMAPeriods = {250};
    daily->setContext(cfg, "1.510300", "沪深300ETF");
    control(timeline, true);
    check(pending.size() == 1 && pending.first().payload.value("count").toInt() == 309,
        "MA250 on a 60-day view must request 309 historical bars");
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
    check(pending.size() == 1 && pending.first().payload.value("count").toInt() == 60,
        "Disabling all MAs must preserve the candle chart without extra history");
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
    control(timeline, false);
    check(!timeline.property("dailyKlineVisible").toBool()
        && timeline.grab().toImage().pixelColor(300, 160) == QColor(Qt::magenta),
        "Releasing Ctrl must restore timeline drawing");

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
        daily->setContext(cfg, "1.510300", "沪深300ETF");
        control(timeline, true);
        pending.takeFirst().callback(QJsonDocument::fromJson(input.readAll()).object(), {});
        timeline.grab().save(QString::fromLocal8Bit(argv[2]));
    }
    return 0;
}
