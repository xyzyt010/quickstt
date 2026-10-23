// Android platform glue (JNI).
//
// Everything that needs the Java side lives here: the app's files dir, the
// clipboard, the microphone permission, and the floating overlay bubble that
// floats above other apps.
//
// ── Why this file is written the way it is ─────────────────────────────────
// The `ndk-context` crate hands us the **Application** object (that is what
// android-activity 0.6 initialises), and an `Application` has no
// `requestPermissions()`, no `startActivity()` and no `window` system service.
// Calling those on it raises a Java-side `NoSuchMethodError` that the JNI
// layer swallows — so the buttons looked alive but did nothing at all.
//
// The Activity instance comes from `AndroidApp::activity_as_ptr()`, and every
// call the framework insists on running on the UI thread (the permission
// dialog, `startActivity`, `WindowManager.addView`) is dispatched with
// `run_on_java_main_thread`.

use std::path::PathBuf;
#[cfg(target_os = "android")]
use std::sync::Mutex;

#[cfg(target_os = "android")]
use jni::objects::{JClass, JObject, JString, JValue};
#[cfg(target_os = "android")]
use jni::sys::{jint, jobject};
#[cfg(target_os = "android")]
use jni::JavaVM;
#[cfg(target_os = "android")]
use slint::android::AndroidApp;

/// True when the app's own window has the focus (i.e. QuickSTT is the app in
/// front). When it does not, another app is focused and that is where a
/// transcript can be typed.
pub fn has_window_focus() -> bool {
    #[cfg(target_os = "android")]
    {
        with_activity(|env, activity| {
            let res = env
                .call_method(activity, "hasWindowFocus", "()Z", &[])
                .ok()?
                .z()
                .ok()?;
            Some(res)
        })
        .unwrap_or(false)
    }
    #[cfg(not(target_os = "android"))]
    false
}

/// Type text into the app that currently has the focus.
///
/// This is the lowest level input API an app can use: real `KeyEvent`s
/// (built by `KeyCharacterMap.getEvents`, so capitals and punctuation come
/// out right) are pushed into the input stack with
/// `InputManager.injectInputEvent` — the same mechanism the system uses for
/// hardware keyboards.
///
/// It needs the `INJECT_EVENTS` permission, which is a system permission: it
/// only works on a rooted phone, on a userdebug/eng build, or when the appop
/// is enabled with
/// `adb shell appops set ai.quickstt.app android:inject_event allow`.
/// When it is not allowed the call fails and the caller falls back to the
/// clipboard.
#[cfg(target_os = "android")]
pub fn inject_text(text: &str) -> Result<(), String> {
    if text.trim().is_empty() {
        return Ok(());
    }
    let app = app().ok_or("the app is not running")?;
    let vm = unsafe { JavaVM::from_raw(app.vm_as_ptr().cast()) }.map_err(|e| e.to_string())?;
    let mut env = vm.attach_current_thread().map_err(|e| e.to_string())?;
    let activity = unsafe { JObject::from_raw(app.activity_as_ptr() as jobject) };

    let result = (|| -> Result<(), String> {
        // The input manager: it is a hidden class, but the system hands it
        // out through the regular getSystemService() call.
        let service = env
            .new_string("input")
            .map_err(|e| e.to_string())?;
        let manager = env
            .call_method(
                &activity,
                "getSystemService",
                "(Ljava/lang/String;)Ljava/lang/Object;",
                &[JValue::from(&service)],
            )
            .map_err(|e| e.to_string())?
            .l()
            .map_err(|e| e.to_string())?;

        // The virtual keyboard's character map: it turns a character into the
        // key events (with the right shift/meta state) a real keyboard sends.
        let kcm_cls = env
            .find_class("android/view/KeyCharacterMap")
            .map_err(|e| e.to_string())?;
        let keymap = env
            .call_static_method(
                kcm_cls,
                "load",
                "(I)Landroid/view/KeyCharacterMap;",
                &[JValue::from(-1i32)],
            )
            .map_err(|e| e.to_string())?
            .l()
            .map_err(|e| e.to_string())?;

        for ch in text.chars() {
            // UTF-16 units (a surrogate pair is two units).
            let mut units = [0u16; 2];
            let units = ch.encode_utf16(&mut units).to_vec();
            let array = env
                .new_char_array(units.len() as i32)
                .map_err(|e| e.to_string())?;
            env.set_char_array_region(&array, 0, &units)
                .map_err(|e| e.to_string())?;
            let events = env
                .call_method(
                    &keymap,
                    "getEvents",
                    "([C)[Landroid/view/KeyEvent;",
                    &[JValue::from(&array)],
                )
                .map_err(|e| e.to_string())?
                .l()
                .map_err(|e| e.to_string())?;
            let events = jni::objects::JObjectArray::from(events);
            let len = env.get_array_length(&events).map_err(|e| e.to_string())?;
            for index in 0..len {
                let event = env
                    .get_object_array_element(&events, index)
                    .map_err(|e| e.to_string())?;
                env.call_method(
                    &manager,
                    "injectInputEvent",
                    "(Landroid/view/InputEvent;I)V",
                    &[JValue::from(&event), JValue::from(INJECT_MODE_WAIT_FOR_FINISH)],
                )
                .map_err(|e| e.to_string())?;
            }
        }
        Ok(())
    })();

    // A refused injection leaves a Java exception pending; clear it so the
    // thread stays usable and report what happened.
    if env.exception_check().unwrap_or(false) {
        let _ = env.exception_clear();
        return Err("Android refused the input (the INJECT_EVENTS permission is missing)".into());
    }
    std::mem::forget(activity);
    result.map_err(|e| e)
}

