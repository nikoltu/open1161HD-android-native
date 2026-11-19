package org.openbw.android;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.OpenableColumns;
import android.system.Os;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

/**
 * v1.0.27 first-run import from ZIP, 7z and RAR archives (split RAR included, in
 * any selection order). An archive may hold the 1.16.1 MPQs anywhere inside it
 * and/or a StarCraft: Remastered install (a folder with .build.info + Data/).
 *
 * MPQs land as files/&lt;Canonical&gt;.mpq; a Remastered install is unpacked to
 * files/casc/&lt;name&gt;/ (only .build.info and Data/), which the engine then
 * reads in place without all-files access. Every write goes to a .part file or
 * a .part folder first, so a failed or cancelled import leaves the previous
 * data untouched. RAR and 7z are decoded natively (libarcimport: RARLab UnRAR,
 * LZMA SDK); ZIP uses java.util.zip.
 */
final class ArchiveImporter {
    static final int KIND_NONE = 0, KIND_ZIP = 1, KIND_7Z = 2, KIND_RAR = 3;
    private static final int NATIVE_RAR = 1, NATIVE_7Z = 2;
    private static final long SPACE_MARGIN = 64L << 20;

    /** Called from native code (method name and signature are fixed). */
    interface Progress { boolean onBytes(long total); }

    interface Ui {
        void status(String text);
        boolean cancelled();
    }

    static final class Result {
        final List<File> mpqs = new ArrayList<>();
        String cascDir;
        final List<String> warnings = new ArrayList<>();
        int archives;
    }

    private static boolean loaded;
    private static Throwable loadError;

    private static synchronized boolean nativeReady() {
        if (!loaded && loadError == null) {
            try { System.loadLibrary("arcimport"); loaded = true; } catch (Throwable t) { loadError = t; }
        }
        return loaded;
    }

    private static native String[] nList(int kind, String path, int fd);
    private static native String nExtract(int kind, String path, int fd, String[] names, String[] dests, Progress p);

    private final Context ctx;
    private final ContentResolver cr;
    private final Ui ui;
    private final File filesDir;
    private final String[] mpqNames;
    private final Result res = new Result();

    private ArchiveImporter(Context ctx, Ui ui, String[] mpqNames) {
        this.ctx = ctx;
        this.cr = ctx.getContentResolver();
        this.ui = ui;
        this.filesDir = ctx.getExternalFilesDir(null);
        this.mpqNames = mpqNames;
    }

    /** Archive kind from the first bytes, or KIND_NONE. */
    static int sniff(ContentResolver cr, Uri uri) {
        byte[] h = new byte[8];
        int n = 0;
        try (InputStream in = cr.openInputStream(uri)) {
            if (in == null) return KIND_NONE;
            while (n < h.length) {
                int r = in.read(h, n, h.length - n);
                if (r <= 0) break;
                n += r;
            }
        } catch (Exception e) {
            return KIND_NONE;
        }
        if (n >= 4 && h[0] == 'P' && h[1] == 'K' && (h[2] == 3 || h[2] == 5 || h[2] == 7)) return KIND_ZIP;
        if (n >= 6 && h[0] == '7' && h[1] == 'z' && (h[2] & 0xff) == 0xBC && (h[3] & 0xff) == 0xAF && h[4] == 0x27 && h[5] == 0x1C) return KIND_7Z;
        if (n >= 7 && h[0] == 'R' && h[1] == 'a' && h[2] == 'r' && h[3] == '!' && h[4] == 0x1A && h[5] == 0x07) return KIND_RAR;
        return KIND_NONE;
    }

    static final class Picked {
        final Uri uri;
        final String name;
        final int kind;
        Picked(Uri uri, String name, int kind) { this.uri = uri; this.name = name; this.kind = kind; }
    }

    /**
     * @param archives  picked files whose sniffed kind is not KIND_NONE
     * @param buildInfo a loose .build.info picked next to a Data-only archive, or null
     * @param mpqNames  canonical MPQ names the launcher knows (required + optional)
     */
    static Result run(Context ctx, List<Picked> archives, Uri buildInfo, String[] mpqNames, Ui ui) {
        ArchiveImporter imp = new ArchiveImporter(ctx, ui, mpqNames);
        imp.runAll(archives, buildInfo);
        return imp.res;
    }

