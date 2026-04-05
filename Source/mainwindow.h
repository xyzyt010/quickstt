
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
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
#include <QScrollArea>
#include <QSlider>

#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QToolButton>


class CollapsibleSection : public QWidget {
  Q_OBJECT
  Q_PROPERTY(int animationHeight READ animationHeight WRITE setAnimationHeight)
public:
  explicit CollapsibleSection(const QString &title = "",
                              const int animationDuration = 300,
                              QWidget *parent = 0);
  void setContentLayout(QLayout *layout);

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
  void updateHeight() { contentArea->setMaximumHeight(m_animationHeight); }
};

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  void setBackend(QProcess *proc);
  void setLocalModelManager(class LocalModelManager *manager);
  void setSmartLifeManager(class SmartLifeManager *manager);

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
  QTreeWidget *smartLifeDeviceTree = nullptr;
  QLineEdit *smartLifeSearchEdit = nullptr;
  QPlainTextEdit *smartLifeConnectionPanel = nullptr;
  QPlainTextEdit *smartLifeHelpPanel = nullptr;
  QPlainTextEdit *smartLifeSelectionPanel = nullptr;

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

  // --- Updates UI ---
  QPushButton *checkUpdateBtn;
  QCheckBox *autoUpdateCheck;
  QLabel *updateStatusLabel;
  QTimer *updateTimer;
  QListWidget *serverUrlList;
  QLineEdit *newServerUrlEdit;

  QProcess *backend;
  class LocalModelManager *localModelManager = nullptr;
  class SmartLifeManager *smartLifeManager = nullptr;

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
  void persistLocalWidgetSelections(bool emitRefresh = true);
  void persistCloudWidgetSelections(bool emitRefresh = true);
  void syncDashboardSelectionFromSettings();
  void refreshListRowStates(QListWidget *list);
  void handleDashboardListHover(QListWidget *list, QListWidgetItem *item,
                                bool hovering);
  void handleDashboardListClick(QListWidget *list, QListWidgetItem *item);
  void refreshSmartLifeUi();
  void rebuildSmartLifeDeviceTree();
  void refreshSmartLifeSelectionDetails();
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
  QPushButton *removeCloseWordBtn;

  // Helpers
  bool isWakeEngineInstalled(const QString &engine);
  void updateWakeEngineUI();
  void refreshWakeWordSelections(const QString &engine,
                                 bool persistIfAdjusted = true);
  void persistCheckedWakeWords();
  bool m_updatingWakeWordList = false;
  bool m_localSelectionLocked = false;
  bool m_cloudSelectionLocked = false;
  QString m_lockedLocalModelName;
  QString m_lockedCloudProviderId;
  QString m_hoveredLocalModelName;
  QString m_hoveredCloudProviderId;
  qint64 m_lastLocalSelectionClickMs = 0;
  qint64 m_lastCloudSelectionClickMs = 0;
};

#endif // MAINWINDOW_H
