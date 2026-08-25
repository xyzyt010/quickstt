// stt_service_native.cpp — Complete native C++ STT service
// Replaces the legacy scripted audio engine stack entirely
// Uses: libvosk.dll (C API), onnxruntime.dll (C API), winmm.dll (waveIn),
// user32.dll (SendInput) Zero Python dependency for Vosk + OWW.
//
// Communicates with QuickSTT_App via stdin/stdout pipe protocol:
//   OUT: STATE|code,message  FINAL_TEXT|text  AUDIO_LEVEL|0-100  DL_PROGRESS|%
//   DL_COMPLETE|name IN:  TOGGLE  STOP  SLEEP  MODEL:name  WAKEWORDS:csv
//   CLOSEWORDS:csv  WAKEMODE:engine  QUIT

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#pragma comment(lib, "winmm.lib")
#else
// Linux: provide minimal Windows compat shims used by remaining code
#include <unistd.h>
#include <dlfcn.h>
#include <limits.h>
#include <malloc.h>
#include <signal.h>
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#ifndef HKEY
using HKEY = void*;
#endif
#define HKEY_CURRENT_USER ((HKEY)0)
#define ERROR_SUCCESS 0
#define KEY_READ 0x20019
#define REG_MULTI_SZ 7
#define REG_SZ 1
#define REG_DWORD 4
using DWORD = unsigned long;
using BYTE = unsigned char;
inline long RegOpenKeyExA(HKEY, const char*, unsigned long, unsigned long, HKEY*) { return 1; }
inline long RegQueryValueExA(HKEY, const char*, unsigned long*, unsigned long*, BYTE*, DWORD*) { return 1; }
inline long RegCloseKey(HKEY) { return 0; }
#endif
#include "platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdarg>
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
#include "ort_loader.h"
#include "wakenet_native.h"
#include "pv_native.h"
#include "vosk_api.h"
#include "portaudio.h"
#include "audio_preprocess.h"

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════════
// Base64 encoder (for sending PCM to Parakeet engine without file I/O)
// ═══════════════════════════════════════════════════════════════════════════════

static const char kB64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t *data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = uint32_t(data[i]) << 16;
    if (i + 1 < len) n |= uint32_t(data[i + 1]) << 8;
    if (i + 2 < len) n |= uint32_t(data[i + 2]);
    out.push_back(kB64Table[(n >> 18) & 63]);
    out.push_back(kB64Table[(n >> 12) & 63]);
    out.push_back((i + 1 < len) ? kB64Table[(n >> 6) & 63] : '=');
    out.push_back((i + 2 < len) ? kB64Table[n & 63] : '=');
  }
  return out;
}

// Forward declaration (defined in Logging section below)
static void svc_log(const char *fmt, ...);

// ═══════════════════════════════════════════════════════════════════════════════
// Parakeet Direct Pipe — persistent child process for zero-file-I/O inference
// Mirrors Handy's in-process approach: PCM in → text out, no disk round-trip
// ═══════════════════════════════════════════════════════════════════════════════

#ifdef _WIN32
struct ParakeetPipe {
  HANDLE hProc = nullptr;
  HANDLE hStdin = nullptr;
  HANDLE hStdout = nullptr;
  bool ready = false;
  bool modelLoaded = false;
  std::string readBuffer;
  std::string lastExePath;
  std::string lastWorkDir;

  // True when the worker process handle is still a live OS process.
  // If the user kills parakeet_engine.exe from Task Manager, ready may still
  // be true until we notice — this is the recovery gate.
  bool isProcessAlive() const {
    if (!hProc)
      return false;
    return WaitForSingleObject(hProc, 0) == WAIT_TIMEOUT;
  }

  void markDead(const char *reason) {
    if (!ready && !hProc)
      return;
    svc_log("ParakeetPipe: marking dead (%s)", reason ? reason : "unknown");
    if (hStdin) {
      CloseHandle(hStdin);
      hStdin = nullptr;
    }
    if (hStdout) {
      CloseHandle(hStdout);
      hStdout = nullptr;
    }
    if (hProc) {
      // Process may already be gone; TerminateProcess is best-effort cleanup.
      if (isProcessAlive())
        TerminateProcess(hProc, 1);
      CloseHandle(hProc);
      hProc = nullptr;
    }
    ready = false;
    modelLoaded = false;
    readBuffer.clear();
  }

  bool launch(const std::string &exePath, const std::string &workDir) {
    // If a previous worker was killed externally, drop stale handles first.
    if (ready && !isProcessAlive())
      markDead("stale process before relaunch");
    if (ready)
      return true;

    lastExePath = exePath;
    lastWorkDir = workDir;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hChildStdinR = nullptr, hChildStdinW = nullptr;
    HANDLE hChildStdoutR = nullptr, hChildStdoutW = nullptr;

    CreatePipe(&hChildStdinR, &hChildStdinW, &sa, 0);
    SetHandleInformation(hChildStdinW, HANDLE_FLAG_INHERIT, 0);
    CreatePipe(&hChildStdoutR, &hChildStdoutW, &sa, 0);
    SetHandleInformation(hChildStdoutR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.hStdInput = hChildStdinR;
    si.hStdOutput = hChildStdoutW;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    std::string cmdLine = "\"" + exePath + "\"";
    BOOL ok = CreateProcessA(
        nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr,
        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);

    CloseHandle(hChildStdinR);
    CloseHandle(hChildStdoutW);

    if (!ok) {
      svc_log("ParakeetPipe: Failed to launch %s", exePath.c_str());
      CloseHandle(hChildStdinW);
      CloseHandle(hChildStdoutR);
      return false;
    }

    hProc = pi.hProcess;
    CloseHandle(pi.hThread);
    hStdin = hChildStdinW;
    hStdout = hChildStdoutR;
    ready = true;
    modelLoaded = false;
    readBuffer.clear();
    svc_log("ParakeetPipe: Engine launched (pid=%lu)", pi.dwProcessId);
    return true;
  }

  bool sendLine(const std::string &json) {
    if (!ready || !isProcessAlive()) {
      markDead("send while process dead");
      return false;
    }
    std::string msg = json + "\n";
    DWORD written = 0;
    if (!WriteFile(hStdin, msg.data(), (DWORD)msg.size(), &written, nullptr)) {
      markDead("WriteFile failed");
      return false;
    }
    return true;
  }

  // Non-blocking read of available output lines (poll-based for pipes)
  std::string tryReadLine(int timeoutMs = 0) {
    if (!ready)
      return "";
    if (!isProcessAlive()) {
      markDead("read while process dead");
      return "";
    }
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    DWORD avail = 0;
    // Poll until data available or timeout
    while (true) {
      if (!isProcessAlive()) {
        markDead("process exited during read poll");
        return "";
      }
      if (!PeekNamedPipe(hStdout, nullptr, 0, nullptr, &avail, nullptr)) {
        markDead("PeekNamedPipe failed (broken pipe)");
        return "";
      }
      if (avail > 0)
        break;
      if (std::chrono::steady_clock::now() >= deadline)
        return "";
      Sleep(10);
    }
    char buf[65536];
    DWORD bytesRead = 0;
    DWORD toRead = (avail < sizeof(buf) - 1) ? avail : sizeof(buf) - 1;
    if (!ReadFile(hStdout, buf, toRead, &bytesRead, nullptr) || bytesRead == 0) {
      markDead("ReadFile failed");
      return "";
    }
    readBuffer.append(buf, bytesRead);
    // Extract first complete line
    size_t nl = readBuffer.find('\n');
    if (nl == std::string::npos)
      return "";
    std::string line = readBuffer.substr(0, nl);
    readBuffer.erase(0, nl + 1);
    // Trim \r
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    return line;
  }

  void shutdown() {
    if (!ready && !hProc)
      return;
    if (ready && isProcessAlive()) {
      // Best-effort graceful quit; ignore failures.
      std::string msg = "{\"action\":\"quit\"}\n";
      DWORD written = 0;
      if (hStdin)
        WriteFile(hStdin, msg.data(), (DWORD)msg.size(), &written, nullptr);
      WaitForSingleObject(hProc, 2000);
    }
    markDead("shutdown");
  }

  // Offload model from memory but keep process alive for fast reload
  bool offloadModel() {
    if (!ready || !isProcessAlive()) {
      markDead("offload while process dead");
      return true; // already unloaded from our POV
    }
    if (!modelLoaded)
      return true;
    if (!sendLine("{\"action\":\"unload\"}"))
      return true; // process gone → effectively unloaded
    std::string resp = tryReadLine(3000);
    if (resp.find("\"ok\"") != std::string::npos) {
      modelLoaded = false;
      return true;
    }
    // If the pipe died mid-unload, treat as unloaded so the next start reloads.
    if (!ready) {
      return true;
    }
    // No ack — still clear the flag so we do not keep believing RAM is held.
    // A subsequent load will re-send the model into the worker.
    svc_log("ParakeetPipe: unload ack missing; clearing modelLoaded flag");
    modelLoaded = false;
    return true;
  }

  // Reload model into the running engine process
  bool loadModel(const std::string &modelPath) {
    if (!ready || !isProcessAlive()) {
      markDead("load while process dead");
      return false;
    }
    if (modelLoaded)
      return true;
    std::string json =
        "{\"action\":\"load\",\"model_path\":\"" + modelPath + "\"}";
    if (!sendLine(json))
      return false;
    // Nemotron 0.6B GGUF can take well over 15s on first Vulkan load.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    while (std::chrono::steady_clock::now() < deadline) {
      std::string line = tryReadLine(500);
      if (!line.empty()) {
        if (line.find("\"ok\"") != std::string::npos) {
          modelLoaded = true;
          return true;
        } else if (line.find("\"error\"") != std::string::npos) {
          return false;
        }
      }
      if (!ready)
        return false;
    }
    return false;
  }
};
#else // Linux / POSIX ParakeetPipe — uses pipe/fork/poll instead of Win32 handles
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
struct ParakeetPipe {
  pid_t pid = -1;
  int fd_stdin = -1;
  int fd_stdout = -1;
  void* hProc = nullptr; // Win32 parity member; unused on POSIX
  bool ready = false;
  bool modelLoaded = false;
  std::string readBuffer;
  std::string lastExePath;
  std::string lastWorkDir;
  bool isProcessAlive() const { if(pid<=0) return false; return ::kill(pid,0)==0; }
  void markDead(const char* reason){
    if(!ready && pid<=0) return;
    svc_log("ParakeetPipe: marking dead (%s)", reason?reason:"unknown");
    if(fd_stdin>=0){ ::close(fd_stdin); fd_stdin=-1; }
    if(fd_stdout>=0){ ::close(fd_stdout); fd_stdout=-1; }
    if(pid>0){ if(isProcessAlive()) ::kill(pid,SIGTERM); ::waitpid(pid,nullptr,WNOHANG); pid=-1; }
    ready=false; modelLoaded=false; readBuffer.clear();
  }
  bool launch(const std::string& exePath, const std::string& workDir){
    if(ready && !isProcessAlive()) markDead("stale process before relaunch");
    if(ready) return true;
    lastExePath=exePath; lastWorkDir=workDir;
    int pin[2], pout[2];
    if(::pipe(pin)!=0 || ::pipe(pout)!=0) { svc_log("pipe failed"); return false; }
    // make read end non-blocking
    ::fcntl(pout[0], F_SETFL, O_NONBLOCK);
    pid = ::fork();
    if(pid<0){ ::close(pin[0]); ::close(pin[1]); ::close(pout[0]); ::close(pout[1]); return false; }
    if(pid==0){
      ::dup2(pin[0], STDIN_FILENO);
      ::dup2(pout[1], STDOUT_FILENO);
      ::dup2(::open("/dev/null", O_WRONLY), STDERR_FILENO);
      ::close(pin[0]); ::close(pin[1]); ::close(pout[0]); ::close(pout[1]);
      if(!workDir.empty()) ::chdir(workDir.c_str());
      ::execl(exePath.c_str(), exePath.c_str(), (char*)nullptr);
      _exit(127);
    }
    ::close(pin[0]); ::close(pout[1]);
    fd_stdin=pin[1]; fd_stdout=pout[0];
    ready=true; modelLoaded=false; readBuffer.clear();
    svc_log("ParakeetPipe: Engine launched (pid=%d)", (int)pid);
    return true;
  }
  bool sendLine(const std::string& json){
    if(!ready || !isProcessAlive()){ markDead("send while dead"); return false; }
    std::string msg=json+"\n";
    ssize_t n=::write(fd_stdin, msg.data(), msg.size());
    if(n!=(ssize_t)msg.size()){ markDead("write failed"); return false; }
    return true;
  }
  std::string tryReadLine(int timeoutMs=0){
    if(!ready) return "";
    if(!isProcessAlive()){ markDead("read while dead"); return ""; }
    auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
    while(true){
      if(!isProcessAlive()){ markDead("process exited during poll"); return ""; }
      struct pollfd pfd{fd_stdout, POLLIN, 0};
      int pret = ::poll(&pfd,1,10);
      if(pret>0 && (pfd.revents&POLLIN)){
        char buf[65536];
        ssize_t r=::read(fd_stdout, buf, sizeof(buf)-1);
        if(r<=0){ markDead("read failed"); return ""; }
        readBuffer.append(buf,r);
        size_t nl=readBuffer.find('\n');
        if(nl!=std::string::npos){
          std::string line=readBuffer.substr(0,nl);
          readBuffer.erase(0,nl+1);
          while(!line.empty() && (line.back()=='\r' || line.back()=='\n')) line.pop_back();
          return line;
        }
      }
      if(std::chrono::steady_clock::now()>=deadline) return "";
    }
  }
  void shutdown(){
    if(!ready && pid<=0) return;
    if(ready && isProcessAlive()){
      std::string msg="{\"action\":\"quit\"}\n";
      ::write(fd_stdin, msg.data(), msg.size());
      for(int i=0;i<20;i++){ if(!isProcessAlive()) break; usleep(100*1000); }
    }
    markDead("shutdown");
  }
  bool offloadModel(){
    if(!ready || !isProcessAlive()){ markDead("offload while dead"); return true; }
    if(!modelLoaded) return true;
    if(!sendLine("{\"action\":\"unload\"}")) return true;
    std::string resp=tryReadLine(3000);
    if(resp.find("\"ok\"")!=std::string::npos){ modelLoaded=false; return true; }
    if(!ready) return true;
    svc_log("ParakeetPipe: unload ack missing; clearing flag");
    modelLoaded=false; return true;
  }
  bool loadModel(const std::string& modelPath){
    if(!ready || !isProcessAlive()){ markDead("load while dead"); return false; }
    if(modelLoaded) return true;
    std::string json="{\"action\":\"load\",\"model_path\":\""+modelPath+"\"}";
    if(!sendLine(json)) return false;
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(180);
    while(std::chrono::steady_clock::now()<deadline){
      std::string line=tryReadLine(500);
      if(!line.empty()){
        if(line.find("\"ok\"")!=std::string::npos){ modelLoaded=true; return true; }
        else if(line.find("\"error\"")!=std::string::npos) return false;
      }
      if(!ready) return false;
    }
    return false;
  }
};
#endif

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
// svc_log uses <cstdarg> (included below via the standard headers block)

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
  // Minimum-time default: suspend the model as soon as it is safe (no active
  // turn / popup / stream / inference). The engine-loop guards make delay=0
  // stable; users can raise it via Dashboard -> OFFLOADDELAY.
  int autoOffloadDelaySec = 0;
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

    // Auto-offload settings (stored as REG_SZ or REG_DWORD by Qt)
    sz = sizeof(buf);
    type = 0;
    if (RegQueryValueExA(key, "autoOffload", nullptr, &type, (BYTE *)buf,
                         &sz) == ERROR_SUCCESS) {
      if (type == REG_SZ) {
        buf[sz] = 0;
        std::string val(buf);
        s.autoOffloadEnabled = (val == "true" || val == "1");
      } else if (type == REG_DWORD) {
        s.autoOffloadEnabled = (*(DWORD *)buf) != 0;
      }
    }

    bool hasSecondsSetting = false;
    sz = sizeof(buf);
    type = 0;
    if (RegQueryValueExA(key, "offloadSeconds", nullptr, &type,
                         (BYTE *)buf, &sz) == ERROR_SUCCESS) {
      int seconds = 15;
      if (type == REG_SZ) {
        buf[sz] = 0;
        try {
          seconds = std::stoi(buf);
          hasSecondsSetting = true;
        } catch (...) {
        }
      } else if (type == REG_DWORD) {
        seconds = (*(DWORD *)buf);
        hasSecondsSetting = true;
      }
      if (hasSecondsSetting) {
        s.autoOffloadDelaySec = seconds;
      }
    }
    if (!hasSecondsSetting) {
      sz = sizeof(buf);
      type = 0;
      if (RegQueryValueExA(key, "offloadMinutes", nullptr, &type,
                           (BYTE *)buf, &sz) == ERROR_SUCCESS) {
        int minutes = 3;
        if (type == REG_SZ) {
          buf[sz] = 0;
          try {
            minutes = std::stoi(buf);
          } catch (...) {
          }
        } else if (type == REG_DWORD) {
          minutes = (*(DWORD *)buf);
        }
        s.autoOffloadDelaySec = minutes * 60;
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

    svc_log("Settings: autoOffload=%s, offloadDelay=%d sec",
            s.autoOffloadEnabled ? "true" : "false", s.autoOffloadDelaySec);
    RegCloseKey(key);
  }
  return s;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Path helpers
// ═══════════════════════════════════════════════════════════════════════════════

static std::string getExeDir() {
#ifdef _WIN32
  char buf[MAX_PATH];
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string path(buf);
  size_t pos = path.find_last_of("\\/");
  return (pos != std::string::npos) ? path.substr(0, pos) : ".";
#else
  char buf[4096]{};
  ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf)-1);
  if (len > 0) { buf[len]='\0'; std::string path(buf); size_t pos = path.find_last_of('/'); if(pos!=std::string::npos) return path.substr(0,pos); }
  char cwd[4096]{}; if(::getcwd(cwd,sizeof(cwd))) return std::string(cwd);
  return ".";
