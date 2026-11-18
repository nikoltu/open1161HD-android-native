package org.openbw.android;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.GestureDetector;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;

/**
 * Classic key layout: a unit's 3x3 command card with the key of every button. A tap
 * moves that command to another key (for every unit that has it -- the original keeps
 * one letter per command too), a long press puts it back on its original letter.
 * Two buttons of one card on the same key are marked in the card: the first one fires.
 * The cards come from assets/kbm_cards.txt (written by `loadscreen_uicheck kbmcards`
 * from the game's own card code); names, letters and icons from the player's MPQs.
 */
final class KeyCardEditor extends LinearLayout {
    interface IntSource { int get(); }

    private static final class Card {
        int race, unit, menu;
        final int[] tbl = new int[9], icon = new int[9];   // tbl: -2 empty, -1 cancel key, 0 no letter
    }

    private final Activity act;
    private final Kit kit;
    private final SharedPreferences p;
    private final File gameDir;
    private final IntSource cancelKey;
    private final List<Card> cards = new ArrayList<>();
    private final Map<Integer, String> text = new HashMap<>();
    private final Map<Integer, Bitmap> icons = new HashMap<>();
    private int[] atlas;
    private Map<Integer, Integer> cmd = new HashMap<>();
    private int race = 1, unit = 7, menu = 0, picked = -1;

    private final LinearLayout body;
    private CardView cardView;
    private LinearLayout strip, menus;
    private TextView info;

