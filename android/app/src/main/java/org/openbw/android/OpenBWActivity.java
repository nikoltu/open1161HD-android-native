package org.openbw.android;

import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.os.Bundle;
import android.os.SystemClock;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;
import android.widget.TextView;
import android.widget.FrameLayout;
import android.widget.VideoView;
import android.media.MediaPlayer;
import android.graphics.Color;
import android.view.Gravity;
import java.util.concurrent.CountDownLatch;
import org.json.JSONArray;
import org.json.JSONObject;

import org.libsdl.app.SDLActivity;

/**
 * The SDL host activity. The game itself runs in native code (libmain.so,
 * see android/app/src/main/cpp/android_main.cpp); this class only wires up
 * which replay to open, the render resolution, and a small overlay with
 * playback controls that injects the viewer's keyboard shortcuts.
 */
public class OpenBWActivity extends SDLActivity {

    // open1161HD Faze 4.7 Variant A: play a full-screen WebM cinematic via the OS
    // video player (VideoView/MediaPlayer), overlaid on top of the SDL surface.
    // Called from native (game thread) with a temp file path; blocks that thread
    // until the clip finishes, errors, or the player taps to skip. Returns true if
    // it played. The OS handles VP9 decode + Vorbis audio + any device-specific
    // (Qualcomm UBWC / tiled) output internally, so this is robust across all SoCs
    // (Snapdragon, MediaTek, Exynos) -- unlike a native CPU-readback path.
    public boolean playCinematic(final String path) {
        final CountDownLatch latch = new CountDownLatch(1);
        final boolean[] result = { false };
        runOnUiThread(new Runnable() {
            @Override public void run() {
                try {
                    final FrameLayout wrap = new FrameLayout(OpenBWActivity.this);
                    wrap.setBackgroundColor(Color.BLACK);
                    wrap.setClickable(true);
                    final VideoView vv = new VideoView(OpenBWActivity.this);
                    // Both SDL's surface and VideoView are SurfaceViews; force the
                    // video above the game surface (must be set before the surface is
                    // created, i.e. before setVideoPath/prepared).
                    vv.setZOrderOnTop(true);
                    wrap.addView(vv, new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER));
                    addContentView(wrap, new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
                    final Runnable finish = new Runnable() {
                        boolean done = false;
                        @Override public void run() {
                            if (done) return; done = true;
                            try { vv.stopPlayback(); } catch (Exception e) {}
                            try { ViewGroup p = (ViewGroup) wrap.getParent(); if (p != null) p.removeView(wrap); } catch (Exception e) {}
                            latch.countDown();
                        }
                    };
                    vv.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
                        @Override public void onPrepared(MediaPlayer mp) { mp.setVolume(1f, 1f); mp.setLooping(false); vv.start(); }
                    });
                    vv.setOnCompletionListener(new MediaPlayer.OnCompletionListener() {
                        @Override public void onCompletion(MediaPlayer mp) { result[0] = true; finish.run(); }
                    });
                    vv.setOnErrorListener(new MediaPlayer.OnErrorListener() {
                        @Override public boolean onError(MediaPlayer mp, int w, int e) { result[0] = false; finish.run(); return true; }
                    });
                    wrap.setOnClickListener(new View.OnClickListener() {
                        @Override public void onClick(View v) { result[0] = true; finish.run(); } // tap = skip (played)
                    });
                    vv.setVideoPath(path);
                } catch (Exception e) {
                    result[0] = false;
                    latch.countDown();
                }
            }
        });
        try { latch.await(); } catch (InterruptedException e) {}
        return result[0];
    }

    /**
     * Raise or hide the soft keyboard for in-game chat. Called from native
     * (ui/sdl2.cpp start_text_input) because SDL_StartTextInput() alone did not
     * bring the keyboard up on the test tablets. Must touch the IMM on the UI
     * thread; the native call comes from the game thread.
     */
    public void showSoftKeyboard(final boolean show) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                try {
                    android.view.inputmethod.InputMethodManager imm =
                            (android.view.inputmethod.InputMethodManager)
                                    getSystemService(INPUT_METHOD_SERVICE);
                    if (imm == null) return;
                    View v = getWindow().getDecorView();
                    if (show) {
                        v.setFocusable(true);
                        v.setFocusableInTouchMode(true);
                        v.requestFocus();
                        imm.showSoftInput(v, android.view.inputmethod.InputMethodManager.SHOW_FORCED);
                    } else {
                        imm.hideSoftInputFromWindow(v.getWindowToken(), 0);
                    }
                } catch (Throwable ignored) {
                }
                if (show) startImeInsetWatch();
            }
        });
    }

    /**
     * Reports the soft keyboard's height to native (v0.83).
     *
     * SDL does not resize its surface when the IME opens, so the chat input
     * line was being drawn underneath the keyboard - you could type, and the
     * text arrived, but you could not see it. There is no callback for "the
     * IME resized", so watch the decor view's layout and diff the visible
     * frame against the window height. Attached once, on the first keyboard
     * request, and left in place: it is a cheap comparison per layout pass.
     */
    private boolean imeWatchAttached = false;
    private int lastImeHeight = -1;

    private void startImeInsetWatch() {
        if (imeWatchAttached) return;
        imeWatchAttached = true;
        final View decor = getWindow().getDecorView();
        decor.getViewTreeObserver().addOnGlobalLayoutListener(
                new android.view.ViewTreeObserver.OnGlobalLayoutListener() {
                    @Override public void onGlobalLayout() {
                        android.graphics.Rect r = new android.graphics.Rect();
                        decor.getWindowVisibleDisplayFrame(r);
                        int h = decor.getHeight() - r.bottom;
                        if (h < 0) h = 0;
                        // Ignore the navigation bar: only treat a sizeable
                        // chunk as a keyboard.
                        if (h < decor.getHeight() / 6) h = 0;
                        // v0.125: report it as a share of the GAME SURFACE (1/10000 of
                        // its height), not as device pixels: the native render buffer
                        // can be half the device resolution, and the surface does not
                        // start at the top of the window (status bar).
                        int frac = 0;
                        if (h > 0) {
                            int top = 0, sh = decor.getHeight();
                            View s = mSurface;
                            if (s != null && s.getHeight() > 0) {
                                int[] loc = new int[2];
                                s.getLocationInWindow(loc);
                                top = loc[1];
                                sh = s.getHeight();
                            }
                            int cov = top + sh - r.bottom;
                            if (cov < 0) cov = 0;
                            if (cov > sh) cov = sh;
                            frac = sh > 0 ? (int) ((long) cov * 10000 / sh) : 0;
                        }
                        if (frac != lastImeHeight) {
                            lastImeHeight = frac;
                            try { nativeSetImeCover(frac); } catch (Throwable ignored) {}
                        }
                    }
                });
    }

    private static native void nativeSetImeCover(int frac);

    public static final String EXTRA_REPLAY = "replay";
    public static final String EXTRA_MAP = "map";
    public static final String EXTRA_MAP_MPQ = "mapMpq";      // v0.35: bundled map internal MPQ path
    public static final String EXTRA_RACE = "race";           // 0 Z, 1 T, 2 P, 5 random
    public static final String EXTRA_ENEMY_RACE = "enemyRace";
    public static final String EXTRA_RACES = "races";         // String: per active slot race csv ("1,0,2"); index 0 = local
    public static final String EXTRA_COLORS = "colors";       // String: per active slot colour csv (0..7); index 0 = local (render-only)
    public static final String EXTRA_NET_HOST = "netHost";    // boolean: host a LAN game
    public static final String EXTRA_NET_JOIN = "netJoin";    // String: host IP to join
    public static final String EXTRA_NET_FAST = "netFast";    // boolean: launcher lobby already agreed - auto-start, no in-game ready
    public static final String EXTRA_NET_PORT = "netPort";    // int: game TCP port (kept distinct from the launcher lobby's 6112)
    public static final String EXTRA_PLAYERS = "players";     // int: single-device player count (2-8)
    public static final String EXTRA_TEAMS = "teams";         // String: per-slot team csv ("0,0,1,1"); empty = FFA
    public static final String EXTRA_TEAM_MODE = "teamMode";  // int: 0 FFA, 1 two teams
    public static final String EXTRA_VOLUME = "volume";
    public static final String EXTRA_MUSIC_VOLUME = "musicVolume";
    public static final String EXTRA_MODS = "mods"; // csv of active mod ids (files/mods/<id>/)
    public static final String EXTRA_GAME_MODE = "gameMode"; // melee | ums | <trg name>
    public static final String EXTRA_CAMPAIGN = "campaign"; // campaign/<race>/<mission> in the MPQ chain
    public static final String EXTRA_VS_AI = "vsAi"; // melee: opponents run the aiscript AI
    public static final String EXTRA_PLAYER_NAME = "playerName"; // UserID: shown as the player's "Name"
    public static final String EXTRA_NAME_PORTRAITS = "namePortraits"; // LAN: "id=name|id=name" (render-only)
    public static final String EXTRA_GAME_SPEED = "gameSpeed"; // int percent, 100 = normal
    public static final String EXTRA_RENDER_SCALE = "renderScale";
    public static final String EXTRA_SHOW_QUEUE = "showQueue"; // HUD QUEUE toggle visibility
    // (EXTRA_UI_LEGACY / EXTRA_RGBA_SPRITES retired in Faze 6 -- getArguments() hard-codes both.)
    public static final String EXTRA_BOT = "botLib"; // v0.24: path to a bot .so (internal storage)
    public static final String EXTRA_LOAD_SAVE = "loadSave"; // v0.86: .sav path to resume from
    public static final String EXTRA_AUTOSAVE_AT = "autosaveAt"; // v0.86 test hook: write slot 1 at frame N

    // Render scale (half of native resolution) is handled on the native
    // side: ui/sdl2.cpp renders into a half-size texture and the GPU
    // upscales it to the full window with vsync.

    private static final int KEYCODE_M = 41;     // in-game pause menu
    private static final int KEYCODE_SHIFT_LEFT = 59; // S-Pen barrel = queue modifier

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL2", "main"};
    }

    @Override
    protected String[] getArguments() {
        java.util.ArrayList<String> args = new java.util.ArrayList<>();
        // v0.86: resuming a disk save. The .sav container embeds the exact launch
        // argv it was made with (minus --load/--version-code), so relaunching with
        // those args rebuilds an identical game_st; native then applies the save.
        // The CURRENT versionCode is re-added here, so a save from an incompatible
        // build is rejected rather than loaded into a different engine.
        String loadSave = getIntent().getStringExtra(EXTRA_LOAD_SAVE);
        if (loadSave != null && !loadSave.isEmpty()) {
            String[] saved = readSaveArgs(loadSave);
            if (saved != null) {
                for (String a : saved) args.add(a);
                // v0.86.001: the save embedded the display/audio flags it was made
                // with; re-append the CURRENT ones so a resumed game honours today's
                // settings (volume, gfx tier, HUD toggles...) instead of the ones
                // from save time. The native parser takes the LAST occurrence, so
                // these override. Every flag here is pure render/audio/UI -- none
                // touch the deterministic sim -- so overriding is desync-safe; the
                // game-defining args (map/race/players/mods/...) stay from the save.
                appendDisplayArgs(args);
                args.add("--version-code"); args.add(String.valueOf(appVersionCode()));
                args.add("--load"); args.add(loadSave);
                return args.toArray(new String[0]);
            }
            // Unreadable save -> fall through to a normal (frame-0) launch.
        }
        String map = getIntent().getStringExtra(EXTRA_MAP);
        String mapMpq = getIntent().getStringExtra(EXTRA_MAP_MPQ);
        String replay = getIntent().getStringExtra(EXTRA_REPLAY);
        if (map != null || mapMpq != null) {
            if (mapMpq != null) { args.add("--map-mpq"); args.add(mapMpq); }
            else { args.add("--map"); args.add(map); }
            args.add("--race");
            args.add(String.valueOf(getIntent().getIntExtra(EXTRA_RACE, 5)));
            args.add("--enemy-race");
            args.add(String.valueOf(getIntent().getIntExtra(EXTRA_ENEMY_RACE, 5)));
            String races = getIntent().getStringExtra(EXTRA_RACES);
            if (races != null && !races.isEmpty()) { args.add("--races"); args.add(races); }
            String colors = getIntent().getStringExtra(EXTRA_COLORS);
            if (colors != null && !colors.isEmpty()) { args.add("--colors"); args.add(colors); }
            args.add("--players");
            args.add(String.valueOf(getIntent().getIntExtra(EXTRA_PLAYERS, 2)));
            // Player colour choice (0..7 = the classic BW colours; render-only). The
            // local player recolours to this; an unset/-1 keeps the slot default. In a
            // LAN game every peer's choice is negotiated + de-duplicated in the lobby.
            { int pc = getSharedPreferences("openbw", MODE_PRIVATE).getInt("playerColor", -1);
              if (pc >= 0) { args.add("--player-color"); args.add(String.valueOf(pc)); } }
            args.add("--team-mode");
            args.add(String.valueOf(getIntent().getIntExtra(EXTRA_TEAM_MODE, 0)));
            String teams = getIntent().getStringExtra(EXTRA_TEAMS);
            if (teams != null && !teams.isEmpty()) { args.add("--teams"); args.add(teams); }
            if (getIntent().getBooleanExtra(EXTRA_NET_HOST, false)) {
                args.add("--net-host");
            }
            if (getIntent().getBooleanExtra(EXTRA_NET_FAST, false)) {
                args.add("--net-fast");
            }
            String join = getIntent().getStringExtra(EXTRA_NET_JOIN);
            if (join != null && !join.isEmpty()) {
                args.add("--net-join");
                args.add(join);
            }
            int netPort = getIntent().getIntExtra(EXTRA_NET_PORT, 0);
            if (netPort > 0) { args.add("--net-port"); args.add(String.valueOf(netPort)); }
        } else if (replay != null) {
            args.add(replay);
        }
        args.add("--volume");
        args.add(String.valueOf(getIntent().getIntExtra(EXTRA_VOLUME, 50)));
        args.add("--music-volume");
        args.add(String.valueOf(getIntent().getIntExtra(EXTRA_MUSIC_VOLUME, 40)));
        String mods = getIntent().getStringExtra(EXTRA_MODS);
        if (mods != null && !mods.isEmpty()) { args.add("--mods"); args.add(mods); }
        String gameMode = getIntent().getStringExtra(EXTRA_GAME_MODE);
        if (gameMode != null && !gameMode.isEmpty()) { args.add("--game-mode"); args.add(gameMode); }
        String campaignDir = getIntent().getStringExtra(EXTRA_CAMPAIGN);
        if (campaignDir != null && !campaignDir.isEmpty()) { args.add("--campaign"); args.add(campaignDir); }
        if (getIntent().getBooleanExtra(EXTRA_VS_AI, false)) { args.add("--vs-ai"); args.add("1"); }
        String playerName = getIntent().getStringExtra(EXTRA_PLAYER_NAME);
        if (playerName != null && !playerName.isEmpty()) { args.add("--player-name"); args.add(playerName); }
        // LAN: the launcher lobby's "id=name|id=name" portrait list (render-only).
        String namePortraits = getIntent().getStringExtra(EXTRA_NAME_PORTRAITS);
        if (namePortraits != null && !namePortraits.isEmpty()) { args.add("--name-portraits"); args.add(namePortraits); }
        args.add("--scale");
        args.add(String.valueOf(getIntent().getIntExtra(EXTRA_RENDER_SCALE, 2)));
        args.add("--show-queue");
        args.add(getIntent().getBooleanExtra(EXTRA_SHOW_QUEUE, true) ? "1" : "0");
        args.add("--game-speed");
        args.add(String.valueOf(getIntent().getIntExtra(EXTRA_GAME_SPEED, 100)));
        // open1161HD Faze 6: the legacy tconsole HUD is retired -- the native HUD
        // v2 is the only HUD now -- and the RGBA (true-colour) sprite pass is the
        // HD foundation, always on. Both are forced here so their launcher toggles
        // could be removed; no EXTRA needed.
        args.add("--ui-legacy"); args.add("0");
        args.add("--rgba-sprites"); args.add("1");
        String botLib = getIntent().getStringExtra(EXTRA_BOT);
        if (botLib != null && !botLib.isEmpty()) { args.add("--bot"); args.add(botLib); }
        // Unified AI difficulty (0 very easy .. 3 hard) chosen in the launcher ("Sunkumas")
        // or passed by an adb test launch; the bot reads it through the v4 difficulty() query.
        if (getIntent().hasExtra("botLevel")) { args.add("--bot-level"); args.add(String.valueOf(getIntent().getIntExtra("botLevel", 2))); }
        // Dev switches are read straight from prefs rather than plumbed through
        // intent extras: they are diagnostics, not part of the game setup.
        android.content.SharedPreferences dev = getSharedPreferences("openbw", MODE_PRIVATE);
        // Launch-only diagnostics (NOT replayed on a save-resume): tracing, the
        // in-engine bug-catcher, adb test hooks -- none of them affect the picture.
        if (getIntent().getBooleanExtra("hdAnimDiag", false)) args.add("--hd-anim-diag"); // DIAG: HD-anim load log
        if (dev.getBoolean("devTriggerTrace", false)) args.add("--trigger-trace");
        if (dev.getBoolean("devAiTrace", false)) args.add("--ai-trace");
        // DIAG in-engine bug-catcher: on via the devDiagnostics dev-setting OR a
        // one-shot "diag" intent extra (for adb-driven test launches).
        if (dev.getBoolean("devDiagnostics", false) || getIntent().getBooleanExtra("diag", false)) args.add("--diag");
        // SCBW2 house-rules: a bitmask of negotiated QoL rules (0 = vanilla). Passed
        // as the "qol" int extra (adb test launches now; the lobby checkboxes in P3).
        int qolBits = getIntent().getIntExtra("qol", 0);
        if (qolBits != 0) { args.add("--qol"); args.add(String.valueOf(qolBits)); }
        // DIAG scripted self-test (adb-driven behaviour verification, no touch).
        String diagSelftest = getIntent().getStringExtra("diagSelftest");
        if (diagSelftest != null && !diagSelftest.isEmpty()) { args.add("--diag-selftest"); args.add(diagSelftest); }
        // open1161hd Faze 4.7: smoke-test a cinematic without campaign navigation.
        // Set the "testCinematic" pref (e.g. "smk\\terran1.smk") to play it once at
        // launch; empty (default) does nothing.
        { String tc = dev.getString("testCinematic", "");
          if (tc != null && !tc.isEmpty()) { args.add("--test-cinematic"); args.add(tc); } }
        // Render / audio / UI flags -- SINGLE source shared with the save-resume
        // path (appendDisplayArgs) so a new UI flag can never drift between them.
        // (--volume / --music-volume / --scale / --show-queue come from the setup
        // intent extras above; the resume path reads those same four from prefs.)
        appendCommonRenderAudioUiArgs(args);
        try {
            String ver = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
            if (ver != null) {
                args.add("--version"); args.add(ver);
                String lbl = writeVersionLabel("v" + ver);
                if (lbl != null) {
                    android.util.DisplayMetrics dm = new android.util.DisplayMetrics();
                    getWindowManager().getDefaultDisplay().getRealMetrics(dm);
                    args.add("--version-label"); args.add(lbl);
                    args.add("--version-label-h"); args.add(String.valueOf(Math.min(dm.widthPixels, dm.heightPixels)));
                }
            }
        } catch (Throwable ignored) {
        }
        // v0.86: engine build id, so a disk save records which build wrote it and a
        // later load can be rejected if the app has been updated since.
        args.add("--version-code"); args.add(String.valueOf(appVersionCode()));
        int autosaveAt = getIntent().getIntExtra(EXTRA_AUTOSAVE_AT, -1); // v0.86 test hook
        if (autosaveAt >= 0) { args.add("--autosave-at"); args.add(String.valueOf(autosaveAt)); }
        return args.toArray(new String[0]);
    }

    // v0.86.001: append the CURRENT display/audio/UI flags from prefs, so a resumed
    // save honours today's settings rather than the ones baked into the save's argv.
    // The bulk of those flags now live in appendCommonRenderAudioUiArgs (shared with
    // a fresh launch); only the handful a resume sources differently -- from prefs
    // rather than the setup intent -- are emitted here. ONLY non-deterministic
    // (render/audio/UI) flags belong on this path; a game-defining flag would desync.
    private void appendDisplayArgs(java.util.ArrayList<String> args) {
        android.content.SharedPreferences dev = getSharedPreferences("openbw", MODE_PRIVATE);
        // A resume has no setup intent, so these come from prefs (a fresh launch
        // reads the same four from its EXTRA_* setup extras instead).
        args.add("--volume");        args.add(String.valueOf(dev.getInt("volume", 50)));
        args.add("--music-volume");  args.add(String.valueOf(dev.getInt("musicVolume", 40)));
        args.add("--scale");         args.add(String.valueOf(dev.getInt("renderScale", 2)));
        args.add("--show-queue");    args.add(dev.getBoolean("showQueue", true) ? "1" : "0");
        { int pc = dev.getInt("playerColor", -1); if (pc >= 0) { args.add("--player-color"); args.add(String.valueOf(pc)); } }
        appendCommonRenderAudioUiArgs(args);
    }

    // Single source of truth for the render / audio / UI flags that BOTH a fresh
    // launch (getArguments) and a save-resume (appendDisplayArgs) must emit. Every
    // flag here is non-deterministic (render/audio/UI only) -- none touch the sim --
    // so all are resume/LAN-safe, and native takes the LAST occurrence so a caller
    // may still override any of them afterward. Centralising them means a new UI
    // flag is added ONCE and can never again drift between the two paths (exactly
    // how --show-queue / --tap-command / --render-interp each went missing from one
    // path before). Values come from the "openbw" prefs; the sun/point/bloom/post-gl
    // adb TEST hooks honour an intent override first, falling back to the pref
    // (absent on a normal launch -> identical to a plain pref read).
    private void appendCommonRenderAudioUiArgs(java.util.ArrayList<String> args) {
        android.content.SharedPreferences dev = getSharedPreferences("openbw", MODE_PRIVATE);
        // The profile's identity portrait (loading card, score screen). The launcher
        // stores the id it is showing ("activePortrait": also the rolled one in random
        // mode), so a resumed save uses the CURRENT profile portrait too.
        { int pp = dev.getInt("activePortrait", -1); if (pp >= 0) { args.add("--player-portrait"); args.add(String.valueOf(pp)); } }
        // HD visual controls (Settings > Graphics).
        args.add("--emissive");      args.add(dev.getBoolean("emissiveGlow", true) ? "1" : "0");
        args.add("--soft-shadows");  args.add(String.valueOf(dev.getInt("shadowSoft", 0)));
        { boolean sun = getIntent().getBooleanExtra("sunShadow", dev.getBoolean("sunShadow", false));
          if (sun) { args.add("--sun-shadow"); args.add("1");
            args.add("--sun-shadow-len"); args.add(String.valueOf(getIntent().getIntExtra("sunShadowLen", dev.getInt("sunShadowLen", 40))));
            args.add("--sun-shadow-sign"); args.add(String.valueOf(getIntent().getIntExtra("sunShadowSign", dev.getInt("sunShadowSign", 1)))); } }
        { boolean pl = getIntent().getBooleanExtra("pointLights", dev.getBoolean("pointLights", false));
          if (pl) { args.add("--point-lights"); args.add("1");
            args.add("--light-strength"); args.add(String.valueOf(getIntent().getIntExtra("lightStrength", dev.getInt("lightStrength", 100)))); } }
        { int bloomPct = getIntent().getIntExtra("bloomStrength", dev.getInt("bloomLevel", 0));
          if (bloomPct > 0) { args.add("--bloom"); args.add(String.valueOf(bloomPct * 2)); } }
        // Per-category audio submixes (Settings > Sound): 0..100 factors on --volume.
        args.add("--sfx-volume");    args.add(String.valueOf(dev.getInt("sfxVolume", 100)));
        args.add("--voice-volume");  args.add(String.valueOf(dev.getInt("voiceVolume", 100)));
        args.add("--ui-volume");     args.add(String.valueOf(dev.getInt("uiVolume", 100)));
        args.add("--voice-freq");    args.add(String.valueOf(dev.getInt("voiceFreq", 100)));
        // Interface controls (Settings > Sąsaja / Valdymas).
        args.add("--show-fps");      args.add(dev.getBoolean("showFps", true) ? "1" : "0");
        args.add("--show-apm");      args.add(dev.getBoolean("showApm", false) ? "1" : "0");
        args.add("--health-always"); args.add(dev.getBoolean("healthBarsAlways", false) ? "1" : "0");
        args.add("--camera-speed");  args.add(String.valueOf(dev.getInt("cameraSpeed", 100)));
        args.add("--tap-command");   args.add(dev.getBoolean("tapCommand", true) ? "1" : "0");
        args.add("--render-interp"); args.add(dev.getBoolean("renderInterp", true) ? "1" : "0");
        args.add("--zoom-buttons");  args.add(dev.getBoolean("showZoomButtons", false) ? "1" : "0");
        // HUD style (Settings > Interface): v2 "Klasikinis" (default) or v3 "Lietimo".
        // adb test hooks: --es hudStyle v3, --ei hud3Template 1 override for one launch.
        { String hs = getIntent().getStringExtra("hudStyle");
          if (hs == null) hs = dev.getString("hudStyle", "v3");
          args.add("--hud"); args.add(hs);
          args.add("--hud3-template"); args.add(String.valueOf(getIntent().getIntExtra("hud3Template", dev.getInt("hud3Template", 0))));
          args.add("--hud3-dock");     args.add(String.valueOf(dev.getInt("hud3Dock", 0)));
          args.add("--hud3-left");     args.add(dev.getBoolean("hud3Left", false) ? "1" : "0");
          args.add("--hud3-scale");    args.add(String.valueOf(dev.getInt("hud3Scale", 100)));
          args.add("--hud3-wheel-ms"); args.add(String.valueOf(dev.getInt("hud3WheelMs", 300)));
          args.add("--hud3-pen");      args.add(dev.getString("hud3Pen", "wheel"));
          args.add("--hud3-vibrate");  args.add(dev.getBoolean("hud3Vibrate", true) ? "1" : "0");
          args.add("--hud3-labels");   args.add(dev.getBoolean("hud3Labels", false) ? "1" : "0");
          args.add("--hud3-allgroups"); args.add(dev.getBoolean("hud3AllGroups", false) ? "1" : "0");
          args.add("--hud3-latch");    args.add(dev.getBoolean("hud3Latch", false) ? "1" : "0");
          args.add("--density");       args.add(String.valueOf(Math.round(getResources().getDisplayMetrics().density * 1000))); }
        if (dev.getBoolean("devNoFog", false)) { args.add("--no-fog"); args.add("1"); }
        args.add("--resource-counts"); args.add(dev.getBoolean("resourceCounts", true) ? "1" : "0");
        args.add("--equip-grey");    args.add(dev.getBoolean("equipGrey", true) ? "1" : "0");
        args.add("--saturation");    args.add(dev.getBoolean("showSaturation", true) ? "1" : "0");
        // SP shader pipeline (GLES2): sharpen + real GPU bloom + SE colour-grade. The
        // "sharpenFilter" pref (default on) runs AUTO (-1 -> Adreno sharpen / Mali
        // present); Bloom>0 or Grade>0 also forces the GL stage on. adb hooks override.
        { int glBloom = getIntent().getIntExtra("bloomStrength", dev.getInt("bloomLevel", 0));
          int glGrade = getIntent().getIntExtra("gradeStrength", dev.getInt("gradeLevel", 0));
          boolean pipeOn = dev.getBoolean("sharpenFilter", true) || getIntent().getBooleanExtra("postGl", false) || glBloom > 0 || glGrade > 0;
          if (pipeOn) {
            args.add("--post-gl");
            int pe = getIntent().getIntExtra("postEffect", dev.getBoolean("sharpenFilter", true) ? -1 : 0);
            args.add("--post-effect"); args.add(String.valueOf(pe));
            int ps = getIntent().getIntExtra("postStrength", dev.getInt("sharpenLevel", 35));
            args.add("--post-strength"); args.add(String.valueOf(ps));
            if (glBloom > 0) { args.add("--gl-bloom"); args.add(String.valueOf(glBloom)); }
            if (glGrade > 0) { args.add("--grade"); args.add(String.valueOf(glGrade)); }
          } }
        // SC:R (Remastered) CASC asset source + HD graphics tier (only bite with CASC).
        { String scr = dev.getString("scrCascDir", "");
          if (scr != null && !scr.isEmpty()) { args.add("--casc"); args.add(scr); } }
        // adb test hook: `--es gfxTier sd|hd2|full` overrides the saved tier for this launch only.
        { String tierX = getIntent().getStringExtra("gfxTier");
          args.add("--gfx-tier");    args.add(tierX != null ? tierX : dev.getString("gfxTier", "hd2")); }
        args.add("--scale-filter");  args.add("linear");
        // In-game screens we author (briefing, pause menu) follow the launcher language.
        S.setLanguage(dev.getString("lang", S.EN));
        args.add("--lang");          args.add(S.language());
    }

    // App versionCode (the manifest build number), used to stamp/gate disk saves.
    private int appVersionCode() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionCode;
        } catch (Throwable t) {
            return 0;
        }
    }

    // Read the launch argv embedded in a .sav container header (see ui.h
    // ui_functions::disk_save). Layout (little-endian): u32 magic("SAVS"),
    // u32 version, u32 frame, u32 map_id, u32 engine, u32 nameLen+name,
    // u32 argsLen+args. The args blob is '\n'-joined. Returns null on any
    // mismatch/short read so the caller can fall back to a normal launch.
    private String[] readSaveArgs(String path) {
        java.io.RandomAccessFile f = null;
        try {
            f = new java.io.RandomAccessFile(path, "r");
            byte[] hdr = new byte[24];
            if (f.read(hdr) != 24) return null;
            if (leU32(hdr, 0) != 0x53564153L) return null;   // "SAVS"
            if (leU32(hdr, 4) != 1L) return null;            // container_version
            long nameLen = leU32(hdr, 20);                   // header[5] = name length
            f.seek(24 + nameLen);
            byte[] al = new byte[4];
            if (f.read(al) != 4) return null;
            long argsLen = leU32(al, 0);
            if (argsLen <= 0 || argsLen > 1 << 20) return argsLen == 0 ? new String[0] : null;
            byte[] ab = new byte[(int) argsLen];
            if (f.read(ab) != argsLen) return null;
            String joined = new String(ab, "UTF-8");
            return joined.isEmpty() ? new String[0] : joined.split("\n");
        } catch (Throwable t) {
            return null;
        } finally {
            if (f != null) try { f.close(); } catch (Throwable ignored) {}
        }
    }

    private static long leU32(byte[] b, int o) {
        return (b[o] & 0xffL) | ((b[o + 1] & 0xffL) << 8) | ((b[o + 2] & 0xffL) << 16) | ((b[o + 3] & 0xffL) << 24);
    }

    private boolean isReplay;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (staleLaunch(savedInstanceState)) return;
        // A resumed disk save (EXTRA_LOAD_SAVE) carries its map inside the .sav
        // container args, not as an intent extra, so the map/campaign checks below
        // would misread it as a replay and stamp the replay-only top-right EXIT
        // overlay onto a normal game. It is a melee game, not a replay -- exclude it.
        isReplay = getIntent().getStringExtra(EXTRA_MAP) == null
                && getIntent().getStringExtra(EXTRA_MAP_MPQ) == null
                && getIntent().getStringExtra(EXTRA_CAMPAIGN) == null
                && getIntent().getStringExtra(EXTRA_LOAD_SAVE) == null;
        // Replay watching is hands-off for minutes at a time.
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        try {
            installPalmRejection();
            installPenHover();
        } catch (Throwable ignored) {
        }
        try {
            addOverlayControls();
        } catch (Throwable ignored) {
        }
        try {
            startLobbyPolling(); // shows the LAN lobby overlay while native is in lobby state
        } catch (Throwable ignored) {
        }
    }

    @Override
    public void onBackPressed() {
        // SDL consumes the hardware Back key as an AC_BACK key event before this
        // is called, so the native side handles Back-in-game (opens the pause
        // menu). If we do get here (e.g. a replay), return to the launcher.
        exitToLauncher();
    }

    // OpenBWActivity runs in its own ":game" process (see the manifest). The
    // native SDL + engine state cannot be recreated in a live process, so
    // exiting simply finishes this activity and kills the game process; the
    // launcher (SetupActivity, in the main process) is still on the back
    // stack and resumes automatically. The next game gets a fresh process.
    private void exitToLauncher() {
        finish();
        System.exit(0);
    }

    // HUD v3 haptics, called from the native side (android_main hud3_vibrate_fn).
    // kind: 0 tick (the wheel opens / a slot changes), 1 confirm (a command went
    // out), 2 error (refused). Settings > Interface can switch it off.
    @SuppressWarnings("deprecation")
    public void vibrate(int kind) {
        try {
            View v = getWindow().getDecorView();
            int c = kind == 1 ? android.view.HapticFeedbackConstants.CONFIRM
                  : kind == 2 ? android.view.HapticFeedbackConstants.REJECT
                  : android.view.HapticFeedbackConstants.CLOCK_TICK;
            if (android.os.Build.VERSION.SDK_INT < 30 && kind != 0) c = android.view.HapticFeedbackConstants.VIRTUAL_KEY;
            final int fc = c;
            v.post(() -> v.performHapticFeedback(fc, android.view.HapticFeedbackConstants.FLAG_IGNORE_VIEW_SETTING));
        } catch (Throwable ignored) {
        }
    }

    // v0.125: the HUD's version label in the launcher's own font and size (the
    // launcher draws "open1161HD v..." with the system font at 10 sp, Kit.TEXT2).
    // The engine only has its bitmap font, so the label is rendered here once and
    // handed over as raw ARGB ("VLBL" w h pixels); the HUD blits it in its corner.
    private String writeVersionLabel(String text) {
        try {
            android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
            p.setTextSize(android.util.TypedValue.applyDimension(android.util.TypedValue.COMPLEX_UNIT_SP, 10,
                    getResources().getDisplayMetrics()));
            p.setColor(0xFF7F9486);
            android.graphics.Paint.FontMetrics fm = p.getFontMetrics();
            int w = (int) Math.ceil(p.measureText(text)) + 2;
            int h = (int) Math.ceil(fm.descent - fm.ascent);
            if (w <= 0 || h <= 0 || w > 1024 || h > 256) return null;
            android.graphics.Bitmap bmp = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888);
            new android.graphics.Canvas(bmp).drawText(text, 1, -fm.ascent, p);
            int[] px = new int[w * h];
            bmp.getPixels(px, 0, w, 0, 0, w, h);
            java.nio.ByteBuffer bb = java.nio.ByteBuffer.allocate(12 + px.length * 4).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            bb.putInt(0x4c424c56).putInt(w).putInt(h);
            for (int v : px) bb.putInt(v);
            java.io.File f = new java.io.File(getCacheDir(), "verlabel.bin");
            java.io.FileOutputStream os = new java.io.FileOutputStream(f);
            try { os.write(bb.array()); } finally { os.close(); }
            return f.getAbsolutePath();
        } catch (Throwable t) {
            return null;
        }
    }

    // v0.124.003: a game may only be started by the launcher, never re-started by
    // Android from a remembered intent. Two cases used to drop the player straight
    // into a (new, from-scratch) game with no launcher:
    //   - the activity is being RE-created (savedInstanceState != null): the game
    //     process was killed in the background, the engine state is gone, and
    //     Android replays the old intent;
    //   - the game activity is the ROOT of its task: it was started directly
    //     (adb `am start`, used for device tests), so the app icon resumes that
    //     task and Android replays its intent every time.
    // Both now go to the launcher instead. Device tests keep a door: a direct
    // start is honoured ONCE per `--el directToken <unique number>`; the same
    // token replayed from history is stale.
    private boolean staleLaunch(Bundle savedInstanceState) {
        boolean stale = savedInstanceState != null;
        if (!stale && isTaskRoot()) {
            long tok = getIntent().getLongExtra("directToken", 0L);
            android.content.SharedPreferences p = getSharedPreferences("openbw", MODE_PRIVATE);
            if (tok == 0L || tok == p.getLong("directTokenUsed", 0L)) stale = true;
            else p.edit().putLong("directTokenUsed", tok).commit();
        }
        if (!stale) return false;
        try {
            if (isTaskRoot()) startActivity(new Intent(this, SetupActivity.class));
        } catch (Throwable ignored) {
        }
        finish();
        System.exit(0);
        return true;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // Kills only the ":game" process; the launcher process survives.
        System.exit(0);
    }

    // While the S-Pen is in contact, finger touches are almost always the
    // resting palm. Samsung delivers the pen as a separate stylus event
    // stream, so palm rejection is just swallowing finger events during
    // (and shortly after) stylus contact before they reach SDL.
    private long stylusActiveUntilMs = 0;
    private boolean spenButtonHeld = false;

    // The S-Pen barrel button acts as a momentary "queue" modifier (like Shift
    // in the desktop game): held while issuing an order, the order is appended
    // to the unit's queue instead of replacing it. Samsung reports the barrel
    // button in getButtonState() as BUTTON_STYLUS_PRIMARY (newer) or
    // BUTTON_SECONDARY (older). We translate press/release into Left-Shift key
    // events, which the native side maps to the queue modifier.
    private static final int SPEN_BARREL_MASK =
            MotionEvent.BUTTON_STYLUS_PRIMARY | MotionEvent.BUTTON_SECONDARY;

    private void updateSpenButton(MotionEvent ev) {
        boolean pressed = (ev.getButtonState() & SPEN_BARREL_MASK) != 0;
        // Never leave the modifier stuck: once the pen fully lifts, clear it.
        int action = ev.getActionMasked();
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            pressed = false;
        }
        if (pressed == spenButtonHeld) return;
        spenButtonHeld = pressed;
        if (pressed) SDLActivity.onNativeKeyDown(KEYCODE_SHIFT_LEFT);
        else SDLActivity.onNativeKeyUp(KEYCODE_SHIFT_LEFT);
    }

    // HUD v3: the S-Pen in the AIR. While the pen hovers, its side button opens the
    // command wheel under the pen; moving highlights a slot; letting the button go
    // picks it; leaving the hover range cancels. Hover events never reach the touch
    // listener, so they are read here and handed to native as a share of the surface
    // (1/10000), then passed on to SDL's own listener (hover = mouse motion, which
    // the card tooltips use). Needs Samsung's "Open Air command with Pen button"
    // off; if the button is not delivered while hovering, the "button + touch" path
    // in the touch listener still works.
    private static native void nativePenAir(int inRange, int xFrac, int yFrac, int button);

    private void installPenHover() {
        final View.OnGenericMotionListener sdl = getMotionListener();
        mSurface.setOnGenericMotionListener((View v, MotionEvent ev) -> {
            try {
                int tool = ev.getToolType(0);
                if (tool == MotionEvent.TOOL_TYPE_STYLUS || tool == MotionEvent.TOOL_TYPE_ERASER) {
                    int action = ev.getActionMasked();
                    boolean hover = action == MotionEvent.ACTION_HOVER_ENTER || action == MotionEvent.ACTION_HOVER_MOVE
                            || action == MotionEvent.ACTION_BUTTON_PRESS || action == MotionEvent.ACTION_BUTTON_RELEASE;
                    boolean exit = action == MotionEvent.ACTION_HOVER_EXIT;
                    if (hover || exit) {
                        int w = v.getWidth(), h = v.getHeight();
                        boolean button = (ev.getButtonState() & SPEN_BARREL_MASK) != 0;
                        if (w > 0 && h > 0) nativePenAir(exit ? 0 : 1, (int) (ev.getX() * 10000 / w), (int) (ev.getY() * 10000 / h), button ? 1 : 0);
                    }
                }
            } catch (Throwable ignored) {
            }
            return sdl != null && sdl.onGenericMotion(v, ev);
        });
    }

    private void installPalmRejection() {
        final View.OnTouchListener sdl = (View.OnTouchListener) mSurface;
        mSurface.setOnTouchListener((View v, MotionEvent ev) -> {
            boolean stylus = false;
            for (int i = 0; i < ev.getPointerCount(); ++i) {
                int t = ev.getToolType(i);
                if (t == MotionEvent.TOOL_TYPE_STYLUS || t == MotionEvent.TOOL_TYPE_ERASER) {
                    stylus = true;
                    break;
                }
            }
            long now = SystemClock.uptimeMillis();
            if (stylus) {
                stylusActiveUntilMs = now + 300;
                updateSpenButton(ev);
                return sdl.onTouch(v, ev);
            }
            if (now < stylusActiveUntilMs) return true; // palm - swallow
            return sdl.onTouch(v, ev);
        });
    }

    private void addOverlayControls() {
        if (mLayout == null) return;

        // F10: the replay player's EXIT is in the native control bar (ui.h
        // draw_replay_bar, rbtn_exit -> the same request_exit as Android Back). The
        // Java top-right EXIT overlay of v0.83.029 is gone: it duplicated that
        // button and sat over the native "VISION" chip. Games use the in-HUD MENU.

        // Playback transport (rewind / seek / pause / speed / vision) now lives
        // in the native RGBA control bar (ui.h draw_replay_ctrl, v0.83.019). The
        // old Java overlay buttons were removed to avoid a duplicated control
        // row -- the native bar reuses the same replay state.
    }

    private int dp(int v) {
        return Math.round(getResources().getDisplayMetrics().density * v);
    }

    // --- v0.41 Android-overlay LAN lobby ----------------------------------
    // The native side owns the LAN connection + authoritative lobby state and
    // publishes it as JSON; we poll it and render a launcher-style lobby over
    // the game surface, pushing ready/leave actions back to native.
    private static native boolean nativeLobbyActive();
    private static native String nativeLobbyStateJson();
    private static native void nativeLobbySetReady(boolean ready);
    private static native void nativeLobbyLeave();

    private View lobbyOverlay;
    private LinearLayout lobbyPlayers;
    private TextView lobbyTitle, lobbyMap, lobbyStatusTv;
    private Button lobbyReadyBtn;
    private boolean lobbyReadyState;
    private Handler lobbyHandler;
    private static final String[] LOBBY_RACE = {"Zerg", "Terran", "Protoss", "?", "?", "Random"};

    private void startLobbyPolling() {
        lobbyHandler = new Handler(Looper.getMainLooper());
        final Runnable poll = new Runnable() {
            @Override public void run() {
                try {
                    if (nativeLobbyActive()) {
                        if (lobbyOverlay == null) showLobbyOverlay();
                        updateLobbyOverlay(nativeLobbyStateJson());
                    } else if (lobbyOverlay != null) {
                        hideLobbyOverlay();
                    }
                } catch (Throwable ignored) {}
                if (lobbyHandler != null) lobbyHandler.postDelayed(this, 300);
            }
        };
        lobbyHandler.postDelayed(poll, 400);
    }

    private void showLobbyOverlay() {
        if (mLayout == null) return;
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(0xFF0B0E16);
        panel.setPadding(dp(28), dp(36), dp(28), dp(24));
        panel.setClickable(true); // swallow touches so they don't hit the game

        lobbyTitle = new TextView(this);
        lobbyTitle.setTextColor(0xFFFFD040);
        lobbyTitle.setTextSize(28);
        panel.addView(lobbyTitle);
        lobbyMap = new TextView(this);
        lobbyMap.setTextColor(0xFFC8D8E8);
        lobbyMap.setPadding(0, dp(12), 0, 0);
        panel.addView(lobbyMap);
        lobbyStatusTv = new TextView(this);
        lobbyStatusTv.setTextColor(0xFF70C8FF);
        lobbyStatusTv.setPadding(0, dp(8), 0, dp(16));
        panel.addView(lobbyStatusTv);
        TextView hdr = new TextView(this);
        hdr.setText(S.t("Žaidėjai:"));
        hdr.setTextColor(0xFF90C8E0);
        panel.addView(hdr);
        lobbyPlayers = new LinearLayout(this);
        lobbyPlayers.setOrientation(LinearLayout.VERTICAL);
        lobbyPlayers.setPadding(0, dp(6), 0, 0);
        panel.addView(lobbyPlayers);

        LinearLayout btns = new LinearLayout(this);
        btns.setOrientation(LinearLayout.HORIZONTAL);
        btns.setPadding(0, dp(24), 0, 0);
        lobbyReadyBtn = new Button(this);
        lobbyReadyBtn.setAllCaps(false);
        lobbyReadyBtn.setText(S.t("Pasiruošti"));
        lobbyReadyBtn.setOnClickListener(v -> nativeLobbySetReady(!lobbyReadyState));
        btns.addView(lobbyReadyBtn);
        Button leave = new Button(this);
        leave.setAllCaps(false);
        leave.setText(S.t("Palikti"));
        leave.setOnClickListener(v -> nativeLobbyLeave());
        btns.addView(leave);
        panel.addView(btns);

        mLayout.addView(panel, new RelativeLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        lobbyOverlay = panel;
        lobbyReadyState = false;
    }

    private void updateLobbyOverlay(String json) {
        if (lobbyOverlay == null || json == null) return;
        try {
            JSONObject o = new JSONObject(json);
            // P4: SCBW2 badge -- how many house-rules the host negotiated for this session
            // (a joiner sees it before start, so nobody is surprised by non-vanilla rules).
            int qol = o.optInt("qol", 0);
            String badge = qol != 0 ? "    SCBW2 ▸ " + Integer.bitCount(qol) : "";
            lobbyTitle.setText((o.optBoolean("host") ? "LAN Lobby (Host)" : "LAN Lobby") + badge);
            lobbyMap.setText(S.t("Žemėlapis:  ") + o.optString("map"));
            lobbyStatusTv.setText(o.optString("status"));
            JSONArray players = o.optJSONArray("players");
            lobbyPlayers.removeAllViews();
            for (int i = 0; players != null && i < players.length(); i++) {
                JSONObject p = players.getJSONObject(i);
                int race = p.optInt("race", -1);
                boolean rdy = p.optBoolean("ready");
                boolean loc = p.optBoolean("local");
                if (loc) lobbyReadyState = rdy;
                TextView row = new TextView(this);
                row.setTextSize(19);
                row.setTextColor(loc ? 0xFFFFE090 : 0xFFE0E0E0);
                row.setPadding(0, dp(7), 0, dp(7));
                String raceS = (race >= 0 && race < 6) ? LOBBY_RACE[race] : "?";
                row.setText(S.t("Slot ") + (p.optInt("slot") + 1) + "    " + p.optString("name")
                        + "    [" + raceS + "]" + (loc ? "   (tu)" : "") + (rdy ? "    ✔ " + S.t("pasiruošęs") : ""));
                lobbyPlayers.addView(row);
            }
            lobbyReadyBtn.setText(lobbyReadyState ? "✔ " + S.t("Pasiruošęs") : S.t("Pasiruošti"));
            lobbyReadyBtn.setBackgroundColor(lobbyReadyState ? 0xFF206030 : 0xFF403018);
        } catch (Throwable ignored) {}
    }

    private void hideLobbyOverlay() {
        if (lobbyOverlay != null && mLayout != null) mLayout.removeView(lobbyOverlay);
        lobbyOverlay = null;
    }

}
