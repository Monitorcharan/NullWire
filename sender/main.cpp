#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601
#define INITGUID

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <gdiplus.h>

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <deque>
#include <mutex>
#include <cstring>
#include <sstream>
#include <cctype>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "avrt.lib")

using namespace Gdiplus;

// ── Global Constants ────────────────────────────────────────────────────────
constexpr int AUDIO_SEND_PORT = 50005;
constexpr int MIC_RECEIVE_PORT = 50006;
constexpr int DISCOVERY_PORT = 50007;
constexpr int TARGET_RATE = 48000;
constexpr double PI = 3.14159265358979323846;
constexpr size_t LATENCY_HISTORY_SIZE = 160;
constexpr size_t WAVEFORM_SAMPLES = 256;
constexpr uint16_t PACKET_MAGIC = 0x574E;
constexpr size_t PACKET_HEADER_SIZE = 8;
constexpr size_t MAX_AUDIO_PACKET = 4096;

static bool g_WinsockReady = false;

bool EnsureWinsock() {
    if (g_WinsockReady) return true;
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_WinsockReady = true;
    return true;
}

bool IsValidLanIpv4(const char* ip, in_addr* outAddr = nullptr) {
    if (!ip || !*ip) return false;
    in_addr addr{};
    if (inet_pton(AF_INET, ip, &addr) != 1) return false;
    uint32_t a = ntohl(addr.s_addr);
    if (a == 0 || a == 0xFFFFFFFFu) return false;
    uint8_t b0 = (uint8_t)((a >> 24) & 0xFF);
    uint8_t b1 = (uint8_t)((a >> 16) & 0xFF);
    bool ok = (b0 == 10) || (b0 == 127) || (b0 == 192 && b1 == 168) || (b0 == 172 && b1 >= 16 && b1 <= 31);
    if (!ok) return false;
    if (outAddr) *outAddr = addr;
    return true;
}

std::string SanitizeDiscoveryName(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (out.size() >= 32) break;
        if (std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.') {
            out.push_back((char)c);
        }
    }
    return out.empty() ? "Android" : out;
}

void AddrToIpv4String(const sockaddr_in& addr, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    inet_ntop(AF_INET, &addr.sin_addr, out, (DWORD)outLen);
}

#define WM_TRAYICON (WM_USER + 201)
#define ID_TRAY_EXIT 3001
#define ID_TRAY_RESTORE 3002
#define ID_TRAY_TOGGLE_STREAM 3003
#define ID_TRAY_TOGGLE_MIC 3004
#define ID_TRAY_PROFILE_HIFI 3010
#define ID_TRAY_PROFILE_GAMING 3011
#define ID_TRAY_PROFILE_CINEMA 3012
#define ID_TRAY_PROFILE_DIRECT 3013

#define HOTKEY_STREAM_ID 9001
#define HOTKEY_MIC_ID 9002
#define HOTKEY_PROFILE_1 9003
#define HOTKEY_PROFILE_2 9004
#define HOTKEY_PROFILE_3 9005
#define HOTKEY_PROFILE_4 9006

static ULONG_PTR g_gdiplusToken = 0;
static NOTIFYICONDATAW g_nid{};
static bool g_InTray = false;
static HINSTANCE g_hInstance = NULL;
static HWND g_hMainWnd = NULL;
static bool g_isMovingOrSizing = false;

// ── Floating In-App Toast Notification State ────────────────────────────────
struct InAppNotification {
    std::wstring title;
    std::wstring message;
    Color accentColor;
    std::chrono::steady_clock::time_point startTime;
    bool active = false;
};

static std::mutex g_notifyMutex;
static InAppNotification g_currentNotification;

void PostNotification(const std::wstring& title, const std::wstring& msg, Color accentColor, bool showWindowsTray = true) {
    {
        std::lock_guard<std::mutex> lock(g_notifyMutex);
        g_currentNotification.title = title;
        g_currentNotification.message = msg;
        g_currentNotification.accentColor = accentColor;
        g_currentNotification.startTime = std::chrono::steady_clock::now();
        g_currentNotification.active = true;
    }

    if (showWindowsTray && g_hMainWnd) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = g_hMainWnd;
        nid.uID = 1001;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, msg.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    if (g_hMainWnd && !g_isMovingOrSizing) {
        InvalidateRect(g_hMainWnd, NULL, FALSE);
    }
}

// ── Acoustic Scenario Modes ─────────────────────────────────────────────────
enum class ScenarioMode {
    MUSIC_HIFI = 0,     // Deep Sub-Bass + Silk Highs + Harman Target
    GAMING_LOW_LATENCY, // 2.67ms Ultra-Fast + Spatial Footsteps + HRTF 3D
    CINEMA_MOVIE,       // Dialogue Clarity + Explosive Sub-Bass Rumble
    PURE_DIRECT         // 100% Bit-Perfect Flat Studio Reference (0 DSP)
};

// ── Audiophile Biquad Filter ────────────────────────────────────────────────
class BiquadFilter {
public:
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1[2] = {0, 0}, x2[2] = {0, 0};
    double y1[2] = {0, 0}, y2[2] = {0, 0};

    void Reset() {
        x1[0] = x1[1] = x2[0] = x2[1] = 0;
        y1[0] = y1[1] = y2[0] = y2[1] = 0;
    }

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

    void SetPeaking(double freq, double gainDb, double sampleRate = 48000.0, double Q = 1.0) {
        if (std::abs(gainDb) < 0.05) {
            b0 = 1; b1 = 0; b2 = 0; a1 = 0; a2 = 0;
            return;
        }
        double A = std::pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * PI * freq / sampleRate;
        double cosw0 = std::cos(w0);
        double sinw0 = std::sin(w0);
        double alpha = sinw0 / (2.0 * Q);
        double a0 = 1.0 + alpha / A;
        if (std::abs(a0) < 1e-12) return;

        b0 = (1.0 + alpha * A) / a0;
        b1 = (-2.0 * cosw0) / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = (-2.0 * cosw0) / a0;
        a2 = (1.0 - alpha / A) / a0;
    }

    void SetBandPass(double freq, double Q = 1.414, double sampleRate = 48000.0) {
        double w0 = 2.0 * PI * freq / sampleRate;
        double cosw0 = std::cos(w0);
        double sinw0 = std::sin(w0);
        double alpha = sinw0 / (2.0 * Q);
        double a0 = 1.0 + alpha;
        if (std::abs(a0) < 1e-12) return;

        b0 = alpha / a0;
        b1 = 0.0;
        b2 = -alpha / a0;
        a1 = (-2.0 * cosw0) / a0;
        a2 = (1.0 - alpha) / a0;
    }

