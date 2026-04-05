#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#ifndef _WIN32_IE
#define _WIN32_IE 0x0300
#endif
#include <algorithm>
#include <cctype>
#include <chrono>
#include <commctrl.h>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <set>
#include <shlobj.h>
#include <sstream>
#include <string>
#include <vector>
#include <wincrypt.h>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

int urlPriority(const std::string &url) {
  std::string host = url;
  size_t scheme = host.find("://");
  if (scheme != std::string::npos)
    host = host.substr(scheme + 3);
  size_t slash = host.find('/');
  if (slash != std::string::npos)
    host = host.substr(0, slash);
  if (!host.empty() && host.front() == '[') {
    size_t close = host.find(']');
    if (close != std::string::npos)
      host = host.substr(1, close - 1);
  } else {
    size_t colon = host.find(':');
    if (colon != std::string::npos)
      host = host.substr(0, colon);
  }

  std::string lower = host;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });

  if (lower == "127.0.0.1" || lower == "::1" || lower == "localhost")
    return 40;
  if (lower.rfind("192.168.", 0) == 0 || lower.rfind("10.", 0) == 0)
    return 0;
  if (lower.rfind("172.", 0) == 0) {
    size_t dot = lower.find('.', 4);
    if (dot != std::string::npos) {
      int second = atoi(lower.substr(4, dot - 4).c_str());
      if (second >= 16 && second <= 31)
        return 0;
    }
  }
  if (lower.rfind("169.254.", 0) == 0)
    return 5;
  if (lower.rfind("fe80:", 0) == 0)
    return 10;
  if (lower.rfind("fc", 0) == 0 || lower.rfind("fd", 0) == 0)
    return 12;
  if (lower.find(':') != std::string::npos)
    return 25;
  return 20;
}

void sortUrlsByPriority(std::vector<std::string> &urls) {
  std::stable_sort(urls.begin(), urls.end(), [](const std::string &a,
                                                const std::string &b) {
    return urlPriority(a) < urlPriority(b);
  });
}

void appendUniqueUrl(std::vector<std::string> &urls, const std::string &url) {
  if (url.empty())
    return;
  if (std::find(urls.begin(), urls.end(), url) == urls.end())
    urls.push_back(url);
}

std::string trimCopy(std::string value) {
  while (!value.empty() &&
         (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
    value.pop_back();
  size_t start = 0;
  while (start < value.size() &&
         (value[start] == ' ' || value[start] == '\r' || value[start] == '\n'))
    ++start;
  return value.substr(start);
}

std::string formatHttpUrlForHost(const std::string &host) {
  if (host.empty())
    return "";
  if (host.find("http://") == 0 || host.find("https://") == 0)
    return host;
  if (host.find(':') != std::string::npos && host.find(']') == std::string::npos)
    return "http://[" + host + "]:5000";
  return "http://" + host + ":5000";
}

void appendDiscoveryResponseUrls(std::vector<std::string> &urls,
                                 const std::string &payload) {
  const std::string prefix = "QUICKSTT_DISCOVERY|";
  if (payload.find(prefix) != 0)
    return;

  const size_t versionSep = payload.find('|', prefix.size());
  if (versionSep == std::string::npos)
    return;

  std::string urlBlock = payload.substr(versionSep + 1);
  std::stringstream ss(urlBlock);
  std::string url;
  while (std::getline(ss, url, ';')) {
    url = trimCopy(url);
    if (!url.empty())
      appendUniqueUrl(urls, url);
  }
}

std::vector<sockaddr_in> discoveryTargets() {
  std::vector<sockaddr_in> targets;
  std::set<unsigned long> seen;
  auto appendTarget = [&](unsigned long addrNetworkOrder) {
    if (seen.insert(addrNetworkOrder).second) {
      sockaddr_in target = {};
      target.sin_family = AF_INET;
      target.sin_port = htons(5001);
      target.sin_addr.s_addr = addrNetworkOrder;
      targets.push_back(target);
    }
  };

  appendTarget(inet_addr("255.255.255.255"));
  appendTarget(inet_addr("127.0.0.1"));

  ULONG bufferSize = 0;
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER;
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &bufferSize) !=
      ERROR_BUFFER_OVERFLOW) {
    return targets;
  }

  std::vector<unsigned char> buffer(bufferSize);
  IP_ADAPTER_ADDRESSES *addresses =
      reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &bufferSize) !=
      NO_ERROR) {
    return targets;
  }

  for (IP_ADAPTER_ADDRESSES *adapter = addresses; adapter;
       adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp)
      continue;
    if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
      continue;

    for (IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress;
         unicast; unicast = unicast->Next) {
      if (!unicast->Address.lpSockaddr ||
          unicast->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }

      sockaddr_in *addr =
          reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr);
      const uint32_t ip = ntohl(addr->sin_addr.s_addr);
      const ULONG prefix = std::min<ULONG>(unicast->OnLinkPrefixLength, 32);
      const uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
      const uint32_t broadcastHostOrder = (ip & mask) | (~mask);
      const unsigned long broadcastNetworkOrder = htonl(broadcastHostOrder);
      appendTarget(broadcastNetworkOrder);
    }
  }

  return targets;
}

