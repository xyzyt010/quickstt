#include "ahk_bridge.h"

#include <QCoreApplication>
#include <QFileInfo>

AhkBridge::AhkBridge(QObject *parent) : QObject(parent) {
  m_proc = new QProcess(this);
  m_proc->setProcessChannelMode(QProcess::SeparateChannels);
  connect(m_proc, &QProcess::readyReadStandardOutput, this,
          &AhkBridge::onReadyReadStdout);
  connect(m_proc, &QProcess::errorOccurred, this, &AhkBridge::onProcessError);
  connect(m_proc, &QProcess::finished, this, &AhkBridge::onProcessFinished);
}

void AhkBridge::setPaths(const QString &runtimeExe, const QString &scriptFile) {
  m_runtimeExe = runtimeExe;
  m_scriptFile = scriptFile;
}

bool AhkBridge::start() { return ensureStarted(); }

void AhkBridge::stop() {
  if (!m_proc)
    return;
  if (m_proc->state() != QProcess::NotRunning) {
    m_proc->terminate();
    if (!m_proc->waitForFinished(800))
      m_proc->kill();
  }
}

bool AhkBridge::isRunning() const {
  return m_proc && m_proc->state() == QProcess::Running;
}

bool AhkBridge::ensureStarted() {
  if (isRunning())
    return true;

  if (m_runtimeExe.isEmpty() || m_scriptFile.isEmpty()) {
    emitError("AHK paths not configured");
    return false;
  }

  if (!QFileInfo::exists(m_runtimeExe)) {
    emitError("AutoHotkey64.exe missing");
    return false;
  }
  if (!QFileInfo::exists(m_scriptFile)) {
    emitError("QuickSTT_Commands.ahk missing");
    return false;
  }

  const QString appDir = QCoreApplication::applicationDirPath();
  m_proc->setWorkingDirectory(appDir);
  m_proc->setProgram(m_runtimeExe);
  m_proc->setArguments({m_scriptFile});
  m_proc->start();

  if (!m_proc->waitForStarted(1500)) {
    emitError("Failed to start AutoHotkey bridge");
    return false;
  }
  return true;
}

void AhkBridge::dispatchTranscript(int id, const QString &text,
                                  bool specialCommandsEnabled) {
  if (!ensureStarted())
    return;

  QString sanitized = text;
  sanitized.replace('\r', ' ');
  sanitized.replace('\n', ' ');

  QByteArray line;
  line.append(QByteArray::number(id));
  line.append('\t');
  line.append(specialCommandsEnabled ? '1' : '0');
  line.append('\t');
  line.append(sanitized.toUtf8());
  line.append('\n');

  m_proc->write(line);
  m_proc->waitForBytesWritten(10);
}

void AhkBridge::onReadyReadStdout() {
  m_stdoutBuffer.append(m_proc->readAllStandardOutput());

  while (true) {
    int newlineIdx = m_stdoutBuffer.indexOf('\n');
    if (newlineIdx < 0)
      break;

    QByteArray rawLine = m_stdoutBuffer.left(newlineIdx);
    m_stdoutBuffer.remove(0, newlineIdx + 1);
    rawLine = rawLine.trimmed();
    if (rawLine.isEmpty())
      continue;

    QList<QByteArray> parts = rawLine.split('\t');
    if (parts.size() >= 4 && parts[0] == "RESP") {
      bool ok = false;
      int id = parts[1].toInt(&ok);
      if (!ok)
        continue;
      const QString kind = QString::fromUtf8(parts[2]);
      const bool commandExecuted = (kind == "cmd");
      parts.removeFirst();
      parts.removeFirst();
      parts.removeFirst();
      const QString statusText = QString::fromUtf8(parts.join("\t"));
      emit resultReady(id, commandExecuted, statusText);
    } else if (parts.size() >= 2 && parts[0] == "ERR") {
      parts.removeFirst();
      emitError(QString::fromUtf8(parts.join("\t")));
    }
  }
}

void AhkBridge::onProcessError(QProcess::ProcessError) {
  emitError("AutoHotkey bridge error");
}

void AhkBridge::onProcessFinished(int, QProcess::ExitStatus) {
  emitError("AutoHotkey bridge stopped");
}

void AhkBridge::emitError(const QString &message) {
  emit bridgeError(message);
}

