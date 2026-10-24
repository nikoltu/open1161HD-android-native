package org.openbw.android;

import android.content.SharedPreferences;
import android.graphics.Bitmap;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;

/**
 * Launcher portraits.
 *
 * The player's identity is an ANIMATED unit portrait decoded from the user's own
 * game files (portrait\...\*.smk in StarDat/BrooDat). This class is the catalog
 * (arr\portdata.dat via JNI), the decoded-clip cache and the per-profile choice.
 * Nothing is written to disk: a clip decodes in ~1 ms and the app ships no art.
 */
final class PortraitStore {
    private PortraitStore() {}

    /** One portrait id of arr\portdata.dat. */
    static final class Entry {
        int id;
        String idle = "", talk = "";   // tbl base, e.g. "tmarine\TMaFid0"
        int nIdle, nTalk;              // variants on disk (0..4)
        int change;                    // smk_change: % chance to switch idle variant at clip end
        String[] voices = {};          // the unit's "what" voice lines (sound\... paths in the archives)
    }

    /** A decoded clip: all frames side by side in one bitmap. */
    static final class Clip {
        Bitmap strip;
        int w, h, frames, msPerFrame;
        int cols;   // 0 = one row of frames (portraits); > 0 = a sheet with this many frames per row
        boolean hd; // an SC:R HD2 clip (224 x 209 after the 15:14 crop): drawn filtered, no scanlines
    }

    // ---- roster: units, heroes and the race advisors (the neutral critters,
    // resources, crystals and flags -- 30 more clips in portdata -- are not offered) ----
    // {race label, subgroup label, {id, name, id, name, ...}}
    static final Object[][] GROUPS = {
        {"Terran", "Kariai ir technika", new Object[]{0, "Marine", 2, "Firebat", 90, "Medic", 1, "Ghost", 3, "Vulture",
                4, "Spider Mine", 6, "Siege Tank", 5, "Goliath", 7, "SCV", 8, "Wraith", 10, "Dropship",
                9, "Science Vessel", 91, "Valkyrie", 11, "Battlecruiser", 15, "Civilian"}},
        {"Terran", "Patarėjas", new Object[]{17, "Adjutant"}},
        {"Terran", "Herojai", new Object[]{13, "Jim Raynor", 12, "Sarah Kerrigan", 14, "Edmund Duke",
                16, "Arcturus Mengsk", 92, "Gerard DuGalle", 93, "Alexei Stukov", 94, "Samir Duran"}},
        {"Zerg", "Padarai", new Object[]{24, "Drone", 20, "Zergling", 21, "Hydralisk", 98, "Lurker", 22, "Ultralisk",
                29, "Defiler", 25, "Overlord", 26, "Mutalisk", 27, "Guardian", 97, "Devourer", 30, "Scourge",
                28, "Queen", 23, "Broodling", 33, "Infested Terran", 18, "Larva", 19, "Egg", 31, "Cocoon"}},
        {"Zerg", "Patarėjas", new Object[]{38, "Zerg patarėjas"}},
        {"Zerg", "Herojai", new Object[]{36, "Infested Kerrigan", 34, "Zasz", 35, "Daggoth", 37, "Hunter Killer"}},
        {"Protoss", "Kariai ir laivai", new Object[]{39, "Probe", 40, "Zealot", 41, "Dragoon", 42, "High Templar",
                49, "Dark Templar", 43, "Archon", 99, "Dark Archon", 56, "Reaver", 44, "Shuttle", 55, "Observer",
                45, "Scout", 100, "Corsair", 47, "Carrier", 48, "Interceptor", 46, "Arbiter"}},
        {"Protoss", "Patarėjas", new Object[]{59, "Aldaris"}},
        {"Protoss", "Herojai", new Object[]{52, "Zeratul", 53, "Tassadar", 50, "Fenix (Zealot)", 51, "Fenix (Dragoon)",
                95, "Artanis", 96, "Raszagal"}},
    };

