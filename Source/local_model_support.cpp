#include "local_model_support.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include <windows.h>
#include <dxgi.h>

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
  if (vendor.contains("nvidia") && target.whisperGpuSupported)
    return QStringLiteral("nvidia_cuda");
  if (vendor.contains("intel") && target.whisperGpuSupported)
    return QStringLiteral("intel_openvino");
  if (vendor.contains("amd") && target.whisperGpuSupported)
    return QStringLiteral("amd_vulkan");
  return QStringLiteral("cpu");
}

QStringList installRootCandidates(const QString &rootKey) {
  QStringList roots;
  const QString appRoot =
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
  if (!appRoot.isEmpty())
    roots << appRoot;

  const QString appData = qEnvironmentVariable("APPDATA");
  if (!appData.isEmpty())
    roots << QDir(appData).filePath(QStringLiteral("QuickSTT"));

  QStringList expanded;
  for (const QString &root : roots) {
    const QString child = (rootKey == QStringLiteral("models"))
                              ? QStringLiteral("models")
                              : QStringLiteral("whisper");
    const QString expandedRoot = QDir::cleanPath(QDir(root).filePath(child));
    if (!expanded.contains(expandedRoot))
      expanded << expandedRoot;
  }
  return expanded;
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
  item.widgetSelectable = true;
  item.directDownload = directDownload;
  return item;
}

LocalModelDescriptor makeWhisperCppDescriptor(const QString &displayName,
                                              const QString &backendKey,
                                              const QString &variantKey,
                                              const QString &accelerationLabel,
                                              const QString &packageId,
                                              const QString &runtimePackageId,
                                              const QStringList &extraPackageIds,
                                              int modelSizeMb,
                                              int runtimeSizeMb,
                                              int minMemoryMb,
                                              int preferredMemoryMb,
                                              bool requiresGpu) {
  LocalModelDescriptor item;
  item.id = cleanIdPart(displayName);
  item.displayName = displayName;
  item.engineFamily = QStringLiteral("whisper.cpp");
  item.accelerationLabel = accelerationLabel;
  item.description =
      QStringLiteral("Optional whisper.cpp package for %1 inference.")
          .arg(accelerationLabel);
  item.widgetHint = QStringLiteral("Widget-ready after install");
  item.backendKey = backendKey;
  item.variantKey = variantKey;
  item.runtimeExecutablePath =
      QStringLiteral("runtimes/whisper_cpp/%1/whisper-cli.exe").arg(backendKey);
  item.installedPath = QStringLiteral("models/whisper_cpp/%1/ggml-%2.bin")
                           .arg(variantKey, variantKey == QStringLiteral("turbo")
                                                ? QStringLiteral("large-v3-turbo")
                                                : variantKey);
  item.runnerModelId = (variantKey == QStringLiteral("turbo"))
                           ? QStringLiteral("large-v3-turbo")
                           : variantKey;
  item.packageId = packageId;
  item.runtimePackageId = runtimePackageId;
  item.extraPackageIds = extraPackageIds;
  item.modelSizeMb = modelSizeMb;
  item.runtimeSizeMb = runtimeSizeMb;
  item.minRecommendedMemoryMb = minMemoryMb;
  item.preferredMemoryMb = preferredMemoryMb;
  item.widgetSelectable = true;
  item.directDownload = true;
  item.requiresGpu = requiresGpu;
  return item;
}