/// Desktop preview: there is no input stack to type into.
#[cfg(not(target_os = "android"))]
pub fn inject_text(_text: &str) -> Result<(), String> {
    Err("typing into other apps is only possible on Android".into())
}

/// Offer the text to other apps through the system share sheet. This always
/// works and needs no permission — it is the fallback when direct key
/// injection is not allowed.
pub fn share_text(text: &str) -> bool {
    #[cfg(target_os = "android")]
    {
        let ok = with_activity(|env, activity| {
            let intent_cls = env.find_class("android/content/Intent").ok()?;
            let intent = env.new_object(&intent_cls, "()V", &[]).ok()?;
            let action = env.new_string("android.intent.action.SEND").ok()?;
            env.call_method(
                &intent,
                "setAction",
                "(Ljava/lang/String;)Landroid/content/Intent;",
                &[JValue::from(&action)],
            )
            .ok()?;
            let mime = env.new_string("text/plain").ok()?;
            env.call_method(
                &intent,
                "setType",
                "(Ljava/lang/String;)Landroid/content/Intent;",
                &[JValue::from(&mime)],
            )
            .ok()?;
            let body = env.new_string(text).ok()?;
            env.call_method(
                &intent,
                "putExtra",
                "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
                &[JValue::from(&env.new_string("android.intent.extra.TEXT").ok()?), JValue::from(&body)],
            )
            .ok()?;
            let chooser = env
                .call_static_method(
                    &intent_cls,
                    "createChooser",
                    "(Landroid/content/Intent;Ljava/lang/CharSequence;)Landroid/content/Intent;",
                    &[JValue::from(&intent), JValue::from(&env.new_string("Send with").ok()?)],
                )
                .ok()?
                .l()
                .ok()?;
            env.call_method(
                activity,
                "startActivity",
                "(Landroid/content/Intent;)V",
                &[JValue::from(&chooser)],
            )
            .ok()?;
            Some(true)
        })
        .unwrap_or(false);
        if !ok {
            log("could not open the share sheet");
        }
        ok
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = text;
        false
    }
}

/// The overlay view + its layout params, kept alive between updates.
#[cfg(target_os = "android")]
struct OverlayHandle {
    view: jni::objects::GlobalRef,
    params: jni::objects::GlobalRef,
}

/// `InputManager.INJECT_INPUT_EVENT_MODE_WAIT_FOR_FINISH` — the injected event
/// is fully processed before the call returns.
#[cfg(target_os = "android")]
const INJECT_MODE_WAIT_FOR_FINISH: i32 = 2;

/// The Android app handle. It owns the Activity instance and the Java UI
/// thread, both of which we need.
#[cfg(target_os = "android")]
static APP: Mutex<Option<AndroidApp>> = Mutex::new(None);

/// The overlay bubble, kept alive between updates.
#[cfg(target_os = "android")]
static OVERLAY: Mutex<Option<OverlayHandle>> = Mutex::new(None);

/// The last problem with the floating bubble.
///
/// The overlay is built on the Java UI thread, so the caller cannot get the
/// result directly; the error is parked here and the UI picks it up.
#[cfg(target_os = "android")]
static OVERLAY_ERROR: Mutex<Option<String>> = Mutex::new(None);

/// Remember (and log) why the bubble could not be shown.
#[cfg(target_os = "android")]
fn set_overlay_error(message: &str) {
    log(message);
    if let Ok(mut slot) = OVERLAY_ERROR.lock() {
        *slot = Some(message.to_string());
    }
}

/// Take the last bubble error (the UI shows it to the user).
pub fn take_overlay_error() -> Option<String> {
    #[cfg(target_os = "android")]
    {
        OVERLAY_ERROR.lock().ok().and_then(|mut slot| slot.take())
    }
    #[cfg(not(target_os = "android"))]
    {
        None
    }
}

/// Remember whether the floating bubble should be on.
pub fn save_overlay_enabled(enabled: bool) {
    #[cfg(target_os = "android")]
    {
        if let Some(dir) = files_dir() {
            let dir = dir.join("quickstt");
            let _ = std::fs::create_dir_all(&dir);
            let _ = std::fs::write(
                dir.join("bubble"),
                if enabled { "1" } else { "0" },
            );
        }
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = enabled;
    }
}

