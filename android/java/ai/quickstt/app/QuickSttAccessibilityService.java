package ai.quickstt.app;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.AccessibilityServiceInfo;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.TextUtils;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityWindowInfo;

import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Optional sideload-only "keyboard companion" (NO Play Store build target).
 *
 * <p>When the user explicitly enables this service in Android Accessibility
 * settings, it:
 * <ul>
 *   <li>watches for a real input-method window ({@code TYPE_INPUT_METHOD},
 *       i.e. Gboard / Samsung Keyboard / any keyboard actually on screen)
 *       together with a focused editable node, and only then shows a small
 *       mic pill (an accessibility overlay — no "draw over apps" needed),</li>
 *   <li>starts/stops QuickSTT dictation through the same native bubble
 *       action used by the floating pill,</li>
 *   <li>inserts the finished transcript directly into the focused editable
 *       node with {@code ACTION_SET_TEXT} (selection-aware append), falling
 *       back to clipboard + {@code ACTION_PASTE}. Password nodes are refused:
 *       nothing is inserted, copied or stored for them by this path.</li>
 * </ul>
 *
 * <p>The pill can be dragged; dragging it to the bottom-centre ✕ zone (or
 * simply dragging it into the bottom strip) hides it until the keyboard is
 * dismissed and shown again. Re-opening QuickSTT also re-arms it.
 *
 * <p>This service is OFF until the user turns it on. Android 13+ may first
 * require App info → ⋮ → "Allow restricted settings" for sideloaded APKs.
 */
public final class QuickSttAccessibilityService extends AccessibilityService {

    private static final String TAG = "QuickSTT";

    /** WindowManager type for accessibility overlays (no overlay permission). */
    private static final int TYPE_ACCESSIBILITY_OVERLAY = 2032;

    private static volatile QuickSttAccessibilityService instance;
    private static volatile boolean pillRecording = false;
    private static volatile boolean pillBusy = false;

    private final Handler main = new Handler(Looper.getMainLooper());

    private WindowManager wm;
    private PillView pill;
    private WindowManager.LayoutParams pillParams;
    private boolean pillShown = false;
    /** Hidden by drag-dismiss; stays hidden until the keyboard cycles. */
    private boolean suppressedUntilKeyboardHidden = false;
    private boolean keyboardVisible = false;

    // ── Lifecycle ────────────────────────────────────────────────────────

    @Override
    public void onServiceConnected() {
        super.onServiceConnected();
        instance = this;
        try {
            AccessibilityServiceInfo info = getServiceInfo();
            if (info != null) {
                info.eventTypes = AccessibilityEvent.TYPE_WINDOWS_CHANGED
                        | AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED
                        | AccessibilityEvent.TYPE_VIEW_FOCUSED
                        | AccessibilityEvent.TYPE_VIEW_TEXT_CHANGED
                        | AccessibilityEvent.TYPE_VIEW_CLICKED;
                info.feedbackType = AccessibilityServiceInfo.FEEDBACK_GENERIC;
                info.flags = AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
                        | AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS;
                info.notificationTimeout = 60;
                setServiceInfo(info);
            }
        } catch (Throwable t) {
            Log.w(TAG, "accessibility service info setup failed", t);
        }
        Overlay.initNative(this);
        Bubble.ensureWatcher(this);
        scheduleEvaluate(120);
        Log.i(TAG, "QuickSttAccessibilityService connected");
    }

    @Override
    public void onDestroy() {
        hidePill();
        if (instance == this) {
            instance = null;
        }
        super.onDestroy();
    }

    @Override
    public boolean onUnbind(Intent intent) {
        hidePill();
        if (instance == this) {
            instance = null;
        }
        return super.onUnbind(intent);
    }

    @Override
    public void onInterrupt() {
    }

    // ── Keyboard / focus tracking ────────────────────────────────────────

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {
        if (event == null) {
            return;
        }
        int t = event.getEventType();
        if (t == AccessibilityEvent.TYPE_WINDOWS_CHANGED
                || t == AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED
                || t == AccessibilityEvent.TYPE_VIEW_FOCUSED
                || t == AccessibilityEvent.TYPE_VIEW_CLICKED
                || t == AccessibilityEvent.TYPE_VIEW_TEXT_CHANGED) {
            scheduleEvaluate(60);
        }
    }

    private void scheduleEvaluate(long delayMs) {
        main.removeCallbacks(evaluateRunnable);
        main.postDelayed(evaluateRunnable, delayMs);
    }

