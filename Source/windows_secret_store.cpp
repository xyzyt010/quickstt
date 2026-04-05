#include "windows_secret_store.h"

#include <QByteArray>
#include <QSettings>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace {

QString effectiveProtectedKey(const QString &legacyKey,
                              const QString &protectedKey) {
  return protectedKey.trimmed().isEmpty() ? legacyKey + QStringLiteral("Protected")
                                          : protectedKey.trimmed();
}

#ifdef _WIN32
QByteArray protectBytes(const QByteArray &plain) {
  if (plain.isEmpty())
    return {};

  DATA_BLOB inBlob;
  inBlob.pbData =
      reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
  inBlob.cbData = DWORD(plain.size());

  DATA_BLOB outBlob;
  outBlob.pbData = nullptr;
  outBlob.cbData = 0;

  if (!CryptProtectData(&inBlob, L"QuickSTT", nullptr, nullptr, nullptr, 0,
                        &outBlob)) {
    return {};
  }

  QByteArray protectedData(reinterpret_cast<const char *>(outBlob.pbData),
                           int(outBlob.cbData));
  LocalFree(outBlob.pbData);
  return protectedData;
}

QByteArray unprotectBytes(const QByteArray &cipher) {
  if (cipher.isEmpty())
    return {};

  DATA_BLOB inBlob;
  inBlob.pbData =
      reinterpret_cast<BYTE *>(const_cast<char *>(cipher.constData()));
  inBlob.cbData = DWORD(cipher.size());

  DATA_BLOB outBlob;
  outBlob.pbData = nullptr;
  outBlob.cbData = 0;

  if (!CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr, 0,
                          &outBlob)) {
    return {};
  }

  QByteArray plain(reinterpret_cast<const char *>(outBlob.pbData),
                   int(outBlob.cbData));
  LocalFree(outBlob.pbData);
  return plain;
}
#endif

} // namespace

QString loadProtectedSetting(QSettings &settings, const QString &legacyKey,
                             const QString &protectedKey) {
  const QString protectedStorageKey =
      effectiveProtectedKey(legacyKey, protectedKey);
  const QString encoded = settings.value(protectedStorageKey).toString().trimmed();

  if (!encoded.isEmpty()) {
#ifdef _WIN32
    const QByteArray cipher = QByteArray::fromBase64(encoded.toUtf8());
    const QByteArray plain = unprotectBytes(cipher);
    if (!plain.isEmpty())
      return QString::fromUtf8(plain);
#else
    return QString::fromUtf8(QByteArray::fromBase64(encoded.toUtf8()));
#endif
  }

  return settings.value(legacyKey).toString();
}

void saveProtectedSetting(QSettings &settings, const QString &legacyKey,
                          const QString &value, const QString &protectedKey) {
  const QString protectedStorageKey =
      effectiveProtectedKey(legacyKey, protectedKey);
  const QString trimmedKey = legacyKey.trimmed();
  if (value.isEmpty()) {
    settings.remove(protectedStorageKey);
    if (!trimmedKey.isEmpty())
      settings.remove(trimmedKey);
    return;
  }

#ifdef _WIN32
  const QByteArray protectedBytes = protectBytes(value.toUtf8());
  if (!protectedBytes.isEmpty()) {
    settings.setValue(protectedStorageKey,
                      QString::fromLatin1(protectedBytes.toBase64()));
    if (!trimmedKey.isEmpty())
      settings.remove(trimmedKey);
    return;
  }
#endif

  settings.setValue(protectedStorageKey,
                    QString::fromLatin1(value.toUtf8().toBase64()));
  if (!trimmedKey.isEmpty())
    settings.remove(trimmedKey);
}
