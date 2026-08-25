#include "home_assistant_manager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>

HomeAssistantManager::HomeAssistantManager(QObject *parent) : QObject(parent) {}

void HomeAssistantManager::setConfig(const Config &config) {
  m_config = config;
  if (!m_config.baseUrl.isEmpty() && m_config.baseUrl.endsWith('/'))
    m_config.baseUrl.chop(1);
}

QNetworkRequest HomeAssistantManager::makeRequest(const QString &path) const {
  QUrl url(m_config.baseUrl + path);
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setRawHeader("Authorization",
                       QStringLiteral("Bearer %1").arg(m_config.token).toUtf8());
  request.setTransferTimeout(15000);
  return request;
}

void HomeAssistantManager::connectAndSync() {
  if (m_config.baseUrl.isEmpty() || m_config.token.isEmpty()) {
    emit errorOccurred("Home Assistant URL and token are required.");
    return;
  }

  m_syncing = true;
  emit connectionStatusChanged(false, "Syncing entities from Home Assistant...");

  QNetworkReply *reply = m_network.get(makeRequest("/api/states"));
  connect(reply, &QNetworkReply::finished, this,
          &HomeAssistantManager::onStatesReply);
}

void HomeAssistantManager::disconnect() {
  m_connected = false;
  m_syncing = false;
  m_entities.clear();
  m_nameToEntityId.clear();
  emit connectionStatusChanged(false, "Disconnected from Home Assistant.");
}

void HomeAssistantManager::onStatesReply() {
  auto *reply = qobject_cast<QNetworkReply *>(sender());
  if (!reply)
    return;
  reply->deleteLater();
  m_syncing = false;

  if (reply->error() != QNetworkReply::NoError) {
    m_connected = false;
    const QString errText = reply->errorString();
    emit connectionStatusChanged(false,
                                 QStringLiteral("HA connection failed: %1").arg(errText));
    emit errorOccurred(errText);
    return;
  }

  const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
  if (!doc.isArray()) {
    emit errorOccurred("Unexpected response from Home Assistant.");
    return;
  }

  m_entities.clear();
  m_nameToEntityId.clear();

  const QJsonArray states = doc.array();
  for (const QJsonValue &val : states) {
    const QJsonObject obj = val.toObject();
    const QString entityId = obj.value("entity_id").toString();
    const QString state = obj.value("state").toString();
    const QJsonObject attrs = obj.value("attributes").toObject();
    const QString friendlyName = attrs.value("friendly_name").toString();

    const QString domain = entityId.section('.', 0, 0);
    static const QStringList supportedDomains = {
        "light", "switch", "fan", "cover", "climate",
        "media_player", "lock", "scene", "script",
        "input_boolean", "automation"};
    if (!supportedDomains.contains(domain))
      continue;

    HaEntity entity;
    entity.entityId = entityId;
    entity.domain = domain;
    entity.friendlyName = friendlyName;
    entity.state = state;
    entity.attributes = attrs;
    m_entities.append(entity);

    if (!friendlyName.isEmpty())
      m_nameToEntityId.insert(friendlyName.toLower().trimmed(), entityId);
  }

  m_connected = true;
  emit connectionStatusChanged(
      true,
      QStringLiteral("Connected to Home Assistant. %1 controllable entities synced.")
          .arg(m_entities.size()));
  emit entitiesSynced(m_entities.size());
}

void HomeAssistantManager::callService(const QString &domain,
                                       const QString &service,
                                       const QString &entityId,
                                       const QJsonObject &data) {
  QJsonObject payload = data;
  payload["entity_id"] = entityId;

  const QString path = QStringLiteral("/api/services/%1/%2").arg(domain, service);
  QNetworkReply *reply =
      m_network.post(makeRequest(path), QJsonDocument(payload).toJson());
  connect(reply, &QNetworkReply::finished, this,
          &HomeAssistantManager::onServiceCallReply);
}

void HomeAssistantManager::onServiceCallReply() {
  auto *reply = qobject_cast<QNetworkReply *>(sender());
  if (!reply)
    return;
  reply->deleteLater();

  if (reply->error() != QNetworkReply::NoError) {
    emit errorOccurred(
        QStringLiteral("HA service call failed: %1").arg(reply->errorString()));
  }
}

