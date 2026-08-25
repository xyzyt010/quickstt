#include "smart_life_manager.h"
#include "windows_secret_store.h"

#include <climits>

#include <QCryptographicHash>
#include <QColor>
#include <QDebug>
#include <QDateTime>
#include <QEventLoop>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace {

struct EndpointDefinition {
  const char *key;
  const char *label;
  const char *url;
};

constexpr EndpointDefinition kEndpointDefinitions[] = {
    {"western_america", "United States", "https://openapi.tuyaus.com"},
    {"eastern_america", "Eastern America", "https://openapi-ueaz.tuyaus.com"},
    {"central_europe", "Europe", "https://openapi.tuyaeu.com"},
    {"western_europe", "Western Europe", "https://openapi-weaz.tuyaeu.com"},
    {"india", "India", "https://openapi.tuyain.com"},
    {"china", "China", "https://openapi.tuyacn.com"},
    {"singapore", "Singapore", "https://openapi-sg.iotbing.com"},
};

QString normalizedMatchText(QString value) {
  value = value.toLower().trimmed();
  for (QChar &ch : value) {
    if (!ch.isLetterOrNumber())
      ch = QLatin1Char(' ');
  }
  return value.simplified();
}

QString endpointUrlFromKey(const QString &endpointKey) {
  const QString normalized = endpointKey.trimmed().toLower();
  if (normalized.startsWith("http://") || normalized.startsWith("https://"))
    return endpointKey.trimmed();
  for (const EndpointDefinition &definition : kEndpointDefinitions) {
    if (normalized == QLatin1String(definition.key))
      return QString::fromLatin1(definition.url);
  }
  return QStringLiteral("https://openapi.tuyaus.com");
}

QString endpointLabelFromKey(const QString &endpointKey) {
  const QString normalized = endpointKey.trimmed().toLower();
  for (const EndpointDefinition &definition : kEndpointDefinitions) {
    if (normalized == QLatin1String(definition.key))
      return QString::fromLatin1(definition.label);
  }
  return endpointKey.trimmed().isEmpty() ? QStringLiteral("Custom Endpoint")
                                         : endpointKey.trimmed();
}

QByteArray sha256Hex(const QByteArray &data) {
  return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QByteArray hmacUpper(const QByteArray &payload, const QByteArray &secret) {
  return QMessageAuthenticationCode::hash(payload, secret,
                                          QCryptographicHash::Sha256)
      .toHex()
      .toUpper();
}

QString md5Hex(QString text) {
  return QString::fromLatin1(
      QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Md5).toHex());
}

bool isLikelyHashedPassword(const QString &password) {
  static const QRegularExpression kMd5Pattern(QStringLiteral("^[A-Fa-f0-9]{32}$"));
  return kMd5Pattern.match(password.trimmed()).hasMatch();
}

bool isPermissionDeniedText(const QString &text) {
  const QString normalized = text.trimmed().toLower();
  return normalized.contains(QStringLiteral("[1106]")) ||
         normalized.contains(QStringLiteral("permission deny"));
}

bool isPowerLikeCode(const QString &code) {
  const QString normalized = code.trimmed().toLower();
  if (normalized.isEmpty())
    return false;
  if (normalized == QLatin1String("switch") ||
      normalized == QLatin1String("switch_led") ||
      normalized == QLatin1String("power") ||
      normalized == QLatin1String("master_switch") ||
      normalized == QLatin1String("light")) {
    return true;
  }
  return normalized.startsWith(QLatin1String("switch_"));
}

QStringList extractPowerCodes(const QJsonArray &items) {
  QStringList codes;
  for (const QJsonValue &value : items) {
    const QJsonObject object = value.toObject();
    const QString code = object.value(QStringLiteral("code")).toString();
    const QString type = object.value(QStringLiteral("type")).toString();
    if (!isPowerLikeCode(code))
      continue;
    if (!type.isEmpty() && type.compare(QStringLiteral("Boolean"),
                                        Qt::CaseInsensitive) != 0) {
      continue;
    }
    if (!codes.contains(code))
      codes << code;
  }
  return codes;
}

QString choosePrimaryPowerCode(const QStringList &codes) {
  const QStringList preferred = {
      QStringLiteral("switch_led"), QStringLiteral("switch"),
      QStringLiteral("switch_1"),   QStringLiteral("power"),
      QStringLiteral("Power"),      QStringLiteral("master_switch"),
      QStringLiteral("light"),
  };
  for (const QString &candidate : preferred) {
    for (const QString &code : codes) {
      if (code.compare(candidate, Qt::CaseInsensitive) == 0)
        return code;
    }
  }
  return codes.isEmpty() ? QString() : codes.first();
}

bool readPowerState(const QJsonArray &statusItems, const QStringList &powerCodes,
                    bool *found) {
  if (found)
    *found = false;
  for (const QString &powerCode : powerCodes) {
    for (const QJsonValue &value : statusItems) {
      const QJsonObject object = value.toObject();
      if (object.value(QStringLiteral("code")).toString().compare(
              powerCode, Qt::CaseInsensitive) != 0) {
        continue;
      }
      if (found)
        *found = true;
      return object.value(QStringLiteral("value")).toBool();
    }
  }
  return false;
}

QJsonObject functionValuesObject(const QJsonObject &functionObject) {
  const QJsonValue values = functionObject.value(QStringLiteral("values"));
  if (values.isObject())
    return values.toObject();
  if (values.isString()) {
    const QJsonDocument document =
        QJsonDocument::fromJson(values.toString().toUtf8());
    if (document.isObject())
      return document.object();
  }
  return {};
}

bool isBrightnessCode(const QString &code) {
  const QString normalized = code.trimmed().toLower();
  return normalized == QLatin1String("bright_value_v2") ||
         normalized == QLatin1String("bright_value_v1") ||
         normalized == QLatin1String("bright_value") ||
         normalized == QLatin1String("bright") ||
         normalized.startsWith(QLatin1String("bright_"));
}

int brightnessCodePriority(const QString &code) {
  const QString normalized = code.trimmed().toLower();
  if (normalized == QLatin1String("bright_value_v2"))
    return 0;
  if (normalized == QLatin1String("bright_value_v1"))
    return 1;
  if (normalized == QLatin1String("bright_value"))
    return 2;
  if (normalized == QLatin1String("bright"))
    return 3;
  return 10;
}

bool isRgbColorCode(const QString &code) {
  const QString normalized = code.trimmed().toLower();
  return normalized == QLatin1String("colour_data") ||
         normalized == QLatin1String("color_data") ||
         normalized == QLatin1String("colour_hex") ||
         normalized == QLatin1String("color_hex");
}

bool isPresetColorCode(const QString &code) {
  const QString normalized = code.trimmed().toLower();
  if (isRgbColorCode(code))
    return false;
  return normalized == QLatin1String("colour") ||
         normalized == QLatin1String("color") ||
         normalized == QLatin1String("colour_mode") ||
         normalized == QLatin1String("color_mode");
}

bool isIntegerLikeFunctionType(const QString &type) {
  const QString normalized = type.trimmed().toLower();
  return normalized.isEmpty() || normalized == QLatin1String("integer") ||
         normalized == QLatin1String("value") || normalized == QLatin1String("number") ||
         normalized == QLatin1String("int");
}

bool isEnumLikeFunctionType(const QString &type) {
  const QString normalized = type.trimmed().toLower();
  return normalized.isEmpty() || normalized == QLatin1String("enum") ||
         normalized == QLatin1String("string") ||
         normalized == QLatin1String("value");
}

bool isColorTemperatureCode(const QString &code) {
  const QString normalized = code.trimmed().toLower();
  if (isRgbColorCode(code))
    return false;
  if (normalized == QLatin1String("temp_value") ||
      normalized == QLatin1String("colour_temp") ||
      normalized == QLatin1String("color_temp") ||
      normalized == QLatin1String("colour_temperature") ||
      normalized == QLatin1String("color_temperature") ||
      normalized == QLatin1String("color_temp_v2") ||
      normalized == QLatin1String("colour_temp_v2") ||
      normalized == QLatin1String("temp") || normalized == QLatin1String("ct")) {
    return true;
  }
  if (normalized.contains(QLatin1String("temp")) &&
      (normalized.contains(QLatin1String("colour")) ||
       normalized.contains(QLatin1String("color")) ||
       normalized.endsWith(QLatin1String("_value")))) {
    return true;
  }
  return normalized.contains(QLatin1String("color_temp")) ||
         normalized.contains(QLatin1String("colour_temp"));
}

bool isWarmCoolSceneCode(const QString &code) {
  const QString normalized = code.trimmed().toLower();
  return normalized == QLatin1String("scene") ||
         normalized == QLatin1String("scene_id") ||
         normalized == QLatin1String("scene_data") ||
         normalized == QLatin1String("light_scene") ||
         normalized == QLatin1String("scene_select");
}

QString inferFunctionTypeFromStatusValue(const QJsonValue &value) {
  if (value.isBool())
    return QStringLiteral("Boolean");
  if (value.isDouble())
    return QStringLiteral("Integer");
  if (value.isObject() || value.isArray())
    return QStringLiteral("Json");
  return QStringLiteral("String");
}

QJsonArray buildSyntheticFunctionsFromStatus(const QJsonArray &status) {
  QJsonArray synthetic;
  for (const QJsonValue &value : status) {
    const QJsonObject object = value.toObject();
    const QString code = object.value(QStringLiteral("code")).toString().trimmed();
    if (code.isEmpty())
      continue;
    QJsonObject function;
    function.insert(QStringLiteral("code"), code);
    function.insert(QStringLiteral("type"),
                    inferFunctionTypeFromStatusValue(object.value(QStringLiteral("value"))));
    synthetic.append(function);
  }
  return synthetic;
}

QJsonValue statusValueForCode(const QJsonArray &statusItems, const QString &code);
void applyWarmWhiteTemperatureTiles(SmartLifeDeviceInfo *device, int tempMin,
                                    int tempMax);

QJsonArray mergeFunctionsWithStatus(const QJsonArray &functions,
                                  const QJsonArray &status) {
  QJsonArray merged = functions;
  QSet<QString> knownCodes;
  for (const QJsonValue &value : functions) {
    const QString code =
        value.toObject().value(QStringLiteral("code")).toString().trimmed();
    if (!code.isEmpty())
      knownCodes.insert(code.toLower());
  }
  for (const QJsonValue &value : status) {
    const QJsonObject object = value.toObject();
    const QString code = object.value(QStringLiteral("code")).toString().trimmed();
    if (code.isEmpty() || knownCodes.contains(code.toLower()))
      continue;
    knownCodes.insert(code.toLower());
    QJsonObject function;
    function.insert(QStringLiteral("code"), code);
    function.insert(QStringLiteral("type"),
                    inferFunctionTypeFromStatusValue(object.value(QStringLiteral("value"))));
    merged.append(function);
  }
  return merged;
}

void supplementLightingFromFunctionCodes(SmartLifeDeviceInfo *device,
                                         const QJsonArray &status) {
  if (!device)
    return;

  if (device->colorCapability == SmartLifeColorCapability::None) {
    for (const QString &code : device->functionCodes) {
      if (!isRgbColorCode(code))
        continue;
      device->colorCapability = SmartLifeColorCapability::Rgb;
      device->colorCode = code;
      device->colorValueType = QStringLiteral("Json");
      break;
    }
  }

  if (device->colorCapability == SmartLifeColorCapability::None) {
    QString tempCode;
    int tempMin = 0;
    int tempMax = 1000;
    for (const QString &code : device->functionCodes) {
      if (!isColorTemperatureCode(code))
        continue;
      tempCode = code;
      const QJsonValue liveValue = statusValueForCode(status, code);
      if (liveValue.isDouble()) {
        const int live = liveValue.toInt();
        tempMin = qMin(tempMin, qMax(0, live - 200));
        tempMax = qMax(tempMax, live + 200);
      }
    }
    if (!tempCode.isEmpty()) {
      device->colorCapability = SmartLifeColorCapability::Preset;
      device->colorCode = tempCode;
      device->colorValueType = QStringLiteral("Integer");
      applyWarmWhiteTemperatureTiles(device, tempMin, tempMax);
    }
  }

  if (device->colorCapability == SmartLifeColorCapability::None) {
    for (const QString &code : device->functionCodes) {
      if (!isPresetColorCode(code))
        continue;
      device->colorCapability = SmartLifeColorCapability::Preset;
      device->colorCode = code;
      device->colorValueType = QStringLiteral("Enum");
      if (device->presetColorLabels.isEmpty())
        applyWarmWhiteTemperatureTiles(device, 0, 1000);
      break;
    }
  }
}

void mergeStatusCodesIntoFunctionCodes(SmartLifeDeviceInfo *device,
                                       const QJsonArray &status) {
  if (!device)
    return;
  for (const QJsonValue &value : status) {
    const QString code =
        value.toObject().value(QStringLiteral("code")).toString().trimmed();
    if (!code.isEmpty() && !device->functionCodes.contains(code))
      device->functionCodes << code;
  }
}

bool presetRangeLooksLikeWarmCool(const QStringList &labels) {
  for (const QString &label : labels) {
    const QString normalized = label.trimmed().toLower();
    if (normalized.contains(QLatin1String("warm")) ||
        normalized.contains(QLatin1String("cool")) ||
        normalized.contains(QLatin1String("cold")) ||
        normalized.contains(QLatin1String("white")) ||
        normalized.contains(QLatin1String("daylight")) ||
        normalized.contains(QLatin1String("neutral"))) {
      return true;
    }
  }
  return false;
}

