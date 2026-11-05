package ai.quickstt.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Choreographer;
import android.view.ContextThemeWrapper;
import android.view.Display;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;

/**
 * Floating squircle widget for QuickSTT.
 *
 * <ul>
 *   <li><b>Collapsed (compact) state:</b> 48dp translucent #121212/#181818 dark-grey squircle —
 *       tapping it starts transcription immediately (or expands while busy).</li>
 *   <li><b>Expanded state:</b> 248x54dp dark-grey squircle bar (23dp radius), no border,
 *       white bars driven 1:1 by the microphone input (rolling history of real RMS levels),
 *       a round circular loading arc, and two sharp minimal buttons:
 *       <b>✕</b> stop &amp; close without output and <b>✓</b> stop &amp; type.</li>
 *   <li><b>Auto-appearance on any text field:</b> combines direct callbacks from
 *       {@link QuickSttInputMethodService} with a 1px full-height transparent
 *       {@link ImeWatcherView} that monitors {@code WindowInsets.Type.ime()} so the
 *       squircle widget pops up whenever any keyboard (QuickSTT, Gboard, Samsung Keyboard,
 *       etc.) opens on any text field inside or outside the app.</li>
 * </ul>
 */
public final class Bubble {

    private static final String TAG = "QuickSTT";

    /** Start or stop the recording (expand on idle / stop &amp; type while recording). */
    public static final int ACTION_RECORD = 1;
    /** Type the last transcript into the app that has the focus. */
    public static final int ACTION_TYPE = 2;
    /** Hide the bubble completely. */
    public static final int ACTION_CLOSE = 3;
    /** Stop transcription and throw the audio away — no text is produced. */
    public static final int ACTION_DISCARD = 4;

    /** TYPE_APPLICATION_OVERLAY: drawn above other apps on Android 8+. */
    private static final int TYPE_APPLICATION_OVERLAY = 2038;

    /** Keep keyboard focus in the underlying app while receiving touch inside the bubble. */
    private static final int FLAGS = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;

    /** Flags for the 1px full-height invisible IME watcher view. */
    private static final int WATCHER_FLAGS = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
            | WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH;

    private static final Handler MAIN_HANDLER = new Handler(Looper.getMainLooper());

    private static BubbleView current;
    private static WindowManager.LayoutParams currentParams;
    private static WindowManager currentManager;
    private static int savedX = -1;
    private static int savedY = -1;
    private static boolean userDraggedPosition = false;

    private static ImeWatcherView watcherView;
    private static WindowManager watcherManager;
    private static Context lastContext;

    private static volatile boolean watcherImeVisible = false;
    private static volatile int watcherImeBottomPx = 0;
    private static volatile boolean imeServiceActive = false;
    private static volatile boolean inAppFieldFocused = false;
    private static volatile boolean userDismissedUntilImeReset = false;

    static {
        try {
            System.loadLibrary("quickstt_android");
        } catch (Throwable e) {
            Log.w(TAG, "quickstt_android library not loaded yet for Bubble", e);
        }
    }

    /** Called when the user interacts with the bubble. */
    private static native void nativeBubbleAction(int action);

    private Bubble() {
    }

    /** True when the user allowed the floating pill to appear on text fields. */
    public static boolean pillOnFocus(Context context) {
        if (context == null) {
            return true;
        }
        try {
            java.io.File f = new java.io.File(context.getFilesDir(), "quickstt/pill_on_focus_v2");
            if (!f.exists()) {
                return true;
            }
            byte[] bytes = new byte[8];
            java.io.FileInputStream in = new java.io.FileInputStream(f);
            int n = in.read(bytes);
            in.close();
            if (n <= 0) {
                return true;
            }
            return !new String(bytes, 0, n, "UTF-8").trim().equals("0");
        } catch (Throwable t) {
            return true;
        }
    }

    /**
     * Ensure the invisible 1px full-height IME watcher window is attached so QuickSTT
     * detects whenever any keyboard opens for a text field in any app.
     */
    public static void ensureWatcher(final Context context) {
        if (context == null) {
            return;
        }
        Overlay.initNative(context);
        if (Looper.myLooper() != Looper.getMainLooper()) {
            MAIN_HANDLER.post(new Runnable() {
                @Override
                public void run() {
                    ensureWatcherOnMain(context);
                }
            });
        } else {
            ensureWatcherOnMain(context);
        }
    }

