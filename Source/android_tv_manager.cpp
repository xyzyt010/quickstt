#include "android_tv_manager.h"

#include "optional_service_support.h"
#include "local_model_support.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>

namespace {

QString normalizeVoiceText(QString text) {
  text = text.toLower().trimmed();
  text.replace(QRegularExpression(QStringLiteral("\\btelevision\\b")),
               QStringLiteral("tv"));
  text.replace(QRegularExpression(QStringLiteral("\\btelevisions\\b")),
               QStringLiteral("tvs"));
  for (QChar &ch : text) {
    if (!ch.isLetterOrNumber())
      ch = ' ';
  }
  return text.simplified();
}

QString boolText(bool value) {
  return value ? QStringLiteral("Yes") : QStringLiteral("No");
}

bool isLikelyLanHost(const QString &hostText) {
  const QString trimmed = hostText.trimmed();
  if (trimmed.isEmpty())
    return false;

  QHostAddress addr;
  if (!addr.setAddress(trimmed))
    return true;

  if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
    const quint32 ip = addr.toIPv4Address();
    if ((ip & 0xFF000000u) == 0x0A000000u)
      return true;
    if ((ip & 0xFFF00000u) == 0xAC100000u)
      return true;
    if ((ip & 0xFFFF0000u) == 0xC0A80000u)
      return true;
    if ((ip & 0xFFFF0000u) == 0xA9FE0000u)
      return true;
    return false;
  }

  return addr.isLinkLocal() || addr.isUniqueLocalUnicast();
}

QString stateString(const QJsonObject &state, const char *key,
                    const QString &fallback = QString()) {
  const QString value = state.value(QLatin1String(key)).toString().trimmed();
  return value.isEmpty() ? fallback : value;
}

bool capabilityFlag(const QJsonObject &state, const char *key,
                    bool fallback = true) {
  const QJsonObject caps = state.value(QStringLiteral("capabilities")).toObject();
  if (caps.contains(QLatin1String(key)))
    return caps.value(QLatin1String(key)).toBool();
  return fallback;
}

QString cleanStateKey(QString key) {
  key = key.toLower().trimmed();
  QString result;
  result.reserve(key.size());
  for (const QChar ch : key)
    result += ch.isLetterOrNumber() ? ch : QChar('_');
  while (result.contains(QStringLiteral("__")))
    result.replace(QStringLiteral("__"), QStringLiteral("_"));
  return result.trimmed();
}

QString normalizedAndroidTvName(QString text) {
  text = text.toLower().trimmed();
  for (QChar &ch : text) {
    if (!ch.isLetterOrNumber())
      ch = QLatin1Char(' ');
  }
  return text.simplified();
}

bool isRemoteControlAction(const QString &action) {
  return action == QLatin1String("power_on") ||
         action == QLatin1String("power_off") ||
         action == QLatin1String("volume_up") ||
         action == QLatin1String("volume_down") ||
         action == QLatin1String("set_volume_percent") ||
         action == QLatin1String("mute") || action == QLatin1String("home") ||
         action == QLatin1String("back") || action == QLatin1String("menu") ||
         action == QLatin1String("settings") ||
         action == QLatin1String("input") || action == QLatin1String("apps") ||
         action == QLatin1String("play_pause") || action == QLatin1String("up") ||
         action == QLatin1String("down") || action == QLatin1String("left") ||
         action == QLatin1String("right") || action == QLatin1String("center");
}

int parseVoiceNumberToken(const QString &tokenText) {
  const QString token = tokenText.trimmed().toLower();
  bool ok = false;
  const int direct = token.toInt(&ok);
  if (ok)
    return direct;

  static const QHash<QString, int> map = {
      {QStringLiteral("one"), 1},       {QStringLiteral("two"), 2},
      {QStringLiteral("three"), 3},     {QStringLiteral("four"), 4},
      {QStringLiteral("five"), 5},      {QStringLiteral("six"), 6},
      {QStringLiteral("seven"), 7},     {QStringLiteral("eight"), 8},
      {QStringLiteral("nine"), 9},      {QStringLiteral("ten"), 10},
      {QStringLiteral("eleven"), 11},   {QStringLiteral("twelve"), 12},
      {QStringLiteral("thirteen"), 13}, {QStringLiteral("fourteen"), 14},
      {QStringLiteral("fifteen"), 15},  {QStringLiteral("sixteen"), 16},
      {QStringLiteral("seventeen"), 17},{QStringLiteral("eighteen"), 18},
      {QStringLiteral("nineteen"), 19}, {QStringLiteral("twenty"), 20},
  };
  return map.value(token, -1);
}

int parseSpokenNumberPhrase(const QString &phraseText) {
  QString text = normalizeVoiceText(phraseText);
  if (text.isEmpty())
    return -1;

  bool ok = false;
  const int direct = text.toInt(&ok);
  if (ok)
    return direct;

  QStringList tokens = text.split(' ', Qt::SkipEmptyParts);
  if (tokens.isEmpty())
    return -1;

  if (tokens.size() > 1 &&
      (tokens.first() == QLatin1String("to") ||
       tokens.first() == QLatin1String("two"))) {
    tokens.removeFirst();
  }
  if (tokens.isEmpty())
    return -1;

  static const QHash<QString, int> units = {
      {QStringLiteral("zero"), 0},   {QStringLiteral("one"), 1},
      {QStringLiteral("two"), 2},    {QStringLiteral("three"), 3},
      {QStringLiteral("four"), 4},   {QStringLiteral("five"), 5},
      {QStringLiteral("six"), 6},    {QStringLiteral("seven"), 7},
      {QStringLiteral("eight"), 8},  {QStringLiteral("nine"), 9},
      {QStringLiteral("ten"), 10},   {QStringLiteral("eleven"), 11},
      {QStringLiteral("twelve"), 12},{QStringLiteral("thirteen"), 13},
      {QStringLiteral("fourteen"), 14},{QStringLiteral("fifteen"), 15},
      {QStringLiteral("sixteen"), 16},{QStringLiteral("seventeen"), 17},
      {QStringLiteral("eighteen"), 18},{QStringLiteral("nineteen"), 19},
  };
  static const QHash<QString, int> tens = {
      {QStringLiteral("twenty"), 20}, {QStringLiteral("thirty"), 30},
      {QStringLiteral("forty"), 40},  {QStringLiteral("fifty"), 50},
      {QStringLiteral("sixty"), 60},  {QStringLiteral("seventy"), 70},
      {QStringLiteral("eighty"), 80}, {QStringLiteral("ninety"), 90},
  };

  int total = 0;
  int current = 0;
  for (const QString &token : tokens) {
    if (token == QLatin1String("and"))
      continue;
    if (units.contains(token)) {
      current += units.value(token);
      continue;
    }
    if (tens.contains(token)) {
      current += tens.value(token);
      continue;
    }
    if (token == QLatin1String("hundred")) {
      if (current == 0)
        current = 1;
      current *= 100;
      continue;
    }
    return -1;
  }
  total += current;
  return total;
}

bool extractTvTargetNumber(QString *text, int *targetNumber) {
  if (!text || !targetNumber)
    return false;

  const QRegularExpression suffixPattern(
      QStringLiteral("^(.*?)(?:\\s+(?:on|for)\\s+tv\\s+([a-z0-9]+))$"));
  const QRegularExpressionMatch match = suffixPattern.match(text->trimmed());
  if (!match.hasMatch())
    return false;

  const int parsed = parseVoiceNumberToken(match.captured(2));
  if (parsed <= 0)
    return false;

  *text = match.captured(1).trimmed();
  *targetNumber = parsed;
  return true;
}

bool capturePercent(const QString &text, const QRegularExpression &pattern,
                    int *valueOut) {
  if (!valueOut)
    return false;
  const QRegularExpressionMatch match = pattern.match(text);
  if (!match.hasMatch())
    return false;
  bool ok = false;
  const int value = match.captured(1).toInt(&ok);
  if (!ok)
    return false;
  *valueOut = value;
  return true;
}

