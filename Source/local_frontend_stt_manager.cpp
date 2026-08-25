#include "local_frontend_stt_manager.h"

#include "local_model_support.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QElapsedTimer>

namespace {

// Idle timeout for CrispASR server: 30 seconds of no activity -> auto-stop
constexpr int kCrispAsrIdleTimeoutMs = 30000;

QString nativePath(const QString &path) {
  return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
}

QString appAudioPreprocessRoot() {
  const QString appDir = QCoreApplication::applicationDirPath();
  const QStringList roots = {
      QDir(appDir).filePath(QStringLiteral("audio_preprocess")),
      QDir(appDir).filePath(QStringLiteral("data/audio_preprocess")),
      QDir(appDir).filePath(QStringLiteral("../third_party/audio_preprocess")),
  };
  for (const QString &root : roots) {
    if (QFileInfo::exists(root))
      return QDir::cleanPath(root);
  }
  return QString();
}

QString newestWavInDirectory(const QString &dirPath) {
  QDir dir(dirPath);
  QFileInfoList files = dir.entryInfoList({QStringLiteral("*.wav")},
                                          QDir::Files, QDir::Time);
  return files.isEmpty() ? QString() : files.first().absoluteFilePath();
}

bool isNoiseLine(const QString &line, const QString &audioPath = QString()) {
  if (line.isEmpty())
    return true;

  static const QStringList exactSkips = {
      QStringLiteral("Started"),     QStringLiteral("Done!"),
      QStringLiteral("Creating recognizer ..."),
      QStringLiteral("recognizer created in 0.000 s"),
      QStringLiteral("----")};
  if (exactSkips.contains(line))
    return true;

  if (!audioPath.isEmpty() && (line == audioPath || line.endsWith(QFileInfo(audioPath).fileName())))
    return true;

  static const QStringList startsWithSkips = {
      QStringLiteral("OfflineRecognizerConfig("),
      QStringLiteral("num threads:"),
      QStringLiteral("decoding method:"),
      QStringLiteral("Elapsed seconds:"),
      QStringLiteral("Real time factor"),
      QStringLiteral("Creating recognizer"),
      QStringLiteral("recognizer created in"),
      QStringLiteral("/project/"),
      QStringLiteral("/workspace/"),
       QStringLiteral("/Users/"),
       QStringLiteral("C:\\"),
       QStringLiteral("D:\\"),
       QStringLiteral("in_sample_rate:"),
       QStringLiteral("output_sample_rate:"),
       QStringLiteral("whisper_"),
       QStringLiteral("system_info:"),
       QStringLiteral("main:"),
       QStringLiteral("ggml_"),
       QStringLiteral("operator():"),
       QStringLiteral("parse-options.cc:Read:"),
       QStringLiteral("sherpa-onnx-offline.exe"),
       QStringLiteral("--moonshine-encoder="),
       QStringLiteral("--moonshine-merged-decoder="),
       QStringLiteral("--nemo-ctc-model="),
       QStringLiteral("--encoder="),
       QStringLiteral("--decoder="),
       QStringLiteral("--joiner="),
       QStringLiteral("--tokens="),
       QStringLiteral("-vo "),
       QStringLiteral("-vsd "),
       QStringLiteral("-fa,"),
       QStringLiteral("-np,"),
       QStringLiteral("-mc "),
       QStringLiteral("-bo "),
       QStringLiteral("-bs ")};
  for (const QString &prefix : startsWithSkips) {
    if (line.startsWith(prefix))
      return true;
  }

  if (line.contains(QStringLiteral(".onnx")) ||
      line.contains(QStringLiteral(".ort")) ||
      line.contains(QStringLiteral("tokens.txt")) ||
      line.contains(QStringLiteral("decoder_model_merged")) ||
      line.contains(QStringLiteral("encoder_model")) ||
      line.contains(QStringLiteral("greedy_search"))) {
    return true;
  }

  // JSON output lines from sherpa-onnx (e.g. {"text":"","timestamps":[],...})
  // are metadata, not transcription text. transcriptFromJsonLine() already
  // extracts the text field; anything that reaches here had an empty text.
  if (line.startsWith(QLatin1Char('{')) && line.endsWith(QLatin1Char('}')))
    return true;

  // Whisper hallucinations on short/quiet audio: [BLANK_AUDIO], (silence), etc.
  if ((line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) ||
      (line.startsWith(QLatin1Char('(')) && line.endsWith(QLatin1Char(')'))))
    return true;

  // Parakeet common hallucinations on silence/noise
  const QString cleanMatch = line.toLower().trimmed();
  if (cleanMatch == QStringLiteral("hey.") ||
      cleanMatch == QStringLiteral("hey") ||
      cleanMatch == QStringLiteral("thank you.") ||
      cleanMatch == QStringLiteral("thank you") ||
      cleanMatch == QStringLiteral("thank you for watching.") ||
      cleanMatch == QStringLiteral("bye.") ||
      cleanMatch == QStringLiteral("amén.") ||
      cleanMatch == QStringLiteral("amen.")) {
    return true;
  }

  return false;
}

QString transcriptFromJsonLine(const QString &line) {
  QJsonParseError error;
  const QJsonDocument doc =
      QJsonDocument::fromJson(line.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject())
    return QString();
  const QString text = doc.object().value(QStringLiteral("text")).toString().trimmed();
  return text;
}

QString sanitizeTranscript(QString text) {
  text = text.trimmed();
  if (text.startsWith(QLatin1Char('"')) && text.endsWith(QLatin1Char('"')) &&
      text.size() >= 2) {
    text = text.mid(1, text.size() - 2).trimmed();
  }
  text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
  return text.trimmed();
}

QString filterHallucinations(const QString &text, const QString &modelName) {
  if (text.isEmpty()) return text;
  
  // Models known to hallucinate filler words
  bool isGGUFModel = modelName.contains(QStringLiteral("Nemotron")) ||
                     modelName.contains(QStringLiteral("Parakeet")) ||
                     modelName.contains(QStringLiteral("Whisper"));
  
  if (!isGGUFModel) return text;
  
  QString result = text;
  
  // Model-specific hallucination patterns
  if (modelName.contains(QStringLiteral("Parakeet")) ||
      modelName.contains(QStringLiteral("Nemotron"))) {
    // Parakeet known hallucinations
    static const QStringList parakeetHallucinations = {
      QStringLiteral("hey"), QStringLiteral("hey."), QStringLiteral("thank you"),
      QStringLiteral("thank you."), QStringLiteral("bye"), QStringLiteral("bye."),
      QStringLiteral("amen"), QStringLiteral("amen."), QStringLiteral("amén"),
      QStringLiteral("amén.")
    };
    for (const QString &hallucination : parakeetHallucinations) {
      QRegularExpression re(QStringLiteral("\\b") + QRegularExpression::escape(hallucination) + QStringLiteral("\\b"),
                            QRegularExpression::CaseInsensitiveOption);
      result.replace(re, QString());
    }
  } else {
    // Generic GGUF model hallucination filter
    static const QStringList genericHallucinations = {
      QStringLiteral("you"), QStringLiteral("hey"), QStringLiteral("uh"),
      QStringLiteral("um"), QStringLiteral("ah"), QStringLiteral("oh"),
      QStringLiteral("hmm"), QStringLiteral("hm"), QStringLiteral("mm")
    };
    for (const QString &hallucination : genericHallucinations) {
      QRegularExpression re(QStringLiteral("\\b") + QRegularExpression::escape(hallucination) + QStringLiteral("\\b"),
                            QRegularExpression::CaseInsensitiveOption);
      result.replace(re, QString());
    }
  }
  
  // Remove repeated words (e.g. "you you you" -> "")
  result.replace(QRegularExpression(QStringLiteral("(\\b\\w+\\b)(\\s+\\1)+"),
                                   QRegularExpression::CaseInsensitiveOption),
                 QString());
  
  // Clean up multiple spaces
  result.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
  return result.trimmed();
}

} // namespace

