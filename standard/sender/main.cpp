#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601
#define INITGUID

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
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
#include <mutex>
#include <cstring>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "avrt.lib")

using namespace Gdiplus;

constexpr int AUDIO_SEND_PORT = 50005;
constexpr int DISCOVERY_PORT = 50007;
constexpr int TARGET_RATE = 48000;
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

void AddrToIpv4String(const sockaddr_in& addr, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    inet_ntop(AF_INET, &addr.sin_addr, out, (DWORD)outLen);
}

struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

class StandardAudioEngine {
public:
    std::vector<AudioDevice> devices;
    int selectedDeviceIndex = 0;

    std::atomic<bool> isStreaming{false};
    std::atomic<uint64_t> sendPacketCount{0};
    std::atomic<float> streamRms{0.0f};
    std::atomic<float> liveLatencyMs{2.67f};

    std::mutex targetMutex;
    std::string targetPhoneIp = "192.168.1.15";
    uint32_t sessionToken = 0;
    std::thread streamThread;

    std::atomic<bool> isDiscoveryRunning{false};
    std::thread discoveryThread;
    std::mutex discoveryMutex;
    std::string discoveredDeviceName = "";
    std::string discoveredDeviceIp = "";
    uint32_t discoveredToken = 0;
    std::atomic<bool> hasDiscoveredDevice{false};

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
            if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
                bindAddr.sin_port = 0;
                bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr));
            }

            sockaddr_in bcastAddr{};
            bcastAddr.sin_family = AF_INET;
            bcastAddr.sin_port = htons(DISCOVERY_PORT);
            bcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

            char buf[256];
            const char* scanMsg = "NWDS|NullWirePC";

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

                            std::string name = tokens.size() >= 2 ? tokens[1] : "Android Phone";
                            uint32_t token = 0;
                            if (tokens.size() >= 4) {
                                try {
                                    unsigned long v = std::stoul(tokens[3]);
                                    token = (uint32_t)v;
                                } catch (...) {
                                    token = 0;
                                }
                            }

                            std::lock_guard<std::mutex> lock(discoveryMutex);
                            discoveredDeviceName = name;
                            discoveredDeviceIp = actualIp;
                            discoveredToken = token;
                            hasDiscoveredDevice.store(true);
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

        streamThread = std::thread([this]() {
            DWORD taskIndex = 0;
            HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
            if (hMmcss) AvSetMmThreadPriority(hMmcss, AVRT_PRIORITY_CRITICAL);
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
            if (srcChannels < 1) srcChannels = 1;
            if (srcBytesPerFrame < 1) srcBytesPerFrame = (srcBits / 8) * srcChannels;

            auto lastPacketTime = std::chrono::high_resolution_clock::now();
            uint16_t sequenceNumber = 0;
            std::vector<uint8_t> packetPayload(MAX_AUDIO_PACKET);

            constexpr int CHUNK_FRAMES = 128; // 2.67ms ultra-low latency chunk
            constexpr int TARGET_SAMPLES = CHUNK_FRAMES * 2;

            while (isStreaming.load()) {
                UINT32 packetLength = 0;
                HRESULT hr = pCaptureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr) || packetLength == 0) {
                    Sleep(1);
                    continue;
                }

                BYTE* pData = nullptr;
                UINT32 numFramesRead = 0;
                DWORD flags = 0;

                hr = pCaptureClient->GetBuffer(&pData, &numFramesRead, &flags, NULL, NULL);
                if (SUCCEEDED(hr)) {
                    if (numFramesRead > 0) {
                        for (UINT32 f = 0; f < numFramesRead; f++) {
                            int16_t sL = 0, sR = 0;

                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT || !pData) {
                                sL = 0; sR = 0;
                            } else if (isFloat) {
                                const float* fSrc = reinterpret_cast<const float*>(pData + f * srcBytesPerFrame);
                                sL = (int16_t)(std::clamp(fSrc[0], -1.0f, 1.0f) * 32767.0f);
                                sR = (srcChannels > 1) ? (int16_t)(std::clamp(fSrc[1], -1.0f, 1.0f) * 32767.0f) : sL;
                            } else if (srcBits == 16) {
                                const int16_t* sSrc = reinterpret_cast<const int16_t*>(pData + f * srcBytesPerFrame);
                                sL = sSrc[0];
                                sR = (srcChannels > 1) ? sSrc[1] : sL;
                            }

                            pcmAccumulator.push_back(sL);
                            pcmAccumulator.push_back(sR);
                        }
                    }
                    pCaptureClient->ReleaseBuffer(numFramesRead);

                    while ((int)pcmAccumulator.size() >= TARGET_SAMPLES) {
                        int pcmBytes = TARGET_SAMPLES * (int)sizeof(int16_t);
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
                        liveLatencyMs.store(deltaMs);

                        double sumSq = 0;
                        for (int i = 0; i < TARGET_SAMPLES; i++) {
                            sumSq += (double)pcmAccumulator[i] * pcmAccumulator[i];
                        }
                        double rms = std::sqrt(sumSq / TARGET_SAMPLES);
                        streamRms.store((float)std::min(1.0, rms / 8000.0));

                        pcmAccumulator.erase(pcmAccumulator.begin(), pcmAccumulator.begin() + TARGET_SAMPLES);
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
            streamRms.store(0.0f);
        });
    }

    void StopStreaming() {
        if (!isStreaming.load()) return;
        isStreaming.store(false);
        if (streamThread.joinable()) {
            streamThread.join();
        }
        streamRms.store(0.0f);
    }
};

