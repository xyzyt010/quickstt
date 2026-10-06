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
#include <QProcess>
#include <QSettings>
#include <QSharedMemory>
#include <QStandardPaths>
#include <QTextStream>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QString g_logPath = "startup_log.txt";
constexpr auto kSingleInstanceKey = "QuickSTT_App_SingleInstance_v2";
constexpr auto kActivationServerName = "QuickSTT_App_Activation_v2";

#if defined(Q_OS_WIN) && !defined(_WIN32)
#error "Q_OS_WIN without _WIN32"
#endif

#ifdef _WIN32
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
#else
// Must NOT use QCoreApplication::applicationDirPath() here — main() resolves
// the app dir before the QApplication object exists.
#include <limits.h>
#include <unistd.h>

QString detectAppDir() {
  char exePath[PATH_MAX];
  const ssize_t len = ::readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
  if (len <= 0)
    return QDir::currentPath();
  exePath[len] = '\0';
  return QFileInfo(QString::fromLocal8Bit(exePath)).absolutePath();
}
#endif // _WIN32

QIcon loadPackagedAppIcon() {
  const QString dir = detectAppDir();
  for (const char *name : {"icon_app.png", "icon_app.ico", "app_icon.svg"})
    if (QFileInfo::exists(QDir(dir).filePath(QLatin1String(name))))
      return QIcon(QDir(dir).filePath(QLatin1String(name)));

  // Installed deb layout: freedesktop hicolor icon shipped by the package.
  for (const char *path :
       {"/usr/share/icons/hicolor/256x256/apps/quickstt.png",
        "/usr/share/pixmaps/quickstt.png"})
    if (QFileInfo::exists(QLatin1String(path)))
      return QIcon(QLatin1String(path));

  // Embedded resource icon — always available regardless of install layout.
  {
    QIcon resourceIcon(QStringLiteral(":/quickstt/app.svg"));
    if (!resourceIcon.isNull())
      return resourceIcon;
  }
  return QIcon();
}

bool notifyRunningInstance(const QByteArray &message = QByteArray("SHOW\n")) {
  QLocalSocket socket;
  socket.connectToServer(QString::fromLatin1(kActivationServerName),
                         QIODevice::WriteOnly);
  if (!socket.waitForConnected(800))
    return false;
  socket.write(message);
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

#ifdef _WIN32
  // Fast secondary path: every tray click / SHOW forward spawns this process.
  // A full QApplication init (platform plugin, fonts) costs ~1s per click —
  // probe the single-instance mutex with raw Win32 first and forward through
  // a GUI-less QCoreApplication (~100ms) instead.
  // Minimal flag scan (no Qt objects needed) for the forward message.
  bool probeDashboard = false, probeHide = false, probeToggle = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i] ? argv[i] : "";
    if (a == "--dashboard")
      probeDashboard = true;
    else if (a == "--hide")
      probeHide = true;
    else if (a == "--toggle-dictation")
      probeToggle = true;
  }
  const bool forcePrimary = qEnvironmentVariableIsSet("QUICKSTT_FORCE_PRIMARY");
  HANDLE hProbe =
      CreateMutexW(NULL, FALSE, L"Local\\QuickSTT_App_SingleInstance_v3");
  const bool alreadyRunning =
      (hProbe && GetLastError() == ERROR_ALREADY_EXISTS);
  if (hProbe)
    CloseHandle(hProbe);
  if (alreadyRunning && !forcePrimary) {
    QCoreApplication fwd(argc, argv);
    const QByteArray fwdMsg = probeDashboard  ? QByteArray("DASHBOARD\n")
                              : probeHide     ? QByteArray("HIDE\n")
                              : probeToggle   ? QByteArray("TOGGLE\n")
                                              : QByteArray("SHOW\n");
    if (notifyRunningInstance(fwdMsg)) {
      qDebug() << "Forwarded to running instance via fast path. Exiting.";
      return 0;
    }
    // Stale lock (mutex exists, nobody listens): relaunch as primary. A
    // QCoreApplication already exists so we cannot upgrade to QApplication
    // in-process — re-exec with a bypass flag instead (one extra hop, no loop:
    // the flag skips this probe). Env var is inherited by the child.
    qDebug() << "Stale lock, relaunching as primary...";
    QStringList fwdArgs;
    for (int i = 1; i < argc; ++i)
      fwdArgs << QString::fromLocal8Bit(argv[i]);
#ifdef _WIN32
    SetEnvironmentVariableW(L"QUICKSTT_FORCE_PRIMARY", L"1");
#endif
    if (QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                fwdArgs)) {
      return 0;
    }
    qDebug() << "Relaunch failed; continuing as primary in this process.";
  }
