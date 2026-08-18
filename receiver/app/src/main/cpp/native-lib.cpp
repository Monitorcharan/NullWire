#include <jni.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <android/log.h>
#include <aaudio/AAudio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/resource.h>

#define TAG "NullWireNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 2;
constexpr size_t RING_BUFFER_SIZE = 65536;
constexpr double PI = 3.14159265358979323846;
constexpr uint16_t PACKET_MAGIC = 0x574E;
constexpr size_t PACKET_HEADER_SIZE = 8;
constexpr size_t MAX_UDP_PACKET = 8192;

// ── Audiophile Biquad Filter ────────────────────────────────────────────────
class BiquadFilter {
public:
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1[2] = {0, 0}, x2[2] = {0, 0};
    double y1[2] = {0, 0}, y2[2] = {0, 0};

    void SetLowShelf(double freq, double gainDb, double sampleRate = 48000.0, double Q = 0.7071) {
        if (std::abs(gainDb) < 0.05) {
            b0 = 1; b1 = 0; b2 = 0; a1 = 0; a2 = 0;
            return;
        }
        double A = std::pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * PI * freq / sampleRate;
        double cosw0 = std::cos(w0);
        double sinw0 = std::sin(w0);
        double alpha = sinw0 / (2.0 * Q);
        double a0 = (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * std::sqrt(A) * alpha;
        if (std::abs(a0) < 1e-12) return;

        b0 = (A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * std::sqrt(A) * alpha)) / a0;
        b1 = (2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0)) / a0;
        b2 = (A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * std::sqrt(A) * alpha)) / a0;
        a1 = (-2.0 * ((A - 1.0) + (A + 1.0) * cosw0)) / a0;
        a2 = ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * std::sqrt(A) * alpha) / a0;
    }

    void SetHighShelf(double freq, double gainDb, double sampleRate = 48000.0, double Q = 0.7071) {
        if (std::abs(gainDb) < 0.05) {
            b0 = 1; b1 = 0; b2 = 0; a1 = 0; a2 = 0;
            return;
        }
        double A = std::pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * PI * freq / sampleRate;
        double cosw0 = std::cos(w0);
        double sinw0 = std::sin(w0);
        double alpha = sinw0 / (2.0 * Q);
        double a0 = (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * std::sqrt(A) * alpha;
        if (std::abs(a0) < 1e-12) return;

        b0 = (A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * std::sqrt(A) * alpha)) / a0;
        b1 = (-2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0)) / a0;
        b2 = (A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * std::sqrt(A) * alpha)) / a0;
        a1 = (2.0 * ((A - 1.0) - (A + 1.0) * cosw0)) / a0;
        a2 = ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * std::sqrt(A) * alpha) / a0;
    }

    inline double Process(double inSample, int ch) {
        if (ch < 0 || ch > 1) return inSample;
        double in = inSample + 1e-25;
        double out = b0 * in + b1 * x1[ch] + b2 * x2[ch] - a1 * y1[ch] - a2 * y2[ch];
        if (!std::isfinite(out)) out = 0.0;
        x2[ch] = x1[ch];
        x1[ch] = in;
        y2[ch] = y1[ch];
        y1[ch] = out;
        return out;
    }
};

// ── Studio Dynamic Master Limiter & Soft Saturator ───────────────────────────
inline double StudioMasterLimiter(double sample) {
    constexpr double threshold = 0.94;
    double absS = std::abs(sample);
    if (absS <= threshold) return sample;
    double excess = absS - threshold;
    double compressed = threshold + (1.0 - threshold) * std::tanh(excess / (1.0 - threshold));
    return (sample > 0) ? compressed : -compressed;
}

// ── Zero-Chirp Adaptive Jitter Ring Buffer with Smooth Decay Concealment ─────
class ZeroChirpJitterBuffer {
private:
    int16_t buffer[RING_BUFFER_SIZE]{};
    std::atomic<size_t> writeHead{0};
    std::atomic<size_t> readHead{0};
    float decayL = 0.0f;
    float decayR = 0.0f;
    bool isPrimed = false;
    static constexpr size_t TARGET_PREROLL = 768; // ~8ms of absorption

public:
    void Clear() {
        writeHead.store(0, std::memory_order_relaxed);
        readHead.store(0, std::memory_order_relaxed);
        std::fill_n(buffer, RING_BUFFER_SIZE, (int16_t)0);
        decayL = 0.0f;
        decayR = 0.0f;
        isPrimed = false;
    }

    size_t AvailableRead() const {
        size_t w = writeHead.load(std::memory_order_acquire);
        size_t r = readHead.load(std::memory_order_relaxed);
        if (w >= r) return w - r;
        return RING_BUFFER_SIZE - (r - w);
    }