void applyWarmWhiteTemperatureTiles(SmartLifeDeviceInfo *device, int tempMin,
                                    int tempMax) {
  if (!device)
    return;
  if (tempMax <= tempMin)
    tempMax = tempMin + 1000;

  struct TemperatureTile {
    const char *label;
    double fraction;
  };
  static const TemperatureTile kTiles[] = {
      {"Warm", 0.0},       {"Soft White", 0.22}, {"White", 0.45},
      {"Neutral", 0.62},   {"Cool", 0.82},       {"Daylight", 1.0},
  };

  device->presetColorLabels.clear();
  device->presetColorCommandValues.clear();
  for (const TemperatureTile &tile : kTiles) {
    const int value =
        tempMin + qRound((tempMax - tempMin) * qBound(0.0, tile.fraction, 1.0));
    device->presetColorLabels << QString::fromLatin1(tile.label);
    device->presetColorCommandValues << QString::number(value);
  }
}

bool isWorkModeCode(const QString &code) {
  return code.trimmed().compare(QStringLiteral("work_mode"), Qt::CaseInsensitive) == 0;
}

QJsonValue jsonCommandValue(const QString &typeHint, const QString &rawValue) {
  const QString type = typeHint.trimmed();
  if (type.compare(QStringLiteral("Integer"), Qt::CaseInsensitive) == 0 ||
      type.compare(QStringLiteral("Value"), Qt::CaseInsensitive) == 0) {
    bool ok = false;
    const int value = rawValue.toInt(&ok);
    if (ok)
      return value;
  }

  bool ok = false;
  const int numeric = rawValue.toInt(&ok);
  if (ok && rawValue == QString::number(numeric))
    return numeric;
  return rawValue;
}

QJsonValue presetCommandValue(const SmartLifeDeviceInfo &device,
                              const QString &commandValue) {
  if (isColorTemperatureCode(device.colorCode)) {
    bool ok = false;
    const int value = commandValue.toInt(&ok);
    if (ok)
      return value;
  }
  return jsonCommandValue(device.colorValueType, commandValue);
}

QString workModeValueForWhite(const SmartLifeDeviceInfo &device) {
  return device.workModeWhiteValue.isEmpty() ? QStringLiteral("white")
                                             : device.workModeWhiteValue;
}

QString workModeValueForColour(const SmartLifeDeviceInfo &device) {
  return device.workModeColourValue.isEmpty() ? QStringLiteral("colour")
                                            : device.workModeColourValue;
}

QJsonValue statusValueForCode(const QJsonArray &statusItems, const QString &code) {
  for (const QJsonValue &value : statusItems) {
    const QJsonObject object = value.toObject();
    if (object.value(QStringLiteral("code")).toString().compare(code,
                                                               Qt::CaseInsensitive) == 0) {
      return object.value(QStringLiteral("value"));
    }
  }
  return {};
}

QString encodeTuyaColourData(const QColor &color) {
  int hue = 0;
  int saturation = 0;
  int value = 0;
  color.getHsv(&hue, &saturation, &value);
  if (hue < 0)
    hue = 0;
  const int scaledSat = qBound(0, qRound(saturation / 255.0 * 1000.0), 1000);
  const int scaledVal = qBound(0, qRound(value / 255.0 * 1000.0), 1000);
  const auto toHex = [](int number) {
    return QString::number(number, 16).rightJustified(4, QLatin1Char('0')).toUpper();
  };
  return toHex(hue) + toHex(scaledSat) + toHex(scaledVal);
}

QColor decodeTuyaColourData(const QString &encoded) {
  const QString compact = encoded.trimmed();
  if (compact.size() < 12)
    return QColor();
  bool ok = false;
  const int hue = compact.mid(0, 4).toInt(&ok, 16);
  if (!ok)
    return QColor();
  const int saturation = compact.mid(4, 4).toInt(&ok, 16);
  if (!ok)
    return QColor();
  const int value = compact.mid(8, 4).toInt(&ok, 16);
  if (!ok)
    return QColor();
  QColor color;
  color.setHsv(hue, qRound(saturation / 1000.0 * 255.0),
               qRound(value / 1000.0 * 255.0));
  return color;
}

QColor guessPresetColor(const QString &label) {
  const QString normalized = label.trimmed().toLower();
  struct NamedColor {
    const char *name;
    const char *hex;
  };
  static const NamedColor kNamedColors[] = {
      {"red", "#FF3B30"},       {"green", "#34C759"},     {"blue", "#007AFF"},
      {"yellow", "#FFCC00"},    {"cyan", "#32ADE6"},      {"magenta", "#FF2D55"},
      {"purple", "#AF52DE"},    {"orange", "#FF9500"},    {"pink", "#FF6482"},
      {"white", "#FFFFFF"},     {"warm", "#FFD9A8"},      {"warmwhite", "#FFD9A8"},
      {"cool", "#D6ECFF"},      {"coolwhite", "#D6ECFF"}, {"daylight", "#F4F8FF"},
      {"night", "#FFB347"},     {"sleep", "#FF8C69"},     {"reading", "#FFF1C1"},
      {"relax", "#C9B6FF"},     {"party", "#FF5AF7"},     {"romantic", "#FF4F81"},
  };
  for (const NamedColor &entry : kNamedColors) {
    if (normalized.contains(QLatin1String(entry.name)))
      return QColor(QString::fromLatin1(entry.hex));
  }
  uint hash = qHash(normalized);
  return QColor::fromHsv(static_cast<int>(hash % 360), 200, 230);
}

bool nameLooksLikeLighting(const QString &name, const QString &productName) {
  const QString normalized =
      normalizedMatchText(name + QLatin1Char(' ') + productName);
  const QStringList hints = {QStringLiteral("light"),   QStringLiteral("lights"),
                             QStringLiteral("lamp"),    QStringLiteral("lamps"),
                             QStringLiteral("bulb"),    QStringLiteral("led"),
                             QStringLiteral("strip"),   QStringLiteral("ceiling"),
                             QStringLiteral("bedside"), QStringLiteral("desk")};
  for (const QString &hint : hints) {
    if (normalized.contains(hint))
      return true;
  }
  return false;
}

bool categoryLooksLikeLighting(const QString &category) {
  const QString normalized = category.trimmed().toLower();
  return normalized == QLatin1String("dj") || normalized == QLatin1String("dd") ||
         normalized == QLatin1String("kg") || normalized == QLatin1String("cz") ||
         normalized == QLatin1String("tgkg") || normalized == QLatin1String("dc");
}

bool deviceHasLightingFunctionCodes(const SmartLifeDeviceInfo &device) {
  for (const QString &code : device.functionCodes) {
    if (isBrightnessCode(code) || isColorTemperatureCode(code) ||
        isPresetColorCode(code) || isRgbColorCode(code) || isWarmCoolSceneCode(code)) {
      return true;
    }
  }
  return false;
}

bool deviceFunctionCodeExists(const SmartLifeDeviceInfo &device,
                              const QString &code) {
  if (code.trimmed().isEmpty())
    return false;
  for (const QString &entry : device.functionCodes) {
    if (entry.compare(code, Qt::CaseInsensitive) == 0)
      return true;
  }
  return false;
}

bool deviceHasVerifiedBrightnessControl(const SmartLifeDeviceInfo &device) {
  return device.supportsBrightness &&
         deviceFunctionCodeExists(device, device.brightnessCode);
}

bool deviceHasVerifiedColorControl(const SmartLifeDeviceInfo &device) {
  if (device.colorCapability == SmartLifeColorCapability::None ||
      device.colorCode.trimmed().isEmpty()) {
    return false;
  }
  if (!deviceFunctionCodeExists(device, device.colorCode))
    return false;
  if (device.colorCapability == SmartLifeColorCapability::Preset)
    return !device.presetColorLabels.isEmpty();
  return device.colorCapability == SmartLifeColorCapability::Rgb;
}

bool deviceLooksLikeLighting(const SmartLifeDeviceInfo &device) {
  return device.controllable &&
         (device.likelyLighting || device.powerCodes.contains(
                                         QStringLiteral("switch_led"),
                                         Qt::CaseInsensitive) ||
          deviceHasLightingFunctionCodes(device) ||
          device.supportsBrightness ||
          device.colorCapability != SmartLifeColorCapability::None ||
          categoryLooksLikeLighting(device.category) ||
          nameLooksLikeLighting(device.name, device.productName));
}

struct RequestResult {
  bool ok = false;
  int httpStatus = 0;
  QJsonDocument document;
  QString errorText;
  QByteArray rawBody;
};

} // namespace

SmartLifeManager::SmartLifeManager(QObject *parent) : QObject(parent) {
  m_network = new QNetworkAccessManager(this);
  m_statusText = QStringLiteral("Smart Life is not connected");
  loadDeviceAliases();
}

QStringList SmartLifeManager::endpointKeys() {
  QStringList keys;
  for (const EndpointDefinition &definition : kEndpointDefinitions)
    keys << QString::fromLatin1(definition.key);
  return keys;
}

QString SmartLifeManager::endpointLabel(const QString &endpointKey) {
  return endpointLabelFromKey(endpointKey);
}

QString SmartLifeManager::endpointUrl(const QString &endpointKey) {
  return endpointUrlFromKey(endpointKey);
}

QStringList SmartLifeManager::appSchemaChoices() {
  return {QStringLiteral("smartlife"), QStringLiteral("tuyaSmart")};
}

namespace {

QString responseMessage(const RequestResult &result) {
  QString code;
  if (result.document.isObject()) {
    const QJsonObject object = result.document.object();
    code = object.value(QStringLiteral("code")).toVariant().toString().trimmed();
    const QString msg = object.value(QStringLiteral("msg")).toString().trimmed();
    QString baseMessage = msg;
    const QString message =
        object.value(QStringLiteral("message")).toString().trimmed();
    if (baseMessage.isEmpty())
      baseMessage = message;

    if (!baseMessage.isEmpty()) {
      QString hint;
      if (code == QLatin1String("1001")) {
        hint = QStringLiteral("The Tuya Access Key looks invalid for this project.");
      } else if (code == QLatin1String("1004")) {
        hint = QStringLiteral("The request signature was rejected. Check Access ID, Access Key, and selected data center.");
      } else if (code == QLatin1String("1008")) {
        hint = QStringLiteral("The Tuya Access ID is invalid.");
      } else if (code == QLatin1String("1010") || code == QLatin1String("1011") ||
                 code == QLatin1String("1400")) {
        hint = QStringLiteral("The Tuya session expired or became invalid. Retry Connect.");
      } else if (code == QLatin1String("1100") || code == QLatin1String("1102") ||
                 code == QLatin1String("1109")) {
        hint = QStringLiteral("One or more required login fields are missing or invalid.");
      } else if (code == QLatin1String("1106")) {
        hint = QStringLiteral("Permission was denied. In Smart Life mode, this usually means the Smart Life account is not linked to the Tuya cloud project, the wrong data center was selected, the project lacks Smart Home API permissions, or the app account credentials are incorrect.");
      } else if (code == QLatin1String("2002")) {
        hint = QStringLiteral("This Smart Life user does not have any accessible devices in the linked project.");
      } else if (code == QLatin1String("2006")) {
        hint = QStringLiteral("The Smart Life user was not found. Check username, country code, and account type.");
      } else if (code == QLatin1String("2017")) {
        hint = QStringLiteral("The selected app schema is invalid. Try Smart Life or Tuya Smart.");
      } else if (code == QLatin1String("2021")) {
        hint = QStringLiteral("The email address is invalid for this Smart Life account.");
      } else if (code == QLatin1String("2022")) {
        hint = QStringLiteral("The phone number or country code is invalid for this Smart Life account.");
      } else if (code == QLatin1String("28841107")) {
        hint = QStringLiteral("The selected Tuya data center is suspended for this cloud project. Open Tuya Cloud Development and enable that data center or switch to the active region for this project.");
      }

      if (!hint.isEmpty())
        baseMessage += QStringLiteral(" Hint: %1").arg(hint);
      if (!code.isEmpty())
        baseMessage = QStringLiteral("[%1] %2").arg(code, baseMessage);
      return baseMessage;
    }
  }
  return result.errorText.trimmed();
}

bool isGenericSmartLifeError(const QString &text) {
  const QString normalized = text.trimmed().toLower();
  return normalized.isEmpty() ||
         normalized.contains(QStringLiteral("unknown error")) ||
         normalized == QLatin1String("request fail") ||
         normalized == QLatin1String("request failed");
}

bool responseSucceeded(const RequestResult &result, QString *errorText = nullptr) {
  if (!result.ok) {
    if (errorText)
      *errorText = responseMessage(result);
    return false;
  }
  if (!result.document.isObject()) {
    if (errorText)
      *errorText = QStringLiteral("Tuya returned an invalid response");
    return false;
  }
  const QJsonObject object = result.document.object();
  if (!object.value(QStringLiteral("success")).toBool()) {
    if (errorText)
      *errorText = responseMessage(result).isEmpty()
                       ? QStringLiteral("Tuya request failed")
                       : responseMessage(result);
    return false;
  }
  return true;
}

QJsonValue responseResultValue(const RequestResult &result) {
  if (!result.document.isObject())
    return {};
  return result.document.object().value(QStringLiteral("result"));
}

QJsonObject responseResultObject(const RequestResult &result) {
  return responseResultValue(result).toObject();
}

QJsonArray responseResultArray(const RequestResult &result) {
  const QJsonValue value = responseResultValue(result);
  if (value.isArray())
    return value.toArray();
  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (const QString &key : {QStringLiteral("devices"),
                               QStringLiteral("functions"),
                               QStringLiteral("status"),
                               QStringLiteral("specification"),
                               QStringLiteral("specifications")}) {
      const QJsonValue nested = object.value(key);
      if (nested.isArray())
        return nested.toArray();
    }
  }
  return {};
}

