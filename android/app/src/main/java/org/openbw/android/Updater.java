package org.openbw.android;

import android.app.Activity;
import android.app.Dialog;
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
import android.graphics.Typeface;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.TextUtils;
import android.text.style.ForegroundColorSpan;
import android.text.style.LeadingMarginSpan;
import android.text.style.RelativeSizeSpan;
import android.text.style.StyleSpan;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

/**
 * In-app update from the public fork's GitHub releases.
 *
 * Flow: GET releases/latest (once per launcher start, or on "Check now") -> compare the
 * tag with our versionName -> offer -> download the APK into cache/update/<name> through
 * a resumable <name>.part -> check that it is signed like the installed app -> ask for the
 * "install unknown apps" permission if needed -> PackageInstaller, which shows Android's
 * own confirmation.
 *
 * Only release-signed builds check automatically: a debug build can never take a release
 * APK (different key). "Check now" works everywhere; a build signed with another key is
 * told so before any install and gets the release page instead.
 * Nothing is sent but an anonymous GET to api.github.com / github.com.
 */
final class Updater {
    static final String REPO = "nikoltu/open1161HD-android-native";
    static final String LATEST = "https://api.github.com/repos/" + REPO + "/releases/latest";
    static final String RELEASES_PAGE = "https://github.com/" + REPO + "/releases";
    /** SHA-256 of the release signing certificate (open1161hd-release.jks). */
    static final String RELEASE_CERT_SHA256 = "778261414f630693f509b0f9a9a36cc426b2bc2ac4e3462e7d3b67ba280ac88e";
    static final String ACTION_INSTALLED = "org.openbw.android.UPDATE_INSTALL_STATUS";
    private static final String USER_AGENT = "open1161HD-updater";

    static final class Release {
        String tag, version, notes, pageUrl, apkName, apkUrl;
        long apkSize;
        boolean hasApk() { return apkUrl != null && apkSize > 0; }
    }