    private static final Map<Integer, String> NAMES = new HashMap<>();
    static {
        for (Object[] g : GROUPS) {
            Object[] items = (Object[]) g[2];
            for (int i = 0; i + 1 < items.length; i += 2) NAMES.put((Integer) items[i], (String) items[i + 1]);
        }
    }

    static String name(int id) {
        String n = NAMES.get(id);
        return n != null ? S.t(n) : ("#" + id);
    }

    // ---- native ---------------------------------------------------------------
    private static boolean libOk;
    static {
        try { System.loadLibrary("openbw_minimap"); libOk = true; }
        catch (Throwable t) { libOk = false; }
    }
    private static native String nativeCatalog(String gameDir);
    private static native int[] nativeDecodeSmk(String gameDir, String path);
    private static native int[] nativeDecodePcx(String gameDir, String path);
    private static native int[] nativeDecodeSmkStep(String gameDir, String path, int step, int maxFrames);
    private static native int[] nativeDecodeWebm(String cascDir, String name, int maxFrames);
    private static native int[] nativeDecodeDds(String cascDir, String name);

    // ---- HD portraits (F5): hd2/portrait/<folder>/<code><variant>.webm in the user's SC:R ----
    // A clip takes hundreds of ms to decode (the project's own VP9 decoder; measured on
    // the PC: 52-126 ms), so: always on one background thread, the SD clip shows at once
    // and the HD one replaces it; decoded strips are kept as JPEG in cacheDir/portraits
    // (not in backups), so the next launcher start shows HD after a ~10 ms file read.
    interface ClipSink { void got(Clip c); }
    private static String cascDir = "";
    private static File hdCacheDir;
    private static final long HD_CACHE_BYTES = 44L * 1024 * 1024;
    private static long hdBytes = 0;
    private static final LinkedHashMap<String, Clip> hdClips = new LinkedHashMap<>(16, 0.75f, true);
    private static final java.util.Set<String> hdMissing = new java.util.HashSet<>();
    private static final Map<String, List<ClipSink>> hdWaiting = new HashMap<>();
    // Two workers: the big portrait on screen must not queue behind a list of small ones.
    private static final java.util.concurrent.ExecutorService hdExecFull = java.util.concurrent.Executors.newSingleThreadExecutor();
    private static final java.util.concurrent.ExecutorService hdExecHalf = java.util.concurrent.Executors.newSingleThreadExecutor();
    private static final android.os.Handler ui = new android.os.Handler(android.os.Looper.getMainLooper());

    /** HD is used when an SC:R folder is set and the graphics tier is not SD. */
    static synchronized void configureHd(String scrDir, String tier, File cacheDir) {
        String d = scrDir == null || "sd".equals(tier) ? "" : scrDir;
        if (!d.equals(cascDir)) { hdClips.clear(); hdBytes = 0; hdMissing.clear(); }
        cascDir = d;
        hdCacheDir = cacheDir == null ? null : new File(cacheDir, "portraits");
    }
    static synchronized boolean hdEnabled() { return libOk && !cascDir.isEmpty() && hdCacheDir != null; }

    // `half`: the clip at half resolution (112 x 104) for row-sized portraits -- a
    // quarter of the memory, so a whole gallery of them fits (67 x ~0.23 MB).
    private static String hdKey(int id, boolean talk, int variant) { return id + (talk ? "t" : "i") + variant; }
    private static String memKey(int id, boolean talk, int variant, boolean half) { return hdKey(id, talk, variant) + (half ? "h" : ""); }

    static synchronized Clip hdCached(int id, boolean talk, int variant, boolean half) { return hdClips.get(memKey(id, talk, variant, half)); }
    static synchronized boolean hdKnownMissing(int id, boolean talk, int variant) { return hdMissing.contains(hdKey(id, talk, variant)); }