bool captureSpokenPercent(const QString &text, const QRegularExpression &pattern,
                          int *valueOut) {
  if (!valueOut)
    return false;
  const QRegularExpressionMatch match = pattern.match(text);
  if (!match.hasMatch())
    return false;
  const int value = parseSpokenNumberPhrase(match.captured(1));
  if (value < 0)
    return false;
  *valueOut = value;
  return true;
}

bool copyPathIfMissing(const QString &sourcePath, const QString &targetPath) {
  const QFileInfo sourceInfo(sourcePath);
  if (!sourceInfo.exists())
    return true;

  if (sourceInfo.isDir()) {
    QDir().mkpath(targetPath);
    const QFileInfoList entries =
        QDir(sourcePath).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QFileInfo &entry : entries) {
      const QString targetEntry = QDir(targetPath).filePath(entry.fileName());
      if (!copyPathIfMissing(entry.absoluteFilePath(), targetEntry))
        return false;
    }
    return true;
  }

  if (QFileInfo::exists(targetPath))
    return true;

  QDir().mkpath(QFileInfo(targetPath).absolutePath());
  return QFile::copy(sourcePath, targetPath);
}

void migrateLegacyAndroidTvState(const QString &legacyRoot,
                                 const QString &targetRoot) {
  const QString cleanLegacy = QDir::cleanPath(legacyRoot);
  const QString cleanTarget = QDir::cleanPath(targetRoot);
  if (cleanLegacy.isEmpty() || cleanTarget.isEmpty() || cleanLegacy == cleanTarget ||
      !QFileInfo::exists(cleanLegacy)) {
    return;
  }

  const QFileInfoList entries =
      QDir(cleanLegacy).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
  for (const QFileInfo &entry : entries) {
    copyPathIfMissing(entry.absoluteFilePath(),
                      QDir(cleanTarget).filePath(entry.fileName()));
  }
}

} // namespace

AndroidTvManager::AndroidTvManager(QObject *parent) : QObject(parent) {
  m_idleShutdownTimer = new QTimer(this);
  m_idleShutdownTimer->setSingleShot(true);
  m_idleShutdownTimer->setInterval(12000);
  connect(m_idleShutdownTimer, &QTimer::timeout, this, [this]() {
    m_preserveConnectionOnNextStop = true;
    stopHelperProcess();
  });

  m_helperStartTimeoutTimer = new QTimer(this);
  m_helperStartTimeoutTimer->setSingleShot(true);
  m_helperStartTimeoutTimer->setInterval(10000);
  connect(m_helperStartTimeoutTimer, &QTimer::timeout, this, [this]() {
    if (!m_helperStarting)
      return;
    m_helperStarting = false;
    stopHelperProcess();
    failQueuedCommands(
        QStringLiteral("Android TV helper took too long to start."));
  });
}

AndroidTvConfig AndroidTvManager::loadConfig() const {
  QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
  AndroidTvConfig config;
  config.profileId =
      settings.value(QStringLiteral("androidTv/currentProfileId")).toString().trimmed();
  config.profileLabel =
      settings.value(QStringLiteral("androidTv/profileLabel")).toString().trimmed();
  config.stateKey =
      settings.value(QStringLiteral("androidTv/stateKey")).toString().trimmed();
  config.host = settings.value(QStringLiteral("androidTv/host")).toString().trimmed();
  config.apiPort = settings.value(QStringLiteral("androidTv/port"), 6466).toInt();
  config.pairingHost =
      settings.value(QStringLiteral("androidTv/pairingHost")).toString().trimmed();
  config.pairingPort =
      settings.value(QStringLiteral("androidTv/pairingPort"), 6467).toInt();
  config.pairingCode =
      settings.value(QStringLiteral("androidTv/pairingCode")).toString().trimmed();
  config.friendlyName =
      settings.value(QStringLiteral("androidTv/friendlyName")).toString().trimmed();
  config.voiceEnabled =
      settings.value(QStringLiteral("androidTv/voiceEnabled"), true).toBool();

  if (config.apiPort <= 0)
    config.apiPort = 6466;
  if (config.pairingPort <= 0)
    config.pairingPort = 6467;
  if (config.pairingHost.isEmpty())
    config.pairingHost = config.host;
  if (config.stateKey.isEmpty())
    config.stateKey = config.host;
  return config;
}

QString AndroidTvManager::helperPythonPath() const {
  return optionalServiceToolPath(QStringLiteral("android_tv_remote"),
                                 QStringLiteral("runtime/python/python.exe"));
}

QString AndroidTvManager::helperScriptPath() const {
  return optionalServiceToolPath(
      QStringLiteral("android_tv_remote"),
      QStringLiteral("runtime/quickstt_android_tv_helper.py"));
}

bool AndroidTvManager::isInstalled() const {
  return QFileInfo::exists(helperPythonPath()) && QFileInfo::exists(helperScriptPath());
}

bool AndroidTvManager::isConfigured() const {
  const AndroidTvConfig config = loadConfig();
  return isInstalled() && !config.host.isEmpty();
}

bool AndroidTvManager::isVoiceEnabled() const {
  return loadConfig().voiceEnabled;
}

bool AndroidTvManager::isPowerStateKnown() const {
  return m_lastState.contains(QStringLiteral("is_on")) &&
         !m_lastState.value(QStringLiteral("is_on")).isNull();
}

bool AndroidTvManager::isTvOn() const {
  return m_lastState.value(QStringLiteral("is_on")).toBool();
}

bool AndroidTvManager::supportsKeyCommands() const {
  return capabilityFlag(m_lastState, "key", true) || isActuallyConnected() ||
         currentConfigHasPairedCredentials();
}

bool AndroidTvManager::supportsPowerCommands() const {
  return capabilityFlag(m_lastState, "power", true) || isActuallyConnected() ||
         currentConfigHasPairedCredentials();
}

bool AndroidTvManager::supportsVolumeCommands() const {
  return capabilityFlag(m_lastState, "volume", true) || isActuallyConnected() ||
         currentConfigHasPairedCredentials();
}

bool AndroidTvManager::supportsAppLinks() const {
  return capabilityFlag(m_lastState, "app_link", false);
}

bool AndroidTvManager::hasVolumeInfo() const {
  const QJsonObject volume = m_lastState.value(QStringLiteral("volume")).toObject();
  return !volume.isEmpty() && volume.value(QStringLiteral("max")).toInt() > 0;
}

bool AndroidTvManager::hasDeviceInfo() const {
  return !m_lastState.value(QStringLiteral("device_info")).toObject().isEmpty();
}

int AndroidTvManager::currentVolumeLevel() const {
  return m_lastState.value(QStringLiteral("volume"))
      .toObject()
      .value(QStringLiteral("level"))
      .toInt(-1);
}

int AndroidTvManager::currentVolumeMax() const {
  return m_lastState.value(QStringLiteral("volume"))
      .toObject()
      .value(QStringLiteral("max"))
      .toInt(-1);
}

int AndroidTvManager::currentVolumePercent() const {
  const int level = currentVolumeLevel();
  const int maxLevel = currentVolumeMax();
  if (level < 0 || maxLevel <= 0)
    return -1;
  const double rawPercent = (static_cast<double>(level) * 100.0) /
                            static_cast<double>(maxLevel);
  return qBound(0, qRound(rawPercent), 100);
}

QString AndroidTvManager::currentAppId() const {
  return m_lastState.value(QStringLiteral("current_app")).toString().trimmed();
}

QString AndroidTvManager::stateRootPath() const {
  const QString root =
      QDir(quickSttDataRoot()).filePath(QStringLiteral("android_tv_state"));
  const QString legacyRoot = QDir(optionalServiceInstallPath(
                                      QStringLiteral("android_tv_remote")))
                                 .filePath(QStringLiteral("state"));
  QDir().mkpath(root);
  migrateLegacyAndroidTvState(legacyRoot, root);
  return root;
}

bool AndroidTvManager::hasPairedCredentials(const AndroidTvConfig &config) const {
  for (const QString &candidate : stateKeyCandidates(config)) {
    const QString certDir = QDir(stateRootPath()).filePath(candidate);
    if (QFileInfo::exists(QDir(certDir).filePath(QStringLiteral("cert.pem"))) &&
        QFileInfo::exists(QDir(certDir).filePath(QStringLiteral("key.pem")))) {
      return true;
    }
  }
  return false;
}

