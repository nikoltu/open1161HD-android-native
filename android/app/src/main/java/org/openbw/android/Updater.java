package org.openbw.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

/**
 * In-app update from the public fork's GitHub releases.
 *
 * Flow: GET releases/latest (once per launcher start, or on "Check now") -> compare the
 * tag (v1.0.24, v1.0.22-hotfix2, ...) with our versionName -> offer -> download the
 * release's .apk into the app cache -> hand it to PackageInstaller; Android shows its
 * own confirmation and refuses an APK signed by another key.
 *
 * Only builds signed with the release key take part in the automatic check: a debug
 * build (developer tablets) can never be updated by a release APK (different key),
 * so it would only nag. "Check now" still works there and says why it cannot install.
 * Nothing is sent but an anonymous GET to api.github.com / github.com.
 */
final class Updater {
    static final String REPO = "nikoltu/open1161HD-android-native";
    static final String LATEST = "https://api.github.com/repos/" + REPO + "/releases/latest";
    /** SHA-256 of the release signing certificate (open1161hd-release.jks). */
    static final String RELEASE_CERT_SHA256 = "778261414f630693f509b0f9a9a36cc426b2bc2ac4e3462e7d3b67ba280ac88e";
    static final String ACTION_INSTALLED = "org.openbw.android.UPDATE_INSTALL_STATUS";

    static final class Release {
        String tag, version, notes, apkName, apkUrl;
        long apkSize;
    }