    /** Delivers the HD clip on the UI thread, or null when there is none for this portrait. */
    static void hdAsync(File gameDir, final int id, final boolean talk, final int variant, final boolean half, ClipSink sink) {
        final String key = hdKey(id, talk, variant), mkey = memKey(id, talk, variant, half);
        final String dir;
        final File cache;
        final Entry e;
        synchronized (PortraitStore.class) {
            Clip c = hdClips.get(mkey);
            if (c != null || hdMissing.contains(key) || !hdEnabled()) { final Clip fc = c; ui.post(() -> sink.got(fc)); return; }
            e = catalog(gameDir).get(id);
            if (e == null) { ui.post(() -> sink.got(null)); return; }
            List<ClipSink> w = hdWaiting.get(mkey);
            if (w != null) { w.add(sink); return; }       // already being decoded
            w = new ArrayList<>();
            w.add(sink);
            hdWaiting.put(mkey, w);
            dir = cascDir;
            cache = hdCacheDir;
        }
        (half ? hdExecHalf : hdExecFull).execute(() -> {
            Clip c = null;
            try {
                String tag = "hd2-" + Integer.toHexString(dir.hashCode()) + "-" + key + "-";
                // one file per clip: the two workers must not both decode the same one
                synchronized (("hdfile:" + tag).intern()) {
                    cache.mkdirs();
                    File hit = null;
                    File[] fs = cache.listFiles();
                    if (fs != null) for (File f : fs) if (f.getName().startsWith(tag) && f.getName().endsWith(".jpg")) hit = f;
                    if (hit == null) {
                        String name = "HD2/portrait/" + (talk ? e.talk : e.idle).replace('\\', '/').toLowerCase(java.util.Locale.ROOT) + variant + ".webm";
                        int[] px = nativeDecodeWebm(dir, name, 48);
                        if (px != null && px.length >= 4 && px[0] > 0 && px[1] > 0 && px[2] > 0 && px.length >= 4 + px[0] * px[2] * px[1]) {
                            Bitmap full = Bitmap.createBitmap(px, 4, px[0] * px[2], px[0] * px[2], px[1], Bitmap.Config.RGB_565);
                            File out = new File(cache, tag + px[0] + "x" + px[1] + "x" + px[2] + ".jpg");
                            try (java.io.FileOutputStream os = new java.io.FileOutputStream(out)) {
                                full.compress(Bitmap.CompressFormat.JPEG, 92, os);
                                hit = out;
                            } catch (Exception ignored) {}
                            if (!half || hit == null) {
                                c = new Clip();
                                c.strip = full; c.w = px[0]; c.h = px[1]; c.frames = px[2]; c.msPerFrame = 100; c.hd = true;
                                if (half) {     // could not write the cache: scale in memory
                                    c.strip = Bitmap.createScaledBitmap(full, (px[0] / 2) * px[2], px[1] / 2, true);
                                    c.w = px[0] / 2; c.h = px[1] / 2;
                                }
                            }
                        }
                    }
                    if (c == null && hit != null) {
                        // "<tag><w>x<h>x<frames>.jpg"; half = the JPEG decoded at 1:2
                        String[] d = hit.getName().substring(tag.length(), hit.getName().length() - 4).split("x");
                        android.graphics.BitmapFactory.Options o = new android.graphics.BitmapFactory.Options();
                        o.inPreferredConfig = Bitmap.Config.RGB_565;
                        o.inSampleSize = half ? 2 : 1;
                        Bitmap b = android.graphics.BitmapFactory.decodeFile(hit.getAbsolutePath(), o);
                        if (b != null && d.length == 3) {
                            int frames = Integer.parseInt(d[2]);
                            c = new Clip();
                            c.strip = b; c.frames = frames; c.msPerFrame = 100; c.hd = true;
                            c.w = b.getWidth() / frames; c.h = b.getHeight();
                            if (c.w <= 0 || c.w * frames != b.getWidth()) c = null;
                        }
                    }
                }
            } catch (Throwable t) {
                c = null;
            }
            final Clip fc = c;
            final List<ClipSink> sinks;
            synchronized (PortraitStore.class) {
                if (fc != null) {
                    hdClips.put(mkey, fc);
                    hdBytes += (long) fc.strip.getByteCount();
                    Iterator<Map.Entry<String, Clip>> it = hdClips.entrySet().iterator();
                    while (hdBytes > HD_CACHE_BYTES && it.hasNext()) {
                        Map.Entry<String, Clip> en = it.next();
                        if (en.getValue() == fc) continue;
                        hdBytes -= en.getValue().strip.getByteCount();
                        it.remove();
                    }
                } else hdMissing.add(key);
                sinks = hdWaiting.remove(mkey);
            }
            if (sinks != null) ui.post(() -> { for (ClipSink sk : sinks) sk.got(fc); });
        });
    }