static void feLog(const QString &msg) {
  QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/fe_stt.log"));
  if (f.open(QIODevice::Append | QIODevice::Text)) {
    f.write(QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8());
    f.write(" ");
    f.write(msg.toUtf8());
    f.write("\n");
    f.close();
  }
}

LocalFrontendSttManager::LocalFrontendSttManager(QObject *parent)
    : QObject(parent), m_process(new QProcess(this)) {
  connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
    m_stdoutBuffer += m_process->readAllStandardOutput();
  });
  connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
    m_stderrBuffer += m_process->readAllStandardError();
  });
  connect(m_process, &QProcess::finished, this,
          &LocalFrontendSttManager::onProcessFinished);
  connect(m_process, &QProcess::errorOccurred, this,
          &LocalFrontendSttManager::onProcessError);
}

void LocalFrontendSttManager::transcribeFile(const QString &modelName,
                                             const QString &audioPath) {
  const QString cleanModel = canonicalLocalModelName(modelName);
  const QString cleanAudio = audioPath.trimmed();
  if (m_busy) {
    m_pendingJobs.enqueue({cleanModel, cleanAudio});
    emit statusChanged(QStringLiteral("Queued %1 (%2 waiting).")
                           .arg(cleanModel)
                           .arg(m_pendingJobs.size()));
    return;
  }

  startTranscription(cleanModel, cleanAudio);
}

