// stt_service_native.cpp — Complete native C++ STT service
// Replaces stt_engine.py + stt_service.py + oww_lite.py entirely
// Uses: libvosk.dll (C API), onnxruntime.dll (C API), winmm.dll (waveIn),
// user32.dll (SendInput) Zero Python dependency for Vosk + OWW. Python only
// needed for Whisper (on-demand).
//
// Communicates with QuickSTT_App via stdin/stdout pipe protocol:
//   OUT: STATE|code,message  FINAL_TEXT|text  AUDIO_LEVEL|0-100  DL_PROGRESS|%
//   DL_COMPLETE|name IN:  TOGGLE  STOP  SLEEP  MODEL:name  WAKEWORDS:csv
//   CLOSEWORDS:csv  WAKEMODE:engine  QUIT

#include <windows.h>
// Prevent formatter from sorting these
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Force-clear SAL annotations from MinGW's sal.h BEFORE including ORT headers.
// MinGW's sal.h defines these with actual GCC attributes that break the
// function pointer declarations in onnxruntime_c_api.h (ORT_API2_STATUS macro).
#ifdef _Check_return_
#undef _Check_return_
#define _Check_return_
#endif
#ifdef _Success_
#undef _Success_
#define _Success_(x)
#endif
#ifdef _In_
#undef _In_
#define _In_
#endif
#ifdef _In_z_
#undef _In_z_
#define _In_z_
#endif
#ifdef _In_opt_
#undef _In_opt_
#define _In_opt_
#endif
#ifdef _In_opt_z_
#undef _In_opt_z_
#define _In_opt_z_
#endif
#ifdef _Out_
#undef _Out_
#define _Out_
#endif
#ifdef _Outptr_
#undef _Outptr_
#define _Outptr_
#endif
#ifdef _Out_opt_
#undef _Out_opt_
#define _Out_opt_
#endif
#ifdef _Inout_
#undef _Inout_
#define _Inout_
#endif
#ifdef _Inout_opt_
#undef _Inout_opt_
#define _Inout_opt_
#endif
#ifdef _Frees_ptr_opt_
#undef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifdef _Ret_maybenull_
#undef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifdef _Ret_notnull_
#undef _Ret_notnull_
#define _Ret_notnull_
#endif
#ifdef _In_reads_
#undef _In_reads_
#define _In_reads_(X)
#endif
#ifdef _Inout_updates_
#undef _Inout_updates_
#define _Inout_updates_(X)
#endif
#ifdef _Out_writes_
#undef _Out_writes_
#define _Out_writes_(X)
#endif
#ifdef _Inout_updates_all_
#undef _Inout_updates_all_
#define _Inout_updates_all_(X)
#endif
#ifdef _Out_writes_bytes_all_
#undef _Out_writes_bytes_all_
#define _Out_writes_bytes_all_(X)
#endif
#ifdef _Out_writes_all_
#undef _Out_writes_all_
#define _Out_writes_all_(X)
#endif
#ifdef _Outptr_result_buffer_maybenull_
#undef _Outptr_result_buffer_maybenull_
#define _Outptr_result_buffer_maybenull_(X)
#endif
#ifdef _Outptr_result_maybenull_
#undef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif

// MinGW fix for ORT_API_CALL and _stdcall
#ifndef _stdcall
#define _stdcall __stdcall
#endif
#ifndef ORT_API_CALL
#define ORT_API_CALL __stdcall
#endif

#include "tflite_loader.h"
#include "oww_tflite.h"
#include "pv_native.h"
#include "vosk_api.h"
#include "portaudio.h"

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════════
// Logging & Events
// ═══════════════════════════════════════════════════════════════════════════════

static std::mutex g_stdout_mtx;

static void svc_log(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  fprintf(stderr, "[ENGINE] ");
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");
  fflush(stderr);
  va_end(args);
}

static void sendEvent(const std::string &type, std::string payload) {
  // Ensure payload is single-line to avoid breaking pipe protocol
  std::replace(payload.begin(), payload.end(), '\n', ' ');
  std::replace(payload.begin(), payload.end(), '\r', ' ');
  std::lock_guard<std::mutex> lock(g_stdout_mtx);
  printf("%s|%s\n", type.c_str(), payload.c_str());
  fflush(stdout);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Minimal JSON helpers (Vosk returns simple JSON like {"text": "hello"})
// ═══════════════════════════════════════════════════════════════════════════════

static std::string jsonGetString(const char *json, const char *key) {
  // Find "key" : "value" in JSON string
  std::string search = std::string("\"") + key + "\"";
  const char *pos = strstr(json, search.c_str());
  if (!pos)
    return "";
  pos += search.length();
  // Skip whitespace and colon
  while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t'))
    pos++;
  if (*pos != '"')
    return "";
  pos++; // skip opening quote
  std::string result;
  while (*pos && *pos != '"') {
    if (*pos == '\\' && *(pos + 1)) {
      pos++;
      if (*pos == 'n')
        result += '\n';
      else if (*pos == 't')
        result += '\t';
      else
        result += *pos;
    } else {
      result += *pos;
    }
    pos++;
  }
  return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Settings (read from Windows Registry — same keys as Python version)
// ═══════════════════════════════════════════════════════════════════════════════

struct Settings {
  std::vector<std::string> wakeWords = {"hey jarvis", "alexa"};
  std::vector<std::string> closeWords = {"stop listening", "go to sleep"};
  std::string wakeEngine = "OpenWakeWord (TFLite)";
  std::string porcupineAccessKey = "";
  std::string recordingDir;
  bool autoOffloadEnabled = true;
  int autoOffloadDelaySec = 120;
  bool autoModelLoad = false;
};

static std::vector<std::string> splitCSV(const std::string &s) {
  std::vector<std::string> result;
  std::istringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) {
    // Trim
    while (!item.empty() && item.front() == ' ')
      item.erase(item.begin());
    while (!item.empty() && item.back() == ' ')
      item.pop_back();
    if (!item.empty())
      result.push_back(item);
  }
  return result;
}

static std::string lowercaseCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return value;
}

static std::string canonicalWakeEngine(std::string value) {
  const std::string lower = lowercaseCopy(value);
  if (lower.find("porcupine") != std::string::npos)
    return "Porcupine (Access Key Required)";
  if (lower.find("vosk") != std::string::npos)
    return "Vosk Keyword (Built-in)";
  return "OpenWakeWord (TFLite)";
}

