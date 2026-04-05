#include "local_model_manager.h"

#include "local_model_support.h"

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

LocalModelManager::LocalModelManager(QObject *parent) : QObject(parent) {
  m_commandProcess = new QProcess(this);
  connect(m_commandProcess,
          qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          &LocalModelManager::onCommandFinished);
  connect(m_commandProcess, &QProcess::readyReadStandardOutput, this,
          &LocalModelManager::onCommandReadyRead);
  connect(m_commandProcess, &QProcess::readyReadStandardError, this,
          &LocalModelManager::onCommandReadyRead);
}

void LocalModelManager::downloadModel(const QString &modelName) {
  if (m_busy) {
    emit operationFailed(modelName, QStringLiteral("Another local model operation is already running."));
    return;
  }

  m_modelName = modelName.trimmed();
  m_packageSequence = localModelPackageSequence(m_modelName);
  if (m_packageSequence.isEmpty()) {
    emit statusMessage(QStringLiteral("%1 is already installed.").arg(m_modelName));
    emit modelInstalled(m_modelName);
    emit catalogChanged();
    return;
  }

  m_busy = true;
  m_isUninstall = false;
  m_packageIndex = -1;
  m_urlIndex = -1;
  emit busyChanged(true);
  emit statusMessage(QStringLiteral("Preparing %1...").arg(m_modelName));
  startNextPackage();
}

void LocalModelManager::uninstallModel(const QString &modelName) {
  if (m_busy) {
    emit operationFailed(modelName, QStringLiteral("Another local model operation is already running."));
    return;
  }

  m_busy = true;
  m_isUninstall = true;
  m_modelName = modelName.trimmed();
  emit busyChanged(true);
  emit statusMessage(QStringLiteral("Removing %1...").arg(m_modelName));

  const LocalModelDescriptor descriptor = localModelDescriptor(m_modelName);
  if (descriptor.displayName.isEmpty() || descriptor.packageId.isEmpty()) {
    failCurrent(QStringLiteral("Unknown local model."));
    return;
  }

  if (!removePackagePaths(descriptor.packageId)) {
    failCurrent(QStringLiteral("Failed to remove model files."));
    return;
  }

  for (const QString &extraPackageId : descriptor.extraPackageIds)
    removePackagePaths(extraPackageId);

  if (!descriptor.runtimePackageId.isEmpty() &&
      installedModelsSharingRuntime(descriptor.runtimePackageId).isEmpty()) {
    removePackagePaths(descriptor.runtimePackageId);
  }

  emit statusMessage(QStringLiteral("Removed %1.").arg(m_modelName));
  emit modelUninstalled(m_modelName);
  emit catalogChanged();
  finishCurrent();
}

void LocalModelManager::onReplyDownloadProgress(qint64 received, qint64 total) {
  if (m_modelName.isEmpty())
    return;
  const int percent = total > 0 ? int((received * 100) / total) : 0;
  const QString packageId =
      (m_packageIndex >= 0 && m_packageIndex < m_packageSequence.size())
          ? m_packageSequence[m_packageIndex]
          : QString();
  const QString packageName = localModelPackage(packageId).displayName;
  emit progressChanged(
      m_modelName, percent,
      packageName.isEmpty()
          ? QStringLiteral("Downloading %1 (%2%)").arg(m_modelName).arg(percent)
          : QStringLiteral("Downloading %1 (%2%)").arg(packageName).arg(percent));
}

void LocalModelManager::onReplyFinished() {
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
    failCurrent(QStringLiteral("Downloaded package is empty."));
    return;
  }

  const QString packageId = m_packageSequence.value(m_packageIndex);
  const LocalModelPackageInfo package = localModelPackage(packageId);
  const QString installRoot = installRootPathForKey(package.installRootKey);
  const QString destinationRoot = package.installSubdir.isEmpty()
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
    QString targetPath = package.ownedPaths.isEmpty()
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
    failCurrent(QStringLiteral("Package extraction failed for %1.").arg(package.displayName));
    return;
  }

  startNextPackage();
}

