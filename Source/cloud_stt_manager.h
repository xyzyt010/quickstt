#ifndef CLOUD_STT_MANAGER_H
#define CLOUD_STT_MANAGER_H

#include <QObject>
#include <QElapsedTimer>
#include <QStringList>

class QNetworkAccessManager;

QStringList allCloudModelCatalog();
QStringList allCloudProviderIds();
QString cloudProviderIdForModel(const QString &modelName);
QString cloudProviderDisplayName(const QString &providerId);
QString cloudProviderAuthSummary(const QString &providerId);
QString cloudProviderStatusText(const QString &providerId);
QString cloudProviderLastStatus(const QString &providerId);
QStringList cloudProviderModelOptions(const QString &providerId);
QString cloudProviderDefaultModelChoice(const QString &providerId);
QString cloudProviderConfiguredModelChoice(const QString &providerId);
QStringList cloudModelsForProvider(const QString &providerId);
QString cloudModelDisplayName(const QString &modelName);
QString cloudModelWidgetLabel(const QString &modelName);
QString cloudModelDescription(const QString &modelName);
QString cloudModelSettingKey(const QString &modelName, const QString &leaf);
QStringList cloudLanguageOptionLabels(const QString &modelName);
QString cloudLanguageCodeForLabel(const QString &modelName,
                                 const QString &label);
QString cloudLanguageLabelForCode(const QString &modelName,
                                  const QString &code);
QString cloudDefaultLanguageLabel(const QString &modelName);
QString cloudModelLastStatus(const QString &modelName);
QString cloudModelStateText(const QString &modelName);
QString cloudModelTooltip(const QString &modelName);
QString cloudModelDetailsText(const QString &modelName);
QString cloudModelRequirementsText(const QString &modelName);
bool cloudModelSupportsPrompt(const QString &modelName);
bool cloudModelSupportsLanguage(const QString &modelName);
bool isCloudModel(const QString &modelName);
bool isCloudModelConfigured(const QString &modelName);
QString normalizeCloudModelSelection(const QString &modelName);

class CloudSttManager : public QObject {
  Q_OBJECT

public:
  explicit CloudSttManager(QObject *parent = nullptr);

  bool isBusy() const { return m_busy; }
  void transcribeFile(const QString &cloudModelName, const QString &audioPath);

signals:
  void statusChanged(const QString &statusText);
  void transcriptionReady(const QString &text);
  void transcriptionFailed(const QString &errorText);

private:
  struct ProviderSettings {
    QString cloudModelId;
    QString providerId;
    QString displayName;
    QString endpoint;
    QString apiKey;
    QString accessToken;
    QString appId;
    QString projectId;
    QString location;
    QString model;
    QString language;
    QString prompt;
    QString mode;
    QString domain;
    QString format;
  };

  ProviderSettings loadSettingsForModel(const QString &cloudModelName) const;
  void startOpenAiRequest(const ProviderSettings &settings,
                          const QString &audioPath);
  void startGoogleRequest(const ProviderSettings &settings,
                          const QString &audioPath);
  void startElevenLabsRequest(const ProviderSettings &settings,
                              const QString &audioPath);
  void startAssemblyAiRequest(const ProviderSettings &settings,
                              const QString &audioPath);
  void startSarvamRequest(const ProviderSettings &settings,
                          const QString &audioPath);
  void startReverieRequest(const ProviderSettings &settings,
                           const QString &audioPath);

  void finishWithTranscript(const QString &text);
  void finishWithError(const QString &errorText);
  void cleanupActiveAudio();
  void storeActiveStatus(const QString &state, const QString &message) const;

  QNetworkAccessManager *m_network = nullptr;
  bool m_busy = false;
  QString m_activeAudioPath;
  QString m_activeProviderId;
  QString m_activeCloudModelName;
  QElapsedTimer m_activeTimer;
};

#endif // CLOUD_STT_MANAGER_H
