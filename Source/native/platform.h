// platform.h — cross-platform dynamic library abstraction for QuickSTT native service
// Windows: uses LoadLibraryA / GetProcAddress / FreeLibrary
// Linux/macOS: uses dlopen / dlsym / dlclose
#pragma once

#include <string>

#ifdef _WIN32
  #include <windows.h>
  using platform_handle_t = HMODULE;
  inline platform_handle_t platform_load(const char* path) { return LoadLibraryA(path); }
  inline void platform_unload(platform_handle_t h) { if (h) FreeLibrary(h); }
  inline void* platform_symbol(platform_handle_t h, const char* sym) { return reinterpret_cast<void*>(GetProcAddress(h, sym)); }
  inline std::string platform_lib_name(const std::string& base) { return base + ".dll"; }
#else
  #include <dlfcn.h>
  using platform_handle_t = void*;
  inline platform_handle_t platform_load(const char* path) { return dlopen(path, RTLD_NOW); }
  inline void platform_unload(platform_handle_t h) { if (h) dlclose(h); }
  inline void* platform_symbol(platform_handle_t h, const char* sym) { return dlsym(h, sym); }
  inline std::string platform_lib_name(const std::string& base) { return "lib" + base + ".so"; }
#endif

// Helper: join path in a cross-platform way (always uses std::filesystem::path)
#include <filesystem>
#include <string>
inline std::string platform_join(const std::string& a, const std::string& b) {
    return (std::filesystem::path(a) / b).string();
}
inline std::string platform_join3(const std::string& a, const std::string& b, const std::string& c) {
    return (std::filesystem::path(a) / b / c).string();
}
