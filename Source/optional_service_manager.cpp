#include "optional_service_manager.h"

#include "local_model_support.h"
#include "optional_service_support.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QProcess>
#include <QUrl>

namespace {

QString cleanIdPart(QString text) {
  text = text.toLower().trimmed();
  QString result;
  result.reserve(text.size());
  for (const QChar ch : text)
    result += ch.isLetterOrNumber() ? ch : QChar('_');
  while (result.contains("__"))
    result.replace("__", "_");
  return result.trimmed();
}

QString psQuoted(QString text) {
  text.replace('\'', QStringLiteral("''"));
  return QStringLiteral("'%1'").arg(text);
}

bool removePathRecursively(const QString &path) {
  QFileInfo info(path);
  if (!info.exists())
    return true;
  if (info.isDir())
    return QDir(path).removeRecursively();
  return QFile::remove(path);
}

bool flattenSingleArchiveRoot(const QString &destinationRoot) {
  QDir rootDir(destinationRoot);
  const QFileInfoList rootEntries =
      rootDir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
  QFileInfoList rootDirs;
  QFileInfoList rootFiles;
  for (const QFileInfo &entry : rootEntries) {
    if (entry.isDir())
      rootDirs << entry;
    else
      rootFiles << entry;
  }

  if (!rootFiles.isEmpty() || rootDirs.size() != 1)
    return false;

  QDir nestedDir(rootDirs.first().absoluteFilePath());
  const QFileInfoList nestedEntries =
      nestedDir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
  for (const QFileInfo &entry : nestedEntries) {
    const QString fromPath = entry.absoluteFilePath();
    const QString toPath = rootDir.filePath(entry.fileName());
    if (QFileInfo::exists(toPath))
      removePathRecursively(toPath);
    if (!QDir().rename(fromPath, toPath))
      return false;
  }
  return rootDir.rmdir(rootDirs.first().fileName());
}

} // namespace

OptionalServiceManager::OptionalServiceManager(QObject *parent)
    : QObject(parent) {}

void OptionalServiceManager::downloadService(const QString &serviceId) {
  const QString cleaned = serviceId.trimmed().toLower();
  if (m_busy) {
    emit operationFailed(
        cleaned, QStringLiteral("Another optional service operation is already running."));
    return;
  }

  m_serviceId = cleaned;
  m_packageSequence = optionalServicePackageSequence(cleaned);
  if (m_packageSequence.isEmpty()) {
    emit statusMessage(optionalServiceStateText(cleaned));
    emit serviceInstalled(cleaned);
    emit catalogChanged();
    return;
  }

  m_busy = true;
  m_isUninstall = false;
  m_packageIndex = -1;
  m_urlIndex = -1;
  emit busyChanged(true);
  emit statusMessage(
      QStringLiteral("Preparing %1...")
          .arg(optionalServiceDescriptor(cleaned).displayName));
  startNextPackage();
}

void OptionalServiceManager::uninstallService(const QString &serviceId) {
  const QString cleaned = serviceId.trimmed().toLower();
  if (m_busy) {
    emit operationFailed(
        cleaned, QStringLiteral("Another optional service operation is already running."));
    return;
  }

  const OptionalServiceDescriptor descriptor = optionalServiceDescriptor(cleaned);
  if (descriptor.id.isEmpty()) {
    emit operationFailed(cleaned, QStringLiteral("Unknown optional service."));
    return;
  }

  m_busy = true;
  m_isUninstall = true;
  m_serviceId = cleaned;
  emit busyChanged(true);
  emit statusMessage(QStringLiteral("Removing %1...").arg(descriptor.displayName));

  bool ok = true;
  for (const QString &packageId : descriptor.packageSequence)
    ok = removePackagePaths(packageId) && ok;

  if (!ok) {
    failCurrent(QStringLiteral("Failed to remove one or more service files."));
    return;
  }

  emit statusMessage(QStringLiteral("Removed %1.").arg(descriptor.displayName));
  emit serviceUninstalled(cleaned);
  emit catalogChanged();
  finishCurrent();
}

