#include "local_model_manager.h"

#include "local_model_support.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
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

LocalModelManager::LocalModelManager(QObject *parent) : QObject(parent) {}

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

  const QString runtimePackageId = localModelRuntimePackageId(m_modelName);
  if (!runtimePackageId.isEmpty() &&
      installedModelsSharingRuntime(runtimePackageId).isEmpty()) {
    removePackagePaths(runtimePackageId);
  }

  if (localModelUsesFrontendTranscriber(m_modelName)) {
    for (const QString &sharedRuntimeId :
         {QStringLiteral("rt_sherpa_onnx_cpu"),
          QStringLiteral("rt_sherpa_onnx_cuda"),
          QStringLiteral("rt_whisper_cpp_cpu")}) {
      if (sharedRuntimeId == runtimePackageId)
        continue;
      if (installedModelsSharingRuntime(sharedRuntimeId).isEmpty())
        removePackagePaths(sharedRuntimeId);
    }
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
  if (package.archivePackage) {
    m_pendingExtractionDest = destinationRoot;
    startExtraction(m_currentDownloadFile, destinationRoot);
    return;
  }

  QString targetPath = package.ownedPaths.isEmpty()
                           ? QDir(destinationRoot).filePath(
                                 QFileInfo(m_currentDownloadFile).fileName())
                           : QDir(installRoot).filePath(package.ownedPaths.first());
  QDir().mkpath(QFileInfo(targetPath).absolutePath());

  // Try same-drive rename first (instant).
  QFile::remove(targetPath);
  bool ok = QFile::rename(m_currentDownloadFile, targetPath);
  if (!ok) {
    // Cross-drive: stream copy with fresh file handles.
    QFile src(m_currentDownloadFile);
    QFile dst(targetPath);
    QString copyError;
    if (!src.open(QIODevice::ReadOnly)) {
      copyError = QStringLiteral("Cannot read temp file: %1").arg(src.errorString());
    } else if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      copyError = QStringLiteral("Cannot write target: %1").arg(dst.errorString());
      src.close();
    } else {
      const qint64 srcSize = src.size();
      qint64 written = 0;
      while (!src.atEnd()) {
        const QByteArray chunk = src.read(262144);
        if (chunk.isEmpty())
          break;
        const qint64 w = dst.write(chunk);
        if (w < 0) {
          copyError = QStringLiteral("Write failed: %1").arg(dst.errorString());
          break;
        }
        written += w;
      }
      dst.close();
      src.close();
      if (copyError.isEmpty() && written == srcSize) {
        ok = true;
        QFile::remove(m_currentDownloadFile);
      } else if (copyError.isEmpty()) {
        copyError = QStringLiteral("Size mismatch: wrote %1 of %2")
                        .arg(written).arg(srcSize);
      }
    }
    if (!ok) {
      QFile::remove(m_currentDownloadFile);
      QFile::remove(targetPath);
      m_currentDownloadFile.clear();
      failCurrent(QStringLiteral("Failed to install %1: %2")
                      .arg(package.displayName, copyError));
      return;
    }
  }
  m_currentDownloadFile.clear();

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
  if (m_hfHelperProcess) {
    m_hfHelperProcess->kill();
    m_hfHelperProcess->deleteLater();
    m_hfHelperProcess = nullptr;
  }
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

  // Handy Nemotron 3.5: large GGUF via huggingface_hub (resumable).
  const LocalModelPackageInfo package =
      localModelPackage(m_packageSequence.value(m_packageIndex));
  if (package.id == QStringLiteral("pkg_nemotron_3_5_asr_streaming_0_6b") ||
      package.id == QStringLiteral("pkg_nemotron_speech_streaming_0_6b")) {
    if (startNemotronHfDownload(package))
      return;
    // Fall through to direct URL if helper is missing.
  }

  tryCurrentUrl();
}

