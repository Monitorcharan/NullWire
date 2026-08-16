package com.nullwire.receiver

object AudioEngine {
    init {
        System.loadLibrary("nullwire_native")
    }

    external fun setSessionToken(token: Int)
    external fun startNativePlayback(port: Int): Boolean
    external fun stopNativePlayback()
    external fun setBassBoost(gainDb: Float)
    external fun setTrebleBoost(gainDb: Float)
    external fun getPlaybackRms(): Float
    external fun getPlaybackPacketCount(): Long
}
