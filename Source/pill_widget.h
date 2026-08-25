#ifndef PILL_WIDGET_H
#define PILL_WIDGET_H

#include "android_tv_manager.h"
#include "cloud_stt_manager.h"
#include "home_assistant_manager.h"
#include "local_frontend_stt_manager.h"
#include "local_model_manager.h"
#include "mainwindow.h"
#include "optional_service_manager.h"
#include "smart_life_manager.h"
#include "text_board.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent> // Added
#include <QComboBox>
#include <QEasingCurve>
#include <QFontMetrics>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSvgRenderer>
#include <QSystemTrayIcon>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>
#include <functional>
#include <windows.h>

// Custom ComboBox to track Open/Close state for Arrow rotation
class PillComboBox : public QComboBox {
  Q_OBJECT
  Q_PROPERTY(qreal arrowRotation READ arrowRotation WRITE setArrowRotation)
public:
  explicit PillComboBox(QWidget *parent = nullptr) : QComboBox(parent) {
    m_arrowAnimation = new QPropertyAnimation(this, "arrowRotation", this);
    m_arrowAnimation->setDuration(180);
    m_arrowAnimation->setEasingCurve(QEasingCurve::OutCubic);
    setMouseTracking(true);
  }
  bool isOpen = false;
  qreal arrowRotation() const { return m_arrowRotation; }
  void setArrowRotation(qreal rotation) {
    m_arrowRotation = rotation;
    update();
  }

signals:
  void popupStateChanged(bool open);

protected:
  void paintEvent(QPaintEvent *event) override {
    QComboBox::paintEvent(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(width() - 15.0, height() / 2.0);
    p.rotate(m_arrowRotation);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor("#FFFFFF"), 2.2, Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    p.drawLine(QPointF(-4.5, -1.5), QPointF(0.0, 3.0));
    p.drawLine(QPointF(0.0, 3.0), QPointF(4.5, -1.5));
  }

  void showPopup() override {
    if (!(QApplication::mouseButtons() & Qt::LeftButton))
      return;
    isOpen = true;
    animateArrowTo(180.0);
    QComboBox::showPopup();
    emit popupStateChanged(true);
  }
  void hidePopup() override {
    isOpen = false;
    QComboBox::hidePopup();
    animateArrowTo(0.0);
    emit popupStateChanged(false);
  }
  void wheelEvent(QWheelEvent *event) override { event->ignore(); }
  void enterEvent(QEnterEvent *event) override {
    updateHoverCursor(mapFromGlobal(QCursor::pos()));
    QComboBox::enterEvent(event);
  }
  void mouseMoveEvent(QMouseEvent *event) override {
    updateHoverCursor(event->pos());
    QComboBox::mouseMoveEvent(event);
  }
  void leaveEvent(QEvent *event) override {
    unsetCursor();
    QComboBox::leaveEvent(event);
  }
  void keyPressEvent(QKeyEvent *event) override {
    switch (event->key()) {
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Down:
    case Qt::Key_Up:
      event->ignore();
      return;
    default:
      break;
    }
    QComboBox::keyPressEvent(event);
  }

private:
  bool isArrowHotspot(const QPoint &pos) const {
    return QRect(width() - 26, 0, 26, height()).contains(pos);
  }

  void updateHoverCursor(const QPoint &pos) {
    setCursor(isArrowHotspot(pos) ? Qt::PointingHandCursor : Qt::ArrowCursor);
  }

  void animateArrowTo(qreal target) {
    if (!m_arrowAnimation)
      return;
    m_arrowAnimation->stop();
    m_arrowAnimation->setStartValue(m_arrowRotation);
    m_arrowAnimation->setEndValue(target);
    m_arrowAnimation->start();
  }

  QPropertyAnimation *m_arrowAnimation = nullptr;
  qreal m_arrowRotation = 0.0;
};

class StatusTextLabel : public QLabel {
public:
  explicit StatusTextLabel(QWidget *parent = nullptr) : QLabel(parent) {
    setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    setTextInteractionFlags(Qt::TextSelectableByMouse);
    setCursor(Qt::IBeamCursor);
    setWordWrap(false);
  }

  void setAvailableRect(const QRect &rect) {
    if (m_availableRect == rect)
      return;
    m_availableRect = rect;
    updateContentGeometry();
  }

  void setText(const QString &text) {
    QLabel::setText(text);
    if (m_textMirrorCallback)
      m_textMirrorCallback(text);
    updateContentGeometry();
  }

  void setTextMirrorCallback(std::function<void(const QString &)> callback) {
    m_textMirrorCallback = callback;
  }

protected:
  void showEvent(QShowEvent *event) override {
    QLabel::showEvent(event);
    updateContentGeometry();
  }