void discoverServerUrlsUdp(std::vector<std::string> &urls) {
  WSADATA wsaData = {};
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    return;

  SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    WSACleanup();
    return;
  }

  BOOL enableBroadcast = TRUE;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
             reinterpret_cast<const char *>(&enableBroadcast),
             sizeof(enableBroadcast));

  DWORD timeoutMs = 900;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));

  sockaddr_in bindAddr = {};
  bindAddr.sin_family = AF_INET;
  bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  bindAddr.sin_port = htons(0);
  if (bind(sock, reinterpret_cast<sockaddr *>(&bindAddr), sizeof(bindAddr)) ==
      SOCKET_ERROR) {
    closesocket(sock);
    WSACleanup();
    return;
  }

  const std::string probe = "QUICKSTT_DISCOVER_V1";
  for (const sockaddr_in &target : discoveryTargets()) {
    sendto(sock, probe.c_str(), static_cast<int>(probe.size()), 0,
           reinterpret_cast<const sockaddr *>(&target), sizeof(target));
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
  char buffer[8192];
  while (std::chrono::steady_clock::now() < deadline) {
    sockaddr_in from = {};
    int fromLen = sizeof(from);
    const int received =
        recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                 reinterpret_cast<sockaddr *>(&from), &fromLen);
    if (received == SOCKET_ERROR) {
      const int err = WSAGetLastError();
      if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK)
        break;
      continue;
    }
    if (received <= 0)
      continue;
    buffer[received] = '\0';
    appendDiscoveryResponseUrls(urls, trimCopy(buffer));
  }

  closesocket(sock);
  WSACleanup();
}

} // namespace

// Read all configured server URLs from server_urls.txt (one per line)
// Falls back to encrypted default + localhost
std::vector<std::string> getServerUrls() {
  std::vector<std::string> urls;

  // Priority 1: UDP LAN discovery from a local QuickSTT server broadcaster
  discoverServerUrlsUdp(urls);
  sortUrlsByPriority(urls);

  // Priority 2: Read from server_urls.txt
  std::ifstream rf("server_urls.txt");
  if (rf.is_open()) {
      std::string line;
      while (std::getline(rf, line)) {
      line = trimCopy(line);
      if (!line.empty())
        appendUniqueUrl(urls, line);
      }
  }

  // Priority 3: Fallback to server.txt (legacy single URL)
  if (urls.empty()) {
    std::ifstream sf("server.txt");
    if (sf.is_open()) {
      std::string line;
      if (std::getline(sf, line) && !line.empty()) {
        line = trimCopy(line);
        if (!line.empty())
          appendUniqueUrl(urls, line);
      }
    }
  }

  // Priority 4: Encrypted default
  std::string hex = "1b1117025f5b5c3e51465546490453425f4043575944044c465f521450"
                    "41490401145d4e115351445f474207022f5f41435553";
  std::string key = "secret";
  std::string decrypted = "";
  for (size_t i = 0; i < hex.length(); i += 2) {
    std::string byteString = hex.substr(i, 2);
    char byte = (char)strtol(byteString.c_str(), NULL, 16);
    decrypted += (char)(byte ^ key[(i / 2) % key.length()]);
  }
  // Add if not already present
  bool found = false;
  for (const auto &u : urls) {
    if (u == decrypted) {
      found = true;
      break;
    }
  }
  if (!found)
    appendUniqueUrl(urls, decrypted);

  // Priority 5: localhost
  found = false;
  for (const auto &u : urls) {
    if (u == "http://127.0.0.1:5000") {
      found = true;
      break;
    }
  }
  if (!found)
    appendUniqueUrl(urls, "http://127.0.0.1:5000");

  sortUrlsByPriority(urls);
  return urls;
}

