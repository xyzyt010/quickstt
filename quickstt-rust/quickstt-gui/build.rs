fn main() {
    // Embed a Windows manifest for proper UI scaling, theme consistency, and administrator privileges if needed.
    if std::env::var("CARGO_CFG_TARGET_OS").unwrap() == "windows" {
        let mut res = winresource::WindowsResource::new();
        res.set_manifest(include_str!("manifest.xml"));
        if let Err(e) = res.compile() {
            println!("cargo:warning=Failed to compile Windows resources: {}", e);
        }
    }
}
