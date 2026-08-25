#include "cloud_stt_manager.h"

#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>

namespace {

constexpr int kAssemblyAiInitialPollDelayMs = 250;
constexpr int kAssemblyAiFastPollDelayMs = 325;
constexpr int kAssemblyAiSlowPollDelayMs = 500;

struct LanguageChoice {
  const char *code;
  const char *label;
};

template <size_t N>
constexpr int languageCount(const LanguageChoice (&)[N]) {
  return int(N);
}

struct CloudProviderDefinition {
  const char *id;
  const char *displayName;
  const char *endpoint;
  const char *authSummary;
  const char *legacyLabel;
};

struct CloudModelDefinition {
  const char *id;
  const char *providerId;
  const char *displayName;
  const char *apiModel;
  const char *description;
  const LanguageChoice *languages;
  int languageCount;
  const char *defaultLanguage;
  bool supportsLanguage;
  bool supportsPrompt;
};

static const LanguageChoice kOpenAiLanguages[] = {
    {"", "Auto-detect"}, {"ar", "Arabic"},   {"bn", "Bengali"},
    {"zh", "Chinese"},   {"nl", "Dutch"},    {"en", "English"},
    {"fi", "Finnish"},   {"fr", "French"},   {"de", "German"},
    {"el", "Greek"},     {"hi", "Hindi"},    {"id", "Indonesian"},
    {"it", "Italian"},   {"ja", "Japanese"}, {"ko", "Korean"},
    {"ms", "Malay"},     {"pl", "Polish"},   {"pt", "Portuguese"},
    {"ro", "Romanian"},  {"ru", "Russian"},  {"sk", "Slovak"},
    {"es", "Spanish"},   {"sv", "Swedish"},  {"ta", "Tamil"},
    {"te", "Telugu"},    {"th", "Thai"},     {"tr", "Turkish"},
    {"uk", "Ukrainian"}, {"ur", "Urdu"},     {"vi", "Vietnamese"},
};

static const LanguageChoice kGoogleGeneralLanguages[] = {
    {"en-US", "English (US)"}, {"en-GB", "English (UK)"},
    {"hi-IN", "Hindi (India)"}, {"es-ES", "Spanish (Spain)"},
    {"es-US", "Spanish (US)"}, {"fr-FR", "French (France)"},
    {"de-DE", "German"}, {"it-IT", "Italian"},
    {"pt-BR", "Portuguese (Brazil)"},
    {"pt-PT", "Portuguese (Portugal)"},
    {"ja-JP", "Japanese"}, {"ko-KR", "Korean"},
    {"nl-NL", "Dutch"}, {"pl-PL", "Polish"},
    {"ru-RU", "Russian"}, {"sv-SE", "Swedish"},
    {"ta-IN", "Tamil (India)"}, {"te-IN", "Telugu (India)"},
    {"tr-TR", "Turkish"}, {"uk-UA", "Ukrainian"},
    {"vi-VN", "Vietnamese"}, {"zh-CN", "Chinese (Simplified)"},
    {"zh-TW", "Chinese (Traditional)"},
};

static const LanguageChoice kGoogleMedicalLanguages[] = {
    {"en-US", "English (US)"},
};

static const LanguageChoice kElevenLabsLanguages[] = {
    {"", "Auto-detect"}, {"eng", "English"},   {"jpn", "Japanese"},
    {"zho", "Chinese"},  {"deu", "German"},    {"hin", "Hindi"},
    {"fra", "French"},   {"kor", "Korean"},    {"por", "Portuguese"},
    {"ita", "Italian"},  {"spa", "Spanish"},   {"ind", "Indonesian"},
    {"nld", "Dutch"},    {"tur", "Turkish"},   {"fil", "Filipino"},
    {"pol", "Polish"},   {"swe", "Swedish"},   {"bul", "Bulgarian"},
    {"ron", "Romanian"}, {"ara", "Arabic"},    {"ces", "Czech"},
    {"ell", "Greek"},    {"fin", "Finnish"},   {"hrv", "Croatian"},
    {"msa", "Malay"},    {"dan", "Danish"},    {"tam", "Tamil"},
    {"ukr", "Ukrainian"}, {"slk", "Slovak"},
};

static const LanguageChoice kAssemblyUniversal3Languages[] = {
    {"", "Auto-detect"}, {"en", "English"}, {"es", "Spanish"},
    {"pt", "Portuguese"}, {"fr", "French"}, {"de", "German"},
    {"it", "Italian"},
};

static const LanguageChoice kAssemblyUniversal2Languages[] = {
    {"", "Auto-detect"}, {"en", "Global English"},
    {"en_au", "English (Australia)"}, {"en_uk", "English (UK)"},
    {"en_us", "English (US)"}, {"es", "Spanish"}, {"fr", "French"},
    {"de", "German"}, {"it", "Italian"}, {"pt", "Portuguese"},
    {"nl", "Dutch"}, {"hi", "Hindi"}, {"ja", "Japanese"},
    {"zh", "Chinese"}, {"ko", "Korean"}, {"pl", "Polish"},
    {"ru", "Russian"}, {"tr", "Turkish"}, {"uk", "Ukrainian"},
    {"vi", "Vietnamese"}, {"ar", "Arabic"}, {"bn", "Bengali"},
    {"ta", "Tamil"}, {"te", "Telugu"}, {"ur", "Urdu"},
};

static const LanguageChoice kReverieLanguages[] = {
    {"en", "English"}, {"hi", "Hindi"}, {"bn", "Bengali"},
    {"gu", "Gujarati"}, {"kn", "Kannada"}, {"ml", "Malayalam"},
    {"mr", "Marathi"}, {"or", "Odia"}, {"pa", "Punjabi"},
    {"ta", "Tamil"}, {"te", "Telugu"},
};

static const LanguageChoice kSarvamLanguages[] = {
    {"unknown", "Auto-detect"}, {"en-IN", "English (India)"},
    {"hi-IN", "Hindi (India)"}, {"bn-IN", "Bengali (India)"},
    {"gu-IN", "Gujarati (India)"}, {"kn-IN", "Kannada (India)"},
    {"ml-IN", "Malayalam (India)"}, {"mr-IN", "Marathi (India)"},
    {"od-IN", "Odia (India)"}, {"pa-IN", "Punjabi (India)"},
    {"ta-IN", "Tamil (India)"}, {"te-IN", "Telugu (India)"},
};

const CloudProviderDefinition kCloudProviders[] = {
    {"openai", "OpenAI", "https://api.openai.com/v1/audio/transcriptions",
     "API key", "Cloud: OpenAI"},
    {"google", "Google Speech-to-Text",
     "https://speech.googleapis.com/v1/speech:recognize",
     "OAuth access token", "Cloud: Google Speech-to-Text"},
    {"elevenlabs", "ElevenLabs STT",
     "https://api.elevenlabs.io/v1/speech-to-text", "API key",
     "Cloud: ElevenLabs STT"},
    {"assemblyai", "AssemblyAI STT", "https://api.assemblyai.com/v2",
     "API key", "Cloud: AssemblyAI STT"},
    {"sarvam", "Sarvam AI STT", "https://api.sarvam.ai/speech-to-text",
     "API subscription key", "Cloud: Sarvam AI STT"},
    {"reverie", "Reverie STT", "https://revapi.reverieinc.com/",
     "REV-API-KEY + REV-APP-ID", "Cloud: Reverie STT"},
};

const CloudModelDefinition kCloudModels[] = {
    {"cld_openai_gpt-4o-mini-transcribe", "openai",
     "gpt-4o-mini-transcribe", "gpt-4o-mini-transcribe",
     "Fast OpenAI transcription model for short dictation turns.",
     kOpenAiLanguages, languageCount(kOpenAiLanguages), "", true, true},
    {"cld_openai_gpt-4o-transcribe", "openai", "gpt-4o-transcribe",
     "gpt-4o-transcribe",
     "Higher-quality OpenAI transcription model for general audio.",
     kOpenAiLanguages, languageCount(kOpenAiLanguages), "", true, true},
    {"cld_openai_gpt-4o-transcribe-diarize", "openai",
     "gpt-4o-transcribe-diarize", "gpt-4o-transcribe-diarize",
     "OpenAI diarizing model that returns speaker-labelled transcript output.",
     kOpenAiLanguages, languageCount(kOpenAiLanguages), "", true, false},
    {"cld_google_chirp_3", "google", "Chirp 3", "chirp_3",
     "High-accuracy multilingual and general speech model.",
     kGoogleGeneralLanguages, languageCount(kGoogleGeneralLanguages), "en-US",
     true, false},
    {"cld_google_latest_long", "google", "Latest Long", "latest_long",
     "General-purpose Google model for longer recordings.",
     kGoogleGeneralLanguages, languageCount(kGoogleGeneralLanguages), "en-US",
     true, false},
    {"cld_google_latest_short", "google", "Latest Short", "latest_short",
     "General-purpose Google model for short utterances.",
     kGoogleGeneralLanguages, languageCount(kGoogleGeneralLanguages), "en-US",
     true, false},
    {"cld_google_telephony", "google", "Telephony", "telephony",
     "Google telephony model for call-center style audio.",
     kGoogleGeneralLanguages, languageCount(kGoogleGeneralLanguages), "en-US",
     true, false},
    {"cld_google_telephony_short", "google", "Telephony Short",
     "telephony_short", "Google telephony model tuned for short call utterances.",
     kGoogleGeneralLanguages, languageCount(kGoogleGeneralLanguages), "en-US",
     true, false},
    {"cld_google_medical_conversation", "google", "Medical Conversation",
     "medical_conversation",
     "Medical conversation model for clinician-patient audio.",
     kGoogleMedicalLanguages, languageCount(kGoogleMedicalLanguages), "en-US",
     true, false},
    {"cld_google_medical_dictation", "google", "Medical Dictation",
     "medical_dictation",
     "Medical dictation model for notes and dictated clinical text.",
     kGoogleMedicalLanguages, languageCount(kGoogleMedicalLanguages), "en-US",
     true, false},
    {"cld_google_phone_call", "google", "Enhanced Phone Call", "phone_call",
     "Enhanced Google phone-call model.", kGoogleGeneralLanguages,
     languageCount(kGoogleGeneralLanguages), "en-US", true, false},
    {"cld_google_video", "google", "Enhanced Video", "video",
     "Enhanced Google video model for higher fidelity audio.",
     kGoogleGeneralLanguages, languageCount(kGoogleGeneralLanguages), "en-US",
     true, false},
    {"cld_elevenlabs_scribe_v2", "elevenlabs", "scribe_v2", "scribe_v2",
     "Current ElevenLabs speech-to-text model for file transcription.",
     kElevenLabsLanguages, languageCount(kElevenLabsLanguages), "", true,
     false},
    {"cld_elevenlabs_scribe_v1", "elevenlabs", "scribe_v1", "scribe_v1",
     "Legacy ElevenLabs speech-to-text model.", kElevenLabsLanguages,
     languageCount(kElevenLabsLanguages), "", true, false},
    {"cld_assemblyai_universal-3-pro", "assemblyai", "Universal-3 Pro",
     "universal-3-pro",
     "AssemblyAI highest-accuracy model for supported languages.",
     kAssemblyUniversal3Languages, languageCount(kAssemblyUniversal3Languages),
     "", true, false},
    {"cld_assemblyai_universal-2", "assemblyai", "Universal-2",
     "universal-2", "AssemblyAI broad multilingual model.",
     kAssemblyUniversal2Languages, languageCount(kAssemblyUniversal2Languages),
     "", true, false},
    {"cld_sarvam_saarika-v2.5", "sarvam", "saarika:v2.5", "saarika:v2.5",
     "Sarvam speech-to-text model for Indian-language dictation.",
     kSarvamLanguages, languageCount(kSarvamLanguages), "unknown", true,
     false},
    {"cld_sarvam_saaras-v3", "sarvam", "saaras:v3", "saaras:v3",
     "Sarvam speech-to-text model for newer multilingual requests.",
     kSarvamLanguages, languageCount(kSarvamLanguages), "unknown", true,
     false},
    {"cld_reverie_file-stt", "reverie", "File STT", "file-stt",
     "Reverie file transcription API with source-language selection.",
     kReverieLanguages, languageCount(kReverieLanguages), "hi", true, false},
};

const CloudProviderDefinition *findProviderById(const QString &providerId) {
  const QString normalized = providerId.trimmed().toLower();
  for (const CloudProviderDefinition &definition : kCloudProviders) {
    if (QString::fromLatin1(definition.id) == normalized)
      return &definition;
  }
  return nullptr;
}

const CloudModelDefinition *findModelDefinition(const QString &modelName) {
  const QString normalized = modelName.trimmed();
  for (const CloudModelDefinition &definition : kCloudModels) {
    if (QString::fromLatin1(definition.id).compare(normalized,
                                                   Qt::CaseInsensitive) == 0) {
      return &definition;
    }
  }
  return nullptr;
}

const CloudModelDefinition *defaultModelForProvider(const QString &providerId) {
  const QString normalized = providerId.trimmed().toLower();
  for (const CloudModelDefinition &definition : kCloudModels) {
    if (QString::fromLatin1(definition.providerId) == normalized)
      return &definition;
  }
  return nullptr;
}

QString providerSettingKey(const QString &providerId, const QString &leaf) {
  return QStringLiteral("cloud/%1/%2").arg(providerId, leaf);
}

QString modelSettingKey(const QString &modelName, const QString &leaf) {
  return QStringLiteral("cloudModels/%1/%2").arg(modelName, leaf);
}

QString providerRuntimeKey(const QString &providerId, const QString &leaf) {
  return QStringLiteral("cloudRuntime/providers/%1/%2").arg(providerId, leaf);
}

QString modelRuntimeKey(const QString &modelName, const QString &leaf) {
  return QStringLiteral("cloudRuntime/models/%1/%2").arg(modelName, leaf);
}

QString readProviderSetting(const QString &providerId, const QString &leaf,
                            const QString &fallback = QString()) {
  QSettings settings("QuickSTT", "Config");
  return settings.value(providerSettingKey(providerId, leaf), fallback)
      .toString()
      .trimmed();
}

QString readRuntimeSetting(const QString &key) {
  QSettings settings("QuickSTT", "Config");
  return settings.value(key).toString().trimmed();
}

QString normalizedCloudModelName(const QString &modelName) {
  const QString trimmed = modelName.trimmed();
  if (trimmed.isEmpty())
    return QString();

  for (const CloudModelDefinition &definition : kCloudModels) {
    const QString id = QString::fromLatin1(definition.id);
    if (id.compare(trimmed, Qt::CaseInsensitive) == 0)
      return id;
    if (QString::fromLatin1(definition.displayName)
            .compare(trimmed, Qt::CaseInsensitive) == 0) {
      return id;
    }
    if (QString::fromLatin1(definition.apiModel)
            .compare(trimmed, Qt::CaseInsensitive) == 0) {
      return id;
    }
  }

  for (const CloudProviderDefinition &provider : kCloudProviders) {
    if (QString::fromLatin1(provider.legacyLabel)
                .compare(trimmed, Qt::CaseInsensitive) == 0 ||
        QString::fromLatin1(provider.displayName)
                .compare(trimmed, Qt::CaseInsensitive) == 0) {
      const CloudModelDefinition *fallback =
          defaultModelForProvider(QString::fromLatin1(provider.id));
      return fallback ? QString::fromLatin1(fallback->id) : QString();
    }
  }

  return trimmed;
}

const CloudModelDefinition *findModelFromAnyName(const QString &modelName) {
  return findModelDefinition(normalizedCloudModelName(modelName));
}

QStringList languageLabels(const CloudModelDefinition &definition) {
  QStringList result;
  for (int i = 0; i < definition.languageCount; ++i)
    result << QString::fromLatin1(definition.languages[i].label);
  return result;
}

QString languageLabelForCode(const CloudModelDefinition &definition,
                             const QString &code) {
  const QString trimmed = code.trimmed();
  for (int i = 0; i < definition.languageCount; ++i) {
    if (QString::fromLatin1(definition.languages[i].code)
            .compare(trimmed, Qt::CaseInsensitive) == 0) {
      return QString::fromLatin1(definition.languages[i].label);
    }
  }
  return trimmed;
}

QString languageCodeForLabel(const CloudModelDefinition &definition,
                             const QString &labelOrCode) {
  const QString trimmed = labelOrCode.trimmed();
  if (trimmed.isEmpty())
    return QString();
  for (int i = 0; i < definition.languageCount; ++i) {
    if (QString::fromLatin1(definition.languages[i].label)
                .compare(trimmed, Qt::CaseInsensitive) == 0 ||
        QString::fromLatin1(definition.languages[i].code)
                .compare(trimmed, Qt::CaseInsensitive) == 0) {
      return QString::fromLatin1(definition.languages[i].code);
    }
  }
  return trimmed;
}

QString readModelSettingValue(const QString &modelName, const QString &leaf,
                              const QString &fallback = QString()) {
  const QString canonical = normalizedCloudModelName(modelName);
  if (canonical.isEmpty())
    return fallback;
  QSettings settings("QuickSTT", "Config");
  return settings.value(modelSettingKey(canonical, leaf), fallback)
      .toString()
      .trimmed();
}

void writeRuntimeStatus(const QString &providerId, const QString &modelName,
                        const QString &state, const QString &message) {
  QSettings settings("QuickSTT", "Config");
  if (!providerId.isEmpty()) {
    settings.setValue(providerRuntimeKey(providerId, "state"), state);
    settings.setValue(providerRuntimeKey(providerId, "message"), message);
  }
  if (!modelName.isEmpty()) {
    const QString canonical = normalizedCloudModelName(modelName);
    if (!canonical.isEmpty()) {
      settings.setValue(modelRuntimeKey(canonical, "state"), state);
      settings.setValue(modelRuntimeKey(canonical, "message"), message);
    }
  }
}

QString runtimeStateForModel(const QString &modelName) {
  const QString canonical = normalizedCloudModelName(modelName);
  if (canonical.isEmpty())
    return QString();

  const QString state = readRuntimeSetting(modelRuntimeKey(canonical, "state"));
  if (!state.isEmpty())
    return state;

  const CloudModelDefinition *definition = findModelDefinition(canonical);
  if (!definition)
    return QString();
  return readRuntimeSetting(providerRuntimeKey(
      QString::fromLatin1(definition->providerId), "state"));
}

QString runtimeMessageForModel(const QString &modelName) {
  const QString canonical = normalizedCloudModelName(modelName);
  if (canonical.isEmpty())
    return QString();

  const QString message =
      readRuntimeSetting(modelRuntimeKey(canonical, "message"));
  if (!message.isEmpty())
    return message;

  const CloudModelDefinition *definition = findModelDefinition(canonical);
  if (!definition)
    return QString();
  return readRuntimeSetting(providerRuntimeKey(
      QString::fromLatin1(definition->providerId), "message"));
}

QString missingConfigurationText(const QString &providerId,
                                 const QString &modelName = QString()) {
  const QString normalized = providerId.trimmed().toLower();
  if (normalized == QStringLiteral("openai"))
    return QStringLiteral("OpenAI API key is missing");
  if (normalized == QStringLiteral("google")) {
    if (normalizedCloudModelName(modelName) ==
            QStringLiteral("cld_google_chirp_3") &&
        readProviderSetting(normalized, "projectId").isEmpty()) {
      return QStringLiteral("Google project ID is required for Chirp 3");
    }
    return QStringLiteral("Google OAuth access token is missing");
  }
  if (normalized == QStringLiteral("elevenlabs"))
    return QStringLiteral("ElevenLabs API key is missing");
  if (normalized == QStringLiteral("assemblyai"))
    return QStringLiteral("AssemblyAI API key is missing");
  if (normalized == QStringLiteral("sarvam"))
    return QStringLiteral("Sarvam API key is missing");
  if (normalized == QStringLiteral("reverie"))
    return QStringLiteral("Reverie API key and App ID are required");
  return QStringLiteral("Cloud provider setup is incomplete");
}

QString formatProviderReadyText(const QString &providerId) {
  return QStringLiteral("%1 is configured").arg(cloudProviderDisplayName(providerId));
}

QString joinGoogleTranscript(const QJsonDocument &document) {
  QStringList lines;
  const QJsonArray results = document.object().value("results").toArray();
  for (const QJsonValue &resultValue : results) {
    const QJsonArray alternatives =
        resultValue.toObject().value("alternatives").toArray();
    if (alternatives.isEmpty())
      continue;
    const QString transcript =
        alternatives.first().toObject().value("transcript").toString().trimmed();
    if (!transcript.isEmpty())
      lines << transcript;
  }
  return lines.join(' ').trimmed();
}

QString firstJsonText(const QJsonDocument &document,
                      const QStringList &candidateKeys) {
  const QJsonObject object = document.object();
  for (const QString &key : candidateKeys) {
    const QJsonValue value = object.value(key);
    if (value.isString()) {
      const QString text = value.toString().trimmed();
      if (!text.isEmpty())
        return text;
    }
  }
  return QString();
}

QString extractJsonMessage(const QJsonValue &value) {
  if (value.isString())
    return value.toString().trimmed();

  if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (const QJsonValue &item : array) {
      const QString nested = extractJsonMessage(item);
      if (!nested.isEmpty())
        return nested;
    }
    return QString();
  }

