package org.openbw.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * G10: one fingerprint of the GAME DATA the active mods feed the engine
 * (files/mods/&lt;id&gt;/data.mpq and files/mods/&lt;id&gt;/data/, the two layers the
 * native loader attaches; anim/ ui/ icons/ tileset/ are presentation only).
 *
 * SHA-256 over, for each mod id in launch order that ships game data:
 *   "mod\n" id "\n", then "data.mpq\n" sha256hex "\n" if present, then for every
 *   file under data/ sorted by its '/'-relative path: "data/" path "\n" sha256hex "\n".
 * The fingerprint is the first 8 bytes as 16 lowercase hex digits, "" when no active
 * mod ships game data (the classic path: nothing is sent, stored or compared).
 *
 * Per-file hashes are cached by path + size + mtime, so a launch re-hashes only
 * what changed.
 */
final class ModFingerprint {
    private ModFingerprint() {}

    static String of(Context ctx, String modsCsv) {
        if (modsCsv == null || modsCsv.isEmpty()) return "";
        File root = new File(ctx.getExternalFilesDir(null), "mods");
        SharedPreferences cache = ctx.getSharedPreferences("modfp", Context.MODE_PRIVATE);
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            boolean any = false;
            for (String id : modsCsv.split(",")) {
                if (id.isEmpty()) continue;
                File dir = new File(root, id);
                File mpq = new File(dir, "data.mpq");
                File data = new File(dir, "data");
                List<String> rel = new ArrayList<>();
                if (data.isDirectory()) collect(data, "", rel);
                Collections.sort(rel);
                if (!mpq.isFile() && rel.isEmpty()) continue;
                any = true;
                put(md, "mod\n" + id + "\n");
                if (mpq.isFile()) put(md, "data.mpq\n" + fileHash(mpq, cache) + "\n");
                for (String r : rel) put(md, "data/" + r + "\n" + fileHash(new File(data, r), cache) + "\n");
            }
            if (!any) return "";
            byte[] d = md.digest();
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 8; i++) sb.append(String.format(java.util.Locale.ROOT, "%02x", d[i] & 0xff));
            return sb.toString();
        } catch (Exception e) {
            return "";
        }
    }

    /** The mod ids a save or launch named with --mods, or "" (args as written into a .sav). */
    static String modsArg(String[] args) {
        if (args == null) return "";
        String m = "";
        for (int i = 0; i + 1 < args.length; i++) if ("--mods".equals(args[i])) m = args[i + 1];
        return m;
    }

    private static void collect(File dir, String prefix, List<String> out) {
        File[] fs = dir.listFiles();
        if (fs == null) return;
        for (File f : fs) {
            String r = prefix.isEmpty() ? f.getName() : prefix + "/" + f.getName();
            if (f.isDirectory()) collect(f, r, out);
            else if (f.isFile()) out.add(r);
        }
    }

    private static void put(MessageDigest md, String s) {
        md.update(s.getBytes(StandardCharsets.UTF_8));
    }

    private static String fileHash(File f, SharedPreferences cache) throws IOException {
        String key = f.getAbsolutePath();
        String stamp = f.length() + ":" + f.lastModified() + ":";
        String hit = cache.getString(key, null);
        if (hit != null && hit.startsWith(stamp)) return hit.substring(stamp.length());
        MessageDigest md;
        try { md = MessageDigest.getInstance("SHA-256"); } catch (Exception e) { throw new IOException(e); }
        byte[] b = new byte[1 << 16];
        try (FileInputStream in = new FileInputStream(f)) {
            int n;
            while ((n = in.read(b)) > 0) md.update(b, 0, n);
        }
        StringBuilder sb = new StringBuilder();
        for (byte x : md.digest()) sb.append(String.format(java.util.Locale.ROOT, "%02x", x & 0xff));
        String h = sb.toString();
        cache.edit().putString(key, stamp + h).apply();
        return h;
    }
}
