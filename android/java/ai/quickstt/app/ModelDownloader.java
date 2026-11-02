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
 * Downloads speech models from HuggingFace with the Android HTTP stack and
 * extracts pre-bundled models (such as Cactus Whistle {@code whistle.cact})
 * from the APK.
 */
public final class ModelDownloader {

    private static final String TAG = "QuickSTT";

    static {
        try {
            System.loadLibrary("quickstt_android");
        } catch (Throwable t) {
            Log.w(TAG, "quickstt_android library not loaded yet for ModelDownloader", t);
        }
    }

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

        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            return "the model folder could not be created (" + parent + ")";
        }

        // If target is already complete on disk, succeed immediately.
        if (target.isFile() && target.length() >= minBytes) {
            reportProgress(target.length(), target.length());
            return "";
        }

        long initialResume = part.isFile() ? part.length() : 0L;
        String firstAttempt = attemptDownload(url, target, part, minBytes, initialResume);
        if (firstAttempt.isEmpty()) {
            return "";
        }

        // If a resumed transfer failed or had a range mismatch, retry cleanly from 0.
        if (initialResume > 0L) {
            Log.w(TAG, "Resumed download failed (" + firstAttempt + "), retrying from scratch");
            part.delete();
            return attemptDownload(url, target, part, minBytes, 0L);
        }
        return firstAttempt;
    }

    private static String attemptDownload(
            String url, File target, File part, long minBytes, long already) {
        HttpURLConnection connection = null;
        try {
            connection = open(url, already);
            int status = connection.getResponseCode();

            if (status == 416) {
                if (already >= minBytes) {
                    if (target.isFile() && !target.delete()) {
                        Log.w(TAG, "the old model file could not be removed");
                    }
                    if (part.renameTo(target)) {
                        reportProgress(target.length(), target.length());
                        return "";
                    }
                }
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
                long rangeTotal = parseRangeTotal(connection.getHeaderField("Content-Range"));
                if (rangeTotal > 0L) {
                    total = rangeTotal;
                } else {
                    long rem = contentLengthHeader(connection);
                    total = rem > 0L ? (already + rem) : 0L;
                }
            } else {
                // Server answered 200 OK: it is sending the full file from byte 0.
                already = 0L;
                total = contentLengthHeader(connection);
            }
            if (total <= 0L) {
                total = minBytes;
            }

            reportProgress(already, total);

            InputStream in = connection.getInputStream();
            try {
                write(in, part, already, total);
            } finally {
                close(in);
            }

            long written = part.isFile() ? part.length() : 0L;
            if (written < minBytes) {
                return "the download was incomplete ("
                        + written + " of " + (total > 0 ? total : minBytes) + " bytes)";
            }

            if (target.isFile() && !target.delete()) {
                Log.w(TAG, "the old model file could not be removed");
            }
            if (!part.renameTo(target)) {
                // Fallback copy if rename across filesystems or locked handle failed
                copyFile(part, target);
                part.delete();
            }
            if (!target.isFile() || target.length() < minBytes) {
                return "the model file could not be moved into place";
            }
            reportProgress(target.length(), target.length());
            return "";
        } catch (Throwable t) {
            Log.e(TAG, "ModelDownloader.download failed for " + url, t);
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
     * @param context  the context to access assets/package
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
            boolean copied = false;

            // 1. Try reading from Context.getAssets()
            InputStream assetIn = null;
            try {
                assetIn = context.getAssets().open("models/" + fileName);
            } catch (Throwable ignored) {
                try {
                    assetIn = context.getAssets().open(fileName);
                } catch (Throwable ignored2) {
                }
            }

            if (assetIn != null) {
                try {
                    streamToFile(assetIn, part);
                    copied = true;
                } finally {
                    close(assetIn);
                }
            }

            // 2. Fall back to reading from the APK zip directly (keeps ZipFile open until copy finishes)
            if (!copied || !part.isFile() || part.length() < minBytes) {
                ZipFile zip = null;
                try {
                    String apkPath = context.getApplicationInfo().sourceDir;
                    zip = new ZipFile(apkPath);
                    ZipEntry entry = zip.getEntry("assets/models/" + fileName);
                    if (entry == null) {
                        entry = zip.getEntry("assets/" + fileName);
                    }
                    if (entry == null) {
                        entry = zip.getEntry("models/" + fileName);
                    }
                    if (entry != null) {
                        InputStream zipIn = zip.getInputStream(entry);
                        try {
                            streamToFile(zipIn, part);
                            copied = true;
                        } finally {
                            close(zipIn);
                        }
                    }
                } catch (Throwable t) {
                    Log.w(TAG, "failed to extract " + fileName + " from APK zip", t);
                } finally {
                    if (zip != null) {
                        try {
                            zip.close();
                        } catch (Throwable ignored) {
                        }
                    }
                }
            }

            if (part.isFile() && part.length() >= minBytes) {
                if (target.isFile()) {
                    target.delete();
                }
                if (!part.renameTo(target)) {
                    copyFile(part, target);
                    part.delete();
                }
                if (target.isFile() && target.length() >= minBytes) {
                    Log.i(TAG, "Extracted bundled model: " + fileName + " (" + target.length() + " bytes)");
                    return true;
                }
            }
            part.delete();
        } catch (Throwable t) {
            Log.w(TAG, "extractBundledModel failed for " + fileName, t);
        }
        return false;
    }

    private static void streamToFile(InputStream in, File dest) throws IOException {
        OutputStream out = new FileOutputStream(dest);
        try {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            out.flush();
        } finally {
            close(out);
        }
    }

    private static void copyFile(File src, File dst) throws IOException {
        java.io.FileInputStream in = new java.io.FileInputStream(src);
        try {
            streamToFile(in, dst);
        } finally {
            close(in);
        }
    }

    /** Open the connection, manually following redirects and asking for raw bytes. */
    private static HttpURLConnection open(String initialUrl, long already) throws IOException {
        String currentUrl = initialUrl;
        HttpURLConnection connection = null;

        for (int redirects = 0; redirects < 10; redirects++) {
            URL url = new URL(currentUrl);
            connection = (HttpURLConnection) url.openConnection();
            connection.setConnectTimeout(30000);
            connection.setReadTimeout(60000);
            connection.setInstanceFollowRedirects(false);
            connection.setRequestProperty("User-Agent", "QuickSTT/2.0 (Android)");
            connection.setRequestProperty("Accept", "*/*");
            // Prevent transparent gzip so Content-Length and byte offsets stay exact
            connection.setRequestProperty("Accept-Encoding", "identity");
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

    /** Write the bytes into the part file and report progress. */
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
                reportProgress(done, total > done ? total : done);
            }
            file.getFD().sync();
        } finally {
            file.close();
        }
    }

    private static void reportProgress(long done, long total) {
        try {
            nativeProgress(done, total);
        } catch (Throwable ignored) {
        }
    }

    /** Extract the total length after '/' in Content-Range: bytes X-Y/TOTAL. */
    private static long parseRangeTotal(String range) {
        if (range != null) {
            int slash = range.lastIndexOf('/');
            if (slash > 0 && slash + 1 < range.length()) {
                String tail = range.substring(slash + 1).trim();
                if (!"*".equals(tail)) {
                    try {
                        long v = Long.parseLong(tail);
                        if (v > 0L) {
                            return v;
                        }
                    } catch (NumberFormatException ignored) {
                    }
                }
            }
        }
        return 0L;
    }

    private static long contentLengthHeader(HttpURLConnection connection) {
        try {
            String raw = connection.getHeaderField("Content-Length");
            if (raw != null && !raw.trim().isEmpty()) {
                long parsed = Long.parseLong(raw.trim());
                if (parsed > 0L) {
                    return parsed;
                }
            }
        } catch (Throwable ignored) {
        }
        try {
            long len = connection.getContentLengthLong();
            if (len > 0L) {
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
        if (stream != null) {
            try {
                stream.close();
            } catch (Throwable ignored) {
            }
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