    // ---- menu art clips (campaign emblems: glue\campaign\terr.smk ...) -------------
    // Measured: ~200x220, 70 frames at 66 ms. Kept as every 2nd frame in an RGB_565
    // sheet (several rows, so no side exceeds a texture limit): ~3 MB per emblem.
    private static final Map<String, Clip> art = new HashMap<>();
    /** Decodes (once) an SMK from the game archives; null when it is not there. Call off the UI thread. */
    static Clip art(File gameDir, String path) {
        if (!libOk || gameDir == null) return null;
        String key = gameDir.getAbsolutePath() + "|" + path;
        synchronized (art) { if (art.containsKey(key)) return art.get(key); }
        Clip c = null;
        int[] px;
        try { px = nativeDecodeSmkStep(gameDir.getAbsolutePath(), path, 2, 40); } catch (Throwable t) { px = null; }
        if (px != null && px.length >= 4 && px[0] > 0 && px[1] > 0 && px[2] > 0) {
            int w = px[0], h = px[1], frames = px[2];
            int stride = (px.length - 4) / h;            // the native strip is as wide as it announced
            int cols = Math.max(1, Math.min(frames, 2048 / w));
            int rows = (frames + cols - 1) / cols;
            try {
                Bitmap sheet = Bitmap.createBitmap(w * cols, h * rows, Bitmap.Config.RGB_565);
                for (int f = 0; f != frames; ++f)
                    sheet.setPixels(px, 4 + f * w, stride, (f % cols) * w, (f / cols) * h, w, h);
                c = new Clip();
                c.strip = sheet; c.w = w; c.h = h; c.frames = frames; c.cols = cols;
                c.msPerFrame = Math.max(33, px[3] / 1000);
            } catch (Throwable t) { c = null; }
        }
        synchronized (art) { art.put(key, c); }
        return c;
    }

    // ---- launcher backdrop: the main-menu space scene from the user's StarDat ------
    private static Bitmap backdrop;
    private static String backdropDir = "";
    static synchronized Bitmap cachedBackground(File gameDir) {
        return gameDir != null && gameDir.getAbsolutePath().equals(backdropDir) ? backdrop : null;
    }
    /** Decodes glue\palmm\backgnd.pcx (640x480); null without game files. Call off the UI thread. */
    static Bitmap background(File gameDir) {
        Bitmap c = cachedBackground(gameDir);
        if (c != null) return c;
        if (!libOk || gameDir == null || !new File(gameDir, "StarDat.mpq").exists()) return null;
        int[] px;
        try { px = nativeDecodePcx(gameDir.getAbsolutePath(), "glue\\palmm\\backgnd.pcx"); } catch (Throwable t) { px = null; }
        if (px == null || px.length < 2 || px[0] <= 0 || px[1] <= 0 || px.length < 2 + px[0] * px[1]) return null;
        Bitmap b = Bitmap.createBitmap(px, 2, px[0], px[0], px[1], Bitmap.Config.ARGB_8888);
        synchronized (PortraitStore.class) { backdrop = b; backdropDir = gameDir.getAbsolutePath(); }
        return b;
    }

