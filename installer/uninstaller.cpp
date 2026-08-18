#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

// ── Windows 11 OOBE Light Palette ────────────────────────────────────────────
#define COLOR_WIN11_BG         RGB(255, 255, 255)
#define COLOR_WIN11_SURFACE    RGB(249, 250, 251)
#define COLOR_WIN11_BLUE       RGB(0, 103, 192)
#define COLOR_WIN11_BLUE_HOVER RGB(24, 115, 196)
#define COLOR_WIN11_RED        RGB(220, 38, 38)
#define COLOR_WIN11_RED_HOVER  RGB(239, 68, 68)
#define COLOR_WIN11_TEXT_TITLE RGB(27, 27, 27)
#define COLOR_WIN11_TEXT_SUB   RGB(94, 94, 94)
#define COLOR_WIN11_BORDER     RGB(229, 231, 235)

enum class UninstStep {
    CONFIRM = 0,
    UNINSTALLING,
    FINISHED
};

static UninstStep g_Step = UninstStep::CONFIRM;
static ULONG_PTR g_gdiplusToken = 0;

static bool g_isActionHovered = false;
static bool g_isCancelHovered = false;

static RECT g_rcActionBtn { 650, 448, 765, 484 };
static RECT g_rcCancelBtn { 560, 448, 635, 484 };

static HBRUSH g_hBrushWhite = NULL;

static std::atomic<int> g_UninstProgress{0};
static std::wstring g_UninstStatusText = L"Ready to remove NullWire Pro...";
static std::mutex g_UninstMutex;

void AddRoundedRect(GraphicsPath& path, RectF rect, float radius) {
    float d = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// ── Official NullWire Brand Vector Logo ───────────────────────────────────────
void DrawOfficialNullWireLogo(Graphics& g, float cx, float cy, float radius) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    float scale = radius / 50.0f;

    auto sp = [&](float x, float y) -> PointF {
        return PointF(cx + (x - 60.0f) * scale, cy + (y - 60.0f) * scale);
    };

    // 1. Clean Subtle Ambient Drop Shadow (No neon halos)
    for (int s = 1; s <= 4; ++s) {
        float shR = radius + s * 1.5f;
        SolidBrush shBrush(Color(static_cast<BYTE>(10 - s * 2), 0, 0, 0));
        g.FillEllipse(&shBrush, cx - shR, cy - shR + s * 1.5f, shR * 2.0f, shR * 2.0f);
    }

    // 2. Deep Navy Background Circle (#0A192F)
    SolidBrush bgNavy(Color(255, 10, 25, 47));
    g.FillEllipse(&bgNavy, cx - 50.0f * scale, cy - 50.0f * scale, 100.0f * scale, 100.0f * scale);

    // 3. Dark Navy Outer Ring 'O' (#061B44)
    GraphicsPath ringPath;
    float rOuter = 36.0f * scale;
    float rInner = 18.0f * scale;
    ringPath.AddEllipse(cx - rOuter, cy - rOuter, rOuter * 2.0f, rOuter * 2.0f);
    ringPath.AddEllipse(cx - rInner, cy - rInner, rInner * 2.0f, rInner * 2.0f);
    SolidBrush ringBrush(Color(255, 6, 27, 68));
    g.FillPath(&ringBrush, &ringPath);

    // 4. Inner Cyan Arc (#00D2FF)
    Pen arcPen(Color(255, 0, 210, 255), 2.5f * scale);
    g.DrawArc(&arcPen, cx - 18.0f * scale, cy - 18.0f * scale, 36.0f * scale, 36.0f * scale, 180.0f, 180.0f);

    // 5. Left Cyan Audio Waveform (M12,60 L24,60 L32,38 L40,82 L48,48 L56,60)
    PointF cyanPts[] = {
        sp(12.0f, 60.0f),
        sp(24.0f, 60.0f),
        sp(32.0f, 38.0f),
        sp(40.0f, 82.0f),
        sp(48.0f, 48.0f),
        sp(56.0f, 60.0f)
    };
    Pen waveCyan(Color(255, 0, 210, 255), 4.0f * scale);
    waveCyan.SetStartCap(LineCapRound);
    waveCyan.SetEndCap(LineCapRound);
    waveCyan.SetLineJoin(LineJoinRound);
    g.DrawLines(&waveCyan, cyanPts, 6);

    // 6. Right Purple Audio Waveform (M64,60 L72,48 L80,82 L88,38 L96,60 L108,60)
    PointF purplePts[] = {
        sp(64.0f, 60.0f),
        sp(72.0f, 48.0f),
        sp(80.0f, 82.0f),
        sp(88.0f, 38.0f),
        sp(96.0f, 60.0f),
        sp(108.0f, 60.0f)
    };
    Pen wavePurple(Color(255, 168, 85, 247), 4.0f * scale);
    wavePurple.SetStartCap(LineCapRound);
    wavePurple.SetEndCap(LineCapRound);
    wavePurple.SetLineJoin(LineJoinRound);
    g.DrawLines(&wavePurple, purplePts, 6);
}

