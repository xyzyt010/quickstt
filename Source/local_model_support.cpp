#include "local_model_support.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#endif

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

QString migrateLegacyModelName(QString text) {
  text = text.trimmed();
  if (text.compare(QStringLiteral("NeMo FastConformer TDT 0.6B"),
                   Qt::CaseInsensitive) == 0) {
    return QStringLiteral("NVIDIA Parakeet TDT 0.6B v3 INT8");
  }
  if (text.compare(QStringLiteral("NeMo FastConformer CTC 110M"),
                   Qt::CaseInsensitive) == 0) {
    return QStringLiteral("NVIDIA Parakeet CTC 110M INT8");
  }
  if (text.compare(QStringLiteral("NVIDIA Parakeet TDT 0.6B v3 (GGUF)"),
                   Qt::CaseInsensitive) == 0) {
    // The former GGUF/CrispASR choice has been retired.  NVIDIA Parakeet is
    // served exclusively by the bundled Rust ONNX engine.
    return QStringLiteral("NVIDIA Parakeet TDT 0.6B v3 INT8 (ONNX/Rust)");
  }
  return text;
}

QString formatMemoryMb(int memoryMb) {
  if (memoryMb <= 0)
    return QStringLiteral("Unknown");
  if (memoryMb >= 1024)
    return QStringLiteral("%1 GB").arg(QString::number(memoryMb / 1024.0, 'f', 1));
  return QStringLiteral("%1 MB").arg(memoryMb);
}

int targetUsableMemoryMb(const ComputeTargetInfo &target) {
  if (target.isCpuFallback)
    return target.systemMemoryMb;
  return qMax(target.dedicatedVramMb, target.sharedMemoryMb);
}

QString targetBackendId(const ComputeTargetInfo &target) {
  if (target.isCpuFallback)
    return QStringLiteral("cpu");

  const QString vendor = target.vendorName.toLower();
  if (vendor.contains("nvidia") && target.localAccelerationDetected)
    return QStringLiteral("nvidia_cuda");
  if (vendor.contains("intel") && target.localAccelerationDetected)
    return QStringLiteral("intel_openvino");
  if (vendor.contains("amd") && target.localAccelerationDetected)
    return QStringLiteral("amd_vulkan");
  return QStringLiteral("cpu");
}

QString normalizeBackendKey(QString key) {
  key = key.trimmed().toLower();
  if (key == QStringLiteral("cuda"))
    return QStringLiteral("nvidia_cuda");
  if (key == QStringLiteral("openvino"))
    return QStringLiteral("intel_openvino");
  if (key.isEmpty())
    return QStringLiteral("cpu");
  return key;
}

QString backendLabel(QString key) {
  key = normalizeBackendKey(key);
  if (key == QStringLiteral("nvidia_cuda"))
    return QStringLiteral("NVIDIA CUDA");
  if (key == QStringLiteral("intel_openvino"))
    return QStringLiteral("Intel OpenVINO");
  return QStringLiteral("CPU ONNX Runtime");
}

QString backendStateTag(QString key) {
  key = normalizeBackendKey(key);
  if (key == QStringLiteral("nvidia_cuda"))
    return QStringLiteral("CUDA");
  if (key == QStringLiteral("intel_openvino"))
    return QStringLiteral("OpenVINO");
  return QStringLiteral("CPU");
}

QString modelBackendSettingKey(const QString &modelName) {
  return QStringLiteral("localModels/%1/backend").arg(cleanIdPart(modelName));
}

ComputeTargetInfo selectedComputeTarget() {
  const QVector<ComputeTargetInfo> targets = detectComputeTargets();
  QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
  QString targetId = settings.value(QStringLiteral("computeTargetId")).toString();
  if (targetId.isEmpty())
    targetId = defaultComputeTargetId(targets);
  return computeTargetById(targets, targetId);
}

QStringList installRootCandidates(const QString &rootKey) {
  QStringList roots;
  const QString appRoot =
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
  if (!appRoot.isEmpty())
    roots << appRoot;

#ifdef Q_OS_WIN
  // Prefer LocalAppData (primary Windows install root for large GGUF models
  // like Nemotron), then Roaming APPDATA for older installs.
  const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
  if (!localAppData.isEmpty())
    roots << QDir(localAppData).filePath(QStringLiteral("QuickSTT"));

  const QString appData = qEnvironmentVariable("APPDATA");
  if (!appData.isEmpty())
    roots << QDir(appData).filePath(QStringLiteral("QuickSTT"));
#else
  // Linux: writable XDG data root — MUST match the native stt_service's
  // getAppDataDir() (~/.local/share/QuickSTT/models) or downloaded models are
  // invisible to the backend. System paths like /usr/lib/quickstt/data stay
  // last as read-only fallbacks.
  QString xdgData = qEnvironmentVariable("XDG_DATA_HOME");
  if (xdgData.trimmed().isEmpty()) {
    const QString home = QDir::homePath();
    if (!home.isEmpty())
      xdgData = QDir(home).filePath(QStringLiteral(".local/share"));
  }
  if (!xdgData.trimmed().isEmpty())
    roots.prepend(QDir(xdgData.trimmed()).filePath(QStringLiteral("QuickSTT")));
#endif

  QStringList expanded;
  for (const QString &root : roots) {
    Q_UNUSED(rootKey);
    const QString expandedRoot =
        QDir::cleanPath(QDir(root).filePath(QStringLiteral("models")));
    if (!expanded.contains(expandedRoot))
      expanded << expandedRoot;
  }
  return expanded;
}

bool copyDirectoryIfMissing(const QString &sourcePath, const QString &targetPath) {
  const QFileInfo sourceInfo(sourcePath);
  if (!sourceInfo.exists())
    return true;

  if (sourceInfo.isDir()) {
    QDir().mkpath(targetPath);
    const QFileInfoList entries =
        QDir(sourcePath).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QFileInfo &entry : entries) {
      const QString targetEntry = QDir(targetPath).filePath(entry.fileName());
      if (!copyDirectoryIfMissing(entry.absoluteFilePath(), targetEntry))
        return false;
    }
    return true;
  }

  if (QFileInfo::exists(targetPath))
    return true;

  QDir().mkpath(QFileInfo(targetPath).absolutePath());
  return QFile::copy(sourcePath, targetPath);
}

void migrateLegacyQuickSttData(const QString &legacyRoot, const QString &targetRoot) {
  if (legacyRoot.isEmpty() || targetRoot.isEmpty())
    return;

  const QString cleanLegacy = QDir::cleanPath(legacyRoot);
  const QString cleanTarget = QDir::cleanPath(targetRoot);
  if (cleanLegacy == cleanTarget || !QFileInfo::exists(cleanLegacy))
    return;

  for (const QString &child :
       {QStringLiteral("models"), QStringLiteral("services")}) {
    const QString legacyChild = QDir(cleanLegacy).filePath(child);
    if (!QFileInfo::exists(legacyChild))
      continue;
    const QString targetChild = QDir(cleanTarget).filePath(child);
    copyDirectoryIfMissing(legacyChild, targetChild);
  }
}