    // ---- start-up title picture (v1.0.9) ---------------------------------------
    /** The original title screen, glue\\title\\title.pcx of the user's archives (640x480). Off the UI thread. */
    static Bitmap titleClassic(File gameDir) {
        if (!libOk || gameDir == null || !new File(gameDir, "StarDat.mpq").exists()) return null;
        int[] px;
        try { px = nativeDecodePcx(gameDir.getAbsolutePath(), "glue\\title\\title.pcx"); } catch (Throwable t) { px = null; }
        if (px == null || px.length < 2 || px[0] <= 0 || px[1] <= 0 || px.length < 2 + px[0] * px[1]) return null;
        return Bitmap.createBitmap(px, 2, px[0], px[0], px[1], Bitmap.Config.ARGB_8888);
    }
    /** The SC:R title screen from the user's SC:R folder (full = 3412x1920, else 1706x960); null without it. */
    static Bitmap titleHd(String cascDir, boolean full) {
        if (!libOk || cascDir == null || cascDir.isEmpty()) return null;
        int[] px;
        try { px = nativeDecodeDds(cascDir, full ? "glue/title/title.dds" : "hd2/glue/title/title.dds"); } catch (Throwable t) { px = null; }
        if (px == null || px.length < 2 || px[0] <= 0 || px[1] <= 0 || px.length < 2 + px[0] * px[1]) return null;
        try {
            return Bitmap.createBitmap(px, 2, px[0], px[0], px[1], Bitmap.Config.ARGB_8888);
        } catch (Throwable t) { return null; }   // out of memory on a small device: the caller falls back
    }

    // ---- catalog ---------------------------------------------------------------
    private static String catalogDir = "";
    private static final Map<Integer, Entry> catalog = new HashMap<>();

    /** The portdata roster of this game dir (empty when there are no game files yet). */
    static synchronized Map<Integer, Entry> catalog(File gameDir) {
        String dir = gameDir == null ? "" : gameDir.getAbsolutePath();
        if (dir.equals(catalogDir) && !catalog.isEmpty()) return catalog;
        catalog.clear();
        catalogDir = dir;
        if (!libOk || dir.isEmpty() || !new File(gameDir, "StarDat.mpq").exists()) return catalog;
        String s;
        try { s = nativeCatalog(dir); } catch (Throwable t) { s = null; }
        if (s == null) return catalog;
        for (String line : s.split("\n")) {
            String[] f = line.split("\\|", -1);
            if (f.length < 6) continue;
            try {
                Entry e = new Entry();
                e.id = Integer.parseInt(f[0]);
                e.idle = f[1]; e.nIdle = Integer.parseInt(f[2]);
                e.talk = f[3]; e.nTalk = Integer.parseInt(f[4]);
                e.change = Integer.parseInt(f[5]);
                if (f.length > 6 && !f[6].isEmpty()) e.voices = f[6].split(";");
                catalog.put(e.id, e);
            } catch (NumberFormatException ignored) {}
        }
        return catalog;
    }

    static boolean available(File gameDir) { return !catalog(gameDir).isEmpty(); }

    static String smkPath(Entry e, boolean talk, int variant) {
        return "portrait\\" + (talk ? e.talk : e.idle) + variant + ".smk";
    }

    // ---- decoded clips (LRU, ~16 MB) --------------------------------------------
    private static final long CACHE_BYTES = 16L * 1024 * 1024;
    private static long cacheBytes = 0;
    private static final LinkedHashMap<String, Clip> clips = new LinkedHashMap<>(64, 0.75f, true);

    /** Decodes (or returns the cached) clip; null when the portrait or the files are missing. */
    static Clip clip(File gameDir, int id, boolean talk, int variant) {
        Entry e;
        String dir;
        synchronized (PortraitStore.class) {
            e = catalog(gameDir).get(id);
            if (e == null) return null;
            int n = talk ? e.nTalk : e.nIdle;
            if (n <= 0) return null;
            if (variant < 0 || variant >= n) variant = 0;
            dir = gameDir.getAbsolutePath();
            Clip c = clips.get(dir + "|" + id + "|" + talk + "|" + variant);
            if (c != null) return c;
        }
        int[] px;
        try { px = nativeDecodeSmk(dir, smkPath(e, talk, variant)); } catch (Throwable t) { px = null; }
        if (px == null || px.length < 4) return null;
        int w = px[0], h = px[1], frames = px[2];
        if (w <= 0 || h <= 0 || frames <= 0 || px.length < 4 + w * frames * h) return null;
        Clip c = new Clip();
        c.w = w; c.h = h; c.frames = frames;
        c.msPerFrame = Math.max(33, px[3] / 1000);
        // the strip is as wide as the clip was announced; the stride below keeps a
        // clip that ended early readable (frames < announced never happens in the MPQs)
        int stride = (px.length - 4) / h;
        c.strip = Bitmap.createBitmap(px, 4, stride, w * frames, h, Bitmap.Config.ARGB_8888);
        synchronized (PortraitStore.class) {
            String key = dir + "|" + id + "|" + talk + "|" + variant;
            Clip old = clips.put(key, c);
            if (old == null) cacheBytes += (long) w * frames * h * 4;
            Iterator<Map.Entry<String, Clip>> it = clips.entrySet().iterator();
            while (cacheBytes > CACHE_BYTES && it.hasNext()) {
                Map.Entry<String, Clip> en = it.next();
                if (en.getValue() == c) continue;
                cacheBytes -= (long) en.getValue().w * en.getValue().frames * en.getValue().h * 4;
                it.remove();
            }
        }
        return c;
    }

