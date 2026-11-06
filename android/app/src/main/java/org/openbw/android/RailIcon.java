package org.openbw.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.View;

/**
 * The phone icon rail's glyphs. Drawn with
 * Canvas strokes in a 24 x 24 box -- the APK ships no game art, so neither does this.
 */
final class RailIcon extends View {
    static final int SINGLE = 0, LAN = 1, CAMPAIGN = 2, SAVES = 3, REPLAYS = 4, MODS = 5, SETTINGS = 6;

    private final int kind;
    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final RectF r = new RectF();

    RailIcon(Context c, int kind) {
        super(c);
        this.kind = kind;
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeCap(Paint.Cap.ROUND);
        p.setStrokeJoin(Paint.Join.ROUND);
        p.setColor(Kit.TEXT2);
    }

    void setColor(int color) { p.setColor(color); invalidate(); }

    @Override protected void onDraw(Canvas c) {
        float s = Math.min(getWidth(), getHeight()) / 24f;
        c.save();
        c.translate((getWidth() - 24 * s) / 2f, (getHeight() - 24 * s) / 2f);
        c.scale(s, s);
        p.setStrokeWidth(1.8f);
        p.setStyle(Paint.Style.STROKE);
        path.reset();
        switch (kind) {
            case SINGLE:                                  // one person
                c.drawCircle(12, 8, 4, p);
                r.set(4, 14, 20, 30);
                c.drawArc(r, 180, 180, false, p);
                break;
            case LAN:                                     // three linked nodes
                c.drawLine(12, 7, 5.5f, 17, p);
                c.drawLine(12, 7, 18.5f, 17, p);
                c.drawLine(5.5f, 17, 18.5f, 17, p);
                p.setStyle(Paint.Style.FILL);
                c.drawCircle(12, 6, 3, p);
                c.drawCircle(5, 18, 3, p);
                c.drawCircle(19, 18, 3, p);
                break;
            case CAMPAIGN:                                // a flag on a pole
                c.drawLine(6, 3, 6, 21, p);
                path.moveTo(6, 4);
                path.lineTo(19, 4);
                path.lineTo(16, 8.5f);
                path.lineTo(19, 13);
                path.lineTo(6, 13);
                c.drawPath(path, p);
                break;
            case SAVES:                                   // a disk
                path.moveTo(4, 4);
                path.lineTo(17, 4);
                path.lineTo(20, 7);
                path.lineTo(20, 20);
                path.lineTo(4, 20);
                path.close();
                c.drawPath(path, p);
                c.drawRect(8, 4, 15, 9, p);
                c.drawRect(7, 13, 17, 20, p);
                break;
            case REPLAYS:                                 // play inside a ring
                c.drawCircle(12, 12, 9, p);
                p.setStyle(Paint.Style.FILL);
                path.moveTo(10, 8);
                path.lineTo(16.5f, 12);
                path.lineTo(10, 16);
                path.close();
                c.drawPath(path, p);
                break;
            case MODS:                                    // stacked layers
                for (int i = 0; i != 3; ++i) {
                    float y = 7 + i * 5;
                    path.reset();
                    path.moveTo(12, y - 4);
                    path.lineTo(20, y);
                    path.lineTo(12, y + 4);
                    path.lineTo(4, y);
                    path.close();
                    c.drawPath(path, p);
                }
                break;
            case SETTINGS:                                // a gear
                c.drawCircle(12, 12, 3, p);
                c.drawCircle(12, 12, 7, p);
                for (int i = 0; i != 8; ++i) {
                    double a = i * Math.PI / 4;
                    float cx = (float) Math.cos(a), sy = (float) Math.sin(a);
                    c.drawLine(12 + cx * 7, 12 + sy * 7, 12 + cx * 10, 12 + sy * 10, p);
                }
                break;
        }
        c.restore();
    }
}
