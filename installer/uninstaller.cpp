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

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

#define COLOR_WHITE RGB(255, 255, 255)
#define COLOR_NAV_BG RGB(246, 248, 250)
#define COLOR_BORDER_LINE RGB(226, 232, 240)
#define COLOR_TEXT_MAIN RGB(15, 23, 42)
#define COLOR_TEXT_SUB RGB(71, 85, 105)

enum class UninstStep {
    CONFIRM = 0,
    UNINSTALLING,
    FINISHED
};

static UninstStep g_Step = UninstStep::CONFIRM;
static ULONG_PTR g_gdiplusToken = 0;

static HWND g_hProgressBar = NULL;
static HWND g_hStatusText = NULL;
static HWND g_hActionBtn = NULL;
static HWND g_hCancelBtn = NULL;

static HFONT g_hFontTitle = NULL;
static HFONT g_hFontNormal = NULL;
static HFONT g_hFontSmall = NULL;

void DrawUninstallerBanner(Graphics& g, int x, int y, int w, int h) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    Point ptStart(x, y);
    Point ptEnd(x, y + h);
    Color colTop(255, 0, 56, 132);       // Deep Blue
    Color colMid(255, 36, 24, 90);       // Rich Indigo
    Color colBottom(255, 126, 27, 117);  // Magenta/Purple

    LinearGradientBrush gradBrush(ptStart, ptEnd, colTop, colBottom);
    Color blendCols[] = { colTop, colMid, colBottom };
    REAL blendPositions[] = { 0.0f, 0.45f, 1.0f };
    gradBrush.SetInterpolationColors(blendCols, blendPositions, 3);
    g.FillRectangle(&gradBrush, x, y, w, h);

    // Constellation lines & nodes
    Pen penNet(Color(60, 255, 255, 255), 1.0f);
    SolidBrush brushNode(Color(220, 255, 255, 255));
    SolidBrush brushGlow(Color(80, 255, 255, 255));

    struct Node { float nx, ny, r; };
    Node nodes[] = {
        { x + 24.0f, y + 140.0f, 2.5f },
        { x + 50.0f, y + 210.0f, 3.5f },
        { x + 120.0f, y + 200.0f, 2.5f },
        { x + 155.0f, y + 260.0f, 4.0f },
        { x + 70.0f, y + 290.0f, 3.0f },
        { x + 130.0f, y + 330.0f, 3.5f },
        { x + 40.0f, y + 370.0f, 5.0f }
    };

    for (int i = 0; i < 6; i++) {
        g.DrawLine(&penNet, nodes[i].nx, nodes[i].ny, nodes[i+1].nx, nodes[i+1].ny);
    }
    for (const auto& nd : nodes) {
        g.FillEllipse(&brushGlow, nd.nx - nd.r - 2.0f, nd.ny - nd.r - 2.0f, (nd.r + 2.0f) * 2, (nd.r + 2.0f) * 2);
        g.FillEllipse(&brushNode, nd.nx - nd.r, nd.ny - nd.r, nd.r * 2, nd.r * 2);
    }

    // Logo Icon
    float logoCX = x + w / 2.0f;
    float logoCY = y + 75.0f;
    float outerR = 38.0f;
    float innerR = 21.0f;

    SolidBrush brushNavyRing(Color(255, 6, 27, 68));
    GraphicsPath ringPath;
    ringPath.AddEllipse(logoCX - outerR, logoCY - outerR, outerR * 2, outerR * 2);
    ringPath.AddEllipse(logoCX - innerR, logoCY - innerR, innerR * 2, innerR * 2);
    g.FillPath(&brushNavyRing, &ringPath);

    Pen penWaveCyan(Color(255, 0, 210, 255), 3.0f);
    Pen penWavePurple(Color(255, 168, 85, 247), 3.0f);

    GraphicsPath waveL;
    waveL.StartFigure();
    waveL.AddLine(logoCX - 62.0f, logoCY, logoCX - 48.0f, logoCY);
    waveL.AddLine(logoCX - 48.0f, logoCY, logoCX - 40.0f, logoCY - 22.0f);
    waveL.AddLine(logoCX - 40.0f, logoCY - 22.0f, logoCX - 32.0f, logoCY + 22.0f);
    waveL.AddLine(logoCX - 32.0f, logoCY + 22.0f, logoCX - 24.0f, logoCY - 12.0f);
    waveL.AddLine(logoCX - 24.0f, logoCY - 12.0f, logoCX - 16.0f, logoCY);
    g.DrawPath(&penWaveCyan, &waveL);

    GraphicsPath waveR;
    waveR.StartFigure();
    waveR.AddLine(logoCX + 16.0f, logoCY, logoCX + 24.0f, logoCY);
    waveR.AddLine(logoCX + 24.0f, logoCY, logoCX + 32.0f, logoCY - 20.0f);
    waveR.AddLine(logoCX + 32.0f, logoCY - 20.0f, logoCX + 40.0f, logoCY + 20.0f);
    waveR.AddLine(logoCX + 40.0f, logoCY + 20.0f, logoCX + 48.0f, logoCY - 10.0f);
    waveR.AddLine(logoCX + 48.0f, logoCY - 10.0f, logoCX + 62.0f, logoCY);
    g.DrawPath(&penWavePurple, &waveR);
}

