// oww_native.h — Native C++ OpenWakeWord detector using ONNX Runtime C API
// Replaces oww_lite.py entirely — same pipeline:
//   audio → melspectrogram.onnx → embedding_model.onnx → wakeword_model.onnx →
//   score
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

static const std::unordered_map<std::string, std::string> OWW_BUILTIN_MAP = {
    {"computer", "computer_v0.1.onnx"},
    {"hey jarvis", "hey_jarvis_v0.1.onnx"},
    {"hey mycroft", "hey_mycroft_v0.1.onnx"},
    {"alexa", "alexa_v0.1.onnx"},
    {"hey rhasspy", "hey_rhasspy_v0.1.onnx"},
    {"timer", "timer_v0.1.onnx"},
    {"weather", "weather_v0.1.onnx"},
};

static inline void oww_log(const std::string &msg) {
  fprintf(stderr, "[OWW-NATIVE] %s\n", msg.c_str());
  fflush(stderr);
}

static inline bool ort_ok(const OrtApi *api, OrtStatus *status,
                          const char *ctx = "") {
  if (!status)
    return true;
  const char *msg = api->GetErrorMessage(status);
  fprintf(stderr, "[ORT-ERROR] %s: %s\n", ctx, msg);
  api->ReleaseStatus(status);
  return false;
}

class NativeWakeWordDetector {
public:
  float threshold = 0.60f;
  bool active = false;

  struct WakeModel {
    OrtSession *session = nullptr;
    int64_t n_features = 16;
    std::string name;
  };
  std::vector<WakeModel> wake_models_;

  NativeWakeWordDetector() = default;
  ~NativeWakeWordDetector() { cleanup(); }

  bool init(OrtLoader &ort, const std::string &model_dir,
            const std::vector<std::string> &wake_words, float thresh = 0.60f) {
    threshold = thresh;
    api_ = ort.api;
    if (!api_)
      return false;

    OrtStatus *st = nullptr;
    st = api_->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "oww", &env_);
    if (!ort_ok(api_, st, "CreateEnv"))
      return false;

    st = api_->CreateSessionOptions(&opts_);
    if (!ort_ok(api_, st, "CreateSessionOptions"))
      return false;
    api_->SetIntraOpNumThreads(opts_, 1);
    api_->SetInterOpNumThreads(opts_, 1);
    api_->SetSessionGraphOptimizationLevel(opts_, ORT_ENABLE_ALL);

    std::string mel_path = model_dir + "\\melspectrogram.onnx";
    if (!loadSession(mel_path, &mel_sess_))
      return false;
    oww_log("Loaded melspectrogram model");

    std::string emb_path = model_dir + "\\embedding_model.onnx";
    if (!loadSession(emb_path, &emb_sess_))
      return false;
    oww_log("Loaded embedding model");

    for (const auto &w : wake_words) {
      std::string key = w;
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      while (!key.empty() && key.front() == ' ')
        key.erase(key.begin());
      while (!key.empty() && key.back() == ' ')
        key.pop_back();

      auto it = OWW_BUILTIN_MAP.find(key);
      if (it == OWW_BUILTIN_MAP.end())
        continue;

      std::string fpath = model_dir + "\\" + it->second;
      if (!fs::exists(fpath))
        continue;

      WakeModel wm;
      if (!loadSession(fpath, &wm.session))
        continue;

      // Get expected input feature count from model
      OrtTypeInfo *ti = nullptr;
      api_->SessionGetInputTypeInfo(wm.session, 0, &ti);
      if (ti) {
        const OrtTensorTypeAndShapeInfo *si = nullptr;
        api_->CastTypeInfoToTensorInfo(ti, &si);
        if (si) {
          size_t ndim = 0;
          api_->GetDimensionsCount(si, &ndim);
          std::vector<int64_t> dims(ndim);
          api_->GetDimensions(si, dims.data(), ndim);
          if (ndim >= 2)
            wm.n_features = dims[1];
        }
        api_->ReleaseTypeInfo(ti);
      }

      wm.name = it->second.substr(0, it->second.find(".onnx"));
      wake_models_.push_back(std::move(wm));
      oww_log("Loaded wakeword: " + it->second +
              " (features=" + std::to_string(wm.n_features) + ")");
    }

    if (wake_models_.empty())
      return false;