static Settings loadSettings() {
  Settings s;
  HKEY key;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\QuickSTT\\Config", 0,
                    KEY_READ, &key) == ERROR_SUCCESS) {
    char buf[4096];
    DWORD sz, type;

    sz = sizeof(buf);
    type = 0;
    if (RegQueryValueExA(key, "wakeWords", nullptr, &type, (BYTE *)buf, &sz) ==
        ERROR_SUCCESS) {
      std::vector<std::string> words;
      if (type == REG_MULTI_SZ) {
        // REG_MULTI_SZ: null-delimited, double-null terminated
        const char *p = buf;
        while (p < buf + sz && *p) {
          std::string w(p);
          if (!w.empty()) words.push_back(w);
          p += w.size() + 1;
        }
      } else {
        buf[sz] = 0;
        words = splitCSV(buf);
      }
      if (!words.empty())
        s.wakeWords = words;
      svc_log("Loaded %zu wake words from registry", s.wakeWords.size());
      for (const auto &w : s.wakeWords) svc_log("  wakeWord: '%s'", w.c_str());
    }

    sz = sizeof(buf);
    type = 0;
    if (RegQueryValueExA(key, "closeWords", nullptr, &type, (BYTE *)buf, &sz) ==
        ERROR_SUCCESS) {
      std::vector<std::string> words;
      if (type == REG_MULTI_SZ) {
        const char *p = buf;
        while (p < buf + sz && *p) {
          std::string w(p);
          if (!w.empty()) words.push_back(w);
          p += w.size() + 1;
        }
      } else {
        buf[sz] = 0;
        words = splitCSV(buf);
      }
      if (!words.empty())
        s.closeWords = words;
    }

    sz = sizeof(buf);
    type = 0;
    if (RegQueryValueExA(key, "wakeEngine", nullptr, &type, (BYTE *)buf, &sz) ==
        ERROR_SUCCESS) {
      buf[sz] = 0;
      s.wakeEngine = canonicalWakeEngine(buf);
    }

    sz = sizeof(buf);
    type = 0;
    if (RegQueryValueExA(key, "porcupineAccessKey", nullptr, &type, (BYTE *)buf, &sz) ==
        ERROR_SUCCESS) {
      buf[sz] = 0;
      s.porcupineAccessKey = buf;
    }

    sz = sizeof(buf);
    type = 0;
    if (RegQueryValueExA(key, "recordingDir", nullptr, &type, (BYTE *)buf,
                         &sz) == ERROR_SUCCESS) {
      buf[sz] = 0;
      s.recordingDir = buf;
    }

    // Auto-offload settings (stored as REG_SZ strings from Qt QSettings)
    // Auto-offload settings (stored as REG_SZ or REG_DWORD by Qt)
    sz = sizeof(buf);
    type = 0;
    if (RegQueryValueExA(key, "autoOffloadEnabled", nullptr, &type, (BYTE *)buf,
                         &sz) == ERROR_SUCCESS) {
      if (type == REG_SZ) {
        buf[sz] = 0;
        std::string val(buf);
        s.autoOffloadEnabled = (val == "true" || val == "1");
      } else if (type == REG_DWORD) {
        s.autoOffloadEnabled = (*(DWORD *)buf) != 0;
      }
    }

    sz = sizeof(buf);
    type = 0;
    if (RegQueryValueExA(key, "autoOffloadDelaySec", nullptr, &type,
                         (BYTE *)buf, &sz) == ERROR_SUCCESS) {
      if (type == REG_SZ) {
        buf[sz] = 0;
        try {
          s.autoOffloadDelaySec = std::stoi(buf);
        } catch (...) {
        }
      } else if (type == REG_DWORD) {
        s.autoOffloadDelaySec = (*(DWORD *)buf);
      }
    }

    sz = sizeof(buf);
    type = 0;
    if (RegQueryValueExA(key, "autoModelLoad", nullptr, &type, (BYTE *)buf,
                         &sz) == ERROR_SUCCESS) {
      if (type == REG_SZ) {
        buf[sz] = 0;
        std::string val(buf);
        s.autoModelLoad = (val == "true" || val == "1");
      } else if (type == REG_DWORD) {
        s.autoModelLoad = (*(DWORD *)buf) != 0;
      }
    }

    RegCloseKey(key);
  }
  return s;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Path helpers
// ═══════════════════════════════════════════════════════════════════════════════

static std::string getExeDir() {
  char buf[MAX_PATH];
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string path(buf);
  size_t pos = path.find_last_of("\\/");
  return (pos != std::string::npos) ? path.substr(0, pos) : ".";
}

static std::string getAppDataDir() {
  std::string root = getExeDir();
  std::string local_data = root + "\\data";
  if (fs::exists(local_data))
    return local_data;

  char *appdata = getenv("APPDATA");
  if (appdata) {
    std::string dir = std::string(appdata) + "\\QuickSTT";
    fs::create_directories(dir);
    return dir;
  }
  return root;
}

static bool writeWavFile(const fs::path &path,
                         const std::vector<int16_t> &samples) {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open())
    return false;

  const uint16_t audioFormat = 1;
  const uint16_t numChannels = 1;
  const uint32_t sampleRate = 16000;
  const uint16_t bitsPerSample = 16;
  const uint16_t blockAlign = uint16_t(numChannels * (bitsPerSample / 8));
  const uint32_t byteRate = sampleRate * blockAlign;
  const uint32_t dataSize =
      uint32_t(samples.size() * sizeof(int16_t));
  const uint32_t riffSize = 36u + dataSize;

  out.write("RIFF", 4);
  out.write(reinterpret_cast<const char *>(&riffSize), sizeof(riffSize));
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  const uint32_t fmtSize = 16;
  out.write(reinterpret_cast<const char *>(&fmtSize), sizeof(fmtSize));
  out.write(reinterpret_cast<const char *>(&audioFormat), sizeof(audioFormat));
  out.write(reinterpret_cast<const char *>(&numChannels), sizeof(numChannels));
  out.write(reinterpret_cast<const char *>(&sampleRate), sizeof(sampleRate));
  out.write(reinterpret_cast<const char *>(&byteRate), sizeof(byteRate));
  out.write(reinterpret_cast<const char *>(&blockAlign), sizeof(blockAlign));
  out.write(reinterpret_cast<const char *>(&bitsPerSample),
            sizeof(bitsPerSample));
  out.write("data", 4);
  out.write(reinterpret_cast<const char *>(&dataSize), sizeof(dataSize));
  if (!samples.empty()) {
    out.write(reinterpret_cast<const char *>(samples.data()), dataSize);
  }
  return out.good();
}