  if (!value.isObject())
    return QString();

  const QJsonObject object = value.toObject();
  static const QStringList keys = {
      QStringLiteral("message"), QStringLiteral("detail"),
      QStringLiteral("error"),   QStringLiteral("cause"),
      QStringLiteral("status"),  QStringLiteral("title"),
      QStringLiteral("text")};
  for (const QString &key : keys) {
    const QString nested = extractJsonMessage(object.value(key));
    if (!nested.isEmpty())
      return nested;
  }
  for (auto it = object.begin(); it != object.end(); ++it) {
    const QString nested = extractJsonMessage(it.value());
    if (!nested.isEmpty())
      return nested;
  }
  return QString();
}

QString bestErrorMessage(QNetworkReply *reply, const QJsonDocument &document) {
  const QString jsonMessage =
      extractJsonMessage(document.isNull() ? QJsonValue()
                                           : QJsonValue(document.object()));
  const int httpStatus =
      reply ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
            : 0;

  if (!jsonMessage.isEmpty())
    return jsonMessage;
  if (httpStatus == 402)
    return QStringLiteral("Provider billing error or insufficient balance");
  if (httpStatus == 401)
    return QStringLiteral("Authentication failed");
  if (httpStatus == 403)
    return QStringLiteral("Permission denied by provider");
  if (httpStatus == 429)
    return QStringLiteral("Provider rate limit exceeded");

  const QString networkMessage =
      reply ? reply->errorString().trimmed() : QString();
  if (!networkMessage.isEmpty())
    return networkMessage;
  return QStringLiteral("Cloud transcription failed");
}

