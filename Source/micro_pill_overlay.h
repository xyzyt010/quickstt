#ifndef MICRO_PILL_OVERLAY_H
#define MICRO_PILL_OVERLAY_H

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QTcpSocket>
#include <QWidget>

class QLabel;
class QTimer;

// Compact bottom-center dictation overlay (Linux port of the Windows
// quickstt_popup micro pill). Connects to the C++ pill widget's TCP bridge on
// 127.0.0.1:19876 and speaks the identical JSON protocol:
//   sends  {"cmd":"popup_start"|"popup_stop"|"popup_sleep"}
//   recv   CONFIG / MODEL_CAP / STATE / AUDIO_LEVEL / PARTIAL_TEXT /
//          FINAL_TEXT / STREAM_TEXT events.
//
// Visual states: hidden → idle pill → recording (waveform) → transcribing
// (animated dots) → brief done tick → hidden. Recognized text is delivered to
// the focused window via xdotool (X11) or the clipboard, mirroring the popup
// output modes (0=type, 1=clipboard, 2=none).
class MicroPillOverlay : public QWidget {
  Q_OBJECT

public:
  explicit MicroPillOverlay(QWidget *parent = nullptr);
  ~MicroPillOverlay() override;

  bool sessionActive() const;
  bool isConnected() const;

public slots:
  void startSession();  // show overlay + popup_start
  void stopSession();   // popup_stop (waits for FINAL_TEXT)
  void cancelSession(); // popup_sleep + immediate hide
  void applyConfig(int mode, int output, bool alwaysOnPill);

signals:
  void sessionFinished(); // emitted after final text was delivered

protected:
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;

private slots:
  void onConnected();
  void onData();
  void onDisconnected();
  void onAnimationTick();

private:
  enum class Phase { Hidden, Idle, Recording, Transcribing, Done, NoModel };

  void sendCommand(const QString &command);
  void handleEvent(const QJsonObject &obj);
  void setPhase(Phase phase);
  void repositionToScreenBottom();
  void deliverTextDelta(const QString &delta);
  void finishSession(bool success);

  QTcpSocket *m_socket = nullptr;
  QTimer *m_animTimer = nullptr;
  QTimer *m_doneHideTimer = nullptr;
  Phase m_phase = Phase::Hidden;

  // Config received over CONFIG event
  int m_activationMode = 0; // 0=push-to-talk, 1=toggle
  int m_outputMode = 0;     // 0=type, 1=clipboard, 2=none
  bool m_streamingCap = false;
  bool m_alwaysOnPill = true;

  // Text delivery bookkeeping
  QString m_streamTypedPrefix;
  bool m_finalDelivered = false;

  // Waveform rendering state
  QList<float> m_targetLevels;
  QList<float> m_displayLevels;
  qreal m_transcribeAnim = 0.0;
  QPixmap m_micIcon;
  QString m_statusText;

  QPoint m_dragOffset;
  bool m_dragging = false;
  bool m_isQuittingContext = false;
};

#endif // MICRO_PILL_OVERLAY_H