static std::string findModelPath(const std::string &models_dir,
                                 const std::string &engine_name) {
  if (!fs::exists(models_dir))
    return "";
  std::string engine = engine_name;
  std::transform(engine.begin(), engine.end(), engine.begin(), ::tolower);
  for (auto &entry : fs::directory_iterator(models_dir)) {
    if (!entry.is_directory())
      continue;
    std::string d = entry.path().filename().string();
    std::string dl = d;
    std::transform(dl.begin(), dl.end(), dl.begin(), ::tolower);
    std::string tokenized = dl;
    for (char &ch : tokenized) {
      if (!std::isalnum(static_cast<unsigned char>(ch)))
        ch = ' ';
    }
    std::unordered_set<std::string> tokens;
    std::istringstream tokenStream(tokenized);
    for (std::string token; tokenStream >> token;)
      tokens.insert(token);
    const bool isSmall = tokens.count("small") > 0;

    if (engine.find("small en") != std::string::npos &&
        isSmall && tokens.count("en") > 0)
      return entry.path().string();
    if (engine.find("large en") != std::string::npos &&
        tokens.count("en") > 0 && tokens.count("us") > 0 && !isSmall)
      return entry.path().string();
    if (engine.find("indian") != std::string::npos &&
        tokens.count("en") > 0 && tokens.count("in") > 0)
      return entry.path().string();
    if (engine.find("small cn") != std::string::npos &&
        tokens.count("cn") > 0 && isSmall)
      return entry.path().string();
    if (engine.find("large cn") != std::string::npos &&
        tokens.count("cn") > 0 && !isSmall)
      return entry.path().string();
    if (engine.find("small ru") != std::string::npos &&
        tokens.count("ru") > 0 && isSmall)
      return entry.path().string();
    if (engine.find("small fr") != std::string::npos &&
        tokens.count("fr") > 0 && isSmall)
      return entry.path().string();
    if (engine.find("large fr") != std::string::npos &&
        tokens.count("fr") > 0 && !isSmall)
      return entry.path().string();
    if (engine.find("small de") != std::string::npos &&
        tokens.count("de") > 0 && isSmall)
      return entry.path().string();
    if (engine.find("large de") != std::string::npos &&
        tokens.count("de") > 0 && !isSmall)
      return entry.path().string();
    if (engine.find("small es") != std::string::npos &&
        tokens.count("es") > 0 && isSmall)
      return entry.path().string();
    if (engine.find("small pt") != std::string::npos &&
        tokens.count("pt") > 0 && isSmall)
      return entry.path().string();
    if (engine.find("small it") != std::string::npos &&
        tokens.count("it") > 0 && isSmall)
      return entry.path().string();
    if (engine.find("small ja") != std::string::npos &&
        tokens.count("ja") > 0 && isSmall)
      return entry.path().string();
  }
  return "";
}

static std::string findOWWModelsDir() {
  std::string root = getExeDir();
  std::vector<std::string> candidates = {
      root + "\\openwakeword\\resources\\models",
      root + "\\_internal\\openwakeword\\resources\\models",
      root + "\\oww_models",
      root + "\\data\\oww_models",
  };
  for (auto &d : candidates) {
    if (fs::exists(d + "\\melspectrogram.onnx"))
      return d;
  }
  return "";
}

static std::string findPVModelsDir() {
  std::string root = getExeDir();
  std::vector<std::string> candidates = {
      root + "\\porcupine_native",
      root + "\\_internal\\porcupine_native",
      root + "\\data\\porcupine_native",
  };
  for (auto &d : candidates) {
    if (fs::exists(d + "\\porcupine_params.pv"))
      return d;
  }
  return "";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Audio Capture — PortAudio API
// ═══════════════════════════════════════════════════════════════════════════════

static const int SAMPLE_RATE = 16000;
static const int CHANNELS = 1;
static const int BITS_PER_SAMPLE = 16;
static const int BUFFER_SAMPLES = 320; // 20ms per buffer
static const int NUM_BUFFERS = 8;      // Ring buffer count

struct AudioCapture {
  PaStream* paStream = nullptr;
  int nativeSampleRate = 16000;
  std::mutex mtx;
  std::deque<std::vector<int16_t>> queue;
  std::condition_variable cv;
  std::atomic<bool> running{false};

  static int paCallback(const void *input, void *output,
                        unsigned long frameCount,
                        const PaStreamCallbackTimeInfo *timeInfo,
                        PaStreamCallbackFlags statusFlags, void *userData) {
    auto *self = static_cast<AudioCapture *>(userData);
    if (input && frameCount > 0 && self->running) {
      const int16_t *buf = static_cast<const int16_t *>(input);
      std::vector<int16_t> samples;
      
      // Hardware Resampling (Decimation)
      if (self->nativeSampleRate != 16000 && self->nativeSampleRate > 0) {
        double ratio = (double)self->nativeSampleRate / 16000.0;
        int dstSamples = (int)(frameCount / ratio);
        samples.resize(dstSamples);
        for (int i = 0; i < dstSamples; ++i) {
          int srcIdx = (int)(i * ratio);
          if (srcIdx >= frameCount)
            srcIdx = frameCount - 1;
          samples[i] = buf[srcIdx];
        }
      } else {
        samples.assign(buf, buf + frameCount);
      }
      {
        std::lock_guard<std::mutex> lock(self->mtx);
        self->queue.push_back(std::move(samples));
        while (self->queue.size() > 50)
          self->queue.pop_front();
      }
      self->cv.notify_one();
    }
    return paContinue;
  }

  bool start() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
      svc_log("PortAudio Init Error: %s", Pa_GetErrorText(err));
      // Even if init errors slightly (common on bad audio drivers), continue and try
    }
    
    PaStreamParameters inputParams;
    memset(&inputParams, 0, sizeof(inputParams));
    inputParams.device = Pa_GetDefaultInputDevice();
    if (inputParams.device == paNoDevice) {
      svc_log("PortAudio: No default input device.");
      return false;
    }
    inputParams.channelCount = CHANNELS;
    inputParams.sampleFormat = paInt16;
    inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    double streamSampleRate = 16000.0;
    PaError fmtErr = Pa_IsFormatSupported(&inputParams, nullptr, 16000.0);
    if (fmtErr != paFormatIsSupported) {
      streamSampleRate = Pa_GetDeviceInfo(inputParams.device)->defaultSampleRate;
      nativeSampleRate = (int)streamSampleRate;
      svc_log("16kHz not supported natively. Falling back to %d Hz with decimation bypass.", nativeSampleRate);
    } else {
      nativeSampleRate = 16000;
    }

    err = Pa_OpenStream(&paStream, &inputParams, nullptr, streamSampleRate, paFramesPerBufferUnspecified, paClipOff, paCallback, this);
    if (err != paNoError) {
      svc_log("Pa_OpenStream failed: %s", Pa_GetErrorText(err));
      return false;
    }
    
    running = true;
    err = Pa_StartStream(paStream);
    if (err != paNoError) {
      svc_log("Pa_StartStream failed: %s", Pa_GetErrorText(err));
      return false;
    }
    svc_log("Audio capture started (PortAudio, %d Hz, %d-sample buffers)", SAMPLE_RATE, BUFFER_SAMPLES);
    return true;
  }

  void stop() {
    running = false;
    if (paStream) {
      Pa_StopStream(paStream);
      Pa_CloseStream(paStream);
      paStream = nullptr;
    }
    Pa_Terminate();
  }

  // Block until audio is available, returns chunk
  bool getChunk(std::vector<int16_t> &out, int timeout_ms = 100) {
    std::unique_lock<std::mutex> lock(mtx);
    if (queue.empty()) {
      cv.wait_for(lock, std::chrono::milliseconds(timeout_ms));
    }
    if (queue.empty())
      return false;
    out = std::move(queue.front());
    queue.pop_front();
    return true;
  }
};