    // ------------------------------------------------------------------ grouping

    private static final Pattern PART = Pattern.compile("(?i)^(.*)\\.part(\\d+)\\.rar$");
    private static final Pattern OLD_FIRST = Pattern.compile("(?i)^(.*)\\.rar$");
    private static final Pattern OLD_NEXT = Pattern.compile("(?i)^(.*)\\.r(\\d\\d)$");

    private static final class Unit {
        final String label;
        final int kind;
        final List<Picked> volumes = new ArrayList<>(); // in volume order
        Unit(String label, int kind) { this.label = label; this.kind = kind; }
    }

    private void runAll(List<Picked> archives, Uri buildInfo) {
        List<Unit> units = new ArrayList<>();
        Map<String, TreeMap<Integer, Picked>> partSets = new LinkedHashMap<>();
        Map<String, TreeMap<Integer, Picked>> oldSets = new LinkedHashMap<>();
        for (Picked p : archives) {
            if (p.kind != KIND_RAR) {
                Unit u = new Unit(p.name, p.kind);
                u.volumes.add(p);
                units.add(u);
                continue;
            }
            Matcher m = PART.matcher(p.name);
            Map<String, TreeMap<Integer, Picked>> sets;
            String base;
            int number;
            if (m.matches()) {
                sets = partSets; base = m.group(1).toLowerCase(Locale.ROOT); number = Integer.parseInt(m.group(2));
            } else if ((m = OLD_NEXT.matcher(p.name)).matches()) {
                sets = oldSets; base = m.group(1).toLowerCase(Locale.ROOT); number = Integer.parseInt(m.group(2)) + 1;
            } else if ((m = OLD_FIRST.matcher(p.name)).matches()) {
                sets = oldSets; base = m.group(1).toLowerCase(Locale.ROOT); number = 0;
            } else {
                // a RAR under another name: a single-volume archive
                Unit u = new Unit(p.name, KIND_RAR);
                u.volumes.add(p);
                units.add(u);
                continue;
            }
            TreeMap<Integer, Picked> set = sets.get(base);
            if (set == null) sets.put(base, set = new TreeMap<>());
            if (set.containsKey(number)) {
                res.warnings.add(p.name + ": " + S.t("ši archyvo dalis pasirinkta du kartus."));
                continue;
            }
            set.put(number, p);
        }
        groupVolumes(partSets, 1, units);
        groupVolumes(oldSets, 0, units);
        for (Unit u : units) {
            if (ui.cancelled()) { res.warnings.add(S.t("Atšaukta.")); return; }
            importUnit(u, buildInfo);
        }
    }

    private void groupVolumes(Map<String, TreeMap<Integer, Picked>> sets, int first, List<Unit> units) {
        for (TreeMap<Integer, Picked> set : sets.values()) {
            Picked any = set.firstEntry().getValue();
            if (!set.containsKey(first)) {
                res.warnings.add(any.name + ": " + S.t("trūksta pirmosios archyvo dalies (.part1.rar arba .rar)."));
                continue;
            }
            int expect = first;
            boolean gap = false;
            for (int k : set.keySet()) {
                if (k != expect++) { gap = true; break; }
            }
            if (gap) {
                res.warnings.add(set.get(first).name + ": " + S.t("trūksta tarpinės archyvo dalies — pasirink visas dalis."));
                continue;
            }
            Unit u = new Unit(set.get(first).name, KIND_RAR);
            u.volumes.addAll(set.values());
            units.add(u);
        }
    }

    // ------------------------------------------------------------------ planning

    private static final class Ent {
        final String name;
        final long size;
        final boolean dir, enc, link;
        Ent(String name, long size, boolean dir, boolean enc, boolean link) {
            this.name = name; this.size = size; this.dir = dir; this.enc = enc; this.link = link;
        }
    }

    /** archive entry name -> destination file */
    private static final class Plan {
        final Map<String, File> targets = new LinkedHashMap<>();
        final Map<String, File> mpqFinal = new HashMap<>(); // .part file -> final
        File cascStage, cascFinal;
        long bytes;
    }