LocalModelDescriptor makeFasterWhisperDescriptor(
    const QString &displayName, const QString &backendKey,
    const QString &variantKey, const QString &accelerationLabel,
    const QString &packageId, int modelSizeMb, int runtimeSizeMb,
    int minMemoryMb, int preferredMemoryMb, bool requiresGpu) {
  LocalModelDescriptor item;
  item.id = cleanIdPart(displayName);
  item.displayName = displayName;
  item.engineFamily = QStringLiteral("faster-whisper");
  item.accelerationLabel = accelerationLabel;
  item.description =
      QStringLiteral("Optional faster-whisper package using a standalone CTranslate2 runner for %1.")
          .arg(accelerationLabel);
  item.widgetHint = QStringLiteral("Widget-ready after install");
  item.backendKey = backendKey;
  item.variantKey = variantKey;
  item.runtimeExecutablePath =
      QStringLiteral("runtimes/faster_whisper/faster_whisper_runner.exe");
  item.installedPath =
      QStringLiteral("models/faster_whisper/%1/model.bin").arg(variantKey);
  item.runnerModelId = variantKey;
  item.packageId = packageId;
  item.runtimePackageId = QStringLiteral("rt_faster_whisper");
  item.modelSizeMb = modelSizeMb;
  item.runtimeSizeMb = runtimeSizeMb;
  item.minRecommendedMemoryMb = minMemoryMb;
  item.preferredMemoryMb = preferredMemoryMb;
  item.widgetSelectable = true;
  item.directDownload = true;
  item.requiresGpu = requiresGpu;
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

    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Tiny_cpu"), QStringLiteral("cpu"),
        QStringLiteral("tiny"), QStringLiteral("CPU"),
        QStringLiteral("pkg_whisper_cpp_tiny"),
        QStringLiteral("rt_whisper_cpp_cpu"), {}, 75, 16, 2048, 4096, false);
    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Small_cpu"), QStringLiteral("cpu"),
        QStringLiteral("small"), QStringLiteral("CPU"),
        QStringLiteral("pkg_whisper_cpp_small"),
        QStringLiteral("rt_whisper_cpp_cpu"), {}, 466, 16, 4096, 8192, false);
    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Turbo_cpu"), QStringLiteral("cpu"),
        QStringLiteral("turbo"), QStringLiteral("CPU"),
        QStringLiteral("pkg_whisper_cpp_turbo"),
        QStringLiteral("rt_whisper_cpp_cpu"), {}, 1600, 16, 8192, 12288,
        false);

    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Tiny_nvidia_cuda"),
        QStringLiteral("nvidia_cuda"), QStringLiteral("tiny"),
        QStringLiteral("NVIDIA CUDA"),
        QStringLiteral("pkg_whisper_cpp_tiny"),
        QStringLiteral("rt_whisper_cpp_nvidia"), {}, 75, 458, 2048, 4096,
        true);
    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Small_nvidia_cuda"),
        QStringLiteral("nvidia_cuda"), QStringLiteral("small"),
        QStringLiteral("NVIDIA CUDA"),
        QStringLiteral("pkg_whisper_cpp_small"),
        QStringLiteral("rt_whisper_cpp_nvidia"), {}, 466, 458, 6144, 8192,
        true);
    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Turbo_nvidia_cuda"),
        QStringLiteral("nvidia_cuda"), QStringLiteral("turbo"),
        QStringLiteral("NVIDIA CUDA"),
        QStringLiteral("pkg_whisper_cpp_turbo"),
        QStringLiteral("rt_whisper_cpp_nvidia"), {}, 1600, 458, 10240, 12288,
        true);

    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Tiny_intel_openvino"),
        QStringLiteral("intel_openvino"), QStringLiteral("tiny"),
        QStringLiteral("Intel OpenVINO GPU"),
        QStringLiteral("pkg_whisper_cpp_tiny"),
        QStringLiteral("rt_whisper_cpp_intel"),
        {QStringLiteral("pkg_whisper_cpp_tiny_intel_encoder")}, 75, 96, 2048,
        4096, true);
    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Small_intel_openvino"),
        QStringLiteral("intel_openvino"), QStringLiteral("small"),
        QStringLiteral("Intel OpenVINO GPU"),
        QStringLiteral("pkg_whisper_cpp_small"),
        QStringLiteral("rt_whisper_cpp_intel"),
        {QStringLiteral("pkg_whisper_cpp_small_intel_encoder")}, 466, 96, 4096,
        6144, true);
    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Turbo_intel_openvino"),
        QStringLiteral("intel_openvino"), QStringLiteral("turbo"),
        QStringLiteral("Intel OpenVINO GPU"),
        QStringLiteral("pkg_whisper_cpp_turbo"),
        QStringLiteral("rt_whisper_cpp_intel"),
        {QStringLiteral("pkg_whisper_cpp_turbo_intel_encoder")}, 1600, 96, 6144,
        8192, true);

    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Tiny_amd_vulkan"),
        QStringLiteral("amd_vulkan"), QStringLiteral("tiny"),
        QStringLiteral("AMD Vulkan GPU"),
        QStringLiteral("pkg_whisper_cpp_tiny"),
        QStringLiteral("rt_whisper_cpp_vulkan"), {}, 75, 40, 2048, 4096,
        true);
    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Small_amd_vulkan"),
        QStringLiteral("amd_vulkan"), QStringLiteral("small"),
        QStringLiteral("AMD Vulkan GPU"),
        QStringLiteral("pkg_whisper_cpp_small"),
        QStringLiteral("rt_whisper_cpp_vulkan"), {}, 466, 40, 4096, 8192,
        true);
    items << makeWhisperCppDescriptor(
        QStringLiteral("whisper.cpp Turbo_amd_vulkan"),
        QStringLiteral("amd_vulkan"), QStringLiteral("turbo"),
        QStringLiteral("AMD Vulkan GPU"),
        QStringLiteral("pkg_whisper_cpp_turbo"),
        QStringLiteral("rt_whisper_cpp_vulkan"), {}, 1600, 40, 8192, 12288,
        true);

    items << makeFasterWhisperDescriptor(
        QStringLiteral("faster-whisper Tiny_cpu"), QStringLiteral("cpu"),
        QStringLiteral("tiny"), QStringLiteral("CPU / CTranslate2"),
        QStringLiteral("pkg_faster_whisper_tiny"), 75, 110, 2048, 4096,
        false);
    items << makeFasterWhisperDescriptor(
        QStringLiteral("faster-whisper Small_cpu"), QStringLiteral("cpu"),
        QStringLiteral("small"), QStringLiteral("CPU / CTranslate2"),
        QStringLiteral("pkg_faster_whisper_small"), 480, 110, 4096, 8192,
        false);
    items << makeFasterWhisperDescriptor(
        QStringLiteral("faster-whisper Turbo_cpu"), QStringLiteral("cpu"),
        QStringLiteral("turbo"), QStringLiteral("CPU / CTranslate2"),
        QStringLiteral("pkg_faster_whisper_turbo"), 1550, 110, 6144, 12288,
        false);

    items << makeFasterWhisperDescriptor(
        QStringLiteral("faster-whisper Tiny_nvidia_cuda"),
        QStringLiteral("nvidia_cuda"), QStringLiteral("tiny"),
        QStringLiteral("NVIDIA CUDA / CTranslate2"),
        QStringLiteral("pkg_faster_whisper_tiny"), 75, 110, 2048, 4096, true);
    items << makeFasterWhisperDescriptor(
        QStringLiteral("faster-whisper Small_nvidia_cuda"),
        QStringLiteral("nvidia_cuda"), QStringLiteral("small"),
        QStringLiteral("NVIDIA CUDA / CTranslate2"),
        QStringLiteral("pkg_faster_whisper_small"), 480, 110, 4096, 8192,
        true);
    items << makeFasterWhisperDescriptor(
        QStringLiteral("faster-whisper Turbo_nvidia_cuda"),
        QStringLiteral("nvidia_cuda"), QStringLiteral("turbo"),
        QStringLiteral("NVIDIA CUDA / CTranslate2"),
        QStringLiteral("pkg_faster_whisper_turbo"), 1550, 110, 6144, 12288,
        true);

    return items;
  }();
  return descriptors;
}

