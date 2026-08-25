#ifndef GLOBAL_HOTKEY_X11_H
#define GLOBAL_HOTKEY_X11_H

#include <QObject>

#if defined(Q_OS_LINUX) && defined(QUICKSTT_HAS_X11) && QUICKSTT_HAS_X11
#define QUICKSTT_X11_HOTKEYS 1
#endif

#ifdef Q_OS_LINUX

// X11 global hotkey registration (XGrabKey on the root window).
//
//   Hotkey 1 — Ctrl+Shift+Space : toggle dictation session (micro pill)
//   Hotkey 2 — Ctrl+Space       : push-to-talk (press = start, release = stop)
//
// Wayland has no global grab protocol; construction succeeds but grabbing is
// skipped and isActive() stays false so callers can surface a hint.
class GlobalHotkeyX11 : public QObject {
  Q_OBJECT

public:
  static constexpr int kToggleDictation = 1; // Ctrl+Shift+Space
  static constexpr int kPushToTalk = 2;      // Ctrl+Space

  explicit GlobalHotkeyX11(QObject *parent = nullptr);
  ~GlobalHotkeyX11() override;

  bool isActive() const { return m_active; }
  QString failureReason() const { return m_failureReason; }

signals:
  void hotkeyPressed(int hotkeyId);
  void hotkeyReleased(int hotkeyId);

private:
  bool grabKey(int hotkeyId, unsigned int modifiers, int keycode);
  void handleXEvent(unsigned int type, int hotkeyId);

  class Display *m_display = nullptr;
  class QSocketNotifier *m_notifier = nullptr;
  bool m_active = false;
  bool m_pTTEngaged = false;
  bool m_toggleEngaged = false;
  QString m_failureReason;
};

#endif // Q_OS_LINUX
#endif // GLOBAL_HOTKEY_X11_H
