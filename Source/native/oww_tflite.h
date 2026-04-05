// oww_tflite.h — Native C++ OpenWakeWord detector using TensorFlow Lite C API
// Same pipeline as oww_native.h (ONNX version) but uses TFLite for ~3MB runtime
// instead of ONNX Runtime's ~15MB:
//   audio → melspectrogram.tflite → embedding_model.tflite → wakeword.tflite → score
#pragma once

#include "tflite_loader.h"
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

// Map of wakeword names to .tflite model filenames
// Includes both full and short-form aliases so dashboard selections match
static const std::unordered_map<std::string, std::string> TFLITE_BUILTIN_MAP = {
    // Full names
    {"hey jarvis", "hey_jarvis_v0.1.tflite"},
    {"hey mycroft", "hey_mycroft_v0.1.tflite"},
    {"hey rhasspy", "hey_rhasspy_v0.1.tflite"},
    {"alexa", "alexa_v0.1.tflite"},
    {"hey glados", "hey_glados_v0.1.tflite"},
    {"timer", "timer_v0.1.tflite"},
    {"weather", "weather_v0.1.tflite"},
    // Short-form aliases (dashboard often stores these)
    {"jarvis", "hey_jarvis_v0.1.tflite"},
    {"mycroft", "hey_mycroft_v0.1.tflite"},
    {"rhasspy", "hey_rhasspy_v0.1.tflite"},
    // "Snowboy" engine keyword — best available neural match
    {"snowboy", "hey_mycroft_v0.1.tflite"},
};

static inline void tfl_log(const std::string &msg) {
  fprintf(stderr, "[OWW-TFLITE] %s\n", msg.c_str());
  fflush(stderr);
}

class TFLiteWakeWordDetector {
public:
  float threshold = 0.60f;
  bool active = false;

  struct WakeModel {
    TfLiteInterpreter *interp = nullptr;
    TfLiteModel *model = nullptr;
    int64_t n_features = 16;
    std::string name;
  };
  std::vector<WakeModel> wake_models_;

  TFLiteWakeWordDetector() = default;
  ~TFLiteWakeWordDetector() { cleanup(); }

  bool init(TfLiteLoader &tfl, const std::string &model_dir,
            const std::vector<std::string> &wake_words, float thresh = 0.60f) {
    threshold = thresh;
    tfl_ = &tfl;
    if (!tfl_->loaded())
      return false;

    // Create shared options (1 thread for minimal overhead)
    opts_ = tfl_->optionsCreate();
    if (!opts_) return false;
    tfl_->optionsSetNumThreads(opts_, 1);

    // Load melspectrogram model (requires fixed input shape [1, 1280])
    std::string mel_path = model_dir + "\\melspectrogram.tflite";
    if (!loadModel(mel_path, &mel_model_, &mel_interp_, {1, 1280}))
      return false;
    tfl_log("Loaded melspectrogram model");

    // Load embedding model (requires fixed input shape [1, 76, 32, 1])
    std::string emb_path = model_dir + "\\embedding_model.tflite";
    if (!loadModel(emb_path, &emb_model_, &emb_interp_, {1, 76, 32, 1}))
      return false;
    tfl_log("Loaded embedding model");

    // Load wakeword models
    for (const auto &w : wake_words) {
      std::string key = w;
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      while (!key.empty() && key.front() == ' ')
        key.erase(key.begin());
      while (!key.empty() && key.back() == ' ')
        key.pop_back();

      auto it = TFLITE_BUILTIN_MAP.find(key);
      if (it == TFLITE_BUILTIN_MAP.end()) {
        tfl_log("No TFLite model for wakeword '" + key + "' — skipping");
        continue;
      }

      std::string fpath = model_dir + "\\" + it->second;
      if (!fs::exists(fpath)) {
        tfl_log("Model file not found: " + fpath + " — skipping");
        continue;
      }

      WakeModel wm;
      if (!loadModel(fpath, &wm.model, &wm.interp))
        continue;

      // Get expected input feature count from model
      TfLiteTensor *inp = tfl_->getInputTensor(wm.interp, 0);
      if (inp && tfl_->tensorNumDims(inp) >= 2) {
        wm.n_features = tfl_->tensorDim(inp, 1);
      }

      wm.name = it->second.substr(0, it->second.find(".tflite"));
      wake_models_.push_back(std::move(wm));
      tfl_log("Loaded wakeword: " + it->second +
              " (features=" + std::to_string(wm.n_features) + ")");
    }

    if (wake_models_.empty())
      return false;

    initBuffers();
    active = true;
    tfl_log("Ready with " + std::to_string(wake_models_.size()) + " model(s)");
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
    remainder_.clear();
    feat_buf_.clear();
    initFeatureBuf();
    for (auto &[k, v] : pred_hist_)
      v.clear();
  }