// ═══════════════════════════════════════════════════════════════════════════════
// STT Engine — The core engine class
// ═══════════════════════════════════════════════════════════════════════════════

enum class EngineMode { IDLE, ACTIVE, SLEEP };

class STTEngine {
public:
  std::atomic<EngineMode> mode{EngineMode::IDLE};
  std::atomic<bool> running{false};

  // Config
  std::string activeEngine = "Vosk Small En";
  Settings settings;
  std::string modelsDir;
  std::string exeDir;

  // Vosk
  VoskAPI vosk;
  VoskModel *voskModel = nullptr;
  VoskRecognizer *voskRec = nullptr;
  std::string activeModelPath;
  mutable std::recursive_mutex modelMutex;

  // OWW (TFLite)
  TfLiteLoader tflLoader;
  TFLiteWakeWordDetector owwDetector;
  bool owwReady = false;

  // PV
  NativePicovoiceDetector pvDetector;
  bool pvReady = false;
  std::vector<int16_t> pvChunkBuffer;

  // Audio
  AudioCapture audio;

  // Timing
  std::chrono::steady_clock::time_point lastSpeechTime;
  std::chrono::steady_clock::time_point lastActivationTime;
  double wakeSuppressedUntil = 0.0;
  int silenceLimitSec = 15;
  std::atomic<bool> cloudTranscription{false};
  std::atomic<bool> cloudAwaitingFrontend{false};
  std::vector<int16_t> cloudPreRollBuffer;
  std::vector<int16_t> cloudUtteranceBuffer;
  std::chrono::steady_clock::time_point cloudLastSpeechTime;
  std::chrono::steady_clock::time_point cloudCaptureStartedAt{};
  bool cloudSpeechStarted = false;

  // Wakeword hit counting
  std::unordered_map<std::string, int> owwHitCounts;
  int voskWakeHits = 0;
  int wakeHitRequirement = 1;
  bool voskFallbackRequired = true;

  bool canOffloadVosk() const {
    if (voskFallbackRequired) return false;
    if (!owwReady && !pvReady) return false;
    return true;
  }

  // OWW chunk accumulator
  std::vector<int16_t> owwChunkBuffer;

  STTEngine() {
    exeDir = getExeDir();
    modelsDir = getAppDataDir() + "\\models";
    settings = loadSettings();
    lastSpeechTime = std::chrono::steady_clock::now();
    lastActivationTime = lastSpeechTime;
  }

  ~STTEngine() {
    stop();
    offloadVoskModel();
    owwDetector.cleanup();
    pvDetector.cleanup();
    tflLoader.unload();
    vosk.unload();
  }

  bool init() {
    // Load libvosk.dll
    std::string voskPath = exeDir + "\\vosk\\libvosk.dll";
    if (!fs::exists(voskPath))
      voskPath = exeDir + "\\libvosk.dll";
    if (!vosk.load(voskPath)) {
      svc_log("FATAL: Failed to load libvosk.dll from %s", voskPath.c_str());
      return false;
    }
    vosk.set_log_level(-1); // Suppress Vosk internal logging
    svc_log("libvosk.dll loaded OK");

    // Load tensorflowlite_c.dll for OWW
    if (tflLoader.load(exeDir)) {
      svc_log("tensorflowlite_c.dll loaded OK");
    } else {
      svc_log("tensorflowlite_c.dll not found — OWW wakeword detection disabled");
    }
    initWakeEngines();
    return true;
  }

  void initWakeEngines() {
    voskFallbackRequired = true; // Default to true before engines specify
    initOWW();
    initPV();
  }

  void initOWW() {
    std::string we = settings.wakeEngine;
    // Skip TFLite OWW pipeline only for Porcupine (has own engine) and Vosk (built-in)
    if (we.find("Porcupine") != std::string::npos || we.find("Vosk") != std::string::npos) {
      svc_log("Wake engine '%s' bypasses OWW pipeline", we.c_str());
      owwReady = false;
      return;
    }
    // Only OpenWakeWord uses TFLite OWW pipeline. Porcupine and Vosk have their own engines.
    svc_log("Wake engine '%s' mapped to TFLite OWW pipeline", we.c_str());
    std::string owwDir = findOWWModelsDir();
    if (owwDir.empty()) {
      svc_log("OWW models directory not found");
      owwReady = false;
      voskFallbackRequired = true;
      return;
    }
    owwReady = owwDetector.init(tflLoader, owwDir, settings.wakeWords, 0.40f);
    if (owwReady && owwDetector.wake_models_.size() < settings.wakeWords.size()) {
       voskFallbackRequired = true;
       svc_log("Not all wake words loaded natively. Enabling Vosk fallback for remaining.");
    } else if (owwReady) {
       voskFallbackRequired = false;
    }
  }

  void initPV() {
    if (settings.wakeEngine.find("Porcupine") == std::string::npos) {
      pvReady = false;
      return;
    }
    std::string pvDir = findPVModelsDir();
    if (pvDir.empty()) {
      svc_log("Picovoice models directory not found");
      pvReady = false;
      return;
    }
    std::string dllPath = pvDir + "\\libpv_porcupine.dll";
    if (!pvDetector.load_dll(dllPath)) {
      svc_log("Failed to load libpv_porcupine.dll");
      pvReady = false;
      return;
    }

    std::string modelPath = pvDir + "\\porcupine_params.pv";
    std::vector<std::string> paths;
    for (const auto& w : settings.wakeWords) {
        std::string kp = w;
        for (char& c : kp) {
            if (c == ' ') c = '_';
        }
        std::string file = pvDir + "\\" + kp + "_windows.ppn";
        if (fs::exists(file)) paths.push_back(file);
    }
    
    if (paths.empty()) {
        svc_log("No valid .ppn keyword models found for Porcupine");
        pvReady = false;
        return;
    }

    if (settings.porcupineAccessKey.empty()) {
        svc_log("Porcupine requires an Access Key. Go to Dashboard and enter one.");
        pvReady = false;
        return;
    }

    pvReady = pvDetector.init(settings.porcupineAccessKey, modelPath, paths, 0.45f);
    if (pvReady) {
        svc_log("Picovoice Porcupine ready with %d words", (int)paths.size());
        if (paths.size() < settings.wakeWords.size()) {
            voskFallbackRequired = true;
            svc_log("Not all wake words loaded natively in PV. Enabling Vosk fallback.");
        } else {
            voskFallbackRequired = false;
        }
    } else {
        svc_log("Picovoice Init Failed (Check Access Key or Net connection)");
        sendEvent("ERROR", "Picovoice Refused: Invalid Access Key");
    }
  }

