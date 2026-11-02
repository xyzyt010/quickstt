package ai.quickstt.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Choreographer;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

/**
 * Always-on floating squircle overlay for QuickSTT.
 *
 * <ul>
 *   <li><b>Collapsed state:</b> 48dp squircle icon (14dp squircle corners) with deep
 *       navy-black surface, electric blue border, and 5 white waveform bars (or a round
 *       spinning loader while onloading/offloading).
 *   <li><b>Expanded state:</b> 248x54dp squircle bar (15dp squircle corners — never an
 *       over-rounded stadium pill) with live waveform, round circular loading animation
 *       for automatic model onload/offload/transcribe, and an explicit <b>START</b> /
 *       <b>STOP</b> button.
 * </ul>
 */
public final class Bubble {

    private static final String TAG = "QuickSTT";

    /** Start or stop the recording. */
    public static final int ACTION_RECORD = 1;
    /** Type the last transcript into the app that has the focus. */
    public static final int ACTION_TYPE = 2;
    /** Hide the bubble completely. */
    public static final int ACTION_CLOSE = 3;

    /** TYPE_APPLICATION_OVERLAY: drawn above other apps on Android 8+. */
    private static final int TYPE_APPLICATION_OVERLAY = 2038;

    /** Keep keyboard focus in the underlying app while receiving touch inside the bubble. */
    private static final int FLAGS = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
            | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;

    private static BubbleView current;
    private static WindowManager.LayoutParams currentParams;
    private static WindowManager currentManager;
    private static int savedX = -1;
    private static int savedY = -1;

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

    /**
     * Show (or update in-place) the floating squircle pill on the main UI thread.
     *
     * @return an empty string on success, otherwise the reason it failed.
     */
    public static String show(Context context, String state, String transcript) {
        try {
            if (!Overlay.granted(context)) {
                return "the “display over other apps” permission is missing";
            }

            if (current != null && currentManager != null && current.isAttachedToWindow()) {
                current.setStateText(state, transcript);
                return "";
            }

            hide(context);

            WindowManager manager = resolveWindowManager(context);
            if (manager == null) {
                return "the window manager is not available";
            }

            BubbleView view = new BubbleView(context.getApplicationContext() != null
                    ? context.getApplicationContext() : context);
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
            params.x = savedX >= 0 ? savedX : dp(context, 18);
            params.y = savedY >= 0 ? savedY : dp(context, 128);

            manager.addView(view, params);

            current = view;
            currentParams = params;
            currentManager = manager;
            return "";
        } catch (Throwable t) {
            Log.e(TAG, "Bubble.show failed", t);
            return describe(t);
        }
    }

