#include "optional_service_support.h"

#include "local_model_support.h"

#include <QDir>
#include <QFileInfo>

namespace {

QVector<OptionalServiceDescriptor> descriptors() {
  static const QVector<OptionalServiceDescriptor> items = {
      {QStringLiteral("smart_life"),
       QStringLiteral("SmartHome Lights"),
       QStringLiteral("Optional Smart Life and Tuya lighting control."),
       QStringLiteral(
           "Use this optional service to enable the SmartHome lights dashboard, "
           "manual light toggles, and voice commands such as "
           "\"turn on bedroom lights\"."),
       QStringLiteral(
           "1. Install SmartHome lights support in the dashboard.\n"
           "2. Open Dashboard -> SmartHome.\n"
           "3. Enter the Tuya cloud project credentials and, if needed, your "
           "Smart Life account details.\n"
           "4. Connect, sync devices, and then use the manual or voice controls."),
       {QStringLiteral("turn on bedroom lights"),
        QStringLiteral("turn off living room lights")},
       {QStringLiteral("pkg_service_smart_life_marker")},
       true},
      {QStringLiteral("android_tv_remote"),
       QStringLiteral("Android TV Remote"),
       QStringLiteral("Optional Android TV and Google TV LAN control."),
       QStringLiteral(
           "Installs the Android TV Remote v2 runtime so QuickSTT can pair, "
           "connect, power on or off, control volume, and navigate Android TV "
           "or Google TV devices without enabling developer mode."),
       QStringLiteral(
           "1. Install Android TV support in Dashboard -> SmartHome.\n"
           "2. Make sure the TV and this PC are on the same network.\n"
           "3. Let QuickSTT scan, then select the TV from the list.\n"
           "4. Press Pair Selected TV.\n"
           "5. Enter the pairing code shown on the TV.\n"
           "6. QuickSTT connects automatically right after pairing.\n\n"
           "Current voice commands:\n"
           "- turn on tv\n"
           "- turn off tv\n"
           "- volume up tv\n"
           "- volume down tv\n"
           "- mute tv\n"
           "- go home on tv\n"
           "- go back on tv\n"
           "- open tv menu\n"
           "- open tv settings\n"
           "- switch tv input\n"
           "- show tv apps\n"
           "- tv select\n"
           "- tv up / down / left / right\n"
           "- play pause on tv"),
       {QStringLiteral("turn on tv"), QStringLiteral("turn off tv"),
        QStringLiteral("volume up tv"), QStringLiteral("mute tv")},
       {QStringLiteral("pkg_service_android_tv_remote_runtime")},
       true}};
  return items;
}

QVector<OptionalServicePackageInfo> packages() {
  static const QVector<OptionalServicePackageInfo> items = {
      {QStringLiteral("pkg_service_smart_life_marker"),
       QStringLiteral("SmartHome Lights Support Files"),
       QStringLiteral("addons/services/smart_life_enable.zip"),
       QString(),
       QStringLiteral("smart_life"),
       {QStringLiteral("smart_life/installed.json")},
       {QStringLiteral("smart_life")},
       true},
      {QStringLiteral("pkg_service_android_tv_remote_runtime"),
       QStringLiteral("Android TV Remote Runtime"),
       QStringLiteral("addons/services/android_tv_remote_runtime.zip"),
       QString(),
       QStringLiteral("android_tv_remote"),
       {QStringLiteral("android_tv_remote/runtime/python/python.exe"),
        QStringLiteral("android_tv_remote/runtime/quickstt_android_tv_helper.py"),
        QStringLiteral("android_tv_remote/runtime/python/Lib/site-packages/androidtvremote2/__init__.py"),
        QStringLiteral("android_tv_remote/runtime/python/Lib/site-packages/zeroconf/__init__.py"),
        QStringLiteral("android_tv_remote/runtime/python/Lib/site-packages/google/protobuf/__init__.py")},
       {QStringLiteral("android_tv_remote")},
       true}};
  return items;
}

bool packageInstalled(const OptionalServicePackageInfo &package) {
  if (package.id.isEmpty() || package.installMarkers.isEmpty())
    return false;

  const QString root = quickSttServicesRoot();
  for (const QString &marker : package.installMarkers) {
    if (!QFileInfo::exists(QDir(root).filePath(marker)))
      return false;
  }
  return true;
}

} // namespace