QNetworkRequest buildJsonRequest(const QString &endpoint) {
  QNetworkRequest request{QUrl(endpoint)};
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/json"));
  return request;
}

QString openAiDiarizedTranscript(const QJsonDocument &document) {
  const QJsonObject root = document.object();
  QStringList lines;
  QString currentSpeaker;
  QString currentText;

  const QJsonArray segments = root.value("segments").toArray();
  for (const QJsonValue &segmentValue : segments) {
    const QJsonObject segment = segmentValue.toObject();
    QString speaker = segment.value("speaker").toString().trimmed();
    if (speaker.isEmpty())
      speaker = segment.value("speaker_label").toString().trimmed();
    if (speaker.isEmpty())
      speaker = segment.value("speaker_id").toString().trimmed();
    QString text = segment.value("text").toString().trimmed();
    if (speaker.isEmpty() || text.isEmpty())
      continue;

    if (speaker != currentSpeaker && !currentText.isEmpty()) {
      lines << QStringLiteral("%1: %2").arg(currentSpeaker, currentText.trimmed());
      currentText.clear();
    }
    currentSpeaker = speaker;
    if (!currentText.isEmpty())
      currentText += QLatin1Char(' ');
    currentText += text;
  }
  if (!currentSpeaker.isEmpty() && !currentText.isEmpty())
    lines << QStringLiteral("%1: %2").arg(currentSpeaker, currentText.trimmed());

  if (!lines.isEmpty())
    return lines.join(QLatin1Char('\n')).trimmed();

  return firstJsonText(document, {"text"});
}

} // namespace