    /** Remove the bubble from the screen. */
    public static String hide(Context context) {
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

    /** Update the bubble state & transcript without recreating the window. */
    public static String update(String state, String transcript) {
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
        final BubbleView view = current;
        if (view == null) {
            return;
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            view.updateLiveLevel(level, recording, busy);
        } else {
            view.post(new Runnable() {
                @Override
                public void run() {
                    view.updateLiveLevel(level, recording, busy);
                }
            });
        }
    }

    /** Expand or collapse the bubble programmatically. */
    public static void setExpanded(final boolean expanded) {
        final BubbleView view = current;
        if (view == null) {
            return;
        }
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                view.setExpanded(expanded);
            }
        });
    }

    private static WindowManager resolveWindowManager(Context context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                Context windowCtx = context.createWindowContext(TYPE_APPLICATION_OVERLAY, null);
                WindowManager wm = (WindowManager) windowCtx.getSystemService(Context.WINDOW_SERVICE);
                if (wm != null) {
                    return wm;
                }
            } catch (Throwable ignored) {
            }
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
     * Custom hardware-accelerated squircle overlay view:
     * 1) Collapsed 48dp squircle icon (14dp squircle radius) with 5 white bars or round spinner.
     * 2) Expanded 248x54dp squircle bar (15dp squircle radius) with blue accents, round
     *    onload/offload/transcribe spinner, and explicit START / STOP button.
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
        private final Paint glowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint wavePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint subWavePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint chipPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint chipBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint statusPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint subTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint spinnerTrackPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint spinnerArcPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        private final RectF cardRect = new RectF();
        private final RectF barRect = new RectF();
        private final RectF chipRect = new RectF();
        private final RectF closeRect = new RectF();
        private final RectF spinnerRect = new RectF();
        private final Path squirclePath = new Path();
        private final Path chipPath = new Path();

        private final float[] barHeights = new float[BAR_COUNT];

        private boolean expanded = false;
        private float expandProgress = 0.0f;
        private boolean recording = false;
        private boolean busy = false;
        private float targetLevel = 0.0f;
        private float smoothLevel = 0.0f;
        private float phase = 0.0f;
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
            // Squircle corners on both collapsed (13dp) and expanded (15dp) — never a circular stadium!
            collapsedRadius = dp(context, 13f);
            expandedRadius = dp(context, 15f);

            bgPaint.setStyle(Paint.Style.FILL);
            bgPaint.setColor(0xF6070C16);

            glowPaint.setStyle(Paint.Style.FILL);

            borderPaint.setStyle(Paint.Style.STROKE);
            borderPaint.setStrokeWidth(dp(context, 1.4f));

            wavePaint.setStyle(Paint.Style.FILL);
            wavePaint.setColor(0xFFFFFFFF);

            subWavePaint.setStyle(Paint.Style.FILL);
            subWavePaint.setColor(0x443B82F6);

            chipPaint.setStyle(Paint.Style.FILL);
            chipBorderPaint.setStyle(Paint.Style.STROKE);
            chipBorderPaint.setStrokeWidth(dp(context, 1.1f));

            textPaint.setColor(0xFFFFFFFF);
            textPaint.setTextSize(TypedValue.applyDimension(
                    TypedValue.COMPLEX_UNIT_SP, 10.5f, context.getResources().getDisplayMetrics()));
            textPaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
            textPaint.setTextAlign(Paint.Align.CENTER);

            statusPaint.setColor(0xFFE2E8F0);
            statusPaint.setTextSize(TypedValue.applyDimension(
                    TypedValue.COMPLEX_UNIT_SP, 11f, context.getResources().getDisplayMetrics()));
            statusPaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
            statusPaint.setTextAlign(Paint.Align.LEFT);

            subTextPaint.setColor(0xFF93C5FD);
            subTextPaint.setTextSize(TypedValue.applyDimension(
                    TypedValue.COMPLEX_UNIT_SP, 13f, context.getResources().getDisplayMetrics()));
            subTextPaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
            subTextPaint.setTextAlign(Paint.Align.CENTER);

            spinnerTrackPaint.setStyle(Paint.Style.STROKE);
            spinnerTrackPaint.setStrokeWidth(dp(context, 2.4f));
            spinnerTrackPaint.setColor(0x3860A5FA);

            spinnerArcPaint.setStyle(Paint.Style.STROKE);
            spinnerArcPaint.setStrokeWidth(dp(context, 2.4f));
            spinnerArcPaint.setStrokeCap(Paint.Cap.ROUND);
            spinnerArcPaint.setColor(0xFF60A5FA);

            for (int i = 0; i < BAR_COUNT; i++) {
                barHeights[i] = 0.20f;
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
                    ? Math.max(0.16f, Math.min(1.0f, targetLevel * 2.6f))
                    : (busy ? 0.35f : 0.10f);
            float alpha = activeTarget > smoothLevel
                    ? Math.min(1.0f, dt * 22.0f)
                    : Math.min(1.0f, dt * 8.5f);
            smoothLevel += (activeTarget - smoothLevel) * alpha;

            float phaseSpeed = recording ? (4.8f + smoothLevel * 5.2f) : (busy ? 5.0f : 1.8f);
            phase += dt * phaseSpeed;
            if (phase > 628.3185f) {
                phase -= 628.3185f;
            }

            spinnerAngle = (spinnerAngle + dt * 320.0f) % 360.0f;

            float center = (BAR_COUNT - 1) * 0.5f;
            for (int i = 0; i < BAR_COUNT; i++) {
                float dist = Math.abs(i - center) / center;
                float envelope = (float) Math.cos(dist * 1.25f);
                envelope = Math.max(0.20f, envelope * envelope);

                float w1 = (float) Math.sin(phase + i * 0.55f);
                float w2 = (float) Math.cos(phase * 0.70f - i * 0.38f);
                float wave = 0.5f + 0.32f * w1 + 0.18f * w2;

                float desired;
                if (recording) {
                    desired = 0.16f + envelope * (0.18f + smoothLevel * 0.82f) * (0.45f + 0.55f * wave);
                } else {
                    desired = 0.18f + envelope * 0.14f;
                }
                desired = Math.max(0.14f, Math.min(1.0f, desired));

                float stiffness = recording ? 26.0f : 14.0f;
                barHeights[i] += (desired - barHeights[i]) * Math.min(1.0f, dt * stiffness);
            }

            invalidate();
            Choreographer.getInstance().postFrameCallback(this);
        }

        /**
         * Builds a true continuous-curvature squircle path inside {@code rect}.
         */
        private static void buildSquirclePath(Path path, RectF rect, float radius) {
            path.reset();
            float w = rect.width();
            float h = rect.height();
            float r = Math.min(radius, Math.min(w, h) * 0.42f);
            // Smooth superellipse control offset (squircle factor)
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

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float w = getWidth();
            float h = getHeight();
            if (w <= 0 || h <= 0) {
                return;
            }

            float strokeHalf = borderPaint.getStrokeWidth() * 0.5f + 1f;
            cardRect.set(strokeHalf, strokeHalf, w - strokeHalf, h - strokeHalf);

            float radius = collapsedRadius + (expandedRadius - collapsedRadius) * expandProgress;
            buildSquirclePath(squirclePath, cardRect, radius);

            // 1. Deep navy-black squircle body
            canvas.drawPath(squirclePath, bgPaint);

            // 2. Subtle blue ambient glow inside the squircle
            float glowIntensity = 0.18f + 0.22f * smoothLevel + (recording ? 0.12f : 0.0f);
            int blueCore = (((int) (glowIntensity * 255)) << 24) | 0x2563EB;
            int blueMid = (((int) (glowIntensity * 135)) << 24) | 0x1D4ED8;
            RadialGradient radial = new RadialGradient(
                    w * 0.35f,
                    h * 0.5f,
                    Math.max(w, h) * 0.78f,
                    new int[]{blueCore, blueMid, 0x00070C16},
                    new float[]{0f, 0.55f, 1f},
                    Shader.TileMode.CLAMP);
            glowPaint.setShader(radial);
            canvas.drawPath(squirclePath, glowPaint);

            // 3. Crisp blue squircle border rim
            int rimAlpha = recording ? 0xEA : 0xB0;
            int rimBright = (rimAlpha << 24) | 0x3B82F6;
            int rimDeep = ((rimAlpha - 0x24) << 24) | 0x1D4ED8;
            int rimSoft = (rimAlpha << 24) | 0x60A5FA;
            borderPaint.setShader(new LinearGradient(
                    0f, 0f, w, h,
                    new int[]{rimBright, rimDeep, rimSoft},
                    new float[]{0f, 0.5f, 1f},
                    Shader.TileMode.CLAMP));
            canvas.drawPath(squirclePath, borderPaint);

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
            float maxBarH = h * 0.50f;
            float centerY = h * 0.5f;

            for (int i = 0; i < COLLAPSED_BARS; i++) {
                int srcIdx = (i * (BAR_COUNT - 1)) / (COLLAPSED_BARS - 1);
                float norm = barHeights[srcIdx];
                float bh = Math.max(barW, norm * maxBarH);
                float x = startX + i * (barW + gap);

                barRect.set(x - 0.8f, centerY - bh * 0.58f, x + barW + 0.8f, centerY + bh * 0.58f);
                canvas.drawRoundRect(barRect, barW, barW, subWavePaint);

                barRect.set(x, centerY - bh * 0.5f, x + barW, centerY + bh * 0.5f);
                canvas.drawRoundRect(barRect, barW * 0.5f, barW * 0.5f, wavePaint);
            }
        }

        private void drawExpandedSquircle(Canvas canvas, float w, float h, float contentAlpha) {
            float padLeft = dp(getContext(), 14f);
            float closeSize = dp(getContext(), 24f);
            float closeRight = w - dp(getContext(), 8f);
            float closeLeft = closeRight - closeSize;
            float centerY = h * 0.5f;

            closeRect.set(closeLeft, centerY - closeSize * 0.5f, closeRight, centerY + closeSize * 0.5f);

            // Dedicated START / STOP squircle button
            float chipW = dp(getContext(), 64f);
            float chipH = dp(getContext(), 32f);
            float chipRight = closeLeft - dp(getContext(), 6f);
            float chipLeft = chipRight - chipW;
            chipRect.set(chipLeft, centerY - chipH * 0.5f, chipRight, centerY + chipH * 0.5f);

            float waveRight = chipLeft - dp(getContext(), 10f);

            if (busy) {
                // Proper round circular loading animation for Onloading / Transcribing / Offloading
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
                Paint.FontMetrics sfm = statusPaint.getFontMetrics();
                float sBase = centerY - (sfm.ascent + sfm.descent) * 0.5f;
                canvas.drawText(shortStatus, spinCx + spinR + dp(getContext(), 8f), sBase, statusPaint);
            } else {
                float waveRegionW = Math.max(dp(getContext(), 54f), waveRight - padLeft);
                float barW = dp(getContext(), 2.8f);
                float gap = (waveRegionW - BAR_COUNT * barW) / Math.max(1, BAR_COUNT - 1);
                float maxBarH = h * 0.58f;

                for (int i = 0; i < BAR_COUNT; i++) {
                    float norm = barHeights[i];
                    float bh = Math.max(barW, norm * maxBarH);
                    float x = padLeft + i * (barW + gap);

                    barRect.set(x - 0.8f, centerY - bh * 0.56f, x + barW + 0.8f, centerY + bh * 0.56f);
                    canvas.drawRoundRect(barRect, barW, barW, subWavePaint);

                    barRect.set(x, centerY - bh * 0.5f, x + barW, centerY + bh * 0.5f);
                    canvas.drawRoundRect(barRect, barW * 0.5f, barW * 0.5f, wavePaint);
                }
            }

            if (contentAlpha <= 0.05f) {
                return;
            }
            int alphaInt = Math.min(255, Math.max(0, Math.round(contentAlpha * 255f)));

            // Squircle START / STOP button (10dp squircle radius)
            buildSquirclePath(chipPath, chipRect, dp(getContext(), 9.5f));
            String chipLabel;
            if (recording) {
                chipLabel = "■ STOP";
                chipPaint.setColor(0xFFDC2626);
                chipBorderPaint.setColor(0xFFF87171);
            } else if (busy) {
                chipLabel = "WAIT";
                chipPaint.setColor(0xFF1E293B);
                chipBorderPaint.setColor(0xFF475569);
            } else {
                chipLabel = "● START";
                chipPaint.setColor(0xFF2563EB);
                chipBorderPaint.setColor(0xFF60A5FA);
            }
            chipPaint.setAlpha(Math.min(alphaInt, 235));
            canvas.drawPath(chipPath, chipPaint);

            chipBorderPaint.setAlpha(Math.min(alphaInt, 230));
            canvas.drawPath(chipPath, chipBorderPaint);

            textPaint.setAlpha(alphaInt);
            Paint.FontMetrics fm = textPaint.getFontMetrics();
            float textBaseline = centerY - (fm.ascent + fm.descent) * 0.5f;
            canvas.drawText(chipLabel, chipRect.centerX(), textBaseline, textPaint);

            // Collapse icon on far right
            subTextPaint.setAlpha(Math.min(alphaInt, 210));
            Paint.FontMetrics cfm = subTextPaint.getFontMetrics();
            float subBaseline = centerY - (cfm.ascent + cfm.descent) * 0.5f;
            canvas.drawText("‹", closeRect.centerX(), subBaseline, subTextPaint);
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

            // 1. Far-right collapse zone -> fold back into the 48dp squircle icon
            if (x >= closeRect.left - dp(getContext(), 4f)) {
                if (recording) {
                    dispatchAction(ACTION_RECORD);
                }
                expanded = false;
                startAnimation();
                return;
            }

            // 2. START / STOP button or waveform tap -> always starts or stops transcription!
            if (!busy || recording) {
                dispatchAction(ACTION_RECORD);
            }
        }
    }
}