bool LocalFrontendSttManager::startTranscription(const QString &cleanModel,
                                                 const QString &cleanAudio) {
  if (cleanModel.isEmpty() || cleanAudio.isEmpty())
    return false;

  if (!localModelUsesFrontendTranscriber(cleanModel)) {
    emit transcriptionFailed(QStringLiteral("Selected model is not a frontend local recognizer."));
    return false;
  }

  QString preprocessDir;
  const QString runtimeAudio =
      preprocessAudioIfAvailable(cleanModel, cleanAudio, &preprocessDir);

  if (cleanModel == QStringLiteral("NVIDIA Parakeet TDT 0.6B v3 INT8 (ONNX/Rust)") ||
      cleanModel == QStringLiteral("NVIDIA Nemotron 3.5 ASR Streaming 0.6B") ||
      cleanModel.contains(QStringLiteral("Nemotron"), Qt::CaseInsensitive)) {
      m_busy = true;
      m_activeModelName = cleanModel;
      m_activeOriginalAudioPath = cleanAudio;
      m_activeAudioPath = runtimeAudio;
      m_activePreprocessDir = preprocessDir;

      // Respawn if worker engine process died or was killed.
      if (!ensureParakeetProcessRunning(cleanModel)) {
        finishFailure(QStringLiteral("Failed to start speech engine worker."));
        return false;
      }

      QJsonObject req;
      req["action"] = "transcribe";
      req["audio_path"] = runtimeAudio;
      if (m_parakeetProcess->write(
              QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n") < 0 ||
          !m_parakeetProcess->waitForBytesWritten(2000)) {
        feLog(QStringLiteral("Engine write failed; respawning worker"));
        discardDeadParakeetProcess();
        if (!ensureParakeetProcessRunning(cleanModel) ||
            m_parakeetProcess->write(
                QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n") < 0) {
          finishFailure(QStringLiteral("Speech engine worker died and could not be restarted."));
          return false;
        }
      }
      
      emit statusChanged(QStringLiteral("Transcribing with %1...").arg(cleanModel));
      return true;
  }

  // --- Whisper.cpp models (launched as direct process, not server) ---
  if (cleanModel.startsWith(QStringLiteral("Whisper "))) {
      m_busy = true;
      m_activeModelName = cleanModel;
      m_activeOriginalAudioPath = cleanAudio;
      m_activeAudioPath = runtimeAudio;
      m_activePreprocessDir = preprocessDir;

      QString executablePath = executablePathForModel(cleanModel);
      QStringList arguments = argumentsForModel(cleanModel, runtimeAudio);
      feLog(QStringLiteral("startTranscription Whisper.cpp model=%1 audio=%2 exe=%3 exists=%4")
                .arg(cleanModel, runtimeAudio, executablePath,
                     QFileInfo::exists(executablePath) ? QStringLiteral("Y") : QStringLiteral("N")));
      if (!QFileInfo::exists(executablePath)) {
        emit transcriptionFailed(
            QStringLiteral("Local runtime is missing for %1.").arg(cleanModel));
        return false;
      }
      if (arguments.isEmpty()) {
        emit transcriptionFailed(
            QStringLiteral("Unsupported local model wiring for %1.").arg(cleanModel));
        return false;
      }

      m_process->setWorkingDirectory(modelRootForModel(cleanModel));
      m_process->setProgram(executablePath);
      m_process->setArguments(arguments);
      m_process->setProcessChannelMode(QProcess::SeparateChannels);
      m_stdoutBuffer.clear();
      m_stderrBuffer.clear();

      QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
      env.insert(QStringLiteral("OMP_WAIT_POLICY"), QStringLiteral("PASSIVE"));
      env.insert(QStringLiteral("ORT_DISABLE_SPINNING"), QStringLiteral("1"));
      env.insert(QStringLiteral("OMP_NUM_THREADS"),
                 QString::number(qMin(4, qMax(1, QThread::idealThreadCount() > 4
                                             ? QThread::idealThreadCount() / 2
                                             : QThread::idealThreadCount()))));
      m_process->setProcessEnvironment(env);

      m_activeModelName = cleanModel;
      m_activeAudioPath = runtimeAudio;
      m_activeOriginalAudioPath = cleanAudio;
      m_activePreprocessDir = preprocessDir;
      m_busy = true;

      emit statusChanged(QStringLiteral("Transcribing with %1...").arg(cleanModel));
      m_process->start();
      return true;
  }

  QString executablePath = executablePathForModel(cleanModel);
  feLog(QStringLiteral("startTranscription model=%1 audio=%2 exe=%3 exists=%4 wavSize=%5")
            .arg(cleanModel, runtimeAudio, executablePath,
                 QFileInfo::exists(executablePath) ? QStringLiteral("Y") : QStringLiteral("N"),
                 QString::number(QFileInfo(runtimeAudio).size())));
  if (!QFileInfo::exists(executablePath)) {
    emit transcriptionFailed(
        QStringLiteral("Local runtime is missing for %1.").arg(cleanModel));
    return false;
  }
  QStringList arguments = argumentsForModel(cleanModel, runtimeAudio);
  feLog(QStringLiteral("args: %1").arg(arguments.join(QStringLiteral(" "))));
  if (arguments.isEmpty()) {
    emit transcriptionFailed(
        QStringLiteral("Unsupported local model wiring for %1.")
            .arg(cleanModel));
    return false;
  }

  if (!QFileInfo::exists(runtimeAudio)) {
    emit transcriptionFailed(QStringLiteral("Audio snippet is missing."));
    return false;
  }

  m_process->setWorkingDirectory(modelRootForModel(cleanModel));
  m_process->setProgram(executablePath);
  m_process->setArguments(arguments);
  m_process->setProcessChannelMode(QProcess::SeparateChannels);
  m_stdoutBuffer.clear();
  m_stderrBuffer.clear();

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("OMP_WAIT_POLICY"), QStringLiteral("PASSIVE"));
  env.insert(QStringLiteral("ORT_DISABLE_SPINNING"), QStringLiteral("1"));
  env.insert(QStringLiteral("OMP_NUM_THREADS"),
             QString::number(qMin(4, qMax(1, QThread::idealThreadCount() > 4
                                         ? QThread::idealThreadCount() / 2
                                         : QThread::idealThreadCount()))));
  m_process->setProcessEnvironment(env);

  m_activeModelName = cleanModel;
  m_activeAudioPath = runtimeAudio;
  m_activeOriginalAudioPath = cleanAudio;
  m_activePreprocessDir = preprocessDir;
  m_busy = true;

  emit statusChanged(QStringLiteral("Transcribing with %1...").arg(cleanModel));
  m_process->start();
  return true;
}

void LocalFrontendSttManager::startNextPendingJob() {
  while (!m_busy && !m_pendingJobs.isEmpty()) {
    const PendingTranscriptionJob job = m_pendingJobs.dequeue();
    if (startTranscription(job.modelName, job.audioPath))
      return;
  }
}

QString LocalFrontendSttManager::executablePathForModel(
    const QString &modelName) const {
  return QDir(quickSttModelsRoot())
      .filePath(localModelRuntimeExecutablePath(modelName));
}

QString LocalFrontendSttManager::modelRootForModel(
    const QString &modelName) const {
  return QDir(quickSttModelsRoot()).filePath(localModelInstalledPath(modelName));
}

QStringList LocalFrontendSttManager::argumentsForModel(
    const QString &modelName, const QString &audioPath) const {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  const QString modelRoot = modelRootForModel(modelName);
  const QString backendKey = localModelBackendKey(modelName);
  const QString provider =
      backendKey == QStringLiteral("nvidia_cuda") ? QStringLiteral("cuda")
                                                  : QStringLiteral("cpu");
  const int idealThreads = QThread::idealThreadCount();
  const int inferThreads = qMin(4, qMax(1, idealThreads > 4 ? idealThreads / 2 : idealThreads));
  QStringList args = {QStringLiteral("--debug=0"),
                      QStringLiteral("--provider=%1").arg(provider),
                      QStringLiteral("--num-threads=%1").arg(inferThreads)};

  if (descriptor.engineFamily == QStringLiteral("moonshine_v2")) {
    args << QStringLiteral("--decoding-method=greedy_search");
    args << QStringLiteral("--moonshine-encoder=%1")
                .arg(nativePath(QDir(modelRoot).filePath(QStringLiteral("encoder_model.ort"))))
         << QStringLiteral("--moonshine-merged-decoder=%1")
                .arg(nativePath(QDir(modelRoot).filePath(QStringLiteral("decoder_model_merged.ort"))))
         << QStringLiteral("--tokens=%1")
                .arg(nativePath(QDir(modelRoot).filePath(QStringLiteral("tokens.txt"))))
         << nativePath(audioPath);
    return args;
  }

  if (descriptor.engineFamily == QStringLiteral("nemo_transducer")) {
    args << QStringLiteral("--decoding-method=greedy_search")
         << QStringLiteral("--model-type=transducer");
    args << QStringLiteral("--encoder=%1")
                .arg(nativePath(QDir(modelRoot).filePath(QStringLiteral("encoder.int8.onnx"))))
         << QStringLiteral("--decoder=%1")
                .arg(nativePath(QDir(modelRoot).filePath(QStringLiteral("decoder.int8.onnx"))))
         << QStringLiteral("--joiner=%1")
                .arg(nativePath(QDir(modelRoot).filePath(QStringLiteral("joiner.int8.onnx"))))
         << QStringLiteral("--tokens=%1")
                .arg(nativePath(QDir(modelRoot).filePath(QStringLiteral("tokens.txt"))))
         << nativePath(audioPath);
    return args;
  }

  if (descriptor.engineFamily == QStringLiteral("nemo_ctc")) {
    args << QStringLiteral("--decoding-method=greedy_search")
         << QStringLiteral("--model-type=nemo_ctc")
         << QStringLiteral("--tokens=%1")
                .arg(nativePath(QDir(modelRoot).filePath(QStringLiteral("tokens.txt"))))
         << QStringLiteral("--nemo-ctc-model=%1")
                .arg(nativePath(QDir(modelRoot).filePath(QStringLiteral("model.int8.onnx"))))
         << nativePath(audioPath);
    return args;
  }

  if (descriptor.engineFamily == QStringLiteral("nemo_ctc_openvino")) {
    return {};
  }

  if (descriptor.engineFamily == QStringLiteral("whisper_cpp")) {
    QString modelFile;
    const QDir modelDir(modelRoot);
    const QStringList binFiles =
        modelDir.entryList({QStringLiteral("ggml-*.bin")}, QDir::Files);
    if (!binFiles.isEmpty())
      modelFile = nativePath(modelDir.filePath(binFiles.first()));
    else
      modelFile = nativePath(modelDir.filePath(QStringLiteral("model.bin")));
    const QString lowerModel = modelFile.toLower();
    const bool englishOnly =
        lowerModel.contains(QStringLiteral(".en")) ||
        descriptor.variantKey.contains(QStringLiteral("_en"));
    QStringList whisperArgs = {
        QStringLiteral("-m"), modelFile,
        QStringLiteral("-f"), nativePath(audioPath),
        QStringLiteral("--language"), englishOnly ? QStringLiteral("en")
                                                   : QStringLiteral("auto"),
        QStringLiteral("--no-timestamps"),
        QStringLiteral("--no-prints"),
        QStringLiteral("-tp"), QStringLiteral("0.0"),
        QStringLiteral("-bo"), QStringLiteral("3"),
        QStringLiteral("-bs"), QStringLiteral("5"),
        QStringLiteral("-t"), QString::number(inferThreads)};
    return whisperArgs;
  }

  return {};
}

QString LocalFrontendSttManager::preprocessAudioIfAvailable(
    const QString &modelName, const QString &audioPath,
    QString *preprocessDir) const {
  if (preprocessDir)
    preprocessDir->clear();

  if (!shouldRunDeepFilterForModel(modelName))
    return audioPath;

  const QString root = appAudioPreprocessRoot();
  if (root.isEmpty())
    return audioPath;

  const QString exe =
      QDir(root).filePath(QStringLiteral("deepfilter/deep-filter.exe"));
  const QString model =
      QDir(root).filePath(QStringLiteral("deepfilter/DeepFilterNet3.tar.gz"));
  if (!QFileInfo::exists(exe) || !QFileInfo::exists(model))
    return audioPath;

  const QString cacheRoot =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  const QString outDir =
      QDir(cacheRoot).filePath(QStringLiteral("deepfilter_%1")
                                   .arg(QDateTime::currentMSecsSinceEpoch()));
  QDir().mkpath(outDir);

  QProcess process;
  process.setProgram(exe);
  process.setArguments({QStringLiteral("-m"), nativePath(model),
                        QStringLiteral("-D"), QStringLiteral("--pf"),
                        QStringLiteral("-a"), QStringLiteral("18"),
                        QStringLiteral("-o"), nativePath(outDir),
                        nativePath(audioPath)});
  process.setProcessChannelMode(QProcess::SeparateChannels);
  feLog(QStringLiteral("deepfilter start exe=%1 model=%2 audio=%3")
            .arg(exe, model, audioPath));
  process.start();
  if (!process.waitForStarted(5000) || !process.waitForFinished(180000)) {
    process.kill();
    process.waitForFinished(2000);
    QDir(outDir).removeRecursively();
    feLog(QStringLiteral("deepfilter unavailable/timeout; using raw audio"));
    return audioPath;
  }

  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    feLog(QStringLiteral("deepfilter failed exit=%1 err=%2")
              .arg(process.exitCode())
              .arg(QString::fromUtf8(process.readAllStandardError()).left(300)));
    QDir(outDir).removeRecursively();
    return audioPath;
  }

  const QString enhanced = newestWavInDirectory(outDir);
  if (enhanced.isEmpty() || !QFileInfo::exists(enhanced)) {
    QDir(outDir).removeRecursively();
    return audioPath;
  }
  if (preprocessDir)
    *preprocessDir = outDir;
  feLog(QStringLiteral("deepfilter ok enhanced=%1").arg(enhanced));
  return enhanced;
}

