package org.openbw.android;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Replay header metadata read by the native side (replay_info_jni.cpp).
 * Loading libmain.so here does not start SDL or the game: the library only
 * runs its static initializers until SDLActivity calls SDL_main.
 */
public final class ReplayInfo {

    public static final class Player {
        public final String name;
        public final int race; // 0 zerg, 1 terran, 2 protoss
        public final boolean computer;

        Player(String name, int race, boolean computer) {
            this.name = name;
            this.race = race;
            this.computer = computer;
        }

        public String raceLetter() {
            switch (race) {
                case 0: return "Z";
                case 1: return "T";
                case 2: return "P";
                default: return "?";
            }
        }
    }

    public final String error;
    public final int frames;
    public final String gameName;
    public final String mapName;
    public final boolean hasComputer;
    public final List<Player> players = new ArrayList<>();

    private ReplayInfo(String error, int frames, String gameName, String mapName, boolean hasComputer) {
        this.error = error;
        this.frames = frames;
        this.gameName = gameName;
        this.mapName = mapName;
        this.hasComputer = hasComputer;
    }

    /** Frames to game time at "fastest" speed (~42 ms per frame). */
    public String duration() {
        int totalSeconds = (int) (frames * 42L / 1000L);
        return String.format(Locale.ROOT, "%d:%02d", totalSeconds / 60, totalSeconds % 60);
    }

    public static ReplayInfo parse(String path) {
        String json;
        try {
            System.loadLibrary("main");
            json = parseNative(path);
        } catch (Throwable t) {
            return new ReplayInfo(String.valueOf(t.getMessage()), 0, "", "", false);
        }
        try {
            JSONObject o = new JSONObject(json);
            if (o.has("error")) {
                return new ReplayInfo(o.getString("error"), 0, "", "", false);
            }
            ReplayInfo r = new ReplayInfo(null,
                    o.getInt("frames"),
                    o.optString("gameName", ""),
                    o.optString("mapName", ""),
                    o.optBoolean("hasComputer", false));
            JSONArray ps = o.optJSONArray("players");
            if (ps != null) {
                for (int i = 0; i < ps.length(); ++i) {
                    JSONObject p = ps.getJSONObject(i);
                    r.players.add(new Player(p.optString("name", "?"), p.optInt("race", 3), p.optBoolean("computer", false)));
                }
            }
            return r;
        } catch (Exception e) {
            return new ReplayInfo("bad json: " + e.getMessage(), 0, "", "", false);
        }
    }

    private static native String parseNative(String path);
}