bool AndroidTvManager::currentConfigHasPairedCredentials() const {
  return hasPairedCredentials(loadConfig());
}

QStringList AndroidTvManager::stateKeyCandidates(const AndroidTvConfig &config) const {
  QStringList candidates;
  auto appendCandidate = [&candidates](const QString &raw) {
    const QString cleaned = cleanStateKey(raw);
    if (!cleaned.isEmpty() &&
        !candidates.contains(cleaned, Qt::CaseInsensitive)) {
      candidates << cleaned;
    }
  };

  appendCandidate(config.stateKey);
  appendCandidate(config.host);
  appendCandidate(config.pairingHost);
  return candidates;
}

QString AndroidTvManager::effectiveStateKey(const AndroidTvConfig &config) const {
  const QStringList candidates = stateKeyCandidates(config);
  for (const QString &candidate : candidates) {
    const QString certDir = QDir(stateRootPath()).filePath(candidate);
    if (QFileInfo::exists(QDir(certDir).filePath(QStringLiteral("cert.pem"))) &&
        QFileInfo::exists(QDir(certDir).filePath(QStringLiteral("key.pem")))) {
      return candidate;
    }
  }
  return candidates.isEmpty() ? QString() : candidates.first();
}

QString AndroidTvManager::endpointString() const {
  const AndroidTvConfig config = loadConfig();
  if (config.host.isEmpty())
    return QStringLiteral("Not set");
  return QStringLiteral("%1:%2").arg(config.host).arg(config.apiPort);
}

QString AndroidTvManager::currentPairingHost() const {
  const AndroidTvConfig config = loadConfig();
  return config.pairingHost.isEmpty() ? config.host : config.pairingHost;
}

bool AndroidTvManager::isActuallyConnected() const {
  if (m_lastState.contains(QStringLiteral("connected")))
    return m_lastState.value(QStringLiteral("connected")).toBool();
  return m_connected;
}

QString AndroidTvManager::connectionSummaryText() const {
  const AndroidTvConfig config = loadConfig();
  QStringList lines;
  lines << QStringLiteral("Saved TV profile: %1")
               .arg(config.profileLabel.isEmpty() ? QStringLiteral("Not selected")
                                                  : config.profileLabel);
  lines << QStringLiteral("Runtime: %1")
               .arg(isInstalled() ? helperPythonPath()
                                  : QStringLiteral("Not installed"));
  lines << QStringLiteral("TV address: %1")
               .arg(config.host.isEmpty() ? QStringLiteral("Not set") : config.host);
  lines << QStringLiteral("Remote port: %1").arg(config.apiPort);
  lines << QStringLiteral("Pair address: %1")
               .arg(currentPairingHost().isEmpty() ? QStringLiteral("Not set")
                                                   : currentPairingHost());
  lines << QStringLiteral("Pair port: %1").arg(config.pairingPort);
  lines << QStringLiteral("Controller name: %1")
               .arg(config.friendlyName.isEmpty() ? QStringLiteral("QuickSTT Android TV")
                                                  : config.friendlyName);
  lines << QStringLiteral("Voice control: %1")
               .arg(boolText(config.voiceEnabled));
  lines << QStringLiteral("Helper connected: %1").arg(boolText(isActuallyConnected()));
  lines << QStringLiteral("Paired credentials: %1")
               .arg(boolText(m_lastState.value(QStringLiteral("paired")).toBool()));
  lines << QStringLiteral("Pairing active: %1")
               .arg(boolText(m_lastState.value(QStringLiteral("pairing_active")).toBool()));

  if (m_lastState.contains(QStringLiteral("is_on")) &&
      !m_lastState.value(QStringLiteral("is_on")).isNull()) {
    lines << QStringLiteral("TV power state: %1")
                 .arg(m_lastState.value(QStringLiteral("is_on")).toBool()
                          ? QStringLiteral("On")
                          : QStringLiteral("Off"));
  }

  const QString currentApp =
      stateString(m_lastState, "current_app", QStringLiteral("Unknown"));
  lines << QStringLiteral("Current app: %1").arg(currentApp);

  const QJsonObject volume = m_lastState.value(QStringLiteral("volume")).toObject();
  if (!volume.isEmpty()) {
    lines << QStringLiteral("Volume: %1 / %2 (%3)")
                 .arg(volume.value(QStringLiteral("level")).toInt())
                 .arg(volume.value(QStringLiteral("max")).toInt())
                 .arg(volume.value(QStringLiteral("muted")).toBool()
                          ? QStringLiteral("Muted")
                          : QStringLiteral("Unmuted"));
  }

  const QJsonObject deviceInfo =
      m_lastState.value(QStringLiteral("device_info")).toObject();
  if (!deviceInfo.isEmpty()) {
    lines << QStringLiteral("Device: %1 %2")
                 .arg(deviceInfo.value(QStringLiteral("manufacturer")).toString(),
                      deviceInfo.value(QStringLiteral("model")).toString());
    lines << QStringLiteral("Software: %1")
                 .arg(deviceInfo.value(QStringLiteral("sw_version")).toString());
  }

  lines << QStringLiteral("Last status: %1").arg(m_statusText);
  return lines.join(QLatin1Char('\n'));
}

QString AndroidTvManager::helpText() const {
  return QStringLiteral(
      "Pair once, then QuickSTT remembers that TV on this device.\n"
      "Press Rescan if the TV changes networks or appears later.\n\n"
      "Voice examples:\n"
      "- turn on tv / turn on the television / power on tv / wake up television\n"
      "- turn off tv / turn off the television / power off tv / sleep television\n"
      "- volume up tv / raise television volume / increase volume on tv / make television louder\n"
      "- volume down tv / lower television volume / decrease volume on tv / make television quieter\n"
      "- turn volume to 40 on television 1 / set volume to 65 on tv 2\n"
      "- increase volume by 10 on television 1 / decrease volume by 15 on tv 2\n"
      "- mute tv / mute the television / tv mute / silence television\n"
      "- go home on tv / tv home / open television home / go to tv home\n"
      "- go back on tv / back on television / tv back / back on the tv\n"
      "- open tv menu / television menu / show tv menu / menu on television\n"
      "- open tv settings / television settings / show tv settings / settings on television\n"
      "- switch tv input / television input / change tv input / open television source\n"
      "- show tv apps / television apps / open tv apps / show apps on television\n"
      "- tv select / select on television / ok on tv / press ok on television\n"
      "- tv up / press up on television / move up on tv / navigate up on television\n"
      "- tv down / press down on television / move down on tv / navigate down on television\n"
      "- tv left / press left on television / move left on tv / navigate left on television\n"
      "- tv right / press right on television / move right on tv / navigate right on television\n"
      "- play pause on tv / television play pause / pause tv / resume television");
}

void AndroidTvManager::setStatus(const QString &statusText) {
  m_statusText = statusText.trimmed().isEmpty()
                     ? QStringLiteral("Android TV control is idle.")
                     : statusText.trimmed();
  emit statusChanged(m_statusText);
}

void AndroidTvManager::setConnected(bool connected) {
  if (m_connected == connected)
    return;
  m_connected = connected;
  emit connectionChanged(m_connected);
}

void AndroidTvManager::applyState(const QJsonObject &stateObject) {
  if (stateObject.isEmpty())
    return;
  m_lastState = stateObject;
  setConnected(stateObject.value(QStringLiteral("connected")).toBool());
  emit stateChanged(m_lastState);
}

