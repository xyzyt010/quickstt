#ifndef OPTIONAL_SERVICE_MANAGER_H
#define OPTIONAL_SERVICE_MANAGER_H

#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QStringList>

class OptionalServiceManager : public QObject {
  Q_OBJECT

public:
  explicit OptionalServiceManager(QObject *parent = nullptr);

  bool isBusy() const { return m_busy; }
  QString activeService() const { return m_serviceId; }

public slots:
  void downloadService(const QString &serviceId);
  void uninstallService(const QString &serviceId);

signals:
  void busyChanged(bool busy);
  void progressChanged(const QString &serviceId, int percent,
                       const QString &statusText);
  void statusMessage(const QString &statusText);
  void serviceInstalled(const QString &serviceId);
  void serviceUninstalled(const QString &serviceId);
  void operationFailed(const QString &serviceId, const QString &errorText);
  void catalogChanged();

private slots:
  void onReplyDownloadProgress(qint64 received, qint64 total);
  void onReplyReadyRead();
  void onReplyFinished();

private:
  void failCurrent(const QString &errorText);
  void finishCurrent();
  void startNextPackage();
  void tryCurrentUrl();
  bool extractArchive(const QString &archivePath,
                      const QString &destinationRoot) const;
  bool removePackagePaths(const QString &packageId);
  QStringList currentPackageUrls() const;

  QNetworkAccessManager m_network;
  QNetworkReply *m_reply = nullptr;
  bool m_busy = false;
  bool m_isUninstall = false;
  QString m_serviceId;
  QStringList m_packageSequence;
  int m_packageIndex = -1;
  int m_urlIndex = -1;
  QString m_currentDownloadFile;
  QFile *m_downloadFile = nullptr;
};

#endif // OPTIONAL_SERVICE_MANAGER_H
