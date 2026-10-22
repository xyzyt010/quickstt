package ai.quickstt.app;

import android.util.Log;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Downloads a model from HuggingFace with the Android HTTP stack.
 *
 * <p>The download runs on its own thread. The number of bytes that really
 * arrived is reported straight back into Rust, so the app can show the real
 * progress — a progress bar that only pretends is worse than no progress
 * bar at all.
 *
 * <p>An interrupted download continues where it stopped: the bytes are
 * written to a {@code .part} file and the next attempt asks the server for
 * the rest with a {@code Range} header.
 *
 * <p>Only a complete file is moved into place. A truncated model is worse
 * than no model at all: the engine would fail on it.
 */
public final class ModelDownloader {

    private static final String TAG = "QuickSTT";

    /** Called on the download thread with the bytes that arrived. */
    private static native void nativeProgress(long done, long total);

    private ModelDownloader() {
    }

    /**
     * Download a file. Blocks until it is done.
     *
     * @param url      where the file comes from
     * @param dest     where the file has to end up
     * @param minBytes the smallest size that counts as a complete file
     * @return an empty string on success, otherwise the reason it failed
     */
    public static String download(String url, String dest, long minBytes) {
        File target = new File(dest);
        File part = new File(dest + ".part");
        HttpURLConnection connection = null;
        try {
            File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                return "the model folder could not be created (" + parent + ")";
            }

            long already = part.isFile() ? part.length() : 0L;
            connection = open(url, already);
            int status = connection.getResponseCode();
            if (status != 200 && status != 206) {
                return "the server answered " + status + " " + safe(connection.getResponseMessage());
            }

            long total;
            if (status == 206) {
                total = already + contentLength(connection, connection.getHeaderField("Content-Range"));
            } else {
                // The server ignored the range request: start from the top.
                total = contentLength(connection, null);
                already = 0L;
            }
            if (total <= 0L) {
                total = minBytes;
            }

            InputStream in = connection.getInputStream();
            try {
                write(in, part, already, total);
            } finally {
                close(in);
            }

            long written = part.isFile() ? part.length() : 0L;
            if (written < minBytes) {
                part.delete();
                return "the download was incomplete (" + written + " of " + total + " bytes)";
            }
            if (written != total) {
                part.delete();
                return "the download was incomplete (" + written + " of " + total + " bytes)";
            }
            if (target.isFile() && !target.delete()) {
                Log.w(TAG, "the old model file could not be removed");
            }
            if (!part.renameTo(target)) {
                return "the model file could not be moved into place";
            }
            return "";
        } catch (Throwable t) {
            return describe(t);
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    /** Open the connection, asking for the rest of the file when there is one. */
    private static HttpURLConnection open(String url, long already) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setConnectTimeout(30000);
        connection.setReadTimeout(60000);
        connection.setInstanceFollowRedirects(true);
        connection.setRequestProperty("User-Agent", "QuickSTT/1.0 (Android)");
        connection.setRequestProperty("Accept", "application/octet-stream");
        if (already > 0L) {
            connection.setRequestProperty("Range", "bytes=" + already + "-");
        }
        return connection;
    }

    /** Write the bytes into the part file and report the progress. */
    private static void write(InputStream in, File part, long already, long total)
            throws IOException {
        RandomAccessFile file = new RandomAccessFile(part, "rw");
        try {
            file.setLength(already);
            file.seek(already);
            byte[] buffer = new byte[65536];
            long done = already;
            int n;
            while ((n = in.read(buffer)) > 0) {
                file.write(buffer, 0, n);
                done += n;
                nativeProgress(done, total);
            }
            file.getFD().sync();
        } finally {
            file.close();
        }
    }

    /** The size of the whole file (the range header holds it in a 206 answer). */
    private static long contentLength(HttpURLConnection connection, String range) {
        if (range != null) {
            int slash = range.lastIndexOf('/');
            if (slash > 0) {
                try {
                    return Long.parseLong(range.substring(slash + 1).trim());
                } catch (NumberFormatException ignored) {
                    // Fall through to the content length.
                }
            }
        }
        try {
            return connection.getContentLengthLong();
        } catch (Throwable t) {
            return 0L;
        }
    }

    private static String safe(String message) {
        return message == null ? "" : message;
    }

    private static void close(Closeable stream) {
        try {
            stream.close();
        } catch (Throwable ignored) {
            // Nothing to do.
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
