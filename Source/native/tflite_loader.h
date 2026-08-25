// tflite_loader.h — Dynamic loader for TensorFlow Lite C API (tensorflowlite_c.dll / libtensorflowlite_c.so)
// Loads all needed TFLite C API functions at runtime via LoadLibrary/GetProcAddress (Win) or dlopen/dlsym (Linux)
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "platform.h"
#include <filesystem>

// Forward-declare opaque TFLite types
typedef struct TfLiteModel TfLiteModel;
typedef struct TfLiteInterpreterOptions TfLiteInterpreterOptions;
typedef struct TfLiteInterpreter TfLiteInterpreter;
typedef struct TfLiteTensor TfLiteTensor;

// TfLiteStatus enum
typedef enum { kTfLiteOk = 0, kTfLiteError = 1 } TfLiteStatus;

// TfLiteType enum (subset we need)
typedef enum {
  kTfLiteFloat32 = 1,
  kTfLiteInt32 = 2,
  kTfLiteUInt8 = 3,
  kTfLiteInt16 = 7,
} TfLiteType;

struct TfLiteLoader {
  platform_handle_t handle = nullptr;

  // Function pointers
  using FnVersion = const char *(*)();
  using FnModelCreateFromFile = TfLiteModel *(*)(const char *);
  using FnModelDelete = void (*)(TfLiteModel *);
  using FnOptionsCreate = TfLiteInterpreterOptions *(*)();
  using FnOptionsDelete = void (*)(TfLiteInterpreterOptions *);
  using FnOptionsSetNumThreads = void (*)(TfLiteInterpreterOptions *, int32_t);
  using FnInterpreterCreate = TfLiteInterpreter *(*)(const TfLiteModel *,
                                                     const TfLiteInterpreterOptions *);
  using FnInterpreterDelete = void (*)(TfLiteInterpreter *);
  using FnAllocateTensors = TfLiteStatus (*)(TfLiteInterpreter *);
  using FnInvoke = TfLiteStatus (*)(TfLiteInterpreter *);
  using FnGetInputTensorCount = int32_t (*)(const TfLiteInterpreter *);
  using FnGetInputTensor = TfLiteTensor *(*)(const TfLiteInterpreter *, int32_t);
  using FnGetOutputTensorCount = int32_t (*)(const TfLiteInterpreter *);
  using FnGetOutputTensor = const TfLiteTensor *(*)(const TfLiteInterpreter *, int32_t);
  using FnResizeInputTensor = TfLiteStatus (*)(TfLiteInterpreter *, int32_t, const int *, int32_t);
  using FnTensorType = TfLiteType (*)(const TfLiteTensor *);
  using FnTensorNumDims = int32_t (*)(const TfLiteTensor *);
  using FnTensorDim = int32_t (*)(const TfLiteTensor *, int32_t);
  using FnTensorByteSize = size_t (*)(const TfLiteTensor *);
  using FnTensorData = void *(*)(const TfLiteTensor *);
  using FnTensorCopyFromBuffer = TfLiteStatus (*)(TfLiteTensor *, const void *, size_t);
  using FnTensorCopyToBuffer = TfLiteStatus (*)(const TfLiteTensor *, void *, size_t);
  using FnTensorName = const char *(*)(const TfLiteTensor *);

  FnVersion version = nullptr;
  FnModelCreateFromFile modelCreateFromFile = nullptr;
  FnModelDelete modelDelete = nullptr;
  FnOptionsCreate optionsCreate = nullptr;
  FnOptionsDelete optionsDelete = nullptr;
  FnOptionsSetNumThreads optionsSetNumThreads = nullptr;
  FnInterpreterCreate interpreterCreate = nullptr;
  FnInterpreterDelete interpreterDelete = nullptr;
  FnAllocateTensors allocateTensors = nullptr;
  FnInvoke invoke = nullptr;
  FnGetInputTensorCount getInputTensorCount = nullptr;
  FnGetInputTensor getInputTensor = nullptr;
  FnGetOutputTensorCount getOutputTensorCount = nullptr;
  FnGetOutputTensor getOutputTensor = nullptr;
  FnResizeInputTensor resizeInputTensor = nullptr;
  FnTensorType tensorType = nullptr;
  FnTensorNumDims tensorNumDims = nullptr;
  FnTensorDim tensorDim = nullptr;
  FnTensorByteSize tensorByteSize = nullptr;
  FnTensorData tensorData = nullptr;
  FnTensorCopyFromBuffer tensorCopyFromBuffer = nullptr;
  FnTensorCopyToBuffer tensorCopyToBuffer = nullptr;
  FnTensorName tensorName = nullptr;

