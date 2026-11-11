package org.openbw.android;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.util.HashMap;
import java.util.Map;

/**
 * Keyboard + mouse bindings (Settings > Valdymas). Codes are what the engine sees:
 * SDL scancodes for keys, MOUSE + SDL button (1 left, 2 middle, 3 right, 4 / 5 side)
 * for mouse buttons. The ids and defaults mirror ui/kbm.h; the engine gets the whole
 * table as --key-binds "id=code,...".
 */
final class Keybinds {
    static final int MOUSE = 1000;
    static final String PREF = "keyBinds";

    static final class Action {
        final String id, label;
        final int def;
        final boolean mouseOnly;
        Action(String id, String label, int def, boolean mouseOnly) {
            this.id = id; this.label = label; this.def = def; this.mouseOnly = mouseOnly;
        }
    }

    static final Action[] ACTIONS;
    static {
        java.util.List<Action> a = new java.util.ArrayList<>();
        a.add(new Action("mouse_select", "Žymėjimas", MOUSE + 1, true));
        a.add(new Action("mouse_command", "Komanda", MOUSE + 3, true));
        a.add(new Action("mouse_pan", "Kameros tempimas", MOUSE + 2, true));
        a.add(new Action("cam_up", "Kamera aukštyn", 82, false));
        a.add(new Action("cam_down", "Kamera žemyn", 81, false));
        a.add(new Action("cam_left", "Kamera kairėn", 80, false));
        a.add(new Action("cam_right", "Kamera dešinėn", 79, false));
        int[] card = {20, 26, 8, 4, 22, 7, 29, 27, 6};
        for (int i = 0; i < 9; i++) a.add(new Action("card" + (i + 1), "Komandos langelis " + (i + 1), card[i], false));
        a.add(new Action("cancel", "Atšaukti", 41, false));
        a.add(new Action("menu", "Meniu", 67, false));
        a.add(new Action("chat", "Pokalbis", 40, false));
        a.add(new Action("alert", "Į paskutinį pavojų", 44, false));
        a.add(new Action("idle", "Laisvas darbininkas", 58, false));
        a.add(new Action("army", "Kariuomenė", 59, false));
        a.add(new Action("center", "Centruoti į pažymėtus", 9, false));
        a.add(new Action("zoom_in", "Priartinti", 46, false));
        a.add(new Action("zoom_out", "Atitolinti", 45, false));
        for (int g = 1; g <= 10; g++) {
            int n = g % 10;
            a.add(new Action("group" + n, "Grupė " + n, n == 0 ? 39 : 29 + n, false));
        }
        for (int i = 1; i <= 3; i++) a.add(new Action("camloc" + i, "Kameros vieta " + i, 61 + i, false));
        ACTIONS = a.toArray(new Action[0]);
    }

    private Keybinds() {}

    static Map<String, Integer> load(SharedPreferences p) {
        Map<String, Integer> m = new HashMap<>();
        for (Action a : ACTIONS) m.put(a.id, a.def);
        String s = p.getString(PREF, "");
        for (String item : s.split(",")) {
            int eq = item.indexOf('=');
            if (eq <= 0) continue;
            try { if (m.containsKey(item.substring(0, eq))) m.put(item.substring(0, eq), Integer.parseInt(item.substring(eq + 1))); }
            catch (NumberFormatException ignored) {}
        }
        return m;
    }

    static String arg(SharedPreferences p) {
        Map<String, Integer> m = load(p);
        StringBuilder b = new StringBuilder();
        for (Action a : ACTIONS) {
            if (b.length() > 0) b.append(',');
            b.append(a.id).append('=').append(m.get(a.id));
        }
        return b.toString();
    }

    private static void save(SharedPreferences p, Map<String, Integer> m) {
        StringBuilder b = new StringBuilder();
        for (Action a : ACTIONS) {
            if (b.length() > 0) b.append(',');
            b.append(a.id).append('=').append(m.get(a.id));
        }
        p.edit().putString(PREF, b.toString()).apply();
    }