  void cleanup() {
    active = false;
    for (auto &wm : wake_models_) {
      if (wm.interp) tfl_->interpreterDelete(wm.interp);
      if (wm.model) tfl_->modelDelete(wm.model);
    }
    wake_models_.clear();
    if (mel_interp_) { tfl_->interpreterDelete(mel_interp_); mel_interp_ = nullptr; }
    if (mel_model_) { tfl_->modelDelete(mel_model_); mel_model_ = nullptr; }
    if (emb_interp_) { tfl_->interpreterDelete(emb_interp_); emb_interp_ = nullptr; }
    if (emb_model_) { tfl_->modelDelete(emb_model_); emb_model_ = nullptr; }
    if (opts_) { tfl_->optionsDelete(opts_); opts_ = nullptr; }
  }

private:
  TfLiteLoader *tfl_ = nullptr;
  TfLiteInterpreterOptions *opts_ = nullptr;
  TfLiteModel *mel_model_ = nullptr;
  TfLiteInterpreter *mel_interp_ = nullptr;
  TfLiteModel *emb_model_ = nullptr;
  TfLiteInterpreter *emb_interp_ = nullptr;

  // Buffers
  std::vector<float> mel_buf_; // [T, 32]
  std::vector<int16_t> remainder_;
  std::vector<float> feat_buf_; // [N * 96] flattened
  std::unordered_map<std::string, std::deque<float>> pred_hist_;

  static constexpr size_t EMB_DIM = 96;

  bool loadModel(const std::string &path, TfLiteModel **model,
                 TfLiteInterpreter **interp, const std::vector<int>& shape = {}) {
    *model = tfl_->modelCreateFromFile(path.c_str());
    if (!*model) {
      tfl_log("Failed to load model: " + path);
      return false;
    }
    *interp = tfl_->interpreterCreate(*model, opts_);
    if (!*interp) {
      tfl_log("Failed to create interpreter for: " + path);
      tfl_->modelDelete(*model);
      *model = nullptr;
      return false;
    }
    
    // Resize input tensor if shape is provided
    if (!shape.empty()) {
      tfl_->resizeInputTensor(*interp, 0, shape.data(), shape.size());
    }

    if (tfl_->allocateTensors(*interp) != kTfLiteOk) {
      tfl_log("Failed to allocate tensors for: " + path);
      tfl_->interpreterDelete(*interp);
      tfl_->modelDelete(*model);
      *interp = nullptr;
      *model = nullptr;
      return false;
    }
    return true;
  }

  void initBuffers() {
    mel_buf_.assign(76 * 32, 1.0f);
    remainder_.clear();
    initFeatureBuf();
  }

  void initFeatureBuf() {
    // Bootstrap: process enough random audio to fill initial embedding buffer
    // Process in 1280-sample chunks matching the fixed TFLite mel model input
    std::vector<int16_t> init(1280 * 50); // ~4 sec of random audio
    for (auto &s : init)
      s = (int16_t)((rand() % 2001) - 1000);
    
    // Process in 1280-sample chunks
    for (size_t off = 0; off + 1280 <= init.size(); off += 1280) {
      auto mel = getMelChunk(init.data() + off);
      mel_buf_.insert(mel_buf_.end(), mel.begin(), mel.end());
    }
    
    // Build initial embeddings from mel buffer
    size_t mf = mel_buf_.size() / 32;
    if (mf >= 76) {
      size_t start = (mf - 76) * 32;
      std::vector<float> win(mel_buf_.begin() + start, mel_buf_.end());
      
      TfLiteTensor *input = tfl_->getInputTensor(emb_interp_, 0);
      tfl_->tensorCopyFromBuffer(input, win.data(), win.size() * sizeof(float));
      
      if (tfl_->invoke(emb_interp_) == kTfLiteOk) {
        const TfLiteTensor *output = tfl_->getOutputTensor(emb_interp_, 0);
        size_t out_bytes = tfl_->tensorByteSize(output);
        size_t cnt = out_bytes / sizeof(float);
        feat_buf_.resize(cnt);
        tfl_->tensorCopyToBuffer(output, feat_buf_.data(), out_bytes);
      } else {
        feat_buf_.assign(EMB_DIM, 0.0f);
      }
    } else {
      feat_buf_.assign(EMB_DIM, 0.0f);
    }
  }

  // ── TFLite inference helpers ──────────────────────────────────────────

  // Run mel model on exactly 1280 samples (fixed TFLite input shape)
  // Returns mel features [T, 32] for this chunk
  std::vector<float> getMelChunk(const int16_t *audio_1280) {
    std::vector<float> fin(1280);
    for (size_t i = 0; i < 1280; i++)
      fin[i] = (float)audio_1280[i];

    // Input tensor already has fixed shape [1, 1280] — no resize needed
    TfLiteTensor *input = tfl_->getInputTensor(mel_interp_, 0);
    tfl_->tensorCopyFromBuffer(input, fin.data(), 1280 * sizeof(float));

    // Run inference
    if (tfl_->invoke(mel_interp_) != kTfLiteOk) {
      return {};
    }

    // Get output
    const TfLiteTensor *output = tfl_->getOutputTensor(mel_interp_, 0);
    size_t out_bytes = tfl_->tensorByteSize(output);
    size_t cnt = out_bytes / sizeof(float);
    std::vector<float> result(cnt);
    tfl_->tensorCopyToBuffer(output, result.data(), out_bytes);

    // Apply normalization: spec/10 + 2
    for (size_t i = 0; i < cnt; i++)
      result[i] = result[i] / 10.0f + 2.0f;

    return result;
  }