bool LocalModelManager::startNemotronHfDownload(
    const LocalModelPackageInfo &package) {
  const QString installRoot = installRootPathForKey(package.installRootKey);
  const QString dest = package.installSubdir.isEmpty()
                           ? installRoot
                           : QDir(installRoot).filePath(package.installSubdir);
  QDir().mkpath(dest);

  // Prefer script shipped next to the app, then source-tree tools/.
  QStringList scriptCandidates = {
      QDir(QCoreApplication::applicationDirPath())
          .filePath(QStringLiteral("tools/nemotron/fetch_and_convert.py")),
      QDir(QCoreApplication::applicationDirPath())
          .filePath(QStringLiteral("../tools/nemotron/fetch_and_convert.py")),
  };
  // Dev tree: walk up from cwd for tools/nemotron/fetch_and_convert.py
  QDir walk(QDir::currentPath());
  for (int i = 0; i < 6; ++i) {
    scriptCandidates << walk.filePath(
        QStringLiteral("tools/nemotron/fetch_and_convert.py"));
    if (!walk.cdUp())
      break;
  }

  QString script;
  for (const QString &c : scriptCandidates) {
    if (QFileInfo::exists(c)) {
      script = QFileInfo(c).absoluteFilePath();
      break;
    }
  }
  if (script.isEmpty())
    return false;

  const QString python =
      QStandardPaths::findExecutable(QStringLiteral("python")) +
      QString(); // may be empty
  QString py = QStandardPaths::findExecutable(QStringLiteral("python"));
  if (py.isEmpty())
    py = QStandardPaths::findExecutable(QStringLiteral("python3"));
  if (py.isEmpty())
    py = QStringLiteral("python");

  if (m_hfHelperProcess) {
    m_hfHelperProcess->kill();
    m_hfHelperProcess->deleteLater();
    m_hfHelperProcess = nullptr;
  }
  m_hfHelperProcess = new QProcess(this);
  connect(m_hfHelperProcess, &QProcess::readyReadStandardOutput, this,
          &LocalModelManager::onHfHelperReadyRead);
  connect(m_hfHelperProcess, &QProcess::readyReadStandardError, this,
          &LocalModelManager::onHfHelperReadyRead);
  connect(m_hfHelperProcess,
          QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
          &LocalModelManager::onHfHelperFinished);

  emit statusMessage(
      QStringLiteral("Downloading Nemotron 3.5 GGUF from Hugging Face..."));
  emit progressChanged(m_modelName, 1,
                       QStringLiteral("Starting Hugging Face download..."));

  m_hfHelperProcess->setProgram(py);
  m_hfHelperProcess->setArguments(
      {script, QStringLiteral("--dest"), QDir::toNativeSeparators(dest),
       QStringLiteral("--quant"), QStringLiteral("Q8_0")});
  m_hfHelperProcess->setWorkingDirectory(QFileInfo(script).absolutePath());
  m_hfHelperProcess->setProcessChannelMode(QProcess::MergedChannels);
  m_hfHelperProcess->start();
  if (!m_hfHelperProcess->waitForStarted(8000)) {
    const QString err = m_hfHelperProcess->errorString();
    m_hfHelperProcess->deleteLater();
    m_hfHelperProcess = nullptr;
    emit statusMessage(
        QStringLiteral("HF helper failed to start (%1); falling back.").arg(err));
    return false;
  }
  Q_UNUSED(python);
  return true;
}

void LocalModelManager::onHfHelperReadyRead() {
  if (!m_hfHelperProcess)
    return;
  const QByteArray raw = m_hfHelperProcess->readAll();
  const QString text = QString::fromUtf8(raw);
  const QStringList lines =
      text.split(QRegularExpression(QStringLiteral("[\r\n]+")),
                 Qt::SkipEmptyParts);
  for (const QString &line : lines) {
    if (line.startsWith(QStringLiteral("PROGRESS "))) {
      // PROGRESS <pct> <message...>
      const QString rest = line.mid(9).trimmed();
      const int sp = rest.indexOf(QLatin1Char(' '));
      int pct = 0;
      QString msg = rest;
      if (sp > 0) {
        pct = rest.left(sp).toInt();
        msg = rest.mid(sp + 1);
      } else {
        pct = rest.toInt();
        msg = QStringLiteral("Downloading...");
      }
      emit progressChanged(m_modelName, pct, msg);
      emit statusMessage(msg);
    } else if (line.startsWith(QStringLiteral("STATUS "))) {
      emit statusMessage(line.mid(7));
    } else if (line.startsWith(QStringLiteral("ERROR "))) {
      emit statusMessage(line.mid(6));
    } else if (line.startsWith(QStringLiteral("DONE "))) {
      emit statusMessage(QStringLiteral("Installed to %1").arg(line.mid(5)));
    } else if (!line.trimmed().isEmpty()) {
      emit statusMessage(line.trimmed());
    }
  }
}