    private static String baseName(String path) {
        int s = path.lastIndexOf('/');
        return s >= 0 ? path.substring(s + 1) : path;
    }

    private static boolean safeComponent(String c) {
        return !c.isEmpty() && !c.equals(".") && !c.equals("..") && c.matches("[A-Za-z0-9._ +()-]+");
    }

    private String canonicalMpq(String base) {
        for (String m : mpqNames) if (m.equalsIgnoreCase(base)) return m;
        return null;
    }

    private Plan plan(Unit u, List<Ent> ents, boolean haveLooseBuildInfo) {
        Plan p = new Plan();
        // MPQs anywhere in the archive; the shallowest copy of a name wins
        Map<String, Ent> mpq = new HashMap<>();
        for (Ent e : ents) {
            if (e.dir || e.link) continue;
            String c = canonicalMpq(baseName(e.name));
            if (c == null) continue;
            Ent old = mpq.get(c);
            if (old == null || old.name.split("/").length > e.name.split("/").length) mpq.put(c, e);
        }
        for (Map.Entry<String, Ent> me : mpq.entrySet()) {
            File part = new File(filesDir, me.getKey() + ".part");
            p.targets.put(me.getValue().name, part);
            p.mpqFinal.put(part.getPath(), new File(filesDir, me.getKey()));
            p.bytes += Math.max(0, me.getValue().size);
        }
        // Remastered: the folder holding .build.info (shallowest) and its Data/
        String root = null;
        for (Ent e : ents) {
            if (e.dir || e.link || !baseName(e.name).equalsIgnoreCase(".build.info")) continue;
            String r = e.name.substring(0, e.name.length() - ".build.info".length());
            if (root == null || r.length() < root.length()) root = r;
        }
        boolean fromLoose = false;
        if (root == null) {
            for (Ent e : ents) {
                String lower = e.name.toLowerCase(Locale.ROOT);
                int k = lower.indexOf("data/data/data.");
                if (k < 0) k = lower.indexOf("data/indices/");
                if (k >= 0 && (k == 0 || lower.charAt(k - 1) == '/')) { root = e.name.substring(0, k); break; }
            }
            if (root == null) return p;
            if (!haveLooseBuildInfo) {
                res.warnings.add(u.label + ": " + S.t("archyve yra SC:R „Data“ aplankas, bet nėra „.build.info“. Pasirink ir „.build.info“ failą kartu su archyvu."));
                return p;
            }
            fromLoose = true;
        }
        String id = u.label.replaceAll("(?i)(\\.part\\d+)?\\.(rar|zip|7z)$", "").replaceAll("[^A-Za-z0-9._-]", "_");
        if (id.isEmpty()) id = "scr";
        File casc = new File(filesDir, "casc");
        p.cascFinal = new File(casc, id);
        p.cascStage = new File(casc, id + ".part");
        int skipped = 0;
        String rootLower = root.toLowerCase(Locale.ROOT);
        for (Ent e : ents) {
            if (e.dir || e.link) continue;
            String lower = e.name.toLowerCase(Locale.ROOT);
            if (!lower.startsWith(rootLower)) continue;
            String rel = e.name.substring(root.length());
            String relLower = rel.toLowerCase(Locale.ROOT);
            boolean info = relLower.equals(".build.info");
            if (!info && !relLower.startsWith("data/")) continue;
            boolean ok = true;
            for (String c : rel.split("/")) if (!safeComponent(c)) { ok = false; break; }
            if (!ok) { ++skipped; continue; }
            // CASC names are lower case (the engine opens <root>/data/data); keep .build.info as is
            p.targets.put(e.name, new File(p.cascStage, info ? ".build.info" : relLower));
            p.bytes += Math.max(0, e.size);
        }
        if (skipped > 0) res.warnings.add(u.label + ": " + S.t("praleista failų su netinkamais vardais: ") + skipped);
        if (fromLoose) p.targets.put("", new File(p.cascStage, ".build.info")); // filled from the loose file
        return p;
    }

    // ------------------------------------------------------------------ per archive