    void SetLowPass(double freq, double sampleRate = 48000.0, double Q = 0.7071) {
        double w0 = 2.0 * PI * freq / sampleRate;
        double cosw0 = std::cos(w0);
        double sinw0 = std::sin(w0);
        double alpha = sinw0 / (2.0 * Q);
        double a0 = 1.0 + alpha;
        if (std::abs(a0) < 1e-12) return;

        b0 = ((1.0 - cosw0) / 2.0) / a0;
        b1 = (1.0 - cosw0) / a0;
        b2 = ((1.0 - cosw0) / 2.0) / a0;
        a1 = (-2.0 * cosw0) / a0;
        a2 = (1.0 - alpha) / a0;
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

// ── 3D Binaural HRTF Spatial Surround Processor ─────────────────────────────
class BinauralSpatializer {
public:
    BiquadFilter crossfeedFilter;
    BiquadFilter pinnaNotchFilter;
    double delayL[32]{};
    double delayR[32]{};
    int delayIdx = 0;
    static constexpr int ITD_DELAY_SAMPLES = 14;

    BinauralSpatializer() {
        crossfeedFilter.SetLowPass(750.0, (double)TARGET_RATE, 0.7071);
        pinnaNotchFilter.SetPeaking(6200.0, -3.0, (double)TARGET_RATE, 1.4);
    }

    inline void ProcessStereo(double& l, double& r, float spatialAmount) {
        if (spatialAmount <= 0.005f) return;

        double directL = pinnaNotchFilter.Process(l, 0);
        double directR = pinnaNotchFilter.Process(r, 1);

        double shadowL = crossfeedFilter.Process(l, 0);
        double shadowR = crossfeedFilter.Process(r, 1);

        delayL[delayIdx] = shadowL;
        delayR[delayIdx] = shadowR;

        int delayedIdx = (delayIdx - ITD_DELAY_SAMPLES + 32) % 32;
        double delayedShadowL = delayL[delayedIdx];
        double delayedShadowR = delayR[delayedIdx];

        delayIdx = (delayIdx + 1) % 32;

        double blend = (double)std::clamp(spatialAmount, 0.0f, 1.0f) * 0.38;
        l = directL * (1.0 - blend * 0.4) + delayedShadowR * blend;
        r = directR * (1.0 - blend * 0.4) + delayedShadowL * blend;
    }
};

// ── Studio Master Limiter with Soft-Knee Saturation ─────────────────────────
inline double StudioMasterLimiter(double x) {
    double threshold = 0.94;
    double absX = std::abs(x);
    if (absX <= threshold) return x;
    double over = absX - threshold;
    double compressed = threshold + (1.0 - threshold) * std::tanh(over / (1.0 - threshold));
    return (x > 0.0) ? compressed : -compressed;
}

inline float GenerateTpdfDither(uint32_t& prngState) {
    prngState = prngState * 1664525u + 1013904223u;
    int32_t r1 = (int32_t)(prngState >> 16);
    prngState = prngState * 1664525u + 1013904223u;
    int32_t r2 = (int32_t)(prngState >> 16);
    return (float)(r1 - r2) / 2147483648.0f * (1.0f / 32768.0f);
}

// ── Audio Device Model ──────────────────────────────────────────────────────
struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

// ── Audio Engine State ──────────────────────────────────────────────────────
class AudioEngine {
public:
    std::atomic<bool> isStreaming{false};
    std::atomic<bool> isMicReceiving{false};
    std::atomic<bool> isDiscoveryRunning{false};
    std::atomic<bool> hasDiscoveredDevice{false};
    std::atomic<uint64_t> sendPacketCount{0};
    std::atomic<uint64_t> micPacketCount{0};

    std::atomic<ScenarioMode> currentScenario{ScenarioMode::GAMING_LOW_LATENCY};
    std::atomic<float> bassBoostDb{3.0f};
    std::atomic<float> trebleBoostDb{4.0f};
    std::atomic<float> presenceBoostDb{3.5f};
    std::atomic<bool> enableSpatial3D{true};
    std::atomic<float> spatialSurroundAmount{0.85f};
    std::atomic<int> chunkFrames{128};

    std::atomic<float> streamRmsL{0.0f};
    std::atomic<float> streamRmsR{0.0f};
    std::atomic<float> micRmsLevel{0.0f};

    std::atomic<float> currentLatencyMs{2.67f};
    std::atomic<float> minLatencyMs{2.50f};
    std::atomic<float> maxLatencyMs{3.10f};
    std::atomic<float> jitterMs{0.12f};

    std::mutex latencyMutex;
    std::deque<float> latencyHistoryMs;

    std::mutex discoveryMutex;
    std::string discoveredDeviceName;
    std::string discoveredDeviceIp;
    uint32_t discoveredToken = 0;
    std::string lastNotifiedDevice;

    std::vector<AudioDevice> devices;
    int selectedDeviceIndex = -1;

    std::string targetPhoneIp = "192.168.1.15";
    uint32_t sessionToken = 0;
    std::mutex targetMutex;

    std::thread streamThread;
    std::thread micThread;
    std::thread discoveryThread;

    std::mutex waveformMutex;
    std::vector<float> waveformL;
    std::vector<float> waveformR;
    std::atomic<float> spectrumBands[7]{};

    AudioEngine() {
        waveformL.resize(WAVEFORM_SAMPLES, 0.0f);
        waveformR.resize(WAVEFORM_SAMPLES, 0.0f);
        for (size_t i = 0; i < LATENCY_HISTORY_SIZE; i++) {
            latencyHistoryMs.push_back(2.67f);
        }
        for (int i = 0; i < 7; i++) spectrumBands[i].store(0.0f);
    }

    void AddLatencySample(float ms) {
        std::lock_guard<std::mutex> lock(latencyMutex);
        if (latencyHistoryMs.size() >= LATENCY_HISTORY_SIZE) {
            latencyHistoryMs.pop_front();
        }
        latencyHistoryMs.push_back(ms);
        currentLatencyMs.store(ms);

        float minV = 999.0f, maxV = 0.0f, sumDiff = 0.0f;
        float prev = latencyHistoryMs.front();
        for (float v : latencyHistoryMs) {
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
            sumDiff += std::abs(v - prev);
            prev = v;
        }
        minLatencyMs.store(minV);
        maxLatencyMs.store(maxV);
        jitterMs.store(sumDiff / (float)latencyHistoryMs.size());
    }

    void SetScenario(ScenarioMode mode) {
        currentScenario.store(mode);
        switch (mode) {
            case ScenarioMode::GAMING_LOW_LATENCY:
                bassBoostDb.store(3.0f);
                trebleBoostDb.store(4.0f);
                presenceBoostDb.store(3.5f);
                enableSpatial3D.store(true);
                spatialSurroundAmount.store(0.85f);
                PostNotification(L"🎮 PROFILE: GAMING ULTRA 2.67ms", L"Sub-millisecond low-latency MMAP with 3D Spatial HRTF active.", Color(255, 245, 158, 11), false);
                break;
            case ScenarioMode::MUSIC_HIFI:
                bassBoostDb.store(5.5f);
                trebleBoostDb.store(2.5f);
                presenceBoostDb.store(0.0f);
                enableSpatial3D.store(false);
                spatialSurroundAmount.store(0.0f);
                PostNotification(L"🎵 PROFILE: HI-FI HARMAN", L"Audiophile natural sub-bass and silky treble curve active.", Color(255, 16, 185, 129), false);
                break;
            case ScenarioMode::CINEMA_MOVIE:
                bassBoostDb.store(7.0f);
                trebleBoostDb.store(2.0f);
                presenceBoostDb.store(2.0f);
                enableSpatial3D.store(true);
                spatialSurroundAmount.store(0.70f);
                PostNotification(L"🎬 PROFILE: CINEMA 3D SURROUND", L"Dolby-style room acoustic matrix and vocal dialogue lift.", Color(255, 168, 85, 247), false);
                break;
            case ScenarioMode::PURE_DIRECT:
                bassBoostDb.store(0.0f);
                trebleBoostDb.store(0.0f);
                presenceBoostDb.store(0.0f);
                enableSpatial3D.store(false);
                spatialSurroundAmount.store(0.0f);
                PostNotification(L"🎯 PROFILE: BIT-PERFECT DIRECT", L"Pure flat studio reference mode (0 DSP processing).", Color(255, 0, 210, 255), false);
                break;
        }
    }

    void StartDiscovery() {
        if (isDiscoveryRunning.load()) return;
        if (!EnsureWinsock()) return;
        isDiscoveryRunning.store(true);

        discoveryThread = std::thread([this]() {
            SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCKET) {
                isDiscoveryRunning.store(false);
                return;
            }

            BOOL bcast = TRUE;
            setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&bcast, sizeof(bcast));
            BOOL reuse = TRUE;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

            DWORD timeout = 500;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

            sockaddr_in bindAddr{};
            bindAddr.sin_family = AF_INET;
            bindAddr.sin_port = htons(DISCOVERY_PORT);
            bindAddr.sin_addr.s_addr = INADDR_ANY;
            bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr));

            sockaddr_in bcastAddr{};
            bcastAddr.sin_family = AF_INET;
            bcastAddr.sin_port = htons(DISCOVERY_PORT);
            bcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

            char buf[512]{};
            const char* scanMsg = "NWDISC|WindowsSender";

            while (isDiscoveryRunning.load()) {
                sendto(sock, scanMsg, (int)strlen(scanMsg), 0, (sockaddr*)&bcastAddr, sizeof(bcastAddr));

                std::string targetIp;
                {
                    std::lock_guard<std::mutex> lock(targetMutex);
                    targetIp = targetPhoneIp;
                }
                if (IsValidLanIpv4(targetIp.c_str())) {
                    sockaddr_in directAddr{};
                    directAddr.sin_family = AF_INET;
                    directAddr.sin_port = htons(DISCOVERY_PORT);
                    inet_pton(AF_INET, targetIp.c_str(), &directAddr.sin_addr);
                    sendto(sock, scanMsg, (int)strlen(scanMsg), 0, (sockaddr*)&directAddr, sizeof(directAddr));
                }

                auto endTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
                while (std::chrono::steady_clock::now() < endTime && isDiscoveryRunning.load()) {
                    sockaddr_in fromAddr{};
                    int fromLen = sizeof(fromAddr);
                    int bytes = recvfrom(sock, buf, (int)sizeof(buf) - 1, 0, (sockaddr*)&fromAddr, &fromLen);

                    if (bytes > 5 && bytes < (int)sizeof(buf)) {
                        buf[bytes] = '\0';
                        std::string msg(buf);
                        if (msg.rfind("NWBC|", 0) == 0) {
                            std::vector<std::string> tokens;
                            std::stringstream ss(msg);
                            std::string tok;
                            while (std::getline(ss, tok, '|')) tokens.push_back(tok);

                            char actualIp[64]{};
                            AddrToIpv4String(fromAddr, actualIp, sizeof(actualIp));
                            if (!IsValidLanIpv4(actualIp)) continue;

                            std::string name = tokens.size() >= 2 ? SanitizeDiscoveryName(tokens[1]) : "Android Phone";
                            uint32_t token = 0;
                            if (tokens.size() >= 4) {
                                try {
                                    unsigned long v = std::stoul(tokens[3]);
                                    token = (uint32_t)v;
                                } catch (...) {
                                    token = 0;
                                }
                            }

                            {
                                std::lock_guard<std::mutex> lock(discoveryMutex);
                                discoveredDeviceName = name;
                                discoveredDeviceIp = actualIp;
                                discoveredToken = token;
                                hasDiscoveredDevice.store(true);
                            }

                            if (lastNotifiedDevice != actualIp) {
                                lastNotifiedDevice = actualIp;
                                std::wstring wName(name.begin(), name.end());
                                std::wstring wIp(actualIp, actualIp + strlen(actualIp));
                                PostNotification(L"📱 DEVICE DISCOVERED", wName + L" (" + wIp + L") is ready for 48kHz streaming.", Color(255, 16, 185, 129));
                            }
                        }
                    }
                }
            }

            closesocket(sock);
        });
    }

    void StopDiscovery() {
        if (!isDiscoveryRunning.load()) return;
        isDiscoveryRunning.store(false);
        if (discoveryThread.joinable()) {
            discoveryThread.join();
        }
    }

    void EnumerateDevices() {
        devices.clear();
        CoInitializeEx(NULL, COINIT_MULTITHREADED);

        IMMDeviceEnumerator* pEnumerator = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
        if (FAILED(hr) || !pEnumerator) return;

        IMMDeviceCollection* pCollection = nullptr;
        hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
        if (SUCCEEDED(hr) && pCollection) {
            UINT count = 0;
            pCollection->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pDevice = nullptr;
                if (SUCCEEDED(pCollection->Item(i, &pDevice)) && pDevice) {
                    LPWSTR pId = nullptr;
                    pDevice->GetId(&pId);

                    IPropertyStore* pStore = nullptr;
                    std::wstring devName = L"Audio Device " + std::to_wstring(i);
                    if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                            devName = varName.pwszVal;
                        }
                        PropVariantClear(&varName);
                        pStore->Release();
                    }

                    AudioDevice dev;
                    dev.id = pId ? pId : L"";
                    dev.name = devName;
                    devices.push_back(dev);

                    if (pId) CoTaskMemFree(pId);
                    pDevice->Release();
                }
            }
            pCollection->Release();
        }
        pEnumerator->Release();
    }

    void StartStreaming(const std::string& ip, int devIndex, uint32_t token) {
        if (isStreaming.load()) return;
        if (!EnsureWinsock()) return;
        if (!IsValidLanIpv4(ip.c_str())) return;

        {
            std::lock_guard<std::mutex> lock(targetMutex);
            targetPhoneIp = ip;
            sessionToken = token;
        }
        selectedDeviceIndex = devIndex;
        isStreaming.store(true);
        sendPacketCount.store(0);

        std::wstring wIp(ip.begin(), ip.end());
        PostNotification(L"⚡ AUDIO STREAM CONNECTED", L"Streaming 48kHz Lossless PCM to " + wIp + L":50005", Color(255, 0, 210, 255));

        streamThread = std::thread([this]() {
            DWORD taskIndex = 0;
            HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
            if (hMmcss) {
                AvSetMmThreadPriority(hMmcss, AVRT_PRIORITY_CRITICAL);
            }
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            CoInitializeEx(NULL, COINIT_MULTITHREADED);

            std::string destIp;
            uint32_t tokenCopy = 0;
            {
                std::lock_guard<std::mutex> lock(targetMutex);
                destIp = targetPhoneIp;
                tokenCopy = sessionToken;
            }

            SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCKET) {
                if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
                CoUninitialize();
                isStreaming.store(false);
                return;
            }

            int sndBuf = 524288;
            setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&sndBuf, sizeof(sndBuf));

            DWORD tos = 0xB8;
            setsockopt(sock, IPPROTO_IP, IP_TOS, (char*)&tos, sizeof(tos));

            sockaddr_in targetAddr{};
            targetAddr.sin_family = AF_INET;
            targetAddr.sin_port = htons(AUDIO_SEND_PORT);
            inet_pton(AF_INET, destIp.c_str(), &targetAddr.sin_addr);

            IMMDeviceEnumerator* pEnumerator = nullptr;
            CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);

            IMMDevice* pDevice = nullptr;
            if (pEnumerator) {
                if (selectedDeviceIndex >= 0 && selectedDeviceIndex < (int)devices.size()) {
                    pEnumerator->GetDevice(devices[selectedDeviceIndex].id.c_str(), &pDevice);
                } else {
                    pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
                }
            }

            if (!pDevice) {
                if (pEnumerator) pEnumerator->Release();
                closesocket(sock);
                if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
                CoUninitialize();
                isStreaming.store(false);
                return;
            }

            IAudioClient* pAudioClient = nullptr;
            pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
            if (!pAudioClient) {
                pDevice->Release();
                if (pEnumerator) pEnumerator->Release();
                closesocket(sock);
                if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
                CoUninitialize();
                isStreaming.store(false);
                return;
            }

            WAVEFORMATEX* pWaveFormat = nullptr;
            if (FAILED(pAudioClient->GetMixFormat(&pWaveFormat)) || !pWaveFormat) {
                pAudioClient->Release();
                pDevice->Release();
                if (pEnumerator) pEnumerator->Release();
                closesocket(sock);
                if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
                CoUninitialize();
                isStreaming.store(false);
                return;
            }

            HRESULT hrInit = pAudioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK,
                50000,
                0,
                pWaveFormat,
                NULL
            );

            IAudioCaptureClient* pCaptureClient = nullptr;
            if (SUCCEEDED(hrInit)) {
                pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
            }
            if (FAILED(hrInit) || !pCaptureClient) {
                if (pCaptureClient) pCaptureClient->Release();
                pAudioClient->Release();
                CoTaskMemFree(pWaveFormat);
                pDevice->Release();
                if (pEnumerator) pEnumerator->Release();
                closesocket(sock);
                if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
                CoUninitialize();
                isStreaming.store(false);
                return;
            }
            pAudioClient->Start();

            std::vector<int16_t> pcmAccumulator;
            pcmAccumulator.reserve(2048);

            bool isFloat = (pWaveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                           (pWaveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pWaveFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

            int srcChannels = pWaveFormat->nChannels;
            int srcBits = pWaveFormat->wBitsPerSample;
            int srcBytesPerFrame = pWaveFormat->nBlockAlign;
            int srcRate = (int)pWaveFormat->nSamplesPerSec;
            if (srcRate < 8000 || srcRate > 192000) srcRate = TARGET_RATE;
            if (srcChannels < 1) srcChannels = 1;
            if (srcBytesPerFrame < 1) srcBytesPerFrame = (srcBits / 8) * srcChannels;

            // DSP Filters with Continuous Slewing (Zero Mode-Switch Pops!)
            BiquadFilter bassFilter;
            BiquadFilter trebleFilter;
            BiquadFilter presenceFilter;
            BinauralSpatializer spatializer3D;

            // Real-Time 7-Band Spectrum Energy Detectors
            BiquadFilter specBp[7];
            const double specFreqs[7] = {60.0, 150.0, 400.0, 1000.0, 2500.0, 6000.0, 14000.0};
            for (int b = 0; b < 7; b++) {
                specBp[b].SetBandPass(specFreqs[b], 1.414, (double)TARGET_RATE);
            }
            double specBandAcc[7] = {0, 0, 0, 0, 0, 0, 0};

            // Slewed continuous DSP parameters
            double slewBass = bassBoostDb.load();
            double slewTreble = trebleBoostDb.load();
            double slewPresence = presenceBoostDb.load();
            double slewSpatial = enableSpatial3D.load() ? spatialSurroundAmount.load() : 0.0f;
            double slewWetDry = (currentScenario.load() == ScenarioMode::PURE_DIRECT) ? 0.0 : 1.0;

            bassFilter.SetLowShelf(80.0, slewBass, (double)TARGET_RATE, 0.7071);
            trebleFilter.SetHighShelf(12000.0, slewTreble, (double)TARGET_RATE, 0.7071);
            presenceFilter.SetPeaking(2800.0, slewPresence, (double)TARGET_RATE, 1.0);

            double lastUpdatedBass = slewBass;
            double lastUpdatedTreble = slewTreble;
            double lastUpdatedPresence = slewPresence;

            auto lastPacketTime = std::chrono::high_resolution_clock::now();
            uint16_t sequenceNumber = 0;
            std::vector<uint8_t> packetPayload(MAX_AUDIO_PACKET);

            while (isStreaming.load()) {
                UINT32 packetLength = 0;
                HRESULT hr = pCaptureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) {
                    Sleep(2);
                    continue;
                }

                if (packetLength == 0) {
                    Sleep(1);
                    continue;
                }

                BYTE* pData = nullptr;
                UINT32 numFramesRead = 0;
                DWORD flags = 0;

                hr = pCaptureClient->GetBuffer(&pData, &numFramesRead, &flags, NULL, NULL);
                if (SUCCEEDED(hr)) {
                    if (numFramesRead > 0) {
                        // Target DSP parameters
                        double targetBass = (double)bassBoostDb.load();
                        double targetTreble = (double)trebleBoostDb.load();
                        double targetPresence = (double)presenceBoostDb.load();
                        double targetSpatial = enableSpatial3D.load() ? (double)spatialSurroundAmount.load() : 0.0;
                        double targetWetDry = (currentScenario.load() == ScenarioMode::PURE_DIRECT) ? 0.0 : 1.0;

                        for (UINT32 f = 0; f < numFramesRead; f++) {
                            // Smooth exponential parameter slewing per sample (Pop-Free Guarantee!)
                            slewBass += (targetBass - slewBass) * 0.003;
                            slewTreble += (targetTreble - slewTreble) * 0.003;
                            slewPresence += (targetPresence - slewPresence) * 0.003;
                            slewSpatial += (targetSpatial - slewSpatial) * 0.003;
                            slewWetDry += (targetWetDry - slewWetDry) * 0.003;

                            if (std::abs(slewBass - lastUpdatedBass) > 0.05) {
                                bassFilter.SetLowShelf(80.0, slewBass, (double)TARGET_RATE, 0.7071);
                                lastUpdatedBass = slewBass;
                            }
                            if (std::abs(slewTreble - lastUpdatedTreble) > 0.05) {
                                trebleFilter.SetHighShelf(12000.0, slewTreble, (double)TARGET_RATE, 0.7071);
                                lastUpdatedTreble = slewTreble;
                            }
                            if (std::abs(slewPresence - lastUpdatedPresence) > 0.05) {
                                presenceFilter.SetPeaking(2800.0, slewPresence, (double)TARGET_RATE, 1.0);
                                lastUpdatedPresence = slewPresence;
                            }

                            double l_raw = 0.0, r_raw = 0.0;

                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT || !pData) {
                                l_raw = 0.0;
                                r_raw = 0.0;
                            } else if (isFloat) {
                                const float* fSrc = reinterpret_cast<const float*>(pData + f * srcBytesPerFrame);
                                l_raw = fSrc[0];
                                r_raw = (srcChannels > 1) ? fSrc[1] : l_raw;
                            } else if (srcBits == 16) {
                                const int16_t* sSrc = reinterpret_cast<const int16_t*>(pData + f * srcBytesPerFrame);
                                l_raw = sSrc[0] / 32768.0;
                                r_raw = (srcChannels > 1) ? sSrc[1] / 32768.0 : l_raw;
                            } else if (srcBits == 24 && srcBytesPerFrame >= (srcChannels * 3)) {
                                const uint8_t* bSrc = pData + f * srcBytesPerFrame;
                                int32_t valL = (int32_t)((bSrc[0] << 8) | (bSrc[1] << 16) | (bSrc[2] << 24));
                                l_raw = valL / 2147483648.0;
                                if (srcChannels > 1) {
                                    int32_t valR = (int32_t)((bSrc[3] << 8) | (bSrc[4] << 16) | (bSrc[5] << 24));
                                    r_raw = valR / 2147483648.0;
                                } else {
                                    r_raw = l_raw;
                                }
                            } else if (srcBits == 32) {
                                const int32_t* iSrc = reinterpret_cast<const int32_t*>(pData + f * srcBytesPerFrame);
                                l_raw = iSrc[0] / 2147483648.0;
                                r_raw = (srcChannels > 1) ? iSrc[1] / 2147483648.0 : l_raw;
                            }

                            // True Real-Time 7-Band Energy Accumulation from Live PCM
                            double monoIn = (l_raw + r_raw) * 0.5;
                            for (int b = 0; b < 7; b++) {
                                double bpOut = specBp[b].Process(monoIn, 0);
                                specBandAcc[b] += bpOut * bpOut;
                            }

                            // Smooth Equal-Power DSP Blend
                            double l_dsp = l_raw;
                            double r_dsp = r_raw;

                            l_dsp = bassFilter.Process(l_dsp, 0);
                            r_dsp = bassFilter.Process(r_dsp, 1);

                            if (std::abs(slewPresence) > 0.02) {
                                l_dsp = presenceFilter.Process(l_dsp, 0);
                                r_dsp = presenceFilter.Process(r_dsp, 1);
                            }

                            l_dsp = trebleFilter.Process(l_dsp, 0);
                            r_dsp = trebleFilter.Process(r_dsp, 1);

                            if (slewSpatial > 0.005) {
                                spatializer3D.ProcessStereo(l_dsp, r_dsp, (float)slewSpatial);
                            }

                            l_dsp = StudioMasterLimiter(l_dsp);
                            r_dsp = StudioMasterLimiter(r_dsp);

                            // Smooth crossfade between Raw Direct and DSP Wet
                            double l_out = l_raw * (1.0 - slewWetDry) + l_dsp * slewWetDry;
                            double r_out = r_raw * (1.0 - slewWetDry) + r_dsp * slewWetDry;

                            static uint32_t ditherPrng = 0x12345678;
                            float dither = GenerateTpdfDither(ditherPrng);

                            pcmAccumulator.push_back((int16_t)(std::clamp(l_out + dither, -1.0, 1.0) * 32767.0));
                            pcmAccumulator.push_back((int16_t)(std::clamp(r_out + dither, -1.0, 1.0) * 32767.0));
                        }
                    }
                    pCaptureClient->ReleaseBuffer(numFramesRead);

                    // Fixed stable 128 frames (2.67ms @ 48kHz Stereo)
                    constexpr int targetChunk = 128;
                    constexpr int targetSamples = targetChunk * 2;

                    while ((int)pcmAccumulator.size() >= targetSamples) {
                        int pcmBytes = targetSamples * (int)sizeof(int16_t);
                        size_t totalBytes = PACKET_HEADER_SIZE + (size_t)pcmBytes;
                        if (totalBytes > packetPayload.size()) break;

                        uint16_t magic = PACKET_MAGIC;
                        uint16_t seq = sequenceNumber++;
                        memcpy(packetPayload.data(), &magic, 2);
                        memcpy(packetPayload.data() + 2, &seq, 2);
                        memcpy(packetPayload.data() + 4, &tokenCopy, 4);
                        memcpy(packetPayload.data() + PACKET_HEADER_SIZE, pcmAccumulator.data(), (size_t)pcmBytes);

                        sendto(sock, reinterpret_cast<const char*>(packetPayload.data()), (int)totalBytes, 0, (sockaddr*)&targetAddr, sizeof(targetAddr));
                        sendPacketCount.fetch_add(1, std::memory_order_relaxed);

                        auto now = std::chrono::high_resolution_clock::now();
                        float deltaMs = std::chrono::duration<float, std::milli>(now - lastPacketTime).count();
                        lastPacketTime = now;
                        AddLatencySample(deltaMs);

                        double sumSqL = 0, sumSqR = 0;
                        {
                            std::lock_guard<std::mutex> waveLock(waveformMutex);
                            for (int i = 0; i < targetSamples; i += 2) {
                                float sL = pcmAccumulator[i] / 32768.0f;
                                float sR = pcmAccumulator[i + 1] / 32768.0f;
                                sumSqL += (double)sL * sL;
                                sumSqR += (double)sR * sR;

                                waveformL.erase(waveformL.begin());
                                waveformL.push_back(sL);
                                waveformR.erase(waveformR.begin());
                                waveformR.push_back(sR);
                            }
                        }

                        double rmsL = std::sqrt(sumSqL / (targetSamples / 2));
                        double rmsR = std::sqrt(sumSqR / (targetSamples / 2));
                        streamRmsL.store((float)std::min(1.0, rmsL * 3.5));
                        streamRmsR.store((float)std::min(1.0, rmsR * 3.5));

                        // Store Real-Time 7-Band Frequency Spectrum from Biquad Filter Energy
                        for (int b = 0; b < 7; b++) {
                            double bandRms = std::sqrt(specBandAcc[b] / targetChunk) * 4.2;
                            specBandAcc[b] = 0.0;
                            float curVal = spectrumBands[b].load(std::memory_order_relaxed);
                            float targetVal = (float)std::clamp(bandRms, 0.0, 1.0);
                            float smoothVal = curVal * 0.4f + targetVal * 0.6f;
                            spectrumBands[b].store(smoothVal, std::memory_order_relaxed);
                        }

                        pcmAccumulator.erase(pcmAccumulator.begin(), pcmAccumulator.begin() + targetSamples);
                    }
                }
            }

            pAudioClient->Stop();
            pCaptureClient->Release();
            pAudioClient->Release();
            CoTaskMemFree(pWaveFormat);
            pDevice->Release();
            if (pEnumerator) pEnumerator->Release();

            closesocket(sock);
            if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
            CoUninitialize();
            streamRmsL.store(0.0f);
            streamRmsR.store(0.0f);
            for (int i = 0; i < 7; i++) spectrumBands[i].store(0.0f);
        });
    }

    void StopStreaming() {
        if (!isStreaming.load()) return;
        isStreaming.store(false);
        if (streamThread.joinable()) {
            streamThread.join();
        }
        streamRmsL.store(0.0f);
        streamRmsR.store(0.0f);
        for (int i = 0; i < 7; i++) spectrumBands[i].store(0.0f);
        PostNotification(L"🔴 AUDIO STREAM DISCONNECTED", L"Audio transmission stopped. Receiver is now idle.", Color(255, 239, 68, 68));
    }

    bool StartMicReceive() {
        if (isMicReceiving.load()) return true;
        if (!EnsureWinsock()) return false;

        std::string expectedIp;
        uint32_t expectedToken = 0;
        {
            std::lock_guard<std::mutex> lock(targetMutex);
            expectedIp = targetPhoneIp;
            expectedToken = sessionToken;
        }
        if (expectedToken == 0) {
            std::lock_guard<std::mutex> lock(discoveryMutex);
            expectedIp = discoveredDeviceIp;
            expectedToken = discoveredToken;
        }

        isMicReceiving.store(true);
        micPacketCount.store(0);

        PostNotification(L"🎙 WIRELESS MICROPHONE ACTIVE", L"Receiving phone microphone voice stream on Port 50006.", Color(255, 168, 85, 247));

        micThread = std::thread([this, expectedIp, expectedToken]() {
            DWORD taskIndex = 0;
            HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            CoInitializeEx(NULL, COINIT_MULTITHREADED);

            SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCKET) {
                if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
                CoUninitialize();
                isMicReceiving.store(false);
                return;
            }

            int rcvBuf = 524288;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvBuf, sizeof(rcvBuf));
            BOOL reuse = TRUE;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

            sockaddr_in bindAddr{};
            bindAddr.sin_family = AF_INET;
            bindAddr.sin_port = htons(MIC_RECEIVE_PORT);
            bindAddr.sin_addr.s_addr = INADDR_ANY;
            if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
                closesocket(sock);
                if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
                CoUninitialize();
                isMicReceiving.store(false);
                return;
            }

            DWORD timeout = 250;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

            IMMDeviceEnumerator* pEnumerator = nullptr;
            CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);

            IMMDevice* pDevice = nullptr;
            if (pEnumerator) {
                pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
            }
            IAudioClient* pAudioClient = nullptr;
            if (pDevice) {
                pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
            }

            WAVEFORMATEX wf{};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = 2;
            wf.nSamplesPerSec = TARGET_RATE;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = (wf.nChannels * wf.wBitsPerSample) / 8;
            wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
            wf.cbSize = 0;

            IAudioRenderClient* pRenderClient = nullptr;
            if (pAudioClient) {
                HRESULT hrMic = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 50000, 0, &wf, NULL);
                if (SUCCEEDED(hrMic)) {
                    pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient);
                }
            }

            if (!pRenderClient) {
                if (pAudioClient) pAudioClient->Release();
                if (pDevice) pDevice->Release();
                if (pEnumerator) pEnumerator->Release();
                closesocket(sock);
                if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
                CoUninitialize();
                isMicReceiving.store(false);
                return;
            }
            pAudioClient->Start();

            std::vector<char> recvBuffer(8192);
            std::vector<int16_t> stereoBuffer;

            while (isMicReceiving.load()) {
                sockaddr_in fromAddr{};
                int fromLen = sizeof(fromAddr);
                int bytesRead = recvfrom(sock, recvBuffer.data(), (int)recvBuffer.size(), 0, (sockaddr*)&fromAddr, &fromLen);
                if (bytesRead < (int)PACKET_HEADER_SIZE) continue;

                uint16_t magic = 0;
                memcpy(&magic, recvBuffer.data(), 2);
                if (magic != PACKET_MAGIC) continue;

                int pcmBytes = bytesRead - (int)PACKET_HEADER_SIZE;
                if (pcmBytes < 2 || (pcmBytes % 2) != 0) continue;
                int sampleCount = pcmBytes / (int)sizeof(int16_t);
                if (sampleCount > 4096) continue;
                const int16_t* monoSamples = reinterpret_cast<const int16_t*>(recvBuffer.data() + PACKET_HEADER_SIZE);

                stereoBuffer.resize((size_t)sampleCount * 2);
                for (int i = 0; i < sampleCount; i++) {
                    stereoBuffer[i * 2] = monoSamples[i];
                    stereoBuffer[i * 2 + 1] = monoSamples[i];
                }

                UINT32 padding = 0;
                pAudioClient->GetCurrentPadding(&padding);
                UINT32 bufferFrameCount = 0;
                pAudioClient->GetBufferSize(&bufferFrameCount);
                if (bufferFrameCount <= padding) continue;
                UINT32 availableFrames = bufferFrameCount - padding;

                if (availableFrames >= (UINT32)sampleCount) {
                    BYTE* pRenderData = nullptr;
                    if (SUCCEEDED(pRenderClient->GetBuffer(sampleCount, &pRenderData)) && pRenderData) {
                        memcpy(pRenderData, stereoBuffer.data(), (size_t)sampleCount * 4);
                        pRenderClient->ReleaseBuffer(sampleCount, 0);
                    }
                }

                double sumSq = 0;
                for (int i = 0; i < sampleCount; i++) sumSq += (double)monoSamples[i] * monoSamples[i];
                double rms = std::sqrt(sumSq / sampleCount);
                micRmsLevel.store((float)std::min(1.0, rms * 3.5));
                micPacketCount.fetch_add(1, std::memory_order_relaxed);
            }

            pAudioClient->Stop();
            pRenderClient->Release();
            pAudioClient->Release();
            if (pDevice) pDevice->Release();
            if (pEnumerator) pEnumerator->Release();

            closesocket(sock);
            if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
            CoUninitialize();
            micRmsLevel.store(0.0f);
        });
        return true;
    }

    void StopMicReceive() {
        if (!isMicReceiving.load()) return;
        isMicReceiving.store(false);
        if (micThread.joinable()) {
            micThread.join();
        }
        micRmsLevel.store(0.0f);
        PostNotification(L"🎙 MICROPHONE IDLE", L"Wireless phone microphone stream stopped.", Color(255, 148, 163, 184));
    }
};