bool AndroidTvManager::ensureHelperStarted() {
  cancelIdleShutdown();
  m_preserveConnectionOnNextStop = false;
  if (m_helperProcess &&
      m_helperProcess->state() == QProcess::Running &&
      m_helperProcess->program() == helperPythonPath()) {
    return true;
  }
  if (m_helperStarting && m_helperProcess &&
      m_helperProcess->state() == QProcess::Starting &&
      m_helperProcess->program() == helperPythonPath()) {
    return true;
  }

  if (!isInstalled()) {
    setStatus(QStringLiteral("Install Android TV support first."));
    emit controlFailed(m_statusText);
    return false;
  }

  if (!m_helperProcess) {
    m_helperProcess = new QProcess(this);
    connect(m_helperProcess, &QProcess::started, this,
            &AndroidTvManager::onHelperStarted);
    connect(m_helperProcess, &QProcess::readyReadStandardOutput, this,
            &AndroidTvManager::onHelperStdoutReady);
    connect(m_helperProcess, &QProcess::readyReadStandardError, this,
            &AndroidTvManager::onHelperStderrReady);
    connect(m_helperProcess, &QProcess::errorOccurred, this,
            &AndroidTvManager::onHelperErrorOccurred);
    connect(m_helperProcess,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                &QProcess::finished),
            this, &AndroidTvManager::onHelperFinished);
  } else if (m_helperProcess->state() != QProcess::NotRunning) {
    stopHelperProcess();
  }

  m_stdoutBuffer.clear();
  m_stderrBuffer.clear();
  m_pendingActions.clear();
  m_pendingConfigs.clear();
  m_queuedPayloads.clear();
  m_helperProcess->setWorkingDirectory(
      optionalServiceInstallPath(QStringLiteral("android_tv_remote")));
  m_helperProcess->setProgram(helperPythonPath());
  m_helperProcess->setArguments({helperScriptPath()});
  m_helperStarting = true;
  setStatus(QStringLiteral("Starting Android TV support..."));
  m_helperProcess->start();
  if (m_helperStartTimeoutTimer)
    m_helperStartTimeoutTimer->start();
  return true;
}

int AndroidTvManager::sendHelperCommand(const QString &action,
                                        const QJsonObject &data,
                                        const AndroidTvConfig *config) {
  if (!ensureHelperStarted())
    return -1;

  const int requestId = m_nextRequestId++;
  QJsonObject payload;
  payload.insert(QStringLiteral("id"), requestId);
  payload.insert(QStringLiteral("action"), action);
  payload.insert(QStringLiteral("data"), data);
  const QByteArray json =
      QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n';
  m_pendingActions.insert(requestId, action);
  if (config)
    m_pendingConfigs.insert(requestId, *config);
  qInfo() << "[ANDROID-TV]" << "request" << requestId << action
          << data.value(QStringLiteral("host")).toString().trimmed();

  if (!m_helperProcess || m_helperStarting ||
      m_helperProcess->state() != QProcess::Running) {
    m_queuedPayloads.append(json);
    return requestId;
  }

  if (m_helperProcess->write(json) != json.size()) {
    m_pendingActions.remove(requestId);
    const QString error =
        QStringLiteral("Failed to send command to the Android TV helper.");
    setStatus(error);
    emit controlFailed(error);
    return -1;
  }
  return requestId;
}

void AndroidTvManager::handleHelperLine(const QByteArray &lineBytes) {
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(lineBytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    qWarning() << "[ANDROID-TV-HELPER] Invalid JSON line:" << lineBytes;
    return;
  }

  const QJsonObject object = document.object();
  const QString type = object.value(QStringLiteral("type")).toString();
  if (type == QLatin1String("response")) {
    handleHelperResponse(object);
  } else if (type == QLatin1String("event")) {
    handleHelperEvent(object);
  }
}

void AndroidTvManager::onHelperStarted() {
  m_helperStarting = false;
  if (m_helperStartTimeoutTimer)
    m_helperStartTimeoutTimer->stop();

  QJsonObject helloPayload;
  const int helloRequestId = m_nextRequestId++;
  helloPayload.insert(QStringLiteral("id"), helloRequestId);
  helloPayload.insert(QStringLiteral("action"), QStringLiteral("hello"));
  helloPayload.insert(QStringLiteral("data"), QJsonObject());
  m_pendingActions.insert(helloRequestId, QStringLiteral("hello"));
  const QByteArray helloJson =
      QJsonDocument(helloPayload).toJson(QJsonDocument::Compact) + '\n';
  if (!m_helperProcess || m_helperProcess->write(helloJson) != helloJson.size()) {
    failQueuedCommands(QStringLiteral("Failed to initialize Android TV support."));
    return;
  }

  flushQueuedCommands();
}

void AndroidTvManager::onHelperStdoutReady() {
  if (!m_helperProcess)
    return;
  m_stdoutBuffer += m_helperProcess->readAllStandardOutput();
  qsizetype newlineIndex = -1;
  while ((newlineIndex = m_stdoutBuffer.indexOf('\n')) >= 0) {
    const QByteArray line = m_stdoutBuffer.left(newlineIndex).trimmed();
    m_stdoutBuffer.remove(0, newlineIndex + 1);
    if (!line.isEmpty())
      handleHelperLine(line);
  }
}

void AndroidTvManager::onHelperStderrReady() {
  if (!m_helperProcess)
    return;
  m_stderrBuffer += m_helperProcess->readAllStandardError();
  qsizetype newlineIndex = -1;
  while ((newlineIndex = m_stderrBuffer.indexOf('\n')) >= 0) {
    const QByteArray line = m_stderrBuffer.left(newlineIndex).trimmed();
    m_stderrBuffer.remove(0, newlineIndex + 1);
    if (!line.isEmpty())
      qWarning() << "[ANDROID-TV-HELPER]" << line;
  }
}

void AndroidTvManager::onHelperErrorOccurred(QProcess::ProcessError) {
  if (m_helperProcess && m_helperProcess->state() == QProcess::Running)
    return;
  if (!m_helperStarting && m_queuedPayloads.isEmpty())
    return;

  m_helperStarting = false;
  if (m_helperStartTimeoutTimer)
    m_helperStartTimeoutTimer->stop();
  const QString error = m_helperProcess
                            ? QStringLiteral("Android TV support failed to start: %1")
                                  .arg(m_helperProcess->errorString())
                            : QStringLiteral("Android TV support failed to start.");
  failQueuedCommands(error);
}

void AndroidTvManager::onHelperFinished(int, QProcess::ExitStatus) {
  cancelIdleShutdown();
  m_helperStarting = false;
  if (m_helperStartTimeoutTimer)
    m_helperStartTimeoutTimer->stop();
  const bool preserveConnection = m_preserveConnectionOnNextStop;
  m_preserveConnectionOnNextStop = false;
  if (!preserveConnection)
    setConnected(false);
  if (m_pendingActions.isEmpty())
    m_pendingConfigs.clear();
  if (!m_pendingActions.isEmpty()) {
    const QString error =
        QStringLiteral("Android TV helper stopped unexpectedly.");
    failQueuedCommands(error);
  }
}

void AndroidTvManager::failQueuedCommands(const QString &errorText) {
  m_queuedPayloads.clear();
  m_pendingActions.clear();
  m_pendingConfigs.clear();
  m_discoveryReconnectPending = false;
  m_discoveryReconnectConfig = AndroidTvConfig{};
  m_discoveryReconnectError.clear();
  setStatus(errorText);
  emit controlFailed(errorText);
}

void AndroidTvManager::flushQueuedCommands() {
  if (!m_helperProcess || m_helperProcess->state() != QProcess::Running)
    return;

  while (!m_queuedPayloads.isEmpty()) {
    const QByteArray payload = m_queuedPayloads.takeFirst();
    if (m_helperProcess->write(payload) != payload.size()) {
      failQueuedCommands(
          QStringLiteral("Failed to send command to the Android TV helper."));
      return;
    }
  }
}

void AndroidTvManager::scheduleIdleShutdown() {
  if (!m_idleShutdownTimer)
    return;
  if (!m_pendingActions.isEmpty())
    return;
  if (m_lastState.value(QStringLiteral("pairing_active")).toBool())
    return;
  if (!m_helperProcess || m_helperProcess->state() != QProcess::Running)
    return;
  m_idleShutdownTimer->start();
}

void AndroidTvManager::cancelIdleShutdown() {
  if (m_idleShutdownTimer)
    m_idleShutdownTimer->stop();
}

