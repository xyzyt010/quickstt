#include "pill_widget.h"
#include "ahk_bridge.h"
#include "local_model_support.h"
#include "startup_utils.h"
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <vector>
#include <windows.h> // SendInput
#include <windows.h> // For SendInput native typing

namespace {
constexpr int kMinWaveformBarCount = 24;
constexpr int kMaxWaveformBarCount = 140;
constexpr qreal kWaveformBaselineWidth = 220.0;
constexpr qreal kWaveformBaselineHeight = 28.0;

int waveformHistoryLimitForWidth(int width) {
  return qBound(kMinWaveformBarCount, int(width / 5.9), kMaxWaveformBarCount);
}

qreal waveformWidthScale(int width) {
  return qBound<qreal>(0.85, qreal(width) / kWaveformBaselineWidth, 1.9);
}

qreal waveformHeightScale(int height) {
  return qBound<qreal>(0.85, qreal(height) / kWaveformBaselineHeight, 2.6);
}

struct WaveformLayout {
  int slotCount = 0;
  qreal startX = 0.0;
  qreal slotStep = 0.0;
  qreal lineWidth = 0.0;
  qreal dotSize = 0.0;
  qreal minLineHeight = 0.0;
  qreal maxLineHeight = 0.0;
  qreal dotThreshold = 0.0;
  qreal lowLineThreshold = 0.0;
};

WaveformLayout buildWaveformLayout(const QRectF &rect) {
  WaveformLayout layout;
  const qreal widthScale = waveformWidthScale(int(rect.width()));
  const qreal heightScale = waveformHeightScale(int(rect.height()));
  const qreal baseLineWidth = rect.width() >= 340.0 ? 1.30 : 1.08;
  const qreal targetStep =
      (rect.width() >= 340.0 ? 7.0 : 6.15 + ((widthScale - 1.0) * 0.50)) +
      (baseLineWidth * 0.40);

  layout.slotCount = qMax(10, int((rect.width() + targetStep) / targetStep));
  layout.lineWidth = baseLineWidth * 1.40;
  layout.dotSize =
      qBound<qreal>(1.33, (layout.lineWidth + 0.08) * 1.40,
                    qMax<qreal>(1.82, rect.height() / 28.0));
  const qreal inset = qMax(layout.dotSize, layout.lineWidth) / 2.0;
  layout.startX = rect.left() + inset;
  const qreal endX = rect.right() - inset;
  layout.minLineHeight =
      qMax<qreal>(layout.dotSize * 1.55, rect.height() / 26.0);
  layout.maxLineHeight = qMax<qreal>(14.0, rect.height() * 0.997);
  layout.dotThreshold =
      qBound<qreal>(0.020,
                    0.027 - ((heightScale - 1.0) * 0.003) -
                        ((widthScale - 1.0) * 0.0015),
                    0.031);
  layout.lowLineThreshold = layout.dotThreshold + 0.065;
  layout.slotStep =
      layout.slotCount > 1 ? (endX - layout.startX) / qreal(layout.slotCount - 1)
                           : 0.0;
  return layout;
}

float sampleWaveformLevel(const QList<float> &samples, int slotIndex,
                          int slotCount) {
  const int totalSamples = samples.size();
  if (totalSamples <= 0)
    return 0.0f;
  if (slotCount <= 1)
    return qBound(0.0f, samples.last(), 1.0f);

  const float pos =
      (float(slotIndex) / float(slotCount - 1)) * float(totalSamples - 1);
  const int leftIndex = qBound(0, int(std::floor(pos)), totalSamples - 1);
  const int rightIndex = qMin(totalSamples - 1, leftIndex + 1);
  const float mix = pos - float(leftIndex);
  const float level =
      samples[leftIndex] * (1.0f - mix) + samples[rightIndex] * mix;
  return qBound(0.0f, level, 1.0f);
}

QString normalizeSpecialCommandText(QString text) {
  text = text.toLower().trimmed();
  for (QChar &ch : text) {
    if (!ch.isLetterOrNumber())
      ch = ' ';
  }
  return text.simplified();
}

int functionKeyNumberFromToken(const QString &token) {
  bool ok = false;
  const int numeric = token.toInt(&ok);
  if (ok && numeric >= 1 && numeric <= 24)
    return numeric;

  if (token == "one")
    return 1;
  if (token == "two")
    return 2;
  if (token == "three")
    return 3;
  if (token == "four")
    return 4;
  if (token == "five")
    return 5;
  if (token == "six")
    return 6;
  if (token == "seven")
    return 7;
  if (token == "eight")
    return 8;
  if (token == "nine")
    return 9;
  if (token == "ten")
    return 10;
  if (token == "eleven")
    return 11;
  if (token == "twelve")
    return 12;
  if (token == "thirteen")
    return 13;
  if (token == "fourteen")
    return 14;
  if (token == "fifteen")
    return 15;
  if (token == "sixteen")
    return 16;
  if (token == "seventeen")
    return 17;
  if (token == "eighteen")
    return 18;
  if (token == "nineteen")
    return 19;
  if (token == "twenty")
    return 20;
  if (token == "twenty one")
    return 21;
  if (token == "twenty two")
    return 22;
  if (token == "twenty three")
    return 23;
  if (token == "twenty four")
    return 24;
  return 0;
}

bool tryResolveFunctionKeyCommand(const QString &normalized, WORD *vkey,
                                  QString *commandName) {
  QString token = normalized;
  if (token.startsWith("function key "))
    token = token.mid(QStringLiteral("function key ").size());
  else if (token.startsWith("function "))
    token = token.mid(QStringLiteral("function ").size());
  else if (token.startsWith("f key "))
    token = token.mid(QStringLiteral("f key ").size());
  else if (token.startsWith("f ")) {
    token = token.mid(2);
  } else if (token.startsWith('f') && token.size() > 1 &&
             token[1].isDigit()) {
    token = token.mid(1);
  } else {
    return false;
  }

  token = token.simplified();
  const int functionNumber = functionKeyNumberFromToken(token);
  if (functionNumber < 1 || functionNumber > 24)
    return false;

  *vkey = WORD(VK_F1 + (functionNumber - 1));
  *commandName = QStringLiteral("F%1").arg(functionNumber);
  return true;
}

bool tryResolveSpecialCommand(const QString &spokenText, WORD *vkey,
                              QString *commandName) {
  const QString norm = normalizeSpecialCommandText(spokenText);
  if (norm.isEmpty())
    return false;

  if (norm == "space" || norm == "spacebar" || norm == "space bar") {
    *vkey = VK_SPACE;
    *commandName = "Space";
    return true;
  }
  if (norm == "back space" || norm == "backspace") {
    *vkey = VK_BACK;
    *commandName = "Backspace";
    return true;
  }
  if (norm == "enter" || norm == "return" || norm == "new line") {
    *vkey = VK_RETURN;
    *commandName = "Enter";
    return true;
  }
  if (norm == "escape" || norm == "esc") {
    *vkey = VK_ESCAPE;
    *commandName = "Escape";
    return true;
  }
  if (norm == "windows" || norm == "win") {
    *vkey = VK_LWIN;
    *commandName = "Windows";
    return true;
  }
  if (norm == "tab") {
    *vkey = VK_TAB;
    *commandName = "Tab";
    return true;
  }
  if (norm == "control" || norm == "ctrl") {
    *vkey = VK_CONTROL;
    *commandName = "Control";
    return true;
  }
  if (norm == "alt" || norm == "menu" || norm == "alternate" ||
      norm == "alternative") {
    *vkey = VK_MENU;
    *commandName = "Alt";
    return true;
  }
  if (norm == "shift") {
    *vkey = VK_SHIFT;
    *commandName = "Shift";
    return true;
  }
  return tryResolveFunctionKeyCommand(norm, vkey, commandName);
}

QString normalizeModelName(const QString &text) {
  int suffixPos = text.indexOf(" [");
  const QString trimmed =
      suffixPos > 0 ? text.left(suffixPos).trimmed() : text.trimmed();
  const QString cloudModel = normalizeCloudModelSelection(trimmed);
  return isCloudModel(cloudModel) ? cloudModel : trimmed;
}

bool isWhisperModel(const QString &modelName) {
  return isWhisperLocalModel(modelName);
}

bool supportsRuntimeModel(const QString &modelName) {
  return localModelSupportsRuntimeNow(modelName);
}

bool supportsDirectDownload(const QString &modelName) {
  return localModelSupportsDirectDownload(modelName);
}

QString buildModelDisplayText(const QString &modelName, bool) {
  if (isCloudModel(modelName))
    return cloudModelWidgetLabel(modelName);
  return modelName.trimmed();
}

QString buildModelTooltip(const QString &modelName, bool installed) {
  if (isCloudModel(modelName))
    return cloudModelTooltip(modelName);
  return localModelTooltip(modelName, installed);
}

QStringList allComboModels() {
  QSettings settings("QuickSTT", "Config");
  const QVector<ComputeTargetInfo> targets = detectComputeTargets();
  QString targetId = settings.value("computeTargetId").toString();
  if (targetId.isEmpty())
    targetId = defaultComputeTargetId(targets);
  const ComputeTargetInfo target = computeTargetById(targets, targetId);
  QStringList names;
  for (const QString &modelName : localDashboardCatalogNames(target)) {
    if (localModelWidgetSelectable(modelName))
      names << modelName;
  }
  return names;
}

QString canonicalWakeEngineLabel(const QString &value) {
  const QString trimmed = value.trimmed();
  if (trimmed.contains("Porcupine", Qt::CaseInsensitive))
    return "Porcupine (Access Key Required)";
  if (trimmed.contains("Vosk", Qt::CaseInsensitive))
    return "Vosk Keyword (Built-in)";
  return "OpenWakeWord (TFLite)";
}

bool matchesWordSet(QStringList actual, QStringList expected) {
  for (QString &item : actual)
    item = item.trimmed().toLower();
  for (QString &item : expected)
    item = item.trimmed().toLower();
  actual.removeAll("");
  expected.removeAll("");
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  return actual == expected;
}

QStringList normalizedWordList(QStringList values) {
  QStringList cleaned;
  for (QString value : values) {
    value = value.trimmed();
    if (!value.isEmpty() && !cleaned.contains(value, Qt::CaseInsensitive))
      cleaned << value;
  }
  return cleaned;
}

void repairLegacyWakewordState(QSettings &settings) {
  bool touched = false;
  const QString canonicalEngine = canonicalWakeEngineLabel(
      settings.value("wakeEngine", "OpenWakeWord (TFLite)").toString());
  if (settings.value("wakeEngine").toString() != canonicalEngine) {
    settings.setValue("wakeEngine", canonicalEngine);
    touched = true;
  }

  QStringList wakeWords = normalizedWordList(settings.value("wakeWords").toStringList());
  const QStringList changedWakeWords =
      normalizedWordList(settings.value("wakeWordsChanged").toStringList());
  if (matchesWordSet(wakeWords, {"hey jarvis", "hey mycroft"})) {
    settings.setValue("wakeWords", QStringList{"hey jarvis", "alexa"});
    settings.setValue("wakeWordsChanged", QStringList{"hey jarvis", "alexa"});
    wakeWords = QStringList{"hey jarvis", "alexa"};
    touched = true;
  }
  if (wakeWords.isEmpty() && changedWakeWords.isEmpty()) {
    settings.setValue("wakeWords", QStringList{"hey jarvis"});
    settings.setValue("wakeWordsChanged", QStringList{"hey jarvis"});
    touched = true;
  }

  QStringList closeWords =
      normalizedWordList(settings.value("closeWords").toStringList());
  const QStringList changedCloseWords =
      normalizedWordList(settings.value("closeWordsChanged").toStringList());
  if (matchesWordSet(closeWords, {"goodbye", "hello"})) {
    settings.setValue("closeWords",
                      QStringList{"stop listening", "go to sleep"});
    settings.setValue("closeWordsChanged",
                      QStringList{"stop listening", "go to sleep"});
    closeWords = QStringList{"stop listening", "go to sleep"};
    touched = true;
  }
  if (closeWords.isEmpty() && changedCloseWords.isEmpty()) {
    settings.setValue("closeWords",
                      QStringList{"stop listening", "go to sleep"});
    settings.setValue("closeWordsChanged",
                      QStringList{"stop listening", "go to sleep"});
    touched = true;
  }

  settings.setValue("nativeTfliteRepairApplied_v1", true);
  settings.setValue("nativeTfliteRepairApplied_v2", true);
  touched = true;
  if (touched)
    settings.sync();
}
} // namespace

