package org.openbw.android;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * G11: Ganymede .oecampaign bundles (campaign.json + missions/<id>.scx|.scm + manifest.json).
 * The editor's Campaign.toJson is the format; the checks mirror CampaignStudio.verifyBundle.
 * Installed under <root>/campaigns/<dir>/<version>/, listed in <root>/campaigns/campaigns.json.
 * A mission is addressed as "oec:<campaign id>/<mission id>" (progress survives new versions).
 */
final class OeCampaigns {
    static final long MAX_ENTRY = 64L << 20, MAX_TOTAL = 512L << 20;
    static final String PREFIX = "oec:";

    static final class Episode { String id, title; int race; }
    static final class Mission { String id, episode, title, code, map, description; int race, player; }
    static final class Edge { String from, event, to; int priority; }
    static final class Campaign {
        String id = "", title = "", author = "", description = "", entry = "";
        int version;
        File dir;
        final List<Episode> episodes = new ArrayList<>();
        final List<Mission> missions = new ArrayList<>();
        final List<Edge> edges = new ArrayList<>();

        Mission mission(String id) {
            for (Mission m : missions) if (m.id.equals(id)) return m;
            return null;
        }
        File mapFile(Mission m) { return new File(dir, m.map); }
        String key(String missionId) { return PREFIX + id + "/" + missionId; }

        /** The edge taken on `event` ("victory" / "defeat"): lowest priority first, as the editor shows it.
         *  null = no such edge (a defeat then offers Retry, a victory ends the campaign). */
        Edge next(String from, String event) {
            Edge best = null;
            for (Edge e : edges)
                if (e.from.equals(from) && e.event.equals(event) && (best == null || e.priority < best.priority)) best = e;
            return best;
        }

        /** Missions the player may start: the entry, every one played, and every target of a played one's edge. */
        Set<String> unlocked(Set<String> done) {
            Set<String> r = new HashSet<>();
            if (!entry.isEmpty()) r.add(entry);
            for (Mission m : missions) if (done.contains(key(m.id))) r.add(m.id);
            for (Edge e : edges) if (!e.to.isEmpty() && done.contains(key(e.from))) r.add(e.to);
            return r;
        }
    }

    /** "oec:<campaign>/<mission>" -> {campaign, mission}; null for any other key. */
    static String[] split(String key) {
        if (key == null || !key.startsWith(PREFIX)) return null;
        int slash = key.lastIndexOf('/');
        if (slash <= PREFIX.length() || slash == key.length() - 1) return null;
        return new String[]{key.substring(PREFIX.length(), slash), key.substring(slash + 1)};
    }

    static Campaign parse(String json) throws JSONException {
        JSONObject o = new JSONObject(json);
        Campaign c = new Campaign();
        if (o.optInt("schema", 1) != 1) throw new JSONException("campaign schema " + o.optInt("schema") + " is newer than this game");
        c.id = o.optString("id", "");
        c.version = o.optInt("version", 1);
        c.title = o.optString("title", "");
        c.author = o.optString("author", "");
        c.description = o.optString("description", "");
        c.entry = o.optString("entry", "");
        JSONArray a = o.optJSONArray("episodes");
        for (int i = 0; a != null && i < a.length(); ++i) {
            JSONObject x = a.getJSONObject(i);
            Episode e = new Episode();
            e.id = x.optString("id", "");
            e.title = x.optString("title", "");
            e.race = x.optInt("race", 1);
            c.episodes.add(e);
        }
        a = o.optJSONArray("missions");
        for (int i = 0; a != null && i < a.length(); ++i) {
            JSONObject x = a.getJSONObject(i);
            Mission m = new Mission();
            m.id = x.optString("id", "");
            m.episode = x.optString("episode", "");
            m.title = x.optString("title", "");
            m.code = x.optString("code", "");
            m.map = x.optString("map", "");
            m.description = x.optString("description", "");
            m.race = x.optInt("race", 1);
            m.player = x.optInt("player", 0);
            c.missions.add(m);
        }
        a = o.optJSONArray("edges");
        for (int i = 0; a != null && i < a.length(); ++i) {
            JSONObject x = a.getJSONObject(i);
            Edge e = new Edge();
            e.from = x.optString("from", "");
            e.event = x.optString("event", "victory");
            e.to = x.optString("to", "");
            e.priority = x.optInt("priority", 0);
            c.edges.add(e);
        }
        return c;
    }

    private static boolean unsafe(String n) {
        return n.isEmpty() || n.startsWith("/") || n.contains("\\") || n.contains("..") || n.contains(":");
    }