    private final Runnable evaluateRunnable = new Runnable() {
        @Override
        public void run() {
            evaluate();
        }
    };

    /** Show the pill exactly while a keyboard + editable field are present. */
    private void evaluate() {
        Rect kb = keyboardBounds();
        boolean kbNow = kb != null;
        if (!kbNow) {
            // Keyboard gone: re-arm drag-dismiss for the next popup.
            suppressedUntilKeyboardHidden = false;
            keyboardVisible = false;
            if (!pillRecording && !pillBusy) {
                hidePill();
            }
            return;
        }
        keyboardVisible = true;
        if (suppressedUntilKeyboardHidden) {
            return;
        }
        AccessibilityNodeInfo focus = focusedEditable();
        if (focus == null) {
            if (!pillRecording && !pillBusy) {
                hidePill();
            }
            return;
        }
        boolean password = false;
        try {
            password = focus.isPassword();
        } catch (Throwable ignored) {
        }
        recycle(focus);
        if (password) {
            // Never offer dictation-insert into password fields from here.
            if (!pillRecording && !pillBusy) {
                hidePill();
            }
            return;
        }
        showPillAbove(kb);
    }

    private Rect keyboardBounds() {
        try {
            List<AccessibilityWindowInfo> windows = getWindows();
            if (windows == null) {
                return null;
            }
            Rect best = null;
            for (AccessibilityWindowInfo w : windows) {
                if (w != null && w.getType() == AccessibilityWindowInfo.TYPE_INPUT_METHOD) {
                    Rect r = new Rect();
                    w.getBoundsInScreen(r);
                    if (r.height() > 40) {
                        best = r;
                    }
                }
            }
            return best;
        } catch (Throwable t) {
            return null;
        }
    }

    private AccessibilityNodeInfo focusedEditable() {
        try {
            AccessibilityNodeInfo n = findFocus(AccessibilityNodeInfo.FOCUS_INPUT);
            if (n != null && n.isEditable()) {
                return n;
            }
            recycle(n);
            AccessibilityNodeInfo root = getRootInActiveWindow();
            if (root == null) {
                return null;
            }
            AccessibilityNodeInfo found = findFocusedEditable(root);
            if (found != root) {
                recycle(root);
            }
            return found;
        } catch (Throwable t) {
            return null;
        }
    }