PillWidget::PillWidget(QWidget *parent) : QWidget(parent) {
  qDebug() << "PillWidget Constructor Start";
  QSettings settings("QuickSTT", "Config");
  repairLegacyWakewordState(settings);
  pillWidth = settings.value("pillWidth", 360).toInt();
  pillHeight = settings.value("pillHeight", 50).toInt();
  pillRadius = settings.value("pillRadius", 25).toInt();
  activeOpacity = settings.value("opacity", 100).toInt();
  iconSize = settings.value("iconSize", 30).toInt();
  trayIconSize = settings.value("trayIconSize", 32).toInt();
  specialCommandsEnabled =
      settings.value("specialCommandsEnabled", true).toBool();
  waveformSensitivity = settings.value("waveformSensitivity", 5).toInt();

  if (!settings.value("startupChecked", false).toBool()) {
    settings.setValue("startupChecked", true);
  }
  if (!settings.contains("startupBackground")) {
    settings.setValue("startupBackground", true);
  }
  QTimer::singleShot(0, this, []() { applyStartupSetting(true); });

  setWindowOpacity(activeOpacity / 100.0);
  setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setMouseTracking(true); // Needed for hover-edge resize cursors
  m_widgetFlexible = settings.value("widgetFlexible", false).toBool();
  resize(pillWidth, pillHeight);

  showWaveform = settings.value("showWaveform", true).toBool();
  waveformDelayTimer = new QTimer(this);
  waveformDelayTimer->setSingleShot(true);
  connect(waveformDelayTimer, &QTimer::timeout, [this]() {
    canShowWaveform = true;
    update();
  });
  waveformAnimationTimer = new QTimer(this);
  waveformAnimationTimer->setInterval(24);
  connect(waveformAnimationTimer, &QTimer::timeout, this,
          &PillWidget::updateWaveformFrame);
  waveformAnimationTimer->start();

  m_transientStatusTimer = new QTimer(this);
  m_transientStatusTimer->setSingleShot(true);
  connect(m_transientStatusTimer, &QTimer::timeout, this, [this]() {
    m_transientStatusText.clear();
    if (isListening) {
      statusLabel->hide();
    } else {
      statusLabel->show();
      statusLabel->setText(currentStatusText);
    }
    update();
  });

  autoShowSuppressTimer = new QTimer(this);
  autoShowSuppressTimer->setSingleShot(true);
  connect(autoShowSuppressTimer, &QTimer::timeout, this,
          [this]() { m_temporarilySuppressAutoShow = false; });

  // Close Button
  closeBtn = new QPushButton("✕", this);
  closeBtn->setFocusPolicy(Qt::NoFocus);
  closeBtn->setStyleSheet(
      "QPushButton { color: #888888; background: transparent; border: none; "
      "font-weight: bold; font-size: 14px; } QPushButton:hover { color: "
      "#FF4444; }");
  connect(closeBtn, &QPushButton::clicked, this, &PillWidget::onCloseClicked);

  redDot = new QPushButton(this);
  redDot->setFocusPolicy(Qt::NoFocus);
  redDot->setStyleSheet("background-color: transparent; border: none;");
  connect(redDot, &QPushButton::clicked, this, &PillWidget::onRedDotClicked);

  micBtn = new QPushButton(this);
  micBtn->setFocusPolicy(Qt::NoFocus);
  micBtn->setStyleSheet("background-color: transparent; border: none;");
  micBtn->setCursor(Qt::PointingHandCursor);
  connect(micBtn, &QPushButton::clicked, this, &PillWidget::onMicClicked);

  modelCombo = new PillComboBox(this);
  modelCombo->setFocusPolicy(Qt::NoFocus);
  modelCombo->setStyleSheet(
      "QComboBox { background-color: #333333; color: #DDDDDD; font-size: 12px; "
      "border-radius: 12px; padding-left: 10px; padding-right: 24px; "
      "border: none; } "
      "QComboBox::drop-down { width: 0px; border: 0px; } "
      "QComboBox QAbstractItemView { background-color: #333333; color: #EEE; "
      "selection-background-color: #555; outline: none; }");
  modelCombo->view()->setTextElideMode(Qt::ElideRight);
  QTimer::singleShot(0, this, &PillWidget::refreshModelCombo);
  connect(modelCombo, &QComboBox::currentTextChanged, this,
          &PillWidget::onModelChanged);

  modelDownloadBtn = new QPushButton("DL", this);
  modelDownloadBtn->setFocusPolicy(Qt::NoFocus);
  modelDownloadBtn->setCursor(Qt::PointingHandCursor);
  modelDownloadBtn->setStyleSheet(
      "QPushButton { background-color: #2A2A2A; color: #E0E0E0; border: 1px "
      "solid #444; border-radius: 10px; padding: 0 6px; font-size: 10px; } "
      "QPushButton:hover { background-color: #3A3A3A; }");
  modelDownloadBtn->setToolTip("Download selected model");
  connect(modelDownloadBtn, &QPushButton::clicked, this,
          &PillWidget::onModelDownloadClicked);

  statusLabel = new StatusTextLabel(this);
  statusLabel->setStyleSheet(
      "color: #888888; font-size: 10px; font-weight: normal; background: "
      "transparent; padding: 0 4px;");
  statusLabel->setText("Idling...");

  textBoardToggleBtn = new CollapseButton(this);
  textBoardToggleBtn->setFocusPolicy(Qt::NoFocus);
  textBoardToggleBtn->setRotation(textBoardOpen ? 0.0 : 180.0);
  connect(textBoardToggleBtn, &QPushButton::clicked, this,
          &PillWidget::toggleTextBoard);

  textBoardWindow = new TextBoardWindow();
  statusLabel->setTextMirrorCallback(
      [this](const QString &text) {
        if (textBoardWindow)
          textBoardWindow->setHeaderText(text);
      });
  textBoardWindow->setHeaderText(statusLabel->text());
  textBoardWindow->setOpacity(settings.value("tbOpacity", 90).toInt());
  textBoardWindow->setTextSize(settings.value("tbTextSize", 16).toInt());
  m_currentModelName =
      normalizeModelName(settings.value("selectedModel").toString());
  connect(textBoardWindow, &TextBoardWindow::attachStateChanged, this,
          [this](bool attached) {
            if (attached && textBoardOpen)
              repositionTextBoard();
          });
  connect(textBoardWindow, &TextBoardWindow::attachedDragStarted, this,
          [this](const QPoint &globalPos) {
            dragPos = globalPos - frameGeometry().topLeft();
            raise();
            if (textBoardWindow)
              textBoardWindow->raise();
          });
  connect(textBoardWindow, &TextBoardWindow::attachedDragMoved, this,
          [this](const QPoint &globalPos) {
            if (textBoardWindow && textBoardOpen && textBoardWindow->isAttached())
              move(globalPos - dragPos);
          });
  connect(textBoardWindow, &TextBoardWindow::attachedDragFinished, this,
          [this]() { repositionTextBoard(); });

  m_cloudSttManager = new CloudSttManager(this);
  connect(m_cloudSttManager, &CloudSttManager::statusChanged, this,
          [this](const QString &statusText) {
            qDebug() << "[CLOUD-STATUS]" << statusText;
            currentStatusText = statusText;
            statusLabel->show();
            statusLabel->setText(statusText);
            if (dashboard) {
              QMetaObject::invokeMethod(dashboard, "onRefreshModels",
                                        Qt::QueuedConnection);
            }
            update();
          });
  connect(m_cloudSttManager, &CloudSttManager::transcriptionReady, this,
          [this](const QString &text) {
            qDebug() << "[CLOUD-RESULT]" << text.left(160);
            if (dashboard) {
              QMetaObject::invokeMethod(dashboard, "onRefreshModels",
                                        Qt::QueuedConnection);
            }
            sendBackendCommand("CLOUD_DONE\n");
            processRecognizedText(text, true);
          });
  connect(m_cloudSttManager, &CloudSttManager::transcriptionFailed, this,
          [this](const QString &errorText) {
            qDebug() << "[CLOUD-ERROR]" << errorText;
            currentStatusText = errorText;
            statusLabel->show();
            statusLabel->setText(errorText);
            showTransientStatus(errorText, 6000);
            if (dashboard) {
              QMetaObject::invokeMethod(dashboard, "onRefreshModels",
                                        Qt::QueuedConnection);
            }
            sendBackendCommand("CLOUD_DONE\n");
            update();
          });

  m_localModelManager = new LocalModelManager(this);
  connect(m_localModelManager, &LocalModelManager::statusMessage, this,
          [this](const QString &statusText) {
            currentStatusText = statusText;
            statusLabel->show();
            statusLabel->setText(statusText);
            refreshModelCombo();
            if (dashboard) {
              QMetaObject::invokeMethod(dashboard, "onRefreshModels",
                                        Qt::QueuedConnection);
            }
            update();
          });
  connect(m_localModelManager, &LocalModelManager::progressChanged, this,
          [this](const QString &, int, const QString &statusText) {
            currentStatusText = statusText;
            statusLabel->show();
            statusLabel->setText(statusText);
            updateModelDownloadButton();
            update();
          });
  connect(m_localModelManager, &LocalModelManager::modelInstalled, this,
          [this](const QString &) {
            isDownloading = false;
            refreshModelCombo();
            updateModelDownloadButton();
            if (dashboard) {
              QMetaObject::invokeMethod(dashboard, "onRefreshModels",
                                        Qt::QueuedConnection);
            }
          });

  m_localTranscriptionManager = new LocalTranscriptionManager(this);
  connect(m_localTranscriptionManager, &LocalTranscriptionManager::statusChanged,
          this, [this](const QString &statusText) {
            currentStatusText = statusText;
            statusLabel->show();
            statusLabel->setText(statusText);
            update();
          });
  connect(m_localTranscriptionManager,
          &LocalTranscriptionManager::transcriptionReady, this,
          [this](const QString &text) {
            qDebug() << "[LOCAL-FRONTEND-RESULT]" << text.left(160);
            sendBackendCommand("CLOUD_DONE\n");
            processRecognizedText(text, false);
          });
  connect(m_localTranscriptionManager,
          &LocalTranscriptionManager::transcriptionFailed, this,
          [this](const QString &errorText) {
            qDebug() << "[LOCAL-FRONTEND-ERROR]" << errorText;
            currentStatusText = errorText;
            statusLabel->show();
            statusLabel->setText(errorText);
            showTransientStatus(errorText, 6000);
            sendBackendCommand("CLOUD_DONE\n");
            update();
          });
  connect(m_localModelManager, &LocalModelManager::modelUninstalled, this,
          [this](const QString &) {
            isDownloading = false;
            refreshModelCombo();
            updateModelDownloadButton();
            if (dashboard) {
              QMetaObject::invokeMethod(dashboard, "onRefreshModels",
                                        Qt::QueuedConnection);
            }
          });
  connect(m_localModelManager, &LocalModelManager::operationFailed, this,
          [this](const QString &, const QString &errorText) {
            isDownloading = false;
            currentStatusText = errorText;
            statusLabel->show();
            statusLabel->setText(errorText);
            refreshModelCombo();
            updateModelDownloadButton();
            update();
          });
  connect(m_localModelManager, &LocalModelManager::busyChanged, this,
          [this](bool busy) {
            isDownloading = busy;
            updateModelDownloadButton();
          });

  m_smartLifeManager = new SmartLifeManager(this);
  connect(m_smartLifeManager, &SmartLifeManager::statusChanged, this,
          [this](const QString &statusText) {
            if (statusText.trimmed().isEmpty())
              return;
            currentStatusText = statusText;
            statusLabel->show();
            statusLabel->setText(statusText);
          });
  connect(m_smartLifeManager, &SmartLifeManager::controlFinished, this,
          [this](const QString &statusText) {
            currentStatusText = statusText;
            statusLabel->show();
            statusLabel->setText(statusText);
            showTransientStatus(statusText, 4500);
          });
  connect(m_smartLifeManager, &SmartLifeManager::controlFailed, this,
          [this](const QString &statusText) {
            currentStatusText = statusText;
            statusLabel->show();
            statusLabel->setText(statusText);
            showTransientStatus(statusText, 6000);
          });

  refreshModelCombo();
  customResize(pillWidth, pillHeight, pillRadius);
  updateModelDownloadButton();

  qDebug() << "Setup Tray...";
  m_svgRenMicActive = new QSvgRenderer(
      QCoreApplication::applicationDirPath() + "/mic_active.svg", this);
  m_svgRenMicInactive = new QSvgRenderer(
      QCoreApplication::applicationDirPath() + "/mic_inactive.svg", this);
  m_svgRenApp = new QSvgRenderer(
      QCoreApplication::applicationDirPath() + "/Untitled-1.svg", this);

  setupTray();
  updateCachedIcons(); // This calls updateTrayIcon internally

  qDebug() << "Setup AHK...";
  m_ahkBridge = new AhkBridge(this);
  QString appDir = QCoreApplication::applicationDirPath();
  m_ahkBridge->setPaths(
      QDir(appDir).filePath("tools/ahk/AutoHotkey64.exe"),
      QDir(appDir).filePath("tools/ahk/QuickSTT_Commands.ahk"));
  connect(m_ahkBridge, &AhkBridge::resultReady, this, &PillWidget::onAhkResult);
  connect(m_ahkBridge, &AhkBridge::bridgeError, this,
          [this](const QString &msg) {
            currentStatusText = msg;
            statusLabel->show();
            statusLabel->setText(currentStatusText);
          });
  m_ahkBridge->start();

  qDebug() << "Setup Backend...";
  backendProcess = new QProcess(this);
  backendProcess->setWorkingDirectory(appDir);
  backendProcess->setStandardErrorFile(
      QDir(appDir).filePath("service_error.log"));
  connect(backendProcess, &QProcess::readyReadStandardOutput, this,
          &PillWidget::onProcessOutput);
  connect(backendProcess, &QProcess::finished, this,
          &PillWidget::onBackendFinished);
  connect(backendProcess, &QProcess::errorOccurred, this,
          &PillWidget::onBackendError);
  startBackend();

  backendHealthTimer = new QTimer(this);
  backendHealthTimer->setInterval(5000);
  connect(backendHealthTimer, &QTimer::timeout, this,
          &PillWidget::ensureBackendRunning);
  backendHealthTimer->start();

  blinkTimer = new QTimer(this);
  connect(blinkTimer, &QTimer::timeout, this, &PillWidget::updateBlink);

  // Auto-offload timer
  {
    QSettings os("QuickSTT", "Config");
    m_autoOffload = os.value("autoOffload", true).toBool();
    m_offloadMinutes = os.value("offloadMinutes", 3).toInt();
  }
  m_offloadTimer = new QTimer(this);
  m_offloadTimer->setSingleShot(true);
  connect(m_offloadTimer, &QTimer::timeout, this, [this]() {
    qDebug() << "Auto-offload: unloading model to save RAM";
    sendBackendCommand("OFFLOAD\n");
    m_modelOffloaded = true;
  });

  qDebug() << "Setup Dashboard...";
  try {
    dashboard = new MainWindow();
    qDebug() << "Dashboard created OK";
  } catch (const std::exception &e) {
    qDebug() << "Dashboard CRASH (std): " << e.what();
    dashboard = nullptr;
  } catch (...) {
    qDebug() << "Dashboard CRASH (unknown)";
    dashboard = nullptr;
  }
  if (dashboard) {
    dashboard->setBackend(backendProcess);
    dashboard->setLocalModelManager(m_localModelManager);
    dashboard->setSmartLifeManager(m_smartLifeManager);
    qDebug() << "Dashboard backend set OK";
    connect(dashboard, &MainWindow::settingChanged, this,
            &PillWidget::onSettingChanged);
    qDebug() << "Dashboard signal connected OK";
  }
  if (trayMenu) {
    qDebug() << "Dashboard tray action added OK";
  } else {
    qDebug() << "WARNING: trayMenu is null!";
  }
  qDebug() << "PillWidget Constructor End";

  // Make all static labels selectable (for status reading/copy)
  for (QLabel *lbl : findChildren<QLabel *>()) {
    lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
  }
}