bool LocalFrontendSttManager::shouldRunDeepFilterForModel(
    const QString &modelName) const {
  const QByteArray overrideValue =
      qgetenv("QUICKSTT_DEEPFILTER_FRONTEND").trimmed().toLower();
  if (overrideValue == "1" || overrideValue == "true" ||
      overrideValue == "on" || overrideValue == "yes")
    return true;
  if (overrideValue == "0" || overrideValue == "false" ||
      overrideValue == "off" || overrideValue == "no")
    return false;

  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  return descriptor.engineFamily != QStringLiteral("nemo_transducer") &&
         descriptor.engineFamily != QStringLiteral("nemo_ctc");
}

QString LocalFrontendSttManager::extractTranscript(
    const QString &modelName, const QString &audioPath, const QString &stdoutText,
    const QString &stderrText) const {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  const bool jsonOnlyModel =
      descriptor.engineFamily == QStringLiteral("nemo_ctc_openvino");



  const auto extractFromChannel = [&](const QString &channelText) {
    const QStringList lines = channelText.split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    QString lastFallback;
    for (const QString &rawLine : lines) {
      const QString line = sanitizeTranscript(rawLine);
      if (line.isEmpty())
        continue;

      const QString jsonText = sanitizeTranscript(transcriptFromJsonLine(line));
      if (!jsonText.isEmpty() && !isNoiseLine(jsonText, audioPath))
        return jsonText;

      if (jsonOnlyModel)
        continue;

      if (isNoiseLine(line, audioPath))
        continue;

      lastFallback = line;
    }
    return sanitizeTranscript(lastFallback);
  };

  const QString stdoutTranscript = extractFromChannel(stdoutText);
  if (!stdoutTranscript.isEmpty())
    return filterHallucinations(stdoutTranscript, modelName);

  const QString stderrTranscript = extractFromChannel(stderrText);
  return filterHallucinations(stderrTranscript, modelName);
}