LocalModelDescriptor makeVoskDescriptor(const QString &displayName,
                                        const QString &packageId, int sizeMb,
                                        int minRamMb, bool directDownload) {
  LocalModelDescriptor item;
  item.id = cleanIdPart(displayName);
  item.displayName = displayName;
  item.engineFamily = QStringLiteral("vosk");
  item.accelerationLabel = QStringLiteral("CPU");
  item.description = QStringLiteral("Built-in native Vosk recognizer model.");
  item.widgetHint = QStringLiteral("Widget-ready");
  item.backendKey = QStringLiteral("cpu");
  item.variantKey = cleanIdPart(displayName);
  item.installedPath = QStringLiteral("models/%1").arg(packageId);
  item.packageId = packageId;
  item.modelSizeMb = sizeMb;
  item.minRecommendedMemoryMb = minRamMb;
  item.preferredMemoryMb = minRamMb + 1024;
  // Only the featured Vosk entry is widget-selectable; the rest are
  // dashboard-only downloads.
  item.widgetSelectable = (displayName == QStringLiteral("Vosk Small En"));
  item.directDownload = directDownload;
#ifndef Q_OS_WIN
  // Linux: the native stt_service dlopens libvosk.so. Provision it as a tiny
  // runtime package so Vosk models work out of the box on deb installs.
  item.runtimePackageId = QStringLiteral("rt_vosk_linux");
#endif
  return item;
}

LocalModelDescriptor makeSherpaDescriptor(const QString &displayName,
                                          const QString &engineFamily,
                                          const QString &variantKey,
                                          const QString &packageId,
                                          const QString &installedPath,
                                          int sizeMb, int minRamMb,
                                          const QString &description) {
  LocalModelDescriptor item;
  item.id = cleanIdPart(displayName);
  item.displayName = displayName;
  item.engineFamily = engineFamily;
  item.accelerationLabel = QStringLiteral("Shared ONNX Runtime");
  item.description = description;
  item.widgetHint = QStringLiteral("Uses Vosk wake base");
  item.backendKey = QStringLiteral("cpu");
  item.variantKey = variantKey;
  item.runtimeExecutablePath =
      QStringLiteral("runtimes/sherpa_onnx/cpu/bin/sherpa-onnx-offline.exe");
  item.installedPath = installedPath;
  item.runnerModelId = variantKey;
  item.packageId = packageId;
  item.runtimePackageId = QStringLiteral("rt_sherpa_onnx_cpu");
  item.modelSizeMb = sizeMb;
  item.runtimeSizeMb = 22;
  item.minRecommendedMemoryMb = minRamMb;
  item.preferredMemoryMb = minRamMb + 1024;
  // Featured widget models: Parakeet TDT + Nemotron stay selectable.
  // Everything else (Moonshine, CTC) is dashboard-only.
  const bool featuredSherpa =
      displayName == QStringLiteral("NVIDIA Parakeet TDT 0.6B v3 INT8 (ONNX/Rust)") ||
      displayName == QStringLiteral("NVIDIA Nemotron 3.5 ASR Streaming 0.6B");
  item.widgetSelectable = featuredSherpa;
  item.directDownload = true;
  return item;
}

LocalModelDescriptor makeWhisperCppDescriptor(const QString &displayName,
                                               const QString &variantKey,
                                               const QString &packageId,
                                               const QString &installedPath,
                                               int sizeMb, int minRamMb,
                                               const QString &description) {
  LocalModelDescriptor item;
  item.id = cleanIdPart(displayName);
  item.displayName = displayName;
  item.engineFamily = QStringLiteral("whisper_cpp");
  item.accelerationLabel = QStringLiteral("whisper.cpp CPU");
  item.description = description;
  item.widgetHint = QStringLiteral("Uses Vosk wake base");
  item.backendKey = QStringLiteral("cpu");
  item.variantKey = variantKey;
  item.runtimeExecutablePath =
      QStringLiteral("runtimes/whisper_cpp/cpu/whisper-cli.exe");
  item.installedPath = installedPath;
  item.runnerModelId = variantKey;
  item.packageId = packageId;
  item.runtimePackageId = QStringLiteral("rt_whisper_cpp_cpu");
  item.modelSizeMb = sizeMb;
  item.runtimeSizeMb = 8;
  item.minRecommendedMemoryMb = minRamMb;
  item.preferredMemoryMb = minRamMb + 1024;
  // Whisper models are dashboard-only; widget shows only the 3 featured.
  item.widgetSelectable = false;
  item.directDownload = true;
  return item;
}