void PillWidget::startBackend() {
  if (m_isQuitting)
    return;
  if (backendProcess->state() != QProcess::NotRunning)
    return;

  QString appDir = QCoreApplication::applicationDirPath();
  QString modelStr = currentComboModelName();

  QSettings s("QuickSTT", "Config");
  QString wakeEngine = canonicalWakeEngineLabel(
      s.value("wakeEngine", "OpenWakeWord (TFLite)").toString());

  QString backendPath = QDir(appDir).filePath("stt_service.exe");
  qDebug() << "Starting native C++ backend (stt_service.exe) - model:" << modelStr
           << "wake:" << wakeEngine;

  if (!QFile::exists(backendPath)) {
    qDebug() << "Backend executable missing:" << backendPath;
    statusLabel->show();
    statusLabel->setText("Audio engine missing");
    QTimer::singleShot(2000, this, &PillWidget::startBackend);
    return;
  }

  backendProcess->setWorkingDirectory(appDir);
  backendProcess->setProgram(backendPath);
  backendProcess->setArguments({});
  backendProcess->start();

  if (!backendProcess->waitForStarted(2000)) {
    qDebug() << "Backend failed to start:" << backendProcess->errorString();
    statusLabel->show();
    statusLabel->setText("Restarting audio engine...");
    QTimer::singleShot(2000, this, &PillWidget::startBackend);
    return;
  }

  qDebug() << "Backend started with PID" << backendProcess->processId();

  const QString selectedModel = currentComboModelName();
  const bool cloudSelected = isCloudModel(selectedModel);
  const QString backendModel =
      cloudSelected ? baseBackendModelName() : selectedModel;

  if (backendModel == "Vosk Small En") {
    for (int i = m_pendingBackendCommands.size() - 1; i >= 0; --i) {
      if (m_pendingBackendCommands[i].startsWith("MODEL:"))
        m_pendingBackendCommands.removeAt(i);
    }
  }

  bool hasQueuedModelCommand = false;
  for (const QByteArray &cmd : m_pendingBackendCommands) {
    if (cmd.startsWith("MODEL:")) {
      hasQueuedModelCommand = true;
      break;
    }
  }

  backendProcess->write(cloudSelected ? "TRANSCRIBE_MODE:CLOUD\n"
                                      : "TRANSCRIBE_MODE:LOCAL\n");

  if (!hasQueuedModelCommand && !backendModel.isEmpty() &&
      backendModel != "Vosk Small En" && supportsRuntimeModel(backendModel) &&
      isModelInstalled(backendModel)) {
    backendProcess->write(("MODEL:" + backendModel + "\n").toUtf8());
  } else if (cloudSelected && isModelInstalled(baseBackendModelName())) {
    backendProcess->write(("MODEL:" + baseBackendModelName() + "\n").toUtf8());
  }

  QList<QByteArray> pendingCommands = m_pendingBackendCommands;
  m_pendingBackendCommands.clear();
  for (const QByteArray &cmd : pendingCommands) {
    backendProcess->write(cmd);
  }
}