/// Should the floating bubble be on?
///
/// This is the *wish*: it is remembered even when the permission is still
/// missing, so the bubble can appear by itself as soon as the user grants
/// the permission in the system settings.
pub fn overlay_enabled() -> bool {
    #[cfg(target_os = "android")]
    {
        files_dir()
            .map(|dir| dir.join("quickstt").join("bubble"))
            .and_then(|p| std::fs::read_to_string(p).ok())
            .map(|s| s.trim() == "1")
            .unwrap_or(false)
    }
    #[cfg(not(target_os = "android"))]
    false
}

/// Print to logcat (stdout/stderr are forwarded to logcat by android-activity).
#[cfg(target_os = "android")]
pub fn log(message: &str) {
    eprintln!("QuickSTT: {message}");
}

/// Desktop preview: the same messages on the terminal.
#[cfg(not(target_os = "android"))]
pub fn log(message: &str) {
    eprintln!("QuickSTT: {message}");
}

/// Remember the app handle so the Activity can be reached later.
#[cfg(target_os = "android")]
pub fn init(app: AndroidApp) {
    let mut slot = APP.lock().unwrap();
    *slot = Some(app);
}

/// The stored app handle (Android only).
#[cfg(target_os = "android")]
fn app() -> Option<AndroidApp> {
    APP.lock().unwrap().clone()
}

/// Run `f` on the Java UI thread (the thread Android requires for dialogs,
/// `startActivity` and window-manager calls).
#[cfg(target_os = "android")]
pub fn on_java_thread<F>(f: F)
where
    F: FnOnce() + Send + 'static,
{
    match app() {
        Some(app) => app.run_on_java_main_thread(Box::new(f)),
        None => eprintln!("QuickSTT: no Android app handle — call platform::init() first"),
    }
}

/// Attach the current thread to the JVM and run `f` with the Activity
/// context. Returns None when anything is unavailable (desktop build, or a
/// JNI error).
#[cfg(target_os = "android")]
fn with_activity<T, F>(f: F) -> Option<T>
where
    F: FnOnce(&mut jni::JNIEnv, &JObject) -> Option<T>,
{
    let app = app()?;
    let vm = unsafe { JavaVM::from_raw(app.vm_as_ptr().cast()) }.ok()?;
    let mut env = vm.attach_current_thread().ok()?;
    // The Activity instance (a JNI global reference we do not own).
    let activity = unsafe { JObject::from_raw(app.activity_as_ptr() as jobject) };
    let out = f(&mut env, &activity);
    // A failed Java call leaves a pending exception on this thread. Clear it,
    // otherwise every later JNI call on that thread fails with
    // "JNI DETECTED ERROR IN APPLICATION: pending exception". The message is
    // read first and logged: a swallowed `ClassNotFoundException` is exactly
    // what a button that "does nothing" looks like.
    if env.exception_check().unwrap_or(false) {
        let thrown = env.exception_occurred().ok();
        let _ = env.exception_clear();
        let described = thrown
            .and_then(|exception| {
                let text = env
                    .call_method(&exception, "toString", "()Ljava/lang/String;", &[])
                    .ok()?
                    .l()
                    .ok()?;
                env.get_string(&JString::from(text)).ok().map(|s| s.into())
            })
            .unwrap_or_else(|| "unknown Java exception".to_string());
        log(&format!("a Java call failed: {described}"));
    }
    // Never delete a reference we do not own.
    std::mem::forget(activity);
    out
}

/// Find one of the app's own Java classes.
///
/// `FindClass` uses the **system** class loader on a thread that was attached
/// with `AttachCurrentThread`, so the app's own classes (the bubble helper,
/// the downloader, the service) are invisible to it.
///
/// The loader has to be `Context.getClassLoader()` — the loader of the
/// **application package**. `activity.getClass().getClassLoader()` looks
/// equivalent but is not: the activity is `android.app.NativeActivity`,
/// which is a framework class loaded by the **boot** class loader, and that
/// one cannot see a single class of the app. Every helper call then failed
/// with `ClassNotFoundException` while the JNI layer silently cleared it —
/// the downloads never started, the bubble never appeared and the settings
/// page never opened, exactly as if the buttons did nothing.
#[cfg(target_os = "android")]
fn app_class<'local>(
    env: &mut jni::JNIEnv<'local>,
    activity: &JObject<'_>,
    name: &str,
) -> Option<JClass<'local>> {
    let loader = env
        .call_method(activity, "getClassLoader", "()Ljava/lang/ClassLoader;", &[])
        .ok()?
        .l()
        .ok()?;
    let binary_name = name.replace('/', ".");
    let text = env.new_string(&binary_name).ok()?;
    let loaded = env
        .call_method(
            &loader,
            "loadClass",
            "(Ljava/lang/String;)Ljava/lang/Class;",
            &[JValue::from(&text)],
        )
        .ok()?
        .l()
        .ok()?;
    // A `java.lang.Class` *is* a jclass.
    Some(JClass::from(loaded))
}

