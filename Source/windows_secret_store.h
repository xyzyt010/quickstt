#ifndef WINDOWS_SECRET_STORE_H
#define WINDOWS_SECRET_STORE_H

#include <QString>

class QSettings;

QString loadProtectedSetting(QSettings &settings, const QString &legacyKey,
                             const QString &protectedKey = QString());
void saveProtectedSetting(QSettings &settings, const QString &legacyKey,
                          const QString &value,
                          const QString &protectedKey = QString());

#endif // WINDOWS_SECRET_STORE_H
