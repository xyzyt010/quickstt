package ai.quickstt.app;

import android.accessibilityservice.AccessibilityService;
import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.TextUtils;
import android.util.Log;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityWindowInfo;

import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Accessibility service that enables automatic and live real-time typing into
 * any focused text box across Android apps without requiring root.
 *
 * <ul>
 *   <li>Detects whether a text field is currently focused ({@link #hasFocusedTextBox()}).
 *   <li>During live dictation, updates the focused text field in real time as partial
 *       transcripts arrive ({@link #typeLivePartial(String)}).
 *   <li>When dictation stops, commits the final transcript into the focused text field
 *       ({@link #commitFinalText(String)}), or returns {@code false} when no text field is
 *       focused so the caller can copy the full transcript to the clipboard all at once.
 * </ul>
 */
public final class QuickSttAccessibilityService extends AccessibilityService {

    private static final String TAG = "QuickSTT";

    private static volatile QuickSttAccessibilityService instance;

    /** Snapshot of the text field before the current dictation turn began. */
    private static String sessionBaseText = null;
    private static int sessionInsertStart = -1;
    private static int sessionInsertEnd = -1;
    private static boolean sessionActive = false;

    @Override
    protected void onServiceConnected() {
        super.onServiceConnected();
        instance = this;
        Log.i(TAG, "QuickSttAccessibilityService connected");
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {
        // Focus is queried on demand via findFocusedEditableNode().
    }

    @Override
    public void onInterrupt() {
    }

    @Override
    public boolean onUnbind(android.content.Intent intent) {
        if (instance == this) {
            instance = null;
        }
        return super.onUnbind(intent);
    }

    @Override
    public void onDestroy() {
        if (instance == this) {
            instance = null;
        }
        super.onDestroy();
    }

    /** True when the accessibility service is enabled in system settings. */
    public static boolean isEnabled(Context context) {
        if (instance != null) {
            return true;
        }
        try {
            String enabled = Settings.Secure.getString(
                    context.getContentResolver(),
                    Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES);
            if (TextUtils.isEmpty(enabled)) {
                return false;
            }
            String pkg = context.getPackageName();
            return enabled.contains(pkg + "/" + QuickSttAccessibilityService.class.getName())
                    || enabled.contains(pkg + "/.QuickSttAccessibilityService");
        } catch (Throwable t) {
            return false;
        }
    }

    /** Reset the live-typing session at the start of a new recording turn. */
    public static synchronized void beginTurn() {
        sessionBaseText = null;
        sessionInsertStart = -1;
        sessionInsertEnd = -1;
        sessionActive = false;
    }

    /** True when an editable text box is currently focused in the foreground app. */
    public static boolean hasFocusedTextBox() {
        final QuickSttAccessibilityService svc = instance;
        if (svc == null) {
            return false;
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            AccessibilityNodeInfo node = svc.findFocusedEditableNode();
            if (node != null) {
                node.recycle();
                return true;
            }
            return false;
        }
        final AtomicBoolean found = new AtomicBoolean(false);
        final CountDownLatch latch = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                try {
                    AccessibilityNodeInfo node = svc.findFocusedEditableNode();
                    if (node != null) {
                        found.set(true);
                        node.recycle();
                    }
                } catch (Throwable ignored) {
                } finally {
                    latch.countDown();
                }
            }
        });
        try {
            latch.await(250, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {
        }
        return found.get();
    }

    /**
     * Update the focused text box in real time with the current partial transcript.
     *
     * @return {@code true} if a focused text box was found and updated.
     */
    public static boolean typeLivePartial(final String partialText) {
        if (partialText == null || partialText.trim().isEmpty()) {
            return false;
        }
        return applyToFocusedNode(partialText.trim(), false) == 1;
    }

    /**
     * Commit the final transcript at the end of a turn.
     *
     * @return {@code true} if typed directly into a focused text box; {@code false} if no
     *         text box was focused (caller should copy to clipboard).
     */
    public static boolean commitFinalText(final String finalText) {
        try {
            if (finalText == null || finalText.trim().isEmpty()) {
                return false;
            }
            return applyToFocusedNode(finalText.trim(), true) == 1;
        } finally {
            beginTurn();
        }
    }

    private static int applyToFocusedNode(final String transcript, final boolean commit) {
        final QuickSttAccessibilityService svc = instance;
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
            latch.await(350, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {
        }
        return result.get();
    }

    private synchronized boolean applyOnMainThread(String transcript, boolean commit) {
        AccessibilityNodeInfo node = findFocusedEditableNode();
        if (node == null) {
            return false;
        }
        try {
            if (!sessionActive || sessionBaseText == null) {
                String existing = readExistingText(node);
                int selStart = node.getTextSelectionStart();
                int selEnd = node.getTextSelectionEnd();
                if (selStart < 0 || selStart > existing.length()) {
                    selStart = existing.length();
                }
                if (selEnd < selStart || selEnd > existing.length()) {
                    selEnd = selStart;
                }
                sessionBaseText = existing;
                sessionInsertStart = selStart;
                sessionInsertEnd = selEnd;
                sessionActive = true;
            }

            String prefix = sessionBaseText.substring(0, sessionInsertStart);
            String suffix = sessionBaseText.substring(sessionInsertEnd);
            String needLeadingSpace = (!prefix.isEmpty()
                    && !Character.isWhitespace(prefix.charAt(prefix.length() - 1)))
                    ? " " : "";
            String combined = prefix + needLeadingSpace + transcript + suffix;

            Bundle args = new Bundle();
            args.putCharSequence(
                    AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE,
                    combined);
            boolean ok = node.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args);
            if (ok) {
                int newCursor = (prefix + needLeadingSpace + transcript).length();
                Bundle sel = new Bundle();
                sel.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, newCursor);
                sel.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, newCursor);
                node.performAction(AccessibilityNodeInfo.ACTION_SET_SELECTION, sel);
            }
            if (commit) {
                sessionActive = false;
                sessionBaseText = null;
            }
            return ok;
        } finally {
            node.recycle();
        }
    }

    private static String readExistingText(AccessibilityNodeInfo node) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            try {
                if (node.isShowingHintText()) {
                    return "";
                }
            } catch (Throwable ignored) {
            }
        }
        CharSequence cs = node.getText();
        if (cs == null) {
            return "";
        }
        CharSequence hint = null;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            try {
                hint = node.getHintText();
            } catch (Throwable ignored) {
            }
        }
        if (hint != null && TextUtils.equals(cs, hint)) {
            return "";
        }
        return cs.toString();
    }

    private AccessibilityNodeInfo findFocusedEditableNode() {
        try {
            AccessibilityNodeInfo focused = findFocus(AccessibilityNodeInfo.FOCUS_INPUT);
            if (focused != null) {
                if (focused.isEditable()) {
                    return focused;
                }
                focused.recycle();
            }
        } catch (Throwable ignored) {
        }

        try {
            AccessibilityNodeInfo root = getRootInActiveWindow();
            if (root != null) {
                AccessibilityNodeInfo found = findFocusedEditableInTree(root);
                if (found != null) {
                    if (found != root) {
                        root.recycle();
                    }
                    return found;
                }
                root.recycle();
            }
        } catch (Throwable ignored) {
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            try {
                List<AccessibilityWindowInfo> windows = getWindows();
                if (windows != null) {
                    for (AccessibilityWindowInfo window : windows) {
                        AccessibilityNodeInfo root = window.getRoot();
                        if (root != null) {
                            AccessibilityNodeInfo found = findFocusedEditableInTree(root);
                            if (found != null) {
                                if (found != root) {
                                    root.recycle();
                                }
                                return found;
                            }
                            root.recycle();
                        }
                    }
                }
            } catch (Throwable ignored) {
            }
        }
        return null;
    }

    private AccessibilityNodeInfo findFocusedEditableInTree(AccessibilityNodeInfo node) {
        if (node == null) {
            return null;
        }
        if (node.isEditable() && node.isFocused()) {
            return AccessibilityNodeInfo.obtain(node);
        }
        int count = node.getChildCount();
        for (int i = 0; i < count; i++) {
            AccessibilityNodeInfo child = node.getChild(i);
            if (child != null) {
                AccessibilityNodeInfo match = findFocusedEditableInTree(child);
                child.recycle();
                if (match != null) {
                    return match;
                }
            }
        }
        return null;
    }
}