    // Android key code -> SDL scancode (the table SDL's Android backend uses). 0 = not bindable.
    static int sdlScancode(int k) {
        if (k >= KeyEvent.KEYCODE_A && k <= KeyEvent.KEYCODE_Z) return 4 + (k - KeyEvent.KEYCODE_A);
        if (k >= KeyEvent.KEYCODE_1 && k <= KeyEvent.KEYCODE_9) return 30 + (k - KeyEvent.KEYCODE_1);
        if (k == KeyEvent.KEYCODE_0) return 39;
        if (k >= KeyEvent.KEYCODE_F1 && k <= KeyEvent.KEYCODE_F12) return 58 + (k - KeyEvent.KEYCODE_F1);
        if (k >= KeyEvent.KEYCODE_NUMPAD_1 && k <= KeyEvent.KEYCODE_NUMPAD_9) return 89 + (k - KeyEvent.KEYCODE_NUMPAD_1);
        switch (k) {
            case KeyEvent.KEYCODE_ENTER: return 40;
            case KeyEvent.KEYCODE_ESCAPE: return 41;
            case KeyEvent.KEYCODE_DEL: return 42;
            case KeyEvent.KEYCODE_TAB: return 43;
            case KeyEvent.KEYCODE_SPACE: return 44;
            case KeyEvent.KEYCODE_MINUS: return 45;
            case KeyEvent.KEYCODE_EQUALS: return 46;
            case KeyEvent.KEYCODE_LEFT_BRACKET: return 47;
            case KeyEvent.KEYCODE_RIGHT_BRACKET: return 48;
            case KeyEvent.KEYCODE_BACKSLASH: return 49;
            case KeyEvent.KEYCODE_SEMICOLON: return 51;
            case KeyEvent.KEYCODE_APOSTROPHE: return 52;
            case KeyEvent.KEYCODE_GRAVE: return 53;
            case KeyEvent.KEYCODE_COMMA: return 54;
            case KeyEvent.KEYCODE_PERIOD: return 55;
            case KeyEvent.KEYCODE_SLASH: return 56;
            case KeyEvent.KEYCODE_BREAK: return 72;
            case KeyEvent.KEYCODE_INSERT: return 73;
            case KeyEvent.KEYCODE_MOVE_HOME: return 74;
            case KeyEvent.KEYCODE_PAGE_UP: return 75;
            case KeyEvent.KEYCODE_FORWARD_DEL: return 76;
            case KeyEvent.KEYCODE_MOVE_END: return 77;
            case KeyEvent.KEYCODE_PAGE_DOWN: return 78;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 79;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 80;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 81;
            case KeyEvent.KEYCODE_DPAD_UP: return 82;
            case KeyEvent.KEYCODE_NUMPAD_DIVIDE: return 84;
            case KeyEvent.KEYCODE_NUMPAD_MULTIPLY: return 85;
            case KeyEvent.KEYCODE_NUMPAD_SUBTRACT: return 86;
            case KeyEvent.KEYCODE_NUMPAD_ADD: return 87;
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return 88;
            case KeyEvent.KEYCODE_NUMPAD_0: return 98;
            case KeyEvent.KEYCODE_NUMPAD_DOT: return 99;
            default: return 0;
        }
    }

    // A MotionEvent button -> SDL mouse button (SDL's Android mapping), 0 = none.
    static int sdlMouseButton(int androidButton) {
        switch (androidButton) {
            case MotionEvent.BUTTON_PRIMARY: return 1;
            case MotionEvent.BUTTON_TERTIARY: return 2;
            case MotionEvent.BUTTON_SECONDARY: return 3;
            case MotionEvent.BUTTON_BACK: return 4;
            case MotionEvent.BUTTON_FORWARD: return 5;
            default: return 0;
        }
    }