QString quickSttServicesRoot() {
  const QString path =
      QDir(quickSttDataRoot()).filePath(QStringLiteral("services"));
  QDir().mkpath(path);
  return path;
}

QString optionalServiceInstallPath(const QString &serviceId) {
  const OptionalServiceDescriptor descriptor = optionalServiceDescriptor(serviceId);
  if (descriptor.id.isEmpty() || descriptor.packageSequence.isEmpty())
    return quickSttServicesRoot();

  const OptionalServicePackageInfo package =
      optionalServicePackage(descriptor.packageSequence.first());
  return package.installSubdir.isEmpty()
             ? quickSttServicesRoot()
             : QDir(quickSttServicesRoot()).filePath(package.installSubdir);
}

QString optionalServiceToolPath(const QString &serviceId,
                                const QString &relativePath) {
  return QDir(optionalServiceInstallPath(serviceId)).filePath(relativePath);
}

OptionalServiceDescriptor optionalServiceDescriptor(const QString &serviceId) {
  const QString key = serviceId.trimmed().toLower();
  for (const OptionalServiceDescriptor &descriptor : descriptors()) {
    if (descriptor.id == key)
      return descriptor;
  }
  return OptionalServiceDescriptor();
}

OptionalServicePackageInfo optionalServicePackage(const QString &packageId) {
  for (const OptionalServicePackageInfo &package : packages()) {
    if (package.id == packageId)
      return package;
  }
  return OptionalServicePackageInfo();
}

QVector<OptionalServiceDescriptor> allOptionalServices() { return descriptors(); }

bool isKnownOptionalService(const QString &serviceId) {
  return !optionalServiceDescriptor(serviceId).id.isEmpty();
}

bool isOptionalServiceInstalled(const QString &serviceId) {
  const OptionalServiceDescriptor descriptor = optionalServiceDescriptor(serviceId);
  if (descriptor.id.isEmpty())
    return false;
  for (const QString &packageId : descriptor.packageSequence) {
    if (!packageInstalled(optionalServicePackage(packageId)))
      return false;
  }
  return true;
}

QStringList optionalServicePackageSequence(const QString &serviceId) {
  QStringList needed;
  const OptionalServiceDescriptor descriptor = optionalServiceDescriptor(serviceId);
  if (descriptor.id.isEmpty())
    return needed;

  for (const QString &packageId : descriptor.packageSequence) {
    if (!packageInstalled(optionalServicePackage(packageId)))
      needed << packageId;
  }
  return needed;
}

QString optionalServiceStateText(const QString &serviceId) {
  const OptionalServiceDescriptor descriptor = optionalServiceDescriptor(serviceId);
  if (descriptor.id.isEmpty())
    return QStringLiteral("Unknown optional service");
  return isOptionalServiceInstalled(serviceId)
             ? QStringLiteral("%1 is installed.").arg(descriptor.displayName)
             : QStringLiteral("%1 is optional and not installed yet.")
                   .arg(descriptor.displayName);
}

QString optionalServiceDetailsText(const QString &serviceId) {
  const OptionalServiceDescriptor descriptor = optionalServiceDescriptor(serviceId);
  return descriptor.id.isEmpty() ? QStringLiteral("Unknown optional service.")
                                 : descriptor.detailsText;
}

QString optionalServiceHelpText(const QString &serviceId) {
  const OptionalServiceDescriptor descriptor = optionalServiceDescriptor(serviceId);
  return descriptor.id.isEmpty() ? QStringLiteral("Unknown optional service.")
                                 : descriptor.helpText;
}
