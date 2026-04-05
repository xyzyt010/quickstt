// vosk_engine.cpp — Pure C++ STT Engine
// Audio: PortAudio (C library)
// STT:   Vosk C API
// Wakeword: Pluggable (Porcupine / Precise / Snowman / MicroWWD)
// Zero Python dependency.

#include "native/portaudio.h"
#include "vosk_api.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>

// ─── Forward ─────────────────────────────────────────────────────────────────
void sendStatus(int code, const QString &msg);

// ─── WAV helpers ─────────────────────────────────────────────────────────────
static void writeWavHeader(QFile &f, int sampleRate, int channels,
                           int bitsPerSample) {
  QByteArray hdr(44, '\0');
  f.write(hdr);
}

static void finaliseWavHeader(QFile &f, int sampleRate, int channels,
                              int bitsPerSample) {
  qint64 dataBytes = f.size() - 44;
  f.seek(0);
  QDataStream ds(&f);
  ds.setByteOrder(QDataStream::LittleEndian);
  f.write("RIFF", 4);
  ds << (quint32)(36 + dataBytes);
  f.write("WAVE", 4);
  f.write("fmt ", 4);
  ds << (quint32)16;
  ds << (quint16)1; // PCM
  ds << (quint16)channels;
  ds << (quint32)sampleRate;
  ds << (quint32)(sampleRate * channels * bitsPerSample / 8);
  ds << (quint16)(channels * bitsPerSample / 8);
  ds << (quint16)bitsPerSample;
  f.write("data", 4);
  ds << (quint32)dataBytes;
}

// ─── StdinReader ─────────────────────────────────────────────────────────────
class StdinReader : public QThread {
  Q_OBJECT
public:
  void run() override {
    std::string line;
    while (std::getline(std::cin, line)) {
      emit lineReceived(QString::fromStdString(line).trimmed());
    }
  }
signals:
  void lineReceived(const QString &line);
};

// ─── VoskEngine ──────────────────────────────────────────────────────────────
class VoskEngine : public QObject {
  Q_OBJECT
public:
  VoskEngine() {
    vosk_set_log_level(-1);

    stdinReader = new StdinReader();
    connect(stdinReader, &StdinReader::lineReceived, this,
            &VoskEngine::handleCommand);
    stdinReader->start();

    model = nullptr;
    rec = nullptr;
    paStream = nullptr;
    recordFile = nullptr;
    isRecording = false;
    nativeSampleRate = 16000;
    mode = "IDLE"; // Start in IDLE so wakeword detection is active from boot

    // Initialize PortAudio
    PaError err = Pa_Initialize();
    if (err != paNoError) {
      sendStatus(3,
                 QString("PortAudio Init Error: %1").arg(Pa_GetErrorText(err)));
    }

    QTimer::singleShot(100, this, [=]() { loadModel("Vosk Small En"); });
  }

  ~VoskEngine() {
    stopRecording();
    stopAudio();
    if (rec)
      vosk_recognizer_free(rec);
    if (model)
      vosk_model_free(model);
    Pa_Terminate();
  }

private slots:
  void handleCommand(const QString &cmd) {
    if (cmd == "TOGGLE") {
      if (mode == "ACTIVE") {
        if (isRecording)
          stopRecording();
        mode = "IDLE";
        sendStatus(0, "Model Ready");
      } else {
        mode = "ACTIVE";
        sendStatus(1, "Listening...");
      }
    } else if (cmd == "STOP" || cmd == "SLEEP") {
      if (isRecording)
        stopRecording();
      // Stay in IDLE mode so wakeword detection keeps running in background
      mode = "IDLE";
      vosk_recognizer_reset(rec);
      sendStatus(-1, "Hidden");
    } else if (cmd.startsWith("MODEL:")) {
      QString mName = cmd.mid(6).trimmed();
      loadModel(mName);
    } else if (cmd.startsWith("RECORD_START:")) {
      QString path = cmd.mid(13).trimmed();
      startRecording(path);
    } else if (cmd == "RECORD_STOP") {
      stopRecording();
    } else if (cmd.startsWith("WAKEWORDS:")) {
      if (rec)
        vosk_recognizer_reset(rec);
      // Engine stays in IDLE — no mode change needed
    } else if (cmd.startsWith("WAKEMODE:")) {
      // Engine stays in IDLE — wakeword engine config change
    } else if (cmd.startsWith("STOPWORDS:")) {
      QString payload = cmd.mid(10);
      QSettings s("QuickSTT", "Config");
      s.setValue("stopWords", payload.split(","));
    } else if (cmd == "OFFLOAD") {
      // Offload model from memory
      stopAudio();
      if (rec) {
        vosk_recognizer_free(rec);
        rec = nullptr;
      }
      if (model) {
        vosk_model_free(model);
        model = nullptr;
      }
      mode = "OFFLOADED";
      sendStatus(0, "Model Offloaded");
    } else if (cmd == "RELOAD") {
      // Reload model
      if (mode == "OFFLOADED" && !activeModelName.isEmpty()) {
        loadModel(activeModelName);
      }
    } else if (cmd == "QUIT") {
      QCoreApplication::quit();
    }
  }