    // ---- the profile's choice ----------------------------------------------------
    // p.<profile>.portrait      int     portdata id, default 0 (Marine)
    // p.<profile>.portraitMode  string  "fixed" | "random" (re-rolled once per app start)
    static final int DEFAULT_ID = 0;
    private static final Map<String, Integer> rolled = new HashMap<>();   // survives recreate(), not the process

    static int chosenId(SharedPreferences pr, String profile) {
        int id = pr.getInt("p." + profile + ".portrait", DEFAULT_ID);
        return NAMES.containsKey(id) ? id : DEFAULT_ID;   // a pick that is no longer offered falls back to the Marine
    }
    static boolean isRandom(SharedPreferences pr, String profile) {
        return "random".equals(pr.getString("p." + profile + ".portraitMode", "fixed"));
    }
    static void choose(SharedPreferences pr, String profile, int id, boolean random) {
        pr.edit().putInt("p." + profile + ".portrait", id)
                .putString("p." + profile + ".portraitMode", random ? "random" : "fixed").apply();
        synchronized (PortraitStore.class) { rolled.remove(profile); }
    }

    // ---- AI opponents (LAUNCHER-REDESIGN 9.1; the same table lives in ui.h) ---------
    // race: 0 Zerg, 1 Terran, 2 Protoss. level: -1 original aiscript AI / other bots /
    // a human whose build sent no portrait, 0..3 the built-in AI's difficulty.
    private static final int[][] AI_PORTRAIT = {
        {38, 17, 59},   // original AI: the race advisor
        {24, 7, 39},    // very easy: Drone, SCV, Probe
        {20, 0, 40},    // easy: Zergling, Marine, Zealot
        {21, 1, 41},    // medium: Hydralisk, Ghost, Dragoon
        {22, 11, 47},   // hard: Ultralisk, Battlecruiser, Carrier
    };
    /** -1 when the race is not known (Random): the caller shows the "no signal" tile. */
    static int aiPortrait(int level, int race) {
        if (race < 0 || race > 2) return -1;
        return AI_PORTRAIT[(level < 0 || level > 3) ? 0 : level + 1][race];
    }

    /** The portrait to show for this profile right now. */
    static int activeId(SharedPreferences pr, String profile, File gameDir) {
        if (!isRandom(pr, profile)) return chosenId(pr, profile);
        synchronized (PortraitStore.class) {
            Integer r = rolled.get(profile);
            if (r != null) return r;
            // "random each time": any portrait of the roster
            List<Integer> pool = new ArrayList<>();
            Map<Integer, Entry> cat = catalog(gameDir);
            for (Object[] g : GROUPS) {
                Object[] items = (Object[]) g[2];
                for (int i = 0; i + 1 < items.length; i += 2) {
                    Entry e = cat.get((Integer) items[i]);
                    if (e != null && e.nIdle > 0) pool.add(e.id);
                }
            }
            int id = pool.isEmpty() ? chosenId(pr, profile) : pool.get(new Random().nextInt(pool.size()));
            rolled.put(profile, id);
            return id;
        }
    }
}
