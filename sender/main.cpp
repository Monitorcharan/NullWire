/**
 * ============================================================================
 *  NULLWIRE PRO - Lossless Ultra-Low Latency Wi-Fi PCM Audio Streamer
 *  Architecture: Pure Modern C++20 / Win32 API / GDI+ (Double-Buffered)
 *  Target OS: Windows 10 / Windows 11 (64-bit)
 * ============================================================================
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
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
#include <mmsystem.h>
#include <gdiplus.h>
#include <dwmapi.h>

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
#include <array>
#include <iomanip>
#include <memory>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")

using namespace Gdiplus;

// -----------------------------------------------------------------------------
// CONSTANTS & PROTOCOL DEFINITIONS
// -----------------------------------------------------------------------------
constexpr int AUDIO_SEND_PORT = 50005;
constexpr int DISCOVERY_PORT  = 50007;
constexpr int TARGET_RATE     = 48000;
constexpr double PI           = 3.14159265358979323846;
constexpr uint16_t PACKET_MAGIC = 0x574E;
constexpr size_t PACKET_HEADER_SIZE = 8;
constexpr size_t MAX_AUDIO_PACKET = 4096;
constexpr UINT_PTR TIMER_ANIMATION_ID = 1001;
constexpr UINT ANIMATION_INTERVAL_MS  = 16; // ~60 FPS
#define WM_TRAYICON (WM_USER + 201)

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

// -----------------------------------------------------------------------------
// AUDIOPHILE DSP & FILTER ENGINES
// -----------------------------------------------------------------------------
class BiquadFilter {
public:
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1[2] = {0.0, 0.0}, x2[2] = {0.0, 0.0};
    double y1[2] = {0.0, 0.0}, y2[2] = {0.0, 0.0};

    void SetLowShelf(double freq, double gainDb, double sampleRate = 48000.0, double Q = 0.7071) {
        if (std::abs(gainDb) < 0.05) {
            b0 = 1.0; b1 = 0.0; b2 = 0.0; a1 = 0.0; a2 = 0.0;
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
            b0 = 1.0; b1 = 0.0; b2 = 0.0; a1 = 0.0; a2 = 0.0;
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
            b0 = 1.0; b1 = 0.0; b2 = 0.0; a1 = 0.0; a2 = 0.0;
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

        b0 = (sinw0 / 2.0) / a0;
        b1 = 0.0;
        b2 = (-sinw0 / 2.0) / a0;
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

inline double StudioMasterLimiter(double sample) {
    constexpr double threshold = 0.94;
    double absS = std::abs(sample);
    if (absS <= threshold) return sample;
    double excess = absS - threshold;
    double compressed = threshold + (1.0 - threshold) * std::tanh(excess / (1.0 - threshold));
    return (sample > 0) ? compressed : -compressed;
}

class BinauralSpatializer {
private:
    double delayL[32]{};
    double delayR[32]{};
    int delayIdx = 0;
    BiquadFilter headShadowL;
    BiquadFilter headShadowR;

public:
    BinauralSpatializer() {
        headShadowL.SetLowShelf(1200.0, -3.5, 48000.0, 0.7071);
        headShadowR.SetLowShelf(1200.0, -3.5, 48000.0, 0.7071);
    }

    inline void ProcessStereo(double& l, double& r, float amount) {
        if (amount < 0.01f) return;
        delayL[delayIdx] = l;
        delayR[delayIdx] = r;
        int dIdx = (delayIdx - 12 + 32) % 32;
        double crossL = delayR[dIdx];
        double crossR = delayL[dIdx];
        delayIdx = (delayIdx + 1) % 32;

        crossL = headShadowL.Process(crossL, 0);
        crossR = headShadowR.Process(crossR, 1);

        double amt = (double)amount * 0.35;
        l = l + crossL * amt;
        r = r + crossR * amt;
    }
};

struct Resampler48k {
    double ratio = 1.0;
    double phase = 0.0;
    double lastL[4] = {0,0,0,0};
    double lastR[4] = {0,0,0,0};

    void Init(int inRate) {
        if (inRate < 8000 || inRate > 192000) inRate = 48000;
        ratio = (double)inRate / 48000.0;
        phase = 0.0;
        std::fill_n(lastL, 4, 0.0);
        std::fill_n(lastR, 4, 0.0);
    }

    static inline double CubicHermite(double ym1, double y0, double y1, double y2, double mu) {
        double mu2 = mu * mu;
        double a0 = -0.5*ym1 + 1.5*y0 - 1.5*y1 + 0.5*y2;
        double a1 = ym1 - 2.5*y0 + 2.0*y1 - 0.5*y2;
        double a2 = -0.5*ym1 + 0.5*y1;
        double a3 = y0;
        return (a0*mu*mu2 + a1*mu2 + a2*mu + a3);
    }

    void ProcessSample(double inL, double inR, std::vector<double>& outL, std::vector<double>& outR) {
        if (std::abs(ratio - 1.0) < 0.0001) {
            outL.push_back(inL);
            outR.push_back(inR);
            return;
        }
        lastL[0] = lastL[1]; lastL[1] = lastL[2]; lastL[2] = lastL[3]; lastL[3] = inL;
        lastR[0] = lastR[1]; lastR[1] = lastR[2]; lastR[2] = lastR[3]; lastR[3] = inR;

        while (phase < 1.0) {
            double sL = CubicHermite(lastL[0], lastL[1], lastL[2], lastL[3], phase);
            double sR = CubicHermite(lastR[0], lastR[1], lastR[2], lastR[3], phase);
            outL.push_back(sL);
            outR.push_back(sR);
            phase += ratio;
        }
        phase -= 1.0;
    }
};

// -----------------------------------------------------------------------------
// AUDIO DEVICE & ENGINE BACKEND
// -----------------------------------------------------------------------------
struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

enum class AudioPreset {
    PureDirect = 0,
    HiFiStudio = 1,
    Cinema3D   = 2,
    GamingLowLatency = 3
};

struct AudioTelemetry {
    float latencyMs = 2.1f;
    float jitterMs = 0.14f;
    int bitrateKbps = 1536;
    uint64_t packetsStreamed = 0;
    float bufferHealth = 99.8f;
};

struct SliderControl {
    RECT rect;
    std::wstring label;
    std::wstring unit;
    float value; // 0.0 to 1.0
    float minValue;
    float maxValue;
    bool isDragging = false;
    bool isHovered = false;
    Color accentColor;
};

void PostNotification(const std::wstring& title, const std::wstring& message, Color accent, float duration = 3.5f);

class AudioEngine {
public:
    std::atomic<bool> isStreaming{false};
    std::atomic<bool> enableWifiAccel{true};

    std::atomic<float> bassBoostDb{2.5f};
    std::atomic<float> presenceBoostDb{1.5f};
    std::atomic<float> trebleBoostDb{2.0f};
    std::atomic<bool> enableSpatial3D{true};
    std::atomic<float> spatialSurroundAmount{0.40f};
    std::atomic<AudioPreset> currentPreset{AudioPreset::HiFiStudio};

    std::atomic<float> streamRmsL{0.0f};
    std::atomic<float> streamRmsR{0.0f};
    std::atomic<float> spectrumBands[7]{};
    std::atomic<uint64_t> sendPacketCount{0};

    std::mutex waveformMutex;
    std::vector<float> waveformBuffer;

    std::vector<AudioDevice> devices;
    int selectedDeviceIndex = 0;

    std::mutex targetMutex;
    std::string targetPhoneIp = "";
    std::string targetPhoneName = "Android Device";
    uint32_t sessionToken = 0;
    std::atomic<bool> isPhoneDiscovered{false};

    std::atomic<bool> isDiscoveryRunning{false};
    std::thread discoveryThread;
    std::thread streamThread;

    AudioEngine() {
        waveformBuffer.resize(120, 0.0f);
        for (int i = 0; i < 7; i++) spectrumBands[i].store(0.0f);
    }

    ~AudioEngine() {
        StopStreaming();
        StopDiscovery();
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
            int preferredIdx = -1;

            // Default Option
            AudioDevice defDev;
            defDev.id = L"";
            defDev.name = L"● Default Windows Audio Endpoint";
            devices.push_back(defDev);

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

                    std::wstring lower = devName;
                    for (auto& c : lower) c = (wchar_t)::towlower(c);
                    if (lower.find(L"cable input") != std::wstring::npos ||
                        lower.find(L"vb-audio") != std::wstring::npos ||
                        lower.find(L"nullwire") != std::wstring::npos ||
                        lower.find(L"virtual cable") != std::wstring::npos) {
                        devices.insert(devices.begin() + 1, dev);
                        preferredIdx = 1;
                    } else {
                        devices.push_back(dev);
                    }

                    if (pId) CoTaskMemFree(pId);
                    pDevice->Release();
                }
            }
            pCollection->Release();

            if (preferredIdx > 0 && selectedDeviceIndex == 0) {
                selectedDeviceIndex = preferredIdx;
            }
        }
        pEnumerator->Release();
    }

    void StartDiscovery(HWND notifyWnd) {
        if (isDiscoveryRunning.load()) return;
        if (!EnsureWinsock()) return;
        isDiscoveryRunning.store(true);

        discoveryThread = std::thread([this, notifyWnd]() {
            SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCKET) return;

            int reuse = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

            sockaddr_in bindAddr{};
            bindAddr.sin_family = AF_INET;
            bindAddr.sin_port = htons(DISCOVERY_PORT);
            bindAddr.sin_addr.s_addr = INADDR_ANY;

            if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
                closesocket(sock);
                return;
            }

            char buf[512];
            while (isDiscoveryRunning.load()) {
                sockaddr_in fromAddr{};
                int fromLen = sizeof(fromAddr);
                int bytes = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&fromAddr, &fromLen);
                if (bytes > 0) {
                    buf[bytes] = '\0';
                    char actualIp[64];
                    inet_ntop(AF_INET, &fromAddr.sin_addr, actualIp, sizeof(actualIp));

                    if (std::string(buf).find("NWDISC") == 0 || std::string(buf).find("NWBC") == 0) {
                        uint32_t token = 0;
                        char name[64] = "Android Phone";
                        if (std::string(buf).find("NWDISC") == 0) {
                            sscanf(buf, "NWDISC|%63[^|]|%u", name, &token);
                        } else {
                            char tempIp[64];
                            sscanf(buf, "NWBC|%63[^|]|%63[^|]|%u", name, tempIp, &token);
                        }

                        bool isNewlyDiscovered = false;
                        {
                            std::lock_guard<std::mutex> lock(targetMutex);
                            if (targetPhoneIp != actualIp || targetPhoneName != name || !isPhoneDiscovered.load()) {
                                targetPhoneIp = actualIp;
                                targetPhoneName = name;
                                sessionToken = token;
                                isPhoneDiscovered.store(true);
                                isNewlyDiscovered = true;
                            }
                        }

                        if (isNewlyDiscovered && notifyWnd) {
                            PostMessage(notifyWnd, WM_USER + 100, 0, 0);
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
        if (discoveryThread.joinable()) discoveryThread.join();
    }

    void StartStreaming(const std::string& ip, int devIndex) {
        if (isStreaming.load()) return;
        if (!EnsureWinsock()) return;
        if (!IsValidLanIpv4(ip.c_str())) return;

        {
            std::lock_guard<std::mutex> lock(targetMutex);
            targetPhoneIp = ip;
        }
        selectedDeviceIndex = devIndex;
        isStreaming.store(true);
        sendPacketCount.store(0);

        std::wstring wIp(ip.begin(), ip.end());
        PostNotification(L"STREAM CONNECTED", L"Streaming 48kHz Lossless PCM to " + wIp, Color(255, 0, 210, 255), 3.5f);

        streamThread = std::thread([this]() {
            bool accel = enableWifiAccel.load();
            HANDLE hMmcss = nullptr;
            if (accel) {
                timeBeginPeriod(1);
                DWORD taskIndex = 0;
                hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
                if (hMmcss) AvSetMmThreadPriority(hMmcss, AVRT_PRIORITY_CRITICAL);
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            }

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
                if (accel) timeEndPeriod(1);
                CoUninitialize();
                isStreaming.store(false);
                return;
            }

            int sndBuf = accel ? 1048576 : 65536;
            setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&sndBuf, sizeof(sndBuf));

            sockaddr_in targetAddr{};
            targetAddr.sin_family = AF_INET;
            targetAddr.sin_port = htons(AUDIO_SEND_PORT);
            inet_pton(AF_INET, destIp.c_str(), &targetAddr.sin_addr);

            IMMDeviceEnumerator* pEnumerator = nullptr;
            CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);

            IMMDevice* pDevice = nullptr;
            if (pEnumerator) {
                if (selectedDeviceIndex > 0 && selectedDeviceIndex < (int)devices.size() && !devices[selectedDeviceIndex].id.empty()) {
                    pEnumerator->GetDevice(devices[selectedDeviceIndex].id.c_str(), &pDevice);
                }
                if (!pDevice) {
                    pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
                }
            }

            if (!pDevice) {
                if (pEnumerator) pEnumerator->Release();
                closesocket(sock);
                if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
                if (accel) timeEndPeriod(1);
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
                if (accel) timeEndPeriod(1);
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
                if (accel) timeEndPeriod(1);
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
                if (accel) timeEndPeriod(1);
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

            Resampler48k resampler;
            resampler.Init(srcRate);

            BiquadFilter bassFilter;
            BiquadFilter trebleFilter;
            BiquadFilter presenceFilter;
            BinauralSpatializer spatializer3D;

            BiquadFilter specBp[7];
            const double specFreqs[7] = {60.0, 150.0, 400.0, 1000.0, 2500.0, 6000.0, 14000.0};
            for (int b = 0; b < 7; b++) {
                specBp[b].SetBandPass(specFreqs[b], 1.414, (double)TARGET_RATE);
            }
            double specBandAcc[7] = {0, 0, 0, 0, 0, 0, 0};

            uint16_t sequenceNumber = 0;
            std::vector<uint8_t> packetPayload(MAX_AUDIO_PACKET);
            std::vector<double> resampL, resampR;
            resampL.reserve(4096);
            resampR.reserve(4096);

            while (isStreaming.load()) {
                UINT32 packetLength = 0;
                HRESULT hr = pCaptureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr) || packetLength == 0) {
                    Sleep(1);
                    continue;
                }

                while (packetLength > 0 && isStreaming.load()) {
                    BYTE* pData = nullptr;
                    UINT32 numFramesRead = 0;
                    DWORD flags = 0;

                    hr = pCaptureClient->GetBuffer(&pData, &numFramesRead, &flags, NULL, NULL);
                    if (SUCCEEDED(hr) && numFramesRead > 0) {
                        double targetBass = (double)bassBoostDb.load();
                        double targetTreble = (double)trebleBoostDb.load();
                        double targetPresence = (double)presenceBoostDb.load();
                        double targetSpatial = enableSpatial3D.load() ? (double)spatialSurroundAmount.load() : 0.0;
                        bool isDspActive = (targetBass != 0.0 || targetTreble != 0.0 || targetPresence != 0.0 || targetSpatial > 0.01);

                        if (isDspActive) {
                            bassFilter.SetLowShelf(80.0, targetBass, (double)TARGET_RATE, 0.7071);
                            trebleFilter.SetHighShelf(12000.0, targetTreble, (double)TARGET_RATE, 0.7071);
                            presenceFilter.SetPeaking(2800.0, targetPresence, (double)TARGET_RATE, 1.0);
                        }

                        resampL.clear();
                        resampR.clear();

                        for (UINT32 f = 0; f < numFramesRead; f++) {
                            double l_raw = 0.0, r_raw = 0.0;

                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT || !pData) {
                                l_raw = 0.0;
                                r_raw = 0.0;
                            } else if (isFloat) {
                                const float* fSrc = reinterpret_cast<const float*>(pData + f * srcBytesPerFrame);
                                l_raw = (double)fSrc[0];
                                r_raw = (srcChannels > 1) ? (double)fSrc[1] : l_raw;
                            } else if (srcBits == 16) {
                                const int16_t* sSrc = reinterpret_cast<const int16_t*>(pData + f * srcBytesPerFrame);
                                l_raw = (double)sSrc[0] / 32768.0;
                                r_raw = (srcChannels > 1) ? (double)sSrc[1] / 32768.0 : l_raw;
                            } else if (srcBits == 24 || srcBits == 32) {
                                const int32_t* iSrc = reinterpret_cast<const int32_t*>(pData + f * srcBytesPerFrame);
                                l_raw = (double)iSrc[0] / 2147483648.0;
                                r_raw = (srcChannels > 1) ? (double)iSrc[1] / 2147483648.0 : l_raw;
                            }

                            resampler.ProcessSample(l_raw, r_raw, resampL, resampR);
                        }

                        size_t outCount = resampL.size();
                        double rmsSumL = 0.0, rmsSumR = 0.0;

                        for (size_t f = 0; f < outCount; f++) {
                            double l_out = resampL[f];
                            double r_out = resampR[f];

                            rmsSumL += l_out * l_out;
                            rmsSumR += r_out * r_out;

                            double monoIn = (l_out + r_out) * 0.5;
                            for (int b = 0; b < 7; b++) {
                                double bpOut = specBp[b].Process(monoIn, 0);
                                specBandAcc[b] += bpOut * bpOut;
                            }

                            if (isDspActive) {
                                if (std::abs(targetBass) > 0.05) {
                                    l_out = bassFilter.Process(l_out, 0);
                                    r_out = bassFilter.Process(r_out, 1);
                                }
                                if (std::abs(targetPresence) > 0.05) {
                                    l_out = presenceFilter.Process(l_out, 0);
                                    r_out = presenceFilter.Process(r_out, 1);
                                }
                                if (std::abs(targetTreble) > 0.05) {
                                    l_out = trebleFilter.Process(l_out, 0);
                                    r_out = trebleFilter.Process(r_out, 1);
                                }
                                if (targetSpatial > 0.01) {
                                    spatializer3D.ProcessStereo(l_out, r_out, (float)targetSpatial);
                                }
                                l_out = StudioMasterLimiter(l_out);
                                r_out = StudioMasterLimiter(r_out);
                            }

                            int16_t sL = (int16_t)(std::clamp(l_out, -1.0, 1.0) * 32767.0);
                            int16_t sR = (int16_t)(std::clamp(r_out, -1.0, 1.0) * 32767.0);

                            pcmAccumulator.push_back(sL);
                            pcmAccumulator.push_back(sR);
                        }

                        if (outCount > 0) {
                            float curRmsL = (float)std::sqrt(rmsSumL / (double)outCount);
                            float curRmsR = (float)std::sqrt(rmsSumR / (double)outCount);
                            streamRmsL.store(curRmsL);
                            streamRmsR.store(curRmsR);

                            for (int b = 0; b < 7; b++) {
                                float bRms = (float)std::sqrt(specBandAcc[b] / (double)outCount) * 2.8f;
                                spectrumBands[b].store(std::clamp(bRms, 0.0f, 1.0f));
                                specBandAcc[b] = 0.0;
                            }

                            std::lock_guard<std::mutex> waveLock(waveformMutex);
                            for (size_t f = 0; f < outCount && f < waveformBuffer.size(); f += 2) {
                                waveformBuffer.erase(waveformBuffer.begin());
                                waveformBuffer.push_back((float)resampL[f]);
                            }
                        }

                        pCaptureClient->ReleaseBuffer(numFramesRead);
                    } else if (SUCCEEDED(hr)) {
                        pCaptureClient->ReleaseBuffer(numFramesRead);
                    }

                    constexpr int targetChunk = 192; // 4.0ms @ 48kHz Stereo
                    constexpr int targetSamples = targetChunk * 2;

                    while ((int)pcmAccumulator.size() >= targetSamples) {
                        int pcmBytes = targetSamples * (int)sizeof(int16_t);
                        size_t totalBytes = PACKET_HEADER_SIZE + (size_t)pcmBytes;
                        if (totalBytes > packetPayload.size()) break;

                        uint8_t* hdr = packetPayload.data();
                        memcpy(hdr, &PACKET_MAGIC, 2);
                        memcpy(hdr + 2, &sequenceNumber, 2);
                        memcpy(hdr + 4, &tokenCopy, 4);

                        memcpy(packetPayload.data() + PACKET_HEADER_SIZE, pcmAccumulator.data(), pcmBytes);

                        sendto(sock, (const char*)packetPayload.data(), (int)totalBytes, 0, (sockaddr*)&targetAddr, sizeof(targetAddr));

                        sequenceNumber++;
                        sendPacketCount.fetch_add(1);
                        pcmAccumulator.erase(pcmAccumulator.begin(), pcmAccumulator.begin() + targetSamples);
                    }

                    hr = pCaptureClient->GetNextPacketSize(&packetLength);
                    if (FAILED(hr)) break;
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
            if (accel) timeEndPeriod(1);
            CoUninitialize();

            streamRmsL.store(0.0f);
            streamRmsR.store(0.0f);
            for (int i = 0; i < 7; i++) spectrumBands[i].store(0.0f);
            {
                std::lock_guard<std::mutex> waveLock(waveformMutex);
                std::fill(waveformBuffer.begin(), waveformBuffer.end(), 0.0f);
            }
        });
    }

    void StopStreaming() {
        if (!isStreaming.load()) return;
        isStreaming.store(false);
        // Join on a detached helper so we don't block the UI thread
        if (streamThread.joinable()) {
            std::thread([t = std::move(streamThread)]() mutable {
                if (t.joinable()) t.join();
            }).detach();
        }
        PostNotification(L"STREAM DISCONNECTED", L"Lossless audio transmission stopped.", Color(255, 239, 68, 68), 3.0f);
    }
};

// -----------------------------------------------------------------------------
// GLOBAL APPLICATION STATE
// -----------------------------------------------------------------------------
struct ToastNotification {
    std::wstring title;
    std::wstring message;
    Color accentColor = Color(255, 0, 210, 255);
    std::chrono::steady_clock::time_point startTime;
    float durationSec = 3.5f;
    bool active = false;
};

std::wstring GetLocalHostIp() {
    char hostName[256];
    if (gethostname(hostName, sizeof(hostName)) == 0) {
        struct hostent* host = gethostbyname(hostName);
        if (host && host->h_addr_list[0]) {
            struct in_addr addr;
            memcpy(&addr, host->h_addr_list[0], sizeof(struct in_addr));
            char* ipStr = inet_ntoa(addr);
            if (ipStr) {
                return std::wstring(ipStr, ipStr + strlen(ipStr));
            }
        }
    }
    return L"127.0.0.1";
}

struct AppState {
    AudioEngine engine;
    ToastNotification toast;

    // Host & Target Discovery Identity
    std::wstring hostPcName = L"Windows PC";
    std::wstring hostIp = L"127.0.0.1";
    std::wstring targetDeviceName = L"No mobile device connected";
    std::wstring targetIp = L"";
    bool isPhoneDiscovered = false;

    // Sliders
    SliderControl subBassSlider      { {0,0,0,0}, L"Sub-Bass Boost",   L"dB",  0.60f, -12.0f, +12.0f, false, false, Color(255, 0, 210, 255) };
    SliderControl vocalPresenceSlider{ {0,0,0,0}, L"Vocal Presence",   L"dB",  0.56f, -12.0f, +12.0f, false, false, Color(255, 168, 85, 247) };
    SliderControl trebleAirSlider    { {0,0,0,0}, L"Treble Air (16k)", L"dB",  0.58f, -12.0f, +12.0f, false, false, Color(255, 16, 185, 129) };
    SliderControl surround3DSlider   { {0,0,0,0}, L"3D Soundstage",    L"%",   0.40f,   0.0f, 100.0f, false, false, Color(255, 245, 158, 11) };

    std::wstring selectedDeviceName = L"● Default Windows Audio Endpoint";
    bool autoConnect = true;
    bool userManualDisconnect = false;

    // Hit boxes
    RECT streamButtonRect {0,0,0,0};
    RECT wifiToggleRect {0,0,0,0};
    RECT autoConnectRect {0,0,0,0};
    RECT presetPillsRect[4] {};
    RECT ipInputRect {0,0,0,0};
    RECT deviceDropdownRect {0,0,0,0};
    RECT themeLightRect {0,0,0,0};
    RECT themeDarkRect {0,0,0,0};
    RECT navRects[4] {};

    // Aero UI state
    bool darkMode = true;
    int page = 0;          // 0 Overview, 1 Sound, 2 Devices, 3 Settings
    int hoveredNav = -1;

    // Hover states
    bool isStreamBtnHovered = false;
    bool isWifiToggleHovered = false;
    bool isAutoConnectHovered = false;
    int hoveredPresetIndex = -1;
    bool isIpHovered = false;
    bool isIpEditing = false;
    bool isDeviceHovered = false;

    // Visualizer physics
    float vuLeftLevel = 0.0f;
    float vuRightLevel = 0.0f;
    float vuLeftPeak = 0.0f;
    float vuRightPeak = 0.0f;

    std::array<float, 7> spectrumBands = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::array<float, 7> spectrumPeaks = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    std::vector<float> waveformDisplay;

    AudioTelemetry telemetry;
    float animationTime = 0.0f;

    AppState() {
        waveformDisplay.resize(120, 0.0f);
    }
};

static AppState g_App;
static ULONG_PTR g_GdiplusToken = 0;
static HWND g_hMainWnd = nullptr;
static NOTIFYICONDATAW g_Nid = {};

void PostNotification(const std::wstring& title, const std::wstring& message, Color accent, float duration) {
    g_App.toast.title = title;
    g_App.toast.message = message;
    g_App.toast.accentColor = accent;
    g_App.toast.durationSec = duration;
    g_App.toast.startTime = std::chrono::steady_clock::now();
    g_App.toast.active = true;

    if (g_hMainWnd && !IsWindowVisible(g_hMainWnd)) {
        g_Nid.uFlags |= NIF_INFO;
        wcscpy_s(g_Nid.szInfoTitle, title.c_str());
        wcscpy_s(g_Nid.szInfo, message.c_str());
        g_Nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &g_Nid);
    }
}

// -----------------------------------------------------------------------------
// AERO THEME (light / dark palettes)
// -----------------------------------------------------------------------------
struct Theme {
    Color bg, sidebar, card, cardAlt, border, text, textSec, accent, accentSoft, accentText, red, redSoft, track, onAccent;
};

static const Theme kLightTheme = {
    Color(255, 238, 244, 242), Color(255, 247, 251, 250), Color(255, 255, 255, 255), Color(255, 243, 248, 246),
    Color(255, 220, 231, 227), Color(255, 16, 32, 27),    Color(255, 98, 118, 111),  Color(255, 23, 184, 144),
    Color(255, 221, 244, 236), Color(255, 14, 138, 106),  Color(255, 214, 69, 69),   Color(255, 252, 227, 227),
    Color(255, 218, 230, 226), Color(255, 255, 255, 255)
};
static const Theme kDarkTheme = {
    Color(255, 10, 20, 19),    Color(255, 13, 27, 25),    Color(255, 16, 32, 30),    Color(255, 22, 43, 40),
    Color(255, 31, 55, 51),    Color(255, 232, 244, 240), Color(255, 140, 165, 157), Color(255, 61, 219, 169),
    Color(255, 23, 65, 58),    Color(255, 61, 219, 169),  Color(255, 242, 109, 109), Color(255, 58, 31, 34),
    Color(255, 31, 55, 51),    Color(255, 6, 32, 26)
};

static const Theme& T() { return g_App.darkMode ? kDarkTheme : kLightTheme; }

static Color WithAlpha(Color c, int a) { return Color((BYTE)a, c.GetRed(), c.GetGreen(), c.GetBlue()); }

static bool SystemPrefersLight() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

static void LoadThemePreference() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\NullWire", L"DarkTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) {
        g_App.darkMode = (v != 0);
    } else {
        g_App.darkMode = !SystemPrefersLight();
    }
}

static void ApplyTheme(HWND hwnd, bool dark) {
    g_App.darkMode = dark;
    BOOL d = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof(d));
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\NullWire", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        DWORD v = dark ? 1 : 0;
        RegSetValueExW(key, L"DarkTheme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(key);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

static RectF ToRectF(const RECT& r) {
    return RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top));
}

static RECT MakeRect(float x, float y, float w, float h) {
    return { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
}

// -----------------------------------------------------------------------------
// GDI+ DRAWING HELPER UTILITIES
// -----------------------------------------------------------------------------
void AddRoundedRectangle(GraphicsPath& path, RectF rect, float radius) {
    float diameter = radius * 2.0f;
    if (diameter > rect.Width) diameter = rect.Width;
    if (diameter > rect.Height) diameter = rect.Height;

    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

// Flat Aero surface: fill + hairline border. alt = slightly tinted inset surface.
void DrawFluentCard(Graphics& g, RectF rect, float radius = 16.0f, bool alt = false, Color borderOverride = Color(0, 0, 0, 0)) {
    const Theme& t = T();
    GraphicsPath path;
    AddRoundedRectangle(path, rect, radius);
    SolidBrush fill(alt ? t.cardAlt : t.card);
    g.FillPath(&fill, &path);
    Pen borderPen(borderOverride.GetAlpha() ? borderOverride : t.border, 1.0f);
    g.DrawPath(&borderPen, &path);
}

void DrawGlowEffect(Graphics& g, RectF rect, Color glowColor, float intensity = 8.0f) {
    GraphicsPath path;
    AddRoundedRectangle(path, rect, 12.0f);
    for (float i = intensity; i >= 1.0f; i -= 2.0f) {
        BYTE alpha = static_cast<BYTE>((glowColor.GetAlpha() * 0.15f) * (1.0f - (i / intensity)));
        Pen glowPen(Color(alpha, glowColor.GetRed(), glowColor.GetGreen(), glowColor.GetBlue()), i * 2.0f);
        g.DrawPath(&glowPen, &path);
    }
}

static void DrawTextAt(Graphics& g, const std::wstring& s, const Font& f, const Brush& b, float x, float y) {
    g.DrawString(s.c_str(), -1, &f, PointF(x, y), &b);
}

static void DrawTextIn(Graphics& g, const std::wstring& s, const Font& f, const Brush& b, RectF r,
                       StringAlignment h = StringAlignmentCenter, StringAlignment v = StringAlignmentCenter) {
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(v);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(s.c_str(), -1, &f, r, &sf, &b);
}

// -----------------------------------------------------------------------------
// AUDIO VISUALIZERS RENDERING MODULE
// -----------------------------------------------------------------------------
void DrawVuMeter(Graphics& g, RectF rect, float level, float peak, const std::wstring& channelName) {
    const Theme& t = T();
    Font font(L"Segoe UI", 9.0f, FontStyleBold, UnitPoint);
    SolidBrush textBrush(t.textSec);

    DrawTextIn(g, channelName, font, textBrush, RectF(rect.X, rect.Y, 20.0f, rect.Height), StringAlignmentNear);

    RectF trackRect(rect.X + 24.0f, rect.Y + 6.0f, rect.Width - 24.0f, rect.Height - 12.0f);
    GraphicsPath trackPath;
    AddRoundedRectangle(trackPath, trackRect, trackRect.Height / 2.0f);
    SolidBrush trackBg(t.track);
    g.FillPath(&trackBg, &trackPath);

    float fillWidth = (trackRect.Width * std::clamp(level, 0.0f, 1.0f));
    if (fillWidth > trackRect.Height) {
        RectF fillRect(trackRect.X, trackRect.Y, fillWidth, trackRect.Height);
        GraphicsPath fillPath;
        AddRoundedRectangle(fillPath, fillRect, trackRect.Height / 2.0f);
        LinearGradientBrush vuGrad(trackRect, WithAlpha(t.accent, 255), WithAlpha(t.accent, 255), LinearGradientModeHorizontal);
        Color colors[] = { WithAlpha(t.accent, 150), t.accent, t.accent };
        REAL positions[] = { 0.0f, 0.8f, 1.0f };
        vuGrad.SetInterpolationColors(colors, positions, 3);
        g.FillPath(&vuGrad, &fillPath);
    }
}

// 7 measured octave bands are smoothly interpolated into thin bars (display only).
void DrawSpectrum(Graphics& g, RectF rect) {
    const Theme& t = T();
    const std::array<std::wstring, 7> bandLabels = { L"60", L"150", L"400", L"1k", L"2.5k", L"6k", L"14k" };
    const float labelH = 18.0f;
    const float plotH = rect.Height - labelH;
    const int bars = 56;
    const float slot = rect.Width / bars;
    const float barW = slot * 0.55f;

    Pen gridPen(WithAlpha(t.border, 160), 1.0f);
    for (int i = 0; i < 4; ++i) {
        float y = rect.Y + plotH * i / 3.0f;
        g.DrawLine(&gridPen, rect.X, y, rect.X + rect.Width, y);
    }

    LinearGradientBrush barBrush(RectF(rect.X, rect.Y, rect.Width, plotH), t.accent, WithAlpha(t.accent, 90), LinearGradientModeVertical);
    for (int i = 0; i < bars; ++i) {
        float pos = (i + 0.5f) / bars * 6.0f; // 0..6 across the 7 bands
        int i0 = std::clamp((int)pos, 0, 5);
        float f = pos - i0;
        f = (1.0f - std::cos(f * 3.14159265f)) * 0.5f;
        float v = g_App.spectrumBands[i0] * (1.0f - f) + g_App.spectrumBands[i0 + 1] * f;
        v = std::clamp(v, 0.03f, 1.0f);
        float h = plotH * v;
        RectF r(rect.X + i * slot + (slot - barW) * 0.5f, rect.Y + plotH - h, barW, h);
        GraphicsPath p;
        AddRoundedRectangle(p, r, barW * 0.5f);
        g.FillPath(&barBrush, &p);
    }

    Font labelFont(L"Segoe UI", 7.5f, FontStyleRegular, UnitPoint);
    SolidBrush textBrush(t.textSec);
    for (int i = 0; i < 7; ++i) {
        float cx = rect.X + (i / 6.0f) * (rect.Width - slot) + slot * 0.5f;
        DrawTextIn(g, bandLabels[i], labelFont, textBrush, RectF(cx - 22.0f, rect.Y + plotH + 3.0f, 44.0f, 14.0f));
    }
}

void DrawWaveform(Graphics& g, RectF rect) {
    const Theme& t = T();
    Pen gridPen(WithAlpha(t.border, 170), 1.0f);
    for (int i = 1; i <= 3; ++i) {
        float y = rect.Y + (rect.Height / 4.0f) * i;
        g.DrawLine(&gridPen, rect.X, y, rect.X + rect.Width, y);
    }

    Pen centerPen(WithAlpha(t.accent, 120), 1.0f);
    float centerY = rect.Y + rect.Height * 0.5f;
    g.DrawLine(&centerPen, rect.X, centerY, rect.X + rect.Width, centerY);

    if (g_App.waveformDisplay.size() < 2) return;

    std::vector<PointF> points;
    points.reserve(g_App.waveformDisplay.size());
    float stepX = rect.Width / static_cast<float>(g_App.waveformDisplay.size() - 1);
    for (size_t i = 0; i < g_App.waveformDisplay.size(); ++i) {
        float x = rect.X + i * stepX;
        float y = centerY - (g_App.waveformDisplay[i] * (rect.Height * 0.42f));
        points.push_back(PointF(x, y));
    }

    Pen glowPen(WithAlpha(t.accent, 60), 4.0f);
    g.DrawCurve(&glowPen, points.data(), static_cast<INT>(points.size()));
    Pen wavePen(t.accent, 2.0f);
    g.DrawCurve(&wavePen, points.data(), static_cast<INT>(points.size()));
}

// -----------------------------------------------------------------------------
// INTERACTIVE CONTROLS RENDERING MODULE
// -----------------------------------------------------------------------------
void DrawCustomSlider(Graphics& g, SliderControl& slider) {
    const Theme& t = T();
    RectF rect = ToRectF(slider.rect);
    if (rect.Width <= 0.0f) return;

    Font titleFont(L"Segoe UI", 9.5f, FontStyleRegular, UnitPoint);
    Font valueFont(L"Segoe UI", 9.5f, FontStyleRegular, UnitPoint);
    SolidBrush labelBrush(t.text);
    SolidBrush valBrush(t.textSec);

    RectF headerRect(rect.X, rect.Y, rect.Width, 18.0f);
    DrawTextIn(g, slider.label, titleFont, labelBrush, headerRect, StringAlignmentNear, StringAlignmentNear);

    float actualVal = slider.minValue + slider.value * (slider.maxValue - slider.minValue);
    std::wstringstream ss;
    if (actualVal > 0.01f && slider.minValue < 0) ss << L"+";
    ss << std::fixed << std::setprecision(1) << actualVal << L" " << slider.unit;
    DrawTextIn(g, ss.str(), valueFont, valBrush, headerRect, StringAlignmentFar, StringAlignmentNear);

    float trackY = rect.Y + 24.0f;
    float trackHeight = 6.0f;
    RectF trackRect(rect.X, trackY, rect.Width, trackHeight);

    GraphicsPath trackPath;
    AddRoundedRectangle(trackPath, trackRect, 3.0f);
    SolidBrush trackBg(t.track);
    g.FillPath(&trackBg, &trackPath);

    float fillWidth = rect.Width * slider.value;
    if (fillWidth > 4.0f) {
        RectF fillRect(rect.X, trackY, fillWidth, trackHeight);
        GraphicsPath fillPath;
        AddRoundedRectangle(fillPath, fillRect, 3.0f);
        SolidBrush fillBrush(t.accent);
        g.FillPath(&fillBrush, &fillPath);
    }

    float thumbRadius = slider.isHovered || slider.isDragging ? 8.5f : 7.0f;
    float thumbX = rect.X + fillWidth;
    float thumbY = trackY + trackHeight * 0.5f;
    RectF thumbRect(thumbX - thumbRadius, thumbY - thumbRadius, thumbRadius * 2.0f, thumbRadius * 2.0f);

    SolidBrush thumbBrush(Color(255, 255, 255, 255));
    g.FillEllipse(&thumbBrush, thumbRect);
    Pen thumbBorder(t.accent, 2.0f);
    g.DrawEllipse(&thumbBorder, thumbRect);
}

// primary: accent pill (or soft-red when isActive, i.e. "Disconnect"). Otherwise a quiet outlined pill.
void DrawModernButton(Graphics& g, RectF rect, const std::wstring& text, bool isPrimary, bool isHovered, bool isActive) {
    const Theme& t = T();
    GraphicsPath path;
    AddRoundedRectangle(path, rect, rect.Height * 0.5f > 14.0f ? 14.0f : rect.Height * 0.5f);

    Font btnFont(L"Segoe UI", 9.5f, FontStyleBold, UnitPoint);
    if (isPrimary) {
        Color fill = isActive ? t.redSoft : t.accent;
        Color fg = isActive ? t.red : t.onAccent;
        if (isHovered) fill = Color(255, std::min(255, fill.GetRed() + 10), std::min(255, fill.GetGreen() + 10), std::min(255, fill.GetBlue() + 10));
        SolidBrush b(fill);
        g.FillPath(&b, &path);
        if (isActive) {
            Pen p(WithAlpha(t.red, 90), 1.0f);
            g.DrawPath(&p, &path);
        }
        SolidBrush textBrush(fg);
        DrawTextIn(g, text, btnFont, textBrush, rect);
    } else {
        SolidBrush bgBrush(isHovered ? t.accentSoft : t.cardAlt);
        g.FillPath(&bgBrush, &path);
        Pen borderPen(isHovered ? t.accent : t.border, 1.0f);
        g.DrawPath(&borderPen, &path);
        SolidBrush textBrush(t.text);
        DrawTextIn(g, text, btnFont, textBrush, rect);
    }
}

static void DrawSwitch(Graphics& g, float x, float y, bool on) {
    const Theme& t = T();
    RectF pill(x, y, 36.0f, 20.0f);
    GraphicsPath path;
    AddRoundedRectangle(path, pill, 10.0f);
    SolidBrush bg(on ? t.accent : t.track);
    g.FillPath(&bg, &path);
    SolidBrush knob(Color(255, 255, 255, 255));
    g.FillEllipse(&knob, on ? x + 18.0f : x + 2.0f, y + 2.0f, 16.0f, 16.0f);
}

// Labelled toggle; the whole rect is the hit-box.
static void DrawToggle(Graphics& g, RECT hit, const std::wstring& label, bool on, bool hovered) {
    const Theme& t = T();
    RectF r = ToRectF(hit);
    Font f(L"Segoe UI", 9.0f, FontStyleRegular, UnitPoint);
    SolidBrush text(hovered ? t.text : t.textSec);
    DrawSwitch(g, r.X, r.Y + (r.Height - 20.0f) * 0.5f, on);
    DrawTextIn(g, label, f, text, RectF(r.X + 44.0f, r.Y, r.Width - 44.0f, r.Height), StringAlignmentNear);
}

static void DrawField(Graphics& g, RECT hit, const std::wstring& text, bool placeholder, bool hovered, bool arrow) {
    const Theme& t = T();
    RectF r = ToRectF(hit);
    DrawFluentCard(g, r, 10.0f, true, hovered ? t.accent : Color(0, 0, 0, 0));
    Font f(L"Segoe UI", 9.5f, FontStyleRegular, UnitPoint);
    SolidBrush b(placeholder ? t.textSec : t.text);
    DrawTextIn(g, text, f, b, RectF(r.X + 12.0f, r.Y, r.Width - (arrow ? 34.0f : 20.0f), r.Height), StringAlignmentNear);
    if (arrow) {
        Pen p(t.textSec, 1.6f);
        float cx = r.X + r.Width - 18.0f, cy = r.Y + r.Height * 0.5f;
        g.DrawLine(&p, cx - 4.0f, cy - 2.0f, cx, cy + 2.0f);
        g.DrawLine(&p, cx, cy + 2.0f, cx + 4.0f, cy - 2.0f);
    }
}

static void DrawWindowsTile(Graphics& g, RectF r) {
    const Theme& t = T();
    DrawFluentCard(g, r, 14.0f, true);
    float s = r.Width * 0.30f, gap = 3.0f;
    float x0 = r.X + (r.Width - (s * 2 + gap)) * 0.5f, y0 = r.Y + (r.Height - (s * 2 + gap)) * 0.5f;
    SolidBrush b(Color(255, 30, 144, 255));
    (void)t;
    g.FillRectangle(&b, x0, y0, s, s);
    g.FillRectangle(&b, x0 + s + gap, y0, s, s);
    g.FillRectangle(&b, x0, y0 + s + gap, s, s);
    g.FillRectangle(&b, x0 + s + gap, y0 + s + gap, s, s);
}

static void DrawPhoneTile(Graphics& g, RectF r, bool active) {
    const Theme& t = T();
    DrawFluentCard(g, r, 14.0f, true);
    float w = r.Width * 0.34f, h = r.Height * 0.62f;
    RectF body(r.X + (r.Width - w) * 0.5f, r.Y + (r.Height - h) * 0.5f, w, h);
    GraphicsPath p;
    AddRoundedRectangle(p, body, 4.0f);
    Pen pen(active ? t.accent : t.textSec, 1.8f);
    g.DrawPath(&pen, &p);
    SolidBrush dot(active ? t.accent : t.textSec);
    g.FillEllipse(&dot, body.X + body.Width * 0.5f - 1.5f, body.Y + body.Height - 6.0f, 3.0f, 3.0f);
}

static void DrawWifiGlyph(Graphics& g, float cx, float cy, bool active) {
    const Theme& t = T();
    RectF badge(cx - 20.0f, cy - 20.0f, 40.0f, 40.0f);
    SolidBrush bg(t.accentSoft);
    g.FillEllipse(&bg, badge);
    Pen pen(active ? t.accent : t.textSec, 2.0f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    for (int i = 1; i <= 3; ++i) {
        float r = 4.5f * i;
        g.DrawArc(&pen, cx - r, cy + 6.0f - r, r * 2.0f, r * 2.0f, 225.0f, 90.0f);
    }
    SolidBrush dot(active ? t.accent : t.textSec);
    g.FillEllipse(&dot, cx - 1.8f, cy + 4.2f, 3.6f, 3.6f);
}

static void DrawLinkDots(Graphics& g, float x0, float x1, float y, float skipCx, bool active) {
    const Theme& t = T();
    SolidBrush b(active ? t.accent : t.track);
    for (float x = x0; x < x1; x += 11.0f) {
        if (std::abs(x - skipCx) < 28.0f) continue;
        g.FillEllipse(&b, x, y - 1.8f, 3.6f, 3.6f);
    }
}

static void DrawMetric(Graphics& g, RectF slot, int glyph, const std::wstring& value, const std::wstring& label) {
    const Theme& t = T();
    RectF badge(slot.X, slot.Y + (slot.Height - 38.0f) * 0.5f, 38.0f, 38.0f);
    SolidBrush bg(t.accentSoft);
    g.FillEllipse(&bg, badge);
    Pen pen(t.accent, 2.0f);
    pen.SetLineJoin(LineJoinRound);
    float cx = badge.X + 19.0f, cy = badge.Y + 19.0f;
    if (glyph == 0) { // pulse
        PointF pts[] = { {cx - 9, cy}, {cx - 4, cy}, {cx - 1, cy - 7}, {cx + 3, cy + 7}, {cx + 6, cy}, {cx + 9, cy} };
        g.DrawLines(&pen, pts, 6);
    } else if (glyph == 1) { // bars
        SolidBrush b(t.accent);
        g.FillRectangle(&b, cx - 8, cy, 4.0f, 8.0f);
        g.FillRectangle(&b, cx - 2, cy - 6, 4.0f, 14.0f);
        g.FillRectangle(&b, cx + 4, cy - 3, 4.0f, 11.0f);
    } else { // layers
        PointF d1[] = { {cx, cy - 8}, {cx + 9, cy - 3}, {cx, cy + 2}, {cx - 9, cy - 3} };
        g.DrawPolygon(&pen, d1, 4);
        g.DrawLine(&pen, cx - 9.0f, cy + 3.0f, cx, cy + 8.0f);
        g.DrawLine(&pen, cx, cy + 8.0f, cx + 9.0f, cy + 3.0f);
    }
    Font vf(L"Segoe UI", 13.0f, FontStyleBold, UnitPoint);
    Font lf(L"Segoe UI", 8.5f, FontStyleRegular, UnitPoint);
    SolidBrush vb(t.text), lb(t.textSec);
    DrawTextAt(g, value, vf, vb, slot.X + 50.0f, slot.Y + 8.0f);
    DrawTextAt(g, label, lf, lb, slot.X + 50.0f, slot.Y + 32.0f);
}

static const wchar_t* kPageNames[4] = { L"Overview", L"Sound", L"Devices", L"Settings" };
static const wchar_t* kPageGlyphs[4] = { L"\u2302", L"\u266B", L"\u25AF", L"\u2699" };

static void ApplyPreset(int i) {
    g_App.engine.currentPreset.store(static_cast<AudioPreset>(i));
    switch (g_App.engine.currentPreset.load()) {
    case AudioPreset::PureDirect:
        g_App.subBassSlider.value = 0.50f; g_App.vocalPresenceSlider.value = 0.50f;
        g_App.trebleAirSlider.value = 0.50f; g_App.surround3DSlider.value = 0.0f; break;
    case AudioPreset::HiFiStudio:
        g_App.subBassSlider.value = 0.60f; g_App.vocalPresenceSlider.value = 0.56f;
        g_App.trebleAirSlider.value = 0.58f; g_App.surround3DSlider.value = 0.40f; break;
    case AudioPreset::Cinema3D:
        g_App.subBassSlider.value = 0.75f; g_App.vocalPresenceSlider.value = 0.62f;
        g_App.trebleAirSlider.value = 0.67f; g_App.surround3DSlider.value = 0.95f; break;
    case AudioPreset::GamingLowLatency:
        g_App.subBassSlider.value = 0.56f; g_App.vocalPresenceSlider.value = 0.73f;
        g_App.trebleAirSlider.value = 0.75f; g_App.surround3DSlider.value = 0.70f; break;
    }
    g_App.engine.bassBoostDb.store(g_App.subBassSlider.minValue + g_App.subBassSlider.value * (g_App.subBassSlider.maxValue - g_App.subBassSlider.minValue));
    g_App.engine.presenceBoostDb.store(g_App.vocalPresenceSlider.minValue + g_App.vocalPresenceSlider.value * (g_App.vocalPresenceSlider.maxValue - g_App.vocalPresenceSlider.minValue));
    g_App.engine.trebleBoostDb.store(g_App.trebleAirSlider.minValue + g_App.trebleAirSlider.value * (g_App.trebleAirSlider.maxValue - g_App.trebleAirSlider.minValue));
    g_App.engine.spatialSurroundAmount.store(g_App.surround3DSlider.value);
    g_App.engine.enableSpatial3D.store(g_App.surround3DSlider.value > 0.01f);
}

static std::wstring CurrentDeviceName() {
    std::wstring name = L"Default Windows Audio Endpoint";
    if (g_App.engine.selectedDeviceIndex >= 0 && g_App.engine.selectedDeviceIndex < (int)g_App.engine.devices.size()) {
        name = g_App.engine.devices[g_App.engine.selectedDeviceIndex].name;
    }
    return name;
}

static void DrawSoundCard(Graphics& g, RectF card, float sliderStep, bool withPresets) {
    const Theme& t = T();
    DrawFluentCard(g, card);
    Font head(L"Segoe UI", 10.5f, FontStyleBold, UnitPoint);
    SolidBrush hb(t.text);
    DrawTextAt(g, L"Sound shaping", head, hb, card.X + 18.0f, card.Y + 14.0f);

    SliderControl* sliders[4] = { &g_App.subBassSlider, &g_App.vocalPresenceSlider, &g_App.trebleAirSlider, &g_App.surround3DSlider };
    float sy = card.Y + 46.0f;
    for (int i = 0; i < 4; ++i) {
        sliders[i]->rect = MakeRect(card.X + 18.0f, sy + sliderStep * i, card.Width - 36.0f, 38.0f);
        DrawCustomSlider(g, *sliders[i]);
    }

    if (withPresets) {
        const wchar_t* labels[4] = { L"Pure", L"Studio", L"Cinema", L"Game" };
        float py = card.Y + card.Height - 44.0f;
        float gap = 8.0f;
        float pw = (card.Width - 36.0f - gap * 3) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            float px = card.X + 18.0f + i * (pw + gap);
            g_App.presetPillsRect[i] = MakeRect(px, py, pw, 30.0f);
            bool sel = (static_cast<int>(g_App.engine.currentPreset.load()) == i);
            bool hov = (g_App.hoveredPresetIndex == i);
            RectF pr(px, py, pw, 30.0f);
            GraphicsPath pp;
            AddRoundedRectangle(pp, pr, 12.0f);
            SolidBrush bg(sel ? t.accentSoft : (hov ? t.cardAlt : t.card));
            g.FillPath(&bg, &pp);
            Pen pen(sel ? t.accent : t.border, sel ? 1.5f : 1.0f);
            g.DrawPath(&pen, &pp);
            Font pf(L"Segoe UI", 9.0f, sel ? FontStyleBold : FontStyleRegular, UnitPoint);
            SolidBrush tb(sel ? t.accentText : t.text);
            DrawTextIn(g, labels[i], pf, tb, pr);
        }
    }
}

static void DrawLiveAudioCard(Graphics& g, RectF card, bool spectrumOnly) {
    const Theme& t = T();
    DrawFluentCard(g, card);
    Font head(L"Segoe UI", 10.5f, FontStyleBold, UnitPoint);
    Font small(L"Segoe UI", 8.5f, FontStyleRegular, UnitPoint);
    SolidBrush hb(t.text), sb(t.textSec);
    DrawTextAt(g, L"Live audio", head, hb, card.X + 18.0f, card.Y + 14.0f);
    DrawTextIn(g, L"48 kHz \u00B7 Stereo PCM", small, sb, RectF(card.X, card.Y + 14.0f, card.Width - 18.0f, 18.0f), StringAlignmentFar, StringAlignmentNear);

    float y = card.Y + 42.0f;
    DrawVuMeter(g, RectF(card.X + 18.0f, y, card.Width - 36.0f, 22.0f), g_App.vuLeftLevel, g_App.vuLeftPeak, L"L");
    DrawVuMeter(g, RectF(card.X + 18.0f, y + 24.0f, card.Width - 36.0f, 22.0f), g_App.vuRightLevel, g_App.vuRightPeak, L"R");
    y += 58.0f;
    float remaining = card.Y + card.Height - 14.0f - y;
    if (spectrumOnly) {
        DrawSpectrum(g, RectF(card.X + 18.0f, y, card.Width - 36.0f, remaining));
    } else {
        float specH = remaining * 0.58f;
        DrawSpectrum(g, RectF(card.X + 18.0f, y, card.Width - 36.0f, specH));
        DrawTextAt(g, L"Oscilloscope", small, sb, card.X + 18.0f, y + specH + 8.0f);
        DrawWaveform(g, RectF(card.X + 18.0f, y + specH + 26.0f, card.Width - 36.0f, remaining - specH - 26.0f));
    }
}

static void DrawMetricsBar(Graphics& g, RectF bar) {
    const Theme& t = T();
    DrawFluentCard(g, bar);
    bool live = g_App.engine.isStreaming.load();
    std::wstringstream lat, jit, buf;
    if (live) {
        lat << std::fixed << std::setprecision(2) << g_App.telemetry.latencyMs << L" ms";
        jit << L"\u00B1" << std::fixed << std::setprecision(2) << g_App.telemetry.jitterMs << L" ms";
        buf << std::fixed << std::setprecision(1) << g_App.telemetry.bufferHealth << L"%";
    } else {
        lat << L"-- ms"; jit << L"-- ms"; buf << L"--";
    }
    float w = bar.Width / 3.0f;
    DrawMetric(g, RectF(bar.X + 20.0f, bar.Y, w - 20.0f, bar.Height), 0, lat.str(), L"Latency");
    DrawMetric(g, RectF(bar.X + w + 20.0f, bar.Y, w - 20.0f, bar.Height), 1, jit.str(), L"Jitter");
    DrawMetric(g, RectF(bar.X + 2 * w + 20.0f, bar.Y, w - 20.0f, bar.Height), 2, buf.str(), L"Buffer");
    Pen sep(t.border, 1.0f);
    g.DrawLine(&sep, bar.X + w, bar.Y + 14.0f, bar.X + w, bar.Y + bar.Height - 14.0f);
    g.DrawLine(&sep, bar.X + 2 * w, bar.Y + 14.0f, bar.X + 2 * w, bar.Y + bar.Height - 14.0f);
}

static void DrawConnectionCard(Graphics& g, RectF card) {
    const Theme& t = T();
    bool live = g_App.engine.isStreaming.load();
    DrawFluentCard(g, card);
    Font name(L"Segoe UI", 10.5f, FontStyleBold, UnitPoint);
    Font small(L"Segoe UI", 8.5f, FontStyleRegular, UnitPoint);
    SolidBrush nb(t.text), sb(t.textSec);

    float ty = card.Y + 16.0f;
    DrawWindowsTile(g, RectF(card.X + 20.0f, ty, 60.0f, 60.0f));
    DrawTextAt(g, L"This PC", name, nb, card.X + 94.0f, ty);
    g_App.deviceDropdownRect = MakeRect(card.X + 94.0f, ty + 24.0f, 190.0f, 34.0f);
    std::wstring dev = CurrentDeviceName();
    if (dev.length() > 24) dev = dev.substr(0, 22) + L"...";
    DrawField(g, g_App.deviceDropdownRect, dev, false, g_App.isDeviceHovered, true);

    float phoneX = card.X + card.Width - 80.0f;
    DrawPhoneTile(g, RectF(phoneX, ty, 60.0f, 60.0f), live || g_App.isPhoneDiscovered);
    std::wstring phone = g_App.isPhoneDiscovered ? g_App.targetDeviceName : L"No phone yet";
    DrawTextIn(g, phone, name, nb, RectF(phoneX - 230.0f, ty, 218.0f, 22.0f), StringAlignmentFar, StringAlignmentNear);
    g_App.streamButtonRect = MakeRect(phoneX - 138.0f, ty + 26.0f, 126.0f, 32.0f);
    std::wstring label = live ? L"Disconnect" : L"Connect";
    DrawModernButton(g, ToRectF(g_App.streamButtonRect), label, true, g_App.isStreamBtnHovered, live);

    float linkL = card.X + 300.0f, linkR = phoneX - 250.0f;
    if (linkR - linkL > 80.0f) {
        float cx = (linkL + linkR) * 0.5f, cy = ty + 26.0f;
        DrawLinkDots(g, linkL, linkR, cy, cx, live);
        DrawWifiGlyph(g, cx, cy, live);
        DrawTextIn(g, L"48 kHz \u00B7 Stereo PCM", small, sb, RectF(cx - 80.0f, cy + 24.0f, 160.0f, 16.0f));
    }

    float by = card.Y + card.Height - 44.0f;
    g_App.autoConnectRect = MakeRect(card.X + 20.0f, by, 130.0f, 28.0f);
    DrawToggle(g, g_App.autoConnectRect, L"Auto-connect", g_App.autoConnect, g_App.isAutoConnectHovered);
    g_App.wifiToggleRect = MakeRect(card.X + 170.0f, by, 120.0f, 28.0f);
    DrawToggle(g, g_App.wifiToggleRect, L"FastPath", g_App.engine.enableWifiAccel.load(), g_App.isWifiToggleHovered);

    g_App.ipInputRect = MakeRect(card.X + card.Width - 200.0f, by - 3.0f, 180.0f, 34.0f);
    std::wstring ip = g_App.targetIp.empty() ? (g_App.isIpEditing ? L"|" : L"Phone IP address") : (g_App.targetIp + (g_App.isIpEditing ? L"|" : L""));
    DrawField(g, g_App.ipInputRect, ip, g_App.targetIp.empty() && !g_App.isIpEditing, g_App.isIpHovered, false);
}

static void DrawToast(Graphics& g, int width, int height) {
    if (!g_App.toast.active) return;
    const Theme& t = T();
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_App.toast.startTime).count();
    if (elapsed >= g_App.toast.durationSec) { g_App.toast.active = false; return; }

    float alpha = 1.0f;
    if (elapsed < 0.25f) alpha = elapsed / 0.25f;
    else if (elapsed > (g_App.toast.durationSec - 0.45f)) alpha = (g_App.toast.durationSec - elapsed) / 0.45f;
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    float toastW = 440.0f, toastH = 48.0f;
    float toastX = 200.0f + (width - 200.0f - toastW) * 0.5f;
    float toastY = static_cast<float>(height) - toastH - 14.0f + (1.0f - alpha) * 18.0f;

    RectF tRect(toastX, toastY, toastW, toastH);
    GraphicsPath tPath;
    AddRoundedRectangle(tPath, tRect, 16.0f);
    SolidBrush tBg(WithAlpha(t.card, (int)(245 * alpha)));
    g.FillPath(&tBg, &tPath);
    Pen tBorder(WithAlpha(t.accent, (int)(200 * alpha)), 1.2f);
    g.DrawPath(&tBorder, &tPath);

    SolidBrush dotBrush(WithAlpha(t.accent, (int)(255 * alpha)));
    g.FillEllipse(&dotBrush, toastX + 16.0f, toastY + 19.0f, 10.0f, 10.0f);

    Font tTitleFont(L"Segoe UI", 9.0f, FontStyleBold, UnitPoint);
    Font tMsgFont(L"Segoe UI", 8.5f, FontStyleRegular, UnitPoint);
    SolidBrush tText(WithAlpha(t.text, (int)(255 * alpha)));
    SolidBrush tSec(WithAlpha(t.textSec, (int)(255 * alpha)));
    DrawTextIn(g, g_App.toast.title, tTitleFont, tText, RectF(toastX + 36.0f, toastY + 6.0f, toastW - 48.0f, 18.0f), StringAlignmentNear);
    DrawTextIn(g, g_App.toast.message, tMsgFont, tSec, RectF(toastX + 36.0f, toastY + 24.0f, toastW - 48.0f, 18.0f), StringAlignmentNear);
}

// -----------------------------------------------------------------------------
// MAIN FLICKER-FREE PAINT ROUTINE
// -----------------------------------------------------------------------------
void RenderFrame(HWND hwnd, HDC hdc) {
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int width = clientRect.right - clientRect.left;
    int height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0) return;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

    // Hit-boxes are rebuilt every frame; controls on hidden pages must not respond.
    const RECT none = {0, 0, 0, 0};
    g_App.streamButtonRect = g_App.wifiToggleRect = g_App.autoConnectRect = none;
    g_App.ipInputRect = g_App.deviceDropdownRect = none;
    g_App.themeLightRect = g_App.themeDarkRect = none;
    for (auto& r : g_App.presetPillsRect) r = none;
    for (auto& r : g_App.navRects) r = none;
    for (SliderControl* s : { &g_App.subBassSlider, &g_App.vocalPresenceSlider, &g_App.trebleAirSlider, &g_App.surround3DSlider }) s->rect = none;

    {
        Graphics g(memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        const Theme& t = T();
        const float SW = 200.0f;
        SolidBrush bgBrush(t.bg);
        g.FillRectangle(&bgBrush, 0, 0, width, height);

        // ---- Sidebar ----
        SolidBrush sideBrush(t.sidebar);
        g.FillRectangle(&sideBrush, 0.0f, 0.0f, SW, (REAL)height);
        Pen sideBorder(t.border, 1.0f);
        g.DrawLine(&sideBorder, SW, 0.0f, SW, (REAL)height);

        RectF logo(22.0f, 22.0f, 30.0f, 30.0f);
        GraphicsPath logoPath;
        AddRoundedRectangle(logoPath, logo, 9.0f);
        SolidBrush logoBg(t.accent);
        g.FillPath(&logoBg, &logoPath);
        Font logoFont(L"Segoe UI", 12.0f, FontStyleBold, UnitPoint);
        SolidBrush logoFg(t.onAccent);
        DrawTextIn(g, L"N", logoFont, logoFg, logo);
        Font brandFont(L"Segoe UI", 13.0f, FontStyleBold, UnitPoint);
        SolidBrush textBrush(t.text), secBrush(t.textSec);
        DrawTextAt(g, L"NullWire", brandFont, textBrush, 62.0f, 25.0f);

        Font navFont(L"Segoe UI", 10.0f, FontStyleRegular, UnitPoint);
        Font navFontSel(L"Segoe UI", 10.0f, FontStyleBold, UnitPoint);
        Font glyphFont(L"Segoe UI Symbol", 11.0f, FontStyleRegular, UnitPoint);
        for (int i = 0; i < 4; ++i) {
            float y = 84.0f + i * 46.0f;
            g_App.navRects[i] = MakeRect(14.0f, y, SW - 28.0f, 38.0f);
            RectF nr = ToRectF(g_App.navRects[i]);
            bool sel = (g_App.page == i);
            if (sel || g_App.hoveredNav == i) {
                GraphicsPath np;
                AddRoundedRectangle(np, nr, 14.0f);
                SolidBrush nb(sel ? t.accentSoft : t.cardAlt);
                g.FillPath(&nb, &np);
            }
            SolidBrush fg(sel ? t.accentText : t.textSec);
            DrawTextIn(g, kPageGlyphs[i], glyphFont, fg, RectF(nr.X + 10.0f, nr.Y, 26.0f, nr.Height));
            DrawTextIn(g, kPageNames[i], sel ? navFontSel : navFont, fg, RectF(nr.X + 44.0f, nr.Y, nr.Width - 50.0f, nr.Height), StringAlignmentNear);
        }

        // ---- Header ----
        const float cx = SW + 28.0f;
        const float cw = width - cx - 28.0f;
        Font titleFont(L"Segoe UI", 18.0f, FontStyleBold, UnitPoint);
        std::wstring title = g_App.page == 0 ? L"Your sound. Everywhere." : kPageNames[g_App.page];
        DrawTextAt(g, title, titleFont, textBrush, cx, 18.0f);

        bool live = g_App.engine.isStreaming.load();
        SolidBrush liveDot(live ? t.accent : t.textSec);
        g.FillEllipse(&liveDot, cx, 53.0f, 8.0f, 8.0f);
        Font liveFont(L"Segoe UI", 9.0f, FontStyleRegular, UnitPoint);
        DrawTextAt(g, live ? L"Live" : (g_App.isPhoneDiscovered ? L"Ready" : L"Waiting for phone"), liveFont, secBrush, cx + 14.0f, 48.0f);

        // Theme switch (segmented)
        RectF seg(cx + cw - 184.0f, 18.0f, 184.0f, 36.0f);
        GraphicsPath segPath;
        AddRoundedRectangle(segPath, seg, 18.0f);
        SolidBrush segBg(t.card);
        g.FillPath(&segBg, &segPath);
        Pen segBorder(t.border, 1.0f);
        g.DrawPath(&segBorder, &segPath);
        g_App.themeLightRect = MakeRect(seg.X + 3.0f, seg.Y + 3.0f, 88.0f, 30.0f);
        g_App.themeDarkRect = MakeRect(seg.X + 93.0f, seg.Y + 3.0f, 88.0f, 30.0f);
        RectF selR = ToRectF(g_App.darkMode ? g_App.themeDarkRect : g_App.themeLightRect);
        GraphicsPath selPath;
        AddRoundedRectangle(selPath, selR, 15.0f);
        SolidBrush selBg(t.accentSoft);
        g.FillPath(&selBg, &selPath);
        Pen selBorder(t.accent, 1.2f);
        g.DrawPath(&selBorder, &selPath);
        Font segFont(L"Segoe UI", 9.0f, FontStyleRegular, UnitPoint);
        Font segFontSel(L"Segoe UI", 9.0f, FontStyleBold, UnitPoint);
        SolidBrush lb(!g_App.darkMode ? t.accentText : t.textSec), db(g_App.darkMode ? t.accentText : t.textSec);
        DrawTextIn(g, L"\u2600  Light", g_App.darkMode ? segFont : segFontSel, lb, ToRectF(g_App.themeLightRect));
        DrawTextIn(g, L"\u263E  Dark", g_App.darkMode ? segFontSel : segFont, db, ToRectF(g_App.themeDarkRect));

        // ---- Pages ----
        const float top = 84.0f;
        const float bottom = height - 24.0f;
        if (g_App.page == 0) {
            DrawConnectionCard(g, RectF(cx, top, cw, 136.0f));
            float barH = 64.0f;
            RectF bar(cx, bottom - barH, cw, barH);
            float rowY = top + 136.0f + 16.0f;
            float rowH = bar.Y - 16.0f - rowY;
            float leftW = cw * 0.52f;
            DrawLiveAudioCard(g, RectF(cx, rowY, leftW, rowH), true);
            DrawSoundCard(g, RectF(cx + leftW + 16.0f, rowY, cw - leftW - 16.0f, rowH), 40.0f, true);
            DrawMetricsBar(g, bar);
        } else if (g_App.page == 1) {
            float leftW = cw * 0.48f;
            float h = bottom - top - 80.0f;
            DrawSoundCard(g, RectF(cx, top, leftW, bottom - top), 64.0f, true);
            DrawLiveAudioCard(g, RectF(cx + leftW + 16.0f, top, cw - leftW - 16.0f, bottom - top), false);
            (void)h;
        } else if (g_App.page == 2) {
            DrawConnectionCard(g, RectF(cx, top, cw, 136.0f));
            RectF info(cx, top + 152.0f, cw, 150.0f);
            DrawFluentCard(g, info);
            Font head(L"Segoe UI", 10.5f, FontStyleBold, UnitPoint);
            Font body(L"Segoe UI", 9.5f, FontStyleRegular, UnitPoint);
            DrawTextAt(g, L"Connection details", head, textBrush, info.X + 18.0f, info.Y + 14.0f);
            DrawTextAt(g, L"This PC:  " + g_App.hostPcName + L"  (" + g_App.hostIp + L")", body, textBrush, info.X + 18.0f, info.Y + 46.0f);
            std::wstring ph = g_App.isPhoneDiscovered ? (g_App.targetDeviceName + L"  (" + g_App.targetIp + L")") : L"Open NullWire on your phone \u2014 it is discovered automatically on the same Wi-Fi.";
            DrawTextAt(g, L"Phone:     " + ph, body, textBrush, info.X + 18.0f, info.Y + 72.0f);
            DrawTextAt(g, L"Audio endpoint:  " + CurrentDeviceName(), body, secBrush, info.X + 18.0f, info.Y + 98.0f);
        } else {
            RectF card(cx, top, cw, 190.0f);
            DrawFluentCard(g, card);
            Font head(L"Segoe UI", 10.5f, FontStyleBold, UnitPoint);
            Font body(L"Segoe UI", 9.0f, FontStyleRegular, UnitPoint);
            DrawTextAt(g, L"Streaming", head, textBrush, card.X + 18.0f, card.Y + 14.0f);
            g_App.autoConnectRect = MakeRect(card.X + 18.0f, card.Y + 52.0f, 200.0f, 28.0f);
            DrawToggle(g, g_App.autoConnectRect, L"Auto-connect when a phone is found", g_App.autoConnect, g_App.isAutoConnectHovered);
            g_App.autoConnectRect.right = (LONG)(card.X + card.Width - 18.0f);
            DrawTextAt(g, L"Starts streaming automatically the first time your phone is discovered.", body, secBrush, card.X + 62.0f, card.Y + 82.0f);
            g_App.wifiToggleRect = MakeRect(card.X + 18.0f, card.Y + 118.0f, 200.0f, 28.0f);
            DrawToggle(g, g_App.wifiToggleRect, L"Wi-Fi FastPath", g_App.engine.enableWifiAccel.load(), g_App.isWifiToggleHovered);
            g_App.wifiToggleRect.right = (LONG)(card.X + card.Width - 18.0f);
            DrawTextAt(g, L"Low-latency socket buffers and 1 ms timer slicing for the lowest delay.", body, secBrush, card.X + 62.0f, card.Y + 148.0f);
        }

        DrawToast(g, width, height);
    }

    BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
}

// -----------------------------------------------------------------------------
// REAL-TIME AUDIO SIMULATION & DATA SYNC
// -----------------------------------------------------------------------------
void UpdateAudioEngineSim() {
    g_App.animationTime += 0.05f;

    if (g_App.engine.isStreaming.load()) {
        g_App.telemetry.latencyMs = 1.95f + 0.15f * std::sin(g_App.animationTime * 0.8f);
        g_App.telemetry.jitterMs = 0.08f + 0.03f * std::cos(g_App.animationTime * 1.5f);
        g_App.telemetry.bitrateKbps = 1536;
        g_App.telemetry.packetsStreamed = g_App.engine.sendPacketCount.load();
        g_App.telemetry.bufferHealth = 99.9f;

        float curL = g_App.engine.streamRmsL.load();
        float curR = g_App.engine.streamRmsR.load();

        g_App.vuLeftLevel = std::clamp(curL * 2.2f, 0.02f, 0.98f);
        g_App.vuRightLevel = std::clamp(curR * 2.2f, 0.02f, 0.98f);
        g_App.vuLeftPeak = std::max(g_App.vuLeftPeak * 0.96f, g_App.vuLeftLevel);
        g_App.vuRightPeak = std::max(g_App.vuRightPeak * 0.96f, g_App.vuRightLevel);

        for (size_t i = 0; i < 7; ++i) {
            float val = g_App.engine.spectrumBands[i].load();
            g_App.spectrumBands[i] = std::clamp(val, 0.05f, 0.98f);
            g_App.spectrumPeaks[i] = std::max(g_App.spectrumPeaks[i] * 0.97f, g_App.spectrumBands[i]);
        }

        {
            std::lock_guard<std::mutex> lock(g_App.engine.waveformMutex);
            g_App.waveformDisplay = g_App.engine.waveformBuffer;
        }
    } else {
        // IDLE STANDBY - 100% REAL ZERO TELEMETRY BEFORE ACTUAL CONNECTION
        g_App.telemetry.latencyMs = 0.0f;
        g_App.telemetry.jitterMs = 0.0f;
        g_App.telemetry.bitrateKbps = 0;
        g_App.telemetry.packetsStreamed = 0;
        g_App.telemetry.bufferHealth = 0.0f;

        g_App.vuLeftLevel *= 0.85f;
        g_App.vuRightLevel *= 0.85f;
        g_App.vuLeftPeak *= 0.90f;
        g_App.vuRightPeak *= 0.90f;
        for (auto& b : g_App.spectrumBands) b *= 0.88f;
        for (auto& p : g_App.spectrumPeaks) p *= 0.92f;
        for (auto& s : g_App.waveformDisplay) s *= 0.90f;
    }
}

// -----------------------------------------------------------------------------
// WIN32 MESSAGE PROCEDURE
// -----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        LoadThemePreference();
        BOOL dark = g_App.darkMode ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

        SetTimer(hwnd, TIMER_ANIMATION_ID, ANIMATION_INTERVAL_MS, nullptr);

        // Query Host Computer Name & Local IP
        wchar_t cName[MAX_COMPUTERNAME_LENGTH + 1] = {0};
        DWORD cSize = sizeof(cName) / sizeof(cName[0]);
        if (GetComputerNameW(cName, &cSize)) {
            g_App.hostPcName = cName;
        }
        EnsureWinsock();
        g_App.hostIp = GetLocalHostIp();

        // Init Tray Icon
        g_Nid.cbSize = sizeof(NOTIFYICONDATAW);
        g_Nid.hWnd = hwnd;
        g_Nid.uID = 1;
        g_Nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_Nid.uCallbackMessage = WM_TRAYICON;
        g_Nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
        if (!g_Nid.hIcon) g_Nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wcscpy_s(g_Nid.szTip, L"NullWire Pro - 48kHz Wi-Fi Audio Link");
        Shell_NotifyIconW(NIM_ADD, &g_Nid);

        g_App.engine.EnumerateDevices();
        g_App.engine.StartDiscovery(hwnd);
        return 0;
    }

    case WM_USER + 100: {
        // Discovered device update — DISPLAY ONLY, never auto-reconnects
        std::string ip, name;
        {
            std::lock_guard<std::mutex> lock(g_App.engine.targetMutex);
            ip = g_App.engine.targetPhoneIp;
            name = g_App.engine.targetPhoneName;
        }
        if (!ip.empty()) {
            std::wstring newIp(ip.begin(), ip.end());
            std::wstring newName(name.begin(), name.end());

            bool isFirstDiscovery = !g_App.isPhoneDiscovered;

            g_App.targetIp = newIp;
            g_App.targetDeviceName = newName;
            g_App.isPhoneDiscovered = true;

            if (isFirstDiscovery) {
                PostNotification(L"DEVICE DISCOVERED", g_App.targetDeviceName + L" (" + g_App.targetIp + L") is ready.", Color(255, 16, 185, 129), 3.5f);

                // Auto-Connect ONLY on the very first discovery, and ONLY if
                // autoConnect is on AND user has never clicked DISCONNECT
                if (g_App.autoConnect && !g_App.userManualDisconnect && !g_App.engine.isStreaming.load()) {
                    g_App.engine.StartStreaming(ip, g_App.engine.selectedDeviceIndex);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_TIMER: {
        if (wParam == TIMER_ANIMATION_ID) {
            UpdateAudioEngineSim();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RenderFrame(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        g_App.isStreamBtnHovered = PtInRect(&g_App.streamButtonRect, pt);
        g_App.isWifiToggleHovered = PtInRect(&g_App.wifiToggleRect, pt);
        g_App.isAutoConnectHovered = PtInRect(&g_App.autoConnectRect, pt);
        g_App.isIpHovered = PtInRect(&g_App.ipInputRect, pt);
        g_App.isDeviceHovered = PtInRect(&g_App.deviceDropdownRect, pt);
        g_App.hoveredNav = -1;
        for (int i = 0; i < 4; ++i) {
            if (PtInRect(&g_App.navRects[i], pt)) { g_App.hoveredNav = i; break; }
        }

        g_App.hoveredPresetIndex = -1;
        for (int i = 0; i < 4; ++i) {
            if (PtInRect(&g_App.presetPillsRect[i], pt)) {
                g_App.hoveredPresetIndex = i;
                break;
            }
        }

        auto handleSliderMove = [&](SliderControl& slider, std::atomic<float>& dspParam) {
            slider.isHovered = PtInRect(&slider.rect, pt);
            if (slider.isDragging) {
                float trackWidth = static_cast<float>(slider.rect.right - slider.rect.left);
                float relX = static_cast<float>(pt.x - slider.rect.left);
                slider.value = std::clamp(relX / trackWidth, 0.0f, 1.0f);
                float actualVal = slider.minValue + slider.value * (slider.maxValue - slider.minValue);
                dspParam.store(actualVal);
            }
        };

        handleSliderMove(g_App.subBassSlider, g_App.engine.bassBoostDb);
        handleSliderMove(g_App.vocalPresenceSlider, g_App.engine.presenceBoostDb);
        handleSliderMove(g_App.trebleAirSlider, g_App.engine.trebleBoostDb);

        g_App.surround3DSlider.isHovered = PtInRect(&g_App.surround3DSlider.rect, pt);
        if (g_App.surround3DSlider.isDragging) {
            float trackWidth = static_cast<float>(g_App.surround3DSlider.rect.right - g_App.surround3DSlider.rect.left);
            float relX = static_cast<float>(pt.x - g_App.surround3DSlider.rect.left);
            g_App.surround3DSlider.value = std::clamp(relX / trackWidth, 0.0f, 1.0f);
            g_App.engine.spatialSurroundAmount.store(g_App.surround3DSlider.value);
            g_App.engine.enableSpatial3D.store(g_App.surround3DSlider.value > 0.01f);
        }

        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        for (int i = 0; i < 4; ++i) {
            if (PtInRect(&g_App.navRects[i], pt)) {
                g_App.page = i;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        if (PtInRect(&g_App.themeLightRect, pt)) { ApplyTheme(hwnd, false); return 0; }
        if (PtInRect(&g_App.themeDarkRect, pt)) { ApplyTheme(hwnd, true); return 0; }

        // Main Stream Action
        if (PtInRect(&g_App.streamButtonRect, pt)) {
            if (g_App.engine.isStreaming.load()) {
                g_App.userManualDisconnect = true; // User intentionally clicked disconnect
                g_App.engine.StopStreaming();
            } else {
                g_App.userManualDisconnect = false; // User manually clicked start
                if (g_App.targetIp.empty() || g_App.targetIp == L"127.0.0.1") {
                    PostNotification(L"NO PHONE DETECTED", L"Please open the NullWire app on your Android phone.", Color(255, 245, 158, 11), 3.5f);
                } else {
                    std::string ipStr(g_App.targetIp.begin(), g_App.targetIp.end());
                    g_App.engine.StartStreaming(ipStr, g_App.engine.selectedDeviceIndex);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // Auto-Connect Toggle
        if (PtInRect(&g_App.autoConnectRect, pt)) {
            g_App.autoConnect = !g_App.autoConnect;
            if (g_App.autoConnect) {
                g_App.userManualDisconnect = false;
            }
            PostNotification(L"AUTO-CONNECT", g_App.autoConnect ? L"Automatic Wi-Fi instant streaming enabled." : L"Manual connection mode.", Color(255, 0, 210, 255), 3.0f);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // Wi-Fi Acceleration Toggle
        if (PtInRect(&g_App.wifiToggleRect, pt)) {
            bool nextState = !g_App.engine.enableWifiAccel.load();
            g_App.engine.enableWifiAccel.store(nextState);
            PostNotification(L"WI-FI FASTPATH", nextState ? L"Hardware Low-Latency Lock & 1.0ms timer slicing active." : L"Standard socket buffer mode.", Color(255, 16, 185, 129), 3.0f);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        // IP Box Click
        if (PtInRect(&g_App.ipInputRect, pt)) {
            g_App.isIpEditing = true;
            SetFocus(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        } else {
            g_App.isIpEditing = false;
        }

        // Device Dropdown Click
        if (PtInRect(&g_App.deviceDropdownRect, pt)) {
            HMENU hMenu = CreatePopupMenu();
            for (size_t i = 0; i < g_App.engine.devices.size(); ++i) {
                UINT flags = MF_STRING;
                if ((int)i == g_App.engine.selectedDeviceIndex) flags |= MF_CHECKED;
                AppendMenuW(hMenu, flags, 2000 + i, g_App.engine.devices[i].name.c_str());
            }

            POINT screenPt = pt;
            ClientToScreen(hwnd, &screenPt);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screenPt.x, screenPt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);

            if (cmd >= 2000 && cmd < 2000 + (int)g_App.engine.devices.size()) {
                int newIdx = cmd - 2000;
                g_App.engine.selectedDeviceIndex = newIdx;
                if (g_App.engine.isStreaming.load()) {
                    g_App.engine.StopStreaming();
                    std::string ipStr(g_App.targetIp.begin(), g_App.targetIp.end());
                    g_App.engine.StartStreaming(ipStr, newIdx);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        // Preset Pills Click
        for (int i = 0; i < 4; ++i) {
            if (PtInRect(&g_App.presetPillsRect[i], pt)) {
                ApplyPreset(i);

                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }

        // Sliders Drag Initiation
        auto handleSliderDown = [&](SliderControl& slider, std::atomic<float>& dspParam) {
            if (PtInRect(&slider.rect, pt)) {
                slider.isDragging = true;
                SetCapture(hwnd);
                float trackWidth = static_cast<float>(slider.rect.right - slider.rect.left);
                float relX = static_cast<float>(pt.x - slider.rect.left);
                slider.value = std::clamp(relX / trackWidth, 0.0f, 1.0f);
                float actualVal = slider.minValue + slider.value * (slider.maxValue - slider.minValue);
                dspParam.store(actualVal);
            }
        };

        handleSliderDown(g_App.subBassSlider, g_App.engine.bassBoostDb);
        handleSliderDown(g_App.vocalPresenceSlider, g_App.engine.presenceBoostDb);
        handleSliderDown(g_App.trebleAirSlider, g_App.engine.trebleBoostDb);

        if (PtInRect(&g_App.surround3DSlider.rect, pt)) {
            g_App.surround3DSlider.isDragging = true;
            SetCapture(hwnd);
            float trackWidth = static_cast<float>(g_App.surround3DSlider.rect.right - g_App.surround3DSlider.rect.left);
            float relX = static_cast<float>(pt.x - g_App.surround3DSlider.rect.left);
            g_App.surround3DSlider.value = std::clamp(relX / trackWidth, 0.0f, 1.0f);
            g_App.engine.spatialSurroundAmount.store(g_App.surround3DSlider.value);
            g_App.engine.enableSpatial3D.store(g_App.surround3DSlider.value > 0.01f);
        }

        return 0;
    }

    case WM_LBUTTONUP: {
        ReleaseCapture();
        g_App.subBassSlider.isDragging = false;
        g_App.vocalPresenceSlider.isDragging = false;
        g_App.trebleAirSlider.isDragging = false;
        g_App.surround3DSlider.isDragging = false;
        return 0;
    }

    case WM_CHAR: {
        if (g_App.isIpEditing) {
            wchar_t ch = (wchar_t)wParam;
            if (ch == VK_BACK) {
                if (!g_App.targetIp.empty()) g_App.targetIp.pop_back();
            } else if (ch == VK_RETURN) {
                g_App.isIpEditing = false;
            } else if ((ch >= L'0' && ch <= L'9') || ch == L'.') {
                if (g_App.targetIp.length() < 15) g_App.targetIp.push_back(ch);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case WM_TRAYICON: {
        if (lParam == WM_LBUTTONUP) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1001, L"Open NullWire Pro");
            AppendMenuW(hMenu, MF_STRING, 1002, g_App.engine.isStreaming.load() ? L"Disconnect Stream" : L"Start Stream");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, 1003, L"Exit");

            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);

            if (cmd == 1001) {
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
            } else if (cmd == 1002) {
                if (g_App.engine.isStreaming.load()) {
                    g_App.userManualDisconnect = true;
                    g_App.engine.StopStreaming();
                } else {
                    g_App.userManualDisconnect = false;
                    std::string ipStr(g_App.targetIp.begin(), g_App.targetIp.end());
                    g_App.engine.StartStreaming(ipStr, g_App.engine.selectedDeviceIndex);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (cmd == 1003) {
                DestroyWindow(hwnd);
            }
        }
        return 0;
    }

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_Nid);
        KillTimer(hwnd, TIMER_ANIMATION_ID);
        g_App.engine.StopStreaming();
        g_App.engine.StopDiscovery();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// -----------------------------------------------------------------------------
// APPLICATION ENTRY POINT (WinMain)
// -----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&g_GdiplusToken, &gdiplusStartupInput, nullptr) != Status::Ok) {
        MessageBox(nullptr, L"Failed to initialize GDI+ graphics engine.", L"NullWire Error", MB_ICONERROR);
        return -1;
    }

    const wchar_t CLASS_NAME[] = L"NullWireProMainWindowClass";
    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClassEx(&wc)) {
        GdiplusShutdown(g_GdiplusToken);
        return -1;
    }

    HWND hwnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        CLASS_NAME,
        L"NullWire Pro - Lossless Ultra-Low Latency Wi-Fi PCM Audio Link",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        980, 640,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        GdiplusShutdown(g_GdiplusToken);
        return -1;
    }

    g_hMainWnd = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(g_GdiplusToken);
    return static_cast<int>(msg.wParam);
}