  void changeEvent(QEvent *event) override {
    QLabel::changeEvent(event);
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::StyleChange:
    case QEvent::EnabledChange:
      updateContentGeometry();
      break;
    default:
      break;
    }
  }

private:
  void updateContentGeometry() {
    if (!parentWidget() || !m_availableRect.isValid())
      return;

    if (!isVisible()) {
      setGeometry(QRect(m_availableRect.topLeft(), QSize(0, 0)));
      return;
    }

    const QString visibleText =
        text().isEmpty() ? QStringLiteral(" ") : text();
    const int textWidth = fontMetrics().boundingRect(visibleText).width();
    const int hintWidth = sizeHint().width();
    const int horizontalPadding = 14;
    const int labelWidth = qMax(
        1, qMin(m_availableRect.width(),
                qMax(hintWidth, textWidth + horizontalPadding)));
    const int labelHeight = qMax(
        1, qMin(m_availableRect.height(), qMax(sizeHint().height(), 14)));
    const int labelY =
        m_availableRect.y() + qMax(0, (m_availableRect.height() - labelHeight) / 2);
    setGeometry(m_availableRect.x(), labelY, labelWidth, labelHeight);
  }

  QRect m_availableRect;
  std::function<void(const QString &)> m_textMirrorCallback;
};

class PillWidget : public QWidget {
  Q_OBJECT

public:
  explicit PillWidget(QWidget *parent = nullptr);
  ~PillWidget();
  bool isAutoShowSuppressed() const { return m_temporarilySuppressAutoShow; }

protected:
  void paintEvent(QPaintEvent *event) override;
  void closeEvent(QCloseEvent *event) override; // Added
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;
  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void moveEvent(QMoveEvent *event) override; // Required to drag textboard
  void resizeEvent(QResizeEvent *event) override;
  void hideEvent(QHideEvent *event) override; // Sync textboard
  void showEvent(QShowEvent *event) override; // Sync textboard
  bool eventFilter(QObject *obj, QEvent *event) override;

public slots:
  void centerOnScreen();
  void openDashboard();
  void restoreFromExternalTrigger();
  void showMainWidgetExplicitly();

private slots:
  void toggleTextBoard();
  void repositionTextBoard();
  void onMicClicked();
  void onRedDotClicked();
  void onCloseClicked();
  void onModelChanged(const QString &text);
  void onModelDownloadClicked();
  void onProcessOutput();
  void updateBlink();

  // Watchdog
  void startBackend();
  void onBackendFinished(int exitCode, QProcess::ExitStatus exitStatus);
  void onBackendError(QProcess::ProcessError error);
  void ensureBackendRunning();
  void updateWaveformFrame();

  void onAhkResult(int reqId, bool commandExecuted, const QString &statusText);

  // Dashboard Logic (openDashboard moved to public slots)
  void onSettingChanged(QString key, QVariant val);

  void toggleStartup(bool enable);

private:
  class AhkBridge *m_ahkBridge = nullptr;
  int m_ahkNextReqId = 1;
  QHash<int, QString> m_ahkPendingText;

  // UI Elements
  QPushButton *micBtn;
  QPushButton *redDot;
  QPushButton *closeBtn;
  QPushButton *modelDownloadBtn;
  CollapseButton *textBoardToggleBtn;
  TextBoardWindow *textBoardWindow;
  bool isRecording = false;
  bool m_mp3Recording = false; // True while writing MP3 to disk
  QProcess *mp3RecordProcess = nullptr;
  bool textBoardOpen = true;
  PillComboBox *modelCombo; // Updated Class
  StatusTextLabel *statusLabel;

  MainWindow *dashboard = nullptr;

  QSystemTrayIcon *trayIcon;
  QMenu *trayMenu;
  QProcess *backendProcess;
  QTimer *blinkTimer;
  QTimer *backendHealthTimer;
  QTimer *autoShowSuppressTimer = nullptr;
  bool m_isQuitting = false;
  int m_lastPartialLen = 0;

  bool isListening = false;
  bool blinkState = false;
  QPoint dragPos;
  QRect waveRect;

  // Resizing State
  bool m_widgetFlexible = false;
  bool m_isResizing = false;
  int m_resizeEdge = 0; // 0=none, 1=right, 2=bottom, 3=bottom-right, 4=top, 5=top-right
  QPoint m_resizeStartPos;
  QPoint m_resizeStartTopLeft;
  QSize m_resizeStartSize;

