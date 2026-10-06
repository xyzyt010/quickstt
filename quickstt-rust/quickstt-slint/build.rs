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
        res.compile().unwrap();
    }
}