/// Call a static method of one of the app's own Java classes.
///
/// Returns None when the class or the method is not there.
#[cfg(target_os = "android")]
fn call_app_static<'local>(
    env: &mut jni::JNIEnv<'local>,
    activity: &JObject<'_>,
    class_name: &str,
    method: &str,
    sig: &str,
    args: &[JValue],
) -> Option<jni::objects::JValueOwned<'local>> {
    let cls = app_class(env, activity, class_name)?;
    env.call_static_method(cls, method, sig, args).ok()
}

/// Read a Java string out of a JNI value.
#[cfg(target_os = "android")]
fn jni_string(env: &mut jni::JNIEnv<'_>, value: jni::objects::JValueOwned<'_>) -> Option<String> {
    let object = value.l().ok()?;
    let text: String = env.get_string(&JString::from(object)).ok()?.into();
    Some(text)
}

/// The app's private files directory (persists across launches).
pub fn files_dir() -> Option<PathBuf> {
    #[cfg(target_os = "android")]
    {
        with_activity(|env, activity| {
            let dir = env
                .call_method(activity, "getFilesDir", "()Ljava/io/File;", &[])
                .ok()?
                .l()
                .ok()?;
            let path = env
                .call_method(&dir, "getAbsolutePath", "()Ljava/lang/String;", &[])
                .ok()?
                .l()
                .ok()?;
            let s: String = env.get_string(&JString::from(path)).ok()?.into();
            Some(PathBuf::from(s))
        })
    }
    #[cfg(not(target_os = "android"))]
    None
}

/// The app's native library directory.
///
/// This is where an executable has to live: since Android 10 the app's home
/// directory is mounted **noexec** for apps that target API 29 or newer, so
/// a binary unpacked into the files directory cannot be started
/// ("Permission denied"). The native library directory belongs to the app,
/// is writable by it and is exec-mounted.
pub fn native_lib_dir() -> Option<PathBuf> {
    #[cfg(target_os = "android")]
    {
        with_activity(|env, activity| {
            let dir = env
                .call_method(activity, "getNativeLibraryDir", "()Ljava/lang/String;", &[])
                .ok()?
                .l()
                .ok()?;
            let path: String = env.get_string(&JString::from(dir)).ok()?.into();
            Some(PathBuf::from(path))
        })
    }
    #[cfg(not(target_os = "android"))]
    None
}

/// The system linker (`/system/bin/linker64`).
///
/// Starting a program through the linker works around the SELinux rule that
/// stops an app from executing a file out of its own (writable) home
/// directory: the linker maps the program instead of the kernel executing
/// it. It is the fallback for the engines when the installer did not extract
/// them into the native library directory.
pub fn linker64() -> Option<PathBuf> {
    #[cfg(target_os = "android")]
    {
        let path = PathBuf::from("/system/bin/linker64");
        if path.exists() {
            return Some(path);
        }
    }
    None
}

/// Copy a file out of the APK into the app's data directory.
///
/// The engines are shipped in the APK's `lib/<abi>` directory. The Java
/// helper reads the entry out of the APK (Java can read a zip entry without
/// any help), so the app can still get the engine if the installer did not
/// extract it.
pub fn stage_from_apk(entry: &str) -> Option<PathBuf> {
    #[cfg(target_os = "android")]
    {
        let path = with_activity(|env, activity| {
            let cls = app_class(env, activity, "ai/quickstt/app/Engine")?;
            let name = env.new_string(entry).ok()?;
            let result = env
                .call_static_method(
                    cls,
                    "extract",
                    "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;",
                    &[JValue::from(activity), JValue::from(&name)],
                )
                .ok()?
                .l()
                .ok()?;
            let text: String = env.get_string(&JString::from(result)).ok()?.into();
            Some(text)
        })?;
        if path.is_empty() {
            return None;
        }
        let path = PathBuf::from(path);
        if path.exists() {
            Some(path)
        } else {
            None
        }
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = entry;
        None
    }
}

/// Copy text to the system clipboard (so it can be pasted into any app).
pub fn copy_to_clipboard(text: &str) -> bool {
    #[cfg(target_os = "android")]
    {
        let ok = with_activity(|env, activity| {
            let body = env.new_string(text).ok()?;
            let value = call_app_static(
                env,
                activity,
                "ai/quickstt/app/Overlay",
                "copyToClipboard",
                "(Landroid/content/Context;Ljava/lang/String;)Z",
                &[JValue::from(activity), JValue::from(&body)],
            )?;
            value.z().ok()
        })
        .unwrap_or(false);
        if !ok {
            log("could not copy to the clipboard");
        }
        ok
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = text;
        false
    }
}

/// True when the microphone permission is already granted.
pub fn has_mic_permission() -> bool {
    #[cfg(target_os = "android")]
    {
        with_activity(|env, activity| {
            let perm = env.new_string("android.permission.RECORD_AUDIO").ok()?;
            let res: jint = env
                .call_method(
                    activity,
                    "checkSelfPermission",
                    "(Ljava/lang/String;)I",
                    &[JValue::from(&perm)],
                )
                .ok()?
                .i()
                .ok()?;
            Some(res == 0) // PackageManager.PERMISSION_GRANTED
        })
        .unwrap_or(false)
    }
    #[cfg(not(target_os = "android"))]
    false
}