QVector<LocalModelPackageInfo> allPackages() {
  static const QVector<LocalModelPackageInfo> packages = {
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

      {QStringLiteral("rt_whisper_cpp_cpu"),
       QStringLiteral("whisper.cpp CPU Runtime"),
       QStringLiteral("models/runtimes/whisper_cpp_cpu_runtime.zip"),
       QStringLiteral(
           "https://github.com/ggml-org/whisper.cpp/releases/download/v1.8.4/whisper-bin-x64.zip"),
       QStringLiteral("whisper"),
       QStringLiteral("runtimes/whisper_cpp/cpu"),
       {QStringLiteral("runtimes/whisper_cpp/cpu/whisper-cli.exe")},
       {QStringLiteral("runtimes/whisper_cpp/cpu")}, true},
      {QStringLiteral("rt_whisper_cpp_nvidia"),
       QStringLiteral("whisper.cpp NVIDIA Runtime"),
       QStringLiteral("models/runtimes/whisper_cpp_nvidia_runtime.zip"),
       QStringLiteral(
           "https://github.com/ggml-org/whisper.cpp/releases/download/v1.8.4/whisper-cublas-12.4.0-bin-x64.zip"),
       QStringLiteral("whisper"),
       QStringLiteral("runtimes/whisper_cpp/nvidia_cuda"),
       {QStringLiteral("runtimes/whisper_cpp/nvidia_cuda/whisper-cli.exe")},
       {QStringLiteral("runtimes/whisper_cpp/nvidia_cuda")}, true},
      {QStringLiteral("rt_whisper_cpp_intel"),
       QStringLiteral("whisper.cpp Intel OpenVINO Runtime"),
       QStringLiteral("models/runtimes/whisper_cpp_intel_openvino_runtime.zip"),
       QString(), QStringLiteral("whisper"),
       QStringLiteral("runtimes/whisper_cpp/intel_openvino"),
       {QStringLiteral(
           "runtimes/whisper_cpp/intel_openvino/whisper-cli.exe")},
       {QStringLiteral("runtimes/whisper_cpp/intel_openvino")}, true},
      {QStringLiteral("rt_whisper_cpp_vulkan"),
       QStringLiteral("whisper.cpp Vulkan Runtime"),
       QStringLiteral("models/runtimes/whisper_cpp_vulkan_runtime.zip"),
       QString(), QStringLiteral("whisper"),
       QStringLiteral("runtimes/whisper_cpp/amd_vulkan"),
       {QStringLiteral("runtimes/whisper_cpp/amd_vulkan/whisper-cli.exe")},
       {QStringLiteral("runtimes/whisper_cpp/amd_vulkan")}, true},

      {QStringLiteral("pkg_whisper_cpp_tiny"),
       QStringLiteral("whisper.cpp Tiny Model"),
       QStringLiteral("models/whisper_cpp/ggml-tiny.bin"),
       QStringLiteral(
           "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin"),
       QStringLiteral("whisper"), QString(),
       {QStringLiteral("models/whisper_cpp/tiny/ggml-tiny.bin")},
       {QStringLiteral("models/whisper_cpp/tiny/ggml-tiny.bin")}, false},
      {QStringLiteral("pkg_whisper_cpp_small"),
       QStringLiteral("whisper.cpp Small Model"),
       QStringLiteral("models/whisper_cpp/ggml-small.bin"),
       QStringLiteral(
           "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin"),
       QStringLiteral("whisper"), QString(),
       {QStringLiteral("models/whisper_cpp/small/ggml-small.bin")},
       {QStringLiteral("models/whisper_cpp/small/ggml-small.bin")}, false},
      {QStringLiteral("pkg_whisper_cpp_turbo"),
       QStringLiteral("whisper.cpp Turbo Model"),
       QStringLiteral("models/whisper_cpp/ggml-large-v3-turbo.bin"),
       QStringLiteral(
           "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin"),
       QStringLiteral("whisper"), QString(),
       {QStringLiteral(
           "models/whisper_cpp/turbo/ggml-large-v3-turbo.bin")},
       {QStringLiteral(
           "models/whisper_cpp/turbo/ggml-large-v3-turbo.bin")},
       false},

      {QStringLiteral("pkg_whisper_cpp_tiny_intel_encoder"),
       QStringLiteral("whisper.cpp Tiny Intel Encoder"),
       QStringLiteral(
           "models/whisper_cpp/openvino/ggml-tiny-encoder-openvino.zip"),
       QStringLiteral(
           "https://huggingface.co/Whisper-Pascal/whisper-openvino/resolve/main/ggml-tiny-encoder-openvino.zip"),
       QStringLiteral("whisper"),
       QStringLiteral("models/whisper_cpp/tiny"),
       {QStringLiteral(
           "models/whisper_cpp/tiny/ggml-tiny-encoder-openvino.xml")},
       {QStringLiteral("models/whisper_cpp/tiny/ggml-tiny-encoder-openvino.bin"),
        QStringLiteral(
            "models/whisper_cpp/tiny/ggml-tiny-encoder-openvino.xml")},
       true},
      {QStringLiteral("pkg_whisper_cpp_small_intel_encoder"),
       QStringLiteral("whisper.cpp Small Intel Encoder"),
       QStringLiteral(
           "models/whisper_cpp/openvino/ggml-small-encoder-openvino.zip"),
       QStringLiteral(
           "https://huggingface.co/Whisper-Pascal/whisper-openvino/resolve/main/ggml-small-encoder-openvino.zip"),
       QStringLiteral("whisper"),
       QStringLiteral("models/whisper_cpp/small"),
       {QStringLiteral(
           "models/whisper_cpp/small/ggml-small-encoder-openvino.xml")},
       {QStringLiteral("models/whisper_cpp/small/ggml-small-encoder-openvino.bin"),
        QStringLiteral(
            "models/whisper_cpp/small/ggml-small-encoder-openvino.xml")},
       true},
      {QStringLiteral("pkg_whisper_cpp_turbo_intel_encoder"),
       QStringLiteral("whisper.cpp Turbo Intel Encoder"),
       QStringLiteral(
           "models/whisper_cpp/openvino/ggml-large-v3-turbo-encoder-openvino.zip"),
       QStringLiteral(
           "https://huggingface.co/Whisper-Pascal/whisper-openvino/resolve/main/ggml-large-v3-turbo-encoder-openvino.zip"),
       QStringLiteral("whisper"),
       QStringLiteral("models/whisper_cpp/turbo"),
       {QStringLiteral(
           "models/whisper_cpp/turbo/ggml-large-v3-turbo-encoder-openvino.xml")},
       {QStringLiteral(
            "models/whisper_cpp/turbo/ggml-large-v3-turbo-encoder-openvino.bin"),
        QStringLiteral(
            "models/whisper_cpp/turbo/ggml-large-v3-turbo-encoder-openvino.xml")},
       true},

      {QStringLiteral("rt_faster_whisper"),
       QStringLiteral("faster-whisper Runtime"),
       QStringLiteral("models/runtimes/faster_whisper_runtime.zip"), QString(),
       QStringLiteral("whisper"),
       QStringLiteral("runtimes/faster_whisper"),
       {QStringLiteral("runtimes/faster_whisper/faster_whisper_runner.exe")},
       {QStringLiteral("runtimes/faster_whisper")}, true},
      {QStringLiteral("pkg_faster_whisper_tiny"),
       QStringLiteral("faster-whisper Tiny Model"),
       QStringLiteral("models/faster_whisper/tiny.zip"), QString(),
       QStringLiteral("whisper"), QStringLiteral("models/faster_whisper/tiny"),
       {QStringLiteral("models/faster_whisper/tiny/model.bin")},
       {QStringLiteral("models/faster_whisper/tiny")}, true},
      {QStringLiteral("pkg_faster_whisper_small"),
       QStringLiteral("faster-whisper Small Model"),
       QStringLiteral("models/faster_whisper/small.zip"), QString(),
       QStringLiteral("whisper"), QStringLiteral("models/faster_whisper/small"),
       {QStringLiteral("models/faster_whisper/small/model.bin")},
       {QStringLiteral("models/faster_whisper/small")}, true},
      {QStringLiteral("pkg_faster_whisper_turbo"),
       QStringLiteral("faster-whisper Turbo Model"),
       QStringLiteral("models/faster_whisper/turbo.zip"), QString(),
       QStringLiteral("whisper"), QStringLiteral("models/faster_whisper/turbo"),
       {QStringLiteral("models/faster_whisper/turbo/model.bin")},
       {QStringLiteral("models/faster_whisper/turbo")}, true},
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

} // namespace

QVector<ComputeTargetInfo> detectComputeTargets() {
  QVector<ComputeTargetInfo> targets;

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
        target.whisperGpuSupported = true;
        break;
      case 0x8086:
        target.vendorName = QStringLiteral("Intel");
        target.backendLabel = QStringLiteral("OpenVINO GPU");
        target.whisperGpuSupported = true;
        break;
      case 0x1002:
      case 0x1022:
        target.vendorName = QStringLiteral("AMD");
        target.backendLabel = QStringLiteral("Vulkan GPU");
        target.whisperGpuSupported = true;
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
  cpuTarget.backendLabel = QStringLiteral("whisper.cpp CPU / faster-whisper CPU");
  cpuTarget.systemMemoryMb = int(memoryStatus.ullTotalPhys / (1024ull * 1024ull));
  cpuTarget.isCpuFallback = true;
  targets << cpuTarget;
  return targets;
}

QString defaultComputeTargetId(const QVector<ComputeTargetInfo> &targets) {
  for (const ComputeTargetInfo &target : targets) {
    if (!target.isCpuFallback && target.whisperGpuSupported)
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
        "CPU-only whisper.cpp and faster-whisper packages are available. Tiny is the safest default; Small and Turbo need more RAM.");
  }
  if (!target.whisperGpuSupported) {
    return QStringLiteral(
        "This GPU is detected, but only CPU whisper packages are enabled for it right now.");
  }
  if (targetBackendId(target) == QStringLiteral("intel_openvino")) {
    if (targetUsableMemoryMb(target) >= 6144) {
      return QStringLiteral(
          "Recommended: whisper.cpp Small_intel_openvino. Tiny is safe. Turbo may work if the driver and VRAM budget hold.");
    }
    return QStringLiteral(
        "Recommended: whisper.cpp Tiny_intel_openvino. Small may work with reduced headroom.");
  }
  if (targetBackendId(target) == QStringLiteral("amd_vulkan")) {
    if (targetUsableMemoryMb(target) >= 8192) {
      return QStringLiteral(
          "Recommended: whisper.cpp Small_amd_vulkan. Tiny is safe. Turbo may work if the Vulkan driver and VRAM budget hold.");
    }
    return QStringLiteral(
        "Recommended: whisper.cpp Tiny_amd_vulkan. Small may work with reduced headroom.");
  }
  if (target.dedicatedVramMb >= 10240) {
    return QStringLiteral(
        "Recommended: whisper.cpp Turbo_nvidia_cuda or faster-whisper Turbo_nvidia_cuda. Tiny and Small should also fit comfortably.");
  }
  if (target.dedicatedVramMb >= 6144) {
    return QStringLiteral(
        "Recommended: whisper.cpp Small_nvidia_cuda or faster-whisper Small_nvidia_cuda. Tiny is safe. Turbo may be heavy.");
  }
  return QStringLiteral(
      "Recommended: whisper.cpp Tiny_nvidia_cuda or faster-whisper Tiny_nvidia_cuda. Small may work with reduced headroom.");
}

