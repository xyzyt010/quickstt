//! Linux autostart via XDG .desktop (mirrors Windows Registry Run key)
//! Also provides unified startup_enabled check for Windows/Linux

use anyhow::Result;
use std::path::PathBuf;

fn autostart_desktop_path() -> Option<PathBuf> {
    let config_dir = dirs::config_dir()?;
    Some(config_dir.join("autostart").join("quickstt.desktop"))
}

fn desktop_entry_content(exec_path: &str) -> String {
    format!(
        "[Desktop Entry]\nType=Application\nName=QuickSTT\nComment=Voice typing widget\nExec={} --minimized\nIcon=quickstt\nTerminal=false\nCategories=Utility;\nX-GNOME-Autostart-enabled=true\n",
        exec_path
    )
}

/// Enable or disable autostart. On Windows it writes Registry; on Linux creates/removes .desktop
pub fn set_autostart_enabled(enabled: bool) -> Result<()> {
    #[cfg(target_os = "windows")]
    {
        use windows::core::PCWSTR;
        use windows::Win32::System::Registry::*;
        let subkey: Vec<u16> = "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
            .encode_utf16().chain(std::iter::once(0)).collect();
        unsafe {
            let mut hkey = HKEY::default();
            RegCreateKeyExW(
                HKEY_CURRENT_USER, PCWSTR(subkey.as_ptr()), 0, None,
                REG_OPTION_NON_VOLATILE, KEY_WRITE, None, &mut hkey, None,
            )?;
            if enabled {
                let exe = std::env::current_exe()?.to_string_lossy().to_string();
                let wide_name: Vec<u16> = "QuickSTT".encode_utf16().chain(std::iter::once(0)).collect();
                let wide_val: Vec<u16> = format!("\"{}\" --minimized", exe).encode_utf16().chain(std::iter::once(0)).collect();
                let data = std::slice::from_raw_parts(wide_val.as_ptr() as *const u8, wide_val.len()*2);
                RegSetValueExW(hkey, PCWSTR(wide_name.as_ptr()), 0, REG_SZ, Some(data))?;
            } else {
                let wide_name: Vec<u16> = "QuickSTT".encode_utf16().chain(std::iter::once(0)).collect();
                let _ = RegDeleteValueW(hkey, PCWSTR(wide_name.as_ptr()));
            }
            let _ = RegCloseKey(hkey);
        }
        return Ok(());
    }
    #[cfg(not(target_os = "windows"))]
    {
        let path = autostart_desktop_path().ok_or_else(|| anyhow::anyhow!("no config dir"))?;
        if enabled {
            if let Some(parent) = path.parent() { std::fs::create_dir_all(parent)?; }
            let exe = std::env::current_exe().map(|p| p.to_string_lossy().to_string()).unwrap_or_else(|_| "/usr/bin/quickstt".to_string());
            std::fs::write(&path, desktop_entry_content(&exe))?;
        } else if path.exists() {
            std::fs::remove_file(&path)?;
        }
        Ok(())
    }
}

pub fn is_autostart_enabled() -> bool {
    #[cfg(target_os = "windows")]
    {
        use windows::core::PCWSTR;
        use windows::Win32::System::Registry::*;
        let subkey: Vec<u16> = "Software\\Microsoft\\Windows\\CurrentVersion\\Run".encode_utf16().chain(std::iter::once(0)).collect();
        unsafe {
            let mut hkey = HKEY::default();
            if RegOpenKeyExW(HKEY_CURRENT_USER, PCWSTR(subkey.as_ptr()), 0, KEY_READ, &mut hkey).is_err() { return false; }
            let name: Vec<u16> = "QuickSTT".encode_utf16().chain(std::iter::once(0)).collect();
            let mut ty = REG_VALUE_TYPE::default();
            let mut sz = 0u32;
            let ok = RegQueryValueExW(hkey, PCWSTR(name.as_ptr()), None, Some(&mut ty), None, Some(&mut sz)).is_ok();
            let _ = RegCloseKey(hkey);
            return ok && sz > 0;
        }
    }
    #[cfg(not(target_os = "windows"))]
    {
        autostart_desktop_path().map(|p| p.exists()).unwrap_or(false)
    }
}
