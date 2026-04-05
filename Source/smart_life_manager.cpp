#include "smart_life_manager.h"
#include "windows_secret_store.h"

#include <QCryptographicHash>
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
    {"singapore", "Singapore", "https://openapi.tuyasg.com"},
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
  QUrl url(baseUrl.resolved(QUrl(path)));
  if (!query.isEmpty())
    url.setQuery(query);

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
  const QString queryString = query.toString(QUrl::FullyEncoded);
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
  if (config.accountMode.compare(QStringLiteral("developer"),
                                 Qt::CaseInsensitive) != 0) {
    config.accountMode = QStringLiteral("smartlife");
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
      "- If Connect fails with permission errors, the project link, region, or API permissions are usually the bottleneck.\n\n"
      "Voice control examples:\n"
      "- turn on bedroom lights\n"
      "- turn off bedroom 2 lights\n"
      "- turn on desk lamp\n"
      "- switch off living room lights");
}

SmartLifeDeviceInfo SmartLifeManager::deviceById(const QString &deviceId) const {
  for (const SmartLifeDeviceInfo &device : m_devices) {
    if (device.id == deviceId)
      return device;
  }
  return {};
}

QString SmartLifeManager::deviceDetailText(const QString &deviceId) const {
  const SmartLifeDeviceInfo device = deviceById(deviceId);
  if (device.id.isEmpty())
    return QStringLiteral("Select a Smart Life device to view its details.");

  QStringList lines;
  lines << device.name;
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
  return lines.join(QLatin1Char('\n'));
}

void SmartLifeManager::setStatus(const QString &statusText) {
  if (m_statusText == statusText)
    return;
  m_statusText = statusText;
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

  if (!responseSucceeded(functionsResult, &failure) &&
      !responseSucceeded(statusResult, &failure)) {
    device->primaryPowerCode = choosePrimaryPowerCode(device->powerCodes);
    device->controllable = !device->primaryPowerCode.isEmpty();
    device->likelyLighting =
        device->controllable && (device->likelyLighting ||
                                 categoryLooksLikeLighting(device->category) ||
                                 nameLooksLikeLighting(device->name,
                                                       device->productName));
    return true;
  }

  const QJsonArray functions =
      responseResultArrayForKey(functionsResult, QStringLiteral("functions"));
  const QJsonArray status =
      responseResultArrayForKey(statusResult, QStringLiteral("status"));

  device->functionCodes.clear();
  for (const QJsonValue &value : functions) {
    const QString code = value.toObject().value(QStringLiteral("code")).toString();
    if (!code.isEmpty() && !device->functionCodes.contains(code))
      device->functionCodes << code;
  }

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

bool SmartLifeManager::sendPowerCommand(const Config &config,
                                        SmartLifeDeviceInfo *device, bool turnOn,
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

  const QString accessToken =
      m_projectToken;
  RequestResult commandResult = performSignedRequest(
      m_network, config, QStringLiteral("POST"),
      QStringLiteral("/v1.0/devices/%1/commands").arg(device->id), {},
      QJsonObject{{QStringLiteral("commands"), commands}}, accessToken, false);

  QString failure;
  if (!responseSucceeded(commandResult, &failure)) {
    commandResult = performSignedRequest(
        m_network, config, QStringLiteral("POST"),
        QStringLiteral("/v1.0/iot-03/devices/%1/commands").arg(device->id), {},
        QJsonObject{{QStringLiteral("commands"), commands}}, accessToken, false);
    if (!responseSucceeded(commandResult, &failure)) {
      if (errorText)
        *errorText = failure;
      return false;
    }
  }

  device->powerOn = turnOn;
  return true;
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

  emit devicesChanged();

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
  if (normalized.startsWith(QStringLiteral("turn on "))) {
    result.recognizedIntent = true;
    result.actionLabel = QStringLiteral("on");
    targetText = normalized.mid(QStringLiteral("turn on ").size());
  } else if (normalized.startsWith(QStringLiteral("switch on "))) {
    result.recognizedIntent = true;
    result.actionLabel = QStringLiteral("on");
    targetText = normalized.mid(QStringLiteral("switch on ").size());
  } else if (normalized.startsWith(QStringLiteral("turn off "))) {
    result.recognizedIntent = true;
    result.actionLabel = QStringLiteral("off");
    targetText = normalized.mid(QStringLiteral("turn off ").size());
  } else if (normalized.startsWith(QStringLiteral("switch off "))) {
    result.recognizedIntent = true;
    result.actionLabel = QStringLiteral("off");
    targetText = normalized.mid(QStringLiteral("switch off ").size());
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
    for (const SmartLifeDeviceInfo &device : m_devices) {
      if (!device.controllable || !predicate(device))
        continue;
      if (lightingOnly && !device.likelyLighting)
        continue;
      ids << device.id;
    }
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
        return normalizedMatchText(device.name) == targetNormalized;
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
        return normalizedMatchText(device.name).contains(targetNormalized);
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
    *feedback = QStringLiteral("%1 %2")
                    .arg(turnOn ? QStringLiteral("Turned on")
                                : QStringLiteral("Turned off"),
                         match.targetLabel);
  }
  return true;
}
