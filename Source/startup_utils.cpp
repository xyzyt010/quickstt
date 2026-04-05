#include "startup_utils.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QString>
#include <QTextStream>

namespace {
QString buildStartupCommand(QString exePath, bool background) {
  exePath.replace("/", "\\");
  QString command = "\"" + exePath + "\"";
  if (background)
    command += " --background";
  return command;
}

QString startupFolderPath() {
  const QString appData = qEnvironmentVariable("APPDATA");
  if (appData.isEmpty())
    return QString();
  return QDir(appData).filePath("Microsoft/Windows/Start Menu/Programs/Startup");
}

void applyStartupScript(const QString &exePath, bool background, bool enabled) {
  const QString folder = startupFolderPath();
  if (folder.isEmpty())
    return;

  const QString scriptPath =
      QDir(folder).filePath("QuickSTT_Startup.cmd");
  if (!enabled) {
    QFile::remove(scriptPath);
    return;
  }

  QFile file(scriptPath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return;

  QString exe = exePath;
  exe.replace("/", "\\");
  QTextStream out(&file);
  out << "@echo off\n";
  out << "start \"\" \"" << exe << "\"";
  if (background)
    out << " --background";
  out << "\n";
  file.close();
}
} // namespace

void applyStartupSetting(bool enabled) {
  QSettings config("QuickSTT", "Config");
  bool background = config.value("startupBackground", true).toBool();

  QSettings bootSettings(
      "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
      QSettings::NativeFormat);

  if (enabled) {
    QString appDir = QCoreApplication::applicationDirPath();
    QString loaderPath = QDir(appDir).filePath("QuickSTT.exe");
    QString launchPath = loaderPath;
    if (QFile::exists(loaderPath)) {
      bootSettings.setValue("QuickSTT",
                            buildStartupCommand(loaderPath, background));
    } else {
      launchPath = QCoreApplication::applicationFilePath();
      bootSettings.setValue(
          "QuickSTT",
          buildStartupCommand(QCoreApplication::applicationFilePath(),
                              background));
    }
    applyStartupScript(launchPath, background, true);

    QString serverPath =
        QDir::cleanPath(appDir + "/../QuickSTT_Server/QuickSTT_Server.exe");
    if (!QFile::exists(serverPath)) {
      serverPath = QDir(appDir).filePath("QuickSTT_Server.exe");
    }
    if (QFile::exists(serverPath)) {
      bootSettings.setValue("QuickSTT_Server",
                            buildStartupCommand(serverPath, background));
    }
  } else {
    bootSettings.remove("QuickSTT");
    bootSettings.remove("QuickSTT_App");
    bootSettings.remove("QuickSTT_Server");
    applyStartupScript(QString(), background, false);
  }
}