HWND g_hProgressBar = NULL;
HWND g_hStatusLabel = NULL;
HWND g_hDetailLabel = NULL;
HWND g_hWnd = NULL;

// GLOBAL connection handle - reused for ALL downloads (massive speed boost)
HINTERNET g_hInternet = NULL;

// Cancel flag
bool g_cancelRequested = false;

void ProcessMessages() {
  MSG msg;
  while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

void SetStatus(const std::string &text) {
  if (g_hStatusLabel)
    SetWindowTextA(g_hStatusLabel, text.c_str());
  ProcessMessages();
}

void SetDetail(const std::string &text) {
  if (g_hDetailLabel)
    SetWindowTextA(g_hDetailLabel, text.c_str());
  ProcessMessages();
}

std::string HttpGet(const std::string &url);

std::string UrlEncode(const std::string &value) {
  std::ostringstream encoded;
  for (char c : value) {
    if (c == '\\') {
      encoded << '/';
    } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
               c == '/') {
      encoded << c;
    } else {
      encoded << '%' << std::uppercase;
      encoded << "0123456789ABCDEF"[(c >> 4) & 0x0F];
      encoded << "0123456789ABCDEF"[c & 0x0F];
    }
  }
  return encoded.str();
}

// Fast download with progress reporting using the global persistent connection
bool DownloadFileWithProgress(const std::string &url, const std::string &path,
                              long long expectedSize, int fileIndex,
                              int totalFiles) {
  if (!g_hInternet || g_cancelRequested)
    return false;

  HINTERNET hUrl =
      InternetOpenUrlA(g_hInternet, url.c_str(), NULL, 0,
                       INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                           INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_NO_UI,
                       0);
  if (!hUrl)
    return false;

  DWORD statusCode = 0;
  DWORD statusLen = sizeof(statusCode);
  HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                 &statusCode, &statusLen, NULL);
  if (statusCode != 200) {
    InternetCloseHandle(hUrl);
    return false;
  }

  std::ofstream outFile(path, std::ios::binary);
  if (!outFile.is_open()) {
    InternetCloseHandle(hUrl);
    return false;
  }

  char buffer[65536]; // 64KB buffer for maximum throughput
  DWORD bytesRead;
  long long totalRead = 0;
  DWORD startTimeTick = GetTickCount();
  DWORD lastUpdateTick = startTimeTick;
  long long lastUpdateRead = 0;

  while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) &&
         bytesRead > 0) {
    outFile.write(buffer, bytesRead);
    totalRead += bytesRead;

    // Update progress at most every 100ms (avoid UI bottleneck)
    DWORD now = GetTickCount();
    if (now - lastUpdateTick > 150 || totalRead == expectedSize) {
      DWORD duration = now - lastUpdateTick;
      if (duration == 0)
        duration = 1;
      double speed = (double)(totalRead - lastUpdateRead) /
                     (duration / 1000.0) / 1048576.0;

      lastUpdateTick = now;
      lastUpdateRead = totalRead;

      if (expectedSize > 0) {
        int pct = (int)((totalRead * 100) / expectedSize);
        char buf[128];
        snprintf(buf, sizeof(buf), "%.1f / %.1f MB (%d%%) - %.2f MBps",
                 totalRead / 1048576.0, expectedSize / 1048576.0, pct, speed);
        SetDetail(buf);
      }
      ProcessMessages();
    }

    if (g_cancelRequested) {
      outFile.close();
      InternetCloseHandle(hUrl);
      DeleteFileA(path.c_str());
      return false;
    }
  }

  outFile.close();
  InternetCloseHandle(hUrl);
  return true;
}

