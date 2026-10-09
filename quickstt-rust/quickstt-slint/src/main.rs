#![windows_subsystem = "windows"]

mod autostart;
mod widget_platform;

slint::include_modules!();

use quickstt_core::error::QuickSttResult;
use quickstt_core::orchestration::{AppMode, AppOrchestrator, AppState, OrchestratorCommand};
use slint::{Model, ModelRc, Timer, TimerMode, VecModel};
use std::sync::{Arc, Mutex};

const IPC_PORT: u16 = 47631;
// MiniPill sizes, LOGICAL px (physical-px math scales them; Slint layout
// mirrors them). Bottom dock: thin 33x8 bar, expands to a roomy
// 240x86 pair (34px status pill above with 10px air, 54x28 mic oval below —
// the bar morphs into the oval, bottom-pinned). Side docks: thin 9x34
// vertical bar, expands to a 252x46 horizontal pair (status pill + 48x34
// mic capsule, mirrored on the left dock so the mic faces the screen
// edge). The expanded pair starts EXACTLY where the mini bar sits: same
// 6px outer edge, same centre — the morph grows in place, never jumps.
// Edge gaps are tight: 6px collapsed AND expanded on the sides, 8px above
// the taskbar at the bottom (see dock_position).
const MINI_W: i32 = 30;
const MINI_H: i32 = 8;
const EXP_W: i32 = 240;
const EXP_H: i32 = 86;
const SIDE_MINI_W: i32 = 13;
const SIDE_MINI_H: i32 = 38;
const SIDE_EXP_W: i32 = 240;
const SIDE_EXP_H: i32 = 58;
// Compact TextBoard popup footprint (logical px): comfortable readable dimensions.
const TB_W: i32 = 340;
const TB_H: i32 = 140;
// Tight dock air-gaps (physical px): collapsed bars rest 6px off the edge,
// and expanded side cards start at that SAME 6px edge — the expanded pair
// grows out of the mini bar in place (outer edge + centre pinned), which is
// what makes the morph read as one continuous object instead of a jump.
const EDGE_GAP: i32 = 6;
const EXP_GAP: i32 = 6;

/// Resize the pill footprint: native window + Slint layout together (see
/// pill.slint win_w/win_h — the layout must track or the TouchArea keeps the
/// old size and clicks on the bigger painted area die silently).
fn set_footprint(p: &PillWidget, w: i32, h: i32) {
    p.window().set_size(slint::LogicalSize {
        width: w as f32,
        height: h as f32,
    });
    p.set_win_w(w as f32);
    p.set_win_h(h as f32);
    p.window().request_redraw();
}

/// Move the pill window: native move without triggering non-client frame repaints.
fn place_pill(p: &PillWidget, x: i32, y: i32) {
    p.window().set_position(slint::PhysicalPosition { x, y });
}

/// Shared menu action (tray menu + pill menu): Dashboard opens
/// the modern Slint dashboard, Show/Hide control the Slint pill widget, Quit stops
/// everything. Also reachable via IPC MENUROW for regression testing.
fn menu_action(
    i: i32,
    dash: &slint::Weak<DashboardWidget>,
    pill: &slint::Weak<PillWidget>,
    dash_visible: &Arc<Mutex<bool>>,
    state: &Arc<Mutex<quickstt_core::orchestration::AppState>>,
) {
    match i {
        0 => {
            log_line("menu: open Slint dashboard");
            if let Some(d) = dash.upgrade() {
                widget_platform::show_app_window(d.window());
                if let Ok(mut v) = dash_visible.lock() {
                    *v = true;
                }
            }
        }
        1 => {
            log_line("menu: show Slint pill widget");
            if let Some(p) = pill.upgrade() {
                widget_platform::show_widget(p.window());
                widget_platform::restack_topmost(p.window());
                if let Ok(mut s) = state.lock() {
                    s.widget_visible = true;
                    s.settings.show_widget = true;
                    if let Err(e) = s.settings.save_all() {
                        log_line(&format!("show_widget persist failed: {e}"));
                    }
                }
            }
        }
        2 => {
            log_line("menu: hide Slint pill widget");
            if let Some(p) = pill.upgrade() {
                let _ = p.hide();
                if let Ok(mut s) = state.lock() {
                    s.widget_visible = false;
                    s.settings.show_widget = false;
                    if let Err(e) = s.settings.save_all() {
                        log_line(&format!("show_widget persist failed: {e}"));
                    }
                }
            }
        }
        _ => {
            log_line("menu: quit app");
            quit_app();
            std::process::exit(0);
        }
    }
}

/// Tray left/double-click: flip the Slint pill widget visibility.
fn toggle_pill_widget(
    pill: &slint::Weak<PillWidget>,
    state: &Arc<Mutex<quickstt_core::orchestration::AppState>>,
    why: &str,
) {
    if let Some(p) = pill.upgrade() {
        let visible = p.window().is_visible();
        if visible {
            log_line(&format!("tray {why}: hide Rust pill widget"));
            let _ = p.hide();
            if let Ok(mut s) = state.lock() {
                s.widget_visible = false;
                s.settings.show_widget = false;
                if let Err(e) = s.settings.save_all() {
                    log_line(&format!("show_widget persist failed: {e}"));
                }
            }
        } else {
            log_line(&format!("tray {why}: show Rust pill widget"));
            widget_platform::show_widget(p.window());
            widget_platform::restack_topmost(p.window());
            if let Ok(mut s) = state.lock() {
                s.widget_visible = true;
                s.settings.show_widget = true;
                if let Err(e) = s.settings.save_all() {
                    log_line(&format!("show_widget persist failed: {e}"));
                }
            }
        }
    }
}

/// Current pill footprint (logical px) from expand + orientation flags.
/// The window matches the content size at REST (exact hit-testing); during
/// the morph it sits at the expanded footprint and jumps (invisibly —
/// transparent pixels) only at expand-start / collapse-landing, never
/// per-tick (per-tick native resizes flash white frames).
fn cur_size(expanded: &Arc<Mutex<bool>>, vertical: &Arc<Mutex<bool>>) -> (i32, i32) {
    let v = *mlock(&vertical);
    if *mlock(&expanded) {
        if v {
            (SIDE_EXP_W, SIDE_EXP_H)
        } else {
            (EXP_W, EXP_H)
        }
    } else if v {
        (SIDE_MINI_W, SIDE_MINI_H)
    } else {
        (MINI_W, MINI_H)
    }
}

/// Dock air-gap: 6px on every side, collapsed and expanded alike (bottom
/// dock ignores the margin — it sits 8px above the taskbar via the work
/// area).
fn dock_gap(expanded: &Arc<Mutex<bool>>) -> i32 {
    if *mlock(&expanded) {
        EXP_GAP
    } else {
        EDGE_GAP
    }
}

/// Dark pill-menu (left-tap) metrics, LOGICAL px — must match menu.slint.
const PM_W: i32 = 248;
const PM_H: i32 = 234;
const PM_MIC_Y0: i32 = 83;
const PM_MIC_Y1: i32 = 117;
const PM_MOD_Y0: i32 = 117;
const PM_MOD_Y1: i32 = 151;
/// Flyout metrics: 264 wide, 38px header block + 32px rows + 6px pad.
const FLY_W: i32 = 264;
const FLY_BASE_H: i32 = 44;
const FLY_ROW_H: i32 = 32;

/// Native Windows tray menu item IDs (muda). Same four rows as the
/// previous Slint-rendered menu: Dashboard / Show Widget / Hide Widget /
/// Quit App. The OS owns placement, focus, keyboard nav and dismissal —
/// no cursor polling, no z-order fights.
const TRAY_ID_DASH: &str = "quickstt-dash";
const TRAY_ID_SHOW: &str = "quickstt-show";
const TRAY_ID_HIDE: &str = "quickstt-hide";
const TRAY_ID_QUIT: &str = "quickstt-quit";

/// Which submenu the shared flyout window currently shows.
#[derive(Clone, Copy, PartialEq, Eq)]
enum SubMode {
    None,
    Mic,
    Model,
}

/// Ask Windows for DARK Win32 menus (black background, white text) for the
/// Ask Windows for DARK Win32 menus (black background, white text) for the
/// native tray popup: SetPreferredAppMode(ForceDark) + FlushMenuThemes from
/// uxtheme.dll (Win10 1809+). Undocumented ordinals 135/136, loaded
/// dynamically with graceful fallback to the standard light menu on older
/// systems. Still 100% native HMENU either way: the OS owns placement,
/// focus, keyboard nav and dismissal — this only flips the theme.
/// Must run before the first menu is created (tray build below).
#[cfg(target_os = "windows")]
fn enable_dark_native_menus() {
    unsafe {
        use windows::core::{PCSTR, PCWSTR};
        use windows::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryW};
        // GetProcAddress by ordinal (these exports have no stable name).
        fn by_ordinal(n: u16) -> PCSTR {
            PCSTR(n as usize as *const u8)
        }
        let dll: Vec<u16> = "uxtheme.dll\0".encode_utf16().collect();
        let Ok(lib) = LoadLibraryW(PCWSTR(dll.as_ptr())) else {
            return;
        };
        // NOTE: no FreeLibrary — uxtheme stays loaded for process lifetime;
        // one outstanding module ref costs nothing.
        let set_mode: Option<unsafe extern "system" fn(i32) -> i32> =
            GetProcAddress(lib, by_ordinal(135)).map(|f| std::mem::transmute(f));
        if let Some(set_mode) = set_mode {
            set_mode(2); // ForceDark
            // Ordinal 104: RefreshImmersiveColorPolicyState
            if let Some(refresh) = GetProcAddress(lib, by_ordinal(104)) {
                let refresh_fn: unsafe extern "system" fn() = std::mem::transmute(refresh);
                refresh_fn();
            }
            let flush: Option<unsafe extern "system" fn()> =
                GetProcAddress(lib, by_ordinal(136)).map(|f| std::mem::transmute(f));
            if let Some(flush) = flush {
                flush();
            }
            log_line("dark native menus requested (ForceDark)");
        } else {
            log_line("dark native menus unavailable (old Windows?)");
        }
    }
}

#[cfg(target_os = "windows")]
fn apply_dark_theme_to_hwnd(hwnd: windows::Win32::Foundation::HWND) {
    unsafe {
        use windows::core::{PCSTR, PCWSTR};
        use windows::Win32::Graphics::Dwm::{DwmSetWindowAttribute, DWMWINDOWATTRIBUTE};
        use windows::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryW};

        let dll: Vec<u16> = "uxtheme.dll\0".encode_utf16().collect();
        if let Ok(lib) = LoadLibraryW(PCWSTR(dll.as_ptr())) {
            if let Some(allow_dark) = GetProcAddress(lib, PCSTR(133 as usize as *const u8)) {
                let allow_dark_fn: unsafe extern "system" fn(windows::Win32::Foundation::HWND, i32) -> i32 =
                    std::mem::transmute(allow_dark);
                allow_dark_fn(hwnd, 1);
            }
            let theme_export: Vec<u8> = b"SetWindowTheme\0".to_vec();
            if let Some(set_theme) = GetProcAddress(lib, PCSTR(theme_export.as_ptr())) {
                let set_theme_fn: unsafe extern "system" fn(
                    windows::Win32::Foundation::HWND,
                    *const u16,
                    *const u16,
                ) -> i32 = std::mem::transmute(set_theme);
                let theme_name: Vec<u16> = "DarkMode_Explorer\0".encode_utf16().collect();
                set_theme_fn(hwnd, theme_name.as_ptr(), std::ptr::null());
            }
            if let Some(flush) = GetProcAddress(lib, PCSTR(136 as usize as *const u8)) {
                let flush_fn: unsafe extern "system" fn() = std::mem::transmute(flush);
                flush_fn();
            }
        }
        let dark: i32 = 1;
        let _ = DwmSetWindowAttribute(
            hwnd,
            DWMWINDOWATTRIBUTE(20),
            &dark as *const _ as _,
            std::mem::size_of::<i32>() as u32,
        );
        let _ = DwmSetWindowAttribute(
            hwnd,
            DWMWINDOWATTRIBUTE(19),
            &dark as *const _ as _,
            std::mem::size_of::<i32>() as u32,
        );
    }
}

/// Native tray popup: owner-drawn Win32 HMENU — #121212 item background,
/// white Segoe UI text, painted at OS level.
///
/// Why owner-draw instead of SetPreferredAppMode(ForceDark): ForceDark only
/// yields the system dark grey and proved unreliable here (white text on a
/// white background — a mixed theme state). The spec is exact #121212 plus
/// white, which only owner-draw guarantees. The popup itself stays a real
/// HMENU shown with TrackPopupMenuEx, so placement, focus, keyboard nav and
/// dismissal remain OS-owned — only the item pixels are ours, painted in
/// WM_MEASUREITEM / WM_DRAWITEM.
#[cfg(target_os = "windows")]
struct TrayMenuState {
    items: Vec<String>,
    font_raw: isize,
    prev_proc: windows::Win32::UI::WindowsAndMessaging::WNDPROC,
    /// Live #32768 menu window already dark-framed (one-shot per popup).
    framed: bool,
}

#[cfg(target_os = "windows")]
static TRAY_MENU_STATE: std::sync::Mutex<Option<TrayMenuState>> =
    std::sync::Mutex::new(None);

/// One-shot dark frame for the live popup-menu window (class "#32768").
/// Called from inside the modal TrackPopupMenuEx loop (first WM_DRAWITEM),
/// so the #32768 window exists and belongs to this thread. Applies native
/// OS theming only: immersive dark mode (dark frame + rounded-corner fill),
/// dark border colour, native round corners. No custom window, no overlay —
/// the menu keeps its system shadow and shape.
#[cfg(target_os = "windows")]
fn darken_live_menu_window() {
    use windows::Win32::Foundation::{BOOL, HWND, LPARAM};
    use windows::Win32::Graphics::Dwm::{
        DWMWA_BORDER_COLOR, DWMWA_USE_IMMERSIVE_DARK_MODE,
        DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND, DwmSetWindowAttribute,
    };
    use windows::Win32::System::Threading::GetCurrentThreadId;
    use windows::Win32::UI::WindowsAndMessaging::{
        EnumThreadWindows, GetClassNameW, IsWindowVisible, WNDENUMPROC,
    };

    unsafe extern "system" fn find_menu_window(hwnd: HWND, _lp: LPARAM) -> BOOL {
        let mut cls = [0u16; 16];
        let len = GetClassNameW(hwnd, &mut cls) as usize;
        // Menu windows carry class "#32768".
        let is_menu =
            len == 6 && cls[0] == 0x23 && cls[1] == 0x33 && cls[2] == 0x32
                && cls[3] == 0x37 && cls[4] == 0x36 && cls[5] == 0x38;
        if is_menu && IsWindowVisible(hwnd).as_bool() {
            let dark: i32 = 1;
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWA_USE_IMMERSIVE_DARK_MODE,
                &dark as *const _ as *const _,
                std::mem::size_of::<i32>() as u32,
            );
            let border: u32 = 0x002A2A2C;
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWA_BORDER_COLOR,
                &border as *const _ as *const _,
                std::mem::size_of::<u32>() as u32,
            );
            let corner = DWMWCP_ROUND;
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWA_WINDOW_CORNER_PREFERENCE,
                &corner as *const _ as *const _,
                std::mem::size_of_val(&corner) as u32,
            );
        }
        true.into()
    }

    unsafe {
        let cb: WNDENUMPROC = Some(find_menu_window);
        let _ = EnumThreadWindows(GetCurrentThreadId(), cb, LPARAM(0));
    }
}

/// Temporary subclass proc for the pill HWND while the tray HMENU is up.
/// Installed just before TrackPopupMenuEx and restored right after (the call
/// is modal on this thread — no reentrancy). Forwards everything except the
/// two owner-draw messages. Never panics: any failure falls through to the
/// previous proc / default sizes (a panic across an extern boundary would
/// abort the process).
#[cfg(target_os = "windows")]
unsafe extern "system" fn tray_menu_subclass_proc(
    hwnd: windows::Win32::Foundation::HWND,
    msg: u32,
    wparam: windows::Win32::Foundation::WPARAM,
    lparam: windows::Win32::Foundation::LPARAM,
) -> windows::Win32::Foundation::LRESULT {
    use windows::Win32::Foundation::{COLORREF, LRESULT, RECT};
    use windows::Win32::Graphics::Gdi::{
        CreateSolidBrush, DT_CALCRECT, DT_LEFT, DT_NOCLIP, DT_SINGLELINE,
        DT_VCENTER, DeleteObject, DrawTextW, FillRect, GetDC, GetDeviceCaps,
        HDC, HGDIOBJ, HFONT, LOGPIXELSY, ReleaseDC, SelectObject, SetBkMode,
        SetTextColor, TRANSPARENT,
    };
    use windows::Win32::UI::Controls::{DRAWITEMSTRUCT, MEASUREITEMSTRUCT};
    use windows::Win32::UI::WindowsAndMessaging::{
        CallWindowProcW, DefWindowProcW, WM_DRAWITEM, WM_MEASUREITEM,
    };

    // ODT_MENU == 1, ODS_SELECTED == 0x0001 (compared raw: the flag newtypes
    // in windows 0.52 expose no bitwise ops).
    const ODT_MENU_RAW: u32 = 1;
    const ODS_SELECTED_RAW: u32 = 1;
    const BG: u32 = 0x00121212;
    const BG_HOT: u32 = 0x002E2E2E;
    const FG: u32 = 0x00FFFFFF;
    const SEPC: u32 = 0x002A2A2C;
    // dwItemData sentinel for the separator row (also carries wID 0, so a
    // separator can never be reported as a command).
    const SEP_DATA: usize = usize::MAX;

    // One-shot per popup: theme the live menu window (#32768) itself so no
    // system-white frame, corners or margins show around the owner-drawn
    // rows. Runs on the first WM_DRAWITEM, i.e. while the menu is visible
    // inside the modal TrackPopupMenuEx loop.
    if msg == WM_DRAWITEM {
        let probe = &*(lparam.0 as *const DRAWITEMSTRUCT);
        if probe.CtlType.0 == ODT_MENU_RAW {
            let needs_frame = TRAY_MENU_STATE
                .lock()
                .map(|g| !g.as_ref().map(|s| s.framed).unwrap_or(true))
                .unwrap_or(false);
            if needs_frame {
                darken_live_menu_window();
                if let Ok(mut g) = TRAY_MENU_STATE.lock() {
                    if let Some(s) = g.as_mut() {
                        s.framed = true;
                    }
                }
            }
        }
    }

    if msg == WM_MEASUREITEM {
        let mis = &mut *(lparam.0 as *mut MEASUREITEMSTRUCT);
        if mis.CtlType.0 == ODT_MENU_RAW {
            if let Ok(guard) = TRAY_MENU_STATE.lock() {
                let hdc: HDC = GetDC(hwnd);
                if hdc.0 != 0 {
                    let dpi = GetDeviceCaps(hdc, LOGPIXELSY).max(96);
                    let scale = dpi as f32 / 96.0;
                    if mis.itemData == SEP_DATA {
                        mis.itemWidth = (232.0 * scale).round() as u32;
                        mis.itemHeight = ((9.0 * scale).round() as u32).max(7);
                    } else if let Some(text) = guard
                        .as_ref()
                        .and_then(|s| s.items.get(mis.itemData))
                    {
                        let font = HFONT(guard.as_ref().map(|s| s.font_raw).unwrap_or(0));
                        let mut wide: Vec<u16> = text.encode_utf16().collect();
                        let mut rc = RECT { left: 0, top: 0, right: 0, bottom: 0 };
                        let old = SelectObject(hdc, HGDIOBJ(font.0));
                        DrawTextW(
                            hdc,
                            &mut wide,
                            &mut rc,
                            DT_CALCRECT | DT_SINGLELINE | DT_LEFT | DT_NOCLIP,
                        );
                        SelectObject(hdc, old);
                        let w = rc.right - rc.left + (56.0 * scale).round() as i32;
                        mis.itemWidth = w.max(0) as u32;
                        mis.itemHeight = (30.0 * scale).round() as u32;
                    }
                    ReleaseDC(hwnd, hdc);
                }
            }
            return LRESULT(0);
        }
    }

    if msg == WM_DRAWITEM {
        let dis = &mut *(lparam.0 as *mut DRAWITEMSTRUCT);
        if dis.CtlType.0 == ODT_MENU_RAW {
            if let Ok(guard) = TRAY_MENU_STATE.lock() {
                let dpi = GetDeviceCaps(dis.hDC, LOGPIXELSY).max(96);
                let scale = dpi as f32 / 96.0;
                let selected = (dis.itemState.0 & ODS_SELECTED_RAW) != 0;
                let bg = CreateSolidBrush(COLORREF(if selected { BG_HOT } else { BG }));
                FillRect(dis.hDC, &dis.rcItem, bg);
                DeleteObject(HGDIOBJ(bg.0));
                let font = HFONT(guard.as_ref().map(|s| s.font_raw).unwrap_or(0));
                if dis.itemData == SEP_DATA {
                    let sep = CreateSolidBrush(COLORREF(SEPC));
                    let mut rc = dis.rcItem;
                    let mid = (rc.top + rc.bottom) / 2;
                    rc.top = mid;
                    rc.bottom = mid + 1;
                    rc.left += (12.0 * scale).round() as i32;
                    rc.right -= (12.0 * scale).round() as i32;
                    if rc.right > rc.left {
                        FillRect(dis.hDC, &rc, sep);
                    }
                    DeleteObject(HGDIOBJ(sep.0));
                } else if let Some(text) =
                    guard.as_ref().and_then(|s| s.items.get(dis.itemData))
                {
                    let mut wide: Vec<u16> = text.encode_utf16().collect();
                    SetBkMode(dis.hDC, TRANSPARENT);
                    SetTextColor(dis.hDC, COLORREF(FG));
                    let old = SelectObject(dis.hDC, HGDIOBJ(font.0));
                    let mut rc = dis.rcItem;
                    rc.left += (16.0 * scale).round() as i32;
                    DrawTextW(
                        dis.hDC,
                        &mut wide,
                        &mut rc,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP,
                    );
                    SelectObject(dis.hDC, old);
                }
            }
            return LRESULT(1);
        }
    }

    let prev = TRAY_MENU_STATE
        .lock()
        .map(|g| g.as_ref().and_then(|s| s.prev_proc))
        .unwrap_or(None);
    match prev {
        Some(p) => CallWindowProcW(Some(p), hwnd, msg, wparam, lparam),
        None => DefWindowProcW(hwnd, msg, wparam, lparam),
    }
}

#[cfg(target_os = "windows")]
fn show_native_tray_menu(hwnd: windows::Win32::Foundation::HWND, x: i32, y: i32) -> Option<u32> {
    use windows::Win32::Foundation::COLORREF;
    use windows::Win32::Graphics::Gdi::{
        CreateFontW, CreateSolidBrush, DeleteObject, GetDC, GetDeviceCaps, HBITMAP, HGDIOBJ, LOGPIXELSY,
        ReleaseDC,
    };
    use windows::Win32::UI::WindowsAndMessaging::{
        CreatePopupMenu, DestroyMenu, GWLP_WNDPROC, HMENU, InsertMenuItemW,
        MENUINFO, MENUINFO_STYLE, MENUITEMINFOW, MENU_ITEM_STATE,
        MIM_APPLYTOSUBMENUS, MIM_BACKGROUND, MIIM_DATA, MIIM_FTYPE, MIIM_ID,
        MFT_OWNERDRAW, PostMessageW, SetForegroundWindow, SetMenuInfo,
        SetWindowLongPtrW, TrackPopupMenuEx, TPM_NONOTIFY, TPM_RETURNCMD,
        TPM_RIGHTBUTTON, WM_NULL, WNDPROC,
    };
    use windows::core::{PCWSTR, PWSTR};

    // (command id, label). Separator carries id 0 + sentinel item data.
    const ITEMS: &[(u32, &str)] = &[
        (1, "Dashboard"),
        (2, "Show/Hide Widget"),
        (0, ""),
        (3, "Quit QuickSTT"),
    ];
    const SEP_DATA: usize = usize::MAX;

    unsafe {
        enable_dark_native_menus();
        let hmenu = CreatePopupMenu().ok()?;
        for (pos, (id, _)) in ITEMS.iter().enumerate() {
            let is_sep = *id == 0;
            let mii = MENUITEMINFOW {
                cbSize: std::mem::size_of::<MENUITEMINFOW>() as u32,
                fMask: MIIM_FTYPE | MIIM_ID | MIIM_DATA,
                fType: MFT_OWNERDRAW,
                fState: MENU_ITEM_STATE(0),
                wID: *id,
                hSubMenu: HMENU(0),
                hbmpChecked: HBITMAP(0),
                hbmpUnchecked: HBITMAP(0),
                dwItemData: if is_sep { SEP_DATA } else { pos },
                dwTypeData: PWSTR(std::ptr::null_mut()),
                cch: 0,
                hbmpItem: HBITMAP(0),
            };
            let _ = InsertMenuItemW(hmenu, pos as u32, true, &mii);
        }

        // Segoe UI 9pt for measurement + paint (shared with the subclass
        // proc through TRAY_MENU_STATE; deleted after Track returns, which
        // is after the last WM_DRAWITEM).
        let hdc0 = GetDC(hwnd);
        let dpi = if hdc0.0 != 0 {
            let d = GetDeviceCaps(hdc0, LOGPIXELSY).max(96);
            ReleaseDC(hwnd, hdc0);
            d
        } else {
            96
        };
        let face: Vec<u16> = "Segoe UI\0".encode_utf16().collect();
        let font = CreateFontW(
            -((9 * dpi + 36) / 72),
            0,
            0,
            0,
            400,
            0,
            0,
            0,
            1,
            0,
            0,
            0,
            0,
            PCWSTR(face.as_ptr()),
        );

        // Subclass for the modal popup only; the previous proc is restored
        // below even when TrackPopupMenuEx fails.
        let prev: WNDPROC = std::mem::transmute(SetWindowLongPtrW(
            hwnd,
            GWLP_WNDPROC,
            tray_menu_subclass_proc as *const () as usize as isize,
        ));
        if let Ok(mut st) = TRAY_MENU_STATE.lock() {
            *st = Some(TrayMenuState {
                items: ITEMS.iter().map(|(_, t)| t.to_string()).collect(),
                font_raw: font.0,
                prev_proc: prev,
                framed: false,
            });
        }

        // Dark menu canvas behind the owner-drawn rows: without this the
        // system paints COLOR_MENU (white) into the frame, margins and
        // rounded corners around our #121212 items.
        let menu_bg = CreateSolidBrush(COLORREF(0x00121212));
        let menu_info = MENUINFO {
            cbSize: std::mem::size_of::<MENUINFO>() as u32,
            fMask: MIM_BACKGROUND | MIM_APPLYTOSUBMENUS,
            dwStyle: MENUINFO_STYLE(0),
            cyMax: 0,
            hbrBack: menu_bg,
            dwContextHelpID: 0,
            dwMenuData: 0,
        };
        let _ = SetMenuInfo(hmenu, &menu_info);

        SetForegroundWindow(hwnd);
        let cmd = TrackPopupMenuEx(
            hmenu,
            TPM_RIGHTBUTTON.0 | TPM_RETURNCMD.0 | TPM_NONOTIFY.0,
            x,
            y,
            hwnd,
            None,
        );
        let _ = PostMessageW(hwnd, WM_NULL, windows::Win32::Foundation::WPARAM(0), windows::Win32::Foundation::LPARAM(0));
        let _ = DestroyMenu(hmenu);
        DeleteObject(HGDIOBJ(menu_bg.0));

        // Restore the original proc + drop the menu state first, then the
        // font (no WM_DRAWITEM can arrive after Track returned).
        let restore: WNDPROC = TRAY_MENU_STATE
            .lock()
            .map(|mut g| g.take().and_then(|s| s.prev_proc))
            .unwrap_or(None);
        if let Some(p) = restore {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, std::mem::transmute::<WNDPROC, isize>(Some(p)));
        }
        if font.0 != 0 {
            DeleteObject(HGDIOBJ(font.0));
        }
        if cmd.0 > 0 {
            Some(cmd.0 as u32)
        } else {
            None
        }
    }
}

#[cfg(target_os = "windows")]
fn apply_dark_theme_to_process_windows() {
    unsafe {
        use windows::Win32::Foundation::{BOOL, HWND, LPARAM};
        use windows::Win32::System::Threading::GetCurrentThreadId;
        use windows::Win32::UI::WindowsAndMessaging::{
            EnumThreadWindows, EnumWindows, FindWindowExW, GetWindowThreadProcessId,
            HWND_MESSAGE,
        };

        let current_pid = std::process::id();
        unsafe extern "system" fn enum_proc(hwnd: HWND, lparam: LPARAM) -> BOOL {
            let target_pid = lparam.0 as u32;
            let mut pid = 0u32;
            GetWindowThreadProcessId(hwnd, Some(&mut pid));
            if pid == target_pid {
                apply_dark_theme_to_hwnd(hwnd);
            }
            BOOL(1)
        }

        // 1. Top-level windows
        let _ = EnumWindows(Some(enum_proc), LPARAM(current_pid as isize));

        // 2. Thread windows on current thread (covers tray-icon and muda hidden helper windows)
        let _ = EnumThreadWindows(GetCurrentThreadId(), Some(enum_proc), LPARAM(current_pid as isize));

        // 3. Message-only windows (HWND_MESSAGE children)
        let mut child = HWND(0);
        for _ in 0..100 {
            child = FindWindowExW(HWND_MESSAGE, child, None, None);
            if child.0 == 0 {
                break;
            }
            let mut pid = 0u32;
            GetWindowThreadProcessId(child, Some(&mut pid));
            if pid == current_pid {
                apply_dark_theme_to_hwnd(child);
            }
        }
    }
}

/// Hide the dark pill menu + its flyout.
fn hide_pill_menus(pm: &slint::Weak<PillMenu>, fly: &slint::Weak<MenuFlyout>) {
    if let Some(m) = pm.upgrade() {
        let _ = m.hide();
    }
    if let Some(f) = fly.upgrade() {
        let _ = f.hide();
    }
}

