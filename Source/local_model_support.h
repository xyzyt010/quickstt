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
  bool whisperGpuSupported = false;
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
bool isWhisperLocalModel(const QString &modelName);
bool localModelWidgetSelectable(const QString &modelName);
bool localModelSupportsDirectDownload(const QString &modelName);
bool localModelSupportsRuntimeNow(const QString &modelName);
bool localModelRequiresFrontendTranscription(const QString &modelName);
bool isLocalModelInstalled(const QString &modelName);
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
LocalModelDescriptor localModelDescriptor(const QString &modelName);
LocalModelPackageInfo localModelPackage(const QString &packageId);
QStringList localModelPackageSequence(const QString &modelName);
QStringList installedModelsSharingRuntime(const QString &runtimePackageId);
QString localModelInstalledPath(const QString &modelName);
QString localModelRuntimeExecutablePath(const QString &modelName);
QString localModelRunnerModelId(const QString &modelName);
QString localModelBackendKey(const QString &modelName);

QString quickSttDataRoot();
QString quickSttModelsRoot();
QString quickSttWhisperRoot();
QString installRootPathForKey(const QString &rootKey);
QStringList configuredServerUrls();

#endif // LOCAL_MODEL_SUPPORT_H