/// Ask the user for the microphone permission (system dialog).
///
/// The dialog must be raised from the Activity **on the UI thread** — that is
/// the only combination Android accepts, and it is also why this used to be a
/// silent no-op.
pub fn request_mic_permission() {
    request_permissions(&["android.permission.RECORD_AUDIO"]);
}

/// Ask the user for a runtime permission (the system dialog).
///
/// The request has to be raised from the Activity on the UI thread: that is
/// the only combination Android accepts.
pub fn request_permissions(permissions: &[&str]) {
    #[cfg(target_os = "android")]
    {
        // The closure has to outlive this call, so the names are copied.
        let names: Vec<String> = permissions.iter().map(|p| p.to_string()).collect();
        log(&format!("asking for {names:?}"));
        on_java_thread(move || {
            let result = with_activity(|env, activity| {
                let string_cls = env.find_class("java/lang/String").ok()?;
                let arr = env
                    .new_object_array(names.len() as i32, &string_cls, JObject::null())
                    .ok()?;
                for (i, permission) in names.iter().enumerate() {
                    let perm = env.new_string(permission).ok()?;
                    env.set_object_array_element(&arr, i as i32, &perm).ok()?;
                }
                env.call_method(
                    activity,
                    "requestPermissions",
                    "([Ljava/lang/String;I)V",
                    &[JValue::from(&arr), JValue::from(1)],
                )
                .ok()?;
                Some(())
            });
            if result.is_none() {
                log("the permission request could not be sent");
            }
        });
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = permissions;
    }
}

/// Start the foreground service that keeps QuickSTT alive in the background.
///
/// Without it Android kills the app as soon as it is not visible — and the
/// floating bubble would go with it. The service shows a small notification
/// that says what it is for.
pub fn start_service() -> Result<(), String> {
    #[cfg(target_os = "android")]
    {
        let outcome = with_activity(|env, activity| {
            call_app_static(
                env,
                activity,
                "ai/quickstt/app/SpeechService",
                "start",
                "(Landroid/content/Context;)V",
                &[JValue::from(activity)],
            )?;
            Some(())
        });
        match outcome {
            Some(()) => {
                log("the background service is running");
                Ok(())
            }
            None => Err("the background service could not be started".into()),
        }
    }
    #[cfg(not(target_os = "android"))]
    {
        Err("the background service only exists on Android".into())
    }
}

/// Stop the background service again.
pub fn stop_service() {
    #[cfg(target_os = "android")]
    {
        let _ = with_activity(|env, activity| {
            call_app_static(
                env,
                activity,
                "ai/quickstt/app/SpeechService",
                "stop",
                "(Landroid/content/Context;)V",
                &[JValue::from(activity)],
            )?;
            Some(())
        });
        log("the background service was stopped");
    }
}

/// Is the app exempt from the battery restrictions?
///
/// Without the exemption Android puts the app to sleep in the background
/// and the bubble stops working.
pub fn ignoring_battery_optimizations() -> bool {
    #[cfg(target_os = "android")]
    {
        with_activity(|env, activity| {
            let value = call_app_static(
                env,
                activity,
                "ai/quickstt/app/Overlay",
                "ignoringBatteryOptimizations",
                "(Landroid/content/Context;)Z",
                &[JValue::from(activity)],
            )?;
            value.z().ok()
        })
        .unwrap_or(false)
    }
    #[cfg(not(target_os = "android"))]
    false
}

/// Ask the system to let QuickSTT run in the background.
///
/// The app's own page first, the list of all apps as the fallback.
pub fn request_battery_exemption() -> Result<(), String> {
    #[cfg(target_os = "android")]
    {
        log("asking for the battery optimisation exemption");
        let outcome = with_activity(|env, activity| {
            let value = call_app_static(
                env,
                activity,
                "ai/quickstt/app/Overlay",
                "requestBatteryExemption",
                "(Landroid/content/Context;)Ljava/lang/String;",
                &[JValue::from(activity)],
            )?;
            jni_string(env, value)
        });
        match outcome {
            Some(message) if message.is_empty() => Ok(()),
            Some(message) => Err(message),
            None => Err("the battery settings could not be opened".into()),
        }
    }
    #[cfg(not(target_os = "android"))]
    {
        Err("the battery settings only exist on Android".into())
    }
}

/// May the app show its notification? (Android 13+ asks for it.)
pub fn can_post_notifications() -> bool {
    #[cfg(target_os = "android")]
    {
        with_activity(|env, activity| {
            let value = call_app_static(
                env,
                activity,
                "ai/quickstt/app/Overlay",
                "canPostNotifications",
                "(Landroid/content/Context;)Z",
                &[JValue::from(activity)],
            )?;
            value.z().ok()
        })
        .unwrap_or(true)
    }
    #[cfg(not(target_os = "android"))]
    true
}