QJsonArray responseResultArrayForKey(const RequestResult &result,
                                     const QString &preferredKey) {
  const QJsonValue value = responseResultValue(result);
  if (value.isArray())
    return value.toArray();
  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    if (!preferredKey.isEmpty()) {
      const QJsonValue preferred = object.value(preferredKey);
      if (preferred.isArray())
        return preferred.toArray();
    }
    return responseResultArray(result);
  }
  return {};
}

RequestResult performSignedRequest(QNetworkAccessManager *network,
                                   const SmartLifeManager::Config &config,
                                   const QString &method, const QString &path,
                                   const QUrlQuery &query,
                                   const QJsonObject &bodyObject,
                                   const QString &accessToken,
                                   bool tokenOperation) {
  RequestResult result;
  if (!network) {
    result.errorText = QStringLiteral("Network manager is not available");
    return result;
  }

  const QUrl baseUrl(endpointUrlFromKey(config.endpointKey));
  QUrlQuery sortedQuery;
  const auto queryItems = query.queryItems(QUrl::FullyDecoded);
  if (!queryItems.isEmpty()) {
    QList<QPair<QString, QString>> sortedItems = queryItems;
    std::sort(sortedItems.begin(), sortedItems.end(),
              [](const QPair<QString, QString> &left,
                 const QPair<QString, QString> &right) {
                if (left.first == right.first)
                  return left.second < right.second;
                return left.first < right.first;
              });
    for (const auto &item : sortedItems)
      sortedQuery.addQueryItem(item.first, item.second);
  }

  QUrl url(baseUrl.resolved(QUrl(path)));
  if (!sortedQuery.isEmpty())
    url.setQuery(sortedQuery);

  const QByteArray bodyBytes =
      bodyObject.isEmpty()
          ? QByteArray()
          : QJsonDocument(bodyObject).toJson(QJsonDocument::Compact);
  const QByteArray contentHash = sha256Hex(bodyBytes);
  const QByteArray timestamp =
      QByteArray::number(QDateTime::currentMSecsSinceEpoch());
  const QByteArray nonce =
      QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
  const QByteArray signedHeaders = QByteArrayLiteral("client_id");
  const QByteArray headersToSign =
      QByteArrayLiteral("client_id:") + config.accessId.toUtf8() + '\n';
  const QString queryString = sortedQuery.toString(QUrl::FullyEncoded);
  QByteArray stringToSign = method.toUpper().toUtf8() + '\n' + contentHash +
                            '\n' + headersToSign + '\n' + path.toUtf8();
  if (!queryString.isEmpty())
    stringToSign += '?' + queryString.toUtf8();

  QByteArray signPayload = config.accessId.toUtf8();
  if (!tokenOperation)
    signPayload += accessToken.toUtf8();
  signPayload += timestamp + nonce + stringToSign;

  QNetworkRequest request(url);
  request.setRawHeader("client_id", config.accessId.toUtf8());
  request.setRawHeader("sign_method", "HMAC-SHA256");
  request.setRawHeader("t", timestamp);
  request.setRawHeader("nonce", nonce);
  request.setRawHeader("Signature-Headers", signedHeaders);
  request.setRawHeader("sign", hmacUpper(signPayload, config.accessKey.toUtf8()));
  if (!tokenOperation)
    request.setRawHeader("access_token", accessToken.toUtf8());
  request.setRawHeader("lang", "en");
  if (!bodyBytes.isEmpty())
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

  QNetworkReply *reply = nullptr;
  const QString upperMethod = method.trimmed().toUpper();
  if (upperMethod == QLatin1String("GET")) {
    reply = network->get(request);
  } else if (upperMethod == QLatin1String("POST")) {
    reply = network->post(request, bodyBytes);
  } else {
    reply = network->sendCustomRequest(request, upperMethod.toUtf8(), bodyBytes);
  }

  QEventLoop loop;
  QTimer timeoutTimer;
  timeoutTimer.setSingleShot(true);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&timeoutTimer, &QTimer::timeout, reply, &QNetworkReply::abort);
  QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
  timeoutTimer.start(25000);
  loop.exec();
  timeoutTimer.stop();

  result.httpStatus =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  result.rawBody = reply->readAll();

  QJsonParseError parseError;
  result.document = QJsonDocument::fromJson(result.rawBody, &parseError);

  if (reply->error() != QNetworkReply::NoError) {
    result.errorText = reply->errorString();
    if (!result.document.isObject()) {
      const QString bodyText = QString::fromUtf8(result.rawBody).trimmed();
      if (!bodyText.isEmpty())
        result.errorText = bodyText;
    }
    reply->deleteLater();
    return result;
  }

  if (parseError.error != QJsonParseError::NoError) {
    result.errorText = QStringLiteral("Unable to parse Tuya response");
    reply->deleteLater();
    return result;
  }

  result.ok = true;
  reply->deleteLater();
  return result;
}

QStringList powerCodesForIntent(const SmartLifeDeviceInfo &device) {
  if (!device.powerCodes.isEmpty())
    return device.powerCodes;
  if (!device.primaryPowerCode.isEmpty())
    return {device.primaryPowerCode};
  return {};
}

QString accountModeDisplay(const QString &mode) {
  return mode.trimmed().compare(QStringLiteral("developer"), Qt::CaseInsensitive) == 0
             ? QStringLiteral("Tuya Developer Project")
             : QStringLiteral("Smart Life Account");
}

} // namespace

SmartLifeManager::Config SmartLifeManager::loadConfig() const {
  QSettings settings("QuickSTT", "Config");
  Config config;
  config.accountMode =
      settings.value("smartLife/accountMode", "smartlife").toString().trimmed();
  config.endpointKey =
      settings.value("smartLife/endpointKey", "western_america").toString().trimmed();
  config.accessId = settings.value("smartLife/accessId").toString().trimmed();
  config.accessKey =
      loadProtectedSetting(settings, QStringLiteral("smartLife/accessKey"))
          .trimmed();
  config.developerUid =
      settings.value("smartLife/developerUid").toString().trimmed();
  config.developerHomeIds =
      settings.value("smartLife/developerHomeIds").toString();
  config.username =
      settings.value("smartLife/username").toString().trimmed();
  config.password =
      loadProtectedSetting(settings, QStringLiteral("smartLife/password"));
  config.countryCode =
      settings.value("smartLife/countryCode", "1").toString().trimmed();
  config.appSchema =
      settings.value("smartLife/appSchema", "smartlife").toString().trimmed();
  config.passwordAlreadyMd5 =
      settings.value("smartLife/passwordAlreadyMd5", false).toBool();
  const bool hasDeveloperConfig =
      !config.developerUid.trimmed().isEmpty() ||
      !config.developerHomeIds.trimmed().isEmpty();
  const bool hasSmartLifeCredentials =
      !config.username.trimmed().isEmpty() || !config.password.trimmed().isEmpty();
  if (config.accountMode.compare(QStringLiteral("developer"),
                                 Qt::CaseInsensitive) != 0) {
    config.accountMode = QStringLiteral("smartlife");
  } else if (!hasDeveloperConfig && hasSmartLifeCredentials) {
    config.accountMode = QStringLiteral("smartlife");
    settings.setValue(QStringLiteral("smartLife/accountMode"), config.accountMode);
    settings.sync();
  }
  if (config.endpointKey.isEmpty())
    config.endpointKey = QStringLiteral("western_america");
  if (config.countryCode.isEmpty())
    config.countryCode = QStringLiteral("1");
  if (config.appSchema.isEmpty())
    config.appSchema = QStringLiteral("smartlife");
  return config;
}

QString SmartLifeManager::connectionSummaryText() const {
  const Config config = loadConfig();
  QStringList lines;
  lines << QStringLiteral("Mode: %1").arg(accountModeDisplay(config.accountMode));
  lines << QStringLiteral("Endpoint: %1 (%2)")
               .arg(endpointLabelFromKey(config.endpointKey),
                    endpointUrlFromKey(config.endpointKey));
  lines << QStringLiteral("Session: %1")
               .arg(m_connected ? QStringLiteral("Connected")
                                : QStringLiteral("Not connected"));
  if (config.accountMode == QLatin1String("developer")) {
    lines << QStringLiteral("Developer UID: %1")
                 .arg(config.developerUid.isEmpty() ? QStringLiteral("Not set")
                                                    : config.developerUid);
    const QStringList homeIds = normalizeConfiguredHomeIds(config.developerHomeIds);
    lines << QStringLiteral("Configured Home IDs: %1")
                 .arg(homeIds.isEmpty() ? QStringLiteral("None")
                                        : homeIds.join(QStringLiteral(", ")));
  } else {
    lines << QStringLiteral("Smart Life Username: %1")
                 .arg(config.username.isEmpty() ? QStringLiteral("Not set")
                                               : config.username);
    lines << QStringLiteral("Country Code: %1").arg(config.countryCode);
    lines << QStringLiteral("App Schema: %1").arg(config.appSchema);
    lines << QStringLiteral("Resolved UID: %1")
                 .arg(m_uid.isEmpty() ? QStringLiteral("Not resolved") : m_uid);
  }
  lines << QStringLiteral("Cached Homes: %1").arg(m_homes.size());
  lines << QStringLiteral("Cached Devices: %1").arg(m_devices.size());
  lines << QStringLiteral("Status: %1")
               .arg(m_statusText.isEmpty() ? QStringLiteral("Idle") : m_statusText);
  return lines.join(QLatin1Char('\n'));
}

QString SmartLifeManager::commandHelpText() const {
  return QStringLiteral(
      "Quick setup:\n"
      "1. Choose Smart Life Account or Tuya Developer Project.\n"
      "2. Select the correct Tuya data center for your account.\n"
      "3. Enter the required credentials.\n"
      "4. Press Connect, then Sync Devices.\n\n"
      "Important:\n"
      "- Smart Life mode still requires a Tuya cloud project Access ID and Access Key.\n"
      "- The Smart Life or Tuya Smart app account must be linked to that project.\n"
      "- If Connect fails with permission errors, the project link, region, or API permissions are usually the bottleneck.\n"
      "- You do not enable a special function inside the Smart Life phone app for QuickSTT.\n"
      "- Colour and warm/cool tiles only appear when Tuya reports a real colour or "
      "temperature data point for that bulb after Sync Devices.\n"
      "- Simple on/off-only bulbs cannot change colour from this app even if the "
      "Smart Life app shows scenes.\n\n"
      "Voice control examples:\n"
      "- turn on bedroom lights\n"
      "- turn bedroom lights on\n"
      "- lights on\n"
      "- turn off bedroom 2 lights\n"
      "- turn on desk lamp\n"
      "- switch off living room lights\n"
      "- turn on reading lamp\n"
      "- turn off sofa light\n"
      "- power off all lights");
}

SmartLifeDeviceInfo SmartLifeManager::deviceById(const QString &deviceId) const {
  for (const SmartLifeDeviceInfo &device : m_devices) {
    if (device.id == deviceId)
      return device;
  }
  return {};
}

QString SmartLifeManager::deviceAlias(const QString &deviceId) const {
  return m_deviceAliases.value(deviceId).trimmed();
}

QString SmartLifeManager::deviceDisplayName(const SmartLifeDeviceInfo &device) const {
  const QString alias = deviceAlias(device.id);
  if (!alias.isEmpty())
    return alias;
  return device.name.isEmpty() ? device.id : device.name;
}

QString SmartLifeManager::deviceDisplayName(const QString &deviceId) const {
  return deviceDisplayName(deviceById(deviceId));
}

