// ort_loader.h — Minimal dynamic loader for ONNX Runtime (Windows/Linux)
#pragma once
#include "onnxruntime_c_api.h"
#include <string>
#include "platform.h"


struct OrtLoader {
  platform_handle_t dll = nullptr;
  const OrtApi *api = nullptr;

  bool load(const std::string &dll_path) {
    dll = platform_load(dll_path.c_str());
    if (!dll)
      return false;

    typedef const OrtApiBase *(ORT_API_CALL * FnGetApiBase)(void);
    auto getApiBase = (FnGetApiBase)platform_symbol(dll, "OrtGetApiBase");
    if (!getApiBase) {
      platform_unload(dll);
      dll = nullptr;
      return false;
    }

    auto base = getApiBase();
    if (!base) {
      platform_unload(dll);
      dll = nullptr;
      return false;
    }

    api = base->GetApi(ORT_API_VERSION);
    return api != nullptr;
  }

  void unload() {
    if (dll) {
      platform_unload(dll);
      dll = nullptr;
    }
    api = nullptr;
  }
};
