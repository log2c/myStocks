#include "timeline_volume.h"

#include <QGuiApplication>
#include <QImage>
#include <QDateTime>
#include <QVector>

struct Point { double price; double volume; };
void check(bool condition, const char* message) { if (!condition) qFatal("%s", message); }
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    AppConfig cfg;
    cfg.timelineChartUpColor = Qt::red;
    cfg.timelineChartDownColor = Qt::green;
    cfg.timelineChartTextColor = Qt::white;
    cfg.timelineChartGridColor = Qt::gray;
    const QVector<Point> points{{101, 10}, {100.5, 20}, {100.5, 15},
        {102, 0}, {101.5, 5}, {101, qQNaN()}};
    QImage image(220, 100, QImage::Format_ARGB32);
    image.fill(Qt::black);
    QPainter painter(&image);
    const QPen original(Qt::blue, 2);
    painter.setPen(original);
    painter.setBrush(Qt::yellow);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRect plot(60, 10, 120, 60);
    timeline_volume::draw(painter, plot, points,
        [](int i) { return 60 + i * 3; }, 100.0, cfg);
    check(painter.pen() == original && painter.brush().color() == Qt::yellow
        && painter.testRenderHint(QPainter::Antialiasing), "Volume must restore painter state for time labels");
    painter.end();
    check(image.pixelColor(60, 65) == Qt::red, "First bar compares with previous close");
    check(image.pixelColor(63, 65) == Qt::green, "Falling minute above previous close must be green");
    check(image.pixelColor(66, 65) == Qt::green, "Unchanged minute keeps previous direction");
    check(image.pixelColor(69, 65) == Qt::black, "Zero-volume minute must have no bar");
    check(image.pixelColor(72, 65) == Qt::green, "Compare with previous minute even if it has zero volume");
    check(image.pixelColor(75, 65) == Qt::black, "Missing volume must have no bar");
    check(image.pixelColor(61, 65) == Qt::red, "Adjacent early-session bars must remain visible");
    check(image.pixelColor(59, 65) == Qt::black, "Edge bars must stay inside the chart");
    // User-reported 510300 regression: full-width minute volumes, not lots
    // or cumulative differences, and first-bar height must reach the top.
    const QVector<Point> etf{{4.229, 1355308}, {4.238, 421948}, {4.234, 419112}};
    QImage etfImage(260, 130, QImage::Format_ARGB32);
    etfImage.fill(Qt::black);
    QPainter etfPainter(&etfImage);
    timeline_volume::draw(etfPainter, QRect(60, 10, 180, 100),
        etf, [](int i) { return 60 + i * 10; }, 4.294, cfg);
    etfPainter.end();
    check(etfImage.pixelColor(60, 11) == Qt::green
        && etfImage.pixelColor(63, 11) == Qt::green,
        "09:30 highest volume must be fully visible in green at the left edge");
    check(etfImage.pixelColor(70, 80) == Qt::red
        && etfImage.pixelColor(70, 77) == Qt::black,
        "09:31 must be a red bar at about 31 percent of the maximum height");
    check(etfImage.pixelColor(80, 80) == Qt::green
        && etfImage.pixelColor(80, 77) == Qt::black,
        "09:32 must be green and about the same height as 09:31");
    check(etfImage.pixelColor(65, 100) == Qt::black,
        "Wide first bar must not overlap the next minute");
    for (int yy = 0; yy < etfImage.height(); ++yy) {
        for (int xx = 0; xx < 55; ++xx) {
            check(etfImage.pixelColor(xx, yy) == Qt::black,
                "Volume plots must leave the label margin blank without quantity text");
        }
    }
    return 0;
}
