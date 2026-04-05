#ifndef LOCAL_TRANSCRIPTION_MANAGER_H
#define LOCAL_TRANSCRIPTION_MANAGER_H

#include <QObject>
#include <QProcess>

class LocalTranscriptionManager : public QObject {
  Q_OBJECT

public:
  explicit LocalTranscriptionManager(QObject *parent = nullptr);

  bool isBusy() const { return m_busy; }
  void transcribeFile(const QString &modelName, const QString &audioPath);

signals:
  void statusChanged(const QString &statusText);
  void transcriptionReady(const QString &text);
  void transcriptionFailed(const QString &errorText);

private slots:
  void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
  void onReadyReadStandardOutput();
  void onReadyReadStandardError();

private:
  void fail(const QString &errorText);
  void resetState();

  QProcess *m_process = nullptr;
  bool m_busy = false;
  QString m_modelName;
  QString m_audioPath;
  QString m_outputBase;
  QByteArray m_stdoutBuffer;
  QByteArray m_stderrBuffer;
};

#endif // LOCAL_TRANSCRIPTION_MANAGER_H