QVector<LocalModelDescriptor>
localDashboardCatalogForTarget(const ComputeTargetInfo &target) {
  QVector<LocalModelDescriptor> filtered;
  const QString backend = targetBackendId(target);
  for (const LocalModelDescriptor &descriptor : allDescriptors()) {
    if (descriptor.engineFamily == QStringLiteral("vosk")) {
      filtered << descriptor;
      continue;
    }
    if (descriptor.backendKey == QStringLiteral("cpu")) {
      filtered << descriptor;
      continue;
    }
    if (descriptor.backendKey == backend)
      filtered << descriptor;
  }
  return filtered;
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

bool isWhisperLocalModel(const QString &modelName) {
  const QString family = localModelDescriptor(modelName).engineFamily;
  return family == QStringLiteral("whisper.cpp") ||
         family == QStringLiteral("faster-whisper");
}

bool localModelWidgetSelectable(const QString &modelName) {
  return localModelDescriptor(modelName).widgetSelectable;
}

bool localModelSupportsDirectDownload(const QString &modelName) {
  return localModelDescriptor(modelName).directDownload;
}

bool localModelSupportsRuntimeNow(const QString &modelName) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  return descriptor.engineFamily == QStringLiteral("vosk") ||
         descriptor.engineFamily == QStringLiteral("whisper.cpp") ||
         descriptor.engineFamily == QStringLiteral("faster-whisper");
}