    /** CampaignStudio.verifyBundle: errors (empty = valid); `files` receives every entry on success. */
    static List<String> verify(File zip, Map<String, byte[]> files) {
        List<String> err = new ArrayList<>();
        try (ZipFile z = new ZipFile(zip)) {
            long total = 0;
            Enumeration<? extends ZipEntry> en = z.entries();
            while (en.hasMoreElements()) {
                ZipEntry e = en.nextElement();
                String n = e.getName();
                if (unsafe(n)) { err.add("Unsafe name: " + n); continue; }
                if (e.isDirectory()) continue;
                if (files.containsKey(n)) { err.add("Two entries named " + n); continue; }
                if (e.getSize() > MAX_ENTRY) { err.add(n + " is too big"); continue; }
                if (e.getCompressedSize() > 0 && e.getSize() / Math.max(1, e.getCompressedSize()) > 200) { err.add(n + " packs too tightly to be a map"); continue; }
                try (InputStream in = z.getInputStream(e)) {
                    byte[] b = readLimited(in, MAX_ENTRY);
                    total += b.length;
                    if (total > MAX_TOTAL) { err.add("The bundle is too big"); break; }
                    files.put(n, b);
                }
            }
        } catch (IOException e) {
            err.add("Not a readable bundle: " + e.getMessage());
            return err;
        }
        byte[] mb = files.get("manifest.json");
        if (mb == null) { err.add("No manifest.json"); return err; }
        Set<String> listed = new HashSet<>();
        try {
            JSONObject man = new JSONObject(new String(mb, StandardCharsets.UTF_8));
            if (!"oecampaign".equals(man.optString("format"))) err.add("manifest.json is not an oecampaign manifest");
            JSONArray l = man.optJSONArray("files");
            if (l == null) { err.add("manifest.json lists no files"); return err; }
            for (int i = 0; i < l.length(); ++i) {
                JSONObject x = l.getJSONObject(i);
                String p = x.optString("path", "");
                listed.add(p);
                byte[] b = files.get(p);
                if (b == null) { err.add("Missing " + p); continue; }
                if (b.length != x.optLong("size", -1)) err.add(p + ": the size does not match the manifest");
                else if (!sha256(b).equalsIgnoreCase(x.optString("sha256", ""))) err.add(p + ": the SHA-256 does not match the manifest");
            }
        } catch (JSONException e) {
            err.add("manifest.json: " + e.getMessage());
            return err;
        }
        for (String n : files.keySet()) if (!n.equals("manifest.json") && !listed.contains(n)) err.add(n + " is not in the manifest");
        byte[] cb = files.get("campaign.json");
        if (cb == null) { err.add("No campaign.json"); return err; }
        try {
            Campaign k = parse(new String(cb, StandardCharsets.UTF_8));
            if (k.id.isEmpty()) err.add("campaign.json has no id");
            Set<String> ids = new HashSet<>();
            for (Mission m : k.missions) {
                if (m.id.isEmpty() || m.id.contains("/") || !ids.add(m.id)) err.add("Bad or repeated mission id: " + m.id);
                if (!files.containsKey(m.map)) err.add(m.title + ": its map " + m.map + " is not in the bundle");
            }
            for (Edge e : k.edges) if (!e.to.isEmpty() && k.mission(e.to) == null) err.add("An edge leads to a missing mission (" + e.to + ")");
            if (k.mission(k.entry) == null) err.add("The first mission is not in the bundle");
        } catch (JSONException e) {
            err.add("campaign.json: " + e.getMessage());
        }
        return err;
    }

    static File base(File root) { return new File(root, "campaigns"); }
    private static File registry(File root) { return new File(base(root), "campaigns.json"); }
    /** Folder name for a campaign id (ids are free text from the editor). */
    static String folder(String id) { return sha256(id.getBytes(StandardCharsets.UTF_8)).substring(0, 16); }

    /** Verifies and installs a bundle; returns the installed campaign. Throws with the reasons joined. */
    static Campaign install(File root, File zip) throws IOException {
        Map<String, byte[]> files = new HashMap<>();
        List<String> err = verify(zip, files);
        if (!err.isEmpty()) throw new IOException(String.join("\n", err));
        Campaign c;
        try {
            c = parse(new String(files.get("campaign.json"), StandardCharsets.UTF_8));
        } catch (JSONException e) {
            throw new IOException("campaign.json: " + e.getMessage());
        }
        File cdir = new File(base(root), folder(c.id));
        File vdir = new File(cdir, String.valueOf(c.version));
        File tmp = new File(cdir, c.version + ".tmp");
        deleteTree(tmp);
        for (Map.Entry<String, byte[]> e : files.entrySet()) {
            File out = new File(tmp, e.getKey());
            File p = out.getParentFile();
            if (p != null && !p.isDirectory() && !p.mkdirs()) throw new IOException("cannot make " + p);
            try (OutputStream o = new FileOutputStream(out)) { o.write(e.getValue()); }
        }
        deleteTree(vdir);
        if (!tmp.renameTo(vdir)) throw new IOException("cannot install " + vdir);
        // one version per campaign: the new one replaces whatever was there
        File[] old = cdir.listFiles();
        if (old != null) for (File f : old) if (!f.equals(vdir)) deleteTree(f);
        JSONObject reg = readRegistry(root);
        try {
            JSONArray list = reg.optJSONArray("campaigns");
            JSONArray out = new JSONArray();
            for (int i = 0; list != null && i < list.length(); ++i)
                if (!c.id.equals(list.getJSONObject(i).optString("id"))) out.put(list.getJSONObject(i));
            out.put(new JSONObject().put("id", c.id).put("version", c.version).put("dir", folder(c.id) + "/" + c.version));
            reg.put("campaigns", out);
        } catch (JSONException e) {
            throw new IOException(e.getMessage());
        }
        writeRegistry(root, reg);
        c.dir = vdir;
        return c;
    }

