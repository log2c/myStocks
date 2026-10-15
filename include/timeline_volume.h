#pragma once

#include "types.h"

#include <QPainter>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>

namespace timeline_volume {

inline QString scaleText(double value, bool english) {
    const double divisor = english
        ? (value >= 1e9 ? 1e9 : value >= 1e6 ? 1e6 : value >= 1e3 ? 1e3 : 1.0)
        : (value >= 1e8 ? 1e8 : value >= 1e4 ? 1e4 : 1.0);
    const QString suffix = english
        ? (divisor == 1e9 ? "B" : divisor == 1e6 ? "M" : divisor == 1e3 ? "K" : "")
        : (divisor == 1e8 ? QStringLiteral("亿") : divisor == 1e4 ? QStringLiteral("万") : QString());
    return QString::number(value / divisor, 'f', divisor > 1.0 ? 1 : 0) + suffix;
}

// Both popup implementations pass their existing price-chart time mapping so
// volume bars share its session breaks and never stretch across unelapsed time.
template<typename Points, typename XOfPoint>
void draw(QPainter& painter, const QRect& plot, const QRect& labelArea,
          const Points& points, XOfPoint xOfPoint, double preClose,
          const AppConfig& cfg, bool english) {
    if (plot.width() <= 0 || plot.height() <= 1) return;
    double maximum = 0.0;
    int spacing = std::numeric_limits<int>::max();
    for (int i = 0; i < points.size(); ++i) {
        if (std::isfinite(points[i].volume) && points[i].volume > 0.0) {
            maximum = qMax(maximum, points[i].volume);
        }
        if (i > 0) {
            const int distance = xOfPoint(i) - xOfPoint(i - 1);
            if (distance > 0) spacing = qMin(spacing, distance);
        }
    }
    // Keep sparse/early-session data thin, with a gap between adjacent minutes.
    const int width = spacing == std::numeric_limits<int>::max()
        ? 1 : qBound(1, qFloor(spacing * 0.7), 4);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(cfg.timelineChartGridColor, 1));
    painter.drawLine(plot.topLeft(), plot.topRight());
    painter.drawLine(plot.bottomLeft(), plot.bottomRight());
    painter.setPen(cfg.timelineChartTextColor);
    painter.drawText(QRect(labelArea.left(), plot.top(), labelArea.width(), 18),
        Qt::AlignRight | Qt::AlignTop, scaleText(maximum, english));
    painter.drawText(QRect(labelArea.left(), plot.bottom() - 16, labelArea.width(), 16),
        Qt::AlignRight | Qt::AlignBottom, QStringLiteral("0"));

    painter.setClipRect(plot, Qt::IntersectClip);
    double previousPrice = preClose;
    QColor previousColor = cfg.timelineChartTextColor;
    const int availableHeight = qMax(1, plot.height() - 1);
    for (int i = 0; i < points.size(); ++i) {
        const auto& point = points[i];
        QColor color = previousColor;
        if (std::isfinite(point.price)) {
            if (std::isfinite(previousPrice)) {
                if (point.price > previousPrice) color = cfg.timelineChartUpColor;
                else if (point.price < previousPrice) color = cfg.timelineChartDownColor;
            }
            previousPrice = point.price;
            previousColor = color;
        }
        // Missing/negative values and zero-volume minutes have no bar. Price
        // direction still advances through those minutes for the next bar.
        if (!std::isfinite(point.volume) || point.volume <= 0.0 || maximum <= 0.0) continue;
        const int height = qMax(1, qRound(point.volume / maximum * availableHeight));
        const int center = qBound(plot.left() + width / 2,
            xOfPoint(i), plot.right() - (width - 1) / 2);
        painter.fillRect(QRect(center - width / 2, plot.bottom() - height, width, height), color);
    }
    painter.restore();
}

} // namespace timeline_volume
