package org.openbw.android;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The launcher's component kit: colour
 * tokens and the few view builders every redesigned screen shares. Everything is
 * drawn by code (GradientDrawable) -- the APK ships no game art.
 */
class Kit {
    // design tokens (LAUNCHER-REDESIGN 11)
    static final int TEXT = 0xFFCFE6CC, TEXT2 = 0xFF7F9486, LABEL = 0xFF4FD46A, ACCENT = 0xFFF2C53D,
            DANGER = 0xFFE0564E, PANEL = 0xDB05080A, LINE = 0xFF5A1717, STEEL = 0xFF5F6B72,
            TRACK = 0xFF1A2226, SEL_BG = 0xFF123A1E;

    final Activity a;
    final float density;
    /** Short screen (phones in landscape, < 480 dp tall): lower controls, tighter padding.
     *  Never true on a tablet, whose layout stays as designed. */
    final boolean compact;

    Kit(Activity a) {
        this.a = a;
        density = a.getResources().getDisplayMetrics().density;
        compact = isCompact(a);
    }

    @SuppressWarnings("deprecation")
    static final int IMMERSIVE_FLAGS = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN;

    /** Game controller play: on in every build since v1.1.0. */
    static boolean gamepadAllowed(android.content.Context c) {
        return true;
    }

    static boolean isCompact(android.content.Context c) {
        return c.getResources().getConfiguration().screenHeightDp < 480;
    }

    int dp(float v) { return Math.round(v * density); }

    /** Control height: the tablet value, or its lower phone counterpart. */
    int hdp(int tabletDp) {
        if (!compact) return dp(tabletDp);
        int v = tabletDp >= 56 ? 40 : tabletDp >= 52 ? 36 : tabletDp >= 48 ? 36 : tabletDp >= 46 ? 38
                : tabletDp >= 44 ? 34 : tabletDp >= 40 ? 32 : tabletDp;
        return dp(v);
    }

    /**
     * Never break a word in the middle: if the longest word of the label does not fit the
     * view's width, shrink the text (down to 9 sp); if it still does not fit, ellipsize.
     * Acts only on overflow, so labels that already fit are left exactly as they were.
     */
    void fitWords(final TextView t) {
        final float base = t.getTextSize();
        t.addOnLayoutChangeListener((v, l, top, r, b, ol, ot, or, ob) -> {
            int w = (r - l) - t.getPaddingLeft() - t.getPaddingRight();
            if (w <= 0 || (r - l) == (or - ol) && t.getTag(R_FIT) != null) return;
            t.setTag(R_FIT, Boolean.TRUE);
            CharSequence cs = t.getText();
            if (cs == null) return;
            String[] words = cs.toString().split("[ \\t\\n]+");
            android.text.TextPaint p = new android.text.TextPaint(t.getPaint());
            float size = base, min = 9f * a.getResources().getDisplayMetrics().scaledDensity;
            while (true) {
                p.setTextSize(size);
                float widest = 0;
                for (String wd : words) widest = Math.max(widest, p.measureText(wd));
                if (widest <= w || size <= min) break;
                size = Math.max(min, size - density * 0.5f);
            }
            final float fs = size;
            final boolean over = words.length > 0 && p.measureText(longest(words)) > w;
            t.post(() -> {
                if (Math.abs(t.getTextSize() - fs) > 0.1f) t.setTextSize(TypedValue.COMPLEX_UNIT_PX, fs);
                if (over) t.setEllipsize(TextUtils.TruncateAt.END);
            });
        });
    }
    private static final int R_FIT = 0x7f0f1161;
    private static String longest(String[] w) {
        String best = "";
        for (String s : w) if (s.length() > best.length()) best = s;
        return best;
    }

    // ---- small building blocks ---------------------------------------------------
    TextView text(String s, int color, float sp) {
        TextView t = new TextView(a);
        t.setText(s);
        t.setTextColor(color);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        return t;
    }

    TextView label(String s) {
        TextView t = text(s.toUpperCase(java.util.Locale.ROOT), LABEL, 12);
        t.setLetterSpacing(0.12f);
        t.setPadding(0, dp(compact ? 6 : 10), 0, dp(compact ? 3 : 6));
        return t;
    }

    GradientDrawable box(int fill, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setStroke(Math.max(1, dp(1)), stroke);
        return g;
    }