bool ExtractZipWithPowerShell(const std::string &zipPath,
                              const std::string &destinationDir) {
  std::string command =
      "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive "
      "-LiteralPath '";
  command += zipPath;
  command += "' -DestinationPath '";
  command += destinationDir;
  command += "' -Force\"";

  STARTUPINFOA si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  char *mutableCmd = _strdup(command.c_str());
  if (!mutableCmd)
    return false;

  const BOOL ok =
      CreateProcessA(NULL, mutableCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                     NULL, NULL, &si, &pi);
  free(mutableCmd);
  if (!ok)
    return false;

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return exitCode == 0;
}

bool ExtractTarArchive(const std::string &tarPath,
                       const std::string &destinationDir) {
  std::string command = "tar -xf \"";
  command += tarPath;
  command += "\" -C \"";
  command += destinationDir;
  command += "\"";

  STARTUPINFOA si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  char *mutableCmd = _strdup(command.c_str());
  if (!mutableCmd)
    return false;

  const BOOL ok =
      CreateProcessA(NULL, mutableCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                     NULL, NULL, &si, &pi);
  free(mutableCmd);
  if (!ok)
    return false;

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return exitCode == 0;
}

bool TryFastPackageInstall(const std::string &baseUrl,
                           const std::string &destinationDir) {
  std::string packageInfo = HttpGet(baseUrl + "/package_info");
  long long packageSize = -1;
  if (!packageInfo.empty()) {
    size_t pos = packageInfo.find("\"size\":");
    if (pos != std::string::npos) {
      size_t qstart = packageInfo.find('"', pos + 7);
      size_t qend =
          qstart == std::string::npos ? std::string::npos
                                      : packageInfo.find('"', qstart + 1);
      if (qstart != std::string::npos && qend != std::string::npos)
        packageSize = _atoi64(packageInfo.substr(qstart + 1, qend - qstart - 1)
                                  .c_str());
    }
  }

  SetStatus("High-speed package detected - downloading full app...");
  SetDetail("Using single-package transfer for maximum network speed");
  const std::string tarPath = destinationDir + "\\quickstt_payload.tar";
  DeleteFileA(tarPath.c_str());

  if (!DownloadFileWithProgress(baseUrl + "/package.tar", tarPath, packageSize,
                                1, 1)) {
    DeleteFileA(tarPath.c_str());
    return false;
  }

  SetStatus("Extracting full app package...");
  SetDetail("Applying downloaded package...");
  const bool extracted = ExtractTarArchive(tarPath, destinationDir);
  DeleteFileA(tarPath.c_str());
  return extracted;
}

std::string HttpGet(const std::string &url) {
  if (!g_hInternet)
    return "";

  HINTERNET hUrl =
      InternetOpenUrlA(g_hInternet, url.c_str(), NULL, 0,
                       INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                           INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_NO_UI,
                       0);
  if (!hUrl)
    return "";

  DWORD statusCode = 0;
  DWORD statusLen = sizeof(statusCode);
  HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                 &statusCode, &statusLen, NULL);
  if (statusCode != 200) {
    InternetCloseHandle(hUrl);
    return "";
  }

  std::string result;
  result.reserve(1024 * 1024); // Pre-allocate 1MB
  char buffer[65536];
  DWORD bytesRead;
  while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) &&
         bytesRead > 0) {
    result.append(buffer, bytesRead);
  }

  InternetCloseHandle(hUrl);
  return result;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam,
                         LPARAM lParam) {
  switch (message) {
  case WM_COMMAND:
    if (LOWORD(wParam) == 1001) {
      g_cancelRequested = true;
      PostQuitMessage(0);
    }
    break;
  case WM_DESTROY:
    g_cancelRequested = true;
    PostQuitMessage(0);
    break;
  default:
    return DefWindowProc(hWnd, message, wParam, lParam);
  }
  return 0;
}