/// Size the flyout window to its row count (Slint lays out from props).
fn set_flyout_footprint(f: &MenuFlyout, n: i32) {
    let h = FLY_BASE_H + n * FLY_ROW_H;
    f.window().set_size(slint::LogicalSize {
        width: FLY_W as f32,
        height: h as f32,
    });
    f.set_item_count(n);
}

/// Fill + place + show the flyout (fade-in completes on the next poll tick).
fn show_flyout(
    fly_w: &slint::Weak<MenuFlyout>,
    title: &str,
    items: &[String],
    selected: i32,
    x: i32,
    y: i32,
) {
    if let Some(f) = fly_w.upgrade() {
        f.set_head_text(title.into());
        let v: Vec<slint::SharedString> =
            items.iter().map(slint::SharedString::from).collect();
        f.set_items(ModelRc::new(VecModel::from_slice(&v)));
        f.set_selected_idx(selected);
        set_flyout_footprint(&f, items.len() as i32);
        f.set_fly_op(0.0);
        f.window().set_position(slint::PhysicalPosition { x, y });
        widget_platform::show_widget(f.window());
        log_line(&format!("flyout '{title}' @ {x},{y} n={}", items.len()));
    }
}

/// Microphone submenu rows: "System default" + OS inputs (capped), with the
/// selected index. Empty setting / unknown device => System default.
fn mic_menu_items(state: &Arc<Mutex<AppState>>) -> (Vec<String>, i32) {
    let mut names = vec!["System default".to_string()];
    for d in quickstt_core::audio::capture::list_input_devices() {
        if names.len() >= 11 {
            break;
        }
        if !names.contains(&d) {
            names.push(d);
        }
    }
    let sel = state
        .lock()
        .map(|s| s.settings.selected_microphone.clone())
        .unwrap_or_default();
    let idx = if sel.trim().is_empty() {
        0
    } else {
        names
            .iter()
            .position(|n| n == sel.trim())
            .map(|i| i as i32)
            .unwrap_or(0)
    };
    (names, idx)
}

/// Model submenu rows: widget model names (capped), with selected index.
fn model_menu_items(state: &Arc<Mutex<AppState>>) -> (Vec<String>, i32) {
    if let Ok(s) = state.lock() {
        let names: Vec<String> =
            s.model_entries.iter().take(10).map(|e| e.name.clone()).collect();
        let sel = if (s.selected_model as usize) < names.len() {
            s.selected_model as i32
        } else {
            -1
        };
        (names, sel)
    } else {
        (Vec::new(), -1)
    }
}

/// Populate + place + show the shared flyout for `mode` next to the open
/// dark menu (right side, or left when the screen forces it). Returns the
/// rows + physical rect so the poll loop can track hover + dismissal.
fn open_submenu(
    fly_w: &slint::Weak<MenuFlyout>,
    menu_w: &slint::Weak<PillMenu>,
    pill_w: &slint::Weak<PillWidget>,
    state: &Arc<Mutex<AppState>>,
    mode: SubMode,
) -> Option<(Vec<String>, (i32, i32, i32, i32))> {
    let (Some(_f), Some(m), Some(p)) =
        (fly_w.upgrade(), menu_w.upgrade(), pill_w.upgrade())
    else {
        return None;
    };
    if !m.window().is_visible() {
        return None;
    }
    let (items, sel) = match mode {
        SubMode::Mic => mic_menu_items(state),
        SubMode::Model => model_menu_items(state),
        SubMode::None => return None,
    };
    if items.is_empty() {
        return None;
    }
    let scale = widget_platform::window_scale(p.window()).max(0.5);
    let mpos = m.window().position();
    let mw = (PM_W as f32 * scale).round() as i32;
    let (wl, wt, wr, wb) = widget_platform::work_area();
    let fw = (FLY_W as f32 * scale).round() as i32;
    let fh =
        ((FLY_BASE_H + items.len() as i32 * FLY_ROW_H) as f32 * scale).round() as i32;
    let band = if mode == SubMode::Mic {
        PM_MIC_Y0
    } else {
        PM_MOD_Y0
    };
    let row_top = mpos.y + (band as f32 * scale).round() as i32;
    let mut fx = mpos.x + mw + 4;
    if fx + fw > wr {
        fx = (mpos.x - fw - 4).max(wl);
    }
    let fy = row_top.clamp(wt, (wb - fh).max(wt));
    show_flyout(
        fly_w,
        if mode == SubMode::Mic {
            "Microphone"
        } else {
            "Model"
        },
        &items,
        sel,
        fx,
        fy,
    );
    Some((items, (fx, fy, fw, fh)))
}

fn forward_to_running(cmd: &str) -> bool {
    use std::io::Write;
    let payload = match cmd {
        "--show" => "SHOW\n",
        "--hide" => "HIDE\n",
        "--toggle" => "TOGGLE\n",
        "--dashboard" => "DASH\n",
        "--quit" => "QUIT\n",
        "--stop" => "STOP\n",
        _ => return false,
    };
    match std::net::TcpStream::connect_timeout(
        &std::net::SocketAddr::from(([127, 0, 0, 1], IPC_PORT)),
        std::time::Duration::from_millis(400),
    ) {
        Ok(mut s) => {
            let _ = s.write_all(payload.as_bytes());
            true
        }
        Err(_) => false,
    }
}

fn spawn_ipc_listener(
    state: Arc<Mutex<quickstt_core::orchestration::AppState>>,
    diag_tx: std::sync::mpsc::Sender<String>,
    tx_cmd: tokio::sync::mpsc::Sender<OrchestratorCommand>,
) {
    std::thread::Builder::new()
        .name("slint-ipc".into())
        .spawn(move || {
            let addr = std::net::SocketAddr::from(([127, 0, 0, 1], IPC_PORT));
            // Retry briefly: a rapid kill→relaunch can find the port still
            // held (dying holder). But NEVER spin forever: an infinite retry
            // used to run a second full pill+tray+overlay with no IPC, the
            // two instances fighting over docks and TOPMOST — stuck ghost
            // overlays and phantom windows. If the port stays held, another
            // instance is alive: say so and exit.
            let listener = {
                let mut tries = 0;
                loop {
                    match std::net::TcpListener::bind(addr) {
                        Ok(l) => break l,
                        Err(e) => {
                            tries += 1;
                            log_line(&format!("ipc bind retry {tries} ({e})"));
                            if tries >= 10 {
                                log_line("ipc port still held — another instance is alive, exiting");
                                std::process::exit(1);
                            }
                            std::thread::sleep(std::time::Duration::from_secs(1));
                        }
                    }
                }
            };
            use std::io::Read;
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { continue };
                // Read to newline (a single read() can return a partial
                // command). Every command is logged: invisible IPC drops
                // used to look exactly like broken features.
                let mut raw = Vec::new();
                let mut one = [0u8; 1];
                loop {
                    match stream.read(&mut one) {
                        Ok(1) => {
                            if one[0] == b'\n' {
                                break;
                            }
                            raw.push(one[0]);
                            if raw.len() > 64 {
                                break;
                            }
                        }
                        _ => break,
                    }
                }
                let cmd = String::from_utf8_lossy(&raw);
                log_line(&format!("ipc cmd: {cmd:?}"));
                let cmd_s = cmd.trim().to_string();
                match cmd_s.as_str() {
                    "SHOW" => {
                        if let Ok(mut s) = state.lock() {
                            s.widget_visible = true;
                        }
                        let _ = diag_tx.send("SHOW".into());
                    }
                    "HIDE" => {
                        if let Ok(mut s) = state.lock() {
                            s.widget_visible = false;
                        }
                        let _ = diag_tx.send("HIDE".into());
                    }
                    "TOGGLE" => {
                        if let Ok(mut s) = state.lock() {
                            s.widget_visible = !s.widget_visible;
                        }
                        let _ = diag_tx.send("TOGGLE".into());
                    }
                    "DASH" => {
                        let _ = diag_tx.send("DASH".into());
                    }
                    // Field diagnostics: the IPC thread only queues; the main
                    // thread dispatches (Slint handles are main-thread
                    // affine — even Weak::upgrade misbehaves off-thread).
                    "CLICKTEST" | "RCLICK" | "DBLCLICK" | "CYCLE" | "OVERLAY" | "WINLIST" | "MENU2OPEN" => {
                        let _ = diag_tx.send(cmd_s);
                        log_line("diag queued");
                    }
                    "QUIT" => {
                        quit_app();
                        std::process::exit(0);
                    }
                    // Remote stop (mic-off): ends any live listen/transcribe
                    // turn and closes capture.
                    "STOP" => {
                        if tx_cmd
                            .try_send(OrchestratorCommand::StopListening)
                            .is_err()
                        {
                            log_line("STOP send FAILED (command loop dead?)");
                        } else {
                            log_line("ipc: stop listening");
                        }
                    }
                    c if c.starts_with("PRESS")
                        || c.starts_with("MOVE")
                        || c.starts_with("RELEASE")
                        || c.starts_with("MENUROW")
                        || c.starts_with("MENU2ROW")
                        || c.starts_with("MENU2MIC")
                        || c.starts_with("MENU2MODEL")
                        || c.starts_with("SAY ") =>
                    {
                        let _ = diag_tx.send(cmd_s);
                        log_line("diag queued");
                    }
                    _ => {}
                }
            }
        })
        .ok();
}

/// Append-only debug log (the GUI has no console under windows_subsystem).
/// Lives in %TEMP%/quickstt-slint.log — the field record for hotkey and
/// transcription issues that are otherwise invisible.
fn log_line(msg: &str) {
    eprintln!("[slint] {msg}");
    let path = std::env::temp_dir().join("quickstt-slint.log");
    if let Ok(mut f) = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
    {
        use std::io::Write as _;
        let secs = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        // ONE write_all per line: writeln! emits several writes and the IPC
        // thread + main thread interleave mid-line into unreadable garble.
        // Short lines fit in a single syscall, so append stays line-atomic.
        let line = format!("[{secs}] {msg}\n");
        let _ = f.write_all(line.as_bytes());
    }
}

/// Poison-tolerant mutex guard: if some other thread panicked while holding
/// the lock, take the inner value and keep running instead of cascading the
/// panic into the event loop. The old `.lock().unwrap()` chain meant ONE bad
/// tick poisoned a mutex and EVERY later tick panicked on it — the app stayed
/// up but frozen (stuck overlay, dead gestures) with no new log output,
/// which looked exactly like a crash. Hot paths (poll, drag, menus) must use
/// this; the 50ms tick additionally runs inside catch_unwind as a backstop.
fn mlock<T>(m: &std::sync::Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// Set the moment Quit starts: background workers (pre-warm, SHOW/HIDE
/// forwarders) must not spawn new C++ processes afterwards, or a pre-warm
/// waking up after quit_both leaves an orphaned widget behind forever.
static QUITTING: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);

/// True when another instance answered (SHOW forwarded, this one exits).
/// Retries for ~2.5s: the first instance may still be starting (no listener
/// yet) when a second launch races it — one blind probe used to miss, and
/// the loser then ran a FULL second pill+tray+overlay fighting the winner
/// over docks and TOPMOST (stuck ghost overlays, phantom windows).
fn ping_running_instance() -> bool {
    use std::io::Write;
    for _ in 0..6 {
        match std::net::TcpStream::connect_timeout(
            &std::net::SocketAddr::from(([127, 0, 0, 1], IPC_PORT)),
            std::time::Duration::from_millis(400),
        ) {
            Ok(mut s) => {
                let _ = s.write_all(b"SHOW\n");
                return true;
            }
            Err(_) => std::thread::sleep(std::time::Duration::from_millis(400)),
        }
    }
    false
}
/// Locate the C++ widget exe in the repo/packaged layout.
#[allow(dead_code)]
fn cpp_exe_path() -> Option<std::path::PathBuf> {
    std::env::current_exe().ok().and_then(|p| {
        p.ancestors().find_map(|a| {
            let c = a.join("QuickSTT_App").join("QuickSTT_App.exe");
            c.is_file().then_some(c)
        })
    })
}

/// Kill a process image by name (best-effort, no console flash). Logs the
/// taskkill result so a lingering widget after Quit leaves a trace.
fn kill_image(name: &str) {
    #[cfg(target_os = "windows")]
    {
        use std::os::windows::process::CommandExt;
        let st = std::process::Command::new("taskkill")
            .args(["/F", "/IM", name])
            .creation_flags(0x08000000) // CREATE_NO_WINDOW
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
        log_line(&format!("kill_image {name}: {st:?}"));
    }
    #[cfg(not(target_os = "windows"))]
    {
        let _ = name;
    }
}

/// Kill by PID with no console flash (transcription-watchdog helper kill).
fn kill_pid(pid: u32) {
    #[cfg(target_os = "windows")]
    unsafe {
        use windows::Win32::Foundation::CloseHandle;
        use windows::Win32::System::Threading::{
            OpenProcess, TerminateProcess, PROCESS_TERMINATE,
        };
        if let Ok(h) = OpenProcess(PROCESS_TERMINATE, false, pid) {
            let _ = TerminateProcess(h, 1);
            let _ = CloseHandle(h);
        }
    }
    #[cfg(not(target_os = "windows"))]
    {
        let _ = pid;
    }
}

/// Direct activation message to the running C++ widget through its
/// QLocalServer named pipe — no process spawn, single-digit milliseconds.
/// QLocalServer "QuickSTT_App_Activation_v2" listens on
/// \\.\pipe\QuickSTT_App_Activation_v2; the server readAll().trimmed()s one
/// message per connection, so one write of b"SHOW\n" == one tray click.
/// Returns true when a primary took the message (nothing to spawn then).
/// Falls back to false when no primary listens (fresh start needed).
#[allow(dead_code)]
fn send_activation_direct(msg: &[u8]) -> bool {
    #[cfg(target_os = "windows")]
    unsafe {
        use windows::core::PCWSTR;
        use windows::Win32::Foundation::CloseHandle;
        use windows::Win32::Storage::FileSystem::{
            CreateFileW, WriteFile, FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_MODE, OPEN_EXISTING,
        };
        use windows::Win32::System::Pipes::WaitNamedPipeW;
        let name: Vec<u16> = "\\\\.\\pipe\\QuickSTT_App_Activation_v2\0"
            .encode_utf16()
            .collect();
        // If the pipe exists but is busy (primary mid-handshake), wait
        // briefly instead of spawning a forwarder process.
        let _ = WaitNamedPipeW(PCWSTR(name.as_ptr()), 300);
        let handle = match CreateFileW(
            PCWSTR(name.as_ptr()),
            0xC0000000, // GENERIC_READ | GENERIC_WRITE
            FILE_SHARE_MODE(0),
            None,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            None,
        ) {
            Ok(h) => h,
            Err(_) => return false,
        };
        let mut written = 0u32;
        let ok = WriteFile(handle, Some(msg), Some(&mut written), None).is_ok()
            && written as usize == msg.len();
        let _ = CloseHandle(handle);
        if ok {
            log_line("activation direct: delivered");
        }
        ok
    }
    #[cfg(not(target_os = "windows"))]
    {
        let _ = msg;
        false
    }
}

/// Settle a spawned C++ secondary: it either exits quickly (it forwarded to
/// a live primary — nothing to adopt) or proves persistent (adopt it as the
/// managed child). Fast-polls every 100ms: the common forward case resolves
/// in ~100-300ms instead of the old fixed 1200ms sleep, which is most of
/// what users felt as "tray click takes a while".
#[allow(dead_code)]
fn adopt_or_forwarded(child: &mut std::process::Child) -> bool {
    use std::time::{Duration, Instant};
    let t0 = Instant::now();
    loop {
        std::thread::sleep(Duration::from_millis(100));
        match child.try_wait() {
            Ok(None) => {
                if t0.elapsed() >= Duration::from_millis(400) {
                    return true; // persistent primary — adopt
                }
            }
            _ => return false, // exited → forwarded (or died)
        }
        if t0.elapsed() >= Duration::from_millis(1500) {
            return matches!(child.try_wait(), Ok(None));
        }
    }
}

pub fn quit_app() {
    QUITTING.store(true, std::sync::atomic::Ordering::SeqCst);
    log_line("quit: stopping QuickSTT and killing child processes");
    slint::quit_event_loop().ok();
    kill_image("QuickSTT_App.exe");
    kill_image("stt_service.exe");
    kill_image("quickstt_popup.exe");
    kill_image("parakeet_engine.exe");
    kill_image("nemotron_engine.exe");
    kill_image("transcribe-cli.exe");
    kill_image("sherpa-onnx-offline.exe");
    kill_image("whisper-cli.exe");
    kill_image("vosk_transcriber.exe");
    kill_image("deep-filter.exe");
}

/// One app quit: stop any strays, then caller exits.
#[allow(dead_code)]
fn quit_both(cpp_child: &Arc<Mutex<Option<std::process::Child>>>) {
    if let Ok(mut c) = cpp_child.lock() {
        if let Some(ch) = c.as_mut() {
            let _ = ch.kill();
        }
        *c = None;
    }
    quit_app();
}

/// Single-app bundling: the C++ widget is the second widget of ONE app — it
/// must never look like its own app (no taskbar button, no Alt-Tab entry,
/// no tray icon of its own). Qt marks its window APPWINDOW by default; swap
/// that for TOOLWINDOW once the window exists (the process needs a moment
/// after launch, so poll briefly). Runs off-thread; idempotent.
#[cfg(target_os = "windows")]
#[allow(dead_code)]
fn style_cpp_widget_window(pid: u32) {
    use windows::Win32::Foundation::{BOOL, HWND, LPARAM};
    use windows::Win32::UI::WindowsAndMessaging::{
        EnumWindows, GetWindowLongW, GetWindowThreadProcessId, IsWindowVisible, SetWindowLongW,
        SetWindowPos, GWL_EXSTYLE, SWP_FRAMECHANGED, SWP_NOACTIVATE, SWP_NOMOVE, SWP_NOSIZE,
        SWP_NOZORDER, WS_EX_APPWINDOW, WS_EX_TOOLWINDOW,
    };
    struct Search {
        pid: u32,
        styled: i32,
    }
    unsafe extern "system" fn visit(hwnd: HWND, lp: LPARAM) -> BOOL {
        let s = &mut *(lp.0 as *mut Search);
        let mut wpid = 0u32;
        GetWindowThreadProcessId(hwnd, Some(&mut wpid));
        // Style every visible top-level window of that PID (Qt normally has
        // exactly one): drop the taskbar/Alt-Tab presence, keep the rest.
        if wpid == s.pid && IsWindowVisible(hwnd).as_bool() {
            let ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
            let want = (ex | WS_EX_TOOLWINDOW.0 as i32) & !(WS_EX_APPWINDOW.0 as i32);
            if want != ex {
                SetWindowLongW(hwnd, GWL_EXSTYLE, want);
                let _ = SetWindowPos(
                    hwnd,
                    HWND(0),
                    0,
                    0,
                    0,
                    0,
                    SWP_NOMOVE
                        | SWP_NOSIZE
                        | SWP_NOACTIVATE
                        | SWP_NOZORDER
                        | SWP_FRAMECHANGED,
                );
            }
            s.styled += 1;
        }
        true.into()
    }
    for _ in 0..60 {
        let mut s = Search { pid, styled: 0 };
        unsafe {
            let _ = EnumWindows(Some(visit), LPARAM(&mut s as *mut Search as isize));
        }
        if s.styled > 0 {
            log_line(&format!(
                "C++ widget bundled: taskbar/Alt-Tab entry removed (pid {pid})"
            ));
            return;
        }
        std::thread::sleep(std::time::Duration::from_millis(200));
    }
    log_line(&format!("C++ widget window not found for styling (pid {pid})"));
}
#[cfg(not(target_os = "windows"))]
#[allow(dead_code)]
fn style_cpp_widget_window(_pid: u32) {}

/// One-app C++ widget management: the Slint tray is the ONLY tray icon.
/// The C++ process always runs with --no-tray (its own icon suppressed);
/// Show launches it on demand and forwards SHOW when it already runs.
/// Fast path first: a direct named-pipe SHOW reaches a live primary in
/// milliseconds with no process spawn at all (tray clicks used to pay
/// spawn + a fixed 1200ms settle sleep — most of the felt delay).
#[allow(dead_code)]
fn ensure_cpp_shown(_cpp_child: &Arc<Mutex<Option<std::process::Child>>>) {}
#[allow(dead_code)]
fn ensure_cpp_dashboard(_cpp_child: &Arc<Mutex<Option<std::process::Child>>>) {}
#[allow(dead_code)]
fn ensure_cpp_hide(_cpp_child: &Arc<Mutex<Option<std::process::Child>>>) {}
#[allow(dead_code)]
fn prewarm_cpp_widget(_cpp_child: &Arc<Mutex<Option<std::process::Child>>>) {}

/// Hallucination guard: single-word ghosts ("yeah" etc.) with near-silence
/// peaks are dropped, never typed. Shared by the streaming and bulk paths.
/// Also catches repetitive loops ("you you you", "yeah yeah yeah") that
/// whisper/parakeet emit on silence — VAD-gated so silence never types.
fn is_ghost_text(trimmed_lower: &str) -> bool {
    // Exact singletons (punctuation-normalised by callers).
    if matches!(
        trimmed_lower,
        "yeah"
            | "yeah."
            | "yes"
            | "yes."
            | "you"
            | "you."
            | "uh"
            | "uh."
            | "um"
            | "um."
            | "hmm"
            | "hmm."
            | "oh"
            | "oh."
            | "hey"
            | "hey."
            | "thank you"
            | "thank you."
            | "thanks"
            | "thanks."
            | "..."
            | "."
            | ""
    ) {
        return true;
    }
    // Repetitive single-word loops: split on whitespace, strip trailing
    // dots/commas, and require every token to be the same ghost word.
    // Catches "you you you", "You, you, you.", "yeah yeah", etc.
    // Also catches 2x+ repeats with filler punctuation.
    let words: Vec<String> = trimmed_lower
        .split_whitespace()
        .map(|w| {
            w.trim_matches(|c: char| c == '.' || c == ',' || c == '!' || c == '?' || c == '"' || c == '\'')
                .to_string()
        })
        .filter(|w| !w.is_empty())
        .collect();
    if words.is_empty() {
        return true;
    }
    // Empty after stripping (e.g. "... ...") is ghost.
    // Single token already handled above, but keep for safety.
    if words.len() == 1 {
        return matches!(
            words[0].as_str(),
            "yeah" | "yes" | "you" | "uh" | "um" | "hmm" | "oh" | "hey" | "thanks" | "thank"
        );
    }
    // 2-40 word loops of one ghost word (covers "you you you..." of any length).
    if words.len() <= 40 {
        let first = words[0].as_str();
        let ghost_word = matches!(
            first,
            "yeah" | "yes" | "you" | "uh" | "um" | "hmm" | "oh" | "hey" | "thanks" | "thank"
        );
        if ghost_word && words.iter().all(|w| w == first) {
            return true;
        }
        // "thank you thank you ..." two-word loop.
        if words.len() % 2 == 0 && words.len() <= 20 {
            let pairs_ghost = words.chunks_exact(2).all(|c| c[0] == "thank" && c[1] == "you");
            if pairs_ghost {
                return true;
            }
        }
    }
    // Model control tokens that slipped through cleaning (<unk> runs from a
    // broken/empty decode, empty brackets) — never type these.
    let compact: String = trimmed_lower
        .chars()
        .filter(|c| !c.is_whitespace())
        .collect();
    if compact.is_empty() {
        return true;
    }
    if compact.contains("<unk>") || compact.contains("<pad>") || compact.contains("</s>") {
        return true;
    }
    false
}

/// VAD-gated drop decision: ghost text is dropped on quiet turns, and
/// repetitive loops are dropped even at moderate peaks (they are never real
/// speech — real speech with peak 20+ contains varied words).
fn should_drop_hallucination(trimmed_lower: &str, sess_peak: u32) -> bool {
    if trimmed_lower.is_empty() {
        return true;
    }
    // Repetitive loops: aggressive — drop up to peak 30 (covers hiss-triggered VAD).
    let words: Vec<&str> = trimmed_lower.split_whitespace().collect();
    if words.len() >= 2 && words.len() <= 40 {
        let norm: Vec<String> = words
            .iter()
            .map(|w| {
                w.trim_matches(|c: char| {
                    c == '.' || c == ',' || c == '!' || c == '?' || c == '"' || c == '\''
                })
                .to_lowercase()
            })
            .collect();
        if !norm.is_empty() && norm.iter().all(|w| w == &norm[0]) {
            let single = matches!(
                norm[0].as_str(),
                "yeah" | "yes" | "you" | "uh" | "um" | "hmm" | "oh" | "hey" | "thanks" | "thank"
            );
            if single && sess_peak < 30 {
                return true;
            }
        }
    }
    // Singletons: classic gate at peak 15, relaxed to 25 for "you" (the most
    // common silence hallucination across whisper/parakeet/vosk).
    if is_ghost_text(trimmed_lower) {
        if trimmed_lower.contains("you") {
            return sess_peak < 25;
        }
        return sess_peak < 15;
    }
    false
}

#[cfg(target_os = "windows")]
unsafe fn release_modifiers() {
    use windows::Win32::UI::Input::KeyboardAndMouse::{
        SendInput, INPUT, INPUT_0, INPUT_KEYBOARD, KEYBDINPUT, KEYEVENTF_KEYUP,
        VK_CONTROL, VK_LCONTROL, VK_LWIN, VK_MENU, VK_RCONTROL, VK_RWIN, VK_SHIFT,
    };
    let keys = [
        VK_LCONTROL, VK_RCONTROL, VK_CONTROL,
        VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN,
    ];
    let inputs: Vec<INPUT> = keys
        .into_iter()
        .map(|vk| INPUT {
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
        })
        .collect();
    SendInput(&inputs, std::mem::size_of::<INPUT>() as i32);
}

#[cfg(target_os = "windows")]
unsafe fn restore_typing_focus(target_hwnd: Option<isize>) {
    use windows::Win32::Foundation::HWND;
    use windows::Win32::System::Threading::{AttachThreadInput, GetCurrentThreadId};
    use windows::Win32::UI::Input::KeyboardAndMouse::SetFocus;
    use windows::Win32::UI::WindowsAndMessaging::{
        GetForegroundWindow, GetWindowThreadProcessId, IsWindow, SetForegroundWindow,
    };

    let cur_thread = GetCurrentThreadId();
    let _current_pid = std::process::id();
    let current_fg = GetForegroundWindow();
    let mut fg_pid = 0u32;
    GetWindowThreadProcessId(current_fg, Some(&mut fg_pid));

    if let Some(target) = target_hwnd {
        let thwnd = HWND(target as _);
        if IsWindow(thwnd).as_bool() {
            let mut target_pid = 0u32;
            let target_thread = GetWindowThreadProcessId(thwnd, Some(&mut target_pid));
            if target_thread != 0 && target_thread != cur_thread {
                let _ = AttachThreadInput(cur_thread, target_thread, true);
                let _ = SetForegroundWindow(thwnd);
                let _ = SetFocus(thwnd);
                let _ = AttachThreadInput(cur_thread, target_thread, false);
            } else {
                let _ = SetForegroundWindow(thwnd);
                let _ = SetFocus(thwnd);
            }
            // Let the target app settle on the restored focus before keys
            // arrive: 20ms loses pastes into slow (Electron/browser) text
            // boxes. 50ms is still imperceptible next to a 160ms tick.
            std::thread::sleep(std::time::Duration::from_millis(50));
            // Verify: Windows foreground-lock can silently reject
            // SetForegroundWindow from a background thread (the old code
            // never checked, so Ctrl+V went to the wrong window and the
            // dictation "vanished").
            if GetForegroundWindow() != thwnd {
                log_line("paste focus restore rejected by foreground-lock (Ctrl+V may miss)");
            }
        } else {
            log_line("paste target window gone, pasting into current focus");
        }
    }
}

