#ifndef SMART_LIFE_MANAGER_H
#define SMART_LIFE_MANAGER_H

#include <QColor>
#include <QObject>
#include <QHash>
#include <QStringList>
#include <QVector>

enum class SmartLifeColorCapability {
  None = 0,
  Preset,
  Rgb,
};

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
  bool supportsBrightness = false;
  QString brightnessCode;
  int brightnessMin = 10;
  int brightnessMax = 1000;
  int brightness = 1000;
  bool hasBrightness = false;
  SmartLifeColorCapability colorCapability = SmartLifeColorCapability::None;
  QString colorCode;
  QString colorValueType;
  QString workModeCode;
  QString workModeWhiteValue = QStringLiteral("white");
  QString workModeColourValue = QStringLiteral("colour");
  QStringList presetColorLabels;
  QStringList presetColorCommandValues;
  int presetColorIndex = -1;
  QColor rgbColor = Qt::white;
  bool hasRgbColor = false;
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
  QString deviceAlias(const QString &deviceId) const;
  QString deviceDisplayName(const QString &deviceId) const;
  QString deviceDetailText(const QString &deviceId) const;
  bool deviceExposesLightingControls(const SmartLifeDeviceInfo &device) const;
  bool deviceHasVerifiedBrightnessControl(const SmartLifeDeviceInfo &device) const;
  bool deviceHasVerifiedColorControl(const SmartLifeDeviceInfo &device) const;
  void setDeviceAlias(const QString &deviceId, const QString &alias);

  void connectAndSync();
  void syncDevices();
  void disconnectSession(bool clearSavedTokens = false);
  void controlDevices(const QStringList &deviceIds, bool turnOn);
  void setDeviceBrightness(const QString &deviceId, int brightness);
  void setDevicePresetColor(const QString &deviceId, int presetIndex);
  void setDeviceRgbColor(const QString &deviceId, const QColor &color);
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
  void deviceStateChanged(const QString &deviceId);
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
  bool sendDeviceCommands(const Config &config, SmartLifeDeviceInfo *device,
                          const QJsonArray &commands, QString *errorText);
  bool sendCommandsWithFallback(const Config &config, SmartLifeDeviceInfo *device,
                                const QJsonArray &commands, QString *errorText);
  void applyLightingFunctions(const QJsonArray &functions,
                              SmartLifeDeviceInfo *device);
  void applyLightingStatus(const QJsonArray &status, SmartLifeDeviceInfo *device);
  void inferLightingCapabilitiesFromKnownCodes(SmartLifeDeviceInfo *device,
                                               const QJsonArray &status);

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
  void loadDeviceAliases();
  void saveDeviceAliases() const;
  QString deviceDisplayName(const SmartLifeDeviceInfo &device) const;
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
  QHash<QString, QString> m_deviceAliases;
};

#endif // SMART_LIFE_MANAGER_H
