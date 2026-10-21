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
 * <p>The engines ship inside the APK, in {@code lib/<abi>/}. The installer
 * normally extracts them into the app's native library directory, which is
 * the only place a modern Android lets an app run a program from. This
 * helper is the fallback for the case where that did not happen: it reads
 * the engine out of the APK and writes it into the app's data directory,
 * from where the system linker can start it.
 */
public final class Engine {

    private Engine() {
    }

    /**
     * Copy an engine out of the APK into the app's data directory.
     *
     * @param entry the file name inside the APK, e.g. {@code libwhisper-cli.so}
     * @return the path of the copy, or an empty string when it failed.
     */
    public static String extract(Context context, String entry) {
        try {
            File dir = new File(context.getFilesDir(), "engines");
            if (!dir.exists() && !dir.mkdirs()) {
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
