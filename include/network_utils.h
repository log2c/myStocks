#pragma once

#include "types.h"

#include <QNetworkProxy>
#include <QString>

namespace network_utils {

QString effectiveUserAgent(const AppConfig& cfg);
QNetworkProxy proxyFromConfig(const AppConfig& cfg);
bool hasProxy(const AppConfig& cfg);

} // namespace network_utils
