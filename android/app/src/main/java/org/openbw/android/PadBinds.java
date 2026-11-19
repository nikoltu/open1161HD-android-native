package org.openbw.android;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

/** Controller buttons: each one does an action picked from a list (native gamepad.h pad_set_map). */
final class PadBinds {
    static final String PREF = "padMap";

    // Native pact_* order: append only, never renumber.
    static final String[] ACTIONS = {
        "Nieko", "Žymėti (laikant — rėmelis)", "Komanda į žymeklio vietą", "Ataka", "Stop", "Į laisvą grupę",
        "Atšaukti", "Meniu", "Laisvas darbininkas", "Kita bazė", "Ankstesnė grupė (du kartus — kamera)",
        "Kita grupė (du kartus — kamera)", "Kamera į pažymėtus", "Visa armija", "Tikslus žymeklis (laikant)",
        "Į paskutinį įspėjimą", "Priartinti", "Atitolinti", "Kitas pogrupis", "Ankstesnis pogrupis",
    };
    static final int A_NONE = 0, A_SELECT = 1;
    // Native ptrig_* order.
    static final String[] TRIGGERS = { "Nieko", "Komandų ratas (laikant)", "Komandų kortelė pulto mygtukais (laikant)" };

    // Native pad_* button order (SDL_GameController); LT / RT follow as slots 15 and 16.
    static final int SLOTS = 17, LT = 15, RT = 16;
    static final int[] DEFAULTS = {1, 2, 3, 5, 6, 0, 7, 14, 4, 8, 9, 12, 13, 10, 11, 2, 1};
    static final String[] GLYPH = {"A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3", "LB", "RB", "↑", "↓", "←", "→", "LT", "RT"};
    static final String[] NAME = {
        "A", "B", "X", "Y", "Back / View", "", "Start / Menu", "Kairės lazdelės paspaudimas", "Dešinės lazdelės paspaudimas",
        "Kairysis petys", "Dešinysis petys", "Kryžmė aukštyn", "Kryžmė žemyn", "Kryžmė kairėn", "Kryžmė dešinėn",
        "Kairysis gaidukas", "Dešinysis gaidukas",
    };
    // Guide (5) belongs to the system and is left out of the editor.
    private static final int[] LEFT = {0, 1, 2, 3, 9, 10, 7, 8};
    private static final int[] RIGHT = {11, 12, 13, 14, 4, 6, 15, 16};

    static boolean on(SharedPreferences p) { return "pad".equals(p.getString("inputMode", "touch")); }

    static int[] load(SharedPreferences p) {
        int[] m = DEFAULTS.clone();
        String s = p.getString(PREF, "");
        String[] parts = s.split(",");
        if (parts.length != SLOTS) return m;
        try {
            for (int i = 0; i < SLOTS; i++) {
                int v = Integer.parseInt(parts[i].trim());
                if (v < 0 || v >= (i >= LT ? TRIGGERS.length : ACTIONS.length)) return DEFAULTS.clone();
                m[i] = v;
            }
        } catch (NumberFormatException e) {
            return DEFAULTS.clone();
        }
        return m;
    }

