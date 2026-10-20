#pragma once

#include "types.h"

#include <QNetworkProxy>
#include <QString>

namespace network_utils {

struct InterfaceChoice { QString id; QString label; bool up; };
QVector<InterfaceChoice> availableInterfaces();

QString effectiveUserAgent(const AppConfig& cfg);
// The preset chosen for this run when the mode is "random"; empty in "custom"
// mode. Chosen once per process so every request of a session agrees.
QString sessionRandomUserAgent();
QNetworkProxy proxyFromConfig(const AppConfig& cfg);
bool hasProxy(const AppConfig& cfg);

} // namespace network_utils