    /** kind: 0 secondary (steel), 1 primary (green), 2 quiet (outline only). */
    TextView button(String s, int kind, Runnable run) {
        TextView b = text(s.toUpperCase(java.util.Locale.ROOT), kind == 1 ? Color.WHITE : TEXT,
                kind == 1 ? (compact ? 14 : 15) : (compact ? 12 : 13));
        b.setLetterSpacing(compact ? 0.06f : 0.1f);
        b.setGravity(Gravity.CENTER);
        b.setTypeface(Typeface.DEFAULT, kind == 1 ? Typeface.BOLD : Typeface.NORMAL);
        b.setPadding(dp(compact ? 12 : 16), 0, dp(compact ? 12 : 16), 0);
        b.setMinHeight(hdp(48));
        b.setMinimumHeight(hdp(48));
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                kind == 1 ? new int[]{0xFF1F6B34, 0xFF114420} : kind == 0 ? new int[]{0xFF2A3338, 0xFF161C20} : new int[]{0, 0});
        g.setStroke(Math.max(1, dp(1)), kind == 1 ? LABEL : kind == 0 ? STEEL : LABEL);
        b.setBackground(g);
        b.setClickable(true);
        b.setOnClickListener(v -> { MenuAudio.play(kind == 1 ? MenuAudio.LOCK : MenuAudio.CLICK); run.run(); });
        return b;
    }

    /** A two-option segmented control; `sink` gets the picked index. */
    LinearLayout segmented(String[] labels, int sel, final IntSink sink) { return segmented(labels, sel, sink, null); }

    /** As above; a cell with enabled[i] == false is greyed out and cannot be picked. */
    LinearLayout segmented(String[] labels, int sel, final IntSink sink, final boolean[] enabled) {
        final LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        final TextView[] cells = new TextView[labels.length];
        for (int i = 0; i != labels.length; ++i) {
            final int idx = i;
            TextView t = text(labels[i], TEXT, labels.length > 4 ? 12 : 13);
            t.setGravity(Gravity.CENTER);
            t.setMaxLines(2);
            t.setPadding(dp(2), 0, dp(2), 0);
            t.setMinHeight(hdp(44));
            t.setMinimumHeight(hdp(44));
            fitWords(t);
            t.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));
            final boolean on = enabled == null || enabled[i];
            if (on) {
                t.setOnClickListener(v -> {
                    MenuAudio.play(MenuAudio.CLICK);
                    for (int k = 0; k != cells.length; ++k) if (enabled == null || enabled[k]) styleSeg(cells[k], k == idx);
                    sink.set(idx);
                });
            } else {
                t.setOnClickListener(v -> MenuAudio.play(MenuAudio.LOCK));
            }
            cells[i] = t;
            styleSeg(t, i == sel);
            if (!on) {
                t.setBackground(box(0x00000000, 0x55FFFFFF & STEEL));
                t.setTextColor(0x66FFFFFF & TEXT2);
            }
            row.addView(t);
        }
        return row;
    }
    /** A tooltip on `anchor`: shown on long press or mouse hover, hidden on release / exit or after a while. */
    void attachTip(final View anchor, final String text) {
        final android.widget.PopupWindow[] tip = {null};
        final Runnable hide = () -> { if (tip[0] != null) { tip[0].dismiss(); tip[0] = null; } };
        final Runnable show = () -> {
            hide.run();
            TextView t = text(text, TEXT, 12);
            int pad = dp(10);
            t.setPadding(pad, dp(7), pad, dp(7));
            t.setBackground(box(PANEL | 0xFF000000, LABEL));
            int maxW = Math.min(dp(520), a.getResources().getDisplayMetrics().widthPixels - dp(32));
            t.setMaxWidth(maxW);
            t.measure(View.MeasureSpec.makeMeasureSpec(maxW, View.MeasureSpec.AT_MOST), View.MeasureSpec.UNSPECIFIED);
            tip[0] = new android.widget.PopupWindow(t, ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, false);
            tip[0].setTouchable(false);
            int[] at = new int[2];
            anchor.getLocationInWindow(at);
            int above = at[1] - t.getMeasuredHeight() - dp(4);
            tip[0].showAsDropDown(anchor, dp(8), above >= dp(8) ? -anchor.getHeight() - t.getMeasuredHeight() - dp(4) : dp(4));
            anchor.removeCallbacks(hide);
            anchor.postDelayed(hide, 6000);
        };
        anchor.setOnLongClickListener(v -> { show.run(); return true; });
        anchor.setOnHoverListener((v, e) -> {
            if (e.getActionMasked() == android.view.MotionEvent.ACTION_HOVER_ENTER) anchor.postDelayed(show, 450);
            else if (e.getActionMasked() == android.view.MotionEvent.ACTION_HOVER_EXIT) { anchor.removeCallbacks(show); hide.run(); }
            return false;
        });
        anchor.addOnAttachStateChangeListener(new View.OnAttachStateChangeListener() {
            @Override public void onViewAttachedToWindow(View v) {}
            @Override public void onViewDetachedFromWindow(View v) { anchor.removeCallbacks(show); hide.run(); }
        });
    }

    void styleSeg(TextView t, boolean on) {
        t.setBackground(box(on ? SEL_BG : 0x00000000, on ? LABEL : STEEL));
        t.setTextColor(on ? Color.WHITE : TEXT2);
    }
    interface IntSink { void set(int v); }
    interface BoolSink { void set(boolean v); }

    /** A toggle chip: an outlined label, green when on. */
    TextView chip(String s, boolean on, final BoolSink sink) {
        final TextView t = text(s, TEXT, 12);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(10), 0, dp(10), 0);
        t.setMinHeight(hdp(40));
        t.setMinimumHeight(hdp(40));
        t.setTag(on);
        styleChip(t, on);
        t.setOnClickListener(v -> {
            MenuAudio.play(MenuAudio.CLICK);
            boolean now = !((Boolean) t.getTag());
            t.setTag(now);
            styleChip(t, now);
            sink.set(now);
        });
        return t;
    }
    void styleChip(TextView t, boolean on) {
        t.setBackground(box(on ? SEL_BG : 0x00000000, on ? LABEL : STEEL));
        t.setTextColor(on ? Color.WHITE : TEXT2);
    }

    /** An on/off switch (size it 52 x 28 dp): green when on. */
    View toggle(boolean on, final BoolSink sink) {
        final FrameLayout f = new FrameLayout(a);
        final View knob = new View(a);
        f.addView(knob, new FrameLayout.LayoutParams(dp(20), dp(20)));
        final boolean[] st = {on};
        final Runnable paint = () -> {
            f.setBackground(box(st[0] ? 0xFF1F6B34 : 0xFF161C20, st[0] ? LABEL : STEEL));
            knob.setBackgroundColor(st[0] ? 0xFFB8F0C4 : 0xFF7F8A90);
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) knob.getLayoutParams();
            lp.gravity = Gravity.CENTER_VERTICAL | (st[0] ? Gravity.END : Gravity.START);
            lp.leftMargin = dp(4);
            lp.rightMargin = dp(4);
            knob.setLayoutParams(lp);
        };
        paint.run();
        f.setOnClickListener(v -> { MenuAudio.play(MenuAudio.CLICK); st[0] = !st[0]; paint.run(); sink.set(st[0]); });
        return f;
    }

    /** A compact secondary button (list rows, pickers): 40 dp, 12 sp. */
    TextView smallButton(String s, Runnable run) {
        TextView b = text(s, TEXT, 12);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(10), 0, dp(10), 0);
        b.setMinHeight(hdp(40));
        b.setMinimumHeight(hdp(40));
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[]{0xFF2A3338, 0xFF161C20});
        g.setStroke(Math.max(1, dp(1)), STEEL);
        b.setBackground(g);
        b.setOnClickListener(v -> { MenuAudio.play(MenuAudio.CLICK); run.run(); });
        return b;
    }

    /** Children flow left to right and wrap onto new lines (chips). */
    static final class Flow extends ViewGroup {
        private final int gap;
        Flow(android.content.Context c, int gapPx) { super(c); gap = gapPx; }
        @Override protected void onMeasure(int wSpec, int hSpec) {
            int maxW = MeasureSpec.getMode(wSpec) == MeasureSpec.UNSPECIFIED ? Integer.MAX_VALUE : MeasureSpec.getSize(wSpec);
            int x = 0, y = 0, rowH = 0, widest = 0;
            for (int i = 0; i != getChildCount(); ++i) {
                View c = getChildAt(i);
                if (c.getVisibility() == GONE) continue;
                // measureChild honours the child's own LayoutParams (fixed widths / heights)
                measureChild(c, MeasureSpec.makeMeasureSpec(maxW == Integer.MAX_VALUE ? 0 : maxW, maxW == Integer.MAX_VALUE ? MeasureSpec.UNSPECIFIED : MeasureSpec.AT_MOST),
                        MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED));
                if (x > 0 && x + c.getMeasuredWidth() > maxW) { x = 0; y += rowH + gap; rowH = 0; }
                x += c.getMeasuredWidth() + gap;
                rowH = Math.max(rowH, c.getMeasuredHeight());
                widest = Math.max(widest, x - gap);
            }
            setMeasuredDimension(resolveSize(widest, wSpec), resolveSize(y + rowH, hSpec));
        }
        @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
            int maxW = r - l, x = 0, y = 0, rowH = 0;
            for (int i = 0; i != getChildCount(); ++i) {
                View c = getChildAt(i);
                if (c.getVisibility() == GONE) continue;
                if (x > 0 && x + c.getMeasuredWidth() > maxW) { x = 0; y += rowH + gap; rowH = 0; }
                c.layout(x, y, x + c.getMeasuredWidth(), y + c.getMeasuredHeight());
                x += c.getMeasuredWidth() + gap;
                rowH = Math.max(rowH, c.getMeasuredHeight());
            }
        }
    }

    View bar(float frac, int color) {
        LinearLayout track = new LinearLayout(a);
        track.setBackgroundColor(TRACK);
        frac = Math.max(0f, Math.min(1f, frac));
        View fill = new View(a);
        fill.setBackgroundColor(color);
        track.addView(fill, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, frac));
        track.addView(new View(a), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f - frac));
        return track;
    }

    LinearLayout barRow(String name, float frac, int color, String value, float sp, int nameW) {
        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(4), 0, dp(4));
        TextView n = text(name, TEXT, sp);
        n.setSingleLine(true);
        n.setEllipsize(TextUtils.TruncateAt.END);
        row.addView(n, new LinearLayout.LayoutParams(nameW, ViewGroup.LayoutParams.WRAP_CONTENT));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(5), 1f);
        lp.leftMargin = dp(10);
        lp.rightMargin = dp(10);
        row.addView(bar(frac, color), lp);
        TextView v = text(value, TEXT2, sp);
        v.setGravity(Gravity.END);
        row.addView(v, new LinearLayout.LayoutParams(dp(64), ViewGroup.LayoutParams.WRAP_CONTENT));
        return row;
    }

    Dialog modal(View content, boolean fullScreen) { return modal(content, fullScreen, true); }

    Dialog modal(View content, boolean fullScreen, final boolean cancelable) {
        // panels swish in and out, like the original menu's
        final Dialog d = new Dialog(a, android.R.style.Theme_Translucent_NoTitleBar) {
            @Override public void show() { MenuAudio.play(MenuAudio.OPEN); super.show(); }
            @Override public void dismiss() { if (isShowing()) MenuAudio.play(MenuAudio.CLOSE); super.dismiss(); }
        };
        FrameLayout scrim = new FrameLayout(a);
        scrim.setBackgroundColor(fullScreen ? 0xF205080A : 0xB8000000);
        scrim.setOnClickListener(v -> { if (!fullScreen && cancelable) d.dismiss(); });
        content.setClickable(true);   // taps on the panel must not fall through to the scrim
        int m = fullScreen ? dp(12) : 0;
        FrameLayout.LayoutParams lp = fullScreen
                ? new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
                : new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        lp.setMargins(m, m, m, m);
        scrim.addView(content, lp);
        // With a game controller connected the focused scrim got the system focus highlight:
        // an 8% white veil over the whole popup.
        if (android.os.Build.VERSION.SDK_INT >= 26) {
            scrim.setDefaultFocusHighlightEnabled(false);
            content.setDefaultFocusHighlightEnabled(false);
        }
        d.setContentView(scrim);
        Window w = d.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
            w.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            if (compact) w.getDecorView().setSystemUiVisibility(IMMERSIVE_FLAGS);   // keep the bars hidden on phones
        }
        return d;
    }

    /**
     * v1.0.26: the launcher's toast -- a Kit pill over the bottom of the activity
     * instead of the system's grey one. Returns a canceller. Without a live activity
     * (e.g. an application context) it falls back to the system toast.
     */
    static Runnable toast(android.content.Context c, CharSequence msg, int duration) {
        if (!(c instanceof Activity) || ((Activity) c).isFinishing()) {
            final android.widget.Toast t = android.widget.Toast.makeText(c, msg, duration);
            t.show();
            return t::cancel;
        }
        final Activity act = (Activity) c;
        final Kit k = new Kit(act);
        final ViewGroup root = (ViewGroup) act.getWindow().getDecorView();
        final TextView t = k.text(String.valueOf(msg), TEXT, k.compact ? 13 : 15);
        t.setBackground(k.box(0xF20B1114, LABEL));
        t.setPadding(k.dp(18), k.dp(10), k.dp(18), k.dp(10));
        t.setGravity(Gravity.CENTER);
        t.setMaxWidth(act.getResources().getDisplayMetrics().widthPixels - k.dp(48));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        lp.bottomMargin = k.dp(k.compact ? 24 : 48);
        t.setAlpha(0f);
        root.addView(t, lp);
        t.animate().alpha(1f).setDuration(150).start();
        final Runnable remove = () -> t.animate().alpha(0f).setDuration(200).withEndAction(() -> root.removeView(t)).start();
        t.postDelayed(remove, duration == android.widget.Toast.LENGTH_LONG ? 3500 : 2000);
        return () -> { t.removeCallbacks(remove); remove.run(); };
    }

    /** A download / copy panel: an optional subtitle, the step and a large percent, the
     *  Meter, then the amount and the speed with the time left. Built on the UI thread;
     *  update() and busy() are UI-thread calls too. */
    ProgressPanel progressPanel(String subtitle) { return new ProgressPanel(this, subtitle); }

    static final class ProgressPanel {
        final LinearLayout view;
        final TextView sub, status, pct, amount, rate;
        final Meter meter;
        private long lastT, lastB;
        private double bps;

        ProgressPanel(Kit k, String subtitle) {
            view = new LinearLayout(k.a);
            view.setOrientation(LinearLayout.VERTICAL);
            sub = k.text(subtitle == null ? "" : subtitle, TEXT2, k.compact ? 12 : 13);
            sub.setSingleLine(true);
            sub.setEllipsize(TextUtils.TruncateAt.MIDDLE);
            if (subtitle == null) sub.setVisibility(View.GONE);
            view.addView(sub);
            LinearLayout top = new LinearLayout(k.a);
            top.setOrientation(LinearLayout.HORIZONTAL);
            top.setGravity(Gravity.BOTTOM);
            status = k.text("", TEXT, k.compact ? 14 : 15);
            status.setMinLines(1);
            top.addView(status, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            pct = k.text("", ACCENT, k.compact ? 22 : 28);
            pct.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
            pct.setGravity(Gravity.END);
            top.addView(pct, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            tlp.topMargin = k.dp(k.compact ? 4 : 8);
            view.addView(top, tlp);
            meter = new Meter(k.a);
            LinearLayout.LayoutParams mlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, k.dp(k.compact ? 12 : 16));
            mlp.topMargin = k.dp(6);
            view.addView(meter, mlp);
            LinearLayout bottom = new LinearLayout(k.a);
            bottom.setOrientation(LinearLayout.HORIZONTAL);
            amount = k.text("", TEXT2, k.compact ? 12 : 13);
            bottom.addView(amount, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            rate = k.text("", TEXT2, k.compact ? 12 : 13);
            rate.setGravity(Gravity.END);
            bottom.addView(rate, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            blp.topMargin = k.dp(6);
            view.addView(bottom, blp);
        }

        /** got of total bytes (total <= 0: length unknown). */
        void update(long got, long total) {
            long now = android.os.SystemClock.uptimeMillis();
            if (lastT == 0) { lastT = now; lastB = got; }
            else if (now - lastT >= 500) {
                double inst = (got - lastB) * 1000.0 / (now - lastT);
                bps = bps == 0 ? inst : bps * 0.7 + inst * 0.3;
                lastT = now;
                lastB = got;
                String r = bps > 0 ? mb((long) bps) + "/s" : "";
                if (bps > 0 && total > got) {
                    long s = (long) Math.ceil((total - got) / bps);
                    r += "   " + S.t("liko ") + (s >= 3600 ? s / 3600 + ":" + two(s / 60 % 60) : s / 60 + "") + ":" + two(s % 60);
                }
                rate.setText(r);
            }
            if (total > 0) {
                meter.setFraction(got / (float) total);
                pct.setText((int) Math.min(100, got * 100 / total) + "%");
                amount.setText(mb(got) + " / " + mb(total));
            } else {
                meter.setIndeterminate(true);
                pct.setText("");
                amount.setText(mb(got));
            }
        }

        /** A step without a byte count: the sweeping meter and no speed. */
        void busy(String s) {
            status.setText(s);
            meter.setIndeterminate(true);
            rate.setText("");
        }

        static String mb(long bytes) { return String.format(java.util.Locale.ROOT, "%.1f MB", bytes / 1048576.0); }
        private static String two(long v) { return v < 10 ? "0" + v : "" + v; }
    }

    /**
     * The launcher's progress bar: a dark track in a steel frame, a green fill split into
     * cells like the game's own meters, and a sweeping band while the length is unknown.
     */
    static final class Meter extends View {
        private final android.graphics.Paint paint = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        private final float d;
        private float frac;
        private boolean indeterminate;
        private long sweepStart;

        Meter(Activity a) {
            super(a);
            d = a.getResources().getDisplayMetrics().density;
        }

        void setFraction(float f) {
            f = Math.max(0f, Math.min(1f, f));
            if (f == frac && !indeterminate) return;
            frac = f;
            indeterminate = false;
            invalidate();
        }

        void setIndeterminate(boolean on) {
            if (on == indeterminate) return;
            indeterminate = on;
            sweepStart = android.os.SystemClock.uptimeMillis();
            invalidate();
        }

        @Override protected void onMeasure(int w, int h) {
            setMeasuredDimension(getDefaultSize(Math.round(160 * d), w), resolveSize(Math.round(14 * d), h));
        }

        @Override protected void onDraw(android.graphics.Canvas c) {
            final float w = getWidth(), h = getHeight(), line = Math.max(1f, d);
            paint.setStyle(android.graphics.Paint.Style.FILL);
            paint.setShader(null);
            paint.setColor(TRACK);
            c.drawRect(0, 0, w, h, paint);
            final float in = line * 2, top = in, bot = h - in, left = in, right = w - in;
            if (indeterminate) {
                float t = ((android.os.SystemClock.uptimeMillis() - sweepStart) % 1400) / 1400f;
                float band = (right - left) * 0.28f;
                float x0 = left - band + (right - left + band) * t;
                paint.setShader(new android.graphics.LinearGradient(x0, 0, x0 + band, 0,
                        new int[]{0x004FD46A, 0xFF4FD46A, 0x004FD46A}, null, android.graphics.Shader.TileMode.CLAMP));
                c.save();
                c.clipRect(left, top, right, bot);
                c.drawRect(x0, top, x0 + band, bot, paint);
                c.restore();
                paint.setShader(null);
                postInvalidateOnAnimation();
            } else if (frac > 0f) {
                float x = left + (right - left) * frac;
                paint.setShader(new android.graphics.LinearGradient(0, top, 0, bot,
                        0xFF6FE88A, 0xFF1F6B34, android.graphics.Shader.TileMode.CLAMP));
                c.drawRect(left, top, x, bot, paint);
                paint.setShader(null);
                // cell gaps, as in the game's meters
                paint.setColor(TRACK);
                float cell = Math.max(6 * d, (right - left) / 40f);
                for (float gx = left + cell; gx < x; gx += cell) c.drawRect(gx - line * 0.5f, top, gx + line * 0.5f, bot, paint);
                paint.setColor(0x80FFFFFF);
                c.drawRect(Math.max(left, x - line), top, x, bot, paint);
            }
            paint.setStyle(android.graphics.Paint.Style.STROKE);
            paint.setStrokeWidth(line);
            paint.setColor(STEEL);
            c.drawRect(line / 2, line / 2, w - line / 2, h - line / 2, paint);
        }
    }
}