QVector<LocalModelDescriptor> allDescriptors() {
  static const QVector<LocalModelDescriptor> descriptors = []() {
    QVector<LocalModelDescriptor> items;

    items << makeVoskDescriptor(QStringLiteral("Vosk Small En"),
                                QStringLiteral("pkg_vosk_small_en"), 50, 1024,
                                true);
    items << makeVoskDescriptor(QStringLiteral("Vosk Large En"),
                                QStringLiteral("pkg_vosk_large_en"), 1800, 4096,
                                true);
    items << makeVoskDescriptor(QStringLiteral("Vosk Indian En"),
                                QStringLiteral("pkg_vosk_indian_en"), 1260, 3072,
                                true);
    items << makeVoskDescriptor(QStringLiteral("Vosk Small Cn"),
                                QStringLiteral("pkg_vosk_small_cn"), 45, 1024,
                                true);
    items << makeVoskDescriptor(QStringLiteral("Vosk Large Cn"),
                                QStringLiteral("pkg_vosk_large_cn"), 1800, 4096,
                                true);
    items << makeVoskDescriptor(QStringLiteral("Vosk Small Ru"),
                                QStringLiteral("pkg_vosk_small_ru"), 90, 1536,
                                false);
    items << makeVoskDescriptor(QStringLiteral("Vosk Small Fr"),
                                QStringLiteral("pkg_vosk_small_fr"), 90, 1536,
                                false);
    items << makeVoskDescriptor(QStringLiteral("Vosk Large Fr"),
                                QStringLiteral("pkg_vosk_large_fr"), 1400, 3072,
                                false);
    items << makeVoskDescriptor(QStringLiteral("Vosk Small De"),
                                QStringLiteral("pkg_vosk_small_de"), 85, 1536,
                                false);
    items << makeVoskDescriptor(QStringLiteral("Vosk Large De"),
                                QStringLiteral("pkg_vosk_large_de"), 1800, 4096,
                                false);
    items << makeVoskDescriptor(QStringLiteral("Vosk Small Es"),
                                QStringLiteral("pkg_vosk_small_es"), 85, 1536,
                                false);
    items << makeVoskDescriptor(QStringLiteral("Vosk Small Pt"),
                                QStringLiteral("pkg_vosk_small_pt"), 85, 1536,
                                false);
    items << makeVoskDescriptor(QStringLiteral("Vosk Small It"),
                                QStringLiteral("pkg_vosk_small_it"), 90, 1536,
                                false);
    items << makeVoskDescriptor(QStringLiteral("Vosk Small Ja"),
                                QStringLiteral("pkg_vosk_small_ja"), 90, 1536,
                                false);
    items << makeSherpaDescriptor(
        QStringLiteral("Moonshine v2 Tiny En"),
        QStringLiteral("moonshine_v2"), QStringLiteral("moonshine_v2_tiny_en"),
        QStringLiteral("pkg_moonshine_v2_tiny_en"),
        QStringLiteral("moonshine_v2/tiny_en"), 43, 1536,
        QStringLiteral(
            "Moonshine v2 tiny-tier English CPU recognizer via sherpa-onnx."));
    items << makeSherpaDescriptor(
        QStringLiteral("Moonshine v2 Base En"),
        QStringLiteral("moonshine_v2"), QStringLiteral("moonshine_v2_base_en"),
        QStringLiteral("pkg_moonshine_v2_base_en"),
        QStringLiteral("moonshine_v2/base_en"), 135, 2048,
        QStringLiteral(
            "Moonshine v2 base-tier English CPU recognizer via sherpa-onnx."));
    {
      LocalModelDescriptor parakeet = makeSherpaDescriptor(
          QStringLiteral("NVIDIA Parakeet TDT 0.6B v3 INT8 (ONNX/Rust)"),
          QStringLiteral("nemo_transducer"),
          QStringLiteral("nemo_fastconformer_tdt_0_6b_v3_int8"),
          QStringLiteral("pkg_nemo_tdt_0_6b_v3_int8"),
          QStringLiteral("nemo/tdt_0_6b_v3_int8"), 640, 4096,
          QStringLiteral(
              "NVIDIA Parakeet transducer recognizer with a shared INT8 ONNX model package via Rust engine."));
      // Direct PCM worker used by both the C++ pill (file) and Ctrl+Space popup.
      parakeet.runtimeExecutablePath =
          QStringLiteral("tools/parakeet/parakeet_engine.exe");
      parakeet.supportsBatchFile = true;
      parakeet.supportsStreaming = false; // utterance batch today; streaming later
      parakeet.supportsDirectPcm = true;
      parakeet.usesPersistentWorker = true;
      parakeet.widgetHint = QStringLiteral("Native direct PCM worker");
      items << parakeet;
    }
    {
      // Nemotron 3.5 ASR Streaming — EXACT same stack as Handy:
      //   model:   handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf (Q8_0)
      //   runtime: transcribe.cpp (tools/nemotron/transcribe.dll + ggml)
      LocalModelDescriptor nemotron = makeSherpaDescriptor(
          QStringLiteral("NVIDIA Nemotron 3.5 ASR Streaming 0.6B"),
          QStringLiteral("nemotron_streaming"),
          QStringLiteral("nemotron_3_5_asr_streaming_0_6b"),
          QStringLiteral("pkg_nemotron_3_5_asr_streaming_0_6b"),
          QStringLiteral(
              "nemotron/nemotron-3.5-asr-streaming-0.6b/"
              "nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf"),
          716, 4096,
          QStringLiteral(
              "Handy's Nemotron 3.5 multilingual streaming ASR (0.6B GGUF via "
              "transcribe.cpp). Ctrl+Space uses live partials; the main pill "
              "uses full-utterance finalize."));
      nemotron.runtimeExecutablePath =
          QStringLiteral("tools/nemotron/nemotron_engine.exe");
      nemotron.accelerationLabel = QStringLiteral("transcribe.cpp (Handy)");
      nemotron.supportsBatchFile = true;
      nemotron.supportsStreaming = true;
      nemotron.supportsDirectPcm = true;
      nemotron.usesPersistentWorker = true;
      nemotron.widgetSelectable = true;
      nemotron.directDownload = true;
      nemotron.widgetHint = QStringLiteral("Streaming · Handy Live");
      items << nemotron;
    }

    items << makeSherpaDescriptor(
        QStringLiteral("NVIDIA Parakeet CTC 110M INT8"),
        QStringLiteral("nemo_ctc"),
        QStringLiteral("nemo_fastconformer_ctc_110m_int8"),
        QStringLiteral("pkg_nemo_ctc_110m_int8"),
        QStringLiteral("nemo/ctc_110m_int8"), 126, 2048,
        QStringLiteral(
            "NVIDIA Parakeet CTC recognizer with a shared INT8 ONNX model package via sherpa-onnx."));
    items << makeWhisperCppDescriptor(
        QStringLiteral("Whisper Small En Q8"),
        QStringLiteral("whisper_small_en_q8"),
        QStringLiteral("pkg_whisper_small_en_q8"),
        QStringLiteral("whisper_cpp/small_en_q8"), 264, 2048,
        QStringLiteral(
            "OpenAI Whisper small.en model in GGML INT8 format via whisper.cpp. High accuracy English-only recognizer."));
    items << makeWhisperCppDescriptor(
        QStringLiteral("Whisper Small En Q5"),
        QStringLiteral("whisper_small_en_q5"),
        QStringLiteral("pkg_whisper_small_en_q5"),
        QStringLiteral("whisper_cpp/small_en_q5"), 190, 1536,
        QStringLiteral(
            "OpenAI Whisper small.en model in GGML Q5 format via whisper.cpp. Fast English-only recognizer with near-Q8 accuracy."));
    items << makeWhisperCppDescriptor(
        QStringLiteral("Whisper Tiny En Q8"),
        QStringLiteral("whisper_tiny_en_q8"),
        QStringLiteral("pkg_whisper_tiny_en_q8"),
        QStringLiteral("whisper_cpp/tiny_en_q8"), 44, 512,
        QStringLiteral(
            "OpenAI Whisper tiny.en model in GGML INT8 format via whisper.cpp. Ultra-fast English-only recognizer."));
    items << makeWhisperCppDescriptor(
        QStringLiteral("Whisper Tiny En Q5"),
        QStringLiteral("whisper_tiny_en_q5"),
        QStringLiteral("pkg_whisper_tiny_en_q5"),
        QStringLiteral("whisper_cpp/tiny_en_q5"), 32, 512,
        QStringLiteral(
            "OpenAI Whisper tiny.en model in GGML Q5 format via whisper.cpp. Fastest English-only recognizer."));
    items << makeWhisperCppDescriptor(
        QStringLiteral("Whisper Base En Q8"),
        QStringLiteral("whisper_base_en_q8"),
        QStringLiteral("pkg_whisper_base_en_q8"),
        QStringLiteral("whisper_cpp/base_en_q8"), 74, 1024,
        QStringLiteral(
            "OpenAI Whisper base.en model in GGML INT8 format via whisper.cpp. Good balance of speed and accuracy."));
    items << makeWhisperCppDescriptor(
        QStringLiteral("Whisper Large-v3 Turbo Q8"),
        QStringLiteral("whisper_large_v3_turbo_q8"),
        QStringLiteral("pkg_whisper_large_v3_turbo_q8"),
        QStringLiteral("whisper_cpp/large_v3_turbo_q8"), 1500, 8192,
        QStringLiteral(
            "OpenAI Whisper large-v3-turbo model in GGML Q8_0 format via whisper.cpp. Highest accuracy English-only recognizer."));

    return items;
  }();
  return descriptors;
}