#endif

  QApplication a(argc, argv);

  // System install dirs (/usr/lib/quickstt) are read-only on Linux — keep
  // diagnostics in the per-user data root. Resolved after QApplication exists
  // so QStandardPaths returns the proper org/app paths.
  if (!QFileInfo(appDir).isWritable()) {
    const QString userLogDir = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (!userLogDir.isEmpty()) {
      QDir().mkpath(userLogDir);
      g_logPath = QDir(userLogDir).filePath("startup_log.txt");
    }
  }
#ifdef _WIN32
  setExplicitAppUserModelId(L"QuickSTT.App");
#endif
  const QIcon appIcon = loadPackagedAppIcon();
  if (!appIcon.isNull())
    a.setWindowIcon(appIcon);
  qDebug() << "QApplication Created";

  // ─── Command line (parsed before the single-instance guard so secondary
  // launches can forward the request to the running instance) ──────────────
  bool background = true;
  bool toggleDictation = false;
  bool noTray = false;
  bool dashboard = false;
  bool hideOnly = false;
  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == "--show") {
      background = false;
      continue;
    }
    if (arg == "--no-tray") {
      // Single-tray mode: the Slint app owns the only tray icon and
      // manages this process (show via activation server, quit via kill).
      noTray = true;
      continue;
    }
    if (arg == "--toggle-dictation")
      toggleDictation = true; // Wayland compositor-shortcut entry point
    if (arg == "--dashboard") {
      // Open the full Qt dashboard (Slint tray/menu entry point).
      dashboard = true;
      continue;
    }
    if (arg == "--hide") {
      // Hide the main C++ Qt widget (Slint tray/menu entry point).
      hideOnly = true;
      continue;
    }
  }
  if (noTray)
    qDebug() << "Single-tray mode requested (--no-tray)";

  // ─── Single Instance Guard (Named Mutex on Windows + Socket Verification) ──
#ifdef _WIN32
  HANDLE hSingleInstanceMutex = CreateMutexW(NULL, TRUE, L"Local\\QuickSTT_App_SingleInstance_v3");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    const QByteArray fwdMsg = dashboard ? QByteArray("DASHBOARD\n")
                              : hideOnly ? QByteArray("HIDE\n")
                                         : QByteArray("SHOW\n");
    if (notifyRunningInstance(fwdMsg)) {
      qDebug() << "Another QuickSTT instance is active and responded. Exiting secondary launch.";
      if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
      return 0;
    }
    qDebug() << "Mutex existed but no active instance responded on socket. Overriding stale lock...";
  }
  qDebug() << "Single instance check passed (Windows Mutex acquired).";
#else
  if (notifyRunningInstance(dashboard      ? QByteArray("DASHBOARD\n")
                            : hideOnly     ? QByteArray("HIDE\n")
                            : toggleDictation ? QByteArray("TOGGLE\n")
                                              : QByteArray("SHOW\n"))) {
    qDebug() << "Another QuickSTT instance is active and responded. Exiting secondary launch.";
    return 0;
  }
#endif
  // --hide with no running instance: nothing to hide, exit quietly instead
  // of starting a whole new widget process.
  if (hideOnly) {
#ifdef _WIN32
    if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
#endif
    return 0;
  }
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
        // The client's write routinely hasn't arrived at newConnection time —
        // readAll() here used to return "" and every forward (SHOW, HIDE,
        // DASHBOARD) collapsed into "restore widget" (a tray Dashboard click
        // just flashed the widget instead of opening the dashboard).
        if (!socket->waitForReadyRead(1500)) {
          qDebug() << "Activation message never arrived; ignoring.";
          socket->disconnectFromServer();
          socket->deleteLater();
          continue;
        }
        const QByteArray message = socket->readAll().trimmed();
        socket->disconnectFromServer();
        socket->deleteLater();
        qDebug() << "Received external trigger:" << message;
        if (!widget || message.isEmpty())
          continue;
        if (message == QByteArray("DASHBOARD")) {
          QTimer::singleShot(0, widget, [widget]() {
            widget->openDashboard();
          });
        } else if (message == QByteArray("HIDE")) {
          QTimer::singleShot(0, widget, [widget]() {
            widget->hideFromExternalTrigger();
          });
        } else if (message == QByteArray("TOGGLE")) {
          QTimer::singleShot(0, widget, [widget]() {
            widget->toggleDictationExternal();
          });
        } else {
          // SHOW is explicit user intent (tray click / second launch): it
          // must surface the widget even inside the transient close-
          // suppression window (that gate is for ambient/event triggers).
          QTimer::singleShot(0, widget, [widget]() {
            widget->showMainWidgetExplicitly();
          });
        }
      }
    });
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
    PillWidget w(noTray);
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
    } else if (toggleDictation) {
      QTimer::singleShot(400, &w, &PillWidget::toggleDictationExternal);
    } else {
      qDebug() << "Started in background/mini-widget mode.";
    }
    if (dashboard) {
      QTimer::singleShot(400, &w, [&w]() { w.openDashboard(); });
      qDebug() << "Dashboard Show Scheduled.";
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