    private AccessibilityNodeInfo findFocusedEditable(AccessibilityNodeInfo node) {
        if (node == null) {
            return null;
        }
        try {
            if (node.isFocused() && node.isEditable()) {
                return node;
            }
            for (int i = 0; i < node.getChildCount(); i++) {
                AccessibilityNodeInfo child = node.getChild(i);
                AccessibilityNodeInfo hit = findFocusedEditable(child);
                if (hit != null) {
                    return hit;
                }
                recycle(child);
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    // ── Pill window (accessibility overlay) ─────────────────────────────

    private void showPillAbove(Rect kb) {
        try {
            if (pillShown) {
                positionPill(kb);
                return;
            }
            if (wm == null) {
                wm = (WindowManager) getSystemService(Context.WINDOW_SERVICE);
            }
            if (wm == null) {
                return;
            }
            if (pill == null) {
                pill = new PillView(this);
            }
            int size = Bubble.dp(this, 56f);
            WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                    size, size,
                    TYPE_ACCESSIBILITY_OVERLAY,
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                    PixelFormat.TRANSLUCENT);
            lp.gravity = Gravity.TOP | Gravity.START;
            pillParams = lp;
            positionPill(kb);
            wm.addView(pill, lp);
            pillShown = true;
        } catch (Throwable t) {
            Log.w(TAG, "accessibility pill show failed", t);
            pillShown = false;
        }
    }

    private void positionPill(Rect kb) {
        if (pillParams == null || kb == null) {
            return;
        }
        int size = Bubble.dp(this, 56f);
        int margin = Bubble.dp(this, 14f);
        int w = getResources().getDisplayMetrics().widthPixels;
        pillParams.x = Math.max(margin, w - size - margin);
        pillParams.y = Math.max(margin, kb.top - size - margin);
        if (pillShown && pill != null && wm != null) {
            try {
                wm.updateViewLayout(pill, pillParams);
            } catch (Throwable ignored) {
            }
        }
    }

    private void hidePill() {
        try {
            if (pillShown && pill != null && wm != null) {
                wm.removeView(pill);
            }
        } catch (Throwable ignored) {
        }
        pillShown = false;
    }

    /** Called when QuickSTT (the app) is opened: re-arm a dismissed pill. */
    public static void onAppOpened() {
        QuickSttAccessibilityService svc = instance;
        if (svc != null) {
            svc.main.post(new Runnable() {
                @Override
                public void run() {
                    svc.suppressedUntilKeyboardHidden = false;
                    svc.scheduleEvaluate(80);
                }
            });
        }
    }

    /** Keep the pill's recording/busy rendering in sync with the engine. */
    public static void syncState(boolean recording, boolean busy) {
        pillRecording = recording;
        pillBusy = busy;
        QuickSttAccessibilityService svc = instance;
        if (svc != null && svc.pill != null) {
            svc.pill.postInvalidateOnAnimation();
            if ((recording || busy) && svc.keyboardVisible && !svc.suppressedUntilKeyboardHidden) {
                svc.scheduleEvaluate(40);
            }
        }
    }

    // ── Static helpers used from Rust (JNI) ─────────────────────────────

    /** True when the user has enabled this accessibility service. */
    public static boolean isEnabled(Context context) {
        if (instance != null) {
            return true;
        }
        if (context == null) {
            return false;
        }
        try {
            AccessibilityManager am = (AccessibilityManager)
                    context.getSystemService(Context.ACCESSIBILITY_SERVICE);
            if (am != null) {
                List<AccessibilityServiceInfo> list =
                        am.getEnabledAccessibilityServiceList(AccessibilityServiceInfo.FEEDBACK_ALL_MASK);
                if (list != null) {
                    for (AccessibilityServiceInfo info : list) {
                        if (info != null && info.getResolveInfo() != null
                                && info.getResolveInfo().serviceInfo != null
                                && context.getPackageName().equals(
                                        info.getResolveInfo().serviceInfo.packageName)) {
                            return true;
                        }
                    }
                }
            }
        } catch (Throwable ignored) {
        }
        try {
            String enabled = Settings.Secure.getString(
                    context.getContentResolver(), Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES);
            return !TextUtils.isEmpty(enabled)
                    && enabled.contains(context.getPackageName() + "/");
        } catch (Throwable ignored) {
            return false;
        }
    }

    /** Open Android's Accessibility settings (installed-services list). */
    public static String openSettings(Context context) {
        try {
            Intent intent = new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return "";
        } catch (Throwable t) {
            return "accessibility settings could not be opened (" + t.getClass().getSimpleName() + ")";
        }
    }

    /** True when the focused editable node is a password field. */
    public static boolean isFocusedFieldSensitive() {
        QuickSttAccessibilityService svc = instance;
        if (svc == null) {
            return false;
        }
        AccessibilityNodeInfo n = svc.focusedEditable();
        if (n == null) {
            return false;
        }
        boolean pw = false;
        try {
            pw = n.isPassword();
        } catch (Throwable ignored) {
        }
        svc.recycle(n);
        return pw;
    }

    /**
     * Insert {@code text} into the focused editable field.
     *
     * @return 1 = inserted, 0 = no editable focus / failed, -1 = password
     *         field (refused — never inserted, copied or stored here).
     */
    public static int insertText(final String text) {
        QuickSttAccessibilityService svc = instance;
        if (svc == null || text == null || text.trim().isEmpty()) {
            return 0;
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return svc.insertOnMain(text.trim());
        }
        final AtomicInteger result = new AtomicInteger(0);
        final CountDownLatch latch = new CountDownLatch(1);
        svc.main.post(new Runnable() {
            @Override
            public void run() {
                try {
                    result.set(svc.insertOnMain(text.trim()));
                } catch (Throwable t) {
                    Log.w(TAG, "accessibility insert failed", t);
                } finally {
                    latch.countDown();
                }
            }
        });
        try {
            latch.await(350, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {
        }
        return result.get();
    }

    private int insertOnMain(String text) {
        AccessibilityNodeInfo node = focusedEditable();
        if (node == null) {
            return 0;
        }
        try {
            if (node.isPassword()) {
                return -1;
            }
            String existing = "";
            try {
                CharSequence cur = node.getText();
                if (cur != null) {
                    existing = cur.toString();
                }
            } catch (Throwable ignored) {
            }
            int selStart = -1;
            int selEnd = -1;
            try {
                selStart = node.getTextSelectionStart();
                selEnd = node.getTextSelectionEnd();
            } catch (Throwable ignored) {
            }
            String insert = text;
            String merged;
            int cursor;
            if (selStart >= 0 && selEnd >= selStart && selStart <= existing.length()) {
                int end = Math.min(selEnd, existing.length());
                String before = existing.substring(0, selStart);
                String after = existing.substring(end);
                if (!before.isEmpty() && !Character.isWhitespace(before.charAt(before.length() - 1))) {
                    insert = " " + insert;
                }
                merged = before + insert + after;
                cursor = before.length() + insert.length();
            } else {
                if (!existing.isEmpty()
                        && !Character.isWhitespace(existing.charAt(existing.length() - 1))) {
                    insert = " " + insert;
                }
                merged = existing + insert;
                cursor = merged.length();
            }

            Bundle args = new Bundle();
            args.putCharSequence(
                    AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, merged);
            boolean ok = false;
            try {
                ok = node.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args);
            } catch (Throwable ignored) {
            }
            if (ok) {
                try {
                    Bundle sel = new Bundle();
                    sel.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, cursor);
                    sel.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, cursor);
                    node.performAction(AccessibilityNodeInfo.ACTION_SET_SELECTION, sel);
                } catch (Throwable ignored) {
                }
                return 1;
            }
            // Fallback: clipboard + ACTION_PASTE at the cursor.
            try {
                ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
                if (cm != null) {
                    cm.setPrimaryClip(ClipData.newPlainText("QuickSTT", text));
                    node.performAction(AccessibilityNodeInfo.ACTION_FOCUS);
                    return node.performAction(AccessibilityNodeInfo.ACTION_PASTE) ? 1 : 0;
                }
            } catch (Throwable ignored) {
            }
            return 0;
        } finally {
            recycle(node);
        }
    }

    private void recycle(AccessibilityNodeInfo n) {
        if (n != null) {
            try {
                n.recycle();
            } catch (Throwable ignored) {
            }
        }
    }

    // ── Pill view ─────────────────────────────────────────────────────────

    private final class PillView extends View {
        private final Paint bg = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint bar = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();

        private float downRawX;
        private float downRawY;
        private float lastRawX;
        private float lastRawY;
        private boolean dragged;

        PillView(Context context) {
            super(context);
            bg.setStyle(Paint.Style.FILL);
            bar.setStyle(Paint.Style.FILL);
            bar.setColor(0xFFFFFFFF);
            ring.setStyle(Paint.Style.STROKE);
            ring.setStrokeWidth(Bubble.dp(context, 1f));
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float w = getWidth();
            float h = getHeight();
            if (w <= 0 || h <= 0) {
                return;
            }
            rect.set(1f, 1f, w - 1f, h - 1f);
            float r = Bubble.dp(getContext(), 16f);
            bg.setColor(pillRecording ? 0xE6B3261E : 0xD9181818);
            canvas.drawRoundRect(rect, r, r, bg);
            ring.setColor(0x40FFFFFF);
            canvas.drawRoundRect(rect, r, r, ring);

            // Five waveform bars (same language as the floating pill).
            float barW = Bubble.dp(getContext(), 3f);
            float gap = Bubble.dp(getContext(), 3.4f);
            float total = 5 * barW + 4 * gap;
            float x = (w - total) / 2f;
            float cy = h / 2f;
            float[] hs = {0.34f, 0.62f, 0.92f, 0.62f, 0.34f};
            for (int i = 0; i < 5; i++) {
                float bh = h * hs[i] * (pillBusy ? 0.55f : 0.52f);
                rect.set(x + i * (barW + gap), cy - bh / 2f, x + i * (barW + gap) + barW, cy + bh / 2f);
                canvas.drawRoundRect(rect, barW / 2f, barW / 2f, bar);
            }
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
                    if (Math.abs(rawX - downRawX) > Bubble.dp(getContext(), 6f)
                            || Math.abs(rawY - downRawY) > Bubble.dp(getContext(), 6f)) {
                        dragged = true;
                    }
                    if (dragged && pillParams != null && wm != null) {
                        pillParams.x += Math.round(rawX - lastRawX);
                        pillParams.y += Math.round(rawY - lastRawY);
                        lastRawX = rawX;
                        lastRawY = rawY;
                        try {
                            wm.updateViewLayout(this, pillParams);
                        } catch (Throwable ignored) {
                        }
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                    if (dragged) {
                        // Drag to the bottom strip = dismiss until the
                        // keyboard is hidden and shown again.
                        int screenH = getResources().getDisplayMetrics().heightPixels;
                        if (rawY > screenH - Bubble.dp(getContext(), 110f)) {
                            suppressedUntilKeyboardHidden = true;
                            hidePill();
                        }
                    } else {
                        Bubble.dispatchAction(Bubble.ACTION_RECORD);
                    }
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    return true;
                default:
                    return false;
            }
        }
    }
}
