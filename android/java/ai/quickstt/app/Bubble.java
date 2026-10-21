package ai.quickstt.app;

import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
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
 * <p>It is a small card with the mic state, the last transcript and three
 * buttons: record, type the transcript into the app in front, and close.
 * The card can be dragged to any corner of the screen.
 *
 * <p>The whole overlay is built here because a {@code View.OnClickListener}
 * cannot be created from JNI, and because the window flags and the layout
 * parameters of an overlay window are easier to get right in Java. Every
 * method returns an error message (or an empty string for success) so the
 * Rust side can tell the user what went wrong instead of failing silently.
 *
 * <p>The class is optional: when the dex is missing (or the class cannot be
 * loaded) the app falls back to a plain read-out bubble.
 */
public final class Bubble {

    /** Start or stop the recording. */
    public static final int ACTION_RECORD = 1;
    /** Type the last transcript into the app that has the focus. */
    public static final int ACTION_TYPE = 2;
    /** Hide the bubble. */
    public static final int ACTION_CLOSE = 3;

    /** TYPE_APPLICATION_OVERLAY: may be drawn over every other app. */
    private static final int TYPE_APPLICATION_OVERLAY = 2038;
    /** FLAG_NOT_FOCUSABLE | FLAG_NOT_TOUCH_MODAL: the app in front keeps the
     *  keyboard focus and every tap outside the bubble still reaches it. */
    private static final int FLAGS = 0x00000008 | 0x00000010;

    /** The bubble that is currently on the screen (if any). */
    private static View current;
    private static WindowManager.LayoutParams currentParams;
    private static WindowManager currentManager;

    static {
        // The library is already loaded by the NativeActivity; loading it
        // again is a no-op and makes the class safe to use on its own.
        try {
            System.loadLibrary("quickstt_android");
        } catch (UnsatisfiedLinkError e) {
            // The bubble then simply has no buttons.
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
            hide(context);

            WindowManager manager = (WindowManager)
                    context.getSystemService(Context.WINDOW_SERVICE);
            if (manager == null) {
                return "the window manager is not available";
            }

            View view = build(context, state, transcript);

            WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    TYPE_APPLICATION_OVERLAY,
                    FLAGS,
                    PixelFormat.TRANSLUCENT);
            params.gravity = Gravity.TOP | Gravity.START;
            params.x = dp(context, 12);
            params.y = dp(context, 90);

            manager.addView(view, params);

            current = view;
            currentParams = params;
            currentManager = manager;
            return "";
        } catch (Throwable t) {
            // A missing permission shows up here as a BadTokenException.
            return describe(t);
        }
    }

    /** Remove the bubble. */
    public static String hide(Context context) {
        try {
            if (current != null && currentManager != null) {
                currentManager.removeView(current);
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

    /** Build the bubble view: the state, the transcript and the buttons. */
    private static View build(Context context, String state, String transcript) {
        LinearLayout row = new LinearLayout(context);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        int pad = dp(context, 10);
        row.setPadding(pad, pad, pad, pad);

        TextView text = new TextView(context);
        text.setText(transcript.isEmpty() ? state : state + "\n" + transcript);
        text.setTextColor(0xFFF2F5F7);
        text.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        text.setMaxWidth(dp(context, 210));
        text.setMaxLines(3);
        row.addView(text, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        row.addView(makeButton(context, "REC", ACTION_RECORD, 0xFF4ADE80));
        row.addView(makeButton(context, "TYPE", ACTION_TYPE, 0xFFE7ECF2));
        row.addView(makeButton(context, "X", ACTION_CLOSE, 0xFF9AA3AD));

        GradientDrawable background = new GradientDrawable();
        background.setColor(0xE6121418);
        background.setCornerRadius(dp(context, 18));
        background.setStroke(dp(context, 1), 0x33FFFFFF);
        row.setBackground(background);

        // Drag the bubble anywhere on the screen. The buttons are children, so
        // they get the touch first and the card is only dragged by its text.
        row.setOnTouchListener(new View.OnTouchListener() {
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
                                // The window is gone; nothing to move.
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

    private static Button makeButton(Context context, String label, final int action, int color) {
        Button button = new Button(context);
        button.setText(label);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        button.setTextColor(0xFF101418);
        button.setAllCaps(false);
        button.setPadding(dp(context, 12), 0, dp(context, 12), 0);
        button.setMinimumWidth(0);
        button.setMinimumHeight(dp(context, 32));
        button.setBackgroundColor(color);
        button.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                nativeBubbleAction(action);
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
