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

    Kit(Activity a) {
        this.a = a;
        density = a.getResources().getDisplayMetrics().density;
    }

    int dp(float v) { return Math.round(v * density); }

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
        t.setPadding(0, dp(10), 0, dp(6));
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
        TextView b = text(s.toUpperCase(java.util.Locale.ROOT), kind == 1 ? Color.WHITE : TEXT, kind == 1 ? 15 : 13);
        b.setLetterSpacing(0.1f);
        b.setGravity(Gravity.CENTER);
        b.setTypeface(Typeface.DEFAULT, kind == 1 ? Typeface.BOLD : Typeface.NORMAL);
        b.setPadding(dp(16), 0, dp(16), 0);
        b.setMinHeight(dp(48));
        b.setMinimumHeight(dp(48));
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                kind == 1 ? new int[]{0xFF1F6B34, 0xFF114420} : kind == 0 ? new int[]{0xFF2A3338, 0xFF161C20} : new int[]{0, 0});
        g.setStroke(Math.max(1, dp(1)), kind == 1 ? LABEL : kind == 0 ? STEEL : LABEL);
        b.setBackground(g);
        b.setClickable(true);
        b.setOnClickListener(v -> { MenuAudio.play(kind == 1 ? MenuAudio.LOCK : MenuAudio.CLICK); run.run(); });
        return b;
    }

    /** A two-option segmented control; `sink` gets the picked index. */
    LinearLayout segmented(String[] labels, int sel, final IntSink sink) {
        final LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        final TextView[] cells = new TextView[labels.length];
        for (int i = 0; i != labels.length; ++i) {
            final int idx = i;
            TextView t = text(labels[i], TEXT, labels.length > 4 ? 12 : 13);
            t.setGravity(Gravity.CENTER);
            t.setMaxLines(2);
            t.setPadding(dp(2), 0, dp(2), 0);
            t.setMinHeight(dp(44));
            t.setMinimumHeight(dp(44));
            t.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f));
            t.setOnClickListener(v -> {
                MenuAudio.play(MenuAudio.CLICK);
                for (int k = 0; k != cells.length; ++k) styleSeg(cells[k], k == idx);
                sink.set(idx);
            });
            cells[i] = t;
            styleSeg(t, i == sel);
            row.addView(t);
        }
        return row;
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
        t.setMinHeight(dp(40));
        t.setMinimumHeight(dp(40));
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
        b.setMinHeight(dp(40));
        b.setMinimumHeight(dp(40));
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

    Dialog modal(View content, boolean fullScreen) {
        // panels swish in and out, like the original menu's
        final Dialog d = new Dialog(a, android.R.style.Theme_Translucent_NoTitleBar) {
            @Override public void show() { MenuAudio.play(MenuAudio.OPEN); super.show(); }
            @Override public void dismiss() { if (isShowing()) MenuAudio.play(MenuAudio.CLOSE); super.dismiss(); }
        };
        FrameLayout scrim = new FrameLayout(a);
        scrim.setBackgroundColor(fullScreen ? 0xF205080A : 0xB8000000);
        scrim.setOnClickListener(v -> { if (!fullScreen) d.dismiss(); });
        content.setClickable(true);   // taps on the panel must not fall through to the scrim
        int m = fullScreen ? dp(12) : 0;
        FrameLayout.LayoutParams lp = fullScreen
                ? new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
                : new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        lp.setMargins(m, m, m, m);
        scrim.addView(content, lp);
        d.setContentView(scrim);
        Window w = d.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
            w.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
        }
        return d;
    }
}
