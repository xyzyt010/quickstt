package ai.quickstt.app;

import android.app.Activity;
import android.app.AppOpsManager;
import android.app.Application;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.Process;
import android.provider.Settings;
import android.view.View;
import android.view.inputmethod.InputMethodManager;

/**
 * The "display over other apps" permission, IME bridge, and floating squircle widget.
 */
public final class Overlay {

    static {
        try {
            System.loadLibrary("quickstt_android");
        } catch (Throwable ignored) {
        }
    }

    /** Called from Rust when the activity comes back to the front. */
    private static native void nativeActivityResumed();

    /** Registers a fallback Context with the native side so background services work without Activity. */
    private static native void nativeInitContext(Context context);

    /** Set once the lifecycle callback is registered. */
    private static boolean watching;

    private Overlay() {
    }

    /** Give the native Rust side a long-lived Context reference even when NativeActivity is closed. */
    public static void initNative(Context context) {
        if (context == null) {
            return;
        }
        try {
            Context app = context.getApplicationContext();
            nativeInitContext(app != null ? app : context);
        } catch (Throwable ignored) {
        }
    }

    /**
     * May the app draw over other apps?
     */
    public static boolean granted(Context context) {
        if (context == null) {
            return false;
        }
        try {
            if (Settings.canDrawOverlays(context)) {
                return true;
            }
        } catch (Throwable ignored) {
        }
        try {
            AppOpsManager ops = (AppOpsManager)
                    context.getSystemService(Context.APP_OPS_SERVICE);
            if (ops != null) {
                int mode = ops.checkOpNoThrow("android:system_alert_window",
                        Process.myUid(), context.getPackageName());
                if (mode == AppOpsManager.MODE_ALLOWED) {
                    return true;
                }
            }
        } catch (Throwable ignored) {
        }
        return false;
    }

    /**
     * Open the page where "display over other apps" is granted.
     *
     * @return an empty string when a page was opened, otherwise the reason.
     */
    public static String openSettings(Context context) {
        String pkg = context.getPackageName();
        String reason = "";

        // 1. QuickSTT's own page — the toggle is right there.
        try {
            Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION);
            intent.setData(Uri.parse("package:" + pkg));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return "";
        } catch (Throwable t) {
            reason = describe(t);
        }

        // 2. The list of all apps that may draw over other apps.
        try {
            Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return "";
        } catch (Throwable t) {
            reason = describe(t);
        }

        // 3. QuickSTT's app-info page (the toggle lives there on many phones).
        try {
            Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
            intent.setData(Uri.parse("package:" + pkg));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return "";
        } catch (Throwable t) {
            reason = describe(t);
        }

