package ai.quickstt.app;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * Cloud transcription (Groq / OpenAI / Deepgram) + API-key storage.
 *
 * Keys are encrypted with an AndroidKeyStore AES/GCM key that never leaves
 * the device keystore; only ciphertext (IV + cipher bytes, Base64) is kept
 * in SharedPreferences. Keys are never logged and never returned to Rust —
 * Rust only asks "is a key stored?" and "transcribe this WAV".
 *
 * HTTPS uses the Android HttpURLConnection stack, like ModelDownloader.
 * transcribe() returns "OK\n<text>" or "ERR\n<reason>".
 */
public final class ApiClient {

    private static final String TAG = "QuickSTT";
    private static final String PREFS = "quickstt_api_keys";
    private static final String ALIAS = "quickstt_api_keys";
    private static final String TRANSFORM = "AES/GCM/NoPadding";

    private ApiClient() {
    }

    // ── Key storage (AndroidKeyStore) ─────────────────────────────────────

    private static SecretKey key() throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        if (ks.containsAlias(ALIAS)) {
            return ((KeyStore.SecretKeyEntry) ks.getEntry(ALIAS, null)).getSecretKey();
        }
        KeyGenerator gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        gen.init(new KeyGenParameterSpec.Builder(ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .build());
        return gen.generateKey();
    }