  bool loadVoskModel() {
    std::lock_guard<std::recursive_mutex> lock(modelMutex);
    std::string path = findModelPath(modelsDir, activeEngine);
    if (path.empty())
      return false;
    activeModelPath = path;

    svc_log("Loading Vosk model: %s", path.c_str());
    auto t0 = std::chrono::steady_clock::now();
    voskModel = vosk.model_new(path.c_str());
    if (!voskModel) {
      svc_log("Failed to load Vosk model");
      return false;
    }
    voskRec = vosk.recognizer_new(voskModel, (float)SAMPLE_RATE);
    if (!voskRec) {
      svc_log("Failed to create recognizer");
      vosk.model_free(voskModel);
      voskModel = nullptr;
      return false;
    }
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    svc_log("Vosk model loaded in %lldms", dt);
    return true;
  }

  void offloadVoskModel() {
    std::lock_guard<std::recursive_mutex> lock(modelMutex);
    if (voskRec) {
      vosk.recognizer_free(voskRec);
      voskRec = nullptr;
    }
    if (voskModel) {
      vosk.model_free(voskModel);
      voskModel = nullptr;
    }
    // Force aggressive Windows memory compaction to drop
    // the working set down below 50MB when dormant
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    svc_log("Vosk model offloaded & memory compacted");
  }

  void reloadVoskModel() {
    std::lock_guard<std::recursive_mutex> lock(modelMutex);
    if (voskModel)
      return; // Already loaded
    sendEvent("STATE", "3,Loading model...");
    if (loadVoskModel()) {
      svc_log("Vosk model reloaded OK");
    } else {
      svc_log("Failed to reload Vosk model");
    }
  }

  void start() {
    if (running)
      return;
    running = true;
    std::thread(&STTEngine::engineLoop, this).detach();
    std::thread(&STTEngine::watchdog, this).detach();
  }

  void stop() {
    running = false;
    audio.stop();
  }

  void toggleListening() {
    if (mode == EngineMode::ACTIVE) {
      mode = EngineMode::IDLE;
      finishCloudTurn();
      suppressWakeword(1.8);
      if (canOffloadVosk()) {
        offloadVoskModel();
      }
      sendEvent("STATE", "0,Model Ready");
    } else {
      activateSTT();
    }
  }

  void forcePause() {
    mode = EngineMode::SLEEP; // Standard pause = go to wakeword but hidden
    finishCloudTurn();
    sendEvent("STATE", "0,Paused");
  }

  void forceSleep() {
    mode = EngineMode::SLEEP;
    suppressWakeword(3.2);
    finishCloudTurn();
    sendEvent("STATE", "-1,Hidden");
  }

  void activateSTT() {
    lastActivationTime = std::chrono::steady_clock::now();
    owwHitCounts.clear();
    voskWakeHits = 0;
    lastSpeechTime = std::chrono::steady_clock::now();
    cloudAwaitingFrontend = false;
    resetCloudCaptureState();
    reloadVoskModel();
    {
      std::lock_guard<std::recursive_mutex> lock(modelMutex);
      if (!voskRec) {
        mode = EngineMode::IDLE;
        sendEvent("STATE", "3,Model Missing");
        return;
      }
    }
    mode = EngineMode::ACTIVE;
    sendEvent("STATE", "1,Listening..."); // Code 1 for Active/Listening
  }

  void setWakeWords(const std::vector<std::string> &words) {
    settings.wakeWords = words;
    owwDetector.cleanup();
    pvDetector.cleanup_porcupine();
    initWakeEngines();
    sendEvent("STATE", "0,Wakewords updated");
  }

  void setCloseWords(const std::vector<std::string> &words) {
    settings.closeWords = words;
    sendEvent("STATE", "0,Close words updated");
  }

  void setWakeEngine(const std::string &engine) {
    settings.wakeEngine = canonicalWakeEngine(engine);
    owwDetector.cleanup();
    pvDetector.cleanup_porcupine();
    initWakeEngines();
    sendEvent("STATE", "0,Wake engine updated");
  }

  void setTranscriptionMode(const std::string &modeValue) {
    const std::string normalized = lowercaseCopy(modeValue);
    const bool cloudMode = normalized.find("cloud") != std::string::npos;
    cloudTranscription = cloudMode;
    cloudAwaitingFrontend = false;
    cloudSpeechStarted = false;
    cloudPreRollBuffer.clear();
    cloudUtteranceBuffer.clear();
    svc_log("Transcription mode set to %s", cloudMode ? "cloud" : "local");
    if (mode == EngineMode::ACTIVE) {
      sendEvent("STATE", cloudMode ? "1,Listening..." : "1,Listening...");
    }
  }

  void finishCloudTurn() {
    cloudAwaitingFrontend = false;
    cloudSpeechStarted = false;
    cloudPreRollBuffer.clear();
    cloudUtteranceBuffer.clear();
    cloudCaptureStartedAt = std::chrono::steady_clock::now();
    lastSpeechTime = cloudCaptureStartedAt;
    if (mode == EngineMode::ACTIVE)
      sendEvent("STATE", "1,Listening...");
  }

private:
  void suppressWakeword(double seconds) {
    std::lock_guard<std::recursive_mutex> lock(modelMutex);
    wakeSuppressedUntil = getTimeSeconds() + seconds;
    cloudAwaitingFrontend = false;
    resetCloudCaptureState();
    owwHitCounts.clear();
    pvChunkBuffer.clear();
    voskWakeHits = 0;
    if (owwReady)
      owwDetector.reset();
    if (voskRec)
      vosk.recognizer_reset(voskRec);
  }