void LocalFrontendSttManager::finishFailure(const QString &errorText) {
  cleanupActiveFiles();
  m_busy = false;
  m_activeModelName.clear();
  m_activeAudioPath.clear();
  m_activeOriginalAudioPath.clear();
  m_activePreprocessDir.clear();
  m_stdoutBuffer.clear();
  m_stderrBuffer.clear();
  emit transcriptionFailed(errorText);
  startNextPendingJob();
}

void LocalFrontendSttManager::onParakeetReadyRead() {
    if (!m_parakeetProcess) return;
    
    while (m_parakeetProcess->canReadLine()) {
        QByteArray line = m_parakeetProcess->readLine().trimmed();
        if (line.isEmpty()) continue;
        
        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            if (obj["status"].toString() == "ok") {
                if (obj.contains("text") && !obj["text"].isNull()) {
                    QString text = obj["text"].toString().trimmed();
                    if (!isNoiseLine(text) && !text.isEmpty()) {
                        emit transcriptionReady(text);
                    }
                    m_busy = false;
                    cleanupActiveFiles();
                    startNextPendingJob();
                } else {
                    // Response to 'load', ignore
                }
            } else if (obj["status"].toString() == "error") {
                QString err = obj["error"].toString();
                emit transcriptionFailed(QStringLiteral("Parakeet Rust Engine Error: %1").arg(err));
                m_busy = false;
                cleanupActiveFiles();
                startNextPendingJob();
            }
        }
    }
}

