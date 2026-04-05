#include "local_transcription_manager.h"

#include "local_model_support.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

namespace {

QString cleanedTranscript(QString text) {
  text.replace("\r\n", "\n");
  text.replace('\r', '\n');
  QStringList lines;
  for (const QString &line : text.split('\n')) {
    const QString trimmed = line.trimmed();
    if (!trimmed.isEmpty())
      lines << trimmed;
  }
  return lines.join(' ').simplified();
}

QString selectedComputeTargetId() {
  QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
  const QVector<ComputeTargetInfo> targets = detectComputeTargets();
  QString targetId = settings.value(QStringLiteral("computeTargetId")).toString();
  if (targetId.isEmpty())
    targetId = defaultComputeTargetId(targets);
  return targetId;
}

QString backendKeyForTarget(const ComputeTargetInfo &target) {
  if (target.isCpuFallback)
    return QStringLiteral("cpu");
  const QString vendor = target.vendorName.toLower();
  if (vendor.contains(QStringLiteral("nvidia")) && target.whisperGpuSupported)
    return QStringLiteral("nvidia_cuda");
  if (vendor.contains(QStringLiteral("intel")) && target.whisperGpuSupported)
    return QStringLiteral("intel_openvino");
  if (vendor.contains(QStringLiteral("amd")) && target.whisperGpuSupported)
    return QStringLiteral("amd_vulkan");
  return QStringLiteral("cpu");
}

int selectedBackendDeviceIndex(const QString &backendKey) {
  const QVector<ComputeTargetInfo> targets = detectComputeTargets();
  const QString targetId = selectedComputeTargetId();
  int backendIndex = 0;
  for (const ComputeTargetInfo &target : targets) {
    if (target.isCpuFallback)
      continue;
    if (backendKeyForTarget(target) != backendKey)
      continue;
    if (target.id == targetId)
      return backendIndex;
    ++backendIndex;
  }
  return 0;
}

} // namespace

LocalTranscriptionManager::LocalTranscriptionManager(QObject *parent)
    : QObject(parent) {
  m_process = new QProcess(this);
  connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this,
          &LocalTranscriptionManager::onProcessFinished);
  connect(m_process, &QProcess::readyReadStandardOutput, this,
          &LocalTranscriptionManager::onReadyReadStandardOutput);
  connect(m_process, &QProcess::readyReadStandardError, this,
          &LocalTranscriptionManager::onReadyReadStandardError);
}