    private Updater() {}

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("openbw", Context.MODE_PRIVATE);
    }

    static boolean enabled(Context c) { return prefs(c).getBoolean("updCheck", true); }

    /** Once per app process: the first launcher onResume after a start checks quietly
     *  (release-signed builds only). Coming back from a game does not. */
    private static boolean checkedThisStart;
    /** One download at a time; a second offer while it runs only re-shows nothing. */
    private static volatile boolean downloading;

    static void maybeAutoCheck(Activity a) {
        if (checkedThisStart || !enabled(a) || !isReleaseSigned(a)) return;
        checkedThisStart = true;
        check(a, false);
    }

    /** interactive = the user pressed "Check now": report every outcome, ignore "skip". */
    static void check(final Activity a, final boolean interactive) {
        if (downloading) {
            if (interactive) Kit.toast(a, S.t("Atnaujinimas jau atsisiunčiamas."), Toast.LENGTH_SHORT);
            return;
        }
        if (interactive) Kit.toast(a, S.t("Tikrinami atnaujinimai…"), Toast.LENGTH_SHORT);
        new Thread(() -> {
            Release r = null;
            String err = null;
            try { r = fetchLatest(testBase(a)); } catch (Exception e) { err = e.getMessage(); }
            final Release rel = r;
            final String error = err;
            a.runOnUiThread(() -> {
                if (a.isFinishing()) return;
                String cur = installedVersion(a);
                if (rel == null) {
                    if (interactive) Kit.toast(a, S.t("Nepavyko patikrinti atnaujinimų: ") + error, Toast.LENGTH_LONG);
                    return;
                }
                if (compareVersions(rel.version, cur) <= 0) {
                    cleanUp(a, null);
                    if (interactive) Kit.toast(a, S.t("Naujausia versija jau įdiegta: ") + cur, Toast.LENGTH_LONG);
                    return;
                }
                boolean skipped = compareVersions(rel.version, stripV(prefs(a).getString("updSkip", ""))) == 0;
                if (skipped && !interactive) return;
                cleanUp(a, rel.apkName);
                offer(a, rel, cur);
            });
        }, "update-check").start();
    }

    /** Debuggable builds only: pref "updTestBase" (e.g. http://127.0.0.1:8765 over adb reverse)
     *  serves latest.json and the APK in GitHub's place, to exercise the panels offline. */
    static String testBase(Context c) {
        boolean dbg = (c.getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        return dbg ? prefs(c).getString("updTestBase", "") : "";
    }

    static Release fetchLatest(String testBase) throws Exception {
        final String allowed = testBase.isEmpty() ? "https://github.com/" + REPO + "/" : testBase + "/";
        HttpURLConnection c = open(testBase.isEmpty() ? LATEST : testBase + "/latest.json", 8000, 10000);
        try {
            c.setRequestProperty("Accept", "application/vnd.github+json");
            int code = c.getResponseCode();
            if (code == 403 || code == 429) throw new java.io.IOException(code + " -- " + S.t("GitHub užklausų limitas viršytas, pabandykite vėliau"));
            if (code != 200) throw new java.io.IOException("HTTP " + code);
            JSONObject o = new JSONObject(readAll(c.getInputStream()));
            if (o.optBoolean("draft") || o.optBoolean("prerelease")) throw new java.io.IOException("no release");
            Release r = new Release();
            r.tag = o.optString("tag_name", "");
            r.version = stripV(r.tag);
            r.notes = o.optString("body", "");
            r.pageUrl = o.optString("html_url", "");
            if (!r.pageUrl.startsWith(allowed)) r.pageUrl = RELEASES_PAGE;
            JSONArray as = o.optJSONArray("assets");
            if (as != null) {
                for (int i = 0; i < as.length(); ++i) {
                    JSONObject x = as.getJSONObject(i);
                    String n = x.optString("name", "");
                    String u = x.optString("browser_download_url", "");
                    if (!n.toLowerCase(Locale.ROOT).endsWith(".apk") || !u.startsWith(allowed)) continue;
                    // The name becomes a cache file name: keep it to a plain file name.
                    r.apkName = n.replaceAll("[^A-Za-z0-9._-]", "_");
                    r.apkUrl = u;
                    r.apkSize = x.optLong("size", 0);
                    break;
                }
            }
            if (r.tag.isEmpty() || versionKey(r.version)[0] + versionKey(r.version)[1] + versionKey(r.version)[2] == 0)
                throw new java.io.IOException("no release");
            return r;
        } finally {
            c.disconnect();
        }
    }

    private static HttpURLConnection open(String url, int connectMs, int readMs) throws java.io.IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setInstanceFollowRedirects(true);   // github.com -> objects.githubusercontent.com (both https)
        c.setConnectTimeout(connectMs);
        c.setReadTimeout(readMs);
        c.setRequestProperty("User-Agent", USER_AGENT);
        return c;
    }

    // ---- versions: 1.0.22 < 1.0.22-hotfix1 < 1.0.22-hotfix2 < 1.0.23; other suffixes (-debug, -rc1) count for nothing ----
    static int compareVersions(String a, String b) {
        long[] x = versionKey(a), y = versionKey(b);
        for (int i = 0; i < x.length; ++i)
            if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
        return 0;
    }
    /** Major, minor, patch, build (padded to 4), then the hotfix number (0 when none). */
    static long[] versionKey(String v) {
        long[] r = new long[5];
        v = stripV(v == null ? "" : v.trim());
        int at = 0, n = 0;
        while (n < 4 && at < v.length() && Character.isDigit(v.charAt(at))) {
            long num = 0;
            while (at < v.length() && Character.isDigit(v.charAt(at))) {
                num = Math.min(Integer.MAX_VALUE, num * 10 + (v.charAt(at) - '0'));
                ++at;
            }
            r[n++] = num;
            if (at + 1 < v.length() && v.charAt(at) == '.' && Character.isDigit(v.charAt(at + 1))) ++at;
            else break;
        }
        String low = v.toLowerCase(Locale.ROOT);
        int h = low.indexOf("-hotfix");
        if (h >= 0) {
            int i = h + 7;
            if (i >= low.length() || !Character.isLetter(low.charAt(i))) {
                long num = 0;
                boolean digits = false;
                while (i < low.length() && Character.isDigit(low.charAt(i))) {
                    num = Math.min(Integer.MAX_VALUE, num * 10 + (low.charAt(i) - '0'));
                    digits = true;
                    ++i;
                }
                r[4] = digits ? Math.max(1, num) : 1;
            }
        }
        return r;
    }
    private static String stripV(String t) {
        return t != null && (t.startsWith("v") || t.startsWith("V")) ? t.substring(1) : (t == null ? "" : t);
    }

    static String installedVersion(Context c) {
        try { return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName; }
        catch (Exception e) { return "0"; }
    }

    // ---- signatures ----
    @SuppressWarnings("deprecation")
    private static Set<String> signerDigests(PackageInfo pi) {
        Set<String> out = new HashSet<>();
        if (pi == null) return out;
        Signature[] sigs;
        if (Build.VERSION.SDK_INT >= 28) sigs = pi.signingInfo == null ? null : pi.signingInfo.getApkContentsSigners();
        else sigs = pi.signatures;
        if (sigs == null) return out;
        for (Signature s : sigs) {
            try {
                byte[] d = MessageDigest.getInstance("SHA-256").digest(s.toByteArray());
                StringBuilder h = new StringBuilder();
                for (byte b : d) h.append(String.format("%02x", b & 0xff));
                out.add(h.toString());
            } catch (Exception ignored) {
            }
        }
        return out;
    }

    @SuppressWarnings("deprecation")
    private static int signingFlags() {
        return Build.VERSION.SDK_INT >= 28 ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES;
    }

    static String[] ownSignerDigests(Context c) {
        try {
            return signerDigests(c.getPackageManager().getPackageInfo(c.getPackageName(), signingFlags())).toArray(new String[0]);
        } catch (Exception e) {
            return new String[0];
        }
    }

    static boolean isReleaseSigned(Context c) {
        try {
            return signerDigests(c.getPackageManager().getPackageInfo(c.getPackageName(), signingFlags()))
                    .contains(RELEASE_CERT_SHA256);
        } catch (Exception e) {
            return false;
        }
    }

    /** 1 = the APK is this app signed by the same key, 0 = another key or another app,
     *  -1 = its signers cannot be read here (Android's installer decides). */
    static int sameSigner(Context c, File apk) {
        try {
            PackageManager pm = c.getPackageManager();
            PackageInfo got = pm.getPackageArchiveInfo(apk.getAbsolutePath(), signingFlags());
            if (got == null) return 0;
            if (!c.getPackageName().equals(got.packageName)) return 0;
            Set<String> theirs = signerDigests(got);
            if (theirs.isEmpty()) return -1;
            Set<String> mine = signerDigests(pm.getPackageInfo(c.getPackageName(), signingFlags()));
            if (mine.isEmpty()) return -1;
            return mine.equals(theirs) ? 1 : 0;
        } catch (Exception e) {
            return -1;
        }
    }

    // ---- files ----
    private static File folder(Context c) { return new File(c.getCacheDir(), "update"); }

    /** Deletes downloads of other releases (all of them when keep == null). */
    private static void cleanUp(Context c, String keep) {
        File[] files = folder(c).listFiles();
        if (files == null) return;
        for (File f : files)
            if (keep == null || !f.getName().startsWith(keep)) f.delete();
    }

    // ---- UI ----
    /** The offer, in the launcher's design: installed -> new version strip, size and
     *  "already downloaded" chip, the release notes as formatted text, then warnings. */
    static void offer(final Activity a, final Release r, String cur) {
        final Kit kit = new Kit(a);
        final boolean c = kit.compact;
        File done = new File(folder(a), r.apkName == null ? "-" : r.apkName);
        boolean ready = r.hasApk() && done.length() == r.apkSize;

        LinearLayout body = new LinearLayout(a);
        body.setOrientation(LinearLayout.VERTICAL);

        LinearLayout strip = new LinearLayout(a);
        strip.setOrientation(LinearLayout.HORIZONTAL);
        strip.setGravity(Gravity.CENTER_VERTICAL);
        strip.setBackground(kit.box(0xFF0B1114, Kit.LINE));
        strip.setPadding(kit.dp(c ? 12 : 16), kit.dp(c ? 8 : 12), kit.dp(c ? 12 : 16), kit.dp(c ? 8 : 12));
        strip.addView(versionCell(kit, S.t("Įdiegta"), cur, Kit.TEXT2));
        TextView arrow = kit.text("›", Kit.LABEL, c ? 26 : 32);
        arrow.setPadding(kit.dp(c ? 12 : 18), 0, kit.dp(c ? 12 : 18), 0);
        strip.addView(arrow);
        strip.addView(versionCell(kit, S.t("Nauja"), r.version, Kit.ACCENT));
        strip.addView(new View(a), new LinearLayout.LayoutParams(0, 1, 1f));
        LinearLayout side = new LinearLayout(a);
        side.setOrientation(LinearLayout.VERTICAL);
        side.setGravity(Gravity.END);
        if (r.hasApk()) {
            TextView sz = kit.text(mb(r.apkSize), Kit.TEXT, c ? 13 : 14);
            sz.setGravity(Gravity.END);
            side.addView(sz);
        }
        if (ready) {
            TextView chip = kit.text(S.t("jau atsisiųsta").toUpperCase(Locale.ROOT), Kit.LABEL, c ? 10 : 11);
            chip.setLetterSpacing(0.1f);
            chip.setBackground(kit.box(Kit.SEL_BG, Kit.LABEL));
            chip.setPadding(kit.dp(8), kit.dp(3), kit.dp(8), kit.dp(3));
            LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            clp.topMargin = kit.dp(4);
            side.addView(chip, clp);
        }
        strip.addView(side);
        body.addView(strip, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        CharSequence notes = formatNotes(r.notes);
        if (notes.length() > 0) {
            body.addView(kit.label(S.t("Kas naujo")));
            TextView nt = kit.text("", Kit.TEXT, c ? 13 : 14);
            nt.setText(notes);
            nt.setLineSpacing(0, 1.15f);
            ScrollView sv = new ScrollView(a);
            sv.setBackground(kit.box(0xFF070B0D, 0xFF1E272B));
            sv.setPadding(kit.dp(c ? 10 : 14), kit.dp(c ? 8 : 10), kit.dp(c ? 10 : 14), kit.dp(c ? 8 : 10));
            sv.setScrollbarFadingEnabled(false);
            sv.addView(nt);
            body.addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        }
        if (!r.hasApk())
            body.addView(warning(kit, S.t("Šiame leidime nėra APK — atsisiųsk jį iš leidimo puslapio."), Kit.ACCENT));
        if (!isReleaseSigned(a))
            body.addView(warning(kit, S.t("Ši kopija pasirašyta kitu raktu (kūrimo versija) — Android jos neatnaujins; reikės ištrinti ir įdiegti iš naujo."), Kit.DANGER));

        KitDialog d = new KitDialog(a).setTitle(S.t("Yra atnaujinimas")).setView(body).setWidth(640);
        if (r.hasApk()) d.setPositiveButton(S.t("Atnaujinti"), (x, w) -> startDownload(a, r));
        else d.setPositiveButton(S.t("Atidaryti leidimo puslapį"), (x, w) -> openPage(a, r));
        d.setNegativeButton(S.t("Vėliau"), null)
         .setNeutralButton(S.t("Praleisti šią versiją"), (x, w) -> prefs(a).edit().putString("updSkip", r.tag).apply())
         .show();
    }

    private static String mb(long bytes) { return Kit.ProgressPanel.mb(bytes); }

    private static View versionCell(Kit kit, String label, String version, int color) {
        LinearLayout cell = new LinearLayout(kit.a);
        cell.setOrientation(LinearLayout.VERTICAL);
        TextView l = kit.text(label.toUpperCase(Locale.ROOT), Kit.TEXT2, kit.compact ? 10 : 11);
        l.setLetterSpacing(0.12f);
        cell.addView(l);
        TextView v = kit.text(version, color, kit.compact ? 18 : 22);
        v.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        cell.addView(v);
        return cell;
    }

    private static View warning(Kit kit, String msg, int color) {
        TextView t = kit.text(msg, Kit.TEXT, kit.compact ? 12 : 13);
        t.setBackground(kit.box(color == Kit.DANGER ? 0xFF2A0E0C : 0xFF2A2208, color));
        t.setPadding(kit.dp(12), kit.dp(8), kit.dp(12), kit.dp(8));
        t.setLineSpacing(0, 1.1f);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = kit.dp(10);
        t.setLayoutParams(lp);
        return t;
    }

    /** Release notes are Markdown: headings in the launcher's green, bullets with a
     *  hanging indent, links and inline marks reduced to their text. */
    static CharSequence formatNotes(String md) {
        SpannableStringBuilder sb = new SpannableStringBuilder();
        if (md == null) return sb;
        boolean blank = false;
        for (String raw : md.replace("\r", "").split("\n")) {
            String line = raw.replaceAll("\\s+$", "");
            if (line.trim().isEmpty()) { blank = sb.length() > 0; continue; }
            line = line.replace("**", "").replace("__", "").replace("`", "")
                    .replaceAll("!?\\[([^\\]]*)\\]\\([^)]*\\)", "$1").replaceAll("<[^>]+>", "");
            if (sb.length() > 0) sb.append(blank ? "\n\n" : "\n");
            blank = false;
            int at = sb.length();
            java.util.regex.Matcher h = java.util.regex.Pattern.compile("^\\s*#{1,6}\\s*(.*)$").matcher(line);
            java.util.regex.Matcher b = java.util.regex.Pattern.compile("^(\\s*)[-*+]\\s+(.*)$").matcher(line);
            if (h.matches()) {
                if (at > 0 && sb.charAt(at - 1) == '\n' && (at < 2 || sb.charAt(at - 2) != '\n')) { sb.insert(at, "\n"); ++at; }
                sb.append(h.group(1).toUpperCase(Locale.ROOT));
                sb.setSpan(new ForegroundColorSpan(Kit.LABEL), at, sb.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                sb.setSpan(new StyleSpan(Typeface.BOLD), at, sb.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                sb.setSpan(new RelativeSizeSpan(0.9f), at, sb.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            } else if (b.matches()) {
                int depth = Math.min(3, b.group(1).replace("\t", "  ").length() / 2);
                sb.append(depth > 0 ? "◦  " : "•  ");
                sb.setSpan(new ForegroundColorSpan(Kit.LABEL), at, at + 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                sb.append(b.group(2));
                int ind = Math.round(16 * android.content.res.Resources.getSystem().getDisplayMetrics().density);
                sb.setSpan(new LeadingMarginSpan.Standard(depth * ind, depth * ind + ind), at, sb.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            } else {
                sb.append(line.trim());
            }
            if (sb.length() > 6000) { sb.append(" …"); break; }
        }
        return sb;
    }

    static void openPage(Activity a, Release r) {
        try {
            a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(r != null && r.pageUrl != null ? r.pageUrl : RELEASES_PAGE)));
        } catch (Exception e) {
            Kit.toast(a, S.t("Nepavyko atidaryti puslapio: ") + e.getMessage(), Toast.LENGTH_LONG);
        }
    }

    /** Downloads through <name>.part; a part left by an earlier try is continued when the server allows. */
    static void startDownload(final Activity a, final Release r) {
        if (downloading) return;
        final File target = new File(folder(a), r.apkName);
        if (target.length() == r.apkSize) { afterDownload(a, r, target); return; }
        downloading = true;
        final Kit.ProgressPanel pp = new Kit(a).progressPanel("v" + r.version + "  ·  " + r.apkName);
        final long already = new File(target.getPath() + ".part").length();
        if (already > 0 && already < r.apkSize) {
            pp.busy(S.t("Tęsiama nuo ") + mb(already) + "…");
            pp.meter.setFraction(already / (float) r.apkSize);
        } else pp.busy(S.t("Jungiamasi…"));
        final boolean[] cancelled = {false};
        final Dialog dlg = new KitDialog(a)
                .setTitle(S.t("Atsisiunčiamas atnaujinimas"))
                .setView(pp.view)
                .setWidth(520)
                .setCancelable(false)
                .setNegativeButton(S.t("Atšaukti"), (d, w) -> cancelled[0] = true)
                .show();
        final Handler ui = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            String err = null;
            File part = new File(target.getPath() + ".part");
            HttpURLConnection c = null;
            try {
                if (!folder(a).isDirectory() && !folder(a).mkdirs()) throw new java.io.IOException(S.t("nepavyko sukurti aplanko"));
                if (part.length() > r.apkSize) part.delete();
                long have = part.length();
                if (have < r.apkSize) {
                    c = open(r.apkUrl, 10000, 20000);
                    if (have > 0) c.setRequestProperty("Range", "bytes=" + have + "-");
                    int code = c.getResponseCode();
                    if (code != 200 && code != 206) throw new java.io.IOException("HTTP " + code);
                    if (code == 200) have = 0;   // the server ignored the range: start over
                    final long from = have;
                    ui.post(() -> {
                        pp.status.setText(from > 0 ? S.t("Tęsiama nuo ") + mb(from) : S.t("Atsisiunčiama"));
                        pp.update(from, r.apkSize);
                    });
                    try (InputStream in = c.getInputStream(); RandomAccessFile out = new RandomAccessFile(part, "rw")) {
                        out.setLength(have);
                        out.seek(have);
                        byte[] buf = new byte[1 << 16];
                        int n, lastP = -1;
                        while ((n = in.read(buf)) > 0) {
                            if (cancelled[0]) throw new java.io.IOException(S.t("atšaukta"));
                            out.write(buf, 0, n);
                            have += n;
                            final int p = (int) (have * 1000 / r.apkSize);
                            if (p != lastP) {
                                lastP = p;
                                final long got = have;
                                ui.post(() -> pp.update(got, r.apkSize));
                            }
                        }
                    }
                }
                ui.post(() -> pp.busy(S.t("Tikrinama…")));
                if (part.length() != r.apkSize) { part.delete(); throw new java.io.IOException(S.t("failas nepilnas")); }
                if (target.exists() && !target.delete()) throw new java.io.IOException(S.t("nepavyko pakeisti seno failo"));
                if (!part.renameTo(target)) throw new java.io.IOException(S.t("nepavyko išsaugoti failo"));
            } catch (Exception e) {
                err = e.getMessage();
            } finally {
                if (c != null) c.disconnect();
                downloading = false;
            }
            final String error = err;
            ui.post(() -> {
                try { dlg.dismiss(); } catch (Exception ignored) {}
                if (a.isFinishing()) return;
                if (cancelled[0]) return;   // the .part stays: the next try continues it
                if (error != null) {
                    new KitDialog(a)
                            .setTitle(S.t("Atsisiųsti nepavyko"))
                            .setMessage(String.valueOf(error))
                            .setPositiveButton(S.t("Bandyti dar kartą"), (d, w) -> startDownload(a, r))
                            .setNegativeButton(S.t("Vėliau"), null)
                            .show();
                    return;
                }
                afterDownload(a, r, target);
            });
        }, "update-download").start();
    }

    /** The downloaded APK must be this app signed like the installed one before Android sees it. */
    static void afterDownload(final Activity a, final Release r, final File apk) {
        int same = sameSigner(a, apk);
        if (same == 0) {
            apk.delete();
            new KitDialog(a)
                    .setTitle(S.t("Atnaujinti vietoje negalima"))
                    .setMessage(S.t("Įdiegta kopija pasirašyta kitu raktu nei leidimas (pvz., kūrimo versija). Android jos neatnaujins: ištrink programą ir įdiek APK iš leidimo puslapio (MPQ/CASC reikės importuoti dar kartą)."))
                    .setPositiveButton(S.t("Atidaryti leidimo puslapį"), (d, w) -> openPage(a, r))
                    .setNegativeButton(S.t("Uždaryti"), null)
                    .show();
            return;
        }
        if (Build.VERSION.SDK_INT >= 26 && !a.getPackageManager().canRequestPackageInstalls()) {
            new KitDialog(a)
                    .setTitle(S.t("Reikia leidimo"))
                    .setMessage(S.t("Atnaujinimas atsisiųstas. Kad galėtų jį įdiegti, programai reikia leidimo diegti programas. Įjunk jį kitame lange ir grįžk — diegimas prasidės pats."))
                    .setPositiveButton(S.t("Atidaryti nustatymus"), (d, w) -> {
                        prefs(a).edit().putString("updPendingApk", apk.getAbsolutePath()).apply();
                        try {
                            a.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                                    Uri.parse("package:" + a.getPackageName())));
                        } catch (Exception e) {
                            Kit.toast(a, S.t("Įdiegti nepavyko: ") + e.getMessage(), Toast.LENGTH_LONG);
                        }
                    })
                    .setNegativeButton(S.t("Vėliau"), null)
                    .show();
            return;
        }
        install(a, apk);
    }

    /** Hands the APK to PackageInstaller; the system shows its own confirmation. */
    static void install(final Activity a, final File apk) {
        final Context app = a.getApplicationContext();
        registerStatus(a);
        new Thread(() -> {
            String err = null;
            try {
                PackageInstaller pi = app.getPackageManager().getPackageInstaller();
                PackageInstaller.SessionParams sp = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                sp.setAppPackageName(app.getPackageName());
                sp.setSize(apk.length());
                int id = pi.createSession(sp);
                try (PackageInstaller.Session s = pi.openSession(id)) {
                    try (InputStream in = new FileInputStream(apk); OutputStream os = s.openWrite("update.apk", 0, apk.length())) {
                        byte[] buf = new byte[1 << 16];
                        int n;
                        while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                        s.fsync(os);
                    }
                    Intent cb = new Intent(ACTION_INSTALLED).setPackage(app.getPackageName());
                    int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
                    s.commit(PendingIntent.getBroadcast(app, id, cb, flags).getIntentSender());
                } catch (Exception e) {
                    pi.abandonSession(id);
                    throw e;
                }
            } catch (Exception e) {
                err = e.getMessage();
            }
            if (err != null) {
                final String error = err;
                a.runOnUiThread(() -> Kit.toast(app, S.t("Įdiegti nepavyko: ") + error, Toast.LENGTH_LONG));
            }
        }, "update-install").start();
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
                    why = S.t("Diegimas atšauktas. Atsisiųstas failas liko — „Tikrinti dabar“ įdiegs jį be naujo atsisiuntimo.");
                else
                    why = S.t("Įdiegti nepavyko: ") + i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                Kit.toast(app, why, Toast.LENGTH_LONG);
            }
        };
        IntentFilter f = new IntentFilter(ACTION_INSTALLED);
        if (Build.VERSION.SDK_INT >= 33) app.registerReceiver(statusReceiver, f, Context.RECEIVER_NOT_EXPORTED);
        else app.registerReceiver(statusReceiver, f);
    }

    /** After the user granted "install unknown apps": install the APK that was waiting for it. */
    static void resumePending(final Activity a) {
        String path = prefs(a).getString("updPendingApk", "");
        if (path.isEmpty()) return;
        if (Build.VERSION.SDK_INT >= 26 && !a.getPackageManager().canRequestPackageInstalls()) return;
        // Coming back from the (portrait) settings screen recreates the launcher; clear the
        // flag only from a live activity so the next instance's onResume picks it up otherwise.
        if (a.isFinishing() || a.isDestroyed()) return;
        prefs(a).edit().remove("updPendingApk").apply();
        File apk = new File(path);
        if (apk.isFile() && apk.getParentFile() != null && apk.getParentFile().equals(folder(a))) install(a, apk);
    }

    private static String readAll(InputStream in) throws java.io.IOException {
        java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) {
            b.write(buf, 0, n);
            if (b.size() > (4 << 20)) throw new java.io.IOException("answer too large");
        }
        in.close();
        return b.toString("UTF-8");
    }
}