void PillWidget::onBackendFinished(int exitCode,
                                   QProcess::ExitStatus exitStatus) {
  qDebug() << "Backend exited. exitCode=" << exitCode
           << " exitStatus=" << exitStatus;
  if (m_isQuitting)
    return;
  statusLabel->show();
  statusLabel->setText("Restarting audio engine...");
  QTimer::singleShot(2000, this,
                     &PillWidget::startBackend); // Auto-Restart after 2s
}

void PillWidget::onBackendError(QProcess::ProcessError error) {
  qDebug() << "Backend process error:" << error
           << backendProcess->errorString();
  if (m_isQuitting)
    return;
  if (backendProcess->state() == QProcess::NotRunning) {
    statusLabel->show();
    statusLabel->setText("Restarting audio engine...");
    QTimer::singleShot(2000, this, &PillWidget::startBackend);
  }
}

void PillWidget::ensureBackendRunning() {
  if (m_isQuitting)
    return;
  if (backendProcess->state() == QProcess::NotRunning) {
    qDebug() << "Backend healthcheck detected a stopped backend. Restarting.";
    startBackend();
  }
}

void PillWidget::enterEvent(QEnterEvent *event) { QWidget::enterEvent(event); }

void PillWidget::leaveEvent(QEvent *event) { QWidget::leaveEvent(event); }

void PillWidget::closeEvent(QCloseEvent *event) {
  if (trayIcon && trayIcon->isVisible()) {
    suppressAutoShowBriefly(3000);
    hide();
    trayIcon->showMessage("QuickSTT", "Minimized to Tray",
                          QSystemTrayIcon::Information, 1000);
    event->ignore();
  } else {
    event->accept();
  }
}

PillWidget::~PillWidget() {
  m_isQuitting = true;
  if (backendHealthTimer)
    backendHealthTimer->stop();
  if (waveformAnimationTimer)
    waveformAnimationTimer->stop();
  if (m_ahkBridge)
    m_ahkBridge->stop();
  delete dashboard;
  delete textBoardWindow;
  if (backendProcess->state() == QProcess::Running) {
    backendProcess->terminate();
    backendProcess->waitForFinished();
  }
}

void PillWidget::onAhkResult(int reqId, bool commandExecuted,
                             const QString &statusText) {
  currentStatusText = statusText;
  statusLabel->show();
  statusLabel->setText(currentStatusText);

  const QString originalText = m_ahkPendingText.take(reqId);
  if (!commandExecuted && !originalText.isEmpty() && textBoardWindow) {
    textBoardWindow->appendText(originalText);
  }
  update();
}

void PillWidget::toggleTextBoard() {
  textBoardOpen = !textBoardOpen;
  textBoardToggleBtn->animateTo(textBoardOpen ? 0.0 : 180.0);
  if (textBoardOpen) {
    textBoardWindow->show();
    repositionTextBoard();
  } else {
    textBoardWindow->hide();
  }
}

void PillWidget::repositionTextBoard() {
  if (textBoardWindow && textBoardOpen && textBoardWindow->isAttached()) {
    textBoardWindow->repositionAttached(geometry());
  }
}

void PillWidget::restoreFromExternalTrigger() {
  suppressAutoShowBriefly(1200);
  ensureBackendRunning();
  if (!isVisible())
    show();
  raise();
  activateWindow();
  if (textBoardWindow && textBoardOpen)
    textBoardWindow->show();
  repositionTextBoard();

  if (currentStatusText.compare(QStringLiteral("Hidden"), Qt::CaseInsensitive) ==
      0) {
    currentStatusText = QStringLiteral("Waking...");
    statusLabel->show();
    statusLabel->setText(currentStatusText);
    sendBackendCommand("TOGGLE\n");
  }
}

void PillWidget::moveEvent(QMoveEvent *event) {
  QWidget::moveEvent(event);
  repositionTextBoard();
}

void PillWidget::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  repositionTextBoard();
}

void PillWidget::hideEvent(QHideEvent *event) {
  if (textBoardWindow)
    textBoardWindow->hide();
  QWidget::hideEvent(event);
}

void PillWidget::showEvent(QShowEvent *event) {
  // Cancel offload timer if still running
  if (m_offloadTimer && m_offloadTimer->isActive()) {
    m_offloadTimer->stop();
    qDebug() << "Auto-offload timer cancelled (widget shown before expiry)";
  }
  // If model was offloaded, reload it
  if (m_modelOffloaded) {
    qDebug() << "Model was offloaded — sending RELOAD";
    sendBackendCommand("RELOAD\n");
    m_modelOffloaded = false;
  }
  if (textBoardWindow && textBoardOpen)
    textBoardWindow->show();
  QWidget::showEvent(event);
  repositionTextBoard();
}

// Helper for Aspect Ratio
void drawSvg(QPainter &p, QSvgRenderer &ren, int w, int h) {
  QSize def = ren.defaultSize();
  if (def.isEmpty()) {
    ren.render(&p, QRectF(0, 0, w, h));
  } else {
    QSizeF scaled = QSizeF(def).scaled(w, h, Qt::KeepAspectRatio);
    double x = (w - scaled.width()) / 2.0;
    double y = (h - scaled.height()) / 2.0;
    ren.render(&p, QRectF(x, y, scaled.width(), scaled.height()));
  }
}

void PillWidget::updateCachedIcons() {
  int s = iconSize;
  cachedMicActive = QPixmap(s, s);
  cachedMicActive.fill(Qt::transparent);
  cachedMicInactive = QPixmap(s, s);
  cachedMicInactive.fill(Qt::transparent);
  QPainter p;

  // Use logical mapping: mic_active.svg -> cachedMicActive
  if (m_svgRenMicActive->isValid()) {
    p.begin(&cachedMicActive);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    drawSvg(p, *m_svgRenMicActive, s, s);
    p.end();
  } else {
    // Fallback: Blue dot for active
    p.begin(&cachedMicActive);
    p.setBrush(QColor("#00AAFF"));
    p.setPen(Qt::NoPen);
    p.drawEllipse(2, 2, s - 4, s - 4);
    p.end();
  }

  if (m_svgRenMicInactive->isValid()) {
    p.begin(&cachedMicInactive);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    drawSvg(p, *m_svgRenMicInactive, s, s);
    p.end();
  } else {
    // Fallback: Gray dot for inactive
    p.begin(&cachedMicInactive);
    p.setBrush(QColor("#555555"));
    p.setPen(Qt::NoPen);
    p.drawEllipse(2, 2, s - 4, s - 4);
    p.end();
  }

  updateTrayIcon();
}

void PillWidget::updateTrayIcon() {
  if (!trayIcon)
    return;

  // Always use Untitled-1.svg for the app/tray/dashboard icon
  int renderSize = 256;
  QPixmap iconPix(renderSize, renderSize);
  iconPix.fill(Qt::transparent);

  QPainter p(&iconPix);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);

  if (m_svgRenApp->isValid()) {
    drawSvg(p, *m_svgRenApp, renderSize, renderSize);
  } else {
    // Fallback: a clean circular icon
    p.setBrush(QColor("#1A1A1A"));
    p.setPen(QPen(QColor("#00AAFF"), 8));
    p.drawEllipse(16, 16, renderSize - 32, renderSize - 32);
  }
  p.end();

  QIcon appIcon(iconPix);
  trayIcon->setIcon(appIcon);
  setWindowIcon(appIcon);

  if (!trayIcon->isVisible())
    trayIcon->show();

  if (dashboard)
    dashboard->setWindowIcon(appIcon);
}

void PillWidget::toggleStartup(bool e) { applyStartupSetting(e); }