void LocalTranscriptionManager::transcribeFile(const QString &modelName,
                                               const QString &audioPath) {
  if (m_busy) {
    emit transcriptionFailed(
        QStringLiteral("Another local transcription request is already running."));
    return;
  }

  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (descriptor.displayName.isEmpty() ||
      !localModelRequiresFrontendTranscription(modelName)) {
    emit transcriptionFailed(QStringLiteral("Unsupported local transcription model."));
    return;
  }

  const QString runtimePath =
      QDir(quickSttWhisperRoot()).filePath(localModelRuntimeExecutablePath(modelName));
  if (!QFileInfo::exists(runtimePath)) {
    emit transcriptionFailed(QStringLiteral("Runtime missing for %1.").arg(modelName));
    return;
  }

  const QString installedPath =
      QDir(quickSttWhisperRoot()).filePath(localModelInstalledPath(modelName));
  const QFileInfo installedInfo(installedPath);
  const QString artifactPath =
      installedInfo.exists() ? installedPath : installedInfo.absolutePath();
  if (!QFileInfo::exists(artifactPath)) {
    emit transcriptionFailed(QStringLiteral("Model files are missing for %1.").arg(modelName));
    return;
  }

  m_busy = true;
  m_modelName = modelName.trimmed();
  m_audioPath = audioPath.trimmed();
  m_stdoutBuffer.clear();
  m_stderrBuffer.clear();
  m_outputBase.clear();

  QStringList args;
  if (descriptor.engineFamily == QStringLiteral("whisper.cpp")) {
    m_outputBase = QDir(QDir::tempPath())
                       .filePath(QStringLiteral("quickstt_whisper_%1")
                                     .arg(QDateTime::currentMSecsSinceEpoch()));
    args << QStringLiteral("--model")
         << QDir::toNativeSeparators(installedPath)
         << QStringLiteral("--file")
         << QDir::toNativeSeparators(m_audioPath)
         << QStringLiteral("--output-txt")
         << QStringLiteral("--output-file")
         << QDir::toNativeSeparators(m_outputBase)
         << QStringLiteral("--no-timestamps")
         << QStringLiteral("--no-prints");

    if (descriptor.backendKey == QStringLiteral("cpu")) {
      args << QStringLiteral("--no-gpu");
    } else if (descriptor.backendKey == QStringLiteral("intel_openvino")) {
      args << QStringLiteral("--ov-e-device") << QStringLiteral("GPU");
    } else if (descriptor.backendKey == QStringLiteral("nvidia_cuda")) {
      args << QStringLiteral("--device")
           << QString::number(selectedBackendDeviceIndex(QStringLiteral("nvidia_cuda")));
    } else if (descriptor.backendKey == QStringLiteral("amd_vulkan")) {
      args << QStringLiteral("--device")
           << QString::number(selectedBackendDeviceIndex(QStringLiteral("amd_vulkan")));
    }
  } else if (descriptor.engineFamily == QStringLiteral("faster-whisper")) {
    args << QStringLiteral("transcribe")
         << QStringLiteral("--model-dir")
         << QDir::toNativeSeparators(installedInfo.absolutePath())
         << QStringLiteral("--audio")
         << QDir::toNativeSeparators(m_audioPath)
         << QStringLiteral("--device")
         << (descriptor.backendKey == QStringLiteral("nvidia_cuda")
                 ? QStringLiteral("cuda")
                 : QStringLiteral("cpu"))
         << QStringLiteral("--device-index")
         << QString::number(descriptor.backendKey == QStringLiteral("nvidia_cuda")
                                ? selectedBackendDeviceIndex(QStringLiteral("nvidia_cuda"))
                                : 0)
         << QStringLiteral("--compute-type")
         << (descriptor.backendKey == QStringLiteral("cpu")
                 ? QStringLiteral("int8")
                 : QStringLiteral("auto"));
  }

  emit statusChanged(QStringLiteral("Transcribing with %1...").arg(modelName));
  m_process->start(runtimePath, args);
  if (!m_process->waitForStarted(5000)) {
    fail(QStringLiteral("Failed to start local transcription runtime."));
  }
}

void LocalTranscriptionManager::onProcessFinished(
    int exitCode, QProcess::ExitStatus exitStatus) {
  const LocalModelDescriptor descriptor = localModelDescriptor(m_modelName);
  const QString stdoutText = QString::fromUtf8(m_stdoutBuffer);
  const QString stderrText = QString::fromUtf8(m_stderrBuffer);

  if (exitStatus != QProcess::NormalExit || exitCode != 0) {
    const QString message =
        cleanedTranscript(stderrText.isEmpty() ? stdoutText : stderrText);
    fail(message.isEmpty() ? QStringLiteral("Local transcription failed.")
                           : message);
    return;
  }

  QString transcript;
  if (descriptor.engineFamily == QStringLiteral("whisper.cpp")) {
    QFile txtFile(m_outputBase + QStringLiteral(".txt"));
    if (txtFile.open(QIODevice::ReadOnly))
      transcript = QString::fromUtf8(txtFile.readAll());
    txtFile.close();
    QFile::remove(txtFile.fileName());
  } else {
    transcript = stdoutText;
  }

  transcript = cleanedTranscript(transcript);
  if (transcript.isEmpty()) {
    fail(QStringLiteral("Local transcription produced no text."));
    return;
  }

  emit transcriptionReady(transcript);
  resetState();
}

void LocalTranscriptionManager::onReadyReadStandardOutput() {
  if (m_process)
    m_stdoutBuffer += m_process->readAllStandardOutput();
}

void LocalTranscriptionManager::onReadyReadStandardError() {
  if (m_process)
    m_stderrBuffer += m_process->readAllStandardError();
}

void LocalTranscriptionManager::fail(const QString &errorText) {
  emit transcriptionFailed(errorText);
  resetState();
}

void LocalTranscriptionManager::resetState() {
  if (m_process && m_process->state() != QProcess::NotRunning)
    m_process->kill();
  m_busy = false;
  m_modelName.clear();
  m_audioPath.clear();
  m_outputBase.clear();
  m_stdoutBuffer.clear();
  m_stderrBuffer.clear();
}
