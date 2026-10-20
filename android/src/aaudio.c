// Minimal AAudio recording shim for Android (NDK, API 26+).
//
// Rust cannot call the NDK's AAudio C API directly, and cpal has no Android
// backend, so this tiny C shim does the recording and hands raw mono float
// frames to Rust.
//
// Design notes:
//   * PCM_FLOAT is requested first (what we want); if the device refuses a
//     mono float stream we retry with stereo and downmix in Rust.
//   * AAudio does NOT convert formats/channels, so the fallback matters.
//   * All functions are safe to call from any thread; the stream handle is
//     guarded by the caller (the Rust side only touches it from the capture
//     thread).

#include <aaudio/AAudio.h>
#include <stdint.h>

static AAudioStream *g_stream = NULL;
static int32_t g_channels = 1;

int qs_audio_start(int32_t sample_rate) {
    if (g_stream != NULL) {
        return 0; // already recording
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        int32_t channels = (attempt == 0) ? 1 : 2;
        AAudioStreamBuilder *builder = NULL;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
            return -1;
        }
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, channels);
        AAudioStreamBuilder_setSampleRate(builder, sample_rate);
        AAudioStreamBuilder_setDataCallback(builder, NULL, NULL);
        aaudio_result_t res = AAudioStreamBuilder_openStream(builder, &g_stream);
        AAudioStreamBuilder_delete(builder);
        if (res == AAUDIO_OK && g_stream != NULL) {
            g_channels = channels;
            if (AAudioStream_requestStart(g_stream) == AAUDIO_OK) {
                return 0;
            }
            AAudioStream_close(g_stream);
            g_stream = NULL;
            return -3;
        }
        g_stream = NULL;
    }
    return -2;
}

/// Read up to `frames` samples. Writes interleaved float samples (channels
/// per frame) and returns the number of FRAMES read, or a negative error.
/// The read blocks up to 100 ms so the caller can poll its stop flag.
int32_t qs_audio_read(float *out, int32_t frames) {
    if (g_stream == NULL) {
        return -1;
    }
    int32_t done = 0;
    while (done < frames) {
        aaudio_result_t r = AAudioStream_read(
            g_stream, out + (int64_t)done * g_channels,
            (frames - done) * g_channels * sizeof(float), 1000000000LL /* 100 ms */);
        if (r < 0) {
            return -2;
        }
        if (r == 0) {
            break; // timed out without data
        }
        done += r;
    }
    return done;
}

int32_t qs_audio_channels(void) {
    return g_channels;
}

int32_t qs_audio_sample_rate(void) {
    if (g_stream == NULL) {
        return 0;
    }
    return AAudioStream_getSampleRate(g_stream);
}

void qs_audio_stop(void) {
    if (g_stream == NULL) {
        return;
    }
    AAudioStream_requestStop(g_stream);
    AAudioStream_close(g_stream);
    g_stream = NULL;
    g_channels = 1;
}
