package ai.quickstt.app;

import android.app.Activity;
import android.app.AppOpsManager;
import android.app.Application;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.PowerManager;
import android.os.Process;
import android.provider.Settings;

/**
 * The "display over other apps" permission and the floating bubble.
 *
 * <p>Three things live here:
 *
 * <ul>
 *   <li>{@link #granted(Context)} — is the permission there?
 *   <li>{@link #openSettings(Context)} — open the system page where the user
 *       grants it. It goes straight to QuickSTT's own page and falls back to
 *       the list of all apps and then to the app-info page, because several
 *       phone makers do not implement the action the same way.
 *   <li>{@link #watch(Context)} — tell Rust when the app comes back to the
 *       front. That is the moment the user returns from the settings, and
 *       the moment the bubble has to appear.
 * </ul>
 *
 * <p>Everything returns an error message instead of throwing, so the app can
 * tell the user what went wrong rather than failing silently.
 */
public final class Overlay {

    /** Called from Rust when the activity comes back to the front. */
    private static native void nativeActivityResumed();

    /** Set once the lifecycle callback is registered. */
    private static boolean watching;

    private Overlay() {
    }

    /**
     * May the app draw over other apps?
     *
     * <p>{@code Settings.canDrawOverlays} is the check Android itself uses.
     * Some phone makers keep the permission in the app-op instead, so that
     * is consulted as well.
     */
    public static boolean granted(Context context) {
        try {
            if (Settings.canDrawOverlays(context)) {
                return true;
            }
        } catch (Throwable ignored) {
            // Fall through to the app-op.
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
            // Nothing else to try.
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
     * Tell Rust when the app comes back to the front.
     *
     * <p>The user leaves the app to grant the permission and comes back —
     * this is where the app notices and puts the bubble on the screen.
     */
    public static void watch(Context context) {
        try {
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
                                nativeActivityResumed();
                            } catch (UnsatisfiedLinkError ignored) {
                                // The library is not loaded (yet).
                            }
                        }

                        @Override
                        public void onActivityCreated(Activity a, Bundle b) {
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
            // Without the callback the app still polls the permission.
        }
    }

    /** Show the floating bubble (see {@link Bubble}). */
    public static String show(Context context, String state, String transcript) {
        return Bubble.show(context, state, transcript);
    }

    /** Take the floating bubble off the screen (see {@link Bubble}). */
    public static String hide(Context context, String state, String transcript) {
        return Bubble.hide(context);
    }

    /**
     * Is QuickSTT exempt from the battery restrictions?
     *
     * <p>Without the exemption Android puts the app to sleep as soon as it
     * is in the background — and the bubble would stop working.
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
     *
     * <p>QuickSTT's own page first, the list of all apps as the fallback —
     * the same flow the reference app uses for the battery exemption.
     *
     * @return an empty string when a page was opened, otherwise the reason.
     */
    public static String requestBatteryExemption(Context context) {
        try {
            Intent intent = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS);
            intent.setData(Uri.parse("package:" + context.getPackageName()));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return "";
        } catch (Throwable t) {
            // The action is not implemented on every phone: show the list.
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
