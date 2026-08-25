#include "audio_preprocess.h"

#include "platform.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#ifdef QUICKSTT_USE_STATIC_RNNOISE
#include "rnnoise.h"
#endif

namespace fs = std::filesystem;

namespace {

constexpr int kSampleRate = 16000;
constexpr int kTenVadHopSamples = 256;
// Wake-friendly default. Dictation endpointing still uses speech hangover
// timing in stt_service_native; this only marks speechLikely for gates.
// Handy Silero-style onset sits near 0.30; 0.28 keeps quiet wake phrases.
constexpr float kTenVadThreshold = 0.28f;

std::string joinPath(const std::string &a, const std::string &b) {
  return (fs::path(a) / b).string();
}

bool existsFile(const std::string &path) {
  std::error_code ec;
  return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}

void *loadLibraryAt(const std::string &path) {
  if (!existsFile(path))
    return nullptr;
  return platform_load(path.c_str());
}

void unloadLibrary(void *dll) {
  if (dll)
    platform_unload(reinterpret_cast<platform_handle_t>(dll));
}

void *procAddress(void *dll, const char *name) {
  return dll ? platform_symbol(reinterpret_cast<platform_handle_t>(dll), name)
              : nullptr;
}

int16_t clampSample(float value) {
  value = std::max(-32768.0f, std::min(32767.0f, value));
  return static_cast<int16_t>(std::lrint(value));
}

} // namespace

bool AudioPreprocessor::init(const std::string &exeDir) {
  std::vector<std::string> roots = {
      joinPath(exeDir, "audio_preprocess"),
      (std::filesystem::path(exeDir) / "data" / "audio_preprocess").string(),
      exeDir,
  };
  initRnnoise(roots);
  initTenVad(roots);
  return m_rnnoiseReady || m_tenVadReady;
}

void AudioPreprocessor::shutdown() {
  if (m_rnnoiseState && m_rnnoiseDestroy) {
    m_rnnoiseDestroy(m_rnnoiseState);
    m_rnnoiseState = nullptr;
  }
  unloadLibrary(m_rnnoiseDll);
  m_rnnoiseDll = nullptr;
  m_rnnoiseReady = false;

  if (m_tenVadHandle && m_tenVadDestroy)
    m_tenVadDestroy(&m_tenVadHandle);
  m_tenVadHandle = nullptr;
  unloadLibrary(m_tenVadDll);
  m_tenVadDll = nullptr;
  m_tenVadReady = false;
}

void AudioPreprocessor::initRnnoise(const std::vector<std::string> &roots) {
#ifdef QUICKSTT_USE_STATIC_RNNOISE
  m_rnnoiseDestroy = reinterpret_cast<RnnoiseDestroyFn>(&rnnoise_destroy);
  m_rnnoiseProcessFrame =
      reinterpret_cast<RnnoiseProcessFrameFn>(&rnnoise_process_frame);
  m_rnnoiseFrameSize = rnnoise_get_frame_size();
  m_rnnoiseState = rnnoise_create(nullptr);
  m_rnnoiseReady = m_rnnoiseState && m_rnnoiseFrameSize == 480;
  if (m_rnnoiseReady)
    return;
#endif

  const std::vector<std::string> relativeCandidates = {
#ifdef _WIN32
      (std::filesystem::path("rnnoise") / "rnnoise.dll").string(),
      "rnnoise.dll",
#else
      (std::filesystem::path("rnnoise") / "librnnoise.so").string(),
      "librnnoise.so",
      (std::filesystem::path("rnnoise") / "rnnoise.so").string(),
#endif
  };
  for (const std::string &root : roots) {
    for (const std::string &rel : relativeCandidates) {
      m_rnnoiseDll = loadLibraryAt(joinPath(root, rel));
      if (m_rnnoiseDll)
        break;
    }
    if (m_rnnoiseDll)
      break;
  }
  if (!m_rnnoiseDll)
    return;

  auto create = reinterpret_cast<RnnoiseCreateFn>(
      procAddress(m_rnnoiseDll, "rnnoise_create"));
  m_rnnoiseDestroy = reinterpret_cast<RnnoiseDestroyFn>(
      procAddress(m_rnnoiseDll, "rnnoise_destroy"));
  m_rnnoiseProcessFrame = reinterpret_cast<RnnoiseProcessFrameFn>(
      procAddress(m_rnnoiseDll, "rnnoise_process_frame"));
  auto getFrameSize = reinterpret_cast<RnnoiseGetFrameSizeFn>(
      procAddress(m_rnnoiseDll, "rnnoise_get_frame_size"));
  if (!create || !m_rnnoiseDestroy || !m_rnnoiseProcessFrame || !getFrameSize) {
    unloadLibrary(m_rnnoiseDll);
    m_rnnoiseDll = nullptr;
    return;
  }

  m_rnnoiseFrameSize = getFrameSize();
  m_rnnoiseState = create(nullptr);
  m_rnnoiseReady = m_rnnoiseState && m_rnnoiseFrameSize == 480;
  if (!m_rnnoiseReady)
    shutdown();
}