// ── Left Column Display (Just Clean Official Logo) ────────────────────────────
void DrawWindows11Illustration(Graphics& g, int cx, int cy) {
    DrawOfficialNullWireLogo(g, static_cast<float>(cx), static_cast<float>(cy), 72.0f);
}




// ── Windows 11 Primary & Secondary Buttons ────────────────────────────────────
void DrawWindows11Button(Graphics& g, const RECT& rc, const wchar_t* text, bool isPrimary, bool isHovered, bool isEnabled = true) {
    RectF btnRect(
        static_cast<REAL>(rc.left),
        static_cast<REAL>(rc.top),
        static_cast<REAL>(rc.right - rc.left),
        static_cast<REAL>(rc.bottom - rc.top)
    );

    FontFamily ff(L"Segoe UI");
    Gdiplus::Font btnFont(&ff, 12, FontStyleBold, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);

    if (isPrimary) {
        GraphicsPath btnPath;
        AddRoundedRect(btnPath, btnRect, 6.0f);

        Color btnCol = isEnabled ? (isHovered ? Color(255, 24, 115, 196) : Color(255, 0, 103, 192)) : Color(255, 204, 204, 204);
        SolidBrush btnBrush(btnCol);
        g.FillPath(&btnBrush, &btnPath);

        SolidBrush textBrush(Color(255, 255, 255, 255));
        g.DrawString(text, -1, &btnFont, btnRect, &sf, &textBrush);
    } else {
        // Flat text / subtle secondary button
        if (isHovered && isEnabled) {
            GraphicsPath btnPath;
            AddRoundedRect(btnPath, btnRect, 6.0f);
            SolidBrush hovBrush(Color(255, 243, 244, 246));
            g.FillPath(&hovBrush, &btnPath);
        }

        SolidBrush textBrush(isEnabled ? (isHovered ? Color(255, 0, 90, 170) : Color(255, 0, 103, 192)) : Color(255, 170, 170, 170));
        g.DrawString(text, -1, &btnFont, btnRect, &sf, &textBrush);
    }
}