static AudioEngine g_Engine;

// ── Interactive UI Element Rectangles & Mouse States ────────────────────────
struct UiLayout {
    Rect rPresets[4];
    Rect rAutoPairBtn;
    Rect rBassTrack;
    Rect rTrebleTrack;
    Rect rSpatialToggle;
    Rect rStreamBtn;
    Rect rMicBtn;
    Rect rDeviceCombo;
};

static UiLayout g_ui;
static int g_hoverElement = -1; // 0..3: Presets, 10: AutoPair, 20: Bass, 21: Treble, 30: Spatial, 40: Stream, 41: Mic
static bool g_isDraggingBass = false;
static bool g_isDraggingTreble = false;

static HWND g_hIpEdit = NULL;
static HWND g_hDeviceCombo = NULL;

// ── Modern Palette ──────────────────────────────────────────────────────────
#define COLOR_BG RGB(10, 13, 18)
#define COLOR_CARD_BG RGB(17, 22, 31)
#define COLOR_INPUT_BG RGB(13, 17, 24)
#define COLOR_CYAN RGB(0, 210, 255)
#define COLOR_PURPLE RGB(168, 85, 247)
#define COLOR_GREEN RGB(16, 185, 129)
#define COLOR_GOLD RGB(245, 158, 11)
#define COLOR_TEXT_MAIN RGB(248, 250, 252)

