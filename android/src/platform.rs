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
use jni::objects::{JObject, JString, JValue};
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
            let res: jint = env
                .call_method(activity, "hasWindowFocus", "()Z", &[])
                .ok()?
                .i()
                .ok()?;
            Some(res != 0)
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
    // "JNI DETECTED ERROR IN APPLICATION: pending exception".
    if env.exception_check().unwrap_or(false) {
        log("a Java call failed - the exception was cleared");
        let _ = env.exception_clear();
    }
    // Never delete a reference we do not own.
    std::mem::forget(activity);
    out
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

/// Copy text to the system clipboard (so it can be pasted into any app).
pub fn copy_to_clipboard(text: &str) -> bool {
    #[cfg(target_os = "android")]
    {
        let ok = with_activity(|env, activity| {
            let service_name = env.new_string("clipboard").ok()?;
            let service = env
                .call_method(
                    activity,
                    "getSystemService",
                    "(Ljava/lang/String;)Ljava/lang/Object;",
                    &[JValue::from(&service_name)],
                )
                .ok()?
                .l()
                .ok()?;
            let clip_cls = env.find_class("android/content/ClipData").ok()?;
            let label = env.new_string("QuickSTT").ok()?;
            let body = env.new_string(text).ok()?;
            let clip = env
                .call_static_method(
                    clip_cls,
                    "newPlainText",
                    "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;",
                    &[JValue::from(&label), JValue::from(&body)],
                )
                .ok()?
                .l()
                .ok()?;
            env.call_method(
                &service,
                "setPrimaryClip",
                "(Landroid/content/ClipData;)V",
                &[JValue::from(&clip)],
            )
            .ok()?;
            Some(true)
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
    #[cfg(target_os = "android")]
    {
        log("asking for the microphone permission");
        on_java_thread(|| {
            let result = with_activity(|env, activity| {
                let perm = env.new_string("android.permission.RECORD_AUDIO").ok()?;
                let string_cls = env.find_class("java.lang.String").ok()?;
                let arr = env
                    .new_object_array(1, &string_cls, JObject::null())
                    .ok()?;
                env.set_object_array_element(&arr, 0, &perm).ok()?;
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
}

/// True when the app may draw over other apps.
pub fn can_draw_overlays() -> bool {
    #[cfg(target_os = "android")]
    {
        with_activity(|env, activity| {
            let cls = env.find_class("android/provider/Settings").ok()?;
            let res: jint = env
                .call_static_method(
                    cls,
                    "canDrawOverlays",
                    "(Landroid/content/Context;)Z",
                    &[JValue::from(activity)],
                )
                .ok()?
                .i()
                .ok()?;
            Some(res != 0)
        })
        .unwrap_or(false)
    }
    #[cfg(not(target_os = "android"))]
    false
}

/// Open the system page where the user grants "display over other apps".
pub fn open_overlay_settings() {
    #[cfg(target_os = "android")]
    {
        log("opening the overlay settings");
        on_java_thread(|| {
            let _ = with_activity(|env, activity| {
                let pkg = env
                    .call_method(activity, "getPackageName", "()Ljava/lang/String;", &[])
                    .ok()?
                    .l()
                    .ok()?;
                let pkg_str: String = env.get_string(&JString::from(pkg)).ok()?.into();
                let uri_str = format!("package:{pkg_str}");
                let intent_cls = env.find_class("android/content/Intent").ok()?;
                let uri_cls = env.find_class("android/net/Uri").ok()?;
                let uri_text = env.new_string(&uri_str).ok()?;
                let uri = env
                    .call_static_method(
                        uri_cls,
                        "parse",
                        "(Ljava/lang/String;)Landroid/net/Uri;",
                        &[JValue::from(&uri_text)],
                    )
                    .ok()?
                    .l()
                    .ok()?;
                let intent = env.new_object(intent_cls, "()V", &[]).ok()?;
                let action = env
                    .new_string("android.settings.action.MANAGE_OVERLAY_PERMISSION")
                    .ok()?;
                env.call_method(
                    &intent,
                    "setAction",
                    "(Ljava/lang/String;)Landroid/content/Intent;",
                    &[JValue::from(&action)],
                )
                .ok()?;
                env.call_method(
                    &intent,
                    "setData",
                    "(Landroid/net/Uri;)Landroid/content/Intent;",
                    &[JValue::from(&uri)],
                )
                .ok()?;
                env.call_method(
                    activity,
                    "startActivity",
                    "(Landroid/content/Intent;)V",
                    &[JValue::from(&intent)],
                )
                .ok()?;
                Some(())
            });
        });
    }
}

/// Show (or update) the floating bubble above other apps.
pub fn show_overlay(state: &str, transcript: &str) -> bool {
    #[cfg(target_os = "android")]
    {
        let body = if transcript.trim().is_empty() {
            state.to_string()
        } else {
            format!("{state}\n{}", transcript.trim())
        };
        // WindowManager calls must happen on the UI thread.
        on_java_thread(move || {
            let _ = with_activity(|env, activity| {
                // Reuse the existing view when we already have one.
                let existing = OVERLAY.lock().ok().and_then(|mut g| g.take());
                if let Some(handle) = existing {
                    let view = handle.view.as_obj();
                    let params = handle.params.as_obj();
                    let text = env.new_string(&body).ok()?;
                    env.call_method(
                        view,
                        "setText",
                        "(Ljava/lang/CharSequence;)V",
                        &[JValue::from(&text)],
                    )
                    .ok()?;
                    let wm = window_manager(env, activity)?;
                    env.call_method(
                        &wm,
                        "updateViewLayout",
                        "(Landroid/view/View;Landroid/view/WindowManager$LayoutParams;)V",
                        &[JValue::from(view), JValue::from(params)],
                    )
                    .ok()?;
                    if let Ok(mut g) = OVERLAY.lock() {
                        *g = Some(handle);
                    }
                    return Some(true);
                }

                // Fresh bubble: TextView on a rounded translucent background.
                let tv_cls = env.find_class("android/widget/TextView").ok()?;
                let view = env
                    .new_object(tv_cls, "(Landroid/content/Context;)V", &[JValue::from(activity)])
                    .ok()?;
                let text = env.new_string(&body).ok()?;
                env.call_method(
                    &view,
                    "setText",
                    "(Ljava/lang/CharSequence;)V",
                    &[JValue::from(&text)],
                )
                .ok()?;
                env.call_method(&view, "setTextColor", "(I)V", &[JValue::from(0xF0F6FCi32)])
                    .ok()?;
                env.call_method(&view, "setTextSize", "(F)V", &[JValue::from(13.0f32)])
                    .ok()?;
                env.call_method(
                    &view,
                    "setPadding",
                    "(IIII)V",
                    &[
                        JValue::from(36i32),
                        JValue::from(24i32),
                        JValue::from(36i32),
                        JValue::from(24i32),
                    ],
                )
                .ok()?;
                // Rounded dark background.
                let gd_cls = env.find_class("android/graphics/drawable/GradientDrawable").ok()?;
                let bg = env.new_object(gd_cls, "()V", &[]).ok()?;
                env.call_method(&bg, "setColor", "(I)V", &[JValue::from(0x6101418i32)])
                    .ok()?;
                env.call_method(&bg, "setCornerRadius", "(F)V", &[JValue::from(48.0f32)])
                    .ok()?;
                env.call_method(
                    &view,
                    "setBackground",
                    "(Landroid/graphics/drawable/Drawable;)V",
                    &[JValue::from(&bg)],
                )
                .ok()?;
                // Layout params: wrap content, top-start, no focus, translucent.
                // The 9-arg LayoutParams ctor is
                // (w, h, type, flags, format, gravity, x, y, windowAnimations).
                let lp_cls = env.find_class("android/view/WindowManager$LayoutParams").ok()?;
                let lp = env
                    .new_object(
                        lp_cls,
                        "(IIIIIIIF)V",
                        &[
                            JValue::from(-2i32),  // width  = WRAP_CONTENT
                            JValue::from(-2i32),  // height = WRAP_CONTENT
                            JValue::from(2038i32), // type  = TYPE_APPLICATION_OVERLAY
                            JValue::from(0x00000008i32 | 0x00000200i32), // NOT_FOCUSABLE | NOT_TOUCHABLE
                            JValue::from(-3i32),  // format = TRANSLUCENT
                            JValue::from(0x00000001i32 | 0x00000030i32), // gravity = TOP | START
                            JValue::from(24i32),  // x
                            JValue::from(160i32), // y
                            JValue::from(0.0f32), // windowAnimations
                        ],
                    )
                    .ok()?;
                // A touchable bubble would swallow taps from other apps, so it
                // is explicitly non-touchable: the bubble is a status readout.
                let wm = window_manager(env, activity)?;
                env.call_method(
                    &wm,
                    "addView",
                    "(Landroid/view/View;Landroid/view/WindowManager$LayoutParams;)V",
                    &[JValue::from(&view), JValue::from(&lp)],
                )
                .ok()?;
                let view = env.new_global_ref(view).ok()?;
                let params = env.new_global_ref(lp).ok()?;
                if let Ok(mut g) = OVERLAY.lock() {
                    *g = Some(OverlayHandle { view, params });
                }
                Some(true)
            });
        });
        true
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = (state, transcript);
        false
    }
}

/// Remove the floating bubble.
pub fn hide_overlay() {
    #[cfg(target_os = "android")]
    {
        let handle = OVERLAY.lock().ok().and_then(|mut g| g.take());
        if let Some(handle) = handle {
            on_java_thread(move || {
                let _ = with_activity(|env, activity| {
                    let view = handle.view.as_obj();
                    let wm = window_manager(env, activity)?;
                    env.call_method(
                        &wm,
                        "removeView",
                        "(Landroid/view/View;)V",
                        &[JValue::from(view)],
                    )
                    .ok()?;
                    Some(())
                });
            });
        }
    }
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