// Layout:
// [RedDot] [Mic][Dropdown] [■■ Status ■■■■■■■■■■■■■] [Arrow] [Close]
void PillWidget::customResize(int w, int h, int r) {
  pillWidth = w;
  pillHeight = h;
  pillRadius = r;
  resize(w, h);

  const int leftMargin = qBound(9, w / 22, 12);
  const int dotGap = qBound(5, w / 55, 8);
  const int micGap = qBound(1, w / 120, 3);
  const int comboGap = qBound(3, w / 90, 5);
  int x = leftMargin;

  // 1. Red Dot (small, 10px)
  int dotSize = 10;
  redDot->setGeometry(x, (h - dotSize) / 2, dotSize, dotSize);
  x += dotSize + dotGap;

  // 2. Mic button
  int micW = iconSize + 6;
  micBtn->setGeometry(x, 0, micW, h);
  x += micW + micGap; // keep this tight, especially on smaller widths

  const bool showDownloadButton =
      modelDownloadBtn && modelDownloadBtn->isVisible();
  const int closeSize = 18;
  const int margin = 12;

  // 3. Dropdown - grows when the widget is stretched wider.
  int comboW = qMax(120, qMin(220, (w / 2) - 20));
  modelCombo->setGeometry(x, (h - 24) / 2, comboW, 24);
  x += comboW + comboGap;

  if (showDownloadButton) {
    int downloadW = 28;
    modelDownloadBtn->setGeometry(x, (h - 22) / 2, downloadW, 22);
    x += downloadW + 6;
  } else if (modelDownloadBtn) {
    modelDownloadBtn->setGeometry(0, 0, 0, 0);
  }

  // 4. Close Button and Arrow pinned right
  closeBtn->setGeometry(w - closeSize - margin, (h - closeSize) / 2, closeSize,
                        closeSize);
  textBoardToggleBtn->setGeometry(w - closeSize - 22 - margin, (h - 22) / 2, 22,
                                  22);

  // 5. Status — everything between dropdown+wave and the arrow
  int statusX = x;
  int statusRight = w - closeSize - 22 - margin - 6;
  int statusW = statusRight - statusX;
  if (statusW < 10)
    statusW = 10;
  statusLabel->setAvailableRect(QRect(statusX, 0, statusW, h));

  const int wavePadX = qMax(1, h / 30);
  const int wavePadY = qMax(1, h / 24);
  waveRect = QRect(statusX + wavePadX, wavePadY,
                   qMax(10, statusW - (wavePadX * 2)),
                   qMax(10, h - (wavePadY * 2)));
  m_waveformHistoryLimit = waveformHistoryLimitForWidth(waveRect.width());
  while (waveformTargetLevels.size() > m_waveformHistoryLimit)
    waveformTargetLevels.removeFirst();
  while (waveformDisplayLevels.size() > m_waveformHistoryLimit)
    waveformDisplayLevels.removeFirst();

  repositionTextBoard();
  update();
}

void PillWidget::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);

  p.setBrush(QColor("#1A1A1A"));
  p.setPen(Qt::NoPen);
  p.drawRoundedRect(rect(), pillRadius, pillRadius);

  // Dot states:
  //  - Grey     = idle (not listening)
  //  - Dark red = transcribing (listening, no MP3 rec)
  //  - Bright blinking red = actively recording MP3
  QColor dotColor;
  if (m_mp3Recording) {
    dotColor = blinkState ? QColor("#FF2222") : QColor("#880000");
  } else if (isListening) {
    dotColor = QColor("#550000"); // steady dark red during transcription
  } else {
    dotColor = QColor("#444444"); // grey when idle
  }
  p.setBrush(dotColor);
  p.drawEllipse(redDot->geometry());

  // Show the normal (un-slashed) mic when idle = "click me to start"
  // Show the slashed mic when listening = "click me to stop"
  QPixmap &pix = isListening ? cachedMicInactive : cachedMicActive;
  if (!pix.isNull()) {
    QRect r = micBtn->geometry();
    // Center alignment
    p.drawPixmap(r.center().x() - pix.width() / 2,
                 r.center().y() - pix.height() / 2, pix);
  }

  if (isDownloading) {
    p.setPen(Qt::green);
    // p.drawText logic handled by label
  }

  // ── Audio Waveform ────────────────────────────────────────────────────────
  // Drawn in the area of statusLabel (which is hidden while listening).
  // Color matches status text: grey #888888. Red during MP3 recording.
  if (isListening && showWaveform && canShowWaveform &&
      !waveformDisplayLevels.isEmpty()) {
    QColor waveColor = m_mp3Recording ? QColor("#FF2222") : QColor("#888888");
    QRectF sr = QRectF(waveRect).adjusted(0.0, 0.5, 0.0, -0.5);
    const qreal centerY = sr.center().y();
    const qreal heightScale = waveformHeightScale(int(sr.height()));
    const WaveformLayout layout = buildWaveformLayout(sr);
    const qreal visualCurve =
        qBound<qreal>(0.60, 0.72 - ((heightScale - 1.0) * 0.045), 0.74);

    p.setPen(Qt::NoPen);
    p.setBrush(waveColor);
    for (int i = 0; i < layout.slotCount; ++i) {
      const qreal x = layout.slotCount > 1 ? layout.startX + (i * layout.slotStep)
                                           : sr.center().x();
      const float level =
          sampleWaveformLevel(waveformDisplayLevels, i, layout.slotCount);
      const qreal visualLevel = std::pow(level, visualCurve);

      if (visualLevel <= layout.dotThreshold) {
        const qreal dotVisual =
            qBound<qreal>(0.0, visualLevel / layout.dotThreshold, 1.0);
        const qreal scaledDotSize =
            layout.dotSize * (0.78 + (dotVisual * 0.22));
        QRectF dotRect(x - (scaledDotSize / 2.0),
                       centerY - (scaledDotSize / 2.0), scaledDotSize,
                       scaledDotSize);
        p.drawEllipse(dotRect);
        continue;
      }

      const qreal normalizedLine =
          qBound<qreal>(0.0,
                        (visualLevel - layout.dotThreshold) /
                            (1.0 - layout.dotThreshold),
                        1.0);
      qreal lineHeight =
          layout.minLineHeight +
          normalizedLine * (layout.maxLineHeight - layout.minLineHeight);
      if (visualLevel < layout.lowLineThreshold) {
        const qreal microMix =
            qBound<qreal>(0.0,
                          (visualLevel - layout.dotThreshold) /
                              (layout.lowLineThreshold - layout.dotThreshold),
                          1.0);
        lineHeight =
            layout.minLineHeight + microMix * (sr.height() * 0.12);
      }

      QRectF barRect(x - (layout.lineWidth / 2.0), centerY - (lineHeight / 2.0),
                     layout.lineWidth, lineHeight);
      p.drawRoundedRect(barRect, layout.lineWidth / 2.0, layout.lineWidth / 2.0);
    }
  }
}

// ... Boilerplate ...
void PillWidget::onSettingChanged(QString key, QVariant val) {
  QSettings s("QuickSTT", "Config");
  s.setValue(key, val);

  if (key == "widgetFlexible") {
    m_widgetFlexible = val.toBool();
    if (!m_widgetFlexible)
      setCursor(Qt::ArrowCursor);
  }
  if (key == "tbOpacity" && textBoardWindow) {
    textBoardWindow->setOpacity(val.toInt());
  }
  if (key == "tbTextSize" && textBoardWindow) {
    textBoardWindow->setTextSize(val.toInt());
  }
  if (key == "pillRadius")
    pillRadius = val.toInt();
  if (key == "opacity")
    activeOpacity = val.toInt();
  if (key == "startupChecked")
    toggleStartup(val.toBool());
  if (key == "startupBackground" && s.value("startupChecked", false).toBool()) {
    toggleStartup(true);
  }
  if (key == "iconSize") {
    iconSize = val.toInt();
    updateCachedIcons();
  } else if (key == "trayIconSize") {
    trayIconSize = val.toInt();
    updateCachedIcons();
  } else if (key == "showWaveform") {
    showWaveform = val.toBool();
  } else if (key == "specialCommandsEnabled") {
    specialCommandsEnabled = val.toBool();
  } else if (key == "waveformSensitivity") {
    waveformSensitivity = val.toInt();
  } else if (key == "refreshModels") {
    refreshModelCombo();
  } else if (key.startsWith("cloud/")) {
    refreshModelCombo();
  } else if (key == "wakeWordsChanged") {
    QStringList words = val.toStringList();
    sendBackendCommand(("WAKEWORDS:" + words.join(",") + "\n").toUtf8());
  } else if (key == "closeWordsChanged") {
    QStringList words = val.toStringList();
    sendBackendCommand(("CLOSEWORDS:" + words.join(",") + "\n").toUtf8());
  } else if (key == "wakeEngineChanged") {
    QString engine = canonicalWakeEngineLabel(val.toString());
    if (engine != val.toString())
      s.setValue("wakeEngine", engine);
    sendBackendCommand(("WAKEMODE:" + engine + "\n").toUtf8());
    if (backendProcess->state() == QProcess::NotRunning) {
      startBackend();
    }
  } else if (key == "recordingDir") {
    sendBackendCommand(("SET_REC_DIR:" + val.toString() + "\n").toUtf8());
  } else if (key == "autoOffload") {
    m_autoOffload = val.toBool();
    if (!m_autoOffload && m_offloadTimer)
      m_offloadTimer->stop();
  } else if (key == "offloadMinutes") {
    m_offloadMinutes = val.toInt();
  }
  setCustomOpacity(activeOpacity);
  customResize(pillWidth, pillHeight, pillRadius);
  update();
}

// ── Native Windows SendInput text typing ─────────────────────────────────────
static void nativeSendText(const QString &text) {
  if (text.isEmpty())
    return;
  std::vector<INPUT> inputs;
  inputs.reserve(text.size() * 2);
  for (const QChar &ch : text) {
    INPUT down = {};
    down.type = INPUT_KEYBOARD;
    down.ki.wScan = ch.unicode();
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    inputs.push_back(down);

    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    inputs.push_back(up);
  }
  if (!inputs.empty()) {
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
  }
}