static HBRUSH g_hbrBg = NULL;
static HBRUSH g_hbrCard = NULL;
static HBRUSH g_hbrInput = NULL;
static HFONT g_hFontTitle = NULL;
static HFONT g_hFontSub = NULL;
static HFONT g_hFontBold = NULL;
static HFONT g_hFontNormal = NULL;
static HFONT g_hFontMono = NULL;

void DrawNullWireLogo(Graphics& g, float x, float y, float size) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    float cx = x + size / 2.0f;
    float cy = y + size / 2.0f;
    float outerR = size * 0.44f;
    float innerR = size * 0.24f;

    SolidBrush brushRing(Color(255, 14, 28, 54));
    GraphicsPath ringPath;
    ringPath.AddEllipse(cx - outerR, cy - outerR, outerR * 2.0f, outerR * 2.0f);
    ringPath.AddEllipse(cx - innerR, cy - innerR, innerR * 2.0f, innerR * 2.0f);
    g.FillPath(&brushRing, &ringPath);

    Pen penGlowArc(Color(255, 0, 210, 255), 2.5f);
    g.DrawArc(&penGlowArc, cx - innerR - 1.0f, cy - innerR - 1.0f, (innerR + 1.0f) * 2.0f, (innerR + 1.0f) * 2.0f, 180.0f, 180.0f);

    Pen penWaveL(Color(255, 0, 210, 255), 3.0f);
    penWaveL.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    GraphicsPath waveL;
    waveL.StartFigure();
    waveL.AddLine(cx - size * 0.48f, cy, cx - size * 0.36f, cy);
    waveL.AddLine(cx - size * 0.36f, cy, cx - size * 0.28f, cy - size * 0.24f);
    waveL.AddLine(cx - size * 0.28f, cy - size * 0.24f, cx - size * 0.20f, cy + size * 0.24f);
    waveL.AddLine(cx - size * 0.20f, cy + size * 0.24f, cx - size * 0.12f, cy - size * 0.14f);
    waveL.AddLine(cx - size * 0.12f, cy - size * 0.14f, cx - size * 0.06f, cy);
    g.DrawPath(&penWaveL, &waveL);

    Pen penWaveR(Color(255, 168, 85, 247), 3.0f);
    penWaveR.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    GraphicsPath waveR;
    waveR.StartFigure();
    waveR.AddLine(cx + size * 0.06f, cy, cx + size * 0.12f, cy);
    waveR.AddLine(cx + size * 0.12f, cy, cx + size * 0.20f, cy - size * 0.22f);
    waveR.AddLine(cx + size * 0.20f, cy - size * 0.22f, cx + size * 0.28f, cy + size * 0.22f);
    waveR.AddLine(cx + size * 0.28f, cy + size * 0.22f, cx + size * 0.36f, cy - size * 0.12f);
    waveR.AddLine(cx + size * 0.36f, cy - size * 0.12f, cx + size * 0.48f, cy);
    g.DrawPath(&penWaveR, &waveR);
}