    private void importUnit(Unit u, Uri buildInfo) {
        List<ParcelFileDescriptor> open = new ArrayList<>();
        File tmpDir = null;
        Plan p = null;
        try {
            ui.status(S.t("Skaitomas archyvas:\n") + u.label);
            String nativePath = null;
            int fd = -1;
            List<Ent> ents;
            ZipFile zip = null;
            if (u.kind == KIND_ZIP) {
                ParcelFileDescriptor pfd = cr.openFileDescriptor(u.volumes.get(0).uri, "r");
                open.add(pfd);
                try { zip = new ZipFile("/proc/self/fd/" + pfd.getFd()); } catch (Exception e) { zip = null; }
                ents = zip != null ? listZip(zip) : listZipStream(u.volumes.get(0).uri);
            } else {
                if (!nativeReady()) {
                    res.warnings.add(u.label + ": " + S.t("RAR / 7z biblioteka neįkelta."));
                    return;
                }
                int kind = u.kind == KIND_RAR ? NATIVE_RAR : NATIVE_7Z;
                String[] listed;
                if (u.kind == KIND_RAR) {
                    tmpDir = new File(ctx.getCacheDir(), "rarvol");
                    deleteTree(tmpDir);
                    tmpDir.mkdirs();
                    for (Picked v : u.volumes) {
                        ParcelFileDescriptor pfd = cr.openFileDescriptor(v.uri, "r");
                        open.add(pfd);
                        Os.symlink("/proc/self/fd/" + pfd.getFd(), new File(tmpDir, safeName(v.name)).getPath());
                    }
                    nativePath = new File(tmpDir, safeName(u.volumes.get(0).name)).getPath();
                    listed = nList(kind, nativePath, -1);
                } else {
                    ParcelFileDescriptor pfd = cr.openFileDescriptor(u.volumes.get(0).uri, "r");
                    open.add(pfd);
                    fd = pfd.getFd();
                    listed = nList(kind, null, fd);
                }
                if ("open".equals(listed[0])) {
                    // descriptor not reusable by path / not seekable: stage real copies
                    deleteTree(tmpDir);
                    tmpDir = new File(filesDir, "import-tmp");
                    deleteTree(tmpDir);
                    tmpDir.mkdirs();
                    long need = 0;
                    for (Picked v : u.volumes) need += Math.max(0, uriSize(v.uri));
                    if (!enoughSpace(u.label, need)) return;
                    for (Picked v : u.volumes) {
                        ui.status(S.t("Kopijuojama:\n") + v.name);
                        copyStream(cr.openInputStream(v.uri), new File(tmpDir, safeName(v.name)), null);
                    }
                    nativePath = new File(tmpDir, safeName(u.volumes.get(0).name)).getPath();
                    fd = -1;
                    listed = nList(kind, nativePath, -1);
                }
                if (!listed[0].isEmpty()) {
                    res.warnings.add(u.label + ": " + errorText(listed[0]));
                    return;
                }
                ents = new ArrayList<>();
                for (int i = 1; i < listed.length; ++i) {
                    String[] f = listed[i].split("\t", 3);
                    ents.add(new Ent(f[2], Long.parseLong(f[1]), f[0].contains("d"), f[0].contains("e"), f[0].contains("l")));
                }
            }
            ++res.archives;
            p = plan(u, ents, buildInfo != null);
            if (p.targets.isEmpty()) {
                if (p.cascStage == null) res.warnings.add(u.label + ": " + S.t("archyve nerasta nei StarCraft 1.16.1 MPQ failų, nei SC:R aplanko."));
                return;
            }
            for (Ent e : ents) {
                if (e.enc && p.targets.containsKey(e.name)) { res.warnings.add(u.label + ": " + errorText("password")); return; }
            }
            if (!enoughSpace(u.label, p.bytes)) return;
            if (p.cascStage != null) { deleteTree(p.cascStage); p.cascStage.mkdirs(); }
            for (File f : p.targets.values()) {
                File parent = f.getParentFile();
                if (parent != null) parent.mkdirs();
                f.delete();
            }
            File loose = p.targets.remove("");
            if (loose != null) copyStream(cr.openInputStream(buildInfo), loose, null);

            final long total = Math.max(1, p.bytes);
            final String label = u.label;
            final long[] lastShown = {-1};
            Progress prog = done -> {
                long mb = done >> 20;
                if (mb != lastShown[0]) {
                    lastShown[0] = mb;
                    ui.status(S.t("Išskleidžiama:\n") + label + "\n" + mb + " / " + (total >> 20) + " MB");
                }
                return !ui.cancelled();
            };
            String err;
            if (u.kind == KIND_ZIP) {
                err = zip != null ? extractZip(zip, p.targets, prog) : extractZipStream(u.volumes.get(0).uri, p.targets, prog);
            } else {
                List<String> names = new ArrayList<>(p.targets.keySet());
                String[] dests = new String[names.size()];
                for (int i = 0; i < dests.length; ++i) dests[i] = p.targets.get(names.get(i)).getPath();
                err = nExtract(u.kind == KIND_RAR ? NATIVE_RAR : NATIVE_7Z, nativePath, fd, names.toArray(new String[0]), dests, prog);
            }
            if (zip != null) try { zip.close(); } catch (Exception ignored) {}
            if (err != null) {
                res.warnings.add(u.label + ": " + errorText(err));
                rollback(p);
                return;
            }
            commit(u, p);
        } catch (Exception e) {
            res.warnings.add(u.label + ": " + S.t("importas nepavyko: ") + e.getMessage());
            if (p != null) rollback(p);
        } finally {
            for (ParcelFileDescriptor pfd : open) try { pfd.close(); } catch (Exception ignored) {}
            if (tmpDir != null) deleteTree(tmpDir);
        }
    }

