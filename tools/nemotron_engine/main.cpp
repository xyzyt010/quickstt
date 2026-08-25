// nemotron_engine — JSON-line STT worker for Handy's Nemotron 3.5 streaming.
//
// Runtime: handy-computer/transcribe.cpp (same as Handy app: transcribe.dll + ggml)
// Model:   handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf (GGUF)
//
// Protocol (shared with parakeet_engine):
//   {"action":"load","model_path":"...","language":"en-US","latency_mode":13}
//   {"action":"unload"}
//   {"action":"transcribe","audio_path":"..."}
//   {"action":"transcribe_pcm","pcm_i16_b64":"..."}
//   {"action":"stream_start","latency_mode":13}
//   {"action":"stream_feed","pcm_i16_b64":"..."}
//   {"action":"stream_end"}
//   {"action":"ping"}
//   {"action":"quit"}
//
// Responses:
//   {"status":"ok","text":"..."}
//   {"status":"ok","partial":"...","committed":"..."}
//   {"status":"error","error":"..."}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define TRANSCRIBE_API __declspec(dllimport)

#include "transcribe.h"
#include "transcribe/parakeet.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> base64Decode(const std::string &in) {
  static const int8_t kTable[256] = {
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
      52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,-1,0,1,2,3,4,5,6,7,8,9,
      10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,-1,26,27,
      28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51};
  std::vector<uint8_t> out;
  out.reserve(in.size() * 3 / 4);
  uint32_t buf = 0;
  int bits = 0;
  for (unsigned char c : in) {
    if (c == '=' || c == '\r' || c == '\n' || c == ' ')
      break;
    int8_t v = kTable[c];
    if (v < 0)
      continue;
    buf = (buf << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t((buf >> bits) & 0xFF));
    }
  }
  return out;
}

std::vector<float> decodePcmI16B64(const std::string &b64) {
  auto bytes = base64Decode(b64);
  std::vector<float> samples(bytes.size() / 2);
  for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
    int16_t s =
        int16_t(uint16_t(bytes[i]) | (uint16_t(bytes[i + 1]) << 8));
    samples[i / 2] = float(s) / 32768.0f;
  }
  return samples;
}

std::string jsonGetString(const std::string &json, const std::string &key) {
  const std::string pat = "\"" + key + "\"";
  size_t k = json.find(pat);
  if (k == std::string::npos)
    return {};
  k = json.find(':', k + pat.size());
  if (k == std::string::npos)
    return {};
  k = json.find('"', k + 1);
  if (k == std::string::npos)
    return {};
  size_t end = k + 1;
  std::string out;
  while (end < json.size()) {
    char c = json[end++];
    if (c == '\\' && end < json.size()) {
      out.push_back(json[end++]);
      continue;
    }
    if (c == '"')
      break;
    out.push_back(c);
  }
  return out;
}

int jsonGetInt(const std::string &json, const std::string &key, int fallback) {
  const std::string pat = "\"" + key + "\"";
  size_t k = json.find(pat);
  if (k == std::string::npos)
    return fallback;
  k = json.find(':', k + pat.size());
  if (k == std::string::npos)
    return fallback;
  ++k;
  while (k < json.size() && (json[k] == ' ' || json[k] == '\t'))
    ++k;
  try {
    return std::stoi(json.substr(k));
  } catch (...) {
    return fallback;
  }
}

std::string jsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\\' || c == '"')
      out.push_back('\\');
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    if (c == '\r')
      continue;
    out.push_back(c);
  }
  return out;
}

void respondOk(const std::string &text = {}, const std::string &partial = {},
               const std::string &committed = {}) {
  std::ostringstream oss;
  oss << "{\"status\":\"ok\"";
  if (!text.empty())
    oss << ",\"text\":\"" << jsonEscape(text) << "\"";
  if (!partial.empty())
    oss << ",\"partial\":\"" << jsonEscape(partial) << "\"";
  if (!committed.empty())
    oss << ",\"committed\":\"" << jsonEscape(committed) << "\"";
  oss << "}";
  std::cout << oss.str() << std::endl;
}

void respondError(const std::string &err) {
  std::cout << "{\"status\":\"error\",\"error\":\"" << jsonEscape(err) << "\"}"
            << std::endl;
}

// Map latency_mode onto Nemotron 3.5 att_context_right.
// Handy streaming default uses att_context_right=6 for real-time accuracy.
int mapLatencyToAttRight(int latencyMode) {
  switch (latencyMode) {
  case 0:
    return 3; // ~160ms low latency
  case 1:
    return 3; // ~160–240ms class
  case 6:
    return 6; // balanced streaming (Handy default)
  case 13:
  default:
    return 13; // max accuracy / 1.12s (Handy offline default)
  }
}