  // Called from the timer to process accumulated audio
  void processAudio() {
    if (!rec)
      return;

    std::lock_guard<std::mutex> lock(audioMutex);
    if (audioBuffer.isEmpty())
      return;

    QByteArray rawData = audioBuffer;
    audioBuffer.clear();

    // ── Resample if native rate != 16kHz ─────────────────────────
    QByteArray data;
    if (nativeSampleRate != 16000 && nativeSampleRate > 0) {
      // Simple linear decimation (e.g. 48000 -> 16000 = every 3rd sample)
      const int16_t *src =
          reinterpret_cast<const int16_t *>(rawData.constData());
      int srcSamples = rawData.size() / 2;
      double ratio = (double)nativeSampleRate / 16000.0;
      int dstSamples = (int)(srcSamples / ratio);
      data.resize(dstSamples * 2);
      int16_t *dst = reinterpret_cast<int16_t *>(data.data());
      for (int i = 0; i < dstSamples; ++i) {
        int srcIdx = (int)(i * ratio);
        if (srcIdx >= srcSamples)
          srcIdx = srcSamples - 1;
        dst[i] = src[srcIdx];
      }
    } else {
      data = rawData;
    }

    // ── Calculate Audio Level ────────────────────────────────────
    const int16_t *samples =
        reinterpret_cast<const int16_t *>(data.constData());
    int numSamples = data.size() / 2;
    if (numSamples > 0) {
      double sumSq = 0;
      for (int i = 0; i < numSamples; ++i) {
        sumSq += (double)samples[i] * samples[i];
      }
      double rms = std::sqrt(sumSq / numSamples);
      int level = 0;
      if (rms > 0) {
        double db = 20.0 * std::log10(rms);
        level = std::max(0, std::min(100, (int)((db - 35.0) / 50.0 * 100.0)));
      }
      std::cout << "AUDIO_LEVEL|" << level << std::endl;
    }

    // ── Write to WAV while recording ─────────────────────────────
    if (isRecording && recordFile && recordFile->isOpen()) {
      recordFile->write(data);
    }

    // ── Feed to Vosk ─────────────────────────────────────────────
    int end_of_speech =
        vosk_recognizer_accept_waveform(rec, data.constData(), data.size());

    if (mode == "ACTIVE") {
      if (end_of_speech) {
        const char *resultStr = vosk_recognizer_result(rec);
        processResult(resultStr);
      }
    } else if (mode == "IDLE") {
      // Listen for wake words via Vosk partial results
      const char *partialStr = vosk_recognizer_partial_result(rec);
      processPartialForWakeWord(partialStr);
    }
  }

private:
  // ── PortAudio callback (static, runs in audio thread) ──────────
  static int paCallback(const void *input, void *output,
                        unsigned long frameCount,
                        const PaStreamCallbackTimeInfo *timeInfo,
                        PaStreamCallbackFlags statusFlags, void *userData) {
    VoskEngine *self = static_cast<VoskEngine *>(userData);
    if (input && frameCount > 0) {
      const char *buf = static_cast<const char *>(input);
      int bytes = frameCount * 2; // 16-bit mono = 2 bytes per sample
      std::lock_guard<std::mutex> lock(self->audioMutex);
      self->audioBuffer.append(buf, bytes);
    }
    return paContinue;
  }

