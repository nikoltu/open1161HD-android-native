package org.openbw.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.SystemClock;
import android.view.View;

import java.io.File;
import java.util.Random;

/**
 * An animated unit portrait (LAUNCHER-REDESIGN F1). Plays the idle clip of a
 * portdata id at its own frame time (100 ms), occasionally switching to another
 * idle variant like the game does, and can play the talking clip once.
 *
 * SD clips are 60x56 and are drawn at an INTEGER multiple without filtering, with
 * a light scanline pass -- upscaled smoothly they turn to mush. Before any game
 * files are imported there is no portrait at all (the app ships no art): the view
 * then draws a procedural "no signal" tile.
 */
final class PortraitView extends View {
    private File gameDir;
    private int id = -1;
    private PortraitStore.Entry entry;
    private PortraitStore.Clip clip;
    private boolean talking;
    private int variant;
    private long clipStart;          // uptime ms the current clip started
    private int frame;
    private boolean animate = true;
    private int borderColor = 0xFFD02020;
    private int borderPx;

    private final Paint paint = new Paint();
    private final Paint line = new Paint();
    private final Paint border = new Paint();
    private final Rect src = new Rect(), dst = new Rect();
    private final Random rnd = new Random();
    private Bitmap noise;
    private int[] noisePx;

    PortraitView(Context c) {
        super(c);
        paint.setFilterBitmap(false);
        paint.setAntiAlias(false);
        line.setColor(0x38000000);
        border.setStyle(Paint.Style.STROKE);
        borderPx = Math.max(2, Math.round(getResources().getDisplayMetrics().density * 1.5f));
        border.setStrokeWidth(borderPx);
        // system animations switched off (accessibility / battery saver): show a still frame
        try {
            if (android.provider.Settings.Global.getFloat(c.getContentResolver(),
                    android.provider.Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f) animate = false;
        } catch (Throwable ignored) {}
    }

    void setBorderColor(int argb) { borderColor = argb; invalidate(); }
    void setAnimate(boolean on) { animate = on; invalidate(); }
    int portraitId() { return id; }

    /** Shows the idle clip of a portdata id (decodes on the calling thread, ~1 ms). */
    void setPortrait(File gameDir, int id) {
        this.gameDir = gameDir;
        this.id = id;
        entry = PortraitStore.catalog(gameDir).get(id);
        talking = false;
        variant = 0;
        ++hdSeq;
        setClip(PortraitStore.clip(gameDir, id, false, 0));
        if (wantHd()) upgradeToHd(false, 0, false);
    }

    // ---- HD (SC:R HD2 clips): every portrait, when the player has an SC:R folder and
    // an HD graphics tier. The SD clip is on screen at once; the HD clip replaces it
    // when the background decode (or the disk cache) delivers it. A clip that is not
    // ready never interrupts the one playing -- no flicker between the SD and the HD
    // look. Row-sized portraits (under 180 px) take the half-resolution clip.
    private int hdSeq;
    private boolean wantHd() { return entry != null && PortraitStore.hdEnabled(); }
    private boolean hdHalf() { return fixedScale < 3; }

    /** Switches to the HD clip (talk / variant) as soon as it exists; returns true when it was shown at once. */
    private boolean upgradeToHd(final boolean talk, final int v, final boolean keepCurrentUntilReady) {
        PortraitStore.Clip have = PortraitStore.hdCached(id, talk, v, hdHalf());
        if (have != null) { talking = talk; variant = v; setClip(have); return true; }
        if (PortraitStore.hdKnownMissing(id, talk, v)) return false;
        final int seq = hdSeq, forId = id;
        PortraitStore.hdAsync(gameDir, id, talk, v, hdHalf(), c -> {
            if (c == null || seq != hdSeq || forId != id) return;
            talking = talk;
            variant = v;
            setClip(c);
        });
        return keepCurrentUntilReady;
    }

    /** For lists that decode off the UI thread: hand over an already decoded idle clip. */
    void setDecoded(File gameDir, int id, PortraitStore.Clip c) {
        this.gameDir = gameDir;
        this.id = id;
        entry = PortraitStore.catalog(gameDir).get(id);
        talking = false;
        variant = 0;
        ++hdSeq;
        setClip(c);
        if (wantHd()) upgradeToHd(false, 0, false);
    }

    private void setClip(PortraitStore.Clip c) {
        clip = c;
        frame = 0;
        clipStart = SystemClock.uptimeMillis();
        invalidate();
    }

    /** The player poked the portrait: it talks, with one of the unit's own voice lines. */
    void speak() {
        playTalk();
        if (entry != null && entry.voices.length > 0) MenuAudio.voice(gameDir, entry.voices[rnd.nextInt(entry.voices.length)]);
    }

    /** Plays the talking clip once, then returns to idle. */
    void playTalk() {
        if (entry == null || entry.nTalk <= 0) return;
        int v = rnd.nextInt(entry.nTalk);
        if (showingHd()) { upgradeToHd(true, v, true); return; }   // HD: keep idling until the talk clip is decoded
        PortraitStore.Clip c = PortraitStore.clip(gameDir, id, true, v);
        if (c == null) return;
        talking = true;
        setClip(c);
    }
    private boolean showingHd() { return clip != null && clip.hd && wantHd(); }

    /** Gallery preview: loop the talking clips (true) or go back to the idle ones (false). */
    void setTalkLoop(boolean on) {
        loopTalk = on;
        if (on) { playTalk(); return; }
        if (showingHd() && upgradeToHd(false, 0, true)) return;
        talking = false;
        variant = 0;
        setClip(PortraitStore.clip(gameDir, id, false, 0));
    }
    private boolean loopTalk;

    /**
     * Menu art (campaign emblems): any decoded clip, fitted into a box -- a whole
     * multiple when it fits at least once, filtered when it has to shrink. No border,
     * no scanlines, nothing drawn while the clip is not there yet.
     */
    void setArt(PortraitStore.Clip c) {
        artMode = true;
        entry = null;
        id = -1;
        talking = false;
        setClip(c);
    }
    private boolean artMode;
    private Paint artPaint;

    /** Fixes the view to an exact integer multiple of the 60x56 clip (plus the border). */
    void setScale(int s) { fixedScale = Math.max(1, s); requestLayout(); }
    private int fixedScale;

    /** The largest integer multiple of the SD clip that fits `px` pixels of width. */
    static int scaleFor(int px) { return Math.max(1, px / 60); }

    @Override protected void onMeasure(int wSpec, int hSpec) {
        if (fixedScale > 0) setMeasuredDimension(60 * fixedScale + 2 * borderPx, 56 * fixedScale + 2 * borderPx);
        else super.onMeasure(wSpec, hSpec);
    }

    private void advance() {
        if (clip == null || !animate) return;
        long el = SystemClock.uptimeMillis() - clipStart;
        int f = (int) (el / clip.msPerFrame);
        if (f < clip.frames) { frame = f; return; }
        // clip ended: talk -> back to idle (or loop, in the gallery preview); idle ->
        // maybe another variant (the game's smk_change chance)
        if (showingHd()) {
            // HD: move on only to clips that are already decoded; ask for the others and
            // loop the current one meanwhile.
            boolean wantTalk = talking && loopTalk;
            int v = wantTalk ? (entry.nTalk > 0 ? rnd.nextInt(entry.nTalk) : 0)
                    : (!talking && !hdHalf() && entry.nIdle > 1 && rnd.nextInt(100) < Math.max(20, entry.change)) ? rnd.nextInt(entry.nIdle) : 0;
            PortraitStore.Clip next = PortraitStore.hdCached(id, wantTalk, v, hdHalf());
            if (next != null) { talking = wantTalk; variant = v; setClip(next); }
            else {
                if (!PortraitStore.hdKnownMissing(id, wantTalk, v)) upgradeToHd(wantTalk, v, true);
                if (talking && !loopTalk) {
                    PortraitStore.Clip idle = PortraitStore.hdCached(id, false, 0, hdHalf());
                    talking = false;
                    variant = 0;
                    setClip(idle != null ? idle : clip);
                } else { frame = 0; clipStart = SystemClock.uptimeMillis(); }
            }
        } else if (talking && !loopTalk) {
            talking = false;
            variant = 0;
            setClip(PortraitStore.clip(gameDir, id, false, 0));
        } else if (talking) {
            setClip(PortraitStore.clip(gameDir, id, true, entry.nTalk > 0 ? rnd.nextInt(entry.nTalk) : 0));
        } else if (entry != null && entry.nIdle > 1 && rnd.nextInt(100) < Math.max(20, entry.change)) {
            variant = rnd.nextInt(entry.nIdle);
            PortraitStore.Clip c = PortraitStore.clip(gameDir, id, false, variant);
            setClip(c != null ? c : clip);
        } else {
            frame = 0;
            clipStart = SystemClock.uptimeMillis();
        }
    }

    @Override protected void onDraw(Canvas canvas) {
        int vw = getWidth(), vh = getHeight();
        if (vw <= 0 || vh <= 0) return;
        int pad = borderPx;
        int aw = vw - 2 * pad, ah = vh - 2 * pad;
        if (artMode) {
            if (clip == null || clip.strip == null) return;
            advance();
            PortraitStore.Clip c = clip;
            // Pre-rendered art, not pixel art: fill the box (up to 2.5x), filtered. The
            // clips sit on pure black, so LIGHTEN melts that background into the panel.
            float sc = Math.min(2.5f, Math.min((float) vw / c.w, (float) vh / c.h));
            boolean whole = sc == (float) Math.floor(sc);
            if (artPaint == null) {
                artPaint = new Paint();
                artPaint.setXfermode(new android.graphics.PorterDuffXfermode(android.graphics.PorterDuff.Mode.LIGHTEN));
            }
            int dw = Math.round(c.w * sc), dh = Math.round(c.h * sc);
            int x0 = (vw - dw) / 2, y0 = (vh - dh) / 2;
            int cx = c.cols > 0 ? frame % c.cols : frame, cy = c.cols > 0 ? frame / c.cols : 0;
            src.set(cx * c.w, cy * c.h, cx * c.w + c.w, cy * c.h + c.h);
            dst.set(x0, y0, x0 + dw, y0 + dh);
            artPaint.setFilterBitmap(!whole);
            canvas.drawBitmap(c.strip, src, dst, artPaint);
            if (animate && isShown()) postInvalidateDelayed(c.msPerFrame);
            return;
        }
        if (clip != null && clip.strip != null) {
            advance();
            PortraitStore.Clip c = clip;
            // an HD clip takes exactly the box its 60x56 SD twin would (same 15:14 shape)
            int scale = Math.max(1, c.hd ? Math.min(aw / 60, ah / 56) : Math.min(aw / c.w, ah / c.h));
            int dw = (c.hd ? 60 : c.w) * scale, dh = (c.hd ? 56 : c.h) * scale;
            int x0 = pad + (aw - dw) / 2, y0 = pad + (ah - dh) / 2;
            src.set(frame * c.w, 0, frame * c.w + c.w, c.h);
            dst.set(x0, y0, x0 + dw, y0 + dh);
            paint.setFilterBitmap(c.hd);
            canvas.drawBitmap(c.strip, src, dst, paint);
            if (!c.hd && scale >= 2) for (int y = y0 + scale - 1; y < y0 + dh; y += scale) canvas.drawRect(x0, y, x0 + dw, y + 1, line);
            border.setColor(borderColor);
            canvas.drawRect(x0 - pad / 2f, y0 - pad / 2f, x0 + dw + pad / 2f, y0 + dh + pad / 2f, border);
            if (animate && isShown()) postInvalidateDelayed(c.msPerFrame);
        } else {
            // "no signal": our own pixels, regenerated ~10x a second
            if (noise == null) { noise = Bitmap.createBitmap(30, 28, Bitmap.Config.ARGB_8888); noisePx = new int[30 * 28]; }
            for (int i = 0; i != noisePx.length; ++i) { int g = 24 + rnd.nextInt(72); noisePx[i] = 0xFF000000 | (g << 16) | (g << 8) | g; }
            noise.setPixels(noisePx, 0, 30, 0, 0, 30, 28);
            // the same box a 60x56 clip would take, so the tile does not jump when a clip arrives
            int scale = 2 * Math.max(1, Math.min(aw / 60, ah / 56));
            int dw = 30 * scale, dh = 28 * scale;
            int x0 = pad + (aw - dw) / 2, y0 = pad + (ah - dh) / 2;
            src.set(0, 0, 30, 28);
            dst.set(x0, y0, x0 + dw, y0 + dh);
            canvas.drawBitmap(noise, src, dst, paint);
            border.setColor(0xFF606870);
            canvas.drawRect(x0 - pad / 2f, y0 - pad / 2f, x0 + dw + pad / 2f, y0 + dh + pad / 2f, border);
            if (animate && isShown()) postInvalidateDelayed(100);
        }
    }

    @Override protected void onVisibilityChanged(View changedView, int visibility) {
        super.onVisibilityChanged(changedView, visibility);
        if (visibility == VISIBLE) { clipStart = SystemClock.uptimeMillis(); invalidate(); }
    }
}