void LocalModelManager::failCurrent(const QString &errorText) {
  if (m_downloadFile) {
    m_downloadFile->close();
    delete m_downloadFile;
    m_downloadFile = nullptr;
  }
  if (!m_currentDownloadFile.isEmpty())
    QFile::remove(m_currentDownloadFile);
  emit statusMessage(errorText);
  emit operationFailed(m_modelName, errorText);
  finishCurrent();
}

void LocalModelManager::finishCurrent() {
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
  if (m_commandProcess && m_commandProcess->state() != QProcess::NotRunning)
    m_commandProcess->kill();
  m_busy = false;
  m_isUninstall = false;
  m_packageSequence.clear();
  m_packageIndex = -1;
  m_urlIndex = -1;
  m_currentDownloadFile.clear();
  emit busyChanged(false);
}

void LocalModelManager::startNextPackage() {
  ++m_packageIndex;
  if (m_packageIndex >= m_packageSequence.size()) {
    emit statusMessage(QStringLiteral("%1 installed.").arg(m_modelName));
    emit modelInstalled(m_modelName);
    emit catalogChanged();
    finishCurrent();
    return;
  }
  m_urlIndex = -1;
  tryCurrentUrl();
}

void LocalModelManager::tryCurrentUrl() {
  if (m_reply) {
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
  }

  const QStringList urls = currentPackageUrls();
  if (urls.isEmpty()) {
    if (trySpecialInstall())
      return;
    const QString packageName =
        localModelPackage(m_packageSequence.value(m_packageIndex)).displayName;
    failCurrent(QStringLiteral("No reachable download source for %1.").arg(packageName));
    return;
  }
  ++m_urlIndex;
  if (m_urlIndex >= urls.size()) {
    if (trySpecialInstall())
      return;
    const QString packageName =
        localModelPackage(m_packageSequence.value(m_packageIndex)).displayName;
    failCurrent(QStringLiteral("No reachable download source for %1.").arg(packageName));
    return;
  }

  const QUrl url(urls[m_urlIndex]);
  QNetworkRequest request(url);
  request.setTransferTimeout(15000);
  emit statusMessage(QStringLiteral("Fetching %1...").arg(url.toString()));

  const QString tmpName = QStringLiteral("QuickSTT_%1_%2.zip")
                              .arg(cleanIdPart(m_modelName))
                              .arg(m_packageIndex);
  m_currentDownloadFile = QDir(QDir::tempPath()).filePath(tmpName);
  QFile::remove(m_currentDownloadFile);
  if (m_downloadFile) {
    m_downloadFile->close();
    delete m_downloadFile;
    m_downloadFile = nullptr;
  }
  m_downloadFile = new QFile(m_currentDownloadFile, this);
  if (!m_downloadFile->open(QIODevice::WriteOnly)) {
    delete m_downloadFile;
    m_downloadFile = nullptr;
    failCurrent(QStringLiteral("Failed to create a temporary download file."));
    return;
  }

  m_reply = m_network.get(request);
  connect(m_reply, &QNetworkReply::downloadProgress, this,
          &LocalModelManager::onReplyDownloadProgress);
  connect(m_reply, &QNetworkReply::readyRead, this,
          &LocalModelManager::onReplyReadyRead);
  connect(m_reply, &QNetworkReply::finished, this,
          &LocalModelManager::onReplyFinished);
}

bool LocalModelManager::trySpecialInstall() {
  const QString packageId = m_packageSequence.value(m_packageIndex);
  const LocalModelDescriptor descriptor = localModelDescriptor(m_modelName);
  if (descriptor.engineFamily != QStringLiteral("faster-whisper") ||
      packageId != descriptor.packageId || !m_commandProcess) {
    return false;
  }

  const QString runnerPath =
      QDir(quickSttWhisperRoot()).filePath(localModelRuntimeExecutablePath(m_modelName));
  if (!QFileInfo::exists(runnerPath)) {
    failCurrent(QStringLiteral("faster-whisper runtime is missing."));
    return true;
  }

  const LocalModelPackageInfo package = localModelPackage(packageId);
  if (package.ownedPaths.isEmpty()) {
    failCurrent(QStringLiteral("Invalid faster-whisper package metadata."));
    return true;
  }

  const QString installRoot = installRootPathForKey(package.installRootKey);
  const QString targetDir =
      QDir(installRoot).filePath(QFileInfo(package.ownedPaths.first()).path());
  QDir().mkpath(targetDir);

  QStringList args;
  args << QStringLiteral("download")
       << QStringLiteral("--model") << localModelRunnerModelId(m_modelName)
       << QStringLiteral("--output-dir") << QDir::toNativeSeparators(targetDir);
  emit statusMessage(QStringLiteral("Downloading %1...").arg(package.displayName));
  m_commandProcess->start(runnerPath, args);
  if (!m_commandProcess->waitForStarted(5000)) {
    failCurrent(QStringLiteral("Failed to start the faster-whisper downloader."));
    return true;
  }
  return true;
}