    private Updater() {}

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("openbw", Context.MODE_PRIVATE);
    }

    static boolean enabled(Context c) { return prefs(c).getBoolean("updCheck", true); }

    /** Once per app process: the first launcher onResume after a start checks quietly
     *  (release-signed builds only). Coming back from a game or a rotation does not. */
    private static boolean checkedThisStart;
    static void maybeAutoCheck(Activity a) {
        if (checkedThisStart || !enabled(a) || !isReleaseSigned(a)) return;
        checkedThisStart = true;
        check(a, false);
    }

    /** interactive = the user pressed "Check now": report every outcome, ignore "skip". */
    static void check(final Activity a, final boolean interactive) {
        if (interactive) Toast.makeText(a, S.t("Tikrinami atnaujinimai…"), Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            Release r = null;
            String err = null;
            try { r = fetchLatest(); } catch (Exception e) { err = e.getMessage(); }
            final Release rel = r;
            final String error = err;
            a.runOnUiThread(() -> {
                if (a.isFinishing()) return;
                String cur = installedVersion(a);
                if (rel == null) {
                    if (interactive) Toast.makeText(a, S.t("Nepavyko patikrinti atnaujinimų: ") + error, Toast.LENGTH_LONG).show();
                    return;
                }
                boolean newer = compareVersions(rel.version, cur) > 0;
                boolean skipped = rel.tag.equals(prefs(a).getString("updSkip", ""));
                if (!newer) {
                    if (interactive) Toast.makeText(a, S.t("Naujausia versija jau įdiegta: ") + cur, Toast.LENGTH_LONG).show();
                    return;
                }
                if (skipped && !interactive) return;
                offer(a, rel, cur);
            });
        }, "update-check").start();
    }

    static Release fetchLatest() throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(LATEST).openConnection();
        c.setConnectTimeout(8000);
        c.setReadTimeout(10000);
        c.setRequestProperty("Accept", "application/vnd.github+json");
        c.setRequestProperty("User-Agent", "open1161HD-updater");
        int code = c.getResponseCode();
        if (code == 403 || code == 429) throw new java.io.IOException(code + " -- " + S.t("GitHub užklausų limitas viršytas, pabandykite vėliau"));
        if (code != 200) throw new java.io.IOException("HTTP " + code);
        String body = readAll(c.getInputStream());
        JSONObject o = new JSONObject(body);
        if (o.optBoolean("draft") || o.optBoolean("prerelease")) throw new java.io.IOException("no release");
        Release r = new Release();
        r.tag = o.optString("tag_name", "");
        r.version = r.tag.startsWith("v") ? r.tag.substring(1) : r.tag;
        r.notes = o.optString("body", "");
        JSONArray as = o.optJSONArray("assets");
        if (as != null) {
            for (int i = 0; i < as.length(); ++i) {
                JSONObject x = as.getJSONObject(i);
                String n = x.optString("name", "");
                if (!n.toLowerCase(java.util.Locale.ROOT).endsWith(".apk")) continue;
                r.apkName = n;
                r.apkUrl = x.optString("browser_download_url", "");
                r.apkSize = x.optLong("size", 0);
                break;
            }
        }
        if (r.tag.isEmpty() || r.apkUrl == null || !r.apkUrl.startsWith("https://github.com/" + REPO + "/"))
            throw new java.io.IOException(S.t("release'e nėra APK"));
        return r;
    }

    // ---- versions: 1.0.22 < 1.0.22-hotfix1 < 1.0.22-hotfix2 < 1.0.23 ----
    static int compareVersions(String a, String b) {
        long[] x = versionKey(a), y = versionKey(b);
        for (int i = 0; i < Math.max(x.length, y.length); ++i) {
            long p = i < x.length ? x[i] : 0, q = i < y.length ? y[i] : 0;
            if (p != q) return p < q ? -1 : 1;
        }
        return 0;
    }
    /** Numeric parts, padded to 4, then the hotfix number (0 when none). */
    static long[] versionKey(String v) {
        if (v == null) v = "";
        String main = v, fix = "";
        int dash = v.indexOf('-');
        if (dash >= 0) { main = v.substring(0, dash); fix = v.substring(dash + 1); }
        List<Long> out = new ArrayList<>();
        for (String p : main.split("\\.")) out.add(leadingNumber(p));
        while (out.size() < 4) out.add(0L);
        out.add(fix.isEmpty() ? 0L : Math.max(1L, leadingNumber(fix.replaceAll("[^0-9]", ""))));
        long[] r = new long[out.size()];
        for (int i = 0; i < r.length; ++i) r[i] = out.get(i);
        return r;
    }
    private static long leadingNumber(String s) {
        int i = 0;
        while (i < s.length() && Character.isDigit(s.charAt(i))) ++i;
        try { return i == 0 ? 0 : Long.parseLong(s.substring(0, i)); } catch (NumberFormatException e) { return 0; }
    }

    static String installedVersion(Context c) {
        try { return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName; }
        catch (Exception e) { return "0"; }
    }

    @SuppressWarnings("deprecation")
    static boolean isReleaseSigned(Context c) {
        try {
            PackageManager pm = c.getPackageManager();
            Signature[] sigs;
            if (Build.VERSION.SDK_INT >= 28) {
                PackageInfo pi = pm.getPackageInfo(c.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
                sigs = pi.signingInfo == null ? null : pi.signingInfo.getApkContentsSigners();
            } else {
                sigs = pm.getPackageInfo(c.getPackageName(), PackageManager.GET_SIGNATURES).signatures;
            }
            if (sigs == null) return false;
            for (Signature s : sigs) {
                byte[] d = MessageDigest.getInstance("SHA-256").digest(s.toByteArray());
                StringBuilder h = new StringBuilder();
                for (byte b : d) h.append(String.format("%02x", b & 0xff));
                if (RELEASE_CERT_SHA256.equals(h.toString())) return true;
            }
        } catch (Exception ignored) {
        }
        return false;
    }

    // ---- UI ----
    static void offer(final Activity a, final Release r, String cur) {
        // Release notes are Markdown; show them as plain text.
        String notes = r.notes == null ? "" : r.notes.trim()
                .replace("**", "").replace("__", "").replace("`", "")
                .replaceAll("(?m)^#{1,6}\\s*", "").replaceAll("(?m)^\\s*[-*]\\s+", "• ")
                .replaceAll("\\[([^\\]]+)\\]\\([^)]+\\)", "$1").replace("\r", "");
        if (notes.length() > 1500) notes = notes.substring(0, 1500) + "…";
        String size = r.apkSize > 0 ? String.format(java.util.Locale.ROOT, "  (%.1f MB)", r.apkSize / 1048576.0) : "";
        String msg = S.t("Įdiegta: ") + cur + "\n" + S.t("Nauja: ") + r.version + size
                + (notes.isEmpty() ? "" : "\n\n" + notes)
                + (isReleaseSigned(a) ? "" : "\n\n" + S.t("Ši kopija pasirašyta kitu raktu (kūrimo versija) — Android jos neatnaujins; reikės ištrinti ir įdiegti iš naujo."));
        new AlertDialog.Builder(a)
                .setTitle(S.t("Yra atnaujinimas"))
                .setMessage(msg)
                .setPositiveButton(S.t("Atnaujinti"), (d, w) -> startDownload(a, r))
                .setNegativeButton(S.t("Vėliau"), null)
                .setNeutralButton(S.t("Praleisti šią versiją"), (d, w) -> prefs(a).edit().putString("updSkip", r.tag).apply())
                .show();
    }

    static void startDownload(final Activity a, final Release r) {
        if (Build.VERSION.SDK_INT >= 26 && !a.getPackageManager().canRequestPackageInstalls()) {
            new AlertDialog.Builder(a)
                    .setTitle(S.t("Reikia leidimo"))
                    .setMessage(S.t("Kad galėtų įdiegti atnaujinimą, programai reikia leidimo diegti programas. Įjunk jį kitame lange ir grįžk — atsisiuntimas prasidės pats."))
                    .setPositiveButton(S.t("Atidaryti nustatymus"), (d, w) -> {
                        prefs(a).edit().putString("updPendingTag", r.tag).apply();
                        a.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                Uri.parse("package:" + a.getPackageName())));
                    })
                    .setNegativeButton(S.t("Atšaukti"), null)
                    .show();
            return;
        }
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (20 * a.getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad / 2);
        final ProgressBar bar = new ProgressBar(a, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        final TextView label = new TextView(a);
        label.setText(r.apkName);
        box.addView(label);
        box.addView(bar);
        final boolean[] cancelled = {false};
        final AlertDialog dlg = new AlertDialog.Builder(a)
                .setTitle(S.t("Atsisiunčiamas atnaujinimas"))
                .setView(box)
                .setCancelable(false)
                .setNegativeButton(S.t("Atšaukti"), (d, w) -> cancelled[0] = true)
                .show();
        final Handler ui = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            File dir = new File(a.getCacheDir(), "update");
            dir.mkdirs();
            File[] old = dir.listFiles();
            if (old != null) for (File f : old) f.delete();
            File out = new File(dir, "update.apk");
            String err = null;
            try {
                HttpURLConnection c = (HttpURLConnection) new URL(r.apkUrl).openConnection();
                c.setInstanceFollowRedirects(true);   // github.com -> objects.githubusercontent.com (both https)
                c.setConnectTimeout(10000);
                c.setReadTimeout(20000);
                c.setRequestProperty("User-Agent", "open1161HD-updater");
                if (c.getResponseCode() != 200) throw new java.io.IOException("HTTP " + c.getResponseCode());
                long total = c.getContentLengthLong() > 0 ? c.getContentLengthLong() : r.apkSize;
                try (InputStream in = c.getInputStream(); OutputStream os = new FileOutputStream(out)) {
                    byte[] buf = new byte[1 << 16];
                    long got = 0;
                    int n, lastP = -1;
                    while ((n = in.read(buf)) > 0) {
                        if (cancelled[0]) throw new java.io.IOException(S.t("atšaukta"));
                        os.write(buf, 0, n);
                        got += n;
                        if (total > 0) {
                            final int p = (int) (got * 1000 / total);
                            if (p != lastP) { lastP = p; ui.post(() -> bar.setProgress(p)); }
                        }
                    }
                }
                if (r.apkSize > 0 && out.length() != r.apkSize) throw new java.io.IOException(S.t("failas nepilnas"));
            } catch (Exception e) {
                err = e.getMessage();
                out.delete();
            }
            final String error = err;
            ui.post(() -> {
                try { dlg.dismiss(); } catch (Exception ignored) {}
                if (cancelled[0]) return;
                if (error != null) {
                    Toast.makeText(a, S.t("Atsisiųsti nepavyko: ") + error, Toast.LENGTH_LONG).show();
                    return;
                }
                install(a, out);
            });
        }, "update-download").start();
    }

    /** Hands the APK to PackageInstaller; the system shows its own confirmation. */
    static void install(final Activity a, File apk) {
        try {
            PackageInstaller pi = a.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams sp = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            sp.setAppPackageName(a.getPackageName());
            sp.setSize(apk.length());
            int id = pi.createSession(sp);
            try (PackageInstaller.Session s = pi.openSession(id)) {
                try (InputStream in = new FileInputStream(apk); OutputStream os = s.openWrite("update.apk", 0, apk.length())) {
                    byte[] buf = new byte[1 << 16];
                    int n;
                    while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                    s.fsync(os);
                }
                registerStatus(a);
                Intent cb = new Intent(ACTION_INSTALLED).setPackage(a.getPackageName());
                int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
                PendingIntent pend = PendingIntent.getBroadcast(a, id, cb, flags);
                s.commit(pend.getIntentSender());
            }
        } catch (Exception e) {
            Toast.makeText(a, S.t("Įdiegti nepavyko: ") + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private static BroadcastReceiver statusReceiver;

    private static void registerStatus(final Activity a) {
        if (statusReceiver != null) return;
        final Context app = a.getApplicationContext();
        statusReceiver = new BroadcastReceiver() {
            @Override public void onReceive(Context ctx, Intent i) {
                int st = i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                if (st == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    Intent confirm = i.getParcelableExtra(Intent.EXTRA_INTENT);
                    if (confirm != null) {
                        confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                        app.startActivity(confirm);
                    }
                    return;
                }
                if (st == PackageInstaller.STATUS_SUCCESS) return;   // the app is replaced and restarted by the system
                String why;
                if (st == PackageInstaller.STATUS_FAILURE_CONFLICT || st == PackageInstaller.STATUS_FAILURE_INCOMPATIBLE)
                    why = S.t("Įdiegta kopija pasirašyta kitu raktu. Ištrink programą ir įdiek atsisiųstą APK iš naujo (MPQ/CASC reikės importuoti dar kartą).");
                else if (st == PackageInstaller.STATUS_FAILURE_ABORTED)
                    why = S.t("Diegimas atšauktas.");
                else
                    why = S.t("Įdiegti nepavyko: ") + i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                Toast.makeText(app, why, Toast.LENGTH_LONG).show();
            }
        };
        IntentFilter f = new IntentFilter(ACTION_INSTALLED);
        if (Build.VERSION.SDK_INT >= 33) app.registerReceiver(statusReceiver, f, Context.RECEIVER_NOT_EXPORTED);
        else app.registerReceiver(statusReceiver, f);
    }

    /** After the user granted "install unknown apps": continue the pending update. */
    static void resumePending(final Activity a) {
        final String tag = prefs(a).getString("updPendingTag", "");
        if (tag.isEmpty() || resumeInFlight) return;
        if (Build.VERSION.SDK_INT >= 26 && !a.getPackageManager().canRequestPackageInstalls()) return;
        resumeInFlight = true;
        new Thread(() -> {
            Release r = null;
            try { r = fetchLatest(); } catch (Exception ignored) {}
            final Release rel = r;
            a.runOnUiThread(() -> {
                resumeInFlight = false;
                // Coming back from the (portrait) settings screen recreates the launcher;
                // the tag is cleared only once a live activity really starts the download,
                // so the next instance's onResume picks it up otherwise.
                if (a.isFinishing() || a.isDestroyed()) return;
                prefs(a).edit().remove("updPendingTag").apply();
                if (rel != null && rel.tag.equals(tag)) startDownload(a, rel);
            });
        }, "update-resume").start();
    }
    private static volatile boolean resumeInFlight;

    private static String readAll(InputStream in) throws java.io.IOException {
        java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        in.close();
        return b.toString("UTF-8");
    }
}