  double getTimeSeconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  double secondsSince(std::chrono::steady_clock::time_point tp) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - tp)
        .count();
  }

  bool containsCloseWord(const std::string &text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto &cw : settings.closeWords) {
      std::string lowerCw = cw;
      std::transform(lowerCw.begin(), lowerCw.end(), lowerCw.begin(),
                     ::tolower);
      if (lower.find(lowerCw) != std::string::npos)
        return true;
    }
    return false;
  }

  int computeAudioLevel(const int16_t *samples, size_t n) {
    if (n == 0)
      return 0;
    double sum = 0;
    for (size_t i = 0; i < n; i++)
      sum += (double)samples[i] * samples[i];
    double rms = std::sqrt(sum / n);
    if (rms <= 0)
      return 0;
    double db = 20.0 * std::log10(rms);
    int level = (int)((db - 35.0) / 50.0 * 100.0);
    return std::max(0, std::min(100, level));
  }

  void resetCloudCaptureState() {
    cloudSpeechStarted = false;
    cloudPreRollBuffer.clear();
    cloudUtteranceBuffer.clear();
    cloudCaptureStartedAt = std::chrono::steady_clock::time_point{};
  }

  void emitCloudUtterance() {
    if (cloudUtteranceBuffer.size() < size_t(SAMPLE_RATE * 0.18)) {
      resetCloudCaptureState();
      return;
    }

    fs::path cacheDir = fs::path(getAppDataDir()) / "cloud_cache";
    std::error_code ec;
    fs::create_directories(cacheDir, ec);

    const auto stampNow = std::chrono::steady_clock::now().time_since_epoch();
    const auto stamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(stampNow).count();
    fs::path wavPath = cacheDir / ("cloud_" + std::to_string(stamp) + ".wav");
    if (!writeWavFile(wavPath, cloudUtteranceBuffer)) {
      svc_log("Failed to write cloud utterance WAV");
      resetCloudCaptureState();
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    const double captureMs =
        cloudCaptureStartedAt == std::chrono::steady_clock::time_point{}
            ? 0.0
            : std::chrono::duration<double, std::milli>(now - cloudCaptureStartedAt)
                  .count();
    const double audioMs =
        (1000.0 * double(cloudUtteranceBuffer.size())) / double(SAMPLE_RATE);
    svc_log("Cloud utterance ready: %s (capture=%.0fms audio=%.0fms)",
            wavPath.string().c_str(), captureMs, audioMs);
    lastSpeechTime = now;
    cloudAwaitingFrontend = true;
    sendEvent("STATE", "2,Cloud transcribing...");
    sendEvent("CLOUD_AUDIO", wavPath.string());
    resetCloudCaptureState();
  }

  void processCloudChunk(const std::vector<int16_t> &chunk, int level) {
    constexpr double kCloudPreRollSec = 0.14;
    constexpr double kCloudMinUtteranceSec = 0.22;
    constexpr double kCloudFastSilenceSec = 0.26;
    constexpr double kCloudSlowSilenceSec = 0.36;
    constexpr double kCloudSlowSilenceAfterSec = 1.9;
    constexpr double kCloudMaxUtteranceSec = 9.0;
    const size_t maxPreRollSamples = size_t(SAMPLE_RATE * kCloudPreRollSec);
    cloudPreRollBuffer.insert(cloudPreRollBuffer.end(), chunk.begin(), chunk.end());
    if (cloudPreRollBuffer.size() > maxPreRollSamples) {
      cloudPreRollBuffer.erase(
          cloudPreRollBuffer.begin(),
          cloudPreRollBuffer.begin() +
              (cloudPreRollBuffer.size() - maxPreRollSamples));
    }

    if (cloudAwaitingFrontend)
      return;

    const bool speechNow = level >= 10;
    const bool holdSpeech = level >= 6;
    const auto now = std::chrono::steady_clock::now();

    if (speechNow) {
      lastSpeechTime = now;
      cloudLastSpeechTime = now;
      if (!cloudSpeechStarted) {
        cloudSpeechStarted = true;
        cloudCaptureStartedAt = now;
        cloudUtteranceBuffer = cloudPreRollBuffer;
      }
    }

    if (!cloudSpeechStarted)
      return;

    cloudUtteranceBuffer.insert(cloudUtteranceBuffer.end(), chunk.begin(),
                                chunk.end());
    const double utteranceSeconds =
        double(cloudUtteranceBuffer.size()) / double(SAMPLE_RATE);
    const double silenceThresholdSec =
        utteranceSeconds >= kCloudSlowSilenceAfterSec ? kCloudSlowSilenceSec
                                                      : kCloudFastSilenceSec;
    const bool utteranceLongEnough =
        cloudUtteranceBuffer.size() >= size_t(SAMPLE_RATE * kCloudMinUtteranceSec);
    const bool silenceReached =
        utteranceLongEnough && !holdSpeech &&
        (std::chrono::duration<double>(now - cloudLastSpeechTime).count() >=
         silenceThresholdSec);
    const bool hardSilenceReached =
        utteranceLongEnough && level <= 2 &&
        (std::chrono::duration<double>(now - cloudLastSpeechTime).count() >=
         0.18);
    const bool maxReached = cloudUtteranceBuffer.size() >=
                            size_t(SAMPLE_RATE * kCloudMaxUtteranceSec);

    if (maxReached || silenceReached || hardSilenceReached)
      emitCloudUtterance();
  }

  void engineLoop() {
    svc_log("Engine loop started, mode=%d", (int)mode.load());

    // Load model initially (then offload if dormant)
    sendEvent("STATE", "3,Loading model...");
    if (!loadVoskModel()) {
      sendEvent("STATE", "3,Model Missing");
      svc_log("No model found — waiting for download");
      // Keep running so watchdog can retry
      while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (loadVoskModel())
          break;
      }
      if (!running)
        return;
    }

    // Offload if starting dormant
    if (mode != EngineMode::ACTIVE && canOffloadVosk()) {
      offloadVoskModel();
    }

    // Start audio capture
    for (int attempt = 0; attempt < 10; attempt++) {
      if (audio.start())
        break;
      svc_log("Audio open failed (attempt %d/10)", attempt + 1);
      sendEvent("STATE",
                "3,Mic retry " + std::to_string(attempt + 1) + "/10...");
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    if (!audio.paStream) {
      svc_log("Failed to open microphone");
      sendEvent("STATE", "3,Mic Error");
      running = false;
      return;
    }

    if (mode == EngineMode::SLEEP)
      sendEvent("STATE", "-1,Hidden");
    else if (mode == EngineMode::ACTIVE)
      sendEvent("STATE", "1,Listening...");
    else
      sendEvent("STATE", "0,Model Ready");

    svc_log("Main loop running, oww=%s", owwReady ? "loaded" : "none");

    // Main audio processing loop
    while (running) {
      std::vector<int16_t> chunk;
      if (!audio.getChunk(chunk, 100))
        continue;
      if (chunk.empty())
        continue;

      // Audio level
      int level = computeAudioLevel(chunk.data(), chunk.size());
      sendEvent("AUDIO_LEVEL", std::to_string(level));

      // ── Wakeword mode (IDLE / SLEEP) ──
      if (mode != EngineMode::ACTIVE) {
        if (getTimeSeconds() < wakeSuppressedUntil)
          continue;

        bool needPV = pvReady;
        bool needOWW = owwReady;
        bool needVosk = false;
        {
          std::lock_guard<std::recursive_mutex> lock(modelMutex);
          needVosk =
              voskRec && (!pvReady && !owwReady || voskFallbackRequired);
        }

        if (needPV) {
          if (pvChunkBuffer.size() > 32000) pvChunkBuffer.clear();
          pvChunkBuffer.insert(pvChunkBuffer.end(), chunk.begin(), chunk.end());
          while ((int)pvChunkBuffer.size() >= pvDetector.frame_length) {
            std::vector<int16_t> processData(pvChunkBuffer.begin(), pvChunkBuffer.begin() + pvDetector.frame_length);
            pvChunkBuffer.erase(pvChunkBuffer.begin(), pvChunkBuffer.begin() + pvDetector.frame_length);
            
            int32_t keyword_index = pvDetector.predict(processData.data());
            if (keyword_index >= 0 && secondsSince(lastActivationTime) > 1.0) {
              svc_log("Picovoice Detected index: %d", (int)keyword_index);
              activateSTT();
              pvChunkBuffer.clear();
              break;
            }
          }
        } 
        
        if (needOWW || needVosk) {
          owwChunkBuffer.insert(owwChunkBuffer.end(), chunk.begin(), chunk.end());
          while (owwChunkBuffer.size() >= 1280) {
            std::vector<int16_t> processData(owwChunkBuffer.begin(), owwChunkBuffer.begin() + 1280);
            owwChunkBuffer.erase(owwChunkBuffer.begin(), owwChunkBuffer.begin() + 1280);

            if (needOWW && mode != EngineMode::ACTIVE) {
              auto scores = owwDetector.predict(processData.data(), processData.size());
              for (auto &[mdl, score] : scores) {
                if (score >= owwDetector.threshold) {
                  owwHitCounts[mdl]++;
                } else {
                  owwHitCounts[mdl] = 0;
                }
                if (owwHitCounts[mdl] >= wakeHitRequirement && secondsSince(lastActivationTime) > 2.0) {
                  svc_log("OWW Detected: %s score=%.2f", mdl.c_str(), score);
                  activateSTT();
                  owwChunkBuffer.clear();
                  break;
                }
              }
            } 
            
            if (needVosk && mode != EngineMode::ACTIVE) {
              int isFinal = 0;
              const char *res_str = nullptr;
              {
                std::lock_guard<std::recursive_mutex> lock(modelMutex);
                if (!voskRec)
                  continue;
                isFinal = vosk.recognizer_accept_waveform(
                    voskRec, (const char *)processData.data(),
                    (int)(processData.size() * 2));
                res_str = isFinal ? vosk.recognizer_result(voskRec)
                                  : vosk.recognizer_partial_result(voskRec);
              }
              std::string partialText = jsonGetString(res_str, isFinal ? "text" : "partial");
              
              std::string lowerPartial = partialText;
              std::transform(lowerPartial.begin(), lowerPartial.end(), lowerPartial.begin(), ::tolower);
              
              bool matched = false;
              for (const auto &w : settings.wakeWords) {
                std::string lw = w;
                std::transform(lw.begin(), lw.end(), lw.begin(), ::tolower);
                if (lowerPartial.find(lw) != std::string::npos) {
                  matched = true;
                  break;
                }
              }
              if (matched) {
                if (secondsSince(lastActivationTime) > 2.0) {
                  svc_log("Vosk fallback wakeword match: '%s'", partialText.c_str());
                  activateSTT();
                  std::lock_guard<std::recursive_mutex> lock(modelMutex);
                  if (voskRec)
                    vosk.recognizer_reset(voskRec);
                  owwChunkBuffer.clear();
                  pvChunkBuffer.clear();
                }
              }

              if (mode != EngineMode::ACTIVE) {
                // Kaldi memory leak prevention: forcibly reset recognizer lattice
                // every ~5 seconds if we haven't hit a natural silence boundary,
                // otherwise it expands infinitely absorbing background noise.
                static auto lastVoskReset = std::chrono::steady_clock::now();
                if (isFinal || secondsSince(lastVoskReset) > 6.0) {
                  std::lock_guard<std::recursive_mutex> lock(modelMutex);
                  if (voskRec)
                    vosk.recognizer_reset(voskRec);
                  lastVoskReset = std::chrono::steady_clock::now();
                }
              }
            }
          }
        }
      }

      // ── Dictation mode (ACTIVE) ──
      else {
        if (cloudTranscription) {
          processCloudChunk(chunk, level);
          continue;
        }

        // Ensure model is loaded
        bool needsReload = false;
        {
          std::lock_guard<std::recursive_mutex> lock(modelMutex);
          needsReload = (voskRec == nullptr);
        }
        if (needsReload) {
          reloadVoskModel();
          std::lock_guard<std::recursive_mutex> lock(modelMutex);
          if (!voskRec) {
            svc_log("Cannot dictate - model not loaded");
            mode = EngineMode::IDLE;
            continue;
          }
        }

        if (level > 2)
          lastSpeechTime = std::chrono::steady_clock::now();

        int accepted = 0;
        {
          std::lock_guard<std::recursive_mutex> lock(modelMutex);
          if (!voskRec) {
            mode = EngineMode::IDLE;
            continue;
          }
          accepted = vosk.recognizer_accept_waveform(
              voskRec, (const char *)chunk.data(), (int)(chunk.size() * 2));
        }

        if (accepted) {
          const char *result = nullptr;
          {
            std::lock_guard<std::recursive_mutex> lock(modelMutex);
            if (!voskRec) {
              mode = EngineMode::IDLE;
              continue;
            }
            result = vosk.recognizer_result(voskRec);
          }
          std::string text = jsonGetString(result, "text");

          if (!text.empty()) {
            // Check close words
            if (containsCloseWord(text)) {
              svc_log("Close word in final: '%s'", text.c_str());
              forceSleep();
              continue;
            }

            lastSpeechTime = std::chrono::steady_clock::now();
            svc_log("VOSK FINAL: '%s'", text.c_str());
            sendEvent("FINAL_TEXT", text);

            // Typing & command execution is handled by the Qt frontend
            // via the FINAL_TEXT event. Don't duplicate it here.
            sendEvent("STATE", "2,Recognized: " + text);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            sendEvent("STATE", "1,Listening...");
          } else {
            svc_log("VOSK FINAL (EMPTY)");
          }
        } else {
          // Check partials for close words
          const char *partial = nullptr;
          {
            std::lock_guard<std::recursive_mutex> lock(modelMutex);
            if (!voskRec) {
              mode = EngineMode::IDLE;
              continue;
            }
            partial = vosk.recognizer_partial_result(voskRec);
          }
          std::string partialText = jsonGetString(partial, "partial");
          if (!partialText.empty()) {
            if (containsCloseWord(partialText)) {
              svc_log("Close word in partial: '%s' -> Sleeping",
                      partialText.c_str());
              std::lock_guard<std::recursive_mutex> lock(modelMutex);
              if (voskRec)
                vosk.recognizer_reset(voskRec);
              forceSleep();
              continue;
            }
            sendEvent("STATE", "2,Listening: " + partialText);
          }
        }
      }
    }

    audio.stop();
    svc_log("Engine loop stopped");
  }

  void watchdog() {
    svc_log("Watchdog started");
    while (running) {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      const bool cloudTurnInFlight = cloudTranscription && cloudAwaitingFrontend;

      // Auto-idle after silence (return to wakeword listening)
      if (!cloudTurnInFlight && mode == EngineMode::ACTIVE &&
          secondsSince(lastSpeechTime) > silenceLimitSec) {
        mode = EngineMode::IDLE;
        suppressWakeword(1.4);
        sendEvent("STATE", "0,Model Ready");
      }

      if (!cloudTurnInFlight && mode != EngineMode::ACTIVE) {
        bool modelLoaded = false;
        {
          std::lock_guard<std::recursive_mutex> lock(modelMutex);
          modelLoaded = (voskModel != nullptr);
        }
        if (!canOffloadVosk() && !modelLoaded) {
          svc_log("Vosk is required for wakeword but offloaded. Reloading...");
          reloadVoskModel();
        } else if (settings.autoOffloadEnabled && canOffloadVosk()) {
          if (modelLoaded &&
              secondsSince(lastSpeechTime) > settings.autoOffloadDelaySec) {
            svc_log("Dormant timeout reached (%d sec) -> Offloading to save RAM",
                    settings.autoOffloadDelaySec);
            offloadVoskModel();
          }
        }
      }
    }
  }
};