    KeyCardEditor(Activity act, Kit kit, SharedPreferences p, File gameDir, IntSource cancelKey) {
        super(act);
        this.act = act;
        this.kit = kit;
        this.p = p;
        this.gameDir = gameDir;
        this.cancelKey = cancelKey;
        setOrientation(VERTICAL);
        body = new LinearLayout(act);
        body.setOrientation(VERTICAL);
        addView(body, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        body.addView(kit.text(S.t("Kraunama…"), Kit.TEXT2, 12));
        new Thread(this::load, "keycards").start();
    }

    // ---- data -------------------------------------------------------------------
    private void load() {
        List<Card> list = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(act.getAssets().open("kbm_cards.txt")))) {
            String line;
            while ((line = r.readLine()) != null) {
                String[] f = line.trim().split(" ");
                if (f.length != 12) continue;
                Card c = new Card();
                c.race = Integer.parseInt(f[0]); c.unit = Integer.parseInt(f[1]); c.menu = Integer.parseInt(f[2]);
                for (int i = 0; i != 9; ++i) {
                    String s = f[3 + i];
                    int colon = s.indexOf(':');
                    if (s.equals("-") || colon < 0) { c.tbl[i] = -2; c.icon[i] = -1; continue; }
                    c.tbl[i] = Integer.parseInt(s.substring(0, colon));
                    c.icon[i] = Integer.parseInt(s.substring(colon + 1));
                }
                list.add(c);
            }
        } catch (Exception e) {
            list.clear();
        }
        TreeSet<Integer> want = new TreeSet<>();
        for (Card c : list) {
            want.add(c.unit + 1);   // the unit's name
            for (int t : c.tbl) if (t > 0) want.add(t);
        }
        int[] idx = new int[want.size()];
        int k = 0;
        for (int t : want) idx[k++] = t;
        final String[] s = PortraitStore.statTxt(gameDir, idx);
        final int[] a = PortraitStore.cmdicons(gameDir);
        act.runOnUiThread(() -> {
            cards.clear();
            cards.addAll(list);
            if (s != null) for (int i = 0; i != idx.length && i != s.length; ++i) text.put(idx[i], s[i]);
            atlas = a;
            build();
        });
    }

    void reload() {
        cmd = Keybinds.loadCmd(p);
        refreshCard();
    }

    void invalidateCard() { refreshCard(); }

    private Bitmap icon(int frame) {
        if (atlas == null || frame < 0 || frame >= atlas[0]) return null;
        Bitmap b = icons.get(frame);
        if (b == null) {
            int w = atlas[1], h = atlas[2];
            b = Bitmap.createBitmap(atlas, 3 + frame * w * h, w, w, h, Bitmap.Config.ARGB_8888);
            icons.put(frame, b);
        }
        return b;
    }

    // A button string: hotkey byte, type byte, label (colour bytes, a newline before the help text).
    private String label(int t) {
        if (t == -1) return S.t("Atšaukti");
        String s = text.get(t);
        if (s == null || s.length() < 3) return "#" + t;
        StringBuilder b = new StringBuilder();
        for (int i = 2; i < s.length(); ++i) {
            char c = s.charAt(i);
            if (c == '\n') break;
            if (c >= 0x20) b.append(c);
        }
        return b.toString().trim();
    }

    private String unitName(int u) {
        String s = text.get(u + 1);
        if (s == null) return "#" + u;
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); ++i) if (s.charAt(i) >= 0x20) b.append(s.charAt(i));
        String n = b.toString().trim();
        for (String pre : new String[]{"Terran ", "Zerg ", "Protoss "}) if (n.startsWith(pre)) return n.substring(pre.length());
        return n;
    }

    private int originalKey(int t) {
        if (t == -1) return cancelKey.get();
        String s = text.get(t);
        if (s == null || s.isEmpty()) return 0;
        char c = Character.toLowerCase(s.charAt(0));
        if (c >= 'a' && c <= 'z') return 4 + (c - 'a');
        if (c == 0x1b) return cancelKey.get();
        return 0;
    }

    private int key(int t) {
        if (t > 0) { Integer o = cmd.get(t); if (o != null) return o; }
        return originalKey(t);
    }

    private Card current() {
        for (Card c : cards) if (c.unit == unit && c.menu == menu) return c;
        return null;
    }

    // ---- views ------------------------------------------------------------------
    private void build() {
        body.removeAllViews();
        cmd = Keybinds.loadCmd(p);
        if (cards.isEmpty() || atlas == null || text.isEmpty()) {
            body.addView(kit.text(S.t("Kortelei reikia žaidimo failų (StarDat.mpq)."), Kit.TEXT2, 12));
            return;
        }
        body.addView(kit.segmented(new String[]{S.t("Zergai"), S.t("Terranai"), S.t("Protosai")}, race, v -> {
            race = v;
            for (Card c : cards) if (c.race == v) { unit = c.unit; break; }
            menu = 0;
            picked = -1;
            fillStrip();
            refreshCard();
        }), new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        HorizontalScrollView hs = new HorizontalScrollView(act);
        hs.setHorizontalScrollBarEnabled(false);
        strip = new LinearLayout(act);
        strip.setOrientation(HORIZONTAL);
        hs.addView(strip);
        LayoutParams hlp = new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        hlp.topMargin = Keybinds.dp(act, 6);
        body.addView(hs, hlp);
        menus = new LinearLayout(act);
        menus.setOrientation(VERTICAL);
        body.addView(menus, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout row = new LinearLayout(act);
        row.setOrientation(HORIZONTAL);
        row.setGravity(Gravity.TOP);
        cardView = new CardView();
        int side = Keybinds.dp(act, 216);
        LayoutParams clp = new LayoutParams(side, side * 34 / 36);
        clp.topMargin = Keybinds.dp(act, 8);
        row.addView(cardView, clp);
        info = kit.text("", Kit.TEXT, 12);
        info.setPadding(Keybinds.dp(act, 12), Keybinds.dp(act, 8), 0, 0);
        row.addView(info, new LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        body.addView(row);
        TextView hint = kit.text(S.t("Bakstelėkite mygtuką ir paspauskite naują klavišą — komanda persikels visiems vienetams, kurie ją turi. Ilgas paspaudimas grąžina originalią raidę."), Kit.TEXT2, 11);
        hint.setPadding(0, Keybinds.dp(act, 6), 0, 0);
        body.addView(hint);
        fillStrip();
        refreshCard();
    }

    private void fillStrip() {
        strip.removeAllViews();
        for (Card c : cards) {
            if (c.race != race || c.menu != 0) continue;
            final int u = c.unit;
            UnitChip chip = new UnitChip(u);
            chip.setContentDescription(unitName(u));
            chip.setOnClickListener(v -> {
                MenuAudio.play(MenuAudio.CLICK);
                unit = u;
                menu = 0;
                picked = -1;
                for (int i = 0; i != strip.getChildCount(); ++i) strip.getChildAt(i).invalidate();
                refreshCard();
            });
            int s = Keybinds.dp(act, 40);
            LayoutParams lp = new LayoutParams(s, s);
            lp.rightMargin = Keybinds.dp(act, 4);
            strip.addView(chip, lp);
        }
    }

    private void refreshCard() {
        if (cardView == null) return;
        menus.removeAllViews();
        boolean worker = false;
        for (Card c : cards) if (c.unit == unit && c.menu > 0) worker = true;
        if (worker) {
            LayoutParams lp = new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = Keybinds.dp(act, 6);
            menus.addView(kit.segmented(new String[]{S.t("Komandos"), S.t("Pastatai"), S.t("Pažangūs pastatai")}, menu, v -> {
                menu = v;
                picked = -1;
                refreshCard();
            }), lp);
        }
        cardView.invalidate();
        updateInfo();
    }

    private void updateInfo() {
        Card c = current();
        if (c == null) { info.setText(""); return; }
        StringBuilder b = new StringBuilder();
        b.append(unitName(unit)).append('\n');
        if (picked >= 0 && c.tbl[picked] != -2) {
            int t = c.tbl[picked];
            b.append(label(t)).append(" — ").append(t == 0 ? S.t("be klavišo") : Keybinds.name(key(t)));
            if (t > 0 && cmd.containsKey(t)) b.append(" (").append(S.t("originaliai")).append(' ').append(Keybinds.name(originalKey(t))).append(')');
            b.append('\n');
        }
        // conflicts inside this card: the first button on the key fires
        Map<Integer, List<Integer>> by = new HashMap<>();
        for (int i = 0; i != 9; ++i) {
            if (c.tbl[i] == -2) continue;
            int k = key(c.tbl[i]);
            if (k <= 0) continue;
            List<Integer> l = by.get(k);
            if (l == null) by.put(k, l = new ArrayList<>());
            l.add(i);
        }
        for (Map.Entry<Integer, List<Integer>> e : by.entrySet()) {
            if (e.getValue().size() < 2) continue;
            b.append("⚠ ").append(Keybinds.name(e.getKey())).append(": ");
            for (int j = 0; j != e.getValue().size(); ++j) {
                if (j > 0) b.append(", ");
                b.append(label(c.tbl[e.getValue().get(j)]));
            }
            b.append(" — ").append(S.t("suveiks pirmasis")).append('\n');
        }
        // a card key shared with another binding: the card wins while that unit is selected
        Map<String, Integer> m = Keybinds.load(p);
        for (Keybinds.Action a : Keybinds.ACTIONS) {
            if (a.mouseOnly || a.id.startsWith("card") || a.id.equals("cancel")) continue;
            Integer code = m.get(a.id);
            if (code != null && by.containsKey(code))
                b.append(Keybinds.name(code)).append(' ').append(S.t("taip pat")).append(" „").append(Keybinds.label(a)).append("“ — ")
                        .append(S.t("kol pažymėtas šis vienetas, pirmenybė komandai")).append('\n');
        }
        info.setText(b.toString().trim());
    }

    private boolean conflict(Card c, int i) {
        int k = key(c.tbl[i]);
        if (k <= 0) return false;
        for (int j = 0; j != 9; ++j) if (j != i && c.tbl[j] != -2 && key(c.tbl[j]) == k) return true;
        return false;
    }

    private void edit(int slot) {
        Card c = current();
        if (c == null || c.tbl[slot] == -2) return;
        picked = slot;
        updateInfo();
        cardView.invalidate();
        final int t = c.tbl[slot];
        if (t == -1) { Kit.toast(act, S.t("Atšaukimo klavišas keičiamas eilutėje „Atšaukti“."), Toast.LENGTH_SHORT); return; }
        if (t == 0) { Kit.toast(act, S.t("Ši komanda originale neturi klavišo."), Toast.LENGTH_SHORT); return; }
        Keybinds.capture(act, kit, label(t), 2, code -> {
            if (code == originalKey(t)) cmd.remove(t); else cmd.put(t, code);
            Keybinds.saveCmd(p, cmd);
            refreshCard();
        });
    }

    private void reset(int slot) {
        Card c = current();
        if (c == null || c.tbl[slot] <= 0) return;
        if (cmd.remove(c.tbl[slot]) != null) {
            Keybinds.saveCmd(p, cmd);
            Kit.toast(act, label(c.tbl[slot]) + " — " + Keybinds.name(originalKey(c.tbl[slot])), Toast.LENGTH_SHORT);
        }
        picked = slot;
        refreshCard();
    }

    // ---- the 3x3 card -----------------------------------------------------------
    private final class CardView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG), pix = new Paint();
        private final RectF r = new RectF();
        private final Rect src = new Rect(), dst = new Rect();
        private final GestureDetector gd;

        CardView() {
            super(act);
            pix.setFilterBitmap(false);
            gd = new GestureDetector(act, new GestureDetector.SimpleOnGestureListener() {
                @Override public boolean onDown(MotionEvent e) { return true; }
                @Override public boolean onSingleTapUp(MotionEvent e) {
                    int s = slotAt(e.getX(), e.getY());
                    if (s >= 0) { MenuAudio.play(MenuAudio.CLICK); edit(s); }
                    return true;
                }
                @Override public void onLongPress(MotionEvent e) {
                    int s = slotAt(e.getX(), e.getY());
                    if (s >= 0) reset(s);
                }
            });
        }

        private int slotAt(float x, float y) {
            int cx = (int) (x * 3 / getWidth()), cy = (int) (y * 3 / getHeight());
            if (cx < 0 || cx > 2 || cy < 0 || cy > 2) return -1;
            return cy * 3 + cx;
        }

        @Override public boolean onTouchEvent(MotionEvent e) { return gd.onTouchEvent(e); }

        @Override protected void onDraw(Canvas cv) {
            Card c = current();
            float cw = getWidth() / 3f, ch = getHeight() / 3f, gap = Keybinds.dp(act, 3);
            for (int i = 0; i != 9; ++i) {
                float x = (i % 3) * cw, y = (i / 3) * ch;
                r.set(x + gap / 2, y + gap / 2, x + cw - gap / 2, y + ch - gap / 2);
                boolean empty = c == null || c.tbl[i] == -2;
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(empty ? 0xFF0B1013 : 0xFF141C20);
                cv.drawRoundRect(r, gap, gap, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(Keybinds.dp(act, i == picked ? 2 : 1));
                boolean bad = !empty && conflict(c, i);
                paint.setColor(bad ? Kit.DANGER : i == picked ? Kit.ACCENT : Kit.STEEL);
                cv.drawRoundRect(r, gap, gap, paint);
                if (empty) continue;
                Bitmap b = icon(c.icon[i]);
                if (b != null) {
                    src.set(0, 0, b.getWidth(), b.getHeight());
                    float s = Math.min((r.width() - 2 * gap) / b.getWidth(), (r.height() - 2 * gap) / b.getHeight());
                    int w = (int) (b.getWidth() * s), h = (int) (b.getHeight() * s);
                    dst.set((int) (r.centerX() - w / 2f), (int) (r.centerY() - h / 2f), (int) (r.centerX() + w / 2f), (int) (r.centerY() + h / 2f));
                    cv.drawBitmap(b, src, dst, pix);
                }
                int t = c.tbl[i];
                int k = key(t);
                if (k <= 0) continue;
                String name = Keybinds.name(k);
                paint.setStyle(Paint.Style.FILL);
                paint.setTextSize(ch * 0.22f);
                paint.setFakeBoldText(true);
                float tw = paint.measureText(name), th = ch * 0.22f, pad = th * 0.25f;
                RectF kr = new RectF(r.right - tw - 2 * pad - gap, r.top + gap, r.right - gap, r.top + gap + th + 2 * pad);
                paint.setColor(0xC0000000);
                cv.drawRect(kr, paint);
                paint.setColor(bad ? Kit.DANGER : (t > 0 && cmd.containsKey(t)) ? Kit.LABEL : Kit.ACCENT);
                cv.drawText(name, kr.left + pad, kr.bottom - pad - paint.descent() * 0.4f, paint);
            }
        }
    }

    private final class UnitChip extends View {
        private final int u;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG), pix = new Paint();
        private final Rect src = new Rect(), dst = new Rect();
        private final RectF r = new RectF();

        UnitChip(int u) {
            super(act);
            this.u = u;
            pix.setFilterBitmap(false);
        }

        @Override protected void onDraw(Canvas cv) {
            float g = Keybinds.dp(act, 1);
            r.set(g, g, getWidth() - g, getHeight() - g);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(u == unit ? Kit.SEL_BG : 0xFF141C20);
            cv.drawRoundRect(r, 2 * g, 2 * g, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(u == unit ? 2 * g : g);
            paint.setColor(u == unit ? Kit.LABEL : Kit.STEEL);
            cv.drawRoundRect(r, 2 * g, 2 * g, paint);
            Bitmap b = icon(u);
            if (b == null) return;
            src.set(0, 0, b.getWidth(), b.getHeight());
            float s = Math.min((getWidth() - 6 * g) / b.getWidth(), (getHeight() - 6 * g) / b.getHeight());
            int w = (int) (b.getWidth() * s), h = (int) (b.getHeight() * s);
            dst.set((getWidth() - w) / 2, (getHeight() - h) / 2, (getWidth() + w) / 2, (getHeight() + h) / 2);
            cv.drawBitmap(b, src, dst, pix);
        }
    }
}
