#include "global_hotkey_x11.h"

#ifdef QUICKSTT_X11_HOTKEYS

#include <QSocketNotifier>
#include <qplatformdefs.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <cerrno>

namespace {
constexpr unsigned int kLockMaskCombinations[] = {
    0, LockMask, Mod2Mask, Mod5Mask, LockMask | Mod2Mask,
    LockMask | Mod5Mask, Mod2Mask | Mod5Mask, LockMask | Mod2Mask | Mod5Mask};
constexpr int kLockComboCount =
    int(sizeof(kLockMaskCombinations) / sizeof(kLockMaskCombinations[0]));
} // namespace

GlobalHotkeyX11::GlobalHotkeyX11(QObject *parent) : QObject(parent) {
  const QByteArray sessionType = qgetenv("XDG_SESSION_TYPE").toLower();
  const QByteArray platform = qgetenv("QT_QPA_PLATFORM").toLower();
  if (sessionType.contains("wayland") || platform.contains("wayland")) {
    m_failureReason = QStringLiteral(
        "Wayland session: bind a compositor shortcut to "
        "`quickstt-app --toggle-dictation` for global hotkey support");
    return;
  }

  m_display = XOpenDisplay(nullptr);
  if (!m_display) {
    m_failureReason = QStringLiteral("X11 display unavailable for global hotkeys");
    return;
  }

  Window root = DefaultRootWindow(m_display);
  const int spaceCode = XKeysymToKeycode(m_display, XK_space);
  if (spaceCode == 0) {
    m_failureReason = QStringLiteral("Space keycode unavailable on this keymap");
    XCloseDisplay(m_display);
    m_display = nullptr;
    return;
  }

  XErrorHandler previousHandler = XSetErrorHandler([](Display *, XErrorEvent *error) -> int {
    // BadAccess = combo already grabbed by another app — silently ignore so
    // the remaining lock-mask variants keep registering.
    return error->error_code == BadAccess ? 0 : 0;
  });

  bool grabbedToggle = false;
  bool grabbedPtt = false;
  for (int i = 0; i < kLockComboCount; ++i) {
    const unsigned int locks = kLockMaskCombinations[i];
    if (grabKey(kToggleDictation, ControlMask | ShiftMask | locks, spaceCode))
      grabbedToggle = true;
    if (grabKey(kPushToTalk, ControlMask | locks, spaceCode))
      grabbedPtt = true;
  }
  XSetErrorHandler(previousHandler);
  XSync(m_display, False);

  if (!grabbedToggle && !grabbedPtt) {
    m_failureReason = QStringLiteral(
        "Ctrl+Shift+Space is already captured by another application");
    XCloseDisplay(m_display);
    m_display = nullptr;
    return;
  }

  m_active = true;
  m_notifier = new QSocketNotifier(XConnectionNumber(m_display),
                                   QSocketNotifier::Read, this);
  connect(m_notifier, &QSocketNotifier::activated, this, [this]() {
    while (XPending(m_display) > 0) {
      XEvent event;
      XNextEvent(m_display, &event);
      if (event.type == KeyPress)
        handleXEvent(KeyPress, int(event.xkey.state));
      else if (event.type == KeyRelease)
        handleXEvent(KeyRelease, int(event.xkey.state));
    }
  });
}

bool GlobalHotkeyX11::grabKey(int hotkeyId, unsigned int modifiers,
                              int keycode) {
  if (!m_display)
    return false;
  Window root = DefaultRootWindow(m_display);
  const int result =
      XGrabKey(m_display, keycode, modifiers, root, True, GrabModeAsync,
               GrabModeAsync);
  Q_UNUSED(hotkeyId);
  return result == Success;
}

void GlobalHotkeyX11::handleXEvent(unsigned int type, int state) {
  const bool shiftHeld = (state & ShiftMask) != 0;
  const bool ctrlHeld = (state & ControlMask) != 0;

  if (type == KeyPress) {
    // Distinguish the two grabs by the modifier state carried in the event.
    const int hotkeyId = (shiftHeld && ctrlHeld) ? kToggleDictation : kPushToTalk;
    if (hotkeyId == kToggleDictation) {
      if (m_toggleEngaged)
        return;
      m_toggleEngaged = true;
      emit hotkeyPressed(kToggleDictation);
    } else if (ctrlHeld) {
      if (m_pTTEngaged)
        return;
      m_pTTEngaged = true;
      emit hotkeyPressed(kPushToTalk);
    }
    return;
  }

  // KeyRelease — we only grabbed Space, so Space-up is our release trigger
  // (mirrors the Windows popup: lifting the key stops push-to-talk).
  if (m_toggleEngaged) {
    m_toggleEngaged = false;
    emit hotkeyReleased(kToggleDictation);
  } else if (m_pTTEngaged) {
    m_pTTEngaged = false;
    emit hotkeyReleased(kPushToTalk);
  }
}

GlobalHotkeyX11::~GlobalHotkeyX11() {
  if (m_display) {
    Window root = DefaultRootWindow(m_display);
    if (const int spaceCode = XKeysymToKeycode(m_display, XK_space)) {
      for (int i = 0; i < kLockComboCount; ++i) {
        const unsigned int locks = kLockMaskCombinations[i];
        XUngrabKey(m_display, spaceCode, ControlMask | ShiftMask | locks, root);
        XUngrabKey(m_display, spaceCode, ControlMask | locks, root);
      }
    }
    XSync(m_display, False);
    XCloseDisplay(m_display);
  }
}

#endif // QUICKSTT_X11_HOTKEYS