#endif
}

static std::string getAppDataDir() {
  std::string root = getExeDir();
#ifdef _WIN32
  std::string local_data = root + "\\data";
  if (fs::exists(local_data)) return local_data;
  char *appdata = getenv("APPDATA");
  if (appdata) { std::string dir = std::string(appdata) + "\\QuickSTT"; fs::create_directories(dir); return dir; }
  return root;
#else
  std::string local_data = (fs::path(root) / "data").string();
  if (fs::exists(local_data)) return local_data;
  const char* xdg = getenv("XDG_DATA_HOME");
  std::string base;
  if (xdg && *xdg) base = std::string(xdg) + "/QuickSTT";
  else {
    const char* home = getenv("HOME");
    if (home) base = std::string(home) + "/.local/share/QuickSTT";
    else base = root;
  }
  fs::create_directories(base);
  return base;
#endif
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

  // Normalize requested engine name
  std::string engine = engine_name;
  std::transform(engine.begin(), engine.end(), engine.begin(), ::tolower);

  // Extract keywords we care about from the engine name
  std::vector<std::string> requested_keywords;
  // Size/Quality qualifiers
  if (engine.find("small") != std::string::npos) requested_keywords.push_back("small");
  else if (engine.find("large") != std::string::npos) requested_keywords.push_back("large");

  // Language codes/qualifiers
  if (engine.find("en") != std::string::npos || engine.find("english") != std::string::npos) requested_keywords.push_back("en");
  if (engine.find("cn") != std::string::npos || engine.find("chinese") != std::string::npos) requested_keywords.push_back("cn");
  if (engine.find("ru") != std::string::npos || engine.find("russian") != std::string::npos) requested_keywords.push_back("ru");
  if (engine.find("fr") != std::string::npos || engine.find("french") != std::string::npos) requested_keywords.push_back("fr");
  if (engine.find("de") != std::string::npos || engine.find("german") != std::string::npos) requested_keywords.push_back("de");
  if (engine.find("es") != std::string::npos || engine.find("spanish") != std::string::npos) requested_keywords.push_back("es");
  if (engine.find("pt") != std::string::npos || engine.find("portuguese") != std::string::npos) requested_keywords.push_back("pt");
  if (engine.find("it") != std::string::npos || engine.find("italian") != std::string::npos) requested_keywords.push_back("it");
  if (engine.find("ja") != std::string::npos || engine.find("japanese") != std::string::npos) requested_keywords.push_back("ja");
  if (engine.find("indian") != std::string::npos || engine.find("in") != std::string::npos) {
    requested_keywords.push_back("in");
  }

  // Iterate over directories
  for (auto &entry : fs::directory_iterator(models_dir)) {
    if (!entry.is_directory())
      continue;
    std::string d = entry.path().filename().string();
    std::string dl = d;
    std::transform(dl.begin(), dl.end(), dl.begin(), ::tolower);

    // Tokenize directory name
    std::string tokenized = dl;
    for (char &ch : tokenized) {
      if (!std::isalnum(static_cast<unsigned char>(ch)))
        ch = ' ';
    }
    std::unordered_set<std::string> dir_tokens;
    std::istringstream tokenStream(tokenized);
    for (std::string token; tokenStream >> token;)
      dir_tokens.insert(token);

    // Special checks to distinguish small/large
    bool dir_is_small = dir_tokens.count("small") > 0;

    // Check if the directory matches all requested keywords
    bool all_matched = true;
    for (const auto &kw : requested_keywords) {
      if (kw == "small") {
        if (!dir_is_small) all_matched = false;
      } else if (kw == "large") {
        if (dir_is_small) all_matched = false;
      } else {
        // Match language/qualifier token (either exact match or substring)
        bool kw_found = false;
        for (const auto &t : dir_tokens) {
          if (t.find(kw) != std::string::npos || kw.find(t) != std::string::npos) {
            kw_found = true;
            break;
          }
        }
        if (!kw_found) all_matched = false;
      }
      if (!all_matched) break;
    }

    if (all_matched && !requested_keywords.empty()) {
      return entry.path().string();
    }
  }

  // Final fallback: if no robust match, check if folder name itself matches part of engine name
  for (auto &entry : fs::directory_iterator(models_dir)) {
    if (!entry.is_directory())
      continue;
    std::string dl = entry.path().filename().string();
    std::transform(dl.begin(), dl.end(), dl.begin(), ::tolower);
    if (engine.find(dl) != std::string::npos || dl.find(engine) != std::string::npos) {
      return entry.path().string();
    }
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

  char *appdata = getenv("APPDATA");
  if (appdata) {
    candidates.push_back(std::string(appdata) + "\\QuickSTT\\models\\oww_models");
    candidates.push_back(std::string(appdata) + "\\QuickSTT\\models\\openwakeword\\resources\\models");
  }

  char *userprofile = getenv("USERPROFILE");
  if (userprofile) {
    std::string up(userprofile);
    candidates.push_back(up + "\\AppData\\Local\\Programs\\Python\\Python311\\Lib\\site-packages\\openwakeword\\resources\\models");
    candidates.push_back(up + "\\AppData\\Local\\Programs\\Python\\Python312\\Lib\\site-packages\\openwakeword\\resources\\models");
    candidates.push_back(up + "\\AppData\\Local\\Programs\\Python\\Python310\\Lib\\site-packages\\openwakeword\\resources\\models");
  }

  for (auto &d : candidates) {
    if (fs::exists(d + "\\melspectrogram.tflite") &&
        fs::exists(d + "\\embedding_model.tflite"))
      return d;
  }
  // Fallback: check if directory exists containing any .onnx wake model
  for (auto &d : candidates) {
    if (fs::exists(d + "\\agent.onnx") || fs::exists(d + "\\hem.onnx") ||
        fs::exists(d + "\\jarvis.onnx") || fs::exists(d + "\\alexa.onnx"))
      return d;
  }
  return "";
}

static std::string findOrtDll() {
  std::string root = getExeDir();
#ifdef _WIN32
  std::vector<std::string> candidates = {
      (fs::path(root) / "onnxruntime.dll").string(),
      (fs::path(root) / "tools" / "nemotron" / "onnxruntime.dll").string(),
      (fs::path(root) / "tools" / "parakeet" / "onnxruntime.dll").string(),
  };
#else
  std::vector<std::string> candidates = {
      (fs::path(root) / "libonnxruntime.so").string(),
      (fs::path(root) / "tools" / "nemotron" / "libonnxruntime.so").string(),
      (fs::path(root) / "tools" / "parakeet" / "libonnxruntime.so").string(),
      (fs::path(root) / "libonnxruntime.so.1").string(),
      "/usr/lib/libonnxruntime.so",
      "/usr/local/lib/libonnxruntime.so",
  };
#endif
  for (auto &f : candidates) if (fs::exists(f)) return f;
  return "";
}

static std::string findPVModelsDir() {
  std::string root = getExeDir();
  std::vector<std::string> candidates = {
      (fs::path(root) / "porcupine_native").string(),
      (fs::path(root) / "_internal" / "porcupine_native").string(),
      (fs::path(root) / "data" / "porcupine_native").string(),
  };
  for (auto &d : candidates) {
    if (fs::exists(fs::path(d) / "porcupine_params.pv"))
      return d;
  }
  return "";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Audio Capture & Normalization Pipeline — Spec Implementation
// ═══════════════════════════════════════════════════════════════════════════════

static void setWindowsMicVolumeBaseline(float levelScalar = 0.75f) {
#ifdef _WIN32
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  IMMDeviceEnumerator *pEnumerator = NULL;
  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER,
                        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
  if (SUCCEEDED(hr) && pEnumerator) {
    IMMDevice *pDevice = NULL;
    hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);
    if (SUCCEEDED(hr) && pDevice) {
      IAudioEndpointVolume *pEndpointVolume = NULL;
      hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL, (void**)&pEndpointVolume);
      if (SUCCEEDED(hr) && pEndpointVolume) {
        pEndpointVolume->SetMasterVolumeLevelScalar(levelScalar, NULL);
        svc_log("Windows microphone baseline volume set to %.2f (75%%)", levelScalar);
        pEndpointVolume->Release();
      }
      pDevice->Release();
    }
    pEnumerator->Release();
  }
#endif
}

// Software Normalization AGC (Automatic Gain Control)
// Frame size: 20 ms (320 samples @ 16 kHz)
// Target level: -20 dBFS RMS
// Attack time: 10 ms (exp(-1.0 / 160.0))
// Release time: 300 ms (exp(-1.0 / 4800.0))
// Hard ceiling: -3 dBFS (soft clip saturating tanh curve)
// Max applied gain: +30 dB, Min applied gain: -20 dB
struct SoftwareAGC {
  double gain_db = 0.0;
  static constexpr double TARGET_DBFS = -20.0;
  static constexpr double ATTACK_MS = 10.0;
  static constexpr double RELEASE_MS = 300.0;
  static constexpr double MIN_GAIN_DB = -20.0;
  static constexpr double MAX_GAIN_DB = 20.0;  // 10x max — prevents noise amplification
  static constexpr double CEILING_DBFS = -3.0;
  static constexpr double NOISE_GATE_DBFS = -55.0; // Skip AGC on near-silence

  static int16_t softClip(double sample, double ceilingLinear = 23196.0) {
    double absVal = std::abs(sample);
    if (absVal <= ceilingLinear) {
      return (int16_t)std::max(-32768.0, std::min(32767.0, sample));
    }
    double maxLinear = 32767.0;
    double range = maxLinear - ceilingLinear;
    if (range <= 0.1) return (int16_t)(sample > 0 ? maxLinear : -maxLinear);
    double normalizedOver = (absVal - ceilingLinear) / range;
    double compressed = ceilingLinear + range * std::tanh(normalizedOver);
    double out = (sample >= 0.0) ? compressed : -compressed;
    return (int16_t)std::max(-32768.0, std::min(32767.0, out));
  }