  void processPartialForWakeWord(const char *jsonStr) {
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(jsonStr));
    QString partial =
        doc.object().value("partial").toString().toLower().trimmed();
    if (partial.isEmpty() || partial.length() < 3)
      return;

    QSettings s("QuickSTT", "Config");
    QStringList words =
        s.value("wakeWords", QStringList() << "computer" << "hey jarvis")
            .toStringList();

    QMap<QString, QStringList> fuzzyMap;
    fuzzyMap["jarvis"] = {"jarvis",   "service", "travis", "jarves", "jar vis",
                          "jar vase", "janice",  "jervis", "nervous"};
    fuzzyMap["computer"] = {"computer", "competer", "commuter"};
    fuzzyMap["alexa"] = {"alexa", "alexis", "alex a", "elect a"};

    for (const QString &w : words) {
      QString wl = w.toLower().trimmed();
      if (partial.contains(wl)) {
        mode = "ACTIVE";
        vosk_recognizer_reset(rec);
        sendStatus(1, "Wake Word!");
        return;
      }
      QStringList keyParts = wl.split(" ");
      for (const QString &kp : keyParts) {
        if (kp.length() < 3)
          continue;
        QStringList variants = fuzzyMap.value(kp, {kp});
        for (const QString &v : variants) {
          if (partial.contains(v)) {
            mode = "ACTIVE";
            vosk_recognizer_reset(rec);
            sendStatus(1, "Wake Word!");
            return;
          }
        }
      }
    }
  }

  QString activeModelName;
  QString mode;
  VoskModel *model;
  VoskRecognizer *rec;
  PaStream *paStream;
  StdinReader *stdinReader;
  QTimer *audioTimer;

  // Audio buffer shared between PortAudio thread and Qt thread
  QByteArray audioBuffer;
  std::mutex audioMutex;
  int nativeSampleRate; // actual mic sample rate (may differ from 16kHz)

  // Recording
  QFile *recordFile;
  bool isRecording;

  void startRecording(const QString &path) {
    if (isRecording)
      stopRecording();
    recordFile = new QFile(path, this);
    if (!recordFile->open(QIODevice::WriteOnly)) {
      sendStatus(1, "Rec open err");
      delete recordFile;
      recordFile = nullptr;
      return;
    }
    writeWavHeader(*recordFile, 16000, 1, 16);
    isRecording = true;
    sendStatus(1, "● REC");
  }

  void stopRecording() {
    if (!isRecording || !recordFile)
      return;
    isRecording = false;
    finaliseWavHeader(*recordFile, 16000, 1, 16);
    recordFile->close();
    delete recordFile;
    recordFile = nullptr;
    if (mode == "ACTIVE")
      sendStatus(1, "Listening...");
    else
      sendStatus(0, "Model Ready");
  }

  void stopAudio() {
    if (paStream) {
      Pa_StopStream(paStream);
      Pa_CloseStream(paStream);
      paStream = nullptr;
    }
  }

  QString getModelPath(const QString &name) {
    QString appData = QDir::cleanPath(
        QString::fromLocal8Bit(getenv("APPDATA")) + "/QuickSTT/models");
    QDir dir(appData);
    if (!dir.exists())
      return "";

    for (const QString &d : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
      if (name.contains("Small En", Qt::CaseInsensitive) &&
          d.contains("small-en"))
        return dir.filePath(d);
      if (name.contains("Large En", Qt::CaseInsensitive) &&
          d.contains("en-us") && !d.contains("small"))
        return dir.filePath(d);
      if (name.contains("Indian", Qt::CaseInsensitive) && d.contains("en-in"))
        return dir.filePath(d);
      if (d.compare(name, Qt::CaseInsensitive) == 0)
        return dir.filePath(d);
    }
    return "";
  }

  void loadModel(const QString &name) {
    mode = "IDLE";
    stopAudio();

    if (rec) {
      vosk_recognizer_free(rec);
      rec = nullptr;
    }
    if (model) {
      vosk_model_free(model);
      model = nullptr;
    }

    activeModelName = name;
    QString path = getModelPath(name);

    if (path.isEmpty() || !QDir(path).exists()) {
      sendStatus(3, "Model Missing");
      QTimer::singleShot(2000, this, [=]() { loadModel(name); });
      return;
    }

    sendStatus(3, "Loading...");

    model = vosk_model_new(path.toLocal8Bit().constData());
    if (!model) {
      sendStatus(3, "Load Error");
      return;
    }

    rec = vosk_recognizer_new(model, 16000.0);
    if (!rec) {
      sendStatus(3, "Init Error");
      return;
    }

    // ── Open PortAudio stream ────────────────────────────────────
    PaStreamParameters inputParams;
    memset(&inputParams, 0, sizeof(inputParams));
    inputParams.device = Pa_GetDefaultInputDevice();
    if (inputParams.device == paNoDevice) {
      sendStatus(3, "No Mic Found");
      return;
    }
    inputParams.channelCount = 1;
    inputParams.sampleFormat = paInt16;
    inputParams.suggestedLatency =
        Pa_GetDeviceInfo(inputParams.device)->defaultHighInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    // Try 16kHz first (ideal for Vosk)
    double streamSampleRate = 16000.0;
    PaError fmtErr = Pa_IsFormatSupported(&inputParams, nullptr, 16000.0);
    if (fmtErr != paFormatIsSupported) {
      // 16kHz not supported — use device default and resample in software
      streamSampleRate =
          Pa_GetDeviceInfo(inputParams.device)->defaultSampleRate;
      nativeSampleRate = (int)streamSampleRate;
      std::cerr << "16kHz not supported by mic, using native rate: "
                << nativeSampleRate << " and resampling" << std::endl;
    } else {
      nativeSampleRate = 16000;
    }

    PaError err = Pa_OpenStream(
        &paStream, &inputParams,
        nullptr, // no output
        streamSampleRate,
        paFramesPerBufferUnspecified, // let PortAudio choose optimal size
        paClipOff, paCallback, this);

    if (err != paNoError) {
      sendStatus(3, QString("Mic Error: %1").arg(Pa_GetErrorText(err)));
      return;
    }

    err = Pa_StartStream(paStream);
    if (err != paNoError) {
      sendStatus(3, QString("Stream Error: %1").arg(Pa_GetErrorText(err)));
      Pa_CloseStream(paStream);
      paStream = nullptr;
      return;
    }

    // Start a timer to poll audio buffer every 30ms from the Qt thread
    audioTimer = new QTimer(this);
    connect(audioTimer, &QTimer::timeout, this, &VoskEngine::processAudio);
    audioTimer->start(30);

    sendStatus(0, "Model Ready");
  }
  void processResult(const char *jsonStr) {
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(jsonStr));
    QString text = doc.object().value("text").toString().trimmed();
    if (text.isEmpty())
      return;

    QString textLower = text.toLower();
    QSettings s("QuickSTT", "Config");

    // Check stop words
    QStringList stopWords =
        s.value("stopWords", QStringList() << "stop listening" << "goodbye")
            .toStringList();
    QStringList closeWords =
        s.value("closeWords", QStringList()
                                  << "stop listening" << "go to sleep")
            .toStringList();
    for (const QString &sw : stopWords + closeWords) {
      if (textLower.contains(sw.toLower().trimmed())) {
        if (isRecording)
          stopRecording();
        mode = "IDLE";
        vosk_recognizer_reset(rec);
        // Send Hidden (-1) to hide the widget — engine stays in IDLE
        // listening for wakewords in the background
        sendStatus(-1, "Hidden");
        return;
      }
    }

    // Send FINAL_TEXT — pill_widget handles typing AND special commands
    std::cout << "FINAL_TEXT|" << text.toStdString() << std::endl;
    sendStatus(1, "Listening...");
  }
};

void sendStatus(int code, const QString &msg) {
  std::cout << "STATE|" << code << "," << msg.toStdString() << std::endl;
}

int main(int argc, char *argv[]) {
  QCoreApplication a(argc, argv);
  VoskEngine engine;
  return a.exec();
}

#include "vosk_engine.moc"