void AndroidTvManager::stopHelperProcess() {
  cancelIdleShutdown();
  if (!m_helperProcess || m_helperProcess->state() == QProcess::NotRunning)
    return;
  m_helperProcess->terminate();
  if (!m_helperProcess->waitForFinished(1500)) {
    m_helperProcess->kill();
    m_helperProcess->waitForFinished(2000);
  }
}

void AndroidTvManager::handleHelperResponse(const QJsonObject &object) {
  const int requestId = object.value(QStringLiteral("id")).toInt(-1);
  const QString action = m_pendingActions.take(requestId);
  const AndroidTvConfig config = m_pendingConfigs.take(requestId);
  const bool ok = object.value(QStringLiteral("ok")).toBool(false);
  const QString message =
      object.value(QStringLiteral("message")).toString().trimmed();
  applyState(object.value(QStringLiteral("state")).toObject());
  qInfo() << "[ANDROID-TV]" << "response" << requestId << action << ok << message;

  if (action == QLatin1String("discover")) {
    const QJsonArray devices = object.value(QStringLiteral("devices")).toArray();
    emit discoveryChanged(devices);
    if (m_discoveryReconnectPending) {
      AndroidTvConfig resolvedConfig;
      if (tryResolveDiscoveryReconnect(m_discoveryReconnectConfig, devices,
                                       &resolvedConfig)) {
        persistConfig(resolvedConfig);
        m_runtimeConfig = AndroidTvConfig{};
        m_discoveryReconnectPending = false;
        m_discoveryReconnectError.clear();
        m_discoveryReconnectConfig = AndroidTvConfig{};
        queueControlCommand(QStringLiteral("connect"),
                            QStringLiteral(
                                "Found your remembered TV on the LAN again. Reconnecting now..."),
                            QJsonObject(), &resolvedConfig);
        return;
      }
      const QString fallbackError =
          m_discoveryReconnectError.trimmed().isEmpty()
              ? QStringLiteral(
                    "QuickSTT could not find the remembered Android TV on your LAN.")
              : m_discoveryReconnectError.trimmed();
      m_discoveryReconnectPending = false;
      m_discoveryReconnectError.clear();
      m_discoveryReconnectConfig = AndroidTvConfig{};
      setStatus(fallbackError);
      emit controlFailed(fallbackError);
      scheduleIdleShutdown();
      return;
    }
  }

  if (!ok) {
    const QString error = message.isEmpty()
                              ? QStringLiteral("Android TV command failed.")
                              : message;
    if (action == QLatin1String("connect") &&
        currentConfigHasPairedCredentials() &&
        error.contains(QStringLiteral("Timed out while talking to the Android TV"),
                       Qt::CaseInsensitive) &&
        !m_discoveryReconnectPending) {
      m_discoveryReconnectPending = true;
      m_discoveryReconnectConfig =
          config.host.trimmed().isEmpty() ? loadConfig() : config;
      m_discoveryReconnectError =
          QStringLiteral("%1 QuickSTT also checked your LAN for the remembered TV. If the TV changed IPs, it will reconnect automatically when found.")
              .arg(error);
      setStatus(QStringLiteral(
          "Timed out connecting to the remembered TV. Checking your LAN for its current address..."));
      QJsonObject discoverData;
      discoverData.insert(QStringLiteral("timeout_ms"), 2600);
      sendHelperCommand(QStringLiteral("discover"), discoverData);
      return;
    }
    setStatus(error);
    emit controlFailed(error);
    scheduleIdleShutdown();
    return;
  }

  if (!config.host.trimmed().isEmpty() &&
      action != QLatin1String("discover") && action != QLatin1String("hello")) {
    m_runtimeConfig = config;
  }

  if (!message.isEmpty() && action != QLatin1String("status"))
    setStatus(message);

  if (action == QLatin1String("prepare_pair")) {
    QString prompt = message;
    const QString tvName = object.value(QStringLiteral("tv_name")).toString().trimmed();
    const QString tvMac = object.value(QStringLiteral("tv_mac")).toString().trimmed();
    if (!tvName.isEmpty()) {
      prompt += QStringLiteral("\n\nTV: %1").arg(tvName);
      if (!tvMac.isEmpty())
        prompt += QStringLiteral(" (%1)").arg(tvMac);
    }
    prompt += QStringLiteral(
        "\n\nLook at the TV, then enter the 6-character pairing code.");
    emit pairingCodeRequested(prompt.trimmed());
  }

  if (action == QLatin1String("finish_pair"))
    applyState(object.value(QStringLiteral("state")).toObject());

  if (action != QLatin1String("hello") && action != QLatin1String("status")) {
    const QString success = message.isEmpty() ? controlActionMessage(action) : message;
    emit controlFinished(success);
  }
  scheduleIdleShutdown();
}

void AndroidTvManager::handleHelperEvent(const QJsonObject &object) {
  const QString name = object.value(QStringLiteral("name")).toString();
  if (name == QLatin1String("state")) {
    applyState(object.value(QStringLiteral("state")).toObject());
    const QString message =
        object.value(QStringLiteral("message")).toString().trimmed();
    if (!message.isEmpty())
      setStatus(message);
    scheduleIdleShutdown();
  }
}

QString AndroidTvManager::controlActionMessage(const QString &action) const {
  if (action == QLatin1String("prepare_pair"))
    return QStringLiteral("Android TV pairing started.");
  if (action == QLatin1String("finish_pair"))
    return QStringLiteral("Android TV pairing finished.");
  if (action == QLatin1String("connect"))
    return QStringLiteral("Android TV connected.");
  if (action == QLatin1String("disconnect"))
    return QStringLiteral("Android TV disconnected.");
  if (action == QLatin1String("status"))
    return QStringLiteral("Android TV status refreshed.");
  if (action == QLatin1String("power_on"))
    return QStringLiteral("Android TV wake command sent.");
  if (action == QLatin1String("power_off"))
    return QStringLiteral("Android TV sleep command sent.");
  if (action == QLatin1String("volume_up"))
    return QStringLiteral("Android TV volume up sent.");
  if (action == QLatin1String("volume_down"))
    return QStringLiteral("Android TV volume down sent.");
  if (action == QLatin1String("set_volume_percent"))
    return QStringLiteral("Android TV volume updated.");
  if (action == QLatin1String("mute"))
    return QStringLiteral("Android TV mute command sent.");
  if (action == QLatin1String("home"))
    return QStringLiteral("Android TV home command sent.");
  if (action == QLatin1String("back"))
    return QStringLiteral("Android TV back command sent.");
  if (action == QLatin1String("menu"))
    return QStringLiteral("Android TV menu command sent.");
  if (action == QLatin1String("settings"))
    return QStringLiteral("Android TV settings command sent.");
  if (action == QLatin1String("input"))
    return QStringLiteral("Android TV input command sent.");
  if (action == QLatin1String("apps"))
    return QStringLiteral("Android TV apps command sent.");
  if (action == QLatin1String("play_pause"))
    return QStringLiteral("Android TV play/pause sent.");
  if (action == QLatin1String("up") || action == QLatin1String("down") ||
      action == QLatin1String("left") || action == QLatin1String("right") ||
      action == QLatin1String("center")) {
    return QStringLiteral("Android TV navigation command sent.");
  }
  return QStringLiteral("Android TV command sent.");
}

AndroidTvConfig AndroidTvManager::resolvedConfigForAction(
    const QString &action) const {
  AndroidTvConfig config = loadConfig();
  const bool hasRuntimeConfig = !m_runtimeConfig.host.trimmed().isEmpty();
  if (!hasRuntimeConfig)
    return config;

  if (action == QLatin1String("prepare_pair") ||
      action == QLatin1String("finish_pair") ||
      action == QLatin1String("discover") ||
      action == QLatin1String("hello")) {
    return config;
  }

  if (isRemoteControlAction(action) || action == QLatin1String("status") ||
      action == QLatin1String("disconnect")) {
    if (isActuallyConnected() || config.host.trimmed().isEmpty())
      return m_runtimeConfig;
  }

  if (action == QLatin1String("connect") && config.host.trimmed().isEmpty())
    return m_runtimeConfig;

  return config;
}

