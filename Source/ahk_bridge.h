#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

class AhkBridge : public QObject {
  Q_OBJECT
public:
  explicit AhkBridge(QObject *parent = nullptr);

  void setPaths(const QString &runtimeExe, const QString &scriptFile);
  bool start();
  void stop();
  bool isRunning() const;

  void dispatchTranscript(int id, const QString &text,
                          bool specialCommandsEnabled);

signals:
  void resultReady(int id, bool commandExecuted, const QString &statusText);
  void bridgeError(const QString &message);

private slots:
  void onReadyReadStdout();
  void onProcessError(QProcess::ProcessError error);
  void onProcessFinished(int exitCode, QProcess::ExitStatus status);

private:
  bool ensureStarted();
  void emitError(const QString &message);

  QProcess *m_proc = nullptr;
  QString m_runtimeExe;
  QString m_scriptFile;
  QByteArray m_stdoutBuffer;
};