// The progress of the download that is running on this thread.
//
// The Java downloader calls back into Rust on the thread that started it,
// so a thread-local is all that is needed to get the number of bytes that
// really arrived into the app.
#[cfg(target_os = "android")]
thread_local! {
    static DOWNLOAD_PROGRESS: std::cell::RefCell<Option<Box<dyn FnMut(u64, u64)>>> =
        const { std::cell::RefCell::new(None) };
}

/// Called by the Java downloader with the bytes that arrived.
#[cfg(target_os = "android")]
#[no_mangle]
pub extern "C" fn Java_ai_quickstt_app_ModelDownloader_nativeProgress(
    _env: jni::JNIEnv,
    _class: jni::sys::jclass,
    done: jni::sys::jlong,
    total: jni::sys::jlong,
) {
    DOWNLOAD_PROGRESS.with(|slot| {
        let hook = slot.borrow_mut().take();
        if let Some(mut hook) = hook {
            hook(done as u64, total as u64);
            slot.borrow_mut().replace(hook);
        }
    });
}

/// Download a file with the Android HTTP stack.
///
/// The Java helper does the work — it is the HTTP stack the phone trusts —
/// and reports the bytes that arrived. The file is only moved into place
/// when it is complete.
#[cfg(target_os = "android")]
pub fn download_file_with_progress<F: FnMut(u64, u64) + 'static>(
    url: &str,
    dest: &std::path::Path,
    min_bytes: u64,
    progress: F,
) -> Result<(), String> {
    let dest_text = dest.to_string_lossy().to_string();
    DOWNLOAD_PROGRESS.with(|slot| {
        *slot.borrow_mut() = Some(Box::new(progress));
    });
    let outcome = with_activity(|env, activity| {
        let url_text = env.new_string(url).ok()?;
        let dest_string = env.new_string(&dest_text).ok()?;
        let value = call_app_static(
            env,
            activity,
            "ai/quickstt/app/ModelDownloader",
            "download",
            "(Ljava/lang/String;Ljava/lang/String;J)Ljava/lang/String;",
            &[
                JValue::from(&url_text),
                JValue::from(&dest_string),
                JValue::from(min_bytes as i64),
            ],
        )?;
        jni_string(env, value)
    });
    DOWNLOAD_PROGRESS.with(|slot| {
        *slot.borrow_mut() = None;
    });
    match outcome {
        Some(message) if message.is_empty() => Ok(()),
        Some(message) => Err(message),
        None => Err("the download could not be started".into()),
    }
}

/// Download a file (the desktop build has no HTTP stack to speak of).
#[cfg(not(target_os = "android"))]
pub fn download_file_with_progress<F: FnMut(u64, u64) + 'static>(
    _url: &str,
    _dest: &std::path::Path,
    _min_bytes: u64,
    _progress: F,
) -> Result<(), String> {
    Err("model downloads are only available on Android".into())
}


/// True when the app may draw over other apps.
///
/// The check runs through the Java helper (`ai.quickstt.app.Overlay`),
/// because it also knows the app-op that some phone makers use instead of
/// the setting. The framework call is the fallback when the helper is not
/// there.
pub fn can_draw_overlays() -> bool {
    #[cfg(target_os = "android")]
    {
        let via_helper = with_activity(|env, activity| {
            let value = call_app_static(
                env,
                activity,
                "ai/quickstt/app/Overlay",
                "granted",
                "(Landroid/content/Context;)Z",
                &[JValue::from(activity)],
            )?;
            value.z().ok()
        });
        if let Some(granted) = via_helper {
            return granted;
        }
        with_activity(|env, activity| {
            let cls = env.find_class("android/provider/Settings").ok()?;
            let res = env
                .call_static_method(
                    cls,
                    "canDrawOverlays",
                    "(Landroid/content/Context;)Z",
                    &[JValue::from(activity)],
                )
                .ok()?
                .z()
                .ok()?;
            Some(res)
        })
        .unwrap_or(false)
    }
    #[cfg(not(target_os = "android"))]
    false
}

/// Extract a bundled model from the APK assets/zip if present.
pub fn extract_bundled_model(file_name: &str, min_bytes: u64) -> bool {
    #[cfg(target_os = "android")]
    {
        with_activity(|env, activity| {
            let name = env.new_string(file_name).ok()?;
            let value = call_app_static(
                env,
                activity,
                "ai/quickstt/app/ModelDownloader",
                "extractBundledModel",
                "(Landroid/content/Context;Ljava/lang/String;J)Z",
                &[
                    JValue::from(activity),
                    JValue::from(&name),
                    JValue::from(min_bytes as i64),
                ],
            )?;
            value.z().ok()
        })
        .unwrap_or(false)
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = (file_name, min_bytes);
        false
    }
}