QVector<LocalModelPackageInfo> allPackages() {
  static const QVector<LocalModelPackageInfo> packages = {
#ifndef Q_OS_WIN
      {QStringLiteral("rt_vosk_linux"),
       QStringLiteral("Vosk Runtime (libvosk.so)"),
       QStringLiteral("models/runtimes/vosk/vosk-linux-x86_64-0.3.45.zip"),
       QStringLiteral("https://github.com/alphacep/vosk-api/releases/download/"
                      "v0.3.45/vosk-linux-x86_64-0.3.45.zip"),
       QStringLiteral("models"), QStringLiteral("runtimes/vosk"),
       {QStringLiteral("runtimes/vosk/libvosk.so")},
       {QStringLiteral("runtimes/vosk/libvosk.so")}, true},
#endif
      {QStringLiteral("pkg_vosk_small_en"), QStringLiteral("Vosk Small En"),
       QStringLiteral("models/vosk/vosk-model-small-en-us-0.15.zip"),
       QStringLiteral(
           "https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip"),
       QStringLiteral("models"), QString(),
       {QStringLiteral("vosk-model-small-en-us-0.15")},
       {QStringLiteral("vosk-model-small-en-us-0.15")}, true},
      {QStringLiteral("pkg_vosk_large_en"), QStringLiteral("Vosk Large En"),
       QStringLiteral("models/vosk/vosk-model-en-us-0.22.zip"),
       QStringLiteral(
           "https://alphacephei.com/vosk/models/vosk-model-en-us-0.22.zip"),
       QStringLiteral("models"), QString(),
       {QStringLiteral("vosk-model-en-us-0.22")},
       {QStringLiteral("vosk-model-en-us-0.22")}, true},
      {QStringLiteral("pkg_vosk_indian_en"), QStringLiteral("Vosk Indian En"),
       QStringLiteral("models/vosk/vosk-model-en-in-0.5.zip"),
       QStringLiteral(
           "https://alphacephei.com/vosk/models/vosk-model-en-in-0.5.zip"),
       QStringLiteral("models"), QString(),
       {QStringLiteral("vosk-model-en-in-0.5")},
       {QStringLiteral("vosk-model-en-in-0.5")}, true},
      {QStringLiteral("pkg_vosk_small_cn"), QStringLiteral("Vosk Small Cn"),
       QStringLiteral("models/vosk/vosk-model-small-cn-0.22.zip"),
       QStringLiteral(
           "https://alphacephei.com/vosk/models/vosk-model-small-cn-0.22.zip"),
       QStringLiteral("models"), QString(),
       {QStringLiteral("vosk-model-small-cn-0.22")},
       {QStringLiteral("vosk-model-small-cn-0.22")}, true},
      {QStringLiteral("pkg_vosk_large_cn"), QStringLiteral("Vosk Large Cn"),
       QStringLiteral("models/vosk/vosk-model-cn-0.22.zip"),
       QStringLiteral("https://alphacephei.com/vosk/models/vosk-model-cn-0.22.zip"),
       QStringLiteral("models"), QString(),
       {QStringLiteral("vosk-model-cn-0.22")},
       {QStringLiteral("vosk-model-cn-0.22")}, true},
      {QStringLiteral("rt_sherpa_onnx_cpu"),
       QStringLiteral("sherpa-onnx CPU Runtime"),
       QStringLiteral("models/runtimes/sherpa_onnx_cpu_runtime.tar.bz2"),
       QStringLiteral("https://github.com/k2-fsa/sherpa-onnx/releases/download/"
                      "v1.12.34/"
                      "sherpa-onnx-v1.12.34-win-x64-shared-MT-Release-no-tts."
                      "tar.bz2"),
       QStringLiteral("models"), QStringLiteral("runtimes/sherpa_onnx/cpu"),
       {QStringLiteral("runtimes/sherpa_onnx/cpu/bin/sherpa-onnx-offline.exe")},
       {QStringLiteral("runtimes/sherpa_onnx/cpu")}, true},
      {QStringLiteral("rt_sherpa_onnx_cuda"),
       QStringLiteral("sherpa-onnx CUDA Runtime"),
       QStringLiteral("models/runtimes/sherpa_onnx_cuda_runtime.tar.bz2"),
       QStringLiteral("https://github.com/k2-fsa/sherpa-onnx/releases/download/"
                      "v1.12.34/"
                      "sherpa-onnx-v1.12.34-win-x64-cuda.tar.bz2"),
       QStringLiteral("models"), QStringLiteral("runtimes/sherpa_onnx/cuda"),
       {QStringLiteral("runtimes/sherpa_onnx/cuda/bin/sherpa-onnx-offline.exe")},
       {QStringLiteral("runtimes/sherpa_onnx/cuda")}, true},
      {QStringLiteral("pkg_moonshine_v2_tiny_en"),
       QStringLiteral("Moonshine v2 Tiny En"),
       QStringLiteral("models/sherpa_onnx/moonshine_v2_tiny_en.tar.bz2"),
       QStringLiteral("https://github.com/k2-fsa/sherpa-onnx/releases/download/"
                      "asr-models/"
                      "sherpa-onnx-moonshine-tiny-en-quantized-2026-02-27.tar."
                      "bz2"),
       QStringLiteral("models"), QStringLiteral("moonshine_v2/tiny_en"),
       {QStringLiteral("moonshine_v2/tiny_en/encoder_model.ort"),
        QStringLiteral("moonshine_v2/tiny_en/decoder_model_merged.ort"),
        QStringLiteral("moonshine_v2/tiny_en/tokens.txt")},
       {QStringLiteral("moonshine_v2/tiny_en")}, true},
      {QStringLiteral("pkg_moonshine_v2_base_en"),
       QStringLiteral("Moonshine v2 Base En"),
       QStringLiteral("models/sherpa_onnx/moonshine_v2_base_en.tar.bz2"),
       QStringLiteral("https://github.com/k2-fsa/sherpa-onnx/releases/download/"
                      "asr-models/"
                      "sherpa-onnx-moonshine-base-en-quantized-2026-02-27.tar."
                      "bz2"),
       QStringLiteral("models"), QStringLiteral("moonshine_v2/base_en"),
       {QStringLiteral("moonshine_v2/base_en/encoder_model.ort"),
        QStringLiteral("moonshine_v2/base_en/decoder_model_merged.ort"),
        QStringLiteral("moonshine_v2/base_en/tokens.txt")},
       {QStringLiteral("moonshine_v2/base_en")}, true},
      {QStringLiteral("pkg_nemo_tdt_0_6b_v3_int8"),
       QStringLiteral("NVIDIA Parakeet TDT 0.6B v3 INT8 (ONNX/Rust)"),
       QStringLiteral("models/sherpa_onnx/nemo_tdt_0_6b_v3_int8.tar.bz2"),
       QStringLiteral("https://github.com/k2-fsa/sherpa-onnx/releases/download/"
                      "asr-models/"
                      "sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8.tar.bz2"),
       QStringLiteral("models"), QStringLiteral("nemo/tdt_0_6b_v3_int8"),
       {QStringLiteral("nemo/tdt_0_6b_v3_int8/encoder.int8.onnx"),
        QStringLiteral("nemo/tdt_0_6b_v3_int8/decoder.int8.onnx"),
        QStringLiteral("nemo/tdt_0_6b_v3_int8/joiner.int8.onnx"),
        QStringLiteral("nemo/tdt_0_6b_v3_int8/tokens.txt")},
       {QStringLiteral("nemo/tdt_0_6b_v3_int8")}, true},
      {QStringLiteral("pkg_nemo_ctc_110m_int8"),
       QStringLiteral("NVIDIA Parakeet CTC 110M INT8"),
       QStringLiteral("models/sherpa_onnx/nemo_ctc_110m_int8.tar.bz2"),
       QStringLiteral("https://github.com/k2-fsa/sherpa-onnx/releases/download/"
                      "asr-models/"
                      "sherpa-onnx-nemo-parakeet_tdt_ctc_110m-en-36000-int8."
                      "tar.bz2"),
       QStringLiteral("models"), QStringLiteral("nemo/ctc_110m_int8"),
       {QStringLiteral("nemo/ctc_110m_int8/model.int8.onnx"),
        QStringLiteral("nemo/ctc_110m_int8/tokens.txt")},
       {QStringLiteral("nemo/ctc_110m_int8")}, true},
      {QStringLiteral("rt_whisper_cpp_cpu"),
       QStringLiteral("whisper.cpp CPU Runtime"),
       QStringLiteral("models/runtimes/whisper_cpp_cpu_runtime.zip"),
       QStringLiteral("https://github.com/ggml-org/whisper.cpp/releases/"
                      "download/v1.8.4/whisper-bin-x64.zip"),
       QStringLiteral("models"), QStringLiteral("runtimes/whisper_cpp/cpu"),
       {QStringLiteral("runtimes/whisper_cpp/cpu/whisper-cli.exe")},
       {QStringLiteral("runtimes/whisper_cpp/cpu")}, true},
      {QStringLiteral("pkg_whisper_small_en_q8"),
       QStringLiteral("Whisper Small En Q8"),
       QString(),
       QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/"
                      "main/ggml-small.en-q8_0.bin"),
       QStringLiteral("models"), QStringLiteral("whisper_cpp/small_en_q8"),
       {QStringLiteral("whisper_cpp/small_en_q8/ggml-small.en-q8_0.bin")},
       {QStringLiteral("whisper_cpp/small_en_q8")}, false},
      {QStringLiteral("pkg_whisper_small_en_q5"),
       QStringLiteral("Whisper Small En Q5"),
       QString(),
       QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/"
                      "main/ggml-small.en-q5_1.bin"),
       QStringLiteral("models"), QStringLiteral("whisper_cpp/small_en_q5"),
       {QStringLiteral("whisper_cpp/small_en_q5/ggml-small.en-q5_1.bin")},
       {QStringLiteral("whisper_cpp/small_en_q5")}, false},
      {QStringLiteral("pkg_whisper_tiny_en_q8"),
       QStringLiteral("Whisper Tiny En Q8"),
       QString(),
       QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/"
                      "main/ggml-tiny.en-q8_0.bin"),
       QStringLiteral("models"), QStringLiteral("whisper_cpp/tiny_en_q8"),
       {QStringLiteral("whisper_cpp/tiny_en_q8/ggml-tiny.en-q8_0.bin")},
       {QStringLiteral("whisper_cpp/tiny_en_q8")}, false},
      {QStringLiteral("pkg_whisper_base_en_q8"),
       QStringLiteral("Whisper Base En Q8"),
       QString(),
       QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/"
                      "main/ggml-base.en-q8_0.bin"),
       QStringLiteral("models"), QStringLiteral("whisper_cpp/base_en_q8"),
       {QStringLiteral("whisper_cpp/base_en_q8/ggml-base.en-q8_0.bin")},
       {QStringLiteral("whisper_cpp/base_en_q8")}, false},
      {QStringLiteral("pkg_whisper_large_v3_turbo_q8"),
       QStringLiteral("Whisper Large-v3 Turbo Q8"),
       QString(),
       QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/"
                      "main/ggml-large-v3-turbo-q8_0.bin"),
       QStringLiteral("models"), QStringLiteral("whisper_cpp/large_v3_turbo_q8"),
       {QStringLiteral("whisper_cpp/large_v3_turbo_q8/ggml-large-v3-turbo-q8_0.bin")},
       {QStringLiteral("whisper_cpp/large_v3_turbo_q8")}, false},
      {QStringLiteral("pkg_whisper_tiny_en_q5"),
       QStringLiteral("Whisper Tiny En Q5"),
       QString(),
       QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/"
                      "main/ggml-tiny.en-q5_1.bin"),
       QStringLiteral("models"), QStringLiteral("whisper_cpp/tiny_en_q5"),
       {QStringLiteral("whisper_cpp/tiny_en_q5/ggml-tiny.en-q5_1.bin")},
       {QStringLiteral("whisper_cpp/tiny_en_q5")}, false},
      // Handy Nemotron 3.5 — direct GGUF from handy-computer (same as Handy app).
      // LocalModelManager special-cases this package id to run
      // tools/nemotron/fetch_and_convert.py (HF hub download, resumable).
      {QStringLiteral("pkg_nemotron_3_5_asr_streaming_0_6b"),
       QStringLiteral("NVIDIA Nemotron 3.5 ASR Streaming 0.6B"),
       QString(),
       QStringLiteral(
           "https://huggingface.co/handy-computer/"
           "nemotron-3.5-asr-streaming-0.6b-gguf/resolve/main/"
           "nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf"),
       QStringLiteral("models"),
       QStringLiteral("nemotron/nemotron-3.5-asr-streaming-0.6b"),
       {QStringLiteral(
           "nemotron/nemotron-3.5-asr-streaming-0.6b/"
           "nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf")},
       {QStringLiteral("nemotron/nemotron-3.5-asr-streaming-0.6b")}, false},

  };
  return packages;
}