void LocalModelManager::onHfHelperFinished(int exitCode,
                                           QProcess::ExitStatus exitStatus) {
  if (m_hfHelperProcess) {
    m_hfHelperProcess->deleteLater();
    m_hfHelperProcess = nullptr;
  }
  if (exitStatus != QProcess::NormalExit || exitCode != 0) {
    failCurrent(QStringLiteral(
        "Nemotron Hugging Face download failed (exit %1). "
        "Ensure Python + huggingface_hub are installed.")
                    .arg(exitCode));
    return;
  }
  // Success — advance package sequence.
  startNextPackage();
}

void LocalModelManager::tryCurrentUrl() {
  if (m_reply) {
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
  }

  const QStringList urls = currentPackageUrls();
  if (urls.isEmpty()) {
    const QString packageName =
        localModelPackage(m_packageSequence.value(m_packageIndex)).displayName;
    failCurrent(QStringLiteral("No reachable download source for %1.").arg(packageName));
    return;
  }
  ++m_urlIndex;
  if (m_urlIndex >= urls.size()) {
    const QString packageName =
        localModelPackage(m_packageSequence.value(m_packageIndex)).displayName;
    failCurrent(QStringLiteral("No reachable download source for %1.").arg(packageName));
    return;
  }

  const QUrl url(urls[m_urlIndex]);
  QNetworkRequest request(url);
  // Large GGUF (~700MB+): disable the short default timeout.
  request.setTransferTimeout(0);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  emit statusMessage(QStringLiteral("Fetching %1...").arg(url.toString()));

  const LocalModelPackageInfo package =
      localModelPackage(m_packageSequence.value(m_packageIndex));
  QString sourceName = QFileInfo(package.serverRelativePath).fileName();
  if (sourceName.isEmpty())
    sourceName = QFileInfo(url.path()).fileName();
  if (sourceName.isEmpty())
    sourceName = QStringLiteral("%1.zip").arg(cleanIdPart(package.id));

  const QString tmpName = QStringLiteral("QuickSTT_%1_%2_%3")
                              .arg(cleanIdPart(m_modelName))
                              .arg(m_packageIndex)
                              .arg(sourceName);
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

void LocalModelManager::startExtraction(const QString &archivePath,
                                         const QString &destinationRoot) {
  if (m_extractProcess) {
    m_extractProcess->kill();
    m_extractProcess->deleteLater();
    m_extractProcess = nullptr;
  }

  m_extractProcess = new QProcess(this);
  connect(m_extractProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
          this, &LocalModelManager::onExtractionFinished);

  const QString lowerPath = archivePath.toLower();
  if (lowerPath.endsWith(QStringLiteral(".tar.bz2")) ||
      lowerPath.endsWith(QStringLiteral(".tar.gz")) ||
      lowerPath.endsWith(QStringLiteral(".tgz")) ||
      lowerPath.endsWith(QStringLiteral(".tar"))) {
    QStringList args;
    args << QStringLiteral("-xf") << QDir::toNativeSeparators(archivePath)
         << QStringLiteral("-C") << QDir::toNativeSeparators(destinationRoot);
    m_extractProcess->start(QStringLiteral("tar"), args);
  } else if (lowerPath.endsWith(QStringLiteral(".zip"))) {
#ifdef Q_OS_WIN
    QStringList args;
    args << QStringLiteral("-NoProfile") << QStringLiteral("-ExecutionPolicy")
         << QStringLiteral("Bypass") << QStringLiteral("-Command")
         << QStringLiteral("Expand-Archive -LiteralPath %1 -DestinationPath %2 -Force")
                .arg(psQuoted(QDir::toNativeSeparators(archivePath)),
                     psQuoted(QDir::toNativeSeparators(destinationRoot)));
    m_extractProcess->start(QStringLiteral("powershell"), args);
#else
    // Linux: Vosk models are zips. Prefer unzip, fall back to bsdtar/python.
    if (!QStandardPaths::findExecutable(QStringLiteral("unzip")).isEmpty()) {
      QStringList args;
      args << QStringLiteral("-o") << QDir::toNativeSeparators(archivePath)
           << QStringLiteral("-d") << QDir::toNativeSeparators(destinationRoot);
      m_extractProcess->start(QStringLiteral("unzip"), args);
    } else if (!QStandardPaths::findExecutable(QStringLiteral("bsdtar")).isEmpty()) {
      QStringList args;
      args << QStringLiteral("-xf") << QDir::toNativeSeparators(archivePath)
           << QStringLiteral("-C") << QDir::toNativeSeparators(destinationRoot);
      m_extractProcess->start(QStringLiteral("bsdtar"), args);
    } else {
      // Python fallback — always available on Ubuntu.
      QString py = QStandardPaths::findExecutable(QStringLiteral("python3"));
      if (py.isEmpty())
        py = QStandardPaths::findExecutable(QStringLiteral("python"));
      if (py.isEmpty())
        py = QStringLiteral("python3");
      QStringList args;
      args << QStringLiteral("-c")
           << QStringLiteral(
                  "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])")
           << QDir::toNativeSeparators(archivePath)
           << QDir::toNativeSeparators(destinationRoot);
      m_extractProcess->start(py, args);
    }
#endif
  } else {
    QStringList args;
    args << QStringLiteral("-NoProfile") << QStringLiteral("-ExecutionPolicy")
         << QStringLiteral("Bypass") << QStringLiteral("-Command")
         << QStringLiteral("Expand-Archive -LiteralPath %1 -DestinationPath %2 -Force")
                .arg(psQuoted(QDir::toNativeSeparators(archivePath)),
                     psQuoted(QDir::toNativeSeparators(destinationRoot)));
    m_extractProcess->start(QStringLiteral("powershell"), args);
  }
}

void LocalModelManager::onExtractionFinished(int exitCode,
                                              QProcess::ExitStatus exitStatus) {
  if (m_extractProcess) {
    m_extractProcess->deleteLater();
    m_extractProcess = nullptr;
  }

  const bool ok = (exitStatus == QProcess::NormalExit && exitCode == 0);
  const QString destinationRoot = m_pendingExtractionDest;
  m_pendingExtractionDest.clear();

  QFile::remove(m_currentDownloadFile);
  m_currentDownloadFile.clear();

  if (!ok) {
    const QString packageId = m_packageSequence.value(m_packageIndex);
    const QString packageName = localModelPackage(packageId).displayName;
    failCurrent(QStringLiteral("Package extraction failed for %1.").arg(packageName));
    return;
  }

  const QString packageId = m_packageSequence.value(m_packageIndex);
  const LocalModelPackageInfo package = localModelPackage(packageId);
  const QString installRoot = installRootPathForKey(package.installRootKey);

  bool markersPresent = !package.installMarkers.isEmpty();
  for (const QString &marker : package.installMarkers) {
    if (!QFileInfo::exists(QDir(installRoot).filePath(marker))) {
      markersPresent = false;
      break;
    }
  }
  if (!markersPresent)
    flattenSingleArchiveRoot(destinationRoot);

  startNextPackage();
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
  if (!package.directUrl.isEmpty()) {
    urls << package.directUrl;
  }
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
  urls.removeDuplicates();
  return urls;
}
