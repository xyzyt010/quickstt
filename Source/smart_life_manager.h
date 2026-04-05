#ifndef SMART_LIFE_MANAGER_H
#define SMART_LIFE_MANAGER_H

#include <QObject>
#include <QStringList>
#include <QVector>

struct SmartLifeHomeInfo {
  QString id;
  QString name;
  QString geoName;
};

struct SmartLifeRoomInfo {
  QString id;
  QString homeId;
  QString name;
};

struct SmartLifeDeviceInfo {
  QString id;
  QString name;
  QString category;
  QString productName;
  QString homeId;
  QString homeName;
  QString roomId;
  QString roomName;
  bool online = false;
  bool controllable = false;
  bool likelyLighting = false;
  bool powerOn = false;
  QString primaryPowerCode;
  QStringList powerCodes;
  QStringList functionCodes;
};

class QNetworkAccessManager;

class SmartLifeManager : public QObject {
  Q_OBJECT

public:
  explicit SmartLifeManager(QObject *parent = nullptr);

  static QStringList endpointKeys();
  static QString endpointLabel(const QString &endpointKey);
  static QString endpointUrl(const QString &endpointKey);
  static QStringList appSchemaChoices();

  bool isConnected() const { return m_connected; }
  QString statusText() const { return m_statusText; }
  QString connectionSummaryText() const;
  QString commandHelpText() const;
  QVector<SmartLifeHomeInfo> homes() const { return m_homes; }
  QVector<SmartLifeRoomInfo> rooms() const { return m_rooms; }
  QVector<SmartLifeDeviceInfo> devices() const { return m_devices; }
  SmartLifeDeviceInfo deviceById(const QString &deviceId) const;
  QString deviceDetailText(const QString &deviceId) const;

  void connectAndSync();
  void syncDevices();
  void disconnectSession(bool clearSavedTokens = false);
  void controlDevices(const QStringList &deviceIds, bool turnOn);
  bool handleVoiceCommand(const QString &spokenText,
                          QString *feedback = nullptr);

  struct Config {
    QString accountMode;
    QString endpointKey;
    QString accessId;
    QString accessKey;
    QString developerUid;
    QString developerHomeIds;
    QString username;
    QString password;
    QString countryCode;
    QString appSchema;
    bool passwordAlreadyMd5 = false;
  };

signals:
  void statusChanged(const QString &statusText);
  void connectionChanged(bool connected);
  void devicesChanged();
  void controlFinished(const QString &message);
  void controlFailed(const QString &message);

private:
  Config loadConfig() const;
  bool ensureAuthenticated(const Config &config, QString *errorText);
  bool requestProjectToken(const Config &config, QString *errorText);
  bool loginAssociatedUser(const Config &config, QString *errorText);
  bool refreshDeviceCache(const Config &config, QString *errorText);
  bool fetchRoomsForHome(const Config &config, const SmartLifeHomeInfo &home,
                         QHash<QString, SmartLifeRoomInfo> *roomMap,
                         QHash<QString, QString> *deviceToRoomId,
                         QString *errorText);
  bool fetchDevicesForHome(const Config &config, const SmartLifeHomeInfo &home,
                           const QHash<QString, SmartLifeRoomInfo> &roomMap,
                           const QHash<QString, QString> &deviceToRoomId,
                           QVector<SmartLifeDeviceInfo> *devices,
                           QString *errorText);
  bool fetchProjectDevices(const Config &config,
                           QVector<SmartLifeDeviceInfo> *devices,
                           QString *errorText);
  bool fetchAssociatedDevices(const Config &config,
                              QVector<SmartLifeDeviceInfo> *devices,
                              QString *errorText);
  bool enrichDeviceSpecification(const Config &config,
                                 SmartLifeDeviceInfo *device,
                                 QString *errorText);
  bool sendPowerCommand(const Config &config, SmartLifeDeviceInfo *device,
                        bool turnOn, QString *errorText);

  struct VoiceMatchResult {
    bool recognizedIntent = false;
    bool matched = false;
    QString actionLabel;
    QString targetLabel;
    QStringList deviceIds;
    QString errorText;
  };

  VoiceMatchResult matchVoiceCommand(const QString &spokenText) const;
  QStringList normalizeConfiguredHomeIds(const QString &rawIds) const;
  QString configFingerprint(const Config &config) const;
  void setStatus(const QString &statusText);
  void setConnected(bool connected);
  void clearCache();
  void clearSessionTokens();

  QNetworkAccessManager *m_network = nullptr;
  bool m_connected = false;
  QString m_statusText;
  QString m_authFingerprint;
  QString m_projectToken;
  qint64 m_projectTokenExpiresAtMs = 0;
  QString m_userToken;
  qint64 m_userTokenExpiresAtMs = 0;
  QString m_uid;
  QVector<SmartLifeHomeInfo> m_homes;
  QVector<SmartLifeRoomInfo> m_rooms;
  QVector<SmartLifeDeviceInfo> m_devices;
};

#endif // SMART_LIFE_MANAGER_H
