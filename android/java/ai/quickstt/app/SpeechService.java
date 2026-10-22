package ai.quickstt.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/**
 * Keeps QuickSTT alive while the user works in another app.
 *
 * <p>Android kills a background app as soon as it is not visible. A
 * floating bubble that has to stay above *other* apps therefore needs a
 * foreground service: it keeps the process alive, it shows a small
 * notification ("QuickSTT is ready") and it is the reason the bubble is
 * still there when the user comes back from the other app.
 *
 * <p>The service is started when the bubble is switched on and stopped when
 * it is switched off. It does not record anything by itself — the recording
 * is done by the native side, the service only keeps the process (and with
 * it the bubble and the engine) alive.
 *
 * <p>This is the same pattern the reference app uses for its playback
 * service: a notification channel, {@code startForeground} with the service
 * type, and {@code START_STICKY} so the service comes back if the system
 * kills it.
 */
public final class SpeechService extends Service {

    private static final String TAG = "QuickSTT";
    private static final String CHANNEL_ID = "quickstt.service";
    private static final int NOTIFICATION_ID = 4711;

    /** The action of the notification button that stops the service. */
    public static final String ACTION_STOP = "ai.quickstt.app.action.STOP";

    /**
     * Start the service.
     *
     * <p>{@code startForegroundService} is used on Android 8 and newer: the
     * service has to call {@code startForeground} right away, which it does
     * in {@link #onStartCommand}.
     */
    public static void start(Context context) {
        Intent intent = new Intent(context, SpeechService.class);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent);
            } else {
                context.startService(intent);
            }
        } catch (Throwable t) {
            Log.w(TAG, "the foreground service could not be started", t);
        }
    }

    /** Stop the service (and with it the bubble's background keep-alive). */
    public static void stop(Context context) {
        try {
            context.stopService(new Intent(context, SpeechService.class));
        } catch (Throwable t) {
            Log.w(TAG, "the foreground service could not be stopped", t);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        createChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            stopSelf();
            return START_NOT_STICKY;
        }
        Notification notification = buildNotification();
        // Android 14 and newer demand a type for every foreground service.
        // The service exists for the microphone, so that is the type it asks
        // for — but only when the microphone permission is there. Without it
        // the service falls back to "special use".
        int type = foregroundServiceType();
        if (type != 0) {
            try {
                startForeground(NOTIFICATION_ID, notification, type);
                return START_STICKY;
            } catch (Throwable t) {
                Log.w(TAG, "startForeground with the type " + type + " failed", t);
            }
        }
        try {
            startForeground(NOTIFICATION_ID, notification);
        } catch (Throwable t) {
            // The service cannot be a foreground service after all. It keeps
            // running as a normal service; the bubble still works as long as
            // the process lives.
            Log.e(TAG, "the foreground service could not be started", t);
        }
        // The service has to come back when the system kills the app.
        return START_STICKY;
    }

    /** The foreground service type that fits the current situation. */
    private int foregroundServiceType() {
        boolean microphone = false;
        try {
            microphone = checkSelfPermission("android.permission.RECORD_AUDIO")
                    == android.content.pm.PackageManager.PERMISSION_GRANTED;
        } catch (Throwable ignored) {
            // Without the permission the microphone type is not allowed.
        }
        if (microphone) {
            return ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
        }
        if (Build.VERSION.SDK_INT >= 34) {
            return ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE;
        }
        return 0;
    }

    @Override
    public void onDestroy() {
        // The bubble belongs to the app, not to the service: hide it when
        // the service goes away so no window is left behind.
        try {
            Bubble.hide(this);
        } catch (Throwable ignored) {
            // Nothing to do.
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    /** The notification that explains what the service is doing. */
    private Notification buildNotification() {
        Intent open = new Intent();
        open.setClassName(this, "android.app.NativeActivity");
        open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent content = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Intent stopIntent = new Intent(this, SpeechService.class).setAction(ACTION_STOP);
        PendingIntent stop = PendingIntent.getService(this, 1, stopIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        builder.setContentTitle("QuickSTT is ready")
                .setContentText("The mic bubble stays above your apps. Tap to open QuickSTT.")
                .setSmallIcon(android.R.drawable.ic_btn_speak_now)
                .setOngoing(true)
                .setContentIntent(content)
                .addAction(new Notification.Action.Builder(
                        android.R.drawable.ic_menu_close_clear_cancel, "Stop", stop).build());
        return builder.build();
    }

    /** The channel the notification lives in (Android 8 and newer). */
    private void createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        try {
            NotificationManager manager = getSystemService(NotificationManager.class);
            if (manager == null) {
                return;
            }
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "QuickSTT", NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Keeps the microphone bubble available in the background.");
            channel.setShowBadge(false);
            manager.createNotificationChannel(channel);
        } catch (Throwable t) {
            Log.w(TAG, "the notification channel could not be created", t);
        }
    }
}