void LocalFrontendSttManager::cleanupActiveFiles() {
  if (!m_activeAudioPath.isEmpty())
    QFile::remove(m_activeAudioPath);
  if (!m_activeOriginalAudioPath.isEmpty() &&
      m_activeOriginalAudioPath != m_activeAudioPath)
    QFile::remove(m_activeOriginalAudioPath);
  if (!m_activePreprocessDir.isEmpty())
    QDir(m_activePreprocessDir).removeRecursively();
}

void LocalFrontendSttManager::onProcessFinished(int exitCode,
                                                QProcess::ExitStatus exitStatus) {
  m_stdoutBuffer += m_process->readAllStandardOutput();
  m_stderrBuffer += m_process->readAllStandardError();
  const QString stdoutText = QString::fromUtf8(m_stdoutBuffer);
  const QString stderrText = QString::fromUtf8(m_stderrBuffer);
  const QString modelName = m_activeModelName;
  const QString audioPath = m_activeAudioPath;

  if (exitStatus != QProcess::NormalExit || exitCode != 0) {
    feLog(QStringLiteral("FAILED exit=%1 stderr=%2").arg(exitCode).arg(stderrText.left(300)));
    const QString errorText =
        QStringLiteral("%1 failed (%2).")
            .arg(modelName.isEmpty() ? QStringLiteral("Local transcription")
                                     : modelName)
            .arg(exitCode);
    finishFailure(errorText);
    return;
  }

  const QString text =
      extractTranscript(modelName, audioPath, stdoutText, stderrText);
  feLog(QStringLiteral("OK stdout=%1 | extracted=%2").arg(stdoutText.left(200).trimmed(), text));
  cleanupActiveFiles();
  m_busy = false;
  m_activeModelName.clear();
  m_activeAudioPath.clear();
  m_activeOriginalAudioPath.clear();
  m_activePreprocessDir.clear();
  m_stdoutBuffer.clear();
  m_stderrBuffer.clear();

  if (text.trimmed().isEmpty()) {
    emit transcriptionFailed(
        QStringLiteral("%1 returned no transcription.").arg(modelName));
    startNextPendingJob();
    return;
  }

  emit statusChanged(QStringLiteral("%1 ready.").arg(modelName));
  emit transcriptionReady(text.trimmed());
  startNextPendingJob();
}

