#pragma once

#include <QSize>
#include <QtGlobal>
#include <cmath>

// Geometry rules for the floating K-line detail window. Its default footprint
// is also its minimum, and every resize keeps the default aspect ratio so the
// price plot, volume bars and period bar stay laid out as designed.
namespace kline_popup {

inline constexpr int kDefaultWidth = 560;
inline constexpr int kDefaultHeight = 360;
inline constexpr int kResizeGripSizePx = 22;
inline constexpr int kScreenMarginPx = 12;

inline QSize normalizedSize(const QSize& requested, const QSize& reference) {
    const QSize minimum(kDefaultWidth, kDefaultHeight);
    const qreal aspectRatio = static_cast<qreal>(kDefaultWidth)
        / static_cast<qreal>(kDefaultHeight);
    const QSize safeReference = reference.isValid() ? reference : minimum;

    // Never go below the default window; the drag direction that moved further
    // from the start size decides which axis follows the pointer.
    const int requestedWidth = qMax(minimum.width(), requested.width());
    const int requestedHeight = qMax(minimum.height(), requested.height());
    const qreal widthScale = static_cast<qreal>(requestedWidth)
        / static_cast<qreal>(qMax(1, safeReference.width()));
    const qreal heightScale = static_cast<qreal>(requestedHeight)
        / static_cast<qreal>(qMax(1, safeReference.height()));

    int width = requestedWidth;
    int height = requestedHeight;
    if (std::fabs(widthScale - 1.0) >= std::fabs(heightScale - 1.0)) {
        height = qRound(static_cast<qreal>(width) / aspectRatio);
    } else {
        width = qRound(static_cast<qreal>(height) * aspectRatio);
    }
    return QSize(width, height);
}

} // namespace kline_popup