    initBuffers();
    active = true;
    oww_log("Ready with " + std::to_string(wake_models_.size()) + " model(s)");
    return true;
  }

  std::unordered_map<std::string, float> predict(const int16_t *audio,
                                                 size_t n) {
    std::unordered_map<std::string, float> out;
    if (!active)
      return out;

    streamFeatures(audio, n);

    for (auto &wm : wake_models_) {
      float score = 0.0f;
      size_t need = (size_t)wm.n_features * EMB_DIM;
      if (feat_buf_.size() >= need) {
        std::vector<float> feats(feat_buf_.end() - need, feat_buf_.end());
        score = runWakeModel(wm, feats);
      }
      auto &hist = pred_hist_[wm.name];
      if (hist.size() < 5)
        score = 0.0f;
      hist.push_back(score);
      if (hist.size() > 30)
        hist.pop_front();
      out[wm.name] = score;
    }
    return out;
  }

  void reset() {
    mel_buf_.assign(76 * 32, 1.0f);
    accum_ = 0;
    remainder_.clear();
    feat_buf_.clear();
    initFeatureBuf();
    for (auto &[k, v] : pred_hist_)
      v.clear();
  }

  void cleanup() {
    active = false;
    for (auto &wm : wake_models_)
      if (wm.session)
        api_->ReleaseSession(wm.session);
    wake_models_.clear();
    if (mel_sess_) {
      api_->ReleaseSession(mel_sess_);
      mel_sess_ = nullptr;
    }
    if (emb_sess_) {
      api_->ReleaseSession(emb_sess_);
      emb_sess_ = nullptr;
    }
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
  OrtSession *mel_sess_ = nullptr;
  OrtSession *emb_sess_ = nullptr;

  // Buffers
  std::vector<float> mel_buf_; // [T, 32]
  size_t accum_ = 0;
  std::vector<int16_t> remainder_;
  std::vector<float> feat_buf_; // [N * 96] flattened
  std::unordered_map<std::string, std::deque<float>> pred_hist_;

  static const size_t EMB_DIM = 96;

  bool loadSession(const std::string &path, OrtSession **sess) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wp(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wp[0], wlen);
    OrtStatus *st = api_->CreateSession(env_, wp.c_str(), opts_, sess);
    return ort_ok(api_, st, path.c_str());
  }

  void initBuffers() {
    mel_buf_.assign(76 * 32, 1.0f);
    accum_ = 0;
    remainder_.clear();
    initFeatureBuf();
  }

  void initFeatureBuf() {
    // Bootstrap with random audio to fill initial embeddings
    std::vector<int16_t> init(16000 * 4);
    for (auto &s : init)
      s = (int16_t)((rand() % 2001) - 1000);
    feat_buf_ = getEmbeddings(init.data(), init.size());
  }

  // ── ORT inference helpers ──────────────────────────────────────────────

  OrtMemoryInfo *makeCpuMem() {
    OrtMemoryInfo *mi = nullptr;
    api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi);
    return mi;
  }

  OrtValue *makeTensor(float *data, size_t count, int64_t *shape, size_t ndim) {
    OrtMemoryInfo *mi = makeCpuMem();
    OrtValue *t = nullptr;
    api_->CreateTensorWithDataAsOrtValue(
        mi, data, count * sizeof(float), shape, ndim,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &t);
    api_->ReleaseMemoryInfo(mi);
    return t;
  }

  // Run a session with single input/output dynamically parsing names
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

    if (!ort_ok(api_, st, "Run"))
      return nullptr;
    return output;
  }

  // Get float data pointer and total element count from output tensor
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

  // ── Pipeline stages ────────────────────────────────────────────────────

  // mel spectrogram: int16 audio → [T, 32] features (spec/10 + 2)
  std::vector<float> getMel(const int16_t *audio, size_t n) {
    std::vector<float> fin(n);
    for (size_t i = 0; i < n; i++)
      fin[i] = (float)audio[i];

    int64_t shape[] = {1, (int64_t)n};
    OrtValue *in = makeTensor(fin.data(), n, shape, 2);
    OrtValue *out = runSession(mel_sess_, in);

    std::vector<float> result;
    if (out) {
      size_t cnt = 0;
      float *d = getTensorData(out, cnt);
      result.resize(cnt);
      for (size_t i = 0; i < cnt; i++)
        result[i] = d[i] / 10.0f + 2.0f;
      api_->ReleaseValue(out);
    }
    if (in)
      api_->ReleaseValue(in);
    return result;
  }

  // Full embedding: int16 audio → [N, 96]
  std::vector<float> getEmbeddings(const int16_t *audio, size_t n) {
    auto mel = getMel(audio, n);
    if (mel.empty())
      return std::vector<float>(EMB_DIM, 0.0f);

    size_t frames = mel.size() / 32;
    std::vector<std::vector<float>> windows;
    for (size_t i = 0; i + 76 <= frames; i += 8)
      windows.emplace_back(mel.begin() + i * 32, mel.begin() + (i + 76) * 32);

    if (windows.empty())
      return std::vector<float>(EMB_DIM, 0.0f);

    size_t batch = windows.size();
    std::vector<float> flat(batch * 76 * 32);
    for (size_t b = 0; b < batch; b++)
      memcpy(&flat[b * 76 * 32], windows[b].data(), 76 * 32 * sizeof(float));

    int64_t shape[] = {(int64_t)batch, 76, 32, 1};
    OrtValue *in = makeTensor(flat.data(), flat.size(), shape, 4);
    OrtValue *out = runSession(emb_sess_, in);

    std::vector<float> result;
    if (out) {
      size_t cnt = 0;
      float *d = getTensorData(out, cnt);
      result.assign(d, d + cnt);
      api_->ReleaseValue(out);
    } else {
      result.assign(EMB_DIM, 0.0f);
    }
    if (in)
      api_->ReleaseValue(in);
    return result;
  }

  // Streaming: accumulate audio, compute embeddings every 1280 samples
  void streamFeatures(const int16_t *x, size_t n) {
    std::vector<int16_t> combined;
    if (!remainder_.empty()) {
      combined = remainder_;
      remainder_.clear();
    }
    combined.insert(combined.end(), x, x + n);

    size_t total = accum_ + combined.size();
    if (total < 1280) {
      accum_ = total;
      return;
    }

    size_t rem = total % 1280;
    size_t use = combined.size() - rem;
    accum_ += use;
    if (rem > 0)
      remainder_.assign(combined.begin() + use, combined.end());

    // only process when we have a full 1280 block
    if (accum_ % 1280 != 0)
      return;

    auto mel = getMel(combined.data(), use);
    mel_buf_.insert(mel_buf_.end(), mel.begin(), mel.end());

    // Trim mel buffer
    size_t max_mel = 970 * 32;
    if (mel_buf_.size() > max_mel)
      mel_buf_.erase(mel_buf_.begin(), mel_buf_.end() - max_mel);

    // Extract last 76-frame window and compute embedding
    size_t mf = mel_buf_.size() / 32;
    if (mf >= 76) {
      size_t start = (mf - 76) * 32;
      std::vector<float> win(mel_buf_.begin() + start, mel_buf_.end());

      int64_t shape[] = {1, 76, 32, 1};
      OrtValue *in = makeTensor(win.data(), win.size(), shape, 4);
      OrtValue *out = runSession(emb_sess_, in);
      if (out) {
        size_t cnt = 0;
        float *d = getTensorData(out, cnt);
        feat_buf_.insert(feat_buf_.end(), d, d + EMB_DIM);
        api_->ReleaseValue(out);
      }
      if (in)
        api_->ReleaseValue(in);
    }

    // Trim feature buffer
    size_t max_feat = 120 * EMB_DIM;
    if (feat_buf_.size() > max_feat)
      feat_buf_.erase(feat_buf_.begin(), feat_buf_.end() - max_feat);

    accum_ = 0;
  }

  // Run wakeword model on features
  float runWakeModel(WakeModel &wm, const std::vector<float> &feats) {
    int64_t shape[] = {1, wm.n_features, (int64_t)EMB_DIM};
    OrtValue *in = makeTensor((float *)feats.data(), feats.size(), shape, 3);
    if (!in)
      return 0.0f;

    OrtValue *out = runSession(wm.session, in);
    float score = 0.0f;
    if (out) {
      size_t cnt = 0;
      float *d = getTensorData(out, cnt);
      score = (cnt > 0) ? *std::max_element(d, d + cnt) : 0.0f;
      api_->ReleaseValue(out);
    }
    if (in)
      api_->ReleaseValue(in);
    return score;
  }
};
