#pragma once

#include "types.h"
#include <QFontMetricsF>
#include <algorithm>
#include <cmath>

namespace daily_kline {

inline QColor movingAverageColor(int period) {
    switch (period) {
    case 5: return QColor("#f0f0f0");
    case 10: return QColor("#ffdf00");
    case 20: return QColor("#e970dc");
    case 30: return QColor("#65c566");
    case 60: return QColor("#39c5cf");
    case 120: return QColor("#5994ff");
    case 250: return QColor("#ff9d3f");
    default: return QColor(Qt::white);
    }
}

inline int historyCount(int visibleDays, const QVector<int>& periods) {
    const auto normalized = normalizeDailyKlineMAPeriods(periods);
    return qBound(1, visibleDays, 800) + (normalized.isEmpty() ? 0 : normalized.last() - 1);
}

// Keep sparse histories readable. A short post-listing history has a wide
// slot when it is stretched across the chart; capping the candle width keeps
// the candle and volume bars balanced without making regular charts bulky.
inline double candleWidth(double slot) {
    return qBound(1.0, slot * 0.7, 24.0);
}

// Full-period simple moving averages of closing prices. Missing history is
// unavailable, never a shorter partial average that looks like a valid MA250.
template<typename Bars>
QVector<double> movingAverage(const Bars& bars, int period) {
    QVector<double> values(bars.size(), qQNaN());
    if (period <= 0) return values;
    double sum = 0;
    int invalid = 0;
    for (int i = 0; i < bars.size(); ++i) {
        if (std::isfinite(bars[i].close)) sum += bars[i].close;
        else ++invalid;
        if (i >= period) {
            if (std::isfinite(bars[i - period].close)) sum -= bars[i - period].close;
            else --invalid;
        }
        if (i >= period - 1 && invalid == 0) values[i] = sum / period;
    }
    return values;
}

struct PriceMarkerLayout {
    QPointF tip;
    QRectF label;
    bool labelOnRight;
};

inline PriceMarkerLayout priceMarkerLayout(const QRectF& plot, const QPointF& anchor,
                                          double textWidth, double height = 18.0) {
    const double spacing = 9.0;
    const double rightRoom = plot.right() - anchor.x();
    const double leftRoom = anchor.x() - plot.left();
    const bool onRight = rightRoom >= leftRoom;
    const double width = std::min(textWidth, std::max(0.0, (onRight ? rightRoom : leftRoom) - spacing));
    // Only the text's space is bounded. Never shift the arrow tip away from
    // the candle's exact (potentially fractional) horizontal center.
    return {anchor, QRectF(onRight ? anchor.x() + spacing : anchor.x() - spacing - width,
        anchor.y() - height / 2, width, height), onRight};
}

inline PriceMarkerLayout priceMarkerLayout(const QRectF& plot, const QPointF& anchor,
                                          const QString& text, const QFontMetricsF& metrics,
                                          double height = 18.0) {
    // Integer advances can round down (e.g. "1163.00"), causing Qt to elide
    // the last digits even though the chart has ample room for the price.
    const double width = std::ceil(metrics.horizontalAdvance(text)) + 4.0;
    return priceMarkerLayout(plot, anchor, width, height);
}

struct Extrema {
    int highIndex = -1;
    int lowIndex = -1;
    double high = qQNaN();
    double low = qQNaN();
};

template<typename Bars>
Extrema visibleExtrema(const Bars& bars, int visibleStart) {
    Extrema result;
    for (int i = qMax(0, visibleStart); i < bars.size(); ++i) {
        if (std::isfinite(bars[i].high) && (result.highIndex < 0 || bars[i].high > result.high)) {
            result.highIndex = i;
            result.high = bars[i].high;
        }
        if (std::isfinite(bars[i].low) && (result.lowIndex < 0 || bars[i].low < result.low)) {
            result.lowIndex = i;
            result.low = bars[i].low;
        }
    }
    return result;
}

struct Gap {
    int startIndex;
    double lower;
    double upper;
    bool upward;
};

// Track full-range gaps formed inside the visible window. Later daily wicks
// reduce the still-open price interval; touching its far edge fills it fully.
// Hidden MA warm-up history must not create extra gap annotations.
template<typename Bars>
QVector<Gap> unfilledGaps(const Bars& bars, int visibleStart) {
    QVector<Gap> gaps;
    for (int i = qMax(1, visibleStart + 1); i < bars.size(); ++i) {
        const auto& bar = bars[i];
        const auto& previous = bars[i - 1];
        if (!std::isfinite(bar.high) || !std::isfinite(bar.low) || bar.low > bar.high) continue;
        for (auto& gap : gaps) {
            if (gap.upward) gap.upper = std::min(gap.upper, bar.low);
            else gap.lower = std::max(gap.lower, bar.high);
        }
        gaps.erase(std::remove_if(gaps.begin(), gaps.end(), [](const Gap& gap) {
            return gap.lower >= gap.upper;
        }), gaps.end());
        if (!std::isfinite(previous.high) || !std::isfinite(previous.low) || previous.low > previous.high) continue;
        if (bar.low > previous.high) gaps.append({i, previous.high, bar.low, true});
        else if (bar.high < previous.low) gaps.append({i, bar.high, previous.low, false});
    }
    return gaps;
}

} // namespace daily_kline