bool localModelRequiresFrontendTranscription(const QString &modelName) {
  const QString family = localModelDescriptor(modelName).engineFamily;
  return family == QStringLiteral("whisper.cpp") ||
         family == QStringLiteral("faster-whisper");
}

bool isLocalModelInstalled(const QString &modelName) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (descriptor.displayName.isEmpty() || descriptor.packageId.isEmpty())
    return false;

  if (!packageInstalled(localModelPackage(descriptor.packageId)))
    return false;

  if (!descriptor.runtimePackageId.isEmpty() &&
      !packageInstalled(localModelPackage(descriptor.runtimePackageId))) {
    return false;
  }

  for (const QString &extraPackageId : descriptor.extraPackageIds) {
    if (!packageInstalled(localModelPackage(extraPackageId)))
      return false;
  }
  return true;
}

QString localModelStateText(const QString &modelName, bool installed) {
  const LocalModelDescriptor descriptor = localModelDescriptor(modelName);
  if (installed)
    return QStringLiteral("[Installed]");
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
    return QStringLiteral("%1 is installed.").arg(modelName);
  if (descriptor.engineFamily == QStringLiteral("whisper.cpp")) {
    return QStringLiteral(
               "%1 downloads the optional whisper.cpp model plus its runtime dependencies.")
        .arg(modelName);
  }
  if (descriptor.engineFamily == QStringLiteral("faster-whisper")) {
    return QStringLiteral(
               "%1 downloads the optional faster-whisper model plus its standalone runner.")
        .arg(modelName);
  }
  if (descriptor.directDownload)
    return QStringLiteral("%1 can be downloaded directly.").arg(modelName);
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

  QStringList lines;
  lines << descriptor.displayName;
  lines << QStringLiteral("Engine: %1").arg(descriptor.engineFamily);
  lines << QStringLiteral("Backend: %1").arg(descriptor.accelerationLabel);
  lines << descriptor.description;
  lines << QStringLiteral("Model package size: %1")
               .arg(formatMemoryMb(descriptor.modelSizeMb));
  if (descriptor.runtimeSizeMb > 0) {
    lines << QStringLiteral("Runtime package size: %1")
                 .arg(formatMemoryMb(descriptor.runtimeSizeMb));
  }
  lines << localModelRecommendationText(modelName, target);
  lines << QStringLiteral("Widget use today: %1")
               .arg(descriptor.widgetSelectable ? QStringLiteral("Supported")
                                                : QStringLiteral("Not supported"));
  return lines.join(QLatin1Char('\n'));
}