void LocalFrontendSttManager::onProcessError(QProcess::ProcessError error) {
  Q_UNUSED(error);
  qDebug() << "[LOCAL-FE] process ERROR:" << m_process->errorString();
  feLog(QStringLiteral("ERROR: %1").arg(m_process->errorString()));
  const QString message =
      m_process->errorString().trimmed().isEmpty()
          ? QStringLiteral("Failed to start the local transcription runtime.")
          : m_process->errorString().trimmed();
  finishFailure(message);
}

// ─── Worker process liveness (Handy-like respawn after external kill) ────

void LocalFrontendSttManager::discardDeadParakeetProcess() {
  if (!m_parakeetProcess)
    return;
  feLog(QStringLiteral("discardDeadParakeetProcess: dropping dead worker"));
  m_parakeetProcess->disconnect(this);
  if (m_parakeetProcess->state() != QProcess::NotRunning) {
    m_parakeetProcess->kill();
    m_parakeetProcess->waitForFinished(1500);
  }
  m_parakeetProcess->deleteLater();
  m_parakeetProcess = nullptr;
}

void LocalFrontendSttManager::discardDeadGgmlServerProcess() {
  if (!m_ggmlServerProcess)
    return;
  feLog(QStringLiteral("discardDeadGgmlServerProcess: dropping dead server"));
  // stopCrispAsrServer already cleans timers + pointer; reuse it.
  stopCrispAsrServer();
}

bool LocalFrontendSttManager::ensureParakeetProcessRunning(
    const QString &modelName) {
  if (m_parakeetProcess &&
      m_parakeetProcess->state() != QProcess::Running) {
    feLog(QStringLiteral("Parakeet worker not running (state=%1) — respawning")
              .arg(int(m_parakeetProcess->state())));
    discardDeadParakeetProcess();
  }

  if (!m_parakeetProcess) {
    m_parakeetProcess = new QProcess(this);
    connect(m_parakeetProcess, &QProcess::readyReadStandardOutput, this,
            &LocalFrontendSttManager::onParakeetReadyRead);
    connect(m_parakeetProcess, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
              feLog(QStringLiteral(
                        "Parakeet worker exited code=%1 status=%2")
                        .arg(exitCode)
                        .arg(int(status)));
              // Leave the pointer; next ensure will discard if not Running.
            });
    connect(m_parakeetProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
              feLog(QStringLiteral("Parakeet worker error=%1: %2")
                        .arg(int(error))
                        .arg(m_parakeetProcess
                                 ? m_parakeetProcess->errorString()
                                 : QStringLiteral("null")));
            });

    const bool isNemotron = modelName.contains(QStringLiteral("Nemotron"), Qt::CaseInsensitive);
    const QString relExe = isNemotron ? QStringLiteral("tools/nemotron/nemotron_engine.exe")
                                      : QStringLiteral("tools/parakeet/parakeet_engine.exe");
    const QString relDir = isNemotron ? QStringLiteral("tools/nemotron")
                                      : QStringLiteral("tools/parakeet");

    const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath(relExe);
    if (!QFile::exists(exe)) {
      feLog(QStringLiteral("Engine worker missing at %1").arg(exe));
      discardDeadParakeetProcess();
      return false;
    }

    m_parakeetProcess->setProgram(exe);
    m_parakeetProcess->setWorkingDirectory(
        QDir(QCoreApplication::applicationDirPath()).filePath(relDir));
    m_parakeetProcess->start();
    if (!m_parakeetProcess->waitForStarted(5000)) {
      feLog(QStringLiteral("Engine worker failed to start: %1")
                .arg(m_parakeetProcess->errorString()));
      discardDeadParakeetProcess();
      return false;
    }

    QJsonObject loadReq;
    loadReq[QStringLiteral("action")] = QStringLiteral("load");
    loadReq[QStringLiteral("model_path")] =
        nativePath(modelRootForModel(modelName));
    m_parakeetProcess->write(
        QJsonDocument(loadReq).toJson(QJsonDocument::Compact) + "\n");
    m_parakeetProcess->waitForBytesWritten(2000);
    // Give the model a brief moment to load before the first transcribe.
    m_parakeetProcess->waitForReadyRead(8000);
    feLog(QStringLiteral("Parakeet worker started pid=%1")
              .arg(m_parakeetProcess->processId()));
  }

  return m_parakeetProcess &&
         m_parakeetProcess->state() == QProcess::Running;
}