  void process(std::vector<int16_t> &samples) {
    if (samples.empty()) return;
    const size_t n = samples.size();
    double sumSq = 0.0;
    for (size_t i = 0; i < n; ++i) {
      double s = (double)samples[i];
      sumSq += s * s;
    }
    double rms = std::sqrt(sumSq / (double)n);
    if (rms < 1.0) rms = 1.0;
    double rms_dbfs = 20.0 * std::log10(rms / 32768.0);

    // Noise gate: don't amplify near-silence (prevents phantom wakewords)
    if (rms_dbfs < NOISE_GATE_DBFS) return;

    double error_db = TARGET_DBFS - rms_dbfs;

    double coeff;
    if (error_db < 0.0) { // Too loud -> Attack (fast 10ms)
      coeff = std::exp(-1.0 / (ATTACK_MS * 16.0));
    } else { // Too quiet -> Release (slow 300ms)
      coeff = std::exp(-1.0 / (RELEASE_MS * 16.0));
    }

    double target_gain = std::max(MIN_GAIN_DB, std::min(MAX_GAIN_DB, gain_db + error_db));
    gain_db = coeff * gain_db + (1.0 - coeff) * target_gain;

    double gain_linear = std::pow(10.0, gain_db / 20.0);
    double ceilingLinear = 32768.0 * std::pow(10.0, CEILING_DBFS / 20.0);

    for (size_t i = 0; i < n; ++i) {
      double scaled = (double)samples[i] * gain_linear;
      samples[i] = softClip(scaled, ceilingLinear);
    }
  }
};

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
    setWindowsMicVolumeBaseline(0.75f);
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
  std::atomic<bool> popupStartRequested{false};
  std::atomic<bool> popupStopRequested{false};
  // True while Ctrl+Space (or toggle) popup session is held open.
  // Suppresses VAD auto-finalize so the full hold is captured until POPUP_STOP.
  bool popupSessionHeld = false;

  // Config
  std::string activeEngine = "Vosk Small En";
  Settings settings;
  std::string modelsDir;
  std::string exeDir;

  // Vosk
  VoskAPI vosk;
  VoskModel *voskModel = nullptr;
  VoskRecognizer *voskRec = nullptr;
  bool voskAvailable = false;
  std::string activeModelPath;
  mutable std::recursive_mutex modelMutex;
  std::atomic<bool> loadingModel{false};

  // OWW (TFLite)
  TfLiteLoader tflLoader;
  TFLiteWakeWordDetector owwDetector;
  bool owwReady = false;

  // WakeWordNet (ONNX)
  OrtLoader ortLoader;
  WakeNetDetector wakeNetDetector;
  bool wakeNetReady = false;

  // PV
  NativePicovoiceDetector pvDetector;
  bool pvReady = false;
  std::vector<int16_t> pvChunkBuffer;

  // Audio
  AudioCapture audio;
  AudioPreprocessor preprocessor;
  SoftwareAGC agc; // Software normalization AGC (spec Section 3)

  // Timing
  std::chrono::steady_clock::time_point lastSpeechTime;
  std::chrono::steady_clock::time_point lastActivationTime;
  std::chrono::steady_clock::time_point cloudSettleUntil;
  double wakeSuppressedUntil = 0.0;
  int silenceLimitSec = 999999;  // Default: no auto-stop (only close words / button)
  std::atomic<bool> cloudTranscription{false};
  std::atomic<bool> cloudAwaitingFrontend{false};
  enum class FrontendSegmentationMode {
    Normal = 0,
    Balanced = 1,
    Fast = 2,
    Accurate = 3
  };
  std::atomic<int> frontendSegmentationMode{
      int(FrontendSegmentationMode::Normal)};
  std::vector<int16_t> cloudPreRollBuffer;
  std::vector<int16_t> cloudUtteranceBuffer;
  std::chrono::steady_clock::time_point cloudLastSpeechTime;
  std::chrono::steady_clock::time_point cloudCaptureStartedAt{};
  bool cloudSpeechStarted = false;

  // Wakeword hit counting
  std::unordered_map<std::string, int> owwHitCounts;
  int voskWakeHits = 0;
  // Single-token wake words score more reliably with 1 consecutive hit;
  // multi-word phrases still benefit from 2. Default lean sensitive so users
  // do not need to shout (threshold itself is also lowered below).
  int wakeHitRequirement = 1;
  bool voskFallbackRequired = true;
  bool frontendRequestedOffload = false;

  bool canOffloadVosk() const {
    if (voskFallbackRequired && !frontendRequestedOffload) return false;
    return true;
  }

  // OWW chunk accumulator
  std::vector<int16_t> owwChunkBuffer;

  // ── Hybrid Acoustic Detector (Clap & Snap Transient Analyzer) ──
  enum class AcousticEventType { None, Clap, Snap };

  struct HybridAcousticDetector {
    bool clapEnabled = false;
    bool snapEnabled = false;
    std::string clapAction = "disabled";   // "wakeword" | "closeword" | "disabled"
    std::string snapAction = "disabled";  // "wakeword" | "closeword" | "disabled"
    float sensitivity = 1.0f;              // 0.2 (strict) to 3.0 (hyper-sensitive)

    // Running state
    float noiseFloor = 15.0f;
    float prevRms = 0.0f;
    double lastTriggerTime = 0.0;
    int warmupFrames = 0;

    AcousticEventType processChunk(const int16_t* samples, size_t count) {
      if (count < 16) return AcousticEventType::None;

      double maxAbs = 0.0;
      double sumSq = 0.0;
      double diffSumSq = 0.0;
      int zeroCrossings = 0;

      for (size_t i = 0; i < count; ++i) {
        double val = std::abs((double)samples[i]);
        if (val > maxAbs) maxAbs = val;
        sumSq += (double)samples[i] * (double)samples[i];
        if (i > 0) {
          double d = (double)samples[i] - (double)samples[i - 1];
          diffSumSq += d * d;
          if ((samples[i] >= 0) != (samples[i - 1] >= 0))
            zeroCrossings++;
        }
      }

      double rms = std::sqrt(sumSq / count);
      double diffRms = std::sqrt(diffSumSq / std::max((size_t)1, count - 1));
      double zcrRate = (double)zeroCrossings / (double)count;
      double spectralRatio = diffRms / (rms + 0.1);

      // Warm-up: let the noise floor stabilize for ~5 frames (~150ms)
      ++warmupFrames;
      if (warmupFrames < 6) {
        noiseFloor = (float)rms;
        prevRms = (float)rms;
        return AcousticEventType::None;
      }

      // Adapt noise floor only during quiet frames
      if (rms < noiseFloor * 2.0 + 20.0) {
        noiseFloor = noiseFloor * 0.97f + (float)rms * 0.03f;
        if (noiseFloor < 3.0f) noiseFloor = 3.0f;
      }

      // Attack detection: how much louder is this frame vs the noise floor?
      double floor = std::max(10.0, (double)noiseFloor);
      double peakOverFloor = maxAbs / floor;
      double rmsOverFloor = rms / floor;
      double rmsJump = rms / (prevRms + 1.0);  // Frame-to-frame energy jump

      prevRms = (float)rms;

      // Sensitivity-adjusted thresholds
      double sens = std::max(0.1, (double)sensitivity);
      // At sensitivity=1.0: need peak 2.5x noise, rms 1.8x noise
      // At sensitivity=1.5: need peak 1.67x, rms 1.2x (very easy)
      double reqPeakRatio = 2.5 / sens;
      double reqRmsRatio  = 1.8 / sens;
      double reqAbsPeak   = 80.0 / sens;  // Absolute minimum

      // Debounce
      double now = std::chrono::duration<double>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      if (now - lastTriggerTime < 0.30) return AcousticEventType::None;

      // Must pass EITHER (peak spike) OR (rms jump from previous frame)
      bool peakTriggered = (peakOverFloor >= reqPeakRatio && maxAbs >= reqAbsPeak);
      bool jumpTriggered = (rmsJump >= 3.0 / sens && maxAbs >= reqAbsPeak);

      if (!peakTriggered && !jumpTriggered) {
        return AcousticEventType::None;
      }

      // Classify: Snap = high zero-crossing + high spectral ratio (crisp, short)
      //           Clap = lower ZCR, broader energy (flatter spectrum)
      AcousticEventType detected = AcousticEventType::None;
      if (zcrRate > 0.10 && spectralRatio > 0.45) {
        if (snapEnabled && snapAction != "disabled")
          detected = AcousticEventType::Snap;
        else if (clapEnabled && clapAction != "disabled")
          detected = AcousticEventType::Clap;  // Fallback to clap action
      } else {
        if (clapEnabled && clapAction != "disabled")
          detected = AcousticEventType::Clap;
        else if (snapEnabled && snapAction != "disabled")
          detected = AcousticEventType::Snap;  // Fallback to snap action
      }

      if (detected != AcousticEventType::None) {
        lastTriggerTime = now;
        svc_log("ACOUSTIC %s: peak=%.0f peakRatio=%.1f rmsRatio=%.1f rmsJump=%.1f zcr=%.2f spectral=%.2f floor=%.1f",
                detected == AcousticEventType::Snap ? "SNAP" : "CLAP",
                maxAbs, peakOverFloor, rmsOverFloor, rmsJump, zcrRate, spectralRatio, floor);
      }
      return detected;
    }
  } acousticDetector;

  // ── Direct STT worker pipe (Parakeet batch / Nemotron streaming) ──
  // Same ParakeetPipe process manager; the child exe + protocol actions differ.
  ParakeetPipe parakeetPipe;
  std::atomic<bool> parakeetDirectMode{false};
  bool parakeetPreferred = false;  // Remembers user wants direct worker even when offloaded
  bool streamingPreferred = false; // TRANSCRIBE_MODE:STREAMING → stream_feed partials
  bool streamSessionActive = false;
  std::string streamCommittedText;
  std::string streamLastPartial; // longest live partial — empty stream_end fallback
  std::string streamWorkerKind; // "parakeet" | "nemotron"
  uint64_t streamGeneration = 0; // bumps every stream_start; drops late finals
  std::vector<int16_t> streamFeedBatch; // coalesce ~160ms before stream_feed
  float streamPeakLevel = 0.f;   // max |pcm| this stream (hallucination gate)
  size_t streamSamplesFed = 0;   // PCM samples delivered this stream
  bool popupPreloadActive = false;  // When true, disables auto-offload (popup needs model always ready)
  std::vector<int16_t> parakeetUtteranceBuf;  // Accumulated speech PCM
  std::vector<int16_t> parakeetPreRollBuf;    // Pre-roll for context
  bool parakeetSpeechActive = false;
  std::chrono::steady_clock::time_point parakeetLastSpeechTime;
  std::chrono::steady_clock::time_point parakeetInferenceStart;
  std::atomic<bool> parakeetInferencePending{false};
  int parakeetConsecutiveSpeechFrames = 0;
  int parakeetConsecutiveSilenceFrames = 0;

  // VAD-gated wake word detection state
  int wakeVadSpeechFrames = 0;     // Sustained speech frame counter
  int wakeVadSilenceFrames = 0;    // Sustained silence frame counter
  std::chrono::steady_clock::time_point wakeVadLastSpeechTime{};
  // 1 frame is enough to open the gate for short wake phrases spoken at a
  // normal volume; the old value of 2 + high OWW threshold forced shouting.
  static constexpr int kWakeVadMinSpeechFrames = 1;
  // Keep accepting a model score briefly after speech ends. OpenWakeWord needs
  // the following context to score a short phrase such as "alexa" correctly.
  static constexpr auto kWakeVadDecisionHold = std::chrono::milliseconds(1800);

  STTEngine() {
    exeDir = getExeDir();
    modelsDir = (fs::path(getAppDataDir()) / "models").string();
    settings = loadSettings();
    lastSpeechTime = std::chrono::steady_clock::now();
    lastActivationTime = lastSpeechTime;
  }

  ~STTEngine() {
    stop();
    parakeetPipe.shutdown();
    offloadVoskModel();
    preprocessor.shutdown();
    owwDetector.cleanup();
    wakeNetDetector.cleanup();
    pvDetector.cleanup();
    tflLoader.unload();
    ortLoader.unload();
    vosk.unload();
  }

  bool init() {
#ifdef _WIN32
    std::string voskPath = (fs::path(exeDir) / "vosk" / "libvosk.dll").string();
    if (!fs::exists(voskPath)) voskPath = (fs::path(exeDir) / "libvosk.dll").string();
    const char* voskLabel="libvosk.dll";
#else
    // Search order: beside exe → exe/vosk → user data runtimes (Rust GUI layout)
    // → system paths. Missing libvosk is NOT fatal: Vosk models are disabled but
    // Parakeet / Nemotron direct pipelines keep working (prevents restart storms).
    std::vector<std::string> voskCandidates = {
        (fs::path(exeDir) / "libvosk.so").string(),
        (fs::path(exeDir) / "vosk" / "libvosk.so").string(),
        (fs::path(getAppDataDir()) / "runtimes" / "vosk" / "libvosk.so").string(),
        "/usr/lib/libvosk.so",
        "/usr/local/lib/libvosk.so",
        "/usr/lib/x86_64-linux-gnu/libvosk.so",
    };
    std::string voskPath;
    for (const auto &candidate : voskCandidates) {
      if (fs::exists(candidate)) { voskPath = candidate; break; }
    }
    const char* voskLabel="libvosk.so";
#endif
    if (!voskPath.empty() && vosk.load(voskPath)) {
      voskAvailable = true;
      vosk.set_log_level(-1);
      svc_log("%s loaded OK", voskLabel);
    } else {
      voskAvailable = false;
      svc_log("WARN: %s not found — Vosk models disabled; "
              "Parakeet/Nemotron pipelines remain available",
              voskLabel);
    }

    preprocessor.init(exeDir);
    svc_log("Audio preprocessing: RNNoise=%s TEN-VAD=%s",
            preprocessor.hasRnnoise() ? "on" : "off",
            preprocessor.hasTenVad() ? "on" : "off");

    if (tflLoader.load(exeDir)) {
#ifdef _WIN32
      svc_log("tensorflowlite_c.dll loaded OK");
#else
      svc_log("libtensorflowlite_c.so loaded OK");
#endif
    } else {
#ifdef _WIN32
      svc_log("tensorflowlite_c.dll not found — OWW wakeword detection disabled");
#else
      svc_log("libtensorflowlite_c.so not found — OWW wakeword detection disabled");
#endif
    }
    initWakeEngines();

    // Initialize Parakeet Direct Pipe (Handy-style: zero file I/O)
    if (initParakeetDirect()) {
      parakeetDirectMode = true;
      svc_log("Parakeet Direct Mode ENABLED — in-process inference, no file I/O");
    } else {
      svc_log("Parakeet Direct Mode unavailable — using legacy cloud/frontend path");
    }

    return true;
  }

  void initWakeEngines() {
    std::lock_guard<std::recursive_mutex> lock(modelMutex);
    voskFallbackRequired = false;
    // Only enable Vosk fallback if user explicitly chose Vosk wake engine
    if (settings.wakeEngine.find("Vosk") != std::string::npos) {
      voskFallbackRequired = true;
    }
    initOWW();
    initPV();
  }

  void initOWW() {
    std::string we = settings.wakeEngine;
    if (we.find("Porcupine") != std::string::npos || we.find("Vosk") != std::string::npos) {
      svc_log("Wake engine '%s' bypasses OWW pipeline", we.c_str());
      owwReady = false;
      return;
    }
    svc_log("Wake engine '%s' mapped to TFLite OWW pipeline", we.c_str());
    std::string owwDir = findOWWModelsDir();
    if (owwDir.empty()) {
      svc_log("OWW models directory not found — falling back to Vosk keyword spotting");
      owwReady = false;
      voskFallbackRequired = true;
      return;
    }
    // 0.25f provides high sensitivity to normal spoken volume.
    owwReady = owwDetector.init(tflLoader, owwDir, settings.wakeWords, 0.25f);

    // Initialize WakeWordNet ONNX detector for custom models (agent, hem, jarvis)
    std::string ortDll = findOrtDll();
    if (!ortDll.empty() && ortLoader.load(ortDll)) {
      wakeNetReady = wakeNetDetector.init(ortLoader, owwDir, settings.wakeWords, 0.25f);
      if (wakeNetReady) {
        svc_log("WakeWordNet ready with %d/%d wake models",
                (int)wakeNetDetector.wake_models_.size(), (int)settings.wakeWords.size());
      }
    }

    if (owwReady || wakeNetReady) {
       svc_log("OWW/WakeNet ready with %d + %d wake models",
               owwReady ? (int)owwDetector.wake_models_.size() : 0,
               wakeNetReady ? (int)wakeNetDetector.wake_models_.size() : 0);
    } else {
       svc_log("OWW/WakeNet init failed — falling back to Vosk keyword spotting");
       voskFallbackRequired = true;
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
    } else {
        svc_log("Picovoice Init Failed (Check Access Key or Net connection)");
        sendEvent("ERROR", "Picovoice Refused: Invalid Access Key");
    }
  }

  // Resolve which JSON-line worker exe to use for the current preference.
  void resolveDirectWorkerPaths(std::string *exeOut, std::string *workOut,
                                std::string *kindOut) const {
    const std::string nemoExe =
        exeDir + "\\tools\\nemotron\\nemotron_engine.exe";
    const std::string nemoDir = exeDir + "\\tools\\nemotron";
    const std::string paraExe =
        exeDir + "\\tools\\parakeet\\parakeet_engine.exe";
    const std::string paraDir = exeDir + "\\tools\\parakeet";
    if (streamingPreferred && fs::exists(nemoExe)) {
      *exeOut = nemoExe;
      *workOut = nemoDir;
      *kindOut = "nemotron";
      return;
    }
    *exeOut = paraExe;
    *workOut = paraDir;
    *kindOut = "parakeet";
  }

  // Ensure the worker process is alive; relaunch if the user killed it.
  // Does not load the model — callers decide load vs offload policy.
  bool ensureParakeetProcessAlive() {
    if (parakeetPipe.ready && parakeetPipe.isProcessAlive()) {
      // If preference switched (batch ↔ streaming) and a different exe is
      // required, recycle the process.
      std::string wantExe, wantDir, wantKind;
      resolveDirectWorkerPaths(&wantExe, &wantDir, &wantKind);
      if (!streamWorkerKind.empty() && streamWorkerKind != wantKind &&
          fs::exists(wantExe)) {
        svc_log("Direct worker kind switch %s -> %s; relaunching",
                streamWorkerKind.c_str(), wantKind.c_str());
        parakeetPipe.markDead("worker kind switch");
      } else {
        return true;
      }
    }
    if (parakeetPipe.ready || parakeetPipe.hProc)
      parakeetPipe.markDead("ensureParakeetProcessAlive");
    parakeetDirectMode = false;
    std::string pipeExe, pipeWorkDir, kind;
    resolveDirectWorkerPaths(&pipeExe, &pipeWorkDir, &kind);
    if (!fs::exists(pipeExe)) {
      svc_log("DirectPipe: engine not found at %s", pipeExe.c_str());
      return false;
    }
    streamWorkerKind = kind;
    return parakeetPipe.launch(pipeExe, pipeWorkDir);
  }

  // ── Direct Pipe: Handy-style zero-file-I/O inference (Parakeet / Nemotron) ──
  bool initParakeetDirect() {
    if (!ensureParakeetProcessAlive())
      return false;
    // Find model path for the active worker kind
    std::string modelPath = (streamWorkerKind == "nemotron")
                                ? findNemotronModelPath()
                                : findParakeetModelPath();
    if (modelPath.empty() && streamWorkerKind == "nemotron") {
      // Fall back to Parakeet weights only if Nemotron package is missing —
      // stream protocol still works once a real Nemotron package is installed.
      svc_log("DirectPipe: No Nemotron model dir; cannot load streaming worker");
      return false;
    }
    if (modelPath.empty()) {
      svc_log("ParakeetPipe: No Parakeet model found");
      return false;
    }
    if (parakeetPipe.modelLoaded)
      return true;
    // Send load command
    std::string loadJson = "{\"action\":\"load\",\"model_path\":\"" +
                           jsonEscape(modelPath) + "\"}";
    if (!parakeetPipe.sendLine(loadJson)) {
      // Process died mid-send — one retry after relaunch.
      if (!ensureParakeetProcessAlive() || !parakeetPipe.sendLine(loadJson)) {
        svc_log("ParakeetPipe: Failed to send load after relaunch");
        return false;
      }
    }
    // Nemotron 0.6B weights are ~2.3 GB — allow a long first load.
    const int loadTimeoutSec =
        (streamWorkerKind == "nemotron") ? 180 : 30;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(loadTimeoutSec);
    while (std::chrono::steady_clock::now() < deadline) {
      std::string line = parakeetPipe.tryReadLine(500);
      if (!line.empty()) {
        if (line.find("\"ok\"") != std::string::npos) {
          parakeetPipe.modelLoaded = true;
          svc_log("DirectPipe: Model loaded (%s) from %s",
                  streamWorkerKind.c_str(), modelPath.c_str());
          return true;
        } else if (line.find("\"error\"") != std::string::npos) {
          svc_log("DirectPipe: Model load failed: %s", line.c_str());
          return false;
        }
      }
      if (!parakeetPipe.ready) {
        svc_log("ParakeetPipe: Process died during model load");
        return false;
      }
    }
    svc_log("ParakeetPipe: Model load timed out");
    return false;
  }

  // Offload Parakeet model from memory (keeps engine process alive for fast reload)
  void offloadParakeetModel() {
    popupPreloadActive = false;
    streamSessionActive = false;
    streamCommittedText.clear();
    if (!parakeetPipe.ready || !parakeetPipe.isProcessAlive()) {
      if (parakeetPipe.ready || parakeetPipe.hProc)
        parakeetPipe.markDead("offload with dead process");
      parakeetDirectMode = false;
      parakeetSpeechActive = false;
      parakeetUtteranceBuf.clear();
      parakeetPreRollBuf.clear();
      if (parakeetPipe.modelLoaded) {
        // Already gone from RAM because the process died.
        parakeetPipe.modelLoaded = false;
        sendEvent("OFFLOADED", "Parakeet model offloaded");
      }
      return;
    }
    if (!parakeetPipe.modelLoaded)
      return;
    svc_log("ParakeetPipe: Offloading model to free RAM...");
    if (parakeetPipe.offloadModel()) {
      parakeetDirectMode = false;
      parakeetSpeechActive = false;
      parakeetUtteranceBuf.clear();
      parakeetPreRollBuf.clear();
      sendEvent("OFFLOADED", "Parakeet model offloaded");
      svc_log("ParakeetPipe: Model offloaded OK");
    } else {
      svc_log("ParakeetPipe: Model offload failed");
    }
  }

  // Reload direct worker model (Parakeet batch or Nemotron streaming).
  // Always resolve the correct worker kind + weights for the active preference.
  bool reloadParakeetModel() {
    std::string wantExe, wantDir, wantKind;
    resolveDirectWorkerPaths(&wantExe, &wantDir, &wantKind);

    if (parakeetPipe.ready && parakeetPipe.isProcessAlive() &&
        parakeetPipe.modelLoaded &&
        (streamWorkerKind.empty() || streamWorkerKind == wantKind)) {
      parakeetDirectMode = true;
      return true;
    }

    // Kind mismatch or dead process → full init (relaunch + correct weights).
    if (!parakeetPipe.ready || !parakeetPipe.isProcessAlive() ||
        (!streamWorkerKind.empty() && streamWorkerKind != wantKind)) {
      if (parakeetPipe.ready || parakeetPipe.hProc)
        parakeetPipe.markDead("reload kind/process mismatch");
      return initParakeetDirect() ? (parakeetDirectMode = true, true) : false;
    }

    // Process alive, model not loaded — load the right weights for this kind.
    std::string modelPath =
        (wantKind == "nemotron" || streamingPreferred)
            ? findNemotronModelPath()
            : findParakeetModelPath();
    if (modelPath.empty() && wantKind == "nemotron") {
      svc_log("DirectPipe: No Nemotron model found for reload");
      return false;
    }
    if (modelPath.empty()) {
      svc_log("ParakeetPipe: No model found for reload");
      return false;
    }
    streamWorkerKind = wantKind.empty() ? streamWorkerKind : wantKind;
    const char *loadingLabel =
        (streamWorkerKind == "nemotron") ? "Loading Nemotron..."
                                         : "Loading Parakeet...";
    svc_log("DirectPipe: Reloading %s model from %s", streamWorkerKind.c_str(),
            modelPath.c_str());
    sendEvent("STATE", std::string("3,") + loadingLabel);
    std::string escapedPath = jsonEscape(modelPath);
    if (parakeetPipe.loadModel(escapedPath)) {
      parakeetDirectMode = true;
      svc_log("DirectPipe: Model reloaded OK (%s)", streamWorkerKind.c_str());
      return true;
    }
    // Load failed on a live process — try a clean relaunch once (Handy-style).
    svc_log("DirectPipe: Model reload failed; attempting full relaunch");
    parakeetPipe.markDead("reload failed");
    if (initParakeetDirect()) {
      parakeetDirectMode = true;
      return true;
    }
    svc_log("DirectPipe: Model reload failed after relaunch");
    return false;
  }

  std::string findParakeetModelPath() {
    // Search for the ONNX Parakeet model directory (cross-platform joins)
    const fs::path dataDir = getAppDataDir();
    std::vector<std::string> candidates = {
        (dataDir / "models" / "nemo" / "tdt_0_6b_v3_int8").string(),
        (dataDir / "models" / "handy_parakeet").string(),
        (fs::path(exeDir) / "data" / "models" / "nemo" / "tdt_0_6b_v3_int8").string(),
        (fs::path(exeDir) / "models" / "nemo" / "tdt_0_6b_v3_int8").string(),
    };
    // Also check APPDATA
    char *appdata = getenv("APPDATA");
    if (appdata) {
      candidates.push_back((fs::path(appdata) / "QuickSTT" / "models" / "nemo" /
                            "tdt_0_6b_v3_int8").string());
      candidates.push_back(
          (fs::path(appdata) / "QuickSTT" / "models" / "handy_parakeet").string());
    }
    for (const auto &c : candidates) {
      if (fs::exists(c)) return c;
    }
    return "";
  }

  // Handy layout: models/nemotron/nemotron-3.5-asr-streaming-0.6b/*.gguf
  // Also accept a direct .gguf path or older speech_streaming_0_6b dirs.
  std::string findNemotronModelPath() {
    const fs::path dataDir = getAppDataDir();
    std::vector<std::string> candidates = {
        (dataDir / "models" / "nemotron" / "nemotron-3.5-asr-streaming-0.6b").string(),
        (dataDir / "models" / "nemotron").string(),
        (dataDir / "models" / "nemotron" / "speech_streaming_0_6b").string(),
        (fs::path(exeDir) / "data" / "models" / "nemotron" /
         "nemotron-3.5-asr-streaming-0.6b").string(),
        (fs::path(exeDir) / "models" / "nemotron" /
         "nemotron-3.5-asr-streaming-0.6b").string(),
        (fs::path(exeDir) / "models" / "nemotron").string(),
    };
    // LocalAppData is the primary QuickSTT models root on Windows.
    char *local = getenv("LOCALAPPDATA");
    if (local) {
      candidates.insert(
          candidates.begin(),
          (fs::path(local) / "QuickSTT" / "models" / "nemotron" /
           "nemotron-3.5-asr-streaming-0.6b").string());
      candidates.push_back(
          (fs::path(local) / "QuickSTT" / "models" / "nemotron").string());
    }
    char *appdata = getenv("APPDATA");
    if (appdata) {
      candidates.push_back(
          (fs::path(appdata) / "QuickSTT" / "models" / "nemotron" /
           "nemotron-3.5-asr-streaming-0.6b").string());
      candidates.push_back(
          (fs::path(appdata) / "QuickSTT" / "models" / "nemotron").string());
    }
    auto dirHasGguf = [](const std::string &dir) -> bool {
      if (!fs::exists(dir) || !fs::is_directory(dir))
        return false;
      for (auto &e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".gguf")
          return true;
      }
      return false;
    };
    for (const auto &c : candidates) {
      if (fs::is_regular_file(c) && fs::path(c).extension() == ".gguf")
        return c;
      if (dirHasGguf(c))
        return c;
    }
    return "";
  }

  static std::string jsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
      if (c == '\\') out += "\\\\";
      else if (c == '"') out += "\\\"";
      else out += c;
    }
    return out;
  }

  // Send accumulated PCM directly to Parakeet engine (no file I/O!)
  void emitParakeetDirect() {
    if (parakeetUtteranceBuf.empty()) return;
    // Popup PTT flushes the full hold — accept shorter clips (100ms).
    // Free dictation keeps a slightly higher floor to skip noise blips.
    const double minSec = popupSessionHeld ? 0.10 : 0.15;
    if (parakeetUtteranceBuf.size() < size_t(SAMPLE_RATE * minSec)) {
      svc_log("ParakeetPipe: utterance too short (%.0fms) — dropped",
              1000.0 * double(parakeetUtteranceBuf.size()) / double(SAMPLE_RATE));
      parakeetUtteranceBuf.clear();
      return;
    }
    // Worker may have been killed mid-session — try one reload before fallback.
    if ((!parakeetPipe.ready || !parakeetPipe.isProcessAlive() ||
         !parakeetPipe.modelLoaded) &&
        parakeetPreferred) {
      svc_log("ParakeetPipe: Not ready at emit — attempting reload");
      reloadParakeetModel();
    }
    if (!parakeetPipe.ready || !parakeetPipe.isProcessAlive() ||
        !parakeetPipe.modelLoaded) {
      svc_log("ParakeetPipe: Not ready, falling back to cloud path");
      // Fall back to the old cloud/file path
      cloudUtteranceBuffer = parakeetUtteranceBuf;
      cloudCaptureStartedAt = std::chrono::steady_clock::now();
      emitCloudUtterance();
      parakeetUtteranceBuf.clear();
      return;
    }

    // Encode i16 PCM as base64 and send directly
    std::string b64 = base64Encode(
        reinterpret_cast<const uint8_t *>(parakeetUtteranceBuf.data()),
        parakeetUtteranceBuf.size() * sizeof(int16_t));

    const double audioMs = (1000.0 * double(parakeetUtteranceBuf.size())) / double(SAMPLE_RATE);
    svc_log("ParakeetPipe: Sending %.0fms audio (%zu samples, %zu b64 chars)",
            audioMs, parakeetUtteranceBuf.size(), b64.size());

    std::string json = "{\"action\":\"transcribe_pcm\",\"pcm_i16_b64\":\"" + b64 + "\"}";
    if (!parakeetPipe.sendLine(json)) {
      svc_log("ParakeetPipe: send failed (worker dead) — falling back to cloud path");
      cloudUtteranceBuffer = parakeetUtteranceBuf;
      cloudCaptureStartedAt = std::chrono::steady_clock::now();
      emitCloudUtterance();
      parakeetUtteranceBuf.clear();
      return;
    }
    parakeetInferencePending = true;
    parakeetInferenceStart = std::chrono::steady_clock::now();
    parakeetUtteranceBuf.clear();
    sendEvent("STATE", "2,Transcribing...");
  }

  void emitPostTranscriptionState() {
    sendEvent("STATE", mode == EngineMode::ACTIVE ? "1,Listening..."
                                                    : "0,Ready");
  }

  // After a popup/push-to-talk turn ends, allow auto-offload again.
  // popupPreloadActive previously froze the model in RAM forever.
  void endPopupWarmHold() {
    if (!popupPreloadActive)
      return;
    popupPreloadActive = false;
    svc_log("ParakeetPipe: popup warm-hold cleared; auto-offload re-enabled");
    // Reset idle clock so the configured offload delay starts from now.
    lastSpeechTime = std::chrono::steady_clock::now();
  }

  // Drop any unread worker lines so a new stream_start/end is not confused
  // with a leftover partial (was a source of stale "Okay" finals).
  void drainStreamWorkerLines(int maxLines = 64) {
    for (int i = 0; i < maxLines; ++i) {
      std::string line = parakeetPipe.tryReadLine(0);
      if (line.empty())
        break;
    }
  }

  void resetStreamCaptureState() {
    streamCommittedText.clear();
    streamLastPartial.clear();
    streamFeedBatch.clear();
    streamPeakLevel = 0.f;
    streamSamplesFed = 0;
  }

  // True when the session had almost no voice energy — Nemotron often emits
  // filler on pure silence; never paste those.
  bool streamLikelyHallucination(const std::string &text) const {
    if (text.empty())
      return true;
    // < ~0.20s of audio or near-silent peak → treat as non-speech.
    if (streamSamplesFed < size_t(SAMPLE_RATE * 0.20) || streamPeakLevel < 0.008f)
      return true;
    // If voice energy is present (>0.02), allow natural words through!
    if (streamPeakLevel >= 0.02f)
      return false;
    std::string lower = lowercaseCopy(text);
    while (!lower.empty() &&
           (lower.back() == '.' || lower.back() == '!' || lower.back() == '?' ||
            lower.back() == ' '))
      lower.pop_back();
    static const char *kFillers[] = {
        "amen", nullptr};
    for (int i = 0; kFillers[i]; ++i) {
      if (lower == kFillers[i])
        return true;
    }
    return false;
  }

  bool beginStreamSession() {
    if (!streamingPreferred)
      return false;
    if ((!parakeetPipe.ready || !parakeetPipe.isProcessAlive() ||
         !parakeetPipe.modelLoaded) &&
        parakeetPreferred) {
      reloadParakeetModel();
    }
    if (!parakeetPipe.ready || !parakeetPipe.isProcessAlive() ||
         !parakeetPipe.modelLoaded) {
      svc_log("Stream: worker not ready for stream_start");
      return false;
    }

    // If a previous stream was left open (offload race, crashed stop), force
    // end it so the decoder does not keep the prior transcript.
    if (streamSessionActive) {
      svc_log("Stream: forcing end of leftover session before restart");
      (void)parakeetPipe.sendLine("{\"action\":\"stream_end\"}");
      drainStreamWorkerLines(32);
      streamSessionActive = false;
    } else {
      // Still drain any stale partials sitting in the pipe.
      drainStreamWorkerLines(16);
    }

    resetStreamCaptureState();
    ++streamGeneration;
    const uint64_t gen = streamGeneration;

    // latency_mode 6 → att_context_right=6 (balanced streaming / Handy Live feel).
    if (!parakeetPipe.sendLine(
            "{\"action\":\"stream_start\",\"latency_mode\":6}")) {
      svc_log("Stream: stream_start send failed");
      return false;
    }
    // Read start acknowledgment with 1000ms deadline.
    std::string ack = parakeetPipe.tryReadLine(1000);
    if (!ack.empty() && ack.find("\"error\"") != std::string::npos) {
      svc_log("Stream: stream_start error: %s", ack.c_str());
      return false;
    }
    // Generation may have advanced if stop raced; only arm if still current.
    if (gen != streamGeneration)
      return false;
    streamSessionActive = true;
    sendEvent("MODEL_CAP", "streaming=1");
    svc_log("Stream: session started (%s) gen=%llu", streamWorkerKind.c_str(),
            (unsigned long long)gen);
    return true;
  }

  void flushStreamFeedBatch(bool force) {
    if (!streamSessionActive || streamFeedBatch.empty())
      return;
    // ~160ms batches (2560 samples @ 16 kHz) cut IPC overhead vs every 20ms
    // frame — big win on Nemotron/Vulkan. Force-flush on session end.
    constexpr size_t kBatchSamples = 2560;
    if (!force && streamFeedBatch.size() < kBatchSamples)
      return;
    if (!parakeetPipe.ready || !parakeetPipe.isProcessAlive()) {
      svc_log("Stream: worker died mid-session");
      streamSessionActive = false;
      parakeetPipe.markDead("stream mid-session");
      streamFeedBatch.clear();
      return;
    }
    std::string b64 = base64Encode(
        reinterpret_cast<const uint8_t *>(streamFeedBatch.data()),
        streamFeedBatch.size() * sizeof(int16_t));
    streamSamplesFed += streamFeedBatch.size();
    streamFeedBatch.clear();
    std::string json =
        "{\"action\":\"stream_feed\",\"pcm_i16_b64\":\"" + b64 + "\"}";
    if (!parakeetPipe.sendLine(json)) {
      svc_log("Stream: stream_feed send failed");
      streamSessionActive = false;
      return;
    }
    // Non-blocking drain of ready partials (do not block audio loop).
    for (int i = 0; i < 32; ++i) {
      std::string line = parakeetPipe.tryReadLine(0);
      if (line.empty())
        break;
      handleStreamWorkerLine(line, /*isFinal=*/false);
    }
  }

  // Feed one mic chunk into the streaming worker and forward Live partials.
  void processStreamingChunk(const std::vector<int16_t> &chunk) {
    if (!streamSessionActive || chunk.empty())
      return;
    if (!parakeetPipe.ready || !parakeetPipe.isProcessAlive()) {
      svc_log("Stream: worker died mid-session");
      streamSessionActive = false;
      parakeetPipe.markDead("stream mid-session");
      return;
    }
    lastSpeechTime = std::chrono::steady_clock::now();
    // Track peak level for hallucination gating on finalize.
    for (int16_t s : chunk) {
      float a = std::fabs(float(s) / 32768.f);
      if (a > streamPeakLevel)
        streamPeakLevel = a;
    }
    streamFeedBatch.insert(streamFeedBatch.end(), chunk.begin(), chunk.end());
    flushStreamFeedBatch(/*force=*/false);
  }

  void handleStreamWorkerLine(const std::string &line, bool isFinal) {
    std::string err = jsonGetString(line.c_str(), "error");
    if (!err.empty()) {
      svc_log("Stream worker error: %s", err.c_str());
      return;
    }
    std::string text = jsonGetString(line.c_str(), "text");
    std::string committed = jsonGetString(line.c_str(), "committed");
    std::string partial = jsonGetString(line.c_str(), "partial");
    if (partial.empty())
      partial = jsonGetString(line.c_str(), "tentative");

    if (!committed.empty())
      streamCommittedText = committed;
    else if (!text.empty() && isFinal)
      streamCommittedText = text;

    if (!isFinal) {
      // Handy Live: committed|tentative
      const std::string tent =
          !partial.empty() ? partial
                           : (!text.empty() && text != streamCommittedText
                                  ? text
                                  : std::string());
      // Recovery buffer must be the FULL live string (committed + tentative),
      // not just the short tail — empty stream_end is common on short turns.
      std::string liveFull = streamCommittedText;
      if (!tent.empty()) {
        if (!liveFull.empty() && liveFull.back() != ' ')
          liveFull.push_back(' ');
        liveFull += tent;
      }
      if (!text.empty() && text.size() >= liveFull.size())
        liveFull = text;
      if (liveFull.size() >= streamLastPartial.size())
        streamLastPartial = liveFull;
      else if (!tent.empty() && tent.size() > streamLastPartial.size())
        streamLastPartial = tent;
      // Don't surface silence-filler commits to the UI/typer early — they were
      // getting pasted as repeated "Okay" across turns.
      if (streamLikelyHallucination(liveFull) && streamSamplesFed < size_t(SAMPLE_RATE)) {
        return;
      }
      sendEvent("STREAM_TEXT", streamCommittedText + "|" + tent);
      if (!tent.empty())
        sendEvent("PARTIAL_TEXT", tent);
      return;
    }

    // Final utterance — prefer explicit final, then committed, then last partial.
    std::string finalText =
        !text.empty() ? text
                      : (!streamCommittedText.empty()
                             ? streamCommittedText
                             : (!committed.empty()
                                    ? committed
                                    : (!partial.empty() ? partial
                                                       : streamLastPartial)));
    if (!partial.empty() && finalText.find(partial) == std::string::npos &&
        partial.size() > finalText.size()) {
      // Worker sometimes returns a short "final" while the longer partial is
      // the real utterance — keep the longer string.
      finalText = partial;
    }
    if (finalText.empty() && !streamLastPartial.empty())
      finalText = streamLastPartial;
    // Prefer the longer of committed+live vs short model final.
    if (!streamLastPartial.empty() &&
        streamLastPartial.size() > finalText.size() + 3)
      finalText = streamLastPartial;
    // Trim
    while (!finalText.empty() &&
           (finalText.back() == ' ' || finalText.back() == '\n'))
      finalText.pop_back();
    while (!finalText.empty() &&
           (finalText.front() == ' ' || finalText.front() == '\n'))
      finalText.erase(finalText.begin());

    if (!finalText.empty() && streamLikelyHallucination(finalText)) {
      svc_log("Stream FINAL suppressed hallucination '%s' (peak=%.3f samples=%zu)",
              finalText.c_str(), streamPeakLevel, streamSamplesFed);
      finalText.clear();
    }

    if (!finalText.empty()) {
      svc_log("Stream FINAL '%s' (peak=%.3f samples=%zu)", finalText.c_str(),
              streamPeakLevel, streamSamplesFed);
      streamCommittedText = finalText;
      sendEvent("STREAM_TEXT", finalText + "|");
      sendEvent("FINAL_TEXT", finalText);
    } else {
      svc_log("Stream FINAL (empty)");
      // Explicit empty so the popup exits Transcribing without reusing stale text.
      sendEvent("FINAL_TEXT", "");
    }
  }

  // End streaming session (popup release / mic off). Returns true if handled.
  bool endStreamSession() {
    if (!streamSessionActive && !streamingPreferred)
      return false;
    if (!streamSessionActive) {
      // Never got stream_start — nothing to finalize.
      resetStreamCaptureState();
      sendEvent("FINAL_TEXT", "");
      return streamingPreferred;
    }
    // Flush any coalesced audio still sitting in the batch buffer first.
    flushStreamFeedBatch(/*force=*/true);

    streamSessionActive = false;
    ++streamGeneration; // invalidate any in-flight partial handlers
    const float peak = streamPeakLevel;
    const size_t fed = streamSamplesFed;

    auto emitFallbackOrEmpty = [&](const char *why) {
      std::string fallback = !streamCommittedText.empty()
                                 ? streamCommittedText
                                 : streamLastPartial;
      if (!fallback.empty() && streamLikelyHallucination(fallback)) {
        svc_log("Stream: %s — suppressed hallucinated fallback '%s'", why,
                fallback.c_str());
        fallback.clear();
      }
      if (!fallback.empty()) {
        svc_log("Stream: %s — fallback '%s' (peak=%.3f samples=%zu)", why,
                fallback.c_str(), peak, fed);
        sendEvent("FINAL_TEXT", fallback);
      } else {
        svc_log("Stream: %s — empty (peak=%.3f samples=%zu)", why, peak, fed);
        sendEvent("FINAL_TEXT", "");
      }
      resetStreamCaptureState();
    };

    if (!parakeetPipe.ready || !parakeetPipe.isProcessAlive() ||
        !parakeetPipe.modelLoaded) {
      endPopupWarmHold();
      emitFallbackOrEmpty("worker dead at end");
      return true;
    }
    sendEvent("STATE", "2,Transcribing...");
    // Drain leftover partial responses so stream_end's reply is not mixed in.
    drainStreamWorkerLines(32);
    if (!parakeetPipe.sendLine("{\"action\":\"stream_end\"}")) {
      svc_log("Stream: stream_end send failed");
      endPopupWarmHold();
      emitFallbackOrEmpty("stream_end send failed");
      emitPostTranscriptionState();
      return true;
    }
    // Wait for a real final (has "text") — skip pure partial/committed acks.
    // Cap at 2.5s: Nemotron finalize is usually fast once audio is flushed;
    // the old 10s wait felt like "no apparent reason" delay.
    bool gotFinalLine = false;
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < deadline) {
      std::string line = parakeetPipe.tryReadLine(100);
      if (line.empty())
        continue;
      // Skip pure status-ok lines without text/committed content.
      const bool hasText = line.find("\"text\"") != std::string::npos;
      const bool hasCommitted = line.find("\"committed\"") != std::string::npos;
      const bool hasPartial = line.find("\"partial\"") != std::string::npos ||
                              line.find("\"tentative\"") != std::string::npos;
      const bool hasError = line.find("\"error\"") != std::string::npos;
      if (hasError) {
        svc_log("Stream: stream_end error line: %s", line.c_str());
        break;
      }
      if (!hasText && (hasPartial || hasCommitted)) {
        // Late partial from the last feed — fold into live buffers, keep waiting.
        handleStreamWorkerLine(line, /*isFinal=*/false);
        continue;
      }
      handleStreamWorkerLine(line, /*isFinal=*/true);
      gotFinalLine = true;
      break;
    }
    if (!gotFinalLine) {
      emitFallbackOrEmpty("stream_end timeout");
    } else if (streamCommittedText.empty() && streamLastPartial.empty()) {
      // handleStreamWorkerLine already emitted empty FINAL_TEXT.
    }
    // handleStreamWorkerLine(isFinal) already cleared via send; ensure clean.
    resetStreamCaptureState();
    endPopupWarmHold();
    emitPostTranscriptionState();
    return true;
  }

  // Poll for Parakeet inference results (called each loop iteration)
  void pollParakeetResult() {
    // Streaming sessions drain their own responses in processStreamingChunk /
    // endStreamSession; do not steal lines here.
    if (streamSessionActive)
      return;
    if (!parakeetInferencePending) return;
    // If the user killed the worker mid-inference, abandon cleanly and allow
    // the next activation to relaunch instead of hanging on Transcribing...
    if (!parakeetPipe.ready || !parakeetPipe.isProcessAlive()) {
      svc_log("ParakeetPipe: Worker died during inference");
      parakeetInferencePending = false;
      parakeetPipe.markDead("died during inference");
      endPopupWarmHold();
      emitPostTranscriptionState();
      return;
    }
    std::string line = parakeetPipe.tryReadLine(0);
    if (line.empty()) {
      // Timeout check: if inference takes > 10s, abandon
      if (secondsSince(parakeetInferenceStart) > 10.0) {
        svc_log("ParakeetPipe: Inference timeout");
        parakeetInferencePending = false;
        endPopupWarmHold();
        emitPostTranscriptionState();
      }
      return;
    }
    parakeetInferencePending = false;
    // Parse JSON response: {"status":"ok","text":"..."}
    std::string text = jsonGetString(line.c_str(), "text");
    if (!text.empty()) {
      // Filter common Parakeet hallucinations
      std::string lower = lowercaseCopy(text);
      if (lower == "hey." || lower == "hey" || lower == "thank you." ||
          lower == "thank you" || lower == "bye." || lower == "bye" ||
          lower == "amen." || lower == "amen" || lower == "you.") {
        svc_log("ParakeetPipe: Filtered hallucination '%s'", text.c_str());
        endPopupWarmHold();
        emitPostTranscriptionState();
        return;
      }
      svc_log("ParakeetPipe: FINAL '%s' (%.0fms inference)",
              text.c_str(),
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - parakeetInferenceStart).count());
      lastSpeechTime = std::chrono::steady_clock::now();
      sendEvent("FINAL_TEXT", text);
      // Popup / push-to-talk session finished — stop holding the model warm so
      // the watchdog can offload after autoOffloadDelaySec (Handy-like).
      endPopupWarmHold();
      emitPostTranscriptionState();
    } else {
      std::string err = jsonGetString(line.c_str(), "error");
      if (!err.empty()) {
        svc_log("ParakeetPipe: Error: %s", err.c_str());
      }
      endPopupWarmHold();
      emitPostTranscriptionState();
    }
  }

  // VAD-driven Parakeet speech segmentation (like Handy's VAD → transcribe)
  // Handy settings: onset=60ms, hangover=450ms (offline), prefill=450ms
  void processParakeetDirectChunk(const std::vector<int16_t> &chunk,
                                   bool speechLikely, bool vadAvailable,
                                   int level) {
    const bool speechNow = vadAvailable ? (speechLikely && level >= 3) : (level >= 10);
    const auto now = std::chrono::steady_clock::now();

    // Pre-roll buffer (keep last 300ms for context — Handy uses 450ms)
    constexpr size_t kPreRollSamples = SAMPLE_RATE * 300 / 1000;
    parakeetPreRollBuf.insert(parakeetPreRollBuf.end(), chunk.begin(), chunk.end());
    if (parakeetPreRollBuf.size() > kPreRollSamples) {
      parakeetPreRollBuf.erase(
          parakeetPreRollBuf.begin(),
          parakeetPreRollBuf.begin() + (parakeetPreRollBuf.size() - kPreRollSamples));
    }

    // ── Ctrl+Space / popup PTT: capture EVERY sample from press → release ──
    // Professional push-to-talk must not wait for VAD speech onset. Waiting
    // dropped the start of phrases and often flushed only ~300ms ("Uh") or
    // nothing at all. Silence mid-hold also must not finalize — only POPUP_STOP.
    if (popupSessionHeld) {
      if (!parakeetSpeechActive) {
        parakeetSpeechActive = true;
        // Seed with pre-roll so audio from just before the hold is kept.
        parakeetUtteranceBuf = parakeetPreRollBuf;
      }
      parakeetUtteranceBuf.insert(parakeetUtteranceBuf.end(), chunk.begin(),
                                  chunk.end());
      if (speechNow || level >= 2) {
        parakeetLastSpeechTime = now;
        lastSpeechTime = now;
      }
      const double utteranceSec =
          double(parakeetUtteranceBuf.size()) / double(SAMPLE_RATE);
      if (utteranceSec >= 60.0) {
        svc_log("POPUP hold: max utterance length reached (%.1fs) — flushing",
                utteranceSec);
        parakeetSpeechActive = false;
        parakeetConsecutiveSpeechFrames = 0;
        parakeetConsecutiveSilenceFrames = 0;
        emitParakeetDirect();
      }
      return;
    }

    if (speechNow) {
      parakeetConsecutiveSpeechFrames++;
      parakeetConsecutiveSilenceFrames = 0;
      parakeetLastSpeechTime = now;
      lastSpeechTime = now;

      if (!parakeetSpeechActive && parakeetConsecutiveSpeechFrames >= 2) {
        // Speech started — begin capturing with pre-roll
        parakeetSpeechActive = true;
        parakeetUtteranceBuf = parakeetPreRollBuf;
      }
    } else {
      parakeetConsecutiveSilenceFrames++;
      parakeetConsecutiveSpeechFrames = 0;
    }

    if (!parakeetSpeechActive) return;

    // Append audio to utterance buffer
    parakeetUtteranceBuf.insert(parakeetUtteranceBuf.end(), chunk.begin(), chunk.end());

    const double utteranceSec = double(parakeetUtteranceBuf.size()) / double(SAMPLE_RATE);
    const bool longEnough = utteranceSec >= 0.60;

    // Time-based silence detection (natural speech pause allowance)
    const double silenceSinceLastSpeech =
        std::chrono::duration<double>(now - parakeetLastSpeechTime).count();
    const double silenceThreshold = utteranceSec > 5.0 ? 1.80 : 1.20;
    const bool silenceEnd = longEnough && silenceSinceLastSpeech >= silenceThreshold;

    // Hard silence cutoff (minimum 1.0s sustained zero-energy silence)
    const bool hardSilence = longEnough && level <= 1 && silenceSinceLastSpeech >= 1.00;

    // Max utterance length for main-widget free dictation
    const bool maxReached = utteranceSec >= 30.0;

    if (silenceEnd || hardSilence || maxReached) {
      parakeetSpeechActive = false;
      parakeetConsecutiveSpeechFrames = 0;
      parakeetConsecutiveSilenceFrames = 0;
      emitParakeetDirect();
    }
  }

  bool loadVoskModel() {
    std::lock_guard<std::recursive_mutex> lock(modelMutex);
    if (!voskAvailable) {
      svc_log("Vosk load skipped: libvosk runtime unavailable");
      sendEvent("STATE", "3,Vosk runtime missing — install a Vosk model");
      return false;
    }
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
    // Force aggressive memory compaction to drop
    // the working set down below 50MB when dormant
#ifdef _WIN32
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
#else
    // POSIX: freed pages return to the OS lazily; malloc_trim tightens the heap.
    malloc_trim(0);
#endif
    svc_log("Vosk model offloaded & memory compacted");
    sendEvent("OFFLOADED", "Vosk model offloaded");
  }

  void reloadVoskModel() {
    {
      std::lock_guard<std::recursive_mutex> lock(modelMutex);
      if (voskModel)
        return;
    }
    bool expected = false;
    if (!loadingModel.compare_exchange_strong(expected, true))
      return;

    sendEvent("STATE", "3,Loading model...");
    if (loadVoskModel()) {
      svc_log("Vosk model reloaded OK");
      sendEvent("STATE", "0,Ready");
    } else {
      svc_log("Failed to reload Vosk model");
      sendEvent("STATE", "3,Model load failed");
    }
    loadingModel = false;
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
      suppressWakeword(1.0);
      if (streamingPreferred && streamSessionActive)
        endStreamSession();
      // Keep model warm in RAM for 0ms instant mic restart
      sendEvent("STATE", "0,Ready");
    } else {
      activateSTT();
    }
  }

  // Push-to-talk stop: flush through the same pipeline selected by the main
  // widget. The popup must never change the active model or VAD/backend path.
  void popupStop() {
    // Keep hold semantics until after the flush decision so min-length and
    // logging still treat this as a full PTT capture.
    const bool wasPopupHold = popupSessionHeld;
    popupSessionHeld = false;
    if (mode != EngineMode::ACTIVE) {
      endPopupWarmHold();
      sendEvent("FINAL_TEXT", "");
      sendEvent("STATE", "0,Ready");
      return;
    }

    // Prefer direct path when the selected model is Parakeet/streaming, even
    // if the worker was killed and modelLoaded is currently false — emit will reload.
    const bool useParakeetDirect =
        parakeetPreferred ||
        (parakeetDirectMode && parakeetPipe.modelLoaded);
    const bool useCloudPipeline = cloudTranscription && !useParakeetDirect;
    mode = EngineMode::IDLE;
    lastSpeechTime = std::chrono::steady_clock::now();  // Reset offload timer

    // Streaming models: finalize with stream_end (Handy Live → final).
    // Full hold was already fed via processStreamingChunk every frame.
    if (streamingPreferred && (streamSessionActive || useParakeetDirect)) {
      suppressWakeword(1.0);
      if (endStreamSession())
        return;
      // Fall through to batch flush if stream never started.
    }

    if (useParakeetDirect) {
      // Do not transition the UI to Ready here. The result is asynchronous,
      // and engineLoop continues polling it while idle.
      // Warm-hold is cleared in pollParakeetResult after FINAL (or on empty).
      suppressWakeword(1.0);
      // PTT always finalizes whatever was captured during the hold — even if
      // VAD never flipped speechActive (we now force-buffer under hold).
      if (!parakeetUtteranceBuf.empty()) {
        svc_log("POPUP_STOP: Flushing %zu samples (%.0fms) to %s (hold=%d)",
                parakeetUtteranceBuf.size(),
                1000.0 * double(parakeetUtteranceBuf.size()) /
                    double(SAMPLE_RATE),
                streamWorkerKind.empty() ? "direct" : streamWorkerKind.c_str(),
                (int)wasPopupHold);
        parakeetSpeechActive = false;
        emitParakeetDirect();
      } else if (parakeetSpeechActive) {
        parakeetSpeechActive = false;
        svc_log("POPUP_STOP: Speech active but no utterance buffer");
        endPopupWarmHold();
        sendEvent("FINAL_TEXT", "");
      } else {
        svc_log("POPUP_STOP: No buffered audio to transcribe");
        endPopupWarmHold();
        // Explicit empty final so the popup can exit Transcribing cleanly
        // instead of waiting on a timeout / stale Ready fallback.
        sendEvent("FINAL_TEXT", "");
      }
      if (!parakeetInferencePending)
        sendEvent("STATE", "0,Ready");
      return;
    }

    if (useCloudPipeline) {
      // Keep cloudAwaitingFrontend intact after emitting the audio. The
      // frontend's FINAL_TEXT still belongs to the popup until it arrives.
      if (cloudSpeechStarted || !cloudUtteranceBuffer.empty()) {
        svc_log("POPUP_STOP: Flushing cloud utterance (%zu samples)",
                cloudUtteranceBuffer.size());
        emitCloudUtterance();
      } else {
        svc_log("POPUP_STOP: No cloud utterance buffered");
        resetCloudCaptureState();
        endPopupWarmHold();
      }
      suppressWakeword(1.0, false);
      if (!cloudAwaitingFrontend)
        sendEvent("STATE", "0,Ready");
      return;
    }

    // Local Vosk mode also needs an explicit final-result flush: stopping the
    // popup before Vosk sees a silence boundary previously dropped the phrase.
    std::string finalText;
    {
      std::lock_guard<std::recursive_mutex> lock(modelMutex);
      if (voskRec && vosk.recognizer_final_result) {
        finalText = jsonGetString(vosk.recognizer_final_result(voskRec), "text");
        vosk.recognizer_reset(voskRec);
      }
    }
    suppressWakeword(1.0);
    endPopupWarmHold();
    if (!finalText.empty()) {
      svc_log("POPUP_STOP: Vosk FINAL '%s'", finalText.c_str());
      sendEvent("FINAL_TEXT", finalText);
    } else {
      svc_log("POPUP_STOP: Vosk FINAL (EMPTY)");
    }
    sendEvent("STATE", "0,Ready");
  }

  // The command reader runs on a different thread from engineLoop. Queue popup
  // transitions so audio buffers and recognizers are only flushed by the audio
  // thread itself.
  void requestPopupStart() { popupStartRequested = true; }
  void requestPopupStop() { popupStopRequested = true; }

  void forcePause() {
    popupSessionHeld = false;
    mode = EngineMode::SLEEP; // Standard pause = go to wakeword but hidden
    finishCloudTurn();
    if (streamSessionActive)
      endStreamSession();
    offloadParakeetModel();  // Free RAM on pause
    sendEvent("STATE", "0,Paused");
  }

  void forceSleep() {
    popupSessionHeld = false;
    mode = EngineMode::SLEEP;
    suppressWakeword(10.0);  // 10s — long enough that ambient noise won't re-trigger
    finishCloudTurn();
    if (streamSessionActive)
      endStreamSession();
    offloadVoskModel();      // Free Vosk RAM when widget closed / sleep
    offloadParakeetModel();  // Free Parakeet RAM when widget closed / sleep
    sendEvent("STATE", "-1,Hidden");
  }

  void activateSTT() {
    suppressWakeword(0.0);
    lastActivationTime = std::chrono::steady_clock::now();
    owwHitCounts.clear();
    voskWakeHits = 0;
    lastSpeechTime = std::chrono::steady_clock::now();
    cloudAwaitingFrontend = false;
    frontendRequestedOffload = false;
    cloudSettleUntil = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(700);
    resetCloudCaptureState();
    // Reset Parakeet direct state
    parakeetSpeechActive = false;
    parakeetUtteranceBuf.clear();
    parakeetPreRollBuf.clear();
    parakeetConsecutiveSpeechFrames = 0;
    parakeetConsecutiveSilenceFrames = 0;
    streamLastPartial.clear();
    streamCommittedText.clear();
    resetStreamCaptureState();
    // Close any leftover stream so the decoder cannot replay the prior turn.
    if (streamSessionActive) {
      (void)parakeetPipe.sendLine("{\"action\":\"stream_end\"}");
      drainStreamWorkerLines(16);
      streamSessionActive = false;
    }

    // Reload direct worker if preferred but offloaded/killed — Handy-style.
    if (parakeetPreferred &&
        (!parakeetPipe.ready || !parakeetPipe.isProcessAlive() ||
         !parakeetPipe.modelLoaded)) {
      sendEvent("STATE",
                streamingPreferred ? "3,Loading Nemotron..."
                                   : "3,Loading model...");
      reloadParakeetModel();
      // After reload, ensure parakeetDirectMode is set so we enter ACTIVE below
      if (parakeetPipe.ready && parakeetPipe.isProcessAlive() && parakeetPipe.modelLoaded) {
        parakeetDirectMode = true;
      }
    }

    if ((parakeetDirectMode || parakeetPreferred) && parakeetPipe.modelLoaded &&
        parakeetPipe.isProcessAlive()) {
      // Direct worker mode — no Vosk needed
      parakeetDirectMode = true;
      mode = EngineMode::ACTIVE;
      streamCommittedText.clear();
      streamSessionActive = false;
      if (streamingPreferred) {
        beginStreamSession();
      }
      sendEvent("WAKEWORD_DETECTED", "0,Wakeword");
      sendEvent("STATE", "1,Listening...");
      return;
    }

    if (!cloudTranscription) {
      reloadVoskModel();
      std::lock_guard<std::recursive_mutex> lock(modelMutex);
      if (!voskRec) {
        mode = EngineMode::IDLE;
        sendEvent("STATE", "3,Model Missing");
        return;
      }
    }
    mode = EngineMode::ACTIVE;
    sendEvent("STATE", "1,Listening...");
  }

  void setWakeWords(const std::vector<std::string> &words) {
    std::lock_guard<std::recursive_mutex> lock(modelMutex);
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

  void suppressWakewordPublic(double seconds, bool resetCloudCapture = true) {
    suppressWakeword(seconds, resetCloudCapture);
  }

  void setWakeEngine(const std::string &engine) {
    std::lock_guard<std::recursive_mutex> lock(modelMutex);
    settings.wakeEngine = canonicalWakeEngine(engine);
    owwDetector.cleanup();
    pvDetector.cleanup_porcupine();
    initWakeEngines();
    sendEvent("STATE", "0,Wake engine updated");
  }

  void setTranscriptionMode(const std::string &modeValue) {
    const std::string normalized = lowercaseCopy(modeValue);
    const bool cloudMode = normalized.find("cloud") != std::string::npos;
    // STREAMING = Nemotron-class live partials; PARAKEET = batch direct PCM.
    const bool wantStreaming = normalized.find("streaming") != std::string::npos;
    const bool parakeetMode = normalized.find("parakeet") != std::string::npos ||
                              wantStreaming;

    streamingPreferred = false;
    streamSessionActive = false;
    streamCommittedText.clear();

    // Respect the model selected by the C++ frontend.  A loaded direct
    // worker must never take over Vosk, cloud, or another frontend model.
    if (!parakeetMode) {
      parakeetDirectMode = false;
      parakeetPreferred = false;
      cloudTranscription = cloudMode;
      svc_log("%s mode selected; direct worker bypassed",
              cloudMode ? "Cloud/frontend" : "Local");
    } else {
      streamingPreferred = wantStreaming;
      // Force relaunch if worker kind must change for streaming.
      if (parakeetPipe.ready && parakeetPipe.isProcessAlive()) {
        std::string wantExe, wantDir, wantKind;
        resolveDirectWorkerPaths(&wantExe, &wantDir, &wantKind);
        if (streamWorkerKind != wantKind && fs::exists(wantExe)) {
          parakeetPipe.markDead("mode switch relaunch");
          parakeetDirectMode = false;
        }
      }
      if (parakeetPipe.ready && parakeetPipe.isProcessAlive() &&
          parakeetPipe.modelLoaded) {
        parakeetDirectMode = true;
        parakeetPreferred = true;
        cloudTranscription = false;
        svc_log("Direct Mode active (%s streaming=%d)", streamWorkerKind.c_str(),
                (int)streamingPreferred);
      } else if (initParakeetDirect()) {
        parakeetDirectMode = true;
        parakeetPreferred = true;
        cloudTranscription = false;
        svc_log("Direct Mode activated (%s streaming=%d)",
                streamWorkerKind.c_str(), (int)streamingPreferred);
      } else {
        // Streaming requested but engine/weights missing → frontend fallback.
        parakeetDirectMode = false;
        parakeetPreferred = false;
        streamingPreferred = false;
        cloudTranscription = true;
        svc_log("Direct Mode unavailable; using frontend fallback");
      }
      sendEvent("MODEL_CAP",
                streamingPreferred ? "streaming=1" : "streaming=0");
    }

    cloudAwaitingFrontend = false;
    cloudSpeechStarted = false;
    cloudPreRollBuffer.clear();
    cloudUtteranceBuffer.clear();
    svc_log("Transcription mode: %s", parakeetDirectMode.load()
                                           ? "parakeet-direct"
                                           : (cloudTranscription ? "cloud" : "local"));
    if (mode == EngineMode::ACTIVE) {
      sendEvent("STATE", "1,Listening...");
    }
  }

  void setFrontendSegmentationMode(const std::string &modeValue) {
    const std::string normalized = lowercaseCopy(modeValue);
    FrontendSegmentationMode nextMode = FrontendSegmentationMode::Normal;
    if (normalized.find("accurate") != std::string::npos ||
        normalized.find("quality") != std::string::npos ||
        normalized.find("parakeet") != std::string::npos) {
      nextMode = FrontendSegmentationMode::Accurate;
    } else if (normalized.find("balanced") != std::string::npos) {
      nextMode = FrontendSegmentationMode::Balanced;
    } else if (normalized.find("fast") != std::string::npos ||
               normalized.find("local") != std::string::npos) {
      nextMode = FrontendSegmentationMode::Fast;
    }
    frontendSegmentationMode = int(nextMode);
    cloudAwaitingFrontend = false;
    resetCloudCaptureState();
    const char *label =
        nextMode == FrontendSegmentationMode::Fast
            ? "fast"
            : (nextMode == FrontendSegmentationMode::Balanced
                   ? "balanced"
                   : (nextMode == FrontendSegmentationMode::Accurate
                          ? "accurate"
                          : "normal"));
    svc_log("Frontend segmentation set to %s", label);
  }

  void finishCloudTurn() {
    cloudAwaitingFrontend = false;
    cloudCaptureStartedAt = std::chrono::steady_clock::now();
    lastSpeechTime = cloudCaptureStartedAt;
    if (!cloudSpeechStarted)
      resetCloudCaptureState();
    if (mode == EngineMode::ACTIVE)
      sendEvent("STATE", "1,Listening...");
  }

private:
  void suppressWakeword(double seconds, bool resetCloudCapture = true) {
    std::lock_guard<std::recursive_mutex> lock(modelMutex);
    if (seconds <= 0.0) {
      wakeSuppressedUntil = 0.0;
    } else {
      wakeSuppressedUntil = getTimeSeconds() + seconds;
    }
    if (resetCloudCapture) {
      cloudAwaitingFrontend = false;
      resetCloudCaptureState();
    }
    owwHitCounts.clear();
    owwChunkBuffer.clear();
    pvChunkBuffer.clear();
    voskWakeHits = 0;
    wakeVadSpeechFrames = 0;
    wakeVadSilenceFrames = 0;
    wakeVadLastSpeechTime = std::chrono::steady_clock::time_point{};
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
    const FrontendSegmentationMode mode =
        FrontendSegmentationMode(frontendSegmentationMode.load());
    const double minimumSeconds =
        mode == FrontendSegmentationMode::Fast
            ? 0.12
            : (mode == FrontendSegmentationMode::Balanced
                   ? 0.35
                   : (mode == FrontendSegmentationMode::Accurate ? 0.55
                                                                 : 0.18));
    if (cloudUtteranceBuffer.size() < size_t(SAMPLE_RATE * minimumSeconds)) {
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

  void processCloudChunk(const std::vector<int16_t> &chunk, int level,
                         bool speechLikely, bool vadAvailable) {
    const FrontendSegmentationMode segmentationMode =
        FrontendSegmentationMode(frontendSegmentationMode.load());
    const bool fastFrontendMode =
        segmentationMode == FrontendSegmentationMode::Fast;
    const bool balancedFrontendMode =
        segmentationMode == FrontendSegmentationMode::Balanced;
    const bool accurateFrontendMode =
        segmentationMode == FrontendSegmentationMode::Accurate;
    const double kCloudPreRollSec = fastFrontendMode
                                        ? 0.08
                                        : accurateFrontendMode
                                              ? 1.25
                                              : (balancedFrontendMode ? 0.18
                                                                      : 0.14);
    const double kCloudMinUtteranceSec = fastFrontendMode
                                             ? 0.12
                                             : accurateFrontendMode
                                                   ? 0.55
                                                   : (balancedFrontendMode ? 0.42
                                                                           : 0.22);
    const double kCloudFastSilenceSec = fastFrontendMode
                                            ? 0.14
                                            : accurateFrontendMode
                                                  ? 0.30
                                                  : (balancedFrontendMode ? 0.22
                                                                          : 0.26);
    const double kCloudSlowSilenceSec = fastFrontendMode
                                            ? 0.22
                                            : accurateFrontendMode
                                                  ? 0.95
                                                  : (balancedFrontendMode ? 0.44
                                                                          : 0.36);
    const double kCloudSlowSilenceAfterSec =
        fastFrontendMode ? 0.90
                         : accurateFrontendMode
                               ? 2.80
                               : (balancedFrontendMode ? 1.70 : 1.90);
    const double kCloudMaxUtteranceSec = fastFrontendMode
                                             ? 3.2
                                             : accurateFrontendMode
                                                   ? 24.0
                                                   : (balancedFrontendMode ? 8.0
                                                                           : 9.0);
    const size_t maxPreRollSamples = size_t(SAMPLE_RATE * kCloudPreRollSec);
    cloudPreRollBuffer.insert(cloudPreRollBuffer.end(), chunk.begin(), chunk.end());
    if (cloudPreRollBuffer.size() > maxPreRollSamples) {
      cloudPreRollBuffer.erase(
          cloudPreRollBuffer.begin(),
          cloudPreRollBuffer.begin() +
              (cloudPreRollBuffer.size() - maxPreRollSamples));
    }

    if (cloudAwaitingFrontend && !balancedFrontendMode && !accurateFrontendMode)
      return;

    if (std::chrono::steady_clock::now() < cloudSettleUntil)
      return;

    const bool levelSpeechNow =
        level >= (fastFrontendMode ? 8
                                   : accurateFrontendMode ? 8
                                                          : balancedFrontendMode
                                                                ? 9
                                                                : 10);
    const bool levelHoldSpeech =
        level >= (fastFrontendMode ? 5
                                   : accurateFrontendMode ? 4
                                                          : balancedFrontendMode
                                                                ? 5
                                                                : 6);
    const bool speechNow = vadAvailable ? (speechLikely && level >= 2)
                                        : levelSpeechNow;
    const bool holdSpeech = vadAvailable ? speechLikely : levelHoldSpeech;
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
        utteranceLongEnough && level <= 1 &&
        (std::chrono::duration<double>(now - cloudLastSpeechTime).count() >= 1.00);
    const bool maxReached = cloudUtteranceBuffer.size() >=
                            size_t(SAMPLE_RATE * kCloudMaxUtteranceSec);

    if (maxReached || silenceReached || hardSilenceReached)
      emitCloudUtterance();
  }

  void engineLoop() {
    svc_log("Engine loop started, mode=%d", (int)mode.load());

    // Verify model exists on disk but do NOT load it eagerly.
    // The model will be loaded on-demand by activateSTT() (wake word
    // or manual toggle) or by the Vosk-fallback auto-reload below.
    {
      std::string path = findModelPath(modelsDir, activeEngine);
      if (path.empty()) {
        sendEvent("STATE", "3,Model Missing");
        svc_log("No model found — waiting for download");
        while (running) {
          std::this_thread::sleep_for(std::chrono::seconds(2));
          path = findModelPath(modelsDir, activeEngine);
          if (!path.empty())
            break;
        }
        if (!running)
          return;
      }
      activeModelPath = path;
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
      sendEvent("STATE", "0,Ready");

    svc_log("Main loop running, oww=%s", owwReady ? "loaded" : "none");

    // Main audio processing loop
    while (running) {
      std::vector<int16_t> chunk;
      if (!audio.getChunk(chunk, 100))
        continue;
      if (chunk.empty())
        continue;

      if (popupStartRequested.exchange(false)) {
        // Mark hold before activate so VAD never auto-finalizes mid-PTT.
        popupSessionHeld = true;
        activateSTT();
      }
      if (popupStopRequested.exchange(false))
        popupStop();

      // Software AGC normalization (spec Section 3) — runs on every frame
      // before RNNoise/VAD so all downstream consumers see consistent levels
      agc.process(chunk);

      const AudioPreprocessResult preprocessed = preprocessor.process(chunk);
      const std::vector<int16_t> &audioChunk = preprocessed.samples.empty()
                                                   ? chunk
                                                   : preprocessed.samples;

      // A popup release changes the engine to IDLE before the Parakeet worker
      // returns. Poll before choosing the IDLE/ACTIVE branch so that result is
      // never stranded while the wake-word loop is running.
      if (parakeetInferencePending)
        pollParakeetResult();

      // Audio level
      int level = computeAudioLevel(audioChunk.data(), audioChunk.size());
      bool vadActive = preprocessed.vadAvailable;
      sendEvent("AUDIO_LEVEL", std::to_string(level));

      if (!vadActive && mode == EngineMode::ACTIVE) {
        // optionally fall-back without VAD, but here we just log.
        svc_log("TEN-VAD is not available for this chunk; continuing without VAD.");
      }
      // ── Acoustic Event Processing (Clap & Snap Hybrid Trigger) ──
      AcousticEventType acEvt = acousticDetector.processChunk(audioChunk.data(), audioChunk.size());
      if (acEvt != AcousticEventType::None) {
        std::string act = (acEvt == AcousticEventType::Snap) ? acousticDetector.snapAction : acousticDetector.clapAction;
        if (act == "wakeword" && mode == EngineMode::IDLE && wakeSuppressedUntil < 999999.0) {
          if (secondsSince(lastActivationTime) > 0.6) {
            svc_log("Acoustic %s triggered WAKEWORD action — activating STT", acEvt == AcousticEventType::Snap ? "Snap" : "Clap");
            activateSTT();
            continue;
          }
        } else if (act == "closeword" && mode == EngineMode::ACTIVE) {
          svc_log("Acoustic %s triggered CLOSEWORD action — stopping STT", acEvt == AcousticEventType::Snap ? "Snap" : "Clap");
          toggleListening();
          continue;
        }
      }

      // ── Wakeword mode (IDLE / SLEEP) ──
      if (mode != EngineMode::ACTIVE) {
        // The command thread can rebuild the TFLite/Picovoice detectors while
        // settings are changed. Keep that rebuild and every streaming predict
        // call in the same lock; otherwise a cleanup can free an interpreter
        // while this audio thread is using it.
        std::lock_guard<std::recursive_mutex> wakeLock(modelMutex);
        if (wakeSuppressedUntil > 0.0 && getTimeSeconds() < wakeSuppressedUntil)
          continue;

        // Transition SLEEP → IDLE once suppression expires so wakewords
        // can operate.  We reload the lightweight Vosk keyword model here
        // (if needed) so wakeword detection has a model to run against.
        if (mode == EngineMode::SLEEP) {
          mode = EngineMode::IDLE;
          wakeSuppressedUntil = 0.0;  // clear stale value
          // Reload the small Vosk keyword model for wakeword detection
          if (voskFallbackRequired && !voskRec) {
            reloadVoskModel();
          }
          sendEvent("STATE", "0,Wakewords active");
        }

        // Feed the wake-word model continuously with the original 16 kHz PCM.
        // TEN-VAD gates *acceptance*, not feature extraction: a hard VAD gate
        // starves the streaming model before short wake phrases can score.
        // Wake path uses a lower energy floor than dictation so quiet speech
        // still opens the gate without making endpointing hypersensitive.
        const bool vadSpeechNow = preprocessed.vadAvailable
                                      ? (preprocessed.speechLikely ||
                                         preprocessed.vadProbability >= 0.12f ||
                                         level >= 2)
                                      : (level >= 2);
        // Periodically trim working set memory when idle to optimize RAM usage (~45MB)
        static auto lastTrimTime = std::chrono::steady_clock::now();
        const auto nowTime = std::chrono::steady_clock::now();
        if (nowTime - lastTrimTime > std::chrono::seconds(10)) {
          lastTrimTime = nowTime;
#ifdef _WIN32
          SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
#endif
        }
        const auto wakeNow = std::chrono::steady_clock::now();
        if (vadSpeechNow) {
          ++wakeVadSpeechFrames;
          wakeVadSilenceFrames = 0;
          wakeVadLastSpeechTime = wakeNow;
        } else {
          wakeVadSpeechFrames = 0;
          ++wakeVadSilenceFrames;
        }
        const bool recentlySpoke =
            wakeVadLastSpeechTime != std::chrono::steady_clock::time_point{} &&
            wakeNow - wakeVadLastSpeechTime <= kWakeVadDecisionHold;
        const bool wakeGateOpen =
            wakeVadSpeechFrames >= kWakeVadMinSpeechFrames || recentlySpoke;

        const bool needPV = pvReady;
        const bool needOWW = owwReady || wakeNetReady;
        const bool needVosk = voskRec && voskFallbackRequired;

        if (needPV) {
          if (pvChunkBuffer.size() > 32000) pvChunkBuffer.clear();
          pvChunkBuffer.insert(pvChunkBuffer.end(), chunk.begin(), chunk.end());
          while ((int)pvChunkBuffer.size() >= pvDetector.frame_length) {
            std::vector<int16_t> processData(pvChunkBuffer.begin(), pvChunkBuffer.begin() + pvDetector.frame_length);
            pvChunkBuffer.erase(pvChunkBuffer.begin(), pvChunkBuffer.begin() + pvDetector.frame_length);
            
            int32_t keyword_index = pvDetector.predict(processData.data());
            if (keyword_index >= 0 && secondsSince(lastActivationTime) > 2.0 &&
                mode == EngineMode::IDLE) {
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
              if (wakeNetReady) {
                auto wnScores = wakeNetDetector.predict(processData.data(), processData.size());
                for (auto &[mdl, score] : wnScores) {
                  scores[mdl] = score;
                }
              }
              for (auto &[mdl, score] : scores) {
                if (score >= 0.55f && secondsSince(lastActivationTime) > 2.0 &&
                    mode == EngineMode::IDLE) {
                  svc_log("OWW Wakeword Triggered: %s (score=%.2f)", mdl.c_str(), score);
                  activateSTT();
                  owwChunkBuffer.clear();
                  pvChunkBuffer.clear();
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
                if (secondsSince(lastActivationTime) > 2.0 &&
                    mode == EngineMode::IDLE) {
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
        // Poll for pending Parakeet inference results (non-blocking)
        // ── Parakeet Direct Mode: VAD-driven, zero file I/O (like Handy) ──
        // Use RAW audio for Parakeet (model handles noise internally, like Handy)
        // RNNoise's crude 16k→48k resampling distorts audio and reduces accuracy
        if (parakeetDirectMode && parakeetPipe.modelLoaded) {
          // Streaming (Nemotron): feed every chunk for live partials.
          // Batch (Parakeet): VAD-segment then transcribe_pcm on endpoint.
          if (streamingPreferred && streamSessionActive) {
            processStreamingChunk(chunk);
          } else if (streamingPreferred && !streamSessionActive) {
            // Session should have started in activateSTT; retry once.
            if (beginStreamSession())
              processStreamingChunk(chunk);
            else
              processParakeetDirectChunk(chunk, preprocessed.speechLikely,
                                         preprocessed.vadAvailable, level);
          } else {
            processParakeetDirectChunk(chunk, preprocessed.speechLikely,
                                        preprocessed.vadAvailable, level);
          }
          continue;
        }

        if (cloudTranscription) {
          processCloudChunk(audioChunk, level, preprocessed.speechLikely,
                            preprocessed.vadAvailable);
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
              voskRec, (const char *)audioChunk.data(),
              (int)(audioChunk.size() * 2));
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
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const bool cloudTurnInFlight = cloudTranscription && cloudAwaitingFrontend;
      const bool parakeetTurnInFlight = parakeetDirectMode && parakeetInferencePending;
      // Never auto-idle / offload during Ctrl+Space hold or a live stream.
      const bool sessionBusy =
          popupSessionHeld || streamSessionActive || parakeetTurnInFlight ||
          cloudTurnInFlight;

      // Auto-idle after silence (return to wakeword listening)
      if (!sessionBusy && mode == EngineMode::ACTIVE &&
          secondsSince(lastSpeechTime) > silenceLimitSec) {
        mode = EngineMode::IDLE;
        suppressWakeword(0.8);
        // Reset parakeet state on idle
        parakeetSpeechActive = false;
        parakeetUtteranceBuf.clear();
        if (streamSessionActive)
          endStreamSession();
        sendEvent("STATE", "0,Ready");
      }

      if (!sessionBusy && mode != EngineMode::ACTIVE) {
        // Parakeet/Nemotron auto-offload: respect Dashboard delay setting
        if (parakeetPreferred && parakeetPipe.ready && parakeetPipe.modelLoaded) {
          if (!popupPreloadActive && settings.autoOffloadEnabled &&
              secondsSince(lastSpeechTime) > settings.autoOffloadDelaySec) {
            svc_log("Direct worker dormant timeout (%d sec) -> Offloading to save RAM",
                    settings.autoOffloadDelaySec);
            offloadParakeetModel();
          }
          continue;  // No Vosk management needed while direct worker is the engine
        }
        bool modelLoaded = false;
        {
          std::lock_guard<std::recursive_mutex> lock(modelMutex);
          modelLoaded = (voskModel != nullptr);
        }
        if (voskFallbackRequired && !modelLoaded && !frontendRequestedOffload) {
          svc_log("Vosk fallback wakeword active — reloading model");
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
// Command reader (stdin) — same pipe protocol as the legacy service
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
    else if (action == "START")
      engine.activateSTT();  // Explicit start (idempotent)
    else if (action == "POPUP_STOP")
      engine.requestPopupStop();  // Flush on the audio thread
    else if (action == "POPUP_START")
      engine.requestPopupStart();  // Activate on the audio thread
    else if (action == "MODEL_CAP") {
      // Forward capability flags to the Ctrl+Space overlay (streaming Live panel).
      // Payload examples: "streaming=1", "streaming=0"
      sendEvent("MODEL_CAP", payload.empty() ? "streaming=0" : payload);
    }
    else if (action == "PRELOAD") {
      const bool enable = payload != "0" && lowercaseCopy(payload) != "off" &&
                          lowercaseCopy(payload) != "false";
      // PRELOAD is only a latency hint for the model already selected in the
      // main widget. It must never permanently freeze the model in RAM:
      // popupPreloadActive is cleared on POPUP_STOP / FINAL (endPopupWarmHold).
      // PRELOAD:0 explicitly ends the warm-hold and re-enables auto-offload.
      if (!enable) {
        engine.endPopupWarmHold();
        svc_log("PRELOAD disabled: warm-hold cleared modelLoaded=%d",
                (int)engine.parakeetPipe.modelLoaded);
      } else if (engine.parakeetPreferred) {
        // Session-scoped warm hold only — watchdog may offload after stop.
        engine.popupPreloadActive = true;
        if (!engine.parakeetPipe.modelLoaded ||
            !engine.parakeetPipe.isProcessAlive()) {
          svc_log("PRELOAD: ensuring selected Parakeet engine is loaded...");
          engine.reloadParakeetModel();
        }
        engine.lastSpeechTime = std::chrono::steady_clock::now();
        svc_log("PRELOAD enabled: modelLoaded=%d alive=%d",
                (int)engine.parakeetPipe.modelLoaded,
                (int)engine.parakeetPipe.isProcessAlive());
      } else {
        engine.popupPreloadActive = false;
        svc_log("PRELOAD ignored: selected model is not Parakeet direct");
      }
    }
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
        // Never unload mid turn — that was causing Nemotron reloads, empty
        // finals, and multi-second stalls while the user was still speaking.
        if (engine.mode == EngineMode::ACTIVE || engine.popupSessionHeld ||
            engine.streamSessionActive || engine.parakeetInferencePending) {
          svc_log("OFFLOAD ignored: session still active (mode=%d popup=%d stream=%d)",
                  (int)engine.mode.load(), (int)engine.popupSessionHeld,
                  (int)engine.streamSessionActive);
        } else {
          engine.frontendRequestedOffload = true;
          engine.offloadVoskModel();
          engine.offloadParakeetModel();
          svc_log("Frontend requested offload (forced)");
        }
      }
    }
    else if (action == "OFFLOADDELAY")
      engine.settings.autoOffloadDelaySec = std::stoi(payload);
    else if (action == "INACTIVITY_STOP") {
      try {
        int sec = std::stoi(payload);
        if (sec <= 0) {
          engine.silenceLimitSec = 999999;
          svc_log("Inactivity auto-stop DISABLED");
        } else {
          engine.silenceLimitSec = sec;
          svc_log("Inactivity auto-stop set to %d sec", sec);
        }
      } catch (...) {}
    }
    else if (action == "CLAP_ACTION") {
      engine.acousticDetector.clapAction = payload;
      engine.acousticDetector.clapEnabled = (payload != "disabled");
      svc_log("Clap action set to '%s'", payload.c_str());
    }
    else if (action == "SNAP_ACTION") {
      engine.acousticDetector.snapAction = payload;
      engine.acousticDetector.snapEnabled = (payload != "disabled");
      svc_log("Snap action set to '%s'", payload.c_str());
    }
    else if (action == "ACOUSTIC_SENSITIVITY") {
      try {
        engine.acousticDetector.sensitivity = std::stof(payload);
        svc_log("Acoustic sensitivity set to %.2f", engine.acousticDetector.sensitivity);
      } catch (...) {}
    }
    else if (action == "WAKEMODE")
      engine.setWakeEngine(payload);
    else if (action == "WAKEWORDMODE") {
      // payload: "Always On" | "Off" | "On with Widget"
      svc_log("WAKEWORDMODE set to: %s", payload.c_str());
      if (payload == "Off") {
        // Disable wakeword detection entirely
        engine.suppressWakewordPublic(999999.0);
        sendEvent("STATE", "0,Wakewords disabled");
      } else {
        // "Always On" or "On with Widget" — re-enable wake detection
        engine.suppressWakewordPublic(0.0);
        sendEvent("STATE", "0,Wakewords active");
      }
    }
    else if (action == "PV_KEY") {
      engine.settings.porcupineAccessKey = payload;
      engine.setWakeEngine(engine.settings.wakeEngine);
    }
    else if (action == "SETLOAD")
      engine.settings.autoModelLoad = (payload == "1");
    else if (action == "TRANSCRIBE_MODE")
      engine.setTranscriptionMode(payload);
    else if (action == "PARAKEET_DIRECT") {
      if (payload == "1" || payload == "true" || payload == "on") {
        engine.parakeetPreferred = true;
        if (!engine.parakeetPipe.ready) {
          if (engine.initParakeetDirect()) {
            engine.parakeetDirectMode = true;
            svc_log("Parakeet Direct Mode enabled via command");
          } else {
            sendEvent("ERROR", "Parakeet engine unavailable");
          }
        } else if (!engine.parakeetPipe.modelLoaded) {
          engine.reloadParakeetModel();
        } else {
          engine.parakeetDirectMode = true;
        }
      } else {
        engine.parakeetDirectMode = false;
        engine.parakeetPreferred = false;
      }
    }
    else if (action == "FRONTEND_SEGMENTATION")
      engine.setFrontendSegmentationMode(payload);
    else if (action == "CLOUD_DONE")
      engine.finishCloudTurn();
    else if (action == "MODEL") {
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
      // Do not interrupt a live dictation / Ctrl+Space / stream session.
      if (engine.mode == EngineMode::ACTIVE || engine.popupSessionHeld ||
          engine.streamSessionActive) {
        svc_log("RELOAD ignored: session still active");
      } else {
        engine.frontendRequestedOffload = false;
        // Direct workers (Parakeet/Nemotron) must reload the selected engine,
        // not Vosk — the old path loaded Vosk on top of Nemotron mid-use.
        if (engine.parakeetPreferred) {
          if (engine.reloadParakeetModel()) {
            sendEvent("STATE", "0,Model Ready");
          } else {
            sendEvent("STATE", "3,Model Missing");
          }
        } else {
          engine.reloadVoskModel();
          std::lock_guard<std::recursive_mutex> lock(engine.modelMutex);
          if (engine.voskRec) {
            sendEvent("STATE", "0,Model Ready");
          } else {
            sendEvent("STATE", "3,Model Missing");
          }
        }
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

static STTEngine *g_signalEngine = nullptr;
extern "C" void quicksttSignalHandler(int sig) {
  (void)sig;
  // Terminate immediately — the GUI sends QUIT over stdin for graceful exits.
  if (g_signalEngine)
    g_signalEngine->stop();
  _exit(0);
}

int main() {
  // Ensure stdout is unbuffered for pipe communication
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);

#ifndef _WIN32
  std::signal(SIGTERM, quicksttSignalHandler);
  std::signal(SIGINT, quicksttSignalHandler);
#endif

  sendEvent("INIT", "Native Service Ready");

  STTEngine engine;
  g_signalEngine = &engine;
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
