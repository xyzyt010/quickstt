#ifndef LOCAL_FRONTEND_STT_MANAGER_H
#define LOCAL_FRONTEND_STT_MANAGER_H

#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QTimer>

class LocalFrontendSttManager : public QObject {
  Q_OBJECT

public:
  explicit LocalFrontendSttManager(QObject *parent = nullptr);

  bool isBusy() const { return m_busy; }
  void transcribeFile(const QString &modelName, const QString &audioPath);
  void stopCrispAsrServer();
  void shutdownAllModels();

signals:
  void statusChanged(const QString &statusText);
  void transcriptionReady(const QString &text);
  void transcriptionFailed(const QString &errorText);

private slots:
  void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
  void onProcessError(QProcess::ProcessError error);
  void onParakeetReadyRead();
  bool isAnyModelActive() const;

private:
  struct PendingTranscriptionJob {
    QString modelName;
    QString audioPath;
  };

  QString executablePathForModel(const QString &modelName) const;
  QString modelRootForModel(const QString &modelName) const;
  QStringList argumentsForModel(const QString &modelName,
                                const QString &audioPath) const;
  QString preprocessAudioIfAvailable(const QString &modelName,
                                     const QString &audioPath,
                                     QString *preprocessDir) const;
  bool shouldRunDeepFilterForModel(const QString &modelName) const;
  QString extractTranscript(const QString &modelName, const QString &audioPath,
                            const QString &stdoutText,
                            const QString &stderrText) const;
  bool startTranscription(const QString &modelName, const QString &audioPath);
  void startNextPendingJob();
  void finishFailure(const QString &errorText);
  void cleanupActiveFiles();
  // Drop a dead worker so the next call respawns cleanly (Task Manager kill).
  void discardDeadParakeetProcess();
  void discardDeadGgmlServerProcess();
  bool ensureParakeetProcessRunning(const QString &modelName);
  bool ensureGgmlServerRunning(const QString &modelName,
                               const QString &modelAbsPath);

  QProcess *m_process = nullptr;
  QProcess *m_parakeetProcess = nullptr;
  QProcess *m_ggmlServerProcess = nullptr;
  QString m_ggmlServerActiveModel;
  QTimer *m_crispAsrIdleTimer = nullptr;
  qint64 m_lastCrispAsrActivityMs = 0;
  QString m_activeModelName;
  QString m_activeAudioPath;
  QString m_activeOriginalAudioPath;
  QString m_activePreprocessDir;
  QByteArray m_stdoutBuffer;
  QByteArray m_stderrBuffer;
  QQueue<PendingTranscriptionJob> m_pendingJobs;
  bool m_busy = false;
};

#endif // LOCAL_FRONTEND_STT_MANAGER_H
