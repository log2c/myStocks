#pragma once

#include "types.h"
#include <QJsonObject>
#include <QObject>
#include <functional>

namespace tdx_bridge {
using Callback = std::function<void(const QJsonObject&, const QString&)>;
// Runs outside the UI thread. Callbacks are delivered only while context lives.
void request(QObject* context, const AppConfig& config, QJsonObject payload, Callback callback);
// Independent from the serialized quotation API; starts only a local relay.
QJsonObject networkProxyBlocking(const AppConfig& config, QString* error);
QJsonObject requestBlocking(const QJsonObject& payload, QString* error);
}
