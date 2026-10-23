package ai.quickstt.app;

import android.content.Context;
import android.util.Log;

import java.io.Closeable;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Downloads a model from HuggingFace with the Android HTTP stack.
 *
 * <p>The download runs on its own thread. The number of bytes that really
 * arrived is reported straight back into Rust, so the app can show the real
 * progress.
 *
 * <p>An interrupted download continues where it stopped: the bytes are
 * written to a {@code .part} file and the next attempt asks the server for
 * the rest with a {@code Range} header.
 *
 * <p>Only a complete file is moved into place.
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

            // If the range was not satisfiable, check if we already have the full file
            if (status == 416) {
                if (already >= minBytes) {
                    if (target.isFile() && !target.delete()) {
                        Log.w(TAG, "the old model file could not be removed");
                    }
                    if (part.renameTo(target)) {
                        return "";
                    }
                }
                // Range invalid: start from scratch
                already = 0L;
                connection.disconnect();
                connection = open(url, 0L);
                status = connection.getResponseCode();
            }

            if (status != 200 && status != 206) {
                return "the server answered " + status + " " + safe(connection.getResponseMessage());
            }

            long total;
            if (status == 206) {
                total = already + contentLength(connection, connection.getHeaderField("Content-Range"));
            } else {
                // The server answered 200: start from scratch.
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
                return "the download was incomplete (" + written + " of " + (total > 0 ? total : minBytes) + " bytes)";
            }
            if (total > 0 && total != minBytes && written != total) {
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

    /**
     * Extracts a bundled model from the APK assets/zip into the app's models directory.
     *
     * @param context the context to access assets/package
     * @param fileName the model file name (e.g. "whistle.cact")
     * @param minBytes the minimum expected size
     * @return true if the model is installed
     */
    public static boolean extractBundledModel(Context context, String fileName, long minBytes) {
        try {
            File modelsDir = new File(new File(context.getFilesDir(), "quickstt"), "models");
            File target = new File(modelsDir, fileName);
            if (target.isFile() && target.length() >= minBytes) {
                return true;
            }
            if (!modelsDir.isDirectory() && !modelsDir.mkdirs()) {
                Log.w(TAG, "could not create models dir: " + modelsDir);
            }

            File part = new File(modelsDir, fileName + ".part");

            // 1. Try reading from Context.getAssets()
            InputStream in = null;
            try {
                in = context.getAssets().open("models/" + fileName);
            } catch (Throwable ignored) {
                try {
                    in = context.getAssets().open(fileName);
                } catch (Throwable ignored2) {
                }
            }

            // 2. Fall back to reading from the APK file directly
            if (in == null) {
                try {
                    String apkPath = context.getApplicationInfo().sourceDir;
                    ZipFile zip = new ZipFile(apkPath);
                    ZipEntry entry = zip.getEntry("assets/models/" + fileName);
                    if (entry == null) {
                        entry = zip.getEntry("assets/" + fileName);
                    }
                    if (entry == null) {
                        entry = zip.getEntry("models/" + fileName);
                    }
                    if (entry != null) {
                        in = zip.getInputStream(entry);
                    }
                } catch (Throwable t) {
                    Log.w(TAG, "failed to inspect APK zip for " + fileName, t);
                }
            }

            if (in == null) {
                return false;
            }

            try {
                OutputStream out = new FileOutputStream(part);
                try {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        out.write(buf, 0, n);
                    }
                    out.flush();
                } finally {
                    out.close();
                }
            } finally {
                in.close();
            }

            if (part.isFile() && part.length() >= minBytes) {
                if (target.isFile()) {
                    target.delete();
                }
                boolean ok = part.renameTo(target);
                if (ok) {
                    Log.i(TAG, "Successfully extracted bundled model: " + fileName + " (" + target.length() + " bytes)");
                    return true;
                }
            }
            part.delete();
        } catch (Throwable t) {
            Log.w(TAG, "extractBundledModel failed for " + fileName, t);
        }
        return false;
    }

    /** Open the connection, manually following redirects and asking for the rest of the file when resuming. */
    private static HttpURLConnection open(String initialUrl, long already) throws IOException {
        String currentUrl = initialUrl;
        HttpURLConnection connection = null;

        for (int redirects = 0; redirects < 10; redirects++) {
            URL url = new URL(currentUrl);
            connection = (HttpURLConnection) url.openConnection();
            connection.setConnectTimeout(30000);
            connection.setReadTimeout(60000);
            connection.setInstanceFollowRedirects(false);
            connection.setRequestProperty("User-Agent", "QuickSTT/1.0 (Android)");
            connection.setRequestProperty("Accept", "*/*");
            if (already > 0L) {
                connection.setRequestProperty("Range", "bytes=" + already + "-");
            }

            connection.connect();
            int status = connection.getResponseCode();

            if (status == HttpURLConnection.HTTP_MOVED_PERM
                    || status == HttpURLConnection.HTTP_MOVED_TEMP
                    || status == HttpURLConnection.HTTP_SEE_OTHER
                    || status == 307
                    || status == 308) {
                String location = connection.getHeaderField("Location");
                connection.disconnect();
                if (location == null || location.isEmpty()) {
                    throw new IOException("Redirect status " + status + " with no Location header");
                }
                currentUrl = new URL(url, location).toExternalForm();
                continue;
            }
            return connection;
        }
        throw new IOException("Too many HTTP redirects (exceeded 10)");
    }

    /** Write the bytes into the part file and report the progress. */
    private static void write(InputStream in, File part, long already, long total)
            throws IOException {
        RandomAccessFile file = new RandomAccessFile(part, "rw");
        try {
            if (already == 0L) {
                file.setLength(0L);
            } else {
                file.setLength(already);
            }
            file.seek(already);
            byte[] buffer = new byte[65536];
            long done = already;
            int n;
            while ((n = in.read(buffer)) > 0) {
                file.write(buffer, 0, n);
                done += n;
                try {
                    nativeProgress(done, total);
                } catch (Throwable ignored) {
                }
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
            long len = connection.getContentLengthLong();
            if (len > 0) {
                return len;
            }
        } catch (Throwable ignored) {
        }
        return 0L;
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
