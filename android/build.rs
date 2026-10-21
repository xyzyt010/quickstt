fn main() {
    slint_build::compile("ui/android.slint").expect("Slint compile failed");

    // The microphone capture shim wraps the NDK's AAudio C API (cpal has no
    // Android backend). It is only compiled for Android targets — the desktop
    // preview build has no NDK and no AAudio.
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "android" {
        ndk_toolchain_on_path();
        cc::Build::new()
            .file("src/aaudio.c")
            .flag_if_supported("-O2")
            .warnings(false)
            .compile("qs_aaudio");
        println!("cargo:rustc-link-lib=aaudio");
        println!("cargo:rerun-if-changed=src/aaudio.c");
    }
}

/// Puts the NDK's LLVM toolchain on PATH.
///
/// The `cc` crate compiles the C shim with the NDK's clang, but it only
/// looks the compiler up on PATH: it runs `aarch64-linux-android21-clang`
/// and, when that is nowhere to be found, falls back to the plain name
/// `aarch64-linux-android-clang` — which the NDK does not ship. The build
/// then dies with
///
///   error occurred in cc-rs: failed to find tool
///   "aarch64-linux-android-clang": No such file or directory
///
/// The NDK is not on PATH by default (neither on a dev machine nor on CI),
/// so it is located here through the usual environment variables and its
/// `bin` directory is prepended to PATH. cargo-apk sets `CC` itself, which
/// takes precedence over this lookup, so this only fills the gap.
fn ndk_toolchain_on_path() {
    let ndk = ["ANDROID_NDK_ROOT", "ANDROID_NDK_HOME", "NDK_HOME"]
        .iter()
        .filter_map(|key| std::env::var(key).ok())
        .find(|path| !path.is_empty() && std::path::Path::new(path).is_dir());
    let Some(ndk) = ndk else { return };

    let host = if cfg!(target_os = "windows") {
        "windows-x86_64"
    } else if cfg!(target_os = "macos") {
        "darwin-x86_64"
    } else {
        "linux-x86_64"
    };
    let bin = std::path::Path::new(&ndk)
        .join("toolchains")
        .join("llvm")
        .join("prebuilt")
        .join(host)
        .join("bin");
    if !bin.is_dir() {
        return;
    }

    let path = std::env::var_os("PATH").unwrap_or_default();
    let mut paths = std::env::split_paths(&path).collect::<Vec<_>>();
    if !paths.iter().any(|p| p == &bin) {
        paths.insert(0, bin.clone());
        if let Ok(joined) = std::env::join_paths(paths) {
            std::env::set_var("PATH", joined);
        }
    }
}