bool packageInstalled(const LocalModelPackageInfo &package) {
  if (package.id.isEmpty())
    return false;

  for (const QString &root : installRootCandidates(package.installRootKey)) {
    bool allFound = !package.installMarkers.isEmpty();
    for (const QString &marker : package.installMarkers) {
      if (!QFileInfo::exists(QDir(root).filePath(marker))) {
        allFound = false;
        break;
      }
    }
    if (allFound)
      return true;
  }
  return false;
}

bool descriptorUsesSherpaRuntime(const LocalModelDescriptor &descriptor) {
  return !descriptor.displayName.isEmpty() &&
         descriptor.engineFamily != QStringLiteral("vosk") &&
         descriptor.engineFamily != QStringLiteral("whisper_cpp") &&
         descriptor.engineFamily != QStringLiteral("nemotron_streaming");
}

QStringList descriptorAvailableBackends(const LocalModelDescriptor &descriptor) {
  QStringList backends;
  if (descriptor.displayName.isEmpty())
    return backends;

  backends << QStringLiteral("cpu");
  if (!descriptorUsesSherpaRuntime(descriptor))
    return backends;

  const ComputeTargetInfo target = selectedComputeTarget();
  if (targetBackendId(target) == QStringLiteral("nvidia_cuda") &&
      target.localAccelerationDetected) {
    backends << QStringLiteral("nvidia_cuda");
  }
  return backends;
}

QString descriptorRequestedBackend(const LocalModelDescriptor &descriptor) {
  if (descriptor.displayName.isEmpty())
    return QStringLiteral("cpu");

  QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
  const QString settingKey = modelBackendSettingKey(descriptor.displayName);
  if (settings.contains(settingKey)) {
    return normalizeBackendKey(settings.value(settingKey).toString());
  }

  if (!descriptorUsesSherpaRuntime(descriptor))
    return QStringLiteral("cpu");

  const ComputeTargetInfo target = selectedComputeTarget();
  const QString preferredBackend = normalizeBackendKey(targetBackendId(target));
  const QStringList availableBackends = descriptorAvailableBackends(descriptor);
  if (availableBackends.contains(preferredBackend)) {
    const QString runtimePackageId =
        preferredBackend == QStringLiteral("nvidia_cuda")
            ? QStringLiteral("rt_sherpa_onnx_cuda")
            : QStringLiteral("rt_sherpa_onnx_cpu");
    if (runtimePackageId.isEmpty() ||
        packageInstalled(localModelPackage(runtimePackageId))) {
      return preferredBackend;
    }
  }

  return QStringLiteral("cpu");
}

QString descriptorEffectiveBackend(const LocalModelDescriptor &descriptor) {
  const QStringList available = descriptorAvailableBackends(descriptor);
  const QString requested = descriptorRequestedBackend(descriptor);
  return available.contains(requested) ? requested : QStringLiteral("cpu");
}

QString runtimePackageIdForBackend(const LocalModelDescriptor &descriptor,
                                   const QString &backendKey) {
  if (!descriptorUsesSherpaRuntime(descriptor))
    return descriptor.runtimePackageId;

  const QString normalized = normalizeBackendKey(backendKey);
  if (normalized == QStringLiteral("nvidia_cuda"))
    return QStringLiteral("rt_sherpa_onnx_cuda");
  return QStringLiteral("rt_sherpa_onnx_cpu");
}

QString runtimeExecutableForBackend(const LocalModelDescriptor &descriptor,
                                    const QString &backendKey) {
  if (!descriptorUsesSherpaRuntime(descriptor))
    return descriptor.runtimeExecutablePath;

  const QString normalized = normalizeBackendKey(backendKey);
  if (normalized == QStringLiteral("nvidia_cuda")) {
    return QStringLiteral("runtimes/sherpa_onnx/cuda/bin/sherpa-onnx-offline.exe");
  }
  return QStringLiteral("runtimes/sherpa_onnx/cpu/bin/sherpa-onnx-offline.exe");
}

