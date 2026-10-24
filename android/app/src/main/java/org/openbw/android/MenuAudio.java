package org.openbw.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.MediaPlayer;
import android.media.SoundPool;
import android.os.Handler;
import android.os.Looper;

import java.io.File;

/**
 * Launcher sound: the original main-menu
 * music and interface clicks, played from the user's own archives.
 *
 * Nothing is bundled: the files are extracted (already decompressed by the MPQ
 * reader) into the app's cache the first time and played from there. The title
 * music lives only in the optional CD archives (StarCraft.mpq / BroodWar.mpq);
 * without them the menu is simply silent. Static, so recreate() (language or
 * profile change) does not restart the music.
 */
final class MenuAudio {
    private MenuAudio() {}

    static final int CLICK = 0, OPEN = 1, CLOSE = 2, LOCK = 3, BUZZ = 4, COUNTDOWN = 5;
    private static final String[] SOUND_FILES = {
        "sound\\glue\\mousedown2.wav", "sound\\glue\\swishin.wav", "sound\\glue\\swishout.wav",
        "sound\\glue\\swishlock.wav", "sound\\misc\\buzz.wav", "sound\\glue\\countdown.wav"};
    private static final String MUSIC_FILE = "music\\title.wav";

    private static boolean libOk;
    static {
        try { System.loadLibrary("openbw_minimap"); libOk = true; }
        catch (Throwable t) { libOk = false; }
    }
    private static native boolean nativeExtractFile(String gameDir, String path, String out);

    private static final Handler ui = new Handler(Looper.getMainLooper());
    private static SharedPreferences prefs;
    private static Context app;
    private static boolean preparing, prepared;
    private static SoundPool pool;
    private static final int[] soundId = new int[SOUND_FILES.length];
    private static File musicFile;
    private static MediaPlayer music;
    private static boolean wantMusic;          // the launcher is in the foreground
    private static float fade = 1f;
    private static final Runnable pauseLater = MenuAudio::pauseNow;

    static boolean musicAvailable() { return musicFile != null; }
    static boolean ready() { return prepared; }

    private static boolean muted() { return prefs != null && prefs.getBoolean("menuMute", false); }
    private static boolean musicOn() { return prefs != null && prefs.getBoolean("menuMusic", true) && !muted(); }
    private static boolean soundsOn() { return prefs != null && prefs.getBoolean("menuSounds", true) && !muted(); }
    private static float master() { return prefs == null ? 0.5f : prefs.getInt("volume", 50) / 100f; }

