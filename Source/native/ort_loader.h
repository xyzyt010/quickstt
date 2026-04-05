// ort_loader.h — Minimal dynamic loader for ONNX Runtime
// Uses the official onnxruntime_c_api.h header — no type redeclarations
#pragma once
#include "onnxruntime_c_api.h"
#include <string>
#include <windows.h>


struct OrtLoader {
  HMODULE dll = nullptr;
  const OrtApi *api = nullptr;

  bool load(const std::string &dll_path) {
    dll = LoadLibraryA(dll_path.c_str());
    if (!dll)
      return false;

    typedef const OrtApiBase *(ORT_API_CALL * FnGetApiBase)(void);
    auto getApiBase = (FnGetApiBase)GetProcAddress(dll, "OrtGetApiBase");
    if (!getApiBase) {
      FreeLibrary(dll);
      dll = nullptr;
      return false;
    }

    auto base = getApiBase();
    if (!base) {
      FreeLibrary(dll);
      dll = nullptr;
      return false;
    }

    api = base->GetApi(ORT_API_VERSION);
    return api != nullptr;
  }

  void unload() {
    if (dll) {
      FreeLibrary(dll);
      dll = nullptr;
    }
    api = nullptr;
  }
};
