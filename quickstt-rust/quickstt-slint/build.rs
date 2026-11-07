fn main() {
    slint_build::compile("ui/main.slint").unwrap();
    #[cfg(target_os = "windows")]
    {
        // PerMonitorV2 DPI awareness -> crisp DirectWrite text at 125/150/200%
        let mut res = winresource::WindowsResource::new();
        res.set_manifest(
            r#"
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/PM</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2, PerMonitor</dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
"#,
        );
        // The blue mic app icon, baked into the exe: Explorer, the taskbar,
        // Alt+Tab and every shortcut that points at this binary show the real
        // app icon instead of the default Rust/generic one. Absolute path —
        // winresource resolves icons relative to its own CWD, which is not
        // guaranteed to be the manifest dir.
        let icon = std::fs::canonicalize(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../assets/icon_app.ico"),
        )
        .expect("assets/icon_app.ico must exist");
        res.set_icon(icon.to_string_lossy().as_ref());
        res.compile().unwrap();
    }
}