QList<AndroidTvConfig> AndroidTvManager::loadProfiles() const {
  QList<AndroidTvConfig> profiles;
  QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
  QByteArray raw = settings.value(QStringLiteral("androidTv/profilesJson")).toByteArray();
  if (raw.isEmpty()) {
    const QString rawString =
        settings.value(QStringLiteral("androidTv/profilesJson")).toString().trimmed();
    if (!rawString.isEmpty())
      raw = rawString.toUtf8();
  }

  const auto appendProfile = [&](const QJsonObject &profileObject) {
    AndroidTvConfig profile;
    profile.profileId =
        profileObject.value(QStringLiteral("id")).toString().trimmed();
    profile.profileLabel =
        profileObject.value(QStringLiteral("label")).toString().trimmed();
    profile.stateKey =
        profileObject.value(QStringLiteral("stateKey")).toString().trimmed();
    profile.host = profileObject.value(QStringLiteral("host")).toString().trimmed();
    profile.apiPort =
        profileObject.value(QStringLiteral("apiPort")).toInt(6466);
    profile.pairingHost =
        profileObject.value(QStringLiteral("pairingHost")).toString().trimmed();
    profile.pairingPort =
        profileObject.value(QStringLiteral("pairingPort")).toInt(6467);
    profile.pairingCode =
        profileObject.value(QStringLiteral("pairingCode")).toString().trimmed();
    profile.friendlyName =
        profileObject.value(QStringLiteral("friendlyName")).toString().trimmed();
    profile.voiceEnabled =
        profileObject.value(QStringLiteral("voiceEnabled")).toBool(true);
    if (profile.apiPort <= 0)
      profile.apiPort = 6466;
    if (profile.pairingPort <= 0)
      profile.pairingPort = 6467;
    if (profile.pairingHost.isEmpty())
      profile.pairingHost = profile.host;
    if (profile.stateKey.isEmpty())
      profile.stateKey = profile.host;
    if (!profile.host.isEmpty())
      profiles.append(profile);
  };

  if (!raw.isEmpty()) {
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (doc.isArray()) {
      const QJsonArray arr = doc.array();
      for (const QJsonValue &value : arr) {
        if (value.isObject())
          appendProfile(value.toObject());
      }
    }
  }

  if (profiles.isEmpty()) {
    const AndroidTvConfig config = loadConfig();
    if (!config.host.isEmpty())
      profiles.append(config);
  }

  return profiles;
}

void AndroidTvManager::persistConfig(const AndroidTvConfig &config) const {
  QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
  if (!config.profileId.trimmed().isEmpty()) {
    settings.setValue(QStringLiteral("androidTv/currentProfileId"),
                      config.profileId.trimmed());
  }
  settings.setValue(QStringLiteral("androidTv/profileLabel"),
                    config.profileLabel.trimmed());
  settings.setValue(QStringLiteral("androidTv/stateKey"),
                    config.stateKey.trimmed());
  settings.setValue(QStringLiteral("androidTv/host"), config.host.trimmed());
  settings.setValue(QStringLiteral("androidTv/port"), config.apiPort);
  settings.setValue(QStringLiteral("androidTv/pairingHost"),
                    config.pairingHost.trimmed().isEmpty() ? config.host.trimmed()
                                                           : config.pairingHost.trimmed());
  settings.setValue(QStringLiteral("androidTv/pairingPort"), config.pairingPort);
  settings.setValue(QStringLiteral("androidTv/pairingCode"),
                    config.pairingCode.trimmed());
  settings.setValue(QStringLiteral("androidTv/friendlyName"),
                    config.friendlyName.trimmed());
  settings.setValue(QStringLiteral("androidTv/voiceEnabled"), config.voiceEnabled);

  QByteArray raw = settings.value(QStringLiteral("androidTv/profilesJson")).toByteArray();
  if (raw.isEmpty()) {
    const QString rawString =
        settings.value(QStringLiteral("androidTv/profilesJson")).toString().trimmed();
    if (!rawString.isEmpty())
      raw = rawString.toUtf8();
  }

  QJsonArray profiles;
  if (!raw.isEmpty()) {
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (doc.isArray())
      profiles = doc.array();
  }

  bool replaced = false;
  for (int i = 0; i < profiles.size(); ++i) {
    if (!profiles.at(i).isObject())
      continue;
    QJsonObject profileObject = profiles.at(i).toObject();
    const QString existingId =
        profileObject.value(QStringLiteral("id")).toString().trimmed();
    const QString existingStateKey =
        profileObject.value(QStringLiteral("stateKey")).toString().trimmed();
    const QString existingLabel =
        profileObject.value(QStringLiteral("label")).toString().trimmed();
    if ((!config.profileId.trimmed().isEmpty() &&
         existingId.compare(config.profileId.trimmed(), Qt::CaseInsensitive) == 0) ||
        (!config.stateKey.trimmed().isEmpty() &&
         existingStateKey.compare(config.stateKey.trimmed(), Qt::CaseInsensitive) == 0) ||
        (!config.profileLabel.trimmed().isEmpty() &&
         existingLabel.compare(config.profileLabel.trimmed(), Qt::CaseInsensitive) == 0)) {
      profileObject.insert(QStringLiteral("id"),
                           config.profileId.trimmed().isEmpty()
                               ? existingId
                               : config.profileId.trimmed());
      profileObject.insert(QStringLiteral("label"), config.profileLabel.trimmed());
      profileObject.insert(QStringLiteral("stateKey"), config.stateKey.trimmed());
      profileObject.insert(QStringLiteral("host"), config.host.trimmed());
      profileObject.insert(QStringLiteral("apiPort"), config.apiPort);
      profileObject.insert(QStringLiteral("pairingHost"),
                           config.pairingHost.trimmed().isEmpty()
                               ? config.host.trimmed()
                               : config.pairingHost.trimmed());
      profileObject.insert(QStringLiteral("pairingPort"), config.pairingPort);
      profileObject.insert(QStringLiteral("pairingCode"),
                           config.pairingCode.trimmed());
      profileObject.insert(QStringLiteral("friendlyName"),
                           config.friendlyName.trimmed());
      profileObject.insert(QStringLiteral("voiceEnabled"), config.voiceEnabled);
      profiles.replace(i, profileObject);
      replaced = true;
      break;
    }
  }

  if (!replaced && !config.host.trimmed().isEmpty()) {
    QJsonObject profileObject;
    profileObject.insert(QStringLiteral("id"), config.profileId.trimmed());
    profileObject.insert(QStringLiteral("label"), config.profileLabel.trimmed());
    profileObject.insert(QStringLiteral("stateKey"), config.stateKey.trimmed());
    profileObject.insert(QStringLiteral("host"), config.host.trimmed());
    profileObject.insert(QStringLiteral("apiPort"), config.apiPort);
    profileObject.insert(QStringLiteral("pairingHost"),
                         config.pairingHost.trimmed().isEmpty()
                             ? config.host.trimmed()
                             : config.pairingHost.trimmed());
    profileObject.insert(QStringLiteral("pairingPort"), config.pairingPort);
    profileObject.insert(QStringLiteral("pairingCode"), config.pairingCode.trimmed());
    profileObject.insert(QStringLiteral("friendlyName"),
                         config.friendlyName.trimmed());
    profileObject.insert(QStringLiteral("voiceEnabled"), config.voiceEnabled);
    profiles.append(profileObject);
  }

  settings.setValue(QStringLiteral("androidTv/profilesJson"),
                    QString::fromUtf8(QJsonDocument(profiles).toJson(
                        QJsonDocument::Compact)));
  settings.sync();
}

