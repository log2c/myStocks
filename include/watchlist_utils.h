#pragma once

#include "types.h"

#include <QString>
#include <QVector>

namespace watchlist_utils {

QString watchCodeKey(const QString& code);
bool isDigitsOnly(const QString& text);
QVector<int> normalizedColumnOrder(const QVector<int>& order);
QString normalizeSectorCode(const QString& rawCode);
QString normalizeFutureCode(const QString& rawCode);
QString normalizeHongKongIndexCode(const QString& rawCode);
bool isPredefinedAshareIndexCode(const QString& rawCode);
bool isPredefinedIndexCode(const QString& rawCode);
bool isHongKongCode(const QString& rawCode);
QVector<StockItem> defaultWatchStocks();
QString defaultDataYamlTemplate();

} // namespace watchlist_utils