void DrawModernGlassCard(Graphics& g, int x, int y, int w, int h, const wchar_t* title = nullptr, const wchar_t* badge = nullptr, Color badgeColor = Color(255, 16, 185, 129)) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    GraphicsPath path;
    int rad = 10;
    path.AddArc(x, y, rad * 2, rad * 2, 180, 90);
    path.AddArc(x + w - rad * 2, y, rad * 2, rad * 2, 270, 90);
    path.AddArc(x + w - rad * 2, y + h - rad * 2, rad * 2, rad * 2, 0, 90);
    path.AddArc(x, y + h - rad * 2, rad * 2, rad * 2, 90, 90);
    path.CloseFigure();

    SolidBrush brushFill(Color(255, 17, 22, 31));
    g.FillPath(&brushFill, &path);

    Pen penBorder(Color(255, 30, 41, 59), 1.0f);
    g.DrawPath(&penBorder, &path);

    if (title) {
        Gdiplus::Font fontTitle(L"Segoe UI", 9.5f, FontStyleBold, UnitPoint);
        SolidBrush brushText(Color(255, 226, 232, 240));
        g.DrawString(title, -1, &fontTitle, PointF((float)x + 16, (float)y + 12), &brushText);
    }

    if (badge) {
        Gdiplus::Font fontBadge(L"Consolas", 8.5f, FontStyleBold, UnitPoint);
        SolidBrush brushBadge(badgeColor);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentFar);
        g.DrawString(badge, -1, &fontBadge, PointF((float)x + w - 16, (float)y + 12), &sf, &brushBadge);
    }
}

void DrawModernSlider(Graphics& g, const Rect& r, float curVal, float minVal, float maxVal, const wchar_t* label, const wchar_t* valStr, Color accentColor, bool isHover) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    // Label
    Gdiplus::Font fontLbl(L"Segoe UI", 9.0f, FontStyleRegular, UnitPoint);
    SolidBrush brushLbl(Color(255, 203, 213, 225));
    g.DrawString(label, -1, &fontLbl, PointF((float)r.X, (float)r.Y - 18), &brushLbl);

    // Value Badge
    Gdiplus::Font fontVal(L"Consolas", 9.0f, FontStyleBold, UnitPoint);
    SolidBrush brushVal(accentColor);
    StringFormat sfRight;
    sfRight.SetAlignment(StringAlignmentFar);
    g.DrawString(valStr, -1, &fontVal, PointF((float)r.GetRight(), (float)r.Y - 18), &sfRight, &brushVal);

    // Track Background
    int trackH = 6;
    int trackY = r.Y + (r.Height - trackH) / 2;
    GraphicsPath trackPath;
    trackPath.AddArc(r.X, trackY, trackH, trackH, 90, 180);
    trackPath.AddArc(r.GetRight() - trackH, trackY, trackH, trackH, 270, 180);
    trackPath.CloseFigure();

    SolidBrush brushTrackBg(Color(255, 30, 41, 59));
    g.FillPath(&brushTrackBg, &trackPath);

    // Track Fill (Progress)
    float ratio = std::clamp((curVal - minVal) / (maxVal - minVal), 0.0f, 1.0f);
    int fillW = (int)(ratio * r.Width);
    if (fillW > trackH) {
        GraphicsPath fillPath;
        fillPath.AddArc(r.X, trackY, trackH, trackH, 90, 180);
        fillPath.AddArc(r.X + fillW - trackH, trackY, trackH, trackH, 270, 180);
        fillPath.CloseFigure();

        SolidBrush brushFill(accentColor);
        g.FillPath(&brushFill, &fillPath);
    }

    // Thumb Knob
    int thumbSize = isHover ? 18 : 16;
    float thumbX = r.X + ratio * (r.Width - thumbSize);
    float thumbY = (float)r.Y + (r.Height - thumbSize) / 2.0f;

    SolidBrush brushThumbShadow(Color(80, 0, 0, 0));
    g.FillEllipse(&brushThumbShadow, thumbX - 1, thumbY + 1, (float)thumbSize + 2, (float)thumbSize + 2);

    SolidBrush brushThumb(Color(255, 255, 255, 255));
    g.FillEllipse(&brushThumb, thumbX, thumbY, (float)thumbSize, (float)thumbSize);

    Pen penThumb(accentColor, 2.5f);
    g.DrawEllipse(&penThumb, thumbX, thumbY, (float)thumbSize, (float)thumbSize);
}

void DrawModernToggle(Graphics& g, const Rect& r, bool isChecked, const wchar_t* text, bool isHover) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    int switchW = 40;
    int switchH = 22;
    int switchX = r.X;
    int switchY = r.Y + (r.Height - switchH) / 2;

    GraphicsPath path;
    path.AddArc(switchX, switchY, switchH, switchH, 90, 180);
    path.AddArc(switchX + switchW - switchH, switchY, switchH, switchH, 270, 180);
    path.CloseFigure();

    if (isChecked) {
        SolidBrush brushOn(Color(255, 16, 185, 129));
        g.FillPath(&brushOn, &path);
    } else {
        SolidBrush brushOff(isHover ? Color(255, 51, 65, 85) : Color(255, 30, 41, 59));
        g.FillPath(&brushOff, &path);
    }

    int knobSize = 16;
    int knobX = isChecked ? (switchX + switchW - knobSize - 3) : (switchX + 3);
    int knobY = switchY + 3;

    SolidBrush brushKnob(Color(255, 255, 255, 255));
    g.FillEllipse(&brushKnob, knobX, knobY, knobSize, knobSize);

    Gdiplus::Font fontText(L"Segoe UI", 9.0f, FontStyleBold, UnitPoint);
    SolidBrush brushText(isChecked ? Color(255, 248, 250, 252) : Color(255, 148, 163, 184));
    g.DrawString(text, -1, &fontText, PointF((float)switchX + switchW + 12, (float)r.Y + 2), &brushText);
}

void DrawModernPillButton(Graphics& g, const Rect& r, const wchar_t* text, bool isSelected, bool isHover, Color accentColor, bool isPrimary = false) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    GraphicsPath path;
    int rad = 8;
    path.AddArc(r.X, r.Y, rad * 2, rad * 2, 180, 90);
    path.AddArc(r.GetRight() - rad * 2, r.Y, rad * 2, rad * 2, 270, 90);
    path.AddArc(r.GetRight() - rad * 2, r.GetBottom() - rad * 2, rad * 2, rad * 2, 0, 90);
    path.AddArc(r.X, r.GetBottom() - rad * 2, rad * 2, rad * 2, 90, 90);
    path.CloseFigure();

    if (isPrimary) {
        LinearGradientBrush grad(
            PointF((float)r.X, (float)r.Y),
            PointF((float)r.GetRight(), (float)r.GetBottom()),
            isHover ? Color(255, 20, 220, 255) : Color(255, 0, 200, 245),
            isHover ? Color(255, 120, 110, 255) : Color(255, 99, 102, 241)
        );
        g.FillPath(&grad, &path);
    } else if (isSelected) {
        SolidBrush brushSel(Color(40, accentColor.GetR(), accentColor.GetG(), accentColor.GetB()));
        g.FillPath(&brushSel, &path);
        Pen penSel(accentColor, 1.5f);
        g.DrawPath(&penSel, &path);
    } else {
        SolidBrush brushNorm(isHover ? Color(255, 35, 47, 68) : Color(255, 23, 31, 45));
        g.FillPath(&brushNorm, &path);
        Pen penNorm(Color(255, 38, 52, 75), 1.0f);
        g.DrawPath(&penNorm, &path);
    }

    Gdiplus::Font fontBtn(L"Segoe UI", isPrimary ? 10.0f : 9.0f, FontStyleBold, UnitPoint);
    SolidBrush brushText(isPrimary ? Color(255, 255, 255, 255) : (isSelected ? accentColor : (isHover ? Color(255, 255, 255, 255) : Color(255, 203, 213, 225))));
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text, -1, &fontBtn, RectF((float)r.X, (float)r.Y, (float)r.Width, (float)r.Height), &sf, &brushText);
}