QStringList allCloudModelCatalog() {
  QStringList result;
  for (const CloudModelDefinition &definition : kCloudModels)
    result << QString::fromLatin1(definition.id);
  return result;
}

QStringList allCloudProviderIds() {
  QStringList result;
  for (const CloudProviderDefinition &definition : kCloudProviders)
    result << QString::fromLatin1(definition.id);
  return result;
}

QString normalizeCloudModelSelection(const QString &modelName) {
  return normalizedCloudModelName(modelName);
}

QString cloudProviderIdForModel(const QString &modelName) {
  const CloudModelDefinition *definition = findModelFromAnyName(modelName);
  return definition ? QString::fromLatin1(definition->providerId) : QString();
}

QString cloudProviderDisplayName(const QString &providerId) {
  const CloudProviderDefinition *definition = findProviderById(providerId);
  return definition ? QString::fromLatin1(definition->displayName) : QString();
}

QString cloudProviderAuthSummary(const QString &providerId) {
  const CloudProviderDefinition *definition = findProviderById(providerId);
  return definition ? QString::fromLatin1(definition->authSummary) : QString();
}

QStringList cloudProviderModelOptions(const QString &providerId) {
  QStringList result;
  for (const QString &cloudModelName : cloudModelsForProvider(providerId)) {
    const CloudModelDefinition *definition = findModelDefinition(cloudModelName);
    if (definition)
      result << QString::fromLatin1(definition->apiModel);
  }
  return result;
}

QString cloudProviderDefaultModelChoice(const QString &providerId) {
  const CloudModelDefinition *definition = defaultModelForProvider(providerId);
  return definition ? QString::fromLatin1(definition->apiModel) : QString();
}

QString cloudProviderConfiguredModelChoice(const QString &providerId) {
  return cloudProviderDefaultModelChoice(providerId);
}

QStringList cloudModelsForProvider(const QString &providerId) {
  QStringList result;
  const QString normalized = providerId.trimmed().toLower();
  for (const CloudModelDefinition &definition : kCloudModels) {
    if (QString::fromLatin1(definition.providerId) == normalized)
      result << QString::fromLatin1(definition.id);
  }
  return result;
}

QString cloudModelDisplayName(const QString &modelName) {
  const QString canonical = normalizedCloudModelName(modelName);
  const CloudModelDefinition *definition = findModelDefinition(canonical);
  if (!definition)
    return canonical;
  return QStringLiteral("%1 / %2")
      .arg(cloudProviderDisplayName(QString::fromLatin1(definition->providerId)),
           QString::fromLatin1(definition->displayName));
}

QString cloudModelWidgetLabel(const QString &modelName) {
  const QString canonical = normalizedCloudModelName(modelName);
  return canonical.isEmpty() ? modelName.trimmed() : canonical;
}

QString cloudModelDescription(const QString &modelName) {
  const CloudModelDefinition *definition = findModelFromAnyName(modelName);
  return definition ? QString::fromLatin1(definition->description) : QString();
}

QString cloudModelSettingKey(const QString &modelName, const QString &leaf) {
  const QString canonical = normalizedCloudModelName(modelName);
  return canonical.isEmpty() ? QString() : modelSettingKey(canonical, leaf);
}

QStringList cloudLanguageOptionLabels(const QString &modelName) {
  const CloudModelDefinition *definition = findModelFromAnyName(modelName);
  return definition ? languageLabels(*definition) : QStringList();
}

QString cloudLanguageCodeForLabel(const QString &modelName,
                                  const QString &label) {
  const CloudModelDefinition *definition = findModelFromAnyName(modelName);
  return definition ? languageCodeForLabel(*definition, label) : label.trimmed();
}

QString cloudLanguageLabelForCode(const QString &modelName,
                                  const QString &code) {
  const CloudModelDefinition *definition = findModelFromAnyName(modelName);
  return definition ? languageLabelForCode(*definition, code) : code.trimmed();
}

QString cloudDefaultLanguageLabel(const QString &modelName) {
  const CloudModelDefinition *definition = findModelFromAnyName(modelName);
  if (!definition)
    return QString();
  return languageLabelForCode(*definition,
                              QString::fromLatin1(definition->defaultLanguage));
}

bool cloudModelSupportsPrompt(const QString &modelName) {
  const CloudModelDefinition *definition = findModelFromAnyName(modelName);
  return definition && definition->supportsPrompt;
}

bool cloudModelSupportsLanguage(const QString &modelName) {
  const CloudModelDefinition *definition = findModelFromAnyName(modelName);
  return definition && definition->supportsLanguage;
}

bool isCloudModel(const QString &modelName) {
  return findModelFromAnyName(modelName) != nullptr;
}

bool isCloudModelConfigured(const QString &modelName) {
  const QString canonical = normalizedCloudModelName(modelName);
  const CloudModelDefinition *definition = findModelDefinition(canonical);
  if (!definition)
    return false;

  const QString providerId = QString::fromLatin1(definition->providerId);
  if (providerId == QStringLiteral("google")) {
    if (readProviderSetting(providerId, "accessToken").isEmpty())
      return false;
    if (canonical == QStringLiteral("cld_google_chirp_3") &&
        readProviderSetting(providerId, "projectId").isEmpty()) {
      return false;
    }
    return true;
  }
  if (providerId == QStringLiteral("reverie")) {
    return !readProviderSetting(providerId, "apiKey").isEmpty() &&
           !readProviderSetting(providerId, "appId").isEmpty();
  }
  if (providerId == QStringLiteral("openai") ||
      providerId == QStringLiteral("elevenlabs") ||
      providerId == QStringLiteral("assemblyai") ||
      providerId == QStringLiteral("sarvam")) {
    return !readProviderSetting(providerId, "apiKey").isEmpty();
  }
  return false;
}

QString cloudProviderLastStatus(const QString &providerId) {
  return readRuntimeSetting(providerRuntimeKey(providerId.trimmed().toLower(),
                                               "message"));
}

QString cloudProviderStatusText(const QString &providerId) {
  const QString normalized = providerId.trimmed().toLower();
  if (normalized.isEmpty())
    return QString();

  QString selectedModel = normalizedCloudModelName(
      QSettings("QuickSTT", "Config")
          .value(QStringLiteral("cloud/%1/selectedModelId").arg(normalized))
          .toString());
  if (!isCloudModel(selectedModel) ||
      cloudProviderIdForModel(selectedModel) != normalized) {
    const QStringList widgetModels =
        QSettings("QuickSTT", "Config").value("cloudWidgetModels").toStringList();
    for (const QString &rawModel : widgetModels) {
      const QString normalizedModel = normalizedCloudModelName(rawModel);
      if (cloudProviderIdForModel(normalizedModel) == normalized) {
        selectedModel = normalizedModel;
        break;
      }
    }
  }
  if (!isCloudModel(selectedModel) ||
      cloudProviderIdForModel(selectedModel) != normalized) {
    const CloudModelDefinition *defaultModel = defaultModelForProvider(normalized);
    if (!defaultModel)
      return QString();
    selectedModel = QString::fromLatin1(defaultModel->id);
  }

  if (!isCloudModelConfigured(selectedModel))
    return missingConfigurationText(normalized, selectedModel);

  const QString lastStatus = cloudProviderLastStatus(normalized);
  if (!lastStatus.isEmpty())
    return lastStatus;
  return formatProviderReadyText(normalized);
}

