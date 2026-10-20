#pragma once

#include "types.h"

#include <QNetworkProxy>
#include <QString>

namespace network_utils {

struct InterfaceChoice { QString id; QString label; bool up; };
QVector<InterfaceChoice> availableInterfaces();

QString effectiveUserAgent(const AppConfig& cfg);
QNetworkProxy proxyFromConfig(const AppConfig& cfg);
bool hasProxy(const AppConfig& cfg);

} // namespace network_utils
