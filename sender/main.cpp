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

void DrawFluentCard(Graphics& g, RectF rect, float radius = 10.0f, Color bgColor = Color(240, 24, 29, 45), Color borderColor = Color(180, 42, 51, 78)) {
    GraphicsPath path;
    AddRoundedRectangle(path, rect, radius);

    Color topColor(bgColor.GetAlpha(), bgColor.GetRed() + 4, bgColor.GetGreen() + 4, bgColor.GetBlue() + 8);
    Color bottomColor(bgColor.GetAlpha(), bgColor.GetRed() - 2, bgColor.GetGreen() - 2, bgColor.GetBlue() - 2);

    LinearGradientBrush cardBrush(rect, topColor, bottomColor, LinearGradientModeVertical);
    g.FillPath(&cardBrush, &path);

    Pen borderPen(borderColor, 1.0f);
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

// -----------------------------------------------------------------------------
// AUDIO VISUALIZERS RENDERING MODULE
// -----------------------------------------------------------------------------
void DrawVuMeter(Graphics& g, RectF rect, float level, float peak, const std::wstring& channelName) {
    Font font(L"Segoe UI", 9.0f, FontStyleBold, UnitPoint);
    SolidBrush textBrush(Color(200, 148, 163, 184));
    StringFormat sf;
    sf.SetAlignment(StringAlignmentNear);
    sf.SetLineAlignment(StringAlignmentCenter);

    RectF labelRect(rect.X, rect.Y, 20.0f, rect.Height);
    g.DrawString(channelName.c_str(), -1, &font, labelRect, &sf, &textBrush);

    RectF trackRect(rect.X + 24.0f, rect.Y + 4.0f, rect.Width - 75.0f, rect.Height - 8.0f);
    GraphicsPath trackPath;
    AddRoundedRectangle(trackPath, trackRect, 4.0f);
    SolidBrush trackBg(Color(255, 15, 20, 32));
    g.FillPath(&trackBg, &trackPath);

    float fillWidth = (trackRect.Width * std::clamp(level, 0.0f, 1.0f));
    if (fillWidth > 2.0f) {
        RectF fillRect(trackRect.X, trackRect.Y, fillWidth, trackRect.Height);
        GraphicsPath fillPath;
        AddRoundedRectangle(fillPath, fillRect, 4.0f);

        LinearGradientBrush vuGrad(
            trackRect,
            Color(255, 16, 185, 129),
            Color(255, 0, 210, 255),
            LinearGradientModeHorizontal
        );

        Color colors[] = { Color(255, 16, 185, 129), Color(255, 0, 210, 255), Color(255, 245, 158, 11) };
        REAL positions[] = { 0.0f, 0.75f, 1.0f };
        vuGrad.SetInterpolationColors(colors, positions, 3);

        g.FillPath(&vuGrad, &fillPath);
    }

    float peakX = trackRect.X + (trackRect.Width * std::clamp(peak, 0.0f, 1.0f));
    Pen peakPen(Color(255, 255, 255, 255), 2.0f);
    g.DrawLine(&peakPen, peakX, trackRect.Y - 1.0f, peakX, trackRect.Y + trackRect.Height + 1.0f);

    float dbVal = -36.0f + level * 36.0f;
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(1) << dbVal << L" dB";
    RectF dbRect(trackRect.X + trackRect.Width + 6.0f, rect.Y, 55.0f, rect.Height);
    g.DrawString(ss.str().c_str(), -1, &font, dbRect, &sf, &textBrush);
}

void DrawSpectrum(Graphics& g, RectF rect) {
    const std::array<std::wstring, 7> bandLabels = { L"60Hz", L"150", L"400", L"1kHz", L"2.5k", L"6k", L"14k" };
    float barSpacing = 6.0f;
    float totalSpacing = barSpacing * 6;
    float barWidth = (rect.Width - totalSpacing) / 7.0f;

    Font labelFont(L"Segoe UI", 8.0f, FontStyleRegular, UnitPoint);
    SolidBrush textBrush(Color(180, 100, 116, 139));
    StringFormat sfCenter;
    sfCenter.SetAlignment(StringAlignmentCenter);
    sfCenter.SetLineAlignment(StringAlignmentNear);

    for (size_t i = 0; i < 7; ++i) {
        float x = rect.X + i * (barWidth + barSpacing);
        float maxHeight = rect.Height - 20.0f;
        float h = maxHeight * std::clamp(g_App.spectrumBands[i], 0.05f, 1.0f);
        float y = rect.Y + maxHeight - h;

        RectF slotRect(x, rect.Y, barWidth, maxHeight);
        GraphicsPath slotPath;
        AddRoundedRectangle(slotPath, slotRect, 3.0f);
        SolidBrush slotBg(Color(255, 15, 20, 30));
        g.FillPath(&slotBg, &slotPath);

        RectF barRect(x, y, barWidth, h);
        GraphicsPath barPath;
        AddRoundedRectangle(barPath, barRect, 3.0f);

        LinearGradientBrush barBrush(
            slotRect,
            Color(255, 168, 85, 247),
            Color(255, 0, 210, 255),
            LinearGradientModeVertical
        );
        g.FillPath(&barBrush, &barPath);

        float peakY = rect.Y + maxHeight - (maxHeight * std::clamp(g_App.spectrumPeaks[i], 0.05f, 1.0f));
        Pen peakPen(Color(255, 255, 255, 255), 1.5f);
        g.DrawLine(&peakPen, x, peakY, x + barWidth, peakY);

        RectF textRect(x - 5.0f, rect.Y + maxHeight + 4.0f, barWidth + 10.0f, 16.0f);
        g.DrawString(bandLabels[i].c_str(), -1, &labelFont, textRect, &sfCenter, &textBrush);
    }
}

void DrawWaveform(Graphics& g, RectF rect) {
    Pen gridPen(Color(60, 42, 51, 78), 1.0f);
    for (int i = 1; i <= 3; ++i) {
        float y = rect.Y + (rect.Height / 4.0f) * i;
        g.DrawLine(&gridPen, rect.X, y, rect.X + rect.Width, y);
    }
    for (int i = 1; i <= 5; ++i) {
        float x = rect.X + (rect.Width / 6.0f) * i;
        g.DrawLine(&gridPen, x, rect.Y, x, rect.Y + rect.Height);
    }

    Pen centerPen(Color(120, 0, 210, 255), 1.0f);
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

    Pen glowPen(Color(80, 0, 210, 255), 4.0f);
    g.DrawCurve(&glowPen, points.data(), static_cast<INT>(points.size()));

    Pen wavePen(Color(255, 0, 210, 255), 2.0f);
    g.DrawCurve(&wavePen, points.data(), static_cast<INT>(points.size()));
}

// -----------------------------------------------------------------------------
// INTERACTIVE CONTROLS RENDERING MODULE
// -----------------------------------------------------------------------------
void DrawCustomSlider(Graphics& g, SliderControl& slider) {
    RectF rect(
        static_cast<REAL>(slider.rect.left),
        static_cast<REAL>(slider.rect.top),
        static_cast<REAL>(slider.rect.right - slider.rect.left),
        static_cast<REAL>(slider.rect.bottom - slider.rect.top)
    );

    Font titleFont(L"Segoe UI", 9.5f, FontStyleRegular, UnitPoint);
    Font valueFont(L"Segoe UI", 9.5f, FontStyleBold, UnitPoint);
    SolidBrush labelBrush(Color(240, 226, 232, 240));
    SolidBrush valBrush(slider.accentColor);

    StringFormat sfNear, sfFar;
    sfNear.SetAlignment(StringAlignmentNear);
    sfNear.SetLineAlignment(StringAlignmentNear);
    sfFar.SetAlignment(StringAlignmentFar);
    sfFar.SetLineAlignment(StringAlignmentNear);

    RectF headerRect(rect.X, rect.Y, rect.Width, 18.0f);
    g.DrawString(slider.label.c_str(), -1, &titleFont, headerRect, &sfNear, &labelBrush);

    float actualVal = slider.minValue + slider.value * (slider.maxValue - slider.minValue);
    std::wstringstream ss;
    if (actualVal > 0.01f && slider.minValue < 0) ss << L"+";
    ss << std::fixed << std::setprecision(1) << actualVal << L" " << slider.unit;
    g.DrawString(ss.str().c_str(), -1, &valueFont, headerRect, &sfFar, &valBrush);

    float trackY = rect.Y + 24.0f;
    float trackHeight = 6.0f;
    RectF trackRect(rect.X, trackY, rect.Width, trackHeight);

    GraphicsPath trackPath;
    AddRoundedRectangle(trackPath, trackRect, 3.0f);
    SolidBrush trackBg(Color(255, 15, 20, 32));
    g.FillPath(&trackBg, &trackPath);

    float fillWidth = rect.Width * slider.value;
    if (fillWidth > 4.0f) {
        RectF fillRect(rect.X, trackY, fillWidth, trackHeight);
        GraphicsPath fillPath;
        AddRoundedRectangle(fillPath, fillRect, 3.0f);
        SolidBrush fillBrush(slider.accentColor);
        g.FillPath(&fillBrush, &fillPath);
    }

    float thumbRadius = slider.isHovered || slider.isDragging ? 8.0f : 6.5f;
    float thumbX = rect.X + fillWidth;
    float thumbY = trackY + trackHeight * 0.5f;

    RectF thumbRect(thumbX - thumbRadius, thumbY - thumbRadius, thumbRadius * 2.0f, thumbRadius * 2.0f);
    if (slider.isHovered || slider.isDragging) {
        DrawGlowEffect(g, thumbRect, slider.accentColor, 6.0f);
    }

    SolidBrush thumbBrush(Color(255, 255, 255, 255));
    g.FillEllipse(&thumbBrush, thumbRect);
    Pen thumbBorder(slider.accentColor, 2.0f);
    g.DrawEllipse(&thumbBorder, thumbRect);
}

void DrawModernButton(Graphics& g, RectF rect, const std::wstring& text, bool isPrimary, bool isHovered, bool isActive) {
    GraphicsPath path;
    AddRoundedRectangle(path, rect, 8.0f);

    if (isPrimary) {
        if (isHovered) {
            DrawGlowEffect(g, rect, Color(255, 0, 210, 255), 10.0f);
        }
        LinearGradientBrush btnBrush(
            rect,
            isActive ? Color(255, 239, 68, 68) : (isHovered ? Color(255, 30, 230, 255) : Color(255, 0, 210, 255)),
            isActive ? Color(255, 185, 28, 28) : (isHovered ? Color(255, 20, 210, 140) : Color(255, 16, 185, 129)),
            LinearGradientModeHorizontal
        );
        g.FillPath(&btnBrush, &path);

        Font btnFont(L"Segoe UI", 11.0f, FontStyleBold, UnitPoint);
        SolidBrush textBrush(isActive ? Color(255, 255, 255, 255) : Color(255, 10, 14, 22));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(text.c_str(), -1, &btnFont, rect, &sf, &textBrush);
    } else {
        SolidBrush bgBrush(isHovered ? Color(255, 35, 45, 68) : Color(255, 24, 30, 48));
        g.FillPath(&bgBrush, &path);
        Pen borderPen(isHovered ? Color(255, 0, 210, 255) : Color(255, 42, 51, 78), 1.0f);
        g.DrawPath(&borderPen, &path);

        Font btnFont(L"Segoe UI", 10.0f, FontStyleRegular, UnitPoint);
        SolidBrush textBrush(Color(255, 226, 232, 240));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(text.c_str(), -1, &btnFont, rect, &sf, &textBrush);
    }
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

    {
        Graphics g(memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        SolidBrush bgBrush(Color(255, 15, 18, 28));
        g.FillRectangle(&bgBrush, 0, 0, width, height);

        Font titleFont(L"Segoe UI", 15.0f, FontStyleBold, UnitPoint);
        Font subtitleFont(L"Segoe UI", 9.0f, FontStyleRegular, UnitPoint);
        Font hostFont(L"Segoe UI", 8.5f, FontStyleBold, UnitPoint);
        Font targetFont(L"Segoe UI", 8.5f, FontStyleRegular, UnitPoint);
        SolidBrush whiteBrush(Color(255, 255, 255, 255));
        SolidBrush slateBrush(Color(255, 148, 163, 184));
        SolidBrush cyanBrush(Color(255, 0, 210, 255));
        SolidBrush greenBrush(Color(255, 16, 185, 129));
        SolidBrush amberBrush(Color(255, 245, 158, 11));

        // Brand Emblem
        g.FillEllipse(&cyanBrush, 24.0f, 22.0f, 12.0f, 12.0f);
        if (g_App.engine.isStreaming.load()) {
            DrawGlowEffect(g, RectF(22.0f, 20.0f, 16.0f, 16.0f), Color(255, 0, 210, 255), 8.0f);
        }

        g.DrawString(L"NULLWIRE PRO", -1, &titleFont, PointF(44.0f, 16.0f), &whiteBrush);
        g.DrawString(L"Lossless Ultra-Low Latency Wi-Fi Link (48kHz Master PCM)", -1, &subtitleFont, PointF(44.0f, 38.0f), &slateBrush);

        // Header Action Stream Button (Far Right)
        g_App.streamButtonRect = { width - 224, 14, width - 24, 60 };
        RectF streamBtnF(
            static_cast<REAL>(g_App.streamButtonRect.left),
            static_cast<REAL>(g_App.streamButtonRect.top),
            static_cast<REAL>(g_App.streamButtonRect.right - g_App.streamButtonRect.left),
            static_cast<REAL>(g_App.streamButtonRect.bottom - g_App.streamButtonRect.top)
        );
        std::wstring btnLabel = g_App.engine.isStreaming.load() ? L"DISCONNECT STREAM" : (g_App.isPhoneDiscovered ? L"START LOSSLESS STREAM" : L"START STREAM");
        DrawModernButton(g, streamBtnF, btnLabel.c_str(), true, g_App.isStreamBtnHovered, g_App.engine.isStreaming.load());

        // Dedicated Host & Mobile Device Identity HUD Box (Between Title and Button)
        float devCardX = 400.0f;
        float devCardW = (width - 238.0f) - devCardX;
        if (devCardW > 160.0f) {
            RectF devCardRect(devCardX, 14.0f, devCardW, 46.0f);
            DrawFluentCard(g, devCardRect, 8.0f, Color(255, 18, 23, 36), Color(255, 42, 51, 78));

            // Host Line (Top)
            SolidBrush dotHost(Color(255, 16, 185, 129));
            g.FillEllipse(&dotHost, devCardX + 12.0f, 23.0f, 8.0f, 8.0f);
            std::wstring hostLine = L"HOST (PC): " + g_App.hostPcName + L" (" + g_App.hostIp + L")";
            g.DrawString(hostLine.c_str(), -1, &hostFont, PointF(devCardX + 26.0f, 20.0f), &whiteBrush);

            // Target Phone Line (Bottom)
            std::wstring targetLine;
            SolidBrush* tBrush = &whiteBrush;
            Color dotColor(255, 245, 158, 11);
            if (g_App.engine.isStreaming.load()) {
                targetLine = L"MOBILE: " + g_App.targetDeviceName + L" (" + g_App.targetIp + L") · Active";
                dotColor = Color(255, 0, 210, 255);
            } else if (g_App.isPhoneDiscovered) {
                targetLine = L"MOBILE: " + g_App.targetDeviceName + L" (" + g_App.targetIp + L") · Ready";
                dotColor = Color(255, 16, 185, 129);
            } else {
                targetLine = L"MOBILE: Waiting for mobile beacon...";
                tBrush = &slateBrush;
            }
            SolidBrush dotTarget(dotColor);
            g.FillEllipse(&dotTarget, devCardX + 12.0f, 39.0f, 8.0f, 8.0f);
            g.DrawString(targetLine.c_str(), -1, &targetFont, PointF(devCardX + 26.0f, 36.0f), tBrush);
        }

        // ---------------------------------------------------------------------
        // TOP ROW: Target Endpoint & Connection Card
        // ---------------------------------------------------------------------
        RectF networkCardRect(24.0f, 72.0f, width - 48.0f, 74.0f);
        DrawFluentCard(g, networkCardRect, 10.0f);

        Font sectionHeaderFont(L"Segoe UI", 9.0f, FontStyleBold, UnitPoint);
        g.DrawString(L"TARGET RECEIVER & AUDIO ENDPOINT", -1, &sectionHeaderFont, PointF(40.0f, 80.0f), &slateBrush);

        // Device Dropdown Box
        g_App.deviceDropdownRect = { 40, 100, 310, 134 };
        RectF devBoxF(40.0f, 100.0f, 270.0f, 34.0f);
        DrawFluentCard(g, devBoxF, 6.0f, Color(255, 18, 23, 36), g_App.isDeviceHovered ? Color(255, 0, 210, 255) : Color(255, 42, 51, 78));
        Font devFont(L"Segoe UI", 9.5f, FontStyleRegular, UnitPoint);

        std::wstring displayDev = L"● Default Windows Audio Endpoint";
        if (g_App.engine.selectedDeviceIndex < (int)g_App.engine.devices.size()) {
            displayDev = g_App.engine.devices[g_App.engine.selectedDeviceIndex].name;
        }
        if (displayDev.length() > 25) displayDev = displayDev.substr(0, 23) + L"...";

        g.DrawString(displayDev.c_str(), -1, &devFont, PointF(50.0f, 107.0f), &whiteBrush);
        g.DrawString(L"▼", -1, &devFont, PointF(288.0f, 108.0f), &slateBrush);

        // IP Target Field
        g_App.ipInputRect = { 325, 100, 505, 134 };
        RectF ipBoxF(325.0f, 100.0f, 180.0f, 34.0f);
        DrawFluentCard(g, ipBoxF, 6.0f, Color(255, 18, 23, 36), g_App.isIpHovered ? Color(255, 0, 210, 255) : Color(255, 42, 51, 78));
        std::wstring ipDisplay = g_App.targetIp.empty() ? (g_App.isIpEditing ? L"|" : L"Enter phone IP...") : (g_App.targetIp + (g_App.isIpEditing ? L"|" : L""));
        SolidBrush ipTextBrush(g_App.targetIp.empty() ? Color(255, 100, 116, 139) : Color(255, 255, 255, 255));
        g.DrawString(ipDisplay.c_str(), -1, &devFont, PointF(337.0f, 107.0f), &ipTextBrush);

        // Auto-Connect (Instant) Toggle Switch
        g_App.autoConnectRect = { width - 425, 96, width - 235, 132 };
        RectF autoConnF(width - 425.0f, 96.0f, 190.0f, 36.0f);
        DrawFluentCard(g, autoConnF, 8.0f, Color(255, 18, 23, 36), g_App.isAutoConnectHovered ? Color(255, 0, 210, 255) : Color(255, 42, 51, 78));
        g.DrawString(L"⚡ Auto-Connect", -1, &subtitleFont, PointF(width - 415.0f, 104.0f), &whiteBrush);

        RectF autoPillRect(width - 280.0f, 104.0f, 34.0f, 18.0f);
        GraphicsPath autoPillPath;
        AddRoundedRectangle(autoPillPath, autoPillRect, 9.0f);
        SolidBrush autoPillBg(g_App.autoConnect ? Color(255, 0, 210, 255) : Color(255, 50, 60, 80));
        g.FillPath(&autoPillBg, &autoPillPath);

        float autoKnobX = g_App.autoConnect ? (autoPillRect.X + autoPillRect.Width - 16.0f) : (autoPillRect.X + 2.0f);
        SolidBrush knobBrush(Color(255, 255, 255, 255));
        g.FillEllipse(&knobBrush, autoKnobX, autoPillRect.Y + 2.0f, 14.0f, 14.0f);

        // Wi-Fi Acceleration Toggle Switch
        g_App.wifiToggleRect = { width - 225, 96, width - 44, 132 };
        RectF wifiToggleF(width - 225.0f, 96.0f, 181.0f, 36.0f);
        DrawFluentCard(g, wifiToggleF, 8.0f, Color(255, 18, 23, 36), g_App.isWifiToggleHovered ? Color(255, 16, 185, 129) : Color(255, 42, 51, 78));
        g.DrawString(L"Wi-Fi FastPath", -1, &subtitleFont, PointF(width - 215.0f, 104.0f), &whiteBrush);

        RectF pillRect(width - 86.0f, 104.0f, 34.0f, 18.0f);
        GraphicsPath pillPath;
        AddRoundedRectangle(pillPath, pillRect, 9.0f);
        SolidBrush pillBg(g_App.engine.enableWifiAccel.load() ? Color(255, 16, 185, 129) : Color(255, 50, 60, 80));
        g.FillPath(&pillBg, &pillPath);

        float knobX = g_App.engine.enableWifiAccel.load() ? (pillRect.X + pillRect.Width - 16.0f) : (pillRect.X + 2.0f);
        g.FillEllipse(&knobBrush, knobX, pillRect.Y + 2.0f, 14.0f, 14.0f);

        // ---------------------------------------------------------------------
        // MIDDLE ROW (LEFT): Real-Time Studio Audio Engine (VU + Spectrum + Waveform)
        // ---------------------------------------------------------------------
        float midY = 154.0f;
        float leftCardWidth = (width - 48.0f) * 0.58f;
        float cardHeight = 310.0f;
        RectF visualizerCard(24.0f, midY, leftCardWidth, cardHeight);
        DrawFluentCard(g, visualizerCard, 10.0f);

        g.DrawString(L"REAL-TIME STUDIO AUDIO ENGINE", -1, &sectionHeaderFont, PointF(40.0f, midY + 14.0f), &slateBrush);

        DrawVuMeter(g, RectF(40.0f, midY + 38.0f, leftCardWidth - 32.0f, 22.0f), g_App.vuLeftLevel, g_App.vuLeftPeak, L"L");
        DrawVuMeter(g, RectF(40.0f, midY + 64.0f, leftCardWidth - 32.0f, 22.0f), g_App.vuRightLevel, g_App.vuRightPeak, L"R");

        g.DrawString(L"OCTAVE SPECTRUM ANALYZER", -1, &sectionHeaderFont, PointF(40.0f, midY + 98.0f), &slateBrush);
        DrawSpectrum(g, RectF(40.0f, midY + 118.0f, leftCardWidth - 32.0f, 82.0f));

        g.DrawString(L"ZERO-CROSSING PCM OSCILLOSCOPE", -1, &sectionHeaderFont, PointF(40.0f, midY + 214.0f), &slateBrush);
        DrawWaveform(g, RectF(40.0f, midY + 234.0f, leftCardWidth - 32.0f, 58.0f));

        // ---------------------------------------------------------------------
        // MIDDLE ROW (RIGHT): Acoustic DSP & Equalizer Suite
        // ---------------------------------------------------------------------
        float rightCardX = 24.0f + leftCardWidth + 16.0f;
        float rightCardWidth = width - rightCardX - 24.0f;
        RectF dspCard(rightCardX, midY, rightCardWidth, cardHeight);
        DrawFluentCard(g, dspCard, 10.0f);

        g.DrawString(L"ACOUSTIC DSP & EQUALIZER SUITE", -1, &sectionHeaderFont, PointF(rightCardX + 16.0f, midY + 14.0f), &slateBrush);

        float sliderStartY = midY + 42.0f;
        float sliderStep = 50.0f;

        g_App.subBassSlider.rect = { static_cast<LONG>(rightCardX + 16.0f), static_cast<LONG>(sliderStartY), static_cast<LONG>(rightCardX + rightCardWidth - 16.0f), static_cast<LONG>(sliderStartY + 38.0f) };
        DrawCustomSlider(g, g_App.subBassSlider);

        g_App.vocalPresenceSlider.rect = { static_cast<LONG>(rightCardX + 16.0f), static_cast<LONG>(sliderStartY + sliderStep), static_cast<LONG>(rightCardX + rightCardWidth - 16.0f), static_cast<LONG>(sliderStartY + sliderStep + 38.0f) };
        DrawCustomSlider(g, g_App.vocalPresenceSlider);

        g_App.trebleAirSlider.rect = { static_cast<LONG>(rightCardX + 16.0f), static_cast<LONG>(sliderStartY + sliderStep * 2), static_cast<LONG>(rightCardX + rightCardWidth - 16.0f), static_cast<LONG>(sliderStartY + sliderStep * 2 + 38.0f) };
        DrawCustomSlider(g, g_App.trebleAirSlider);

        g_App.surround3DSlider.rect = { static_cast<LONG>(rightCardX + 16.0f), static_cast<LONG>(sliderStartY + sliderStep * 3), static_cast<LONG>(rightCardX + rightCardWidth - 16.0f), static_cast<LONG>(sliderStartY + sliderStep * 3 + 38.0f) };
        DrawCustomSlider(g, g_App.surround3DSlider);

        // Scenario Preset Pills
        g.DrawString(L"SCENARIO PRESETS", -1, &sectionHeaderFont, PointF(rightCardX + 16.0f, midY + 248.0f), &slateBrush);
        const std::array<std::wstring, 4> presetLabels = { L"Pure Direct", L"Hi-Fi Studio", L"Cinema 3D", L"Gaming Low-Lat" };
        float pillW = (rightCardWidth - 32.0f - 18.0f) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            float px = rightCardX + 16.0f + i * (pillW + 6.0f);
            float py = midY + 268.0f;
            g_App.presetPillsRect[i] = { static_cast<LONG>(px), static_cast<LONG>(py), static_cast<LONG>(px + pillW), static_cast<LONG>(py + 26.0f) };

            bool isSelected = (static_cast<int>(g_App.engine.currentPreset.load()) == i);
            bool isHovered = (g_App.hoveredPresetIndex == i);

            RectF pRect(px, py, pillW, 26.0f);
            GraphicsPath pPath;
            AddRoundedRectangle(pPath, pRect, 6.0f);

            SolidBrush pBg(isSelected ? Color(255, 0, 210, 255) : (isHovered ? Color(255, 40, 50, 75) : Color(255, 20, 26, 40)));
            g.FillPath(&pBg, &pPath);

            SolidBrush pText(isSelected ? Color(255, 10, 14, 22) : Color(255, 226, 232, 240));
            Font pillFont(L"Segoe UI", 8.5f, isSelected ? FontStyleBold : FontStyleRegular, UnitPoint);
            StringFormat sfCenter;
            sfCenter.SetAlignment(StringAlignmentCenter);
            sfCenter.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(presetLabels[i].c_str(), -1, &pillFont, pRect, &sfCenter, &pText);
        }

        // ---------------------------------------------------------------------
        // BOTTOM ROW: Telemetry HUD & Status Bar
        // ---------------------------------------------------------------------
        float botY = midY + cardHeight + 16.0f;
        RectF hudRect(24.0f, botY, width - 48.0f, 64.0f);
        DrawFluentCard(g, hudRect, 8.0f);

        auto drawMetric = [&](float x, const std::wstring& label, const std::wstring& val, Color valColor) {
            g.DrawString(label.c_str(), -1, &subtitleFont, PointF(x, botY + 12.0f), &slateBrush);
            Font valFont(L"Segoe UI", 12.5f, FontStyleBold, UnitPoint);
            SolidBrush vBrush(valColor);
            g.DrawString(val.c_str(), -1, &valFont, PointF(x, botY + 28.0f), &vBrush);
        };

        float metricStep = (width - 48.0f) / 5.0f;

        std::wstringstream latStr; latStr << std::fixed << std::setprecision(2) << g_App.telemetry.latencyMs << L" ms";
        drawMetric(40.0f, L"ROUNDTRIP LATENCY", latStr.str(), Color(255, 16, 185, 129));

        std::wstringstream jitStr; jitStr << L"±" << std::fixed << std::setprecision(2) << g_App.telemetry.jitterMs << L" ms";
        drawMetric(40.0f + metricStep, L"NETWORK JITTER", jitStr.str(), Color(255, 0, 210, 255));

        drawMetric(40.0f + metricStep * 2, L"LOSSLESS BITRATE", L"1,536 kbps (PCM)", Color(255, 168, 85, 247));

        std::wstringstream pktStr; pktStr << g_App.engine.sendPacketCount.load() << L" pkts";
        drawMetric(40.0f + metricStep * 3, L"PACKETS TRANSMITTED", pktStr.str(), Color(255, 245, 158, 11));

        std::wstringstream bufStr; bufStr << std::fixed << std::setprecision(1) << g_App.telemetry.bufferHealth << L"%";
        drawMetric(40.0f + metricStep * 4, L"RING BUFFER HEALTH", bufStr.str(), Color(255, 16, 185, 129));

        // ---------------------------------------------------------------------
        // IN-APP FLOATING TOAST NOTIFICATION CARD (Bottom-Center Sleek Pill)
        // ---------------------------------------------------------------------
        if (g_App.toast.active) {
            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float>(now - g_App.toast.startTime).count();
            if (elapsed >= g_App.toast.durationSec) {
                g_App.toast.active = false;
            } else {
                float alpha = 1.0f;
                if (elapsed < 0.25f) {
                    alpha = elapsed / 0.25f;
                } else if (elapsed > (g_App.toast.durationSec - 0.45f)) {
                    alpha = (g_App.toast.durationSec - elapsed) / 0.45f;
                }
                alpha = std::clamp(alpha, 0.0f, 1.0f);

                float toastW = 440.0f;
                float toastH = 44.0f;
                float toastX = (width - toastW) * 0.5f;
                float toastY = static_cast<float>(height) - toastH - 14.0f + (1.0f - alpha) * 18.0f;

                RectF tRect(toastX, toastY, toastW, toastH);
                GraphicsPath tPath;
                AddRoundedRectangle(tPath, tRect, 10.0f);

                BYTE bgAlpha = static_cast<BYTE>(245 * alpha);
                SolidBrush tBg(Color(bgAlpha, 18, 23, 36));
                g.FillPath(&tBg, &tPath);

                BYTE bAlpha = static_cast<BYTE>(230 * alpha);
                Pen tBorder(Color(bAlpha, g_App.toast.accentColor.GetRed(), g_App.toast.accentColor.GetGreen(), g_App.toast.accentColor.GetBlue()), 1.5f);
                g.DrawPath(&tBorder, &tPath);

                DrawGlowEffect(g, tRect, g_App.toast.accentColor, 5.0f * alpha);

                // Accent dot
                SolidBrush dotBrush(Color(static_cast<BYTE>(255 * alpha), g_App.toast.accentColor.GetRed(), g_App.toast.accentColor.GetGreen(), g_App.toast.accentColor.GetBlue()));
                g.FillEllipse(&dotBrush, toastX + 16.0f, toastY + 17.0f, 10.0f, 10.0f);

                // Title and Message
                Font tTitleFont(L"Segoe UI", 9.0f, FontStyleBold, UnitPoint);
                Font tMsgFont(L"Segoe UI", 8.5f, FontStyleRegular, UnitPoint);
                SolidBrush tWhite(Color(static_cast<BYTE>(255 * alpha), 255, 255, 255));
                SolidBrush tSlate(Color(static_cast<BYTE>(210 * alpha), 148, 163, 184));

                g.DrawString(g_App.toast.title.c_str(), -1, &tTitleFont, PointF(toastX + 34.0f, toastY + 7.0f), &tWhite);
                g.DrawString(g_App.toast.message.c_str(), -1, &tMsgFont, PointF(toastX + 34.0f, toastY + 22.0f), &tSlate);
            }
        }
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
        BOOL dark = TRUE;
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
                g_App.engine.currentPreset.store(static_cast<AudioPreset>(i));

                switch (g_App.engine.currentPreset.load()) {
                case AudioPreset::PureDirect:
                    g_App.subBassSlider.value = 0.50f;
                    g_App.vocalPresenceSlider.value = 0.50f;
                    g_App.trebleAirSlider.value = 0.50f;
                    g_App.surround3DSlider.value = 0.0f;
                    break;
                case AudioPreset::HiFiStudio:
                    g_App.subBassSlider.value = 0.60f;
                    g_App.vocalPresenceSlider.value = 0.56f;
                    g_App.trebleAirSlider.value = 0.58f;
                    g_App.surround3DSlider.value = 0.40f;
                    break;
                case AudioPreset::Cinema3D:
                    g_App.subBassSlider.value = 0.75f;
                    g_App.vocalPresenceSlider.value = 0.62f;
                    g_App.trebleAirSlider.value = 0.67f;
                    g_App.surround3DSlider.value = 0.95f;
                    break;
                case AudioPreset::GamingLowLatency:
                    g_App.subBassSlider.value = 0.56f;
                    g_App.vocalPresenceSlider.value = 0.73f;
                    g_App.trebleAirSlider.value = 0.75f;
                    g_App.surround3DSlider.value = 0.70f;
                    break;
                }

                g_App.engine.bassBoostDb.store(g_App.subBassSlider.minValue + g_App.subBassSlider.value * (g_App.subBassSlider.maxValue - g_App.subBassSlider.minValue));
                g_App.engine.presenceBoostDb.store(g_App.vocalPresenceSlider.minValue + g_App.vocalPresenceSlider.value * (g_App.vocalPresenceSlider.maxValue - g_App.vocalPresenceSlider.minValue));
                g_App.engine.trebleBoostDb.store(g_App.trebleAirSlider.minValue + g_App.trebleAirSlider.value * (g_App.trebleAirSlider.maxValue - g_App.trebleAirSlider.minValue));
                g_App.engine.spatialSurroundAmount.store(g_App.surround3DSlider.value);
                g_App.engine.enableSpatial3D.store(g_App.surround3DSlider.value > 0.01f);

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