std::string resolveGgufPath(const std::string &modelPath) {
  fs::path p(modelPath);
  if (fs::is_regular_file(p) && p.extension() == ".gguf")
    return p.string();
  if (!fs::is_directory(p))
    return {};

  // Prefer Q8_0 (Handy's sweet spot), then any .gguf.
  const char *prefs[] = {
      "nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf",
      "nemotron-3.5-asr-streaming-0.6b-Q5_K_M.gguf",
      "nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf",
      "nemotron-3.5-asr-streaming-0.6b-F16.gguf",
  };
  for (const char *name : prefs) {
    fs::path c = p / name;
    if (fs::exists(c))
      return c.string();
  }
  for (auto &e : fs::directory_iterator(p)) {
    if (e.path().extension() == ".gguf")
      return e.path().string();
  }
  // One level deeper (e.g. nemotron-3.5-asr-streaming-0.6b/)
  for (auto &e : fs::directory_iterator(p)) {
    if (!e.is_directory())
      continue;
    for (auto &f : fs::directory_iterator(e.path())) {
      if (f.path().extension() == ".gguf")
        return f.path().string();
    }
  }
  return {};
}

// Minimal 16-bit mono WAV reader (PCM only).
bool readWavPcmF32(const std::string &path, std::vector<float> *out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  char riff[12];
  in.read(riff, 12);
  if (!in || std::string(riff, 4) != "RIFF" || std::string(riff + 8, 4) != "WAVE")
    return false;
  uint16_t audioFormat = 0, channels = 0, bits = 0;
  uint32_t sampleRate = 0, dataSize = 0;
  bool gotFmt = false, gotData = false;
  std::vector<char> data;
  while (in && !(gotFmt && gotData)) {
    char id[4];
    uint32_t sz = 0;
    in.read(id, 4);
    in.read(reinterpret_cast<char *>(&sz), 4);
    if (!in)
      break;
    if (std::string(id, 4) == "fmt ") {
      uint16_t blockAlign = 0;
      uint32_t byteRate = 0;
      in.read(reinterpret_cast<char *>(&audioFormat), 2);
      in.read(reinterpret_cast<char *>(&channels), 2);
      in.read(reinterpret_cast<char *>(&sampleRate), 4);
      in.read(reinterpret_cast<char *>(&byteRate), 4);
      in.read(reinterpret_cast<char *>(&blockAlign), 2);
      in.read(reinterpret_cast<char *>(&bits), 2);
      if (sz > 16)
        in.seekg(sz - 16, std::ios::cur);
      gotFmt = true;
    } else if (std::string(id, 4) == "data") {
      data.resize(sz);
      in.read(data.data(), sz);
      dataSize = sz;
      gotData = true;
    } else {
      in.seekg(sz, std::ios::cur);
    }
  }
  if (!gotFmt || !gotData || audioFormat != 1 || channels < 1 || bits != 16)
    return false;
  if (sampleRate != 16000) {
    // Still accept; caller should resample. We just load mono.
  }
  const size_t nFrames = dataSize / (channels * 2);
  out->resize(nFrames);
  const int16_t *src = reinterpret_cast<const int16_t *>(data.data());
  for (size_t i = 0; i < nFrames; ++i) {
    int acc = 0;
    for (int c = 0; c < channels; ++c)
      acc += src[i * channels + c];
    (*out)[i] = float(acc / channels) / 32768.0f;
  }
  return true;
}

std::string exeDir() {
  // Prefer directory of this executable (where transcribe.dll + ggml live).
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    fs::path p(buf);
    return p.parent_path().string();
  }
#endif
  return ".";
}

} // namespace