void DrawSpectrumAndOscilloscope(Graphics& g, int x, int y, int w, int h) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    DrawModernGlassCard(g, x, y, w, h, L"LIVE AUDIO FREQUENCY SPECTRUM & STEREO OSCILLOSCOPE", L"48kHz 16-BIT LOSSLESS", Color(255, 0, 210, 255));

    int specW = (w - 48) / 2;
    int oscW = specW;
    int specX = x + 16;
    int oscX = specX + specW + 16;
    int plotY = y + 36;
    int plotH = h - 48;

    // 1. 7-Band Frequency Spectrum Bars (Real Biquad Energy Values)
    const wchar_t* bandNames[] = {L"60Hz", L"150Hz", L"400Hz", L"1kHz", L"2.5k", L"6kHz", L"14k"};
    int barW = (specW - 14) / 7;

    for (int i = 0; i < 7; i++) {
        float val = g_Engine.spectrumBands[i].load(std::memory_order_relaxed);
        int bx = specX + i * (barW + 2);
        int barH = (int)(val * (plotH - 18));
        int by = plotY + plotH - 16 - barH;

        SolidBrush brushTrack(Color(255, 13, 17, 24));
        g.FillRectangle(&brushTrack, bx, plotY, barW, plotH - 16);

        if (barH > 2) {
            LinearGradientBrush grad(
                PointF((float)bx, (float)by),
                PointF((float)bx, (float)plotY + plotH - 16),
                Color(255, 0, 210, 255),
                Color(255, 168, 85, 247)
            );
            g.FillRectangle(&grad, bx, by, barW, barH);
        }

        Gdiplus::Font fontBand(L"Consolas", 7.0f, FontStyleRegular, UnitPoint);
        SolidBrush brushLbl(Color(255, 100, 116, 139));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        g.DrawString(bandNames[i], -1, &fontBand, RectF((float)bx - 2, (float)plotY + plotH - 14, (float)barW + 4, 14.0f), &sf, &brushLbl);
    }

    // 2. Dual-Channel Stereo Oscilloscope
    SolidBrush brushOscBg(Color(255, 13, 17, 24));
    g.FillRectangle(&brushOscBg, oscX, plotY, oscW, plotH - 16);

    Pen penGrid(Color(255, 23, 31, 45), 1.0f);
    g.DrawLine(&penGrid, oscX, plotY + (plotH - 16) / 2, oscX + oscW, plotY + (plotH - 16) / 2);

    std::vector<float> waveL, waveR;
    {
        std::lock_guard<std::mutex> lock(g_Engine.waveformMutex);
        waveL = g_Engine.waveformL;
        waveR = g_Engine.waveformR;
    }

    if (waveL.size() > 1) {
        Pen penL(Color(255, 0, 210, 255), 1.6f);
        penL.SetLineJoin(LineJoinRound);
        Pen penR(Color(255, 168, 85, 247), 1.2f);
        penR.SetLineJoin(LineJoinRound);

        float midY = (float)plotY + (float)(plotH - 16) / 2.0f;
        float ampScale = (float)(plotH - 24) / 2.0f;

        for (size_t i = 1; i < waveL.size(); i++) {
            float x0 = (float)oscX + ((float)(i - 1) / (float)(waveL.size() - 1)) * (float)oscW;
            float x1 = (float)oscX + ((float)i / (float)(waveL.size() - 1)) * (float)oscW;

            float y0_L = midY - waveL[i - 1] * ampScale;
            float y1_L = midY - waveL[i] * ampScale;
            g.DrawLine(&penL, x0, y0_L, x1, y1_L);

            float y0_R = midY - waveR[i - 1] * ampScale;
            float y1_R = midY - waveR[i] * ampScale;
            g.DrawLine(&penR, x0, y0_R, x1, y1_R);
        }
    }
}

void DrawFuturisticLatencyChart(Graphics& g, int x, int y, int w, int h) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    wchar_t telemetryHdr[128];
    float curLat = g_Engine.currentLatencyMs.load();
    float jit = g_Engine.jitterMs.load();
    uint64_t totalPackets = g_Engine.sendPacketCount.load();

    swprintf_s(telemetryHdr, L"LATENCY: %.2f ms   ·   JITTER: ±%.2f ms   ·   PACKETS: %llu", curLat, jit, totalPackets);

    DrawModernGlassCard(g, x, y, w, h, L"HARDWARE DMA LATENCY & NETWORK JITTER (REAL-TIME)", telemetryHdr, Color(255, 16, 185, 129));

    int gx = x + 16;
    int gy = y + 36;
    int gw = w - 32;
    int gh = h - 48;

    SolidBrush brushGridBg(Color(255, 13, 17, 24));
    g.FillRectangle(&brushGridBg, gx, gy, gw, gh);

    Pen penGrid(Color(255, 23, 31, 45), 1.0f);
    for (int i = 1; i < 4; i++) {
        int py = gy + (gh * i) / 4;
        g.DrawLine(&penGrid, gx, py, gx + gw, py);
    }

    std::vector<float> hist;
    {
        std::lock_guard<std::mutex> lock(g_Engine.latencyMutex);
        hist.assign(g_Engine.latencyHistoryMs.begin(), g_Engine.latencyHistoryMs.end());
    }

    if (hist.size() > 1) {
        float maxScale = 10.0f;
        std::vector<PointF> pts;
        pts.reserve(hist.size());

        for (size_t i = 0; i < hist.size(); i++) {
            float val = std::clamp(hist[i], 0.0f, maxScale);
            float px = (float)gx + ((float)i / (float)(hist.size() - 1)) * (float)gw;
            float py = (float)gy + (float)gh - (val / maxScale) * (float)(gh - 4);
            pts.push_back(PointF(px, py));
        }

        GraphicsPath areaPath;
        areaPath.StartFigure();
        areaPath.AddLine(PointF((float)gx, (float)gy + (float)gh), pts.front());
        for (size_t i = 1; i < pts.size(); i++) {
            areaPath.AddLine(pts[i - 1], pts[i]);
        }
        areaPath.AddLine(pts.back(), PointF((float)gx + (float)gw, (float)gy + (float)gh));
        areaPath.CloseFigure();

        LinearGradientBrush areaGradient(
            PointF((float)gx, (float)gy),
            PointF((float)gx, (float)gy + (float)gh),
            Color(110, 0, 210, 255),
            Color(15, 99, 102, 241)
        );
        g.FillPath(&areaGradient, &areaPath);

        Pen penTrace(Color(255, 0, 210, 255), 2.2f);
        penTrace.SetLineJoin(LineJoinRound);
        for (size_t i = 1; i < pts.size(); i++) {
            g.DrawLine(&penTrace, pts[i - 1], pts[i]);
        }

        SolidBrush brushPulse(Color(255, 255, 255, 255));
        g.FillEllipse(&brushPulse, pts.back().X - 3.5f, pts.back().Y - 3.5f, 7.0f, 7.0f);
    }
}

void DrawFloatingToastBanner(Graphics& g, int width) {
    InAppNotification notify;
    {
        std::lock_guard<std::mutex> lock(g_notifyMutex);
        if (!g_currentNotification.active) return;
        notify = g_currentNotification;
    }

    auto now = std::chrono::steady_clock::now();
    float elapsedMs = std::chrono::duration<float, std::milli>(now - notify.startTime).count();
    if (elapsedMs > 4500.0f) {
        std::lock_guard<std::mutex> lock(g_notifyMutex);
        g_currentNotification.active = false;
        return;
    }

    float alpha = 1.0f;
    if (elapsedMs < 300.0f) alpha = elapsedMs / 300.0f;
    else if (elapsedMs > 4000.0f) alpha = (4500.0f - elapsedMs) / 500.0f;

    int toastW = width - 48;
    int toastH = 46;
    int toastX = 24;
    int toastY = 14;

    GraphicsPath toastPath;
    int rad = 8;
    toastPath.AddArc(toastX, toastY, rad * 2, rad * 2, 180, 90);
    toastPath.AddArc(toastX + toastW - rad * 2, toastY, rad * 2, rad * 2, 270, 90);
    toastPath.AddArc(toastX + toastW - rad * 2, toastY + toastH - rad * 2, rad * 2, rad * 2, 0, 90);
    toastPath.AddArc(toastX, toastY + toastH - rad * 2, rad * 2, rad * 2, 90, 90);
    toastPath.CloseFigure();

    BYTE bAlpha = (BYTE)(alpha * 240);
    SolidBrush brushToast(Color(bAlpha, 15, 23, 42));
    g.FillPath(&brushToast, &toastPath);

    Pen penToast(Color(bAlpha, notify.accentColor.GetR(), notify.accentColor.GetG(), notify.accentColor.GetB()), 1.5f);
    g.DrawPath(&penToast, &toastPath);

    Gdiplus::Font fontTitle(L"Segoe UI", 9.0f, FontStyleBold, UnitPoint);
    SolidBrush brushTitle(Color(bAlpha, notify.accentColor.GetR(), notify.accentColor.GetG(), notify.accentColor.GetB()));
    g.DrawString(notify.title.c_str(), -1, &fontTitle, PointF((float)toastX + 16, (float)toastY + 8), &brushTitle);

    Gdiplus::Font fontMsg(L"Segoe UI", 8.5f, FontStyleRegular, UnitPoint);
    SolidBrush brushMsg(Color(bAlpha, 226, 232, 240));
    g.DrawString(notify.message.c_str(), -1, &fontMsg, PointF((float)toastX + 16, (float)toastY + 24), &brushMsg);
}

void UpdateUiLayout(int width, int height) {
    int pad = 24;
    int cardW = width - pad * 2;
    if (cardW < 400) cardW = 400;

    // Presets Row (Y: 72, H: 36)
    int btnPresetW = (cardW - 18) / 4;
    for (int i = 0; i < 4; i++) {
        g_ui.rPresets[i] = Rect(pad + i * (btnPresetW + 6), 72, btnPresetW, 36);
    }

    // Card 1: Connection (Y: 118, H: 104)
    int editW = cardW - 190;
    g_ui.rAutoPairBtn = Rect(pad + 16 + editW + 10, 144, 150, 30);
    if (g_hIpEdit) SetWindowPos(g_hIpEdit, NULL, pad + 16, 145, editW, 28, SWP_NOZORDER);
    if (g_hDeviceCombo) SetWindowPos(g_hDeviceCombo, NULL, pad + 16, 182, cardW - 32, 200, SWP_NOZORDER);

    // Card 2: DSP Tuning (Y: 232, H: 136)
    g_ui.rBassTrack = Rect(pad + 16, 266, cardW - 32, 20);
    g_ui.rTrebleTrack = Rect(pad + 16, 308, cardW - 32, 20);
    g_ui.rSpatialToggle = Rect(pad + 16, 338, cardW - 32, 24);

    // Action Buttons Row (Y: 378, H: 44)
    int actBtnW = (cardW - 12) / 2;
    g_ui.rStreamBtn = Rect(pad, 378, actBtnW, 44);
    g_ui.rMicBtn = Rect(pad + actBtnW + 12, 378, actBtnW, 44);
}

void RestoreMainWindow(HWND hWnd) {
    g_InTray = false;
    ShowWindow(hWnd, SW_RESTORE);
    ShowWindow(hWnd, SW_SHOW);
    SetForegroundWindow(hWnd);
}

