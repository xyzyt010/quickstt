//! Text injection via Windows SendInput API + clipboard fallback.
//! Ultra-fast, instantaneous pasting without unnecessary sleeps.

use windows::Win32::UI::Input::KeyboardAndMouse::{
    SendInput, INPUT, INPUT_0, INPUT_KEYBOARD, KEYBDINPUT, KEYEVENTF_KEYUP, KEYEVENTF_UNICODE,
};

/// Types text directly into the active window using SendInput Unicode events.
pub fn type_text_live(text: &str) {
    if text.is_empty() {
        return;
    }
    eprintln!("[POPUP] Live-typing {} chars via SendInput", text.len());

    let mut inputs: Vec<INPUT> = Vec::with_capacity(text.len() * 2);
    for ch in text.encode_utf16() {
        // Key down
        inputs.push(INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 {
                ki: KEYBDINPUT {
                    wVk: Default::default(),
                    wScan: ch,
                    dwFlags: KEYEVENTF_UNICODE,
                    time: 0,
                    dwExtraInfo: 0,
                },
            },
        });
        // Key up
        inputs.push(INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 {
                ki: KEYBDINPUT {
                    wVk: Default::default(),
                    wScan: ch,
                    dwFlags: KEYEVENTF_UNICODE | KEYEVENTF_KEYUP,
                    time: 0,
                    dwExtraInfo: 0,
                },
            },
        });
    }

    if !inputs.is_empty() {
        unsafe {
            SendInput(&inputs, std::mem::size_of::<INPUT>() as i32);
        }
    }
}

/// Copies text to clipboard only (no typing).
pub fn copy_only(text: &str) {
    if text.is_empty() {
        return;
    }
    eprintln!("[POPUP] Copy-only mode: {} chars", text.len());
    copy_to_clipboard(text);
}

/// Types the given text into the active window instantaneously using clipboard paste (Ctrl+V).
pub fn type_text(text: &str) {
    if text.is_empty() {
        return;
    }

    eprintln!("[POPUP] Instant-Typing {} chars into active window", text.len());

    // 1. Copy to clipboard immediately
    copy_to_clipboard(text);

    // 2. Release modifier keys so physical Ctrl/Shift/Alt doesn't interfere
    release_modifiers();

    // 3. Ultra-fast paste with tiny 10ms settling gap
    std::thread::sleep(std::time::Duration::from_millis(10));
    paste_from_clipboard();
}

/// Copies text to the Windows clipboard using Win32 API.
fn copy_to_clipboard(text: &str) {
    use windows::Win32::Foundation::HANDLE;
    use windows::Win32::System::DataExchange::{
        CloseClipboard, EmptyClipboard, OpenClipboard, SetClipboardData,
    };
    use windows::Win32::System::Memory::{GlobalAlloc, GlobalLock, GlobalUnlock, GMEM_MOVEABLE};

    unsafe {
        if OpenClipboard(None).is_err() {
            eprintln!("[POPUP] Failed to open clipboard");
            return;
        }

        let _ = EmptyClipboard();

        let wide: Vec<u16> = text.encode_utf16().chain(std::iter::once(0)).collect();
        let byte_len = wide.len() * 2;

        let hmem = GlobalAlloc(GMEM_MOVEABLE, byte_len);
        if let Ok(h) = hmem {
            let ptr = GlobalLock(h);
            if !ptr.is_null() {
                std::ptr::copy_nonoverlapping(wide.as_ptr() as *const u8, ptr as *mut u8, byte_len);
                let _ = GlobalUnlock(h);
                let _ = SetClipboardData(13, HANDLE(h.0 as *mut _));
            }
        }

        let _ = CloseClipboard();
        eprintln!("[POPUP] Copied to clipboard: {} chars", text.len());
    }
}

/// Releases modifier keys so SendInput characters aren't interpreted as shortcuts.
fn release_modifiers() {
    use windows::Win32::UI::Input::KeyboardAndMouse::{
        VK_CONTROL, VK_LCONTROL, VK_LWIN, VK_MENU, VK_RCONTROL, VK_RWIN, VK_SHIFT,
    };

    let keys = [
        VK_LCONTROL,
        VK_RCONTROL,
        VK_CONTROL,
        VK_SHIFT,
        VK_MENU,
        VK_LWIN,
        VK_RWIN,
    ];
    let mut inputs: Vec<INPUT> = Vec::new();

    for vk in keys {
        inputs.push(INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 {
                ki: KEYBDINPUT {
                    wVk: vk,
                    wScan: 0,
                    dwFlags: KEYEVENTF_KEYUP,
                    time: 0,
                    dwExtraInfo: 0,
                },
            },
        });
    }

    unsafe {
        SendInput(&inputs, std::mem::size_of::<INPUT>() as i32);
    }
}

/// Simulates Ctrl+V to paste from clipboard into the active window.
fn paste_from_clipboard() {
    use windows::Win32::UI::Input::KeyboardAndMouse::VK_CONTROL;

    let vk_v = windows::Win32::UI::Input::KeyboardAndMouse::VIRTUAL_KEY(0x56); // 'V' key

    let inputs = [
        // Ctrl down
        INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 {
                ki: KEYBDINPUT {
                    wVk: VK_CONTROL,
                    wScan: 0,
                    dwFlags: Default::default(),
                    time: 0,
                    dwExtraInfo: 0,
                },
            },
        },
        // V down
        INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 {
                ki: KEYBDINPUT {
                    wVk: vk_v,
                    wScan: 0,
                    dwFlags: Default::default(),
                    time: 0,
                    dwExtraInfo: 0,
                },
            },
        },
        // V up
        INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 {
                ki: KEYBDINPUT {
                    wVk: vk_v,
                    wScan: 0,
                    dwFlags: KEYEVENTF_KEYUP,
                    time: 0,
                    dwExtraInfo: 0,
                },
            },
        },
        // Ctrl up
        INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 {
                ki: KEYBDINPUT {
                    wVk: VK_CONTROL,
                    wScan: 0,
                    dwFlags: KEYEVENTF_KEYUP,
                    time: 0,
                    dwExtraInfo: 0,
                },
            },
        },
    ];

    unsafe {
        SendInput(&inputs, std::mem::size_of::<INPUT>() as i32);
        eprintln!("[POPUP] Instant Ctrl+V paste executed");
    }
}