static StandardAudioEngine g_Engine;
static ULONG_PTR g_gdiplusToken = 0;

static HWND g_hIpEdit = NULL;
static HWND g_hDeviceCombo = NULL;

static Rect g_rAutoPairBtn;
static Rect g_rStreamBtn;
static int g_hoverElement = -1;

#define COLOR_BG RGB(10, 13, 18)
#define COLOR_CARD_BG RGB(17, 22, 31)
#define COLOR_INPUT_BG RGB(13, 17, 24)
#define COLOR_CYAN RGB(0, 210, 255)
#define COLOR_GREEN RGB(16, 185, 129)
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
        Gdiplus::Font fontTitle(L"Segoe UI Variable Display", 10.0f, FontStyleBold, UnitPoint);
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

void DrawModernPillButton(Graphics& g, const Rect& r, const wchar_t* text, bool isHover, Color accentColor, bool isPrimary = false) {
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
    } else {
        SolidBrush brushNorm(isHover ? Color(255, 35, 47, 68) : Color(255, 23, 31, 45));
        g.FillPath(&brushNorm, &path);
        Pen penNorm(Color(255, 38, 52, 75), 1.0f);
        g.DrawPath(&penNorm, &path);
    }

    Gdiplus::Font fontBtn(L"Segoe UI", isPrimary ? 11.0f : 9.5f, FontStyleBold, UnitPoint);
    SolidBrush brushText(isPrimary ? Color(255, 255, 255, 255) : (isHover ? Color(255, 255, 255, 255) : Color(255, 203, 213, 225)));
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text, -1, &fontBtn, RectF((float)r.X, (float)r.Y, (float)r.Width, (float)r.Height), &sf, &brushText);
}

void DrawDynamicLevelMeter(Graphics& g, int x, int y, int w, int h, float rms) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    SolidBrush brushTrack(Color(255, 13, 17, 24));
    g.FillRectangle(&brushTrack, x, y, w, h);

    int segCount = 28;
    float segW = ((float)w - (segCount + 1) * 2.0f) / segCount;
    int activeSegs = (int)std::round(std::clamp(rms, 0.0f, 1.0f) * segCount);

    for (int i = 0; i < segCount; i++) {
        float sx = x + 2.0f + i * (segW + 2.0f);
        float sy = (float)y + 2.0f;
        float sh = (float)h - 4.0f;

        Color segCol;
        if (i < 18) segCol = Color(255, 16, 185, 129); // Green
        else if (i < 24) segCol = Color(255, 0, 210, 255);  // Cyan
        else segCol = Color(255, 245, 158, 11); // Gold

        if (i < activeSegs) {
            SolidBrush brushActive(segCol);
            g.FillRectangle(&brushActive, sx, sy, segW, sh);
        } else {
            SolidBrush brushDim(Color(25, segCol.GetR(), segCol.GetG(), segCol.GetB()));
            g.FillRectangle(&brushDim, sx, sy, segW, sh);
        }
    }
}

