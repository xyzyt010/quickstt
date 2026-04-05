#include "pill_widget.h"
#include "setup_wizard.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QMetaObject>
#include <QSettings>
#include <QSharedMemory>
#include <QTextStream>
#include <windows.h>

namespace {
QString g_logPath = "startup_log.txt";
constexpr auto kSingleInstanceKey = "QuickSTT_App_SingleInstance_v2";
constexpr auto kActivationServerName = "QuickSTT_App_Activation_v2";

QString detectAppDir() {
  wchar_t exePath[MAX_PATH];
  DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  if (len == 0 || len >= MAX_PATH)
    return QDir::currentPath();
  return QFileInfo(QString::fromWCharArray(exePath)).absolutePath();
}

bool notifyRunningInstance() {
  QLocalSocket socket;
  socket.connectToServer(QString::fromLatin1(kActivationServerName),
                         QIODevice::WriteOnly);
  if (!socket.waitForConnected(800))
    return false;
  socket.write("SHOW\n");
  socket.flush();
  socket.waitForBytesWritten(800);
  socket.disconnectFromServer();
  return true;
}
} // namespace

void customMessageHandler(QtMsgType type, const QMessageLogContext &context,
                          const QString &msg) {
  QFile outFile(g_logPath);
  if (!outFile.open(QIODevice::WriteOnly | QIODevice::Append))
    return;
  QTextStream ts(&outFile);
  ts << QDateTime::currentDateTime().toString("HH:mm:ss.zzz") << " - " << msg
     << Qt::endl;
}

int main(int argc, char *argv[]) {
  QString appDir = detectAppDir();
  QDir::setCurrent(appDir);
  g_logPath = QDir(appDir).filePath("startup_log.txt");

  // Clear old log
  QFile(g_logPath).remove();
  qInstallMessageHandler(customMessageHandler);

  qDebug() << "--- APP STARTING ---";
  qDebug() << "Working directory set to" << QDir::currentPath();

  QApplication a(argc, argv);
  qDebug() << "QApplication Created";

  // ─── Single Instance Guard (QSharedMemory) ──────────────────────────────
  // QSharedMemory is cross-platform and automatically cleaned up when
  // the owning process exits, even on a crash (OS reclaims resources).
  // Unlike mutexes, shared memory segments are always released by the OS.
  static QSharedMemory singleInstanceGuard(QString::fromLatin1(kSingleInstanceKey));

  // On some systems (e.g. after a crash on Linux), shared memory can persist.
  // Try to attach first to clean up any stale segment, then detach.
  if (singleInstanceGuard.attach()) {
    singleInstanceGuard.detach();
  }

  if (!singleInstanceGuard.create(1)) {
    // Another instance owns this shared memory segment
    qDebug() << "Another QuickSTT instance is already running. Exiting."
             << "(" << singleInstanceGuard.errorString() << ")";
    if (notifyRunningInstance()) {
      qDebug() << "Running instance notified to restore itself.";
    } else {
      qDebug() << "Failed to notify the running instance.";
    }
    return 0;
  }
  qDebug() << "Single instance check passed (shared memory created).";
  // ─────────────────────────────────────────────────────────────────────────

  a.setQuitOnLastWindowClosed(false);
  qDebug() << "QuitOnLastWindowClosed Set";

  PillWidget *widget = nullptr;
  QLocalServer activationServer;
  QLocalServer::removeServer(QString::fromLatin1(kActivationServerName));
  if (!activationServer.listen(QString::fromLatin1(kActivationServerName))) {
    qDebug() << "Activation server listen failed:" << activationServer.errorString();
  } else {
    QObject::connect(&activationServer, &QLocalServer::newConnection, &a, [&]() {
      while (activationServer.hasPendingConnections()) {
        QLocalSocket *socket = activationServer.nextPendingConnection();
        if (!socket)
          continue;
        socket->readAll();
        socket->disconnectFromServer();
        socket->deleteLater();
        qDebug() << "Received external restore trigger.";
        if (widget) {
          QMetaObject::invokeMethod(widget, "restoreFromExternalTrigger",
                                    Qt::QueuedConnection);
        }
      }
    });
  }

  bool background = false;
  for (int i = 1; i < argc; ++i) {
    if (QString(argv[i]) == "--background") {
      background = true;
      break;
    }
  }

  bool setupShown = false;
  QSettings settings("QuickSTT", "Config");
  if (settings.value("firstLaunch", true).toBool() &&
      !settings.value("setupCompleted", false).toBool()) {
    qDebug() << "Launching first-run setup wizard...";
    SetupWizard wizard;
    if (wizard.exec() != QDialog::Accepted) {
      qDebug() << "Setup wizard cancelled. Exiting.";
      return 0;
    }
    wizard.applySettings();
    setupShown = true;
    background = false;
  }

  try {
    qDebug() << "Initializing PillWidget...";
    PillWidget w;
    widget = &w;
    qDebug() << "PillWidget Constructor Finished.";

    if (!background || setupShown) {
      w.show();
      qDebug() << "Widget Show Called.";
    } else {
      qDebug() << "Started in background mode.";
    }

    qDebug() << "Entering Event Loop...";
    return a.exec();
  } catch (const std::exception &e) {
    qDebug() << "FATAL EXCEPTION: " << e.what();
    return -1;
  } catch (...) {
    qDebug() << "UNKNOWN CRASH DETECTED";
    return -1;
  }
}
