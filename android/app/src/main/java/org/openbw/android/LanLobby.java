package org.openbw.android;

import android.os.Handler;
import android.os.Looper;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Launcher-side LAN lobby (#6). The whole lobby lives in the launcher: the host
 * runs a small host-authoritative TCP server, joiners connect, and slots / races
 * / ready-state sync live over the wire while everyone is still in the launcher.
 * Only when the host starts (all ready) does each device launch the actual game
 * (net melee), where the native lockstep layer takes over.
 *
 * Wire protocol: newline-delimited JSON.
 *   join -> host  : {"hello":"<userId>","race":<int>,"portrait":<int>}
 *                   ("portrait" = the profile's portdata id; unknown keys are ignored
 *                   by older builds on either side, a missing one reads as -1)
 *   host -> all   : {"state":{...}}   (authoritative snapshot, see State.toJson)
 *   join -> host  : {"race":<int>} | {"ready":<bool>} | {"leave":true}
 *   host -> all   : {"start":true}
 * UDP discovery (port {@link #PORT}): a probe "OPENBW_DISCOVER" is answered with
 *   "OPENBW\t<port>\t<map>\t<used>/<total>\t<ver>\t<mods>\t<speed>\t<host name>\t<host portrait>" so joiners can
 *   browse live hosts (same format the native responder used).
 */
public class LanLobby {
    public static final int PORT = 6112;

    public static class Slot {
        public String userId = "?";
        public int race = -1;      // -1 = random, 0/1/2 concrete
        public int color = -1;     // -1 = auto (slot index); 0..7 = chosen BW colour
        public int portrait = -1;  // portdata id of the player's profile portrait; -1 = an older build
        public boolean ready;
        public boolean host;
    }

    public static class State {
        public String map = "";
        public int speed = 100;
        public int target = 2;
        public boolean started;
        public final List<Slot> slots = new ArrayList<>();

        JSONObject toJson() {
            try {
                JSONObject o = new JSONObject();
                o.put("map", map);
                o.put("speed", speed);
                o.put("target", target);
                o.put("started", started);
                JSONArray arr = new JSONArray();
                for (Slot s : slots) {
                    JSONObject j = new JSONObject();
                    j.put("userId", s.userId);
                    j.put("race", s.race);
                    j.put("color", s.color);
                    j.put("portrait", s.portrait);
                    j.put("ready", s.ready);
                    j.put("host", s.host);
                    arr.put(j);
                }
                o.put("slots", arr);
                return o;
            } catch (Exception e) {
                return new JSONObject();
            }
        }

        static State fromJson(JSONObject o) {
            State s = new State();
            s.map = o.optString("map", "");
            s.speed = o.optInt("speed", 100);
            s.target = o.optInt("target", 2);
            s.started = o.optBoolean("started", false);
            JSONArray arr = o.optJSONArray("slots");
            if (arr != null) {
                for (int i = 0; i < arr.length(); i++) {
                    JSONObject j = arr.optJSONObject(i);
                    if (j == null) continue;
                    Slot sl = new Slot();
                    sl.userId = j.optString("userId", "?");
                    sl.race = j.optInt("race", -1);
                    sl.color = j.optInt("color", -1);
                    sl.portrait = j.optInt("portrait", -1);
                    sl.ready = j.optBoolean("ready", false);
                    sl.host = j.optBoolean("host", false);
                    s.slots.add(sl);
                }
            }
            return s;
        }

        public boolean allReady() {
            // A game needs at least 2 players; the host may start with fewer than
            // the target if not everyone showed up, as long as those present are ready.
            if (slots.size() < 2) return false;
            for (Slot s : slots) if (!s.ready) return false;
            return true;
        }
    }

    public interface Listener {
        /** Authoritative lobby snapshot (host and joiners). */
        void onState(State s);
        /** All ready + host started: launch the game with these parameters. */
        void onStart(boolean isHost, String hostIp, int myRace, int speed, int players);
        void onError(String msg);
        void onClosed();
    }

    private final Handler ui = new Handler(Looper.getMainLooper());
    // All socket writes go through this so they never run on the UI thread
    // (button clicks call setReady/setRace) - Android throws
    // NetworkOnMainThreadException otherwise, which silently killed every update.
    private final java.util.concurrent.ExecutorService io =
            java.util.concurrent.Executors.newSingleThreadExecutor();
    private final Listener listener;
    private volatile boolean running = true;
    private boolean isHost;
    private String myUserId = "Player";
    private int myPortrait = -1;

    /** The local profile's portrait id; call before startHost / startJoin. */
    public void setLocalPortrait(int id) { myPortrait = id; }
    private String hostIp;

    private final String appVersion, modsCsv;

    public LanLobby(Listener l, String appVersion, String modsCsv) {
        this.listener = l;
        this.appVersion = appVersion == null ? "" : appVersion;
        this.modsCsv = modsCsv == null || modsCsv.isEmpty() ? "-" : modsCsv;
    }

    // ==================== HOST ====================
    private ServerSocket server;
    private DatagramSocket udp;
    private final List<ClientConn> clients = new ArrayList<>();
    private final State state = new State();

    private static class ClientConn {
        Socket sock;
        OutputStream out;
        Slot slot;
    }

    public synchronized void startHost(String userId, int race, String map, int players, int speed) {
        isHost = true;
        myUserId = userId;
        hostIp = "127.0.0.1";
        state.map = map;
        state.target = Math.max(2, Math.min(8, players));
        state.speed = speed;
        Slot h = new Slot();
        h.userId = userId;
        h.race = race;
        h.portrait = myPortrait;
        h.host = true;
        state.slots.add(h);
        new Thread(this::hostAcceptLoop, "lobby-accept").start();
        new Thread(this::hostUdpLoop, "lobby-udp").start();
        emitState();
    }

    private void hostAcceptLoop() {
        try {
            server = new ServerSocket();
            server.setReuseAddress(true);
            server.bind(new java.net.InetSocketAddress((InetAddress) null, PORT));
        } catch (Exception e) {
            fail("Nepavyko sukurti lobby. Ar kitas žaidimas jau hostina?");
            return;
        }
        while (running) {
            try {
                Socket s = server.accept();
                handleNewClient(s);
            } catch (Exception e) {
                break;
            }
        }
    }

    private synchronized void handleNewClient(Socket s) {
        try {
            // Lobby full? refuse.
            if (state.started || state.slots.size() >= state.target) {
                try { s.close(); } catch (Exception ignored) {}
                return;
            }
            BufferedReader r = new BufferedReader(new InputStreamReader(s.getInputStream(), "UTF-8"));
            OutputStream o = s.getOutputStream();
            String line = r.readLine();
            if (line == null) { s.close(); return; }
            JSONObject hello = new JSONObject(line);
            Slot sl = new Slot();
            sl.userId = hello.optString("hello", "Guest");
            sl.race = hello.optInt("race", -1);
            sl.portrait = hello.optInt("portrait", -1);
            ClientConn c = new ClientConn();
            c.sock = s;
            c.out = o;
            c.slot = sl;
            clients.add(c);
            state.slots.add(sl);
            new Thread(() -> hostClientReadLoop(c, r), "lobby-client").start();
            emitState();
            broadcastState();
        } catch (Exception e) {
            try { s.close(); } catch (Exception ignored) {}
        }
    }

    private void hostClientReadLoop(ClientConn c, BufferedReader r) {
        try {
            String line;
            while (running && (line = r.readLine()) != null) {
                JSONObject m = new JSONObject(line);
                synchronized (this) {
                    if (m.has("race")) c.slot.race = m.optInt("race", -1);
                    if (m.has("color") && colorFree(m.optInt("color", -1), c.slot)) c.slot.color = m.optInt("color", -1);
                    if (m.has("ready")) c.slot.ready = m.optBoolean("ready", false);
                    if (m.optBoolean("leave", false)) break;
                }
                emitState();
                broadcastState();
            }
        } catch (Exception ignored) {
        } finally {
            removeClient(c);
        }
    }

    private synchronized void removeClient(ClientConn c) {
        clients.remove(c);
        state.slots.remove(c.slot);
        try { c.sock.close(); } catch (Exception ignored) {}
        if (running && !state.started) {
            emitState();
            broadcastState();
        }
    }

    private synchronized void broadcastState() {
        String msg;
        try {
            msg = new JSONObject().put("state", state.toJson()).toString() + "\n";
        } catch (Exception e) {
            return;
        }
        final byte[] b;
        try { b = msg.getBytes("UTF-8"); } catch (Exception e) { return; }
        final List<ClientConn> targets = new ArrayList<>(clients);
        io.submit(() -> {
            for (ClientConn c : targets) {
                try { c.out.write(b); c.out.flush(); } catch (Exception e) { /* read loop cleans up */ }
            }
        });
    }

    /** Host: change own race. */
    public synchronized void setHostRace(int race) {
        if (state.slots.isEmpty()) return;
        state.slots.get(0).race = race;
        emitState();
        broadcastState();
    }

    /** True if `color` is free (not already taken by a slot other than `self`).
     * -1 (auto) is always free. The host authoritatively rejects a taken colour so
     * two players can never end up the same colour. */
    synchronized boolean colorFree(int color, Slot self) {
        if (color < 0) return true;
        for (Slot s : state.slots) if (s != self && s.color == color) return false;
        return true;
    }

    /** Host: change own colour (rejected if already taken). */
    public synchronized void setHostColor(int color) {
        if (state.slots.isEmpty()) return;
        Slot me = state.slots.get(0);
        if (!colorFree(color, me)) return;
        me.color = color;
        emitState();
        broadcastState();
    }

    /** Host: recolour ANY slot (the host may set other players' colours). Rejected if
     * the colour is already held by a different slot. */
    public synchronized void setSlotColor(int slotIndex, int color) {
        if (!isHost || slotIndex < 0 || slotIndex >= state.slots.size()) return;
        Slot s = state.slots.get(slotIndex);
        if (!colorFree(color, s)) return;
        s.color = color;
        emitState();
        broadcastState();
    }

    /** Host: toggle own ready. */
    public synchronized void setHostReady(boolean ready) {
        if (state.slots.isEmpty()) return;
        state.slots.get(0).ready = ready;
        emitState();
        broadcastState();
    }

    /** Host: begin the game (only meaningful when everyone is ready). */
    public synchronized void startGame() {
        if (!isHost || state.started) return;
        if (!state.allReady()) return;
        state.started = true;
        broadcastStart();
        // Give clients a moment to receive the start + release their sockets,
        // then the host tears down the lobby sockets so the native TCP server
        // can bind the same port.
        int race = state.slots.get(0).race;
        // Launch with the ACTUAL number of players present, not the chosen target -
        // otherwise the native side waits forever for a peer that never joined.
        int players = state.slots.size();
        int speed = state.speed;
        ui.postDelayed(() -> {
            shutdownSockets();
            listener.onStart(true, null, race, speed, players);
        }, 400);
    }

    private synchronized void broadcastStart() {
        String msg;
        try {
            msg = new JSONObject().put("start", true).toString() + "\n";
        } catch (Exception e) {
            return;
        }
        final byte[] b;
        try { b = msg.getBytes("UTF-8"); } catch (Exception e) { return; }
        final List<ClientConn> targets = new ArrayList<>(clients);
        // Synchronous on the io thread: it must land before we tear down sockets.
        try {
            io.submit(() -> {
                for (ClientConn c : targets) {
                    try { c.out.write(b); c.out.flush(); } catch (Exception ignored) {}
                }
            }).get(1, java.util.concurrent.TimeUnit.SECONDS);
        } catch (Exception ignored) {}
    }

    private void hostUdpLoop() {
        try {
            udp = new DatagramSocket(null);
            udp.setReuseAddress(true);
            udp.bind(new java.net.InetSocketAddress(PORT));
        } catch (Exception e) {
            return; // discovery is best-effort
        }
        byte[] buf = new byte[64];
        while (running) {
            try {
                DatagramPacket in = new DatagramPacket(buf, buf.length);
                udp.receive(in);
                String msg = new String(in.getData(), 0, in.getLength(), "US-ASCII");
                if (msg.startsWith("OPENBW_DISCOVER")) {
                    String reply = discoveryReply();
                    byte[] rb = reply.getBytes("UTF-8");
                    udp.send(new DatagramPacket(rb, rb.length, in.getAddress(), in.getPort()));
                }
            } catch (Exception e) {
                break;
            }
        }
    }

    private synchronized String discoveryReply() {
        int used = state.slots.size();
        int total = state.target;
        // Fields 7+ (host name, host portrait id) are appended: an older scanner reads
        // only what it knows, so the reply stays compatible both ways.
        Slot h = state.slots.isEmpty() ? null : state.slots.get(0);
        return "OPENBW\t" + PORT + "\t" + state.map + "\t" + used + "/" + total
                + "\t" + appVersion + "\t" + modsCsv + "\t" + state.speed
                + "\t" + (h == null ? "" : h.userId.replace('\t', ' ')) + "\t" + (h == null ? -1 : h.portrait);
    }

    // ==================== JOIN ====================
    private Socket joinSock;
    private OutputStream joinOut;

    public void startJoin(String userId, int race, String hostIp) {
        isHost = false;
        myUserId = userId;
        this.hostIp = hostIp;
        final int myRace = race;
        new Thread(() -> {
            try {
                joinSock = new Socket();
                joinSock.connect(new java.net.InetSocketAddress(hostIp, PORT), 8000);
                joinOut = joinSock.getOutputStream();
                BufferedReader r = new BufferedReader(new InputStreamReader(joinSock.getInputStream(), "UTF-8"));
                JSONObject hello = new JSONObject().put("hello", userId).put("race", myRace).put("portrait", myPortrait);
                sendJoin(hello.toString());
                String line;
                while (running && (line = r.readLine()) != null) {
                    JSONObject m = new JSONObject(line);
                    if (m.has("state")) {
                        State s = State.fromJson(m.getJSONObject("state"));
                        ui.post(() -> listener.onState(s));
                    } else if (m.optBoolean("start", false)) {
                        // Release our socket before the game claims the port.
                        try { joinSock.close(); } catch (Exception ignored) {}
                        ui.postDelayed(() -> listener.onStart(false, hostIp, myRace, 0, 0), 200);
                        return;
                    }
                }
                if (running) fail("Ryšys su hostu nutrūko.");
            } catch (Exception e) {
                fail("Nepavyko prisijungti prie hosto (" + hostIp + ").");
            }
        }, "lobby-join").start();
    }

    private void sendJoin(String jsonLine) {
        final String line = jsonLine + "\n";
        try {
            io.submit(() -> {
                try {
                    if (joinOut != null) {
                        joinOut.write(line.getBytes("UTF-8"));
                        joinOut.flush();
                    }
                } catch (Exception ignored) {}
            });
        } catch (Exception ignored) {
            // io shut down (leaving) - nothing to send.
        }
    }

    /** Joiner: change own race. */
    public void setJoinRace(int race) {
        try { sendJoin(new JSONObject().put("race", race).toString()); } catch (Exception ignored) {}
    }

    /** Joiner: request own colour (the host applies it only if still free). */
    public void setJoinColor(int color) {
        try { sendJoin(new JSONObject().put("color", color).toString()); } catch (Exception ignored) {}
    }

    /** Joiner: toggle own ready. */
    public void setJoinReady(boolean ready) {
        try { sendJoin(new JSONObject().put("ready", ready).toString()); } catch (Exception ignored) {}
    }

    // ==================== COMMON ====================
    public void close() {
        running = false;
        if (!isHost) {
            try { sendJoin(new JSONObject().put("leave", true).toString()); } catch (Exception ignored) {}
        }
        shutdownSockets();
        ui.post(listener::onClosed);
    }

    private void shutdownSockets() {
        try { if (server != null) server.close(); } catch (Exception ignored) {}
        try { if (udp != null) udp.close(); } catch (Exception ignored) {}
        try { if (joinSock != null) joinSock.close(); } catch (Exception ignored) {}
        synchronized (this) {
            for (ClientConn c : new ArrayList<>(clients)) {
                try { c.sock.close(); } catch (Exception ignored) {}
            }
        }
        io.shutdown();
    }

    private void emitState() {
        final State snap = snapshot();
        ui.post(() -> listener.onState(snap));
    }

    private synchronized State snapshot() {
        return State.fromJson(state.toJson());
    }

    private void fail(String msg) {
        if (!running) return;
        running = false;
        shutdownSockets();
        ui.post(() -> listener.onError(msg));
    }

    // ==================== DISCOVERY (client-side scan) ====================
    public static class HostInfo {
        public String ip = "";
        public String map = "";
        public int used, total, speed = 100;
        public String ver = "", mods = "", name = "";   // "" = an older host did not say
        public int portrait = -1;
    }

    /** Blocking UDP scan (call off the UI thread). Returns discovered hosts. */
    public static List<HostInfo> scan(List<InetAddress> broadcasts, int timeoutMs) {
        List<HostInfo> out = new ArrayList<>();
        DatagramSocket sock = null;
        try {
            sock = new DatagramSocket();
            sock.setBroadcast(true);
            sock.setSoTimeout(300);
            byte[] probe = "OPENBW_DISCOVER".getBytes("US-ASCII");
            for (InetAddress b : broadcasts) {
                try { sock.send(new DatagramPacket(probe, probe.length, b, PORT)); } catch (Exception ignored) {}
            }
            long deadline = System.currentTimeMillis() + timeoutMs;
            byte[] buf = new byte[160];
            while (System.currentTimeMillis() < deadline) {
                DatagramPacket in = new DatagramPacket(buf, buf.length);
                try { sock.receive(in); } catch (java.net.SocketTimeoutException te) { continue; }
                String msg = new String(in.getData(), 0, in.getLength(), "UTF-8");
                String[] p = msg.split("\t");
                if (p.length >= 2 && p[0].equals("OPENBW")) {
                    String ip = in.getAddress().getHostAddress();
                    boolean dup = false;
                    for (HostInfo h : out) if (h.ip.equals(ip)) { dup = true; break; }
                    if (dup) continue;
                    HostInfo h = new HostInfo();
                    h.ip = ip;
                    h.map = p.length >= 3 ? p[2] : "?";
                    if (p.length >= 4) {
                        String[] uc = p[3].split("/");
                        try { h.used = Integer.parseInt(uc[0]); } catch (Exception ignored) {}
                        try { if (uc.length > 1) h.total = Integer.parseInt(uc[1]); } catch (Exception ignored) {}
                    }
                    if (p.length >= 5) h.ver = p[4];
                    if (p.length >= 6) h.mods = p[5];
                    if (p.length >= 7) { try { h.speed = Integer.parseInt(p[6]); } catch (Exception ignored) {} }
                    if (p.length >= 8) h.name = p[7];
                    if (p.length >= 9) { try { h.portrait = Integer.parseInt(p[8].trim()); } catch (Exception ignored) {} }
                    out.add(h);
                }
            }
        } catch (Exception ignored) {
        } finally {
            if (sock != null) sock.close();
        }
        Collections.sort(out, (a, b) -> a.ip.compareTo(b.ip));
        return out;
    }
}
