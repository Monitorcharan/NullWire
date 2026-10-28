package com.nullwire.receiver;

/** Thin JNI bridge to the C++ (NDK/AAudio) engine that does all real-time audio work. */
public final class AudioEngine {
    static {
        System.loadLibrary("nullwire_native");
    }

    private AudioEngine() {}

    public static native void setSessionToken(int token);
    public static native boolean startNativePlayback(int port);
    public static native void stopNativePlayback();
    public static native void setBassBoost(float gainDb);
    public static native void setTrebleBoost(float gainDb);
    public static native float getPlaybackRms();
    public static native long getPlaybackPacketCount();
    public static native void setWifiAcceleration(boolean enabled);
    public static native float getBufferedMs();
    public static native long getTrimmedFrames();
    public static native long getUnderrunCount();
    public static native void resetBuffers();
}
