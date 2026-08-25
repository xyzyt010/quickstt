// wakenet_native.h — Native C++ WakeWordNet detector
// Implements 40-mel log-filterbank feature extraction + ONNX model inference
// for custom WakeWordNet models (agent, hem, jarvis, etc.)
//
// Pipeline: int16 audio → STFT → 40-mel filterbank → log → ONNX classifier → score
//
// Config: sample_rate=16000, n_mels=40, n_fft=400, hop_length=160
#pragma once

#include "ort_loader.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

// Map of wakeword names to their WakeWordNet .onnx model filenames
static const std::unordered_map<std::string, std::string> WAKENET_BUILTIN_MAP = {
    {"agent", "agent.onnx"},
    {"hem", "hem.onnx"},
    {"jarvis", "jarvis.onnx"},
    {"hey jarvis", "hey_jarvis.onnx"},
    {"alexa", "alexa.onnx"},
};

static inline void wn_log(const std::string &msg) {
  fprintf(stderr, "[WAKENET] %s\n", msg.c_str());
  fflush(stderr);
}

static inline bool wn_ort_ok(const OrtApi *api, OrtStatus *status,
                             const char *ctx = "") {
  if (!status)
    return true;
  const char *msg = api->GetErrorMessage(status);
  fprintf(stderr, "[WAKENET-ORT-ERROR] %s: %s\n", ctx, msg);
  api->ReleaseStatus(status);
  return false;
}

class WakeNetDetector {
public:
  float threshold = 0.5f;
  bool active = false;

  struct WakeModel {
    OrtSession *session = nullptr;
    std::string name;
  };
  std::vector<WakeModel> wake_models_;

  WakeNetDetector() = default;
  ~WakeNetDetector() { cleanup(); }

  bool init(OrtLoader &ort, const std::string &model_dir,
            const std::vector<std::string> &wake_words, float thresh = 0.5f) {
    threshold = thresh;
    api_ = ort.api;
    if (!api_)
      return false;

    OrtStatus *st = nullptr;
    st = api_->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "wakenet", &env_);
    if (!wn_ort_ok(api_, st, "CreateEnv"))
      return false;

    st = api_->CreateSessionOptions(&opts_);
    if (!wn_ort_ok(api_, st, "CreateSessionOptions"))
      return false;
    api_->SetIntraOpNumThreads(opts_, 1);
    api_->SetInterOpNumThreads(opts_, 1);
    api_->SetSessionGraphOptimizationLevel(opts_, ORT_ENABLE_ALL);

    // Build mel filterbank matrix
    buildMelFilterbank();

    // Load wake word models
    for (const auto &w : wake_words) {
      std::string key = w;
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      while (!key.empty() && key.front() == ' ')
        key.erase(key.begin());
      while (!key.empty() && key.back() == ' ')
        key.pop_back();

      auto it = WAKENET_BUILTIN_MAP.find(key);
      if (it == WAKENET_BUILTIN_MAP.end())
        continue;

      std::string fpath = (fs::path(model_dir) / it->second).string();
      if (!fs::exists(fpath))
        continue;

      WakeModel wm;
      if (!loadSession(fpath, &wm.session))
        continue;

      wm.name = key;
      wake_models_.push_back(std::move(wm));
      wn_log("Loaded WakeWordNet model: " + it->second + " for '" + key + "'");
    }

    if (wake_models_.empty())
      return false;

    // Pre-allocate buffers
    mel_frame_buf_.clear();
    remainder_.clear();