void CreateDirectories(const std::string &path) {
  size_t pos = 0;
  while ((pos = path.find_first_of("/\\", pos + 1)) != std::string::npos) {
    std::string dir = path.substr(0, pos);
    CreateDirectoryA(dir.c_str(), NULL);
  }
}

// Get SHA256 hash of a local file
std::string GetLocalFileHash(const std::string &path) {
  HCRYPTPROV hProv = 0;
  HCRYPTHASH hHash = 0;
  HANDLE hFile = INVALID_HANDLE_VALUE;
  std::string result = "";

  if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES,
                           CRYPT_VERIFYCONTEXT)) {
    return "";
  }

  if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
    CryptReleaseContext(hProv, 0);
    return "";
  }

  hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile != INVALID_HANDLE_VALUE) {
    BYTE buffer[65536];
    DWORD bytesRead = 0;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) &&
           bytesRead > 0) {
      CryptHashData(hHash, buffer, bytesRead, 0);
    }
    CloseHandle(hFile);

    BYTE rgbHash[32];
    DWORD cbHash = 32;
    if (CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &cbHash, 0)) {
      char hex[65];
      for (DWORD i = 0; i < cbHash; i++) {
        sprintf(hex + (i * 2), "%02x", rgbHash[i]);
      }
      result = std::string(hex);
    }
  }

  CryptDestroyHash(hHash);
  CryptReleaseContext(hProv, 0);
  return result;
}

// Get file size on disk
long long GetLocalFileSize(const std::string &path) {
  HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile == INVALID_HANDLE_VALUE)
    return -1;
  LARGE_INTEGER size;
  if (!GetFileSizeEx(hFile, &size)) {
    CloseHandle(hFile);
    return -1;
  }
  CloseHandle(hFile);
  return size.QuadPart;
}

struct FileEntry {
  std::string path;
  std::string hash;
  long long size;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
  if (!IsUserAnAdmin()) {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    SHELLEXECUTEINFOA sei = {sizeof(sei)};
    sei.lpVerb = "runas";
    sei.lpFile = exePath;
    sei.lpParameters = lpCmdLine;
    sei.nShow = nCmdShow;
    if (ShellExecuteExA(&sei)) {
      return 0; // successfully launched elevated instance
    }
    // If elevation was cancelled by user, continue anyway or exit?
    // Let's just warn them but continue.
    MessageBoxA(NULL,
                "Running without Administrator privileges.\nFirewall "
                "configurations may fail.",
                "Warning", MB_ICONWARNING);
  }

  INITCOMMONCONTROLSEX icex;
  icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
  icex.dwICC = ICC_PROGRESS_CLASS;
  InitCommonControlsEx(&icex);

  WNDCLASSEXA wcex = {sizeof(WNDCLASSEXA)};
  wcex.style = CS_HREDRAW | CS_VREDRAW;
  wcex.lpfnWndProc = WndProc;
  wcex.hInstance = hInstance;
  wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
  wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wcex.lpszClassName = "QuickSTTLoaderClass";
  RegisterClassExA(&wcex);

  int width = 480, height = 180;
  int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
  g_hWnd = CreateWindowA("QuickSTTLoaderClass", "QuickSTT Portable",
                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                         (sx - width) / 2, (sy - height) / 2, width, height,
                         NULL, NULL, hInstance, NULL);
  if (!g_hWnd)
    return 0;