QString localModelDisplayState(const QString &modelName, bool installed,
                               bool widgetChecked) {
  QString text = modelName + QLatin1Char(' ') + localModelStateText(modelName, installed);
  if (widgetChecked)
    text += QStringLiteral(" [Widget]");
  return text;
}

LocalModelDescriptor localModelDescriptor(const QString &modelName) {
  const QString trimmed = modelName.trimmed();
  for (const LocalModelDescriptor &descriptor : allDescriptors()) {
    if (descriptor.displayName.compare(trimmed, Qt::CaseInsensitive) == 0)
      return descriptor;
  }
  return LocalModelDescriptor();
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

  if (!descriptor.runtimePackageId.isEmpty() &&
      !packageInstalled(localModelPackage(descriptor.runtimePackageId))) {
    sequence << descriptor.runtimePackageId;
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
    if (descriptor.runtimePackageId == runtimePackageId &&
        isLocalModelInstalled(descriptor.displayName)) {
      models << descriptor.displayName;
    }
  }
  return models;
}

QString localModelInstalledPath(const QString &modelName) {
  return localModelDescriptor(modelName).installedPath;
}

QString localModelRuntimeExecutablePath(const QString &modelName) {
  return localModelDescriptor(modelName).runtimeExecutablePath;
}

QString localModelRunnerModelId(const QString &modelName) {
  return localModelDescriptor(modelName).runnerModelId;
}

QString localModelBackendKey(const QString &modelName) {
  return localModelDescriptor(modelName).backendKey;
}

QString quickSttDataRoot() {
  const QString appRoot =
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
  QDir().mkpath(appRoot);
  return appRoot;
}

QString quickSttModelsRoot() {
  const QString path =
      QDir(quickSttDataRoot()).filePath(QStringLiteral("models"));
  QDir().mkpath(path);
  return path;
}

QString quickSttWhisperRoot() {
  const QString path =
      QDir(quickSttDataRoot()).filePath(QStringLiteral("whisper"));
  QDir().mkpath(path);
  return path;
}

QString installRootPathForKey(const QString &rootKey) {
  return rootKey == QStringLiteral("models") ? quickSttModelsRoot()
                                             : quickSttWhisperRoot();
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
  if (!urls.contains(QStringLiteral("http://127.0.0.1:5000")))
    urls << QStringLiteral("http://127.0.0.1:5000");
  if (!urls.contains(QStringLiteral("http://localhost:5000")))
    urls << QStringLiteral("http://localhost:5000");
  urls.removeAll(QString());
  urls.removeDuplicates();
  return urls;
}
