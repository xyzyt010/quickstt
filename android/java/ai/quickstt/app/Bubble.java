package ai.quickstt.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
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
 * Always-on live expandable floating overlay for QuickSTT.
 *
 * <ul>
 *   <li><b>Collapsed state:</b> Sleek 48dp rounded-corner icon (14dp corner radius)
 *       with a deep black background, subtle purple highlight border, and a live
 *       5-bar white waveform (no microphone icons).
 *   <li><b>Expanded state (on press):</b> Smoothly morphs into a circular stadium
 *       pill (54dp height, 27dp radius) with a pure black background, slight
 *       purple ambient highlights, and a 17-bar 60/120fps super-smooth white
 *       waveform driven by real-time microphone audio levels.
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
     * Show (or update in-place) the floating bubble on the main UI thread.
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

            // Clean up any stale view reference before adding a fresh one
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

    /** Feed live microphone RMS level (0..1) and recording/busy flags into the waveform. */
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

    /** Expand or collapse the bubble programmatically (e.g. for testing from the app). */
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
     * Custom hardware-accelerated overlay view rendering:
     * 1) Collapsed 48dp rounded-corner icon (14dp radius) with 5 white waveform bars
     * 2) Expanded 236x54dp circular stadium pill (27dp radius) with black background,
     *    slight purple highlights, 17 super-smooth white waveform bars, and TYPE/collapse controls.
     */
    private static final class BubbleView extends View implements Choreographer.FrameCallback {

        private static final int BAR_COUNT = 17;
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
        private final Paint subTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        private final RectF cardRect = new RectF();
        private final RectF barRect = new RectF();
        private final RectF chipRect = new RectF();
        private final RectF closeRect = new RectF();

        private final float[] barHeights = new float[BAR_COUNT];
        private final float[] barVelocities = new float[BAR_COUNT];

        private boolean expanded = false;
        private float expandProgress = 0.0f; // 0 = rounded-corner icon, 1 = circular pill
        private boolean recording = false;
        private boolean busy = false;
        private boolean hasTranscript = false;
        private float targetLevel = 0.0f;
        private float smoothLevel = 0.0f;
        private float phase = 0.0f;
        private long lastFrameNanos = 0L;
        private boolean animating = false;

        private String stateLabel = "Ready";
        private String transcriptPreview = "";

        private float downRawX;
        private float downRawY;
        private float lastRawX;
        private float lastRawY;
        private boolean dragged;

        BubbleView(Context context) {
            super(new ContextThemeWrapper(context, android.R.style.Theme_DeviceDefault));
            collapsedSize = dp(context, 48f);
            expandedWidth = dp(context, 236f);
            expandedHeight = dp(context, 54f);
            collapsedRadius = dp(context, 14f);
            expandedRadius = expandedHeight * 0.5f;

            bgPaint.setStyle(Paint.Style.FILL);
            bgPaint.setColor(0xF6050508);

            glowPaint.setStyle(Paint.Style.FILL);

            borderPaint.setStyle(Paint.Style.STROKE);
            borderPaint.setStrokeWidth(dp(context, 1.35f));

            wavePaint.setStyle(Paint.Style.FILL);
            wavePaint.setColor(0xFFFFFFFF);

            subWavePaint.setStyle(Paint.Style.FILL);
            subWavePaint.setColor(0x44C084FC);

            chipPaint.setStyle(Paint.Style.FILL);
            chipBorderPaint.setStyle(Paint.Style.STROKE);
            chipBorderPaint.setStrokeWidth(dp(context, 1f));

            textPaint.setColor(0xFFFFFFFF);
            textPaint.setTextSize(TypedValue.applyDimension(
                    TypedValue.COMPLEX_UNIT_SP, 10.5f, context.getResources().getDisplayMetrics()));
            textPaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
            textPaint.setTextAlign(Paint.Align.CENTER);

            subTextPaint.setColor(0xFFD8B4FE);
            subTextPaint.setTextSize(TypedValue.applyDimension(
                    TypedValue.COMPLEX_UNIT_SP, 12f, context.getResources().getDisplayMetrics()));
            subTextPaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
            subTextPaint.setTextAlign(Paint.Align.CENTER);

            for (int i = 0; i < BAR_COUNT; i++) {
                barHeights[i] = 0.18f;
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
            String t = transcript != null ? transcript.trim() : "";
            this.stateLabel = s.isEmpty() ? "Ready" : s;
            this.transcriptPreview = t;
            this.hasTranscript = !t.isEmpty();

            String low = s.toLowerCase();
            if (low.contains("listen") || low.contains("record")) {
                this.recording = true;
                this.busy = false;
                this.expanded = true;
            } else if (low.contains("transcrib") || low.contains("process") || low.contains("finish")) {
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

            // Smooth morph between collapsed rounded-corner icon (0) and circular pill (1)
            float targetExpand = expanded ? 1.0f : 0.0f;
            float expandSpeed = expanded ? 14.0f : 12.0f;
            expandProgress += (targetExpand - expandProgress) * Math.min(1.0f, dt * expandSpeed);
            if (Math.abs(targetExpand - expandProgress) < 0.002f) {
                expandProgress = targetExpand;
            }

            int curW = Math.round(collapsedSize + (expandedWidth - collapsedSize) * expandProgress);
            int curH = Math.round(collapsedSize + (expandedHeight - collapsedSize) * expandProgress);
            resizeWindow(curW, curH);

            // Smooth audio envelope (fast attack, silky release)
            float activeTarget = recording
                    ? Math.max(0.14f, Math.min(1.0f, targetLevel * 2.6f))
                    : (busy ? 0.42f : 0.08f);
            float alpha = activeTarget > smoothLevel
                    ? Math.min(1.0f, dt * 22.0f)
                    : Math.min(1.0f, dt * 8.5f);
            smoothLevel += (activeTarget - smoothLevel) * alpha;

            // Advance waveform phase smoothly
            float phaseSpeed = recording ? (4.8f + smoothLevel * 5.2f) : (busy ? 6.4f : 2.1f);
            phase += dt * phaseSpeed;
            if (phase > 628.3185f) {
                phase -= 628.3185f;
            }

            // Update each bar with spring-smoothed multi-harmonic wave motion
            float center = (BAR_COUNT - 1) * 0.5f;
            for (int i = 0; i < BAR_COUNT; i++) {
                float dist = Math.abs(i - center) / center; // 0 at center, 1 at edges
                float envelope = (float) Math.cos(dist * 1.28f); // bell envelope
                envelope = Math.max(0.18f, envelope * envelope);

                float w1 = (float) Math.sin(phase + i * 0.52f);
                float w2 = (float) Math.cos(phase * 0.68f - i * 0.37f);
                float w3 = (float) Math.sin(phase * 1.43f + i * 0.91f);
                float wave = 0.5f + 0.26f * w1 + 0.16f * w2 + 0.08f * w3;

                float desired;
                if (recording) {
                    desired = 0.14f + envelope * (0.16f + smoothLevel * 0.84f) * (0.45f + 0.55f * wave);
                } else if (busy) {
                    float ripple = 0.5f + 0.5f * (float) Math.sin(phase * 1.5f - i * 0.45f);
                    desired = 0.15f + envelope * 0.58f * ripple;
                } else {
                    float breath = 0.5f + 0.5f * (float) Math.sin(phase - i * 0.4f);
                    desired = 0.14f + envelope * 0.22f * breath;
                }
                desired = Math.max(0.12f, Math.min(1.0f, desired));

                // Critically damped spring interpolation for ultra-smooth bar motion
                float stiffness = recording ? 26.0f : 16.0f;
                barHeights[i] += (desired - barHeights[i]) * Math.min(1.0f, dt * stiffness);
            }

            invalidate();
            Choreographer.getInstance().postFrameCallback(this);
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

            // Morph corner radius from rounded-corner icon (14dp) to circular stadium pill (h/2)
            float radius = collapsedRadius + (expandedRadius - collapsedRadius) * expandProgress;

            // 1. Deep pure black background (#F6050508)
            canvas.drawRoundRect(cardRect, radius, radius, bgPaint);

            // 2. Slight purple ambient highlight glow inside the black pill
            float glowIntensity = 0.16f + 0.22f * smoothLevel + (recording ? 0.10f : 0.0f);
            int purpleCore = (((int) (glowIntensity * 255)) << 24) | 0x9333EA;
            int purpleMid = (((int) (glowIntensity * 130)) << 24) | 0x7C3AED;
            RadialGradient radial = new RadialGradient(
                    w * (0.35f + 0.12f * (float) Math.sin(phase * 0.35f)),
                    h * 0.5f,
                    Math.max(w, h) * 0.75f,
                    new int[]{purpleCore, purpleMid, 0x00050508},
                    new float[]{0f, 0.55f, 1f},
                    Shader.TileMode.CLAMP);
            glowPaint.setShader(radial);
            canvas.drawRoundRect(cardRect, radius, radius, glowPaint);

            // 3. Slight purple highlight border rim
            int rimAlpha = recording ? 0xD8 : 0x96;
            int rimBright = (rimAlpha << 24) | 0xA855F7;
            int rimDeep = ((rimAlpha - 0x28) << 24) | 0x6D28D9;
            int rimSoft = (rimAlpha << 24) | 0xC084FC;
            borderPaint.setShader(new LinearGradient(
                    0f, 0f, w, h,
                    new int[]{rimBright, rimDeep, rimSoft},
                    new float[]{0f, 0.5f, 1f},
                    Shader.TileMode.CLAMP));
            canvas.drawRoundRect(cardRect, radius, radius, borderPaint);

            // 4. Super-smooth White Waveforms (no mic icons)
            if (expandProgress < 0.35f) {
                drawCollapsedWaveform(canvas, w, h);
            } else {
                drawExpandedPill(canvas, w, h, (expandProgress - 0.35f) / 0.65f);
            }
        }

        private void drawCollapsedWaveform(Canvas canvas, float w, float h) {
            float barW = dp(getContext(), 2.8f);
            float gap = dp(getContext(), 3.2f);
            float totalW = COLLAPSED_BARS * barW + (COLLAPSED_BARS - 1) * gap;
            float startX = (w - totalW) * 0.5f;
            float maxBarH = h * 0.52f;
            float centerY = h * 0.5f;

            for (int i = 0; i < COLLAPSED_BARS; i++) {
                int srcIdx = (i * (BAR_COUNT - 1)) / (COLLAPSED_BARS - 1);
                float norm = barHeights[srcIdx];
                float bh = Math.max(barW, norm * maxBarH);
                float x = startX + i * (barW + gap);

                // Subtle purple under-glow bar
                barRect.set(x - 0.8f, centerY - bh * 0.58f, x + barW + 0.8f, centerY + bh * 0.58f);
                canvas.drawRoundRect(barRect, barW, barW, subWavePaint);

                // Crisp pure white waveform bar
                barRect.set(x, centerY - bh * 0.5f, x + barW, centerY + bh * 0.5f);
                canvas.drawRoundRect(barRect, barW * 0.5f, barW * 0.5f, wavePaint);
            }
        }

        private void drawExpandedPill(Canvas canvas, float w, float h, float contentAlpha) {
            float padLeft = dp(getContext(), 16f);
            float closeSize = dp(getContext(), 26f);
            float closeRight = w - dp(getContext(), 10f);
            float closeLeft = closeRight - closeSize;
            float centerY = h * 0.5f;

            closeRect.set(closeLeft, centerY - closeSize * 0.5f, closeRight, centerY + closeSize * 0.5f);

            float chipW = dp(getContext(), 50f);
            float chipH = dp(getContext(), 28f);
            float chipRight = closeLeft - dp(getContext(), 6f);
            float chipLeft = chipRight - chipW;
            chipRect.set(chipLeft, centerY - chipH * 0.5f, chipRight, centerY + chipH * 0.5f);

            // Waveform region occupies the left & center of the circular pill
            float waveRight = chipLeft - dp(getContext(), 10f);
            float waveRegionW = Math.max(dp(getContext(), 60f), waveRight - padLeft);
            float barW = dp(getContext(), 2.6f);
            float gap = (waveRegionW - BAR_COUNT * barW) / Math.max(1, BAR_COUNT - 1);
            float maxBarH = h * 0.62f;

            for (int i = 0; i < BAR_COUNT; i++) {
                float norm = barHeights[i];
                float bh = Math.max(barW, norm * maxBarH);
                float x = padLeft + i * (barW + gap);

                // Slight purple halo on taller bars
                barRect.set(x - 0.8f, centerY - bh * 0.56f, x + barW + 0.8f, centerY + bh * 0.56f);
                canvas.drawRoundRect(barRect, barW, barW, subWavePaint);

                // Pure white smooth waveform bar
                barRect.set(x, centerY - bh * 0.5f, x + barW, centerY + bh * 0.5f);
                canvas.drawRoundRect(barRect, barW * 0.5f, barW * 0.5f, wavePaint);
            }

            if (contentAlpha <= 0.05f) {
                return;
            }
            int alphaInt = Math.min(255, Math.max(0, Math.round(contentAlpha * 255f)));

            // Action pill button (STOP while listening, ··· while busy, TYPE when idle/ready)
            String chipLabel = recording ? "STOP" : (busy ? "···" : (hasTranscript ? "TYPE" : "LIVE"));
            chipPaint.setColor(recording ? 0x38A855F7 : (hasTranscript ? 0x2E9333EA : 0x22FFFFFF));
            chipPaint.setAlpha(Math.min(alphaInt, recording ? 95 : 70));
            canvas.drawRoundRect(chipRect, chipH * 0.5f, chipH * 0.5f, chipPaint);

            chipBorderPaint.setColor(recording ? 0xCCA855F7 : 0x77A855F7);
            chipBorderPaint.setAlpha(Math.min(alphaInt, 190));
            canvas.drawRoundRect(chipRect, chipH * 0.5f, chipH * 0.5f, chipBorderPaint);

            textPaint.setAlpha(alphaInt);
            Paint.FontMetrics fm = textPaint.getFontMetrics();
            float textBaseline = centerY - (fm.ascent + fm.descent) * 0.5f;
            canvas.drawText(chipLabel, chipRect.centerX(), textBaseline, textPaint);

            // Collapse icon (minimal chevron/cross to fold back into the rounded-corner icon)
            subTextPaint.setAlpha(Math.min(alphaInt, 200));
            Paint.FontMetrics sfm = subTextPaint.getFontMetrics();
            float subBaseline = centerY - (sfm.ascent + sfm.descent) * 0.5f;
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
                // Pressing the rounded-corner icon expands it into the circular pill
                // and immediately starts live recording!
                expanded = true;
                startAnimation();
                if (!recording && !busy) {
                    dispatchAction(ACTION_RECORD);
                }
                return;
            }

            // Inside the expanded circular pill:
            // 1. Far-right collapse zone -> fold back into the rounded-corner icon
            if (x >= closeRect.left - dp(getContext(), 4f)) {
                if (recording) {
                    dispatchAction(ACTION_RECORD);
                }
                expanded = false;
                startAnimation();
                return;
            }

            // 2. Action chip zone -> STOP if recording, TYPE if transcript is ready, else RECORD
            if (x >= chipRect.left - dp(getContext(), 4f) && x <= chipRect.right + dp(getContext(), 4f)) {
                if (recording) {
                    dispatchAction(ACTION_RECORD);
                } else if (hasTranscript && !busy) {
                    dispatchAction(ACTION_TYPE);
                } else if (!busy) {
                    dispatchAction(ACTION_RECORD);
                }
                return;
            }

            // 3. Tapping the waveform area toggles live recording
            if (!busy) {
                dispatchAction(ACTION_RECORD);
            }
        }
    }
}
