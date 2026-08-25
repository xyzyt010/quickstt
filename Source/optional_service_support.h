#ifndef OPTIONAL_SERVICE_SUPPORT_H
#define OPTIONAL_SERVICE_SUPPORT_H

#include <QString>
#include <QStringList>
#include <QVector>

struct OptionalServicePackageInfo {
  QString id;
  QString displayName;
  QString serverRelativePath;
  QString directUrl;
  QString installSubdir;
  QStringList installMarkers;
  QStringList ownedPaths;
  bool archivePackage = true;
};

struct OptionalServiceDescriptor {
  QString id;
  QString displayName;
  QString shortDescription;
  QString detailsText;
  QString helpText;
  QStringList voiceExamples;
  QStringList packageSequence;
  bool includedInFullBundle = false;
};

QString quickSttServicesRoot();
QString optionalServiceInstallPath(const QString &serviceId);
QString optionalServiceToolPath(const QString &serviceId,
                                const QString &relativePath);

OptionalServiceDescriptor optionalServiceDescriptor(const QString &serviceId);
OptionalServicePackageInfo optionalServicePackage(const QString &packageId);
QVector<OptionalServiceDescriptor> allOptionalServices();

bool isKnownOptionalService(const QString &serviceId);
bool isOptionalServiceInstalled(const QString &serviceId);
QStringList optionalServicePackageSequence(const QString &serviceId);
QString optionalServiceStateText(const QString &serviceId);
QString optionalServiceDetailsText(const QString &serviceId);
QString optionalServiceHelpText(const QString &serviceId);

#endif // OPTIONAL_SERVICE_SUPPORT_H