QString cloudModelLastStatus(const QString &modelName) {
  const QString canonical = normalizedCloudModelName(modelName);
  if (canonical.isEmpty())
    return QString();
  const QString message = runtimeMessageForModel(canonical);
  if (!message.isEmpty())
    return message;

  const QString providerId = cloudProviderIdForModel(canonical);
  if (!isCloudModelConfigured(canonical))
    return missingConfigurationText(providerId, canonical);
  return formatProviderReadyText(providerId);
}

QString cloudModelStateText(const QString &modelName) {
  const QString canonical = normalizedCloudModelName(modelName);
  if (canonical.isEmpty())
    return QStringLiteral("[Setup]");
  if (!isCloudModelConfigured(canonical))
    return QStringLiteral("[Setup]");
  const QString state = runtimeStateForModel(canonical).toLower();
  if (state == QStringLiteral("error"))
    return QStringLiteral("[Error]");
  if (state == QStringLiteral("busy"))
    return QStringLiteral("[Busy]");
  return QStringLiteral("[API]");
}

QString cloudModelDetailsText(const QString &modelName) {
  const QString canonical = normalizedCloudModelName(modelName);
  const CloudModelDefinition *definition = findModelDefinition(canonical);
  if (!definition)
    return QString();

  QStringList lines;
  lines << cloudModelDisplayName(canonical);
  lines << QStringLiteral("Widget ID: %1").arg(cloudModelWidgetLabel(canonical));
  lines << QString::fromLatin1(definition->description);

  const QString languageCode = readModelSettingValue(
      canonical, "language",
      readProviderSetting(QString::fromLatin1(definition->providerId), "language",
                          QString::fromLatin1(definition->defaultLanguage)));
  if (definition->supportsLanguage) {
    const QString label = cloudLanguageLabelForCode(canonical, languageCode);
    lines << QStringLiteral("Language: %1")
                 .arg(label.isEmpty() ? QStringLiteral("Auto-detect") : label);
  }

  if (definition->supportsPrompt) {
    const QString prompt = readModelSettingValue(
        canonical, "prompt",
        readProviderSetting(QString::fromLatin1(definition->providerId),
                            "prompt"));
    lines << QStringLiteral("Prompt: %1")
                 .arg(prompt.isEmpty() ? QStringLiteral("None") : prompt);
  }

  lines << QStringLiteral("Status: %1").arg(cloudModelLastStatus(canonical));
  return lines.join(QLatin1Char('\n'));
}

QString cloudModelRequirementsText(const QString &modelName) {
  const QString canonical = normalizedCloudModelName(modelName);
  const CloudModelDefinition *definition = findModelDefinition(canonical);
  if (!definition)
    return QString();

  const QString providerId = QString::fromLatin1(definition->providerId);
  QStringList lines;
  lines << QStringLiteral("Provider authentication: %1")
               .arg(cloudProviderAuthSummary(providerId));

  if (providerId == QStringLiteral("openai")) {
    lines << QStringLiteral("Required inputs: API key, audio upload");
    if (definition->supportsLanguage)
      lines << QStringLiteral("Optional inputs: language");
    if (definition->supportsPrompt)
      lines << QStringLiteral("Optional inputs: prompt / spelling guidance");
    if (canonical == QStringLiteral("cld_openai_gpt-4o-transcribe-diarize"))
      lines << QStringLiteral("Model behavior: returns speaker-labelled output and ignores prompt text");
  } else if (providerId == QStringLiteral("google")) {
    lines << QStringLiteral("Required inputs: OAuth access token, audio upload");
    if (canonical == QStringLiteral("cld_google_chirp_3"))
      lines << QStringLiteral("Additional required inputs: Project ID and Location");
    else
      lines << QStringLiteral("Additional provider inputs: Location is optional for non-Chirp Google models");
    lines << QStringLiteral("Model behavior: language selection is supported");
  } else if (providerId == QStringLiteral("elevenlabs")) {
    lines << QStringLiteral("Required inputs: API key, audio upload");
    lines << QStringLiteral("Model behavior: language selection is optional");
  } else if (providerId == QStringLiteral("assemblyai")) {
    lines << QStringLiteral("Required inputs: API key, audio upload");
    lines << QStringLiteral("Model behavior: language selection is optional and jobs are processed asynchronously");
  } else if (providerId == QStringLiteral("sarvam")) {
    lines << QStringLiteral("Required inputs: API key, audio upload");
    lines << QStringLiteral("Additional provider inputs: mode");
    lines << QStringLiteral("Model behavior: language selection is optional");
  } else if (providerId == QStringLiteral("reverie")) {
    lines << QStringLiteral("Required inputs: REV-API-KEY, REV-APP-ID, audio upload");
    lines << QStringLiteral("Additional provider inputs: domain and format");
    lines << QStringLiteral("Model behavior: source-language selection is required for best results");
  }

  if (definition->supportsPrompt && providerId != QStringLiteral("openai"))
    lines << QStringLiteral("Prompt / instructions are supported for this model");
  else if (!definition->supportsPrompt)
    lines << QStringLiteral("Prompt / instructions are not used by this model");

  return lines.join(QLatin1Char('\n'));
}

QString cloudModelTooltip(const QString &modelName) {
  return cloudModelDetailsText(modelName);
}

CloudSttManager::CloudSttManager(QObject *parent) : QObject(parent) {
  m_network = new QNetworkAccessManager(this);
}

CloudSttManager::ProviderSettings
CloudSttManager::loadSettingsForModel(const QString &cloudModelName) const {
  ProviderSettings settings;
  const QString canonical = normalizedCloudModelName(cloudModelName);
  const CloudModelDefinition *modelDefinition = findModelDefinition(canonical);
  if (!modelDefinition)
    return settings;

  const CloudProviderDefinition *providerDefinition =
      findProviderById(QString::fromLatin1(modelDefinition->providerId));
  if (!providerDefinition)
    return settings;

  settings.cloudModelId = canonical;
  settings.providerId = QString::fromLatin1(providerDefinition->id);
  settings.displayName = cloudModelDisplayName(canonical);
  settings.endpoint = QString::fromLatin1(providerDefinition->endpoint);
  settings.model = QString::fromLatin1(modelDefinition->apiModel);

  const QString providerId = settings.providerId;
  const QString defaultLanguage =
      QString::fromLatin1(modelDefinition->defaultLanguage);
  const QString rawLanguage = readModelSettingValue(
      canonical, "language",
      readProviderSetting(providerId, "language", defaultLanguage));
  settings.language = languageCodeForLabel(*modelDefinition, rawLanguage);
  settings.prompt = readModelSettingValue(
      canonical, "prompt", readProviderSetting(providerId, "prompt"));
  settings.mode = readProviderSetting(providerId, "mode", "transcribe");
  settings.domain = readProviderSetting(providerId, "domain", "generic");
  settings.format = readProviderSetting(providerId, "format", "16k_int16");
  settings.apiKey = readProviderSetting(providerId, "apiKey");
  settings.accessToken = readProviderSetting(providerId, "accessToken");
  settings.appId = readProviderSetting(providerId, "appId");
  settings.projectId = readProviderSetting(providerId, "projectId");
  settings.location = readProviderSetting(providerId, "location", "global");
  return settings;
}