void OptionalServiceManager::onReplyDownloadProgress(qint64 received,
                                                     qint64 total) {
  if (m_serviceId.isEmpty())
    return;
  const int percent = total > 0 ? int((received * 100) / total) : 0;
  const QString packageId =
      (m_packageIndex >= 0 && m_packageIndex < m_packageSequence.size())
          ? m_packageSequence[m_packageIndex]
          : QString();
  const QString packageName = optionalServicePackage(packageId).displayName;
  emit progressChanged(
      m_serviceId, percent,
      packageName.isEmpty()
          ? QStringLiteral("Downloading service package (%1%)").arg(percent)
          : QStringLiteral("Downloading %1 (%2%)")
                .arg(packageName)
                .arg(percent));
}

void OptionalServiceManager::onReplyReadyRead() {
  if (!m_reply || !m_downloadFile)
    return;
  const QByteArray chunk = m_reply->readAll();
  if (chunk.isEmpty())
    return;
  if (m_downloadFile->write(chunk) != chunk.size())
    failCurrent(QStringLiteral("Failed while writing the downloaded service package."));
}

void OptionalServiceManager::onReplyFinished() {
  if (!m_reply)
    return;

  QNetworkReply *reply = m_reply;
  m_reply = nullptr;
  reply->deleteLater();

  if (m_downloadFile) {
    m_downloadFile->flush();
    m_downloadFile->close();
    delete m_downloadFile;
    m_downloadFile = nullptr;
  }

  if (reply->error() != QNetworkReply::NoError) {
    if (!m_currentDownloadFile.isEmpty())
      QFile::remove(m_currentDownloadFile);
    tryCurrentUrl();
    return;
  }

  if (!QFileInfo::exists(m_currentDownloadFile) ||
      QFileInfo(m_currentDownloadFile).size() <= 0) {
    failCurrent(QStringLiteral("Downloaded service package is empty."));
    return;
  }

  const OptionalServicePackageInfo package =
      optionalServicePackage(m_packageSequence.value(m_packageIndex));
  const QString installRoot = quickSttServicesRoot();
  const QString destinationRoot =
      package.installSubdir.isEmpty()
          ? installRoot
          : QDir(installRoot).filePath(package.installSubdir);
  QDir().mkpath(destinationRoot);

  emit statusMessage(QStringLiteral("Installing %1...").arg(package.displayName));
  bool ok = false;
  if (package.archivePackage) {
    ok = extractArchive(m_currentDownloadFile, destinationRoot);
    if (ok) {
      bool markersPresent = !package.installMarkers.isEmpty();
      for (const QString &marker : package.installMarkers) {
        if (!QFileInfo::exists(QDir(installRoot).filePath(marker))) {
          markersPresent = false;
          break;
        }
      }
      if (!markersPresent)
        flattenSingleArchiveRoot(destinationRoot);
    }
  } else {
    const QString targetPath =
        package.ownedPaths.isEmpty()
            ? QDir(destinationRoot).filePath(
                  QFileInfo(m_currentDownloadFile).fileName())
            : QDir(installRoot).filePath(package.ownedPaths.first());
    QDir().mkpath(QFileInfo(targetPath).absolutePath());
    QFile::remove(targetPath);
    ok = QFile::copy(m_currentDownloadFile, targetPath);
  }

  QFile::remove(m_currentDownloadFile);
  m_currentDownloadFile.clear();

  if (!ok) {
    failCurrent(
        QStringLiteral("Package extraction failed for %1.").arg(package.displayName));
    return;
  }

  startNextPackage();
}

void OptionalServiceManager::failCurrent(const QString &errorText) {
  if (m_downloadFile) {
    m_downloadFile->close();
    delete m_downloadFile;
    m_downloadFile = nullptr;
  }
  if (!m_currentDownloadFile.isEmpty())
    QFile::remove(m_currentDownloadFile);
  emit statusMessage(errorText);
  emit operationFailed(m_serviceId, errorText);
  finishCurrent();
}

void OptionalServiceManager::finishCurrent() {
  if (m_reply) {
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
  }
  if (m_downloadFile) {
    m_downloadFile->close();
    delete m_downloadFile;
    m_downloadFile = nullptr;
  }
  m_busy = false;
  m_isUninstall = false;
  m_packageSequence.clear();
  m_packageIndex = -1;
  m_urlIndex = -1;
  m_currentDownloadFile.clear();
  emit busyChanged(false);
}