bool AndroidTvManager::tryResolveDiscoveryReconnect(
    const AndroidTvConfig &config, const QJsonArray &devices,
    AndroidTvConfig *resolvedConfig) const {
  if (!resolvedConfig)
    return false;

  const QString normalizedLabel = normalizedAndroidTvName(config.profileLabel);
  const QString normalizedFriendly =
      normalizedAndroidTvName(config.friendlyName);
  const QString normalizedState = cleanStateKey(config.stateKey);

  const auto matchesDevice = [&](const QJsonObject &device) {
    const QString host = device.value(QStringLiteral("host")).toString().trimmed();
    if (!host.isEmpty() &&
        cleanStateKey(host).compare(normalizedState, Qt::CaseInsensitive) == 0) {
      return true;
    }
    const QString name = normalizedAndroidTvName(
        device.value(QStringLiteral("name")).toString().trimmed());
    if (!name.isEmpty() &&
        ((!normalizedLabel.isEmpty() && name == normalizedLabel) ||
         (!normalizedFriendly.isEmpty() && name == normalizedFriendly))) {
      return true;
    }
    const QString serviceName =
        normalizedAndroidTvName(
            device.value(QStringLiteral("service_name")).toString().trimmed());
    if (!serviceName.isEmpty() &&
        ((!normalizedLabel.isEmpty() && serviceName.contains(normalizedLabel)) ||
         (!normalizedFriendly.isEmpty() &&
          serviceName.contains(normalizedFriendly)))) {
      return true;
    }
    return false;
  };

  QList<QJsonObject> candidates;
  for (const QJsonValue &value : devices) {
    if (!value.isObject())
      continue;
    const QJsonObject device = value.toObject();
    if (matchesDevice(device))
      candidates.append(device);
  }
  if (candidates.isEmpty() && devices.size() == 1 && devices.first().isObject())
    candidates.append(devices.first().toObject());
  if (candidates.isEmpty())
    return false;

  const QJsonObject device = candidates.first();
  AndroidTvConfig updated = config;
  const QString host = device.value(QStringLiteral("host")).toString().trimmed();
  if (host.isEmpty())
    return false;
  updated.host = host;
  updated.pairingHost = host;
  updated.apiPort = device.value(QStringLiteral("api_port")).toInt(6466);
  updated.pairingPort = device.value(QStringLiteral("pair_port")).toInt(6467);
  if (updated.profileLabel.trimmed().isEmpty())
    updated.profileLabel = device.value(QStringLiteral("name")).toString().trimmed();
  *resolvedConfig = updated;
  return true;
}

void AndroidTvManager::queueControlCommand(const QString &action,
                                           const QString &queuedStatusText,
                                           const QJsonObject &extraData,
                                           const AndroidTvConfig *overrideConfig) {
  const AndroidTvConfig config =
      (overrideConfig && !overrideConfig->host.trimmed().isEmpty())
          ? *overrideConfig
          : resolvedConfigForAction(action);
  if (!isInstalled()) {
    const QString error = QStringLiteral("Install Android TV support first.");
    setStatus(error);
    emit controlFailed(error);
    return;
  }

  if ((action != QLatin1String("disconnect") && action != QLatin1String("hello")) &&
      config.host.isEmpty()) {
    const QString error =
        QStringLiteral("Select a discovered or remembered TV first.");
    setStatus(error);
    emit controlFailed(error);
    return;
  }

  if (action == QLatin1String("finish_pair") && config.pairingCode.trimmed().isEmpty()) {
    const QString error =
        QStringLiteral("Enter the pairing code shown on the TV first.");
    setStatus(error);
    emit controlFailed(error);
    return;
  }

  QJsonObject data;
  data.insert(QStringLiteral("host"),
              action == QLatin1String("prepare_pair") ||
                      action == QLatin1String("finish_pair")
                  ? currentPairingHost()
                  : config.host);
  data.insert(QStringLiteral("api_port"), config.apiPort);
  data.insert(QStringLiteral("pair_port"), config.pairingPort);
  data.insert(QStringLiteral("client_name"),
              config.friendlyName.trimmed().isEmpty()
                  ? QStringLiteral("QuickSTT Android TV")
                  : config.friendlyName.trimmed());
  data.insert(QStringLiteral("state_root"), stateRootPath());
  data.insert(QStringLiteral("state_key"), effectiveStateKey(config));
  if (action == QLatin1String("finish_pair"))
    data.insert(QStringLiteral("pairing_code"), config.pairingCode.trimmed());
  for (auto it = extraData.begin(); it != extraData.end(); ++it)
    data.insert(it.key(), it.value());

  if (!queuedStatusText.trimmed().isEmpty())
    setStatus(queuedStatusText);
  sendHelperCommand(action, data, &config);
}

void AndroidTvManager::startPairing() {
  queueControlCommand(QStringLiteral("prepare_pair"),
                      QStringLiteral("Starting Android TV pairing..."));
}

void AndroidTvManager::finishPairing() {
  queueControlCommand(QStringLiteral("finish_pair"),
                      QStringLiteral("Finishing Android TV pairing..."));
}

void AndroidTvManager::pairDevice() { startPairing(); }

void AndroidTvManager::scanForDevices(int timeoutMs) {
  if (!isInstalled()) {
    const QString error = QStringLiteral("Install Android TV support first.");
    setStatus(error);
    emit controlFailed(error);
    return;
  }

  QJsonObject data;
  data.insert(QStringLiteral("timeout_ms"), qMax(1200, timeoutMs));
  setStatus(QStringLiteral("Scanning your LAN for Android TV devices..."));
  sendHelperCommand(QStringLiteral("discover"), data);
}

void AndroidTvManager::connectDevice() {
  queueControlCommand(QStringLiteral("connect"),
                      QStringLiteral("Connecting to Android TV..."));
}

void AndroidTvManager::disconnectDevice() {
  queueControlCommand(QStringLiteral("disconnect"),
                      QStringLiteral("Disconnecting from Android TV..."));
}

void AndroidTvManager::forgetCurrentDevice() {
  const AndroidTvConfig config = loadConfig();
  disconnectDevice();
  for (const QString &candidate : stateKeyCandidates(config)) {
    const QString certDir = QDir(stateRootPath()).filePath(candidate);
    if (QFileInfo::exists(certDir))
      QDir(certDir).removeRecursively();
  }
  m_lastState = QJsonObject();
  m_runtimeConfig = AndroidTvConfig{};
  setConnected(false);
  setStatus(QStringLiteral("QuickSTT forgot the saved pairing for this TV."));
}

void AndroidTvManager::refreshState() {
  queueControlCommand(QStringLiteral("status"), QString());
}

void AndroidTvManager::turnOn() {
  queueControlCommand(QStringLiteral("power_on"),
                      QStringLiteral("Sending Android TV power-on command..."));
}

void AndroidTvManager::turnOff() {
  queueControlCommand(QStringLiteral("power_off"),
                      QStringLiteral("Sending Android TV power-off command..."));
}

void AndroidTvManager::volumeUp() {
  queueControlCommand(QStringLiteral("volume_up"),
                      QStringLiteral("Sending Android TV volume up..."));
}

void AndroidTvManager::volumeDown() {
  queueControlCommand(QStringLiteral("volume_down"),
                      QStringLiteral("Sending Android TV volume down..."));
}

void AndroidTvManager::setVolumePercent(int percent) {
  const int clamped = qBound(0, percent, 100);
  if (currentVolumePercent() == clamped) {
    setStatus(QStringLiteral("Android TV volume is already at %1%.").arg(clamped));
    return;
  }
  QJsonObject extraData;
  extraData.insert(QStringLiteral("target_percent"), clamped);
  queueControlCommand(QStringLiteral("set_volume_percent"),
                      QStringLiteral("Setting Android TV volume to %1%...")
                          .arg(clamped),
                      extraData);
}

void AndroidTvManager::muteToggle() {
  queueControlCommand(QStringLiteral("mute"),
                      QStringLiteral("Sending Android TV mute command..."));
}

void AndroidTvManager::goHome() {
  queueControlCommand(QStringLiteral("home"),
                      QStringLiteral("Sending Android TV home command..."));
}

void AndroidTvManager::goBack() {
  queueControlCommand(QStringLiteral("back"),
                      QStringLiteral("Sending Android TV back command..."));
}

void AndroidTvManager::openMenu() {
  queueControlCommand(QStringLiteral("menu"),
                      QStringLiteral("Sending Android TV menu command..."));
}