    size_t AvailableWrite() const {
        return (RING_BUFFER_SIZE - 1) - AvailableRead();
    }

    size_t Write(const int16_t* data, size_t count) {
        if (!data || count == 0) return 0;
        size_t avail = AvailableWrite();
        size_t toWrite = std::min(count, avail);
        if (toWrite == 0) return 0;

        size_t w = writeHead.load(std::memory_order_relaxed);
        for (size_t i = 0; i < toWrite; i++) {
            buffer[(w + i) % RING_BUFFER_SIZE] = data[i];
        }
        writeHead.store((w + toWrite) % RING_BUFFER_SIZE, std::memory_order_release);
        return toWrite;
    }

    size_t Read(int16_t* outData, size_t count) {
        if (!outData || count == 0) return 0;
        size_t avail = AvailableRead();

        // 1. Initial Pre-roll Check (smooth buffer filling)
        if (!isPrimed) {
            if (avail < TARGET_PREROLL) {
                std::fill_n(outData, count, (int16_t)0);
                return 0;
            }
            isPrimed = true;
        }

        if (avail == 0) {
            isPrimed = false;
            std::fill_n(outData, count, (int16_t)0);
            return 0;
        }

        // 2. Read available valid frames
        size_t toRead = std::min(count, avail);
        if (toRead & 1) toRead -= 1; // Stereo alignment
        size_t r = readHead.load(std::memory_order_relaxed);

        for (size_t i = 0; i < toRead; i++) {
            outData[i] = buffer[(r + i) % RING_BUFFER_SIZE];
        }

        if (toRead >= 2) {
            decayL = (float)outData[toRead - 2];
            decayR = (float)outData[toRead - 1];
            readHead.store((r + toRead) % RING_BUFFER_SIZE, std::memory_order_release);
        }

        // 3. Smooth Zero-Chirp Continuous Fade
        if (toRead < count) {
            size_t missingFrames = (count - toRead) / 2;
            for (size_t f = 0; f < missingFrames; f++) {
                decayL *= 0.94f;
                decayR *= 0.94f;
                outData[toRead + f * 2] = (int16_t)decayL;
                outData[toRead + f * 2 + 1] = (int16_t)decayR;
            }
        }

        return toRead;
    }
};

class NativeEngine {
public:
    ZeroChirpJitterBuffer ringBuffer;
    std::atomic<bool> isPlaybackRunning{false};
    std::atomic<bool> isMicRunning{false};
    std::atomic<uint64_t> playbackPackets{0};
    std::atomic<uint64_t> micPackets{0};
    std::atomic<float> playbackRms{0.0f};
    std::atomic<float> micRms{0.0f};

    std::atomic<float> bassGainDb{0.0f};
    std::atomic<float> trebleGainDb{0.0f};
    float lastBass = 0.0f;
    float lastTreble = 0.0f;
    BiquadFilter bassFilter;
    BiquadFilter trebleFilter;

    std::atomic<uint32_t> sessionToken{0};
    std::atomic<uint32_t> pinnedSourceIp{0};
    std::atomic<bool> wifiAccelEnabled{true};

    int playbackSocket = -1;
    int micSocket = -1;

    AAudioStream* playbackStream = nullptr;
    AAudioStream* micStream = nullptr;

    std::thread networkRecvThread;
    std::thread networkMicThread;
};

static NativeEngine g_NativeEngine;

static bool IsPrivateOrLinkLocalIpv4(uint32_t addrNetOrder) {
    uint32_t a = ntohl(addrNetOrder);
    uint8_t b0 = (uint8_t)((a >> 24) & 0xFF);
    uint8_t b1 = (uint8_t)((a >> 16) & 0xFF);
    if (b0 == 10) return true;
    if (b0 == 192 && b1 == 168) return true;
    if (b0 == 172 && b1 >= 16 && b1 <= 31) return true;
    if (b0 == 127) return true;
    if (b0 == 169 && b1 == 254) return true;
    return false;
}