#[cfg(target_os = "windows")]
fn copy_to_windows_clipboard(text: &str) -> bool {
    use windows::Win32::Foundation::HANDLE;
    use windows::Win32::System::DataExchange::{
        CloseClipboard, EmptyClipboard, OpenClipboard, SetClipboardData,
    };
    use windows::Win32::System::Memory::{GlobalAlloc, GlobalLock, GlobalUnlock, GMEM_MOVEABLE};

    // The clipboard is a shared lock: another app can hold it open exactly
    // when we paste (the old code tried once and silently kept going, so
    // Ctrl+V pasted stale content and the dictation "vanished").
    for attempt in 0..6 {
        unsafe {
            if OpenClipboard(None).is_ok() {
                let ok = (|| {
                    let _ = EmptyClipboard();
                    let wide: Vec<u16> =
                        text.encode_utf16().chain(std::iter::once(0)).collect();
                    let byte_len = wide.len() * 2;
                    let h = GlobalAlloc(GMEM_MOVEABLE, byte_len).ok()?;
                    let ptr = GlobalLock(h);
                    if ptr.is_null() {
                        return None;
                    }
                    std::ptr::copy_nonoverlapping(
                        wide.as_ptr() as *const u8,
                        ptr as *mut u8,
                        byte_len,
                    );
                    let _ = GlobalUnlock(h);
                    SetClipboardData(13, HANDLE(h.0 as isize)).ok()?;
                    Some(())
                })();
                let _ = CloseClipboard();
                if ok.is_some() {
                    return true;
                }
                log_line("clipboard write failed after open (global alloc/set failed)");
                return false;
            }
        }
        if attempt < 5 {
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }
    log_line("clipboard busy: OpenClipboard failed 6x, paste skipped (text kept in history)");
    false
}

#[cfg(target_os = "windows")]
unsafe fn paste_via_ctrl_v(target_hwnd: Option<isize>) {
    use windows::Win32::UI::Input::KeyboardAndMouse::{
        SendInput, INPUT, INPUT_0, INPUT_KEYBOARD, KEYBDINPUT, KEYBD_EVENT_FLAGS, KEYEVENTF_KEYUP,
        VIRTUAL_KEY, VK_CONTROL,
    };

    release_modifiers();
    restore_typing_focus(target_hwnd);
    // Clipboard write needs a beat before Ctrl+V: back-to-back injects lose
    // the paste into slow targets (text stays in the clipboard only).
    std::thread::sleep(std::time::Duration::from_millis(40));

    let vk_v = VIRTUAL_KEY(0x56);
    let inputs = [
        // Ctrl down
        INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 {
                ki: KEYBDINPUT {
                    wVk: VK_CONTROL,
                    wScan: 0,
                    dwFlags: KEYBD_EVENT_FLAGS(0),
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
                    dwFlags: KEYBD_EVENT_FLAGS(0),
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

    let sent = SendInput(&inputs, std::mem::size_of::<INPUT>() as i32);
    if sent == 0 {
        log_line("SendInput injected 0/4 paste events (input blocked?)");
    } else {
        log_line("SendInput injected fast paste (Ctrl+V)");
    }
}

#[cfg(target_os = "windows")]
#[allow(dead_code)]
unsafe fn send_unicode_text(text: &str) {
    use windows::Win32::UI::Input::KeyboardAndMouse::{
        SendInput, INPUT, INPUT_0, INPUT_KEYBOARD, KEYBDINPUT, KEYEVENTF_KEYUP,
        KEYEVENTF_UNICODE, VIRTUAL_KEY,
    };

    let mut inputs: Vec<INPUT> = Vec::with_capacity(text.len() * 2);
    for unit in text.encode_utf16() {
        let ki_down = KEYBDINPUT {
            wVk: VIRTUAL_KEY(0),
            wScan: unit,
            dwFlags: KEYEVENTF_UNICODE,
            time: 0,
            dwExtraInfo: 0,
        };
        inputs.push(INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 { ki: ki_down },
        });
        let ki_up = KEYBDINPUT {
            wVk: VIRTUAL_KEY(0),
            wScan: unit,
            dwFlags: KEYEVENTF_UNICODE | KEYEVENTF_KEYUP,
            time: 0,
            dwExtraInfo: 0,
        };
        inputs.push(INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 { ki: ki_up },
        });
    }

    if !inputs.is_empty() {
        let sent = SendInput(&inputs, std::mem::size_of::<INPUT>() as i32);
        log_line(&format!("SendInput sent {sent}/{} events", inputs.len()));
    }
}

#[cfg(target_os = "windows")]
fn capture_current_fg_window(target: &Arc<Mutex<Option<isize>>>) {
    unsafe {
        use windows::Win32::UI::WindowsAndMessaging::{GetForegroundWindow, GetWindowThreadProcessId};
        let fg = GetForegroundWindow();
        if fg.0 != 0 {
            let mut pid = 0u32;
            GetWindowThreadProcessId(fg, Some(&mut pid));
            if pid != std::process::id() {
                if let Ok(mut lock) = target.lock() {
                    *lock = Some(fg.0 as isize);
                }
            }
        }
    }
}
#[cfg(not(target_os = "windows"))]
static OWN_XIDS: std::sync::Mutex<Vec<u32>> = std::sync::Mutex::new(Vec::new());

#[cfg(not(target_os = "windows"))]
#[allow(dead_code)]
fn note_own_xid(xid: Option<u32>) {
    if let Some(x) = xid {
        if let Ok(mut v) = OWN_XIDS.lock() {
            if !v.contains(&x) {
                v.push(x);
            }
        }
    }
}

#[cfg(not(target_os = "windows"))]
#[allow(dead_code)]
fn is_own_xid(xid: i64) -> bool {
    if xid <= 0 {
        return false;
    }
    OWN_XIDS
        .lock()
        .map(|v| v.contains(&(xid as u32)))
        .unwrap_or(false)
}

#[cfg(not(target_os = "windows"))]
fn capture_current_fg_window(target: &Arc<Mutex<Option<isize>>>) {
    // X11: the focused XID at session/PTT start, so the paste restores it
    // after our pill took focus. Wayland has no global focus query — the
    // paste goes to whatever holds focus (hands-free flow keeps it there).
    if std::env::var_os("WAYLAND_DISPLAY").is_some() {
        return;
    }
    if let Ok(out) = std::process::Command::new("xdotool")
        .arg("getwindowfocus")
        .output()
    {
        if let Ok(s) = String::from_utf8(out.stdout) {
            if let Ok(xid) = s.trim().parse::<i64>() {
                // Our own pill/menus can hold focus on Linux (no NOACTIVATE
                // equivalent before the WM_HINTS input=False hint lands on
                // every WM): never record ourselves as the paste target, or
                // dictation gets typed back into our own window.
                if xid > 0 && !is_own_xid(xid) {
                    if let Ok(mut lock) = target.lock() {
                        *lock = Some(xid as isize);
                    }
                }
            }
        }
    }
}

/// Linux paste: clipboard + Ctrl+V into the focused window, mirroring the
/// Windows timing (40ms settle, 50ms refocus). Wayland via wl-copy + wtype,
/// X11 via xclip + xdotool (all installed by scripts/install.sh).
#[cfg(not(target_os = "windows"))]
fn deliver_linux(text: &str, target: Option<isize>) {
    use std::io::Write;
    use std::process::{Command, Stdio};
    let wayland = std::env::var_os("WAYLAND_DISPLAY").is_some();
    // 1. Clipboard.
    if wayland {
        if let Ok(mut c) = Command::new("wl-copy")
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
        {
            let _ = c.stdin.as_mut().map(|s| s.write_all(text.as_bytes()));
            let _ = c.wait();
        }
    } else {
        // xclip preferred, xsel fallback.
        let mut done = false;
        if let Ok(mut c) = Command::new("xclip")
            .args(["-selection", "clipboard"])
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
        {
            let _ = c.stdin.as_mut().map(|s| s.write_all(text.as_bytes()));
            done = c.wait().map(|s| s.success()).unwrap_or(false);
        }
        if !done {
            if let Ok(mut c) = Command::new("xsel")
                .args(["--clipboard", "--input"])
                .stdin(Stdio::piped())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .spawn()
            {
                let _ = c.stdin.as_mut().map(|s| s.write_all(text.as_bytes()));
                let _ = c.wait();
            }
        }
    }
    std::thread::sleep(std::time::Duration::from_millis(40));
    // 2. Focus + keystroke.
    if wayland {
        // wtype types into the focused Wayland window (virtual-keyboard
        // protocol — no focus query possible, hands-free keeps the target).
        let _ = Command::new("wtype")
            .args(["-M", "ctrl", "-k", "v", "-m", "ctrl"])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    } else {
        if let Some(xid) = target {
            if xid > 0 {
                let _ = Command::new("xdotool")
                    .arg("windowactivate")
                    .arg(xid.to_string())
                    .stdout(Stdio::null())
                    .stderr(Stdio::null())
                    .status();
                std::thread::sleep(std::time::Duration::from_millis(50));
            }
        }
        let _ = Command::new("xdotool")
            .args(["key", "ctrl+v"])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }
    log_line(&format!("linux paste delivered {} chars", text.len()));
}

/// Deliver fresh transcript text per output mode:
/// - 0 = Type (fast paste via clipboard + Ctrl+V, with Unicode typing)
/// - 1 = Clipboard (fast paste via clipboard + Ctrl+V)
/// - 2 = Show only (TextBoard preview only)
fn deliver_transcription_output(delta: &str, output_mode: u32, target_hwnd: Option<isize>) {
    let text = delta.trim();
    if text.is_empty() {
        return;
    }
    let text = format!("{text} ");
    match output_mode {
        2 => {
            log_line("output_mode: show only (TextBoard)");
        }
        _ => {
            log_line(&format!(
                "output_mode: fast paste into text box ({} chars, mode={output_mode})",
                text.len()
            ));
            #[cfg(target_os = "windows")]
            {
                // Skip the keystroke when the clipboard never took the text:
                // injecting Ctrl+V then pastes stale content (the "paste
                // lost my dictation" report). Text stays in history/paste-row.
                if copy_to_windows_clipboard(&text) {
                    unsafe {
                        paste_via_ctrl_v(target_hwnd);
                    }
                }
            }
            #[cfg(not(target_os = "windows"))]
            deliver_linux(&text, target_hwnd);
        }
    }
}

#[allow(dead_code)]
fn toggle_cpp_widget(
    _cpp_child: &Arc<Mutex<Option<std::process::Child>>>,
    _cpp_visible: &Arc<Mutex<bool>>,
    _why: &str,
) {}

/// SNI tray for Linux (StatusNotifierItem over D-Bus): visible in the XFCE4
/// systray plugin, Cinnamon and Wayland SNI hosts with no panel plugin.
/// tray-icon's Linux backend speaks libappindicator, which stays invisible
/// on panels lacking the Indicator Plugin (this Mint XFCE box included).
#[cfg(target_os = "linux")]
mod sni_tray {
    use std::sync::mpsc::{Receiver, Sender};

    #[derive(Debug, Clone, Copy)]
    pub enum SniClick {
        Activate,
        Dashboard,
        ToggleWidget,
        Quit,
        Online,
    }

    #[derive(Debug)]
    pub struct QuickTray {
        tx: Sender<SniClick>,
        icon: Vec<u8>,
        w: i32,
        h: i32,
    }

    fn row(tx: &Sender<SniClick>, label: &str, click: SniClick) -> ksni::MenuItem<QuickTray> {
        let tx = tx.clone();
        let mut item = ksni::menu::StandardItem::default();
        item.label = label.to_string();
        item.activate = Box::new(move |_tray: &mut QuickTray| {
            let _ = tx.send(click);
        });
        ksni::MenuItem::from(item)
    }

    impl ksni::Tray for QuickTray {
        fn id(&self) -> String {
            "quickstt".to_string()
        }
        fn title(&self) -> String {
            "QuickSTT".to_string()
        }
        fn category(&self) -> ksni::Category {
            ksni::Category::ApplicationStatus
        }
        fn status(&self) -> ksni::Status {
            ksni::Status::Active
        }
        fn icon_pixmap(&self) -> Vec<ksni::Icon> {
            vec![ksni::Icon {
                width: self.w,
                height: self.h,
                data: self.icon.clone(),
            }]
        }
        fn menu(&self) -> Vec<ksni::MenuItem<Self>> {
            vec![
                row(&self.tx, "Open Dashboard", SniClick::Dashboard),
                row(&self.tx, "Show / Hide Widget", SniClick::ToggleWidget),
                row(&self.tx, "Quit QuickSTT", SniClick::Quit),
            ]
        }
        fn activate(&mut self, _x: i32, _y: i32) {
            let _ = self.tx.send(SniClick::Activate);
        }
        fn watcher_online(&self) {
            let _ = self.tx.send(SniClick::Online);
        }
    }

    /// Spawn the SNI service (blocking API: self-driving, no executor for
    /// us to manage). Returns the click channel — poll it in the 50ms loop
    /// next to the TrayIconEvent pump.
    pub fn spawn(icon: Option<(Vec<u8>, u32, u32)>) -> Receiver<SniClick> {
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::Builder::new()
            .name("sni-tray".to_string())
            .spawn(move || {
                let (mut argb, w, h) = icon.unwrap_or_default();
                // RGBA -> ARGB32 (SNI wire order), per the ksni Icon docs.
                for px in argb.chunks_exact_mut(4) {
                    px.rotate_right(1);
                }
                let tray = QuickTray {
                    tx,
                    icon: argb,
                    w: w as i32,
                    h: h as i32,
                };
                use ksni::blocking::TrayMethods;
                match tray.assume_sni_available(true).spawn() {
                    Ok(_handle) => {
                        crate::log_line("sni tray: service started");
                    }
                    Err(e) => crate::log_line(&format!("sni tray unavailable ({e})")),
                }
            })
            .ok();
        rx
    }
}

/// Tray icon: pre-baked PNG (has alpha), ICO fallback. (An SVG render path
/// via resvg/usvg used to live here — identical pixels, but the renderer
/// stack cost megabytes of resident code for a one-shot 64px raster.)
fn load_tray_icon() -> Option<(Vec<u8>, u32, u32)> {
    // PNG (has alpha), then ICO fallbacks.
    // Returns raw RGBA data so callers can create fresh Icon instances
    // (needed for retry logic since TrayIconBuilder consumes the Icon).
    for (bytes, fmt) in [
        (
            &include_bytes!("../../assets/icon_app.png")[..],
            image::ImageFormat::Png,
        ),
        (
            &include_bytes!("../../assets/icon_app.ico")[..],
            image::ImageFormat::Ico,
        ),
    ] {
        if let Ok(img) = image::load_from_memory_with_format(bytes, fmt) {
            let rgba = img.to_rgba8();
            let (w, h) = rgba.dimensions();
            return Some((rgba.into_raw(), w, h));
        }
    }
    None
}

fn main() -> QuickSttResult<()> {
    #[cfg(target_os = "windows")]
    enable_dark_native_menus();

    // Lean renderer: Slint's default GL (FemtoVG/ANGLE/D3D12 + vendor driver)
    // idles at ~190MB private bytes for our few-hundred-pixel widgets. The
    // CPU software rasterizer draws this near-static UI identically for ~6MB
    // (measured 219MB -> 26MB working set). Honour an explicit user override.
    if std::env::var_os("SLINT_BACKEND").is_none() {
        std::env::set_var("SLINT_BACKEND", "winit-software");
    }
    // Crash breadcrumbs: a panic anywhere (UI timer, hotkey thread, workers)
    // appends to the log file — a silent death previously left zero trace.
    std::panic::set_hook(Box::new(|info| {
        let path = std::env::temp_dir().join("quickstt-slint.log");
        if let Ok(mut f) = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(path)
        {
            use std::io::Write as _;
            let _ = writeln!(f, "[panic] {info}");
        }
    }));
    // Single instance: forward CLI to running pill
    let cli = std::env::args().nth(1).unwrap_or_default();
    if !cli.is_empty() && forward_to_running(&cli) {
        return Ok(());
    }
    // Bare second launch just reveals the running pill instead of cloning
    // it (a clone would fight over the tray, IPC port and Ctrl+Space).
    if cli.is_empty() && ping_running_instance() {
        return Ok(());
    }
    log_line(&format!("quickstt-slint starting pid={}", std::process::id()));
    // Single bundled app: sweep any stray C++ widget left by a dead session
    // (it would sit in the taskbar looking like a second app). From here on
    // the only widget is the one this process spawns and styles itself.
    kill_image("QuickSTT_App.exe");
    log_line("startup: stray C++ widget swept (single-app bundle)");

    // Route core diagnostics (tracing info!/warn! — engine spawn, failures,
    // backend notes) into the same file log. Without a subscriber all of
    // that was silently dropped and hung turns were undiagnosable.
    {
        #[derive(Clone, Copy)]
        struct SlintLogWriter;
        impl std::io::Write for SlintLogWriter {
            fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
                let path = std::env::temp_dir().join("quickstt-slint.log");
                if let Ok(mut f) = std::fs::OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(path)
                {
                    let _ = f.write_all(buf);
                }
                Ok(buf.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
        impl<'a> tracing_subscriber::fmt::MakeWriter<'a> for SlintLogWriter {
            type Writer = SlintLogWriter;
            fn make_writer(&'a self) -> Self::Writer {
                SlintLogWriter
            }
        }
        let _ = tracing_subscriber::fmt()
            .with_writer(SlintLogWriter)
            .with_ansi(false)
            .with_max_level(tracing::Level::INFO)
            .try_init();
    }

    let (orchestrator, rx_cmd) = AppOrchestrator::new()?;
    let state = orchestrator.get_state();
    let tx_cmd = orchestrator.get_command_sender();

    let wakeword_handle: Option<quickstt_core::wakeword_service::WakewordHandle> = {
        let models_dir = quickstt_core::wakeword_loader::default_models_dir();
        match quickstt_core::wakeword_service::spawn_background_service(&models_dir, tx_cmd.clone()) {
            Some(h) => {
                log_line(&format!("wakeword service spawned from {:?}", models_dir));
                Some(h)
            }
            None => {
                log_line(&format!("wakeword service unavailable in {:?}", models_dir));
                None
            }
        }
    };
    let last_external_fg: Arc<Mutex<Option<isize>>> = Arc::new(Mutex::new(None));

    std::thread::spawn({
        let state = state.clone();
        let audio_control_tx = orchestrator.audio_control_tx_clone();
        let audio_tx = orchestrator.audio_tx_clone();
        move || {
            let rt = tokio::runtime::Runtime::new().unwrap();
            rt.block_on(async {
                AppOrchestrator::run_command_loop(state, rx_cmd, audio_control_tx, audio_tx).await;
            });
        }
    });

    let pill = PillWidget::new().unwrap();
    let dashboard = DashboardWidget::new().unwrap();
    {
        let (sw, sh) = widget_platform::screen_size();
        log_line(&format!("screen {sw}x{sh}"));
    }

    widget_platform::show_widget(pill.window());
    widget_platform::restack_topmost(pill.window());

    // Start collapsed, bottom-center (matches C++/egui default)
    // Restore the persisted dock station when present so updates and
    // restarts keep the pill where the user put it (never silently reset).
    {
        let dock = state
            .lock()
            .map(|s| s.settings.pill_dock.min(2) as usize)
            .unwrap_or(0);
        let scale = widget_platform::window_scale(pill.window()).max(0.5);
        let vert = dock != 0;
        let (cw, ch) = if vert {
            (SIDE_MINI_W, SIDE_MINI_H)
        } else {
            (MINI_W, MINI_H)
        };
        let (x, y) = widget_platform::dock_position(dock, cw, ch, EDGE_GAP, scale);
        place_pill(&pill, x, y);
        set_footprint(&pill, cw, ch);
        pill.set_vertical(vert);
        pill.set_mirror(dock == 2);
        // Restored visibility (default shown): a hidden pill stays hidden —
        // the app, tray, hotkeys and wakewords keep running either way, and
        // any trigger re-shows within a tick.
        let start_hidden = state
            .lock()
            .map(|mut s| {
                s.widget_visible = s.settings.show_widget;
                !s.settings.show_widget
            })
            .unwrap_or(false);
        if start_hidden {
            let _ = pill.hide();
            log_line("widget starts hidden (restored preference)");
        }
        if let Ok(s) = state.lock() {
            log_line(&format!(
                "settings restored: model='{}' wake='{}' sens={}/{} clap={} offload={}/{}s dock={dock} pos=({x},{y})",
                s.settings.selected_model,
                s.settings.wake_word_mode,
                s.settings.wakeword_sensitivity,
                s.settings.vad_sensitivity,
                s.settings.transient_action,
                s.settings.auto_offload,
                s.settings.offload_seconds,
            ));
        }
    }
    let corner_idx = Arc::new(Mutex::new(
        state
            .lock()
            .map(|s| s.settings.pill_dock.min(2) as usize)
            .unwrap_or(0),
    ));
    let dash_visible = Arc::new(Mutex::new(false));
    // Hover-expand state: cursor-driven in the 50ms poll (robust against
    // Slint hover-event quirks); session activity forces expanded. No pin:
    // the pill is collapsed unless hovered or live (hover-only per spec).
    let expanded = Arc::new(Mutex::new(false));
    // PTT held (Ctrl+Space down): display-layer recording look. Core has no
    // foreground-Recording mode (StartListening arms wakeword mode; the
    // segmenter buffers underneath), so the pill would otherwise show idle
    // while the user holds PTT. While held: phase-1 bars + red mic + expand.
    let ptt_held = Arc::new(Mutex::new(false));
    // Mic-live (capsule toggled ON): same display-layer recording look as
    // PTT. Without this the mic click arms capture silently (WakewordListening
    // renders as idle) and looks dead. Set on mic-start, cleared on mic-stop
    // / turn end / watchdog abort.
    let mic_live = Arc::new(Mutex::new(false));
    // Suppress the quick-tap menu right after a dock hop (a double-click's
    // second release must not pop the menu).
    let suppress_menu_until: Arc<Mutex<Option<std::time::Instant>>> =
        Arc::new(Mutex::new(None));
    // Deferred double-click hop: Slint fires `double-clicked` on the second
    // PRESS while that press is still down, so hopping immediately teleports
    // + resizes the window mid-gesture (white-frame flash, stale grab makes
    // the drag fight the hop). Latch (time, press_n) here; the 50ms poll
    // commits it only once the gesture declared itself: moved -> cancel (a
    // drag, fast or slow-start), clean release -> commit. The window NEVER
    // teleports under a held finger.
    let pending_hop: Arc<Mutex<Option<(std::time::Instant, u32)>>> =
        Arc::new(Mutex::new(None));
    // "Hide for 1 hour" snooze: while set in the future the pill stays
    // hidden (tray icon + menus still work; it reappears on expiry).
    let hide_until: Arc<Mutex<Option<std::time::Instant>>> = Arc::new(Mutex::new(None));
    // Transcript-history mode: the TextBoard shows the FULL buffer instead
    // of just the current turn until the user closes it / a new session.
    let tb_full: Arc<Mutex<bool>> = Arc::new(Mutex::new(false));
    // Last finished turn (trimmed): "Paste last transcript" types this.
    let last_turn: Arc<Mutex<String>> = Arc::new(Mutex::new(String::new()));
    // Smooth footprint morph: window size+pos lerp over ~240ms so expand /
    // collapse glides instead of snapping. One animation at a time.
    // Staging curves for the content choreography (see the per-tick drive):
    // ease_out_cubic snaps the capsule out fast then settles; smootherstep
    // fades the dictate pill in late and buttery.
    fn ease_out_cubic(t: f32) -> f32 {
        1.0 - (1.0 - t).powi(3)
    }
    fn smootherstep(t: f32) -> f32 {
        t * t * t * (t * (t * 6.0 - 15.0) + 10.0)
    }
    // Content-only morph progress, driven at 60fps by the anim ticker. The
    // native window sits at the fixed expanded footprint per orientation and
    // NEVER resizes mid-morph (native resizes flash white frames) — only
    // content props animate, so the morph is pure Slint interpolation.
    // mg0/uo0 let a glide preempt the opposite one mid-flight (quick
    // sweep-across never waits out the full 340ms before coming back).
    #[derive(Clone, Copy)]
    struct FootAnim {
        t0: std::time::Instant,
        dur_ms: u64,
        expanding: bool,
        mg0: f32,
        uo0: f32,
    }
    let foot_anim: Arc<Mutex<Option<FootAnim>>> = Arc::new(Mutex::new(None));
    // Overlay watchdog: when the docking overlay was last shown for a real
    // drag. If it is still visible with no active drag ~1.5s later, the
    // drop was lost (press/release desync) and the poll force-hides it —
    // a stranded fullscreen shade used to sit on-screen forever looking
    // like a frozen crash.
    let overlay_shown_at: Arc<Mutex<Option<std::time::Instant>>> =
        Arc::new(Mutex::new(None));
    // Orientation: false = bottom dock (horizontal), true = side docks
    // (vertical bar / card). Follows corner_idx (0 = bottom).
    let vertical = Arc::new(Mutex::new(false));

/// Field black-box recorder: on real drags (and hop commits) capture two
/// timestamped screenshots + full window censuses with zero coordination —
/// the user just uses the pill and the evidence lands in %TEMP% (fixed
/// filenames, overwritten per event: quickstt-{tag}-0/1.bmp). Worker-thread
/// only (pure Win32, no Slint handles) so it can fire mid-gesture.
fn spawn_dragcap(tag: &'static str) {
    std::thread::Builder::new()
        .name(format!("dragcap-{tag}"))
        .spawn(move || {
            for i in 0..2 {
                // Style-bit sampling THROUGH the wait (not after): catches
                // caption flicker far below the 50ms poll's resolution.
                sample_styles(if i == 0 { 700 } else { 900 });
                let path = std::env::temp_dir().join(format!("quickstt-{tag}-{i}.bmp"));
                let ok = widget_platform::save_screen_bmp(&path);
                crate::log_line(&format!("dragcap {tag}-{i} shot={ok} {}", path.display()));
                widget_platform::log_winlist();
            }
        })
        .ok();
}

/// Tile trap: fires on EVERY pill press. A worker thread snapshots cursor +
/// foreground + the FULL window census (class/pid/rect/style — catches
/// tooltips, foreign captions, drag images) immediately and through a ~2s
/// hold (tooltips pop ~500ms+ in, so the early burst alone misses them),
/// with screenshots at t0/t1. Ring of 4 press slots, fixed filenames
/// (quickstt-press-{i}-t0/t1.bmp), overwritten per press — the user holds
/// once and the evidence is in %TEMP%.
fn spawn_presscap() {
    static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    let i = N.fetch_add(1, std::sync::atomic::Ordering::Relaxed) % 4;
    std::thread::Builder::new()
        .name(format!("presscap-{i}"))
        .spawn(move || {
            // Census burst through the hold (cheap): a sub-350ms popup
            // still lands in the log. Screenshots only at t0 and ~360ms
            // (mid-hold) to bound disk writes — plus a dense burst of
            // small region shots around the live cursor (~40ms apart) so
            // a brief flash near the pill is caught in pixels too.
            widget_platform::press_forensics(&format!("{i}-t0"));
            for (k, ms) in [120u64, 240, 360, 480].iter().enumerate() {
                std::thread::sleep(std::time::Duration::from_millis(
                    ms - if k == 0 { 0 } else { [120, 240, 360][k - 1] },
                ));
                if k == 2 {
                    widget_platform::press_forensics(&format!("{i}-t1"));
                } else {
                    widget_platform::press_census(&format!("{i}-c{k}"));
                }
            }
            let (cx0, cy0) = widget_platform::cursor_position();
            for (k, ms) in [60u64, 100, 140, 180, 220, 300, 420, 560].iter().enumerate() {
                std::thread::sleep(std::time::Duration::from_millis(
                    ms - if k == 0 { 480 } else { [60, 100, 140, 180, 220, 300, 420][k - 1] },
                ));
                // Follow the live cursor (the tile tracks the pill/cursor);
                // fall back to the press point if unreadable.
                let (cx, cy) = widget_platform::cursor_position();
                let (cx, cy) = if cx == 0 && cy == 0 { (cx0, cy0) } else { (cx, cy) };
            let path = std::env::temp_dir()
                .join(format!("quickstt-press-{i}-r{k}.bmp"));
                widget_platform::save_region_bmp(&path, cx, cy, 900, 620);
            }
            // Late-hold census tail (screenshots done — census is cheap):
            // hover tooltips pop ~500ms+ into a stationary hold, after the
            // early burst; these catch the tile's window red-handed with
            // class/owner-pid/rect/title.
            for (k, ms) in [1000u64, 1400, 1800, 2200].iter().enumerate() {
                std::thread::sleep(std::time::Duration::from_millis(
                    ms - if k == 0 { 560 } else { [1000, 1400, 1800][k - 1] },
                ));
                widget_platform::press_census(&format!("{i}-h{k}"));
            }
            crate::log_line(&format!("presscap {i} burst done"));
        })
        .ok();
}

/// High-rate style sampler (field proof for the caption theory): watches our
/// QuickSTT windows' GWL_STYLE every ~3ms and logs every caption-bit flip +
/// a summary. The 50ms poll and the census both sample too slowly to catch a
/// flicker driven per move-event; this cannot miss anything living >=3ms.
#[cfg(target_os = "windows")]
fn sample_styles(ms: u64) {
    use windows::Win32::Foundation::{BOOL, HWND, LPARAM};
    use windows::Win32::System::Threading::GetCurrentProcessId;
    use windows::Win32::UI::WindowsAndMessaging::{
        EnumWindows, GetWindowLongW, GetWindowTextW, GetWindowThreadProcessId,
        IsWindowVisible, GWL_STYLE,
    };
    const WS_CAPTION_BIT: i32 = 0x00C0_0000;
    struct Found {
        v: Vec<(isize, String)>,
    }
    unsafe extern "system" fn visit(h: HWND, lp: LPARAM) -> BOOL {
        let t = &mut *(lp.0 as *mut Found);
        if !IsWindowVisible(h).as_bool() {
            return true.into();
        }
        let mut pid: u32 = 0;
        GetWindowThreadProcessId(h, Some(&mut pid as *mut u32));
        if pid != GetCurrentProcessId() {
            return true.into();
        }
        let mut buf = [0u16; 32];
        let len = GetWindowTextW(h, &mut buf) as usize;
        let title = String::from_utf16_lossy(&buf[..len.min(32)]);
        if title.starts_with("QuickSTT") {
            t.v.push((h.0 as isize, title));
        }
        true.into()
    }
    let mut found = Found { v: Vec::new() };
    unsafe {
        let _ = EnumWindows(Some(visit), LPARAM(&mut found as *mut Found as isize));
    }
    if found.v.is_empty() {
        return;
    }
    let start = std::time::Instant::now();
    let budget = std::time::Duration::from_millis(ms);
    let mut last: Vec<bool> = vec![false; found.v.len()];
    let mut flips = 0u32;
    let mut logged = 0u32;
    let mut ever = false;
    while start.elapsed() < budget {
        for (idx, (h, title)) in found.v.iter().enumerate() {
            let st = unsafe { GetWindowLongW(HWND(*h as _), GWL_STYLE) };
            let cap = st & WS_CAPTION_BIT != 0;
            if cap {
                ever = true;
            }
            if cap != last[idx] {
                flips += 1;
                last[idx] = cap;
                if logged < 10 {
                    logged += 1;
                    crate::log_line(&format!(
                        "styleflip '{title}' t=+{}ms captioned={cap} st={st:#x}",
                        start.elapsed().as_millis()
                    ));
                }
            }
        }
        std::thread::sleep(std::time::Duration::from_millis(3));
    }
    crate::log_line(&format!(
        "stylesample {}hwnd flips={flips} ever_captioned={ever}",
        found.v.len()
    ));
}

#[cfg(not(target_os = "windows"))]
fn sample_styles(_ms: u64) {}

/// Manual-drag state machine (native caption-drag can't show live snap
/// targets: press records cursor+window, motion follows the cursor, the
/// fullscreen overlay shows 3 pill-sized ghost outlines, release ALWAYS
/// snaps to the closest station — the pill is stationary, never free-floating).
#[derive(Clone, Copy, PartialEq)]
#[allow(dead_code)]
enum DragTarget {
    Pill,
    Dashboard,
    TextBoard,
}
struct DragState {
    active: bool,
    target: DragTarget,
    grab_cursor: (i32, i32),
    grab_win: (i32, i32),
    snap_idx: Option<usize>,
    press_at: std::time::Instant,
    press_n: u32,
    mic_used: bool,
    // Last station-highlight change (debounces rapid closest-station flips
    // so the overlay selection never flickers mid-drag).
    snap_at: std::time::Instant,
    // Last drag_moved activity (any press-owned motion event). Together with
    // press_at this detects a LOST RELEASE (focus stolen mid-press by UAC /
    // toast / Alt-Tab: Slint never delivers the up-event): a press with no
    // motion for seconds is dead — the poll force-ends it so every later
    // mouse move can't pop the overlay "out of nowhere".
    last_move: std::time::Instant,
    // True once the cursor travelled >=8px within the current press
    // (set from drag_moved while held). A double-click-with-drag sets
    // this; a clean double-click never dispatches motion, so the dock
    // hop can't be fooled by the live cursor wandering between ticks.
    moved_far: bool,
    // Always true now: the pill lives exactly on a dock (magnetic snap).
    docked_exact: bool,
}

/// Dock hop core: advance to the next of the 3 docks, adopt its orientation
/// and resize+move there. Real double-clicks pass force=false (suppressed
/// mid-drag so press-hold-drag keeps dragging); the CYCLE diagnostic passes
/// force=true for a deterministic hop.
fn dock_hop(
    pill_w: &slint::Weak<PillWidget>,
    corner_idx: &Arc<Mutex<usize>>,
    expanded: &Arc<Mutex<bool>>,
    vertical: &Arc<Mutex<bool>>,
    drag: &Arc<Mutex<DragState>>,
    suppress_menu_until: &Arc<Mutex<Option<std::time::Instant>>>,
    force: bool,
) {
    if !force {
        // A double-click WITH drag is a drag, not a dock hop: the second
        // press of a press-hold-drag also counts as a double-click in
        // Slint, which would otherwise teleport the window mid-gesture
        // and make dock-to-dock dragging impossible. Only real travel
        // within the press suppresses (latched in drag_moved).
        let dragging_far = drag
            .lock()
            .map(|d| d.active && d.moved_far)
            .unwrap_or(false);
        if dragging_far {
            log_line("ui: dock hop suppressed (double-click drag)");
            return;
        }
    }
    if let Some(p) = pill_w.upgrade() {
        let mut idx = mlock(&corner_idx);
        *idx = (*idx + 1) % 3;
        log_line(&format!("ui: dock cycle -> {}", *idx));
        let vert = *idx != 0;
        *mlock(&vertical) = vert;
        p.set_vertical(vert);
        p.set_mirror(*idx == 2);
        let (cw, ch) = cur_size(expanded, vertical);
        let scale = widget_platform::window_scale(p.window()).max(0.5);
        let (x, y) = widget_platform::dock_position(*idx, cw, ch, dock_gap(expanded), scale);
        drop(idx);
        place_pill(&p, x, y);
        set_footprint(&p, cw, ch);
        // A double-click's second release must not pop the tap menu:
        // swallow quick-tap menus briefly after every hop.
        if let Ok(mut s) = suppress_menu_until.lock() {
            *s = Some(std::time::Instant::now() + std::time::Duration::from_millis(600));
        }
        if let Ok(mut d) = drag.lock() {
            d.snap_idx = None;
            d.docked_exact = true;
        }
    }
}
    let drag = Arc::new(Mutex::new(DragState {
        active: false,
        target: DragTarget::Pill,
        grab_cursor: (0, 0),
        grab_win: (0, 0),
        snap_idx: None,
        press_at: std::time::Instant::now(),
        press_n: 0,
        mic_used: false,
        snap_at: std::time::Instant::now(),
        last_move: std::time::Instant::now(),
        moved_far: false,
        docked_exact: true,
    }));
    // Field-recorder arming: one dragcap per press (reset here, spent at the
    // first overlay show of the press).
    let dragcap_done: Arc<Mutex<bool>> = Arc::new(Mutex::new(false));

    // IPC-thread -> main-thread diagnostics (Slint is main-thread affine).
    let (diag_tx, diag_rx) = std::sync::mpsc::channel::<String>();
    spawn_ipc_listener(
        state.clone(),
        diag_tx,
        tx_cmd.clone(),
    );

    // Pre-warm engine path detection off the UI thread so the first PTT tap
    // doesn't pay cold filesystem probing inside the release-flush.
    std::thread::Builder::new()
        .name("engine-prewarm".into())
        .spawn(|| {
            let _ = quickstt_core::models::engine::cached_config();
        })
        .ok();

    // Ctrl+Space PTT: press opens the mic channel instantly, release flushes
    // even a 0.3s word ("ok") straight to the engine. Registration retries
    // until it sticks: another app (stale popup bridge, duplicate instance)
    // may hold the key at startup, and a silent failure here used to look
    // exactly like "Ctrl+Space does nothing".
    let hotkey_ok = Arc::new(Mutex::new(false));
    let hotkey_ok_poll = hotkey_ok.clone();
    let app_start = std::time::Instant::now();
    {
        use global_hotkey::hotkey::{Code, HotKey, Modifiers};
        use global_hotkey::{GlobalHotKeyEvent, GlobalHotKeyManager, HotKeyState};
        let ptt = HotKey::new(Some(Modifiers::CONTROL), Code::Space);
        let ptt_id = ptt.id();
        let tx = tx_cmd.clone();
        let st = state.clone();
        let ph = ptt_held.clone();
        let fg_ptt = last_external_fg.clone();
        std::thread::Builder::new()
            .name("ptt-hotkey".into())
            .spawn(move || {
                // No-X guard (Linux): the global-hotkey backend segfaults in
                // XDefaultRootWindow on a null display (proven by core dump
                // on a DISPLAY-less Wayland launch). Skip registration
                // instead of crashing; hotkeys simply stay unavailable.
                #[cfg(not(target_os = "windows"))]
                if !widget_platform::can_open_x_display() {
                    log_line("hotkeys disabled: no X display reachable");
                    return;
                }
                let manager = loop {
                    match GlobalHotKeyManager::new() {
                        Ok(m) => match m.register(ptt.clone()) {
                            Ok(()) => {
                                log_line("Ctrl+Space PTT registered");
                                break m;
                            }
                            Err(e) => {
                                log_line(&format!("Ctrl+Space busy, retrying ({e})"));
                            }
                        },
                        Err(e) => {
                            log_line(&format!("hotkey manager unavailable, retrying ({e})"));
                        }
                    }
                    std::thread::sleep(std::time::Duration::from_secs(2));
                };
                // Dropping the manager would unregister the OS hook, so it
                // lives until the process exits.
                std::mem::forget(manager);
                *mlock(&hotkey_ok) = true;
                loop {
                    // Pump THIS thread's message queue on Windows: the global-hotkey
                    // hidden window was created on this thread, and its
                    // WM_HOTKEY is only dispatched when this thread pumps.
                    // Without this, registration succeeds but press/release
                    // events sit in the queue forever (exactly the observed
                    // "Ctrl+Space does nothing"; egui worked because its main
                    // thread pumps messages). Non-Windows needs no pump — the
                    // try_recv + 5ms sleep below already paces the loop.
                    #[cfg(target_os = "windows")]
                    {
                        use windows::Win32::Foundation::HWND;
                        use windows::Win32::UI::WindowsAndMessaging::{
                            DispatchMessageW, PeekMessageW, MSG, PM_REMOVE,
                        };
                        unsafe {
                            let mut msg = MSG::default();
                            while PeekMessageW(&mut msg, HWND(0), 0, 0, PM_REMOVE)
                                .as_bool()
                            {
                                let _ = DispatchMessageW(&msg);
                            }
                        }
                    }
                    match GlobalHotKeyEvent::receiver().try_recv() {
                        Ok(ev) if ev.id == ptt_id => {
                            if ev.state == HotKeyState::Pressed {
                                capture_current_fg_window(&fg_ptt);
                                log_line("PTT press");
                                *mlock(&ph) = true;
                                if let Ok(mut s) = st.lock() {
                                    s.widget_visible = true;
                                    if s.status_message.eq_ignore_ascii_case("Hidden") {
                                        s.status_message = "Listening...".into();
                                    }
                                }
                                if tx.try_send(OrchestratorCommand::StartListening).is_err() {
                                    log_line("PTT send FAILED (command loop dead?)");
                                }
                            } else if ev.state == HotKeyState::Released {
                                log_line("PTT release");
                                *mlock(&ph) = false;
                                if tx.try_send(OrchestratorCommand::StopListening).is_err() {
                                    log_line("PTT send FAILED (command loop dead?)");
                                }
                            }
                        }
                        Ok(_) => {}
                        Err(_) => std::thread::sleep(std::time::Duration::from_millis(5)),
                    }
                }
            })
            .ok();
    }

    pill.set_live_text("".into());
    pill.set_alert_text("".into());
    pill.set_expanded(false);
    pill.set_phase(0);
    pill.set_upper_w(184);
    pill.set_upper_op(1.0);
    pill.set_slide_px(0);
    pill.set_mic_grow(1.0);
    let levels: Vec<f32> = vec![0.0; 24];
    pill.set_levels(ModelRc::new(VecModel::from_slice(&levels)));

    // ── pill callbacks ──
    // Mic capsule toggles dictation with instant visual feedback. The core
    // arms WakewordListening (which renders as idle), so the pill ALSO tracks
    // mic_live locally: ON => phase-1 bars + red mic + expanded, OFF =>
    // collapse. Marks mic_used so the release is not mistaken for a
    // background left-tap (menu).
    // Mic press-down joins the shared drag cycle (press-drag from the mic
    // moves the pill) and marks the gesture mic-owned so the release never
    // pops the tap menu.
    pill.on_mic_pressed({
        let drag = drag.clone();
        let fg_mic_press = last_external_fg.clone();
        move || {
            capture_current_fg_window(&fg_mic_press);
            mlock(&drag).mic_used = true;
        }
    });
    pill.on_mic_clicked({
        let tx_cmd = tx_cmd.clone();
        let pill = pill.as_weak();
        let drag = drag.clone();
        let state = state.clone();
        let mic_live = mic_live.clone();
        let fg_mic_click = last_external_fg.clone();
        move || {
            capture_current_fg_window(&fg_mic_click);
            log_line("ui: mic clicked");
            mlock(&drag).mic_used = true;
            // A press that travelled is a DRAG from the mic, not a tap:
            // the drag cycle already ran on release — never toggle.
            if drag.lock().map(|d| d.moved_far).unwrap_or(false) {
                log_line("ui: mic drag — toggle suppressed");
                return;
            }
            if pill.upgrade().is_none() {
                return;
            }
            // Live if the mic is already ON locally, PTT is held, or the core
            // is mid-turn — otherwise a second tap while arming would restart
            // instead of stopping.
            let live = *mlock(&mic_live)
                || state
                    .lock()
                    .map(|s| {
                        s.mode == AppMode::Recording
                            || s.mode == AppMode::Transcribing
                            || s.mode == AppMode::WakewordListening
                    })
                    .unwrap_or(false);
            let cmd = if live {
                *mlock(&mic_live) = false;
                OrchestratorCommand::StopListening
            } else {
                *mlock(&mic_live) = true;
                log_line("ui: mic ON (capture opening, bars up)");
                OrchestratorCommand::StartListening
            };
            if tx_cmd.try_send(cmd).is_err() {
                log_line("mic send FAILED (command loop dead?)");
                *mlock(&mic_live) = false;
            }
        }
    });

    // Models are managed in the full Qt dashboard (right-click the pill
    // or use the tray menu) — the mini pill itself carries no menu.

    {
        let dash = dashboard.as_weak();
        let dash_visible = dash_visible.clone();
        pill.on_open_dashboard(move || {
            log_line("ui: dashboard open");
            if let Ok(mut v) = dash_visible.lock() {
                *v = true;
            }
            if let Some(d) = dash.upgrade() {
                widget_platform::show_app_window(d.window());
            }
        });
    }

    // (MiniPill has no model chip / close button: model+language live in the
    // dashboard and the pinned menu, hide lives in the tray. Double-click
    // anywhere cycles docks; the dashboard lives in the tray menu.)

    // ── Drag callbacks (DragState lives above, near `expanded`) ──

    let overlay = DockOverlay::new().unwrap();
    let _ = overlay.hide();
    widget_platform::force_layered(overlay.window());

    // Place the 3 exact station ghosts (mini footprints at the true dock
    // gaps) + the 3 exact expanded previews the selected ghost morphs into.
    // Rects only — fullscreen sizing happens at the show sites after show().
    let layout_overlay = {
        let overlay = overlay.as_weak();
        let pill_w = pill.as_weak();
        move || {
            let (Some(o), Some(p)) = (overlay.upgrade(), pill_w.upgrade()) else {
                return;
            };
            let scale = widget_platform::window_scale(p.window()).max(0.5);
            // NOTE: rects only here. Fullscreen sizing happens at the show
            // sites AFTER show(): a set_size before first show neither sticks
            // (window kept its 1920x1080 markup size -> 2400x1350 physical)
            // nor ever paints (invisible overlay).
            // Station rects in LOGICAL px (physical dock math / scale).
            // Mini ghosts sit on the exact dock footprints with the exact
            // gaps; the selected ghost morphs into the exact expanded
            // preview (set below) — truthful drop targets, never blobs.
            let station = |idx: usize, lw: i32, lh: i32, gap: i32| {
                let (px, py) = widget_platform::dock_position(idx, lw, lh, gap, scale);
                (
                    (px as f32 / scale).round() as i32,
                    (py as f32 / scale).round() as i32,
                )
            };
            let (m0x, m0y) = station(0, MINI_W, MINI_H, EDGE_GAP);
            // Idle ghosts run ~1.75x the mini footprint (58x14 bottom,
            // 15x59 sides), centred on the dock anchor — clearly visible,
            // still unmistakably the mini. Selected ghost still morphs into
            // the full expanded preview.
            // Ghost geometry is LENGTH props in Slint (int*length position
            // math silently breaks: step-4 proof) — convert here.
            let (g0w, g0h) = (MINI_W * 7 / 4, MINI_H * 7 / 4);
            o.set_dock0_x((m0x - (g0w - MINI_W) / 2) as f32);
            o.set_dock0_y((m0y - (g0h - MINI_H) / 2) as f32);
            o.set_dock0_w(g0w as f32);
            o.set_dock0_h(g0h as f32);
            let (m1x, m1y) = station(1, SIDE_MINI_W, SIDE_MINI_H, EDGE_GAP);
            let (g1w, g1h) = (SIDE_MINI_W * 7 / 4, SIDE_MINI_H * 7 / 4);
            o.set_dock1_x((m1x - (g1w - SIDE_MINI_W) / 2) as f32);
            o.set_dock1_y((m1y - (g1h - SIDE_MINI_H) / 2) as f32);
            o.set_dock1_w(g1w as f32);
            o.set_dock1_h(g1h as f32);
            let (m2x, m2y) = station(2, SIDE_MINI_W, SIDE_MINI_H, EDGE_GAP);
            o.set_dock2_x((m2x - (g1w - SIDE_MINI_W) / 2) as f32);
            o.set_dock2_y((m2y - (g1h - SIDE_MINI_H) / 2) as f32);
            o.set_dock2_w(g1w as f32);
            o.set_dock2_h(g1h as f32);
            // Expanded-preview rects: the EXACT footprints the pill would
            // take at each station (same dock math, same gaps as the real
            // thing — bottom 240x86, sides 252x46). The selected ghost morphs
            // mini->expanded so the drop preview is truthful, never a blob.
            let (e0x, e0y) = station(0, EXP_W, EXP_H, EXP_GAP);
            o.set_dock0_ex(e0x as f32);
            o.set_dock0_ey(e0y as f32);
            o.set_dock0_ew(EXP_W as f32);
            o.set_dock0_eh(EXP_H as f32);
            let (e1x, e1y) = station(1, SIDE_EXP_W, SIDE_EXP_H, EXP_GAP);
            o.set_dock1_ex(e1x as f32);
            o.set_dock1_ey(e1y as f32);
            o.set_dock1_ew(SIDE_EXP_W as f32);
            o.set_dock1_eh(SIDE_EXP_H as f32);
            let (e2x, e2y) = station(2, SIDE_EXP_W, SIDE_EXP_H, EXP_GAP);
            o.set_dock2_ex(e2x as f32);
            o.set_dock2_ey(e2y as f32);
            o.set_dock2_ew(SIDE_EXP_W as f32);
            o.set_dock2_eh(SIDE_EXP_H as f32);
            o.set_active_dock(-1);
        }
    };
    // Shared by press-move (first real movement) and the OVERLAY test hook.
    let layout_overlay = std::sync::Arc::new(layout_overlay);

    // Closest dock to the pill center (physical px) — ALWAYS snaps (the pill
    // is stationary: whichever station is closest on release wins, no matter
    // how far). Also drives the black-outline highlight on the overlay.
    let nearest_dock = {
        let expanded_nd = expanded.clone();
        let vertical_nd = vertical.clone();
        move |cx: i32, cy: i32, scale: f32| -> usize {
            let (cw, ch) = cur_size(&expanded_nd, &vertical_nd);
            let s = scale.max(0.5);
            let pw = (cw as f32 * s).round() as i32;
            let ph = (ch as f32 * s).round() as i32;
            let mut best: usize = 0;
            let mut best_d = i32::MAX;
            let nd_gap = dock_gap(&expanded_nd);
            for (i, (dx, dy)) in [
                widget_platform::dock_position(0, cw, ch, nd_gap, scale),
                widget_platform::dock_position(1, cw, ch, nd_gap, scale),
                widget_platform::dock_position(2, cw, ch, nd_gap, scale),
            ]
            .iter()
            .enumerate()
            {
                let d =
                    (cx - (dx + pw / 2)).abs().max((cy - (dy + ph / 2)).abs());
                if d < best_d {
                    best_d = d;
                    best = i;
                }
            }
            best
        }
    };

    // Drag + double-click dock cycling (3 docks now)
    {
        let pill_w = pill.as_weak();
        let drag = drag.clone();
        let foot_ds = foot_anim.clone();
        let expanded_ds = expanded.clone();
        let dragcap_ds = dragcap_done.clone();
        pill.on_drag_start(move || {
            let Some(p) = pill_w.upgrade() else {
                return;
            };
            let mut d = mlock(&drag);
            d.press_at = std::time::Instant::now();
            d.last_move = d.press_at;
            d.press_n = d.press_n.wrapping_add(1);
            // Finish any in-flight morph INSTANTLY (content props only —
            // the window never moves): a half-morphed pill would fight the
            // finger.
            if let Some(an) = mlock(&foot_ds).take() {
                if an.expanding {
                    p.set_mic_grow(1.0);
                    p.set_upper_op(1.0);
                    p.set_slide_px(0);
                    p.set_rise_px(0);
                } else {
                    p.set_expanded(false);
                    *mlock(&expanded_ds) = false;
                    p.set_mic_grow(0.0);
                    p.set_upper_op(0.0);
                    p.set_slide_px(0);
                    p.set_rise_px(0);
                }
            }
            if d.active {
                return;
            }
            d.moved_far = false;
            let pos = p.window().position();
            d.active = true;
            d.target = DragTarget::Pill;
            d.grab_cursor = widget_platform::cursor_position();
            d.grab_win = (pos.x, pos.y);
            d.snap_idx = None;
            d.snap_at = std::time::Instant::now();
            *mlock(&dragcap_ds) = false;
            log_line(&format!(
                "ui: drag start @ {},{} win {},{}",
                d.grab_cursor.0, d.grab_cursor.1, pos.x, pos.y
            ));
            drop(d);
            // Tile trap: snapshot the world on press (census + screenshot
            // now and mid-hold) so a press-correlated popup is identified by
            // class/owner, not guessed at.
            spawn_presscap();
            // No overlay on PRESS: a tap is not a drag. The shade appears on
            // the first real movement (on_drag_moved) — showing it here
            // flashed a fullscreen blip on every mic tap and every stop tap.
            widget_platform::restack_topmost(p.window());
        });
    }
    {
        let pill_w = pill.as_weak();
        let overlay_w = overlay.as_weak();
        let drag = drag.clone();
        let vertical_dm = vertical.clone();
        let expanded_dm = expanded.clone();
        let layout_dm = layout_overlay.clone();
        let shown_dm = overlay_shown_at.clone();
        let dragcap_dm = dragcap_done.clone();
        pill.on_drag_moved(move || {
            let (Some(p), Some(o)) = (pill_w.upgrade(), overlay_w.upgrade()) else {
                return;
            };
            let mut d = mlock(&drag);
            if !d.active || d.target != DragTarget::Pill {
                return;
            }
            d.last_move = std::time::Instant::now();
            let (cx, cy) = widget_platform::cursor_position();
            // Latch real travel within this press (drives the
            // double-click-drag hop suppression; hover motion never reaches
            // here because no press is active).
            if (cx - d.grab_cursor.0)
                .abs()
                .max((cy - d.grab_cursor.1).abs())
                >= 8
            {
                d.moved_far = true;
            }
            // The shade appears on FIRST REAL MOVEMENT only — never on
            // press, so quick taps (mic on/off) never flash the screen.
            // The press must also be FRESH (<10s): with a lost release the
            // press goes stale and must never pop the overlay again.
                // Fullscreen sizing AFTER show (pre-show set_size breaks first
                // paint). Native set_size is authoritative for the HWND (ov
                // props alone leave the markup-default size); rects after
                // sizing so ghosts land pixel-exact.
            if d.moved_far
                && d.press_at.elapsed() < std::time::Duration::from_secs(10)
                && !o.window().is_visible()
            {
                widget_platform::show_overlay(o.window());
                let scale_ov = widget_platform::window_scale(p.window()).max(0.5);
                let (sw, sh) = widget_platform::screen_size();
                // Fullscreen via live props + authoritative native set_size
                // (props alone do not resize the HWND).
                let (lw, lh) = (
                    (sw as f32 / scale_ov).round(),
                    (sh as f32 / scale_ov).round(),
                );
                o.set_ov_w(lw);
                o.set_ov_h(lh);
                // Full-damage nudge FIRST (see dock.slint scrim): identical
                // rects across shows damage nothing, so flip the bool here.
                o.set_refresh_dark(!o.get_refresh_dark());
                // Native size is authoritative for the HWND (ov props alone
                // do not resize it: without this the window keeps its markup
                // defaults, oversized and off-true).
                o.window().set_size(slint::LogicalSize {
                    width: lw,
                    height: lh,
                });
                o.window()
                    .set_position(slint::PhysicalPosition { x: 0, y: 0 });
                // Strip + paint AFTER sizing (a resize drops the request made
                // inside show_overlay and can re-assert chrome): this is the
                // request that survives.
                widget_platform::ensure_frameless(o.window(), true);
                o.window().request_redraw();
                layout_dm();
                widget_platform::restack_topmost(p.window());
                *mlock(&shown_dm) = Some(std::time::Instant::now());
                log_line(&format!(
                    "overlay shown (dragging) ovrect={:?} pillrect={:?}",
                    widget_platform::native_rect(o.window()),
                    widget_platform::native_rect(p.window())
                ));
                // Field evidence, zero coordination: screenshot + census land
                // in %TEMP% while the gesture is still live.
                if !*mlock(&dragcap_dm) {
                    *mlock(&dragcap_dm) = true;
                    spawn_dragcap("drag");
                }
            }
            let nx = d.grab_win.0 + (cx - d.grab_cursor.0);
            let ny = d.grab_win.1 + (cy - d.grab_cursor.1);
            let (cw, ch) = cur_size(&expanded_dm, &vertical_dm);
            let scale = widget_platform::window_scale(p.window()).max(0.5);
            let pw = (cw as f32 * scale).round() as i32;
            let ph = (ch as f32 * scale).round() as i32;
            let (sw, sh) = widget_platform::screen_size();
            let nx = nx.clamp(0, (sw - pw).max(0));
            let ny = ny.clamp(0, (sh - ph).max(0));
            drop(d);
            place_pill(&p, nx, ny);
            // Closest station wins the highlight + drop target, debounced:
            // flips within 120ms of the last change are ignored so the
            // selection pop never flickers when the cursor sits between
            // stations. First highlight of a press applies instantly.
            let snap = nearest_dock(nx + pw / 2, ny + ph / 2, scale);
            {
                let mut dd = mlock(&drag);
                let cur = dd.snap_idx;
                if Some(snap) != cur
                    && (cur.is_none()
                        || dd.snap_at.elapsed() > std::time::Duration::from_millis(120))
                {
                    dd.snap_idx = Some(snap);
                    dd.snap_at = std::time::Instant::now();
                    o.set_active_dock(snap as i32);
                    log_line(&format!("ui: drag snap {snap} @ {nx},{ny}"));
                } else if cur.is_none() {
                    dd.snap_idx = Some(snap);
                    o.set_active_dock(snap as i32);
                }
            }
        });
    }
    // NOTE: pill.on_drag_end is wired AFTER the shared black menu exists
    // (below), because a quick left-tap (no drag, not on mic) opens that same
    // menu — and right-click opens it too. See the drag_end block below.
    {
        let pending_hop_cc = pending_hop.clone();
        let drag_cc = drag.clone();
        pill.on_cycle_corner(move || {
            // Never hop synchronously: the second press of the double-click
            // is still down here, and an immediate hop would teleport+resize
            // the window under the finger (white flash, stale grab). The
            // poll commits or cancels this once the gesture declares itself.
            let n = mlock(&drag_cc).press_n;
            *mlock(&pending_hop_cc) = Some((std::time::Instant::now(), n));
            log_line("ui: double-click latched (hop pending)");
        });
    }

    // Dashboard window close request (native OS 'X' button)
    {
        let dash_vis = dash_visible.clone();
        dashboard.window().on_close_requested(move || {
            if let Ok(mut v) = dash_vis.lock() {
                *v = false;
            }
            slint::CloseRequestResponse::HideWindow
        });
    }
    dashboard.on_drag_start(|| {});
    dashboard.on_drag_moved(|| {});
    dashboard.on_drag_end(|| {});
    let _ = dashboard.hide();

    // ── TextBoard: floating transcript popup (Slint twin of the egui
    // TextBoard viewport). Auto-shows near the pill on transcription
    // activity; × closes it until the next session starts.
    let textboard = TextBoard::new().unwrap();
    let _ = textboard.hide();
    widget_platform::force_layered(textboard.window());
    textboard.set_title_text("QuickSTT TextBoard".into());
    textboard.set_body_text("Transcript will appear here...".into());
    let tb_closed = Arc::new(Mutex::new(false));
    // Auto-follow: the visible board tracks the pill (hover-expand, dock
    // hops). A user drag of the board disables follow until it re-shows.
    let tb_follow = Arc::new(Mutex::new(true));
    {
        let tb = textboard.as_weak();
        let drag = drag.clone();
        let tb_follow = tb_follow.clone();
        textboard.on_drag_start(move || {
            let Some(w) = tb.upgrade() else { return };
            *mlock(&tb_follow) = false;
            let mut d = mlock(&drag);
            if d.active {
                return;
            }
            let pos = w.window().position();
            d.active = true;
            d.target = DragTarget::TextBoard;
            d.grab_cursor = widget_platform::cursor_position();
            d.grab_win = (pos.x, pos.y);
            d.snap_idx = None;
        });
    }
    {
        let tb = textboard.as_weak();
        let drag = drag.clone();
        textboard.on_drag_moved(move || {
            let Some(w) = tb.upgrade() else { return };
            let d = mlock(&drag);
            if !d.active || d.target != DragTarget::TextBoard {
                return;
            }
            let (cx, cy) = widget_platform::cursor_position();
            let nx = d.grab_win.0 + (cx - d.grab_cursor.0);
            let ny = d.grab_win.1 + (cy - d.grab_cursor.1);
            drop(d);
            w.window()
                .set_position(slint::PhysicalPosition { x: nx.max(0), y: ny.max(0) });
        });
    }
    {
        let drag = drag.clone();
        textboard.on_drag_end(move || {
            let mut d = mlock(&drag);
            if d.active && d.target == DragTarget::TextBoard {
                d.active = false;
            }
        });
    }
    {
        let tb = textboard.as_weak();
        let tb_closed = tb_closed.clone();
        let tb_full = tb_full.clone();
        textboard.on_close_board(move || {
            if let Some(w) = tb.upgrade() {
                let _ = w.hide();
            }
            *mlock(&tb_closed) = true;
            *mlock(&tb_full) = false;
            log_line("textboard closed by user");
        });
    }
    {
        let tb = textboard.as_weak();
        textboard.on_copy_text(move || {
            let Some(w) = tb.upgrade() else { return };
            let text = w.get_body_text().to_string();
            #[cfg(target_os = "windows")]
            copy_to_windows_clipboard(&text);
            w.set_copied_indicator(true);
            log_line(&format!("textboard text copied to clipboard ({} chars)", text.len()));
            let tb_reset = tb.clone();
            slint::Timer::single_shot(std::time::Duration::from_millis(1500), move || {
                if let Some(w2) = tb_reset.upgrade() {
                    w2.set_copied_indicator(false);
                }
            });
        });
    }

    // ── tray: one app, one icon (this one). Previous-generation rows:
    // Dashboard / Show Widget / Hide Widget / Quit App. "Widget" is the main
    // C++ Qt widget: Show/Hide (and plain tray clicks) own its visibility,
    // Dashboard opens the full Qt dashboard, Quit stops everything.
    // The tray menu itself is NATIVE (muda): the OS owns placement, focus,
    // keyboard/a11y and dismissal, so it works every time — no more
    // hand-rolled cursor polling or topmost/z-order fights. The Slint
    // TrayMenu window below is retained only for the pill's own flows; tray
    // right-click no longer opens it.
    // Dark Win32 theme for the native tray popup (black bg, white text) —
    // must precede menu creation; falls back to light on old Windows.
    #[cfg(target_os = "windows")]
    enable_dark_native_menus();
    // Build tray icon with retries — Shell_NotifyIconW can return E_FAIL
    // transiently on Windows, especially right after startup or if the
    // notification area hasn't fully initialised.
    // Linux uses the SNI tray below (libappindicator needs a panel
    // Indicator Plugin most XFCE panels lack, so it stays invisible).
    #[cfg(not(target_os = "linux"))]
    let _tray = {
        let icon_for_retry = load_tray_icon();
        let mut last_err = String::new();
        let mut tray_result = None;
        for attempt in 0..5 {
            let mut b = tray_icon::TrayIconBuilder::new()
                .with_tooltip("QuickSTT");
            if let Some(ref icon_data) = icon_for_retry {
                // Clone the icon data for each retry.
                if let Ok(ic) = tray_icon::Icon::from_rgba(
                    icon_data.0.clone(), icon_data.1, icon_data.2,
                ) {
                    b = b.with_icon(ic);
                }
            }
            match b.build() {
                Ok(tray) => {
                    if attempt > 0 {
                        log_line(&format!("tray icon created on attempt {}", attempt + 1));
                    }
                    tray_result = Some(tray);
                    break;
                }
                Err(e) => {
                    last_err = format!("{e}");
                    log_line(&format!(
                        "tray icon attempt {} failed: {e}", attempt + 1
                    ));
                    std::thread::sleep(std::time::Duration::from_millis(200));
                }
            }
        }
        if tray_result.is_none() {
            log_line(&format!(
                "tray: notification area unavailable ({last_err}); running in widget-only mode"
            ));
        }
        tray_result
    };
    // SNI tray (Linux): polled below next to the TrayIconEvent pump.
    #[cfg(target_os = "linux")]
    let sni_rx = sni_tray::spawn(load_tray_icon());
    #[cfg(target_os = "linux")]
    let sni_rx = std::sync::Arc::new(std::sync::Mutex::new(sni_rx));
    #[cfg(target_os = "windows")]
    apply_dark_theme_to_process_windows();

    // Black menu popup, shared by tray right-click and pill right-click.
    // Shown at an explicit event position when the caller has one (tray
    // events carry the exact click point), else at the live cursor.
    let traymenu = TrayMenu::new().unwrap();
    let _ = traymenu.hide();
    widget_platform::force_layered(traymenu.window());
    let menu_open_at: Arc<Mutex<Option<std::time::Instant>>> = Arc::new(Mutex::new(None));
    let show_traymenu = {
        let tm = traymenu.as_weak();
        let pill_wm = pill.as_weak();
        let menu_open_at = menu_open_at.clone();
        move |pos: Option<(i32, i32)>| {
            let (Some(t), Some(p)) = (tm.upgrade(), pill_wm.upgrade()) else {
                return;
            };
            if t.window().is_visible() {
                let _ = t.hide();
                return;
            }
            const MW: i32 = 220;
            const MH: i32 = 144;
            let scale = widget_platform::window_scale(p.window()).max(0.5);
            // Clamp into the WORK area (not the full screen) so the menu
            // never lands behind the taskbar.
            let (wl, wt, wr, wb) = widget_platform::work_area();
            let ww = (MW as f32 * scale).round() as i32;
            let hh = (MH as f32 * scale).round() as i32;
            let (cx, cy) = pos.unwrap_or_else(widget_platform::cursor_position);
            let nx = (cx - ww / 2).min((wr - ww - 8).max(wl + 8)).max(wl + 8);
            let ny = if cy + hh > wb {
                (wb - hh - 6).max(wt)
            } else {
                (cy + 6).max(wt)
            };
            t.window()
                .set_position(slint::PhysicalPosition { x: nx, y: ny });
            widget_platform::show_widget(t.window());
            widget_platform::restack_topmost(t.window());
            t.window().request_redraw();
            *mlock(&menu_open_at) = Some(std::time::Instant::now());
            log_line(&format!("menu shown @ {nx},{ny} size {ww}x{hh}"));
        }
    };
    let show_traymenu = std::sync::Arc::new(show_traymenu);
    // (Pill right-click is wired below, after the dark menu exists: it opens
    // the dark pill menu. The classic black tray menu lives on the tray
    // icon's right-click.)
    let pillmenu = PillMenu::new().unwrap();
    let _ = pillmenu.hide();
    widget_platform::force_layered(pillmenu.window());
    let flyout = MenuFlyout::new().unwrap();
    let _ = flyout.hide();
    widget_platform::force_layered(flyout.window());
    // Pinned submenu (row click keeps it open for touch users); None =
    // hover-driven. FlyoutUi mirrors the live flyout for the poll loop.
    struct FlyoutUi {
        mode: SubMode,
        items: Vec<String>,
        rect: Option<(i32, i32, i32, i32)>,
    }
    let menu_pin: Arc<Mutex<Option<SubMode>>> = Arc::new(Mutex::new(None));
    let flyout_ui: Arc<Mutex<FlyoutUi>> = Arc::new(Mutex::new(FlyoutUi {
        mode: SubMode::None,
        items: Vec::new(),
        rect: None,
    }));
    let menu2_open_at: Arc<Mutex<Option<std::time::Instant>>> = Arc::new(Mutex::new(None));
    let show_pillmenu = {
        let pm = pillmenu.as_weak();
        let pill_wm = pill.as_weak();
        let fly = flyout.as_weak();
        let pin = menu_pin.clone();
        let ui = flyout_ui.clone();
        let opened = menu2_open_at.clone();
        move |pos: Option<(i32, i32)>| {
            let (Some(m), Some(p)) = (pm.upgrade(), pill_wm.upgrade()) else {
                return;
            };
            if let Some(f) = fly.upgrade() {
                let _ = f.hide();
            }
            *mlock(&pin) = None;
            if let Ok(mut u) = ui.lock() {
                u.mode = SubMode::None;
                u.items.clear();
                u.rect = None;
            }
            let scale = widget_platform::window_scale(p.window()).max(0.5);
            let (wl, wt, wr, wb) = widget_platform::work_area();
            let ww = (PM_W as f32 * scale).round() as i32;
            let hh = (PM_H as f32 * scale).round() as i32;
            let (cx, cy) = pos.unwrap_or_else(widget_platform::cursor_position);
            let nx = cx.min((wr - ww).max(wl)).max(wl);
            let ny = if cy + hh > wb {
                (wb - hh).max(wt)
            } else {
                cy.max(wt)
            };
            m.set_menu_op(0.0);
            m.window()
                .set_position(slint::PhysicalPosition { x: nx, y: ny });
            widget_platform::show_widget(m.window());
            widget_platform::restack_topmost(m.window());
            m.window().request_redraw();
            *mlock(&opened) = Some(std::time::Instant::now());
            log_line(&format!("pill menu shown @ {nx},{ny}"));
            log_line(&widget_platform::window_state_debug("pillmenu", m.window()));
        }
    };
    let show_pillmenu = std::sync::Arc::new(show_pillmenu);
    // Pill right-click opens the dark pill menu (Hide / Settings /
    // Microphone / Model / History / Paste) at the cursor. Menus are
    // right-click only; a quick left-tap opens nothing.
    {
        let show = show_pillmenu.clone();
        pill.on_right_clicked(move || {
            log_line("ui: right click -> dark pill menu");
            show(None);
        });
    }
    // Row actions (shared by Slint taps; MENU2ROW tests the same path via
    // synthetic clicks, so IPC needs no separate mapping here).
    {
        let pm = pillmenu.as_weak();
        let fly = flyout.as_weak();
        let pw = pill.as_weak();
        let anim = foot_anim.clone();
        let hide_until = hide_until.clone();
        pillmenu.on_hide_row(move || {
            *mlock(&hide_until) =
                Some(std::time::Instant::now() + std::time::Duration::from_secs(3600));
            hide_pill_menus(&pm, &fly);
            if let Some(p) = pw.upgrade() {
                let (mg0, uo0) = (p.get_mic_grow(), p.get_upper_op());
                *mlock(&anim) = Some(FootAnim {
                    t0: std::time::Instant::now(),
                    dur_ms: 200,
                    expanding: false,
                    mg0,
                    uo0,
                });
            }
            log_line("menu: pill hidden for 1 hour");
        });
    }
    {
        let pm = pillmenu.as_weak();
        let fly = flyout.as_weak();
        let pw = pill.as_weak();
        let anim = foot_anim.clone();
        let dash_menu = dashboard.as_weak();
        let pill_menu_w = pill.as_weak();
        let dash_visible_menu = dash_visible.clone();
        let state_menu = state.clone();
        pillmenu.on_settings_row(move || {
            hide_pill_menus(&pm, &fly);
            if let Some(p) = pw.upgrade() {
                let (mg0, uo0) = (p.get_mic_grow(), p.get_upper_op());
                *mlock(&anim) = Some(FootAnim {
                    t0: std::time::Instant::now(),
                    dur_ms: 200,
                    expanding: false,
                    mg0,
                    uo0,
                });
            }
            menu_action(0, &dash_menu, &pill_menu_w, &dash_visible_menu, &state_menu);
        });
    }
    {
        let pm = pillmenu.as_weak();
        let fly = flyout.as_weak();
        let pw = pill.as_weak();
        let state = state.clone();
        let pin = menu_pin.clone();
        let ui = flyout_ui.clone();
        pillmenu.on_mic_row(move || {
            let mut pinned = mlock(&pin);
            if *pinned == Some(SubMode::Mic) {
                *pinned = None;
                if let Some(f) = fly.upgrade() {
                    let _ = f.hide();
                }
                if let Ok(mut u) = ui.lock() {
                    u.mode = SubMode::None;
                    u.rect = None;
                }
                log_line("menu: mic flyout unpinned");
            } else {
                *pinned = Some(SubMode::Mic);
                drop(pinned);
                if let Some((items, rect)) =
                    open_submenu(&fly, &pm, &pw, &state, SubMode::Mic)
                {
                    if let Ok(mut u) = ui.lock() {
                        u.mode = SubMode::Mic;
                        u.items = items;
                        u.rect = Some(rect);
                    }
                }
                log_line("menu: mic flyout pinned");
            }
        });
    }
    {
        let pm = pillmenu.as_weak();
        let fly = flyout.as_weak();
        let pw = pill.as_weak();
        let state = state.clone();
        let pin = menu_pin.clone();
        let ui = flyout_ui.clone();
        pillmenu.on_model_row(move || {
            let mut pinned = mlock(&pin);
            if *pinned == Some(SubMode::Model) {
                *pinned = None;
                if let Some(f) = fly.upgrade() {
                    let _ = f.hide();
                }
                if let Ok(mut u) = ui.lock() {
                    u.mode = SubMode::None;
                    u.rect = None;
                }
                log_line("menu: model flyout unpinned");
            } else {
                *pinned = Some(SubMode::Model);
                drop(pinned);
                if let Some((items, rect)) =
                    open_submenu(&fly, &pm, &pw, &state, SubMode::Model)
                {
                    if let Ok(mut u) = ui.lock() {
                        u.mode = SubMode::Model;
                        u.items = items;
                        u.rect = Some(rect);
                    }
                }
                log_line("menu: model flyout pinned");
            }
        });
    }
    {
        let pm = pillmenu.as_weak();
        let fly = flyout.as_weak();
        let pw = pill.as_weak();
        let anim = foot_anim.clone();
        let state = state.clone();
        let tb_full = tb_full.clone();
        let tb_closed = tb_closed.clone();
        pillmenu.on_history_row(move || {
            let has = state
                .lock()
                .map(|s| !s.transcript_buffer.trim().is_empty())
                .unwrap_or(false);
            if has {
                *mlock(&tb_full) = true;
                *mlock(&tb_closed) = false;
                hide_pill_menus(&pm, &fly);
                if let Some(p) = pw.upgrade() {
                    let (mg0, uo0) = (p.get_mic_grow(), p.get_upper_op());
                    *mlock(&anim) = Some(FootAnim {
                        t0: std::time::Instant::now(),
                        dur_ms: 200,
                        expanding: false,
                        mg0,
                        uo0,
                    });
                }
                log_line("menu: transcript history");
            } else {
                log_line("menu: history empty — nothing to show");
            }
        });
    }
    {
        let pm = pillmenu.as_weak();
        let fly = flyout.as_weak();
        let pw = pill.as_weak();
        let anim = foot_anim.clone();
        let last_turn = last_turn.clone();
        let last_external_fg_paste = last_external_fg.clone();
        pillmenu.on_paste_row(move || {
            let t = last_turn.lock().map(|s| s.clone()).unwrap_or_default();
            if t.trim().is_empty() {
                log_line("menu: nothing to paste yet");
            } else {
                hide_pill_menus(&pm, &fly);
                if let Some(p) = pw.upgrade() {
                    let (mg0, uo0) = (p.get_mic_grow(), p.get_upper_op());
                    *mlock(&anim) = Some(FootAnim {
                        t0: std::time::Instant::now(),
                        dur_ms: 200,
                        expanding: false,
                        mg0,
                        uo0,
                    });
                }
                log_line(&format!("menu: paste last turn ({} chars)", t.len()));
                let target_fg = *mlock(&last_external_fg_paste);
                deliver_transcription_output(&t, 0, target_fg);
            }
        });
    }
    {
        let pm = pillmenu.as_weak();
        let fly = flyout.as_weak();
        let pw = pill.as_weak();
        let anim = foot_anim.clone();
        let pin = menu_pin.clone();
        let ui = flyout_ui.clone();
        let tx_cmd = tx_cmd.clone();
        flyout.on_item_clicked(move |i: i32| {
            let (mode, items) = ui
                .lock()
                .map(|u| (u.mode, u.items.clone()))
                .unwrap_or((SubMode::None, Vec::new()));
            match mode {
                SubMode::Mic => {
                    let name = items.get(i as usize).cloned().unwrap_or_default();
                    let value = if i == 0 {
                        String::new()
                    } else {
                        name.clone()
                    };
                    if tx_cmd
                        .try_send(OrchestratorCommand::SelectMicrophone(value))
                        .is_err()
                    {
                        log_line("menu: mic select FAILED (command loop dead?)");
                    } else {
                        log_line(&format!("menu: microphone -> {name:?}"));
                    }
                }
                SubMode::Model => {
                    if i >= 0 {
                        if tx_cmd
                            .try_send(OrchestratorCommand::SelectModel(i as usize))
                            .is_err()
                        {
                            log_line("menu: model select FAILED (command loop dead?)");
                        } else {
                            log_line(&format!("menu: model -> row {i}"));
                        }
                    }
                }
                SubMode::None => {}
            }
            *mlock(&pin) = None;
            if let Ok(mut u) = ui.lock() {
                u.mode = SubMode::None;
                u.rect = None;
            }
            hide_pill_menus(&pm, &fly);
            if let Some(p) = pw.upgrade() {
                let (mg0, uo0) = (p.get_mic_grow(), p.get_upper_op());
                *mlock(&anim) = Some(FootAnim {
                    t0: std::time::Instant::now(),
                    dur_ms: 200,
                    expanding: false,
                    mg0,
                    uo0,
                });
            }
        });
    }
    // Pill drag-end: magnetic snap to the CLOSEST station (always — the pill
    // is stationary). A quick left-tap intentionally opens NOTHING (menus
    // belong to right-click only); a mic tap never did anything else.
    {
        let pill_w = pill.as_weak();
        let overlay_w = overlay.as_weak();
        let drag = drag.clone();
        let corner_idx = corner_idx.clone();
        let vertical_de = vertical.clone();
        let expanded_de = expanded.clone();
        let state_de = state.clone();
        pill.on_drag_end(move || {
            let mut d = mlock(&drag);
            if !d.active || d.target != DragTarget::Pill {
                return;
            }
            // Closest station wins even if press never moved (tap = stay).
            let (ccx, ccy) = widget_platform::cursor_position();
            let moved = (ccx - d.grab_cursor.0)
                .abs()
                .max((ccy - d.grab_cursor.1).abs());
            d.mic_used = false;
            d.active = false;
            // Magnetic snap target: live snap if dragging, else closest dock
            // to the current window center (covers taps + CYCLE-less drops).
            let snap: usize = if let Some(i) = d.snap_idx.take() {
                i
            } else if let Some(p) = pill_w.upgrade() {
                let pos = p.window().position();
                let scale = widget_platform::window_scale(p.window()).max(0.5);
                let (cw, ch) = cur_size(&expanded_de, &vertical_de);
                let pw = (cw as f32 * scale).round() as i32;
                let ph = (ch as f32 * scale).round() as i32;
                let mut best: usize = *mlock(&corner_idx);
                let mut best_d = i32::MAX;
                let de_gap = dock_gap(&expanded_de);
                for (i, (dx, dy)) in [
                    widget_platform::dock_position(0, cw, ch, de_gap, scale),
                    widget_platform::dock_position(1, cw, ch, de_gap, scale),
                    widget_platform::dock_position(2, cw, ch, de_gap, scale),
                ]
                .iter()
                .enumerate()
                {
                    let dd = (pos.x + pw / 2 - (dx + pw / 2))
                        .abs()
                        .max((pos.y + ph / 2 - (dy + ph / 2)).abs());
                    if dd < best_d {
                        best_d = dd;
                        best = i;
                    }
                }
                best
            } else {
                *mlock(&corner_idx)
            };
            log_line(&format!("ui: drag end snap={snap} moved={moved}"));
            drop(d);
            if let Some(o) = overlay_w.upgrade() {
                if o.window().is_visible() {
                    let _ = o.hide();
                    if let Some(p) = pill_w.upgrade() {
                        log_line(&format!(
                            "overlay hidden (drop) pillrect={:?}",
                            widget_platform::native_rect(p.window())
                        ));
                    } else {
                        log_line("overlay hidden (drop)");
                    }
                }
            }
            if let Some(p) = pill_w.upgrade() {
                let vert = snap != 0;
                *mlock(&vertical_de) = vert;
                p.set_vertical(vert);
                p.set_mirror(snap == 2);
                let (cw, ch) = cur_size(&expanded_de, &vertical_de);
                let scale = widget_platform::window_scale(p.window()).max(0.5);
                let (x, y) = widget_platform::dock_position(snap, cw, ch, dock_gap(&expanded_de), scale);
                place_pill(&p, x, y);
                set_footprint(&p, cw, ch);
                *mlock(&corner_idx) = snap;
                mlock(&drag).docked_exact = true;
                // Persist the station immediately: updates and restarts
                // must keep the pill where the user docked it.
                if let Ok(mut s) = state_de.lock() {
                    if s.settings.pill_dock != snap as u32 {
                        s.settings.pill_dock = snap as u32;
                        if let Err(e) = s.settings.save_all() {
                            log_line(&format!("dock persist failed: {e}"));
                        } else {
                            log_line(&format!("dock persisted: station={snap}"));
                        }
                    }
                }
            }
        });
    }
    {
        let tm = traymenu.as_weak();
        let dash_menu = dashboard.as_weak();
        let pill_menu_w = pill.as_weak();
        let dash_visible_menu = dash_visible.clone();
        let state_menu = state.clone();
        traymenu.on_row_clicked(move |i| {
            if let Some(t) = tm.upgrade() {
                let _ = t.hide();
            }
            menu_action(i, &dash_menu, &pill_menu_w, &dash_visible_menu, &state_menu);
        });
    }

    // ── 50ms poll: state -> Slint props (waveform smoothing here) ──
    let mut smooth = vec![0.0f32; 24];
    let mut tick: u32 = 0;
    let mut prev_transcribing = false;
    let mut was_live = false;
    let mut done_until = std::time::Instant::now();
    let mut last_hover = std::time::Instant::now();
    // Overlay paint insurance throttle (see watchdog block below).
    let mut last_ov_insure: Option<std::time::Instant> = None;
    // Last dock position WE requested (taskbar-watch dedup, see below).
    let mut last_dock_req: Option<(i32, i32)> = None;
    // Last X11 chrome re-strip (Linux keeps ABOVE/state pinned).
    #[cfg(target_os = "linux")]
    let mut last_xstrip = std::time::Instant::now()
        .checked_sub(std::time::Duration::from_secs(10))
        .unwrap_or_else(std::time::Instant::now);
    // Last own-XID census (Linux focus-capture filter, see below).
    #[cfg(target_os = "linux")]
    let mut last_ownxids = std::time::Instant::now()
        .checked_sub(std::time::Duration::from_secs(10))
        .unwrap_or_else(std::time::Instant::now);
    let mut last_upper_w = 184i32;
    let mut last_phase = -1i32;
    let mut logged_phase = -1i32;
    let mut logged_msg = String::new();
    let mut sess_start_len = 0usize;
    let mut sess_peak = 0u32;
    // Peak EXCLUDING the first 800ms (trigger onset): the clap that starts
    // a session peaks near 100 and whitewashes sess_peak, so the ghost gate
    // below would pass every hallucination. A deliberately spoken word
    // sustains level past 800ms; a trigger transient never does.
    let mut speech_peak = 0u32;
    let mut sess_start_t = std::time::Instant::now();
    let mut last_buf_len = 0usize;
    // Bulk end-delivery (C++-style): start offset of the current turn.
    let mut turn_base_len = 0usize;
    let mut tb_until = std::time::Instant::now();
    let mut transcribing_since: Option<std::time::Instant> = None;
    let mut alert_until = std::time::Instant::now();
    let mut alert_msg = String::new();
    let mut logged_poison = false;
    let mut last_dbg = false;
    let mut last_dbg_p = false;
    let mut frame_log_at: Option<std::time::Instant> = None;
    let mut dbl_pending = 0u32;
    // Idle model offload bookkeeping (dashboard autoOffload/offloadSeconds).
    let mut last_turn_end: Option<std::time::Instant> = None;
    let mut offload_sent = false;
    let timer = Timer::default();
    let pill_w = pill.as_weak();
    let dash_w = dashboard.as_weak();
    let overlay_w = overlay.as_weak();
    let traymenu_w = traymenu.as_weak();
    let textboard_w = textboard.as_weak();
    let pillmenu_w = pillmenu.as_weak();
    let flyout_w = flyout.as_weak();
    let tb_closed_timer = tb_closed.clone();
    let tb_follow_timer = tb_follow.clone();
    let tb_full_timer = tb_full.clone();
    let last_turn_timer = last_turn.clone();
    let hide_until_timer = hide_until.clone();
    let menu_pin_timer = menu_pin.clone();
    let flyout_ui_timer = flyout_ui.clone();
    let menu2_open_at_timer = menu2_open_at.clone();
    let mut menu2_outside: Option<std::time::Instant> = None;
    let mut prev_snoozed = false;
    #[allow(unused_variables)]
    let show_traymenu_poll = show_traymenu.clone();
    // Regression opener for the dark pill menu (left-tap no longer opens
    // it — menus are right-click only — so tests open it via IPC).
    let show_pillmenu_timer = show_pillmenu.clone();
    let mut tray_last_up: Option<std::time::Instant> = None;
    let mut tray_pending_single: Option<std::time::Instant> = None;
    let mut tray_dbl_at: Option<std::time::Instant> = None;
    let mut traymenu_outside: Option<std::time::Instant> = None;
    let menu_open_at_timer = menu_open_at.clone();
    let dash_visible_close = dash_visible.clone();
    let dash_visible_timer = dash_visible.clone();
    let dash_timer = dashboard.as_weak();
    let state_timer = state.clone();
    let expanded_timer = expanded.clone();
    let vertical_timer = vertical.clone();
    let drag_timer = drag.clone();
    let pending_hop_timer = pending_hop.clone();
    let corner_idx_timer = corner_idx.clone();
    let tx_cmd_timer = tx_cmd.clone();
    let ptt_held_timer = ptt_held.clone();
    let mic_live_timer = mic_live.clone();
    let foot_anim_timer = foot_anim.clone();
    let overlay_shown_timer = overlay_shown_at.clone();
    let layout_overlay_timer = layout_overlay.clone();
    let suppress_menu_timer = suppress_menu_until.clone();
    let diag_rx_timer = diag_rx;
    let last_external_fg_timer = last_external_fg.clone();
    let wakeword_handle_timer = wakeword_handle.clone();
    // 60fps morph ticker: drives FootAnim content props only (the window
    // never resizes mid-morph — native resizes flash white frames). The 50ms
    // poll only STARTS glides; this consumes them. Slower + smoother by
    // request: 340ms expand / 200ms collapse, capsule first (ease-out to
    // 50%), dictate floats up late (smootherstep 58-100% + 14px drift + 6px
    // rise). Collapse is the exact mirror. Idles on one lock check.
    let anim_ticker = Timer::default();
    {
        let pill_a = pill.as_weak();
        let foot_a = foot_anim.clone();
        let expanded_a = expanded.clone();
        let corner_a = corner_idx.clone();
        let vertical_a = vertical.clone();
        anim_ticker.start(
            TimerMode::Repeated,
            std::time::Duration::from_millis(16),
            move || {
                let an = mlock(&foot_a).take();
                let Some(an) = an else { return };
                let Some(p) = pill_a.upgrade() else { return };
                let now = std::time::Instant::now();
                let t = now
                    .duration_since(an.t0)
                    .as_millis()
                    .min(an.dur_ms as u128) as f32
                    / an.dur_ms as f32;
                // ease-in-out cubic (window-glide feel, content-only)
                let e = if t < 0.5 {
                    4.0 * t * t * t
                } else {
                    1.0 - (-2.0 * t + 2.0).powi(3) / 2.0
                };
                if an.expanding {
                    let mg = an.mg0
                        + (1.0 - an.mg0) * ease_out_cubic((e / 0.50).clamp(0.0, 1.0));
                    let uo = an.uo0
                        + (1.0 - an.uo0) * smootherstep(((e - 0.58) / 0.42).clamp(0.0, 1.0));
                    p.set_mic_grow(mg);
                    p.set_upper_op(uo);
                    p.set_slide_px(((1.0 - uo) * -14.0).round() as i32);
                    p.set_rise_px(((1.0 - uo) * 6.0).round() as i32);
                } else {
                    let uo = an.uo0 * (1.0 - smootherstep((e / 0.42).clamp(0.0, 1.0)));
                    let mg = an.mg0 * (1.0 - ease_out_cubic(((e - 0.50) / 0.50).clamp(0.0, 1.0)));
                    p.set_mic_grow(mg);
                    p.set_upper_op(uo);
                    p.set_slide_px(((1.0 - uo) * -14.0).round() as i32);
                    p.set_rise_px(((1.0 - uo) * 6.0).round() as i32);
                }
                if t >= 1.0 {
                    if !an.expanding {
                        p.set_expanded(false);
                        *mlock(&expanded_a) = false;
                        p.set_mic_grow(0.0);
                        p.set_upper_op(0.0);
                        p.set_slide_px(0);
                        p.set_rise_px(0);
                        // Window follows back to the mini footprint now that
                        // the content IS the mini bar (invisible jump).
                        {
                            let idx = *mlock(&corner_a);
                            let vert = *mlock(&vertical_a);
                            let (mw2, mh2) = if vert {
                                (SIDE_MINI_W, SIDE_MINI_H)
                            } else {
                                (MINI_W, MINI_H)
                            };
                            let sc = widget_platform::window_scale(p.window()).max(0.5);
                            let (jx, jy) =
                                widget_platform::dock_position(idx, mw2, mh2, EDGE_GAP, sc);
                            place_pill(&p, jx, jy);
                            set_footprint(&p, mw2, mh2);
                        }
                        log_line("ui: collapse landed");
                    } else {
                        p.set_mic_grow(1.0);
                        p.set_upper_op(1.0);
                        p.set_slide_px(0);
                        p.set_rise_px(0);
                        log_line("ui: expand landed");
                    }
                } else {
                    *mlock(&foot_a) = Some(an);
                }
            },
        );
    }
    #[cfg(target_os = "linux")]
    let sni_rx_poll = sni_rx.clone();
    timer.start(
        TimerMode::Repeated,
        std::time::Duration::from_millis(50),
        move || {            // Backstop: one panicking tick (bad lock, platform error, OOB)
            // must never take down the event loop — catch it, log it, keep
            // going. Poisoned mutexes additionally self-heal via mlock, so
            // the NEXT tick runs on healthy state instead of re-panicking.
            let tick_ok = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            tick = tick.wrapping_add(1);
            let (Some(p), Some(d)) = (pill_w.upgrade(), dash_w.upgrade()) else {
                return;
            };

            // Frameless re-assert: beats any backend race that restores the
            // OS caption. No-op most ticks (single GetWindowLong compare).
            // Reframe + missing-HWND events are LOGGED (shared 5s cooldown):
            // a framed pill or invisible-but-shown overlay must leave a
            // timestamped trace.
            let mut note = |msg: &str| {
                let tnow = std::time::Instant::now();
                if frame_log_at
                    .map(|t| tnow.duration_since(t).as_secs() >= 5)
                    .unwrap_or(true)
                {
                    frame_log_at = Some(tnow);
                    log_line(msg);
                }
            };
            if widget_platform::ensure_frameless(p.window(), false) {
                let st = widget_platform::window_state_debug("pill", p.window());
                note(&format!("pill frame stripped {st}"));
            }
            // X11 chrome re-assert (~2s while visible): WMs can wipe our
            // ABOVE/state atoms on map or workspace switch — re-strip keeps
            // the pill frameless and above the taskbar. ensure_widget_top
            // adds the true-widget layer (override-redirect once + raise
            // heartbeat, so fullscreen apps can't bury the pill either).
            // Idempotent and cheap (a few local round-trips); the strip
            // itself logs.
            #[cfg(target_os = "linux")]
            if p.window().is_visible()
                && last_xstrip.elapsed() > std::time::Duration::from_secs(2)
            {
                last_xstrip = std::time::Instant::now();
                widget_platform::strip_chrome(p.window());
                widget_platform::ensure_widget_top(p.window());
            }
            // Own-XID census (~5s): learn our pill/menu/dashboard XIDs so
            // focus capture never mistakes us for the user's textbox (the
            // paste would land back in our own window).
            #[cfg(target_os = "linux")]
            if last_ownxids.elapsed() > std::time::Duration::from_secs(5) {
                last_ownxids = std::time::Instant::now();
                note_own_xid(widget_platform::xid_of(p.window()));
                note_own_xid(widget_platform::xid_of(d.window()));
                if let Some(o) = overlay_w.upgrade() {
                    note_own_xid(widget_platform::xid_of(o.window()));
                }
                if let Some(tb) = textboard_w.upgrade() {
                    note_own_xid(widget_platform::xid_of(tb.window()));
                }
                if let Some(m) = pillmenu_w.upgrade() {
                    note_own_xid(widget_platform::xid_of(m.window()));
                }
                if let Some(f) = flyout_w.upgrade() {
                    note_own_xid(widget_platform::xid_of(f.window()));
                }
            }
            // Windows-only signal: on X11 there is no HWND by design (the
            // strip path uses the XID instead), so don't cry wolf there.
            #[cfg(target_os = "windows")]
            if p.window().is_visible() && !widget_platform::has_hwnd(p.window()) {
                note("pill visible without HWND (strip skipped)");
            }
            // Fresh-paint barrage next to every strip: a newly shown window
            // whose first paint request was dropped (e.g. by the post-show
            // resize) sits blank — invisible if unpainted-transparent, a
            // white box if unpainted-opaque. Re-requesting while visible
            // closes that hole within a tick. Small windows: every tick
            // (cheap); the fullscreen overlay has its own 150ms throttle.
            if d.window().is_visible() {
                d.window().request_redraw();
            }
            if let Some(o) = overlay_w.upgrade() {
                if o.window().is_visible() {
                    widget_platform::ensure_frameless(o.window(), true);
                    if !widget_platform::has_hwnd(o.window()) {
                        note("overlay visible without HWND");
                    }
                }
            }
            if let Some(tb) = textboard_w.upgrade() {
                if tb.window().is_visible() {
                    if widget_platform::ensure_frameless(tb.window(), false) {
                        let st = widget_platform::window_state_debug("textboard", tb.window());
                        note(&format!("textboard frame stripped {st}"));
                    }
                    tb.window().request_redraw();
                }
            }
            if let Some(m) = pillmenu_w.upgrade() {
                if m.window().is_visible() {
                    if widget_platform::ensure_frameless(m.window(), false) {
                        let st = widget_platform::window_state_debug("pillmenu", m.window());
                        note(&format!("pillmenu frame stripped {st}"));
                    }
                    m.window().request_redraw();
                }
            }
            if let Some(f) = flyout_w.upgrade() {
                if f.window().is_visible() {
                    if widget_platform::ensure_frameless(f.window(), false) {
                        let st = widget_platform::window_state_debug("flyout", f.window());
                        note(&format!("flyout frame stripped {st}"));
                    }
                    f.window().request_redraw();
                }
            }
            // Main-thread synthetic input (bypasses winit): proves Slint
            // core + callbacks independent of OS delivery. Full gesture
            // rig: CLICKTEST/RCLICK/DBLCLICK + PRESS/MOVE/RELEASE x y
            // (logical px). Kept permanently for regression testing.
            // Synthetic clicks land at the window center (collapsed bar and
            // expanded card differ per orientation; a fixed point would
            // miss the narrow side bar entirely).
            let click_pos = {
                let e = *mlock(&expanded_timer);
                let v = *mlock(&vertical_timer);
                let (w, h) = if e {
                    if v {
                        (SIDE_EXP_W, SIDE_EXP_H)
                    } else {
                        (EXP_W, EXP_H)
                    }
                } else if v {
                    (SIDE_MINI_W, SIDE_MINI_H)
                } else {
                    (MINI_W, MINI_H)
                };
                slint::LogicalPosition::new(w as f32 / 2.0, h as f32 / 2.0)
            };
            while let Ok(cmd) = diag_rx_timer.try_recv() {
                use slint::platform::{PointerEventButton, WindowEvent};
                fn xy(s: &str) -> (f32, f32) {
                    let mut it = s
                        .split_whitespace()
                        .skip(1)
                        .filter_map(|v| v.parse::<f32>().ok());
                    (it.next().unwrap_or(36.0), it.next().unwrap_or(6.0))
                }
                if cmd == "CLICKTEST" {
                    let pos = click_pos;
                    p.window().dispatch_event(WindowEvent::PointerPressed {
                        position: pos,
                        button: PointerEventButton::Left,
                    });
                    p.window().dispatch_event(WindowEvent::PointerReleased {
                        position: pos,
                        button: PointerEventButton::Left,
                    });
                    log_line("clicktest dispatched (main)");
                } else if cmd == "WINLIST" {
                    // Regression hook: dump every visible non-empty top-level
                    // window (title, rect, owner PID, style bits) so a phantom
                    // tile can be identified by process, not guessed at.
                    widget_platform::log_winlist();
                } else if cmd == "OVERLAY" {
                    // Regression hook: force the drag preview visible so it
                    // can be screenshotted/verified without a physical drag.
                    // The watchdog hides it again ~1.5s later (which also
                    // proves the watchdog path).
                    if let Some(o) = overlay_w.upgrade() {
                        widget_platform::show_overlay(o.window());
                        let scale =
                            widget_platform::window_scale(p.window()).max(0.5);
                        let (sw, sh) = widget_platform::screen_size();
                        let (lw, lh) = (
                            (sw as f32 / scale).round(),
                            (sh as f32 / scale).round(),
                        );
                        o.set_ov_w(lw);
                        o.set_ov_h(lh);
                        // Full-damage nudge FIRST (see dock.slint scrim).
                        o.set_refresh_dark(!o.get_refresh_dark());
                        // Native size is authoritative for the HWND (see drag
                        // path above).
                        o.window().set_size(slint::LogicalSize {
                            width: lw,
                            height: lh,
                        });
                        o.window()
                            .set_position(slint::PhysicalPosition { x: 0, y: 0 });
                        // Same post-size strip + surviving paint request as
                        // the drag path (see above).
                        widget_platform::ensure_frameless(o.window(), true);
                        o.window().request_redraw();
                        layout_overlay_timer();
                        // Ghost geometry readback for the log record.
                        log_line(&format!(
                            "overlay ghosts g0={},{},{}x{} g1={},{},{}x{} g2={},{},{}x{}",
                            o.get_dock0_x(),
                            o.get_dock0_y(),
                            o.get_dock0_w(),
                            o.get_dock0_h(),
                            o.get_dock1_x(),
                            o.get_dock1_y(),
                            o.get_dock1_w(),
                            o.get_dock1_h(),
                            o.get_dock2_x(),
                            o.get_dock2_y(),
                            o.get_dock2_w(),
                            o.get_dock2_h()
                        ));
                        *mlock(&overlay_shown_timer) = Some(std::time::Instant::now());
                        let sz = o.window().size();
                        let ps = o.window().position();
                        log_line(&format!(
                            "overlay test show (visible={} slint={}x{} scr={}x{} scale={:.2} pos={},{} active={})",
                            o.window().is_visible(),
                            sz.width,
                            sz.height,
                            sw,
                            sh,
                            scale,
                            ps.x,
                            ps.y,
                            o.get_active_dock()
                        ));
                        // TEMP-DIAG: overlay props readback removed with the
                        // refresh_dark experiment.
                        // Win32 ground truth (independent of Slint): is there
                        // really a fullscreen layered topmost "QuickSTT Dock"?
                        #[cfg(target_os = "windows")]
                        {
                            use windows::Win32::Foundation::{BOOL, HWND, LPARAM, RECT};
                            use windows::Win32::UI::WindowsAndMessaging::{
                                EnumWindows, GetWindowLongW, GetWindowRect, GetWindowTextW,
                                IsWindowVisible, GWL_EXSTYLE, GWL_STYLE,
                            };
                            struct T {
                                n: i32,
                            }
                            unsafe extern "system" fn visit(h: HWND, lp: LPARAM) -> BOOL {
                                let mut buf = [0u16; 64];
                                let len = GetWindowTextW(h, &mut buf) as usize;
                                if String::from_utf16_lossy(&buf[..len.min(64)])
                                    == "QuickSTT Dock"
                                {
                                    let t = &mut *(lp.0 as *mut T);
                                    t.n += 1;
                                    let mut rc = RECT::default();
                                    let _ = GetWindowRect(h, &mut rc);
                                    let st = GetWindowLongW(h, GWL_STYLE);
                                    let ex = GetWindowLongW(h, GWL_EXSTYLE);
                                    let vis = IsWindowVisible(h).as_bool();
                                    crate::log_line(&format!(
                                        "overlay HWND truth: vis={vis} rect={},{},{},{} style={st:#x} ex={ex:#x}",
                                        rc.left, rc.top, rc.right, rc.bottom
                                    ));
                                }
                                true.into()
                            }
                            let mut t = T { n: 0 };
                            unsafe {
                                let _ =
                                    EnumWindows(Some(visit), LPARAM(&mut t as *mut T as isize));
                            }
                            log_line(&format!("overlay HWND count={}", t.n));
                        }
                        // Same field evidence as a real drag (screenshot +
                        // census) so the test path verifies identically.
                        spawn_dragcap("test");
                    }
                } else if cmd == "RCLICK" {
                    let pos = click_pos;
                    p.window().dispatch_event(WindowEvent::PointerPressed {
                        position: pos,
                        button: PointerEventButton::Right,
                    });
                    p.window().dispatch_event(WindowEvent::PointerReleased {
                        position: pos,
                        button: PointerEventButton::Right,
                    });
                    log_line("rclicktest dispatched (main)");
                } else if cmd == "DBLCLICK" {
                    let pos = click_pos;
                    p.window().dispatch_event(WindowEvent::PointerPressed {
                        position: pos,
                        button: PointerEventButton::Left,
                    });
                    p.window().dispatch_event(WindowEvent::PointerReleased {
                        position: pos,
                        button: PointerEventButton::Left,
                    });
                    // Second pair after ~200ms so Slint counts a double even
                    // when ticks run slow under load (6 ticks risked
                    // exceeding the OS double-click window).
                    dbl_pending = 4;
                    log_line("dblclick first pair (main)");
                } else if cmd.starts_with("PRESS") {
                    let (x, y) = xy(&cmd);
                    p.window().dispatch_event(WindowEvent::PointerPressed {
                        position: slint::LogicalPosition::new(x, y),
                        button: PointerEventButton::Left,
                    });
                    log_line(&format!("presstest ({x},{y})"));
                } else if cmd.starts_with("MOVE") {
                    let (x, y) = xy(&cmd);
                    p.window().dispatch_event(WindowEvent::PointerMoved {
                        position: slint::LogicalPosition::new(x, y),
                    });
                    log_line(&format!("movetest ({x},{y})"));
                } else if cmd.starts_with("RELEASE") {
                    let (x, y) = xy(&cmd);
                    p.window().dispatch_event(WindowEvent::PointerReleased {
                        position: slint::LogicalPosition::new(x, y),
                        button: PointerEventButton::Left,
                    });
                    log_line(&format!("releasetest ({x},{y})"));
                } else if cmd == "SHOW" {
                    widget_platform::show_widget(p.window());
                    widget_platform::restack_topmost(p.window());
                } else if cmd == "HIDE" {
                    let _ = p.hide();
                    log_line("diag: pill hidden via ipc (event loop must survive this)");
                } else if cmd == "TOGGLE" {
                    toggle_pill_widget(&pill_w, &state_timer, "ipc");
                } else if cmd == "DASH" {
                    if let Some(d) = dash_timer.upgrade() {
                        widget_platform::show_app_window(d.window());
                        if let Ok(mut v) = dash_visible_timer.lock() {
                            *v = true;
                        }
                    }
                } else if cmd.starts_with("MENUROW") {
                    let n: i32 = cmd
                        .split_whitespace()
                        .nth(1)
                        .and_then(|v| v.parse().ok())
                        .unwrap_or(-1);
                    log_line(&format!("menutest row {n}"));
                    menu_action(n, &dash_timer, &pill_w, &dash_visible_timer, &state_timer);
                } else if cmd == "CYCLE" {
                    // Deterministic dock hop (bypasses Slint double-click
                    // detection, which needs OS-timed press pairs).
                    dock_hop(
                        &pill_w,
                        &corner_idx_timer,
                        &expanded_timer,
                        &vertical_timer,
                        &drag_timer,
                        &suppress_menu_timer,
                        true,
                    );
                    log_line("cycle dispatched (main)");
                } else if cmd == "MENU2OPEN" {
                    show_pillmenu_timer(None);
                    log_line("menu2opentest (main)");
                } else if cmd.starts_with("MENU2ROW") {
                    // Dark-menu regression tap: synthetic left-click on the
                    // row (Hide 0, Settings 1, Mic 2, Model 3, History 4,
                    // Paste 5) — exercises the real Slint path incl. flyouts.
                    let n: i32 = cmd
                        .split_whitespace()
                        .nth(1)
                        .and_then(|v| v.parse().ok())
                        .unwrap_or(-1);
                    let y = match n {
                        0 => 23.0,
                        1 => 57.0,
                        2 => 100.0,
                        3 => 134.0,
                        4 => 177.0,
                        5 => 211.0,
                        _ => -1.0,
                    };
                    if y > 0.0 {
                        if let Some(m) = pillmenu_w.upgrade() {
                            use slint::platform::{PointerEventButton, WindowEvent};
                            let pos = slint::LogicalPosition::new(124.0, y);
                            m.window().dispatch_event(WindowEvent::PointerPressed {
                                position: pos,
                                button: PointerEventButton::Left,
                            });
                            m.window().dispatch_event(WindowEvent::PointerReleased {
                                position: pos,
                                button: PointerEventButton::Left,
                            });
                        }
                        log_line(&format!("menu2 clicktest row {n}"));
                    }
                } else if cmd.starts_with("MENU2MIC") {
                    // Direct mic-select path (flyout hover needs a cursor).
                    let i: usize = cmd
                        .split_whitespace()
                        .nth(1)
                        .and_then(|v| v.parse().ok())
                        .unwrap_or(usize::MAX);
                    let (names, _) = mic_menu_items(&state_timer);
                    if let Some(name) = names.get(i) {
                        let v = if i == 0 {
                            String::new()
                        } else {
                            name.clone()
                        };
                        let _ = tx_cmd_timer
                            .try_send(OrchestratorCommand::SelectMicrophone(v));
                        log_line(&format!("menu2 mic select {i} {name:?}"));
                    }
                } else if cmd.starts_with("MENU2MODEL") {
                    let i: usize = cmd
                        .split_whitespace()
                        .nth(1)
                        .and_then(|v| v.parse().ok())
                        .unwrap_or(usize::MAX);
                    let _ = tx_cmd_timer.try_send(OrchestratorCommand::SelectModel(i));
                    log_line(&format!("menu2 model select {i}"));
                } else if let Some(text) = cmd.strip_prefix("SAY ") {
                    // Test hook: inject transcript text as if the engine just
                    // recognized it (drives the TextBoard E2E check).
                    let text = text.to_string();
                    if let Ok(mut st) = state_timer.lock() {
                        if !st.transcript_buffer.is_empty() {
                            st.transcript_buffer.push('\n');
                        }
                        st.transcript_buffer.push_str(&text);
                    }
                    tb_until = std::time::Instant::now() + std::time::Duration::from_secs(8);
                    log_line(&format!("saytest +{} chars", text.len()));
                }
            }
            if dbl_pending > 0 {
                dbl_pending -= 1;
                if dbl_pending == 0 {
                    use slint::platform::{PointerEventButton, WindowEvent};
                    let pos = click_pos;
                    p.window().dispatch_event(WindowEvent::PointerPressed {
                        position: pos,
                        button: PointerEventButton::Left,
                    });
                    p.window().dispatch_event(WindowEvent::PointerReleased {
                        position: pos,
                        button: PointerEventButton::Left,
                    });
                    log_line("dblclick second pair (main)");
                }
            }
            let dbg_h = p.get_dbg_hover();
            if dbg_h != last_dbg {
                last_dbg = dbg_h;
                log_line(&format!("dbg hover={dbg_h}"));
            }
            let dbg_p = p.get_dbg_pressed();
            if dbg_p != last_dbg_p {
                last_dbg_p = dbg_p;
                log_line(&format!("dbg pressed={dbg_p}"));
            }
            // A panic while holding one of these would silently kill every
            // pill gesture (callbacks unwrap). Report once if it happens.
            if !logged_poison {
                logged_poison = true;
                let mut bad = Vec::new();
                if drag_timer.is_poisoned() {
                    bad.push("drag");
                }
                if expanded_timer.is_poisoned() {
                    bad.push("expanded");
                }
                if state_timer.is_poisoned() {
                    bad.push("state");
                }
                log_line(&format!(
                    "mutex poison check: {}",
                    if bad.is_empty() {
                        "clean".to_string()
                    } else {
                        format!("POISONED: {}", bad.join(","))
                    }
                ));
            }

            // Tray clicks own the main C++ Qt widget: single-click toggles it
            // (deferred 350ms so a double-click doesn't toggle twice),
            // double-click toggles it immediately. Right-click is owned by
            // the NATIVE muda tray menu (attached at build) — the OS shows
            // and dismisses it, so we only take Slint menus down here and
            // never open our own (no double menus, no focus fights).
            while let Ok(ev) = tray_icon::TrayIconEvent::receiver().try_recv() {
                log_line(&format!("tray ev: {ev:?}"));
                match ev {
                    tray_icon::TrayIconEvent::Click {
                        button,
                        button_state,
                        position,
                        ..
                    } => {
                        let _at = Some((position.x as i32, position.y as i32));
                        match (button, button_state) {
                            (tray_icon::MouseButton::Right, tray_icon::MouseButtonState::Up) => {
                                let at = (position.x as i32, position.y as i32);
                                log_line(&format!("tray right-click @ {:?} -> show native dark menu", at));
                                #[cfg(target_os = "windows")]
                                {
                                    let hwnd_raw = widget_platform::hwnd_of(p.window()).unwrap_or(0);
                                    let hwnd = windows::Win32::Foundation::HWND(hwnd_raw as _);
                                    if let Some(cmd) = show_native_tray_menu(hwnd, at.0, at.1) {
                                        match cmd {
                                            1 => {
                                                log_line("tray native menu: Dashboard");
                                                menu_action(0, &dash_timer, &pill_w, &dash_visible_timer, &state_timer);
                                            }
                                            2 => {
                                                log_line("tray native menu: Show/Hide Widget");
                                                toggle_pill_widget(&pill_w, &state_timer, "tray-native-menu");
                                            }
                                            3 => {
                                                log_line("tray native menu: Quit QuickSTT");
                                                quit_app();
                                                std::process::exit(0);
                                            }
                                            _ => {}
                                        }
                                    }
                                }
                                #[cfg(not(target_os = "windows"))]
                                show_traymenu_poll(Some(at));
                            }
                            (
                                tray_icon::MouseButton::Left,
                                tray_icon::MouseButtonState::Up,
                            ) => {
                                // A DoubleClick just toggled: swallow the
                                // trailing Up so it doesn't toggle back.
                                if tray_dbl_at
                                    .map(|t| {
                                        t.elapsed()
                                            < std::time::Duration::from_millis(600)
                                    })
                                    .unwrap_or(false)
                                {
                                    continue;
                                }
                                let now = std::time::Instant::now();
                                if tray_last_up
                                    .map(|t| {
                                        now.duration_since(t)
                                            < std::time::Duration::from_millis(350)
                                    })
                                    .unwrap_or(false)
                                {
                                    tray_last_up = None;
                                    tray_pending_single = None;
                                    if let Some(t) = traymenu_w.upgrade() {
                                        let _ = t.hide();
                                    }
                                    toggle_pill_widget(
                                        &pill_w,
                                        &state_timer,
                                        "double",
                                    );
                                } else {
                                    tray_last_up = Some(now);
                                    tray_pending_single = Some(now);
                                }
                            }
                            _ => {}
                        }
                    }
                    tray_icon::TrayIconEvent::DoubleClick {
                        button, position, ..
                    } => {
                        let _at = Some((position.x as i32, position.y as i32));
                        match button {
                            tray_icon::MouseButton::Left => {
                                tray_last_up = None;
                                tray_pending_single = None;
                                tray_dbl_at = Some(std::time::Instant::now());
                                if let Some(t) = traymenu_w.upgrade() {
                                    let _ = t.hide();
                                }
                                toggle_pill_widget(
                                    &pill_w,
                                    &state_timer,
                                    "double",
                                );
                            }
                            tray_icon::MouseButton::Right => {
                                if let Some(t) = traymenu_w.upgrade() {
                                    let _ = t.hide();
                                }
                            }
                            _ => {}
                        }
                    }
                    _ => {}
                }
            }
            // Native tray menu selections (muda): Dashboard / Show / Hide /
            // Quit — routed through the same shared menu_action as before.
            while let Ok(ev) = muda::MenuEvent::receiver().try_recv() {
                let row = if ev.id == muda::MenuId::new(TRAY_ID_DASH) {
                    Some(0)
                } else if ev.id == muda::MenuId::new(TRAY_ID_SHOW) {
                    Some(1)
                } else if ev.id == muda::MenuId::new(TRAY_ID_HIDE) {
                    Some(2)
                } else if ev.id == muda::MenuId::new(TRAY_ID_QUIT) {
                    Some(3)
                } else {
                    None
                };
                if let Some(i) = row {
                    log_line(&format!("tray native menu row {i}"));
                    menu_action(i, &dash_timer, &pill_w, &dash_visible_timer, &state_timer);
                }
            }
            // Deferred single left-click (fires only when no second click
            // follows within 350ms): toggles the Slint pill widget.
            if let Some(t) = tray_pending_single {
                if t.elapsed() > std::time::Duration::from_millis(350) {
                    tray_pending_single = None;
                    toggle_pill_widget(&pill_w, &state_timer, "left");
                }
            }
            // SNI tray clicks (Linux): same actions as the native menu rows.
            #[cfg(target_os = "linux")]
            if let Ok(rx) = sni_rx_poll.lock() {
                use sni_tray::SniClick::*;
                while let Ok(click) = rx.try_recv() {
                    match click {
                        Activate => toggle_pill_widget(&pill_w, &state_timer, "sni"),
                        Dashboard => menu_action(
                            0,
                            &dash_timer,
                            &pill_w,
                            &dash_visible_timer,
                            &state_timer,
                        ),
                        ToggleWidget => toggle_pill_widget(&pill_w, &state_timer, "sni-menu"),
                        Quit => {
                            quit_app();
                            std::process::exit(0);
                        }
                        Online => log_line("tray: SNI item registered on the bus"),
                    }
                }
            }
            // Menu upkeep: frameless re-assert + outside-dismiss (cursor off
            // the menu 500ms closes it, with an 800ms grace right after
            // opening so it can never flash-and-vanish).
            if let Some(t) = traymenu_w.upgrade() {
                if t.window().is_visible() {
                    widget_platform::ensure_frameless(t.window(), false);
                    // Same fresh-paint barrage as the poll re-asserts above.
                    t.window().request_redraw();
                    let pos = t.window().position();
                    let scale = widget_platform::window_scale(p.window()).max(0.5);
                    let ww = (220.0 * scale).round() as i32;
                    let hh = (144.0 * scale).round() as i32;
                    let (cx, cy) = widget_platform::cursor_position();
                    let inside = cx >= pos.x
                        && cx <= pos.x + ww
                        && cy >= pos.y
                        && cy <= pos.y + hh;
                    let fresh = menu_open_at_timer
                        .lock()
                        .unwrap()
                        .map(|at| at.elapsed() < std::time::Duration::from_millis(300))
                        .unwrap_or(false);

                    #[cfg(target_os = "windows")]
                    let clicked_outside = unsafe {
                        use windows::Win32::UI::Input::KeyboardAndMouse::{GetAsyncKeyState, VK_LBUTTON, VK_RBUTTON};
                        (GetAsyncKeyState(VK_LBUTTON.0 as i32) as u16 & 0x8000 != 0
                            || GetAsyncKeyState(VK_RBUTTON.0 as i32) as u16 & 0x8000 != 0)
                            && !inside
                            && !fresh
                    };
                    #[cfg(not(target_os = "windows"))]
                    let clicked_outside = false;

                    if inside {
                        traymenu_outside = None;
                    } else if clicked_outside
                        || (!fresh
                            && traymenu_outside
                                .map(|at: std::time::Instant| {
                                    at.elapsed() > std::time::Duration::from_millis(500)
                                })
                                .unwrap_or(false))
                    {
                        let _ = t.hide();
                        traymenu_outside = None;
                        log_line("tray menu dismissed");
                    } else if traymenu_outside.is_none() {
                        traymenu_outside = Some(std::time::Instant::now());
                    }
                } else {
                    traymenu_outside = None;
                }
            }
            // Dark pill-menu upkeep: fade-in, drag-hide, submenu hover
            // routing (Microphone / Model rows open the shared flyout; the
            // flyout stays while hovered or pinned by row click), and
            // outside-dismiss for the pair.
            {
                let menu_vis = pillmenu_w
                    .upgrade()
                    .map(|m| m.window().is_visible())
                    .unwrap_or(false);
                // A fresh pill drag owns the screen: take the menus down.
                let dragging_now = drag_timer
                    .lock()
                    .map(|dd| dd.active)
                    .unwrap_or(false);
                if dragging_now && menu_vis {
                    hide_pill_menus(&pillmenu_w, &flyout_w);
                    *mlock(&menu_pin_timer) = None;
                    if let Ok(mut u) = flyout_ui_timer.lock() {
                        u.mode = SubMode::None;
                        u.rect = None;
                    }
                }
                if let Some(m) = pillmenu_w.upgrade() {
                    if m.window().is_visible() {
                        if m.get_menu_op() < 1.0 {
                            m.set_menu_op(1.0);
                        }
                        if let Some(f) = flyout_w.upgrade() {
                            if f.window().is_visible() && f.get_fly_op() < 1.0 {
                                f.set_fly_op(1.0);
                            }
                        }
                        let scale = widget_platform::window_scale(p.window()).max(0.5);
                        let mpos = m.window().position();
                        let mw = (PM_W as f32 * scale).round() as i32;
                        let mh = (PM_H as f32 * scale).round() as i32;
                        let (cx, cy) = widget_platform::cursor_position();
                        let in_band = |y0: i32, y1: i32| {
                            cx >= mpos.x
                                && cx <= mpos.x + mw
                                && cy >= mpos.y + (y0 as f32 * scale).round() as i32
                                && cy <= mpos.y + (y1 as f32 * scale).round() as i32
                        };
                        let in_fly = flyout_ui_timer
                            .lock()
                            .map(|u| {
                                u.rect
                                    .map(|(fx, fy, fw, fh)| {
                                        cx >= fx && cx <= fx + fw && cy >= fy && cy <= fy + fh
                                    })
                                    .unwrap_or(false)
                            })
                            .unwrap_or(false);
                        let pinned = menu_pin_timer.lock().map(|v| *v).unwrap_or(None);
                        let active = flyout_ui_timer
                            .lock()
                            .map(|u| u.mode)
                            .unwrap_or(SubMode::None);
                        let fly_rect = flyout_ui_timer
                            .lock()
                            .map(|u| u.rect)
                            .unwrap_or(None);

                        let in_corridor = if let (SubMode::Mic | SubMode::Model, Some((fx, fy, fw, fh))) = (active, fly_rect) {
                            let (row_y0, row_y1) = if active == SubMode::Mic {
                                (PM_MIC_Y0, PM_MIC_Y1)
                            } else {
                                (PM_MOD_Y0, PM_MOD_Y1)
                            };
                            let ry0 = mpos.y + (row_y0 as f32 * scale).round() as i32;
                            let ry1 = mpos.y + (row_y1 as f32 * scale).round() as i32;
                            let min_y = ry0.min(fy) - 12;
                            let max_y = ry1.max(fy + fh) + 12;
                            let min_x = mpos.x.min(fx) - 12;
                            let max_x = (mpos.x + mw).max(fx + fw) + 12;
                            cx >= min_x && cx <= max_x && cy >= min_y && cy <= max_y
                        } else {
                            false
                        };

                        let want = if let Some(pm) = pinned {
                            Some(pm)
                        } else if in_band(PM_MIC_Y0, PM_MIC_Y1) {
                            Some(SubMode::Mic)
                        } else if in_band(PM_MOD_Y0, PM_MOD_Y1) {
                            Some(SubMode::Model)
                        } else if (in_fly || in_corridor) && active != SubMode::None {
                            Some(active)
                        } else {
                            None
                        };
                        match (want, active) {
                            (Some(wm), am) if wm != am => {
                                if let Some((items, rect)) = open_submenu(
                                    &flyout_w,
                                    &pillmenu_w,
                                    &pill_w,
                                    &state_timer,
                                    wm,
                                ) {
                                    if let Ok(mut u) = flyout_ui_timer.lock() {
                                        u.mode = wm;
                                        u.items = items;
                                        u.rect = Some(rect);
                                    }
                                }
                            }
                            (None, _) if pinned.is_none() => {
                                if let Some(f) = flyout_w.upgrade() {
                                    if f.window().is_visible() {
                                        let _ = f.hide();
                                    }
                                }
                                if let Ok(mut u) = flyout_ui_timer.lock() {
                                    if u.mode != SubMode::None {
                                        u.mode = SubMode::None;
                                        u.rect = None;
                                    }
                                }
                            }
                            _ => {}
                        }
                        // Outside-dismiss for the cluster (menu + flyout + pill).
                        let in_menu = cx >= mpos.x
                            && cx <= mpos.x + mw
                            && cy >= mpos.y
                            && cy <= mpos.y + mh;
                        let in_pill = {
                            let ppos = p.window().position();
                            let (cw, ch) = cur_size(&expanded_timer, &vertical_timer);
                            let pw = (cw as f32 * scale).round() as i32;
                            let ph = (ch as f32 * scale).round() as i32;
                            let m = (4.0 * scale).round() as i32;
                            cx >= ppos.x - m
                                && cx <= ppos.x + pw + m
                                && cy >= ppos.y - m
                                && cy <= ppos.y + ph + m
                        };
                        let in_cluster = in_menu || in_fly || in_pill || in_corridor;
                        let fresh = menu2_open_at_timer
                            .lock()
                            .unwrap()
                            .map(|at| {
                                at.elapsed() < std::time::Duration::from_millis(500)
                            })
                            .unwrap_or(false);

                        #[cfg(target_os = "windows")]
                        let clicked_outside = unsafe {
                            use windows::Win32::UI::Input::KeyboardAndMouse::{GetAsyncKeyState, VK_LBUTTON, VK_RBUTTON};
                            (GetAsyncKeyState(VK_LBUTTON.0 as i32) as u16 & 0x8000 != 0
                                || GetAsyncKeyState(VK_RBUTTON.0 as i32) as u16 & 0x8000 != 0)
                                && !in_cluster
                                && !fresh
                        };
                        #[cfg(not(target_os = "windows"))]
                        let clicked_outside = false;

                        if clicked_outside {
                            hide_pill_menus(&pillmenu_w, &flyout_w);
                            *mlock(&menu_pin_timer) = None;
                            if let Ok(mut u) = flyout_ui_timer.lock() {
                                u.mode = SubMode::None;
                                u.rect = None;
                            }
                            menu2_outside = None;
                            // Seamless collapse of pill
                            let is_exp = expanded_timer.lock().map(|e| *e).unwrap_or(false);
                            let dragging_now = drag_timer.lock().map(|dd| dd.active).unwrap_or(false);
                            let now = std::time::Instant::now();
                            if !dragging_now && is_exp {
                                let (mg0, uo0) = (p.get_mic_grow(), p.get_upper_op());
                                *mlock(&foot_anim_timer) = Some(FootAnim {
                                    t0: now,
                                    dur_ms: 200,
                                    expanding: false,
                                    mg0,
                                    uo0,
                                });
                                log_line("ui: menu+pill cluster left -> collapse glide start");
                            }
                            log_line("pill menu dismissed (click outside)");
                        }
                    } else {
                        menu2_outside = None;
                        // Menu closed: flyout + pin follow it down.
                        if let Some(f) = flyout_w.upgrade() {
                            if f.window().is_visible() {
                                let _ = f.hide();
                            }
                        }
                        *mlock(&menu_pin_timer) = None;
                        if let Ok(mut u) = flyout_ui_timer.lock() {
                            u.mode = SubMode::None;
                            u.rect = None;
                        }
                    }
                }
            }

            if let Ok(s) = state_timer.lock() {
                // Live session: mic capsule ON, PTT held, or core mid-turn.
                // Wakeword-ARMED idle is NOT a session (renders as idle,
                // hover-to-expand) — counting it kept the pill expanded
                // forever with a fake "Listening…" and dead bars.
                let recording = s.mode == AppMode::Recording;
                let transcribing = s.mode == AppMode::Transcribing;
                // PTT held + mic capsule ON both count as live for display.
                let ptt = *mlock(&ptt_held_timer);
                let mic_on = *mlock(&mic_live_timer);
                let live_session = recording || transcribing || ptt || mic_on;

                if !live_session {
                    capture_current_fg_window(&last_external_fg_timer);
                }
                // Ghost-turn auto-cancel: a clap/wakeword that opens a turn
                // but is followed by silence would otherwise sit "Listening"
                // forever (nothing else closes a trigger-opened silent turn)
                // or transcribe the trigger noise into "you"/"yeah" and type
                // it. If 6s in no transcript has arrived AND the segmenter
                // VAD never opened, close the turn silently — no engine call,
                // no text, no ghost. 6s (not 2s): a human needs a beat after
                // the trigger ack before they start dictating; deliberate
                // speech always opens the VAD first (it is the same VAD that
                // decides what gets transcribed), and held PTT/mic turns are
                // never cancelled (explicit user intent). A short pill hint
                // explains the close so it never reads as a glitch.
                if recording
                    && !transcribing
                    && !ptt
                    && !mic_on
                    && std::time::Instant::now().duration_since(sess_start_t)
                        > std::time::Duration::from_millis(6000)
                    && s.transcript_buffer.len() == sess_start_len
                    && !s.session_saw_speech
                {
                    log_line("session auto-cancelled (no speech after trigger)");
                    alert_msg = "Nothing detected — speak up".to_string();
                    alert_until =
                        std::time::Instant::now() + std::time::Duration::from_millis(2500);
                    let _ = tx_cmd_timer.try_send(OrchestratorCommand::StopListening);
                }

                // Sync wakeword detection engine with session state.
                // The background mic stays up when wakewords OR the clap
                // transient are armed (claps work with wakewords fully off)
                // and parks while a session is live — the foreground thread
                // owns STOP claps mid-session.
                let ww_on = s.wakeword_active
                    || (!s.settings.wake_word_mode.eq_ignore_ascii_case("Off")
                        && !s.settings.wake_word_mode.trim().is_empty());
                let transient_armed = s.settings.transient_action != 2;
                let bg_wanted = (ww_on || transient_armed) && !live_session;
                if let Some(ref wh) = wakeword_handle_timer {
                    if bg_wanted {
                        if !wh.is_active() {
                            wh.start();
                        }
                    } else if wh.is_active() {
                        wh.stop();
                    }
                }
                if transcribing {
                    p.set_loading_tick((tick / 2 % 3) as i32);
                }
                let now = std::time::Instant::now();
                // Session audio peak over the WHOLE session: mid-speech
                // pauses never trigger the nothing-heard error — only
                // silence from the very start does.
                if live_session && !was_live {
                    sess_start_len = s.transcript_buffer.len();
                    sess_peak = 0;
                    speech_peak = 0;
                    sess_start_t = now;
                    // A new session re-arms the TextBoard auto-show (a user
                    // close keeps it down only for the finished session) and
                    // leaves history mode (fresh turns show current only).
                    *mlock(&tb_closed_timer) = false;
                    *mlock(&tb_full_timer) = false;
                    log_line("session start");
                    // Trigger-opened turns (wakeword/clap — not held PTT or
                    // mic tap) get a proactive coaching hint: there is no
                    // beep, so users otherwise say the wakeword, wait, and
                    // meet the auto-cancel. "Speak your command" teaches the
                    // one-breath flow in-product.
                    if !ptt && !mic_on {
                        alert_msg = "Speak your command…".to_string();
                        alert_until =
                            now + std::time::Duration::from_millis(3000);
                    }
                }
                if live_session {
                    sess_peak = sess_peak.max(s.audio_level as u32);
                    if now.duration_since(sess_start_t)
                        > std::time::Duration::from_millis(800)
                    {
                        speech_peak = speech_peak.max(s.audio_level as u32);
                    }
                }
                was_live = live_session;
                // Done pulse: the engine just finished a turn (any landing
                // mode — success lands in wakeword-armed, errors land Idle).
                let mut turn_just_done = false;
                if prev_transcribing && !transcribing {
                    turn_just_done = true;
                    done_until = now + std::time::Duration::from_millis(600);
                    tb_until = now + std::time::Duration::from_secs(8);
                    // Mic capsule auto-clears at turn end so a tap-ON never
                    // wedges the pill expanded when the turn completes.
                    *mlock(&mic_live_timer) = false;
                    let grown = s
                        .transcript_buffer
                        .len()
                        .saturating_sub(sess_start_len);
                    log_line(&format!("turn done: +{grown} chars, peak={sess_peak}"));
                    // Remember the finished turn for "Paste last transcript".
                    *mlock(&last_turn_timer) = s.transcript_buffer
                        [sess_start_len.min(s.transcript_buffer.len())..]
                        .trim()
                        .to_string();
                    if grown == 0 && sess_peak < 10 {
                        alert_msg = "Nothing detected — speak up".to_string();
                        alert_until = now + std::time::Duration::from_millis(2500);
                        log_line("alert: nothing detected");
                    }
                }
                prev_transcribing = transcribing;
                // Idle auto-offload (dashboard "Auto-offload STT model" +
                // delay): once a turn ends and the widget settles, release
                // the Rust STT session from RAM. Next turn reloads on demand.
                if turn_just_done {
                    last_turn_end = Some(now);
                    offload_sent = false;
                }
                if live_session {
                    offload_sent = false;
                }
                if !offload_sent && !live_session && !transcribing {
                    if let Some(t0) = last_turn_end {
                        let wait = match s.settings.offload_seconds {
                            0 => std::time::Duration::from_millis(500),
                            secs => std::time::Duration::from_secs(secs as u64),
                        };
                        if s.settings.auto_offload && now.duration_since(t0) >= wait
                        {
                            if tx_cmd_timer
                                .try_send(OrchestratorCommand::OffloadModel)
                                .is_ok()
                            {
                                offload_sent = true;
                                log_line("idle offload sent");
                            }
                        }
                    }
                }
                // Bulk end-delivery (C++-style): while a session is live the
                // buffer only accumulates — nothing trickles out word by
                // word. The whole turn goes out in ONE burst (a single
                // SendInput run / a single clipboard write) when the engine
                // finalizes the turn, or on the first settled tick after
                // (covers fast PTT that never raises Transcribing, session
                // end, streaming commits and SAY tests). A shrink (buffer
                // cleared elsewhere) just re-baselines. Hallucination guard:
                // VAD-gated — silence loops ("you you you") never type.
                let is_streaming = s.model_entries
                    .get(s.selected_model)
                    .map(|e| quickstt_core::models::catalog::is_streaming_model(&e.name))
                    .unwrap_or(false);
                let out_mode = s.settings.ctrl_space_output;
                let blen = s.transcript_buffer.len();
                if blen < turn_base_len {
                    turn_base_len = blen;
                }
                if blen < last_buf_len {
                    last_buf_len = blen;
                }
                let settled = !live_session && !transcribing;
                if is_streaming {
                    // Streaming: no TextBoard preview — deltas go straight to
                    // the target app word-by-word (direct paste, no preview).
                    if blen > last_buf_len {
                        let delta = s.transcript_buffer[last_buf_len..blen].to_owned();
                        let trimmed = delta.trim();
                        if !trimmed.is_empty() {
                            let low = trimmed.to_lowercase();
                            if should_drop_hallucination(low.as_str(), speech_peak) {
                                log_line(&format!(
                                    "streaming hallucination dropped {delta:?} peak={sess_peak}"
                                ));
                            } else {
                                log_line(&format!("streaming turn output delta +{} chars", delta.len()));
                                let target_fg = *mlock(&last_external_fg_timer);
                                deliver_transcription_output(&delta, out_mode, target_fg);
                            }
                        }
                        last_buf_len = blen;
                    }
                    if turn_just_done || settled {
                        turn_base_len = blen;
                        last_buf_len = blen;
                    }
                } else {
                    if (turn_just_done || settled) && blen > turn_base_len {
                        let full = s.transcript_buffer[turn_base_len..].trim().to_owned();
                        if !full.is_empty() {
                            let low = full.to_lowercase();
                            if should_drop_hallucination(low.as_str(), speech_peak) {
                                log_line(&format!(
                                    "hallucination dropped {full:?} peak={sess_peak}"
                                ));
                            } else {
                                log_line(&format!("batch turn output bulk +{} chars", full.len()));
                                let target_fg = *mlock(&last_external_fg_timer);
                                deliver_transcription_output(&full, out_mode, target_fg);
                            }
                        }
                        turn_base_len = blen;
                        last_buf_len = blen;
                    }
                }
                let msg = if s.status_message.is_empty() {
                    "Ready".to_string()
                } else {
                    s.status_message.clone()
                };
                // Phase morph: idle / recording bars / transcribing dots /
                // done check / no-model hint. Mic-ON and PTT both render as
                // recording (bars + red mic).
                let low = msg.to_lowercase();
                let nomodel = low.contains("no model")
                    || low.contains("not installed")
                    || low.contains("missing")
                    || low.contains("download failed")
                    || low.contains("model load failed")
                    || low.contains("add model");
                let phase = if recording || ptt || mic_on {
                    1
                } else if transcribing {
                    2
                } else if now < done_until {
                    3
                } else if nomodel {
                    4
                } else {
                    0
                };
                if phase != logged_phase || msg != logged_msg {
                    log_line(&format!("phase {logged_phase}->{phase} status={msg:?}"));
                    logged_phase = phase;
                    logged_msg = msg.clone();
                }
                p.set_phase(phase);
                // Watchdog: a hung engine must never wedge the pill in
                // "Transcribing…" forever. Past 150s, kill the helper by PID
                // (Slint's own parakeet child only — C++ untouched) and abort
                // the turn; the next turn respawns the engine session.
                if phase == 2 {
                    if transcribing_since.is_none() {
                        transcribing_since = Some(now);
                    } else if transcribing_since
                        .map(|t| {
                            now.duration_since(t) > std::time::Duration::from_secs(150)
                        })
                        .unwrap_or(false)
                    {
                        transcribing_since = None;
                        *mlock(&mic_live_timer) = false;
                        if let Some(pid) =
                            quickstt_core::models::engine::parakeet_child_pid()
                        {
                            log_line(&format!("watchdog: killing hung parakeet {pid}"));
                            kill_pid(pid);
                        }
                        if let Some(pid) =
                            quickstt_core::models::engine::photon_child_pid()
                        {
                            log_line(&format!("watchdog: killing hung photon {pid}"));
                            kill_pid(pid);
                        }
                        if tx_cmd_timer
                            .try_send(OrchestratorCommand::AbortTranscribe)
                            .is_err()
                        {
                            log_line("watchdog send FAILED (command loop dead?)");
                        }
                    }
                } else {
                    transcribing_since = None;
                }
                // Pill carries NO transcript text (bars / fixed labels only —
                // the transcript lives in the TextBoard). This kills the old
                // "second turn shows both" echo inside the pill and the
                // soundwave-area text entirely.
                let pill_live: String = if nomodel { msg.clone() } else { String::new() };
                if p.get_live_text().as_str() != pill_live {
                    p.set_live_text(pill_live.clone().into());
                }
                // Tight status pill: fixed strings only (no transcript
                // measuring, so no jitter) with just a little whitespace.
                // Bottom 90..200, side 90..196, transcribing >=150 so the
                // label+dots never clip.
                // Idle hint is two runs (grey "Dictate" + white
                // "Ctrl + Space" at 15px, 8px gap, generous side margins):
                // measure both so the roomy (+40%) pill shrink-wraps exactly
                // (~184px: ~150 text + 34 padding).
                let meas = if phase == 0 {
                    widget_platform::measure_text(p.window(), "Dictate", 15.0, false)
                        + widget_platform::measure_text(p.window(), "Ctrl + Space", 15.0, true)
                        + 32.0
                } else if phase == 2 {
                    widget_platform::measure_text(p.window(), "Transcribing…", 12.0, true) + 24.0
                } else {
                    120.0
                };
                let vert_now = *mlock(&vertical_timer);
                let (lo, hi) = if vert_now {
                    (90.0, 200.0)
                } else {
                    (90.0, 200.0)
                };
                let mut uw = ((meas.clamp(lo, hi) / 2.0).round() * 2.0) as i32;
                if phase == 2 {
                    uw = uw.max(140);
                }
                if (uw - last_upper_w).abs() > 8 || phase != last_phase {
                    p.set_upper_w(uw);
                    log_line(&format!("upper_w {last_upper_w}->{uw} phase {phase}"));
                    last_upper_w = uw;
                    last_phase = phase;
                }
                // Alerts: nothing-heard, or Ctrl+Space held by another app.
                if !*mlock(&hotkey_ok_poll)
                    && app_start.elapsed() > std::time::Duration::from_secs(8)
                    && now >= alert_until
                {
                    alert_msg = "Ctrl+Space is used by another app".to_string();
                    alert_until = now + std::time::Duration::from_secs(4);
                    log_line("alert: hotkey busy");
                }
                if now < alert_until {
                    if p.get_alert_text().as_str() != alert_msg {
                        p.set_alert_text(alert_msg.clone().into());
                    }
                } else if !p.get_alert_text().is_empty() {
                    p.set_alert_text("".into());
                }
                // ── dashboard sync (Slint dashboard is dormant; the full Qt
                // dashboard owns models/languages) ──
                {
                    fn rc_str(v: &[String]) -> ModelRc<slint::SharedString> {
                        ModelRc::new(VecModel::from_slice(
                            &v.iter().map(slint::SharedString::from).collect::<Vec<_>>(),
                        ))
                    }
                    fn cur_str(m: &ModelRc<slint::SharedString>) -> Vec<String> {
                        (0..m.row_count())
                            .filter_map(|r| m.row_data(r))
                            .map(|v| v.to_string())
                            .collect()
                    }
                    let names: Vec<String> =
                        s.model_entries.iter().map(|e| e.name.clone()).collect();
                    if cur_str(&d.get_models()) != names {
                        d.set_models(rc_str(&names));
                    }
                    if d.get_selected_model() != s.selected_model as i32 {
                        d.set_selected_model(s.selected_model as i32);
                    }
                    if let Some(e) = s.model_entries.get(s.selected_model) {
                        if cur_str(&d.get_languages()) != e.languages {
                            d.set_languages(rc_str(&e.languages));
                        }
                        let lang_idx = e
                            .languages
                            .iter()
                            .position(|l| l == &s.selected_language)
                            .unwrap_or(0)
                            as i32;
                        if d.get_selected_language() != lang_idx {
                            d.set_selected_language(lang_idx);
                        }
                    }
                    let installed = s.model_entries.iter().filter(|e| e.installed).count();
                    let info = format!(
                        "{} widget models, {} installed — transcription language follows the selected model.",
                        s.model_entries.len(),
                        installed
                    );
                    if d.get_models_info().as_str() != info {
                        d.set_models_info(info.into());
                    }
                    // ── hardware-aware catalog cards (image-inspired) ──
                    // Badges: Active (selected) > Recommended > none. Search +
                    // language filtering happen here so Slint stays dumb.
                    {
                        use crate::ModelCard;
                        let q = s.catalog_search.trim().to_lowercase();
                        let lf = s.catalog_lang_filter.clone();
                        let mut cards: Vec<ModelCard> = Vec::new();
                        for (idx, e) in s.model_entries.iter().enumerate() {
                            if !q.is_empty()
                                && !e.name.to_lowercase().contains(&q)
                                && !e.blurb.to_lowercase().contains(&q)
                            {
                                continue;
                            }
                            if lf != "All Languages" && !e.languages.iter().any(|l| l == &lf) {
                                continue;
                            }
                            let streaming =
                                quickstt_core::models::catalog::is_streaming_model(&e.name);
                            let langs_label = if e.languages.len() == 1
                                && e.languages[0] == "English"
                            {
                                if streaming {
                                    "English only • Streaming".to_string()
                                } else {
                                    "English only".to_string()
                                }
                            } else {
                                let n = e
                                    .languages
                                    .iter()
                                    .filter(|l| *l != "Auto")
                                    .count()
                                    .max(1);
                                let mut lbl = format!("{} languages", n);
                                if e.engine_family == "canary" {
                                    lbl.push_str(" • Translate");
                                } else if streaming {
                                    lbl.push_str(" • Streaming");
                                }
                                lbl
                            };
                            let size_label = if e.size_mb >= 1024 {
                                format!("{:.1} GB", e.size_mb as f32 / 1024.0)
                            } else {
                                format!("{} MB", e.size_mb)
                            };
                            let is_active = idx == s.selected_model;
                            let is_rec = Some(idx) == s.recommended_model && !is_active;
                            let badge = if is_active {
                                "✓ Active"
                            } else if is_rec {
                                "Recommended"
                            } else {
                                ""
                            };
                            let downloading =
                                s.is_downloading && s.download_name == e.name;
                            cards.push(ModelCard {
                                name: e.name.clone().into(),
                                blurb: e.blurb.clone().into(),
                                badge: badge.into(),
                                accuracy: e.accuracy as i32,
                                speed: e.speed as i32,
                                langs: langs_label.into(),
                                size: size_label.into(),
                                installed: e.installed,
                                is_active,
                                index: idx as i32,
                                downloading,
                            });
                        }
                        let cur_n = d.get_catalog().row_count();
                        let mut same = cur_n == cards.len();
                        if same {
                            for (i, c) in cards.iter().enumerate() {
                                if let Some(cur) = d.get_catalog().row_data(i) {
                                    if cur.name != c.name
                                        || cur.badge != c.badge
                                        || cur.installed != c.installed
                                        || cur.is_active != c.is_active
                                        || cur.index != c.index
                                        || cur.downloading != c.downloading
                                    {
                                        same = false;
                                        break;
                                    }
                                } else {
                                    same = false;
                                    break;
                                }
                            }
                        }
                        if !same {
                            d.set_catalog(ModelRc::new(VecModel::from_slice(&cards)));
                        }
                        // Language filter options: union of entry languages.
                        let mut lang_opts = vec!["All Languages".to_string()];
                        for e in s.model_entries.iter() {
                            for l in e.languages.iter() {
                                if l != "Auto" && !lang_opts.contains(l) {
                                    lang_opts.push(l.clone());
                                }
                            }
                        }
                        if cur_str(&d.get_catalog_langs()) != lang_opts {
                            d.set_catalog_langs(rc_str(&lang_opts));
                        }
                        let lang_idx = lang_opts
                            .iter()
                            .position(|l| l == &s.catalog_lang_filter)
                            .unwrap_or(0)
                            as i32;
                        if d.get_catalog_lang_idx() != lang_idx {
                            d.set_catalog_lang_idx(lang_idx);
                        }
                        if d.get_catalog_search().as_str() != s.catalog_search.as_str() {
                            d.set_catalog_search(s.catalog_search.clone().into());
                        }
                        let rec_name = s
                            .recommended_model
                            .and_then(|i| s.model_entries.get(i))
                            .map(|e| e.name.clone())
                            .unwrap_or_default();
                        if d.get_recommend_name().as_str() != rec_name {
                            d.set_recommend_name(rec_name.into());
                        }
                        if d.get_recommend_reason().as_str() != s.recommend_reason.as_str() {
                            d.set_recommend_reason(s.recommend_reason.clone().into());
                        }
                        let hw_line = if s.hardware_summary.is_empty() {
                            String::new()
                        } else {
                            format!("Acceleration: {}", s.hardware_summary)
                        };
                        if d.get_hardware_info().as_str() != hw_line {
                            d.set_hardware_info(hw_line.into());
                        }
                    }
                    if d.get_wakeword_enabled() != s.wakeword_active {
                        d.set_wakeword_enabled(s.wakeword_active);
                    }
                    let sens = s.settings.wakeword_sensitivity.clamp(0, 100) as i32;
                    if d.get_wakeword_sensitivity() != sens {
                        d.set_wakeword_sensitivity(sens);
                    }
                    // Exact enforced rule, computed by the same function the
                    // engine uses — the slider value always means this.
                    let thr =
                        quickstt_core::ml::wakeword::WakeWordEngine::threshold_for(
                            sens.max(0) as u32,
                        );
                    let thr_text = format!(
                        "Trigger rule: score ≥ {:.2} — 2 hits if strong, else 3 in a row, voice-gated. Lower is hotter.",
                        thr
                    );
                    if d.get_wakeword_threshold_text().as_str() != thr_text.as_str() {
                        d.set_wakeword_threshold_text(thr_text.into());
                    }
                    if d.get_is_downloading() != s.is_downloading {
                        d.set_is_downloading(s.is_downloading);
                    }
                    if d.get_download_progress() != s.download_progress as i32 {
                        d.set_download_progress(s.download_progress as i32);
                    }
                    if d.get_download_status().as_str() != s.download_status.as_str() {
                        d.set_download_status(s.download_status.clone().into());
                    }
                    if d.get_download_speed().as_str() != s.download_speed_text.as_str() {
                        d.set_download_speed(s.download_speed_text.clone().into());
                    }
                    if d.get_download_bytes().as_str() != s.download_bytes_text.as_str() {
                        d.set_download_bytes(s.download_bytes_text.clone().into());
                    }
                    if d.get_download_file().as_str() != s.download_file_label.as_str() {
                        d.set_download_file(s.download_file_label.clone().into());
                    }
                    if d.get_live_audio_level() != s.audio_level as i32 {
                        d.set_live_audio_level(s.audio_level as i32);
                    }
                    let vad_sens = s.settings.vad_sensitivity.clamp(0, 100) as i32;
                    if d.get_vad_sensitivity() != vad_sens {
                        d.set_vad_sensitivity(vad_sens);
                    }
                }
                // waveform: push audio_level, decay smoothly
                let target = (s.audio_level as f32 / 100.0).clamp(0.0, 1.0);
                let live_bars = live_session || transcribing;
                // Fast attack: when speech starts, wake the whole 24-bar
                // history at once — otherwise bars fill in from the left
                // and the waveform sits lopsided for a second.
                if live_bars && target > 0.05 && smooth.iter().all(|v| *v < 0.05) {
                    for v in smooth.iter_mut() {
                        *v = target;
                    }
                }
                // shift + push: newest at end
                smooth.remove(0);
                smooth.push(target);
                // ease display toward history
                let display: Vec<f32> = smooth
                    .iter()
                    .map(|v| {
                        if live_bars {
                            let eased = (*v * 0.9 + target * 0.1).clamp(0.02, 1.0);
                            // Snap to 1/6 steps: bar heights land on even
                            // pixels (2,4,..,14) with integer centering, so
                            // bars never sit half-px (blurred on software raster).
                            (eased * 6.0).round() / 6.0
                        } else {
                            0.0
                        }
                    })
                    .collect();
                p.set_levels(ModelRc::new(VecModel::from_slice(&display)));

                // ── TextBoard sync: CURRENT TURN ONLY (never history) —
                // unless history mode was opened from the dark menu, which
                // shows the FULL buffer until closed / next session.
                // Streaming models: NO preview — deltas paste directly into
                // the target app word-by-word; the board stays hidden unless
                // the user explicitly opened history.
                if let Some(tb) = textboard_w.upgrade() {
                    let show_full = *mlock(&tb_full_timer);
                    let start = if show_full {
                        0
                    } else {
                        sess_start_len.min(s.transcript_buffer.len())
                    };
                    let turn_t = s.transcript_buffer[start..].trim();
                    let part_t = s.partial_result.trim();
                    let full = if !part_t.is_empty() && !turn_t.is_empty() {
                        format!("{turn_t}\n{part_t}")
                    } else if !part_t.is_empty() {
                        part_t.to_string()
                    } else if turn_t.is_empty() {
                        "Transcript will appear here...".to_string()
                    } else {
                        turn_t.to_string()
                    };
                    let display_tb: String = if full.len() > 1200 {
                        full.chars()
                            .rev()
                            .take(1200)
                            .collect::<String>()
                            .chars()
                            .rev()
                            .collect()
                    } else {
                        full
                    };
                    if tb.get_body_text().as_str() != display_tb {
                        tb.set_body_text(display_tb.into());
                    }
                    if tb.get_title_text().as_str() != msg {
                        tb.set_title_text(msg.clone().into());
                    }
                    let closed = *mlock(&tb_closed_timer);
                    // Streaming suppresses the preview board (direct paste only).
                    let streaming_now = s.model_entries
                        .get(s.selected_model)
                        .map(|e| quickstt_core::models::catalog::is_streaming_model(&e.name))
                        .unwrap_or(false);
                    let want_tb = !closed
                        && !(turn_t.is_empty() && part_t.is_empty())
                        && (show_full
                            || (!streaming_now
                                && (live_session || transcribing || now < tb_until)));
                    if want_tb && !tb.window().is_visible() {
                        *mlock(&tb_follow_timer) = true;
                        widget_platform::show_widget(tb.window());
                        log_line("textboard shown");
                        log_line(&widget_platform::window_state_debug("textboard", tb.window()));
                    }
                    // Follow the pill while visible (tracks hover-expand and
                    // dock hops so the board never covers the pill); a user
                    // drag of the board disables follow until it re-shows.
                    if want_tb
                        && tb.window().is_visible()
                        && *mlock(&tb_follow_timer)
                    {
                        // Park above the pill (bottom dock) or beside it
                        // (side docks), inside the WORK area so the board
                        // never slides behind the taskbar either.
                        let scale =
                            widget_platform::window_scale(p.window()).max(0.5);
                        let tw = (TB_W as f32 * scale).round() as i32;
                        let th = (TB_H as f32 * scale).round() as i32;
                        let (wl, wt, wr, wb) = widget_platform::work_area();
                        let ppos = p.window().position();
                        let (fw, fh) = {
                            let e = *mlock(&expanded_timer);
                            let v = *mlock(&vertical_timer);
                            let (w, h) = if e {
                                if v {
                                    (SIDE_EXP_W, SIDE_EXP_H)
                                } else {
                                    (EXP_W, EXP_H)
                                }
                            } else if v {
                                (SIDE_MINI_W, SIDE_MINI_H)
                            } else {
                                (MINI_W, MINI_H)
                            };
                            (
                                (w as f32 * scale).round() as i32,
                                (h as f32 * scale).round() as i32,
                            )
                        };
                        let idx = *mlock(&corner_idx_timer);
                        let vert = *mlock(&vertical_timer);
                        let (nx, ny) = if !vert || idx == 0 {
                            (ppos.x + fw / 2 - tw / 2, ppos.y - th - 12)
                        } else if idx == 1 {
                            (ppos.x - tw - 12, ppos.y + fh / 2 - th / 2)
                        } else {
                            (ppos.x + fw + 12, ppos.y + fh / 2 - th / 2)
                        };
                        tb.window().set_position(slint::PhysicalPosition {
                            x: nx.clamp(wl, (wr - tw).max(wl)),
                            y: ny.clamp(wt, (wb - th).max(wt)),
                        });
                    } else if !want_tb && tb.window().is_visible() {
                        let _ = tb.hide();
                        log_line("textboard hidden");
                    }
                }

                // Hover-only expand (no pin): collapsed unless hovered or live.
                // "Hide for 1 hour" snooze overrides widget_visible (tray +
                // menus keep working; the pill comes back on expiry).
                let dragging = drag_timer.lock().map(|dd| dd.active).unwrap_or(false);
                let session = live_session || transcribing;
                let snoozed = hide_until_timer
                    .lock()
                    .map(|h| h.map(|u| now < u).unwrap_or(false))
                    .unwrap_or(false);
                if snoozed && !prev_snoozed {
                    log_line("pill snoozed (hide 1h)");
                }
                if !snoozed && prev_snoozed {
                    log_line("pill snooze expired — back");
                }
                prev_snoozed = snoozed;
                if s.widget_visible && !snoozed {
                    if !p.window().is_visible() {
                        widget_platform::show_widget(p.window());
                    }
                    // Morph drive lives in the 16ms anim ticker (60fps,
                    // content props only — the window never resizes); the
                    // poll only STARTS glides and reads `animating` below.
                    let animating = foot_anim_timer.lock().map(|a| a.is_some()).unwrap_or(false);
                    // Stale-press force-end: a press with no motion for 5s+ and
                    // older than 10s overall lost its release (focus ripped
                    // away mid-press). Left armed, EVERY later mouse move
                    // would pop the overlay "out of nowhere" and swallow the
                    // next real press (drag_start early-returns while active).
                    // A legit 15s motionless hold is the only casualty (press
                    // again to drag) — rare and logged.
                    {
                        let mut dd = mlock(&drag_timer);
                        if dd.active
                            && dd.press_at.elapsed() > std::time::Duration::from_secs(10)
                            && dd.last_move.elapsed() > std::time::Duration::from_secs(5)
                        {
                            dd.active = false;
                            dd.mic_used = false;
                            dd.snap_idx = None;
                            log_line("stale press force-ended (lost release)");
                        }
                    }
                    // Deferred double-click hop commit. Decision per tick:
                    // - latched press travelled -> CANCEL (a drag: fast ones
                    //   and slow-start ones alike — never hop under motion).
                    // - a press still held motionless -> WAIT for the release
                    //   (5s cap, then commit+rebase so nothing sticks).
                    // - released clean -> COMMIT (the hop).
                    // NOTE: take() only on decision (guard drops at statement
                    // end) — matching on *mlock() in the scrutinee keeps the
                    // guard alive across the body and self-deadlocks the main
                    // thread on a re-lock (frozen app, live IPC).
                    // Decide from a snapshot (guard drops at statement end),
                    // take + act only on decision — never hold across body.
                    let hop_decision: Option<bool> = {
                        let guard = mlock(&pending_hop_timer);
                        match *guard {
                            None => None,
                            Some((t0, n0)) => {
                                if t0.elapsed()
                                    < std::time::Duration::from_millis(260)
                                {
                                    None
                                } else {
                                    let (active, moved, pn) = drag_timer
                                        .lock()
                                        .map(|dd| {
                                            (dd.active, dd.moved_far, dd.press_n)
                                        })
                                        .unwrap_or((false, false, 0));
                                    if moved && pn == n0 {
                                        Some(false) // travelled -> cancel
                                    } else if pn != n0 {
                                        // Latched press superseded by a newer
                                        // press: hop only with no finger down,
                                        // otherwise the stale hop dies — never
                                        // teleport under a foreign gesture.
                                        Some(!active)
                                    } else if active {
                                        if t0.elapsed()
                                            > std::time::Duration::from_secs(5)
                                        {
                                            Some(true) // stuck hold -> commit
                                        } else {
                                            None // held still -> wait release
                                        }
                                    } else {
                                        Some(true) // released clean -> commit
                                    }
                                }
                            }
                        }
                    };
                    if let Some(commit) = hop_decision {
                        mlock(&pending_hop_timer).take();
                        if !commit {
                            log_line("ui: dock hop cancelled (became drag)");
                        } else {
                                dock_hop(
                                    &pill_w,
                                    &corner_idx_timer,
                                    &expanded_timer,
                                    &vertical_timer,
                                    &drag_timer,
                                    &suppress_menu_timer,
                                    true,
                                );
                                let (ccx, ccy) = widget_platform::cursor_position();
                                let pos = p.window().position();
                                log_line(&format!(
                                    "ui: hop committed rect={:?}",
                                    widget_platform::native_rect(p.window())
                                ));
                                if let Ok(mut dd) = drag_timer.lock() {
                                    if dd.active {
                                        dd.grab_cursor = (ccx, ccy);
                                        dd.grab_win = (pos.x, pos.y);
                                    }
                                }
                                spawn_dragcap("hop");
                            }
                        }
                    // Overlay watchdog: a visible shade with no active drag
                    // and no fresh show-stamp is a lost drop — force-hide it
                    // so it can never sit on-screen looking like a frozen
                    // crash. (Normal drags re-stamp every press; drag_end
                    // hides explicitly.)
                    if let Some(o) = overlay_w.upgrade() {
                        if o.window().is_visible() {
                            let dragging_now = drag_timer
                                .lock()
                                .map(|d| d.active)
                                .unwrap_or(false);
                            let age_ms = overlay_shown_timer
                                .lock()
                                .map(|s| {
                                    s.map(|at| at.elapsed().as_millis()).unwrap_or(u128::MAX)
                                })
                                .unwrap_or(u128::MAX);
                            // No-drag grace 1.5s; absolute 5s cap even
                            // mid-drag (a motionless 5s hold is a lost drop —
                            // the next move re-shows, so this self-heals).
                            let reason = if !dragging_now && age_ms > 1500 {
                                Some("no active drag")
                            } else if age_ms > 5000 {
                                Some("stale active drag")
                            } else {
                                None
                            };
                            if let Some(why) = reason {
                                let _ = o.hide();
                                log_line(&format!("overlay watchdog: force-hide ({why})"));
                            }
                        }
                    }
                    // Paint insurance: while a drag is live and the shade is
                    // up, re-request its frame every ~150ms (and keep the
                    // pill stacked above it). A post-show resize can silently
                    // drop the show-time paint request, and under an input
                    // flood nothing re-flags it — the shade then sits
                    // visible-but-blank for the whole drag.
                    if let Some(o) = overlay_w.upgrade() {
                        if o.window().is_visible()
                            && drag_timer
                                .lock()
                                .map(|dd| dd.active)
                                .unwrap_or(false)
                            && last_ov_insure
                                .map(|t| {
                                    t.elapsed()
                                        > std::time::Duration::from_millis(150)
                                })
                                .unwrap_or(true)
                        {
                            last_ov_insure = Some(std::time::Instant::now());
                            o.window().request_redraw();
                            widget_platform::restack_topmost(p.window());
                        }
                    }
                    // Taskbar-watch (every ~250ms): if the taskbar revealed,
                    // hid, or changed size/setting, the taskbar-aware dock
                    // moved — glide the bottom-docked pill with it so it
                    // always sits just above the taskbar, collapsed AND
                    // expanded. Skipped while dragging/animating (they own
                    // the window) and off the bottom dock.
                    if tick % 5 == 0
                        && *mlock(&corner_idx_timer) == 0
                        && drag_timer
                            .lock()
                            .map(|d| !d.active && d.docked_exact)
                            .unwrap_or(false)
                        && !animating
                    {
                        let is_exp = *mlock(&expanded_timer);
                        let vert = *mlock(&vertical_timer);
                        let (lw, lh) = if is_exp {
                            if vert {
                                (SIDE_EXP_W, SIDE_EXP_H)
                            } else {
                                (EXP_W, EXP_H)
                            }
                        } else if vert {
                            (SIDE_MINI_W, SIDE_MINI_H)
                        } else {
                            (MINI_W, MINI_H)
                        };
                        let scale = widget_platform::window_scale(p.window()).max(0.5);
                        let (dx, dy) =
                            widget_platform::dock_position(0, lw, lh, dock_gap(&expanded_timer), scale);
                        let pos = p.window().position();
                        // Request-dedup: Wayland compositors own final
                        // placement, so the reported position never converges
                        // with our math — comparing against it re-fires every
                        // tick (position fight + log spam + CPU). Compare
                        // against OUR last request instead: identical targets
                        // are never re-sent.
                        if last_dock_req != Some((dx, dy)) {
                            log_line(&format!(
                                "taskbar shift ({},{})->({dx},{dy})",
                                pos.x, pos.y
                            ));
                            p.window()
                                .set_position(slint::PhysicalPosition { x: dx, y: dy });
                            last_dock_req = Some((dx, dy));
                        }
                    }
                    // ── hover-expand into two pieces (cursor-driven in Rust).
                    // No expand while dragging (drag owns the gesture); a
                    // live session forces expanded. Otherwise hover-only.
                    let scale = widget_platform::window_scale(p.window()).max(0.5);
                    let is_exp = *mlock(&expanded_timer);
                    let vert = *mlock(&vertical_timer);
                    let (ew, eh) = if vert {
                        (SIDE_EXP_W, SIDE_EXP_H)
                    } else {
                        (EXP_W, EXP_H)
                    };
                    let (mw, mh) = if vert {
                        (SIDE_MINI_W, SIDE_MINI_H)
                    } else {
                        (MINI_W, MINI_H)
                    };
                    let pos = p.window().position();
                    let (cw, ch) = if is_exp { (ew, eh) } else { (mw, mh) };
                    let (pw, ph) = (
                        (cw as f32 * scale).round() as i32,
                        (ch as f32 * scale).round() as i32,
                    );
                    // Tight hover: expand when the cursor gets close (12px), but
                    // collapse the moment it leaves the expanded card (1px) —
                    // no lingering. Once collapsed, the 12px cushion makes the
                    // mini easy to re-acquire without boundary oscillation.
                    let m = ((if is_exp { 1 } else { 12 }) as f32 * scale).round() as i32;
                    let (ccx, ccy) = widget_platform::cursor_position();
                    let hovering = ccx >= pos.x - m
                        && ccx <= pos.x + pw + m
                        && ccy >= pos.y - m
                        && ccy <= pos.y + ph + m;
                    let menu_open = pillmenu_w
                        .upgrade()
                        .map(|m| m.window().is_visible())
                        .unwrap_or(false);
                    let want = ((hovering || menu_open) && !dragging) || session;
                    if want {
                        last_hover = std::time::Instant::now();
                    }
                    // Which glide (if any) is in flight: None / expanding / collapsing.
                    let glide: Option<bool> = foot_anim_timer
                        .lock()
                        .map(|a| a.map(|an| an.expanding))
                        .unwrap_or(None);
                    if want && !dragging && (!is_exp || glide == Some(false)) && glide != Some(true) {
                        // Start the staged morph (340ms, 60fps) from the LIVE
                        // props, so sweeping back in mid-collapse snaps back
                        // instead of waiting the collapse out. Fresh expands
                        // jump mini -> expanded footprint NOW, while the
                        // content is still the mini bar: transparent pixels
                        // move for free, so the jump is invisible — and the
                        // morph itself is pure Slint interpolation with zero
                        // native resizes (resizes flash white frames).
                        let (mg0, uo0) = (p.get_mic_grow(), p.get_upper_op());
                        if !is_exp {
                            let idx = *mlock(&corner_idx_timer);
                            let (nx, ny) =
                                widget_platform::dock_position(idx, ew, eh, EDGE_GAP, scale);
                            place_pill(&p, nx, ny);
                            set_footprint(&p, ew, eh);
                        }
                        *mlock(&foot_anim_timer) = Some(FootAnim {
                            t0: now,
                            dur_ms: 340,
                            expanding: true,
                            mg0,
                            uo0,
                        });
                        p.set_expanded(true);
                        *mlock(&expanded_timer) = true;
                        log_line("ui: expand glide start");
                    } else if !want
                        && is_exp
                        && !dragging
                        && glide != Some(false)
                        && last_hover.elapsed() > std::time::Duration::from_millis(60)
                    {
                        // Collapse the moment the cursor is out (60ms ≈ one
                        // poll tick: immediate, but immune to single-tick
                        // cursor noise). Preempts an in-flight expand from
                        // the live props — a quick sweep-across never waits
                        // out the expand first. Fast 200ms mirror: dictate
                        // sinks + fades first, then the capsule shrinks.
                        // Window untouched (fixed footprint).
                        let (mg0, uo0) = (p.get_mic_grow(), p.get_upper_op());
                        *mlock(&foot_anim_timer) = Some(FootAnim {
                            t0: now,
                            dur_ms: 200,
                            expanding: false,
                            mg0,
                            uo0,
                        });
                        log_line("ui: collapse glide start");
                    }
                } else if p.window().is_visible() {
                    let _ = p.window().hide();
                    // The pill going away takes the shade with it (a visible
                    // overlay over a hidden pill is exactly the "frozen
                    // preview page" failure).
                    if let Some(o) = overlay_w.upgrade() {
                        if o.window().is_visible() {
                            let _ = o.hide();
                        }
                    }
                    // Next show starts collapsed.
                    let (mw0, mh0) = if *mlock(&vertical_timer) {
                        (SIDE_MINI_W, SIDE_MINI_H)
                    } else {
                        (MINI_W, MINI_H)
                    };
                    set_footprint(&p, mw0, mh0);
                    p.set_expanded(false);
                    p.set_upper_op(0.0);
                    *mlock(&expanded_timer) = false;
                    *mlock(&foot_anim_timer) = None;
                }
            }
            }));
            if tick_ok.is_err() {
                log_line("poll tick panic — caught, continuing");
            }
        },
    );

    // dashboard close -> hide flag
    {
        let dash_visible = dash_visible_close.clone();
        let dash_w = dashboard.as_weak();
        dashboard.on_close_dashboard(move || {
            if let Some(d) = dash_w.upgrade() {
                let _ = d.hide();
            }
            if let Ok(mut v) = dash_visible.lock() {
                *v = false;
            }
        });
    }

    // ── dashboard: model + synced language dropdowns, style, wakeword, vad, system settings ──
    {
        let tx = tx_cmd.clone();
        let state = state.clone();
        dashboard.on_select_model(move |name: slint::SharedString| {
            let idx = state
                .lock()
                .ok()
                .and_then(|s| s.model_entries.iter().position(|e| e.name == name.as_str()));
            if let Some(i) = idx {
                let _ = tx.try_send(OrchestratorCommand::SelectModel(i));
            }
        });
    }
    {
        let tx = tx_cmd.clone();
        dashboard.on_select_language(move |name: slint::SharedString| {
            let _ = tx.try_send(OrchestratorCommand::SelectLanguage(name.to_string()));
        });
    }
    {
        let tx = tx_cmd.clone();
        let wh = wakeword_handle.clone();
        dashboard.on_set_wakeword_enabled(move |on| {
            if let Some(ref w) = wh {
                if on {
                    w.start();
                } else {
                    w.stop();
                }
            }
            let _ = tx.try_send(OrchestratorCommand::ToggleWakeword(on));
        });
    }
    {
        let tx = tx_cmd.clone();
        let wh = wakeword_handle.clone();
        dashboard.on_set_wakeword_sensitivity(move |v| {
            if let Some(ref w) = wh {
                w.set_sensitivity(v as u32);
            }
            let _ = tx.try_send(OrchestratorCommand::SetWakewordSensitivity(v as u32));
        });
    }
    {
        let state = state.clone();
        let wh = wakeword_handle.clone();
        dashboard.on_set_vad_sensitivity(move |v| {
            let v = v.clamp(0, 100) as u32;
            if let Ok(mut s) = state.lock() {
                s.settings.vad_sensitivity = v;
                let _ = s.settings.save_all();
            }
            // Live-wires the background voice gate; the foreground
            // segmenter picks it up when its mic opens.
            if let Some(ref w) = wh {
                w.set_vad_sensitivity(v);
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_auto_stop_silence(move |on| {
            if let Ok(mut s) = state.lock() {
                s.settings.auto_stop_silence = on;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_silence_duration(move |v| {
            if let Ok(mut s) = state.lock() {
                s.settings.silence_duration = v.clamp(1, 30) as u32;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_auto_offload(move |on| {
            if let Ok(mut s) = state.lock() {
                s.settings.auto_offload = on;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_offload_seconds(move |v| {
            if let Ok(mut s) = state.lock() {
                s.settings.offload_seconds = v.clamp(5, 300) as u32;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_start_minimized(move |on| {
            if let Ok(mut s) = state.lock() {
                s.settings.startup_background = on;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_lrc_enabled(move |on| {
            if let Ok(mut s) = state.lock() {
                s.settings.lrc_enabled = on;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_ctrl_space_enabled(move |on| {
            if let Ok(mut s) = state.lock() {
                s.settings.ctrl_space_enabled = on;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_ctrl_space_mode(move |v| {
            if let Ok(mut s) = state.lock() {
                s.settings.ctrl_space_mode = v.clamp(0, 1) as u32;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_ctrl_space_output(move |v| {
            if let Ok(mut s) = state.lock() {
                s.settings.ctrl_space_output = v.clamp(0, 2) as u32;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_show_waveform(move |on| {
            if let Ok(mut s) = state.lock() {
                s.settings.show_waveform = on;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_waveform_sensitivity(move |v| {
            if let Ok(mut s) = state.lock() {
                s.settings.waveform_sensitivity = v.clamp(1, 10) as u32;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_tray_icon_size(move |v| {
            if let Ok(mut s) = state.lock() {
                s.settings.tray_icon_size = v.clamp(16, 64) as u32;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        let wh = wakeword_handle.clone();
        dashboard.on_toggle_wakeword_phrase(move |phrase: slint::SharedString, on: bool| {
            if let Some(ref w) = wh {
                w.set_phrase_enabled(phrase.to_string(), on);
            }
            // Only community heads exist in the pipeline; legacy custom
            // phrases are quarantined (never scored), so only these persist.
            if let Ok(mut s) = state.lock() {
                match phrase.as_str() {
                    "hey jarvis" => s.settings.ww_hey_jarvis = on,
                    "alexa" => s.settings.ww_alexa = on,
                    _ => {}
                }
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        let wh = wakeword_handle.clone();
        dashboard.on_set_transient_action(move |v| {
            let action = v.clamp(0, 2) as u32;
            if let Ok(mut s) = state.lock() {
                // ONE clap setting — legacy mirrors kept in sync so nothing
                // can ever observe them disagreeing.
                s.settings.transient_action = action;
                s.settings.clap_action = action;
                s.settings.snap_action = action;
                let _ = s.settings.save_all();
            }
            if let Some(ref w) = wh {
                w.set_transient_action(action);
            }
        });
    }
    {
        let tx = tx_cmd.clone();
        dashboard.on_download_catalog_model(move |idx: i32| {
            if idx >= 0 {
                let _ = tx.try_send(OrchestratorCommand::DownloadModel(idx as usize));
            }
        });
    }
    {
        dashboard.on_uninstall_catalog_model(move |_idx: i32| {
            log_line("dashboard: uninstall model requested");
        });
    }
    {
        let tx = tx_cmd.clone();
        dashboard.on_select_catalog_model(move |idx: i32| {
            if idx >= 0 {
                let _ = tx.try_send(OrchestratorCommand::SelectModel(idx as usize));
            }
        });
    }
    {
        let state = state.clone();
        let tx = tx_cmd.clone();
        dashboard.on_use_recommended_model(move || {
            let idx = state.lock().map(|s| s.recommended_model).unwrap_or(None);
            if let Some(i) = idx {
                let _ = tx.try_send(OrchestratorCommand::SelectModel(i));
                log_line(&format!("dashboard: use recommended model {i}"));
            } else {
                log_line("dashboard: no recommended model");
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_catalog_search(move |t: slint::SharedString| {
            if let Ok(mut s) = state.lock() {
                s.catalog_search = t.to_string();
            }
        });
    }
    {
        let state = state.clone();
        dashboard.on_set_catalog_lang_filter(move |v: slint::SharedString| {
            if let Ok(mut s) = state.lock() {
                s.catalog_lang_filter = v.to_string();
            }
        });
    }
    {
        dashboard.on_browse_recording_dir(move || {
            log_line("dashboard: browse recording dir requested");
        });
    }
    {
        dashboard.on_check_updates(move || {
            log_line("dashboard: check updates requested");
        });
    }
    {
        let pill_w = pill.as_weak();
        let state = state.clone();
        dashboard.on_set_ui_opacity(move |v| {
            if let Some(p) = pill_w.upgrade() {
                p.set_pill_opacity((v as f32 / 100.0).clamp(0.2, 1.0));
            }
            if let Ok(mut s) = state.lock() {
                s.settings.active_opacity = v.clamp(20, 100) as u32;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let pill_w = pill.as_weak();
        let state = state.clone();
        dashboard.on_set_ui_pill_radius(move |v| {
            if let Some(p) = pill_w.upgrade() {
                p.set_pill_radius(v as f32);
            }
            if let Ok(mut s) = state.lock() {
                s.settings.pill_radius = v.clamp(0, 25) as u32;
                let _ = s.settings.save_all();
            }
        });
    }
    {
        let state = state.clone();
        let dash_w = dashboard.as_weak();
        dashboard.on_set_start_with_windows(move |on| {
            let ok = autostart::set_autostart_enabled(on).is_ok();
            if let Ok(mut s) = state.lock() {
                s.settings.startup_enabled = on && ok;
                let _ = s.settings.save_all();
            }
            if !ok {
                if let Some(d) = dash_w.upgrade() {
                    d.set_start_with_windows(autostart::is_autostart_enabled());
                }
            }
        });
    }

    // Apply persisted style + dashboard initial values.
    {
        if let Ok(mut s) = state.lock() {
            // Clap transient: respect the persisted action (dashboard unlocked).
            // Wakewords self-heal ON below; claps stay exactly as the user set.
            let transient_action = s.settings.transient_action.min(2);
            s.settings.transient_action = transient_action;
            s.settings.clap_action = transient_action;
            s.settings.snap_action = transient_action;
            // Cement wakewords ON by default for every future update:
            // fresh installs already default to Always On + both phrases;
            // existing installs that drifted Off (e.g. during the broken-head
            // era) self-heal once here, then the user's own toggles persist.
            if s.settings.wake_word_mode.eq_ignore_ascii_case("Off")
                || s.settings.wake_word_mode.trim().is_empty()
            {
                s.settings.wake_word_mode = "Always On".to_string();
                s.wakeword_active = true;
            }
            if !s.settings.ww_hey_jarvis && !s.settings.ww_alexa {
                s.settings.ww_hey_jarvis = true;
                s.settings.ww_alexa = true;
            }
            let _ = s.settings.save_all();
            let ww_enabled = s.wakeword_active
                || (!s.settings.wake_word_mode.eq_ignore_ascii_case("Off")
                    && !s.settings.wake_word_mode.trim().is_empty());
            if let Some(ref wh) = wakeword_handle {
                wh.set_sensitivity(s.settings.wakeword_sensitivity);
                wh.set_vad_sensitivity(s.settings.vad_sensitivity);
                // Precise community heads only (legacy customs quarantined:
                // proven non-discriminating, never enter the pipeline).
                // Respect the persisted per-phrase toggles.
                wh.set_phrase_enabled("hey jarvis".to_string(), s.settings.ww_hey_jarvis);
                wh.set_phrase_enabled("alexa".to_string(), s.settings.ww_alexa);
                wh.set_transient_action(s.settings.transient_action);
                // Background mic runs whenever wakewords OR the clap are
                // armed (claps work with wakewords fully off).
                let transient_armed = s.settings.transient_action != 2;
                if ww_enabled || transient_armed {
                    wh.start();
                }
            }
            pill.set_pill_opacity((s.settings.active_opacity as f32 / 100.0).clamp(0.2, 1.0));
            pill.set_pill_radius(s.settings.pill_radius as f32);
            dashboard.set_ui_opacity(s.settings.active_opacity.clamp(20, 100) as i32);
            dashboard.set_ui_pill_radius(s.settings.pill_radius.clamp(0, 25) as i32);
            dashboard.set_wakeword_enabled(ww_enabled);
            dashboard.set_wakeword_sensitivity(s.settings.wakeword_sensitivity.clamp(0, 100) as i32);
            dashboard.set_vad_sensitivity(s.settings.vad_sensitivity.clamp(0, 100) as i32);
            dashboard.set_start_with_windows(autostart::is_autostart_enabled());
            dashboard.set_start_minimized(s.settings.startup_background);
            dashboard.set_lrc_enabled(s.settings.lrc_enabled);
            dashboard.set_auto_stop_silence(s.settings.auto_stop_silence);
            dashboard.set_silence_duration(s.settings.silence_duration as i32);
            dashboard.set_auto_offload(s.settings.auto_offload);
            dashboard.set_offload_seconds(s.settings.offload_seconds as i32);
            dashboard.set_ctrl_space_enabled(s.settings.ctrl_space_enabled);
            dashboard.set_ctrl_space_mode(s.settings.ctrl_space_mode as i32);
            dashboard.set_ctrl_space_output(s.settings.ctrl_space_output as i32);
            dashboard.set_show_waveform(s.settings.show_waveform);
            dashboard.set_waveform_sensitivity(s.settings.waveform_sensitivity as i32);
            dashboard.set_tray_icon_size(s.settings.tray_icon_size as i32);
            dashboard.set_ww_hey_jarvis(s.settings.ww_hey_jarvis);
            dashboard.set_ww_alexa(s.settings.ww_alexa);
            dashboard.set_transient_action(s.settings.transient_action.min(2) as i32);
            if !s.settings.recording_dir.is_empty() {
                dashboard.set_recording_dir(s.settings.recording_dir.clone().into());
            }
        }
    }

    // Full event loop — NOT pill.run(): run() returns when its window
    // hides, which made Show/Hide quit the whole app (tray and all).
    // run_event_loop lives until quit_event_loop (see quit_app) no matter
    // how many windows hide — hiding the pill only hides the pill.
    if let Err(e) = slint::run_event_loop() {
        log_line(&format!("event loop ended with error: {e}"));
    }
    log_line("EVENT LOOP RETURNED — main exiting");
    Ok(())
}

// ── Headless pixel probe (tests only): renders PillWidget with the software
// renderer into a buffer — no window, no cursor, no input — so geometry and
// centering can be measured in whole pixels instead of eyeballed.
#[cfg(test)]
mod pixel_probe {
    use slint::platform::software_renderer::{
        MinimalSoftwareWindow, RepaintBufferType,
    };
    use slint::platform::{Platform, WindowAdapter};
    use slint::Rgb8Pixel;
    use std::rc::Rc;

    struct Headless {
        window: Rc<MinimalSoftwareWindow>,
    }
    impl Platform for Headless {
        fn create_window_adapter(
            &self,
        ) -> Result<Rc<dyn WindowAdapter>, slint::PlatformError> {
            Ok(self.window.clone())
        }
        fn run_event_loop(&self) -> Result<(), slint::PlatformError> {
            Ok(())
        }
    }

    fn render_idle_hint(upper_w: i32) -> (Vec<Rgb8Pixel>, i32, i32) {
        let window = MinimalSoftwareWindow::new(RepaintBufferType::NewBuffer);
        slint::platform::set_platform(Box::new(Headless {
            window: window.clone(),
        }))
        .unwrap();
        let pill = crate::PillWidget::new().unwrap();
        // Expanded bottom idle, mirroring runtime (win 240x86, pill 194x34).
        pill.set_win_w(240.0);
        pill.set_win_h(86.0);
        pill.set_expanded(true);
        pill.set_vertical(false);
        pill.set_mirror(false);
        pill.set_phase(0);
        pill.set_upper_w(upper_w);
        pill.set_mic_grow(1.0);
        pill.set_upper_op(1.0);
        pill.set_slide_px(0);
        pill.set_rise_px(0);
        pill.set_pill_opacity(1.0);
        pill.set_live_text("".into());
        pill.set_alert_text("".into());
        window.set_size(slint::PhysicalSize::new(240, 86));
        let mut buf = vec![Rgb8Pixel::new(0, 0, 0); 240 * 86];
        window.draw_if_needed(|renderer| {
            renderer.render(&mut buf, 240);
        });
        (buf, 240, 86)
    }

    fn save_bmp(path: &std::path::Path, buf: &[Rgb8Pixel], w: i32, h: i32) {
        let mut px = Vec::with_capacity((w * h * 4) as usize);
        for p in buf {
            px.push(p.b);
            px.push(p.g);
            px.push(p.r);
            px.push(0);
        }
        let mut hdr = [0u8; 54];
        hdr[0] = b'B';
        hdr[1] = b'M';
        hdr[2..6].copy_from_slice(&((54 + px.len()) as u32).to_le_bytes());
        hdr[10] = 54;
        hdr[14] = 40;
        hdr[18..22].copy_from_slice(&(w as u32).to_le_bytes());
        // Negative height = top-down (buffer order).
        hdr[22..26].copy_from_slice(&((-(h as i32)) as u32).to_le_bytes());
        hdr[26] = 1;
        hdr[28] = 32;
        std::fs::write(path, [hdr.to_vec(), px].concat()).unwrap();
    }

    #[test]
    fn probe_idle_hint_centering() {
        let upper_w = 194;
        let (buf, w, _h) = render_idle_hint(upper_w);
        let path = std::env::temp_dir().join("pill-probe.bmp");
        save_bmp(&path, &buf, w, 86);
        // Pill rect from the same formulas as pill.slint (bottom dock):
        // x=(240-uw)/2, y=13, w=uw, h=34. Scan the interior (2px ring
        // excluded so the 1px border can't pollute text extents).
        let (px0, py0) = ((240 - upper_w) / 2, 13);
        let (mut l, mut r, mut t, mut b) = (w, 0, 86, 0);
        // Rows clamped to the status band (13..47): the white mic icon in
        // the capsule below must not pollute the text extents.
        for y in 13..47 {
            for x in (px0 + 2)..(px0 + upper_w - 2) {
                let p = &buf[(y * w + x) as usize];
                if p.r.max(p.g).max(p.b) > 100 {
                    l = l.min(x);
                    r = r.max(x);
                    t = t.min(y);
                    b = b.max(y);
                }
            }
        }
        println!("probe bmp: {}", path.display());
        println!("ink cols {l}..{r} rows {t}..{b}");
        println!(
            "h margins: left={} right={}",
            l - px0,
            px0 + upper_w - 1 - r
        );
        println!(
            "v margins: top={} bottom={}",
            t - py0,
            py0 + 34 - 1 - b
        );
    }
}