void AndroidTvManager::openSettings() {
  queueControlCommand(QStringLiteral("settings"),
                      QStringLiteral("Opening Android TV settings..."));
}

void AndroidTvManager::openInputSelector() {
  queueControlCommand(QStringLiteral("input"),
                      QStringLiteral("Opening Android TV input selector..."));
}

void AndroidTvManager::showApps() {
  queueControlCommand(QStringLiteral("apps"),
                      QStringLiteral("Opening Android TV apps..."));
}

void AndroidTvManager::playPause() {
  queueControlCommand(QStringLiteral("play_pause"),
                      QStringLiteral("Sending Android TV play/pause..."));
}

void AndroidTvManager::navigateUp() {
  queueControlCommand(QStringLiteral("up"),
                      QStringLiteral("Sending Android TV up command..."));
}

void AndroidTvManager::navigateDown() {
  queueControlCommand(QStringLiteral("down"),
                      QStringLiteral("Sending Android TV down command..."));
}

void AndroidTvManager::navigateLeft() {
  queueControlCommand(QStringLiteral("left"),
                      QStringLiteral("Sending Android TV left command..."));
}

void AndroidTvManager::navigateRight() {
  queueControlCommand(QStringLiteral("right"),
                      QStringLiteral("Sending Android TV right command..."));
}

void AndroidTvManager::navigateCenter() {
  queueControlCommand(QStringLiteral("center"),
                      QStringLiteral("Sending Android TV select command..."));
}

bool AndroidTvManager::handleVoiceCommand(const QString &spokenText,
                                          QString *feedback) {
  if (!isVoiceEnabled() || !isInstalled())
    return false;

  QString normalized = normalizeVoiceText(spokenText);
  AndroidTvConfig targetConfig = loadConfig();
  int targetTvNumber = -1;
  if (extractTvTargetNumber(&normalized, &targetTvNumber)) {
    const QList<AndroidTvConfig> profiles = loadProfiles();
    if (targetTvNumber <= 0 || targetTvNumber > profiles.size()) {
      const QString error = QStringLiteral("QuickSTT could not find TV %1.")
                                .arg(targetTvNumber);
      setStatus(error);
      emit controlFailed(error);
      if (feedback)
        *feedback = error;
      return true;
    }
    targetConfig = profiles.at(targetTvNumber - 1);
  }

  QString action;
  QJsonObject extraData;

  int targetPercent = -1;
  const QRegularExpression setVolumePattern(
      QStringLiteral("^(?:turn|set|change|adjust|increase|decrease|raise|lower)\\s+(?:the\\s+)?volume\\s+(?:to|two)\\s+([a-z0-9 ]+?)(?:\\s+on\\s+tv)?$"));
  const QRegularExpression shortSetVolumePattern(
      QStringLiteral("^(?:tv\\s+)?volume\\s+(?:to|two)\\s+([a-z0-9 ]+)$"));
  const QRegularExpression volumeByPattern(
      QStringLiteral("^(increase|raise|turn up|decrease|lower|turn down)\\s+(?:the\\s+)?volume(?:\\s+on\\s+tv)?\\s+by\\s+([a-z0-9 ]+)$"));

  const auto matchesAny = [&normalized](std::initializer_list<const char *> phrases) {
    for (const char *phrase : phrases) {
      if (normalized == QLatin1String(phrase))
        return true;
    }
    return false;
  };

  if (captureSpokenPercent(normalized, setVolumePattern, &targetPercent) ||
      captureSpokenPercent(normalized, shortSetVolumePattern, &targetPercent)) {
    action = QStringLiteral("set_volume_percent");
    extraData.insert(QStringLiteral("target_percent"), qBound(0, targetPercent, 100));
  } else {
    const QRegularExpressionMatch volumeByMatch = volumeByPattern.match(normalized);
    if (volumeByMatch.hasMatch()) {
      const QString direction = volumeByMatch.captured(1);
      const int delta = parseSpokenNumberPhrase(volumeByMatch.captured(2));
      if (delta > 0) {
        const int current = currentVolumePercent();
        if (current > 0) {
          const int next =
              direction.startsWith(QStringLiteral("decrease")) ||
                      direction.startsWith(QStringLiteral("lower")) ||
                      direction.startsWith(QStringLiteral("turn down"))
                  ? qMax(1, current - delta)
                  : qMin(100, current + delta);
          action = QStringLiteral("set_volume_percent");
          extraData.insert(QStringLiteral("target_percent"), next);
        } else {
          action = direction.startsWith(QStringLiteral("decrease")) ||
                           direction.startsWith(QStringLiteral("lower")) ||
                           direction.startsWith(QStringLiteral("turn down"))
                       ? QStringLiteral("volume_down")
                       : QStringLiteral("volume_up");
        }
      }
    }
  }

  if (action.isEmpty() &&
      matchesAny({"turn on tv", "turn on the tv", "power on tv",
                  "wake up tv"})) {
    action = QStringLiteral("power_on");
  } else if (action.isEmpty() &&
             matchesAny({"turn off tv", "turn off the tv",
                         "power off tv", "sleep tv"})) {
    action = QStringLiteral("power_off");
  } else if (action.isEmpty() &&
             matchesAny({"volume up tv", "tv volume up",
                         "raise tv volume", "make tv louder",
                         "increase volume on tv", "increase the volume on tv",
                         "raise television volume", "increase volume"})) {
    action = QStringLiteral("volume_up");
  } else if (action.isEmpty() &&
             matchesAny({"volume down tv", "tv volume down",
                         "lower tv volume", "make tv quieter",
                         "decrease volume on tv", "decrease the volume on tv",
                         "lower television volume", "decrease volume"})) {
    action = QStringLiteral("volume_down");
  } else if (action.isEmpty() &&
             matchesAny({"mute tv", "mute the tv", "tv mute",
                         "silence tv"})) {
    action = QStringLiteral("mute");
  } else if (action.isEmpty() &&
             matchesAny({"go home on tv", "home on tv", "tv home",
                         "go to tv home"})) {
    action = QStringLiteral("home");
  } else if (action.isEmpty() &&
             matchesAny({"go back on tv", "back on tv", "tv back",
                         "back on the tv"})) {
    action = QStringLiteral("back");
  } else if (action.isEmpty() &&
             matchesAny({"open tv menu", "tv menu", "show tv menu",
                         "menu on tv"})) {
    action = QStringLiteral("menu");
  } else if (action.isEmpty() &&
             matchesAny({"open tv settings", "tv settings",
                         "show tv settings", "settings on tv"})) {
    action = QStringLiteral("settings");
  } else if (action.isEmpty() &&
             matchesAny({"switch tv input", "tv input", "change tv input",
                         "open tv source"})) {
    action = QStringLiteral("input");
  } else if (action.isEmpty() &&
             matchesAny({"show tv apps", "tv apps", "open tv apps",
                         "show apps on tv"})) {
    action = QStringLiteral("apps");
  } else if (action.isEmpty() &&
             matchesAny({"tv select", "select on tv", "ok on tv",
                         "press ok on tv"})) {
    action = QStringLiteral("center");
  } else if (action.isEmpty() &&
             matchesAny({"tv up", "press up on tv", "move up on tv",
                         "navigate up on tv"})) {
    action = QStringLiteral("up");
  } else if (action.isEmpty() &&
             matchesAny({"tv down", "press down on tv", "move down on tv",
                         "navigate down on tv"})) {
    action = QStringLiteral("down");
  } else if (action.isEmpty() &&
             matchesAny({"tv left", "press left on tv", "move left on tv",
                         "navigate left on tv"})) {
    action = QStringLiteral("left");
  } else if (action.isEmpty() &&
             matchesAny({"tv right", "press right on tv", "move right on tv",
                         "navigate right on tv"})) {
    action = QStringLiteral("right");
  } else if (action.isEmpty() &&
             matchesAny({"play pause on tv", "tv play pause",
                         "pause tv", "resume tv"})) {
    action = QStringLiteral("play_pause");
  } else {
    return false;
  }

  queueControlCommand(action, controlActionMessage(action), extraData, &targetConfig);
  if (feedback)
    *feedback = m_statusText;
  return true;
}
