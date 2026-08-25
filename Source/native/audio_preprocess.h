#ifndef QUICKSTT_AUDIO_PREPROCESS_H
#define QUICKSTT_AUDIO_PREPROCESS_H

#include <cstdint>
#include <string>
#include <vector>

struct AudioPreprocessResult {
  std::vector<int16_t> samples;
  bool speechLikely = true;
  float vadProbability = 1.0f;
  bool vadAvailable = false;
};

class AudioPreprocessor {
public:
  bool init(const std::string &exeDir);
  void shutdown();
  AudioPreprocessResult process(const std::vector<int16_t> &input);
  bool hasRnnoise() const { return m_rnnoiseReady; }
  bool hasTenVad() const { return m_tenVadReady; }

private:
  void initRnnoise(const std::vector<std::string> &roots);
  void initTenVad(const std::vector<std::string> &roots);
  std::vector<int16_t> processRnnoise(const std::vector<int16_t> &input);
  void updateTenVad(const std::vector<int16_t> &samples,
                    AudioPreprocessResult &result);

  void *m_rnnoiseDll = nullptr;
  void *m_rnnoiseState = nullptr;
  int m_rnnoiseFrameSize = 0;
  using RnnoiseCreateFn = void *(*)(void *);
  using RnnoiseDestroyFn = void (*)(void *);
  using RnnoiseProcessFrameFn = float (*)(void *, float *, const float *);
  using RnnoiseGetFrameSizeFn = int (*)();
  RnnoiseDestroyFn m_rnnoiseDestroy = nullptr;
  RnnoiseProcessFrameFn m_rnnoiseProcessFrame = nullptr;
  std::vector<int16_t> m_rnnoisePending16k;
  bool m_rnnoiseReady = false;

  void *m_tenVadDll = nullptr;
  void *m_tenVadHandle = nullptr;
  using TenVadCreateFn = int (*)(void **, size_t, float);
  using TenVadProcessFn = int (*)(void *, const int16_t *, size_t, float *, int *);
  using TenVadDestroyFn = int (*)(void **);
  using TenVadVersionFn = const char *(*)();
  TenVadProcessFn m_tenVadProcess = nullptr;
  TenVadDestroyFn m_tenVadDestroy = nullptr;
  std::vector<int16_t> m_tenVadPending;
  int m_tenVadSpeechHoldFrames = 0;
  float m_lastVadProbability = 1.0f;
  bool m_tenVadReady = false;
};

#endif // QUICKSTT_AUDIO_PREPROCESS_H