void PillWidget::onProcessOutput() {
  while (backendProcess->canReadLine()) {
    QString l = backendProcess->readLine().trimmed();
    QStringList p = l.split("|");
    if (p.size() < 2)
      continue;
    QString eventType = p[0];
    QString payload = p.mid(1).join("|");

    if (eventType == "STATE") {
      int commaPos = payload.indexOf(',');
      int c = commaPos >= 0 ? payload.left(commaPos).toInt() : payload.toInt();
      QString stateText =
          commaPos >= 0 ? payload.mid(commaPos + 1).trimmed() : QString();
      qDebug() << "[STATE] code=" << c << " text=" << stateText;

      if (c == 1 || c == 2) {
        currentStatusText = stateText;
        if (m_transientStatusTimer && m_transientStatusTimer->isActive() &&
            !m_transientStatusText.isEmpty()) {
          statusLabel->show();
          statusLabel->setText(m_transientStatusText);
        } else {
          statusLabel->hide();
        }

        if (!isListening) {
          canShowWaveform = false;
          waveformDelayTimer->start(45);

          if (isHidden() && !m_temporarilySuppressAutoShow) {
            show();
            raise();
          }
        }

        isListening = true;
        if (!isVisible() && !m_temporarilySuppressAutoShow)
          show();
        isRecording = (c == 2);

        updateCachedIcons();
        update();
      } else if (c == 0) {
        isListening = false;
        isRecording = false;
        canShowWaveform = false;
        waveformDelayTimer->stop();
        audioWaveform.clear();
        waveformTargetLevels.clear();
        waveformDisplayLevels.clear();
        currentStatusText = stateText.isEmpty() ? "Ready" : stateText;
        statusLabel->show();
        statusLabel->setText(currentStatusText);
        if (m_mp3Recording) {
          m_mp3Recording = false;
          blinkTimer->stop();
          blinkState = false;
          if (mp3RecordProcess &&
              mp3RecordProcess->state() == QProcess::Running) {
            mp3RecordProcess->write("q");
            mp3RecordProcess->waitForFinished(2000);
          }
        }
        blinkTimer->stop();
        blinkState = false;
        updateCachedIcons();
        update();
      } else if (c == -1) {
        isListening = false;
        isRecording = false;
        canShowWaveform = false;
        audioWaveform.clear();
        waveformTargetLevels.clear();
        waveformDisplayLevels.clear();
        blinkTimer->stop();
        currentStatusText = "Hidden";
        statusLabel->show();
        statusLabel->setText(currentStatusText);
        updateCachedIcons();
        hide();
      } else if (c == 3) {
        statusLabel->show();
        currentStatusText = stateText;
        if (!stateText.isEmpty())
          statusLabel->setText(stateText);
        updateCachedIcons();
      }
      update();
    } else if (eventType == "DL_PROGRESS" || eventType == "DL_STATS") {
      isDownloading = true;
      statusLabel->show();
      QString data = payload;
      if (data.contains("|")) {
        QStringList parts = data.split("|");
        if (parts.size() >= 3) {
          downloadProgress = parts[0].toInt();
          downloadSpeed = parts[1];
          totalDownloadSize = parts[2];
          statusLabel->setText(QString("DL: %1% (%2) - %3")
                                   .arg(downloadProgress)
                                   .arg(totalDownloadSize)
                                   .arg(downloadSpeed));
        }
      } else {
        downloadProgress = data.toInt();
        statusLabel->setText("DL: " + QString::number(downloadProgress) + "%");
      }
      update();
    } else if (eventType == "DL_STATUS") {
      isDownloading = true;
      statusLabel->show();
      statusLabel->setText(payload);
      update();
    } else if (eventType == "DL_COMPLETE") {
      isDownloading = false;
      refreshModelCombo();
      if (dashboard) {
        QMetaObject::invokeMethod(dashboard, "onRefreshModels",
                                  Qt::QueuedConnection);
      }
      statusLabel->show();
      if (payload.compare("Failed", Qt::CaseInsensitive) == 0) {
        currentStatusText = "Download failed";
      } else if (!payload.isEmpty()) {
        currentStatusText = "Preparing " + payload + "...";
      } else if (currentStatusText.isEmpty()) {
        currentStatusText = "Ready";
      }
      statusLabel->setText(currentStatusText);
      update();
    } else if (eventType == "DL_ERROR") {
      isDownloading = false;
      refreshModelCombo();
      if (dashboard) {
        QMetaObject::invokeMethod(dashboard, "onRefreshModels",
                                  Qt::QueuedConnection);
      }
      statusLabel->show();
      currentStatusText = "Download failed";
      statusLabel->setText(currentStatusText);
      update();
    } else if (eventType == "UNINSTALL_COMPLETE") {
      refreshModelCombo();
      if (dashboard) {
        QMetaObject::invokeMethod(dashboard, "onRefreshModels",
                                  Qt::QueuedConnection);
      }
      statusLabel->show();
      currentStatusText =
          payload.isEmpty() ? "Model removed" : ("Removed " + payload);
      statusLabel->setText(currentStatusText);
      update();
    } else if (eventType == "PARTIAL_TEXT") {
      // Partials are for engine diagnostics only — no native typing
      // This avoids doubled text, command flicker, and scroll glitches
      qDebug() << "[PARTIAL]" << payload.trimmed();

    } else if (eventType == "CLOUD_AUDIO") {
      const QString audioPath = payload.trimmed();
      if (audioPath.isEmpty()) {
        update();
        continue;
      }
      const QString activeModel = currentComboModelName();
      if (isCloudModel(activeModel)) {
        if (m_cloudSttManager)
          m_cloudSttManager->transcribeFile(activeModel, audioPath);
        else
          sendBackendCommand("CLOUD_DONE\n");
      } else if (localModelRequiresFrontendTranscription(activeModel)) {
        if (m_localTranscriptionManager)
          m_localTranscriptionManager->transcribeFile(activeModel, audioPath);
        else
          sendBackendCommand("CLOUD_DONE\n");
      } else {
        sendBackendCommand("CLOUD_DONE\n");
      }

    } else if (eventType == "FINAL_TEXT") {
      const QString trimmed = payload.trimmed();
      if (trimmed.isEmpty()) {
        update();
        continue;
      }

      // Dedup guard: skip if same text received within 800ms
      static QString s_lastTypedText;
      static qint64 s_lastTypedMs = 0;
      qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
      if (trimmed == s_lastTypedText && (nowMs - s_lastTypedMs) < 800) {
        qDebug() << "[DEDUP] Skipping duplicate:" << trimmed;
        continue;
      }
      s_lastTypedText = trimmed;
      s_lastTypedMs = nowMs;

      processRecognizedText(trimmed, false);

      update();
    } else if (eventType == "ERROR") {
      statusLabel->show();
      currentStatusText = payload;
      statusLabel->setText(payload);
      update();
    } else if (eventType == "AUDIO_LEVEL") {
      int rawLevel = payload.toInt();
      const qreal widthScale = waveformWidthScale(qMax(1, waveRect.width()));
      const qreal heightScale = waveformHeightScale(qMax(1, waveRect.height()));
      const qreal sizeSensitivity = qBound<qreal>(
          0.68, 0.76 + ((widthScale - 1.0) * 0.24) +
                    ((heightScale - 1.0) * 0.52),
          1.55);
      const qreal sensitivityGain =
          (0.42 + (qreal(waveformSensitivity) / 15.0) * 0.40) *
          sizeSensitivity;
      qreal normalized =
          qBound<qreal>(0.0, (qreal(rawLevel) / 100.0) * sensitivityGain, 1.0);
      const qreal noiseFloor =
          qBound<qreal>(0.105,
                        0.145 - ((widthScale - 1.0) * 0.016) -
                            ((heightScale - 1.0) * 0.028),
                        0.185);
      if (normalized <= noiseFloor) {
        normalized = 0.0;
      } else {
        normalized = (normalized - noiseFloor) / (1.0 - noiseFloor);
      }

      const qreal curve =
          qBound<qreal>(0.88,
                        1.08 - ((heightScale - 1.0) * 0.16) -
                            ((widthScale - 1.0) * 0.08),
                        1.12);
      const qreal sizeBoost = qBound<qreal>(
          0.78, 0.84 + ((heightScale - 1.0) * 0.40) +
                    ((widthScale - 1.0) * 0.20),
          1.55);
      float shaped =
          qBound(0.0f, float(std::pow(normalized, curve) * sizeBoost), 1.0f);
      if (shaped < 0.05f)
        shaped = 0.0f;

      waveformTargetLevels.append(shaped);
      while (waveformTargetLevels.size() > m_waveformHistoryLimit)
        waveformTargetLevels.removeFirst();
    }
  }
}

void PillWidget::setupTray() {
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    QMessageBox::critical(this, "QuickSTT", "No Tray System!");
  }
  trayIcon = new QSystemTrayIcon(this);
  updateTrayIcon();
  trayMenu = new QMenu(this);
  trayMenu->addAction("Dashboard", this, &PillWidget::openDashboard);
  trayMenu->addAction("Show Widget", this,
                      &PillWidget::restoreFromExternalTrigger);
  trayMenu->addAction("Hide Widget", this, [this]() {
    suppressAutoShowBriefly(3000);
    hide();
  });
  trayMenu->addAction("Quit App", this, [this]() {
    m_isQuitting = true;
    if (backendHealthTimer)
      backendHealthTimer->stop();
    if (trayIcon)
      trayIcon->hide();
    if (backendProcess && backendProcess->state() == QProcess::Running) {
      backendProcess->write("QUIT\n");
    }
    qApp->quit();
  });
  trayIcon->setContextMenu(trayMenu);
  trayIcon->show();
  connect(trayIcon, &QSystemTrayIcon::activated,
          [=](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger) {
              if (isVisible()) {
                suppressAutoShowBriefly(3000);
                hide();
              } else {
                restoreFromExternalTrigger();
              }
            }
          });
}
void PillWidget::openDashboard() {
  if (!dashboard)
    return;
  dashboard->show();
  dashboard->raise();
  dashboard->activateWindow();
}
void PillWidget::onMicClicked() {
  // Stopping mic also stops any MP3 recording in progress
  if (m_mp3Recording) {
    m_mp3Recording = false;
    blinkTimer->stop();
    blinkState = false;
    if (mp3RecordProcess && mp3RecordProcess->state() == QProcess::Running) {
      mp3RecordProcess->write("q");
      mp3RecordProcess->waitForFinished(2000);
    }
    update();
  }
  sendBackendCommand("TOGGLE\n");
}