// ── Rhasspy-style intent matching ────────────────────────────────────────────

bool HomeAssistantManager::handleVoiceCommand(const QString &spokenText,
                                              QString *feedback) {
  if (!m_connected || m_entities.isEmpty())
    return false;

  QString normalized = spokenText.toLower().trimmed();
  for (QChar &ch : normalized) {
    if (!ch.isLetterOrNumber())
      ch = ' ';
  }
  normalized = normalized.simplified();

  IntentMatch match;
  if (!matchIntent(normalized, &match))
    return false;

  callService(match.domain, match.service, match.entityId, match.serviceData);

  const HaEntity *entity = nullptr;
  for (const HaEntity &e : m_entities) {
    if (e.entityId == match.entityId) {
      entity = &e;
      break;
    }
  }
  const QString displayName =
      entity ? entity->friendlyName : match.entityId;
  *feedback = QStringLiteral("HA: %1 → %2").arg(displayName, match.action);
  emit commandExecuted(*feedback);
  return true;
}

bool HomeAssistantManager::matchIntent(const QString &normalized,
                                       IntentMatch *match) const {
  static const QRegularExpression reTurnOnOff(
      "^(?:turn|switch)\\s+(on|off)\\s+(?:the\\s+)?(.+)$");
  static const QRegularExpression reTurnOnOff2(
      "^(?:turn|switch)\\s+(?:the\\s+)?(.+?)\\s+(on|off)$");
  static const QRegularExpression reBrightness(
      "^set\\s+(?:the\\s+)?(.+?)\\s+(?:brightness\\s+)?to\\s+(\\d+)\\s*(?:percent|%)?$");
  static const QRegularExpression reDim(
      "^dim\\s+(?:the\\s+)?(.+)$");
  static const QRegularExpression reBrighten(
      "^brighten\\s+(?:the\\s+)?(.+)$");
  static const QRegularExpression reTemp(
      "^set\\s+(?:the\\s+)?(.+?)\\s+(?:temperature\\s+)?to\\s+(\\d+)\\s*(?:degrees?)?$");
  static const QRegularExpression reLockUnlock(
      "^(lock|unlock)\\s+(?:the\\s+)?(.+)$");
  static const QRegularExpression reActivate(
      "^(?:activate|run|trigger)\\s+(?:the\\s+)?(.+)$");
  static const QRegularExpression reToggle(
      "^toggle\\s+(?:the\\s+)?(.+)$");

  QRegularExpressionMatch m;

  m = reTurnOnOff.match(normalized);
  if (m.hasMatch()) {
    const QString onOff = m.captured(1);
    const QString entityId = findEntityByName(m.captured(2));
    if (entityId.isEmpty())
      return false;
    match->action = onOff == "on" ? "Turn On" : "Turn Off";
    match->domain = "homeassistant";
    match->service = onOff == "on" ? "turn_on" : "turn_off";
    match->entityId = entityId;
    return true;
  }

  m = reTurnOnOff2.match(normalized);
  if (m.hasMatch()) {
    const QString onOff = m.captured(2);
    const QString entityId = findEntityByName(m.captured(1));
    if (entityId.isEmpty())
      return false;
    match->action = onOff == "on" ? "Turn On" : "Turn Off";
    match->domain = "homeassistant";
    match->service = onOff == "on" ? "turn_on" : "turn_off";
    match->entityId = entityId;
    return true;
  }

  m = reBrightness.match(normalized);
  if (m.hasMatch()) {
    const QString entityId = findEntityByName(m.captured(1));
    if (entityId.isEmpty() || !entityId.startsWith("light."))
      return false;
    int pct = m.captured(2).toInt();
    pct = qBound(0, pct, 100);
    match->action = QStringLiteral("Brightness %1%").arg(pct);
    match->domain = "light";
    match->service = "turn_on";
    match->entityId = entityId;
    match->serviceData["brightness_pct"] = pct;
    return true;
  }

  m = reDim.match(normalized);
  if (m.hasMatch()) {
    const QString entityId = findEntityByName(m.captured(1));
    if (entityId.isEmpty() || !entityId.startsWith("light."))
      return false;
    match->action = "Dim";
    match->domain = "light";
    match->service = "turn_on";
    match->entityId = entityId;
    match->serviceData["brightness_step_pct"] = -20;
    return true;
  }

  m = reBrighten.match(normalized);
  if (m.hasMatch()) {
    const QString entityId = findEntityByName(m.captured(1));
    if (entityId.isEmpty() || !entityId.startsWith("light."))
      return false;
    match->action = "Brighten";
    match->domain = "light";
    match->service = "turn_on";
    match->entityId = entityId;
    match->serviceData["brightness_step_pct"] = 20;
    return true;
  }

  m = reTemp.match(normalized);
  if (m.hasMatch()) {
    const QString entityId = findEntityByName(m.captured(1));
    if (entityId.isEmpty() || !entityId.startsWith("climate."))
      return false;
    int temp = m.captured(2).toInt();
    match->action = QStringLiteral("Temperature %1°").arg(temp);
    match->domain = "climate";
    match->service = "set_temperature";
    match->entityId = entityId;
    match->serviceData["temperature"] = temp;
    return true;
  }

  m = reLockUnlock.match(normalized);
  if (m.hasMatch()) {
    const QString action = m.captured(1);
    const QString entityId = findEntityByName(m.captured(2));
    if (entityId.isEmpty() || !entityId.startsWith("lock."))
      return false;
    match->action = action == "lock" ? "Lock" : "Unlock";
    match->domain = "lock";
    match->service = action;
    match->entityId = entityId;
    return true;
  }

  m = reActivate.match(normalized);
  if (m.hasMatch()) {
    const QString entityId = findEntityByName(m.captured(1));
    if (entityId.isEmpty())
      return false;
    const QString domain = entityId.section('.', 0, 0);
    if (domain != "scene" && domain != "script" && domain != "automation")
      return false;
    match->action = "Activate";
    match->domain = domain;
    match->service = domain == "scene" ? "turn_on" : "trigger";
    match->entityId = entityId;
    return true;
  }

  m = reToggle.match(normalized);
  if (m.hasMatch()) {
    const QString entityId = findEntityByName(m.captured(1));
    if (entityId.isEmpty())
      return false;
    match->action = "Toggle";
    match->domain = "homeassistant";
    match->service = "toggle";
    match->entityId = entityId;
    return true;
  }

  return false;
}