void StartUninstallationThread(HWND hWnd) {
    g_Step = UninstStep::UNINSTALLING;
    ShowWindow(g_hProgressBar, SW_SHOW);
    ShowWindow(g_hStatusText, SW_SHOW);
    EnableWindow(g_hActionBtn, FALSE);
    EnableWindow(g_hCancelBtn, FALSE);
    InvalidateRect(hWnd, NULL, TRUE);

    std::thread([hWnd]() {
        auto uiStatus = [hWnd](const wchar_t* text, int pct) {
            SetWindowTextW(g_hStatusText, text);
            SendMessage(g_hProgressBar, PBM_SETPOS, pct, 0);
        };

        uiStatus(L"Stopping running NullWire instances...", 20);
        HWND hRunning = FindWindowW(L"NullWireProClass", NULL);
        if (hRunning) {
            PostMessageW(hRunning, WM_CLOSE, 0, 0);
            Sleep(500);
        }
        system("taskkill /f /im NullWireSender.exe 2>nul");

        uiStatus(L"Removing desktop and start menu shortcuts...", 50);
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

        uiStatus(L"Removing registry settings...", 75);
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NullWirePro");

        uiStatus(L"Removing installed binaries...", 90);
        wchar_t currentExe[MAX_PATH];
        GetModuleFileNameW(NULL, currentExe, MAX_PATH);
        PathRemoveFileSpecW(currentExe);
        std::wstring installDir = currentExe;

        std::wstring senderPath = installDir + L"\\NullWireSender.exe";
        DeleteFileW(senderPath.c_str());

        // Schedule uninstaller file self-deletion via cmd
        std::wstring selfPath = installDir + L"\\NullWire_Uninstall.exe";
        std::wstring cmd = L"/c ping 127.0.0.1 -n 2 > nul & del /f /q \"" + selfPath + L"\" & rmdir \"" + installDir + L"\"";
        ShellExecuteW(NULL, L"open", L"cmd.exe", cmd.c_str(), NULL, SW_HIDE);

        uiStatus(L"Uninstallation completed.", 100);
        g_Step = UninstStep::FINISHED;
        PostMessageW(hWnd, WM_USER + 101, 0, 0);
    }).detach();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hFontTitle = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontNormal = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            InitCommonControls();

            g_hProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | PBS_SMOOTH, 195, 140, 330, 18, hWnd, (HMENU)301, NULL, NULL);
            SendMessage(g_hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

            g_hStatusText = CreateWindowW(L"STATIC", L"Removing components...", WS_CHILD, 195, 170, 330, 20, hWnd, (HMENU)302, NULL, NULL);
            SendMessage(g_hStatusText, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

            g_hActionBtn = CreateWindowW(L"BUTTON", L"Uninstall", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 362, 322, 75, 24, hWnd, (HMENU)401, NULL, NULL);
            SendMessage(g_hActionBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            g_hCancelBtn = CreateWindowW(L"BUTTON", L"Cancel", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 452, 322, 75, 24, hWnd, (HMENU)402, NULL, NULL);
            SendMessage(g_hCancelBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            break;
        }

        case WM_USER + 101: {
            ShowWindow(g_hProgressBar, SW_HIDE);
            ShowWindow(g_hStatusText, SW_HIDE);
            SetWindowTextW(g_hActionBtn, L"Finish");
            EnableWindow(g_hActionBtn, TRUE);
            ShowWindow(g_hCancelBtn, SW_HIDE);
            InvalidateRect(hWnd, NULL, TRUE);
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_TEXT_MAIN);
            SetBkColor(hdc, COLOR_WHITE);
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            int totalW = rcClient.right;
            int totalH = rcClient.bottom;

            int bannerW = 175;
            int contentH = 312;

            HBRUSH hbrWhite = CreateSolidBrush(COLOR_WHITE);
            RECT rcContent = { bannerW, 0, totalW, contentH };
            FillRect(hdc, &rcContent, hbrWhite);
            DeleteObject(hbrWhite);

            HBRUSH hbrNav = CreateSolidBrush(COLOR_NAV_BG);
            RECT rcNav = { 0, contentH, totalW, totalH };
            FillRect(hdc, &rcNav, hbrNav);
            DeleteObject(hbrNav);

            HPEN hPenLine = CreatePen(PS_SOLID, 1, COLOR_BORDER_LINE);
            SelectObject(hdc, hPenLine);
            MoveToEx(hdc, 0, contentH, NULL);
            LineTo(hdc, totalW, contentH);
            DeleteObject(hPenLine);

            Graphics graphics(hdc);
            DrawUninstallerBanner(graphics, 0, 0, bannerW, contentH);

            SetBkMode(hdc, TRANSPARENT);

            if (g_Step == UninstStep::CONFIRM) {
                SelectObject(hdc, g_hFontTitle);
                SetTextColor(hdc, COLOR_TEXT_MAIN);
                RECT rcT = { 195, 28, totalW - 20, 80 };
                DrawTextW(hdc, L"Uninstall NullWire Pro", -1, &rcT, DT_WORDBREAK);

                SelectObject(hdc, g_hFontNormal);
                SetTextColor(hdc, COLOR_TEXT_SUB);
                RECT rcD = { 195, 80, totalW - 20, contentH - 20 };
                DrawTextW(hdc, L"Are you sure you want to completely remove NullWire Pro and all of its components from your computer?\r\n\r\nClick Uninstall to proceed, or Cancel to exit.", -1, &rcD, DT_WORDBREAK);
            } else if (g_Step == UninstStep::UNINSTALLING) {
                SelectObject(hdc, g_hFontTitle);
                SetTextColor(hdc, COLOR_TEXT_MAIN);
                TextOutW(hdc, 195, 28, L"Uninstalling NullWire Pro", 25);

                SelectObject(hdc, g_hFontNormal);
                SetTextColor(hdc, COLOR_TEXT_SUB);
                TextOutW(hdc, 195, 65, L"Please wait while NullWire Pro is removed from your computer.", 61);
            } else if (g_Step == UninstStep::FINISHED) {
                SelectObject(hdc, g_hFontTitle);
                SetTextColor(hdc, COLOR_TEXT_MAIN);
                RECT rcT = { 195, 28, totalW - 20, 80 };
                DrawTextW(hdc, L"Uninstallation Complete", -1, &rcT, DT_WORDBREAK);

                SelectObject(hdc, g_hFontNormal);
                SetTextColor(hdc, COLOR_TEXT_SUB);
                RECT rcD = { 195, 80, totalW - 20, contentH - 20 };
                DrawTextW(hdc, L"NullWire Pro was successfully removed from your computer.\r\n\r\nClick Finish to close this wizard.", -1, &rcD, DT_WORDBREAK);
            }

            EndPaint(hWnd, &ps);
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == 401) {
                if (g_Step == UninstStep::CONFIRM) {
                    StartUninstallationThread(hWnd);
                } else if (g_Step == UninstStep::FINISHED) {
                    PostMessageW(hWnd, WM_CLOSE, 0, 0);
                }
            } else if (wmId == 402) {
                PostMessageW(hWnd, WM_CLOSE, 0, 0);
            }
            break;
        }

        case WM_DESTROY: {
            if (g_hFontTitle) DeleteObject(g_hFontTitle);
            if (g_hFontNormal) DeleteObject(g_hFontNormal);
            if (g_hFontSmall) DeleteObject(g_hFontSmall);
            PostQuitMessage(0);
            break;
        }

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    wc.hIconSm = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(1), IMAGE_ICON, 16, 16, 0);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NullWireUninstallerClass";

    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(
        0,
        L"NullWireUninstallerClass",
        L"NullWire Pro Uninstall",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        550, 395,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) return 1;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    CoUninitialize();
    return (int)msg.wParam;
}