bool modelPayloadInstalled(const LocalModelDescriptor &descriptor) {
  if (descriptor.displayName.isEmpty() || descriptor.packageId.isEmpty())
    return false;
  if (!packageInstalled(localModelPackage(descriptor.packageId)))
    return false;
  for (const QString &extraPackageId : descriptor.extraPackageIds) {
    if (!packageInstalled(localModelPackage(extraPackageId)))
      return false;
  }
  return true;
}

} // namespace

QVector<ComputeTargetInfo> detectComputeTargets() {
  QVector<ComputeTargetInfo> targets;
#ifdef _WIN32
  IDXGIFactory1 *factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(IID_IDXGIFactory1,
                                   reinterpret_cast<void **>(&factory))) &&
      factory) {
    for (UINT index = 0;; ++index) {
      IDXGIAdapter1 *adapter = nullptr;
      if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
        break;
      if (!adapter)
        continue;

      DXGI_ADAPTER_DESC1 desc = {};
      const HRESULT descResult = adapter->GetDesc1(&desc);
      adapter->Release();
      if (FAILED(descResult) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        continue;

      ComputeTargetInfo target;
      target.displayName = QString::fromWCharArray(desc.Description).trimmed();
      target.id = QStringLiteral("gpu_%1_%2")
                      .arg(quint32(desc.AdapterLuid.HighPart), 0, 16)
                      .arg(quint32(desc.AdapterLuid.LowPart), 0, 16);
      target.dedicatedVramMb = int(desc.DedicatedVideoMemory / (1024 * 1024));
      target.sharedMemoryMb = int(desc.SharedSystemMemory / (1024 * 1024));
      target.systemMemoryMb = int(desc.DedicatedSystemMemory / (1024 * 1024));
      target.integrated = target.dedicatedVramMb == 0;

      switch (desc.VendorId) {
      case 0x10DE:
        target.vendorName = QStringLiteral("NVIDIA");
        target.backendLabel = QStringLiteral("CUDA");
        target.localAccelerationDetected = true;
        break;
      case 0x8086:
        target.vendorName = QStringLiteral("Intel");
        target.backendLabel = QStringLiteral("OpenVINO GPU");
        target.localAccelerationDetected = true;
        break;
      case 0x1002:
      case 0x1022:
        target.vendorName = QStringLiteral("AMD");
        target.backendLabel = QStringLiteral("Vulkan GPU");
        target.localAccelerationDetected = true;
        break;
      default:
        target.vendorName = QStringLiteral("Vendor 0x%1")
                                .arg(desc.VendorId, 4, 16, QChar('0'))
                                .toUpper();
        target.backendLabel = QStringLiteral("Detection only");
        break;
      }
      targets << target;
    }
    factory->Release();
  }

  MEMORYSTATUSEX memoryStatus = {};
  memoryStatus.dwLength = sizeof(memoryStatus);
  GlobalMemoryStatusEx(&memoryStatus);

  ComputeTargetInfo cpuTarget;
  cpuTarget.id = QStringLiteral("cpu");
  cpuTarget.displayName = QStringLiteral("CPU Fallback");
  cpuTarget.vendorName = QStringLiteral("System CPU");
  cpuTarget.backendLabel = QStringLiteral("Native Vosk CPU");
  cpuTarget.systemMemoryMb = int(memoryStatus.ullTotalPhys / (1024ull * 1024ull));
  cpuTarget.isCpuFallback = true;
  targets << cpuTarget;
#else
  // Linux: no DXGI — read total RAM from /proc/meminfo and expose CPU target.
  qint64 totalKb = 0;
  QFile meminfo(QStringLiteral("/proc/meminfo"));
  if (meminfo.open(QIODevice::ReadOnly | QIODevice::Text)) {
    while (!meminfo.atEnd()) {
      const QStringList parts =
          QString::fromLatin1(meminfo.readLine()).split(QLatin1Char(' '),
                                                        Qt::SkipEmptyParts);
      if (parts.size() >= 2 && parts.first() == QLatin1String("MemTotal:")) {
        totalKb = parts.at(1).toLongLong();
        break;
      }
    }
  }

  ComputeTargetInfo cpuTarget;
  cpuTarget.id = QStringLiteral("cpu");
  cpuTarget.displayName = QStringLiteral("CPU Fallback");
  cpuTarget.vendorName = QStringLiteral("System CPU");
  cpuTarget.backendLabel = QStringLiteral("Native Vosk CPU");
  cpuTarget.systemMemoryMb = int(totalKb / 1024);
  cpuTarget.isCpuFallback = true;
  targets << cpuTarget;
#endif
  return targets;
}

QString defaultComputeTargetId(const QVector<ComputeTargetInfo> &targets) {
  for (const ComputeTargetInfo &target : targets) {
    if (!target.isCpuFallback && target.localAccelerationDetected)
      return target.id;
  }
  for (const ComputeTargetInfo &target : targets) {
    if (!target.isCpuFallback)
      return target.id;
  }
  return targets.isEmpty() ? QStringLiteral("cpu") : targets.first().id;
}

ComputeTargetInfo computeTargetById(const QVector<ComputeTargetInfo> &targets,
                                    const QString &targetId) {
  for (const ComputeTargetInfo &target : targets) {
    if (target.id == targetId)
      return target;
  }
  if (!targets.isEmpty())
    return targets.first();
  return ComputeTargetInfo();
}

QString computeTargetSummaryText(const ComputeTargetInfo &target) {
  if (target.id.isEmpty())
    return QStringLiteral("No GPU information detected.");

  QStringList lines;
  lines << QStringLiteral("Target: %1").arg(target.displayName);
  lines << QStringLiteral("Vendor: %1").arg(target.vendorName);
  lines << QStringLiteral("Backend path: %1").arg(target.backendLabel);
  if (!target.isCpuFallback) {
    lines << QStringLiteral("Dedicated VRAM: %1")
                 .arg(formatMemoryMb(target.dedicatedVramMb));
    lines << QStringLiteral("Shared memory: %1")
                 .arg(formatMemoryMb(target.sharedMemoryMb));
  } else {
    lines << QStringLiteral("System memory: %1")
                 .arg(formatMemoryMb(target.systemMemoryMb));
  }
  return lines.join(QLatin1Char('\n'));
}

QString computeTargetRecommendationText(const ComputeTargetInfo &target) {
  if (target.id.isEmpty())
    return QStringLiteral("GPU detection is unavailable on this system.");
  if (target.isCpuFallback) {
    return QStringLiteral(
        "CPU mode supports built-in Vosk plus on-demand sherpa-onnx local models.");
  }
  if (!target.localAccelerationDetected) {
    return QStringLiteral(
        "This GPU is detected, but local downloads in this build run on CPU.");
  }
  if (targetBackendId(target) == QStringLiteral("intel_openvino")) {
    return QStringLiteral(
        "Intel GPU detected. Sherpa-onnx models run on CPU, but the OpenVINO GPU model "
        "(NVIDIA Parakeet CTC 110M OpenVINO GPU) runs natively on the Intel GPU.");
  }
  if (targetBackendId(target) == QStringLiteral("amd_vulkan")) {
    return QStringLiteral(
        "AMD GPU detection is available, but the current local engine path remains CPU-only.");
  }
  return QStringLiteral(
      "NVIDIA GPU detection is available. Optional sherpa-onnx CUDA runtimes can be downloaded without re-downloading the model.");
}

