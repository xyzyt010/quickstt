#include "global_hotkey_x11.h"

#ifdef QUICKSTT_X11_HOTKEYS

#include <QSocketNotifier>

#include <X11/Xlib.h>
#include <X11/keysym.h>

namespace {
constexpr unsigned int kLockMaskCombinations[] = {
    0, LockMask, Mod2Mask, Mod5Mask, LockMask | Mod2Mask,
    LockMask | Mod5Mask, Mod2Mask | Mod5Mask, LockMask | Mod2Mask | Mod5Mask};
constexpr int kLockComboCount =
    int(sizeof(kLockMaskCombinations) / sizeof(kLockMaskCombinations[0]));

inline Display *asDisplay(void *handle) {
  return static_cast<Display *>(handle);
}
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

  Display *display = XOpenDisplay(nullptr);
  if (!display) {
    m_failureReason =
        QStringLiteral("X11 display unavailable for global hotkeys");
    return;
  }
  m_display = display;

  Window root = DefaultRootWindow(display);
  const int spaceCode = XKeysymToKeycode(display, XK_space);
  if (spaceCode == 0) {
    m_failureReason = QStringLiteral("Space keycode unavailable on this keymap");
    XCloseDisplay(display);
    m_display = nullptr;
    return;
  }

  XErrorHandler previousHandler =
      XSetErrorHandler([](Display *, XErrorEvent *error) -> int {
        // BadAccess = combo already grabbed by another app — ignore so the
        // remaining lock-mask variants keep registering.
        (void)error;
        return 0;
      });

  bool anyGrabbed = false;
  for (int i = 0; i < kLockComboCount; ++i) {
    const unsigned int locks = kLockMaskCombinations[i];
    // Best effort: ignore BadAccess (already grabbed by IBus etc.) for individual masks.
    if (grabKey(kToggleDictation, ControlMask | ShiftMask | locks, spaceCode))
      anyGrabbed = true;
    if (grabKey(kPushToTalk, ControlMask | locks, spaceCode))
      anyGrabbed = true;
  }
  XSetErrorHandler(previousHandler);
  XSync(display, False);

  if (!anyGrabbed) {
    m_failureReason = QStringLiteral(
        "Global hotkeys already grabbed (Mint IBus uses Ctrl+Space — disable IBus Ctrl+Space in Language Support → Keyboard input method → IBus Preferences → General → Next input method)");
    XCloseDisplay(display);
    m_display = nullptr;
    return;
  }
  m_active = true;
  m_notifier =
      new QSocketNotifier(XConnectionNumber(display), QSocketNotifier::Read,
                          this);
  connect(m_notifier, &QSocketNotifier::activated, this, [this]() {
    Display *d = asDisplay(m_display);
    if (!d)
      return;
    while (XPending(d) > 0) {
      XEvent event;
      XNextEvent(d, &event);
      if (event.type == KeyPress)
        handleXEvent(KeyPress, int(event.xkey.state));
      else if (event.type == KeyRelease)
        handleXEvent(KeyRelease, int(event.xkey.state));
    }
  });
}

bool GlobalHotkeyX11::grabKey(int hotkeyId, unsigned int modifiers,
                              int keycode) {
  Display *display = asDisplay(m_display);
  if (!display)
    return false;
  Window root = DefaultRootWindow(display);
  const int result =
      XGrabKey(display, keycode, modifiers, root, True, GrabModeAsync,
               GrabModeAsync);
  Q_UNUSED(hotkeyId);
  return result == Success;
}

void GlobalHotkeyX11::handleXEvent(unsigned int type, int state) {
  const bool shiftHeld = (state & ShiftMask) != 0;
  const bool ctrlHeld = (state & ControlMask) != 0;

  if (type == KeyPress) {
    // The two grabs are distinguished by the modifier state in the event:
    // only the Ctrl+Shift+Space grab delivers shift+ctrl presses.
    if (shiftHeld && ctrlHeld) {
      if (m_toggleEngaged)
        return; // auto-repeat
      m_toggleEngaged = true;
      emit hotkeyPressed(kToggleDictation);
    } else if (ctrlHeld) {
      if (m_pTTEngaged)
        return; // auto-repeat
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
  Display *display = asDisplay(m_display);
  if (display) {
    Window root = DefaultRootWindow(display);
    if (const int spaceCode = XKeysymToKeycode(display, XK_space)) {
      for (int i = 0; i < kLockComboCount; ++i) {
        const unsigned int locks = kLockMaskCombinations[i];
        XUngrabKey(display, spaceCode, ControlMask | ShiftMask | locks, root);
        XUngrabKey(display, spaceCode, ControlMask | locks, root);
      }
    }
    XSync(display, False);
    XCloseDisplay(display);
  }
}

#endif // QUICKSTT_X11_HOTKEYS
