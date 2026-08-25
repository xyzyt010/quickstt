#include "pill_widget.h"
#include "setup_wizard.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
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

using SetAppUserModelIdFn = HRESULT(WINAPI *)(PCWSTR);

QString detectAppDir() {
  wchar_t exePath[MAX_PATH];
  DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  if (len == 0 || len >= MAX_PATH)
    return QDir::currentPath();
  return QFileInfo(QString::fromWCharArray(exePath)).absolutePath();
}

void setExplicitAppUserModelId(const wchar_t *appId) {
  HMODULE shell32 = LoadLibraryW(L"shell32.dll");
  if (!shell32)
    return;
  const auto fn = reinterpret_cast<SetAppUserModelIdFn>(
      GetProcAddress(shell32, "SetCurrentProcessExplicitAppUserModelID"));
  if (fn)
    fn(appId);
  FreeLibrary(shell32);
}

QIcon loadPackagedAppIcon() {
  const QString iconPath = QDir(detectAppDir()).filePath("icon_app.ico");
  return QFileInfo::exists(iconPath) ? QIcon(iconPath) : QIcon();
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

  // Keep startup history for diagnostics
  qInstallMessageHandler(customMessageHandler);

  qDebug() << "--- APP STARTING ---";
  qDebug() << "Working directory set to" << QDir::currentPath();

  QApplication a(argc, argv);
  setExplicitAppUserModelId(L"QuickSTT.App");
  const QIcon appIcon = loadPackagedAppIcon();
  if (!appIcon.isNull())
    a.setWindowIcon(appIcon);
  qDebug() << "QApplication Created";

  // ─── Single Instance Guard (Windows Named Mutex + Socket Verification) ──
  HANDLE hSingleInstanceMutex = CreateMutexW(NULL, TRUE, L"Local\\QuickSTT_App_SingleInstance_v3");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (notifyRunningInstance()) {
      qDebug() << "Another QuickSTT instance is active and responded. Exiting secondary launch.";
      if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
      return 0;
    }
    qDebug() << "Mutex existed but no active instance responded on socket. Overriding stale lock...";
  }
  qDebug() << "Single instance check passed (Windows Mutex acquired).";
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
          QTimer::singleShot(0, widget, [widget]() {
            if (!widget->isAutoShowSuppressed()) {
              widget->restoreFromExternalTrigger();
            } else {
              qDebug() << "External restore suppressed — widget recently closed";
            }
          });
        }
      }
    });
  }

  bool background = true;
  for (int i = 1; i < argc; ++i) {
    if (QString(argv[i]) == "--show") {
      background = false;
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

    if (!background) {
      QTimer::singleShot(50, &w, [&w]() {
        w.centerOnScreen();
        w.show();
        w.raise();
        w.activateWindow();
      });
      qDebug() << "Widget Show Scheduled.";
    } else {
      qDebug() << "Started in background/mini-widget mode.";
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