bool LocalFrontendSttManager::ensureGgmlServerRunning(
    const QString &modelName, const QString &modelAbsPath) {
  if (m_ggmlServerProcess &&
      m_ggmlServerProcess->state() != QProcess::Running) {
    feLog(QStringLiteral("CrispASR server not running — respawning"));
    discardDeadGgmlServerProcess();
  }

  if (!m_ggmlServerProcess) {
    m_ggmlServerProcess = new QProcess(this);
    const QString exe =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("tools/crispasr/crispasr-server.exe"));
    if (!QFile::exists(exe)) {
      feLog(QStringLiteral("CrispASR server missing at %1").arg(exe));
      discardDeadGgmlServerProcess();
      return false;
    }

    m_ggmlServerProcess->setProgram(exe);
    m_ggmlServerProcess->setWorkingDirectory(
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("tools/crispasr")));

    QStringList serverArgs = {QStringLiteral("-m"), nativePath(modelAbsPath),
                              QStringLiteral("--host"),
                              QStringLiteral("127.0.0.1"),
                              QStringLiteral("--port"), QStringLiteral("8095")};



    const int idealThreads = QThread::idealThreadCount();
    const int serverThreads =
        qMax(2, idealThreads > 4 ? idealThreads - 1 : idealThreads);
    serverArgs << QStringLiteral("--threads")
               << QString::number(serverThreads);

    m_ggmlServerProcess->setArguments(serverArgs);
    m_ggmlServerProcess->start();
    if (!m_ggmlServerProcess->waitForStarted(10000)) {
      feLog(QStringLiteral("CrispASR server failed to start: %1")
                .arg(m_ggmlServerProcess->errorString()));
      discardDeadGgmlServerProcess();
      return false;
    }
    m_ggmlServerProcess->waitForReadyRead(5000);
    m_ggmlServerActiveModel = modelName;
    feLog(QStringLiteral("CrispASR server started: model=%1 threads=%2 args=%3")
              .arg(modelName, QString::number(serverThreads),
                   serverArgs.join(QLatin1Char(' '))));
  } else if (m_ggmlServerActiveModel != modelName) {
    QProcess::execute(
        QStringLiteral("curl"),
        {QStringLiteral("-s"), QStringLiteral("http://127.0.0.1:8095/load"),
         QStringLiteral("-F"),
         QStringLiteral("model=%1").arg(nativePath(modelAbsPath))});
    m_ggmlServerActiveModel = modelName;
  }

  return m_ggmlServerProcess &&
         m_ggmlServerProcess->state() == QProcess::Running;
}

// ─── CrispASR server lifecycle ───────────────────────────────────────────

void LocalFrontendSttManager::stopCrispAsrServer() {
  if (m_crispAsrIdleTimer) {
    m_crispAsrIdleTimer->stop();
    delete m_crispAsrIdleTimer;
    m_crispAsrIdleTimer = nullptr;
  }

  if (!m_ggmlServerProcess)
    return;

  feLog(QStringLiteral("stopCrispAsrServer: stopping CrispASR server (pid=%1)")
            .arg(m_ggmlServerProcess->processId()));

  // Try graceful shutdown first
  m_ggmlServerProcess->terminate();
  if (!m_ggmlServerProcess->waitForFinished(3000)) {
    feLog(QStringLiteral("stopCrispAsrServer: force killing CrispASR server"));
    m_ggmlServerProcess->kill();
    m_ggmlServerProcess->waitForFinished(2000);
  }

  delete m_ggmlServerProcess;
  m_ggmlServerProcess = nullptr;
  m_ggmlServerActiveModel.clear();
  m_lastCrispAsrActivityMs = 0;

  feLog(QStringLiteral("stopCrispAsrServer: CrispASR server stopped"));
}

void LocalFrontendSttManager::shutdownAllModels() {
  feLog(QStringLiteral("shutdownAllModels: shutting down all model processes"));
  stopCrispAsrServer();

  if (m_parakeetProcess) {
    feLog(QStringLiteral("shutdownAllModels: stopping Parakeet process"));
    m_parakeetProcess->terminate();
    if (!m_parakeetProcess->waitForFinished(3000)) {
      m_parakeetProcess->kill();
      m_parakeetProcess->waitForFinished(2000);
    }
    delete m_parakeetProcess;
    m_parakeetProcess = nullptr;
  }

  if (m_process && m_process->state() == QProcess::Running) {
    feLog(QStringLiteral("shutdownAllModels: stopping main process"));
    m_process->terminate();
    if (!m_process->waitForFinished(3000)) {
      m_process->kill();
      m_process->waitForFinished(2000);
    }
  }

  m_busy = false;
  m_activeModelName.clear();
  m_activeAudioPath.clear();
  m_activeOriginalAudioPath.clear();
  m_activePreprocessDir.clear();
  m_stdoutBuffer.clear();
  m_stderrBuffer.clear();

  // Clear pending jobs
  while (!m_pendingJobs.isEmpty())
    m_pendingJobs.dequeue();

  feLog(QStringLiteral("shutdownAllModels: all models shut down"));
}

bool LocalFrontendSttManager::isAnyModelActive() const {
  return (m_ggmlServerProcess &&
          m_ggmlServerProcess->state() == QProcess::Running) ||
         (m_parakeetProcess &&
          m_parakeetProcess->state() == QProcess::Running) ||
         (m_process && m_process->state() == QProcess::Running);
}