  g_hStatusLabel = CreateWindowA("STATIC", "Initializing...",
                                 WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 15, 450,
                                 20, g_hWnd, NULL, hInstance, NULL);

  g_hProgressBar =
      CreateWindowA(PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 20,
                    45, 430, 22, g_hWnd, NULL, hInstance, NULL);

  g_hDetailLabel =
      CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_CENTER, 10, 75,
                    450, 20, g_hWnd, NULL, hInstance, NULL);

  HWND hSkipBtn = CreateWindowA("BUTTON", "Skip Update & Launch",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 140, 110,
                                200, 25, g_hWnd, (HMENU)1001, hInstance, NULL);

  // Check for --background flag
  bool backgroundMode = false;
  std::string cmdLine(lpCmdLine);
  if (cmdLine.find("--background") != std::string::npos) {
    backgroundMode = true;
  }

  // Set CWD to where this exe lives
  char exePathBuf[MAX_PATH];
  GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
  std::string baseDir(exePathBuf);
  baseDir = baseDir.substr(0, baseDir.find_last_of("\\/"));
  SetCurrentDirectoryA(baseDir.c_str());
  std::string mainAppPath = baseDir + "\\QuickSTT_App.exe";

  bool mainAppExists =
      GetFileAttributesA(mainAppPath.c_str()) != INVALID_FILE_ATTRIBUTES;

  if (backgroundMode && mainAppExists) {
    // Instant launch for background mode - no window, no updates
    goto launch;
  }

  ShowWindow(g_hWnd, nCmdShow);
  UpdateWindow(g_hWnd);
  ProcessMessages();

  {
    // Open ONE persistent internet connection (reused for everything)
    SetStatus("Connecting to update server...");
    g_hInternet =
        InternetOpenA("QuickSTT/1.1", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);

    // Set timeouts: 2s connect, 5s receive (VERY fast fallback)
    DWORD timeout = 2000;
    InternetSetOptionA(g_hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout,
                       sizeof(timeout));
    timeout = 5000;
    InternetSetOptionA(g_hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout,
                       sizeof(timeout));
    timeout = 2000;
    InternetSetOptionA(g_hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout,
                       sizeof(timeout));
    // Enable HTTP/1.1 keep-alive
    DWORD keepAlive = 1;
    InternetSetOptionA(g_hInternet, INTERNET_OPTION_HTTP_VERSION, &keepAlive,
                       sizeof(keepAlive));

    // Try each configured server URL until one responds
    std::vector<std::string> serverUrls = getServerUrls();
    std::string baseUrl;
    std::string manifest;

    for (size_t si = 0; si < serverUrls.size(); si++) {
      baseUrl = serverUrls[si];
      SetStatus("Trying server " + std::to_string(si + 1) + "/" +
                std::to_string(serverUrls.size()) + ": " + baseUrl);
      manifest = HttpGet(baseUrl + "/manifest_txt");
      if (!manifest.empty())
        break;
    }

    if (manifest.empty() && !mainAppExists) {
      MessageBoxA(g_hWnd,
                  "Cannot connect to update server and no local app found.\n\n"
                  "Ensure QuickSTT_App.exe is present in the same folder.",
                  "QuickSTT - Setup Required", MB_ICONERROR);
      if (g_hInternet)
        InternetCloseHandle(g_hInternet);
      return 1;
    }

    // If no server reachable but local app exists: just boot it
    // (offline/portable mode)
    if (manifest.empty() && mainAppExists) {
      SetStatus("Offline mode - launching local version");
      Sleep(300);
      if (g_hInternet)
        InternetCloseHandle(g_hInternet);
      goto launch;
    }

    if (!manifest.empty()) {
      if (!mainAppExists) {
        if (TryFastPackageInstall(baseUrl, baseDir)) {
          SetStatus("Full app installed - launching!");
          SetDetail("");
          Sleep(400);
          if (g_hInternet)
            InternetCloseHandle(g_hInternet);
          goto launch;
        }
        SetStatus("Package transfer unavailable - falling back to file sync...");
        SetDetail("");
      }

      // Parse manifest into structured entries
      std::stringstream ss(manifest);
      std::string line;
      std::vector<FileEntry> allFiles;

      while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        size_t firstPipe = line.find('|');
        size_t secondPipe = line.find('|', firstPipe + 1);
        if (firstPipe != std::string::npos && secondPipe != std::string::npos) {
          FileEntry entry;
          entry.path = line.substr(0, firstPipe);
          entry.hash = line.substr(firstPipe + 1, secondPipe - firstPipe - 1);
          entry.size = atoll(line.substr(secondPipe + 1).c_str());
          allFiles.push_back(entry);
        }
      }

      // --- VERSION CHECK: Only download if server has something newer ---
      std::string localVersion = "";
      std::ifstream lvf("version.txt");
      if (lvf.is_open()) {
        std::getline(lvf, localVersion);
        while (!localVersion.empty() &&
               (localVersion.back() == '\r' || localVersion.back() == '\n' ||
                localVersion.back() == ' '))
          localVersion.pop_back();
      }
      std::string serverVersionJson = HttpGet(baseUrl + "/check_update");
      std::string serverVersion = "";
      size_t vpos = serverVersionJson.find("\"version\":");
      if (vpos != std::string::npos) {
        size_t qstart = serverVersionJson.find('"', vpos + 10);
        size_t qend = serverVersionJson.find('"', qstart + 1);
        if (qstart != std::string::npos && qend != std::string::npos)
          serverVersion =
              serverVersionJson.substr(qstart + 1, qend - qstart - 1);
      }
      if (!serverVersion.empty() && !localVersion.empty() &&
          serverVersion == localVersion && mainAppExists) {
        SetStatus("Up to date (v" + localVersion + ") - launching!");
        Sleep(400);
        if (g_hInternet)
          InternetCloseHandle(g_hInternet);
        goto launch;
      }

      // Phase 1: Quick scan - determine what needs downloading
      SetStatus("Scanning local files...");
      ProcessMessages();

      std::vector<FileEntry> toDownload;
      long long totalDownloadSize = 0;
      int skipped = 0;

      for (const auto &entry : allFiles) {
        long long localSize = GetLocalFileSize(entry.path);
        if (localSize == entry.size) {
          // Even if size matches, check hash for absolute certainty
          // This prevents "stuck" updates where a binary changed but stayed the
          // same size
          std::string lHash = GetLocalFileHash(entry.path);
          if (lHash == entry.hash) {
            skipped++;
            continue;
          }
        }
        // Missing or wrong size/hash - needs download
        toDownload.push_back(entry);
        totalDownloadSize += entry.size;
      }

      if (toDownload.empty()) {
        SetStatus("All files up to date! (" + std::to_string(skipped) +
                  " verified)");
        SetDetail("");
      } else {
        // Sort by size (download small files first for visible progress)
        std::sort(toDownload.begin(), toDownload.end(),
                  [](const FileEntry &a, const FileEntry &b) {
                    return a.size < b.size;
                  });

        int total = (int)toDownload.size();
        SendMessage(g_hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, total));

        // Format total size
        char totalSizeBuf[64];
        if (totalDownloadSize > 1048576) {
          snprintf(totalSizeBuf, sizeof(totalSizeBuf), "%.1f MB",
                   totalDownloadSize / 1048576.0);
        } else {
          snprintf(totalSizeBuf, sizeof(totalSizeBuf), "%.0f KB",
                   totalDownloadSize / 1024.0);
        }

        SetStatus("Downloading " + std::to_string(total) + " files (" +
                  totalSizeBuf + ")...");

        // ONLY KILL APPLICATIONS IF WE ACTUALLY HAVE DOWNLOADS STARTING
        // This prevents the server from being killed if no update is actually
        // found.
        if (!backgroundMode) {
          SetDetail("Clearing workspace...");
          system("taskkill /F /IM QuickSTT_App.exe /T 2>nul");
          // If we are connecting to localhost or 127.0.0.1, we must NOT kill
          // the server app or we'll lose the source!
          if (baseUrl.find("127.0.0.1") == std::string::npos &&
              baseUrl.find("localhost") == std::string::npos) {
            system("taskkill /F /IM QuickSTT_Server_App.exe /T 2>nul");
          }
          Sleep(500);
          SetDetail("");
        }

        int downloaded = 0, failed = 0;
        long long downloadedBytes = 0;
        bool zipDownloaded = false;

        for (int i = 0; i < total && !g_cancelRequested; ++i) {
          const FileEntry &entry = toDownload[i];

          // Show what we're downloading
          std::string shortName = entry.path;
          if (shortName.length() > 40) {
            shortName = "..." + shortName.substr(shortName.length() - 37);
          }

          SetStatus("[" + std::to_string(i + 1) + "/" + std::to_string(total) +
                    "] " + shortName);

          CreateDirectories(entry.path);
          std::string encodedName = UrlEncode(entry.path);
          if (DownloadFileWithProgress(baseUrl + "/download/" + encodedName,
                                       entry.path, entry.size, i + 1, total)) {
            downloaded++;
            downloadedBytes += entry.size;
            if (entry.path.find(".zip") != std::string::npos) {
              zipDownloaded = true;
            }
          } else {
            failed++;
          }
          SendMessage(g_hProgressBar, PBM_SETPOS, i + 1, 0);
        }

        // Summary
        char summaryBuf[256];
        if (downloadedBytes > 1048576) {
          snprintf(summaryBuf, sizeof(summaryBuf), "%d downloaded (%.1f MB)",
                   downloaded, downloadedBytes / 1048576.0);
        } else {
          snprintf(summaryBuf, sizeof(summaryBuf), "%d downloaded (%.0f KB)",
                   downloaded, downloadedBytes / 1024.0);
        }
        std::string summary(summaryBuf);
        if (skipped > 0)
          summary += ", " + std::to_string(skipped) + " cached";
        if (failed > 0)
          summary += ", " + std::to_string(failed) + " failed";
        SetStatus(summary);
        SetDetail("");

        if (zipDownloaded) {
          SetStatus("Extracting core environment... Please wait.");
          ProcessMessages();
          // Assuming _internal.zip
          system("powershell -WindowStyle Hidden -Command \"Expand-Archive "
                 "-Force -Path '_internal.zip' -DestinationPath '.'\"");
          SetStatus("Extraction Complete.");
        }

        // ── Write updated version so next launch is instant ──────────────
        if (!serverVersion.empty() && failed == 0) {
          std::ofstream vout("version.txt", std::ios::out | std::ios::trunc);
          if (vout.is_open()) {
            vout << serverVersion;
            vout.close();
          }
        }
      }
    } else {
      SetStatus("Offline mode - using local files");
    }

    // Close global connection
    if (g_hInternet)
      InternetCloseHandle(g_hInternet);
  }

launch:
  Sleep(500);
  SetStatus("Launching QuickSTT...");
  SetDetail("");

  STARTUPINFOA si = {sizeof(si)};
  PROCESS_INFORMATION pi;
  std::string launchCommand = "\"" + mainAppPath + "\"";
  if (backgroundMode) {
    launchCommand += " --background";
  }

  std::vector<char> cmdBuf(launchCommand.begin(), launchCommand.end());
  cmdBuf.push_back('\0');

  if (CreateProcessA(mainAppPath.c_str(), cmdBuf.data(), NULL, NULL, FALSE, 0,
                     NULL, baseDir.c_str(), &si, &pi)) {
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  } else {
    MessageBoxA(g_hWnd,
                "Failed to launch QuickSTT_App.exe\n\n"
                "The application files may not have downloaded correctly.\n"
                "Please try running QuickSTT.exe again.",
                "QuickSTT - Launch Error", MB_ICONERROR);
  }

  return 0;
}
