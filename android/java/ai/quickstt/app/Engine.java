package ai.quickstt.app;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Puts the speech engines where they can be started.
 *
 * <p>The engines ship inside the APK, in {@code lib/<abi>/}. With
 * {@code android:extractNativeLibs="true"} the installer unpacks them into
 * the app's native library directory, which is the only place a modern
 * Android lets an app run a program from (the home directory is mounted
 * {@code noexec}). This helper is the fallback for the case where that did
 * not happen: it reads the engine out of the APK and writes it into the
 * native library directory itself, and only falls back to the data directory
 * when even that is not possible.
 */
public final class Engine {

    private Engine() {
    }

    /**
     * Make an engine available and return the path it can be started from.
     *
     * @param entry the file name inside the APK, e.g. {@code libwhisper-cli.so}
     * @return the path of the engine, or an empty string when it failed.
     */
    public static String extract(Context context, String entry) {
        try {
            // 1. The native library directory: exec-mounted, so the engine
            //    can be started directly.
            File nativeDir = nativeLibraryDir(context);
            if (nativeDir != null) {
                String path = write(context, nativeDir, entry);
                if (!path.isEmpty()) {
                    return path;
                }
            }

            // 2. The data directory: the system linker can still start a
            //    program from there.
            File dir = new File(context.getFilesDir(), "engines");
            if (!dir.exists() && !dir.mkdirs()) {
                return "";
            }
            return write(context, dir, entry);
        } catch (Throwable t) {
            return "";
        }
    }

    /** The directory the installer unpacks the native libraries into. */
    private static File nativeLibraryDir(Context context) {
        try {
            String path = context.getApplicationInfo().nativeLibraryDir;
            if (path == null || path.isEmpty()) {
                return null;
            }
            File dir = new File(path);
            if (!dir.isDirectory() && !dir.mkdirs()) {
                return null;
            }
            return dir;
        } catch (Throwable t) {
            return null;
        }
    }

    /** Copy the engine out of the APK into {@code dir}. */
    private static String write(Context context, File dir, String entry) {
        try {
            if (!dir.isDirectory() && !dir.mkdirs()) {
                return "";
            }
            File out = new File(dir, entry);
            if (out.exists() && out.length() > 0) {
                makeExecutable(out);
                return out.getAbsolutePath();
            }

            String apk = context.getApplicationInfo().sourceDir;
            ZipFile zip = new ZipFile(apk);
            try {
                ZipEntry found = null;
                Enumeration<? extends ZipEntry> entries = zip.entries();
                while (entries.hasMoreElements()) {
                    ZipEntry candidate = entries.nextElement();
                    if (candidate.getName().endsWith("/" + entry)) {
                        found = candidate;
                        break;
                    }
                }
                if (found == null) {
                    return "";
                }
                InputStream in = zip.getInputStream(found);
                try {
                    OutputStream sink = new FileOutputStream(out);
                    try {
                        byte[] buffer = new byte[65536];
                        int n;
                        while ((n = in.read(buffer)) > 0) {
                            sink.write(buffer, 0, n);
                        }
                    } finally {
                        sink.close();
                    }
                } finally {
                    in.close();
                }
            } finally {
                zip.close();
            }
            makeExecutable(out);
            return out.getAbsolutePath();
        } catch (Throwable t) {
            return "";
        }
    }

    private static void makeExecutable(File file) {
        try {
            file.setReadable(true, false);
            file.setExecutable(true, false);
        } catch (Throwable ignored) {
            // The file is still usable through the system linker.
        }
    }
}