    active = true;
    wn_log("Ready with " + std::to_string(wake_models_.size()) +
           " WakeWordNet model(s)");
    return true;
  }

  // Process audio and return scores per wake word
  std::unordered_map<std::string, float> predict(const int16_t *audio,
                                                  size_t n) {
    std::unordered_map<std::string, float> out;
    if (!active)
      return out;

    // Accumulate audio and extract mel frames
    streamMelFrames(audio, n);

    // Need at least ~50 frames (~0.5s) for reliable detection
    if (mel_frame_buf_.size() < MIN_FRAMES * N_MELS)
      return out;

    // Run each wake model on the accumulated mel features
    for (auto &wm : wake_models_) {
      float score = runWakeModel(wm);
      auto &hist = pred_hist_[wm.name];
      if (hist.size() < 3)
        score = 0.0f; // Warmup guard
      hist.push_back(score);
      if (hist.size() > 20)
        hist.pop_front();
      out[wm.name] = score;
    }
    return out;
  }

  void reset() {
    mel_frame_buf_.clear();
    remainder_.clear();
    for (auto &[k, v] : pred_hist_)
      v.clear();
  }

  void cleanup() {
    active = false;
    for (auto &wm : wake_models_)
      if (wm.session)
        api_->ReleaseSession(wm.session);
    wake_models_.clear();
    if (opts_) {
      api_->ReleaseSessionOptions(opts_);
      opts_ = nullptr;
    }
    if (env_) {
      api_->ReleaseEnv(env_);
      env_ = nullptr;
    }
  }

private:
  const OrtApi *api_ = nullptr;
  OrtEnv *env_ = nullptr;
  OrtSessionOptions *opts_ = nullptr;

  // Audio params matching config.json
  static const int SAMPLE_RATE = 16000;
  static const int N_MELS = 40;
  static const int N_FFT = 400;
  static const int HOP_LENGTH = 160;
  static const size_t MIN_FRAMES = 50;   // ~0.5s minimum
  static const size_t MAX_FRAMES = 100;  // ~1.0s sliding window

  // Mel filterbank matrix [N_MELS x (N_FFT/2+1)]
  std::vector<std::vector<float>> mel_filterbank_;

  // Buffers
  std::vector<float> mel_frame_buf_; // flattened [T, N_MELS]
  std::vector<int16_t> remainder_;
  std::unordered_map<std::string, std::deque<float>> pred_hist_;

  // Hann window for STFT
  std::vector<float> hann_window_;

  bool loadSession(const std::string &path, OrtSession **sess) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wp(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wp[0], wlen);
    OrtStatus *st = api_->CreateSession(env_, wp.c_str(), opts_, sess);
#else
    OrtStatus *st = api_->CreateSession(env_, path.c_str(), opts_, sess);