    static List<Campaign> installed(File root) {
        List<Campaign> r = new ArrayList<>();
        JSONArray list = readRegistry(root).optJSONArray("campaigns");
        for (int i = 0; list != null && i < list.length(); ++i) {
            JSONObject x = list.optJSONObject(i);
            if (x == null) continue;
            String rel = x.optString("dir", "");
            if (unsafe(rel)) continue;
            File dir = new File(base(root), rel);
            try {
                Campaign c = parse(new String(readFile(new File(dir, "campaign.json")), StandardCharsets.UTF_8));
                c.dir = dir;
                r.add(c);
            } catch (IOException | JSONException ignored) {
            }
        }
        return r;
    }

    static Campaign find(File root, String id) {
        for (Campaign c : installed(root)) if (c.id.equals(id)) return c;
        return null;
    }

    static void remove(File root, String id) throws IOException {
        JSONObject reg = readRegistry(root);
        try {
            JSONArray list = reg.optJSONArray("campaigns");
            JSONArray out = new JSONArray();
            for (int i = 0; list != null && i < list.length(); ++i)
                if (!id.equals(list.getJSONObject(i).optString("id"))) out.put(list.getJSONObject(i));
            reg.put("campaigns", out);
        } catch (JSONException e) {
            throw new IOException(e.getMessage());
        }
        writeRegistry(root, reg);
        deleteTree(new File(base(root), folder(id)));
    }

    private static JSONObject readRegistry(File root) {
        try {
            return new JSONObject(new String(readFile(registry(root)), StandardCharsets.UTF_8));
        } catch (IOException | JSONException e) {
            return new JSONObject();
        }
    }
    private static void writeRegistry(File root, JSONObject reg) throws IOException {
        File f = registry(root);
        File p = f.getParentFile();
        if (p != null && !p.isDirectory() && !p.mkdirs()) throw new IOException("cannot make " + p);
        File tmp = new File(p, f.getName() + ".tmp");
        try (OutputStream o = new FileOutputStream(tmp)) { o.write(reg.toString().getBytes(StandardCharsets.UTF_8)); }
        if (f.exists() && !f.delete()) { tmp.delete(); throw new IOException("cannot replace " + f); }
        if (!tmp.renameTo(f)) throw new IOException("cannot write " + f);
    }

    static byte[] readFile(File f) throws IOException {
        if (f.length() > MAX_ENTRY) throw new IOException(f.getName() + " is too big");
        try (InputStream in = new FileInputStream(f)) { return readLimited(in, MAX_ENTRY); }
    }
    private static byte[] readLimited(InputStream in, long max) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] b = new byte[65536];
        int n;
        while ((n = in.read(b)) > 0) { o.write(b, 0, n); if (o.size() > max) throw new IOException("too big"); }
        return o.toByteArray();
    }
    static void deleteTree(File f) {
        File[] k = f.listFiles();
        if (k != null) for (File c : k) deleteTree(c);
        f.delete();
    }
    static String sha256(byte[] b) {
        try {
            byte[] d = MessageDigest.getInstance("SHA-256").digest(b);
            StringBuilder s = new StringBuilder();
            for (byte x : d) s.append(String.format(Locale.ROOT, "%02x", x & 0xff));
            return s.toString();
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    /** oec-result.txt (native, G11) -> {mission key, "victory"/"defeat"}; null when absent. Consumed. */
    static String[] consumeResult(File dataDir) {
        File f = new File(dataDir, "oec-result.txt");
        if (!f.isFile()) return null;
        String[] r = null;
        try {
            String[] l = new String(readFile(f), StandardCharsets.UTF_8).split("\n");
            if (l.length >= 2) r = new String[]{l[0].trim(), l[1].trim()};
        } catch (IOException ignored) {
        }
        f.delete();
        return r;
    }
}