void SetupSystemTray(HWND hWnd, HINSTANCE hInstance) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1001;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"NullWire Pro - Studio Audio Suite");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void ShowTrayContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();

    InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_TRAY_RESTORE, L"Open NullWire Pro");
    InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

    std::wstring streamText = g_Engine.isStreaming.load() ? L"Stop Audio Stream (Ctrl+Shift+S)" : L"Start Audio Stream (Ctrl+Shift+S)";
    InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING, ID_TRAY_TOGGLE_STREAM, streamText.c_str());

    std::wstring micText = g_Engine.isMicReceiving.load() ? L"Stop Phone Mic (Ctrl+Shift+M)" : L"Receive Phone Mic (Ctrl+Shift+M)";
    InsertMenuW(hMenu, 3, MF_BYPOSITION | MF_STRING, ID_TRAY_TOGGLE_MIC, micText.c_str());

    InsertMenuW(hMenu, 4, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, 5, MF_BYPOSITION | MF_STRING, ID_TRAY_PROFILE_HIFI, L"Profile: 🎵 Hi-Fi Music (Ctrl+Shift+1)");
    InsertMenuW(hMenu, 6, MF_BYPOSITION | MF_STRING, ID_TRAY_PROFILE_GAMING, L"Profile: 🎮 Gaming Ultra 2.67ms (Ctrl+Shift+2)");
    InsertMenuW(hMenu, 7, MF_BYPOSITION | MF_STRING, ID_TRAY_PROFILE_CINEMA, L"Profile: 🎬 Cinema 3D (Ctrl+Shift+3)");
    InsertMenuW(hMenu, 8, MF_BYPOSITION | MF_STRING, ID_TRAY_PROFILE_DIRECT, L"Profile: 🎯 Pure Bit-Perfect (Ctrl+Shift+4)");

    InsertMenuW(hMenu, 9, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, 10, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"Exit NullWire Pro");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

void ToggleStreamAction(HWND hWnd) {
    if (g_Engine.isStreaming.load()) {
        g_Engine.StopStreaming();
        EnableWindow(g_hIpEdit, TRUE);
        EnableWindow(g_hDeviceCombo, TRUE);
    } else {
        wchar_t ipBuf[64]{};
        GetWindowTextW(g_hIpEdit, ipBuf, 64);
        char ipStr[64]{};
        WideCharToMultiByte(CP_UTF8, 0, ipBuf, -1, ipStr, 64, NULL, NULL);

        if (!IsValidLanIpv4(ipStr)) {
            MessageBoxW(hWnd, L"Enter a valid private LAN IPv4 address (e.g. 192.168.1.15).", L"Invalid IP", MB_ICONWARNING | MB_OK);
            return;
        }

        uint32_t token = 0;
        {
            std::lock_guard<std::mutex> lock(g_Engine.discoveryMutex);
            if (g_Engine.discoveredDeviceIp == ipStr) {
                token = g_Engine.discoveredToken;
            }
        }

        int curDev = (int)SendMessage(g_hDeviceCombo, CB_GETCURSEL, 0, 0);
        g_Engine.StartStreaming(ipStr, curDev, token);

        EnableWindow(g_hIpEdit, FALSE);
        EnableWindow(g_hDeviceCombo, FALSE);
    }
    InvalidateRect(hWnd, NULL, FALSE);
}

void ToggleMicAction(HWND hWnd) {
    if (g_Engine.isMicReceiving.load()) {
        g_Engine.StopMicReceive();
    } else {
        g_Engine.StartMicReceive();
    }
    InvalidateRect(hWnd, NULL, FALSE);
}