  // Full embedding: int16 audio → [N, 96]
  std::vector<float> getEmbeddings(const int16_t *audio, size_t n) {
    // Process audio in fixed 1280-sample chunks
    std::vector<float> all_mel;
    for (size_t off = 0; off + 1280 <= n; off += 1280) {
      auto mel = getMelChunk(audio + off);
      all_mel.insert(all_mel.end(), mel.begin(), mel.end());
    }
    if (all_mel.empty())
      return std::vector<float>(EMB_DIM, 0.0f);

    size_t frames = all_mel.size() / 32;
    std::vector<std::vector<float>> windows;
    for (size_t i = 0; i + 76 <= frames; i += 8)
      windows.emplace_back(all_mel.begin() + i * 32, all_mel.begin() + (i + 76) * 32);

    if (windows.empty())
      return std::vector<float>(EMB_DIM, 0.0f);

    // Process windows one at a time
    std::vector<float> result;
    for (auto &win : windows) {
      TfLiteTensor *input = tfl_->getInputTensor(emb_interp_, 0);
      tfl_->tensorCopyFromBuffer(input, win.data(), win.size() * sizeof(float));

      if (tfl_->invoke(emb_interp_) != kTfLiteOk)
        continue;

      const TfLiteTensor *output = tfl_->getOutputTensor(emb_interp_, 0);
      size_t out_bytes = tfl_->tensorByteSize(output);
      size_t cnt = out_bytes / sizeof(float);
      std::vector<float> emb(cnt);
      tfl_->tensorCopyToBuffer(output, emb.data(), out_bytes);
      result.insert(result.end(), emb.begin(), emb.end());
    }

    if (result.empty())
      return std::vector<float>(EMB_DIM, 0.0f);
    return result;
  }

  // Streaming: accumulate audio, compute embeddings every 1280 samples
  void streamFeatures(const int16_t *x, size_t n) {
    // Accumulate into remainder buffer
    remainder_.insert(remainder_.end(), x, x + n);

    // Process all complete 1280-sample chunks
    while (remainder_.size() >= 1280) {
      auto mel = getMelChunk(remainder_.data());
      remainder_.erase(remainder_.begin(), remainder_.begin() + 1280);
      mel_buf_.insert(mel_buf_.end(), mel.begin(), mel.end());
    }

    // Trim mel buffer
    size_t max_mel = 970 * 32;
    if (mel_buf_.size() > max_mel)
      mel_buf_.erase(mel_buf_.begin(), mel_buf_.end() - max_mel);

    // Extract last 76-frame window and compute embedding
    size_t mf = mel_buf_.size() / 32;
    if (mf >= 76) {
      size_t start = (mf - 76) * 32;
      std::vector<float> win(mel_buf_.begin() + start, mel_buf_.end());

      TfLiteTensor *input = tfl_->getInputTensor(emb_interp_, 0);
      tfl_->tensorCopyFromBuffer(input, win.data(), win.size() * sizeof(float));

      if (tfl_->invoke(emb_interp_) == kTfLiteOk) {
        const TfLiteTensor *output = tfl_->getOutputTensor(emb_interp_, 0);
        size_t out_bytes = tfl_->tensorByteSize(output);
        size_t cnt = out_bytes / sizeof(float);
        std::vector<float> emb(cnt);
        tfl_->tensorCopyToBuffer(output, emb.data(), out_bytes);
        feat_buf_.insert(feat_buf_.end(), emb.begin(),
                         emb.begin() + (int)std::min(cnt, EMB_DIM));
      }
    }

    // Trim feature buffer
    size_t max_feat = 120 * EMB_DIM;
    if (feat_buf_.size() > max_feat)
      feat_buf_.erase(feat_buf_.begin(), feat_buf_.end() - max_feat);
  }

  // Run wakeword model on features
  float runWakeModel(WakeModel &wm, const std::vector<float> &feats) {
    int dims[] = {1, (int)wm.n_features, (int)EMB_DIM};
    tfl_->resizeInputTensor(wm.interp, 0, dims, 3);
    tfl_->allocateTensors(wm.interp);

    TfLiteTensor *input = tfl_->getInputTensor(wm.interp, 0);
    if (!input)
      return 0.0f;

    tfl_->tensorCopyFromBuffer(input, feats.data(),
                               feats.size() * sizeof(float));

    if (tfl_->invoke(wm.interp) != kTfLiteOk)
      return 0.0f;

    const TfLiteTensor *output = tfl_->getOutputTensor(wm.interp, 0);
    size_t out_bytes = tfl_->tensorByteSize(output);
    size_t cnt = out_bytes / sizeof(float);
    std::vector<float> scores(cnt);
    tfl_->tensorCopyToBuffer(output, scores.data(), out_bytes);

    return (cnt > 0) ? *std::max_element(scores.begin(), scores.end()) : 0.0f;
  }
};