void PillWidget::onRedDotClicked() {
  if (!isListening)
    return; // idle — nothing to do

  if (!m_mp3Recording) {
    // Start recording: create mp3/ dir and tell backend to start writing WAV
    QDir().mkpath(QCoreApplication::applicationDirPath() + "/mp3");
    QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString outPath =
        QCoreApplication::applicationDirPath() + "/mp3/rec_" + stamp + ".wav";
    sendBackendCommand(("RECORD_START:" + outPath + "\n").toUtf8());
    m_mp3Recording = true;
    blinkTimer->start(500);
    statusLabel->setText("\u25cf REC");
    update();
  } else {
    // Stop recording
    sendBackendCommand("RECORD_STOP\n");
    m_mp3Recording = false;
    blinkTimer->stop();
    blinkState = false;
    statusLabel->setText("Listening...");
    update();
  }
}
void PillWidget::onCloseClicked() {
  suppressAutoShowBriefly(3200);
  sendBackendCommand("SLEEP\n");
  isListening = false;
  isRecording = false;
  canShowWaveform = false;
  audioWaveform.clear();
  waveformTargetLevels.clear();
  waveformDisplayLevels.clear();
  blinkTimer->stop();
  blinkState = false;
  currentStatusText = "Hidden";
  statusLabel->show();
  statusLabel->setText(currentStatusText);
  updateCachedIcons();
  update();
  hide();
  if (textBoardWindow)
    textBoardWindow->hide();
  if (trayIcon)
    trayIcon->showMessage("QuickSTT", "Active in Tray",
                          QSystemTrayIcon::Information, 1000);
  // Start auto-offload timer ONLY if wakeword engine doesn't need the model
  // C++ wakeword detection (Vosk keyword) needs the model to keep running
  QSettings offloadSettings("QuickSTT", "Config");
  QString wakeEng = canonicalWakeEngineLabel(
      offloadSettings.value("wakeEngine", "OpenWakeWord (TFLite)").toString());
  bool wakeNeedsModel = wakeEng.contains("Vosk", Qt::CaseInsensitive);
  if (m_autoOffload && m_offloadTimer && !wakeNeedsModel) {
    int ms = m_offloadMinutes * 60 * 1000;
    if (ms <= 0)
      ms = 500; // "Instant" = 500ms grace period
    m_offloadTimer->start(ms);
    qDebug() << "Auto-offload timer started:" << m_offloadMinutes << "min";
  } else if (wakeNeedsModel) {
    qDebug() << "Auto-offload skipped: wakeword engine needs model active";
  }
}
void PillWidget::onModelChanged(const QString &text) {
  Q_UNUSED(text);
  QString modelName = currentComboModelName();
  if (modelName.isEmpty())
    return;
  m_currentModelName = modelName;
  QSettings("QuickSTT", "Config").setValue("selectedModel", modelName);
  modelCombo->setToolTip(
      buildModelTooltip(modelName, isModelInstalled(modelName)));
  updateModelDownloadButton();

  if (isCloudModel(modelName)) {
    statusLabel->show();
    if (!isCloudModelConfigured(modelName)) {
      currentStatusText = "Configure cloud provider credentials in Dashboard";
      statusLabel->setText(currentStatusText);
      update();
      return;
    }
    if (!isModelInstalled(baseBackendModelName())) {
      currentStatusText = "Vosk Small En is required as the local wake base";
      statusLabel->setText(currentStatusText);
      update();
      return;
    }
    currentStatusText = "Switching to " + modelName + "...";
    statusLabel->setText(currentStatusText);
    sendBackendCommand("TRANSCRIBE_MODE:CLOUD\n");
    sendBackendCommand(("MODEL:" + baseBackendModelName() + "\n").toUtf8());
    update();
    return;
  }

  if (localModelRequiresFrontendTranscription(modelName)) {
    statusLabel->show();
    if (!isModelInstalled(baseBackendModelName())) {
      currentStatusText = "Vosk Small En is required as the local wake base";
      statusLabel->setText(currentStatusText);
      update();
      return;
    }
    if (!isModelInstalled(modelName)) {
      currentStatusText = supportsDirectDownload(modelName)
                              ? "Model not installed - click DL"
                              : "Add model files manually first";
      statusLabel->setText(currentStatusText);
      update();
      return;
    }
    currentStatusText = "Switching to " + modelName + "...";
    statusLabel->setText(currentStatusText);
    sendBackendCommand("TRANSCRIBE_MODE:CLOUD\n");
    sendBackendCommand(("MODEL:" + baseBackendModelName() + "\n").toUtf8());
    update();
    return;
  }

  sendBackendCommand("TRANSCRIBE_MODE:LOCAL\n");

  if (!supportsRuntimeModel(modelName)) {
    statusLabel->show();
    currentStatusText = "Model not available in this build";
    statusLabel->setText(currentStatusText);
    update();
    return;
  }

  if (!isModelInstalled(modelName)) {
    statusLabel->show();
    if (supportsDirectDownload(modelName)) {
      currentStatusText = "Model not installed - click DL";
      statusLabel->setText(currentStatusText);
    } else {
      currentStatusText = "Add model files manually first";
      statusLabel->setText(currentStatusText);
    }
    update();
    return;
  }

  currentStatusText = "Switching to " + modelName + "...";
  statusLabel->show();
  statusLabel->setText(currentStatusText);
  sendBackendCommand(("MODEL:" + modelName + "\n").toUtf8());
  update();
}

void PillWidget::onModelDownloadClicked() {
  const QString modelName = currentComboModelName();
  if (modelName.isEmpty())
    return;
  if (isCloudModel(modelName)) {
    statusLabel->show();
    currentStatusText = "Cloud providers do not use local downloads";
    statusLabel->setText(currentStatusText);
    updateModelDownloadButton();
    update();
    return;
  }
  if (isModelInstalled(modelName)) {
    statusLabel->show();
    currentStatusText = modelName + " is already installed";
    statusLabel->setText(currentStatusText);
    updateModelDownloadButton();
    update();
    return;
  }
  if (!supportsDirectDownload(modelName)) {
    statusLabel->show();
    currentStatusText = "Direct download is not available";
    statusLabel->setText(currentStatusText);
    updateModelDownloadButton();
    update();
    return;
  }
  statusLabel->show();
  currentStatusText = "Downloading " + modelName + "...";
  statusLabel->setText(currentStatusText);
  if (m_localModelManager)
    m_localModelManager->downloadModel(modelName);
  updateModelDownloadButton();
  update();
}

void PillWidget::checkStartup() {
  QSettings s("QuickSTT", "Config");
  if (!s.value("startupChecked", false).toBool()) {
    if (QMessageBox::question(0, "QuickSTT", "Startup?") == QMessageBox::Yes)
      toggleStartup(true);
    s.setValue("startupChecked", true);
  }
}
void PillWidget::setCustomOpacity(int val) {
  activeOpacity = val;
  setWindowOpacity(val / 100.0);
}
void PillWidget::updateBlink() {
  blinkState = !blinkState;
  update();
}
void PillWidget::contextMenuEvent(QContextMenuEvent *e) { openDashboard(); }
void PillWidget::mouseDoubleClickEvent(QMouseEvent *e) {
  if (childAt(e->pos()) == redDot) {
    onCloseClicked();
    return;
  }
  QWidget::mouseDoubleClickEvent(e);
}
void PillWidget::mousePressEvent(QMouseEvent *e) {
  if (e->button() == Qt::LeftButton) {
    if (m_widgetFlexible && m_resizeEdge != 0) {
      m_isResizing = true;
      m_resizeStartPos = e->globalPosition().toPoint();
      m_resizeStartTopLeft = frameGeometry().topLeft();
      m_resizeStartSize = size();
    } else {
      dragPos = e->globalPosition().toPoint() - frameGeometry().topLeft();
    }
  }
}
void PillWidget::mouseReleaseEvent(QMouseEvent *e) {
  if (e->button() == Qt::LeftButton) {
    if (m_isResizing) {
      m_isResizing = false;
      // Save new dimensions
      QSettings s("QuickSTT", "Config");
      s.setValue("pillWidth", width());
      s.setValue("pillHeight", height());
    }
  }
  QWidget::mouseReleaseEvent(e);
}
void PillWidget::mouseMoveEvent(QMouseEvent *e) {
  if (m_widgetFlexible) {
    int margin = 10;
    int edge = 0; // 0=none, 1=right, 2=bottom, 3=bottom-right, 4=top, 5=top-right

    if (!m_isResizing) {
      bool right = e->pos().x() >= width() - margin;
      bool top = e->pos().y() <= margin;
      bool bottom = e->pos().y() >= height() - margin;

      if (right && top)
        edge = 5;
      else if (right && bottom)
        edge = 3;
      else if (right)
        edge = 1;
      else if (top)
        edge = 4;
      else if (bottom)
        edge = 2;

      m_resizeEdge = edge;

      if (edge == 5)
        setCursor(Qt::SizeBDiagCursor);
      else if (edge == 3)
        setCursor(Qt::SizeFDiagCursor);
      else if (edge == 1)
        setCursor(Qt::SizeHorCursor);
      else if (edge == 2 || edge == 4)
        setCursor(Qt::SizeVerCursor);
      else
        setCursor(Qt::ArrowCursor);
    } else {
      QPoint diff = e->globalPosition().toPoint() - m_resizeStartPos;
      int newW = m_resizeStartSize.width();
      int newH = m_resizeStartSize.height();
      QPoint newTopLeft = m_resizeStartTopLeft;

      if (m_resizeEdge == 1 || m_resizeEdge == 3 || m_resizeEdge == 5)
        newW = qMax(200, m_resizeStartSize.width() + diff.x());
      if (m_resizeEdge == 2 || m_resizeEdge == 3)
        newH = qMax(40, m_resizeStartSize.height() + diff.y());
      if (m_resizeEdge == 4 || m_resizeEdge == 5) {
        newH = qMax(40, m_resizeStartSize.height() - diff.y());
        const int fixedBottom =
            m_resizeStartTopLeft.y() + m_resizeStartSize.height();
        newTopLeft.setY(fixedBottom - newH);
      }

      if (newTopLeft != frameGeometry().topLeft())
        move(newTopLeft);
      customResize(newW, newH, pillRadius);
      return;
    }
  }

  if ((e->buttons() & Qt::LeftButton) && !m_isResizing) {
    move(e->globalPosition().toPoint() - dragPos);
  }
}

