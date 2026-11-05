package ai.quickstt.app;

import android.content.Context;
import android.content.Intent;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.inputmethodservice.InputMethodService;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.text.TextUtils;
import android.util.Log;
import android.util.TypedValue;
import android.view.Choreographer;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodManager;

import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Standard Android Voice Input Method ({@link InputMethodService}) for QuickSTT.
 *
 * <p>Uses {@code android.permission.BIND_INPUT_METHOD} — Android's official API for
 * voice typing into focused text fields — so Google Play Protect's Enhanced Fraud
 * Protection never blocks APK installation.
 */
public final class QuickSttInputMethodService extends InputMethodService {

    private static final String TAG = "QuickSTT";

    private static volatile QuickSttInputMethodService instance;
    private static volatile boolean awaitingPickerOnResume = false;

    private static boolean turnStartedComposing = false;
    private static boolean turnNeedsLeadingSpace = false;

    private static volatile boolean liveRecording = false;
    private static volatile boolean liveBusy = false;
    private static volatile float liveLevel = 0.0f;
    private static volatile String liveStateLabel = "Ready";

    private VoiceBarView voiceBar;

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        Overlay.initNative(this);
        Bubble.ensureWatcher(this);
        Log.i(TAG, "QuickSttInputMethodService created");
    }

    @Override
    public boolean onEvaluateFullscreenMode() {
        return false;
    }

    @Override
    public boolean onEvaluateInputViewShown() {
        super.onEvaluateInputViewShown();
        return true;
    }

    @Override
    public View onCreateInputView() {
        voiceBar = new VoiceBarView(this);
        return voiceBar;
    }

    @Override
    public void onStartInput(EditorInfo attribute, boolean restarting) {
        super.onStartInput(attribute, restarting);
        instance = this;
        Overlay.initNative(this);
        Bubble.ensureWatcher(this);
        if (!restarting) {
            beginTurn();
        }
        if (isEditableEditor(attribute) || isInputViewShown()) {
            Bubble.onImeActive(this, true);
        }
    }

    @Override
    public void onStartInputView(EditorInfo info, boolean restarting) {
        super.onStartInputView(info, restarting);
        instance = this;
        Bubble.onImeActive(this, true);
    }

    @Override
    public void onWindowShown() {
        super.onWindowShown();
        instance = this;
        Bubble.onImeActive(this, true);
    }

    @Override
    public void onFinishInputView(boolean finishingInput) {
        if (finishingInput) {
            Bubble.onImeActive(this, false);
        }
        super.onFinishInputView(finishingInput);
    }

    @Override
    public void onWindowHidden() {
        Bubble.onImeActive(this, false);
        super.onWindowHidden();
    }

    @Override
    public void onFinishInput() {
        beginTurn();
        super.onFinishInput();
    }

    @Override
    public void onDestroy() {
        Bubble.onImeActive(this, false);
        if (voiceBar != null) {
            voiceBar.stopAnimation();
            voiceBar = null;
        }
        if (instance == this) {
            instance = null;
        }
        super.onDestroy();
    }

    private static boolean isEditableEditor(EditorInfo attribute) {
        if (attribute == null) {
            return false;
        }
        int cls = attribute.inputType & InputType.TYPE_MASK_CLASS;
        if (cls != InputType.TYPE_NULL) {
            return true;
        }
        return !TextUtils.isEmpty(attribute.fieldName)
                || attribute.fieldId > 0
                || !TextUtils.isEmpty(attribute.hintText);
    }

    /** Synchronize recording/loading state from Bubble into the IME voice bar if visible. */
    public static void syncState(String state, boolean recording, boolean busy, float level) {
        if (state != null && !state.trim().isEmpty()) {
            liveStateLabel = state.trim();
        }
        liveRecording = recording;
        liveBusy = busy;
        liveLevel = level;
        final QuickSttInputMethodService svc = instance;
        if (svc != null && svc.voiceBar != null) {
            svc.voiceBar.postInvalidateOnAnimation();
        }
    }

    /** True when QuickSTT Voice Auto-Type is enabled in Android's keyboard settings. */
    public static boolean isEnabled(Context context) {
        if (context == null) {
            return instance != null;
        }
        try {
            InputMethodManager imm = (InputMethodManager) context.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) {
                List<InputMethodInfo> list = imm.getEnabledInputMethodList();
                if (list != null) {
                    String pkg = context.getPackageName();
                    for (InputMethodInfo info : list) {
                        if (info != null && pkg.equals(info.getPackageName())) {
                            return true;
                        }
                    }
                }
            }
        } catch (Throwable ignored) {
        }
        try {
            String enabled = Settings.Secure.getString(
                    context.getContentResolver(),
                    Settings.Secure.ENABLED_INPUT_METHODS);
            if (!TextUtils.isEmpty(enabled) && enabled.contains(context.getPackageName())) {
                return true;
            }
        } catch (Throwable ignored) {
        }
        return false;
    }

    /** True when QuickSTT Voice Auto-Type is currently selected as the active input method. */
    public static boolean isSelected(Context context) {
        if (context == null) {
            return instance != null;
        }
        try {
            String def = Settings.Secure.getString(
                    context.getContentResolver(),
                    Settings.Secure.DEFAULT_INPUT_METHOD);
            if (!TextUtils.isEmpty(def) && def.startsWith(context.getPackageName() + "/")) {
                return true;
            }
        } catch (Throwable ignored) {
        }
        return instance != null;
    }

    /** True when QuickSTT Voice Auto-Type is both enabled in Settings and selected as active IME. */
    public static boolean isReady(Context context) {
        return isEnabled(context) && isSelected(context);
    }

    /**
     * When the user returns from Android's On-Screen Keyboard settings after enabling
     * QuickSTT Voice Auto-Type, automatically pop open the system input-method picker
     * so they can select it in one tap.
     */
    public static void maybeShowPendingPickerOnResume(final Context context) {
        if (!awaitingPickerOnResume || context == null) {
            return;
        }
        if (isEnabled(context) && !isSelected(context)) {
            awaitingPickerOnResume = false;
            new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
                @Override
                public void run() {
                    try {
                        InputMethodManager imm =
                                (InputMethodManager) context.getSystemService(Context.INPUT_METHOD_SERVICE);
                        if (imm != null) {
                            imm.showInputMethodPicker();
                        }
                    } catch (Throwable ignored) {
                    }
                }
            }, 280L);
        } else if (isSelected(context)) {
            awaitingPickerOnResume = false;
        }
    }

    /**
     * If QuickSTT Voice Auto-Type is not enabled yet, open Android's On-Screen Keyboard
     * settings; if it is already enabled, open the system input-method picker dialog so
     * the user can select it in one tap.
     */
    public static String openSettingsOrPicker(final Context context) {
        try {
            if (isEnabled(context)) {
                awaitingPickerOnResume = false;
                final InputMethodManager imm =
                        (InputMethodManager) context.getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) {
                    new Handler(Looper.getMainLooper()).post(new Runnable() {
                        @Override
                        public void run() {
                            try {
                                imm.showInputMethodPicker();
                            } catch (Throwable ignored) {
                            }
                        }
                    });
                    return "";
                }
            }
            awaitingPickerOnResume = true;
            Intent intent = new Intent(Settings.ACTION_INPUT_METHOD_SETTINGS);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return "";
        } catch (Throwable t) {
            return "keyboard settings could not be opened (" + t.getClass().getSimpleName() + ")";
        }
    }

    /**
     * Discard any live composing text that was streamed into the focused field
     * (used when the user cancels a turn with the ✕ button).
     */
    public static boolean cancelLiveTyping() {
        final QuickSttInputMethodService svc = instance;
        if (svc == null) {
            return false;
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return svc.finishCancel();
        }
        final AtomicBoolean ok = new AtomicBoolean(false);
        final CountDownLatch latch = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                try {
                    ok.set(svc.finishCancel());
                } catch (Throwable t) {
                    Log.w(TAG, "cancelLiveTyping failed", t);
                } finally {
                    latch.countDown();
                }
            }
        });
        try {
            latch.await(250, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {
        }
        return ok.get();
    }

    private synchronized boolean finishCancel() {
        if (!turnStartedComposing) {
            return false;
        }
        try {
            InputConnection ic = getCurrentInputConnection();
            if (ic != null) {
                ic.beginBatchEdit();
                ic.setComposingText("", 1);
                ic.finishComposingText();
                ic.endBatchEdit();
            }
        } catch (Throwable ignored) {
        }
        turnStartedComposing = false;
        turnNeedsLeadingSpace = false;
        return true;
    }

    /** Reset composing state at the start of a new recording turn. */
    public static synchronized void beginTurn() {
        turnStartedComposing = false;
        turnNeedsLeadingSpace = false;
    }

    /** True when an editable text box is currently focused and bound to the IME. */
    public static boolean hasFocusedTextBox() {
        final QuickSttInputMethodService svc = instance;
        if (svc == null) {
            return false;
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return svc.hasActiveEditableField();
        }
        final AtomicBoolean focused = new AtomicBoolean(false);
        final CountDownLatch latch = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                try {
                    focused.set(svc.hasActiveEditableField());
                } catch (Throwable ignored) {
                } finally {
                    latch.countDown();
                }
            }
        });
        try {
            latch.await(200, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {
        }
        return focused.get();
    }

    /**
     * Update the focused text box in real time with the current partial transcript.
     */
    public static boolean typeLivePartial(final String partialText) {
        if (partialText == null || partialText.trim().isEmpty()) {
            return false;
        }
        return applyToInputConnection(partialText.trim(), false) == 1;
    }

    /**
     * Commit the final transcript into the focused text box when recording stops.
     */
    public static boolean commitFinalText(final String finalText) {
        try {
            if (finalText == null || finalText.trim().isEmpty()) {
                return false;
            }
            return applyToInputConnection(finalText.trim(), true) == 1;
        } finally {
            beginTurn();
        }
    }

    private static int applyToInputConnection(final String transcript, final boolean commit) {
        final QuickSttInputMethodService svc = instance;
        if (svc == null) {
            return 0;
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return svc.applyOnMainThread(transcript, commit) ? 1 : 0;
        }
        final AtomicInteger result = new AtomicInteger(0);
        final CountDownLatch latch = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                try {
                    if (svc.applyOnMainThread(transcript, commit)) {
                        result.set(1);
                    }
                } catch (Throwable t) {
                    Log.w(TAG, "applyOnMainThread failed", t);
                } finally {
                    latch.countDown();
                }
            }
        });
        try {
            latch.await(300, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {
        }
        return result.get();
    }

    private boolean hasActiveEditableField() {
        try {
            InputConnection ic = getCurrentInputConnection();
            if (ic == null) {
                return false;
            }
            EditorInfo info = getCurrentInputEditorInfo();
            return isEditableEditor(info) || isInputViewShown();
        } catch (Throwable t) {
            return false;
        }
    }

    private synchronized boolean applyOnMainThread(String transcript, boolean commit) {
        InputConnection ic = getCurrentInputConnection();
        if (ic == null) {
            return false;
        }
        try {
            if (!turnStartedComposing) {
                CharSequence before = ic.getTextBeforeCursor(1, 0);
                turnNeedsLeadingSpace = before != null
                        && before.length() > 0
                        && !Character.isWhitespace(before.charAt(0));
                turnStartedComposing = true;
            }
            String display = (turnNeedsLeadingSpace ? " " : "") + transcript;
            ic.beginBatchEdit();
            boolean ok;
            if (commit) {
                ok = ic.commitText(display, 1);
                ic.finishComposingText();
                turnStartedComposing = false;
                turnNeedsLeadingSpace = false;
            } else {
                ok = ic.setComposingText(display, 1);
            }
            ic.endBatchEdit();
            return ok;
        } catch (Throwable t) {
            Log.w(TAG, "InputConnection update failed", t);
            return false;
        }
    }

    void switchBackToKeyboard() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                if (switchToPreviousInputMethod()) {
                    return;
                }
            }
        } catch (Throwable ignored) {
        }
        try {
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.showInputMethodPicker();
            }
        } catch (Throwable ignored) {
        }
    }

    void handleBackspace() {
        try {
            InputConnection ic = getCurrentInputConnection();
            if (ic != null) {
                CharSequence selected = ic.getSelectedText(0);
                if (!TextUtils.isEmpty(selected)) {
                    ic.commitText("", 1);
                } else {
                    ic.deleteSurroundingText(1, 0);
                }
            }
        } catch (Throwable ignored) {
        }
    }

    /**
     * Compact 54dp Dark-Grey (#121212 / #1C1C1C) Squircle Voice Input Bar shown at
     * the bottom of the screen when QuickSTT Voice Auto-Type is the active keyboard.
     */
    private final class VoiceBarView extends View implements Choreographer.FrameCallback {

        private final int barHeightPx;
        private final Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint cardPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint btnPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint btnBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint statusPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint spinnerTrackPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint spinnerArcPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        private final RectF cardRect = new RectF();
        private final RectF kbdBtnRect = new RectF();
        private final RectF recBtnRect = new RectF();
        private final RectF delBtnRect = new RectF();
        private final RectF spinnerRect = new RectF();
        private final Path squirclePath = new Path();

        private float spinnerAngle = 0f;
        private long lastFrameNanos = 0L;
        private boolean animating = false;

        VoiceBarView(Context context) {
            super(context);
            barHeightPx = Bubble.dp(context, 56f);

            bgPaint.setStyle(Paint.Style.FILL);
            bgPaint.setColor(0xFF121212);

            cardPaint.setStyle(Paint.Style.FILL);
            cardPaint.setColor(0xFF1C1C1C);

            borderPaint.setStyle(Paint.Style.STROKE);
            borderPaint.setStrokeWidth(Bubble.dp(context, 1.0f));
            borderPaint.setColor(0xFF2A2A2A);

            btnPaint.setStyle(Paint.Style.FILL);
            btnBorderPaint.setStyle(Paint.Style.STROKE);
            btnBorderPaint.setStrokeWidth(Bubble.dp(context, 1.0f));

            textPaint.setColor(0xFFF5F5F5);
            textPaint.setTextSize(TypedValue.applyDimension(
                    TypedValue.COMPLEX_UNIT_SP, 11.5f, context.getResources().getDisplayMetrics()));
            textPaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
            textPaint.setTextAlign(Paint.Align.CENTER);

            statusPaint.setColor(0xFFEDEDED);
            statusPaint.setTextSize(TypedValue.applyDimension(
                    TypedValue.COMPLEX_UNIT_SP, 12f, context.getResources().getDisplayMetrics()));
            statusPaint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
            statusPaint.setTextAlign(Paint.Align.LEFT);

            spinnerTrackPaint.setStyle(Paint.Style.STROKE);
            spinnerTrackPaint.setStrokeWidth(Bubble.dp(context, 2.2f));
            spinnerTrackPaint.setColor(0x2EFFFFFF);

            spinnerArcPaint.setStyle(Paint.Style.STROKE);
            spinnerArcPaint.setStrokeWidth(Bubble.dp(context, 2.2f));
            spinnerArcPaint.setStrokeCap(Paint.Cap.ROUND);
            spinnerArcPaint.setColor(0xFFF5F5F5);

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
        protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
            int width = MeasureSpec.getSize(widthMeasureSpec);
            setMeasuredDimension(width, barHeightPx);
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
            spinnerAngle = (spinnerAngle + dt * 320f) % 360f;
            if (liveRecording || liveBusy) {
                invalidate();
            }
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

            canvas.drawRect(0, 0, w, h, bgPaint);

            float padH = Bubble.dp(getContext(), 8f);
            float padV = Bubble.dp(getContext(), 5f);
            cardRect.set(padH, padV, w - padH, h - padV);
            float r = Bubble.dp(getContext(), 13f);
            canvas.drawRoundRect(cardRect, r, r, cardPaint);
            borderPaint.setColor(liveRecording ? 0xFFEF4444 : 0xFF2A2A2A);
            canvas.drawRoundRect(cardRect, r, r, borderPaint);

            float cy = h * 0.5f;
            float btnH = Bubble.dp(getContext(), 34f);
            float btnR = Bubble.dp(getContext(), 9.5f);

            // 1. Left "⌨ Keyboard" squircle button
            float kbdLeft = cardRect.left + Bubble.dp(getContext(), 6f);
            float kbdW = Bubble.dp(getContext(), 76f);
            kbdBtnRect.set(kbdLeft, cy - btnH * 0.5f, kbdLeft + kbdW, cy + btnH * 0.5f);
            btnPaint.setColor(0xFF262626);
            btnBorderPaint.setColor(0xFF383838);
            canvas.drawRoundRect(kbdBtnRect, btnR, btnR, btnPaint);
            canvas.drawRoundRect(kbdBtnRect, btnR, btnR, btnBorderPaint);

            Paint.FontMetrics fm = textPaint.getFontMetrics();
            float base = cy - (fm.ascent + fm.descent) * 0.5f;
            canvas.drawText("⌨ Keyboard", kbdBtnRect.centerX(), base, textPaint);

            // 2. Far-right "⌫" Backspace squircle button
            float delRight = cardRect.right - Bubble.dp(getContext(), 6f);
            float delW = Bubble.dp(getContext(), 42f);
            delBtnRect.set(delRight - delW, cy - btnH * 0.5f, delRight, cy + btnH * 0.5f);
            canvas.drawRoundRect(delBtnRect, btnR, btnR, btnPaint);
            canvas.drawRoundRect(delBtnRect, btnR, btnR, btnBorderPaint);
            canvas.drawText("⌫", delBtnRect.centerX(), base, textPaint);

            // 3. "● START" / "■ STOP" squircle button
            float recRight = delBtnRect.left - Bubble.dp(getContext(), 6f);
            float recW = Bubble.dp(getContext(), 76f);
            recBtnRect.set(recRight - recW, cy - btnH * 0.5f, recRight, cy + btnH * 0.5f);
            String recLabel;
            if (liveRecording) {
                recLabel = "■ STOP";
                btnPaint.setColor(0xFFDC2626);
                btnBorderPaint.setColor(0xFFF87171);
            } else if (liveBusy) {
                recLabel = "WAIT";
                btnPaint.setColor(0xFF242424);
                btnBorderPaint.setColor(0xFF3D3D3D);
            } else {
                recLabel = "● START";
                btnPaint.setColor(0xFF2563EB);
                btnBorderPaint.setColor(0xFF60A5FA);
            }
            canvas.drawRoundRect(recBtnRect, btnR, btnR, btnPaint);
            canvas.drawRoundRect(recBtnRect, btnR, btnR, btnBorderPaint);
            canvas.drawText(recLabel, recBtnRect.centerX(), base, textPaint);

            // 4. Center status + round spinner
            float centerLeft = kbdBtnRect.right + Bubble.dp(getContext(), 10f);
            if (liveBusy) {
                float spinR = Bubble.dp(getContext(), 8.5f);
                float spinCx = centerLeft + spinR;
                spinnerRect.set(spinCx - spinR, cy - spinR, spinCx + spinR, cy + spinR);
                canvas.drawOval(spinnerRect, spinnerTrackPaint);
                canvas.drawArc(spinnerRect, spinnerAngle, 250f, false, spinnerArcPaint);
                canvas.drawText(liveStateLabel, spinCx + spinR + Bubble.dp(getContext(), 7f), base, statusPaint);
            } else {
                String label = liveRecording ? "Listening (live)…" : "QuickSTT Ready";
                canvas.drawText(label, centerLeft, base, statusPaint);
            }
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            if (event.getActionMasked() == MotionEvent.ACTION_UP) {
                float x = event.getX();
                float y = event.getY();
                if (kbdBtnRect.contains(x, y)) {
                    switchBackToKeyboard();
                    return true;
                }
                if (delBtnRect.contains(x, y)) {
                    handleBackspace();
                    return true;
                }
                if (recBtnRect.contains(x, y) || (!liveBusy || liveRecording)) {
                    Bubble.onImeActive(getContext(), true);
                    Bubble.dispatchAction(Bubble.ACTION_RECORD);
                    return true;
                }
            }
            return true;
        }
    }
}