aaudio_data_callback_result_t AudioPlaybackCallback(
        AAudioStream* stream,
        void* userData,
        void* audioData,
        int32_t numFrames) {

    if (!userData || !audioData || numFrames <= 0) {
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    auto* engine = static_cast<NativeEngine*>(userData);
    int16_t* outputBuffer = static_cast<int16_t*>(audioData);
    size_t totalSamples = (size_t)numFrames * CHANNELS;

    engine->ringBuffer.Read(outputBuffer, totalSamples);

    float curBass = engine->bassGainDb.load(std::memory_order_relaxed);
    float curTreble = engine->trebleGainDb.load(std::memory_order_relaxed);

    if (std::abs(curBass) > 0.05f || std::abs(curTreble) > 0.05f) {
        if (curBass != engine->lastBass) {
            engine->bassFilter.SetLowShelf(80.0, (double)curBass, SAMPLE_RATE, 0.7071);
            engine->lastBass = curBass;
        }
        if (curTreble != engine->lastTreble) {
            engine->trebleFilter.SetHighShelf(12000.0, (double)curTreble, SAMPLE_RATE, 0.7071);
            engine->lastTreble = curTreble;
        }

        for (int32_t f = 0; f < numFrames; f++) {
            double l_raw = outputBuffer[f * 2] / 32768.0;
            double r_raw = outputBuffer[f * 2 + 1] / 32768.0;

            double l_out = engine->bassFilter.Process(l_raw, 0);
            double r_out = engine->bassFilter.Process(r_raw, 1);

            l_out = engine->trebleFilter.Process(l_out, 0);
            r_out = engine->trebleFilter.Process(r_out, 1);

            outputBuffer[f * 2] = (int16_t)(std::clamp(l_out, -1.0, 1.0) * 32767.0);
            outputBuffer[f * 2 + 1] = (int16_t)(std::clamp(r_out, -1.0, 1.0) * 32767.0);
        }
    }

    double sumSquares = 0.0;
    for (size_t s = 0; s < totalSamples; s += 4) {
        double v = (double)outputBuffer[s];
        sumSquares += v * v;
    }
    double rms = std::sqrt(sumSquares / (double)(totalSamples / 4));
    engine->playbackRms.store((float)std::min(1.0, rms / 10000.0), std::memory_order_relaxed);

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

extern "C" {

JNIEXPORT void JNICALL
Java_com_nullwire_receiver_AudioEngine_setSessionToken(JNIEnv* env, jobject thiz, jint token) {
    g_NativeEngine.sessionToken.store((uint32_t)token, std::memory_order_release);
    g_NativeEngine.pinnedSourceIp.store(0, std::memory_order_release);
}

JNIEXPORT jboolean JNICALL
Java_com_nullwire_receiver_AudioEngine_startNativePlayback(JNIEnv* env, jobject thiz, jint port) {
    if (g_NativeEngine.isPlaybackRunning.load()) return JNI_TRUE;
    if (port < 1024 || port > 65535) {
        LOGE("Invalid playback port %d", port);
        return JNI_FALSE;
    }

    g_NativeEngine.ringBuffer.Clear();
    g_NativeEngine.pinnedSourceIp.store(0, std::memory_order_release);

    g_NativeEngine.playbackSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_NativeEngine.playbackSocket < 0) {
        LOGE("Failed to create playback socket");
        return JNI_FALSE;
    }

    int reuse = 1;
    setsockopt(g_NativeEngine.playbackSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int rcvBuf = 1048576; // 1MB receive socket buffer
    setsockopt(g_NativeEngine.playbackSocket, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons((uint16_t)port);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_NativeEngine.playbackSocket, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        LOGE("Failed to bind playback socket on port %d", port);
        close(g_NativeEngine.playbackSocket);
        g_NativeEngine.playbackSocket = -1;
        return JNI_FALSE;
    }

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(g_NativeEngine.playbackSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    AAudioStreamBuilder* builder = nullptr;
    AAudio_createStreamBuilder(&builder);
    if (!builder) {
        close(g_NativeEngine.playbackSocket);
        g_NativeEngine.playbackSocket = -1;
        return JNI_FALSE;
    }

    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, CHANNELS);
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(builder, AudioPlaybackCallback, &g_NativeEngine);

    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &g_NativeEngine.playbackStream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK || !g_NativeEngine.playbackStream) {
        LOGE("Failed to open AAudio stream: %s", AAudio_convertResultToText(result));
        close(g_NativeEngine.playbackSocket);
        g_NativeEngine.playbackSocket = -1;
        g_NativeEngine.playbackStream = nullptr;
        return JNI_FALSE;
    }

    int32_t burstFrames = AAudioStream_getFramesPerBurst(g_NativeEngine.playbackStream);
    if (burstFrames > 0) {
        int32_t targetBuf = std::max(burstFrames * 3, 576);
        AAudioStream_setBufferSizeInFrames(g_NativeEngine.playbackStream, targetBuf);
    }

    result = AAudioStream_requestStart(g_NativeEngine.playbackStream);
    if (result != AAUDIO_OK) {
        LOGE("Failed to start AAudio stream: %s", AAudio_convertResultToText(result));
        AAudioStream_close(g_NativeEngine.playbackStream);
        g_NativeEngine.playbackStream = nullptr;
        close(g_NativeEngine.playbackSocket);
        g_NativeEngine.playbackSocket = -1;
        return JNI_FALSE;
    }

    g_NativeEngine.isPlaybackRunning.store(true);
    g_NativeEngine.playbackPackets.store(0);

    g_NativeEngine.networkRecvThread = std::thread([]() {
        setpriority(PRIO_PROCESS, 0, -19);
        std::vector<uint8_t> recvBuf(MAX_UDP_PACKET);

        while (g_NativeEngine.isPlaybackRunning.load(std::memory_order_acquire)) {
            sockaddr_in fromAddr{};
            socklen_t fromLen = sizeof(fromAddr);
            ssize_t bytes = recvfrom(
                g_NativeEngine.playbackSocket,
                recvBuf.data(),
                recvBuf.size(),
                0,
                (struct sockaddr*)&fromAddr,
                &fromLen);

            if (bytes < (ssize_t)PACKET_HEADER_SIZE) continue;
            if ((size_t)bytes > MAX_UDP_PACKET) continue;

            uint16_t magic = 0;
            memcpy(&magic, recvBuf.data(), 2);
            if (magic != PACKET_MAGIC) continue;

            size_t pcmBytes = (size_t)bytes - PACKET_HEADER_SIZE;
            if (pcmBytes < 4 || (pcmBytes % 4) != 0) continue;

            const int16_t* pcm = reinterpret_cast<const int16_t*>(recvBuf.data() + PACKET_HEADER_SIZE);
            size_t samples = pcmBytes / sizeof(int16_t);
            g_NativeEngine.ringBuffer.Write(pcm, samples);
            g_NativeEngine.playbackPackets.fetch_add(1, std::memory_order_relaxed);
        }
    });

    LOGI("AAudio engine started on port %d", port);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_nullwire_receiver_AudioEngine_stopNativePlayback(JNIEnv* env, jobject thiz) {
    if (!g_NativeEngine.isPlaybackRunning.load()) return;

    g_NativeEngine.isPlaybackRunning.store(false, std::memory_order_release);

    if (g_NativeEngine.playbackSocket >= 0) {
        shutdown(g_NativeEngine.playbackSocket, SHUT_RDWR);
        close(g_NativeEngine.playbackSocket);
        g_NativeEngine.playbackSocket = -1;
    }

    if (g_NativeEngine.networkRecvThread.joinable()) {
        g_NativeEngine.networkRecvThread.join();
    }

    if (g_NativeEngine.playbackStream) {
        AAudioStream_requestStop(g_NativeEngine.playbackStream);
        AAudioStream_close(g_NativeEngine.playbackStream);
        g_NativeEngine.playbackStream = nullptr;
    }

    g_NativeEngine.playbackRms.store(0.0f);
    g_NativeEngine.pinnedSourceIp.store(0);
    LOGI("AAudio engine stopped");
}

JNIEXPORT void JNICALL
Java_com_nullwire_receiver_AudioEngine_setBassBoost(JNIEnv* env, jobject thiz, jfloat gainDb) {
    float clamped = std::clamp(gainDb, 0.0f, 12.0f);
    g_NativeEngine.bassGainDb.store(clamped, std::memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_com_nullwire_receiver_AudioEngine_setTrebleBoost(JNIEnv* env, jobject thiz, jfloat gainDb) {
    float clamped = std::clamp(gainDb, 0.0f, 10.0f);
    g_NativeEngine.trebleGainDb.store(clamped, std::memory_order_relaxed);
}

JNIEXPORT jfloat JNICALL
Java_com_nullwire_receiver_AudioEngine_getPlaybackRms(JNIEnv* env, jobject thiz) {
    return g_NativeEngine.playbackRms.load(std::memory_order_relaxed);
}

JNIEXPORT jlong JNICALL
Java_com_nullwire_receiver_AudioEngine_getPlaybackPacketCount(JNIEnv* env, jobject thiz) {
    return (jlong)g_NativeEngine.playbackPackets.load(std::memory_order_relaxed);
}

JNIEXPORT void JNICALL
Java_com_nullwire_receiver_AudioEngine_setWifiAcceleration(JNIEnv* env, jobject thiz, jboolean enabled) {
    g_NativeEngine.wifiAccelEnabled.store(enabled == JNI_TRUE, std::memory_order_release);
    int prio = (enabled == JNI_TRUE) ? -19 : 0;
    setpriority(PRIO_PROCESS, 0, prio);
}

} // extern "C"