QVector<LocalModelDescriptor>
localDashboardCatalogForTarget(const ComputeTargetInfo &target) {
  Q_UNUSED(target);
  return allDescriptors();
}

QStringList localDashboardCatalogNames(const ComputeTargetInfo &target) {
  QStringList names;
  for (const LocalModelDescriptor &descriptor : localDashboardCatalogForTarget(target))
    names << descriptor.displayName;
  return names;
}

bool isKnownLocalModel(const QString &modelName) {
  return !localModelDescriptor(modelName).displayName.isEmpty();
}

bool localModelWidgetSelectable(const QString &modelName) {
  return localModelDescriptor(modelName).widgetSelectable;
}

bool localModelSupportsDirectDownload(const QString &modelName) {
  return localModelDescriptor(modelName).directDownload;
}

bool localModelSupportsRuntimeNow(const QString &modelName) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  return descriptor.engineFamily == QStringLiteral("vosk");
}

bool localModelUsesFrontendTranscriber(const QString &modelName) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (descriptor.displayName.isEmpty() || descriptor.engineFamily == QStringLiteral("vosk"))
    return false;
  return !localModelUsesNativeDirectPipeline(modelName);
}

bool localModelSupportsStreaming(const QString &modelName) {
  return localModelDescriptor(modelName).supportsStreaming;
}

bool localModelSupportsDirectPcm(const QString &modelName) {
  return localModelDescriptor(modelName).supportsDirectPcm;
}

bool localModelUsesPersistentWorker(const QString &modelName) {
  return localModelDescriptor(modelName).usesPersistentWorker;
}

bool localModelUsesNativeDirectPipeline(const QString &modelName) {
  const LocalModelDescriptor d = localModelDescriptor(modelName);
  if (d.displayName.isEmpty())
    return false;
  if (!d.supportsDirectPcm || !d.usesPersistentWorker)
    return false;

  // Parakeet TDT is the shipped direct-PCM worker today.
  if (d.engineFamily == QStringLiteral("nemo_transducer") ||
      d.displayName.contains(QStringLiteral("Parakeet TDT"),
                             Qt::CaseInsensitive)) {
    return true;
  }

  // Nemotron (and future streaming workers): only when BOTH the engine binary
  // AND the model GGUF weights payload are actually present.
  if (d.engineFamily == QStringLiteral("nemotron_streaming") &&
      !d.runtimeExecutablePath.isEmpty()) {
    const QString exe = QDir(QCoreApplication::applicationDirPath())
                            .filePath(d.runtimeExecutablePath);
    return QFileInfo::exists(exe) && modelPayloadInstalled(d);
  }

  return false;
}

bool isLocalModelInstalled(const QString &modelName) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (!modelPayloadInstalled(descriptor))
    return false;

  const QString runtimePackageId = localModelRuntimePackageId(modelName);
  return runtimePackageId.isEmpty() ||
         packageInstalled(localModelPackage(runtimePackageId));
}

QStringList localModelAvailableBackendKeys(const QString &modelName) {
  return descriptorAvailableBackends(localModelDescriptor(modelName));
}

QString localModelBackendLabelForKey(const QString &backendKey) {
  return backendLabel(backendKey);
}

QString localModelSelectedBackendKey(const QString &modelName) {
  return descriptorEffectiveBackend(localModelDescriptor(modelName));
}

void setLocalModelSelectedBackendKey(const QString &modelName,
                                     const QString &backendKey) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (descriptor.displayName.isEmpty())
    return;

  const QString normalized = normalizeBackendKey(backendKey);
  const QString effective =
      descriptorAvailableBackends(descriptor).contains(normalized)
          ? normalized
          : QStringLiteral("cpu");
  QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
  settings.setValue(modelBackendSettingKey(descriptor.displayName), effective);
}

QString localModelBackendStatusText(const QString &modelName) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (descriptor.displayName.isEmpty())
    return QStringLiteral("Unknown local model.");
  if (!descriptorUsesSherpaRuntime(descriptor))
    return QStringLiteral("This model uses the built-in CPU runtime only.");

  const QString backendKey = localModelSelectedBackendKey(modelName);
  const QString runtimePackageId = runtimePackageIdForBackend(descriptor, backendKey);
  const bool modelInstalled = modelPayloadInstalled(descriptor);
  const bool runtimeInstalled =
      runtimePackageId.isEmpty() ||
      packageInstalled(localModelPackage(runtimePackageId));

  QStringList lines;
  lines << QStringLiteral("Selected backend: %1")
               .arg(localModelBackendLabelForKey(backendKey));
  lines << QStringLiteral(
               "Shared ONNX model files are reused across backend runtimes. Switching backend downloads only the runtime dependency.");

  if (modelInstalled && runtimeInstalled) {
    lines << QStringLiteral("%1 is ready for %2.")
                 .arg(descriptor.displayName,
                      localModelBackendLabelForKey(backendKey));
  } else if (modelInstalled) {
    lines << QStringLiteral(
                 "Model files are already installed. Download the %1 runtime only.")
                 .arg(localModelBackendLabelForKey(backendKey));
  } else {
    lines << QStringLiteral(
                 "The first download installs the shared model package plus the selected backend runtime.");
  }

  const ComputeTargetInfo target = selectedComputeTarget();
  if (targetBackendId(target) == QStringLiteral("intel_openvino")) {
    lines << QStringLiteral(
                 "Intel OpenVINO is not exposed by the current sherpa-onnx Windows runtime, so Intel systems stay on CPU for these models.");
  }
  return lines.join(QLatin1Char('\n'));
}

QString localModelStateText(const QString &modelName, bool installed) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (installed)
    return QStringLiteral("[Installed]");
  if (modelPayloadInstalled(descriptor) &&
      !localModelRuntimePackageId(modelName).isEmpty()) {
    return QStringLiteral("[DL RT]");
  }
  if (descriptor.directDownload)
    return QStringLiteral("[DL]");
  if (descriptor.widgetSelectable)
    return QStringLiteral("[Manual]");
  return QStringLiteral("[N/A]");
}

QString localModelTooltip(const QString &modelName, bool installed) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (descriptor.displayName.isEmpty())
    return QStringLiteral("Unknown local model.");
  if (installed)
    return QStringLiteral("%1 is installed for %2.")
        .arg(modelName, localModelBackendLabelForKey(localModelSelectedBackendKey(modelName)));
  if (modelPayloadInstalled(descriptor)) {
    return QStringLiteral(
               "%1 model files are already installed. Download the selected %2 runtime only.")
        .arg(modelName, localModelBackendLabelForKey(localModelSelectedBackendKey(modelName)));
  }
  if (descriptor.directDownload)
    return QStringLiteral(
               "%1 is optional and can be downloaded on demand from the QuickSTT server.")
        .arg(modelName);
  return QStringLiteral("%1 is not available in this build.").arg(modelName);
}

QString localModelBestFitTag(const QString &modelName,
                             const ComputeTargetInfo &target) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (descriptor.displayName.isEmpty())
    return QString();
  if (descriptor.requiresGpu &&
      (target.isCpuFallback || targetBackendId(target) != descriptor.backendKey)) {
    return QStringLiteral("Unavailable for selected target");
  }
  const int availableMb = targetUsableMemoryMb(target);
  if (availableMb <= 0)
    return QStringLiteral("Unknown fit");
  if (availableMb >= descriptor.preferredMemoryMb)
    return QStringLiteral("Best fit");
  if (availableMb >= descriptor.minRecommendedMemoryMb)
    return QStringLiteral("Supported");
  return QStringLiteral("Low headroom");
}