    private static void ensureWatcherOnMain(Context context) {
        try {
            lastContext = context.getApplicationContext() != null
                    ? context.getApplicationContext() : context;
            if (!Overlay.granted(context) || !pillOnFocus(context)) {
                stopWatcherOnMain();
                return;
            }
            if (watcherView != null && watcherManager != null && watcherView.isAttachedToWindow()) {
                watcherView.checkImeInsets();
                return;
            }
            stopWatcherOnMain();

            Context vCtx = visualContext(context);
            WindowManager wm = resolveWindowManager(vCtx);
            if (wm == null) {
                return;
            }
            ImeWatcherView view = new ImeWatcherView(vCtx);
            WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                    1,
                    WindowManager.LayoutParams.MATCH_PARENT,
                    TYPE_APPLICATION_OVERLAY,
                    WATCHER_FLAGS,
                    PixelFormat.TRANSPARENT);
            lp.gravity = Gravity.TOP | Gravity.START;
            lp.x = 0;
            lp.y = 0;
            wm.addView(view, lp);
            watcherView = view;
            watcherManager = wm;
            view.checkImeInsets();
        } catch (Throwable t) {
            Log.w(TAG, "ensureWatcherOnMain failed", t);
        }
    }

    public static void stopWatcher() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            MAIN_HANDLER.post(new Runnable() {
                @Override
                public void run() {
                    stopWatcherOnMain();
                }
            });
        } else {
            stopWatcherOnMain();
        }
    }

    private static void stopWatcherOnMain() {
        try {
            if (watcherView != null) {
                watcherView.stopWatching();
            }
            if (watcherView != null && watcherManager != null) {
                try {
                    watcherManager.removeViewImmediate(watcherView);
                } catch (Throwable ignored) {
                    try {
                        watcherManager.removeView(watcherView);
                    } catch (Throwable ignored2) {
                    }
                }
            }
        } catch (Throwable ignored) {
        } finally {
            watcherView = null;
            watcherManager = null;
            watcherImeVisible = false;
        }
    }

    /** Called by {@link QuickSttInputMethodService} when an editable field becomes active/inactive. */
    public static void onImeActive(final Context context, final boolean active) {
        if (context != null) {
            lastContext = context.getApplicationContext() != null
                    ? context.getApplicationContext() : context;
        }
        if (Looper.myLooper() != Looper.getMainLooper()) {
            MAIN_HANDLER.post(new Runnable() {
                @Override
                public void run() {
                    handleImeActiveOnMain(context != null ? context : lastContext, active);
                }
            });
        } else {
            handleImeActiveOnMain(context != null ? context : lastContext, active);
        }
    }

    private static void handleImeActiveOnMain(Context context, boolean active) {
        boolean wasActive = imeServiceActive;
        imeServiceActive = active;
        if (active) {
            if (!wasActive) {
                userDismissedUntilImeReset = false;
            }
            if (context != null && pillOnFocus(context) && !userDismissedUntilImeReset) {
                ensureWatcherOnMain(context);
                showOnMain(context, "Ready", "");
            }
        } else {
            userDismissedUntilImeReset = false;
            maybeAutoHidePillOnMain(context);
        }
    }

    /** Called when the user taps a text field / transcript box inside the QuickSTT app itself. */
    public static void onInAppFieldFocused(final Context context, final boolean focused) {
        if (context != null) {
            lastContext = context.getApplicationContext() != null
                    ? context.getApplicationContext() : context;
        }
        if (Looper.myLooper() != Looper.getMainLooper()) {
            MAIN_HANDLER.post(new Runnable() {
                @Override
                public void run() {
                    inAppFieldFocused = focused;
                    if (focused) {
                        userDismissedUntilImeReset = false;
                        ensureWatcherOnMain(context);
                        showOnMain(context, "Ready", "");
                    } else {
                        maybeAutoHidePillOnMain(context);
                    }
                }
            });
        } else {
            inAppFieldFocused = focused;
            if (focused) {
                userDismissedUntilImeReset = false;
                ensureWatcherOnMain(context);
                showOnMain(context, "Ready", "");
            } else {
                maybeAutoHidePillOnMain(context);
            }
        }
    }

    private static void onWatcherImeChangedOnMain(Context context, boolean visible, int imeBottomPx) {
        boolean wasVisible = watcherImeVisible;
        watcherImeVisible = visible;
        watcherImeBottomPx = imeBottomPx;
        if (visible) {
            if (!wasVisible) {
                userDismissedUntilImeReset = false;
            }
            if (context != null && pillOnFocus(context) && !userDismissedUntilImeReset) {
                showOnMain(context, "Ready", "");
                adjustPositionAboveImeOnMain(context, imeBottomPx);
            }
        } else {
            userDismissedUntilImeReset = false;
            inAppFieldFocused = false;
            maybeAutoHidePillOnMain(context);
        }
    }

    private static void adjustPositionAboveImeOnMain(Context context, int imeBottomPx) {
        if (userDraggedPosition || current == null || currentParams == null || currentManager == null || imeBottomPx <= 0) {
            return;
        }
        try {
            int screenH = context.getResources().getDisplayMetrics().heightPixels;
            int targetY = Math.max(dp(context, 72), screenH - imeBottomPx - dp(context, 68));
            if (Math.abs(currentParams.y - targetY) > dp(context, 8)) {
                currentParams.y = targetY;
                currentManager.updateViewLayout(current, currentParams);
            }
        } catch (Throwable ignored) {
        }
    }

    private static boolean isRecordingOrBusy() {
        return current != null && (current.recording || current.busy);
    }

    private static void maybeAutoHidePillOnMain(Context context) {
        if (imeServiceActive || watcherImeVisible || inAppFieldFocused || isRecordingOrBusy()) {
            return;
        }
        hidePillOnlyOnMain();
    }

    /** True when either the IME watcher or QuickSttInputMethodService sees an active text field. */
    public static boolean isAnyTextFieldActive() {
        return imeServiceActive || watcherImeVisible || inAppFieldFocused
                || QuickSttInputMethodService.hasFocusedTextBox();
    }

    /**
     * Show (or update in-place) the floating squircle pill on the main UI thread.
     *
     * @return an empty string on success, otherwise the reason it failed.
     */
    public static String show(final Context context, final String state, final String transcript) {
        final Context ctx = context != null ? context : lastContext;
        if (ctx == null) {
            return "no context available";
        }
        lastContext = ctx.getApplicationContext() != null ? ctx.getApplicationContext() : ctx;
        if (!Overlay.granted(ctx)) {
            return "the “display over other apps” permission is missing";
        }
        if (Looper.myLooper() != Looper.getMainLooper()) {
            MAIN_HANDLER.post(new Runnable() {
                @Override
                public void run() {
                    ensureWatcherOnMain(ctx);
                    showOnMain(ctx, state, transcript);
                }
            });
            return "";
        }
        ensureWatcherOnMain(ctx);
        return showOnMain(ctx, state, transcript);
    }

    private static String showOnMain(Context context, String state, String transcript) {
        try {
            if (!Overlay.granted(context)) {
                return "the “display over other apps” permission is missing";
            }

            if (current != null && currentManager != null && current.isAttachedToWindow()) {
                current.setStateText(state, transcript);
                return "";
            }

            hidePillOnlyOnMain();

            Context vCtx = visualContext(context);
            WindowManager manager = resolveWindowManager(vCtx);
            if (manager == null) {
                return "the window manager is not available";
            }

            BubbleView view = new BubbleView(vCtx);
            view.setStateText(state, transcript);

            int initW = view.collapsedSizePx();
            int initH = view.collapsedSizePx();

            WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                    initW,
                    initH,
                    TYPE_APPLICATION_OVERLAY,
                    FLAGS,
                    PixelFormat.TRANSLUCENT);
            params.gravity = Gravity.TOP | Gravity.START;
            int defaultX = Math.max(dp(vCtx, 16),
                    vCtx.getResources().getDisplayMetrics().widthPixels - initW - dp(vCtx, 18));
            int defaultY = watcherImeBottomPx > 0
                    ? Math.max(dp(vCtx, 72),
                            vCtx.getResources().getDisplayMetrics().heightPixels - watcherImeBottomPx - dp(vCtx, 68))
                    : dp(vCtx, 140);
            params.x = savedX >= 0 ? savedX : defaultX;
            params.y = savedY >= 0 ? savedY : defaultY;

            manager.addView(view, params);

            current = view;
            currentParams = params;
            currentManager = manager;
            return "";
        } catch (Throwable t) {
            Log.e(TAG, "Bubble.showOnMain failed", t);
            return describe(t);
        }
    }

    /** Remove the visible pill from the screen (keeps the IME watcher active if enabled). */
    public static String hide(final Context context) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            MAIN_HANDLER.post(new Runnable() {
                @Override
                public void run() {
                    hidePillOnlyOnMain();
                }
            });
            return "";
        }
        return hidePillOnlyOnMain();
    }

    private static String hidePillOnlyOnMain() {
        try {
            if (current != null) {
                current.stopAnimation();
            }
            if (current != null && currentManager != null) {
                try {
                    currentManager.removeViewImmediate(current);
                } catch (Throwable ignored) {
                    try {
                        currentManager.removeView(current);
                    } catch (Throwable ignored2) {
                    }
                }
            }
            current = null;
            currentParams = null;
            currentManager = null;
            return "";
        } catch (Throwable t) {
            current = null;
            currentParams = null;
            currentManager = null;
            return describe(t);
        }
    }

    /** Update the bubble state &amp; transcript without recreating the window. */
    public static String update(final String state, final String transcript) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            MAIN_HANDLER.post(new Runnable() {
                @Override
                public void run() {
                    if (current != null) {
                        current.setStateText(state, transcript);
                    }
                }
            });
            return "";
        }
        try {
            if (current != null) {
                current.setStateText(state, transcript);
                return "";
            }
            return "no bubble";
        } catch (Throwable t) {
            return describe(t);
        }
    }

    /** Feed live microphone RMS level (0..1) and recording/busy flags into the pill. */
    public static void setLevel(final float level, final boolean recording, final boolean busy) {
        QuickSttInputMethodService.syncState(null, recording, busy, level);
        final BubbleView view = current;
        if (view == null) {
            if ((recording || busy) && lastContext != null) {
                show(lastContext, recording ? "Listening…" : "Working…", "");
            }
            return;
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            view.updateLiveLevel(level, recording, busy);
        } else {
            MAIN_HANDLER.post(new Runnable() {
                @Override
                public void run() {
                    if (current != null) {
                        current.updateLiveLevel(level, recording, busy);
                    }
                }
            });
        }
    }

    /** Expand or collapse the bubble programmatically. */
    public static void setExpanded(final boolean expanded) {
        MAIN_HANDLER.post(new Runnable() {
            @Override
            public void run() {
                if (current != null) {
                    current.setExpanded(expanded);
                }
            }
        });
    }

    /**
     * Build a proper Display-backed WindowContext on Android 11+ (API 30+) so
     * {@code createWindowContext} never throws {@code UnsupportedOperationException}
     * when given an {@code Application} context.
     */
    private static Context visualContext(Context context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                DisplayManager dm = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
                Display display = dm != null ? dm.getDisplay(Display.DEFAULT_DISPLAY) : null;
                Context base = display != null ? context.createDisplayContext(display) : context;
                Context winCtx = base.createWindowContext(TYPE_APPLICATION_OVERLAY, null);
                if (winCtx != null) {
                    return winCtx;
                }
            } catch (Throwable ignored) {
            }
        }
        return context;
    }

    private static WindowManager resolveWindowManager(Context context) {
        try {
            Context vCtx = visualContext(context);
            WindowManager wm = (WindowManager) vCtx.getSystemService(Context.WINDOW_SERVICE);
            if (wm != null) {
                return wm;
            }
        } catch (Throwable ignored) {
        }
        try {
            WindowManager wm = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
            if (wm != null) {
                return wm;
            }
        } catch (Throwable ignored) {
        }
        try {
            Context app = context.getApplicationContext();
            if (app != null) {
                return (WindowManager) app.getSystemService(Context.WINDOW_SERVICE);
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    static void dispatchAction(int action) {
        try {
            nativeBubbleAction(action);
        } catch (Throwable t) {
            Log.w(TAG, "nativeBubbleAction failed", t);
        }
    }

    static void resizeWindow(int widthPx, int heightPx) {
        if (current != null && currentParams != null && currentManager != null) {
            if (currentParams.width != widthPx || currentParams.height != heightPx) {
                currentParams.width = widthPx;
                currentParams.height = heightPx;
                try {
                    currentManager.updateViewLayout(current, currentParams);
                } catch (Throwable ignored) {
                }
            }
        }
    }

    static void moveWindowBy(int dx, int dy) {
        if (current != null && currentParams != null && currentManager != null) {
            currentParams.x = Math.max(0, currentParams.x + dx);
            currentParams.y = Math.max(0, currentParams.y + dy);
            savedX = currentParams.x;
            savedY = currentParams.y;
            userDraggedPosition = true;
            try {
                currentManager.updateViewLayout(current, currentParams);
            } catch (Throwable ignored) {
            }
        }
    }

    static int dp(Context context, float value) {
        return Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, value,
                context.getResources().getDisplayMetrics()));
    }

    private static String describe(Throwable t) {
        String message = t.getMessage();
        if (message == null || message.isEmpty()) {
            message = t.getClass().getSimpleName();
        }
        return message;
    }

    /**
     * 1px full-height transparent overlay view that detects whenever any soft keyboard
     * (IME) opens or closes on the display, even when another keyboard such as Gboard
     * is the active input method.
     */
    private static final class ImeWatcherView extends View {

        private boolean watching = true;

        private final Runnable periodicCheck = new Runnable() {
            @Override
            public void run() {
                if (!watching) {
                    return;
                }
                checkImeInsets();
                MAIN_HANDLER.postDelayed(this, 450L);
            }
        };

        ImeWatcherView(Context context) {
            super(context);
            setWillNotDraw(true);
            MAIN_HANDLER.postDelayed(periodicCheck, 250L);
        }

        void stopWatching() {
            watching = false;
            MAIN_HANDLER.removeCallbacks(periodicCheck);
        }

        @Override
        protected void onDetachedFromWindow() {
            stopWatching();
            super.onDetachedFromWindow();
        }

        @Override
        public WindowInsets onApplyWindowInsets(WindowInsets insets) {
             inspectInsets(insets);
            return super.onApplyWindowInsets(insets);
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            if (event != null && event.getActionMasked() == MotionEvent.ACTION_OUTSIDE) {
                // The user just tapped somewhere on screen (e.g. a text field).
                // Probe IME insets as the keyboard animates open.
                scheduleFollowUpChecks();
            }
            return false;
        }

        private void scheduleFollowUpChecks() {
            final int[] delaysMs = {80, 200, 380, 650};
            for (int delay : delaysMs) {
                MAIN_HANDLER.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        if (watching) {
                            checkImeInsets();
                        }
                    }
                }, delay);
            }
        }

        void checkImeInsets() {
            if (!watching || !isAttachedToWindow()) {
                return;
            }
            try {
                WindowInsets root = getRootWindowInsets();
                if (root != null) {
                    inspectInsets(root);
                }
            } catch (Throwable ignored) {
            }
        }

        private void inspectInsets(WindowInsets insets) {
            if (insets == null) {
                return;
            }
            boolean visible = false;
            int imeBottom = 0;
            try {
                if (Build.VERSION.SDK_INT >= 30) {
                    imeBottom = insets.getInsets(WindowInsets.Type.ime()).bottom;
                    visible = insets.isVisible(WindowInsets.Type.ime())
                            || imeBottom > dp(getContext(), 40);
                } else {
                    int sysBottom = insets.getSystemWindowInsetBottom();
                    int stableBottom = insets.getStableInsetBottom();
                    imeBottom = Math.max(0, sysBottom - stableBottom);
                    visible = imeBottom > dp(getContext(), 60);
                }
            } catch (Throwable ignored) {
            }
            if (visible != watcherImeVisible || (visible && Math.abs(imeBottom - watcherImeBottomPx) > dp(getContext(), 12))) {
                onWatcherImeChangedOnMain(getContext(), visible, imeBottom);
            }
        }
    }

    /**
     * Custom hardware-accelerated squircle overlay view (#121212 / #181818 dark grey theme).
     */
    private static final class BubbleView extends View implements Choreographer.FrameCallback {

        private static final int BAR_COUNT = 13;
        private static final int COLLAPSED_BARS = 5;

        private final int collapsedSize;
        private final int expandedWidth;
        private final int expandedHeight;
        private final float collapsedRadius;
        private final float expandedRadius;

        private final Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint wavePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint subWavePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint chipPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint spinnerTrackPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint spinnerArcPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint glyphPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        private final RectF cardRect = new RectF();
        private final RectF barRect = new RectF();
        private final RectF discardBtnRect = new RectF();
        private final RectF acceptBtnRect = new RectF();
        private final RectF spinnerRect = new RectF();
        private final Path squirclePath = new Path();
        private final Path glyphPath = new Path();

        /** Rolling history of real microphone levels, oldest → newest. */
        private final float[] history = new float[BAR_COUNT];
        private int historyHead = 0;
        private final float[] ordered = new float[BAR_COUNT];

        boolean expanded = false;
        private float expandProgress = 0.0f;
        boolean recording = false;
        boolean busy = false;
        private float targetLevel = 0.0f;
        private float smoothLevel = 0.0f;
        private float spinnerAngle = 0.0f;
        private long lastFrameNanos = 0L;
        private boolean animating = false;

        private String stateLabel = "Ready";

        private float downRawX;
        private float downRawY;
        private float lastRawX;
        private float lastRawY;
        private boolean dragged;

        BubbleView(Context context) {
            super(new ContextThemeWrapper(context, android.R.style.Theme_DeviceDefault));
            collapsedSize = dp(context, 48f);
            expandedWidth = dp(context, 248f);
            expandedHeight = dp(context, 54f);
            collapsedRadius = dp(context, 15f);
            expandedRadius = dp(context, 23f);

            bgPaint.setStyle(Paint.Style.FILL);
            bgPaint.setColor(0xD8181818);

            borderPaint.setStyle(Paint.Style.STROKE);
            borderPaint.setStrokeWidth(dp(context, 1.0f));
            borderPaint.setColor(0x40FFFFFF);

            wavePaint.setStyle(Paint.Style.FILL);
            wavePaint.setColor(0xFFFFFFFF);

            subWavePaint.setStyle(Paint.Style.FILL);
            subWavePaint.setColor(0x38FFFFFF);

            chipPaint.setStyle(Paint.Style.FILL);

            spinnerTrackPaint.setStyle(Paint.Style.STROKE);
            spinnerTrackPaint.setStrokeWidth(dp(context, 2.3f));
            spinnerTrackPaint.setColor(0x2EFFFFFF);

            spinnerArcPaint.setStyle(Paint.Style.STROKE);
            spinnerArcPaint.setStrokeWidth(dp(context, 2.3f));
            spinnerArcPaint.setStrokeCap(Paint.Cap.ROUND);
            spinnerArcPaint.setColor(0xFFF5F5F5);

            glyphPaint.setStyle(Paint.Style.STROKE);
            glyphPaint.setStrokeCap(Paint.Cap.BUTT);
            glyphPaint.setStrokeJoin(Paint.Join.MITER);
            glyphPaint.setColor(0xFFFFFFFF);

            for (int i = 0; i < BAR_COUNT; i++) {
                history[i] = 0.08f;
            }
            startAnimation();
        }

        int collapsedSizePx() {
            return collapsedSize;
        }

        void setExpanded(boolean wantExpanded) {
            this.expanded = wantExpanded;
            startAnimation();
        }

        void setStateText(String state, String transcript) {
            String s = state != null ? state.trim() : "";
            this.stateLabel = s.isEmpty() ? "Ready" : s;

            String low = s.toLowerCase();
            if (low.contains("listen") || low.contains("record") || low.contains("live")) {
                this.recording = true;
                this.busy = false;
                this.expanded = true;
            } else if (low.contains("transcrib") || low.contains("process") || low.contains("finish")
                    || low.contains("onload") || low.contains("offload") || low.contains("load")) {
                this.recording = false;
                this.busy = true;
                this.expanded = true;
            } else {
                this.recording = false;
                this.busy = false;
            }
            QuickSttInputMethodService.syncState(this.stateLabel, this.recording, this.busy, this.targetLevel);
            startAnimation();
            invalidate();
        }

        void updateLiveLevel(float level, boolean isRecording, boolean isBusy) {
            this.targetLevel = Math.max(0.0f, Math.min(1.0f, level));
            this.recording = isRecording;
            this.busy = isBusy;
            if (isRecording || isBusy) {
                this.expanded = true;
            }
            history[historyHead] = 0.08f + this.targetLevel;
            historyHead = (historyHead + 1) % BAR_COUNT;
            QuickSttInputMethodService.syncState(this.stateLabel, this.recording, this.busy, this.targetLevel);
            startAnimation();
        }

        void startAnimation() {
            if (!animating) {
                animating = true;
                lastFrameNanos = System.nanoTime();
                Choreographer.getInstance().postFrameCallback(this);
            }
        }

        void stopAnimation() {
            animating = false;
            Choreographer.getInstance().removeFrameCallback(this);
        }

        @Override
        protected void onDetachedFromWindow() {
            stopAnimation();
            super.onDetachedFromWindow();
        }

        @Override
        public void doFrame(long frameTimeNanos) {
            if (!animating) {
                return;
            }
            float dt = (frameTimeNanos - lastFrameNanos) * 1e-9f;
            if (dt <= 0f || dt > 0.1f) {
                dt = 0.016f;
            }
            lastFrameNanos = frameTimeNanos;

            float targetExpand = expanded ? 1.0f : 0.0f;
            float expandSpeed = expanded ? 14.0f : 12.0f;
            expandProgress += (targetExpand - expandProgress) * Math.min(1.0f, dt * expandSpeed);
            if (Math.abs(targetExpand - expandProgress) < 0.002f) {
                expandProgress = targetExpand;
            }

            int curW = Math.round(collapsedSize + (expandedWidth - collapsedSize) * expandProgress);
            int curH = Math.round(collapsedSize + (expandedHeight - collapsedSize) * expandProgress);
            resizeWindow(curW, curH);

            float activeTarget = recording
                    ? Math.max(0.10f, Math.min(1.0f, targetLevel * 2.6f))
                    : (busy ? 0.35f : 0.08f);
            float alpha = activeTarget > smoothLevel
                    ? Math.min(1.0f, dt * 24.0f)
                    : Math.min(1.0f, dt * 9.0f);
            smoothLevel += (activeTarget - smoothLevel) * alpha;

            spinnerAngle = (spinnerAngle + dt * 330.0f) % 360.0f;

            for (int i = 0; i < BAR_COUNT; i++) {
                ordered[i] = history[(historyHead + i) % BAR_COUNT];
            }

            invalidate();
            Choreographer.getInstance().postFrameCallback(this);
        }

        /** Builds a true continuous-curvature squircle path inside {@code rect}. */
        private static void buildSquirclePath(Path path, RectF rect, float radius) {
            path.reset();
            float w = rect.width();
            float h = rect.height();
            float r = Math.min(radius, Math.min(w, h) * 0.46f);
            float c = r * 0.12f;
            float l = rect.left;
            float t = rect.top;
            float ri = rect.right;
            float b = rect.bottom;

            path.moveTo(l + r, t);
            path.lineTo(ri - r, t);
            path.cubicTo(ri - c, t, ri, t + c, ri, t + r);
            path.lineTo(ri, b - r);
            path.cubicTo(ri, b - c, ri - c, b, ri - r, b);
            path.lineTo(l + r, b);
            path.cubicTo(l + c, b, l, b - c, l, b - r);
            path.lineTo(l, t + r);
            path.cubicTo(l, t + c, l + c, t, l + r, t);
            path.close();
        }

        private void drawRoundSpinner(Canvas canvas, float cx, float cy, float radius) {
            spinnerRect.set(cx - radius, cy - radius, cx + radius, cy + radius);
            canvas.drawOval(spinnerRect, spinnerTrackPaint);
            canvas.drawArc(spinnerRect, spinnerAngle, 250f, false, spinnerArcPaint);
        }

        private float barNorm(int i) {
            float v;
            if (recording) {
                v = ordered[Math.max(0, Math.min(BAR_COUNT - 1, i))];
                v = Math.max(0.12f, Math.min(1.0f, v));
            } else if (busy) {
                v = 0.28f + 0.18f * (float) Math.abs(Math.sin(i * 0.9f));
            } else {
                v = 0.14f + 0.08f * (float) Math.abs(Math.sin(i * 0.8f));
            }
            return v;
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float w = getWidth();
            float h = getHeight();
            if (w <= 0 || h <= 0) {
                return;
            }

            cardRect.set(0f, 0f, w, h);

            float radius = collapsedRadius + (expandedRadius - collapsedRadius) * expandProgress;
            buildSquirclePath(squirclePath, cardRect, radius);

            // Translucent #181818 dark-grey squircle body.
            int alpha = (int) (0xBF + (0xF2 - 0xBF) * expandProgress);
            bgPaint.setColor((alpha << 24) | 0x181818);
            canvas.drawPath(squirclePath, bgPaint);

            if (expandProgress < 0.35f) {
                drawCollapsedSquircle(canvas, w, h);
            } else {
                drawExpandedSquircle(canvas, w, h, (expandProgress - 0.35f) / 0.65f);
            }
        }

        private void drawCollapsedSquircle(Canvas canvas, float w, float h) {
            if (busy) {
                drawRoundSpinner(canvas, w * 0.5f, h * 0.5f, dp(getContext(), 10.5f));
                return;
            }
            float barW = dp(getContext(), 2.8f);
            float gap = dp(getContext(), 3.2f);
            float totalW = COLLAPSED_BARS * barW + (COLLAPSED_BARS - 1) * gap;
            float startX = (w - totalW) * 0.5f;
            float maxBarH = h * 0.52f;
            float centerY = h * 0.5f;

            for (int i = 0; i < COLLAPSED_BARS; i++) {
                int srcIdx = (i * (BAR_COUNT - 1)) / (COLLAPSED_BARS - 1);
                float norm = barNorm(srcIdx);
                float bh = Math.max(barW * 1.4f, norm * maxBarH);
                float x = startX + i * (barW + gap);

                barRect.set(x, centerY - bh * 0.5f, x + barW, centerY + bh * 0.5f);
                canvas.drawRoundRect(barRect, barW * 0.5f, barW * 0.5f, wavePaint);
            }
        }

        private void drawExpandedSquircle(Canvas canvas, float w, float h, float contentAlpha) {
            float padLeft = dp(getContext(), 14f);
            float centerY = h * 0.5f;

            float btnH = dp(getContext(), 32f);
            float acceptRight = w - dp(getContext(), 10f);
            float acceptW = dp(getContext(), 46f);
            acceptBtnRect.set(acceptRight - acceptW, centerY - btnH * 0.5f, acceptRight, centerY + btnH * 0.5f);
            float discardRight = acceptBtnRect.left - dp(getContext(), 6f);
            float discardW = dp(getContext(), 46f);
            discardBtnRect.set(discardRight - discardW, centerY - btnH * 0.5f, discardRight, centerY + btnH * 0.5f);

            float waveRight = discardBtnRect.left - dp(getContext(), 10f);

            if (busy) {
                float spinR = dp(getContext(), 9.5f);
                float spinCx = padLeft + spinR + dp(getContext(), 2f);
                drawRoundSpinner(canvas, spinCx, centerY, spinR);

                String low = stateLabel.toLowerCase();
                String shortStatus = "Transcribing…";
                if (low.contains("onload")) {
                    shortStatus = "Onloading…";
                } else if (low.contains("offload")) {
                    shortStatus = "Offloading…";
                } else if (low.contains("load")) {
                    shortStatus = "Loading…";
                }
                Paint statusPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
                statusPaint.setColor(0xFFE5E5E5);
                statusPaint.setTextSize(TypedValue.applyDimension(
                        TypedValue.COMPLEX_UNIT_SP, 11f, getResources().getDisplayMetrics()));
                statusPaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
                Paint.FontMetrics m = statusPaint.getFontMetrics();
                float sBase = centerY - (m.ascent + m.descent) * 0.5f;
                canvas.drawText(shortStatus, spinCx + spinR + dp(getContext(), 8f), sBase, statusPaint);
            } else {
                float waveRegionW = Math.max(dp(getContext(), 54f), waveRight - padLeft);
                float barW = dp(getContext(), 2.9f);
                float gap = (waveRegionW - BAR_COUNT * barW) / Math.max(1, BAR_COUNT - 1);
                float maxBarH = h * 0.56f;

                for (int i = 0; i < BAR_COUNT; i++) {
                    float norm = barNorm(i);
                    float bh = Math.max(barW, norm * maxBarH);
                    float x = padLeft + i * (barW + gap);

                    barRect.set(x - 0.8f, centerY - bh * 0.56f, x + barW + 0.8f, centerY + bh * 0.56f);
                    canvas.drawRoundRect(barRect, barW, barW, subWavePaint);

                    barRect.set(x, centerY - bh * 0.5f, x + barW, centerY + bh * 0.5f);
                    canvas.drawRoundRect(barRect, barW * 0.5f, barW * 0.5f, wavePaint);
                }
            }

            int alphaInt = Math.min(255, Math.max(0, Math.round(contentAlpha * 255f)));
            int acceptAlpha = busy ? Math.round(alphaInt * 0.30f) : alphaInt;

            drawDiscardButton(canvas, discardBtnRect, alphaInt);
            drawAcceptButton(canvas, acceptBtnRect, acceptAlpha);
        }

        private void drawDiscardButton(Canvas canvas, RectF r, int alpha) {
            Path chip = squircleForGlyph(r);
            chipPaint.setColor(0xFF303036);
            chipPaint.setAlpha(Math.min(alpha, 235));
            canvas.drawPath(chip, chipPaint);

            float cx = r.centerX();
            float cy = r.centerY();
            float half = dp(getContext(), 7.2f);
            glyphPaint.setAlpha(alpha);
            glyphPaint.setStrokeWidth(dp(getContext(), 2.3f));
            glyphPath.reset();
            glyphPath.moveTo(cx - half, cy - half);
            glyphPath.lineTo(cx + half, cy + half);
            glyphPath.moveTo(cx + half, cy - half);
            glyphPath.lineTo(cx - half, cy + half);
            canvas.drawPath(glyphPath, glyphPaint);
        }

        private void drawAcceptButton(Canvas canvas, RectF r, int alpha) {
            Path chip = squircleForGlyph(r);
            chipPaint.setColor(0xFF2563EB);
            chipPaint.setAlpha(Math.min(alpha, 245));
            canvas.drawPath(chip, chipPaint);

            float cx = r.centerX();
            float cy = r.centerY();
            float hw = dp(getContext(), 8.5f);
            float hh = dp(getContext(), 6.2f);
            glyphPaint.setAlpha(alpha);
            glyphPaint.setStrokeWidth(dp(getContext(), 2.4f));
            glyphPath.reset();
            glyphPath.moveTo(cx - hw, cy + hh * 0.15f);
            glyphPath.lineTo(cx - hw * 0.28f, cy + hh);
            glyphPath.lineTo(cx + hw, cy - hh);
            canvas.drawPath(glyphPath, glyphPaint);
        }

        private final Path scratchPath = new Path();

        private Path squircleForGlyph(RectF r) {
            buildSquirclePath(scratchPath, r, dp(getContext(), 8f));
            return scratchPath;
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            float rawX = event.getRawX();
            float rawY = event.getRawY();
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downRawX = rawX;
                    downRawY = rawY;
                    lastRawX = rawX;
                    lastRawY = rawY;
                    dragged = false;
                    return true;

                case MotionEvent.ACTION_MOVE:
                    float totalDx = Math.abs(rawX - downRawX);
                    float totalDy = Math.abs(rawY - downRawY);
                    if (totalDx > dp(getContext(), 6f) || totalDy > dp(getContext(), 6f)) {
                        dragged = true;
                    }
                    if (dragged) {
                        int stepX = Math.round(rawX - lastRawX);
                        int stepY = Math.round(rawY - lastRawY);
                        lastRawX = rawX;
                        lastRawY = rawY;
                        moveWindowBy(stepX, stepY);
                    }
                    return true;

                case MotionEvent.ACTION_UP:
                    if (!dragged) {
                        handleTap(event.getX(), event.getY());
                    }
                    return true;

                default:
                    return false;
            }
        }

        private void handleTap(float x, float y) {
            if (!expanded) {
                expanded = true;
                startAnimation();
                if (!recording && !busy) {
                    dispatchAction(ACTION_RECORD);
                }
                return;
            }

            if (discardBtnRect.contains(x, y)) {
                userDismissedUntilImeReset = true;
                hidePillOnlyOnMain();
                dispatchAction(ACTION_DISCARD);
                return;
            }
            if (acceptBtnRect.contains(x, y)) {
                if (!busy) {
                    dispatchAction(ACTION_RECORD);
                }
                return;
            }
            expanded = false;
            startAnimation();
        }
    }
}
