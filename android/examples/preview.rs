//! Desktop preview of the Android UI — same scene, phone-sized window, so the
//! touch layout can be reviewed on Windows/Linux without an emulator.
#![cfg(not(target_os = "android"))]

fn main() {
    quickstt_android::run();
}
