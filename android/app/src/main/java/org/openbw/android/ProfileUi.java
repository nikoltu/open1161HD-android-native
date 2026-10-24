package org.openbw.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The profile card and the portrait gallery. Kept out of SetupActivity: the launcher talks to it through
 * {@link Host} only. The look comes from {@link Kit}.
 */
final class ProfileUi extends Kit {
    interface Host {
        SharedPreferences prefs();
        String activeProfile();
        File gameDir();
        int playerColor();                     // ARGB of the local player's colour
        List<String[]> campaignProgress();     // {episode title, missions done, missions total}
        void reload();                         // profile or portrait changed: rebuild the launcher
    }

    private static final int[] RACE_BAR = {0xFF4F9BD0, 0xFFC0603A, 0xFFD8A840, 0xFF9AA6A0};   // T Z P R
    private static final String[] RACE_LABEL = {"Terran", "Zerg", "Protoss", "Random"};
    private static final String[] RACE_STAT = {"T", "Z", "P", "R"};

    private final Host host;

    ProfileUi(Activity a, Host host) {
        super(a);
        this.host = host;
    }

    private int stat(String profile, String suffix) {
        return host.prefs().getInt("p." + profile + ".stat." + suffix, 0);
    }

    // ---- profile card (5.11) --------------------------------------------------------
    void showProfileCard() {
        final String profile = host.activeProfile();
        final SharedPreferences pr = host.prefs();
        final File gameDir = host.gameDir();
        final int screenW = a.getResources().getDisplayMetrics().widthPixels;
        final int screenH = a.getResources().getDisplayMetrics().heightPixels;

        LinearLayout panel = new LinearLayout(a);
        panel.setOrientation(LinearLayout.HORIZONTAL);
        panel.setBackground(box(PANEL, LINE));
        panel.setPadding(dp(18), dp(18), dp(18), dp(18));

        // left: the big portrait
        LinearLayout left = new LinearLayout(a);
        left.setOrientation(LinearLayout.VERTICAL);
        final PortraitView big = new PortraitView(a);
        int bigW = Math.min(dp(300), Math.min(screenW / 3, (screenH - dp(240)) * 60 / 56));
        big.setScale(PortraitView.scaleFor(bigW));
        big.setBorderColor(host.playerColor());
        big.setPortrait(gameDir, PortraitStore.activeId(pr, profile, gameDir));
        big.setOnClickListener(v -> big.speak());
        left.addView(big);
        final Dialog[] self = new Dialog[1];
        TextView change = button(S.t("Keisti portretą"), 0, () -> {
            if (!PortraitStore.available(gameDir)) {
                Toast.makeText(a, S.t("Portretai atsiras importavus žaidimo failus."), Toast.LENGTH_LONG).show();
                return;
            }
            showGallery(() -> { if (self[0] != null) self[0].dismiss(); host.reload(); });
        });
        LinearLayout.LayoutParams chLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        chLp.topMargin = dp(12);
        left.addView(change, chLp);
        panel.addView(left, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // right: name, stats, campaigns, actions
        LinearLayout right = new LinearLayout(a);
        right.setOrientation(LinearLayout.VERTICAL);
        right.setPadding(dp(20), 0, 0, 0);
        TextView name = text(profile, Color.WHITE, 24);
        name.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        right.addView(name);

        int games = stat(profile, "games");
        right.addView(label(S.t("Pergalės pagal rasę") + " · " + games + " " + S.t("žaid.")));
        int maxWins = 1;
        for (String k : RACE_STAT) maxWins = Math.max(maxWins, stat(profile, "wins." + k));
        for (int i = 0; i != 4; ++i) {
            int wins = stat(profile, "wins." + RACE_STAT[i]);
            int played = stat(profile, "games." + RACE_STAT[i]);      // counted since v0.106; older games have no per-race total
            float frac = played > 0 ? (float) wins / played : (float) wins / maxWins;
            right.addView(barRow(RACE_LABEL[i], wins == 0 ? 0f : frac, RACE_BAR[i],
                    played > 0 ? wins + " / " + played : String.valueOf(wins), 14, dp(84)));
        }

        List<String[]> eps = host.campaignProgress();
        if (eps != null && !eps.isEmpty()) {
            right.addView(label(S.t("Kampanijos")));
            for (String[] e : eps) {
                int done = Integer.parseInt(e[1]), total = Math.max(1, Integer.parseInt(e[2]));
                right.addView(barRow(e[0], (float) done / total, LABEL, done + "/" + e[2], 12, dp(230)));
            }
        }

        LinearLayout actions = new LinearLayout(a);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setGravity(Gravity.CENTER_VERTICAL);
        actions.setPadding(0, dp(14), 0, 0);
        LinearLayout.LayoutParams gap = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        gap.rightMargin = dp(8);
        actions.addView(button(S.t("Pervardyti"), 0, () -> showRename(self)), gap);
        actions.addView(button(S.t("Kitas profilis"), 0, () -> showSwitch(self)), gap);
        actions.addView(button(S.t("Naujas profilis"), 0, () -> showCreate(self)), gap);
        actions.addView(new View(a), new LinearLayout.LayoutParams(0, 1, 1f));
        actions.addView(button(S.t("Uždaryti"), 2, () -> { if (self[0] != null) self[0].dismiss(); }));
        right.addView(actions);

        ScrollView sv = new ScrollView(a);   // short screens: the right column scrolls, the portrait stays
        sv.addView(right);
        panel.addView(sv, new LinearLayout.LayoutParams(Math.min(dp(640), screenW * 5 / 10), ViewGroup.LayoutParams.WRAP_CONTENT));

        self[0] = modal(panel, false);
        self[0].show();
    }

    // ---- rename / switch / create -------------------------------------------------------
    static String cleanName(String name) {
        // keep pref keys and the tab-delimited LAN announcement clean
        name = name.trim().replaceAll("[.|\\t\\r\\n]", "_");
        if (name.length() > 24) name = name.substring(0, 24);
        return name;
    }

    private void showCreate(final Dialog[] card) {
        final EditText input = new EditText(a);
        input.setHint(S.t("Vardas (rodomas žaidime)"));
        input.setInputType(android.text.InputType.TYPE_CLASS_TEXT);
        new AlertDialog.Builder(a)
                .setTitle(S.t("Naujas profilis"))
                .setView(input)
                .setPositiveButton(S.t("Sukurti"), (d, w) -> {
                    String name = cleanName(input.getText().toString());
                    if (name.isEmpty()) return;
                    Set<String> set = new HashSet<>(host.prefs().getStringSet("profiles", new HashSet<String>()));
                    set.add(name);
                    host.prefs().edit().putStringSet("profiles", set).putString("activeProfile", name).apply();
                    if (card[0] != null) card[0].dismiss();
                    host.reload();
                })
                .setNegativeButton(S.t("Atšaukti"), null)
                .show();
    }

    /** Renames the active profile: every p.<old>.* key moves to p.<new>.* (progress, stats, portrait). */
    private void showRename(final Dialog[] card) {
        final String old = host.activeProfile();
        final EditText input = new EditText(a);
        input.setText(old);
        input.setSelectAllOnFocus(true);
        input.setInputType(android.text.InputType.TYPE_CLASS_TEXT);
        new AlertDialog.Builder(a)
                .setTitle(S.t("Pervardyti profilį"))
                .setView(input)
                .setPositiveButton(S.t("Gerai"), (d, w) -> {
                    String name = cleanName(input.getText().toString());
                    if (name.isEmpty() || name.equals(old)) return;
                    SharedPreferences pr = host.prefs();
                    Set<String> set = new HashSet<>(pr.getStringSet("profiles", new HashSet<String>()));
                    if (set.contains(name)) {
                        Toast.makeText(a, S.t("Toks profilis jau yra."), Toast.LENGTH_LONG).show();
                        return;
                    }
                    SharedPreferences.Editor e = pr.edit();
                    String from = "p." + old + ".", to = "p." + name + ".";
                    for (Map.Entry<String, ?> en : pr.getAll().entrySet()) {
                        String k = en.getKey();
                        if (!k.startsWith(from)) continue;
                        String nk = to + k.substring(from.length());
                        Object v = en.getValue();
                        if (v instanceof Integer) e.putInt(nk, (Integer) v);
                        else if (v instanceof Boolean) e.putBoolean(nk, (Boolean) v);
                        else if (v instanceof Long) e.putLong(nk, (Long) v);
                        else if (v instanceof Float) e.putFloat(nk, (Float) v);
                        else if (v instanceof String) e.putString(nk, (String) v);
                        else if (v instanceof Set) {
                            Set<String> copy = new HashSet<>();
                            for (Object o : (Set<?>) v) copy.add(String.valueOf(o));
                            e.putStringSet(nk, copy);
                        }
                        e.remove(k);
                    }
                    set.remove(old);
                    set.add(name);
                    e.putStringSet("profiles", set).putString("activeProfile", name).apply();
                    if (card[0] != null) card[0].dismiss();
                    host.reload();
                })
                .setNegativeButton(S.t("Atšaukti"), null)
                .show();
    }

    private void showSwitch(final Dialog[] card) {
        final SharedPreferences pr = host.prefs();
        Set<String> set = pr.getStringSet("profiles", new HashSet<String>());
        final String[] names = set == null ? new String[0] : set.toArray(new String[0]);
        Arrays.sort(names, String.CASE_INSENSITIVE_ORDER);
        final File gameDir = host.gameDir();
        LinearLayout list = new LinearLayout(a);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(16), dp(8), dp(16), dp(8));
        final AlertDialog[] dlg = new AlertDialog[1];
        for (final String n : names) {
            LinearLayout row = new LinearLayout(a);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(4), dp(6), dp(4), dp(6));
            PortraitView pv = new PortraitView(a);
            pv.setScale(1);
            pv.setBorderColor(n.equals(host.activeProfile()) ? LABEL : STEEL);
            pv.setPortrait(gameDir, PortraitStore.activeId(pr, n, gameDir));
            row.addView(pv);
            LinearLayout col = new LinearLayout(a);
            col.setOrientation(LinearLayout.VERTICAL);
            col.setPadding(dp(12), 0, 0, 0);
            col.addView(text(n, Color.WHITE, 17));
            col.addView(text(S.t("Žaidimų ") + stat(n, "games"), TEXT2, 12));
            row.addView(col);
            row.setOnClickListener(v -> {
                pr.edit().putString("activeProfile", n).apply();
                if (dlg[0] != null) dlg[0].dismiss();
                if (card[0] != null) card[0].dismiss();
                host.reload();
            });
            list.addView(row);
        }
        ScrollView sv = new ScrollView(a);
        sv.addView(list);
        dlg[0] = new AlertDialog.Builder(a)
                .setTitle(S.t("Pasirinkite profilį"))
                .setView(sv)
                .setPositiveButton(S.t("Naujas"), (d, w) -> showCreate(card))
                .setNegativeButton(S.t("Atšaukti"), null)
                .show();
    }

    // ---- portrait gallery (5.12, 8.3) -----------------------------------------------------
    /** Full-screen gallery; `onChosen` runs after the profile's portrait was changed. */
    void showGallery(final Runnable onChosen) {
        final String profile = host.activeProfile();
        final SharedPreferences pr = host.prefs();
        final File gameDir = host.gameDir();
        final Map<Integer, PortraitStore.Entry> cat = PortraitStore.catalog(gameDir);
        final int screenW = a.getResources().getDisplayMetrics().widthPixels;
        final int screenH = a.getResources().getDisplayMetrics().heightPixels;
        final int[] sel = {PortraitStore.chosenId(pr, profile)};
        final boolean[] random = {PortraitStore.isRandom(pr, profile)};
        final Dialog[] self = new Dialog[1];
        final boolean[] alive = {true};


        LinearLayout root = new LinearLayout(a);
        root.setOrientation(LinearLayout.HORIZONTAL);

        // ---- left: preview ----
        final int leftW = Math.max(dp(240), Math.min(dp(380), screenW * 27 / 100));
        LinearLayout left = new LinearLayout(a);
        left.setOrientation(LinearLayout.VERTICAL);
        left.setBackground(box(PANEL, LINE));
        left.setPadding(dp(14), dp(10), dp(14), dp(14));
        TextView title = label(S.t("Portretų galerija"));
        title.setPadding(0, 0, 0, dp(8));
        left.addView(title);
        final PortraitView preview = new PortraitView(a);
        int pvW = Math.min(leftW - dp(28) - dp(4), (screenH - dp(420)) * 60 / 56);
        preview.setScale(PortraitView.scaleFor(pvW));
        preview.setBorderColor(LABEL);
        left.addView(preview);
        final TextView pName = text("", Color.WHITE, 20);
        pName.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        pName.setPadding(0, dp(10), 0, dp(2));
        left.addView(pName);
        final TextView pGroup = text("", TEXT2, 12);
        left.addView(pGroup);
        pGroup.setPadding(0, 0, 0, dp(10));
        final boolean[] talk = {false};
        final LinearLayout[] clipSeg = new LinearLayout[1];
        final FrameLayout clipSegHolder = new FrameLayout(a);
        left.addView(clipSegHolder, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        left.addView(label(S.t("Režimas")));
        left.addView(segmented(new String[]{S.t("Fiksuotas"), S.t("Atsitiktinis kaskart")}, random[0] ? 1 : 0, v -> random[0] = v == 1),
                new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        TextView rHint = text(S.t("Naujas portretas kiekvieną kartą atidarius programą."), TEXT2, 11);
        rHint.setPadding(0, dp(8), 0, 0);
        left.addView(rHint);
        left.addView(new View(a), new LinearLayout.LayoutParams(1, 0, 1f));
        LinearLayout btns = new LinearLayout(a);
        btns.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams cancelLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(52));
        cancelLp.rightMargin = dp(8);
        btns.addView(button(S.t("Atšaukti"), 0, () -> { if (self[0] != null) self[0].dismiss(); }), cancelLp);
        btns.addView(button(S.t("Pasirinkti"), 1, () -> {
            PortraitStore.choose(pr, profile, sel[0], random[0]);
            if (self[0] != null) self[0].dismiss();
            if (onChosen != null) onChosen.run();
        }), new LinearLayout.LayoutParams(0, dp(52), 1f));
        left.addView(btns);
        ScrollView leftScroll = new ScrollView(a);   // phones: the preview column may be taller than the screen
        leftScroll.setFillViewport(true);
        leftScroll.addView(left, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(leftScroll, new LinearLayout.LayoutParams(leftW, ViewGroup.LayoutParams.MATCH_PARENT));

        // ---- right: the grid ----
        LinearLayout grid = new LinearLayout(a);
        grid.setOrientation(LinearLayout.VERTICAL);
        grid.setPadding(dp(14), dp(8), dp(14), dp(14));
        final int cellScale = Math.max(1, Math.round(dp(84) / 60f));
        final int cellGap = dp(8);
        final PortraitView probe = new PortraitView(a);
        probe.setScale(cellScale);
        probe.measure(0, 0);
        final int cellW = probe.getMeasuredWidth();
        final int avail = screenW - leftW - dp(24) - dp(12) - dp(28);
        final int cols = Math.max(2, (avail + cellGap) / (cellW + cellGap));

        final List<PortraitView> cells = new ArrayList<>();
        final List<TextView> cellNames = new ArrayList<>();
        final Runnable[] refresh = new Runnable[1];
        String lastRace = "";
        for (Object[] g : PortraitStore.GROUPS) {
            final String race = (String) g[0], sub = (String) g[1];
            Object[] items = (Object[]) g[2];
            List<Integer> ids = new ArrayList<>();
            for (int i = 0; i + 1 < items.length; i += 2) {
                PortraitStore.Entry e = cat.get((Integer) items[i]);
                if (e != null && e.nIdle > 0) ids.add(e.id);
            }
            if (ids.isEmpty()) continue;
            if (!race.equals(lastRace)) {
                lastRace = race;
                LinearLayout hdr = new LinearLayout(a);
                hdr.setOrientation(LinearLayout.HORIZONTAL);
                hdr.setGravity(Gravity.BOTTOM);
                hdr.setPadding(0, dp(10), 0, dp(4));
                TextView rt = text(S.t(race).toUpperCase(java.util.Locale.ROOT), ACCENT, 17);
                rt.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
                hdr.addView(rt);
                grid.addView(hdr);
                View rule = new View(a);
                rule.setBackgroundColor(LINE);
                grid.addView(rule, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(1))));
            }
            TextView st = text(S.t(sub).toUpperCase(java.util.Locale.ROOT), TEXT2, 11);
            st.setLetterSpacing(0.1f);
            st.setPadding(0, dp(8), 0, dp(6));
            grid.addView(st);
            LinearLayout row = null;
            for (int i = 0; i != ids.size(); ++i) {
                if (i % cols == 0) {
                    row = new LinearLayout(a);
                    row.setOrientation(LinearLayout.HORIZONTAL);
                    grid.addView(row);
                }
                final int id = ids.get(i);
                LinearLayout cell = new LinearLayout(a);
                cell.setOrientation(LinearLayout.VERTICAL);
                final PortraitView pv = new PortraitView(a);
                pv.setScale(cellScale);
                pv.setBorderColor(STEEL);
                pv.setTag(id);
                cell.addView(pv);
                TextView nm = text(PortraitStore.name(id), TEXT2, 11);
                nm.setGravity(Gravity.CENTER_HORIZONTAL);
                nm.setSingleLine(true);
                nm.setEllipsize(TextUtils.TruncateAt.END);
                nm.setPadding(0, dp(2), 0, dp(6));
                nm.setTag(id);
                cell.addView(nm, new LinearLayout.LayoutParams(cellW, ViewGroup.LayoutParams.WRAP_CONTENT));
                cell.setOnClickListener(v -> { sel[0] = id; refresh[0].run(); });
                LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                clp.rightMargin = cellGap;
                row.addView(cell, clp);
                cells.add(pv);
                cellNames.add(nm);
            }
        }
        ScrollView gs = new ScrollView(a);
        gs.setBackground(box(PANEL, LINE));
        gs.addView(grid);
        LinearLayout.LayoutParams gsl = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
        gsl.leftMargin = dp(12);
        root.addView(gs, gsl);

        // selection -> preview panel + cell highlight
        refresh[0] = () -> {
            int id = sel[0];
            preview.setPortrait(gameDir, id);
            talk[0] = false;
            pName.setText(PortraitStore.name(id));
            String grp = "";
            for (Object[] g : PortraitStore.GROUPS) {
                Object[] items = (Object[]) g[2];
                for (int i = 0; i + 1 < items.length; i += 2)
                    if ((Integer) items[i] == id) grp = S.t((String) g[0]) + " · " + S.t((String) g[1]);
            }
            pGroup.setText(grp);
            clipSegHolder.removeAllViews();
            clipSeg[0] = segmented(new String[]{S.t("Ramybė"), S.t("Kalbantis")}, 0, v -> {
                talk[0] = v == 1;
                preview.setTalkLoop(talk[0]);
            });
            clipSegHolder.addView(clipSeg[0]);
            for (int i = 0; i != cells.size(); ++i) {
                boolean on = (Integer) cells.get(i).getTag() == id;
                cells.get(i).setBorderColor(on ? LABEL : STEEL);
                cellNames.get(i).setTextColor(on ? Color.WHITE : TEXT2);
                cellNames.get(i).setTypeface(Typeface.DEFAULT, on ? Typeface.BOLD : Typeface.NORMAL);
            }
        };
        preview.setOnClickListener(v -> preview.speak());
        refresh[0].run();

        self[0] = modal(root, true);
        self[0].setOnDismissListener(d -> alive[0] = false);
        self[0].show();

        // decode the idle clips off the UI thread (~1-4 ms each), top of the grid first
        final Handler ui = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            for (final PortraitView pv : cells) {
                if (!alive[0]) return;
                final int id = (Integer) pv.getTag();
                final PortraitStore.Clip c = PortraitStore.clip(gameDir, id, false, 0);
                ui.post(() -> { if (alive[0]) pv.setDecoded(gameDir, id, c); });
            }
        }, "portrait-gallery").start();
    }
}
