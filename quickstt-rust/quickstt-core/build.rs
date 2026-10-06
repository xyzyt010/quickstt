// Mirror livekit-wakeword: tract backend everywhere except aarch64 Windows.
fn main() {
    println!("cargo::rustc-check-cfg=cfg(use_ort_tract)");
    let target = std::env::var("TARGET").unwrap_or_default();
    if target == "aarch64-pc-windows-msvc" {
        return;
    }
    println!("cargo::rustc-cfg=use_ort_tract");
}
