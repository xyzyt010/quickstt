#ifndef ANDROID_TV_MANAGER_H
#define ANDROID_TV_MANAGER_H

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

struct AndroidTvConfig {
  QString profileId;
  QString profileLabel;
  QString stateKey;
  QString host;
  int apiPort = 6466;
  QString pairingHost;
  int pairingPort = 6467;
  QString pairingCode;
  QString friendlyName;
  bool voiceEnabled = false;
};

class AndroidTvManager : public QObject {
  Q_OBJECT

public:
  explicit AndroidTvManager(QObject *parent = nullptr);

  AndroidTvConfig loadConfig() const;
  bool isInstalled() const;
  bool isConfigured() const;
  bool isVoiceEnabled() const;
  bool isConnected() const { return isActuallyConnected(); }
  bool currentConfigHasPairedCredentials() const;
  bool isPowerStateKnown() const;
  bool isTvOn() const;
  bool supportsKeyCommands() const;
  bool supportsPowerCommands() const;
  bool supportsVolumeCommands() const;
  bool supportsAppLinks() const;
  bool hasVolumeInfo() const;
  bool hasDeviceInfo() const;
  int currentVolumeLevel() const;
  int currentVolumeMax() const;
  int currentVolumePercent() const;
  QString currentAppId() const;
  QString helperPythonPath() const;
  QString helperScriptPath() const;
  QString statusText() const { return m_statusText; }
  QString connectionSummaryText() const;
  QString helpText() const;
  bool handleVoiceCommand(const QString &spokenText, QString *feedback);

public slots:
  void startPairing();
  void finishPairing();
  void pairDevice();
  void scanForDevices(int timeoutMs = 2200);
  void connectDevice();
  void disconnectDevice();
  void forgetCurrentDevice();
  void refreshState();
  void turnOn();
  void turnOff();
  void volumeUp();
  void volumeDown();
  void setVolumePercent(int percent);
  void muteToggle();
  void goHome();
  void goBack();
  void openMenu();
  void openSettings();
  void openInputSelector();
  void showApps();
  void playPause();
  void navigateUp();
  void navigateDown();
  void navigateLeft();
  void navigateRight();
  void navigateCenter();

signals:
  void statusChanged(const QString &statusText);
  void connectionChanged(bool connected);
  void stateChanged(const QJsonObject &state);
  void controlFinished(const QString &statusText);
  void controlFailed(const QString &errorText);
  void discoveryChanged(const QJsonArray &devices);
  void pairingCodeRequested(const QString &promptText);

private:
  void scheduleIdleShutdown();
  void cancelIdleShutdown();
  void stopHelperProcess();
  void setStatus(const QString &statusText);
  void setConnected(bool connected);
  void applyState(const QJsonObject &stateObject);
  bool ensureHelperStarted();
  int sendHelperCommand(const QString &action,
                        const QJsonObject &data = QJsonObject(),
                        const AndroidTvConfig *config = nullptr);
  QString endpointString() const;
  QString stateRootPath() const;
  bool hasPairedCredentials(const AndroidTvConfig &config) const;
  QString effectiveStateKey(const AndroidTvConfig &config) const;
  QStringList stateKeyCandidates(const AndroidTvConfig &config) const;
  AndroidTvConfig resolvedConfigForAction(const QString &action) const;
  void handleHelperLine(const QByteArray &lineBytes);
  void handleHelperResponse(const QJsonObject &object);
  void handleHelperEvent(const QJsonObject &object);
  void onHelperStarted();
  void onHelperStdoutReady();
  void onHelperStderrReady();
  void onHelperErrorOccurred(QProcess::ProcessError error);
  void onHelperFinished(int exitCode, QProcess::ExitStatus exitStatus);
  void queueControlCommand(const QString &action,
                           const QString &queuedStatusText = QString(),
                           const QJsonObject &extraData = QJsonObject(),
                           const AndroidTvConfig *overrideConfig = nullptr);
  QString controlActionMessage(const QString &action) const;
  QString currentPairingHost() const;
  bool isActuallyConnected() const;
  void failQueuedCommands(const QString &errorText);
  void flushQueuedCommands();
  QList<AndroidTvConfig> loadProfiles() const;
  void persistConfig(const AndroidTvConfig &config) const;
  bool tryResolveDiscoveryReconnect(const AndroidTvConfig &config,
                                    const QJsonArray &devices,
                                    AndroidTvConfig *resolvedConfig) const;

  QString m_statusText = QStringLiteral("Android TV control is idle.");
  class QProcess *m_helperProcess = nullptr;
  QByteArray m_stdoutBuffer;
  QByteArray m_stderrBuffer;
  QList<QByteArray> m_queuedPayloads;
  QHash<int, QString> m_pendingActions;
  QHash<int, AndroidTvConfig> m_pendingConfigs;
  int m_nextRequestId = 1;
  QJsonObject m_lastState;
  bool m_connected = false;
  bool m_helperStarting = false;
  bool m_preserveConnectionOnNextStop = false;
  bool m_discoveryReconnectPending = false;
  AndroidTvConfig m_runtimeConfig;
  AndroidTvConfig m_discoveryReconnectConfig;
  QString m_discoveryReconnectError;
  QTimer *m_idleShutdownTimer = nullptr;
  QTimer *m_helperStartTimeoutTimer = nullptr;
};

#endif // ANDROID_TV_MANAGER_H
