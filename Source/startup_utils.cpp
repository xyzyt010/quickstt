#include "startup_utils.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QString>

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

void removeLegacyStartupScript() {
  const QString folder = startupFolderPath();
  if (folder.isEmpty())
    return;

  QFile::remove(QDir(folder).filePath("QuickSTT_Startup.cmd"));
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
    if (QFile::exists(loaderPath)) {
      bootSettings.setValue("QuickSTT",
                            buildStartupCommand(loaderPath, background));
    } else {
      bootSettings.setValue(
          "QuickSTT",
          buildStartupCommand(QCoreApplication::applicationFilePath(),
                              background));
    }
    removeLegacyStartupScript();

    QString serverPath =
        QDir::cleanPath(appDir + "/../QuickSTT_Server/QuickSTT_Server_App.exe");
    if (!QFile::exists(serverPath)) {
      serverPath = QDir(appDir).filePath("QuickSTT_Server_App.exe");
    }
    if (QFile::exists(serverPath)) {
      bootSettings.setValue("QuickSTT_Server",
                            buildStartupCommand(serverPath, background));
    }
  } else {
    bootSettings.remove("QuickSTT");
    bootSettings.remove("QuickSTT_App");
    bootSettings.remove("QuickSTT_Server");
    removeLegacyStartupScript();
  }
}
