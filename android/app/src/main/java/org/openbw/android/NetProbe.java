package org.openbw.android;

/**
 * LAN (v0.6) spike: calls into the native asio/sync layer to confirm the
 * networking transport builds and can bind a socket on this device. Thrown
 * away once real multiplayer lands.
 */
public final class NetProbe {
    public static boolean run(int port) {
        try {
            System.loadLibrary("main");
            return probe(port);
        } catch (Throwable t) {
            return false;
        }
    }

    private static native boolean probe(int port);
}
