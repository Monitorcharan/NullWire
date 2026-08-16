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
#include <algorithm>
#include <cctype>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uuid.lib")

using namespace Gdiplus;

// ── Colors ──────────────────────────────────────────────────────────────────
#define COLOR_WHITE RGB(255, 255, 255)
#define COLOR_NAV_BG RGB(246, 248, 250)
#define COLOR_BORDER_LINE RGB(226, 232, 240)
#define COLOR_TEXT_MAIN RGB(15, 23, 42)
#define COLOR_TEXT_SUB RGB(71, 85, 105)
#define COLOR_PRIMARY_BLUE RGB(0, 102, 204)

enum class WizardStep {
    WELCOME = 0,
    DESTINATION,
    INSTALLING,
    FINISHED
};

static WizardStep g_CurrentStep = WizardStep::WELCOME;
static ULONG_PTR g_gdiplusToken = 0;

static HWND g_hInstallPathEdit = NULL;
static HWND g_hBrowseBtn = NULL;
static HWND g_hDesktopCheck = NULL;
static HWND g_hStartMenuCheck = NULL;
static HWND g_hLaunchCheck = NULL;
static HWND g_hProgressBar = NULL;
static HWND g_hStatusText = NULL;

static HWND g_hBackBtn = NULL;
static HWND g_hNextBtn = NULL;
static HWND g_hCancelBtn = NULL;

static HFONT g_hFontTitle = NULL;
static HFONT g_hFontNormal = NULL;
static HFONT g_hFontBold = NULL;
static HFONT g_hFontSmall = NULL;

std::wstring GetDefaultInstallPath() {
    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        return std::wstring(localAppData) + L"\\NullWire";
    }
    return L"C:\\NullWire";
}

bool IsSafeInstallDir(const std::wstring& path) {
    if (path.size() < 8 || path.size() >= MAX_PATH) return false;
    if (path.find(L"..") != std::wstring::npos) return false;
    for (wchar_t c : path) {
        if (c < 32 || c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'*' || c == L'?') return false;
    }

    wchar_t full[MAX_PATH]{};
    if (!GetFullPathNameW(path.c_str(), MAX_PATH, full, NULL)) return false;

    wchar_t winDir[MAX_PATH]{};
    wchar_t sysDir[MAX_PATH]{};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    GetSystemDirectoryW(sysDir, MAX_PATH);
    if (winDir[0] && PathIsPrefixW(winDir, full)) return false;
    if (sysDir[0] && PathIsPrefixW(sysDir, full)) return false;
    if (PathIsRootW(full)) return false;
    return true;
}

bool CreateShortcut(const std::wstring& targetPath, const std::wstring& shortcutPath, const std::wstring& description) {
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool didInit = SUCCEEDED(hrInit);
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (LPVOID*)&psl);
    if (SUCCEEDED(hr) && psl) {
        psl->SetPath(targetPath.c_str());
        psl->SetDescription(description.c_str());

        wchar_t dir[MAX_PATH];
        wcsncpy_s(dir, targetPath.c_str(), _TRUNCATE);
        PathRemoveFileSpecW(dir);
        psl->SetWorkingDirectory(dir);

        IPersistFile* ppf = nullptr;
        hr = psl->QueryInterface(IID_IPersistFile, (LPVOID*)&ppf);
        if (SUCCEEDED(hr) && ppf) {
            hr = ppf->Save(shortcutPath.c_str(), TRUE);
            ppf->Release();
        }
        psl->Release();
    }
    if (didInit) CoUninitialize();
    return SUCCEEDED(hr);
}