QString SmartLifeManager::deviceDetailText(const QString &deviceId) const {
  const SmartLifeDeviceInfo device = deviceById(deviceId);
  if (device.id.isEmpty())
    return QStringLiteral("Select a Smart Life device to view its details.");

  QStringList lines;
  lines << deviceDisplayName(device);
  const QString alias = deviceAlias(device.id);
  if (!alias.isEmpty())
    lines << QStringLiteral("Original Name: %1").arg(device.name.isEmpty() ? device.id
                                                                           : device.name);
  lines << QStringLiteral("Device ID: %1").arg(device.id);
  lines << QStringLiteral("Home: %1")
               .arg(device.homeName.isEmpty() ? QStringLiteral("Unknown")
                                              : device.homeName);
  lines << QStringLiteral("Room: %1")
               .arg(device.roomName.isEmpty() ? QStringLiteral("Unassigned")
                                              : device.roomName);
  lines << QStringLiteral("Category: %1")
               .arg(device.category.isEmpty() ? QStringLiteral("Unknown")
                                              : device.category);
  lines << QStringLiteral("Product: %1")
               .arg(device.productName.isEmpty() ? QStringLiteral("Unknown")
                                                 : device.productName);
  lines << QStringLiteral("Online: %1").arg(device.online ? "Yes" : "No");
  lines << QStringLiteral("Controllable: %1")
               .arg(device.controllable ? "Yes" : "No");
  lines << QStringLiteral("Power State: %1").arg(device.powerOn ? "On" : "Off");
  lines << QStringLiteral("Primary Power Code: %1")
               .arg(device.primaryPowerCode.isEmpty()
                        ? QStringLiteral("Not detected")
                        : device.primaryPowerCode);
  lines << QStringLiteral("All Power Codes: %1")
               .arg(device.powerCodes.isEmpty()
                        ? QStringLiteral("None")
                        : device.powerCodes.join(QStringLiteral(", ")));
  lines << QStringLiteral("Lighting Candidate: %1")
               .arg(device.likelyLighting ? "Yes" : "No");
  if (device.supportsBrightness) {
    lines << QStringLiteral("Brightness: %1 (%2-%3)")
                 .arg(device.brightness)
                 .arg(device.brightnessMin)
                 .arg(device.brightnessMax);
  }
  if (!device.functionCodes.isEmpty()) {
    lines << QStringLiteral("API Function Codes: %1")
                 .arg(device.functionCodes.join(QStringLiteral(", ")));
  }
  lines << QStringLiteral("Brightness API: %1")
               .arg(deviceHasVerifiedBrightnessControl(device) ? QStringLiteral("Yes")
                                                               : QStringLiteral("No"));
  lines << QStringLiteral("Colour API: %1")
               .arg(deviceHasVerifiedColorControl(device) ? QStringLiteral("Yes")
                                                          : QStringLiteral("No"));
  if (device.colorCapability == SmartLifeColorCapability::Rgb) {
    lines << QStringLiteral("Color Mode: Full RGB (%1)")
                 .arg(device.colorCode.isEmpty() ? QStringLiteral("unknown")
                                                : device.colorCode);
  } else if (device.colorCapability == SmartLifeColorCapability::Preset) {
    lines << QStringLiteral("Color Mode: %1 (%2)")
                 .arg(device.colorValueType.compare(QStringLiteral("Integer"),
                                                    Qt::CaseInsensitive) == 0
                        ? QStringLiteral("Warm / Cool White")
                        : QStringLiteral("Preset"))
                 .arg(device.colorCode.isEmpty() ? QStringLiteral("unknown")
                                                : device.colorCode);
    if (!device.presetColorLabels.isEmpty())
      lines << QStringLiteral("Color Options: %1")
                   .arg(device.presetColorLabels.join(QStringLiteral(", ")));
  } else if (device.likelyLighting && !deviceHasVerifiedColorControl(device)) {
    lines << QStringLiteral(
        "Colour: not detected after sync. Press Sync Devices again. If Colour API "
        "stays No, the cloud project may not expose this bulb's colour data point "
        "(the Smart Life app can still use local/scene control).");
  }
  return lines.join(QLatin1Char('\n'));
}

void SmartLifeManager::setDeviceAlias(const QString &deviceId, const QString &alias) {
  const QString cleanId = deviceId.trimmed();
  if (cleanId.isEmpty())
    return;
  const QString cleanAlias = alias.trimmed();
  if (cleanAlias.isEmpty()) {
    if (!m_deviceAliases.remove(cleanId))
      return;
  } else {
    if (m_deviceAliases.value(cleanId) == cleanAlias)
      return;
    m_deviceAliases.insert(cleanId, cleanAlias);
  }
  saveDeviceAliases();
  emit devicesChanged();
}

void SmartLifeManager::loadDeviceAliases() {
  m_deviceAliases.clear();
  QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
  const QJsonDocument document = QJsonDocument::fromJson(
      settings.value(QStringLiteral("smartLife/deviceAliasesJson"))
          .toString()
          .toUtf8());
  if (!document.isObject())
    return;
  const QJsonObject object = document.object();
  for (auto it = object.begin(); it != object.end(); ++it) {
    const QString alias = it.value().toString().trimmed();
    if (!alias.isEmpty())
      m_deviceAliases.insert(it.key(), alias);
  }
}