    private void commit(Unit u, Plan p) throws IOException {
        for (Map.Entry<String, File> t : p.targets.entrySet()) {
            File part = t.getValue();
            File fin = p.mpqFinal.get(part.getPath());
            if (fin == null) continue;
            if (fin.exists() && !fin.delete()) throw new IOException(S.f("nepavyko perrašyti %s", fin.getName()));
            if (!part.renameTo(fin)) throw new IOException(S.f("nepavyko pervadinti %s", part.getName()));
            res.mpqs.add(fin);
        }
        if (p.cascStage != null && p.cascStage.isDirectory()) {
            boolean ok = new File(p.cascStage, ".build.info").isFile() && new File(p.cascStage, "data/data").isDirectory();
            if (!ok) {
                res.warnings.add(u.label + ": " + S.t("SC:R aplankas archyve nepilnas (trūksta „data/data“ arba „.build.info“)."));
                deleteTree(p.cascStage);
                return;
            }
            deleteTree(p.cascFinal);
            if (!p.cascStage.renameTo(p.cascFinal)) throw new IOException(S.f("nepavyko pervadinti %s", p.cascStage.getName()));
            res.cascDir = p.cascFinal.getAbsolutePath();
        }
    }

    private void rollback(Plan p) {
        for (File f : p.targets.values()) if (p.mpqFinal.containsKey(f.getPath())) f.delete();
        if (p.cascStage != null) deleteTree(p.cascStage);
    }

    // ------------------------------------------------------------------ ZIP

