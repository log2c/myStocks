#pragma once

#include "types.h"
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