void SmartLifeManager::saveDeviceAliases() const {
  QJsonObject object;
  for (auto it = m_deviceAliases.begin(); it != m_deviceAliases.end(); ++it) {
    if (!it.value().trimmed().isEmpty())
      object.insert(it.key(), it.value().trimmed());
  }
  QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
  settings.setValue(QStringLiteral("smartLife/deviceAliasesJson"),
                    QString::fromUtf8(
                        QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void SmartLifeManager::setStatus(const QString &statusText) {
  if (m_statusText == statusText)
    return;
  m_statusText = statusText;
  qInfo() << "[SMARTLIFE]" << m_statusText;
  emit statusChanged(m_statusText);
}

void SmartLifeManager::setConnected(bool connected) {
  if (m_connected == connected)
    return;
  m_connected = connected;
  emit connectionChanged(m_connected);
}

void SmartLifeManager::clearCache() {
  m_homes.clear();
  m_rooms.clear();
  m_devices.clear();
  emit devicesChanged();
}

QStringList SmartLifeManager::normalizeConfiguredHomeIds(const QString &rawIds) const {
  QString normalized = rawIds;
  normalized.replace('\n', ',');
  normalized.replace(';', ',');
  QStringList values = normalized.split(',', Qt::SkipEmptyParts);
  for (QString &value : values)
    value = value.trimmed();
  values.removeAll(QString());
  values.removeDuplicates();
  return values;
}

QString SmartLifeManager::configFingerprint(const Config &config) const {
  return QStringList{
             config.accountMode.trimmed().toLower(),
             config.endpointKey.trimmed().toLower(),
             config.accessId.trimmed(),
             config.accessKey,
             config.developerUid.trimmed(),
             normalizeConfiguredHomeIds(config.developerHomeIds).join(QStringLiteral(",")),
             config.username.trimmed().toLower(),
             config.password,
             config.countryCode.trimmed(),
             config.appSchema.trimmed().toLower(),
             config.passwordAlreadyMd5 ? QStringLiteral("1") : QStringLiteral("0"),
         }
      .join(QStringLiteral("|"));
}

void SmartLifeManager::clearSessionTokens() {
  m_authFingerprint.clear();
  m_projectToken.clear();
  m_projectTokenExpiresAtMs = 0;
  m_userToken.clear();
  m_userTokenExpiresAtMs = 0;
  m_uid.clear();
}

bool SmartLifeManager::requestProjectToken(const Config &config,
                                           QString *errorText) {
  if (config.accessId.isEmpty() || config.accessKey.isEmpty()) {
    if (errorText)
      *errorText = QStringLiteral(
          "Tuya Access ID and Access Key are required. Smart Life mode still needs a linked Tuya cloud project.");
    return false;
  }

  QUrlQuery query;
  query.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("1"));
  const RequestResult result =
      performSignedRequest(m_network, config, QStringLiteral("GET"),
                           QStringLiteral("/v1.0/token"), query, {}, QString(),
                           true);
  QString failure;
  if (!responseSucceeded(result, &failure)) {
    if (errorText)
      *errorText = failure;
    return false;
  }

  const QJsonObject tokenObject = responseResultObject(result);
  m_projectToken = tokenObject.value(QStringLiteral("access_token")).toString();
  const int expireSeconds =
      tokenObject.value(QStringLiteral("expire_time")).toInt(0);
  m_projectTokenExpiresAtMs = QDateTime::currentMSecsSinceEpoch() +
                              (qint64(qMax(60, expireSeconds)) * 1000) - 60000;
  if (m_projectToken.isEmpty()) {
    if (errorText)
      *errorText = QStringLiteral("Tuya project token was empty");
    return false;
  }
  return true;
}

bool SmartLifeManager::loginAssociatedUser(const Config &config,
                                           QString *errorText) {
  if (config.accessId.isEmpty() || config.accessKey.isEmpty()) {
    if (errorText)
      *errorText = QStringLiteral(
          "Tuya Access ID and Access Key are required before Smart Life account login can work.");
    return false;
  }
  if (config.username.isEmpty() || config.password.isEmpty()) {
    if (errorText)
      *errorText = QStringLiteral(
          "Smart Life username and password are required.");
    return false;
  }

  auto tryLoginWithPassword = [&](const QString &passwordCandidate,
                                  QString *localError) -> bool {
    QJsonObject body;
    body.insert(QStringLiteral("username"), config.username);
    body.insert(QStringLiteral("password"), passwordCandidate);
    body.insert(QStringLiteral("country_code"), config.countryCode);
    body.insert(QStringLiteral("schema"), config.appSchema);

    const RequestResult result = performSignedRequest(
        m_network, config, QStringLiteral("POST"),
        QStringLiteral("/v1.0/iot-01/associated-users/actions/authorized-login"),
        {}, body, m_projectToken, false);
    QString failure;
    if (!responseSucceeded(result, &failure)) {
      if (localError)
        *localError = failure;
      return false;
    }

    const QJsonObject tokenObject = responseResultObject(result);
    m_userToken = tokenObject.value(QStringLiteral("access_token")).toString();
    m_uid = tokenObject.value(QStringLiteral("uid")).toString();
    const int expireSeconds =
        tokenObject.value(QStringLiteral("expire_time")).toInt(0);
    m_userTokenExpiresAtMs = QDateTime::currentMSecsSinceEpoch() +
                             (qint64(qMax(60, expireSeconds)) * 1000) - 60000;
    return !m_userToken.isEmpty() && !m_uid.isEmpty();
  };

  QString localError;
  const QString trimmedPassword = config.password.trimmed();
  if (config.passwordAlreadyMd5 || isLikelyHashedPassword(trimmedPassword)) {
    if (!tryLoginWithPassword(trimmedPassword, &localError)) {
      if (errorText)
        *errorText = localError;
      return false;
    }
    return true;
  }

  if (tryLoginWithPassword(config.password, &localError))
    return true;

  QString hashedError;
  if (tryLoginWithPassword(md5Hex(config.password), &hashedError))
    return true;

  if (errorText) {
    if (!hashedError.isEmpty() &&
        (isGenericSmartLifeError(localError) || hashedError.contains('['))) {
      *errorText = hashedError;
    } else if (hashedError.isEmpty()) {
      *errorText = localError;
    } else {
      *errorText = QStringLiteral("%1 (MD5 retry: %2)")
                       .arg(localError, hashedError);
    }
  }
  return false;
}

bool SmartLifeManager::ensureAuthenticated(const Config &config,
                                           QString *errorText) {
  const QString currentFingerprint = configFingerprint(config);
  if (!m_authFingerprint.isEmpty() && m_authFingerprint != currentFingerprint) {
    qWarning() << "[SMARTLIFE] Configuration changed, clearing old session tokens";
    clearSessionTokens();
    clearCache();
    setConnected(false);
  }

  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  if (config.accountMode == QLatin1String("developer")) {
    if (!m_projectToken.isEmpty() && nowMs < m_projectTokenExpiresAtMs) {
      m_authFingerprint = currentFingerprint;
      return true;
    }
    const bool ok = requestProjectToken(config, errorText);
    if (ok)
      m_authFingerprint = currentFingerprint;
    return ok;
  }

  if (m_projectToken.isEmpty() || nowMs >= m_projectTokenExpiresAtMs) {
    if (!requestProjectToken(config, errorText))
      return false;
  }
  if (!m_userToken.isEmpty() && nowMs < m_userTokenExpiresAtMs && !m_uid.isEmpty()) {
    m_authFingerprint = currentFingerprint;
    return true;
  }
  QString loginError;
  const bool ok = loginAssociatedUser(config, &loginError);
  if (ok) {
    m_authFingerprint = currentFingerprint;
    return true;
  }

  if (!m_projectToken.isEmpty() && isPermissionDeniedText(loginError)) {
    qWarning() << "[SMARTLIFE] Associated-user auth was denied, falling back to project-linked device access:"
               << loginError;
    m_userToken.clear();
    m_userTokenExpiresAtMs = 0;
    m_uid.clear();
    m_authFingerprint = currentFingerprint;
    if (errorText)
      *errorText = loginError;
    return true;
  }

  if (errorText)
    *errorText = loginError;
  return false;
}

bool SmartLifeManager::fetchRoomsForHome(const Config &config,
                                         const SmartLifeHomeInfo &home,
                                         QHash<QString, SmartLifeRoomInfo> *roomMap,
                                         QHash<QString, QString> *deviceToRoomId,
                                         QString *) {
  const QString accessToken =
      m_projectToken;
  const RequestResult roomResult =
      performSignedRequest(m_network, config, QStringLiteral("GET"),
                           QStringLiteral("/v1.0/homes/%1/rooms").arg(home.id),
                           {}, {}, accessToken, false);
  QString failure;
  if (!responseSucceeded(roomResult, &failure))
    return true;

  const QJsonArray roomsArray = responseResultArray(roomResult);
  for (const QJsonValue &value : roomsArray) {
    const QJsonObject object = value.toObject();
    SmartLifeRoomInfo room;
    room.id = object.value(QStringLiteral("room_id"))
                  .toVariant()
                  .toString()
                  .trimmed();
    if (room.id.isEmpty())
      room.id = object.value(QStringLiteral("id")).toVariant().toString().trimmed();
    room.homeId = home.id;
    room.name = object.value(QStringLiteral("name")).toString().trimmed();
    if (room.id.isEmpty())
      continue;
    if (roomMap)
      roomMap->insert(room.id, room);

    const RequestResult roomDevicesResult = performSignedRequest(
        m_network, config, QStringLiteral("GET"),
        QStringLiteral("/v1.0/homes/%1/rooms/%2/devices").arg(home.id, room.id),
        {}, {}, accessToken, false);
    if (!responseSucceeded(roomDevicesResult))
      continue;

    for (const QJsonValue &deviceValue : responseResultArray(roomDevicesResult)) {
      const QString deviceId =
          deviceValue.toObject().value(QStringLiteral("id")).toString().trimmed();
      if (!deviceId.isEmpty() && deviceToRoomId)
        deviceToRoomId->insert(deviceId, room.id);
    }
  }
  return true;
}

void SmartLifeManager::applyLightingFunctions(const QJsonArray &functions,
                                              SmartLifeDeviceInfo *device) {
  if (!device)
    return;

  QString bestBrightnessCode;
  int bestBrightnessPriority = 1000;
  QString bestRgbCode;
  QString bestRgbType;
  QString bestPresetCode;
  QString bestPresetType;
  QString bestTemperatureCode;
  int temperatureMin = 0;
  int temperatureMax = 1000;
  QString workModeCode;

  for (const QJsonValue &value : functions) {
    const QJsonObject object = value.toObject();
    const QString code = object.value(QStringLiteral("code")).toString();
    const QString type = object.value(QStringLiteral("type")).toString();
    if (code.isEmpty())
      continue;

    if (isWorkModeCode(code)) {
      workModeCode = code;
      const QJsonObject values = functionValuesObject(object);
      const QJsonArray range = values.value(QStringLiteral("range")).toArray();
      for (const QJsonValue &entry : range) {
        QString modeValue;
        if (entry.isString())
          modeValue = entry.toString().trimmed();
        else if (entry.isDouble())
          modeValue = QString::number(entry.toInt());
        else
          continue;
        const QString modeKey = modeValue.toLower();
        if (modeKey.contains(QLatin1String("white")) ||
            modeKey.contains(QLatin1String("warm"))) {
          device->workModeWhiteValue = modeValue;
        } else if (modeKey.contains(QLatin1String("colour")) ||
                   modeKey.contains(QLatin1String("color"))) {
          device->workModeColourValue = modeValue;
        }
      }
      continue;
    }

    if (isBrightnessCode(code) && isIntegerLikeFunctionType(type)) {
      const int priority = brightnessCodePriority(code);
      if (priority < bestBrightnessPriority) {
        bestBrightnessPriority = priority;
        bestBrightnessCode = code;
        const QJsonObject values = functionValuesObject(object);
        device->brightnessMin = values.value(QStringLiteral("min")).toInt(10);
        device->brightnessMax = values.value(QStringLiteral("max")).toInt(1000);
        if (device->brightnessMax <= device->brightnessMin)
          device->brightnessMax = device->brightnessMin + 990;
      }
      continue;
    }

    if (isRgbColorCode(code)) {
      bestRgbCode = code;
      bestRgbType = type;
      continue;
    }

    if (isPresetColorCode(code) && isEnumLikeFunctionType(type)) {
      bestPresetCode = code;
      bestPresetType = type;
      const QJsonObject values = functionValuesObject(object);
      const QJsonArray range = values.value(QStringLiteral("range")).toArray();
      device->presetColorLabels.clear();
      device->presetColorCommandValues.clear();
      for (const QJsonValue &entry : range) {
        QString label;
        QString commandValue;
        if (entry.isString()) {
          label = entry.toString().trimmed();
          commandValue = label;
        } else if (entry.isDouble()) {
          commandValue = QString::number(entry.toInt());
          label = commandValue;
        } else {
          continue;
        }
        if (commandValue.isEmpty())
          continue;
        device->presetColorLabels << label;
        device->presetColorCommandValues << commandValue;
      }
      continue;
    }

    if (isColorTemperatureCode(code) && isIntegerLikeFunctionType(type)) {
      bestTemperatureCode = code;
      const QJsonObject values = functionValuesObject(object);
      temperatureMin = values.value(QStringLiteral("min")).toInt(0);
      temperatureMax = values.value(QStringLiteral("max")).toInt(1000);
    }
  }

  if (!bestBrightnessCode.isEmpty()) {
    device->supportsBrightness = true;
    device->brightnessCode = bestBrightnessCode;
    device->brightness = qBound(device->brightnessMin, device->brightness,
                              device->brightnessMax);
  }

  if (!bestRgbCode.isEmpty()) {
    device->colorCapability = SmartLifeColorCapability::Rgb;
    device->colorCode = bestRgbCode;
    device->colorValueType = bestRgbType;
  } else if (!bestTemperatureCode.isEmpty()) {
    device->colorCapability = SmartLifeColorCapability::Preset;
    device->colorCode = bestTemperatureCode;
    device->colorValueType = QStringLiteral("Integer");
    applyWarmWhiteTemperatureTiles(device, temperatureMin, temperatureMax);
  } else if (!bestPresetCode.isEmpty() && !device->presetColorLabels.isEmpty()) {
    device->colorCapability = SmartLifeColorCapability::Preset;
    device->colorCode = bestPresetCode;
    device->colorValueType = bestPresetType;
  } else {
    device->colorCapability = SmartLifeColorCapability::None;
    device->colorCode.clear();
    device->colorValueType.clear();
    device->presetColorLabels.clear();
    device->presetColorCommandValues.clear();
    device->presetColorIndex = -1;
  }

  if (!workModeCode.isEmpty())
    device->workModeCode = workModeCode;
}

void ensureDefaultWarmCoolColorForControllableLights(SmartLifeDeviceInfo *device);

void SmartLifeManager::inferLightingCapabilitiesFromKnownCodes(
    SmartLifeDeviceInfo *device, const QJsonArray &status) {
  if (!device)
    return;

  mergeStatusCodesIntoFunctionCodes(device, status);

  if (device->colorCapability == SmartLifeColorCapability::None) {
    QString tempCode;
    int tempMin = 0;
    int tempMax = 1000;
    for (const QString &code : device->functionCodes) {
      if (!isColorTemperatureCode(code))
        continue;
      tempCode = code;
      const QJsonValue liveValue = statusValueForCode(status, code);
      if (liveValue.isDouble()) {
        const int live = liveValue.toInt();
        tempMin = qMin(tempMin, qMax(0, live - 200));
        tempMax = qMax(tempMax, live + 200);
      }
    }
    if (!tempCode.isEmpty()) {
      device->colorCapability = SmartLifeColorCapability::Preset;
      device->colorCode = tempCode;
      device->colorValueType = QStringLiteral("Integer");
      applyWarmWhiteTemperatureTiles(device, tempMin, tempMax);
    }
  }

  if (device->colorCapability == SmartLifeColorCapability::None &&
      device->likelyLighting) {
    QString tempCode;
    for (const QString &code : device->functionCodes) {
      if (isColorTemperatureCode(code)) {
        tempCode = code;
        break;
      }
    }
    if (!tempCode.isEmpty()) {
      device->colorCapability = SmartLifeColorCapability::Preset;
      device->colorCode = tempCode;
      device->colorValueType = QStringLiteral("Integer");
      applyWarmWhiteTemperatureTiles(device, 0, 1000);
    }
  }

  if (!device->supportsBrightness) {
    for (const QString &code : device->functionCodes) {
      if (!isBrightnessCode(code))
        continue;
      device->supportsBrightness = true;
      device->brightnessCode = code;
      device->brightnessMin = 10;
      device->brightnessMax = 1000;
      const QJsonValue liveValue = statusValueForCode(status, code);
      if (liveValue.isDouble()) {
        device->brightness = liveValue.toInt(device->brightnessMax);
        device->hasBrightness = true;
      }
      break;
    }
  }

  ensureDefaultWarmCoolColorForControllableLights(device);
  device->likelyLighting = deviceLooksLikeLighting(*device);
}

void ensureDefaultWarmCoolColorForControllableLights(SmartLifeDeviceInfo *device) {
  if (!device || !device->controllable)
    return;
  if (device->colorCapability == SmartLifeColorCapability::Rgb)
    return;
  if (device->colorCapability == SmartLifeColorCapability::Preset &&
      !device->presetColorLabels.isEmpty()) {
    return;
  }

  QString tempCode;
  for (const QString &code : device->functionCodes) {
    if (isColorTemperatureCode(code)) {
      tempCode = code;
      break;
    }
  }
  if (tempCode.isEmpty())
    return;

  device->colorCapability = SmartLifeColorCapability::Preset;
  device->colorCode = tempCode;
  device->colorValueType = QStringLiteral("Integer");
  applyWarmWhiteTemperatureTiles(device, 0, 1000);

  if (!device->supportsBrightness) {
    for (const QString &code : device->functionCodes) {
      if (!isBrightnessCode(code))
        continue;
      device->supportsBrightness = true;
      device->brightnessCode = code;
      device->brightnessMin = 10;
      device->brightnessMax = 1000;
      break;
    }
  }
}

bool SmartLifeManager::deviceExposesLightingControls(
    const SmartLifeDeviceInfo &device) const {
  return deviceHasVerifiedBrightnessControl(device) ||
         deviceHasVerifiedColorControl(device);
}

bool SmartLifeManager::deviceHasVerifiedBrightnessControl(
    const SmartLifeDeviceInfo &device) const {
  return ::deviceHasVerifiedBrightnessControl(device);
}

bool SmartLifeManager::deviceHasVerifiedColorControl(
    const SmartLifeDeviceInfo &device) const {
  return ::deviceHasVerifiedColorControl(device);
}

void SmartLifeManager::applyLightingStatus(const QJsonArray &status,
                                           SmartLifeDeviceInfo *device) {
  if (!device)
    return;

  if (device->supportsBrightness) {
    const QJsonValue brightnessValue =
        statusValueForCode(status, device->brightnessCode);
    if (!brightnessValue.isUndefined()) {
      device->brightness = brightnessValue.toInt(device->brightness);
      device->brightness =
          qBound(device->brightnessMin, device->brightness, device->brightnessMax);
      device->hasBrightness = true;
    }
  }

  if (device->colorCapability == SmartLifeColorCapability::Rgb &&
      !device->colorCode.isEmpty()) {
    const QJsonValue colorValue = statusValueForCode(status, device->colorCode);
    if (colorValue.isString()) {
      const QString encoded = colorValue.toString().trimmed();
      if (encoded.startsWith(QLatin1Char('#'))) {
        const QColor parsed(encoded);
        if (parsed.isValid()) {
          device->rgbColor = parsed;
          device->hasRgbColor = true;
        }
      } else {
        const QColor parsed = decodeTuyaColourData(encoded);
        if (parsed.isValid()) {
          device->rgbColor = parsed;
          device->hasRgbColor = true;
        }
      }
    } else if (colorValue.isObject()) {
      const QJsonObject object = colorValue.toObject();
      const int hue = object.value(QStringLiteral("h")).toInt(-1);
      const int saturation = object.value(QStringLiteral("s")).toInt(-1);
      const int value = object.value(QStringLiteral("v")).toInt(-1);
      if (hue >= 0 && saturation >= 0 && value >= 0) {
        QColor color;
        color.setHsv(hue, qRound(saturation / 1000.0 * 255.0),
                     qRound(value / 1000.0 * 255.0));
        device->rgbColor = color;
        device->hasRgbColor = true;
      }
    }
  } else if (device->colorCapability == SmartLifeColorCapability::Preset &&
             !device->colorCode.isEmpty()) {
    const QJsonValue colorValue = statusValueForCode(status, device->colorCode);
    QString currentValue = colorValue.toString().trimmed();
    if (currentValue.isEmpty() && colorValue.isDouble())
      currentValue = QString::number(colorValue.toInt());
    if (!currentValue.isEmpty()) {
      int index = device->presetColorCommandValues.indexOf(currentValue);
      if (index < 0 && colorValue.isDouble()) {
        const int currentInt = colorValue.toInt();
        int bestDistance = INT_MAX;
        for (int i = 0; i < device->presetColorCommandValues.size(); ++i) {
          const int candidate = device->presetColorCommandValues.at(i).toInt();
          const int distance = qAbs(candidate - currentInt);
          if (distance < bestDistance) {
            bestDistance = distance;
            index = i;
          }
        }
      }
      device->presetColorIndex = index;
    }
  }
}

bool SmartLifeManager::enrichDeviceSpecification(const Config &config,
                                                 SmartLifeDeviceInfo *device,
                                                 QString *) {
  if (!device)
    return false;
  const QString accessToken = m_projectToken;
  RequestResult functionsResult = performSignedRequest(
      m_network, config, QStringLiteral("GET"),
      QStringLiteral("/v1.0/devices/%1/functions").arg(device->id), {}, {},
      accessToken, false);
  QString failure;
  if (!responseSucceeded(functionsResult, &failure)) {
    functionsResult = performSignedRequest(
        m_network, config, QStringLiteral("GET"),
        QStringLiteral("/v1.0/iot-03/devices/%1/functions").arg(device->id),
        {}, {}, accessToken, false);
  }

  RequestResult statusResult = performSignedRequest(
      m_network, config, QStringLiteral("GET"),
      QStringLiteral("/v1.0/devices/%1/status").arg(device->id), {}, {},
      accessToken, false);
  if (!responseSucceeded(statusResult, &failure)) {
    statusResult = performSignedRequest(
        m_network, config, QStringLiteral("GET"),
        QStringLiteral("/v1.0/iot-03/devices/%1/status").arg(device->id), {},
        {}, accessToken, false);
  }

  const QJsonArray status =
      responseSucceeded(statusResult, &failure)
          ? responseResultArrayForKey(statusResult, QStringLiteral("status"))
          : QJsonArray{};

  if (!responseSucceeded(functionsResult, &failure) &&
      !responseSucceeded(statusResult, &failure)) {
    device->primaryPowerCode = choosePrimaryPowerCode(device->powerCodes);
    device->controllable = !device->primaryPowerCode.isEmpty();
    mergeStatusCodesIntoFunctionCodes(device, status);
    inferLightingCapabilitiesFromKnownCodes(device, status);
    return true;
  }

  QJsonArray functions =
      responseResultArrayForKey(functionsResult, QStringLiteral("functions"));
  if (functions.isEmpty() && !status.isEmpty())
    functions = buildSyntheticFunctionsFromStatus(status);
  else if (!status.isEmpty())
    functions = mergeFunctionsWithStatus(functions, status);

  device->functionCodes.clear();
  for (const QJsonValue &value : functions) {
    const QString code = value.toObject().value(QStringLiteral("code")).toString();
    if (!code.isEmpty() && !device->functionCodes.contains(code))
      device->functionCodes << code;
  }
  mergeStatusCodesIntoFunctionCodes(device, status);

  QStringList mergedPowerCodes = device->powerCodes;
  for (const QString &code : extractPowerCodes(functions)) {
    if (!mergedPowerCodes.contains(code))
      mergedPowerCodes << code;
  }
  for (const QString &code : extractPowerCodes(status)) {
    if (!mergedPowerCodes.contains(code))
      mergedPowerCodes << code;
  }
  device->powerCodes = mergedPowerCodes;
  device->primaryPowerCode = choosePrimaryPowerCode(device->powerCodes);
  device->controllable = !device->primaryPowerCode.isEmpty();
  device->likelyLighting =
      device->controllable &&
      (device->powerCodes.contains(QStringLiteral("switch_led"),
                                   Qt::CaseInsensitive) ||
       categoryLooksLikeLighting(device->category) ||
       nameLooksLikeLighting(device->name, device->productName));

  bool foundPowerState = false;
  const bool currentPowerState =
      readPowerState(status, device->powerCodes, &foundPowerState);
  if (foundPowerState)
    device->powerOn = currentPowerState;

  applyLightingFunctions(functions, device);
  supplementLightingFromFunctionCodes(device, status);
  applyLightingStatus(status, device);
  inferLightingCapabilitiesFromKnownCodes(device, status);
  supplementLightingFromFunctionCodes(device, status);
  return true;
}

bool SmartLifeManager::fetchAssociatedDevices(const Config &config,
                                              QVector<SmartLifeDeviceInfo> *devices,
                                              QString *errorText) {
  if (!devices) {
    if (errorText)
      *errorText = QStringLiteral("No Smart Life device container was provided");
    return false;
  }

  QString lastRowKey;
  bool hasMore = true;
  int pageGuard = 0;

  while (hasMore && pageGuard < 50) {
    ++pageGuard;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("size"), QStringLiteral("100"));
    if (!lastRowKey.isEmpty())
      query.addQueryItem(QStringLiteral("last_row_key"), lastRowKey);

    const RequestResult result =
        performSignedRequest(m_network, config, QStringLiteral("GET"),
                             QStringLiteral("/v1.0/iot-01/associated-users/devices"),
                             query, {}, m_projectToken, false);
    QString failure;
    if (!responseSucceeded(result, &failure)) {
      if (errorText)
        *errorText = failure;
      return false;
    }

    const QJsonObject resultObject = responseResultObject(result);
    const QJsonArray deviceArray =
        resultObject.value(QStringLiteral("devices")).toArray();
    for (const QJsonValue &value : deviceArray) {
      const QJsonObject object = value.toObject();
      SmartLifeDeviceInfo device;
      device.id = object.value(QStringLiteral("id")).toString().trimmed();
      if (device.id.isEmpty())
        continue;
      device.name = object.value(QStringLiteral("name")).toString().trimmed();
      device.category = object.value(QStringLiteral("category")).toString().trimmed();
      device.productName =
          object.value(QStringLiteral("product_name")).toString().trimmed();
      device.homeId = object.value(QStringLiteral("owner_id")).toString().trimmed();
      device.homeName =
          device.homeId.isEmpty()
              ? QStringLiteral("Associated Devices")
              : QStringLiteral("Home %1").arg(device.homeId);
      device.online = object.value(QStringLiteral("online")).toBool();

      const QJsonArray statusItems = object.value(QStringLiteral("status")).toArray();
      device.powerCodes = extractPowerCodes(statusItems);
      device.primaryPowerCode = choosePrimaryPowerCode(device.powerCodes);
      device.controllable = !device.primaryPowerCode.isEmpty();
      device.likelyLighting =
          device.controllable &&
          (device.powerCodes.contains(QStringLiteral("switch_led"),
                                      Qt::CaseInsensitive) ||
           categoryLooksLikeLighting(device.category) ||
           nameLooksLikeLighting(device.name, device.productName));

      bool foundPowerState = false;
      device.powerOn =
          readPowerState(statusItems, device.powerCodes, &foundPowerState);
      enrichDeviceSpecification(config, &device, nullptr);
      devices->append(device);
    }

    hasMore = resultObject.value(QStringLiteral("has_more")).toBool(false);
    lastRowKey = resultObject.value(QStringLiteral("last_row_key")).toString().trimmed();
  }

  return true;
}

bool SmartLifeManager::fetchProjectDevices(const Config &config,
                                          QVector<SmartLifeDeviceInfo> *devices,
                                          QString *errorText) {
  if (!devices) {
    if (errorText)
      *errorText = QStringLiteral("No Smart Life project device container was provided");
    return false;
  }

  QString lastRowKey;
  bool hasMore = true;
  int pageGuard = 0;

  while (hasMore && pageGuard < 50) {
    ++pageGuard;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("source_type"), QStringLiteral("homeApp"));
    query.addQueryItem(QStringLiteral("source_id"),
                       config.appSchema.isEmpty() ? QStringLiteral("smartlife")
                                                  : config.appSchema);
    query.addQueryItem(QStringLiteral("page_size"), QStringLiteral("100"));
    if (!lastRowKey.isEmpty())
      query.addQueryItem(QStringLiteral("last_row_key"), lastRowKey);

    const RequestResult result = performSignedRequest(
        m_network, config, QStringLiteral("GET"),
        QStringLiteral("/v1.3/iot-03/devices"), query, {}, m_projectToken,
        false);
    QString failure;
    if (!responseSucceeded(result, &failure)) {
      if (errorText)
        *errorText = failure;
      return false;
    }

    const QJsonObject resultObject = responseResultObject(result);
    const QJsonArray deviceArray = resultObject.value(QStringLiteral("list")).toArray();
    for (const QJsonValue &value : deviceArray) {
      const QJsonObject object = value.toObject();
      SmartLifeDeviceInfo device;
      device.id = object.value(QStringLiteral("id")).toString().trimmed();
      if (device.id.isEmpty())
        continue;

      device.name = object.value(QStringLiteral("name")).toString().trimmed();
      device.productName =
          object.value(QStringLiteral("product_name")).toString().trimmed();
      if (device.name.isEmpty())
        device.name = device.productName;
      device.category = object.value(QStringLiteral("category")).toString().trimmed();
      device.homeId = object.value(QStringLiteral("owner_id")).toString().trimmed();
      device.homeName =
          device.homeId.isEmpty()
              ? QStringLiteral("Project-linked devices")
              : QStringLiteral("Linked Home %1").arg(device.homeId);
      device.online = object.value(QStringLiteral("online")).toBool();
      device.likelyLighting =
          categoryLooksLikeLighting(device.category) ||
          nameLooksLikeLighting(device.name, device.productName);
      enrichDeviceSpecification(config, &device, nullptr);
      devices->append(device);
    }

    hasMore = resultObject.value(QStringLiteral("has_more")).toBool(false);
    lastRowKey = resultObject.value(QStringLiteral("last_row_key")).toString().trimmed();
  }

  if (devices->isEmpty()) {
    if (errorText) {
      *errorText = QStringLiteral(
          "No project-linked devices were returned for app type \"%1\". Check the selected app type or relink the app account in Tuya Developer Platform.")
                       .arg(config.appSchema.isEmpty() ? QStringLiteral("smartlife")
                                                       : config.appSchema);
    }
    return false;
  }
  return true;
}