void CloudSttManager::transcribeFile(const QString &cloudModelName,
                                     const QString &audioPath) {
  if (m_busy) {
    qDebug() << "[CLOUD]" << "busy, rejecting new request for" << cloudModelName;
    emit transcriptionFailed(QStringLiteral("Cloud transcription is still busy"));
    return;
  }

  QFileInfo audioInfo(audioPath);
  if (!audioInfo.exists() || !audioInfo.isFile()) {
    qDebug() << "[CLOUD]" << "captured audio missing:" << audioPath;
    emit transcriptionFailed(QStringLiteral("Captured audio file is missing"));
    return;
  }

  const ProviderSettings settings = loadSettingsForModel(cloudModelName);
  if (settings.providerId.isEmpty()) {
    qDebug() << "[CLOUD]" << "unknown cloud provider selection for"
             << cloudModelName;
    emit transcriptionFailed(QStringLiteral("Unknown cloud provider selection"));
    return;
  }

  m_busy = true;
  m_activeAudioPath = audioPath;
  m_activeProviderId = settings.providerId;
  m_activeCloudModelName = settings.cloudModelId;
  m_activeTimer.restart();

  qDebug() << "[CLOUD]" << "transcribeFile"
           << "model=" << settings.cloudModelId
           << "provider=" << settings.providerId
           << "endpoint=" << settings.endpoint
           << "audio=" << audioPath;

  if (!isCloudModelConfigured(settings.cloudModelId)) {
    finishWithError(missingConfigurationText(settings.providerId,
                                            settings.cloudModelId));
    return;
  }

  storeActiveStatus(QStringLiteral("busy"),
                    QStringLiteral("Preparing %1").arg(settings.displayName));

  if (settings.providerId == QStringLiteral("openai")) {
    startOpenAiRequest(settings, audioPath);
    return;
  }
  if (settings.providerId == QStringLiteral("google")) {
    startGoogleRequest(settings, audioPath);
    return;
  }
  if (settings.providerId == QStringLiteral("elevenlabs")) {
    startElevenLabsRequest(settings, audioPath);
    return;
  }
  if (settings.providerId == QStringLiteral("assemblyai")) {
    startAssemblyAiRequest(settings, audioPath);
    return;
  }
  if (settings.providerId == QStringLiteral("sarvam")) {
    startSarvamRequest(settings, audioPath);
    return;
  }
  if (settings.providerId == QStringLiteral("reverie")) {
    startReverieRequest(settings, audioPath);
    return;
  }

  finishWithError(QStringLiteral("Cloud provider is not implemented"));
}

void CloudSttManager::startOpenAiRequest(const ProviderSettings &settings,
                                         const QString &audioPath) {
  emit statusChanged(QStringLiteral("Uploading %1...").arg(settings.displayName));

  QNetworkRequest request{QUrl(settings.endpoint)};
  request.setRawHeader("Authorization", "Bearer " + settings.apiKey.toUtf8());

  auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

  QHttpPart filePart;
  filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                     QStringLiteral("form-data; name=\"file\"; filename=\"%1\"")
                         .arg(QFileInfo(audioPath).fileName()));
  filePart.setHeader(QNetworkRequest::ContentTypeHeader,
                     QStringLiteral("audio/wav"));
  auto *file = new QFile(audioPath);
  if (!file->open(QIODevice::ReadOnly)) {
    multiPart->deleteLater();
    finishWithError(QStringLiteral("Failed to open captured audio"));
    return;
  }
  file->setParent(multiPart);
  filePart.setBodyDevice(file);
  multiPart->append(filePart);

  QHttpPart modelPart;
  modelPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                      QStringLiteral("form-data; name=\"model\""));
  modelPart.setBody(settings.model.toUtf8());
  multiPart->append(modelPart);

  if (!settings.language.isEmpty()) {
    QHttpPart languagePart;
    languagePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QStringLiteral("form-data; name=\"language\""));
    languagePart.setBody(settings.language.toUtf8());
    multiPart->append(languagePart);
  }

  if (!settings.prompt.isEmpty() &&
      settings.model != QStringLiteral("gpt-4o-transcribe-diarize")) {
    QHttpPart promptPart;
    promptPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                         QStringLiteral("form-data; name=\"prompt\""));
    promptPart.setBody(settings.prompt.toUtf8());
    multiPart->append(promptPart);
  }

  QHttpPart formatPart;
  formatPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"response_format\""));
  formatPart.setBody(settings.model == QStringLiteral("gpt-4o-transcribe-diarize")
                         ? QByteArray("diarized_json")
                         : QByteArray("json"));
  multiPart->append(formatPart);

  QNetworkReply *reply = m_network->post(request, multiPart);
  multiPart->setParent(reply);
  connect(reply, &QNetworkReply::finished, this, [this, reply, settings]() {
    const QByteArray body = reply->readAll();
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (reply->error() != QNetworkReply::NoError) {
      const QString message = bestErrorMessage(reply, document);
      reply->deleteLater();
      finishWithError(message);
      return;
    }

    QString transcript = settings.model == QStringLiteral("gpt-4o-transcribe-diarize")
                             ? openAiDiarizedTranscript(document)
                             : firstJsonText(document, {"text"});
    reply->deleteLater();
    if (transcript.isEmpty()) {
      finishWithError(QStringLiteral("OpenAI returned an empty transcript"));
      return;
    }
    finishWithTranscript(transcript);
  });
}

void CloudSttManager::startGoogleRequest(const ProviderSettings &settings,
                                         const QString &audioPath) {
  QFile file(audioPath);
  if (!file.open(QIODevice::ReadOnly)) {
    finishWithError(QStringLiteral("Failed to open captured audio"));
    return;
  }

  emit statusChanged(QStringLiteral("Sending %1...").arg(settings.displayName));
  const QByteArray encodedAudio = file.readAll().toBase64();

  if (settings.model == QStringLiteral("chirp_3")) {
    if (settings.projectId.isEmpty()) {
      finishWithError(QStringLiteral("Google project ID is required for Chirp 3"));
      return;
    }

    const QString endpoint =
        QStringLiteral("https://speech.googleapis.com/v2/projects/%1/locations/%2/"
                       "recognizers/_:recognize")
            .arg(settings.projectId,
                 settings.location.isEmpty() ? QStringLiteral("global")
                                             : settings.location);
    QNetworkRequest request = buildJsonRequest(endpoint);
    request.setRawHeader("Authorization",
                         "Bearer " + settings.accessToken.toUtf8());

    QJsonObject features;
    features.insert(QStringLiteral("enableAutomaticPunctuation"), true);

    QJsonObject config;
    config.insert(QStringLiteral("autoDecodingConfig"), QJsonObject());
    config.insert(QStringLiteral("model"), settings.model);
    config.insert(QStringLiteral("features"), features);
    config.insert(QStringLiteral("languageCodes"),
                  QJsonArray{settings.language.isEmpty() ? QStringLiteral("en-US")
                                                         : settings.language});

    QJsonObject payload;
    payload.insert(QStringLiteral("config"), config);
    payload.insert(QStringLiteral("content"),
                   QString::fromLatin1(encodedAudio));

    QNetworkReply *reply =
        m_network->post(request, QJsonDocument(payload).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
      const QByteArray body = reply->readAll();
      const QJsonDocument document = QJsonDocument::fromJson(body);
      if (reply->error() != QNetworkReply::NoError) {
        const QString message = bestErrorMessage(reply, document);
        reply->deleteLater();
        finishWithError(message);
        return;
      }

      const QString transcript = joinGoogleTranscript(document);
      reply->deleteLater();
      if (transcript.isEmpty()) {
        finishWithError(QStringLiteral("Google returned an empty transcript"));
        return;
      }
      finishWithTranscript(transcript);
    });
    return;
  }

  QNetworkRequest request = buildJsonRequest(settings.endpoint);
  request.setRawHeader("Authorization", "Bearer " + settings.accessToken.toUtf8());

  QJsonObject config;
  config.insert(QStringLiteral("encoding"), QStringLiteral("LINEAR16"));
  config.insert(QStringLiteral("sampleRateHertz"), 16000);
  config.insert(QStringLiteral("languageCode"),
                settings.language.isEmpty() ? QStringLiteral("en-US")
                                            : settings.language);
  config.insert(QStringLiteral("enableAutomaticPunctuation"), true);
  if (!settings.model.isEmpty())
    config.insert(QStringLiteral("model"), settings.model);
  if (settings.model == QStringLiteral("phone_call") ||
      settings.model == QStringLiteral("video")) {
    config.insert(QStringLiteral("useEnhanced"), true);
  }

  QJsonObject audio;
  audio.insert(QStringLiteral("content"), QString::fromLatin1(encodedAudio));

  const QJsonDocument payload(
      QJsonObject{{QStringLiteral("config"), config},
                  {QStringLiteral("audio"), audio}});

  QNetworkReply *reply = m_network->post(request, payload.toJson());
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    const QByteArray body = reply->readAll();
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (reply->error() != QNetworkReply::NoError) {
      const QString message = bestErrorMessage(reply, document);
      reply->deleteLater();
      finishWithError(message);
      return;
    }

    const QString transcript = joinGoogleTranscript(document);
    reply->deleteLater();
    if (transcript.isEmpty()) {
      finishWithError(QStringLiteral("Google returned an empty transcript"));
      return;
    }
    finishWithTranscript(transcript);
  });
}