int main() {
  std::cout.setf(std::ios::unitbuf);

#ifdef _WIN32
  // Trim initial working set to keep idle memory < 15MB
  SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
#endif

  // Load ggml backend modules next to this exe (Handy's layout).
  const std::string artifactDir = exeDir();
  {
    transcribe_status st = transcribe_init_backends(artifactDir.c_str());
    if (st != TRANSCRIBE_OK) {
      // Fall back to default package-local scan.
      st = transcribe_init_backends_default();
      if (st != TRANSCRIBE_OK) {
        // Still try — CPU may be compiled in.
        std::cerr << "warn: transcribe_init_backends: "
                  << transcribe_status_string(st) << std::endl;
      }
    }
  }

  struct transcribe_session *session = nullptr;
  bool streaming = false;
  int latencyMode = 6;
  std::string language = "en-US";
  std::string line;

  auto freeSession = [&]() {
    if (session) {
      transcribe_session_free(session);
      session = nullptr;
    }
    streaming = false;
#ifdef _WIN32
    // Immediately release model weights from RAM back to Windows OS
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
#endif
  };

  auto beginStream = [&]() -> bool {
    if (!session) {
      respondError("Model not loaded");
      return false;
    }
    // ALWAYS reset stream session before beginning a new stream
    // to wipe any stale audio frames, KV caches, or decoder context.
    transcribe_stream_reset(session);
    streaming = false;

    struct transcribe_run_params run;
    transcribe_run_params_init(&run);
    run.language = language.c_str();
    run.timestamps = TRANSCRIBE_TIMESTAMPS_NONE;

    struct transcribe_stream_params sp;
    transcribe_stream_params_init(&sp);
    sp.commit_policy = TRANSCRIBE_STREAM_COMMIT_STABLE_PREFIX;

    struct transcribe_parakeet_stream_ext ext;
    const struct transcribe_model *model = transcribe_get_model(session);
    bool useExt = false;
    if (model &&
        transcribe_model_accepts_ext_kind(model, TRANSCRIBE_EXT_SLOT_STREAM,
                                          TRANSCRIBE_EXT_KIND_PARAKEET_STREAM)) {
      transcribe_parakeet_stream_ext_init(&ext);
      ext.att_context_right = mapLatencyToAttRight(latencyMode);
      sp.family = &ext.ext;
      useExt = true;
    }
    (void)useExt;

    transcribe_status st = transcribe_stream_begin(session, &run, &sp);
    if (st != TRANSCRIBE_OK) {
      respondError(std::string("stream_begin failed: ") +
                   transcribe_status_string(st));
      return false;
    }
    streaming = true;
    return true;
  };

  while (std::getline(std::cin, line)) {
    if (line.empty())
      continue;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();

    const std::string action = jsonGetString(line, "action");
    if (action.empty()) {
      respondError("missing action");
      continue;
    }

    if (action == "ping") {
      respondOk();
      continue;
    }
    if (action == "quit") {
      freeSession();
      respondOk();
      break;
    }
    if (action == "unload") {
      freeSession();
      respondOk();
      continue;
    }

    if (action == "load") {
      freeSession();
      latencyMode = jsonGetInt(line, "latency_mode", latencyMode);
      std::string lang = jsonGetString(line, "language");
      if (!lang.empty())
        language = lang;
      std::string modelPath = jsonGetString(line, "model_path");
      if (modelPath.empty()) {
        respondError("model_path required");
        continue;
      }
      std::string gguf = resolveGgufPath(modelPath);
      if (gguf.empty()) {
        respondError("no .gguf found at " + modelPath);
        continue;
      }

      struct transcribe_model_load_params lp;
      transcribe_model_load_params_init(&lp);
      // AUTO picks Vulkan if Handy-style ggml-vulkan is present, else CPU.
      lp.backend = TRANSCRIBE_BACKEND_AUTO;

      struct transcribe_session_params sp;
      transcribe_session_params_init(&sp);
      unsigned int hw = std::thread::hardware_concurrency();
      sp.n_threads = (hw > 0) ? (int)std::min(4u, hw) : 4;

      transcribe_status st =
          transcribe_open(gguf.c_str(), &lp, &sp, &session);
      if (st != TRANSCRIBE_OK || !session) {
        respondError(std::string("load failed: ") +
                     transcribe_status_string(st) + " path=" + gguf);
        session = nullptr;
        continue;
      }

      // Verify streaming capability (Handy Live requires it).
      struct transcribe_capabilities caps;
      transcribe_capabilities_init(&caps);
      if (transcribe_model_get_capabilities(transcribe_get_model(session),
                                            &caps) == TRANSCRIBE_OK) {
        if (!caps.supports_streaming) {
          freeSession();
          respondError("model does not support streaming");
          continue;
        }
      }
      respondOk();
      continue;
    }

    if (!session) {
      respondError("Model not loaded");
      continue;
    }

    if (action == "stream_start") {
      latencyMode = jsonGetInt(line, "latency_mode", latencyMode);
      std::string lang = jsonGetString(line, "language");
      if (!lang.empty())
        language = lang;
      if (beginStream())
        respondOk();
      continue;
    }

    if (action == "stream_feed") {
      if (!streaming) {
        if (!beginStream())
          continue;
      }
      std::string b64 = jsonGetString(line, "pcm_i16_b64");
      if (b64.empty()) {
        respondError("No pcm_i16_b64 provided");
        continue;
      }
      auto samples = decodePcmI16B64(b64);
      if (samples.empty()) {
        respondError("Empty PCM buffer");
        continue;
      }
      struct transcribe_stream_update upd;
      transcribe_stream_update_init(&upd);
      transcribe_status st = transcribe_stream_feed(
          session, samples.data(), int(samples.size()), &upd);
      if (st != TRANSCRIBE_OK) {
        streaming = false;
        respondError(std::string("stream_feed failed: ") +
                     transcribe_status_string(st));
        continue;
      }
      struct transcribe_stream_text text;
      transcribe_stream_text_init(&text);
      std::string committed, partial;
      if (transcribe_stream_get_text(session, &text) == TRANSCRIBE_OK) {
        if (text.committed_text)
          committed = text.committed_text;
        if (text.tentative_text)
          partial = text.tentative_text;
        else if (text.full_text && committed.empty())
          partial = text.full_text;
      }
      respondOk(/*text=*/{}, /*partial=*/partial, /*committed=*/committed);
      continue;
    }

    if (action == "stream_end") {
      if (!streaming) {
        // Nothing streamed — empty final.
        respondOk("");
        continue;
      }
      struct transcribe_stream_update upd;
      transcribe_stream_update_init(&upd);
      transcribe_status st = transcribe_stream_finalize(session, &upd);
      streaming = false;
      if (st != TRANSCRIBE_OK) {
        respondError(std::string("stream_end failed: ") +
                     transcribe_status_string(st));
        continue;
      }
      const char *ft = transcribe_full_text(session);
      std::string text = ft ? ft : "";
      // Prefer committed+tentative composition if full is empty.
      if (text.empty()) {
        struct transcribe_stream_text stxt;
        transcribe_stream_text_init(&stxt);
        if (transcribe_stream_get_text(session, &stxt) == TRANSCRIBE_OK) {
          if (stxt.committed_text)
            text += stxt.committed_text;
          if (stxt.tentative_text)
            text += stxt.tentative_text;
        }
      }
      // Reset session to release KV cache and clear decoder states immediately
      transcribe_stream_reset(session);
      respondOk(text);
      continue;
    }

    if (action == "transcribe_pcm") {
      std::string b64 = jsonGetString(line, "pcm_i16_b64");
      if (b64.empty()) {
        respondError("No pcm_i16_b64 provided");
        continue;
      }
      auto samples = decodePcmI16B64(b64);
      if (samples.empty()) {
        respondError("Empty PCM buffer");
        continue;
      }
      if (streaming) {
        transcribe_stream_reset(session);
        streaming = false;
      }
      struct transcribe_run_params run;
      transcribe_run_params_init(&run);
      run.language = language.c_str();
      run.timestamps = TRANSCRIBE_TIMESTAMPS_NONE;
      transcribe_status st =
          transcribe_run(session, samples.data(), int(samples.size()), &run);
      if (st != TRANSCRIBE_OK) {
        respondError(std::string("transcribe_pcm failed: ") +
                     transcribe_status_string(st));
        continue;
      }
      const char *ft = transcribe_full_text(session);
      respondOk(ft ? ft : "");
      continue;
    }

    if (action == "transcribe") {
      std::string audioPath = jsonGetString(line, "audio_path");
      if (audioPath.empty()) {
        respondError("audio_path required");
        continue;
      }
      std::vector<float> samples;
      if (!readWavPcmF32(audioPath, &samples) || samples.empty()) {
        respondError("failed to read wav: " + audioPath);
        continue;
      }
      if (streaming) {
        transcribe_stream_reset(session);
        streaming = false;
      }
      struct transcribe_run_params run;
      transcribe_run_params_init(&run);
      run.language = language.c_str();
      run.timestamps = TRANSCRIBE_TIMESTAMPS_NONE;
      transcribe_status st =
          transcribe_run(session, samples.data(), int(samples.size()), &run);
      if (st != TRANSCRIBE_OK) {
        respondError(std::string("transcribe failed: ") +
                     transcribe_status_string(st));
        continue;
      }
      const char *ft = transcribe_full_text(session);
      respondOk(ft ? ft : "");
      continue;
    }

    respondError("unknown action: " + action);
  }

  freeSession();
  return 0;
}