bool SmartLifeManager::fetchDevicesForHome(
    const Config &config, const SmartLifeHomeInfo &home,
    const QHash<QString, SmartLifeRoomInfo> &roomMap,
    const QHash<QString, QString> &deviceToRoomId,
    QVector<SmartLifeDeviceInfo> *devices, QString *errorText) {
  if (!devices)
    return false;
  const QString accessToken =
      m_projectToken;
  const RequestResult deviceResult =
      performSignedRequest(m_network, config, QStringLiteral("GET"),
                           QStringLiteral("/v1.0/homes/%1/devices").arg(home.id),
                           {}, {}, accessToken, false);
  QString failure;
  if (!responseSucceeded(deviceResult, &failure)) {
    if (errorText)
      *errorText = failure;
    return false;
  }

  const QJsonArray deviceArray = responseResultArray(deviceResult);
  for (const QJsonValue &value : deviceArray) {
    const QJsonObject object = value.toObject();
    SmartLifeDeviceInfo device;
    device.id = object.value(QStringLiteral("id")).toString().trimmed();
    if (device.id.isEmpty())
      continue;
    device.name = object.value(QStringLiteral("name")).toString().trimmed();
    device.category = object.value(QStringLiteral("category")).toString().trimmed();
    device.productName =
        object.value(QStringLiteral("product_name")).toString().trimmed();
    device.homeId = home.id;
    device.homeName = home.name;
    device.online = object.value(QStringLiteral("online")).toBool();

    const QString roomId = deviceToRoomId.value(device.id);
    device.roomId = roomId;
    if (!roomId.isEmpty())
      device.roomName = roomMap.value(roomId).name;

    const QJsonArray statusItems = object.value(QStringLiteral("status")).toArray();
    device.powerCodes = extractPowerCodes(statusItems);
    device.primaryPowerCode = choosePrimaryPowerCode(device.powerCodes);
    device.controllable = !device.primaryPowerCode.isEmpty();
    device.likelyLighting =
        device.controllable &&
        (device.powerCodes.contains(QStringLiteral("switch_led"),
                                    Qt::CaseInsensitive) ||
         categoryLooksLikeLighting(device.category) ||
         nameLooksLikeLighting(device.name, device.productName));

    bool foundPowerState = false;
    device.powerOn =
        readPowerState(statusItems, device.powerCodes, &foundPowerState);
    enrichDeviceSpecification(config, &device, nullptr);
    devices->append(device);
  }
  return true;
}