QString HomeAssistantManager::findEntityByName(const QString &name) const {
  const QString key = name.toLower().trimmed();
  if (key.isEmpty())
    return {};

  auto it = m_nameToEntityId.constFind(key);
  if (it != m_nameToEntityId.constEnd())
    return it.value();

  return bestFuzzyEntityMatch(key);
}

QString HomeAssistantManager::bestFuzzyEntityMatch(const QString &query) const {
  int bestDist = INT_MAX;
  QString bestId;

  for (auto it = m_nameToEntityId.constBegin();
       it != m_nameToEntityId.constEnd(); ++it) {
    const int dist = levenshtein(query, it.key());
    const int maxLen = qMax(query.size(), it.key().size());
    if (maxLen == 0)
      continue;
    if (dist <= qMax(2, maxLen / 4) && dist < bestDist) {
      bestDist = dist;
      bestId = it.value();
    }
  }

  if (!bestId.isEmpty())
    return bestId;

  for (auto it = m_nameToEntityId.constBegin();
       it != m_nameToEntityId.constEnd(); ++it) {
    if (it.key().contains(query) || query.contains(it.key())) {
      return it.value();
    }
  }

  return {};
}

int HomeAssistantManager::levenshtein(const QString &a, const QString &b) {
  const int m = a.size();
  const int n = b.size();
  if (m == 0) return n;
  if (n == 0) return m;

  QVector<int> prev(n + 1), curr(n + 1);
  for (int j = 0; j <= n; ++j)
    prev[j] = j;

  for (int i = 1; i <= m; ++i) {
    curr[0] = i;
    for (int j = 1; j <= n; ++j) {
      int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      curr[j] = qMin(prev[j] + 1, qMin(curr[j - 1] + 1, prev[j - 1] + cost));
    }
    std::swap(prev, curr);
  }
  return prev[n];
}