    public static boolean setKey(Context context, String provider, String apiKey) {
        try {
            if (apiKey == null || apiKey.trim().isEmpty()) {
                return false;
            }
            Cipher cipher = Cipher.getInstance(TRANSFORM);
            cipher.init(Cipher.ENCRYPT_MODE, key());
            byte[] iv = cipher.getIV();
            byte[] enc = cipher.doFinal(apiKey.trim().getBytes("UTF-8"));
            ByteArrayOutputStream all = new ByteArrayOutputStream();
            all.write(iv);
            all.write(enc);
            prefs(context).edit()
                    .putString(provider, Base64.encodeToString(all.toByteArray(), Base64.NO_WRAP))
                    .apply();
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "setKey failed for " + provider, t);
            return false;
        }
    }

    public static boolean hasKey(Context context, String provider) {
        return !readKey(context, provider).isEmpty();
    }

    public static void clearKey(Context context, String provider) {
        try {
            prefs(context).edit().remove(provider).apply();
        } catch (Throwable ignored) {
        }
    }

    private static String readKey(Context context, String provider) {
        try {
            String stored = prefs(context).getString(provider, "");
            if (stored == null || stored.isEmpty()) {
                return "";
            }
            byte[] all = Base64.decode(stored, Base64.NO_WRAP);
            if (all.length <= 12) {
                return "";
            }
            byte[] iv = new byte[12];
            System.arraycopy(all, 0, iv, 0, 12);
            Cipher cipher = Cipher.getInstance(TRANSFORM);
            cipher.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(128, iv));
            byte[] plain = cipher.doFinal(all, 12, all.length - 12);
            return new String(plain, "UTF-8").trim();
        } catch (Throwable t) {
            return "";
        }
    }

    private static SharedPreferences prefs(Context context) {
        return context.getApplicationContext()
                .getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /** Free bytes on the volume holding {@code path} (download precheck). */
    public static long freeSpaceBytes(String path) {
        try {
            android.os.StatFs fs = new android.os.StatFs(path);
            return fs.getAvailableBytes();
        } catch (Throwable t) {
            return -1L;
        }
    }

    // ── Transcription ─────────────────────────────────────────────────────

    public static String transcribe(Context context, String provider, String model,
                                    String wavPath, String language) {
        try {
            String apiKey = readKey(context, provider);
            if (apiKey.isEmpty()) {
                return "ERR\nNo API key stored for " + provider + " — connect it in Models first";
            }
            File wav = new File(wavPath);
            if (!wav.isFile() || wav.length() <= 44L) {
                return "ERR\nRecording file is missing or empty";
            }
            byte[] audio = readAll(wav);
            if ("deepgram".equals(provider)) {
                return deepgram(apiKey, model, audio, language);
            } else if ("openai".equals(provider)) {
                return openAiCompatible("https://api.openai.com/v1/audio/transcriptions",
                        apiKey, model, audio, language);
            } else if ("groq".equals(provider)) {
                return openAiCompatible("https://api.groq.com/openai/v1/audio/transcriptions",
                        apiKey, model, audio, language);
            }
            return "ERR\nUnknown provider " + provider;
        } catch (Throwable t) {
            // Never include the key; class + short message only.
            String msg = t.getMessage();
            return "ERR\n" + (msg == null || msg.isEmpty() ? t.getClass().getSimpleName() : msg);
        }
    }

    private static String openAiCompatible(String endpoint, String apiKey, String model,
                                           byte[] audio, String language) throws Exception {
        String boundary = "quickstt-" + System.currentTimeMillis();
        HttpURLConnection conn = (HttpURLConnection) new URL(endpoint).openConnection();
        conn.setConnectTimeout(15000);
        conn.setReadTimeout(120000);
        conn.setDoOutput(true);
        conn.setRequestMethod("POST");
        conn.setRequestProperty("Authorization", "Bearer " + apiKey);
        conn.setRequestProperty("Content-Type", "multipart/form-data; boundary=" + boundary);
        OutputStream out = conn.getOutputStream();
        writeField(out, boundary, "model", model);
        if (language != null && !language.isEmpty() && !"auto".equals(language)) {
            writeField(out, boundary, "language", language);
        }
        writeField(out, boundary, "response_format", "json");
        out.write(("--" + boundary + "\r\n"
                + "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                + "Content-Type: audio/wav\r\n\r\n").getBytes("UTF-8"));
        out.write(audio);
        out.write(("\r\n--" + boundary + "--\r\n").getBytes("UTF-8"));
        out.flush();
        out.close();
        return finish(conn, "text");
    }

    private static String deepgram(String apiKey, String model, byte[] audio,
                                   String language) throws Exception {
        StringBuilder qs = new StringBuilder("model=")
                .append(URLEncoder.encode(model, "UTF-8"))
                .append("&smart_format=true");
        if (language != null && !language.isEmpty() && !"auto".equals(language)) {
            qs.append("&language=").append(URLEncoder.encode(language, "UTF-8"));
        } else {
            qs.append("&detect_language=true");
        }
        HttpURLConnection conn = (HttpURLConnection)
                new URL("https://api.deepgram.com/v1/listen?" + qs).openConnection();
        conn.setConnectTimeout(15000);
        conn.setReadTimeout(120000);
        conn.setDoOutput(true);
        conn.setRequestMethod("POST");
        conn.setRequestProperty("Authorization", "Token " + apiKey);
        conn.setRequestProperty("Content-Type", "audio/wav");
        OutputStream out = conn.getOutputStream();
        out.write(audio);
        out.flush();
        out.close();
        return finish(conn, "transcript");
    }

    private static String finish(HttpURLConnection conn, String field) throws Exception {
        int code = conn.getResponseCode();
        InputStream stream = code >= 200 && code < 300
                ? conn.getInputStream() : conn.getErrorStream();
        String body = stream == null ? "" : new String(readAll(stream), "UTF-8");
        conn.disconnect();
        if (code < 200 || code >= 300) {
            String snippet = body.length() > 180 ? body.substring(0, 180) : body;
            return "ERR\nProvider answered HTTP " + code + (snippet.isEmpty() ? "" : ": " + snippet);
        }
        String text = extractJsonString(body, field);
        if (text == null) {
            return "ERR\nProvider reply had no transcript";
        }
        return "OK\n" + text.trim();
    }

    private static void writeField(OutputStream out, String boundary, String name,
                                   String value) throws Exception {
        out.write(("--" + boundary + "\r\nContent-Disposition: form-data; name=\""
                + name + "\"\r\n\r\n" + value + "\r\n").getBytes("UTF-8"));
    }

    private static byte[] readAll(File file) throws Exception {
        return readAll(new FileInputStream(file));
    }

    private static byte[] readAll(InputStream in) throws Exception {
        try {
            ByteArrayOutputStream buf = new ByteArrayOutputStream();
            byte[] chunk = new byte[32768];
            int n;
            while ((n = in.read(chunk)) > 0) {
                buf.write(chunk, 0, n);
            }
            return buf.toByteArray();
        } finally {
            try {
                in.close();
            } catch (Throwable ignored) {
            }
        }
    }

    /** Minimal JSON string-field extraction (handles \\ escapes). */
    static String extractJsonString(String body, String field) {
        if (body == null) {
            return null;
        }
        String key = "\"" + field + "\"";
        int at = body.indexOf(key);
        if (at < 0) {
            return null;
        }
        int colon = body.indexOf(':', at + key.length());
        if (colon < 0) {
            return null;
        }
        int i = colon + 1;
        while (i < body.length() && Character.isWhitespace(body.charAt(i))) {
            i++;
        }
        if (i >= body.length() || body.charAt(i) != '"') {
            return null;
        }
        StringBuilder out = new StringBuilder();
        for (i = i + 1; i < body.length(); i++) {
            char c = body.charAt(i);
            if (c == '"') {
                return out.toString();
            }
            if (c == '\\' && i + 1 < body.length()) {
                char e = body.charAt(++i);
                switch (e) {
                    case 'n': out.append('\n'); break;
                    case 't': out.append('\t'); break;
                    case 'r': out.append('\r'); break;
                    case 'u':
                        if (i + 4 < body.length()) {
                            try {
                                out.append((char) Integer.parseInt(body.substring(i + 1, i + 5), 16));
                            } catch (Throwable ignored) {
                            }
                            i += 4;
                        }
                        break;
                    default: out.append(e);
                }
            } else {
                out.append(c);
            }
        }
        return null;
    }
}