bool SmartLifeManager::refreshDeviceCache(const Config &config,
                                          QString *errorText) {
  QVector<SmartLifeHomeInfo> refreshedHomes;
  QVector<SmartLifeRoomInfo> refreshedRooms;
  QVector<SmartLifeDeviceInfo> refreshedDevices;

  const QString accessToken =
      m_projectToken;
  if (accessToken.isEmpty()) {
    if (errorText)
      *errorText = QStringLiteral("Tuya session is not authenticated");
    return false;
  }

  if (config.accountMode == QLatin1String("developer")) {
    if (!config.developerUid.isEmpty()) {
      const RequestResult homeResult = performSignedRequest(
          m_network, config, QStringLiteral("GET"),
          QStringLiteral("/v1.0/users/%1/homes").arg(config.developerUid), {}, {},
          accessToken, false);
      QString failure;
      if (!responseSucceeded(homeResult, &failure)) {
        if (errorText)
          *errorText = failure;
        return false;
      }
      for (const QJsonValue &value : responseResultArray(homeResult)) {
        const QJsonObject object = value.toObject();
        SmartLifeHomeInfo home;
        home.id = object.value(QStringLiteral("home_id"))
                      .toVariant()
                      .toString()
                      .trimmed();
        home.name = object.value(QStringLiteral("name")).toString().trimmed();
        home.geoName =
            object.value(QStringLiteral("geo_name")).toString().trimmed();
        if (!home.id.isEmpty())
          refreshedHomes << home;
      }
    }

    const QStringList configuredHomeIds =
        normalizeConfiguredHomeIds(config.developerHomeIds);
    for (const QString &homeId : configuredHomeIds) {
      bool alreadyPresent = false;
      for (const SmartLifeHomeInfo &home : refreshedHomes) {
        if (home.id == homeId) {
          alreadyPresent = true;
          break;
        }
      }
      if (!alreadyPresent) {
        SmartLifeHomeInfo home;
        home.id = homeId;
        home.name = QStringLiteral("Home %1").arg(homeId);
        refreshedHomes << home;
      }
    }

    if (refreshedHomes.isEmpty()) {
      if (errorText) {
        *errorText = QStringLiteral(
            "Developer mode needs a linked app user UID or one or more home IDs");
      }
      return false;
    }
  } else {
    if (!m_uid.isEmpty()) {
      const RequestResult homeResult =
          performSignedRequest(m_network, config, QStringLiteral("GET"),
                               QStringLiteral("/v1.0/users/%1/homes").arg(m_uid),
                               {}, {}, accessToken, false);
      QString failure;
      if (responseSucceeded(homeResult, &failure)) {
        for (const QJsonValue &value : responseResultArray(homeResult)) {
          const QJsonObject object = value.toObject();
          SmartLifeHomeInfo home;
          home.id = object.value(QStringLiteral("home_id"))
                        .toVariant()
                        .toString()
                        .trimmed();
          home.name = object.value(QStringLiteral("name")).toString().trimmed();
          home.geoName =
              object.value(QStringLiteral("geo_name")).toString().trimmed();
          if (!home.id.isEmpty())
            refreshedHomes << home;
        }
      } else {
        qWarning() << "[SMARTLIFE] Home lookup failed, falling back to associated devices:"
                   << failure;
      }
    }

    if (refreshedHomes.isEmpty() && !m_uid.isEmpty()) {
      if (!fetchAssociatedDevices(config, &refreshedDevices, errorText))
        return false;
    }

    if (refreshedHomes.isEmpty() && refreshedDevices.isEmpty()) {
      QString projectFallbackError;
      if (!fetchProjectDevices(config, &refreshedDevices, &projectFallbackError)) {
        if (errorText) {
          *errorText = projectFallbackError.isEmpty()
                           ? QStringLiteral("No Smart Life homes or linked project devices were returned")
                           : projectFallbackError;
        }
        return false;
      }
    }

    if (!refreshedDevices.isEmpty()) {
      QHash<QString, bool> seenHomes;
      for (const SmartLifeDeviceInfo &device : std::as_const(refreshedDevices)) {
        const QString homeId =
            device.homeId.isEmpty() ? QStringLiteral("linked_project") : device.homeId;
        if (seenHomes.contains(homeId))
          continue;
        seenHomes.insert(homeId, true);
        SmartLifeHomeInfo home;
        home.id = homeId;
        home.name = device.homeName.isEmpty() ? QStringLiteral("Project-linked devices")
                                              : device.homeName;
        refreshedHomes << home;
      }
    }
  }

  if (refreshedDevices.isEmpty()) {
    for (const SmartLifeHomeInfo &home : refreshedHomes) {
      setStatus(QStringLiteral("Smart Life: syncing %1").arg(
          home.name.isEmpty() ? home.id : home.name));
      QHash<QString, SmartLifeRoomInfo> roomMap;
      QHash<QString, QString> deviceToRoomId;
      fetchRoomsForHome(config, home, &roomMap, &deviceToRoomId, nullptr);
      for (auto roomIt = roomMap.cbegin(); roomIt != roomMap.cend(); ++roomIt)
        refreshedRooms << roomIt.value();

      if (!fetchDevicesForHome(config, home, roomMap, deviceToRoomId,
                               &refreshedDevices, errorText)) {
        return false;
      }
    }
  }

  m_homes = refreshedHomes;
  m_rooms = refreshedRooms;
  m_devices = refreshedDevices;
  std::sort(m_devices.begin(), m_devices.end(),
            [](const SmartLifeDeviceInfo &left, const SmartLifeDeviceInfo &right) {
              return left.name.toLower() < right.name.toLower();
            });
  setConnected(true);
  if (config.accountMode == QLatin1String("smartlife") && m_uid.isEmpty()) {
    setStatus(QStringLiteral("Smart Life ready via project link: %1 devices in %2 homes")
                  .arg(m_devices.size())
                  .arg(m_homes.size()));
  } else {
    setStatus(QStringLiteral("Smart Life ready: %1 devices in %2 homes")
                  .arg(m_devices.size())
                  .arg(m_homes.size()));
  }
  emit devicesChanged();
  return true;
}

bool SmartLifeManager::sendDeviceCommands(const Config &config,
                                          SmartLifeDeviceInfo *device,
                                          const QJsonArray &commands,
                                          QString *errorText) {
  if (!device) {
    if (errorText)
      *errorText = QStringLiteral("Invalid Smart Life device");
    return false;
  }
  if (!device->online) {
    if (errorText)
      *errorText = QStringLiteral("%1 is offline").arg(device->name);
    return false;
  }
  if (commands.isEmpty()) {
    if (errorText)
      *errorText = QStringLiteral("No Smart Life commands were provided");
    return false;
  }

  QStringList accessTokensToTry;
  if (!m_projectToken.trimmed().isEmpty())
    accessTokensToTry << m_projectToken.trimmed();
  if (!m_userToken.trimmed().isEmpty() &&
      !accessTokensToTry.contains(m_userToken.trimmed())) {
    accessTokensToTry << m_userToken.trimmed();
  }

  QStringList failures;
  for (const QString &accessToken : accessTokensToTry) {
    QString failure;
    RequestResult commandResult = performSignedRequest(
        m_network, config, QStringLiteral("POST"),
        QStringLiteral("/v1.0/devices/%1/commands").arg(device->id), {},
        QJsonObject{{QStringLiteral("commands"), commands}}, accessToken, false);

    if (!responseSucceeded(commandResult, &failure)) {
      commandResult = performSignedRequest(
          m_network, config, QStringLiteral("POST"),
          QStringLiteral("/v1.0/iot-03/devices/%1/commands").arg(device->id), {},
          QJsonObject{{QStringLiteral("commands"), commands}}, accessToken, false);
      if (!responseSucceeded(commandResult, &failure)) {
        if (!failure.trimmed().isEmpty())
          failures << failure.trimmed();
        continue;
      }
    }
    return true;
  }

  if (errorText) {
    *errorText = failures.isEmpty()
                     ? QStringLiteral("Smart Life command failed for this device")
                     : failures.join(QStringLiteral("\n"));
  }
  return false;
}

bool SmartLifeManager::sendCommandsWithFallback(const Config &config,
                                                SmartLifeDeviceInfo *device,
                                                const QJsonArray &commands,
                                                QString *errorText) {
  if (!device)
    return false;

  if (sendDeviceCommands(config, device, commands, errorText))
    return true;

  const QString batchError =
      errorText && !errorText->trimmed().isEmpty()
          ? errorText->trimmed()
          : QStringLiteral("Tuya rejected the combined command");

  QJsonArray withoutWorkMode;
  for (const QJsonValue &value : commands) {
    const QJsonObject object = value.toObject();
    if (!device->workModeCode.isEmpty() &&
        object.value(QStringLiteral("code")).toString().compare(
            device->workModeCode, Qt::CaseInsensitive) == 0) {
      continue;
    }
    withoutWorkMode.append(object);
  }
  if (!withoutWorkMode.isEmpty() &&
      withoutWorkMode.size() != commands.size()) {
    if (sendDeviceCommands(config, device, withoutWorkMode, errorText))
      return true;
  }

  QStringList stepFailures;
  bool anyStepSucceeded = false;
  for (const QJsonValue &value : commands) {
    QJsonArray singleCommand;
    singleCommand.append(value);
    QString stepError;
    if (sendDeviceCommands(config, device, singleCommand, &stepError)) {
      anyStepSucceeded = true;
      continue;
    }
    const QString code =
        value.toObject().value(QStringLiteral("code")).toString();
    stepFailures << QStringLiteral("%1: %2")
                        .arg(code.isEmpty() ? QStringLiteral("command") : code,
                             stepError.trimmed().isEmpty()
                                 ? QStringLiteral("failed")
                                 : stepError.trimmed());
  }

  if (anyStepSucceeded)
    return true;

  if (errorText) {
    *errorText =
        QStringLiteral("%1\nTried step-by-step: %2")
            .arg(batchError, stepFailures.join(QStringLiteral("; ")));
  }
  return false;
}

bool SmartLifeManager::sendPowerCommand(const Config &config,
                                        SmartLifeDeviceInfo *device, bool turnOn,
                                        QString *errorText) {
  if (!device) {
    if (errorText)
      *errorText = QStringLiteral("Invalid Smart Life device");
    return false;
  }

  const QStringList powerCodes = powerCodesForIntent(*device);
  if (powerCodes.isEmpty()) {
    if (errorText)
      *errorText = QStringLiteral("%1 does not expose a power command").arg(
          device->name);
    return false;
  }

  QJsonArray commands;
  for (const QString &code : powerCodes) {
    commands.append(QJsonObject{{QStringLiteral("code"), code},
                                {QStringLiteral("value"), turnOn}});
  }

  if (!sendDeviceCommands(config, device, commands, errorText))
    return false;
  device->powerOn = turnOn;
  emit deviceStateChanged(device->id);
  return true;
}

void SmartLifeManager::setDeviceBrightness(const QString &deviceId, int brightness) {
  const Config config = loadConfig();
  QString errorText;
  if (!ensureAuthenticated(config, &errorText)) {
    setConnected(false);
    setStatus(errorText);
    emit controlFailed(errorText);
    return;
  }

  auto it = std::find_if(m_devices.begin(), m_devices.end(),
                         [&](const SmartLifeDeviceInfo &device) {
                           return device.id == deviceId;
                         });
  if (it == m_devices.end() || !it->supportsBrightness) {
    emit controlFailed(QStringLiteral("Brightness is not available for this device"));
    return;
  }

  const int clamped =
      qBound(it->brightnessMin, brightness, it->brightnessMax);
  QJsonArray commands;
  if (!it->workModeCode.isEmpty()) {
    commands.append(QJsonObject{
        {QStringLiteral("code"), it->workModeCode},
        {QStringLiteral("value"),
         jsonCommandValue(QStringLiteral("Enum"), workModeValueForWhite(*it))}});
  }
  commands.append(QJsonObject{{QStringLiteral("code"), it->brightnessCode},
                              {QStringLiteral("value"), clamped}});
  if (!it->powerOn && !it->primaryPowerCode.isEmpty()) {
    commands.prepend(QJsonObject{{QStringLiteral("code"), it->primaryPowerCode},
                                 {QStringLiteral("value"), true}});
  }

  if (!sendCommandsWithFallback(config, &(*it), commands, &errorText)) {
    emit controlFailed(errorText);
    return;
  }

  it->brightness = clamped;
  it->hasBrightness = true;
  it->powerOn = true;
  emit deviceStateChanged(deviceId);
  emit controlFinished(QStringLiteral("Brightness updated for %1")
                           .arg(deviceDisplayName(*it)));
}

void SmartLifeManager::setDevicePresetColor(const QString &deviceId, int presetIndex) {
  const Config config = loadConfig();
  QString errorText;
  if (!ensureAuthenticated(config, &errorText)) {
    setConnected(false);
    setStatus(errorText);
    emit controlFailed(errorText);
    return;
  }

  auto it = std::find_if(m_devices.begin(), m_devices.end(),
                         [&](const SmartLifeDeviceInfo &device) {
                           return device.id == deviceId;
                         });
  if (it == m_devices.end() ||
      it->colorCapability != SmartLifeColorCapability::Preset ||
      presetIndex < 0 || presetIndex >= it->presetColorCommandValues.size()) {
    emit controlFailed(QStringLiteral("Preset color is not available for this device"));
    return;
  }

  const QString commandValue = it->presetColorCommandValues.at(presetIndex);
  QJsonArray commands;
  if (!it->workModeCode.isEmpty()) {
    const bool temperaturePreset =
        isColorTemperatureCode(it->colorCode) ||
        it->colorValueType.compare(QStringLiteral("Integer"), Qt::CaseInsensitive) == 0;
    const QString modeValue = temperaturePreset ? workModeValueForWhite(*it)
                                                : workModeValueForColour(*it);
    commands.append(QJsonObject{
        {QStringLiteral("code"), it->workModeCode},
        {QStringLiteral("value"), jsonCommandValue(QStringLiteral("Enum"), modeValue)}});
  }
  commands.append(QJsonObject{{QStringLiteral("code"), it->colorCode},
                              {QStringLiteral("value"),
                               presetCommandValue(*it, commandValue)}});
  if (!it->powerOn && !it->primaryPowerCode.isEmpty()) {
    commands.prepend(QJsonObject{{QStringLiteral("code"), it->primaryPowerCode},
                                 {QStringLiteral("value"), true}});
  }

  if (!sendCommandsWithFallback(config, &(*it), commands, &errorText)) {
    emit controlFailed(QStringLiteral("Colour command failed for %1 (%2=%3): %4")
                           .arg(deviceDisplayName(*it), it->colorCode, commandValue,
                                errorText));
    return;
  }

  it->presetColorIndex = presetIndex;
  it->powerOn = true;
  emit deviceStateChanged(deviceId);
  emit controlFinished(QStringLiteral("Color updated for %1")
                           .arg(deviceDisplayName(*it)));
}

void SmartLifeManager::setDeviceRgbColor(const QString &deviceId, const QColor &color) {
  const Config config = loadConfig();
  QString errorText;
  if (!ensureAuthenticated(config, &errorText)) {
    setConnected(false);
    setStatus(errorText);
    emit controlFailed(errorText);
    return;
  }

  auto it = std::find_if(m_devices.begin(), m_devices.end(),
                         [&](const SmartLifeDeviceInfo &device) {
                           return device.id == deviceId;
                         });
  if (it == m_devices.end() ||
      it->colorCapability != SmartLifeColorCapability::Rgb ||
      it->colorCode.isEmpty()) {
    emit controlFailed(QStringLiteral("RGB color is not available for this device"));
    return;
  }

  QJsonValue payload;
  const QString type = it->colorValueType.trimmed();
  if (type.compare(QStringLiteral("String"), Qt::CaseInsensitive) == 0 &&
      it->colorCode.contains(QStringLiteral("hex"), Qt::CaseInsensitive)) {
    payload = QStringLiteral("#%1%2%3")
                    .arg(color.red(), 2, 16, QLatin1Char('0'))
                    .arg(color.green(), 2, 16, QLatin1Char('0'))
                    .arg(color.blue(), 2, 16, QLatin1Char('0'))
                    .toUpper();
  } else {
    payload = encodeTuyaColourData(color);
  }

  QJsonArray commands;
  if (!it->workModeCode.isEmpty()) {
    commands.append(QJsonObject{
        {QStringLiteral("code"), it->workModeCode},
        {QStringLiteral("value"),
         jsonCommandValue(QStringLiteral("Enum"), workModeValueForColour(*it))}});
  }
  commands.append(
      QJsonObject{{QStringLiteral("code"), it->colorCode}, {QStringLiteral("value"), payload}});
  if (!it->powerOn && !it->primaryPowerCode.isEmpty()) {
    commands.prepend(QJsonObject{{QStringLiteral("code"), it->primaryPowerCode},
                                 {QStringLiteral("value"), true}});
  }

  if (!sendCommandsWithFallback(config, &(*it), commands, &errorText)) {
    emit controlFailed(errorText);
    return;
  }

  it->rgbColor = color;
  it->hasRgbColor = true;
  it->powerOn = true;
  emit deviceStateChanged(deviceId);
  emit controlFinished(QStringLiteral("Color updated for %1")
                           .arg(deviceDisplayName(*it)));
}