// ═══════════════════════════════════════════════════════════════════════════════
// Command reader (stdin) — same pipe protocol as Python stt_service.py
// ═══════════════════════════════════════════════════════════════════════════════

static void inputLoop(STTEngine &engine) {
  char line[4096];
  while (fgets(line, sizeof(line), stdin)) {
    std::string cmd(line);
    // Trim
    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r'))
      cmd.pop_back();
    if (cmd.empty())
      continue;

    // Split on first ':'
    std::string action, payload;
    size_t colon = cmd.find(':');
    if (colon != std::string::npos) {
      action = cmd.substr(0, colon);
      payload = cmd.substr(colon + 1);
    } else {
      action = cmd;
    }
    // Uppercase
    std::transform(action.begin(), action.end(), action.begin(), ::toupper);

    if (action == "TOGGLE")
      engine.toggleListening();
    else if (action == "STOP")
      engine.forcePause();
    else if (action == "SLEEP")
      engine.forceSleep();
    else if (action == "WAKEWORDS")
      engine.setWakeWords(splitCSV(payload));
    else if (action == "CLOSEWORDS")
      engine.setCloseWords(splitCSV(payload));
    else if (action == "OFFLOAD") {
      if (payload == "1" || payload == "0" || payload == "true" || payload == "false") {
        engine.settings.autoOffloadEnabled = (payload == "1" || payload == "true");
      } else {
        if (engine.canOffloadVosk()) {
          engine.offloadVoskModel();
        }
      }
    }
    else if (action == "OFFLOADDELAY")
      engine.settings.autoOffloadDelaySec = std::stoi(payload);
    else if (action == "WAKEMODE")
      engine.setWakeEngine(payload);
    else if (action == "PV_KEY") {
      engine.settings.porcupineAccessKey = payload;
      engine.setWakeEngine(engine.settings.wakeEngine);
    }
    else if (action == "SETLOAD")
      engine.settings.autoModelLoad = (payload == "1");
    else if (action == "TRANSCRIBE_MODE")
      engine.setTranscriptionMode(payload);
    else if (action == "CLOUD_DONE")
      engine.finishCloudTurn();
    else if (action == "MODEL") {
      if (payload.find("Whisper") != std::string::npos) {
        sendEvent("ERROR",
                  "Whisper is not available in the native build yet");
        sendEvent("STATE", "3,Whisper not ready");
      } else {
        {
          std::lock_guard<std::recursive_mutex> lock(engine.modelMutex);
          engine.activeEngine = payload;
        }
        engine.offloadVoskModel(); // Release old model!
        svc_log("Engine switched to: %s", payload.c_str());
        if (engine.settings.autoModelLoad) {
          svc_log("Immediate load enabled, reloading...");
          engine.reloadVoskModel();
          std::lock_guard<std::recursive_mutex> lock(engine.modelMutex);
          if (engine.voskRec)
            sendEvent("STATE", "0,Switched to " + payload);
          else
            sendEvent("STATE", "3,Model Missing");
        } else {
          if (!findModelPath(engine.modelsDir, payload).empty())
            sendEvent("STATE", "0,Switched to " + payload);
          else
            sendEvent("STATE", "3,Model Missing");
        }
      }
    } else if (action == "DOWNLOAD") {
      svc_log("DOWNLOAD command for %s (Native downloader not implemented)",
              payload.c_str());
      // For now just simulate complete if already exists
      if (!findModelPath(engine.modelsDir, payload).empty()) {
        sendEvent("DL_COMPLETE", payload);
      } else {
        sendEvent("ERROR", "Native downloader missing - Use dashboard to "
                           "prepare models");
      }
    } else if (action == "RELOAD") {
      svc_log("RELOAD requested from frontend");
      engine.reloadVoskModel();
      std::lock_guard<std::recursive_mutex> lock(engine.modelMutex);
      if (engine.voskRec) {
        sendEvent("STATE", "0,Model Ready");
      } else {
        sendEvent("STATE", "3,Model Missing");
      }
    } else if (action == "QUIT") {
      engine.stop();
      exit(0);
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main entry point
// ═══════════════════════════════════════════════════════════════════════════════

int main() {
  // Ensure stdout is unbuffered for pipe communication
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);

  sendEvent("INIT", "Native Service Ready");

  STTEngine engine;
  if (!engine.init()) {
    sendEvent("ERROR", "Engine init failed");
    return 1;
  }

  sendEvent("STATE", "3,Starting engine...");
  engine.start();

  // Block main thread reading stdin commands
  inputLoop(engine);

  return 0;
}