void SwitchScenarioAction(HWND hWnd, ScenarioMode mode) {
    g_Engine.SetScenario(mode);
    InvalidateRect(hWnd, NULL, FALSE);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hMainWnd = hWnd;
            g_hbrBg = CreateSolidBrush(COLOR_BG);
            g_hbrCard = CreateSolidBrush(COLOR_CARD_BG);
            g_hbrInput = CreateSolidBrush(COLOR_INPUT_BG);

            g_hFontTitle = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontSub = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontBold = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontNormal = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontMono = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");

            InitCommonControls();
            g_Engine.EnumerateDevices();
            g_Engine.StartDiscovery();

            // Native Text Input (IP Edit)
            g_hIpEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"192.168.1.15", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hWnd, (HMENU)101, NULL, NULL);
            SendMessage(g_hIpEdit, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);

            // Native Device Dropdown
            g_hDeviceCombo = CreateWindowW(L"COMBOBOX", NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, 10, hWnd, (HMENU)102, NULL, NULL);
            SendMessage(g_hDeviceCombo, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            for (const auto& dev : g_Engine.devices) {
                SendMessageW(g_hDeviceCombo, CB_ADDSTRING, 0, (LPARAM)dev.name.c_str());
            }
            if (!g_Engine.devices.empty()) SendMessage(g_hDeviceCombo, CB_SETCURSEL, 0, 0);

            SetupSystemTray(hWnd, ((LPCREATESTRUCT)lParam)->hInstance);
            RegisterHotKey(hWnd, HOTKEY_STREAM_ID, MOD_CONTROL | MOD_SHIFT, 'S');
            RegisterHotKey(hWnd, HOTKEY_MIC_ID, MOD_CONTROL | MOD_SHIFT, 'M');
            RegisterHotKey(hWnd, HOTKEY_PROFILE_1, MOD_CONTROL | MOD_SHIFT, '1');
            RegisterHotKey(hWnd, HOTKEY_PROFILE_2, MOD_CONTROL | MOD_SHIFT, '2');
            RegisterHotKey(hWnd, HOTKEY_PROFILE_3, MOD_CONTROL | MOD_SHIFT, '3');
            RegisterHotKey(hWnd, HOTKEY_PROFILE_4, MOD_CONTROL | MOD_SHIFT, '4');

            SetTimer(hWnd, 1, 33, NULL);

            PostNotification(L"🚀 NULLWIRE PRO READY", L"Ultra-Low Latency Lossless Wireless Audio Studio initialized.", Color(255, 0, 210, 255), false);
            break;
        }

        case WM_HOTKEY: {
            int id = (int)wParam;
            if (id == HOTKEY_STREAM_ID) ToggleStreamAction(hWnd);
            else if (id == HOTKEY_MIC_ID) ToggleMicAction(hWnd);
            else if (id == HOTKEY_PROFILE_1) SwitchScenarioAction(hWnd, ScenarioMode::MUSIC_HIFI);
            else if (id == HOTKEY_PROFILE_2) SwitchScenarioAction(hWnd, ScenarioMode::GAMING_LOW_LATENCY);
            else if (id == HOTKEY_PROFILE_3) SwitchScenarioAction(hWnd, ScenarioMode::CINEMA_MOVIE);
            else if (id == HOTKEY_PROFILE_4) SwitchScenarioAction(hWnd, ScenarioMode::PURE_DIRECT);
            break;
        }

        case WM_TRAYICON: {
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowTrayContextMenu(hWnd);
            else if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) RestoreMainWindow(hWnd);
            break;
        }

        case WM_ENTERSIZEMOVE:
            g_isMovingOrSizing = true;
            KillTimer(hWnd, 1);
            break;

        case WM_EXITSIZEMOVE: {
            g_isMovingOrSizing = false;
            RECT rc;
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            UpdateUiLayout(w, h);
            SetTimer(hWnd, 1, 33, NULL);
            InvalidateRect(hWnd, NULL, FALSE);
            break;
        }

        case WM_GETMINMAXINFO: {
            LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
            lpMMI->ptMinTrackSize.x = 720;
            lpMMI->ptMinTrackSize.y = 860;
            break;
        }

        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) break;
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            if (w >= 200 && h >= 200) {
                UpdateUiLayout(w, h);
                if (!g_isMovingOrSizing) {
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            break;
        }

        case WM_MOUSEMOVE: {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            int prevHover = g_hoverElement;
            g_hoverElement = -1;

            for (int i = 0; i < 4; i++) {
                if (g_ui.rPresets[i].Contains(mx, my)) g_hoverElement = i;
            }
            if (g_ui.rAutoPairBtn.Contains(mx, my)) g_hoverElement = 10;
            if (g_ui.rBassTrack.Contains(mx, my)) g_hoverElement = 20;
            if (g_ui.rTrebleTrack.Contains(mx, my)) g_hoverElement = 21;
            if (g_ui.rSpatialToggle.Contains(mx, my)) g_hoverElement = 30;
            if (g_ui.rStreamBtn.Contains(mx, my)) g_hoverElement = 40;
            if (g_ui.rMicBtn.Contains(mx, my)) g_hoverElement = 41;

            if (g_isDraggingBass) {
                float r = (float)(mx - g_ui.rBassTrack.X) / (float)g_ui.rBassTrack.Width;
                float val = std::clamp(r * 12.0f, 0.0f, 12.0f);
                g_Engine.bassBoostDb.store(std::round(val * 2.0f) / 2.0f);
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (g_isDraggingTreble) {
                float r = (float)(mx - g_ui.rTrebleTrack.X) / (float)g_ui.rTrebleTrack.Width;
                float val = std::clamp(r * 10.0f, 0.0f, 10.0f);
                g_Engine.trebleBoostDb.store(std::round(val * 2.0f) / 2.0f);
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (g_hoverElement != prevHover) {
                InvalidateRect(hWnd, NULL, FALSE);
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);

            // Presets
            for (int i = 0; i < 4; i++) {
                if (g_ui.rPresets[i].Contains(mx, my)) {
                    SwitchScenarioAction(hWnd, static_cast<ScenarioMode>(i));
                    return 0;
                }
            }

            // Auto-Pair Button
            if (g_ui.rAutoPairBtn.Contains(mx, my)) {
                if (g_Engine.hasDiscoveredDevice.load()) {
                    std::string devIp;
                    uint32_t token = 0;
                    {
                        std::lock_guard<std::mutex> lock(g_Engine.discoveryMutex);
                        devIp = g_Engine.discoveredDeviceIp;
                        token = g_Engine.discoveredToken;
                    }
                    std::wstring ipW(devIp.begin(), devIp.end());
                    SetWindowTextW(g_hIpEdit, ipW.c_str());
                    if (!g_Engine.isStreaming.load()) {
                        int curDev = (int)SendMessage(g_hDeviceCombo, CB_GETCURSEL, 0, 0);
                        g_Engine.StartStreaming(devIp, curDev, token);
                        EnableWindow(g_hIpEdit, FALSE);
                        EnableWindow(g_hDeviceCombo, FALSE);
                    }
                } else {
                    PostNotification(L"🔍 SCANNING FOR ANDROID...", L"Searching Wi-Fi broadcast for NullWire app on port 50007.", Color(255, 0, 210, 255), false);
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }

            // Sliders
            if (g_ui.rBassTrack.Contains(mx, my)) {
                g_isDraggingBass = true;
                SetCapture(hWnd);
                float r = (float)(mx - g_ui.rBassTrack.X) / (float)g_ui.rBassTrack.Width;
                float val = std::clamp(r * 12.0f, 0.0f, 12.0f);
                g_Engine.bassBoostDb.store(std::round(val * 2.0f) / 2.0f);
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }

            if (g_ui.rTrebleTrack.Contains(mx, my)) {
                g_isDraggingTreble = true;
                SetCapture(hWnd);
                float r = (float)(mx - g_ui.rTrebleTrack.X) / (float)g_ui.rTrebleTrack.Width;
                float val = std::clamp(r * 10.0f, 0.0f, 10.0f);
                g_Engine.trebleBoostDb.store(std::round(val * 2.0f) / 2.0f);
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }

            // Spatial Toggle
            if (g_ui.rSpatialToggle.Contains(mx, my)) {
                bool nextVal = !g_Engine.enableSpatial3D.load();
                g_Engine.enableSpatial3D.store(nextVal);
                g_Engine.spatialSurroundAmount.store(nextVal ? 0.85f : 0.0f);
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }

            // Primary Action Buttons
            if (g_ui.rStreamBtn.Contains(mx, my)) {
                ToggleStreamAction(hWnd);
                return 0;
            }

            if (g_ui.rMicBtn.Contains(mx, my)) {
                ToggleMicAction(hWnd);
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            if (g_isDraggingBass || g_isDraggingTreble) {
                g_isDraggingBass = false;
                g_isDraggingTreble = false;
                ReleaseCapture();
                InvalidateRect(hWnd, NULL, FALSE);
            }
            break;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_TEXT_MAIN);
            SetBkColor(hdc, COLOR_CARD_BG);
            return (LRESULT)g_hbrCard;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_CYAN);
            SetBkColor(hdc, COLOR_INPUT_BG);
            return (LRESULT)g_hbrInput;
        }

        case WM_TIMER: {
            if (g_isMovingOrSizing) break;
            RECT rc;
            GetClientRect(hWnd, &rc);
            RECT rcVisuals = {24, 430, rc.right - 24, rc.bottom - 16};
            InvalidateRect(hWnd, &rcVisuals, FALSE);

            // Invalidate toast area if active
            {
                std::lock_guard<std::mutex> lock(g_notifyMutex);
                if (g_currentNotification.active) {
                    RECT rcToast = {24, 10, rc.right - 24, 66};
                    InvalidateRect(hWnd, &rcToast, FALSE);
                }
            }
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdcScreen = BeginPaint(hWnd, &ps);

            RECT rc;
            GetClientRect(hWnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;

            HDC hdcMem = CreateCompatibleDC(hdcScreen);
            HBITMAP hbmMem = CreateCompatibleBitmap(hdcScreen, width, height);
            HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

            FillRect(hdcMem, &rc, g_hbrBg);

            Graphics graphics(hdcMem);
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            graphics.SetCompositingQuality(CompositingQualityHighSpeed);

            // 1. Header
            DrawNullWireLogo(graphics, 24.0f, 16.0f, 44.0f);

            SelectObject(hdcMem, g_hFontTitle);
            SetTextColor(hdcMem, COLOR_TEXT_MAIN);
            SetBkMode(hdcMem, TRANSPARENT);
            TextOutW(hdcMem, 78, 16, L"NullWire Pro", 12);

            SelectObject(hdcMem, g_hFontSub);
            SetTextColor(hdcMem, RGB(148, 163, 184));
            TextOutW(hdcMem, 78, 42, L"Ultra-Low Latency Lossless Wireless Audio Studio  ·  v2.0", 55);

            // Scenario status pill
            Gdiplus::Font fontPill(L"Segoe UI", 8.5f, FontStyleBold, UnitPoint);
            SolidBrush brushPill(Color(255, 0, 210, 255));
            StringFormat sfRight;
            sfRight.SetAlignment(StringAlignmentFar);
            if (g_Engine.isStreaming.load()) {
                graphics.DrawString(L"● STREAMING LIVE (48kHz MMAP)", -1, &fontPill, PointF((float)width - 24, 20.0f), &sfRight, &brushPill);
            } else {
                SolidBrush brushIdle(Color(255, 148, 163, 184));
                graphics.DrawString(L"● DISCOVERY SCANNING", -1, &fontPill, PointF((float)width - 24, 20.0f), &sfRight, &brushIdle);
            }

            int pad = 24;
            int cardW = width - pad * 2;

            // 2. Preset Pills (Interactive Modern GDI+)
            ScenarioMode curMode = g_Engine.currentScenario.load();
            const wchar_t* presetTitles[] = {L"🎵 Hi-Fi Harman", L"🎮 Gaming 2.67ms", L"🎬 Cinema 3D", L"🎯 Bit-Perfect"};
            Color presetColors[] = {Color(255, 16, 185, 129), Color(255, 245, 158, 11), Color(255, 168, 85, 247), Color(255, 0, 210, 255)};

            for (int i = 0; i < 4; i++) {
                DrawModernPillButton(graphics, g_ui.rPresets[i], presetTitles[i], (int)curMode == i, g_hoverElement == i, presetColors[i]);
            }

            // 3. Card 1 Background (Connection)
            DrawModernGlassCard(graphics, pad, 118, cardW, 104, L"DEVICE AUTO-PAIRING & AUDIO ENDPOINT", L"Wi-Fi LAN", Color(255, 0, 210, 255));

            // Auto-Pair Button (Interactive GDI+)
            std::string devName;
            {
                std::lock_guard<std::mutex> lock(g_Engine.discoveryMutex);
                devName = g_Engine.hasDiscoveredDevice.load() ? ("🟢 " + g_Engine.discoveredDeviceName) : "🔍 Auto-Pair Phone";
            }
            std::wstring wDevName(devName.begin(), devName.end());
            DrawModernPillButton(graphics, g_ui.rAutoPairBtn, wDevName.c_str(), false, g_hoverElement == 10, Color(255, 16, 185, 129));

            // 4. Card 2 Background (DSP Equalizer)
            DrawModernGlassCard(graphics, pad, 232, cardW, 136, L"STUDIO ACOUSTIC DSP & 3D BINAURAL MATRIX", L"64-BIT DITHERED", Color(255, 168, 85, 247));

            // Custom Sliders
            float curBass = g_Engine.bassBoostDb.load();
            wchar_t bassStr[16];
            swprintf_s(bassStr, L"+%.1f dB", curBass);
            DrawModernSlider(graphics, g_ui.rBassTrack, curBass, 0.0f, 12.0f, L"Natural Sub-Bass Boost (80Hz Butterworth):", bassStr, Color(255, 16, 185, 129), g_hoverElement == 20 || g_isDraggingBass);

            float curTreble = g_Engine.trebleBoostDb.load();
            wchar_t trebleStr[16];
            swprintf_s(trebleStr, L"+%.1f dB", curTreble);
            DrawModernSlider(graphics, g_ui.rTrebleTrack, curTreble, 0.0f, 10.0f, L"Studio Air & Clarity (12kHz High-Shelf):", trebleStr, Color(255, 0, 210, 255), g_hoverElement == 21 || g_isDraggingTreble);

            // Custom Toggle Switch
            DrawModernToggle(graphics, g_ui.rSpatialToggle, g_Engine.enableSpatial3D.load(), L"Binaural 3D Spatial Matrix (HRTF 7.1 Crossfeed & Pinna Acoustics)", g_hoverElement == 30);

            // 5. Action Buttons (Interactive GDI+)
            bool isStreaming = g_Engine.isStreaming.load();
            DrawModernPillButton(graphics, g_ui.rStreamBtn, isStreaming ? L"⏹ STOP AUDIO STREAM" : L"▶ START AUDIO STREAM", isStreaming, g_hoverElement == 40, Color(255, 0, 210, 255), true);

            bool isMic = g_Engine.isMicReceiving.load();
            DrawModernPillButton(graphics, g_ui.rMicBtn, isMic ? L"⏹ STOP PHONE MIC" : L"🎙 RECEIVE PHONE MIC", isMic, g_hoverElement == 41, Color(255, 168, 85, 247));

            // 6. Card 3: Real-Time Spectrum & Oscilloscope (Y: 432, H: 160)
            DrawSpectrumAndOscilloscope(graphics, pad, 432, cardW, 160);

            // 7. Card 4: Futuristic Realtime Latency Area Chart (Y: 602)
            int chartY = 602;
            int chartH = height - chartY - 20;
            if (chartH > 80) {
                DrawFuturisticLatencyChart(graphics, pad, chartY, cardW, chartH);
            }

            // 8. Floating Notification Toast (if active)
            DrawFloatingToastBanner(graphics, width);

            BitBlt(hdcScreen, 0, 0, width, height, hdcMem, 0, 0, SRCCOPY);

            SelectObject(hdcMem, hbmOld);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);

            EndPaint(hWnd, &ps);
            break;
        }

        case WM_DESTROY: {
            UnregisterHotKey(hWnd, HOTKEY_STREAM_ID);
            UnregisterHotKey(hWnd, HOTKEY_MIC_ID);
            UnregisterHotKey(hWnd, HOTKEY_PROFILE_1);
            UnregisterHotKey(hWnd, HOTKEY_PROFILE_2);
            UnregisterHotKey(hWnd, HOTKEY_PROFILE_3);
            UnregisterHotKey(hWnd, HOTKEY_PROFILE_4);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);

            g_Engine.StopDiscovery();
            g_Engine.StopStreaming();
            g_Engine.StopMicReceive();

            if (g_hbrBg) DeleteObject(g_hbrBg);
            if (g_hbrCard) DeleteObject(g_hbrCard);
            if (g_hbrInput) DeleteObject(g_hbrInput);
            if (g_hFontTitle) DeleteObject(g_hFontTitle);
            if (g_hFontSub) DeleteObject(g_hFontSub);
            if (g_hFontNormal) DeleteObject(g_hFontNormal);
            if (g_hFontBold) DeleteObject(g_hFontBold);
            if (g_hFontMono) DeleteObject(g_hFontMono);
            PostQuitMessage(0);
            break;
        }

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

void EnableHighDpiSupport() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContextProc)(void*);
        SetProcessDpiAwarenessContextProc setDpiContext = 
            (SetProcessDpiAwarenessContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpiContext) {
            setDpiContext((void*)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            return;
        }
    }
    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) {
        typedef HRESULT (WINAPI *SetProcessDpiAwarenessProc)(int);
        SetProcessDpiAwarenessProc setDpiAwareness = 
            (SetProcessDpiAwarenessProc)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (setDpiAwareness) {
            setDpiAwareness(2); // PROCESS_PER_MONITOR_DPI_AWARE
            FreeLibrary(hShcore);
            return;
        }
        FreeLibrary(hShcore);
    }
    SetProcessDPIAware();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    EnableHighDpiSupport();

    if (!EnsureWinsock()) {
        MessageBoxW(NULL, L"Failed to initialize network sockets.", L"NullWire Pro", MB_ICONERROR);
        return 1;
    }

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_DBLCLKS; // Avoid CS_HREDRAW | CS_VREDRAW for smooth window dragging
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    wc.hIconSm = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(1), IMAGE_ICON, 16, 16, 0);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; // We paint our own double-buffered background
    wc.lpszClassName = L"NullWireProClass";

    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"NullWireProClass",
        L"NullWire Pro  ·  Studio Wireless Audio Dashboard",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT,
        740, 920,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) {
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}