void SmartLifeManager::connectAndSync() {
  const Config config = loadConfig();
  QString errorText;
  setStatus(QStringLiteral("Smart Life: connecting..."));
  if (!ensureAuthenticated(config, &errorText)) {
    setConnected(false);
    setStatus(errorText);
    emit controlFailed(errorText);
    return;
  }
  if (!refreshDeviceCache(config, &errorText)) {
    setConnected(false);
    setStatus(errorText);
    emit controlFailed(errorText);
    return;
  }
}

void SmartLifeManager::syncDevices() {
  const Config config = loadConfig();
  QString errorText;
  setStatus(QStringLiteral("Smart Life: refreshing device cache..."));
  if (!ensureAuthenticated(config, &errorText)) {
    setConnected(false);
    setStatus(errorText);
    emit controlFailed(errorText);
    return;
  }
  if (!refreshDeviceCache(config, &errorText)) {
    setConnected(false);
    setStatus(errorText);
    emit controlFailed(errorText);
  }
}

void SmartLifeManager::disconnectSession(bool clearSavedTokens) {
  if (clearSavedTokens) {
    QSettings settings("QuickSTT", "Config");
    const QStringList keys = {
        QStringLiteral("smartLife/accountMode"),
        QStringLiteral("smartLife/endpointKey"),
        QStringLiteral("smartLife/accessId"),
        QStringLiteral("smartLife/developerUid"),
        QStringLiteral("smartLife/developerHomeIds"),
        QStringLiteral("smartLife/username"),
        QStringLiteral("smartLife/countryCode"),
        QStringLiteral("smartLife/appSchema"),
        QStringLiteral("smartLife/passwordAlreadyMd5"),
    };
    for (const QString &key : keys)
      settings.remove(key);
    saveProtectedSetting(settings, QStringLiteral("smartLife/accessKey"), QString());
    saveProtectedSetting(settings, QStringLiteral("smartLife/password"), QString());
  }

  clearSessionTokens();
  clearCache();
  setConnected(false);
  setStatus(clearSavedTokens ? QStringLiteral("Smart Life disconnected and credentials cleared")
                             : QStringLiteral("Smart Life disconnected"));
}

void SmartLifeManager::controlDevices(const QStringList &deviceIds, bool turnOn) {
  QStringList uniqueIds = deviceIds;
  uniqueIds.removeAll(QString());
  uniqueIds.removeDuplicates();
  if (uniqueIds.isEmpty()) {
    const QString message =
        QStringLiteral("No Smart Life devices were selected");
    setStatus(message);
    emit controlFailed(message);
    return;
  }

  const Config config = loadConfig();
  QString errorText;
  if (!ensureAuthenticated(config, &errorText)) {
    setConnected(false);
    setStatus(errorText);
    emit controlFailed(errorText);
    return;
  }
  if (m_devices.isEmpty() && !refreshDeviceCache(config, &errorText)) {
    setConnected(false);
    setStatus(errorText);
    emit controlFailed(errorText);
    return;
  }

  int successCount = 0;
  QStringList failures;
  for (const QString &deviceId : uniqueIds) {
    auto it = std::find_if(m_devices.begin(), m_devices.end(),
                           [&](const SmartLifeDeviceInfo &device) {
                             return device.id == deviceId;
                           });
    if (it == m_devices.end()) {
      failures << QStringLiteral("Missing device %1").arg(deviceId);
      continue;
    }
    QString commandError;
    if (!sendPowerCommand(config, &(*it), turnOn, &commandError)) {
      failures << QStringLiteral("%1: %2").arg(it->name, commandError);
      continue;
    }
    ++successCount;
  }

  QString message;
  if (successCount > 0) {
    message = QStringLiteral("%1 device%2 turned %3")
                  .arg(successCount)
                  .arg(successCount == 1 ? QString() : QStringLiteral("s"))
                  .arg(turnOn ? QStringLiteral("on") : QStringLiteral("off"));
    if (!failures.isEmpty())
      message += QStringLiteral(" (%1 issue%2)")
                     .arg(failures.size())
                     .arg(failures.size() == 1 ? QString() : QStringLiteral("s"));
    setStatus(message);
    emit controlFinished(message);
    if (!failures.isEmpty())
      emit controlFailed(failures.join(QStringLiteral("\n")));
    return;
  }

  message = failures.isEmpty() ? QStringLiteral("No Smart Life devices changed state")
                               : failures.join(QStringLiteral("\n"));
  setStatus(message);
  emit controlFailed(message);
}

SmartLifeManager::VoiceMatchResult
SmartLifeManager::matchVoiceCommand(const QString &spokenText) const {
  VoiceMatchResult result;
  const QString normalized = normalizedMatchText(spokenText);
  if (normalized.isEmpty())
    return result;

  QString targetText;
  QRegularExpressionMatch match;
  const QRegularExpression leadingActionPattern(
      QStringLiteral("^(?:turn|switch|power)\\s+(on|off)\\s+(.+)$"));
  const QRegularExpression trailingActionPattern(
      QStringLiteral("^(?:turn|switch|power)\\s+(.+?)\\s+(on|off)$"));
  const QRegularExpression shortActionPattern(
      QStringLiteral("^(.+?)\\s+(on|off)$"));

  if ((match = leadingActionPattern.match(normalized)).hasMatch()) {
    result.recognizedIntent = true;
    result.actionLabel = match.captured(1);
    targetText = match.captured(2);
  } else if ((match = trailingActionPattern.match(normalized)).hasMatch()) {
    result.recognizedIntent = true;
    result.actionLabel = match.captured(2);
    targetText = match.captured(1);
  } else if ((match = shortActionPattern.match(normalized)).hasMatch()) {
    const QString possibleTarget = match.captured(1).trimmed();
    if (possibleTarget.contains(QStringLiteral("light")) ||
        possibleTarget.contains(QStringLiteral("lamp")) ||
        possibleTarget == QLatin1String("all")) {
      result.recognizedIntent = true;
      result.actionLabel = match.captured(2);
      targetText = possibleTarget;
    } else {
      return result;
    }
  } else {
    return result;
  }

  const bool mentionsLights =
      targetText.contains(QStringLiteral(" light")) ||
      targetText.contains(QStringLiteral(" lights")) ||
      targetText.endsWith(QStringLiteral(" light")) ||
      targetText.endsWith(QStringLiteral(" lights")) ||
      targetText.contains(QStringLiteral(" lamp")) ||
      targetText.contains(QStringLiteral(" lamps"));
  const bool mentionsAll = targetText == QLatin1String("all") ||
                           targetText.startsWith(QStringLiteral("all "));

  QString cleanedTarget = targetText;
  const QStringList fillerWords = {QStringLiteral("all"),    QStringLiteral("the"),
                                   QStringLiteral("lights"), QStringLiteral("light"),
                                   QStringLiteral("lamps"),  QStringLiteral("lamp"),
                                   QStringLiteral("please")};
  for (const QString &word : fillerWords) {
    cleanedTarget.replace(QRegularExpression(QStringLiteral("\\b%1\\b").arg(
                            QRegularExpression::escape(word))),
                          QStringLiteral(" "));
  }
  cleanedTarget = cleanedTarget.simplified();

  auto collectDevices = [&](auto predicate, bool lightingOnly) {
    QStringList ids;
    QStringList fallbackIds;
    for (const SmartLifeDeviceInfo &device : m_devices) {
      if (!device.controllable || !predicate(device))
        continue;
      fallbackIds << device.id;
      if (!lightingOnly || device.likelyLighting)
        ids << device.id;
    }
    if (ids.isEmpty() && lightingOnly)
      return fallbackIds;
    return ids;
  };

  if (cleanedTarget.isEmpty()) {
    if (mentionsLights || mentionsAll) {
      result.matched = true;
      result.targetLabel = QStringLiteral("all lights");
      result.deviceIds =
          collectDevices([](const SmartLifeDeviceInfo &) { return true; }, true);
      if (result.deviceIds.isEmpty())
        result.errorText = QStringLiteral("No controllable lights are available");
      return result;
    }
    result.errorText = QStringLiteral("No Smart Life target was detected");
    return result;
  }

  const QString targetNormalized = normalizedMatchText(cleanedTarget);
  const bool lightingOnly = mentionsLights;

  const auto exactRoomIds = collectDevices(
      [&](const SmartLifeDeviceInfo &device) {
        return normalizedMatchText(device.roomName) == targetNormalized;
      },
      lightingOnly);
  if (!exactRoomIds.isEmpty()) {
    result.matched = true;
    result.targetLabel = cleanedTarget;
    result.deviceIds = exactRoomIds;
    return result;
  }

  const auto exactHomeIds = collectDevices(
      [&](const SmartLifeDeviceInfo &device) {
        return normalizedMatchText(device.homeName) == targetNormalized;
      },
      lightingOnly);
  if (!exactHomeIds.isEmpty()) {
    result.matched = true;
    result.targetLabel = cleanedTarget;
    result.deviceIds = exactHomeIds;
    return result;
  }

  const auto exactDeviceIds = collectDevices(
      [&](const SmartLifeDeviceInfo &device) {
        const QString rawName = normalizedMatchText(device.name);
        const QString aliasName = normalizedMatchText(deviceAlias(device.id));
        return (!rawName.isEmpty() && rawName == targetNormalized) ||
               (!aliasName.isEmpty() && aliasName == targetNormalized);
      },
      false);
  if (!exactDeviceIds.isEmpty()) {
    result.matched = true;
    result.targetLabel = cleanedTarget;
    result.deviceIds = exactDeviceIds;
    return result;
  }

  const auto fuzzyRoomIds = collectDevices(
      [&](const SmartLifeDeviceInfo &device) {
        return normalizedMatchText(device.roomName).contains(targetNormalized);
      },
      lightingOnly);
  if (!fuzzyRoomIds.isEmpty()) {
    result.matched = true;
    result.targetLabel = cleanedTarget;
    result.deviceIds = fuzzyRoomIds;
    return result;
  }

  const auto fuzzyHomeIds = collectDevices(
      [&](const SmartLifeDeviceInfo &device) {
        return normalizedMatchText(device.homeName).contains(targetNormalized);
      },
      lightingOnly);
  if (!fuzzyHomeIds.isEmpty()) {
    result.matched = true;
    result.targetLabel = cleanedTarget;
    result.deviceIds = fuzzyHomeIds;
    return result;
  }

  const auto fuzzyDeviceIds = collectDevices(
      [&](const SmartLifeDeviceInfo &device) {
        const QString rawName = normalizedMatchText(device.name);
        const QString aliasName = normalizedMatchText(deviceAlias(device.id));
        return (!rawName.isEmpty() && rawName.contains(targetNormalized)) ||
               (!aliasName.isEmpty() && aliasName.contains(targetNormalized));
      },
      false);
  if (!fuzzyDeviceIds.isEmpty()) {
    result.matched = true;
    result.targetLabel = cleanedTarget;
    result.deviceIds = fuzzyDeviceIds;
    return result;
  }

  result.errorText =
      QStringLiteral("No matching Smart Life target was found for \"%1\"")
          .arg(cleanedTarget);
  return result;
}

bool SmartLifeManager::handleVoiceCommand(const QString &spokenText,
                                          QString *feedback) {
  VoiceMatchResult match = matchVoiceCommand(spokenText);
  if (!match.recognizedIntent)
    return false;

  const Config config = loadConfig();
  QString errorText;
  if (!ensureAuthenticated(config, &errorText)) {
    setConnected(false);
    setStatus(errorText);
    emit controlFailed(errorText);
    if (feedback)
      *feedback = errorText;
    return true;
  }

  if (m_devices.isEmpty()) {
    if (!refreshDeviceCache(config, &errorText)) {
      setConnected(false);
      setStatus(errorText);
      emit controlFailed(errorText);
      if (feedback)
        *feedback = errorText;
      return true;
    }
    match = matchVoiceCommand(spokenText);
  }

  if (!match.matched || match.deviceIds.isEmpty()) {
    const QString failure = match.errorText.isEmpty()
                                ? QStringLiteral("No Smart Life device matched the request")
                                : match.errorText;
    setStatus(failure);
    emit controlFailed(failure);
    if (feedback)
      *feedback = failure;
    return true;
  }

  const bool turnOn = match.actionLabel.compare(QStringLiteral("on"),
                                                Qt::CaseInsensitive) == 0;
  controlDevices(match.deviceIds, turnOn);
  if (feedback) {
    *feedback = statusText();
  }
  return true;
}
