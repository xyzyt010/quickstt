//! Global hotkey via WH_KEYBOARD_LL hook — push-to-talk style.
//! Sends Press on Ctrl+Space down, Release on Ctrl or Space up.
//! Suppresses the key from all other apps (Handy, etc.)

use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;

// Win32 types
type HHOOK = isize;
type LPARAM = isize;
type WPARAM = usize;
type LRESULT = isize;
type DWORD = u32;
type UINT = u32;
type HINSTANCE = isize;
type HMODULE = isize;

const WH_KEYBOARD_LL: i32 = 13;
const WM_KEYDOWN: u32 = 0x0100;
const WM_KEYUP: u32 = 0x0101;
const WM_SYSKEYDOWN: u32 = 0x0104;
const WM_SYSKEYUP: u32 = 0x0105;
const VK_CONTROL: u32 = 0x11;
const VK_SPACE: u32 = 0x20;
const VK_LCONTROL: u32 = 0xA2;
const VK_RCONTROL: u32 = 0xA3;
const LLKHF_INJECTED: u32 = 0x10;

#[repr(C)]
struct KBDLLHOOKSTRUCT {
    vk_code: DWORD,
    scan_code: DWORD,
    flags: DWORD,
    time: DWORD,
    dw_extra_info: usize,
}

#[link(name = "user32")]
extern "system" {
    fn SetWindowsHookExW(idHook: i32, lpfn: usize, hmod: HINSTANCE, dwThreadId: DWORD) -> HHOOK;
    fn UnhookWindowsHookEx(hhk: HHOOK) -> i32;
    fn CallNextHookEx(hhk: HHOOK, nCode: i32, wParam: WPARAM, lParam: LPARAM) -> LRESULT;
    fn GetMessageW(lpMsg: *mut MSG, hWnd: isize, wMsgFilterMin: UINT, wMsgFilterMax: UINT) -> i32;
    fn TranslateMessage(lpMsg: *const MSG) -> i32;
    fn DispatchMessageW(lpMsg: *const MSG) -> LRESULT;
    fn GetAsyncKeyState(vKey: i32) -> i16;
}

#[link(name = "kernel32")]
extern "system" {
    fn GetModuleHandleW(lpModuleName: *const u16) -> HMODULE;
}

#[repr(C)]
#[derive(Copy, Clone)]
struct MSG {
    hwnd: isize,
    message: u32,
    wParam: usize,
    lParam: isize,
    time: u32,
    pt_x: i32,
    pt_y: i32,
    lprivate: u32,
}

/// Events sent from the hook thread to the main app
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum HotkeyEvent {
    Press,   // Ctrl+Space held down → start recording
    Release, // Ctrl or Space released → stop recording
}

// Global state for the hook callback
static HOOK_ACTIVE: AtomicBool = AtomicBool::new(false);
static mut SENDER: Option<mpsc::Sender<HotkeyEvent>> = None;
static mut CTRL_HELD: bool = false;
static mut SPACE_HELD: bool = false;
static mut COMBO_ACTIVE: bool = false; // True while Ctrl+Space combo is engaged

unsafe extern "system" fn keyboard_hook_proc(
    n_code: i32,
    w_param: WPARAM,
    l_param: LPARAM,
) -> LRESULT {
    if n_code >= 0 && HOOK_ACTIVE.load(Ordering::SeqCst) {
        let kb = &*(l_param as *const KBDLLHOOKSTRUCT);
        let vk = kb.vk_code;
        let is_injected = (kb.flags & LLKHF_INJECTED) != 0;
        let is_down = w_param == WM_KEYDOWN as usize || w_param == WM_SYSKEYDOWN as usize;
        let is_up = w_param == WM_KEYUP as usize || w_param == WM_SYSKEYUP as usize;

        // Track Ctrl
        if vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL {
            if is_down {
                CTRL_HELD = true;
                let space_down = SPACE_HELD || ((GetAsyncKeyState(VK_SPACE as i32) as u16 & 0x8000) != 0);
                if space_down && !COMBO_ACTIVE {
                    SPACE_HELD = true;
                    COMBO_ACTIVE = true;
                    eprintln!("[POPUP] Ctrl+Space engaged (Ctrl pressed) -> Press");
                    if let Some(ref tx) = SENDER {
                        let _ = tx.send(HotkeyEvent::Press);
                    }
                    return 1;
                }
            } else if is_up {
                CTRL_HELD = false;
                // If combo was active, Ctrl release = stop recording
                if COMBO_ACTIVE {
                    COMBO_ACTIVE = false;
                    eprintln!("[POPUP] Ctrl released -> Release");
                    if let Some(ref tx) = SENDER {
                        let _ = tx.send(HotkeyEvent::Release);
                    }
                    return 1; // Suppress
                }
            }
        }

        // Track Space
        if vk == VK_SPACE && !is_injected {
            if is_down && SPACE_HELD {
                return 1;
            } else if is_up && SPACE_HELD {
                let should_release = COMBO_ACTIVE;
                SPACE_HELD = false;
                COMBO_ACTIVE = false;
                if should_release {
                    eprintln!("[POPUP] Space released -> Release");
                    if let Some(ref tx) = SENDER {
                        let _ = tx.send(HotkeyEvent::Release);
                    }
                }
                return 1;
            } else if is_down {
                let ctrl_down = CTRL_HELD
                    || ((GetAsyncKeyState(VK_CONTROL as i32) as u16 & 0x8000) != 0)
                    || ((GetAsyncKeyState(VK_LCONTROL as i32) as u16 & 0x8000) != 0)
                    || ((GetAsyncKeyState(VK_RCONTROL as i32) as u16 & 0x8000) != 0);
                if ctrl_down && !COMBO_ACTIVE {
                    SPACE_HELD = true;
                    CTRL_HELD = true;
                    COMBO_ACTIVE = true;
                    eprintln!("[POPUP] Ctrl+Space engaged (Space pressed) -> Press");
                    if let Some(ref tx) = SENDER {
                        let _ = tx.send(HotkeyEvent::Press);
                    }
                    return 1; // Suppress
                }
            }
        }

        // Suppress Space when combo is active (auto-repeat)
        if vk == VK_SPACE && COMBO_ACTIVE {
            return 1;
        }
    }

    CallNextHookEx(0, n_code, w_param, l_param)
}

/// Runs the keyboard hook message loop with automatic self-healing watchdog.
pub fn run_hotkey_loop(tx: mpsc::Sender<HotkeyEvent>) {
    unsafe {
        SENDER = Some(tx);
        HOOK_ACTIVE.store(true, Ordering::SeqCst);

        let h_instance = GetModuleHandleW(ptr::null());
        loop {
            let hook = SetWindowsHookExW(
                WH_KEYBOARD_LL,
                keyboard_hook_proc as *const () as usize,
                h_instance,
                0,
            );

            if hook == 0 {
                eprintln!("[POPUP] Warning: Failed to install keyboard hook, retrying in 2s...");
                std::thread::sleep(std::time::Duration::from_secs(2));
                continue;
            }
            eprintln!("[POPUP] Keyboard hook active (Ctrl+Space push-to-talk)");

            let mut msg: MSG = std::mem::zeroed();
            while GetMessageW(&mut msg, 0, 0, 0) > 0 {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            UnhookWindowsHookEx(hook);
            eprintln!("[POPUP] Keyboard hook lost or reset — auto-healing...");
            std::thread::sleep(std::time::Duration::from_millis(500));
        }
    }
}
