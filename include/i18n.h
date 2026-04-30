#pragma once

#include <QString>
#include <QStringList>

namespace i18n {

QString normalizeLanguage(const QString& language);
QString resolveLanguage(const QString& language);
QString t(const QString& key, const QString& language);
QStringList columnNames(const QString& language);

} // namespace i18n