void RegisterInWindowsAddRemovePrograms(const std::wstring& installDir, const std::wstring& exePath, const std::wstring& uninstallerPath) {
    HKEY hKey;
    const wchar_t* subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NullWirePro";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        const wchar_t* displayName = L"NullWire Pro";
        const wchar_t* publisher = L"NullWire Audio Engineering";
        const wchar_t* version = L"2.0.0";
        
        RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ, (const BYTE*)displayName, (DWORD)((wcslen(displayName) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"DisplayIcon", 0, REG_SZ, (const BYTE*)exePath.c_str(), (DWORD)((exePath.length() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"Publisher", 0, REG_SZ, (const BYTE*)publisher, (DWORD)((wcslen(publisher) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"DisplayVersion", 0, REG_SZ, (const BYTE*)version, (DWORD)((wcslen(version) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"UninstallString", 0, REG_SZ, (const BYTE*)uninstallerPath.c_str(), (DWORD)((uninstallerPath.length() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"InstallLocation", 0, REG_SZ, (const BYTE*)installDir.c_str(), (DWORD)((installDir.length() + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

void BrowseFolder(HWND hWnd) {
    BROWSEINFOW bi{};
    bi.hwndOwner = hWnd;
    bi.lpszTitle = L"Select NullWire Pro Installation Destination:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            if (!IsSafeInstallDir(std::wstring(path) + L"\\NullWire") && !IsSafeInstallDir(path)) {
                CoTaskMemFree(pidl);
                MessageBoxW(hWnd, L"Please choose a user-writable folder outside Windows system directories.", L"Invalid folder", MB_ICONWARNING);
                return;
            }
            std::wstring fullPath = std::wstring(path) + L"\\NullWire";
            SetWindowTextW(g_hInstallPathEdit, fullPath.c_str());
        }
        CoTaskMemFree(pidl);
    }
}

void UpdateStepControls(HWND hWnd) {
    ShowWindow(g_hInstallPathEdit, SW_HIDE);
    ShowWindow(g_hBrowseBtn, SW_HIDE);
    ShowWindow(g_hDesktopCheck, SW_HIDE);
    ShowWindow(g_hStartMenuCheck, SW_HIDE);
    ShowWindow(g_hLaunchCheck, SW_HIDE);
    ShowWindow(g_hProgressBar, SW_HIDE);
    ShowWindow(g_hStatusText, SW_HIDE);

    switch (g_CurrentStep) {
        case WizardStep::WELCOME:
            EnableWindow(g_hBackBtn, FALSE);
            SetWindowTextW(g_hNextBtn, L"Next >");
            EnableWindow(g_hNextBtn, TRUE);
            EnableWindow(g_hCancelBtn, TRUE);
            break;

        case WizardStep::DESTINATION:
            EnableWindow(g_hBackBtn, TRUE);
            SetWindowTextW(g_hNextBtn, L"Install");
            EnableWindow(g_hNextBtn, TRUE);
            EnableWindow(g_hCancelBtn, TRUE);
            ShowWindow(g_hInstallPathEdit, SW_SHOW);
            ShowWindow(g_hBrowseBtn, SW_SHOW);
            ShowWindow(g_hDesktopCheck, SW_SHOW);
            ShowWindow(g_hStartMenuCheck, SW_SHOW);
            break;

        case WizardStep::INSTALLING:
            EnableWindow(g_hBackBtn, FALSE);
            EnableWindow(g_hNextBtn, FALSE);
            EnableWindow(g_hCancelBtn, FALSE);
            ShowWindow(g_hProgressBar, SW_SHOW);
            ShowWindow(g_hStatusText, SW_SHOW);
            break;

        case WizardStep::FINISHED:
            EnableWindow(g_hBackBtn, FALSE);
            SetWindowTextW(g_hNextBtn, L"Finish");
            EnableWindow(g_hNextBtn, TRUE);
            EnableWindow(g_hCancelBtn, FALSE);
            ShowWindow(g_hLaunchCheck, SW_SHOW);
            break;
    }

    InvalidateRect(hWnd, NULL, TRUE);
}

void DrawNullWireLogoBanner(Graphics& g, int x, int y, int w, int h) {
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
        { x + 40.0f, y + 370.0f, 5.0f },
        { x + 10.0f, y + 270.0f, 2.0f }
    };

    g.DrawLine(&penNet, nodes[0].nx, nodes[0].ny, nodes[1].nx, nodes[1].ny);
    g.DrawLine(&penNet, nodes[1].nx, nodes[1].ny, nodes[2].nx, nodes[2].ny);
    g.DrawLine(&penNet, nodes[2].nx, nodes[2].ny, nodes[3].nx, nodes[3].ny);
    g.DrawLine(&penNet, nodes[1].nx, nodes[1].ny, nodes[4].nx, nodes[4].ny);
    g.DrawLine(&penNet, nodes[4].nx, nodes[4].ny, nodes[5].nx, nodes[5].ny);
    g.DrawLine(&penNet, nodes[3].nx, nodes[3].ny, nodes[5].nx, nodes[5].ny);
    g.DrawLine(&penNet, nodes[4].nx, nodes[4].ny, nodes[6].nx, nodes[6].ny);
    g.DrawLine(&penNet, nodes[0].nx, nodes[0].ny, nodes[7].nx, nodes[7].ny);
    g.DrawLine(&penNet, nodes[7].nx, nodes[7].ny, nodes[1].nx, nodes[1].ny);

    for (const auto& nd : nodes) {
        g.FillEllipse(&brushGlow, nd.nx - nd.r - 2.0f, nd.ny - nd.r - 2.0f, (nd.r + 2.0f) * 2, (nd.r + 2.0f) * 2);
        g.FillEllipse(&brushNode, nd.nx - nd.r, nd.ny - nd.r, nd.r * 2, nd.r * 2);
    }

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

    Pen penInnerGlow(Color(255, 0, 210, 255), 2.0f);
    g.DrawArc(&penInnerGlow, logoCX - innerR - 2, logoCY - innerR - 2, (innerR + 2) * 2, (innerR + 2) * 2, 180, 180);
}

bool ExtractResourceToFile(HINSTANCE hInst, int resId, const std::wstring& outPath) {
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(resId), MAKEINTRESOURCEW(10));
    if (!hRes) return false;
    HGLOBAL hMem = LoadResource(hInst, hRes);
    if (!hMem) return false;
    DWORD size = SizeofResource(hInst, hRes);
    const void* pData = LockResource(hMem);
    if (!pData || size == 0) return false;

    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(hFile, pData, size, &written, NULL);
    CloseHandle(hFile);
    return written == size;
}

void StartInstallationThread(HWND hWnd) {
    wchar_t installPath[MAX_PATH];
    GetWindowTextW(g_hInstallPathEdit, installPath, MAX_PATH);
    std::wstring installDir = installPath;
    wchar_t canonical[MAX_PATH]{};
    if (GetFullPathNameW(installDir.c_str(), MAX_PATH, canonical, NULL)) {
        installDir = canonical;
        SetWindowTextW(g_hInstallPathEdit, installDir.c_str());
    }

    if (!IsSafeInstallDir(installDir)) {
        MessageBoxW(hWnd, L"The destination folder is not allowed. Choose a folder under your user profile, such as Local AppData\\NullWire.", L"Invalid destination", MB_ICONWARNING);
        return;
    }

    g_CurrentStep = WizardStep::INSTALLING;
    UpdateStepControls(hWnd);

    BOOL makeDesktop = (SendMessage(g_hDesktopCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL makeStartMenu = (SendMessage(g_hStartMenuCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);

    std::thread([hWnd, installDir, makeDesktop, makeStartMenu, hInst]() {
        auto uiStatus = [hWnd](const wchar_t* text, int pct) {
            SetWindowTextW(g_hStatusText, text);
            SendMessage(g_hProgressBar, PBM_SETPOS, pct, 0);
        };

        uiStatus(L"Creating installation directories...", 20);
        SHCreateDirectoryExW(NULL, installDir.c_str(), NULL);

        uiStatus(L"Extracting binaries and GUI Uninstaller...", 50);

        wchar_t currentExe[MAX_PATH];
        GetModuleFileNameW(NULL, currentExe, MAX_PATH);
        PathRemoveFileSpecW(currentExe);

        std::wstring destSender = installDir + L"\\NullWireSender.exe";
        std::wstring destUninst = installDir + L"\\NullWire_Uninstall.exe";

        // Try extracting Sender from disk first, then embedded resource
        std::wstring srcSender = std::wstring(currentExe) + L"\\NullWireSender.exe";
        if (!PathFileExistsW(srcSender.c_str())) srcSender = std::wstring(currentExe) + L"\\..\\sender\\NullWireSender.exe";
        if (!PathFileExistsW(srcSender.c_str())) srcSender = std::wstring(currentExe) + L"\\..\\NullWireSender.exe";

        bool senderOk = false;
        if (PathFileExistsW(srcSender.c_str()) && CopyFileW(srcSender.c_str(), destSender.c_str(), FALSE)) {
            senderOk = true;
        } else {
            senderOk = ExtractResourceToFile(hInst, 101, destSender);
        }

        if (!senderOk || !PathFileExistsW(destSender.c_str())) {
            PostMessage(hWnd, WM_USER + 102, 0, 0);
            return;
        }

        // Try extracting Uninstaller from disk first, then embedded resource
        std::wstring srcUninst = std::wstring(currentExe) + L"\\NullWire_Uninstall.exe";
        if (!PathFileExistsW(srcUninst.c_str())) srcUninst = std::wstring(currentExe) + L"\\..\\installer\\NullWire_Uninstall.exe";
        if (!PathFileExistsW(srcUninst.c_str())) srcUninst = std::wstring(currentExe) + L"\\..\\NullWire_Uninstall.exe";

        if (PathFileExistsW(srcUninst.c_str())) {
            CopyFileW(srcUninst.c_str(), destUninst.c_str(), FALSE);
        } else {
            ExtractResourceToFile(hInst, 102, destUninst);
        }

        std::wstring uninstallerPath = destUninst;

        uiStatus(L"Creating desktop and start menu shortcuts...", 75);

        if (makeDesktop) {
            wchar_t deskPath[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, deskPath))) {
                std::wstring sc = std::wstring(deskPath) + L"\\NullWire Pro.lnk";
                CreateShortcut(destSender, sc, L"NullWire Pro - Ultra-Low Latency Lossless Audio");
            }
        }

        if (makeStartMenu) {
            wchar_t startPath[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, startPath))) {
                std::wstring sc = std::wstring(startPath) + L"\\NullWire Pro.lnk";
                CreateShortcut(destSender, sc, L"NullWire Pro - Ultra-Low Latency Lossless Audio");
            }
        }

        uiStatus(L"Registering with Windows Installed Apps...", 90);
        RegisterInWindowsAddRemovePrograms(installDir, destSender, uninstallerPath);

        uiStatus(L"Completed.", 100);
        g_CurrentStep = WizardStep::FINISHED;
        PostMessage(hWnd, WM_USER + 101, 0, 0);
    }).detach();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hFontTitle = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontBold = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontNormal = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hFontSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            InitCommonControls();

            g_hInstallPathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", GetDefaultInstallPath().c_str(), WS_CHILD | ES_AUTOHSCROLL, 195, 120, 245, 24, hWnd, (HMENU)101, NULL, NULL);
            SendMessage(g_hInstallPathEdit, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            g_hBrowseBtn = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | BS_PUSHBUTTON, 450, 119, 75, 26, hWnd, (HMENU)102, NULL, NULL);
            SendMessage(g_hBrowseBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            g_hDesktopCheck = CreateWindowW(L"BUTTON", L"Create a &desktop shortcut", WS_CHILD | BS_AUTOCHECKBOX, 195, 165, 300, 20, hWnd, (HMENU)201, NULL, NULL);
            SendMessage(g_hDesktopCheck, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            SendMessage(g_hDesktopCheck, BM_SETCHECK, BST_CHECKED, 0);

            g_hStartMenuCheck = CreateWindowW(L"BUTTON", L"Create a &Start Menu shortcut", WS_CHILD | BS_AUTOCHECKBOX, 195, 192, 300, 20, hWnd, (HMENU)202, NULL, NULL);
            SendMessage(g_hStartMenuCheck, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            SendMessage(g_hStartMenuCheck, BM_SETCHECK, BST_CHECKED, 0);

            g_hProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | PBS_SMOOTH, 195, 140, 330, 18, hWnd, (HMENU)301, NULL, NULL);
            SendMessage(g_hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

            g_hStatusText = CreateWindowW(L"STATIC", L"Extracting files...", WS_CHILD, 195, 170, 330, 20, hWnd, (HMENU)302, NULL, NULL);
            SendMessage(g_hStatusText, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

            g_hLaunchCheck = CreateWindowW(L"BUTTON", L"Launch NullWire Pro", WS_CHILD | BS_AUTOCHECKBOX, 195, 215, 300, 22, hWnd, (HMENU)203, NULL, NULL);
            SendMessage(g_hLaunchCheck, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            SendMessage(g_hLaunchCheck, BM_SETCHECK, BST_CHECKED, 0);

            g_hBackBtn = CreateWindowW(L"BUTTON", L"< Back", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 280, 322, 75, 24, hWnd, (HMENU)401, NULL, NULL);
            SendMessage(g_hBackBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            g_hNextBtn = CreateWindowW(L"BUTTON", L"Next >", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 362, 322, 75, 24, hWnd, (HMENU)402, NULL, NULL);
            SendMessage(g_hNextBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            g_hCancelBtn = CreateWindowW(L"BUTTON", L"Cancel", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 452, 322, 75, 24, hWnd, (HMENU)403, NULL, NULL);
            SendMessage(g_hCancelBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            UpdateStepControls(hWnd);
            break;
        }

        case WM_USER + 101:
            UpdateStepControls(hWnd);
            break;

        case WM_USER + 102:
            g_CurrentStep = WizardStep::DESTINATION;
            UpdateStepControls(hWnd);
            MessageBoxW(hWnd, L"Setup could not copy NullWireSender.exe. Build the sender first and keep it next to the installer.", L"Install failed", MB_ICONERROR);
            break;

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, COLOR_TEXT_MAIN);
            SetBkColor(hdc, COLOR_WHITE);
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }

        case WM_CTLCOLORBTN: {
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
            DrawNullWireLogoBanner(graphics, 0, 0, bannerW, contentH);

            SetBkMode(hdc, TRANSPARENT);

            switch (g_CurrentStep) {
                case WizardStep::WELCOME: {
                    SelectObject(hdc, g_hFontTitle);
                    SetTextColor(hdc, COLOR_TEXT_MAIN);
                    RECT rcT = { 195, 28, totalW - 20, 80 };
                    DrawTextW(hdc, L"Welcome to the NullWire Pro\r\nSetup Wizard", -1, &rcT, DT_WORDBREAK);

                    SelectObject(hdc, g_hFontNormal);
                    SetTextColor(hdc, COLOR_TEXT_SUB);
                    RECT rcD = { 195, 100, totalW - 20, contentH - 20 };
                    DrawTextW(hdc, L"This will install NullWire Pro v2.0 on your computer.\r\n\r\nNullWire Pro delivers true lossless, bit-perfect, ultra-low latency audio streaming over 5GHz Wi-Fi with dedicated gaming and music scenario profiles.\r\n\r\nClick Next to continue, or Cancel to exit Setup.", -1, &rcD, DT_WORDBREAK);
                    break;
                }

                case WizardStep::DESTINATION: {
                    SelectObject(hdc, g_hFontTitle);
                    SetTextColor(hdc, COLOR_TEXT_MAIN);
                    TextOutW(hdc, 195, 28, L"Select Destination Location", 27);

                    SelectObject(hdc, g_hFontNormal);
                    SetTextColor(hdc, COLOR_TEXT_SUB);
                    RECT rcD = { 195, 62, totalW - 20, 115 };
                    DrawTextW(hdc, L"Where should NullWire Pro be installed?\r\nTo continue, click Install. If you would like to select a different folder, click Browse.", -1, &rcD, DT_WORDBREAK);

                    RECT rcSpace = { 195, 230, totalW - 20, 260 };
                    SelectObject(hdc, g_hFontSmall);
                    DrawTextW(hdc, L"At least 15 MB of free disk space is required.", -1, &rcSpace, DT_SINGLELINE);
                    break;
                }

                case WizardStep::INSTALLING: {
                    SelectObject(hdc, g_hFontTitle);
                    SetTextColor(hdc, COLOR_TEXT_MAIN);
                    TextOutW(hdc, 195, 28, L"Installing NullWire Pro", 23);

                    SelectObject(hdc, g_hFontNormal);
                    SetTextColor(hdc, COLOR_TEXT_SUB);
                    TextOutW(hdc, 195, 65, L"Please wait while Setup installs NullWire Pro on your computer.", 63);
                    break;
                }

                case WizardStep::FINISHED: {
                    SelectObject(hdc, g_hFontTitle);
                    SetTextColor(hdc, COLOR_TEXT_MAIN);
                    RECT rcT = { 195, 28, totalW - 20, 90 };
                    DrawTextW(hdc, L"Completing the NullWire Pro\r\nSetup Wizard", -1, &rcT, DT_WORDBREAK);

                    SelectObject(hdc, g_hFontNormal);
                    SetTextColor(hdc, COLOR_TEXT_SUB);
                    TextOutW(hdc, 195, 120, L"Click the \"Finish\" button to exit the Setup Wizard.", 51);
                    break;
                }
            }

            EndPaint(hWnd, &ps);
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);

            if (wmId == 102) {
                BrowseFolder(hWnd);
            } else if (wmId == 401) {
                if (g_CurrentStep == WizardStep::DESTINATION) {
                    g_CurrentStep = WizardStep::WELCOME;
                    UpdateStepControls(hWnd);
                }
            } else if (wmId == 402) {
                if (g_CurrentStep == WizardStep::WELCOME) {
                    g_CurrentStep = WizardStep::DESTINATION;
                    UpdateStepControls(hWnd);
                } else if (g_CurrentStep == WizardStep::DESTINATION) {
                    StartInstallationThread(hWnd);
                } else if (g_CurrentStep == WizardStep::FINISHED) {
                    BOOL shouldLaunch = (SendMessage(g_hLaunchCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    if (shouldLaunch) {
                        wchar_t installPath[MAX_PATH];
                        GetWindowTextW(g_hInstallPathEdit, installPath, MAX_PATH);
                        std::wstring destSender = std::wstring(installPath) + L"\\NullWireSender.exe";
                        if (IsSafeInstallDir(installPath) && PathFileExistsW(destSender.c_str())) {
                            ShellExecuteW(NULL, L"open", destSender.c_str(), NULL, installPath, SW_SHOW);
                        }
                    }
                    PostMessageW(hWnd, WM_CLOSE, 0, 0);
                }
            } else if (wmId == 403) {
                if (MessageBoxW(hWnd, L"Are you sure you want to exit NullWire Pro Setup?", L"Exit Setup", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    PostMessageW(hWnd, WM_CLOSE, 0, 0);
                }
            }
            break;
        }

        case WM_DESTROY: {
            if (g_hFontTitle) DeleteObject(g_hFontTitle);
            if (g_hFontBold) DeleteObject(g_hFontBold);
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
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
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
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NullWireModernWizardClass";

    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"NullWireModernWizardClass",
        L"Setup - NullWire Pro",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
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