void CloudSttManager::startElevenLabsRequest(const ProviderSettings &settings,
                                             const QString &audioPath) {
  emit statusChanged(QStringLiteral("Uploading %1...").arg(settings.displayName));

  QNetworkRequest request{QUrl(settings.endpoint)};
  request.setRawHeader("xi-api-key", settings.apiKey.toUtf8());

  auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

  QHttpPart filePart;
  filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                     QStringLiteral("form-data; name=\"file\"; filename=\"%1\"")
                         .arg(QFileInfo(audioPath).fileName()));
  filePart.setHeader(QNetworkRequest::ContentTypeHeader,
                     QStringLiteral("audio/wav"));
  auto *file = new QFile(audioPath);
  if (!file->open(QIODevice::ReadOnly)) {
    multiPart->deleteLater();
    finishWithError(QStringLiteral("Failed to open captured audio"));
    return;
  }
  file->setParent(multiPart);
  filePart.setBodyDevice(file);
  multiPart->append(filePart);

  QHttpPart modelPart;
  modelPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                      QStringLiteral("form-data; name=\"model_id\""));
  modelPart.setBody(settings.model.toUtf8());
  multiPart->append(modelPart);

  if (!settings.language.isEmpty()) {
    QHttpPart languagePart;
    languagePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QStringLiteral("form-data; name=\"language_code\""));
    languagePart.setBody(settings.language.toUtf8());
    multiPart->append(languagePart);
  }

  QHttpPart formatPart;
  formatPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"file_format\""));
  formatPart.setBody(QByteArray("pcm_s16le_16"));
  multiPart->append(formatPart);

  QNetworkReply *reply = m_network->post(request, multiPart);
  multiPart->setParent(reply);
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    const QByteArray body = reply->readAll();
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (reply->error() != QNetworkReply::NoError) {
      const QString message = bestErrorMessage(reply, document);
      reply->deleteLater();
      finishWithError(message);
      return;
    }

    QString transcript = firstJsonText(document, {"text", "transcript"});
    if (transcript.isEmpty()) {
      const QJsonArray words = document.object().value("words").toArray();
      QStringList tokens;
      for (const QJsonValue &wordValue : words) {
        const QString token =
            wordValue.toObject().value("text").toString().trimmed();
        if (!token.isEmpty())
          tokens << token;
      }
      transcript = tokens.join(' ').trimmed();
    }
    reply->deleteLater();
    if (transcript.isEmpty()) {
      finishWithError(QStringLiteral("ElevenLabs returned an empty transcript"));
      return;
    }
    finishWithTranscript(transcript);
  });
}

void CloudSttManager::startAssemblyAiRequest(const ProviderSettings &settings,
                                             const QString &audioPath) {
  emit statusChanged(QStringLiteral("Uploading %1...").arg(settings.displayName));

  QFile *file = new QFile(audioPath);
  if (!file->open(QIODevice::ReadOnly)) {
    file->deleteLater();
    finishWithError(QStringLiteral("Failed to open captured audio"));
    return;
  }

  QNetworkRequest uploadRequest{QUrl(settings.endpoint + "/upload")};
  uploadRequest.setRawHeader("authorization", settings.apiKey.toUtf8());
  uploadRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/octet-stream"));

  QNetworkReply *uploadReply = m_network->post(uploadRequest, file);
  file->setParent(uploadReply);

  connect(uploadReply, &QNetworkReply::finished, this,
          [this, uploadReply, settings]() {
            const QByteArray body = uploadReply->readAll();
            const QJsonDocument document = QJsonDocument::fromJson(body);
            if (uploadReply->error() != QNetworkReply::NoError) {
              const QString message = bestErrorMessage(uploadReply, document);
              uploadReply->deleteLater();
              finishWithError(message);
              return;
            }

            const QString uploadUrl = firstJsonText(document, {"upload_url"});
            uploadReply->deleteLater();
            if (uploadUrl.isEmpty()) {
              finishWithError(QStringLiteral("AssemblyAI upload URL missing"));
              return;
            }

            emit statusChanged(
                QStringLiteral("Submitting %1...").arg(settings.displayName));

            QJsonObject payload;
            payload.insert(QStringLiteral("audio_url"), uploadUrl);
            payload.insert(QStringLiteral("punctuate"), true);
            payload.insert(QStringLiteral("format_text"), true);
            if (!settings.language.isEmpty())
              payload.insert(QStringLiteral("language_code"), settings.language);
            if (!settings.model.isEmpty())
              payload.insert(QStringLiteral("speech_models"),
                             QJsonArray{settings.model});

            QNetworkRequest transcriptRequest =
                buildJsonRequest(settings.endpoint + "/transcript");
            transcriptRequest.setRawHeader("authorization",
                                           settings.apiKey.toUtf8());

            QNetworkReply *transcriptReply =
                m_network->post(transcriptRequest,
                                QJsonDocument(payload).toJson());
            connect(transcriptReply, &QNetworkReply::finished, this,
                    [this, transcriptReply, settings]() {
                      const QByteArray transcriptBody =
                          transcriptReply->readAll();
                      const QJsonDocument transcriptDoc =
                          QJsonDocument::fromJson(transcriptBody);
                      if (transcriptReply->error() !=
                          QNetworkReply::NoError) {
                        const QString message =
                            bestErrorMessage(transcriptReply, transcriptDoc);
                        transcriptReply->deleteLater();
                        finishWithError(message);
                        return;
                      }

                      const QString transcriptId =
                          firstJsonText(transcriptDoc, {"id"});
                      transcriptReply->deleteLater();
                      if (transcriptId.isEmpty()) {
                        finishWithError(
                            QStringLiteral("AssemblyAI transcript ID missing"));
                        return;
                      }

                      const QString pollUrl =
                          settings.endpoint + "/transcript/" + transcriptId;
                      auto pollAttempt = std::make_shared<int>(0);
                      auto poll = std::make_shared<std::function<void()>>();
                      *poll = [this, pollUrl, settings, poll, pollAttempt]() {
                        emit statusChanged(
                            QStringLiteral("Waiting for %1...")
                                .arg(settings.displayName));
                        QNetworkRequest pollRequest{QUrl(pollUrl)};
                        pollRequest.setRawHeader("authorization",
                                                 settings.apiKey.toUtf8());
                        QNetworkReply *pollReply = m_network->get(pollRequest);
                        connect(pollReply, &QNetworkReply::finished, this,
                                [this, pollReply, poll, pollAttempt]() {
                                  const QByteArray pollBody =
                                      pollReply->readAll();
                                  const QJsonDocument pollDoc =
                                      QJsonDocument::fromJson(pollBody);
                                  if (pollReply->error() !=
                                      QNetworkReply::NoError) {
                                    const QString message =
                                        bestErrorMessage(pollReply, pollDoc);
                                    pollReply->deleteLater();
                                    finishWithError(message);
                                    return;
                                  }

                                  const QString status =
                                      firstJsonText(pollDoc, {"status"});
                                  if (status == QStringLiteral("completed")) {
                                    const QString transcript = firstJsonText(
                                        pollDoc, {"text", "transcript"});
                                    pollReply->deleteLater();
                                    if (transcript.isEmpty()) {
                                      finishWithError(QStringLiteral(
                                          "AssemblyAI returned an empty transcript"));
                                      return;
                                    }
                                    finishWithTranscript(transcript);
                                    return;
                                  }

                                  if (status == QStringLiteral("error") ||
                                      status == QStringLiteral("failed")) {
                                    const QString message =
                                        bestErrorMessage(pollReply, pollDoc);
                                    pollReply->deleteLater();
                                    finishWithError(message);
                                    return;
                                  }

                                  pollReply->deleteLater();
                                  ++(*pollAttempt);
                                  const int delayMs =
                                      (*pollAttempt < 8)
                                          ? kAssemblyAiFastPollDelayMs
                                          : kAssemblyAiSlowPollDelayMs;
                                  QTimer::singleShot(delayMs, this, *poll);
                                });
                      };
                      QTimer::singleShot(kAssemblyAiInitialPollDelayMs, this,
                                         *poll);
                    });
          });
}

