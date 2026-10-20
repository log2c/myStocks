#pragma once

#include "types.h"
#include <QFontMetricsF>
#include <algorithm>
#include <cmath>

namespace daily_kline {

// Never zoom further in than this: below a handful of candles the chart stops
// reading as a K-line series.
inline constexpr int kMinViewCount = 5;

// The bridge caps a K-line reply at this many bars. The chart loads that full
// depth up front so panning left reaches real history rather than the edge of
// the initially visible range.
inline constexpr int kMaxHistoryCount = 1049;

// The candles currently on screen: [start, start + count) of the loaded bars.
struct ViewWindow {
    int start = 0;
    int count = 0;
};

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

// The untouched chart shows the configured range, right-aligned to the newest
// candle. Everything the bridge returned can be panned into, and zooming out
// may reveal more than the configured range.
inline ViewWindow defaultView(int total, int preferred, int minCount = kMinViewCount) {
    if (total <= 0) return {};
    const int minimum = qMin(qMax(1, minCount), total);
    const int count = qBound(minimum, qBound(1, preferred, total), total);
    return {total - count, count};
}

inline ViewWindow clampView(const ViewWindow& view, int total, int minCount = kMinViewCount) {
    if (total <= 0) return {};
    const int minimum = qMin(qMax(1, minCount), total);
    const int count = qBound(minimum, view.count, total);
    return {qBound(0, view.start, total - count), count};
}

// Visible candles after a zoom, rounded so that the candle under the anchor
// stays put. A factor below 1 shows fewer candles.
inline ViewWindow zoomView(const ViewWindow& view, int total, double anchorBar,
                           double factor, int minCount = kMinViewCount) {
    if (total <= 0 || view.count <= 0 || !std::isfinite(factor) || factor <= 0.0) {
        return clampView(view, total, minCount);
    }
    const int minimum = qMin(qMax(1, minCount), total);
    const int count = qBound(minimum, qRound(view.count * factor), total);
    const double fraction = (anchorBar - view.start) / view.count;
    return clampView({qRound(anchorBar - fraction * count), count}, total, minCount);
}

inline ViewWindow panView(const ViewWindow& view, int total, double deltaBars,
                          int minCount = kMinViewCount) {
    return clampView({qRound(view.start + deltaBars), view.count}, total, minCount);
}

// Pick the window for a freshly loaded series. A user who panned or zoomed
// keeps that window anchored to the right edge, so a refresh appends candles
// instead of snapping the chart back to the newest one.
inline ViewWindow refreshView(const ViewWindow& previous, int oldTotal, int newTotal,
                              int preferred, bool touched, int minCount = kMinViewCount) {
    if (!touched || oldTotal <= 0 || previous.count <= 0) {
        return defaultView(newTotal, preferred, minCount);
    }
    const int rightGap = oldTotal - (previous.start + previous.count);
    return clampView({newTotal - previous.count - rightGap, previous.count}, newTotal, minCount);
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
Extrema visibleExtrema(const Bars& bars, int visibleStart, int visibleEnd = -1) {
    Extrema result;
    const int end = visibleEnd < 0 ? static_cast<int>(bars.size())
                                   : qMin(visibleEnd, static_cast<int>(bars.size()));
    for (int i = qMax(0, visibleStart); i < end; ++i) {
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
// Hidden MA warm-up history must not create extra gap annotations, and bars
// panned past the right edge must not create them either.
template<typename Bars>
QVector<Gap> unfilledGaps(const Bars& bars, int visibleStart, int visibleEnd = -1) {
    QVector<Gap> gaps;
    const int end = visibleEnd < 0 ? static_cast<int>(bars.size())
                                   : qMin(visibleEnd, static_cast<int>(bars.size()));
    for (int i = qMax(1, visibleStart + 1); i < end; ++i) {
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
