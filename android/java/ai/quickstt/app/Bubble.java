package ai.quickstt.app;

import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.util.Log;
import android.util.TypedValue;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The floating bubble that lives above every other app.
 *
 * <p>It is a sleek, minimal card with the mic state, the last transcript and three
 * buttons: record, type the transcript into the app in front, and close.
 * The card can be dragged to any corner of the screen.
 *
 * <p>Styled in a clean monochrome dark palette (#121212 / #1E1E1E / white text).
 */
public final class Bubble {

    private static final String TAG = "QuickSTT";

    /** Start or stop the recording. */
    public static final int ACTION_RECORD = 1;
    /** Type the last transcript into the app that has the focus. */
    public static final int ACTION_TYPE = 2;
    /** Hide the bubble. */
    public static final int ACTION_CLOSE = 3;

    /** TYPE_APPLICATION_OVERLAY: may be drawn over other apps. */
    private static final int TYPE_APPLICATION_OVERLAY = 2038;
    /** FLAG_NOT_FOCUSABLE | FLAG_NOT_TOUCH_MODAL: the app in front keeps the
     *  keyboard focus and every tap outside the bubble still reaches it. */
    private static final int FLAGS = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
            | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL;

    /** The bubble that is currently on the screen (if any). */
    private static View current;
    private static WindowManager.LayoutParams currentParams;
    private static WindowManager currentManager;

    static {
        try {
            System.loadLibrary("quickstt_android");
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "quickstt_android library not loaded yet for Bubble", e);
        }
    }

    /** Called from Rust when the user taps one of the buttons. */
    private static native void nativeBubbleAction(int action);

    private Bubble() {
    }

    /**
     * Show (or replace) the bubble.
     *
     * @return an empty string on success, otherwise the reason it failed.
     */
    public static String show(Context context, String state, String transcript) {
        try {
            if (current != null && currentManager != null) {
                String updated = update(state, transcript);
                if (updated != null && updated.isEmpty()) {
                    return "";
                }
            }

            hide(context);

            WindowManager manager = null;
            Context themedContext = null;

            // On Android 11+ (API 30+), createWindowContext provides the proper visual context for overlays
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                try {
                    Context windowContext = context.createWindowContext(TYPE_APPLICATION_OVERLAY, null);
                    manager = (WindowManager) windowContext.getSystemService(Context.WINDOW_SERVICE);
                    themedContext = new ContextThemeWrapper(windowContext, android.R.style.Theme_DeviceDefault);
                } catch (Throwable t) {
                    Log.w(TAG, "createWindowContext failed, trying direct context", t);
                }
            }

            if (manager == null) {
                try {
                    manager = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
                    themedContext = new ContextThemeWrapper(context, android.R.style.Theme_DeviceDefault);
                } catch (Throwable t) {
                    Log.w(TAG, "context getSystemService failed", t);
                }
            }

            if (manager == null) {
                try {
                    Context app = context.getApplicationContext();
                    manager = (WindowManager) app.getSystemService(Context.WINDOW_SERVICE);
                    themedContext = new ContextThemeWrapper(app, android.R.style.Theme_DeviceDefault);
                } catch (Throwable t) {
                    Log.w(TAG, "app getSystemService failed", t);
                }
            }

            if (manager == null) {
                return "the window manager is not available";
            }
            if (themedContext == null) {
                themedContext = context;
            }

            View view = build(themedContext, state, transcript);

            WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    TYPE_APPLICATION_OVERLAY,
                    FLAGS,
                    PixelFormat.TRANSLUCENT);
            params.gravity = Gravity.TOP | Gravity.START;
            params.x = dp(context, 16);
            params.y = dp(context, 96);

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

    /** Remove the bubble. */
    public static String hide(Context context) {
        try {
            if (current != null && currentManager != null) {
                try {
                    currentManager.removeView(current);
                } catch (Throwable ignored) {
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

    /** Update the text without rebuilding the bubble (no flicker). */
    public static String update(String state, String transcript) {
        try {
            if (current instanceof LinearLayout) {
                LinearLayout row = (LinearLayout) current;
                if (row.getChildCount() > 0 && row.getChildAt(0) instanceof TextView) {
                    TextView text = (TextView) row.getChildAt(0);
                    text.setText(transcript.isEmpty() ? state : state + "\n" + transcript);
                    return "";
                }
            }
            return "no bubble";
        } catch (Throwable t) {
            return describe(t);
        }
    }

    /** Build the bubble view: the state, the transcript and the buttons. Minimalist monochrome. */
    private static View build(Context context, String state, String transcript) {
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        int padH = dp(context, 12);
        int padV = dp(context, 8);
        row.setPadding(padH, padV, padH, padV);

        TextView text = new TextView(context);
        text.setText(transcript.isEmpty() ? state : state + "\n" + transcript);
        text.setTextColor(0xFFEDEDED);
        text.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        text.setMaxWidth(dp(context, 190));
        text.setMaxLines(3);
        LinearLayout.LayoutParams textLp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        textLp.setMarginEnd(dp(context, 8));
        row.addView(text, textLp);

        // Minimal monochrome buttons
        row.addView(makeButton(context, "REC", ACTION_RECORD, 0xFFFFFFFF, 0xFF121212));
        row.addView(makeButton(context, "TYPE", ACTION_TYPE, 0xFF262626, 0xFFFFFFFF));
        row.addView(makeButton(context, "✕", ACTION_CLOSE, 0xFF1C1C1C, 0xFFAAAAAA));

        // Background: clean dark grey with subtle outline (#121212 / #262626)
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xF0121212);
        background.setCornerRadius(dp(context, 20));
        background.setStroke(dp(context, 1), 0xFF2A2A2A);
        row.setBackground(background);
        row.setElevation(dp(context, 8));

        // Drag the bubble anywhere on the screen
        text.setOnTouchListener(new View.OnTouchListener() {
            private float downX;
            private float downY;
            private int startX;
            private int startY;

            @Override
            public boolean onTouch(View v, MotionEvent event) {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downX = event.getRawX();
                        downY = event.getRawY();
                        if (currentParams != null) {
                            startX = currentParams.x;
                            startY = currentParams.y;
                        }
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        if (currentParams != null && currentManager != null) {
                            currentParams.x = startX + (int) (event.getRawX() - downX);
                            currentParams.y = startY + (int) (event.getRawY() - downY);
                            try {
                                currentManager.updateViewLayout(current, currentParams);
                            } catch (Throwable ignored) {
                            }
                        }
                        return true;
                    default:
                        return false;
                }
            }
        });
        return row;
    }

    private static Button makeButton(Context context, String label, final int action, int bgColor, int textColor) {
        Button button = new Button(context);
        button.setText(label);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        button.setTextColor(textColor);
        button.setAllCaps(false);
        button.setPadding(dp(context, 10), 0, dp(context, 10), 0);
        button.setMinimumWidth(0);
        button.setMinimumHeight(dp(context, 30));

        GradientDrawable btnBg = new GradientDrawable();
        btnBg.setColor(bgColor);
        btnBg.setCornerRadius(dp(context, 15));
        btnBg.setStroke(dp(context, 1), 0x33FFFFFF);
        button.setBackground(btnBg);

        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, dp(context, 30));
        lp.setMarginEnd(dp(context, 4));
        button.setLayoutParams(lp);

        button.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                try {
                    nativeBubbleAction(action);
                } catch (Throwable t) {
                    Log.w(TAG, "nativeBubbleAction failed", t);
                }
            }
        });
        return button;
    }

    private static String describe(Throwable t) {
        String message = t.getMessage();
        if (message == null || message.isEmpty()) {
            message = t.getClass().getSimpleName();
        }
        return message;
    }

    private static int dp(Context context, int value) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, value,
                context.getResources().getDisplayMetrics());
    }
}