    private static List<Ent> listZip(ZipFile zip) {
        List<Ent> out = new ArrayList<>();
        for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
            ZipEntry e = en.nextElement();
            out.add(new Ent(e.getName().replace('\\', '/'), e.getSize(), e.isDirectory(), false, false));
        }
        return out;
    }

    private List<Ent> listZipStream(Uri uri) throws IOException {
        List<Ent> out = new ArrayList<>();
        try (ZipInputStream zin = new ZipInputStream(cr.openInputStream(uri))) {
            ZipEntry e;
            while ((e = zin.getNextEntry()) != null) {
                if (ui.cancelled()) throw new IOException(S.t("Atšaukta."));
                out.add(new Ent(e.getName().replace('\\', '/'), e.getSize(), e.isDirectory(), false, false));
            }
        }
        return out;
    }

    private String extractZip(ZipFile zip, Map<String, File> targets, Progress prog) {
        long[] done = {0};
        try {
            for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                File dst = targets.get(e.getName().replace('\\', '/'));
                if (dst == null || e.isDirectory()) continue;
                if (!copyStream(zip.getInputStream(e), dst, n -> prog.onBytes(done[0] += n))) return "cancel";
            }
        } catch (java.util.zip.ZipException ze) {
            String m = ze.getMessage();
            return m != null && m.toLowerCase(Locale.ROOT).contains("encrypt") ? "password" : "corrupt";
        } catch (IOException ioe) {
            return "write";
        }
        return null;
    }

    private String extractZipStream(Uri uri, Map<String, File> targets, Progress prog) {
        long[] done = {0};
        int got = 0;
        try (ZipInputStream zin = new ZipInputStream(cr.openInputStream(uri))) {
            ZipEntry e;
            while ((e = zin.getNextEntry()) != null) {
                File dst = targets.get(e.getName().replace('\\', '/'));
                if (dst == null || e.isDirectory()) continue;
                if (!copyStream(new NonClosing(zin), dst, n -> prog.onBytes(done[0] += n))) return "cancel";
                ++got;
            }
        } catch (java.util.zip.ZipException ze) {
            String m = ze.getMessage();
            return m != null && m.toLowerCase(Locale.ROOT).contains("encrypt") ? "password" : "corrupt";
        } catch (IOException ioe) {
            return "write";
        }
        return got < targets.size() ? "corrupt" : null;
    }

    private static final class NonClosing extends java.io.FilterInputStream {
        NonClosing(InputStream in) { super(in); }
        @Override public void close() {}
    }

    // ------------------------------------------------------------------ helpers

    private interface Chunk { boolean add(long n); }

    /** Copies and fsyncs; returns false when cancelled (the file is removed). */
    private boolean copyStream(InputStream in, File dst, Chunk chunk) throws IOException {
        if (in == null) throw new IOException(S.t("nepavyko atidaryti"));
        try (InputStream src = in; FileOutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = src.read(buf)) > 0) {
                out.write(buf, 0, n);
                if (ui.cancelled() || (chunk != null && !chunk.add(n))) {
                    out.close();
                    dst.delete();
                    return false;
                }
            }
            out.flush();
            try { out.getFD().sync(); } catch (Exception ignored) {}
        }
        return true;
    }

    private boolean enoughSpace(String label, long need) {
        long free;
        try { free = new StatFs(filesDir.getPath()).getAvailableBytes(); } catch (Exception e) { return true; }
        if (need + SPACE_MARGIN <= free) return true;
        res.warnings.add(label + ": " + S.f("nepakanka vietos: reikia %s, laisva %s.", mb(need + SPACE_MARGIN), mb(free)));
        return false;
    }

    private static String mb(long b) { return (b >> 20) + " MB"; }

    private long uriSize(Uri uri) {
        try (Cursor c = cr.query(uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (Exception ignored) {}
        return -1;
    }

    private static String safeName(String name) {
        String s = name.replaceAll("[/\\\\:]", "_");
        return s.isEmpty() ? "archive" : s;
    }

    static void deleteTree(File f) {
        if (f == null || !f.exists()) return;
        File[] kids = f.isDirectory() && !isSymlink(f) ? f.listFiles() : null;
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    private static boolean isSymlink(File f) {
        try { return android.system.OsConstants.S_ISLNK(Os.lstat(f.getPath()).st_mode); } catch (Exception e) { return false; }
    }

    static String errorText(String token) {
        switch (token) {
            case "password": return S.t("archyvas apsaugotas slaptažodžiu — tokie nepalaikomi.");
            case "volume": return S.t("trūksta archyvo dalies. Pasirink visas dalis (.partN.rar arba .rar + .r00…).");
            case "dictionary": return S.t("archyvo žodynas per didelis šiam įrenginiui (> 256 MB). Perpakuok mažesniu žodynu arba ZIP.");
            case "corrupt": return S.t("archyvas pažeistas arba neatsisiųstas iki galo.");
            case "cancel": return S.t("Atšaukta.");
            case "open": return S.t("nepavyko atidaryti archyvo.");
            case "write": return S.t("nepavyko įrašyti (ar pakanka vietos?).");
            case "memory": return S.t("per mažai atminties archyvui išskleisti.");
            case "unsupported": return S.t("nepalaikomas glaudinimo būdas arba archyvas apsaugotas slaptažodžiu.");
            default: return token;
        }
    }
}
