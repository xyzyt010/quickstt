#ifndef LOCAL_MODEL_MANAGER_H
#define LOCAL_MODEL_MANAGER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QProcess>
#include <QStringList>

struct LocalModelPackageInfo;

class LocalModelManager : public QObject {
  Q_OBJECT

public:
  explicit LocalModelManager(QObject *parent = nullptr);

  bool isBusy() const { return m_busy; }
  QString activeModel() const { return m_modelName; }

public slots:
  void downloadModel(const QString &modelName);
  void uninstallModel(const QString &modelName);

signals:
  void busyChanged(bool busy);
  void progressChanged(const QString &modelName, int percent,
                       const QString &statusText);
  void statusMessage(const QString &statusText);
  void modelInstalled(const QString &modelName);
  void modelUninstalled(const QString &modelName);
  void operationFailed(const QString &modelName, const QString &errorText);
  void catalogChanged();

private slots:
  void onReplyDownloadProgress(qint64 received, qint64 total);
  void onReplyReadyRead();
  void onReplyFinished();
  void onExtractionFinished(int exitCode, QProcess::ExitStatus exitStatus);
  void onHfHelperFinished(int exitCode, QProcess::ExitStatus exitStatus);
  void onHfHelperReadyRead();

private:
  void failCurrent(const QString &errorText);
  void finishCurrent();
  void startNextPackage();
  void tryCurrentUrl();
  void startExtraction(const QString &archivePath,
                       const QString &destinationRoot);
  /// Handy Nemotron 3.5 GGUF: resumable HF download via Python helper.
  bool startNemotronHfDownload(const LocalModelPackageInfo &package);
  bool removePackagePaths(const QString &packageId);
  QStringList currentPackageUrls() const;

  QNetworkAccessManager m_network;
  QNetworkReply *m_reply = nullptr;
  QProcess *m_extractProcess = nullptr;
  QProcess *m_hfHelperProcess = nullptr;
  bool m_busy = false;
  bool m_isUninstall = false;
  QString m_modelName;
  QStringList m_packageSequence;
  int m_packageIndex = -1;
  int m_urlIndex = -1;
  QString m_currentDownloadFile;
  QFile *m_downloadFile = nullptr;
  QString m_pendingExtractionDest;
};

#endif // LOCAL_MODEL_MANAGER_H
