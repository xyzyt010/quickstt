#ifndef LOCAL_MODEL_SUPPORT_H
#define LOCAL_MODEL_SUPPORT_H

#include <QVector>
#include <QString>
#include <QStringList>

struct ComputeTargetInfo {
  QString id;
  QString displayName;
  QString vendorName;
  QString backendLabel;
  int dedicatedVramMb = 0;
  int sharedMemoryMb = 0;
  int systemMemoryMb = 0;
  bool integrated = false;
  bool software = false;
  bool localAccelerationDetected = false;
  bool isCpuFallback = false;
};

struct LocalModelPackageInfo {
  QString id;
  QString displayName;
  QString serverRelativePath;
  QString directUrl;
  QString installRootKey;
  QString installSubdir;
  QStringList installMarkers;
  QStringList ownedPaths;
  bool archivePackage = true;
};

struct LocalModelDescriptor {
  QString id;
  QString displayName;
  QString engineFamily;
  QString accelerationLabel;
  QString description;
  QString widgetHint;
  QString backendKey;
  QString variantKey;
  QString runtimeExecutablePath;
  QString installedPath;
  QString runnerModelId;
  QString packageId;
  QString runtimePackageId;
  QStringList extraPackageIds;
  int modelSizeMb = 0;
  int runtimeSizeMb = 0;
  int minRecommendedMemoryMb = 0;
  int preferredMemoryMb = 0;
  bool widgetSelectable = false;
  bool directDownload = false;
  bool requiresGpu = false;
  // Framework capabilities — drive swappable runtime selection instead of
  // hard-coded model-name branches in the UI / hotkey paths.
  bool supportsBatchFile = true;     // transcribe from wav/file (C++ pill)
  bool supportsStreaming = false;    // partials while recording (Ctrl+Space)
  bool supportsDirectPcm = false;    // JSON-line PCM without file I/O
  bool usesPersistentWorker = false; // long-lived child process (load/unload)
};

QVector<ComputeTargetInfo> detectComputeTargets();
QString defaultComputeTargetId(const QVector<ComputeTargetInfo> &targets);
ComputeTargetInfo computeTargetById(const QVector<ComputeTargetInfo> &targets,
                                    const QString &targetId);
QString computeTargetSummaryText(const ComputeTargetInfo &target);
QString computeTargetRecommendationText(const ComputeTargetInfo &target);

QVector<LocalModelDescriptor>
localDashboardCatalogForTarget(const ComputeTargetInfo &target);
QStringList localDashboardCatalogNames(const ComputeTargetInfo &target);
bool isKnownLocalModel(const QString &modelName);
bool localModelWidgetSelectable(const QString &modelName);
bool localModelSupportsDirectDownload(const QString &modelName);
bool localModelSupportsRuntimeNow(const QString &modelName);
bool localModelUsesFrontendTranscriber(const QString &modelName);
bool localModelSupportsStreaming(const QString &modelName);
bool localModelSupportsDirectPcm(const QString &modelName);
bool localModelUsesPersistentWorker(const QString &modelName);
/// True when the Ctrl+Space popup should use the native direct PCM pipe
/// (Parakeet / future Nemotron streaming workers) instead of CLOUD_AUDIO files.
bool localModelUsesNativeDirectPipeline(const QString &modelName);
bool isLocalModelInstalled(const QString &modelName);
QStringList localModelAvailableBackendKeys(const QString &modelName);
QString localModelBackendLabelForKey(const QString &backendKey);
QString localModelSelectedBackendKey(const QString &modelName);
void setLocalModelSelectedBackendKey(const QString &modelName,
                                     const QString &backendKey);
QString localModelBackendStatusText(const QString &modelName);
QString localModelStateText(const QString &modelName, bool installed);
QString localModelTooltip(const QString &modelName, bool installed);
QString localModelDetailsText(const QString &modelName,
                              const ComputeTargetInfo &target);
QString localModelRecommendationText(const QString &modelName,
                                     const ComputeTargetInfo &target);
QString localModelBestFitTag(const QString &modelName,
                             const ComputeTargetInfo &target);
QString localModelDisplayState(const QString &modelName, bool installed,
                               bool widgetChecked);
QString canonicalLocalModelName(const QString &modelName);
LocalModelDescriptor localModelDescriptor(const QString &modelName);
LocalModelPackageInfo localModelPackage(const QString &packageId);
QStringList localModelPackageSequence(const QString &modelName);
QStringList installedModelsSharingRuntime(const QString &runtimePackageId);
QString localModelInstalledPath(const QString &modelName);
QString localModelRuntimeExecutablePath(const QString &modelName);
QString localModelRuntimePackageId(const QString &modelName);
QString localModelRunnerModelId(const QString &modelName);
QString localModelBackendKey(const QString &modelName);

QString quickSttDataRoot();
QString quickSttModelsRoot();
QString installRootPathForKey(const QString &rootKey);
QStringList configuredServerUrls();

#endif // LOCAL_MODEL_SUPPORT_H