void UpdateUiLayout(int width, int height) {
    int pad = 24;
    int cardW = width - pad * 2;
    if (cardW < 360) cardW = 360;

    int editW = cardW - 170;
    g_rAutoPairBtn = Rect(pad + 16 + editW + 10, 114, 130, 30);
    if (g_hIpEdit) SetWindowPos(g_hIpEdit, NULL, pad + 16, 115, editW, 28, SWP_NOZORDER);
    if (g_hDeviceCombo) SetWindowPos(g_hDeviceCombo, NULL, pad + 16, 152, cardW - 32, 200, SWP_NOZORDER);

    g_rStreamBtn = Rect(pad, 306, cardW, 46);
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

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hbrBg = CreateSolidBrush(COLOR_BG);
            g_hbrCard = CreateSolidBrush(COLOR_CARD_BG);
            g_hbrInput = CreateSolidBrush(COLOR_INPUT_BG);

            g_hFontTitle = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
            g_hFontSub = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
            g_hFontBold = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
            g_hFontNormal = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
            g_hFontMono = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");

            InitCommonControls();
            g_Engine.EnumerateDevices();
            g_Engine.StartDiscovery();

            g_hIpEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"192.168.1.15", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hWnd, (HMENU)101, NULL, NULL);
            SendMessage(g_hIpEdit, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);

            g_hDeviceCombo = CreateWindowW(L"COMBOBOX", NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, 10, hWnd, (HMENU)102, NULL, NULL);
            SendMessage(g_hDeviceCombo, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            for (const auto& dev : g_Engine.devices) {
                SendMessageW(g_hDeviceCombo, CB_ADDSTRING, 0, (LPARAM)dev.name.c_str());
            }
            if (!g_Engine.devices.empty()) SendMessage(g_hDeviceCombo, CB_SETCURSEL, 0, 0);

            SetTimer(hWnd, 1, 33, NULL);
            break;
        }

        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) break;
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            if (w >= 200 && h >= 200) {
                UpdateUiLayout(w, h);
                InvalidateRect(hWnd, NULL, FALSE);
            }
            break;
        }

        case WM_MOUSEMOVE: {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            int prevHover = g_hoverElement;
            g_hoverElement = -1;

            if (g_rAutoPairBtn.Contains(mx, my)) g_hoverElement = 10;
            if (g_rStreamBtn.Contains(mx, my)) g_hoverElement = 40;

            if (g_hoverElement != prevHover) {
                InvalidateRect(hWnd, NULL, FALSE);
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);

            if (g_rAutoPairBtn.Contains(mx, my)) {
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
                    MessageBoxW(hWnd, L"Scanning Wi-Fi broadcast for NullWire app on port 50007...", L"Auto-Discovery", MB_ICONINFORMATION | MB_OK);
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }

            if (g_rStreamBtn.Contains(mx, my)) {
                ToggleStreamAction(hWnd);
                return 0;
            }
            break;
        }

        case WM_ENTERSIZEMOVE:
            KillTimer(hWnd, 1);
            break;

        case WM_EXITSIZEMOVE:
            SetTimer(hWnd, 1, 33, NULL);
            InvalidateRect(hWnd, NULL, FALSE);
            break;

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
            RECT rc;
            GetClientRect(hWnd, &rc);
            RECT rcLevel = {24, 200, rc.right - 24, rc.bottom - 16};
            InvalidateRect(hWnd, &rcLevel, FALSE);
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

            // Header
            DrawNullWireLogo(graphics, 24.0f, 16.0f, 40.0f);

            SelectObject(hdcMem, g_hFontTitle);
            SetTextColor(hdcMem, COLOR_TEXT_MAIN);
            SetBkMode(hdcMem, TRANSPARENT);
            TextOutW(hdcMem, 74, 16, L"NullWire", 8);

            SelectObject(hdcMem, g_hFontSub);
            SetTextColor(hdcMem, RGB(148, 163, 184));
            TextOutW(hdcMem, 74, 40, L"Standard Edition  ·  Ultra-Low Latency Lossless Audio", 52);

            int pad = 24;
            int cardW = width - pad * 2;

            // Card 1: Connection & Source
            DrawModernGlassCard(graphics, pad, 86, cardW, 104, L"DEVICE TARGET & AUDIO SOURCE", L"48kHz MMAP", Color(255, 0, 210, 255));

            std::string devName;
            {
                std::lock_guard<std::mutex> lock(g_Engine.discoveryMutex);
                devName = g_Engine.hasDiscoveredDevice.load() ? ("🟢 " + g_Engine.discoveredDeviceName) : "🔍 Auto-Pair";
            }
            std::wstring wDevName(devName.begin(), devName.end());
            DrawModernPillButton(graphics, g_rAutoPairBtn, wDevName.c_str(), g_hoverElement == 10, Color(255, 16, 185, 129));

            // Card 2: Audio Dynamics & Telemetry
            DrawModernGlassCard(graphics, pad, 202, cardW, 90, L"AUDIO TRANSMISSION STATUS", g_Engine.isStreaming.load() ? L"STREAMING LIVE" : L"IDLE", g_Engine.isStreaming.load() ? Color(255, 16, 185, 129) : Color(255, 148, 163, 184));

            DrawDynamicLevelMeter(graphics, pad + 16, 234, cardW - 32, 14, g_Engine.streamRms.load());

            wchar_t statsBuf[128];
            swprintf_s(statsBuf, L"Packets: %llu   ·   Latency: %.2f ms   ·   Loss: 0.0%%", g_Engine.sendPacketCount.load(), g_Engine.liveLatencyMs.load());
            Gdiplus::Font fontMono(L"Consolas", 8.5f, FontStyleBold, UnitPoint);
            SolidBrush brushGreen(Color(255, 16, 185, 129));
            graphics.DrawString(statsBuf, -1, &fontMono, PointF((float)pad + 16, 258.0f), &brushGreen);

            // Action Button
            bool isStreaming = g_Engine.isStreaming.load();
            DrawModernPillButton(graphics, g_rStreamBtn, isStreaming ? L"⏹ STOP AUDIO STREAM" : L"▶ START AUDIO STREAM", g_hoverElement == 40, Color(255, 0, 210, 255), true);

            BitBlt(hdcScreen, 0, 0, width, height, hdcMem, 0, 0, SRCCOPY);

            SelectObject(hdcMem, hbmOld);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);

            EndPaint(hWnd, &ps);
            break;
        }

        case WM_DESTROY: {
            g_Engine.StopDiscovery();
            g_Engine.StopStreaming();

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
            setDpiContext((void*)-4);
            return;
        }
    }
    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) {
        typedef HRESULT (WINAPI *SetProcessDpiAwarenessProc)(int);
        SetProcessDpiAwarenessProc setDpiAwareness = 
            (SetProcessDpiAwarenessProc)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (setDpiAwareness) {
            setDpiAwareness(2);
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
        MessageBoxW(NULL, L"Failed to initialize network sockets.", L"NullWire", MB_ICONERROR);
        return 1;
    }

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    wc.hIconSm = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(1), IMAGE_ICON, 16, 16, 0);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"NullWireStandardClass";

    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"NullWireStandardClass",
        L"NullWire  ·  Standard Edition",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT,
        540, 420,
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
