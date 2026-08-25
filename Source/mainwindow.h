
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QFrame>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QInputDialog>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QParallelAnimationGroup>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QPointer>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSlider>
#include <QVBoxLayout>

#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QToolButton>

namespace fa {
class QtAwesome;
}


class CollapsibleSection : public QWidget {
  Q_OBJECT
  Q_PROPERTY(int animationHeight READ animationHeight WRITE setAnimationHeight)
public:
  explicit CollapsibleSection(const QString &title = "",
                              const int animationDuration = 300,
                              QWidget *parent = 0);
  void setContentLayout(QLayout *layout);
  void setExpanded(bool expanded);
  void refreshExpandedHeight();

  int animationHeight() const { return m_animationHeight; }
  void setAnimationHeight(int h) {
    m_animationHeight = h;
    updateHeight();
  }

public slots:
  void toggle(bool collapsed);

private:
  QToolButton *toggleButton;
  QScrollArea *contentArea;
  QParallelAnimationGroup *toggleAnimation;
  int m_animationHeight;
  bool m_fullyExpanded = false;
  int expandedContentHeight() const;
  void updateHeight() {
    if (toggleButton && toggleButton->isChecked() && m_fullyExpanded) {
      contentArea->setMaximumHeight(16777215);
      contentArea->setMinimumHeight(0);
    } else if (m_animationHeight > 0 && toggleButton && toggleButton->isChecked()) {
      contentArea->setMaximumHeight(16777215);
      contentArea->setMinimumHeight(m_animationHeight);
    } else {
      contentArea->setMaximumHeight(m_animationHeight);
      contentArea->setMinimumHeight(0);
    }
  }
};

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  void setBackend(QProcess *proc);
  void setLocalModelManager(class LocalModelManager *manager);
  void setOptionalServiceManager(class OptionalServiceManager *manager);
  void setSmartLifeManager(class SmartLifeManager *manager);
  void setAndroidTvManager(class AndroidTvManager *manager);
  void setHomeAssistantManager(class HomeAssistantManager *manager);

signals:
  void settingChanged(QString key, QVariant val);

private slots:
  void onRefreshModels();
  void onDownloadClicked();
  void onApplyFavorites();
  void onUninstallClicked();

