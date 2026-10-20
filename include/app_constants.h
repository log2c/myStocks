#pragma once

#include <QtGlobal>

namespace app_constants {

inline constexpr auto kOrganizationName = "myStocks";
inline constexpr auto kApplicationName = "myStocks";
inline constexpr auto kSettingsFileName = "settings.ini";
inline constexpr auto kStableConfigPrefix = "cfg/";
inline constexpr auto kLegacyConfigPrefix = "general/";
inline constexpr auto kWatchIndexesKey = "watch/indexes";
inline constexpr qint64 kNetworkCacheTtlSecs = 30;
inline constexpr qint64 kNetworkCacheTtlMs = kNetworkCacheTtlSecs * 1000;
inline constexpr qint64 kMarketBreadthPopupAutoRefreshDelayMs = 1000;
inline constexpr qint64 kMarketBreadthPopupAutoRefreshIntervalMs =
    kNetworkCacheTtlMs + kMarketBreadthPopupAutoRefreshDelayMs;
inline constexpr qint64 kTrayErrorBadgeDurationMs = 5000;
inline constexpr qint64 kStartupSplashMinimumDisplayMs = 2500;
// A timeline series is ~241 points per day; keeping a bounded number of cached
// symbols stops a long session of hovering from growing the heap without limit.
inline constexpr int kTimelineCacheMaxEntries = 16;

} // namespace app_constants