  // Icon Caches
  QPixmap cachedMicActive;
  QPixmap cachedMicInactive;
  void updateCachedIcons();

  // Configs
  int pillWidth = 360;
  int pillHeight = 50;
  int pillRadius = 25;
  int activeOpacity = 100;
  int iconSize = 21; // 30% smaller than original 30px
  int trayIconSize = 32;

  bool showWaveform = true;
  bool specialCommandsEnabled = true;
  int waveformSensitivity = 5; // 1-15: 1=least sensitive, 15=most sensitive
  QTimer *waveformDelayTimer;
  QTimer *waveformAnimationTimer = nullptr;
  bool canShowWaveform = false;
  QList<float> waveformTargetLevels;
  QList<float> waveformDisplayLevels;

  bool isDownloading = false;
  int downloadProgress = 0;
  QString currentStatusText = "";
  QString downloadSpeed = "";
  QString totalDownloadSize = "";
  QString m_currentModelName;
  QList<QByteArray> m_pendingBackendCommands;
  bool m_temporarilySuppressAutoShow = false;
  QTimer *m_transientStatusTimer = nullptr;
  QTimer *m_modelLoadTimeoutTimer = nullptr;
  QString m_transientStatusText;

  // Auto-offload state
  QTimer *m_offloadTimer = nullptr;
  int m_offloadSeconds = 15;
  bool m_autoOffload = true;
  bool m_modelOffloaded = false;

  // Sound Waveform state
  QList<int> audioWaveform;

  void setupTray();
  void updateTrayIcon();
  void applyNativeWindowIcons();
  void checkStartup();
  void customResize(int w, int h, int r);
  void setCustomOpacity(int val);
  void evaluateAutoOffload();
  void sendBackendCommand(const QByteArray &command);
  void refreshModelCombo();
  void updateModelDownloadButton();
  bool isModelInstalled(const QString &modelName) const;
  QString currentComboModelName() const;
  void ensureDashboardCreated();
  void attemptAutoReconnectSmartHome();
  void attemptAutoReconnectAndroidTv();
  void suppressAutoShowBriefly(int durationMs = 2200);
  void showTransientStatus(const QString &text, int durationMs = 5000);
  void processRecognizedText(const QString &text, bool fromCloud);
  void setWidgetStatusText(const QString &text,
                           bool allowWhileListening = false);
  bool isIgnorableRecognitionText(const QString &text) const;
  bool containsConfiguredCloseWord(const QString &text) const;
  QString baseBackendModelName() const;
  QByteArray frontendSegmentationCommandForModel(const QString &modelName) const;

  // Cached SVG Renderers
  QSvgRenderer *m_svgRenMicActive = nullptr;
  QSvgRenderer *m_svgRenMicInactive = nullptr;
  QSvgRenderer *m_svgRenApp = nullptr;
  HICON m_smallWinIcon = nullptr;
  HICON m_bigWinIcon = nullptr;
  int m_waveformHistoryLimit = 64;
  CloudSttManager *m_cloudSttManager = nullptr;
  LocalFrontendSttManager *m_localFrontendSttManager = nullptr;
  LocalModelManager *m_localModelManager = nullptr;
  OptionalServiceManager *m_optionalServiceManager = nullptr;
  SmartLifeManager *m_smartLifeManager = nullptr;
  AndroidTvManager *m_androidTvManager = nullptr;
  HomeAssistantManager *m_homeAssistantManager = nullptr;
  QString m_lastHandledTranscript;
  qint64 m_lastHandledTranscriptMs = 0;
  // Streaming (Nemotron) main-pill paste-as-you-speak: text already typed
  // from committed STREAM_TEXT so FINAL only appends the remaining delta.
  QString m_streamTypedPrefix;
  bool m_smartLifeAutoRestoreAttempted = false;
  bool m_androidTvAutoRestoreAttempted = false;
  QTimer *m_ramCompactTimer = nullptr;
  void compactWorkingSet();

  // Ctrl+Space Popup TCP bridge
  QTcpServer *m_popupServer = nullptr;
  QTcpSocket *m_popupClient = nullptr;
  bool m_popupActive = false;  // True when popup is controlling STT
  // Keep native state events owned by the popup until its final idle event.
  // Without this, a late STATE event reopens the main pill after Ctrl+Space.
  bool m_popupFinalDelivered = false;
  bool m_popupStopRequested = false;
  void initPopupServer();
  void onPopupConnected();
  void onPopupData();
  void forwardEventToPopup(const QString &event, const QString &payload);
  void launchPopupProcess();
  QProcess *m_popupProcess = nullptr;
};

#endif // PILL_WIDGET_H