void AudioPreprocessor::initTenVad(const std::vector<std::string> &roots) {
  const std::vector<std::string> relativeCandidates = {
#ifdef _WIN32
      (std::filesystem::path("ten_vad") / "ten_vad.dll").string(),
      "ten_vad.dll",
#else
      (std::filesystem::path("ten_vad") / "libten_vad.so").string(),
      "libten_vad.so",
#endif
  };
  for (const std::string &root : roots) {
    for (const std::string &rel : relativeCandidates) {
      m_tenVadDll = loadLibraryAt(joinPath(root, rel));
      if (m_tenVadDll)
        break;
    }
    if (m_tenVadDll)
      break;
  }
  if (!m_tenVadDll)
    return;

  auto create =
      reinterpret_cast<TenVadCreateFn>(procAddress(m_tenVadDll, "ten_vad_create"));
  m_tenVadProcess = reinterpret_cast<TenVadProcessFn>(
      procAddress(m_tenVadDll, "ten_vad_process"));
  m_tenVadDestroy = reinterpret_cast<TenVadDestroyFn>(
      procAddress(m_tenVadDll, "ten_vad_destroy"));
  if (!create || !m_tenVadProcess || !m_tenVadDestroy ||
      create(&m_tenVadHandle, kTenVadHopSamples, kTenVadThreshold) != 0 ||
      !m_tenVadHandle) {
    unloadLibrary(m_tenVadDll);
    m_tenVadDll = nullptr;
    m_tenVadReady = false;
    return;
  }
  m_tenVadReady = true;
}

AudioPreprocessResult
AudioPreprocessor::process(const std::vector<int16_t> &input) {
  AudioPreprocessResult result;
  result.samples = m_rnnoiseReady ? processRnnoise(input) : input;
  updateTenVad(result.samples, result);
  return result;
}

std::vector<int16_t>
AudioPreprocessor::processRnnoise(const std::vector<int16_t> &input) {
  m_rnnoisePending16k.insert(m_rnnoisePending16k.end(), input.begin(),
                             input.end());
  std::vector<int16_t> output;
  output.reserve(input.size());

  while (m_rnnoisePending16k.size() >= 160) {
    std::vector<float> frame48k(480);
    for (int i = 0; i < 160; ++i) {
      const float current = static_cast<float>(m_rnnoisePending16k[i]);
      const float next = i + 1 < 160
                             ? static_cast<float>(m_rnnoisePending16k[i + 1])
                             : current;
      frame48k[i * 3] = current;
      frame48k[i * 3 + 1] = current + (next - current) / 3.0f;
      frame48k[i * 3 + 2] = current + 2.0f * (next - current) / 3.0f;
    }

    std::vector<float> denoised48k(480);
    m_rnnoiseProcessFrame(m_rnnoiseState, denoised48k.data(), frame48k.data());
    for (int i = 0; i < 160; ++i) {
      const float mixed = (denoised48k[i * 3] + denoised48k[i * 3 + 1] +
                           denoised48k[i * 3 + 2]) /
                          3.0f;
      output.push_back(clampSample(mixed));
    }
    m_rnnoisePending16k.erase(m_rnnoisePending16k.begin(),
                              m_rnnoisePending16k.begin() + 160);
  }

  return output.empty() ? input : output;
}

void AudioPreprocessor::updateTenVad(const std::vector<int16_t> &samples,
                                     AudioPreprocessResult &result) {
  if (!m_tenVadReady) {
    result.speechLikely = true;
    result.vadProbability = 1.0f;
    result.vadAvailable = false;
    return;
  }

  result.vadAvailable = true;
  m_tenVadPending.insert(m_tenVadPending.end(), samples.begin(), samples.end());
  while (m_tenVadPending.size() >= kTenVadHopSamples) {
    float probability = 0.0f;
    int speechFlag = 0;
    if (m_tenVadProcess(m_tenVadHandle, m_tenVadPending.data(),
                        kTenVadHopSamples, &probability, &speechFlag) == 0) {
      m_lastVadProbability = probability;
      if (speechFlag)
        m_tenVadSpeechHoldFrames = 12;
      else if (m_tenVadSpeechHoldFrames > 0)
        --m_tenVadSpeechHoldFrames;
    }
    m_tenVadPending.erase(m_tenVadPending.begin(),
                          m_tenVadPending.begin() + kTenVadHopSamples);
  }

  result.vadProbability = m_lastVadProbability;
  result.speechLikely = m_tenVadSpeechHoldFrames > 0;
}