#endif
    return wn_ort_ok(api_, st, path.c_str());
  }

  // ── Mel Filterbank Construction ─────────────────────────────────────────

  static float hzToMel(float hz) {
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
  }
  static float melToHz(float mel) {
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
  }

  void buildMelFilterbank() {
    const int num_fft_bins = N_FFT / 2 + 1; // 201
    const float fmin = 0.0f;
    const float fmax = (float)SAMPLE_RATE / 2.0f; // 8000 Hz

    float mel_min = hzToMel(fmin);
    float mel_max = hzToMel(fmax);

    // N_MELS + 2 equally spaced points in mel scale
    std::vector<float> mel_points(N_MELS + 2);
    for (int i = 0; i < N_MELS + 2; i++)
      mel_points[i] = melToHz(mel_min + (mel_max - mel_min) * i / (N_MELS + 1));

    // Convert to FFT bin indices
    std::vector<int> bins(N_MELS + 2);
    for (int i = 0; i < N_MELS + 2; i++)
      bins[i] = (int)std::floor((N_FFT + 1) * mel_points[i] / SAMPLE_RATE);

    mel_filterbank_.resize(N_MELS, std::vector<float>(num_fft_bins, 0.0f));
    for (int m = 0; m < N_MELS; m++) {
      for (int k = bins[m]; k < bins[m + 1] && k < num_fft_bins; k++) {
        mel_filterbank_[m][k] =
            (float)(k - bins[m]) / (float)(bins[m + 1] - bins[m]);
      }
      for (int k = bins[m + 1]; k < bins[m + 2] && k < num_fft_bins; k++) {
        mel_filterbank_[m][k] =
            (float)(bins[m + 2] - k) / (float)(bins[m + 2] - bins[m + 1]);
      }
    }

    // Build Hann window
    hann_window_.resize(N_FFT);
    for (int i = 0; i < N_FFT; i++)
      hann_window_[i] =
          0.5f * (1.0f - std::cos(2.0f * 3.14159265f * i / (N_FFT - 1)));
  }

  // ── Simple real-valued DFT (no complex FFT library needed) ──────────────
  // Computes power spectrum |X(k)|^2 for k = 0..N_FFT/2
  void computePowerSpectrum(const float *windowed, float *power_out) {
    const int half = N_FFT / 2 + 1;
    for (int k = 0; k < half; k++) {
      float re = 0.0f, im = 0.0f;
      for (int n = 0; n < N_FFT; n++) {
        float angle = 2.0f * 3.14159265f * k * n / N_FFT;
        re += windowed[n] * std::cos(angle);
        im -= windowed[n] * std::sin(angle);
      }
      power_out[k] = re * re + im * im;
    }
  }

  // ── Mel spectrogram from raw audio ──────────────────────────────────────
  // Returns flattened [num_frames, N_MELS]
  std::vector<float> computeMelFrames(const int16_t *audio, size_t n) {
    std::vector<float> result;
    if (n < (size_t)N_FFT)
      return result;

    const int num_fft_bins = N_FFT / 2 + 1;
    std::vector<float> windowed(N_FFT);
    std::vector<float> power(num_fft_bins);

    size_t num_frames = (n - N_FFT) / HOP_LENGTH + 1;
    result.reserve(num_frames * N_MELS);

    for (size_t f = 0; f < num_frames; f++) {
      size_t offset = f * HOP_LENGTH;

      // Apply Hann window and convert to float
      for (int i = 0; i < N_FFT; i++)
        windowed[i] = (float)audio[offset + i] / 32768.0f * hann_window_[i];

      // Power spectrum
      computePowerSpectrum(windowed.data(), power.data());

      // Apply mel filterbank + log
      for (int m = 0; m < N_MELS; m++) {
        float energy = 0.0f;
        for (int k = 0; k < num_fft_bins; k++)
          energy += mel_filterbank_[m][k] * power[k];
        result.push_back(std::log(std::max(energy, 1e-10f)));
      }
    }
    return result;
  }

  // ── Streaming mel accumulation ──────────────────────────────────────────
  void streamMelFrames(const int16_t *audio, size_t n) {
    // Prepend remainder from last call
    std::vector<int16_t> combined;
    if (!remainder_.empty()) {
      combined = remainder_;
      remainder_.clear();
    }
    combined.insert(combined.end(), audio, audio + n);

    if (combined.size() < (size_t)N_FFT) {
      remainder_ = combined;
      return;
    }

    auto mel = computeMelFrames(combined.data(), combined.size());
    mel_frame_buf_.insert(mel_frame_buf_.end(), mel.begin(), mel.end());

    // Keep remainder for next call
    size_t frames_used =
        (combined.size() - N_FFT) / HOP_LENGTH + 1;
    size_t samples_consumed = (frames_used - 1) * HOP_LENGTH + N_FFT;
    if (samples_consumed < combined.size())
      remainder_.assign(combined.begin() + samples_consumed, combined.end());

    // Trim to MAX_FRAMES sliding window
    size_t max_samples = MAX_FRAMES * N_MELS;
    if (mel_frame_buf_.size() > max_samples)
      mel_frame_buf_.erase(mel_frame_buf_.begin(),
                           mel_frame_buf_.end() - max_samples);
  }

  // ── ORT inference helpers ───────────────────────────────────────────────

  OrtMemoryInfo *makeCpuMem() {
    OrtMemoryInfo *mi = nullptr;
    api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi);
    return mi;
  }

  OrtValue *makeTensor(float *data, size_t count, int64_t *shape,
                       size_t ndim) {
    OrtMemoryInfo *mi = makeCpuMem();
    OrtValue *t = nullptr;
    api_->CreateTensorWithDataAsOrtValue(
        mi, data, count * sizeof(float), shape, ndim,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &t);
    api_->ReleaseMemoryInfo(mi);
    return t;
  }

  OrtValue *runSession(OrtSession *sess, OrtValue *input) {
    OrtAllocator *alloc = nullptr;
    api_->GetAllocatorWithDefaultOptions(&alloc);
    char *in_name = nullptr;
    char *out_name = nullptr;
    api_->SessionGetInputName(sess, 0, alloc, &in_name);
    api_->SessionGetOutputName(sess, 0, alloc, &out_name);

    if (!in_name || !out_name) {
      if (in_name)
        alloc->Free(alloc, in_name);
      if (out_name)
        alloc->Free(alloc, out_name);
      return nullptr;
    }

    const char *ins[] = {in_name};
    const char *outs[] = {out_name};

    OrtValue *output = nullptr;
    OrtStatus *st =
        api_->Run(sess, nullptr, ins, (const OrtValue *const *)&input, 1, outs,
                  1, &output);

    alloc->Free(alloc, in_name);
    alloc->Free(alloc, out_name);

    if (!wn_ort_ok(api_, st, "Run"))
      return nullptr;
    return output;
  }

  float *getTensorData(OrtValue *t, size_t &count) {
    float *data = nullptr;
    api_->GetTensorMutableData(t, (void **)&data);

    OrtTensorTypeAndShapeInfo *info = nullptr;
    api_->GetTensorTypeAndShape(t, &info);
    size_t ndim = 0;
    api_->GetDimensionsCount(info, &ndim);
    std::vector<int64_t> dims(ndim);
    api_->GetDimensions(info, dims.data(), ndim);
    count = 1;
    for (auto d : dims)
      count *= (size_t)d;
    api_->ReleaseTensorTypeAndShapeInfo(info);
    return data;
  }

  // ── Run WakeWordNet model ───────────────────────────────────────────────
  // Input: [1, N_MELS, T] where T = number of accumulated frames
  float runWakeModel(WakeModel &wm) {
    size_t num_frames = mel_frame_buf_.size() / N_MELS;
    if (num_frames < MIN_FRAMES)
      return 0.0f;

    // Transpose from [T, N_MELS] (row-major) to [N_MELS, T] (model expects)
    std::vector<float> transposed(N_MELS * num_frames);
    for (size_t t = 0; t < num_frames; t++) {
      for (int m = 0; m < N_MELS; m++) {
        transposed[m * num_frames + t] = mel_frame_buf_[t * N_MELS + m];
      }
    }

    int64_t shape[] = {1, (int64_t)N_MELS, (int64_t)num_frames};
    OrtValue *in =
        makeTensor(transposed.data(), transposed.size(), shape, 3);
    if (!in)
      return 0.0f;

    OrtValue *out = runSession(wm.session, in);
    float score = 0.0f;
    if (out) {
      size_t cnt = 0;
      float *d = getTensorData(out, cnt);
      // WakeWordNet typically outputs [1, 2] (not-wake, wake) or [1, 1] (sigmoid)
      if (cnt >= 2) {
        // Softmax output: take the "wake" class probability
        float max_val = std::max(d[0], d[1]);
        float exp0 = std::exp(d[0] - max_val);
        float exp1 = std::exp(d[1] - max_val);
        score = exp1 / (exp0 + exp1);
      } else if (cnt == 1) {
        // Sigmoid output
        score = 1.0f / (1.0f + std::exp(-d[0]));
      }
      api_->ReleaseValue(out);
    }
    if (in)
      api_->ReleaseValue(in);
    return score;
  }
};
