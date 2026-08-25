// pv_native.h — Picovoice Porcupine (Windows/Linux)
#pragma once

#include <string>
#include <vector>
#include "platform.h"
#include <cstdint>

typedef int32_t pv_status_t;
struct pv_porcupine;
typedef struct pv_porcupine pv_porcupine_t;

// Function pointers
typedef pv_status_t (*pv_porcupine_init_func)(
    const char *access_key,
    const char *model_path,
    const char *device,
    int32_t num_keywords,
    const char * const *keyword_paths,
    const float *sensitivities,
    pv_porcupine_t **object);

typedef pv_status_t (*pv_porcupine_process_func)(
    pv_porcupine_t *object,
    const int16_t *pcm,
    int32_t *keyword_index);

typedef void (*pv_porcupine_delete_func)(pv_porcupine_t *object);
typedef const char * (*pv_status_to_string_func)(pv_status_t status);
typedef int32_t (*pv_porcupine_frame_length_func)(void);
typedef int32_t (*pv_sample_rate_func)(void);
typedef void (*pv_set_sdk_func)(const char *sdk);

class NativePicovoiceDetector {
public:
    NativePicovoiceDetector() = default;
    ~NativePicovoiceDetector() { cleanup(); }

    bool load_dll(const std::string& dllPath) {
        if (hModule) return true;
#ifdef _WIN32
        hModule = LoadLibraryExA(dllPath.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hModule) {
            fprintf(stderr, "[PICOVOICE] Failed to load DLL (%s). Error Code: %d\n", dllPath.c_str(), (int)GetLastError());
            return false;
        }
        fprintf(stderr, "[PICOVOICE] LoadLibraryExA Succeeded.\n");
#else
        hModule = platform_load(dllPath.c_str());
        if (!hModule) {
            fprintf(stderr, "[PICOVOICE] Failed to load %s: %s\n", dllPath.c_str(), dlerror());
            return false;
        }
        fprintf(stderr, "[PICOVOICE] dlopen Succeeded.\n");
#endif
        pv_init = (pv_porcupine_init_func)platform_symbol(hModule, "pv_porcupine_init");
        pv_proc = (pv_porcupine_process_func)platform_symbol(hModule, "pv_porcupine_process");
        pv_del  = (pv_porcupine_delete_func)platform_symbol(hModule, "pv_porcupine_delete");
        pv_err  = (pv_status_to_string_func)platform_symbol(hModule, "pv_status_to_string");
        pv_len  = (pv_porcupine_frame_length_func)platform_symbol(hModule, "pv_porcupine_frame_length");
        pv_rate = (pv_sample_rate_func)platform_symbol(hModule, "pv_sample_rate");
        pv_sdk  = (pv_set_sdk_func)platform_symbol(hModule, "pv_set_sdk");

        return pv_init && pv_proc && pv_del && pv_err && pv_len;
    }

    bool init(const std::string& access_key, const std::string& model_path, const std::vector<std::string>& keyword_paths, float sensitivity) {
        cleanup_porcupine();
        if (!hModule || keyword_paths.empty() || access_key.empty()) return false;

        frame_length = pv_len();
        std::vector<const char*> paths_raw;
        std::vector<float> sens_raw;
        for (const auto& kp : keyword_paths) {
            paths_raw.push_back(kp.c_str());
            sens_raw.push_back(sensitivity);
        }

        if (pv_sdk) {
            pv_sdk("python");
        }

        fprintf(stderr, "[PICOVOICE] Calling pv_porcupine_init...\n");
        pv_status_t status = pv_init(
            access_key.c_str(),
            model_path.c_str(),
            "cpu",
            (int32_t)paths_raw.size(),
            paths_raw.data(),
            sens_raw.data(),
            &handle
        );
        fprintf(stderr, "[PICOVOICE] pv_porcupine_init returned block\n");

        if (status != 0) { // PV_STATUS_SUCCESS
            fprintf(stderr, "[PICOVOICE] Init Error: %s\n", pv_err(status));
            return false;
        }

        active = true;
        return true;
    }

    int32_t predict(const int16_t* pcm) {
        if (!active || !handle) return -1;
        int32_t keyword_index = -1;
        pv_status_t status = pv_proc(handle, pcm, &keyword_index);
        if (status != 0) {
            return -1;
        }
        return keyword_index;
    }

    void cleanup_porcupine() {
        if (handle && pv_del) {
            pv_del(handle);
            handle = nullptr;
        }
        active = false;
    }

    void cleanup() {
        cleanup_porcupine();
        if (hModule) {
            platform_unload(hModule);
            hModule = nullptr;
        }
    }

    bool active = false;
    int32_t frame_length = 512;

private:
    platform_handle_t hModule = nullptr;
    pv_porcupine_t* handle = nullptr;

    pv_porcupine_init_func pv_init = nullptr;
    pv_porcupine_process_func pv_proc = nullptr;
    pv_porcupine_delete_func pv_del = nullptr;
    pv_status_to_string_func pv_err = nullptr;
    pv_porcupine_frame_length_func pv_len = nullptr;
    pv_sample_rate_func pv_rate = nullptr;
    pv_set_sdk_func pv_sdk = nullptr;
};