void ExecuteHiddenCommandWithDir(const std::wstring& cmd, const std::wstring& workDir = L"") {
    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring cmdCopy = cmd;
    const wchar_t* pDir = workDir.empty() ? NULL : workDir.c_str();
    if (CreateProcessW(NULL, &cmdCopy[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, pDir, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void StartUninstallationThread(HWND hWnd) {
    g_Step = UninstStep::UNINSTALLING;
    InvalidateRect(hWnd, NULL, TRUE);

    std::thread([hWnd]() {
        auto uiStatus = [hWnd](const wchar_t* text, int pct) {
            {
                std::lock_guard<std::mutex> lock(g_UninstMutex);
                g_UninstStatusText = text;
                g_UninstProgress.store(pct);
            }
            InvalidateRect(hWnd, NULL, FALSE);
        };

        uiStatus(L"Stopping running NullWire instances...", 15);
        HWND hRunning = FindWindowW(L"NullWireProMainWindowClass", NULL);
        if (!hRunning) hRunning = FindWindowW(L"NullWireProClass", NULL);
        if (hRunning) {
            PostMessageW(hRunning, WM_CLOSE, 0, 0);
            Sleep(500);
        }
        system("taskkill /f /im NullWire.exe 2>nul");
        system("taskkill /f /im NullWireSender.exe 2>nul");

        wchar_t currentExe[MAX_PATH];
        GetModuleFileNameW(NULL, currentExe, MAX_PATH);
        PathRemoveFileSpecW(currentExe);
        std::wstring installDir = currentExe;

        uiStatus(L"Uninstalling Virtual Audio Cable Driver...", 40);

        // 1. Run Setup uninstaller if present in install dir or local appdata
        std::wstring vbcableDir = installDir + L"\\driver\\vbcable";
        std::wstring vbcableSetup = vbcableDir + L"\\VBCABLE_Setup_x64.exe";
        if (!PathFileExistsW(vbcableSetup.c_str())) {
            wchar_t localApp[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localApp))) {
                vbcableDir = std::wstring(localApp) + L"\\NullWire\\driver\\vbcable";
                vbcableSetup = vbcableDir + L"\\VBCABLE_Setup_x64.exe";
            }
        }
        if (PathFileExistsW(vbcableSetup.c_str())) {
            ExecuteHiddenCommandWithDir(L"\"" + vbcableSetup + L"\" -u -h", vbcableDir);
        }

        // 2. Force remove OEM driver package from Windows Driver Store via PowerShell + PnPUtil
        std::wstring psDriverClean = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Get-WindowsDriver -Online -All | Where-Object { $_.OriginalFileName -like '*vbmmecable*' -or $_.ProviderName -like '*VB-Audio*' -or $_.OriginalFileName -like '*vbaudio*' } | ForEach-Object { pnputil.exe /delete-driver $_.Driver /uninstall /force }\"";
        ExecuteHiddenCommandWithDir(psDriverClean);

        // 3. Stop and remove any residual driver services
        ExecuteHiddenCommandWithDir(L"cmd.exe /c net stop \"VB-Audio Cable\" 2>nul");
        ExecuteHiddenCommandWithDir(L"cmd.exe /c sc.exe delete \"VB-Audio Cable\" 2>nul");

        // 4. Force Windows PnP device tree refresh
        ExecuteHiddenCommandWithDir(L"pnputil.exe /scan-devices");

        uiStatus(L"Removing desktop and start menu shortcuts...", 65);
        wchar_t deskPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, deskPath))) {
            std::wstring sc = std::wstring(deskPath) + L"\\NullWire Pro.lnk";
            DeleteFileW(sc.c_str());
        }

        wchar_t startPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, startPath))) {
            std::wstring sc = std::wstring(startPath) + L"\\NullWire Pro.lnk";
            DeleteFileW(sc.c_str());
        }

        uiStatus(L"Removing registry settings...", 80);
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NullWirePro");

        uiStatus(L"Removing installed binaries...", 95);
        std::wstring senderPath1 = installDir + L"\\NullWire.exe";
        std::wstring senderPath2 = installDir + L"\\NullWireSender.exe";
        DeleteFileW(senderPath1.c_str());
        DeleteFileW(senderPath2.c_str());

        // Schedule uninstaller file self-deletion via cmd
        std::wstring selfPath1 = installDir + L"\\Uninstall.exe";
        std::wstring selfPath2 = installDir + L"\\NullWire_Uninstall.exe";
        std::wstring cmd = L"/c ping 127.0.0.1 -n 2 > nul & del /f /q \"" + selfPath1 + L"\" & del /f /q \"" + selfPath2 + L"\" & rmdir /s /q \"" + installDir + L"\"";
        ShellExecuteW(NULL, L"open", L"cmd.exe", cmd.c_str(), NULL, SW_HIDE);

        uiStatus(L"Uninstallation completed successfully.", 100);
        g_Step = UninstStep::FINISHED;
        PostMessageW(hWnd, WM_APP + 1, 0, 0);
    }).detach();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hBrushWhite = CreateSolidBrush(COLOR_WIN11_BG);
            InitCommonControls();
            break;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool prevAction = g_isActionHovered;
            bool prevCancel = g_isCancelHovered;

            g_isActionHovered = PtInRect(&g_rcActionBtn, pt);
            g_isCancelHovered = PtInRect(&g_rcCancelBtn, pt);

            if (prevAction != g_isActionHovered || prevCancel != g_isCancelHovered) {
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

            // Primary Action Button (Uninstall / OK)
            if (PtInRect(&g_rcActionBtn, pt)) {
                if (g_Step == UninstStep::CONFIRM) {
                    StartUninstallationThread(hWnd);
                } else if (g_Step == UninstStep::FINISHED) {
                    PostMessageW(hWnd, WM_CLOSE, 0, 0);
                }
                return 0;
            }

            // Cancel Button
            if (PtInRect(&g_rcCancelBtn, pt)) {
                if (g_Step != UninstStep::UNINSTALLING) {
                    PostMessageW(hWnd, WM_CLOSE, 0, 0);
                }
                return 0;
            }

            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            int w = rcClient.right - rcClient.left;
            int h = rcClient.bottom - rcClient.top;

            // Double Buffering
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hbmMem = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

            Graphics g(hdcMem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

            // 1. Pure Clean Windows 11 Background
            SolidBrush brushBg(Color(255, 255, 255, 255));
            g.FillRectangle(&brushBg, 0, 0, w, h);

            // 2. Left Column Illustration (Width 300px)
            DrawWindows11Illustration(g, 150, 240);

            // 3. Right Column Content (X = 340 to 760)
            FontFamily ff(L"Segoe UI");
            Gdiplus::Font fontHero(&ff, 22, FontStyleBold, UnitPixel);
            Gdiplus::Font fontSub(&ff, 12, FontStyleRegular, UnitPixel);
            Gdiplus::Font fontSecTitle(&ff, 12, FontStyleBold, UnitPixel);
            Gdiplus::Font fontBody(&ff, 12, FontStyleRegular, UnitPixel);
            Gdiplus::Font fontBullet(&ff, 11, FontStyleRegular, UnitPixel);

            SolidBrush textTitleBrush(Color(255, 27, 27, 27));
            SolidBrush textSubBrush(Color(255, 94, 94, 94));
            SolidBrush textBodyBrush(Color(255, 40, 40, 40));
            SolidBrush textBlueBrush(Color(255, 0, 103, 192));

            switch (g_Step) {
                case UninstStep::CONFIRM: {
                    DrawOfficialNullWireLogo(g, 355.0f, 52.0f, 13.0f);
                    g.DrawString(L"Uninstall NullWire Pro", -1, &fontHero, PointF(376.0f, 40.0f), &textTitleBrush);
                    g.DrawString(L"Remove NullWire Pro and all associated virtual audio drivers completely from this PC.", -1, &fontSub, RectF(340.0f, 75.0f, 420.0f, 40.0f), NULL, &textSubBrush);

                    g.DrawString(L"Components that will be removed:", -1, &fontSecTitle, PointF(340.0f, 135.0f), &textTitleBrush);

                    const wchar_t* items[] = {
                        L"NullWire Pro 64-bit Core Engine and runtime configurations",
                        L"Microsoft-Certified Virtual Audio Cable Driver & Driver Store packages",
                        L"Audio device endpoints & Windows registry integration",
                        L"Desktop and Start Menu application shortcuts"
                    };

                    float itemY = 162.0f;
                    for (const auto& it : items) {
                        SolidBrush dotBrush(Color(255, 0, 103, 192));
                        g.FillEllipse(&dotBrush, 342.0f, itemY + 4.0f, 5.0f, 5.0f);
                        g.DrawString(it, -1, &fontBullet, PointF(355.0f, itemY), &textBodyBrush);
                        itemY += 24.0f;
                    }
                    break;
                }

                case UninstStep::UNINSTALLING: {
                    DrawOfficialNullWireLogo(g, 355.0f, 52.0f, 13.0f);
                    g.DrawString(L"Uninstalling NullWire Pro", -1, &fontHero, PointF(376.0f, 40.0f), &textTitleBrush);
                    g.DrawString(L"Purging application files, driver store packages, and registry keys...", -1, &fontSub, RectF(340.0f, 75.0f, 420.0f, 40.0f), NULL, &textSubBrush);

                    int curPct = g_UninstProgress.load();
                    std::wstring statusMsg;
                    {
                        std::lock_guard<std::mutex> lock(g_UninstMutex);
                        statusMsg = g_UninstStatusText;
                    }

                    g.DrawString(statusMsg.c_str(), -1, &fontSecTitle, PointF(340.0f, 155.0f), &textTitleBrush);

                    // Windows 11 Blue Progress Bar
                    float barW = 415.0f;
                    float barH = 6.0f;
                    RectF trackRect(340.0f, 185.0f, barW, barH);
                    GraphicsPath trackPath;
                    AddRoundedRect(trackPath, trackRect, 3.0f);
                    SolidBrush trackBrush(Color(255, 229, 231, 235));
                    g.FillPath(&trackBrush, &trackPath);

                    float fillW = (barW * curPct) / 100.0f;
                    if (fillW > 0.0f) {
                        RectF fillRect(340.0f, 185.0f, fillW, barH);
                        GraphicsPath fillPath;
                        AddRoundedRect(fillPath, fillRect, 3.0f);
                        SolidBrush fillBrush(Color(255, 0, 103, 192));
                        g.FillPath(&fillBrush, &fillPath);
                    }

                    // Progress Percentage
                    std::wstring pctStr = std::to_wstring(curPct) + L"%";
                    g.DrawString(pctStr.c_str(), -1, &fontSecTitle, PointF(340.0f + barW - 35.0f, 200.0f), &textBlueBrush);
                    break;
                }

                case UninstStep::FINISHED: {
                    DrawOfficialNullWireLogo(g, 355.0f, 52.0f, 13.0f);
                    g.DrawString(L"Uninstallation Completed", -1, &fontHero, PointF(376.0f, 40.0f), &textTitleBrush);
                    g.DrawString(L"NullWire Pro and all virtual audio drivers were cleanly removed from this system.", -1, &fontSub, RectF(340.0f, 75.0f, 420.0f, 40.0f), NULL, &textSubBrush);

                    // Success Pill
                    RectF succRect(340.0f, 145.0f, 415.0f, 52.0f);
                    GraphicsPath succPath;
                    AddRoundedRect(succPath, succRect, 8.0f);
                    SolidBrush succBg(Color(255, 240, 253, 244));
                    g.FillPath(&succBg, &succPath);
                    Pen succBorder(Color(255, 187, 247, 208), 1.0f);
                    g.DrawPath(&succBorder, &succPath);

                    SolidBrush succText(Color(255, 22, 101, 52));
                    g.DrawString(L"✓  All Drivers & System Components Purged", -1, &fontSecTitle, PointF(356.0f, 155.0f), &succText);
                    SolidBrush succSubText(Color(255, 21, 128, 61));
                    g.DrawString(L"Windows audio endpoint routing has been restored to defaults.", -1, &fontBullet, PointF(356.0f, 172.0f), &succSubText);
                    break;
                }
            }

            // Bottom Action Buttons
            if (g_Step != UninstStep::UNINSTALLING) {
                if (g_Step == UninstStep::CONFIRM) {
                    DrawWindows11Button(g, g_rcCancelBtn, L"Cancel", false, g_isCancelHovered);
                }

                std::wstring actionText = (g_Step == UninstStep::FINISHED) ? L"OK" : L"Uninstall";
                DrawWindows11Button(g, g_rcActionBtn, actionText.c_str(), true, g_isActionHovered);
            }

            BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
            SelectObject(hdcMem, hbmOld);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);

            EndPaint(hWnd, &ps);
            break;
        }

        case WM_APP + 1:
            g_Step = UninstStep::FINISHED;
            InvalidateRect(hWnd, NULL, TRUE);
            break;

        case WM_DESTROY:
            if (g_hBrushWhite) DeleteObject(g_hBrushWhite);
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"NullWireProUninstaller";

    RegisterClassExW(&wc);

    int windowW = 800;
    int windowH = 530;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - windowW) / 2;
    int posY = (screenH - windowH) / 2;

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"NullWireProUninstaller",
        L"NullWire Pro · Uninstall",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, windowW, windowH,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) {
        GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}