void LocalModelManager::onCommandFinished(int exitCode,
                                          QProcess::ExitStatus exitStatus) {
  if (!m_busy || m_packageIndex < 0 || m_packageIndex >= m_packageSequence.size())
    return;

  const QString stdOut = QString::fromUtf8(m_commandProcess->readAllStandardOutput());
  const QString stdErr = QString::fromUtf8(m_commandProcess->readAllStandardError());
  const QString details = (stdOut + "\n" + stdErr).trimmed();
  if (exitStatus != QProcess::NormalExit || exitCode != 0) {
    failCurrent(details.isEmpty()
                    ? QStringLiteral("Local package command failed.")
                    : details);
    return;
  }

  startNextPackage();
}

void LocalModelManager::onReplyReadyRead() {
  if (!m_reply || !m_downloadFile)
    return;
  const QByteArray chunk = m_reply->readAll();
  if (chunk.isEmpty())
    return;
  if (m_downloadFile->write(chunk) != chunk.size()) {
    failCurrent(QStringLiteral("Failed while writing the downloaded package."));
  }
}

void LocalModelManager::onCommandReadyRead() {
  if (!m_commandProcess)
    return;
  const QString stdOut = QString::fromUtf8(m_commandProcess->readAllStandardOutput()).trimmed();
  const QString stdErr = QString::fromUtf8(m_commandProcess->readAllStandardError()).trimmed();
  const QString merged = QStringList{stdOut, stdErr}.join(QLatin1Char('\n')).trimmed();
  if (!merged.isEmpty())
    emit statusMessage(merged);
}

bool LocalModelManager::extractArchive(const QString &archivePath,
                                       const QString &destinationRoot) const {
  QStringList args;
  args << QStringLiteral("-NoProfile") << QStringLiteral("-ExecutionPolicy")
       << QStringLiteral("Bypass") << QStringLiteral("-Command")
       << QStringLiteral("Expand-Archive -LiteralPath %1 -DestinationPath %2 -Force")
              .arg(psQuoted(QDir::toNativeSeparators(archivePath)),
                   psQuoted(QDir::toNativeSeparators(destinationRoot)));
  return QProcess::execute(QStringLiteral("powershell"), args) == 0;
}

bool LocalModelManager::removePackagePaths(const QString &packageId) {
  const LocalModelPackageInfo package = localModelPackage(packageId);
  if (package.id.isEmpty())
    return false;

  bool ok = true;
  const QString installRoot = installRootPathForKey(package.installRootKey);
  for (const QString &ownedPath : package.ownedPaths) {
    ok = removePathRecursively(QDir(installRoot).filePath(ownedPath)) && ok;
  }
  return ok;
}

QStringList LocalModelManager::currentPackageUrls() const {
  const LocalModelPackageInfo package =
      localModelPackage(m_packageSequence.value(m_packageIndex));
  QStringList urls;
  if (!package.serverRelativePath.isEmpty()) {
    for (const QString &baseUrl : configuredServerUrls()) {
      QString normalizedBase = baseUrl.trimmed();
      if (normalizedBase.endsWith('/'))
        normalizedBase.chop(1);
      urls << QStringLiteral("%1/download/%2")
                  .arg(normalizedBase,
                       package.serverRelativePath);
    }
  }
  if (!package.directUrl.isEmpty())
    urls << package.directUrl;
  urls.removeDuplicates();
  return urls;
}