    /** Extracts (once) and loads the menu audio in the background. Safe to call on every launcher start. */
    static synchronized void prepare(Context ctx, final File gameDir, SharedPreferences pr) {
        prefs = pr;
        app = ctx.getApplicationContext();
        if (prepared || preparing || !libOk || gameDir == null || !new File(gameDir, "StarDat.mpq").exists()) return;
        preparing = true;
        new Thread(() -> {
            try {
                File dir = new File(app.getCacheDir(), "audio");
                dir.mkdirs();
                // the cache follows the archives it was cut from
                File sd = new File(gameDir, "StarDat.mpq"), cd = new File(gameDir, "StarCraft.mpq"), bw = new File(gameDir, "BroodWar.mpq");
                String stamp = sd.length() + ":" + sd.lastModified() + ":" + cd.length() + ":" + bw.length();
                File stampFile = new File(dir, "stamp.txt");
                String have = "";
                try (java.io.FileInputStream in = new java.io.FileInputStream(stampFile)) {
                    byte[] b = new byte[128];
                    int n = in.read(b);
                    if (n > 0) have = new String(b, 0, n, "UTF-8");
                } catch (Exception ignored) {}
                if (!stamp.equals(have)) {
                    File[] old = dir.listFiles();
                    if (old != null) for (File f : old) f.delete();
                }
                SoundPool sp = new SoundPool.Builder().setMaxStreams(4).setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build()).build();
                for (int i = 0; i != SOUND_FILES.length; ++i) {
                    File f = new File(dir, "s" + i + ".wav");
                    if (!f.isFile() || f.length() == 0) nativeExtractFile(gameDir.getAbsolutePath(), SOUND_FILES[i], f.getAbsolutePath());
                    if (f.isFile() && f.length() > 0) soundId[i] = sp.load(f.getAbsolutePath(), 1);
                }
                File m = new File(dir, "title.wav");
                if (!m.isFile() || m.length() == 0) nativeExtractFile(gameDir.getAbsolutePath(), MUSIC_FILE, m.getAbsolutePath());
                try (java.io.FileOutputStream o = new java.io.FileOutputStream(stampFile)) { o.write(stamp.getBytes("UTF-8")); } catch (Exception ignored) {}
                final File mf = m.isFile() && m.length() > 0 ? m : null;
                ui.post(() -> {
                    pool = sp;
                    musicFile = mf;
                    prepared = true;
                    preparing = false;
                    if (wantMusic) startMusic();
                });
            } catch (Throwable t) {
                preparing = false;
            }
        }, "menu-audio").start();
    }

    /** The launcher came to the foreground. */
    static void resume() {
        ui.removeCallbacks(pauseLater);
        wantMusic = true;
        startMusic();
    }

    /** The launcher left the foreground (a game starts, the app is backgrounded). Slightly
     *  delayed, so a recreate() -- pause immediately followed by resume -- leaves no gap. */
    static void pause() {
        wantMusic = false;
        ui.removeCallbacks(pauseLater);
        ui.postDelayed(pauseLater, 350);
    }

    private static void pauseNow() {
        if (wantMusic) return;
        try { if (music != null && music.isPlaying()) music.pause(); } catch (Throwable ignored) {}
        abandonFocus();
    }

    /** Prefs changed (switches, volume sliders, quick mute). */
    static void apply() {
        if (!musicOn()) {
            try { if (music != null && music.isPlaying()) music.pause(); } catch (Throwable ignored) {}
        } else if (wantMusic) startMusic();
        setMusicVolume();
    }

    private static void setMusicVolume() {
        if (music == null || prefs == null) return;
        float v = master() * prefs.getInt("musicVolume", 40) / 100f * fade;
        try { music.setVolume(v, v); } catch (Throwable ignored) {}
    }

    private static void startMusic() {
        if (!prepared || musicFile == null || !musicOn() || !wantMusic) return;
        try {
            if (music == null) {
                MediaPlayer mp = new MediaPlayer();
                mp.setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build());
                mp.setDataSource(musicFile.getAbsolutePath());
                mp.setLooping(true);
                mp.prepare();
                music = mp;
                fade = 0f;
            }
            if (!music.isPlaying()) {
                requestFocus();
                setMusicVolume();
                music.start();
                if (fade < 1f) fadeIn();
            }
        } catch (Throwable t) {
            try { if (music != null) music.release(); } catch (Throwable ignored) {}
            music = null;
            musicFile = null;      // unplayable on this device: stay silent
        }
    }

    private static void fadeIn() {
        ui.postDelayed(new Runnable() {
            @Override public void run() {
                fade = Math.min(1f, fade + 0.1f);
                setMusicVolume();
                if (fade < 1f && music != null) ui.postDelayed(this, 50);
            }
        }, 50);
    }

    /** A short interface sound; silent when the sounds are off or not loaded yet. */
    static void play(int which) {
        if (pool == null || !soundsOn() || which < 0 || which >= soundId.length || soundId[which] == 0) return;
        float v = master() * prefs.getInt("uiVolume", 100) / 100f;
        try { pool.play(soundId[which], v, v, 1, 0, 1f); } catch (Throwable ignored) {}
    }

    // ---- unit voice lines (a poked portrait answers) ----
    private static final java.util.Map<String, Integer> voiceIds = new java.util.HashMap<>();
    private static String pendingVoice;
    private static int lastVoiceStream;

    /** Plays one voice line from the archives (extracted to the cache on first use). */
    static void voice(final File gameDir, final String mpqPath) {
        if (pool == null || !soundsOn() || !libOk || gameDir == null || mpqPath == null) return;
        Integer id = voiceIds.get(mpqPath);
        if (id != null) { playVoice(id); return; }
        pendingVoice = mpqPath;
        final SoundPool sp = pool;
        new Thread(() -> {
            try {
                File f = new File(new File(app.getCacheDir(), "audio"), "v" + Integer.toHexString(mpqPath.hashCode()) + ".wav");
                if (!f.isFile() || f.length() == 0) nativeExtractFile(gameDir.getAbsolutePath(), mpqPath, f.getAbsolutePath());
                if (!f.isFile() || f.length() == 0) return;
                sp.setOnLoadCompleteListener((p, sampleId, status) -> ui.post(() -> {
                    // play only the line that was asked for last
                    Integer want = voiceIds.get(pendingVoice);
                    if (status == 0 && want != null && want == sampleId) playVoice(sampleId);
                }));
                final int sid = sp.load(f.getAbsolutePath(), 1);
                ui.post(() -> voiceIds.put(mpqPath, sid));
            } catch (Throwable ignored) {}
        }, "menu-voice").start();
    }

    private static void playVoice(int sampleId) {
        if (pool == null || !soundsOn()) return;
        float v = master() * prefs.getInt("voiceVolume", 100) / 100f;
        try {
            if (lastVoiceStream != 0) pool.stop(lastVoiceStream);     // one voice at a time
            lastVoiceStream = pool.play(sampleId, v, v, 2, 0, 1f);
        } catch (Throwable ignored) {}
    }

    // ---- audio focus: another app's sound pauses ours ----
    private static final AudioManager.OnAudioFocusChangeListener focus = change -> {
        if (change == AudioManager.AUDIOFOCUS_LOSS || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) {
            try { if (music != null && music.isPlaying()) music.pause(); } catch (Throwable ignored) {}
        } else if (change == AudioManager.AUDIOFOCUS_GAIN && wantMusic) {
            startMusic();
        }
    };
    @SuppressWarnings("deprecation")
    private static void requestFocus() {
        try {
            AudioManager am = (AudioManager) app.getSystemService(Context.AUDIO_SERVICE);
            if (am != null) am.requestAudioFocus(focus, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
        } catch (Throwable ignored) {}
    }
    @SuppressWarnings("deprecation")
    private static void abandonFocus() {
        try {
            AudioManager am = (AudioManager) app.getSystemService(Context.AUDIO_SERVICE);
            if (am != null) am.abandonAudioFocus(focus);
        } catch (Throwable ignored) {}
    }
}