QString localModelRecommendationText(const QString &modelName,
                                     const ComputeTargetInfo &target) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (descriptor.displayName.isEmpty())
    return QString();
  return QStringLiteral("%1 for %2. Minimum %3, preferred %4.")
      .arg(localModelBestFitTag(modelName, target), target.displayName,
           formatMemoryMb(descriptor.minRecommendedMemoryMb),
           formatMemoryMb(descriptor.preferredMemoryMb));
}

QString localModelDetailsText(const QString &modelName,
                              const ComputeTargetInfo &target) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (descriptor.displayName.isEmpty())
    return QStringLiteral("Select a local model to view its details.");

  const QString backendKey = localModelSelectedBackendKey(modelName);
  const QStringList backendLabels = [&]() {
    QStringList labels;
    for (const QString &key : localModelAvailableBackendKeys(modelName))
      labels << localModelBackendLabelForKey(key);
    return labels;
  }();

  QStringList lines;
  lines << descriptor.displayName;
  lines << QStringLiteral("Engine: %1").arg(descriptor.engineFamily);
  lines << QStringLiteral("Backend: %1")
               .arg(descriptorUsesSherpaRuntime(descriptor)
                        ? localModelBackendLabelForKey(backendKey)
                        : descriptor.accelerationLabel);
  lines << descriptor.description;
  if (descriptor.directDownload) {
    lines << QStringLiteral(
        "Delivery: Optional on-demand download from the QuickSTT server.");
  }
  if (descriptorUsesSherpaRuntime(descriptor)) {
    lines << QStringLiteral("Available backends on this device: %1")
                 .arg(backendLabels.join(QStringLiteral(", ")));
    lines << QStringLiteral(
        "Shared model behavior: The ONNX model package is installed once. Switching backend downloads only the matching runtime dependency.");
  }
  lines << QStringLiteral("Model package size: %1")
               .arg(formatMemoryMb(descriptor.modelSizeMb));
  if (descriptor.runtimeSizeMb > 0 && !localModelRuntimePackageId(modelName).isEmpty()) {
    lines << QStringLiteral("Runtime package size: %1")
                 .arg(formatMemoryMb(descriptor.runtimeSizeMb));
  }
  lines << localModelRecommendationText(modelName, target);
  lines << localModelBackendStatusText(modelName);
  lines << QStringLiteral("Widget use today: %1")
               .arg(descriptor.widgetSelectable ? QStringLiteral("Supported")
                                                : QStringLiteral("Not supported"));
  return lines.join(QLatin1Char('\n'));
}

QString localModelDisplayState(const QString &modelName, bool installed,
                               bool widgetChecked) {
  QString text = modelName + QLatin1Char(' ') + localModelStateText(modelName, installed);
  if (localModelUsesFrontendTranscriber(modelName))
    text += QStringLiteral(" [%1]").arg(backendStateTag(localModelSelectedBackendKey(modelName)));
  if (widgetChecked)
    text += QStringLiteral(" [Widget]");
  return text;
}

LocalModelDescriptor localModelDescriptor(const QString &modelName) {
  const QString trimmed = canonicalLocalModelName(modelName);
  for (const LocalModelDescriptor &descriptor : allDescriptors()) {
    if (descriptor.displayName.compare(trimmed, Qt::CaseInsensitive) == 0)
      return descriptor;
  }
  return LocalModelDescriptor();
}

QString canonicalLocalModelName(const QString &modelName) {
  const QString trimmed = migrateLegacyModelName(modelName);
  for (const LocalModelDescriptor &descriptor : allDescriptors()) {
    if (descriptor.displayName.compare(trimmed, Qt::CaseInsensitive) == 0)
      return descriptor.displayName;
  }
  return trimmed.isEmpty() ? QString() : QStringLiteral("Vosk Small En");
}

LocalModelPackageInfo localModelPackage(const QString &packageId) {
  for (const LocalModelPackageInfo &package : allPackages()) {
    if (package.id == packageId)
      return package;
  }
  return LocalModelPackageInfo();
}

QStringList localModelPackageSequence(const QString &modelName) {
  QStringList sequence;
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (descriptor.displayName.isEmpty())
    return sequence;

  const QString runtimePackageId = localModelRuntimePackageId(modelName);
  if (!runtimePackageId.isEmpty() &&
      !packageInstalled(localModelPackage(runtimePackageId))) {
    sequence << runtimePackageId;
  }

  if (!packageInstalled(localModelPackage(descriptor.packageId)))
    sequence << descriptor.packageId;

  for (const QString &extraPackageId : descriptor.extraPackageIds) {
    if (!packageInstalled(localModelPackage(extraPackageId)))
      sequence << extraPackageId;
  }
  return sequence;
}

QStringList installedModelsSharingRuntime(const QString &runtimePackageId) {
  QStringList models;
  for (const LocalModelDescriptor &descriptor : allDescriptors()) {
    if (descriptor.displayName.isEmpty())
      continue;
    if (localModelRuntimePackageId(descriptor.displayName) == runtimePackageId &&
        modelPayloadInstalled(descriptor)) {
      models << descriptor.displayName;
    }
  }
  return models;
}

QString localModelInstalledPath(const QString &modelName) {
  return localModelDescriptor(modelName).installedPath;
}

QString localModelRuntimeExecutablePath(const QString &modelName) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  return runtimeExecutableForBackend(descriptor, localModelSelectedBackendKey(modelName));
}

QString localModelRuntimePackageId(const QString &modelName) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  return runtimePackageIdForBackend(descriptor, localModelSelectedBackendKey(modelName));
}

QString localModelRunnerModelId(const QString &modelName) {
  return localModelDescriptor(modelName).runnerModelId;
}

QString localModelBackendKey(const QString &modelName) {
  return localModelSelectedBackendKey(modelName);
}

QString quickSttDataRoot() {
  static const QString root = []() {
    const QString legacyRoot =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
#ifdef Q_OS_WIN
    const QString appData = qEnvironmentVariable("APPDATA").trimmed();
    const QString preferredRoot =
        appData.isEmpty()
            ? legacyRoot
            : QDir(appData).filePath(QStringLiteral("QuickSTT"));
#else
    // Linux: writable XDG data root shared with the native stt_service
    // (~/.local/share/QuickSTT). System install dirs are read-only.
    QString preferredBase = qEnvironmentVariable("XDG_DATA_HOME").trimmed();
    if (preferredBase.isEmpty())
      preferredBase = QDir::home().filePath(QStringLiteral(".local/share"));
    const QString preferredRoot =
        QDir(preferredBase).filePath(QStringLiteral("QuickSTT"));
#endif
    QDir().mkpath(preferredRoot);
    migrateLegacyQuickSttData(legacyRoot, preferredRoot);
    return preferredRoot;
  }();
  return root;
}

QString quickSttModelsRoot() {
  const QString path =
      QDir(quickSttDataRoot()).filePath(QStringLiteral("models"));
  QDir().mkpath(path);
  return path;
}

QString installRootPathForKey(const QString &rootKey) {
  Q_UNUSED(rootKey);
  return quickSttModelsRoot();
}

QStringList configuredServerUrls() {
  QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
  QStringList urls = settings.value(QStringLiteral("serverUrls")).toStringList();
  if (urls.isEmpty()) {
    QFile serverFile(QDir(QCoreApplication::applicationDirPath())
                         .filePath(QStringLiteral("server.txt")));
    if (serverFile.open(QIODevice::ReadOnly)) {
      const QString url = QString::fromUtf8(serverFile.readLine()).trimmed();
      if (!url.isEmpty())
        urls << url;
    }
  }

  urls.removeAll(QString());
  urls.removeDuplicates();
  return urls;
}
