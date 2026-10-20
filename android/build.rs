fn main() {
    slint_build::compile("ui/android.slint").expect("Slint compile failed");

    // The microphone capture shim wraps the NDK's AAudio C API (cpal has no
    // Android backend). It is only compiled for Android targets — the desktop
    // preview build has no NDK and no AAudio.
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "android" {
        cc::Build::new()
            .file("src/aaudio.c")
            .flag_if_supported("-O2")
            .warnings(false)
            .compile("qs_aaudio");
        println!("cargo:rustc-link-lib=aaudio");
        println!("cargo:rerun-if-changed=src/aaudio.c");
    }
}