        return "the system settings could not be opened (" + reason + ")";
    }

    /**
     * Tell Rust when the app comes back to the front and keep the IME watcher active.
     */
    public static void watch(Context context) {
        try {
            initNative(context);
            Bubble.ensureWatcher(context);
            if (context instanceof Activity) {
                try {
                    ((Activity) context).setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
                } catch (Throwable ignored) {
                }
            }
            if (watching) {
                return;
            }
            Context app = context.getApplicationContext();
            if (!(app instanceof Application)) {
                return;
            }
            watching = true;
            ((Application) app).registerActivityLifecycleCallbacks(
                    new Application.ActivityLifecycleCallbacks() {
                        @Override
                        public void onActivityResumed(Activity activity) {
                            try {
                                activity.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
                            } catch (Throwable ignored) {
                            }
                            try {
                                initNative(activity);
                                Bubble.ensureWatcher(activity);
                                QuickSttInputMethodService.maybeShowPendingPickerOnResume(activity);
                            } catch (Throwable ignored) {
                            }
                            try {
                                nativeActivityResumed();
                            } catch (UnsatisfiedLinkError ignored) {
                            }
                        }

                        @Override
                        public void onActivityCreated(Activity a, Bundle b) {
                            try {
                                a.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
                            } catch (Throwable ignored) {
                            }
                        }

                        @Override
                        public void onActivityStarted(Activity a) {
                        }

                        @Override
                        public void onActivityPaused(Activity a) {
                        }

                        @Override
                        public void onActivityStopped(Activity a) {
                        }

                        @Override
                        public void onActivitySaveInstanceState(Activity a, Bundle b) {
                        }

                        @Override
                        public void onActivityDestroyed(Activity a) {
                        }
                    });
        } catch (Throwable ignored) {
        }
    }

    /** Ensure the 1px full-height IME watcher window is running (or stopped if disabled). */
    public static void ensureWatcher(Context context) {
        if (Bubble.pillOnFocus(context) && granted(context)) {
            Bubble.ensureWatcher(context);
        } else {
            Bubble.stopWatcher();
        }
    }

    /**
     * Called when the user taps the transcript box inside the QuickSTT app:
     * shows the floating squircle widget immediately and requests the soft keyboard.
     */
    public static void focusInAppTextBox(final Context context) {
        Bubble.onInAppFieldFocused(context, true);
        if (context == null) {
            return;
        }
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                try {
                    if (context instanceof Activity) {
                        Activity act = (Activity) context;
                        View decor = act.getWindow() != null ? act.getWindow().getDecorView() : null;
                        InputMethodManager imm = (InputMethodManager)
                                act.getSystemService(Context.INPUT_METHOD_SERVICE);
                        if (imm != null && decor != null) {
                            decor.requestFocus();
                            imm.showSoftInput(decor, InputMethodManager.SHOW_IMPLICIT);
                        }
                    }
                } catch (Throwable ignored) {
                }
            }
        });
    }

    /** Show the floating squircle widget (see {@link Bubble}). */
    public static String show(Context context, String state, String transcript) {
        return Bubble.show(context, state, transcript);
    }

    /** Take the floating bubble off the screen (see {@link Bubble}). */
    public static String hide(Context context, String state, String transcript) {
        return Bubble.hide(context);
    }

    /** Feed live microphone RMS level and state to the floating bubble waveform. */
    public static void setLevel(float level, boolean recording, boolean busy) {
        Bubble.setLevel(level, recording, busy);
    }

    /** Expand or collapse the floating bubble between rounded icon and circular pill. */
    public static void setExpanded(boolean expanded) {
        Bubble.setExpanded(expanded);
    }

    /** True when the QuickSTT Voice Auto-Type input method is both enabled and selected. */
    public static boolean accessibilityEnabled(Context context) {
        return QuickSttInputMethodService.isReady(context);
    }

    /** True when the QuickSTT Voice Auto-Type input method is enabled in system settings. */
    public static boolean imeEnabled(Context context) {
        return QuickSttInputMethodService.isEnabled(context);
    }

    /** Open the Android Keyboard settings or Input Method Picker so the user can enable auto-typing. */
    public static String openAccessibilitySettings(Context context) {
        return QuickSttInputMethodService.openSettingsOrPicker(context);
    }

    /** Reset the live-typing session at the start of a recording turn. */
    public static void beginTypingTurn() {
        QuickSttInputMethodService.beginTurn();
    }

    /** True when an editable text box is currently focused in the active window. */
    public static boolean hasFocusedTextBox() {
        return QuickSttInputMethodService.hasFocusedTextBox();
    }

    /** Type a live partial transcript into the focused text box in real time. */
    public static boolean typeLivePartial(String text) {
        return QuickSttInputMethodService.typeLivePartial(text);
    }

    /** Commit the final transcript into the focused text box when recording stops. */
    public static boolean commitFinalText(String text) {
        return QuickSttInputMethodService.commitFinalText(text);
    }

    /** Discard any live composing text that was typed during this turn. */
    public static boolean cancelTypingTurn() {
        return QuickSttInputMethodService.cancelLiveTyping();
    }

    /**
     * Is QuickSTT exempt from the battery restrictions?
     */
    public static boolean ignoringBatteryOptimizations(Context context) {
        try {
            PowerManager power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
            return power != null
                    && power.isIgnoringBatteryOptimizations(context.getPackageName());
        } catch (Throwable t) {
            return false;
        }
    }

    /**
     * Ask the system to let QuickSTT run in the background.
     */
    public static String requestBatteryExemption(Context context) {
        try {
            Intent intent = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS);
            intent.setData(Uri.parse("package:" + context.getPackageName()));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return "";
        } catch (Throwable t) {
            try {
                Intent intent = new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS);
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                context.startActivity(intent);
                return "";
            } catch (Throwable t2) {
                return "the battery settings could not be opened (" + describe(t2) + ")";
            }
        }
    }

    /** May QuickSTT show its notification? (Android 13+ asks for it.) */
    public static boolean canPostNotifications(Context context) {
        try {
            if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.TIRAMISU) {
                return true;
            }
            return context.checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                    == android.content.pm.PackageManager.PERMISSION_GRANTED;
        } catch (Throwable t) {
            return true;
        }
    }

    /** Copy text to the clipboard safely from any thread on the main Looper. */
    public static boolean copyToClipboard(final Context context, final String text) {
        try {
            android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());
            handler.post(new Runnable() {
                @Override
                public void run() {
                    try {
                        android.content.ClipboardManager cm = (android.content.ClipboardManager)
                                context.getSystemService(Context.CLIPBOARD_SERVICE);
                        if (cm != null) {
                            android.content.ClipData clip = android.content.ClipData.newPlainText("QuickSTT", text);
                            cm.setPrimaryClip(clip);
                        }
                    } catch (Throwable t) {
                        android.util.Log.w("QuickSTT", "could not copy to clipboard", t);
                    }
                }
            });
            return true;
        } catch (Throwable t) {
            return false;
        }
    }

    private static String describe(Throwable t) {
        String message = t.getMessage();
        if (message == null || message.isEmpty()) {
            message = t.getClass().getSimpleName();
        }
        return message;
    }
}