    static String name(int code) {
        if (code <= 0) return "—";
        if (code > MOUSE) {
            switch (code - MOUSE) {
                case 1: return S.t("Kairysis pelės");
                case 2: return S.t("Vidurinis pelės");
                case 3: return S.t("Dešinysis pelės");
                case 4: return S.t("Pelės atgal");
                case 5: return S.t("Pelės pirmyn");
                default: return S.t("Pelės ") + (code - MOUSE);
            }
        }
        if (code >= 4 && code <= 29) return String.valueOf((char) ('A' + code - 4));
        if (code >= 30 && code <= 38) return String.valueOf((char) ('1' + code - 30));
        if (code == 39) return "0";
        if (code >= 58 && code <= 69) return "F" + (code - 57);
        if (code >= 89 && code <= 97) return "Num " + (code - 88);
        switch (code) {
            case 40: return "Enter";
            case 41: return "Esc";
            case 42: return "Backspace";
            case 43: return "Tab";
            case 44: return S.t("Tarpas");
            case 45: return "-";
            case 46: return "=";
            case 47: return "[";
            case 48: return "]";
            case 49: return "\\";
            case 51: return ";";
            case 52: return "'";
            case 53: return "`";
            case 54: return ",";
            case 55: return ".";
            case 56: return "/";
            case 72: return "Pause";
            case 73: return "Insert";
            case 74: return "Home";
            case 75: return "Page Up";
            case 76: return "Delete";
            case 77: return "End";
            case 78: return "Page Down";
            case 79: return "→";
            case 80: return "←";
            case 81: return "↓";
            case 82: return "↑";
            case 84: return "Num /";
            case 85: return "Num *";
            case 86: return "Num -";
            case 87: return "Num +";
            case 88: return "Num Enter";
            case 98: return "Num 0";
            case 99: return "Num .";
            default: return "#" + code;
        }
    }

    // "Grupė 3" -> S.t("Grupė") + " 3": the numbered labels share one translation.
    static String label(Action a) {
        int sp = a.label.lastIndexOf(' ');
        if (sp > 0 && Character.isDigit(a.label.charAt(sp + 1))) return S.t(a.label.substring(0, sp)) + a.label.substring(sp);
        return S.t(a.label);
    }

    private static boolean fits(Action a, int code) {
        return a.mouseOnly ? code > MOUSE : code > 0;
    }

    /** Puts `code` on `a`. Another action holding it gets a's old code, when that fits it;
     *  otherwise nothing changes and the reason is returned. */
    private static String assign(Map<String, Integer> m, Action a, int code) {
        int old = m.get(a.id);
        if (old == code) return null;
        for (Action o : ACTIONS) {
            if (o == a || m.get(o.id) != code) continue;
            if (!fits(o, old)) return S.t("Jau naudojama: ") + label(o);
            m.put(o.id, old);
        }
        m.put(a.id, code);
        return null;
    }

