package org.openbw.android;

import android.app.Activity;
import android.app.Dialog;
import android.content.DialogInterface;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * v1.0.26: the launcher's own dialog -- the same calls as AlertDialog.Builder
 * (title, message, view, items, three buttons, cancelable), drawn with Kit: the
 * shell's panel, green title, Kit buttons, the menu swish. Replaces the stock
 * Android alert boxes that ignored the launcher design.
 */
class KitDialog {
    private final Activity a;
    private final Kit kit;
    private CharSequence title, message;
    private View view;
    private CharSequence[] items;
    private DialogInterface.OnClickListener itemsListener;
    private final CharSequence[] btnLabel = new CharSequence[3];      // positive, negative, neutral
    private final DialogInterface.OnClickListener[] btnListener = new DialogInterface.OnClickListener[3];
    private boolean cancelable = true;
    private int widthDp;
    private DialogInterface.OnDismissListener onDismiss;

    KitDialog(Activity a) {
        this.a = a;
        this.kit = new Kit(a);
    }

    KitDialog setTitle(CharSequence s) { title = s; return this; }
    KitDialog setMessage(CharSequence s) { message = s; return this; }
    KitDialog setView(View v) { view = v; return this; }
    KitDialog setItems(CharSequence[] it, DialogInterface.OnClickListener l) { items = it; itemsListener = l; return this; }
    KitDialog setPositiveButton(CharSequence s, DialogInterface.OnClickListener l) { btnLabel[0] = s; btnListener[0] = l; return this; }
    KitDialog setNegativeButton(CharSequence s, DialogInterface.OnClickListener l) { btnLabel[1] = s; btnListener[1] = l; return this; }
    KitDialog setNeutralButton(CharSequence s, DialogInterface.OnClickListener l) { btnLabel[2] = s; btnListener[2] = l; return this; }
    KitDialog setCancelable(boolean c) { cancelable = c; return this; }
    /** Panel width in dp (default 560, or 760 with a custom view); capped to the screen. */
    KitDialog setWidth(int dp) { widthDp = dp; return this; }
    KitDialog setOnDismissListener(DialogInterface.OnDismissListener l) { onDismiss = l; return this; }

    Dialog show() {
        Dialog d = create();
        d.show();
        return d;
    }

    Dialog create() {
        final Dialog[] dref = new Dialog[1];
        LinearLayout panel = new LinearLayout(a);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(kit.box(0xFF000000, Kit.LINE));
        int pad = kit.dp(kit.compact ? 14 : 20);
        panel.setPadding(pad, pad, pad, pad);

        if (title != null && title.length() > 0) {
            TextView t = kit.text(title.toString().toUpperCase(java.util.Locale.ROOT), Kit.LABEL, kit.compact ? 15 : 17);
            t.setLetterSpacing(0.1f);
            t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
            t.setPadding(0, 0, 0, kit.dp(kit.compact ? 8 : 10));
            panel.addView(t);
            View rule = new View(a);                             // the launcher's red section divider
            rule.setBackgroundColor(Kit.LINE);
            LinearLayout.LayoutParams rl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, kit.dp(1)));
            rl.bottomMargin = kit.dp(kit.compact ? 10 : 14);
            panel.addView(rule, rl);
        }
        // message / items scroll inside the panel; a supplied view keeps its own layout
        LinearLayout body = new LinearLayout(a);
        body.setOrientation(LinearLayout.VERTICAL);
        if (message != null && message.length() > 0) {
            TextView m = kit.text(message.toString(), Kit.TEXT, kit.compact ? 14 : 15);
            m.setLineSpacing(0, 1.15f);
            body.addView(m);
        }
        if (items != null) {
            for (int i = 0; i < items.length; i++) {
                final int which = i;
                TextView row = kit.text(String.valueOf(items[i]), Kit.TEXT, kit.compact ? 14 : 16);
                row.setPadding(kit.dp(12), kit.dp(kit.compact ? 8 : 11), kit.dp(12), kit.dp(kit.compact ? 8 : 11));
                row.setBackground(kit.box(0xFF000000, Kit.LINE));
                row.setClickable(true);
                row.setOnClickListener(v -> {
                    MenuAudio.play(MenuAudio.CLICK);
                    if (itemsListener != null) itemsListener.onClick(dref[0], which);
                    dref[0].dismiss();
                });
                LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                rlp.bottomMargin = kit.dp(4);
                body.addView(row, rlp);
            }
        }
        if (body.getChildCount() > 0) {
            ScrollView sv = new ScrollView(a);
            sv.addView(body);
            panel.addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        }
        if (view != null) {
            if (view.getParent() instanceof ViewGroup) ((ViewGroup) view.getParent()).removeView(view);
            panel.addView(view, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        }

        boolean anyBtn = btnLabel[0] != null || btnLabel[1] != null || btnLabel[2] != null;
        if (anyBtn) {
            LinearLayout bar = new LinearLayout(a);
            bar.setOrientation(LinearLayout.HORIZONTAL);
            bar.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
            bar.setPadding(0, kit.dp(14), 0, 0);
            final int[] order = {2, 1, 0};                       // neutral left, then negative, positive
            final int[] which = {DialogInterface.BUTTON_POSITIVE, DialogInterface.BUTTON_NEGATIVE, DialogInterface.BUTTON_NEUTRAL};
            boolean first = true;
            for (int k : order) {
                if (btnLabel[k] == null) continue;
                final int idx = k;
                if (k == 2) {   // neutral sits on its own at the left
                    bar.addView(kit.button(btnLabel[k].toString(), 2, () -> press(dref[0], idx, which[idx])),
                            new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, kit.hdp(48)));
                    bar.addView(new View(a), new LinearLayout.LayoutParams(0, 1, 1f));
                    first = false;
                    continue;
                }
                TextView b = kit.button(btnLabel[k].toString(), k == 0 ? 1 : 0, () -> press(dref[0], idx, which[idx]));
                b.setMinWidth(kit.dp(kit.compact ? 110 : 140));
                LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, kit.hdp(48));
                if (!first) blp.leftMargin = kit.dp(10);
                bar.addView(b, blp);
                first = false;
            }
            panel.addView(bar);
        }

        int sw = a.getResources().getDisplayMetrics().widthPixels;
        int sh = a.getResources().getDisplayMetrics().heightPixels;
        int w = Math.min(sw - kit.dp(32), kit.dp(widthDp > 0 ? widthDp : view != null ? 760 : 560));
        LinearLayout sizer = new LinearLayout(a);                 // caps the panel to the screen
        sizer.setOrientation(LinearLayout.VERTICAL);
        sizer.addView(panel, new LinearLayout.LayoutParams(w, ViewGroup.LayoutParams.WRAP_CONTENT));
        panel.setLayoutParams(new LinearLayout.LayoutParams(w, ViewGroup.LayoutParams.WRAP_CONTENT));
        panel.setMinimumHeight(0);
        final int maxH = sh - kit.dp(32);
        panel.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (b - t > maxH) { ViewGroup.LayoutParams lp = v.getLayoutParams(); lp.height = maxH; v.setLayoutParams(lp); }
        });

        Dialog d = kit.modal(sizer, false, cancelable);
        d.setCancelable(cancelable);
        if (onDismiss != null) d.setOnDismissListener(onDismiss);
        dref[0] = d;
        return d;
    }

    private void press(Dialog d, int idx, int which) {
        if (btnListener[idx] != null) btnListener[idx].onClick(d, which);
        d.dismiss();
    }
}
