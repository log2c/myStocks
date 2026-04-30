#pragma once

#include <QString>

namespace crash_logging {

void installCrashHandlers();
void appendCrashContext(const QString& title, const QString& details = QString());
void appendCurrentStackTrace(const QString& title, const QString& details = QString());

} // namespace crash_logging