  bool loaded() const { return handle != nullptr; }

  bool load(const std::string &dir) {
#ifdef _WIN32
    std::string dllPath = (std::filesystem::path(dir) / "tensorflowlite_c.dll").string();
    handle = platform_load(dllPath.c_str());
    if (!handle) handle = platform_load("tensorflowlite_c.dll");
    const char* libLabel = "tensorflowlite_c.dll";
#else
    std::string dllPath = (std::filesystem::path(dir) / "libtensorflowlite_c.so").string();
    handle = platform_load(dllPath.c_str());
    if (!handle) handle = platform_load("libtensorflowlite_c.so");
    const char* libLabel = "libtensorflowlite_c.so";
#endif
    if (!handle) {
      fprintf(stderr, "[TFLITE] Failed to load %s\n", libLabel);
      return false;
    }

#define LOAD_FN(name, sym)                                                     \
  name = (decltype(name))platform_symbol(handle, sym);                          \
  if (!name) {                                                                 \
    fprintf(stderr, "[TFLITE] Missing symbol: %s\n", sym);                     \
    platform_unload(handle);                                                   \
    handle = nullptr;                                                          \
    return false;                                                              \
   }

    LOAD_FN(version, "TfLiteVersion");
    LOAD_FN(modelCreateFromFile, "TfLiteModelCreateFromFile");
    LOAD_FN(modelDelete, "TfLiteModelDelete");
    LOAD_FN(optionsCreate, "TfLiteInterpreterOptionsCreate");
    LOAD_FN(optionsDelete, "TfLiteInterpreterOptionsDelete");
    LOAD_FN(optionsSetNumThreads, "TfLiteInterpreterOptionsSetNumThreads");
    LOAD_FN(interpreterCreate, "TfLiteInterpreterCreate");
    LOAD_FN(interpreterDelete, "TfLiteInterpreterDelete");
    LOAD_FN(allocateTensors, "TfLiteInterpreterAllocateTensors");
    LOAD_FN(invoke, "TfLiteInterpreterInvoke");
    LOAD_FN(getInputTensorCount, "TfLiteInterpreterGetInputTensorCount");
    LOAD_FN(getInputTensor, "TfLiteInterpreterGetInputTensor");
    LOAD_FN(getOutputTensorCount, "TfLiteInterpreterGetOutputTensorCount");
    LOAD_FN(getOutputTensor, "TfLiteInterpreterGetOutputTensor");
    LOAD_FN(resizeInputTensor, "TfLiteInterpreterResizeInputTensor");
    LOAD_FN(tensorType, "TfLiteTensorType");
    LOAD_FN(tensorNumDims, "TfLiteTensorNumDims");
    LOAD_FN(tensorDim, "TfLiteTensorDim");
    LOAD_FN(tensorByteSize, "TfLiteTensorByteSize");
    LOAD_FN(tensorData, "TfLiteTensorData");
    LOAD_FN(tensorCopyFromBuffer, "TfLiteTensorCopyFromBuffer");
    LOAD_FN(tensorCopyToBuffer, "TfLiteTensorCopyToBuffer");
    LOAD_FN(tensorName, "TfLiteTensorName");

#undef LOAD_FN

    fprintf(stderr, "[TFLITE] %s loaded OK (v%s)\n",
#ifdef _WIN32
            "tensorflowlite_c.dll",
#else
            "libtensorflowlite_c.so",
#endif
            version());
    return true;
  }

  void unload() {
    if (handle) {
      platform_unload(handle);
      handle = nullptr;
    }
  }
};