/// Open the system page where the user grants "display over other apps".
///
/// The page is QuickSTT's own entry, so the user only has to flip one
/// switch. When the phone does not know that action, the helper falls back
/// to the list of all apps and then to the app-info page.
///
/// Returns the reason when no page could be opened — the app shows it
/// instead of pretending the button worked.
pub fn open_overlay_settings() -> Result<(), String> {
    #[cfg(target_os = "android")]
    {
        log("opening the overlay settings");
        let outcome = with_activity(|env, activity| {
            let value = call_app_static(
                env,
                activity,
                "ai/quickstt/app/Overlay",
                "openSettings",
                "(Landroid/content/Context;)Ljava/lang/String;",
                &[JValue::from(activity)],
            )?;
            jni_string(env, value)
        });
        match outcome {
            Some(message) if message.is_empty() => Ok(()),
            Some(message) => Err(message),
            None => Err("the system settings could not be opened".into()),
        }
    }
    #[cfg(not(target_os = "android"))]
    {
        Err("the overlay settings only exist on Android".into())
    }
}

/// Let the Java helper tell us when the app comes back to the front.
///
/// That is the moment the user returns from the system settings after
/// granting the permission — the app picks it up and shows the bubble
/// without the user having to press anything.
pub fn watch_overlay_permission() {
    #[cfg(target_os = "android")]
    {
        let _ = with_activity(|env, activity| {
            call_app_static(
                env,
                activity,
                "ai/quickstt/app/Overlay",
                "watch",
                "(Landroid/content/Context;)V",
                &[JValue::from(activity)],
            )?;
            Some(())
        });
    }
}

/// Call a static method of the Java bubble helper.
///
/// The helper lives in `java/ai/quickstt/app/Bubble.java` and is compiled
/// into a dex that is added to the APK. Without it the app falls back to a
/// plain read-out bubble.
///
/// `Ok(Ok(()))` means the helper did the job, `Ok(Err(message))` means it
/// refused (the message says why) and `None` means the helper is not there.
#[cfg(target_os = "android")]
fn call_bubble_helper(
    env: &mut jni::JNIEnv<'_>,
    activity: &JObject,
    method: &str,
    state: &str,
    transcript: &str,
) -> Option<Result<(), String>> {
    let state_text = env.new_string(state).ok()?;
    let body = env.new_string(transcript).ok()?;
    let result = call_app_static(
        env,
        activity,
        "ai/quickstt/app/Overlay",
        method,
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        &[
            JValue::from(activity),
            JValue::from(&state_text),
            JValue::from(&body),
        ],
    )?;
    let message = jni_string(env, result)?;
    if message.is_empty() {
        Some(Ok(()))
    } else {
        Some(Err(message))
    }
}

/// The fallback bubble: a TextView on a rounded translucent background.
#[cfg(target_os = "android")]
fn create_plain_bubble<'local>(
    env: &mut jni::JNIEnv<'local>,
    activity: &JObject,
    state: &str,
    transcript: &str,
) -> Option<JObject<'local>> {
    let body = if transcript.trim().is_empty() {
        state.to_string()
    } else {
        format!("{state}\n{}", transcript.trim())
    };
    let tv_cls = env.find_class("android/widget/TextView").ok()?;
    let view = env
        .new_object(
            tv_cls,
            "(Landroid/content/Context;)V",
            &[JValue::from(activity)],
        )
        .ok()?;
    let text = env.new_string(&body).ok()?;
    env.call_method(
        &view,
        "setText",
        "(Ljava/lang/CharSequence;)V",
        &[JValue::from(&text)],
    )
    .ok()?;
    env.call_method(&view, "setTextColor", "(I)V", &[JValue::from(0xFFEDEDEDu32 as i32)])
        .ok()?;
    env.call_method(&view, "setTextSize", "(F)V", &[JValue::from(12.0f32)])
        .ok()?;
    env.call_method(
        &view,
        "setPadding",
        "(IIII)V",
        &[
            JValue::from(32i32),
            JValue::from(20i32),
            JValue::from(32i32),
            JValue::from(20i32),
        ],
    )
    .ok()?;
    let gd_cls = env.find_class("android/graphics/drawable/GradientDrawable").ok()?;
    let bg = env.new_object(gd_cls, "()V", &[]).ok()?;
    env.call_method(&bg, "setColor", "(I)V", &[JValue::from(0xF0121212u32 as i32)])
        .ok()?;
    env.call_method(&bg, "setCornerRadius", "(F)V", &[JValue::from(20.0f32)])
        .ok()?;
    env.call_method(
        &view,
        "setBackground",
        "(Landroid/graphics/drawable/Drawable;)V",
        &[JValue::from(&bg)],
    )
    .ok()?;
    Some(view)
}

/// Take the current bubble off the screen.
#[cfg(target_os = "android")]
fn remove_overlay_view(env: &mut jni::JNIEnv<'_>, activity: &JObject) {
    let handle = OVERLAY.lock().ok().and_then(|mut g| g.take());
    let Some(handle) = handle else { return };
    let view = handle.view.as_obj();
    if let Some(wm) = window_manager(env, activity) {
        let _ = env.call_method(
            &wm,
            "removeView",
            "(Landroid/view/View;)V",
            &[JValue::from(view)],
        );
    }
}