    static String arg(SharedPreferences p) {
        int[] m = load(p);
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < SLOTS; i++) { if (i > 0) b.append(','); b.append(m[i]); }
        return b.toString();
    }

    private static void save(SharedPreferences p, int[] m) {
        boolean def = java.util.Arrays.equals(m, DEFAULTS);
        if (def) { p.edit().remove(PREF).apply(); return; }
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < SLOTS; i++) { if (i > 0) b.append(','); b.append(m[i]); }
        p.edit().putString(PREF, b.toString()).apply();
    }

    private static String actionName(int slot, int v) {
        return S.t(slot >= LT ? TRIGGERS[v] : ACTIONS[v]);
    }

    /** null when the change is allowed; otherwise why not. */
    private static String check(int[] m, int slot, int v) {
        if (slot >= LT || m[slot] != A_SELECT || v == A_SELECT) return null;
        for (int i = 0; i < LT; i++) if (i != slot && m[i] == A_SELECT) return null;
        return S.t("Bent vienas mygtukas turi žymėti.");
    }

    static LinearLayout table(final Activity act, final Kit kit, final SharedPreferences p) {
        final int[] m = load(p);
        final LinearLayout list = new LinearLayout(act);
        list.setOrientation(LinearLayout.VERTICAL);
        final TextView[] actView = new TextView[SLOTS];
        final View[] resets = new View[SLOTS];
        final Runnable refresh = () -> {
            for (int i = 0; i < SLOTS; i++) {
                if (actView[i] == null) continue;
                actView[i].setText(actionName(i, m[i]) + "  ▾");
                actView[i].setTextColor(m[i] == DEFAULTS[i] ? Kit.ACCENT : 0xFFFFFFFF);
                if (resets[i] != null) resets[i].setVisibility(m[i] == DEFAULTS[i] ? View.INVISIBLE : View.VISIBLE);
            }
        };

        LinearLayout head = new LinearLayout(act);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        head.setPadding(0, Keybinds.dp(act, 6), 0, Keybinds.dp(act, 6));
        TextView hint = kit.text(S.t("Kairė lazdelė — žymeklis, dešinė — kamera (ar rato kryptis). Mygtuko veiksmą pasirink iš sąrašo. Laikant kortelės gaiduką: LB / L3 — kitas / ankstesnis pogrupis."), Kit.TEXT2, 11);
        hint.setPadding(0, 0, Keybinds.dp(act, 12), 0);
        head.addView(hint, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        head.addView(kit.smallButton(S.t("Atstatyti visus"), () -> {
            System.arraycopy(DEFAULTS, 0, m, 0, SLOTS);
            save(p, m);
            refresh.run();
            Kit.toast(act, S.t("Pulto mygtukai atstatyti."), Toast.LENGTH_SHORT);
        }), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, kit.hdp(40)));
        list.addView(head);

        if (act.getResources().getConfiguration().screenWidthDp >= 560) {
            list.addView(new Diagram(act, kit, p, m, actView, refresh), new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            refresh.run();
            return list;
        }
        final boolean two = act.getResources().getConfiguration().screenWidthDp >= 1100;
        LinearLayout cols = new LinearLayout(act);
        cols.setOrientation(two ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        cols.setBaselineAligned(false);
        LinearLayout left = new LinearLayout(act), right = new LinearLayout(act);
        left.setOrientation(LinearLayout.VERTICAL);
        right.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams l1 = two ? new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        if (two) l1.rightMargin = Keybinds.dp(act, 28);
        cols.addView(left, l1);
        cols.addView(right, two ? new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        list.addView(cols);

        left.addView(kit.label(S.t("Mygtukai")));
        for (int s : LEFT) left.addView(row(act, kit, p, m, s, actView, resets, refresh), rowLp(act));
        right.addView(kit.label(S.t("Kryžmė, meniu ir gaidukai")));
        for (int s : RIGHT) right.addView(row(act, kit, p, m, s, actView, resets, refresh), rowLp(act));
        refresh.run();
        return list;
    }

    private static LinearLayout.LayoutParams rowLp(Activity act) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = Keybinds.dp(act, 2);
        return lp;
    }

    private static View row(final Activity act, final Kit kit, final SharedPreferences p, final int[] m, final int slot,
                            TextView[] actView, View[] resets, final Runnable refresh) {
        LinearLayout row = new LinearLayout(act);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(Keybinds.dp(act, 8), Keybinds.dp(act, 3), 0, Keybinds.dp(act, 3));
        row.setBackground(kit.box(0x00000000, 0xFF1C2428));
        TextView glyph = kit.text(GLYPH[slot], Kit.ACCENT, 12);
        glyph.setTypeface(Typeface.DEFAULT_BOLD);
        glyph.setGravity(Gravity.CENTER);
        glyph.setBackground(kit.box(0xFF05080A, Kit.LINE));
        row.addView(glyph, new LinearLayout.LayoutParams(Keybinds.dp(act, 46), Keybinds.dp(act, 26)));
        TextView label = kit.text(S.t(NAME[slot]), Kit.TEXT, 13);
        label.setSingleLine(true);
        label.setEllipsize(android.text.TextUtils.TruncateAt.END);
        label.setPadding(Keybinds.dp(act, 12), 0, Keybinds.dp(act, 8), 0);
        row.addView(label, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        final TextView pick = picker(act, kit, p, m, slot, refresh);
        actView[slot] = pick;
        row.addView(pick, new LinearLayout.LayoutParams(Keybinds.dp(act, 250), kit.hdp(36)));
        TextView reset = kit.smallButton("↺", () -> {
            String err = check(m, slot, DEFAULTS[slot]);
            if (err != null) { Kit.toast(act, err, Toast.LENGTH_SHORT); return; }
            m[slot] = DEFAULTS[slot];
            save(p, m);
            refresh.run();
        });
        reset.setContentDescription(S.t("Atstatyti numatytą"));
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(kit.hdp(36), kit.hdp(36));
        rlp.leftMargin = Keybinds.dp(act, 6);
        resets[slot] = reset;
        row.addView(reset, rlp);
        return row;
    }

    // One button's action picker: a tap opens the list, a long press restores the default.
    private static TextView picker(final Activity act, final Kit kit, final SharedPreferences p, final int[] m, final int slot,
                                   final Runnable refresh) {
        final TextView pick = kit.smallButton("", () -> {});
        pick.setTextColor(Kit.ACCENT);
        pick.setSingleLine(true);
        pick.setEllipsize(android.text.TextUtils.TruncateAt.END);
        pick.setOnLongClickListener(v -> {
            if (m[slot] == DEFAULTS[slot]) return false;
            String err = check(m, slot, DEFAULTS[slot]);
            if (err != null) { Kit.toast(act, err, Toast.LENGTH_SHORT); return true; }
            m[slot] = DEFAULTS[slot];
            save(p, m);
            refresh.run();
            Kit.toast(act, S.t("Atstatyta numatyta."), Toast.LENGTH_SHORT);
            return true;
        });
        pick.setOnClickListener(v -> {
            MenuAudio.play(MenuAudio.CLICK);
            final String[] src = slot >= LT ? TRIGGERS : ACTIONS;
            CharSequence[] items = new CharSequence[src.length];
            for (int i = 0; i < src.length; i++) items[i] = (i == m[slot] ? "● " : "    ") + S.t(src[i]);
            new KitDialog(act).setTitle(S.t(NAME[slot]).startsWith(GLYPH[slot]) ? S.t(NAME[slot]) : GLYPH[slot] + " — " + S.t(NAME[slot])).setItems(items, (d, which) -> {
                String err = check(m, slot, which);
                if (err != null) { Kit.toast(act, err, Toast.LENGTH_SHORT); return; }
                m[slot] = which;
                save(p, m);
                refresh.run();
            }).show();
        });
        return pick;
    }

    // Xbox-layout face colours for the glyph chips.
    static int glyphColor(int slot) {
        switch (slot) {
            case 0: return 0xFF6CC24A;
            case 1: return 0xFFE0564E;
            case 2: return 0xFF4C8DF0;
            case 3: return 0xFFF2C53D;
            default: return Kit.ACCENT;
        }
    }

    /**
     * The controller drawn in the middle (Xbox layout, design space 1000 x 720) with each button's action picker
     * at the side and a leader line to it. The D-pad and the face buttons share one leader each, fanned by a bracket.
     */
    static final class Diagram extends FrameLayout {
        private static final int[] L_SLOTS = {15, 9, 4, 7, 11, 12, 13, 14};
        private static final int[] L_GROUP = {0, 1, 2, 3, 4, 4, 4, 4};
        private static final int[] R_SLOTS = {16, 10, 6, 3, 2, 1, 0, 8};
        private static final int[] R_GROUP = {0, 1, 2, 3, 3, 3, 3, 4};
        // Per group: anchor x, y, then optional elbow points (design space) the leader passes on its way out.
        private static final float[][] L_ANCHOR = {{268, 30}, {250, 98}, {432, 236, 432, 142, 120, 142}, {292, 250}, {372, 390}};
        private static final float[][] R_ANCHOR = {{732, 30}, {750, 98}, {568, 236, 568, 142, 880, 142}, {832, 250}, {610, 392}};

        private final Activity act;
        private final boolean small;
        private final View[] lCells = new View[8], rCells = new View[8];
        private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG), body = new Paint(Paint.ANTI_ALIAS_FLAG),
                edge = new Paint(Paint.ANTI_ALIAS_FLAG), dot = new Paint(Paint.ANTI_ALIAS_FLAG),
                txt = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
        private float cw, ox, oy, sc, rowH, gapG, lx, rx;
        private final float[] lY = new float[8], rY = new float[8];

        Diagram(Activity act, Kit kit, SharedPreferences p, int[] m, TextView[] actView, Runnable refresh) {
            super(act);
            this.act = act;
            small = act.getResources().getConfiguration().screenWidthDp < 900;
            setWillNotDraw(false);
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(dp(1.4f));
            line.setColor(0xB04FD46A);
            body.setStyle(Paint.Style.FILL);
            edge.setStyle(Paint.Style.STROKE);
            edge.setStrokeWidth(dp(1.6f));
            edge.setColor(Kit.STEEL);
            dot.setColor(Kit.ACCENT);
            txt.setTextAlign(Paint.Align.CENTER);
            txt.setTypeface(Typeface.DEFAULT_BOLD);
            for (int side = 0; side < 2; side++) {
                int[] slots = side == 0 ? L_SLOTS : R_SLOTS;
                for (int i = 0; i < 8; i++) {
                    int slot = slots[i];
                    LinearLayout cell = new LinearLayout(act);
                    cell.setOrientation(LinearLayout.HORIZONTAL);
                    cell.setGravity(Gravity.CENTER_VERTICAL);
                    TextView glyph = kit.text(GLYPH[slot], glyphColor(slot), small ? 11 : 12);
                    glyph.setTypeface(Typeface.DEFAULT_BOLD);
                    glyph.setGravity(Gravity.CENTER);
                    glyph.setBackground(kit.box(0xFF05080A, slot <= 3 ? glyphColor(slot) : Kit.LINE));
                    glyph.setContentDescription(S.t(NAME[slot]));
                    TextView pick = picker(act, kit, p, m, slot, refresh);
                    if (small) {
                        pick.setTextSize(11);
                        pick.setSingleLine(false);
                        pick.setMaxLines(2);
                        pick.setPadding(dpi(4), 0, dpi(4), 0);
                    }
                    actView[slot] = pick;
                    LinearLayout.LayoutParams gl = new LinearLayout.LayoutParams(dpi(small ? 32 : 46), dpi(26));
                    LinearLayout.LayoutParams pl = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
                    if (side == 0) { pl.rightMargin = dpi(small ? 4 : 6); cell.addView(pick, pl); cell.addView(glyph, gl); }
                    else { pl.leftMargin = dpi(small ? 4 : 6); cell.addView(glyph, gl); cell.addView(pick, pl); }
                    addView(cell);
                    if (side == 0) lCells[i] = cell; else rCells[i] = cell;
                }
            }
        }

        private float dp(float v) { return v * act.getResources().getDisplayMetrics().density; }
        private int dpi(float v) { return Math.round(dp(v)); }

        @Override protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec);
            float gap = dp(small ? 12 : 34);
            if (small) {
                // Narrow pane: the controller keeps a fixed share, the pickers take the rest (two-line names).
                cw = Math.min(dp(270), (w - Math.max(dp(170), w * 0.34f) - 2 * gap) / 2);
            } else {
                cw = Math.max(dp(190), Math.min(dp(270), w * 0.27f));
            }
            rowH = dp(small ? 46 : 44);
            gapG = dp(small ? 8 : 12);
            float rowsH = 8 * rowH + 4 * gapG;
            float ctrlW = Math.max(dp(160), w - 2 * cw - 2 * gap);
            sc = Math.min(ctrlW / 1000f, rowsH * 1.08f / 720f);
            float ctrlH = 720 * sc;
            float h = Math.max(ctrlH, rowsH) + dp(16);
            ox = (w - 1000 * sc) / 2;
            lx = Math.max(0, ox - gap - cw);
            rx = Math.min(w - cw, ox + 1000 * sc + gap);
            oy = (h - ctrlH) / 2;
            float top = (h - rowsH) / 2, y = top;
            for (int i = 0; i < 8; i++) { if (i > 0 && L_GROUP[i] != L_GROUP[i - 1]) y += gapG; lY[i] = y + rowH / 2; y += rowH; }
            y = top;
            for (int i = 0; i < 8; i++) { if (i > 0 && R_GROUP[i] != R_GROUP[i - 1]) y += gapG; rY[i] = y + rowH / 2; y += rowH; }
            int cwI = Math.round(cw), chI = Math.round(rowH - dp(6));
            for (int i = 0; i < 8; i++) {
                lCells[i].measure(MeasureSpec.makeMeasureSpec(cwI, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(chI, MeasureSpec.EXACTLY));
                rCells[i].measure(MeasureSpec.makeMeasureSpec(cwI, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(chI, MeasureSpec.EXACTLY));
            }
            setMeasuredDimension(w, Math.round(h));
        }

        @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
            int w = r - l, cwI = Math.round(cw);
            for (int i = 0; i < 8; i++) {
                int hh = lCells[i].getMeasuredHeight();
                int l0 = Math.round(lx), r0 = Math.round(rx);
                lCells[i].layout(l0, Math.round(lY[i] - hh / 2f), l0 + cwI, Math.round(lY[i] + hh / 2f));
                rCells[i].layout(r0, Math.round(rY[i] - hh / 2f), r0 + cwI, Math.round(rY[i] + hh / 2f));
            }
        }

        private float X(float x) { return ox + x * sc; }
        private float Y(float y) { return oy + y * sc; }

        @Override protected void onDraw(Canvas c) {
            drawPad(c);
            leaders(c, true);
            leaders(c, false);
        }

        private void leaders(Canvas c, boolean left) {
            int[] group = left ? L_GROUP : R_GROUP;
            float[][] anc = left ? L_ANCHOR : R_ANCHOR;
            float[] ys = left ? lY : rY;
            float colX = left ? lx + cw + dp(2) : rx - dp(2);
            float brX = left ? lx + cw + dp(12) : rx - dp(12);
            int i = 0;
            while (i < 8) {
                int g = group[i], j = i;
                while (j + 1 < 8 && group[j + 1] == g) j++;
                float ax = X(anc[g][0]), ay = Y(anc[g][1]);
                path.reset();
                path.moveTo(ax, ay);
                for (int k = 2; k + 1 < anc[g].length; k += 2) path.lineTo(X(anc[g][k]), Y(anc[g][k + 1]));
                if (i == j) {
                    path.lineTo(brX, ys[i]);
                    path.lineTo(colX, ys[i]);
                } else {
                    float mid = (ys[i] + ys[j]) / 2;
                    path.lineTo(left ? brX + dp(10) : brX - dp(10), mid);
                    path.lineTo(brX, mid);
                    path.moveTo(brX, ys[i]);
                    path.lineTo(brX, ys[j]);
                    for (int k = i; k <= j; k++) { path.moveTo(brX, ys[k]); path.lineTo(colX, ys[k]); }
                }
                c.drawPath(path, line);
                c.drawCircle(ax, ay, dp(3.2f), dot);
                i = j + 1;
            }
        }

        private void shape(Canvas c, int fill) {
            body.setColor(fill);
            c.drawPath(path, body);
            c.drawPath(path, edge);
        }

        private void drawPad(Canvas c) {
            for (int side = 0; side < 2; side++) {
                float s = side == 0 ? 1 : -1, x0 = side == 0 ? 0 : 1000;
                // trigger
                path.reset();
                path.moveTo(X(x0 + s * 205), Y(80));
                path.cubicTo(X(x0 + s * 208), Y(22), X(x0 + s * 235), Y(4), X(x0 + s * 270), Y(4));
                path.cubicTo(X(x0 + s * 305), Y(4), X(x0 + s * 330), Y(20), X(x0 + s * 335), Y(74));
                path.close();
                shape(c, 0xFF0B1114);
                // bumper
                path.reset();
                path.moveTo(X(x0 + s * 128), Y(160));
                path.cubicTo(X(x0 + s * 150), Y(92), X(x0 + s * 235), Y(66), X(x0 + s * 400), Y(84));
                path.lineTo(X(x0 + s * 400), Y(116));
                path.cubicTo(X(x0 + s * 260), Y(104), X(x0 + s * 190), Y(118), X(x0 + s * 152), Y(168));
                path.close();
                shape(c, 0xFF0E161A);
            }
            path.reset();
            path.moveTo(X(300), Y(112));
            path.cubicTo(X(400), Y(98), X(600), Y(98), X(700), Y(112));
            path.cubicTo(X(810), Y(100), X(905), Y(140), X(935), Y(240));
            path.cubicTo(X(985), Y(400), X(1000), Y(600), X(935), Y(675));
            path.cubicTo(X(885), Y(730), X(790), Y(705), X(755), Y(625));
            path.cubicTo(X(720), Y(550), X(685), Y(515), X(610), Y(512));
            path.lineTo(X(390), Y(512));
            path.cubicTo(X(315), Y(515), X(280), Y(550), X(245), Y(625));
            path.cubicTo(X(210), Y(705), X(115), Y(730), X(65), Y(675));
            path.cubicTo(X(0), Y(600), X(15), Y(400), X(65), Y(240));
            path.cubicTo(X(95), Y(140), X(190), Y(100), X(300), Y(112));
            path.close();
            shape(c, 0xFF141C21);
            stick(c, 292, 250);
            stick(c, 610, 392);
            float d = 22, L = 58, cx = 372, cy = 390;
            path.reset();
            path.moveTo(X(cx - d), Y(cy - L));
            path.lineTo(X(cx + d), Y(cy - L));
            path.lineTo(X(cx + d), Y(cy - d));
            path.lineTo(X(cx + L), Y(cy - d));
            path.lineTo(X(cx + L), Y(cy + d));
            path.lineTo(X(cx + d), Y(cy + d));
            path.lineTo(X(cx + d), Y(cy + L));
            path.lineTo(X(cx - d), Y(cy + L));
            path.lineTo(X(cx - d), Y(cy + d));
            path.lineTo(X(cx - L), Y(cy + d));
            path.lineTo(X(cx - L), Y(cy - d));
            path.lineTo(X(cx - d), Y(cy - d));
            path.close();
            shape(c, 0xFF0B1114);
            face(c, 740, 316, "A", 0);
            face(c, 800, 250, "B", 1);
            face(c, 680, 250, "X", 2);
            face(c, 740, 184, "Y", 3);
            body.setColor(0xFF0B1114);
            for (float bx : new float[]{432, 568}) {
                c.drawCircle(X(bx), Y(236), 20 * sc, body);
                c.drawCircle(X(bx), Y(236), 20 * sc, edge);
            }
            int e = edge.getColor();
            edge.setColor(0x665F6B72);
            c.drawCircle(X(500), Y(176), 30 * sc, edge);
            edge.setColor(e);
        }

        private void stick(Canvas c, float x, float y) {
            body.setColor(0xFF0B1114);
            c.drawCircle(X(x), Y(y), 64 * sc, body);
            c.drawCircle(X(x), Y(y), 64 * sc, edge);
            body.setColor(0xFF1C262C);
            c.drawCircle(X(x), Y(y), 44 * sc, body);
            c.drawCircle(X(x), Y(y), 44 * sc, edge);
        }

        private void face(Canvas c, float x, float y, String s, int slot) {
            body.setColor(0xFF0B1114);
            c.drawCircle(X(x), Y(y), 30 * sc, body);
            int e = edge.getColor();
            edge.setColor(glyphColor(slot));
            c.drawCircle(X(x), Y(y), 30 * sc, edge);
            edge.setColor(e);
            txt.setColor(glyphColor(slot));
            txt.setTextSize(30 * sc);
            c.drawText(s, X(x), Y(y) + 11 * sc, txt);
        }
    }

    private PadBinds() {}
}
