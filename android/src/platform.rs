// Android platform glue (JNI).
//
// Everything that needs the Java side lives here: the app's files dir, the
// clipboard, the microphone permission, and the floating overlay bubble that
// floats above other apps.
//
// The overlay is a plain `TextView` added to the WindowManager with
// TYPE_APPLICATION_OVERLAY. It shows the current state (idle / listening /
// transcribing) plus the last transcript, so dictation feedback is visible
// from any app.

use std::path::PathBuf;
#[cfg(target_os = "android")]
use std::sync::Mutex;

#[cfg(target_os = "android")]
use jni::objects::{JObject, JString, JValue};
#[cfg(target_os = "android")]
use jni::sys::{jint, jobject};
#[cfg(target_os = "android")]
use jni::JavaVM;

/// The overlay view + its layout params, kept alive between updates.
#[cfg(target_os = "android")]
struct OverlayHandle {
    view: jni::objects::GlobalRef,
    params: jni::objects::GlobalRef,
}

#[cfg(target_os = "android")]
static OVERLAY: Mutex<Option<OverlayHandle>> = Mutex::new(None);

/// Attach the current thread to the JVM and run `f` with the Activity
/// context. Returns None when anything is unavailable (desktop build, or a
/// JNI error).
#[cfg(target_os = "android")]
fn with_activity<T, F>(f: F) -> Option<T>
where
    F: FnOnce(&mut jni::JNIEnv, &JObject) -> Option<T>,
{
    let ctx = ndk_context::android_context();
    let vm = unsafe { JavaVM::from_raw(ctx.vm().cast()) }.ok()?;
    let mut env = vm.attach_current_thread().ok()?;
    // The context handle is owned by the Android runtime — never delete it.
    let activity = unsafe { JObject::from_raw(ctx.context() as jobject) };
    let out = f(&mut env, &activity);
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
        with_activity(|env, activity| {
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
        .unwrap_or(false)
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
pub fn request_mic_permission() {
    #[cfg(target_os = "android")]
    {
        with_activity(|env, activity| {
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
        with_activity(|env, activity| {
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
            let intent = env
                .new_object(intent_cls, "()V", &[])
                .ok()?;
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
    }
}

/// Show (or update) the floating bubble above other apps.
pub fn show_overlay(state: &str, transcript: &str) -> bool {
    #[cfg(target_os = "android")]
    {
        with_activity(|env, activity| {
            let body = if transcript.trim().is_empty() {
                state.to_string()
            } else {
                format!("{state}\n{}", transcript.trim())
            };

            // Reuse the existing view when we already have one.
            let existing = OVERLAY.lock().ok().and_then(|mut g| g.take());
            if let Some(handle) = existing {
                let view = handle.view.as_obj();
                let params = handle.params.as_obj();
                let text = env.new_string(&body).ok()?;
                env.call_method(view, "setText", "(Ljava/lang/CharSequence;)V", &[JValue::from(&text)])
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
            env.call_method(&view, "setText", "(Ljava/lang/CharSequence;)V", &[JValue::from(&text)])
                .ok()?;
            env.call_method(&view, "setTextColor", "(I)V", &[JValue::from(0xF0F6FCi32)])
                .ok()?;
            env.call_method(&view, "setTextSize", "(F)V", &[JValue::from(13.0f32)])
                .ok()?;
            env.call_method(&view, "setPadding", "(IIII)V", &[JValue::from(36i32), JValue::from(24i32), JValue::from(36i32), JValue::from(24i32)])
                .ok()?;
            // Rounded dark background.
            let gd_cls = env.find_class("android/graphics/drawable/GradientDrawable").ok()?;
            let bg = env.new_object(gd_cls, "()V", &[]).ok()?;
            env.call_method(&bg, "setColor", "(I)V", &[JValue::from(0x6101418i32)]).ok()?;
            env.call_method(&bg, "setCornerRadius", "(F)V", &[JValue::from(48.0f32)]).ok()?;
            env.call_method(&view, "setBackground", "(Landroid/graphics/drawable/Drawable;)V", &[JValue::from(&bg)])
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
            // A touchable bubble would swallow taps from other apps, so it is
            // explicitly non-touchable: the bubble is a status readout.
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
        })
        .unwrap_or(false)
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
            with_activity(|env, activity| {
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
