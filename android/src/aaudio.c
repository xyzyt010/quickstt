// Minimal AAudio recording shim for Android (NDK, API 26+).
//
// Rust cannot call the NDK's AAudio C API directly, and cpal has no Android
// backend, so this tiny C shim does the recording and hands raw mono float
// frames to Rust.
//
// Design notes:
//   * AAUDIO_DIRECTION_INPUT is set so the stream captures from the microphone.
//   * PCM_FLOAT is requested first; if the device refuses float recording,
//     it falls back to PCM_I16 and converts to float on the fly.
//   * Mono is requested first, with stereo fallback downmixed in Rust.
//   * All functions are safe to call from any thread; the stream handle is
//     guarded by the caller (the Rust capture thread).

#include <aaudio/AAudio.h>
#include <stdint.h>
#include <stdlib.h>

static AAudioStream *g_stream = NULL;
static int32_t g_channels = 1;
static aaudio_format_t g_format = AAUDIO_FORMAT_PCM_FLOAT;

int qs_audio_start(int32_t sample_rate) {
    if (g_stream != NULL) {
        return 0; // already recording
    }
    const aaudio_format_t formats[2] = {
        AAUDIO_FORMAT_PCM_FLOAT,
        AAUDIO_FORMAT_PCM_I16
    };
    const int32_t channel_counts[2] = {1, 2};
    const aaudio_sharing_mode_t sharing_modes[2] = {
        AAUDIO_SHARING_MODE_SHARED,
        AAUDIO_SHARING_MODE_EXCLUSIVE
    };
    const aaudio_performance_mode_t perf_modes[2] = {
        AAUDIO_PERFORMANCE_MODE_LOW_LATENCY,
        AAUDIO_PERFORMANCE_MODE_NONE
    };

    for (int f = 0; f < 2; ++f) {
        for (int c = 0; c < 2; ++c) {
            for (int s = 0; s < 2; ++s) {
                for (int p = 0; p < 2; ++p) {
                    AAudioStreamBuilder *builder = NULL;
                    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
                        return -1;
                    }
                    // Crucial: Must be INPUT for microphone recording!
                    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
                    AAudioStreamBuilder_setPerformanceMode(builder, perf_modes[p]);
                    AAudioStreamBuilder_setSharingMode(builder, sharing_modes[s]);
                    AAudioStreamBuilder_setFormat(builder, formats[f]);
                    AAudioStreamBuilder_setChannelCount(builder, channel_counts[c]);
                    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
                    AAudioStreamBuilder_setDataCallback(builder, NULL, NULL);

                    aaudio_result_t res = AAudioStreamBuilder_openStream(builder, &g_stream);
                    AAudioStreamBuilder_delete(builder);

                    if (res == AAUDIO_OK && g_stream != NULL) {
                        g_channels = channel_counts[c];
                        g_format = AAudioStream_getFormat(g_stream);
                        if (AAudioStream_requestStart(g_stream) == AAUDIO_OK) {
                            return 0;
                        }
                        AAudioStream_close(g_stream);
                        g_stream = NULL;
                        return -3;
                    }
                    g_stream = NULL;
                }
            }
        }
    }
    return -2;
}

/// Read up to `frames` frames. Writes interleaved float samples (channels
/// per frame) and returns the number of FRAMES read, or a negative error.
/// The read blocks up to 100 ms so the caller can poll its stop flag.
int32_t qs_audio_read(float *out, int32_t frames) {
    if (g_stream == NULL) {
        return -1;
    }
    int32_t done = 0;
    const int64_t timeout_ns = 100000000LL; // 100 ms

    if (g_format == AAUDIO_FORMAT_PCM_FLOAT) {
        while (done < frames) {
            int32_t remaining_frames = frames - done;
            aaudio_result_t r = AAudioStream_read(
                g_stream,
                out + (int64_t)done * g_channels,
                remaining_frames,
                timeout_ns
            );
            if (r < 0) {
                return -2;
            }
            if (r == 0) {
                break; // timeout
            }
            done += r;
        }
    } else {
        // Device provides PCM_I16: read into scratch and convert to normalized float (-1.0 to 1.0)
        int16_t scratch[1024];
        int32_t max_scratch_frames = 1024 / g_channels;
        while (done < frames) {
            int32_t to_read = frames - done;
            if (to_read > max_scratch_frames) {
                to_read = max_scratch_frames;
            }
            aaudio_result_t r = AAudioStream_read(
                g_stream,
                scratch,
                to_read,
                timeout_ns
            );
            if (r < 0) {
                return -2;
            }
            if (r == 0) {
                break;
            }
            int32_t total_samples = r * g_channels;
            float *dest = out + (int64_t)done * g_channels;
            for (int32_t i = 0; i < total_samples; ++i) {
                dest[i] = (float)scratch[i] / 32768.0f;
            }
            done += r;
        }
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
    g_format = AAUDIO_FORMAT_PCM_FLOAT;
}
