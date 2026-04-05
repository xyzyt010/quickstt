// vosk_api.h — Dynamic loader for libvosk.dll (Vosk C API)
// No import library (.lib) needed — loads via LoadLibrary/GetProcAddress
#pragma once
#include <stdexcept>
#include <string>
#include <windows.h>


// Opaque handles (same as Vosk C API)
typedef struct VoskModel VoskModel;
typedef struct VoskRecognizer VoskRecognizer;

// Function pointer types matching libvosk exports
typedef VoskModel *(*fn_vosk_model_new)(const char *model_path);
typedef void (*fn_vosk_model_free)(VoskModel *model);
typedef VoskRecognizer *(*fn_vosk_recognizer_new)(VoskModel *model,
                                                  float sample_rate);
typedef void (*fn_vosk_recognizer_free)(VoskRecognizer *recognizer);
typedef int (*fn_vosk_recognizer_accept_waveform)(VoskRecognizer *r,
                                                  const char *data, int length);
typedef const char *(*fn_vosk_recognizer_result)(VoskRecognizer *r);
typedef const char *(*fn_vosk_recognizer_partial_result)(VoskRecognizer *r);
typedef const char *(*fn_vosk_recognizer_final_result)(VoskRecognizer *r);
typedef void (*fn_vosk_recognizer_reset)(VoskRecognizer *r);
typedef void (*fn_vosk_set_log_level)(int log_level);

struct VoskAPI {
  HMODULE dll = nullptr;

  fn_vosk_model_new model_new = nullptr;
  fn_vosk_model_free model_free = nullptr;
  fn_vosk_recognizer_new recognizer_new = nullptr;
  fn_vosk_recognizer_free recognizer_free = nullptr;
  fn_vosk_recognizer_accept_waveform recognizer_accept_waveform = nullptr;
  fn_vosk_recognizer_result recognizer_result = nullptr;
  fn_vosk_recognizer_partial_result recognizer_partial_result = nullptr;
  fn_vosk_recognizer_final_result recognizer_final_result = nullptr;
  fn_vosk_recognizer_reset recognizer_reset = nullptr;
  fn_vosk_set_log_level set_log_level = nullptr;

  bool load(const std::string &dll_path) {
    dll = LoadLibraryA(dll_path.c_str());
    if (!dll)
      return false;

#define LOAD(name)                                                             \
  name = (fn_vosk_##name)GetProcAddress(dll, "vosk_" #name);                   \
  if (!name)                                                                   \
    return false;
    LOAD(model_new)
    LOAD(model_free)
    LOAD(recognizer_new)
    LOAD(recognizer_free)
    LOAD(recognizer_accept_waveform)
    LOAD(recognizer_result)
    LOAD(recognizer_partial_result)
    LOAD(recognizer_final_result)
    LOAD(recognizer_reset)
    LOAD(set_log_level)
#undef LOAD
    return true;
  }

  void unload() {
    if (dll) {
      FreeLibrary(dll);
      dll = nullptr;
    }
  }
};