void CloudSttManager::startSarvamRequest(const ProviderSettings &settings,
                                         const QString &audioPath) {
  emit statusChanged(QStringLiteral("Uploading %1...").arg(settings.displayName));

  QNetworkRequest request{QUrl(settings.endpoint)};
  request.setRawHeader("api-subscription-key", settings.apiKey.toUtf8());

  auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

  QHttpPart filePart;
  filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                     QStringLiteral("form-data; name=\"file\"; filename=\"%1\"")
                         .arg(QFileInfo(audioPath).fileName()));
  filePart.setHeader(QNetworkRequest::ContentTypeHeader,
                     QStringLiteral("audio/wav"));
  auto *file = new QFile(audioPath);
  if (!file->open(QIODevice::ReadOnly)) {
    multiPart->deleteLater();
    finishWithError(QStringLiteral("Failed to open captured audio"));
    return;
  }
  file->setParent(multiPart);
  filePart.setBodyDevice(file);
  multiPart->append(filePart);

  if (!settings.model.isEmpty()) {
    QHttpPart modelPart;
    modelPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                        QStringLiteral("form-data; name=\"model\""));
    modelPart.setBody(settings.model.toUtf8());
    multiPart->append(modelPart);
  }

  if (!settings.mode.isEmpty()) {
    QHttpPart modePart;
    modePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"mode\""));
    modePart.setBody(settings.mode.toUtf8());
    multiPart->append(modePart);
  }

  if (!settings.language.isEmpty()) {
    QHttpPart languagePart;
    languagePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QStringLiteral("form-data; name=\"language_code\""));
    languagePart.setBody(settings.language.toUtf8());
    multiPart->append(languagePart);
  }

  QNetworkReply *reply = m_network->post(request, multiPart);
  multiPart->setParent(reply);
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    const QByteArray body = reply->readAll();
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (reply->error() != QNetworkReply::NoError) {
      const QString message = bestErrorMessage(reply, document);
      reply->deleteLater();
      finishWithError(message);
      return;
    }

    const QString transcript = firstJsonText(document, {"transcript", "text"});
    reply->deleteLater();
    if (transcript.isEmpty()) {
      finishWithError(QStringLiteral("Sarvam returned an empty transcript"));
      return;
    }
    finishWithTranscript(transcript);
  });
}

void CloudSttManager::startReverieRequest(const ProviderSettings &settings,
                                          const QString &audioPath) {
  emit statusChanged(QStringLiteral("Uploading %1...").arg(settings.displayName));

  QNetworkRequest request{QUrl(settings.endpoint)};
  request.setRawHeader("REV-API-KEY", settings.apiKey.toUtf8());
  request.setRawHeader("REV-APPNAME", "stt_file");
  request.setRawHeader("REV-APP-ID", settings.appId.toUtf8());
  request.setRawHeader(
      "src_lang",
      (settings.language.isEmpty() ? QStringLiteral("hi") : settings.language)
          .toUtf8());
  request.setRawHeader(
      "domain",
      (settings.domain.isEmpty() ? QStringLiteral("generic") : settings.domain)
          .toUtf8());
  if (!settings.format.isEmpty())
    request.setRawHeader("format", settings.format.toUtf8());

  auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

  QHttpPart filePart;
  filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                     QStringLiteral(
                         "form-data; name=\"audio_file\"; filename=\"%1\"")
                         .arg(QFileInfo(audioPath).fileName()));
  filePart.setHeader(QNetworkRequest::ContentTypeHeader,
                     QStringLiteral("audio/wav"));
  auto *file = new QFile(audioPath);
  if (!file->open(QIODevice::ReadOnly)) {
    multiPart->deleteLater();
    finishWithError(QStringLiteral("Failed to open captured audio"));
    return;
  }
  file->setParent(multiPart);
  filePart.setBodyDevice(file);
  multiPart->append(filePart);

  QNetworkReply *reply = m_network->post(request, multiPart);
  multiPart->setParent(reply);
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    const QByteArray body = reply->readAll();
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (reply->error() != QNetworkReply::NoError) {
      const QString message = bestErrorMessage(reply, document);
      reply->deleteLater();
      finishWithError(message);
      return;
    }

    const QString transcript =
        firstJsonText(document, {"display_text", "text", "transcript"});
    reply->deleteLater();
    if (transcript.isEmpty()) {
      finishWithError(QStringLiteral("Reverie returned an empty transcript"));
      return;
    }
    finishWithTranscript(transcript);
  });
}

void CloudSttManager::finishWithTranscript(const QString &text) {
  const QString trimmed = text.trimmed();
  storeActiveStatus(QStringLiteral("ok"),
                    QStringLiteral("Last transcription succeeded"));
  cleanupActiveAudio();
  m_busy = false;
  const qint64 elapsedMs = m_activeTimer.isValid() ? m_activeTimer.elapsed() : -1;
  qDebug() << "[CLOUD]" << "transcript ready"
           << "elapsedMs=" << elapsedMs << trimmed.left(160);
  if (trimmed.isEmpty()) {
    emit transcriptionFailed(QStringLiteral("Cloud provider returned no text"));
    return;
  }
  emit statusChanged(QStringLiteral("Cloud transcription complete"));
  emit transcriptionReady(trimmed);
}

void CloudSttManager::finishWithError(const QString &errorText) {
  const QString trimmed =
      errorText.trimmed().isEmpty() ? QStringLiteral("Cloud transcription failed")
                                    : errorText.trimmed();
  storeActiveStatus(QStringLiteral("error"), trimmed);
  cleanupActiveAudio();
  m_busy = false;
  const qint64 elapsedMs = m_activeTimer.isValid() ? m_activeTimer.elapsed() : -1;
  qDebug() << "[CLOUD]" << "transcription failed:"
           << "elapsedMs=" << elapsedMs << trimmed;
  emit statusChanged(trimmed);
  emit transcriptionFailed(trimmed);
}

void CloudSttManager::cleanupActiveAudio() {
  if (!m_activeAudioPath.isEmpty())
    QFile::remove(m_activeAudioPath);
  m_activeAudioPath.clear();
  m_activeProviderId.clear();
  m_activeCloudModelName.clear();
}

void CloudSttManager::storeActiveStatus(const QString &state,
                                        const QString &message) const {
  writeRuntimeStatus(m_activeProviderId, m_activeCloudModelName, state, message);
}
