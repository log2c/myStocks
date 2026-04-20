#pragma once

#include "types.h"

#include <QString>

namespace app_logging {

QString logDirectoryPath();
QString normalizeLogLevel(const QString& rawLevel);

void initFileLogger(const AppConfig& cfg);
void setLogConfig(bool enabled, const QString& level);

} // namespace app_logging