/// Show (or update) the floating bubble above other apps.
///
/// The bubble is built by the Java helper (`ai.quickstt.app.Bubble`) so it
/// can have buttons and be dragged around. When the helper is not available
/// the app falls back to a plain read-out bubble, so the overlay always
/// works.
///
/// The work happens on the Java UI thread (Android requires that for
/// window-manager calls), so a failure is reported through
/// `take_overlay_error()` instead of the return value.
pub fn show_overlay(state: &str, transcript: &str) -> Result<(), String> {
    #[cfg(target_os = "android")]
    {
        if !can_draw_overlays() {
            let message =
                "the “display over other apps” permission is missing — the bubble cannot be shown".to_string();
            set_overlay_error(&message);
            return Err(message);
        }
        let state = state.to_string();
        let transcript = transcript.trim().to_string();
        on_java_thread(move || {
            let _ = with_activity(|env, activity| {
                show_bubble(env, activity, &state, &transcript);
                Some(())
            });
        });
        Ok(())
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = (state, transcript);
        Ok(())
    }
}

/// Remove the floating bubble.
pub fn hide_overlay() {
    #[cfg(target_os = "android")]
    {
        on_java_thread(|| {
            let _ = with_activity(|env, activity| {
                // The Java helper removes its own bubble.
                if let Some(Err(message)) = call_bubble_helper(env, activity, "hide", "", "") {
                    set_overlay_error(&message);
                }
                // …and this removes the fallback bubble.
                remove_overlay_view(env, activity);
                Some(())
            });
        });
    }
}

/// Put the bubble on the screen (Java helper first, plain view as fallback).
#[cfg(target_os = "android")]
fn show_bubble(env: &mut jni::JNIEnv<'_>, activity: &JObject, state: &str, transcript: &str) {
    match call_bubble_helper(env, activity, "show", state, transcript) {
        Some(Ok(())) => return,
        Some(Err(message)) => {
            // The helper refused (most likely the overlay permission is
            // missing): fall back to the plain bubble, but say what happened.
            set_overlay_error(&format!("the bubble helper failed: {message}"));
        }
        None => {
            // No helper class: the plain bubble is all we have.
            log("the bubble helper is missing — using the plain bubble");
        }
    }
    let Some(view) = create_plain_bubble(env, activity, state, transcript) else {
        set_overlay_error("the bubble could not be built");
        return;
    };
    if add_overlay_view(env, activity, view).is_none() {
        set_overlay_error("the bubble could not be added to the screen");
    }
}

/// Add a view to the window manager as a floating overlay.
#[cfg(target_os = "android")]
fn add_overlay_view(
    env: &mut jni::JNIEnv<'_>,
    activity: &JObject,
    view: JObject<'_>,
) -> Option<()> {
    // Layout params: wrap content, top-start, no focus, translucent.
    //
    // Only the public five-argument constructor exists
    // (width, height, type, flags, format) — the longer one is hidden in
    // the framework, so the position is set afterwards.
    let lp_cls = env.find_class("android/view/WindowManager$LayoutParams").ok()?;
    let lp = env
        .new_object(
            lp_cls,
            "(IIIII)V",
            &[
                JValue::from(-2i32),  // width  = WRAP_CONTENT
                JValue::from(-2i32),  // height = WRAP_CONTENT
                JValue::from(2038i32), // type  = TYPE_APPLICATION_OVERLAY
                // NOT_FOCUSABLE | NOT_TOUCH_MODAL: the app in front keeps the
                // keyboard focus and taps outside the bubble still reach it.
                JValue::from(0x00000008i32 | 0x00000010i32),
                JValue::from(-3i32),  // format = TRANSLUCENT
            ],
        )
        .ok()?;
    // gravity = TOP | START, and the position.
    env.set_field(&lp, "gravity", "I", JValue::from(0x00000001i32 | 0x00000030i32))
        .ok()?;
    env.set_field(&lp, "x", "I", JValue::from(24i32)).ok()?;
    env.set_field(&lp, "y", "I", JValue::from(120i32)).ok()?;
    let wm = window_manager(env, activity)?;
    env.call_method(
        &wm,
        "addView",
        "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V",
        &[JValue::from(&view), JValue::from(&lp)],
    )
    .ok()?;
    let view = env.new_global_ref(view).ok()?;
    let params = env.new_global_ref(lp).ok()?;
    if let Ok(mut g) = OVERLAY.lock() {
        *g = Some(OverlayHandle { view, params });
    }
    Some(())
}

#[cfg(target_os = "android")]
fn window_manager<'a>(
    env: &mut jni::JNIEnv<'a>,
    activity: &JObject,
) -> Option<JObject<'a>> {
    let name = env.new_string("window").ok()?;
    env.call_method(
        activity,
        "getSystemService",
        "(Ljava/lang/String;)Ljava/lang/Object;",
        &[JValue::from(&name)],
    )
    .ok()?
    .l()
    .ok()
}