void OptionalServiceManager::startNextPackage() {
  ++m_packageIndex;
  if (m_packageIndex >= m_packageSequence.size()) {
    emit statusMessage(
        QStringLiteral("%1 installed.")
            .arg(optionalServiceDescriptor(m_serviceId).displayName));
    emit serviceInstalled(m_serviceId);
    emit catalogChanged();
    finishCurrent();
    return;
  }
  m_urlIndex = -1;
  tryCurrentUrl();
}

void OptionalServiceManager::tryCurrentUrl() {
  if (m_reply) {
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
  }

  const QStringList urls = currentPackageUrls();
  if (urls.isEmpty()) {
    const QString packageName =
        optionalServicePackage(m_packageSequence.value(m_packageIndex)).displayName;
    failCurrent(QStringLiteral("No reachable download source for %1.")
                    .arg(packageName));
    return;
  }

  ++m_urlIndex;
  if (m_urlIndex >= urls.size()) {
    const QString packageName =
        optionalServicePackage(m_packageSequence.value(m_packageIndex)).displayName;
    failCurrent(QStringLiteral("No reachable download source for %1.")
                    .arg(packageName));
    return;
  }

  const QUrl url(urls[m_urlIndex]);
  QNetworkRequest request(url);
  request.setTransferTimeout(15000);
  emit statusMessage(QStringLiteral("Fetching %1...").arg(url.toString()));

  const QString tmpName = QStringLiteral("QuickSTT_service_%1_%2.zip")
                              .arg(cleanIdPart(m_serviceId))
                              .arg(m_packageIndex);
  m_currentDownloadFile = QDir(QDir::tempPath()).filePath(tmpName);
  QFile::remove(m_currentDownloadFile);
  delete m_downloadFile;
  m_downloadFile = new QFile(m_currentDownloadFile, this);
  if (!m_downloadFile->open(QIODevice::WriteOnly)) {
    delete m_downloadFile;
    m_downloadFile = nullptr;
    failCurrent(QStringLiteral("Failed to create a temporary download file."));
    return;
  }

  m_reply = m_network.get(request);
  connect(m_reply, &QNetworkReply::downloadProgress, this,
          &OptionalServiceManager::onReplyDownloadProgress);
  connect(m_reply, &QNetworkReply::readyRead, this,
          &OptionalServiceManager::onReplyReadyRead);
  connect(m_reply, &QNetworkReply::finished, this,
          &OptionalServiceManager::onReplyFinished);
}

bool OptionalServiceManager::extractArchive(const QString &archivePath,
                                            const QString &destinationRoot) const {
  QStringList args;
  args << QStringLiteral("-NoProfile") << QStringLiteral("-ExecutionPolicy")
       << QStringLiteral("Bypass") << QStringLiteral("-Command")
       << QStringLiteral("Expand-Archive -LiteralPath %1 -DestinationPath %2 -Force")
              .arg(psQuoted(QDir::toNativeSeparators(archivePath)),
                   psQuoted(QDir::toNativeSeparators(destinationRoot)));
  return QProcess::execute(QStringLiteral("powershell"), args) == 0;
}

bool OptionalServiceManager::removePackagePaths(const QString &packageId) {
  const OptionalServicePackageInfo package = optionalServicePackage(packageId);
  if (package.id.isEmpty())
    return false;

  bool ok = true;
  const QString installRoot = quickSttServicesRoot();
  for (const QString &ownedPath : package.ownedPaths)
    ok = removePathRecursively(QDir(installRoot).filePath(ownedPath)) && ok;
  return ok;
}

QStringList OptionalServiceManager::currentPackageUrls() const {
  const OptionalServicePackageInfo package =
      optionalServicePackage(m_packageSequence.value(m_packageIndex));
  QStringList urls;
  if (!package.serverRelativePath.isEmpty()) {
    for (const QString &baseUrl : configuredServerUrls()) {
      QString normalizedBase = baseUrl.trimmed();
      if (normalizedBase.endsWith('/'))
        normalizedBase.chop(1);
      urls << QStringLiteral("%1/download/%2")
                  .arg(normalizedBase, package.serverRelativePath);
    }
  }
  if (!package.directUrl.isEmpty())
    urls << package.directUrl;
  urls.removeDuplicates();
  return urls;
}
