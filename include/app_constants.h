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

} // namespace app_constants