void PillWidget::updateWaveformFrame() {
  QList<float> targetLevels = waveformTargetLevels;
  while (targetLevels.size() < m_waveformHistoryLimit)
    targetLevels.prepend(0.0f);
  while (targetLevels.size() > m_waveformHistoryLimit)
    targetLevels.removeFirst();

  if (waveformDisplayLevels.size() != targetLevels.size())
    waveformDisplayLevels = targetLevels;

  bool changed = false;
  for (int i = 0; i < targetLevels.size(); ++i) {
    float target = (isListening && showWaveform && canShowWaveform)
                       ? targetLevels[i]
                       : 0.0f;
    float current = waveformDisplayLevels.value(i, 0.0f);
    float speed = target > current ? 0.74f : 0.46f;
    float next = current + (target - current) * speed;
    if (std::fabs(next - current) > 0.002f)
      changed = true;
    if (next < 0.003f)
      next = 0.0f;
    waveformDisplayLevels[i] = next;
  }

  if (changed)
    update();
}

void PillWidget::sendBackendCommand(const QByteArray &command) {
  if (!backendProcess)
    return;

  QByteArray normalized = command;
  if (!normalized.endsWith('\n'))
    normalized.append('\n');

  static const QList<QByteArray> replacePrefixes = {
      "MODEL:",      "DOWNLOAD:", "WAKEWORDS:",
      "CLOSEWORDS:", "WAKEMODE:", "SET_REC_DIR:",
      "TRANSCRIBE_MODE:"};

  for (const QByteArray &prefix : replacePrefixes) {
    if (normalized.startsWith(prefix)) {
      for (int i = m_pendingBackendCommands.size() - 1; i >= 0; --i) {
        if (m_pendingBackendCommands[i].startsWith(prefix))
          m_pendingBackendCommands.removeAt(i);
      }
      break;
    }
  }

  if (backendProcess->state() == QProcess::Running) {
    backendProcess->write(normalized);
    return;
  }

  if (!m_pendingBackendCommands.contains(normalized))
    m_pendingBackendCommands.append(normalized);

  startBackend();
}

void PillWidget::refreshModelCombo() {
  if (!modelCombo)
    return;

  QSettings settings("QuickSTT", "Config");
  QStringList configuredModels = settings.value("widgetModels").toStringList();
  configuredModels.append(settings.value("cloudWidgetModels").toStringList());
  configuredModels.append(settings.value("favoriteModels").toStringList());
  configuredModels.append(allComboModels());

  QSet<QString> seenModels;
  QStringList favs;
  for (const QString &name : configuredModels) {
    const QString rawName = normalizeModelName(name);
    if (rawName.isEmpty() || seenModels.contains(rawName))
      continue;
    seenModels.insert(rawName);
    favs << rawName;
  }
  QString selectedModelSetting =
      normalizeModelName(settings.value("selectedModel").toString());
  if (isCloudModel(selectedModelSetting) && !favs.contains(selectedModelSetting))
    favs << selectedModelSetting;
  if (favs.isEmpty())
    favs = {"Vosk Small En", "Vosk Large En"};

  QString preferredModel =
      normalizeModelName(settings.value("selectedModel").toString());
  if (preferredModel.isEmpty())
    preferredModel = QStringLiteral("Vosk Small En");
  if (preferredModel.isEmpty())
    preferredModel = m_currentModelName;
  if (preferredModel.isEmpty())
    preferredModel = currentComboModelName();

  QSignalBlocker blocker(modelCombo);
  modelCombo->clear();

  int preferredIndex = -1;
  int firstInstalledIndex = -1;
  bool preferredInstalled = false;
  for (const QString &modelName : favs) {
    bool installed = isModelInstalled(modelName);
    int index = modelCombo->count();
    modelCombo->addItem(buildModelDisplayText(modelName, installed), modelName);
    modelCombo->setItemData(index, buildModelTooltip(modelName, installed),
                            Qt::ToolTipRole);
    if (firstInstalledIndex < 0 && installed)
      firstInstalledIndex = index;
    if (modelName == preferredModel) {
      preferredIndex = index;
      preferredInstalled = installed;
    }
  }

  if (preferredIndex >= 0 && (preferredInstalled || firstInstalledIndex < 0)) {
    modelCombo->setCurrentIndex(preferredIndex);
  } else if (firstInstalledIndex >= 0) {
    modelCombo->setCurrentIndex(firstInstalledIndex);
  } else if (modelCombo->count() > 0) {
    modelCombo->setCurrentIndex(0);
  }

  m_currentModelName = currentComboModelName();
  if (!m_currentModelName.isEmpty())
    settings.setValue("selectedModel", m_currentModelName);
  modelCombo->setToolTip(buildModelTooltip(
      m_currentModelName, isModelInstalled(m_currentModelName)));
  updateModelDownloadButton();
}

void PillWidget::updateModelDownloadButton() {
  if (!modelDownloadBtn)
    return;

  const QString modelName = currentComboModelName();
  const bool showButton = !modelName.isEmpty() &&
                          !isCloudModel(modelName) &&
                          !isModelInstalled(modelName) &&
                          supportsDirectDownload(modelName) && !isDownloading;
  const bool visibilityChanged = modelDownloadBtn->isVisible() != showButton;
  modelDownloadBtn->setVisible(showButton);
  if (showButton)
    modelDownloadBtn->setToolTip("Download " + modelName);
  if (visibilityChanged)
    customResize(pillWidth, pillHeight, pillRadius);
}

bool PillWidget::isModelInstalled(const QString &modelName) const {
  if (modelName.trimmed().isEmpty())
    return false;
  if (isCloudModel(modelName))
    return isCloudModelConfigured(modelName);
  return isLocalModelInstalled(modelName);
}

QString PillWidget::currentComboModelName() const {
  if (!modelCombo)
    return QString();

  QString rawName = modelCombo->currentData().toString().trimmed();
  if (!rawName.isEmpty())
    return rawName;
  return normalizeModelName(modelCombo->currentText());
}

QString PillWidget::baseBackendModelName() const {
  return QStringLiteral("Vosk Small En");
}

bool PillWidget::containsConfiguredCloseWord(const QString &text) const {
  const QString lowered = text.trimmed().toLower();
  if (lowered.isEmpty())
    return false;

  QSettings settings("QuickSTT", "Config");
  QStringList closeWords =
      settings.value("closeWords",
                     QStringList{"stop listening", "go to sleep"})
          .toStringList();
  for (const QString &closeWord : closeWords) {
    const QString normalized = closeWord.trimmed().toLower();
    if (!normalized.isEmpty() && lowered.contains(normalized))
      return true;
  }
  return false;
}

void PillWidget::processRecognizedText(const QString &text, bool fromCloud) {
  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty())
    return;

  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  if (trimmed == m_lastHandledTranscript &&
      (nowMs - m_lastHandledTranscriptMs) < 800) {
    qDebug() << "[DEDUP] Skipping duplicate:" << trimmed;
    return;
  }
  m_lastHandledTranscript = trimmed;
  m_lastHandledTranscriptMs = nowMs;

  if (containsConfiguredCloseWord(trimmed)) {
    if (textBoardWindow)
      textBoardWindow->appendText(trimmed);
    sendBackendCommand("SLEEP\n");
    currentStatusText = "Hidden";
    statusLabel->show();
    statusLabel->setText(currentStatusText);
    update();
    return;
  }

  if (m_smartLifeManager) {
    QString smartFeedback;
    if (m_smartLifeManager->handleVoiceCommand(trimmed, &smartFeedback)) {
      if (textBoardWindow)
        textBoardWindow->appendText("SMART: " + trimmed);
      if (!smartFeedback.trimmed().isEmpty())
        showTransientStatus(smartFeedback, 4500);
      return;
    }
  }

  const bool appHasFocus = (QApplication::activeWindow() != nullptr);
  bool isCommand = false;
  QString commandName;
  if (specialCommandsEnabled) {
    WORD vkey = 0;
    if (tryResolveSpecialCommand(trimmed, &vkey, &commandName)) {
      isCommand = true;
      if (!appHasFocus) {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = vkey;
        SendInput(1, &down, sizeof(INPUT));
        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &up, sizeof(INPUT));
        qDebug() << "[COMMAND]" << commandName;
      } else {
        qDebug() << "[COMMAND-SUPPRESSED]" << commandName;
      }

      if (textBoardWindow)
        textBoardWindow->appendText("COMMAND: " + commandName);
    }
  }

  if (!isCommand) {
    if (!appHasFocus) {
      nativeSendText(trimmed + " ");
      qDebug() << (fromCloud ? "[CLOUD-TYPED]" : "[TYPED]") << trimmed;
    } else {
      qDebug() << (fromCloud ? "[CLOUD-UI-ONLY]" : "[UI-ONLY]") << trimmed;
    }

    if (textBoardWindow)
      textBoardWindow->appendText(trimmed);
  }
}

void PillWidget::suppressAutoShowBriefly(int durationMs) {
  m_temporarilySuppressAutoShow = true;
  if (autoShowSuppressTimer) {
    autoShowSuppressTimer->stop();
    autoShowSuppressTimer->start(qMax(250, durationMs));
  }
}

void PillWidget::showTransientStatus(const QString &text, int durationMs) {
  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty() || !statusLabel || !m_transientStatusTimer)
    return;
  m_transientStatusText = trimmed;
  statusLabel->show();
  statusLabel->setText(trimmed);
  m_transientStatusTimer->stop();
  m_transientStatusTimer->start(qMax(1200, durationMs));
  update();
}