private:
  QTabWidget *tabs;
  QListWidget *modelList = nullptr;
  QListWidget *cloudModelList = nullptr;
  QLineEdit *modelSearchEdit;
  QPushButton *addModelBtn;
  QPushButton *modelLibraryBtn;
  QPushButton *removeModelBtn;
  QPushButton *downloadBtn;
  QPushButton *applyFavBtn;
  QPushButton *uninstallBtn;
  QCompleter *modelCompleter = nullptr;
  QLabel *widgetLimitLabel = nullptr;
  QStackedWidget *cloudProviderSettingsStack = nullptr;
  QLabel *localModelDetailsLabel = nullptr;
  QComboBox *localModelBackendCombo = nullptr;
  QLabel *localModelBackendStatusLabel = nullptr;
  QLabel *cloudSelectedModelLabel = nullptr;
  QPlainTextEdit *cloudProviderStatusPanel = nullptr;
  QPlainTextEdit *cloudInputSummaryPanel = nullptr;
  QPlainTextEdit *cloudModelDetailsPanel = nullptr;
  QComboBox *cloudLanguageCombo = nullptr;
  QPlainTextEdit *cloudPromptEdit = nullptr;
  QComboBox *smartLifeAccountModeCombo = nullptr;
  QComboBox *smartLifeEndpointCombo = nullptr;
  QStackedWidget *smartLifeCredentialStack = nullptr;
  QLineEdit *smartLifeAccessIdEdit = nullptr;
  QLineEdit *smartLifeAccessKeyEdit = nullptr;
  QLineEdit *smartLifeDeveloperUidEdit = nullptr;
  QPlainTextEdit *smartLifeDeveloperHomeIdsEdit = nullptr;
  QLineEdit *smartLifeUsernameEdit = nullptr;
  QLineEdit *smartLifePasswordEdit = nullptr;
  QLineEdit *smartLifeCountryCodeEdit = nullptr;
  QComboBox *smartLifeSchemaCombo = nullptr;
  QCheckBox *smartLifePasswordMd5Check = nullptr;
  QLabel *smartLifeStatusLabel = nullptr;
  QLabel *smartLifeDeviceSummaryLabel = nullptr;
  QTreeWidget *smartLifeDeviceTree = nullptr;
  QLineEdit *smartLifeSearchEdit = nullptr;
  QScrollArea *smartLifeQuickToggleScrollArea = nullptr;
  QWidget *smartLifeQuickToggleContainer = nullptr;
  QVBoxLayout *smartLifeQuickToggleLayout = nullptr;
  QPlainTextEdit *smartLifeConnectionPanel = nullptr;
  QPlainTextEdit *smartLifeHelpPanel = nullptr;
  QPlainTextEdit *smartLifeSelectionPanel = nullptr;
  QStackedWidget *smartLifeRightPane = nullptr;
  QScrollArea *smartLifeDeviceInspectorScroll = nullptr;
  QWidget *smartLifeDeviceInspectorWidget = nullptr;
  QLabel *smartLifeInstallStateLabel = nullptr;
  QPushButton *smartLifeInstallBtn = nullptr;
  QPushButton *smartLifeUninstallBtn = nullptr;
  QGroupBox *smartLifeConnectionGroupBox = nullptr;
  QGroupBox *smartLifeInfoGroupBox = nullptr;
  QGroupBox *smartLifeDevicesGroupBox = nullptr;
  CollapsibleSection *smartLifeDevicesSection = nullptr;

  // Home Assistant widgets
  QLineEdit *haUrlEdit = nullptr;
  QLineEdit *haTokenEdit = nullptr;
  QPushButton *haConnectBtn = nullptr;
  QPushButton *haDisconnectBtn = nullptr;
  QLabel *haStatusLabel = nullptr;
  QListWidget *haEntityList = nullptr;
  QPlainTextEdit *haInfoPanel = nullptr;
  QComboBox *smartHomeSourceCombo = nullptr;

  QLabel *androidTvInstallStateLabel = nullptr;
  QPushButton *androidTvInstallBtn = nullptr;
  QPushButton *androidTvUninstallBtn = nullptr;
  QPushButton *androidTvScanBtn = nullptr;
  QCheckBox *androidTvAutoScanCheck = nullptr;
  QGroupBox *androidTvDiscoveryGroupBox = nullptr;
  QGroupBox *androidTvProfilesGroupBox = nullptr;
  QPushButton *androidTvStartPairBtn = nullptr;
  QPushButton *androidTvDisconnectBtn = nullptr;
  QListWidget *androidTvDiscoveryList = nullptr;
  QLabel *androidTvDiscoveryStatusLabel = nullptr;
  QListWidget *androidTvProfileList = nullptr;
  QLineEdit *androidTvProfileNameEdit = nullptr;
  QPushButton *androidTvNewProfileBtn = nullptr;
  QPushButton *androidTvSaveProfileBtn = nullptr;
  QPushButton *androidTvDeleteProfileBtn = nullptr;
  QLineEdit *androidTvHostEdit = nullptr;
  QLineEdit *androidTvPortEdit = nullptr;
  QLineEdit *androidTvPairHostEdit = nullptr;
  QLineEdit *androidTvPairPortEdit = nullptr;
  QLineEdit *androidTvPairCodeEdit = nullptr;
  QLineEdit *androidTvFriendlyNameEdit = nullptr;
  QCheckBox *androidTvVoiceEnabledCheck = nullptr;
  QLabel *androidTvStatusLabel = nullptr;
  QPlainTextEdit *androidTvSummaryPanel = nullptr;
  QPlainTextEdit *androidTvHelpPanel = nullptr;
  QPlainTextEdit *androidTvRemoteStatusPanel = nullptr;
  QGroupBox *androidTvSetupGroupBox = nullptr;
  QGroupBox *androidTvControlsGroupBox = nullptr;
  QToolButton *androidTvPowerBtn = nullptr;
  QToolButton *androidTvMuteBtn = nullptr;
  QToolButton *androidTvInputBtn = nullptr;
  QToolButton *androidTvAppsBtn = nullptr;
  QToolButton *androidTvMenuBtn = nullptr;
  QToolButton *androidTvSettingsBtn = nullptr;
  QToolButton *androidTvUpBtn = nullptr;
  QToolButton *androidTvLeftBtn = nullptr;
  QToolButton *androidTvOkBtn = nullptr;
  QToolButton *androidTvRightBtn = nullptr;
  QToolButton *androidTvDownBtn = nullptr;
  QToolButton *androidTvHomeBtn = nullptr;
  QToolButton *androidTvBackBtn = nullptr;
  QToolButton *androidTvPlayPauseBtn = nullptr;
  QToolButton *androidTvVolumeDownBtn = nullptr;
  QToolButton *androidTvVolumeUpBtn = nullptr;
  QSlider *androidTvVolumeSlider = nullptr;
  QLineEdit *androidTvVolumeValueEdit = nullptr;
  QTimer *androidTvVolumeCommitTimer = nullptr;
  QTimer *androidTvVolumeDisplayHoldTimer = nullptr;

  QSlider *rSlider, *oSlider, *iconSizeSlider, *txtOpSlider, *txtSizeSlider;
  QLineEdit *rEdit, *oEdit, *iconSizeEdit, *txtOpEdit, *txtSizeEdit;

  QSlider *trayIconSizeSlider;
  QLineEdit *trayIconSizeEdit;

  QCheckBox *startupCheck;
  QCheckBox *startupBackgroundCheck;
  QCheckBox *specialCommandsCheck;
  QCheckBox *hapticsCheck;
  QCheckBox *soundCheck;
  QCheckBox *widgetFlexibleCheck;
  QComboBox *wakeWordModeCombo;

  // --- Updates UI ---
  QPushButton *checkUpdateBtn;
  QCheckBox *autoUpdateCheck;
  QLabel *updateStatusLabel;
  QTimer *updateTimer;
  QListWidget *serverUrlList;
  QLineEdit *newServerUrlEdit;

  QProcess *backend;
  class LocalModelManager *localModelManager = nullptr;
  class OptionalServiceManager *optionalServiceManager = nullptr;
  class SmartLifeManager *smartLifeManager = nullptr;
  class AndroidTvManager *androidTvManager = nullptr;
  class HomeAssistantManager *homeAssistantManager = nullptr;
  fa::QtAwesome *qtAwesome = nullptr;

  void setupPair(QSlider *slider, QLineEdit *edit, QString key, int min, int max,
                 int def);
  void setupUpdatesTab();
  void setupWakewordTab();
  void reloadLocalModelCatalog();
  void addModelToDashboardList(const QString &modelName, bool widgetChecked);
  void addCloudModelToDashboardList(const QString &modelName, bool widgetChecked);
  void refreshDashboardModelItem(QListWidgetItem *item);
  void refreshCloudDashboardItem(QListWidgetItem *item);
  void refreshDashboardModelStatuses();
  void refreshSelectionDetails();
  void refreshLocalModelBackendUi();
  void persistLocalWidgetSelections(bool emitRefresh = true);
  void persistCloudWidgetSelections(bool emitRefresh = true);
  void syncDashboardSelectionFromSettings();
  void refreshListRowStates(QListWidget *list);
  void handleDashboardListHover(QListWidget *list, QListWidgetItem *item,
                                bool hovering);
  void handleDashboardListClick(QListWidget *list, QListWidgetItem *item);
  void refreshSmartLifeUi();
  void refreshAndroidTvUi();
  void refreshAndroidTvProfileList(const QString &selectedProfileId = QString());
  void refreshAndroidTvDiscoveryList();
  void reconcileRememberedAndroidTvProfilesWithDiscovery();
  void applyDiscoveredAndroidTvDevice(const QJsonObject &device);
  void selectAndroidTvProfile(const QString &profileId, bool updateEditors = true);
  void createNewAndroidTvProfile();
  void saveCurrentAndroidTvProfile();
  void deleteCurrentAndroidTvProfile();
  void persistActiveAndroidTvSettings();
  void rememberCurrentAndroidTvProfileIfNeeded();
  void attemptAutoReconnectSmartHome();
  void attemptAutoReconnectAndroidTv();
  QJsonArray loadAndroidTvProfiles() const;
  void saveAndroidTvProfiles(const QJsonArray &profiles);
  QJsonObject androidTvProfileFromEditors(const QString &profileId = QString()) const;
  void applyAndroidTvProfileToEditors(const QJsonObject &profile);
  void clearAndroidTvProfileEditors();
  QString androidTvSelectedProfileId() const;
  void rebuildSmartLifeDeviceTree();
  void rebuildSmartLifeQuickToggleList();
  void refreshSmartLifeSelectionDetails();
  void refreshSmartLifeDeviceInspector();
  void refreshSmartLifeTreeDeviceRows();
  QStringList selectedSmartLifeDeviceIds() const;
  void applySmartLifeSearchFilter();
  void updateWidgetLimitLabel();
  QString selectedModelName(QListWidgetItem *item) const;
  int checkedWidgetModelCount() const;
  QStringList getServerUrls();
  void checkForUpdates();
  void tryNextServer(int index, const QStringList &urls,
                     const QString &localVersion);

  // --- General UI ---
  QLineEdit *recPathEdit;
  QCheckBox *lrcCheck;

  // --- Wakeword / Close Word UI ---
  QComboBox *wakeEngineCombo;
  QLabel *wakeEngineStatusLabel;
  QPushButton *wakeEngineInstallBtn;
  QPushButton *wakeEngineUninstallBtn;
  QStackedWidget *wakeEngineSettingsStack;

  // Porcupine-specific
  QLineEdit *porcupineKeyEdit;

  // Common wake/close word lists
  QListWidget *wakeWordList;
  QLabel *wakeWordHintLabel;

  QListWidget *closeWordList;
  QLineEdit *newCloseWordEdit;
  QPushButton *addCloseWordBtn;

  // Helpers
  bool isWakeEngineInstalled(const QString &engine);
  void updateWakeEngineUI();
  void refreshWakeWordSelections(const QString &engine,
                                 bool persistIfAdjusted = true);
  void persistCheckedWakeWords();
  void addCloseWordRow(const QString &word);
  void persistCloseWords();
  bool m_updatingWakeWordList = false;
  bool m_localSelectionLocked = false;
  bool m_cloudSelectionLocked = false;
  QString m_lockedLocalModelName;
  QString m_lockedCloudProviderId;
  QJsonArray m_androidTvDiscoveredDevices;
  QJsonArray m_androidTvVisibleDiscoveredDevices;
  QString m_activeAndroidTvProfileId;
  QString m_hoveredLocalModelName;
  QString m_hoveredCloudProviderId;
  qint64 m_lastLocalSelectionClickMs = 0;
  bool m_smartHomeAutoReconnectAttempted = false;
  bool m_androidTvAutoReconnectAttempted = false;
  qint64 m_lastCloudSelectionClickMs = 0;
  bool m_updatingAndroidTvProfileUi = false;
  bool m_androidTvInitialScanDone = false;
  bool m_updatingAndroidTvVolumeSlider = false;
  int m_pendingAndroidTvVolumePercent = -1;
  int m_androidTvTargetVolumePercent = -1;
  QString m_androidTvServiceMessage;
  QPointer<QInputDialog> m_androidTvPairingDialog;
};

#endif // MAINWINDOW_H