    private static int dp(Activity act, float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, act.getResources().getDisplayMetrics()));
    }

    private static String section(Action a) {
        if (a.mouseOnly) return "Pelė";
        if (a.id.startsWith("cam_")) return "Kamera";
        if (a.id.startsWith("card")) return "Komandų kortelė";
        if (a.id.startsWith("group")) return "Grupės";
        if (a.id.startsWith("camloc")) return "Kameros vietos";
        return "Žaidimas";
    }

    /** The bindings table for Settings > Valdymas: icon, action, key (tap to rebind), reset. */
    static LinearLayout table(final Activity act, final Kit kit, final SharedPreferences p) {
        final Map<String, Integer> m = load(p);
        final LinearLayout list = new LinearLayout(act);
        list.setOrientation(LinearLayout.VERTICAL);
        final Map<String, TextView> keyViews = new HashMap<>();
        final Map<String, View> resets = new HashMap<>();
        final Runnable refresh = () -> {
            for (Action o : ACTIONS) {
                keyViews.get(o.id).setText(name(m.get(o.id)));
                resets.get(o.id).setVisibility(m.get(o.id) == o.def ? View.INVISIBLE : View.VISIBLE);
            }
        };

        LinearLayout head = new LinearLayout(act);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        head.setPadding(0, dp(act, 6), 0, dp(act, 6));
        TextView hint = kit.text(S.t("Komandų langeliai 1–9 — 3×3 kortelė (1–3 viršuje). Shift + komanda — į eilę; Ctrl + grupė — priskirti, Shift + grupė — pridėti, du kartus — kamera į grupę; Ctrl + kameros vieta — įsiminti."), Kit.TEXT2, 11);
        hint.setPadding(0, 0, dp(act, 12), 0);
        head.addView(hint, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        head.addView(kit.smallButton(S.t("Atstatyti visus"), () -> {
            p.edit().remove(PREF).apply();
            m.clear();
            m.putAll(load(p));
            refresh.run();
            Toast.makeText(act, S.t("Klavišai atstatyti."), Toast.LENGTH_SHORT).show();
        }), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, kit.hdp(40)));
        list.addView(head);

        // wide screens: mouse, camera, game and camera locations on the left, the card and groups on the right
        final boolean two = act.getResources().getConfiguration().screenWidthDp >= 700;
        LinearLayout cols = new LinearLayout(act);
        cols.setOrientation(two ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        cols.setBaselineAligned(false);
        LinearLayout left = new LinearLayout(act), right = new LinearLayout(act);
        left.setOrientation(LinearLayout.VERTICAL);
        right.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams l1 = two ? new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        if (two) l1.rightMargin = dp(act, 28);
        cols.addView(left, l1);
        cols.addView(right, two ? new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        list.addView(cols);

        String sec = null;
        LinearLayout col = left;
        for (final Action a : ACTIONS) {
            if (!section(a).equals(sec)) {
                sec = section(a);
                col = sec.equals("Komandų kortelė") || sec.equals("Grupės") ? right : left;
                col.addView(kit.label(S.t(sec)));
            }
            LinearLayout row = new LinearLayout(act);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(act, 8), dp(act, 3), 0, dp(act, 3));
            row.setBackground(kit.box(0x00000000, 0xFF1C2428));
            row.addView(new ActionIcon(act, a.id), new LinearLayout.LayoutParams(dp(act, 24), dp(act, 24)));
            TextView label = kit.text(label(a), Kit.TEXT, 13);
            label.setPadding(dp(act, 12), 0, dp(act, 8), 0);
            row.addView(label, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            final TextView key = kit.smallButton(name(m.get(a.id)), () -> {});
            key.setTextColor(Kit.ACCENT);
            key.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            key.setMinWidth(dp(act, 120));
            key.setOnClickListener(v -> {
                MenuAudio.play(MenuAudio.CLICK);
                capture(act, kit, a, code -> {
                    String err = assign(m, a, code);
                    if (err != null) { Toast.makeText(act, err, Toast.LENGTH_SHORT).show(); return; }
                    save(p, m);
                    refresh.run();
                });
            });
            keyViews.put(a.id, key);
            row.addView(key, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, kit.hdp(36)));
            TextView reset = kit.smallButton("↺", () -> {
                String err = assign(m, a, a.def);
                if (err != null) { Toast.makeText(act, err, Toast.LENGTH_SHORT).show(); return; }
                save(p, m);
                refresh.run();
            });
            reset.setContentDescription(S.t("Atstatyti numatytą"));
            LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(kit.hdp(36), kit.hdp(36));
            rlp.leftMargin = dp(act, 6);
            resets.put(a.id, reset);
            row.addView(reset, rlp);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(act, 2);
            col.addView(row, lp);
        }
        refresh.run();
        return list;
    }

    interface CodeSink { void put(int code); }

    private static void capture(Activity act, Kit kit, Action a, CodeSink sink) {
        final android.app.Dialog[] dlg = new android.app.Dialog[1];
        final View pad = new View(act) {
            @Override public boolean onTouchEvent(MotionEvent ev) { return mouse(ev); }
            @Override public boolean onGenericMotionEvent(MotionEvent ev) { return mouse(ev); }
            private boolean mouse(MotionEvent ev) {
                if (!ev.isFromSource(InputDevice.SOURCE_MOUSE)) return false;
                int b = 0;
                if (ev.getActionMasked() == MotionEvent.ACTION_BUTTON_PRESS) b = sdlMouseButton(ev.getActionButton());
                else if (ev.getActionMasked() == MotionEvent.ACTION_DOWN) b = sdlMouseButton(Integer.lowestOneBit(ev.getButtonState()));
                if (b == 0) return ev.getActionMasked() != MotionEvent.ACTION_HOVER_MOVE;
                if (dlg[0] != null) dlg[0].dismiss();
                sink.put(MOUSE + b);
                return true;
            }
        };
        pad.setBackground(kit.box(0x22FF8A1E, 0xFF7A4A1E));
        LinearLayout box = new LinearLayout(act);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(act, 20), dp(act, 12), dp(act, 20), dp(act, 16));
        box.setBackground(kit.box(Kit.PANEL, Kit.LINE));
        box.addView(kit.label(label(a)));
        TextView t = kit.text(a.mouseOnly ? S.t("Spustelėkite pelės mygtuku žemiau esančiame lauke.")
                : S.t("Paspauskite klavišą arba spustelėkite pelės mygtuku žemiau esančiame lauke."), Kit.TEXT, 13);
        t.setPadding(0, 0, 0, dp(act, 10));
        box.addView(t);
        box.addView(pad, new LinearLayout.LayoutParams(dp(act, 360), dp(act, 130)));
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.gravity = Gravity.END;
        blp.topMargin = dp(act, 12);
        box.addView(kit.button(S.t("Atšaukti"), 2, () -> { if (dlg[0] != null) dlg[0].dismiss(); }), blp);
        dlg[0] = kit.modal(box, false);
        dlg[0].setOnKeyListener((d, keyCode, ev) -> {
            if (ev.isFromSource(InputDevice.SOURCE_MOUSE)) return true;   // side buttons: read as mouse motion
            if (ev.getAction() != KeyEvent.ACTION_DOWN) return false;
            if (keyCode == KeyEvent.KEYCODE_BACK) return false;
            int code = sdlScancode(keyCode);
            if (code == 0 || a.mouseOnly) {
                Toast.makeText(act, a.mouseOnly ? S.t("Čia tinka tik pelės mygtukas.") : S.t("Šio klavišo priskirti negalima."), Toast.LENGTH_SHORT).show();
                return true;
            }
            d.dismiss();
            sink.put(code);
            return true;
        });
        dlg[0].show();
        pad.setFocusable(true);
        pad.requestFocus();
    }

    /** A small stroke glyph per action, in the RailIcon manner (24 x 24 box). */
    static final class ActionIcon extends View {
        private final String id;
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final android.graphics.Path path = new android.graphics.Path();
        private final RectF r = new RectF(), arc = new RectF(5, 3, 19, 17);
        ActionIcon(Activity act, String id) {
            super(act);
            this.id = id;
            p.setStrokeCap(Paint.Cap.ROUND);
            p.setStrokeJoin(Paint.Join.ROUND);
        }
        private void arrow(Canvas c, float dx, float dy) {
            float hx = 12 + 7 * dx, hy = 12 + 7 * dy;
            c.drawLine(12 - 7 * dx, 12 - 7 * dy, hx, hy, p);
            c.drawLine(hx, hy, hx - 4 * dx - 4 * dy, hy - 4 * dy - 4 * dx, p);
            c.drawLine(hx, hy, hx - 4 * dx + 4 * dy, hy - 4 * dy + 4 * dx, p);
        }
        private void mouse(Canvas c, int btn) {
            r.set(6, 3, 18, 21);
            c.drawRoundRect(r, 6, 6, p);
            c.drawLine(12, 3, 12, 10, p);
            c.drawLine(6, 10, 18, 10, p);
            p.setStyle(Paint.Style.FILL);
            p.setColor(Kit.ACCENT);
            if (btn == 1) r.set(7.5f, 4.5f, 11, 9);
            else if (btn == 3) r.set(13, 4.5f, 16.5f, 9);
            else r.set(11, 5, 13, 9);
            c.drawRect(r, p);
        }
        private void fillText(Canvas c, String s, float x, float y, float size) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(Kit.ACCENT);
            p.setTextSize(size);
            p.setTextAlign(Paint.Align.CENTER);
            p.setFakeBoldText(true);
            c.drawText(s, x, y, p);
        }
        @Override protected void onDraw(Canvas c) {
            float s = Math.min(getWidth(), getHeight()) / 24f;
            c.save();
            c.translate((getWidth() - 24 * s) / 2f, (getHeight() - 24 * s) / 2f);
            c.scale(s, s);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(1.8f);
            p.setColor(Kit.TEXT2);
            path.reset();
            if (id.equals("mouse_select")) mouse(c, 1);
            else if (id.equals("mouse_command")) mouse(c, 3);
            else if (id.equals("mouse_pan")) mouse(c, 2);
            else if (id.equals("cam_up")) arrow(c, 0, -1);
            else if (id.equals("cam_down")) arrow(c, 0, 1);
            else if (id.equals("cam_left")) arrow(c, -1, 0);
            else if (id.equals("cam_right")) arrow(c, 1, 0);
            else if (id.startsWith("card")) {
                int n = Integer.parseInt(id.substring(4)) - 1;
                p.setStrokeWidth(1f);
                for (int i = 0; i < 9; i++) {
                    float x = 3 + (i % 3) * 6.5f, y = 3 + (i / 3) * 6.5f;
                    r.set(x, y, x + 5, y + 5);
                    p.setStyle(i == n ? Paint.Style.FILL : Paint.Style.STROKE);
                    p.setColor(i == n ? Kit.ACCENT : Kit.TEXT2);
                    c.drawRect(r, p);
                }
            } else if (id.startsWith("group")) {
                r.set(3, 4, 21, 20);
                c.drawRoundRect(r, 2, 2, p);
                fillText(c, id.substring(5), 12, 16, 11);
            } else if (id.startsWith("camloc")) {
                path.moveTo(12, 22);
                path.lineTo(5.6f, 13);
                path.arcTo(arc, 155, 230);
                path.close();
                c.drawPath(path, p);
                fillText(c, id.substring(6), 12, 13.5f, 9);
            } else switch (id) {
                case "cancel": c.drawLine(6, 6, 18, 18, p); c.drawLine(18, 6, 6, 18, p); break;
                case "menu": c.drawLine(4, 7, 20, 7, p); c.drawLine(4, 12, 20, 12, p); c.drawLine(4, 17, 20, 17, p); break;
                case "chat":
                    r.set(3, 4, 21, 16);
                    c.drawRoundRect(r, 3, 3, p);
                    c.drawLine(8, 16, 7, 20, p);
                    c.drawLine(7, 20, 12, 16, p);
                    break;
                case "alert":
                    path.moveTo(12, 3); path.lineTo(21, 20); path.lineTo(3, 20); path.close();
                    c.drawPath(path, p);
                    c.drawLine(12, 9, 12, 14, p);
                    c.drawPoint(12, 17, p);
                    break;
                case "idle":
                    c.drawCircle(11, 8, 3.5f, p);
                    r.set(4, 14, 18, 28);
                    c.drawArc(r, 180, 180, false, p);
                    fillText(c, "z", 19.5f, 7, 8);
                    break;
                case "army":
                    c.drawLine(4, 4, 20, 20, p); c.drawLine(20, 4, 4, 20, p);
                    c.drawLine(4, 15, 9, 20, p); c.drawLine(15, 20, 20, 15, p);
                    break;
                case "center":
                    c.drawCircle(12, 12, 6.5f, p);
                    c.drawLine(12, 2, 12, 7, p); c.drawLine(12, 17, 12, 22, p);
                    c.drawLine(2, 12, 7, 12, p); c.drawLine(17, 12, 22, 12, p);
                    break;
                case "zoom_in": case "zoom_out":
                    c.drawCircle(10, 10, 6.5f, p);
                    c.drawLine(15, 15, 21, 21, p);
                    c.drawLine(7, 10, 13, 10, p);
                    if (id.equals("zoom_in")) c.drawLine(10, 7, 10, 13, p);
                    break;
                default: break;
            }
            c.restore();
        }
    }

    /** The keyboard + mouse flag: a small orange keyboard. Hover or a long press tells what it means. */
    static final class FlagView extends View {
        private final Paint body = new Paint(Paint.ANTI_ALIAS_FLAG), key = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF r = new RectF();
        private final Activity act;
        private final Runnable hide = this::hideTip;
        private android.widget.PopupWindow tip;
        FlagView(Activity act) {
            super(act);
            this.act = act;
            body.setColor(0xFFFF8A1E);
            key.setColor(0xFF3A1C04);
            setContentDescription(S.t("Žaidžia klaviatūra ir pele"));
            int pad = dp(act, 6);
            setPadding(pad, pad, pad, pad);
            setOnHoverListener((v, e) -> {
                if (e.getActionMasked() == MotionEvent.ACTION_HOVER_ENTER) showTip(0);
                else if (e.getActionMasked() == MotionEvent.ACTION_HOVER_EXIT) hideTip();
                return false;
            });
            setOnLongClickListener(v -> { showTip(2500); return true; });
            setOnClickListener(v -> { if (tip != null) hideTip(); else showTip(2500); });
        }
        private void showTip(long autoHideMs) {
            hideTip();
            Kit kit = new Kit(act);
            TextView t = kit.text(S.t("Žaidžia klaviatūra ir pele"), Kit.TEXT, 12);
            t.setPadding(dp(act, 10), dp(act, 6), dp(act, 10), dp(act, 6));
            t.setBackground(kit.box(Kit.PANEL, 0xFFFF8A1E));
            t.measure(MeasureSpec.UNSPECIFIED, MeasureSpec.UNSPECIFIED);
            tip = new android.widget.PopupWindow(t, ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, false);
            tip.setTouchable(false);
            tip.showAsDropDown(this, (getWidth() - t.getMeasuredWidth()) / 2, -getHeight() - t.getMeasuredHeight() - dp(act, 2));
            if (autoHideMs > 0) postDelayed(hide, autoHideMs);
        }
        private void hideTip() {
            removeCallbacks(hide);
            if (tip != null) { tip.dismiss(); tip = null; }
        }
        @Override protected void onDetachedFromWindow() { hideTip(); super.onDetachedFromWindow(); }
        @Override protected void onMeasure(int w, int h) {
            int hh = Math.round(getResources().getDisplayMetrics().density * 12);
            setMeasuredDimension(hh * 8 / 5 + getPaddingLeft() + getPaddingRight(), hh + getPaddingTop() + getPaddingBottom());
        }
        @Override protected void onDraw(Canvas c) {
            c.save();
            c.translate(getPaddingLeft(), getPaddingTop());
            float w = getWidth() - getPaddingLeft() - getPaddingRight(), h = getHeight() - getPaddingTop() - getPaddingBottom(), u = h / 7f;
            r.set(0, 0, w, h);
            c.drawRoundRect(r, u, u, body);
            float kx0 = 2 * u, kw = (w - 4 * u) / 5f;
            for (int row = 0; row < 2; row++) {
                float ky = u * (1 + 2 * row);
                for (int k = 0; k < 5; k++) c.drawRect(kx0 + k * kw, ky, kx0 + k * kw + kw - u * 0.6f, ky + u, key);
            }
            c.drawRect(w / 4, 5 * u, w - w / 4, 6 * u, key);
            c.restore();
        }
    }

    static boolean kbm(SharedPreferences p) { return "kbm".equals(p.getString("inputMode", "touch")); }
}
