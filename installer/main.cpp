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
#include <vector>
#include <atomic>
#include <mutex>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uuid.lib")

using namespace Gdiplus;

// ── Windows 11 OOBE Light Palette ────────────────────────────────────────────
#define COLOR_WIN11_BG         RGB(255, 255, 255)
#define COLOR_WIN11_SURFACE    RGB(249, 250, 251)
#define COLOR_WIN11_BLUE       RGB(0, 103, 192)
#define COLOR_WIN11_BLUE_HOVER RGB(24, 115, 196)
#define COLOR_WIN11_TEXT_TITLE RGB(27, 27, 27)
#define COLOR_WIN11_TEXT_SUB   RGB(94, 94, 94)
#define COLOR_WIN11_BORDER     RGB(229, 231, 235)

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

// Options state
static bool g_optDriver = true;
static bool g_optDesktop = true;
static bool g_optStartMenu = true;
static bool g_optLaunch = true;

// Hover & UI State
static bool g_isNextHovered = false;
static bool g_isCancelHovered = false;
static bool g_isBackHovered = false;
static bool g_isBrowseHovered = false;

static RECT g_rcNextBtn { 670, 448, 765, 484 };
static RECT g_rcCancelBtn { 580, 448, 655, 484 };
static RECT g_rcBackBtn { 495, 448, 565, 484 };
static RECT g_rcBrowseBtn { 675, 175, 755, 207 };

static RECT g_rcOptDriver { 340, 222, 755, 246 };
static RECT g_rcOptDesktop { 340, 254, 755, 278 };
static RECT g_rcOptStartMenu { 340, 286, 755, 310 };
static RECT g_rcOptLaunch { 340, 220, 755, 248 };

static HFONT g_hFontNormal = NULL;
static HBRUSH g_hBrushWhite = NULL;

static std::atomic<int> g_InstallProgress{0};
static std::wstring g_InstallStatusText = L"Initializing NullWire Pro Setup Engine...";
static std::vector<std::pair<std::wstring, bool>> g_InstallSteps;
static std::mutex g_InstallMutex;

std::wstring GetDefaultInstallPath() {
    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        return std::wstring(localAppData) + L"\\NullWire";
    }
    return L"C:\\NullWire";
}

bool IsSafeInstallDir(const std::wstring& path) {
    if (path.size() < 6 || path.size() >= MAX_PATH) return false;
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
        const wchar_t* displayName = L"NullWire Pro - Ultra-Low Latency Audio Suite";
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

void BrowseFolder(HWND hWnd) {
    BROWSEINFOW bi{};
    bi.hwndOwner = hWnd;
    bi.lpszTitle = L"Select NullWire Pro Destination:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            if (!IsSafeInstallDir(std::wstring(path) + L"\\NullWire") && !IsSafeInstallDir(path)) {
                CoTaskMemFree(pidl);
                MessageBoxW(hWnd, L"Please select a user-writable folder.", L"Invalid Destination", MB_ICONWARNING);
                return;
            }
            std::wstring fullPath = std::wstring(path) + L"\\NullWire";
            SetWindowTextW(g_hInstallPathEdit, fullPath.c_str());
        }
        CoTaskMemFree(pidl);
    }
}

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




// ── Windows 11 Custom Checkbox Component ──────────────────────────────────────
void DrawWindows11Checkbox(Graphics& g, int x, int y, bool isChecked, const wchar_t* label, bool isBold = false) {
    RectF boxRect(static_cast<REAL>(x), static_cast<REAL>(y), 18.0f, 18.0f);
    GraphicsPath bPath;
    AddRoundedRect(bPath, boxRect, 4.0f);

    if (isChecked) {
        SolidBrush blueBrush(Color(255, 0, 103, 192));
        g.FillPath(&blueBrush, &bPath);

        // Checkmark
        Pen checkPen(Color(255, 255, 255, 255), 2.0f);
        checkPen.SetStartCap(LineCapRound);
        checkPen.SetEndCap(LineCapRound);
        PointF p1(x + 4.0f, y + 9.0f);
        PointF p2(x + 7.5f, y + 13.0f);
        PointF p3(x + 14.0f, y + 5.5f);
        g.DrawLine(&checkPen, p1, p2);
        g.DrawLine(&checkPen, p2, p3);
    } else {
        SolidBrush whiteBrush(Color(255, 255, 255, 255));
        g.FillPath(&whiteBrush, &bPath);
        Pen borderPen(Color(255, 140, 140, 140), 1.5f);
        g.DrawPath(&borderPen, &bPath);
    }

    FontFamily ff(L"Segoe UI");
    Gdiplus::Font textFont(&ff, 12, isBold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush textBrush(Color(255, 27, 27, 27));
    g.DrawString(label, -1, &textFont, PointF(static_cast<REAL>(x + 28), static_cast<REAL>(y + 1)), &textBrush);
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
        MessageBoxW(hWnd, L"The destination folder is not allowed. Please choose a valid directory.", L"Invalid Destination", MB_ICONWARNING);
        return;
    }

    g_CurrentStep = WizardStep::INSTALLING;
    ShowWindow(g_hInstallPathEdit, SW_HIDE);
    ShowWindow(g_hBrowseBtn, SW_HIDE);
    InvalidateRect(hWnd, NULL, TRUE);

    BOOL makeDesktop = g_optDesktop;
    BOOL makeStartMenu = g_optStartMenu;
    BOOL installDriver = g_optDriver;
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);

    {
        std::lock_guard<std::mutex> lock(g_InstallMutex);
        g_InstallSteps.clear();
        g_InstallSteps.push_back({ L"Preparing destination folder...", false });
        g_InstallSteps.push_back({ L"Deploying 64-bit Core Engine...", false });
        g_InstallSteps.push_back({ L"Extracting Virtual Audio Driver Suite...", false });
        if (installDriver) {
            g_InstallSteps.push_back({ L"Installing Virtual Audio Driver into Windows PnP...", false });
            g_InstallSteps.push_back({ L"Scanning & activating audio endpoints...", false });
        }
        g_InstallSteps.push_back({ L"Configuring system shortcuts...", false });
        g_InstallSteps.push_back({ L"Registering Windows setup manifest...", false });
    }

    std::thread([hWnd, installDir, makeDesktop, makeStartMenu, installDriver, hInst]() {
        auto setStatus = [hWnd](const std::wstring& text, int pct, int stepIndex) {
            {
                std::lock_guard<std::mutex> lock(g_InstallMutex);
                g_InstallStatusText = text;
                g_InstallProgress.store(pct);
                if (stepIndex >= 0 && stepIndex < (int)g_InstallSteps.size()) {
                    g_InstallSteps[stepIndex].second = true;
                }
            }
            InvalidateRect(hWnd, NULL, FALSE);
        };

        // Step 0: Create Folders
        setStatus(L"Creating system directories...", 15, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        SHCreateDirectoryExW(NULL, installDir.c_str(), NULL);
        std::wstring driverDir = installDir + L"\\driver";
        SHCreateDirectoryExW(NULL, driverDir.c_str(), NULL);

        // Step 1: Copy Binaries
        setStatus(L"Deploying NullWire Pro Core Engine...", 30, 1);
        wchar_t currentExe[MAX_PATH];
        GetModuleFileNameW(NULL, currentExe, MAX_PATH);
        PathRemoveFileSpecW(currentExe);

        std::wstring destSender = installDir + L"\\NullWire.exe";
        std::wstring destUninst = installDir + L"\\Uninstall.exe";

        std::wstring srcSender = std::wstring(currentExe) + L"\\NullWire.exe";
        if (!PathFileExistsW(srcSender.c_str())) srcSender = std::wstring(currentExe) + L"\\NullWireSender.exe";
        if (!PathFileExistsW(srcSender.c_str())) srcSender = std::wstring(currentExe) + L"\\..\\sender\\NullWire.exe";
        if (!PathFileExistsW(srcSender.c_str())) srcSender = std::wstring(currentExe) + L"\\..\\sender\\NullWireSender.exe";
        if (!PathFileExistsW(srcSender.c_str())) srcSender = std::wstring(currentExe) + L"\\..\\NullWire.exe";
        if (!PathFileExistsW(srcSender.c_str())) srcSender = std::wstring(currentExe) + L"\\..\\NullWireSender.exe";

        if (PathFileExistsW(srcSender.c_str())) {
            CopyFileW(srcSender.c_str(), destSender.c_str(), FALSE);
        } else {
            ExtractResourceToFile(hInst, 101, destSender);
        }

        std::wstring srcUninst = std::wstring(currentExe) + L"\\Uninstall.exe";
        if (!PathFileExistsW(srcUninst.c_str())) srcUninst = std::wstring(currentExe) + L"\\NullWire_Uninstall.exe";
        if (!PathFileExistsW(srcUninst.c_str())) srcUninst = std::wstring(currentExe) + L"\\..\\installer\\Uninstall.exe";
        if (!PathFileExistsW(srcUninst.c_str())) srcUninst = std::wstring(currentExe) + L"\\..\\installer\\NullWire_Uninstall.exe";
        if (!PathFileExistsW(srcUninst.c_str())) srcUninst = std::wstring(currentExe) + L"\\..\\Uninstall.exe";
        if (!PathFileExistsW(srcUninst.c_str())) srcUninst = std::wstring(currentExe) + L"\\..\\NullWire_Uninstall.exe";

        if (PathFileExistsW(srcUninst.c_str())) {
            CopyFileW(srcUninst.c_str(), destUninst.c_str(), FALSE);
        } else {
            ExtractResourceToFile(hInst, 102, destUninst);
        }

        // Step 2: Deploy Driver Files
        setStatus(L"Extracting Certified Virtual Audio Cable Driver Suite...", 50, 2);
        std::wstring destVbCable = driverDir + L"\\vbcable";
        SHCreateDirectoryExW(NULL, destVbCable.c_str(), NULL);

        std::wstring destSetupExe = destVbCable + L"\\VBCABLE_Setup_x64.exe";
        std::wstring destSys = destVbCable + L"\\vbaudio_cable64_win7.sys";
        std::wstring destCat = destVbCable + L"\\vbaudio_cable64_win7.cat";
        std::wstring destInf = destVbCable + L"\\vbMmeCable64_win7.inf";

        std::wstring srcDriverDir = std::wstring(currentExe) + L"\\driver";
        if (!PathFileExistsW(srcDriverDir.c_str())) srcDriverDir = std::wstring(currentExe) + L"\\..\\driver";
        std::wstring srcVbCable = srcDriverDir + L"\\vbcable";

        if (PathFileExistsW((srcVbCable + L"\\VBCABLE_Setup_x64.exe").c_str())) {
            CopyFileW((srcVbCable + L"\\VBCABLE_Setup_x64.exe").c_str(), destSetupExe.c_str(), FALSE);
            CopyFileW((srcVbCable + L"\\vbaudio_cable64_win7.sys").c_str(), destSys.c_str(), FALSE);
            CopyFileW((srcVbCable + L"\\vbaudio_cable64_win7.cat").c_str(), destCat.c_str(), FALSE);
            CopyFileW((srcVbCable + L"\\vbMmeCable64_win7.inf").c_str(), destInf.c_str(), FALSE);
        } else {
            ExtractResourceToFile(hInst, 103, destSetupExe);
            ExtractResourceToFile(hInst, 104, destSys);
            ExtractResourceToFile(hInst, 105, destCat);
            ExtractResourceToFile(hInst, 106, destInf);
        }

        int curStepIdx = 3;

        // Step 3 & 4: Install Certified Virtual Audio Cable Driver
        if (installDriver) {
            setStatus(L"Installing Microsoft-Certified Virtual Audio Cable...", 70, curStepIdx++);
            if (PathFileExistsW(destSetupExe.c_str())) {
                ExecuteHiddenCommandWithDir(L"\"" + destSetupExe + L"\" -i -h", destVbCable);
            }

            setStatus(L"Activating Virtual Audio Cable Endpoint...", 80, curStepIdx++);
            ExecuteHiddenCommandWithDir(L"pnputil.exe /scan-devices");
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }

        // Step 5: Shortcuts
        setStatus(L"Configuring desktop & start menu shortcuts...", 90, curStepIdx++);
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

        // Step 6: Registration
        setStatus(L"Registering setup manifest...", 100, curStepIdx++);
        RegisterInWindowsAddRemovePrograms(installDir, destSender, destUninst);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        PostMessageW(hWnd, WM_APP + 1, 0, 0);
    }).detach();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hFontNormal = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hBrushWhite = CreateSolidBrush(COLOR_WIN11_BG);

            InitCommonControls();

            // Destination Location Controls
            std::wstring defPath = GetDefaultInstallPath();
            g_hInstallPathEdit = CreateWindowExW(0, L"EDIT", defPath.c_str(), WS_CHILD | ES_AUTOHSCROLL, 342, 177, 320, 26, hWnd, (HMENU)101, NULL, NULL);
            SendMessage(g_hInstallPathEdit, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            break;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, COLOR_WIN11_BG);
            SetTextColor(hdc, COLOR_WIN11_TEXT_TITLE);
            return (LRESULT)g_hBrushWhite;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool prevNext = g_isNextHovered;
            bool prevCancel = g_isCancelHovered;
            bool prevBack = g_isBackHovered;
            bool prevBrowse = g_isBrowseHovered;

            g_isNextHovered = PtInRect(&g_rcNextBtn, pt);
            g_isCancelHovered = PtInRect(&g_rcCancelBtn, pt);
            g_isBackHovered = PtInRect(&g_rcBackBtn, pt);
            g_isBrowseHovered = (g_CurrentStep == WizardStep::DESTINATION) && PtInRect(&g_rcBrowseBtn, pt);

            if (prevNext != g_isNextHovered || prevCancel != g_isCancelHovered || prevBack != g_isBackHovered || prevBrowse != g_isBrowseHovered) {
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

            // Primary Next / Install / Finish Button
            if (PtInRect(&g_rcNextBtn, pt)) {
                if (g_CurrentStep == WizardStep::WELCOME) {
                    g_CurrentStep = WizardStep::DESTINATION;
                    ShowWindow(g_hInstallPathEdit, SW_SHOW);
                    InvalidateRect(hWnd, NULL, TRUE);
                } else if (g_CurrentStep == WizardStep::DESTINATION) {
                    StartInstallationThread(hWnd);
                } else if (g_CurrentStep == WizardStep::FINISHED) {
                    if (g_optLaunch) {
                        wchar_t installPath[MAX_PATH];
                        GetWindowTextW(g_hInstallPathEdit, installPath, MAX_PATH);
                        std::wstring destSender = std::wstring(installPath) + L"\\NullWire.exe";
                        if (!PathFileExistsW(destSender.c_str())) destSender = std::wstring(installPath) + L"\\NullWireSender.exe";
                        if (IsSafeInstallDir(installPath) && PathFileExistsW(destSender.c_str())) {
                            ShellExecuteW(NULL, L"open", destSender.c_str(), NULL, installPath, SW_SHOW);
                        }
                    }
                    PostMessageW(hWnd, WM_CLOSE, 0, 0);
                }
                return 0;
            }

            // Cancel Button
            if (PtInRect(&g_rcCancelBtn, pt)) {
                if (g_CurrentStep != WizardStep::INSTALLING) {
                    if (MessageBoxW(hWnd, L"Are you sure you want to exit NullWire Pro setup?", L"Exit Setup", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        PostMessageW(hWnd, WM_CLOSE, 0, 0);
                    }
                }
                return 0;
            }

            // Back Button
            if (g_CurrentStep == WizardStep::DESTINATION && PtInRect(&g_rcBackBtn, pt)) {
                g_CurrentStep = WizardStep::WELCOME;
                ShowWindow(g_hInstallPathEdit, SW_HIDE);
                InvalidateRect(hWnd, NULL, TRUE);
                return 0;
            }

            // Browse Button
            if (g_CurrentStep == WizardStep::DESTINATION && PtInRect(&g_rcBrowseBtn, pt)) {
                BrowseFolder(hWnd);
                return 0;
            }

            // Checkbox Clicks on Destination Step
            if (g_CurrentStep == WizardStep::DESTINATION) {
                if (PtInRect(&g_rcOptDriver, pt)) { g_optDriver = !g_optDriver; InvalidateRect(hWnd, NULL, FALSE); return 0; }
                if (PtInRect(&g_rcOptDesktop, pt)) { g_optDesktop = !g_optDesktop; InvalidateRect(hWnd, NULL, FALSE); return 0; }
                if (PtInRect(&g_rcOptStartMenu, pt)) { g_optStartMenu = !g_optStartMenu; InvalidateRect(hWnd, NULL, FALSE); return 0; }
            }

            // Checkbox Click on Finished Step
            if (g_CurrentStep == WizardStep::FINISHED) {
                if (PtInRect(&g_rcOptLaunch, pt)) { g_optLaunch = !g_optLaunch; InvalidateRect(hWnd, NULL, FALSE); return 0; }
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

            // 3. Right Column Layout (X = 340 to 760)
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

            // Dynamic Step Content
            switch (g_CurrentStep) {
                case WizardStep::WELCOME: {
                    DrawOfficialNullWireLogo(g, 355.0f, 52.0f, 13.0f);
                    g.DrawString(L"Set up NullWire Pro", -1, &fontHero, PointF(376.0f, 40.0f), &textTitleBrush);
                    g.DrawString(L"Lossless, bit-perfect, ultra-low latency Wi-Fi audio streaming directly to your Android device.", -1, &fontSub, RectF(340.0f, 75.0f, 420.0f, 40.0f), NULL, &textSubBrush);

                    // Requirement-style Feature List (matching the PIN requirements in screenshot)
                    g.DrawString(L"Audio streaming capabilities:", -1, &fontSecTitle, PointF(340.0f, 135.0f), &textTitleBrush);

                    const wchar_t* specs[] = {
                        L"100% Lossless Bit-Perfect 48kHz / 16-bit Master PCM",
                        L"Sub-2.67ms Hardware DMA Ultra-Low Latency Link",
                        L"Dedicated Microsoft-Certified Virtual Audio Cable Driver",
                        L"Real-Time Acoustic DSP Equalizer & Spatial 3D Soundstage",
                        L"Wi-Fi FastPath MMCSS Real-Time Scheduling Engine",
                        L"Instant Zero-Touch Android Discovery & Auto-Connect"
                    };

                    float specY = 162.0f;
                    for (const auto& sp : specs) {
                        // Clean Windows 11 Bullet Dot
                        SolidBrush dotBrush(Color(255, 0, 103, 192));
                        g.FillEllipse(&dotBrush, 342.0f, specY + 4.0f, 5.0f, 5.0f);
                        g.DrawString(sp, -1, &fontBullet, PointF(355.0f, specY), &textBodyBrush);
                        specY += 22.0f;
                    }
                    break;
                }

                case WizardStep::DESTINATION: {
                    DrawOfficialNullWireLogo(g, 355.0f, 52.0f, 13.0f);
                    g.DrawString(L"Installation Options", -1, &fontHero, PointF(376.0f, 40.0f), &textTitleBrush);
                    g.DrawString(L"Choose your installation directory and configure system integration components.", -1, &fontSub, RectF(340.0f, 75.0f, 420.0f, 40.0f), NULL, &textSubBrush);

                    g.DrawString(L"Install destination folder:", -1, &fontSecTitle, PointF(340.0f, 150.0f), &textTitleBrush);

                    // Destination Edit Box Outline
                    RectF editBorderRect(340.0f, 174.0f, 324.0f, 32.0f);
                    GraphicsPath ebPath;
                    AddRoundedRect(ebPath, editBorderRect, 4.0f);
                    Pen ebPen(Color(255, 209, 213, 219), 1.0f);
                    g.DrawPath(&ebPen, &ebPath);

                    // Browse Button
                    RECT rcBr = { 675, 174, 755, 206 };
                    DrawWindows11Button(g, rcBr, L"Browse...", false, g_isBrowseHovered);

                    // Checkboxes
                    DrawWindows11Checkbox(g, 340, 225, g_optDriver, L"Install Microsoft-Certified Virtual Audio Cable Driver", true);
                    DrawWindows11Checkbox(g, 340, 257, g_optDesktop, L"Create desktop shortcut");
                    DrawWindows11Checkbox(g, 340, 289, g_optStartMenu, L"Add shortcut to Start Menu");
                    break;
                }

                case WizardStep::INSTALLING: {
                    DrawOfficialNullWireLogo(g, 355.0f, 52.0f, 13.0f);
                    g.DrawString(L"Installing NullWire Pro", -1, &fontHero, PointF(376.0f, 40.0f), &textTitleBrush);
                    g.DrawString(L"Deploying low-latency drivers and configuring real-time audio pipeline...", -1, &fontSub, RectF(340.0f, 75.0f, 420.0f, 40.0f), NULL, &textSubBrush);

                    int curPct = g_InstallProgress.load();
                    std::wstring statusMsg;
                    {
                        std::lock_guard<std::mutex> lock(g_InstallMutex);
                        statusMsg = g_InstallStatusText;
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

                case WizardStep::FINISHED: {
                    DrawOfficialNullWireLogo(g, 355.0f, 52.0f, 13.0f);
                    g.DrawString(L"Setup Completed", -1, &fontHero, PointF(376.0f, 40.0f), &textTitleBrush);
                    g.DrawString(L"NullWire Pro and the Virtual Audio Cable driver are ready for high-fidelity streaming.", -1, &fontSub, RectF(340.0f, 75.0f, 420.0f, 40.0f), NULL, &textSubBrush);

                    // Success Pill
                    RectF succRect(340.0f, 145.0f, 415.0f, 52.0f);
                    GraphicsPath succPath;
                    AddRoundedRect(succPath, succRect, 8.0f);
                    SolidBrush succBg(Color(255, 240, 253, 244));
                    g.FillPath(&succBg, &succPath);
                    Pen succBorder(Color(255, 187, 247, 208), 1.0f);
                    g.DrawPath(&succBorder, &succPath);

                    SolidBrush succText(Color(255, 22, 101, 52));
                    g.DrawString(L"✓  Ready for 48kHz / 16-bit Master PCM Streaming", -1, &fontSecTitle, PointF(356.0f, 155.0f), &succText);
                    SolidBrush succSubText(Color(255, 21, 128, 61));
                    g.DrawString(L"Launch NullWire on PC and Android to begin instantaneous audio link.", -1, &fontBullet, PointF(356.0f, 172.0f), &succSubText);

                    DrawWindows11Checkbox(g, 340, 225, g_optLaunch, L"Launch NullWire Pro now", true);
                    break;
                }
            }

            // Bottom Action Buttons
            if (g_CurrentStep == WizardStep::DESTINATION) {
                DrawWindows11Button(g, g_rcBackBtn, L"Back", false, g_isBackHovered);
            }

            if (g_CurrentStep != WizardStep::INSTALLING) {
                DrawWindows11Button(g, g_rcCancelBtn, L"Cancel", false, g_isCancelHovered);
            }

            std::wstring nextText = (g_CurrentStep == WizardStep::FINISHED) ? L"OK" : ((g_CurrentStep == WizardStep::DESTINATION) ? L"Install" : L"Next");
            DrawWindows11Button(g, g_rcNextBtn, nextText.c_str(), true, g_isNextHovered, g_CurrentStep != WizardStep::INSTALLING);

            BitBlt(hdc, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
            SelectObject(hdcMem, hbmOld);
            DeleteObject(hbmMem);
            DeleteDC(hdcMem);

            EndPaint(hWnd, &ps);
            break;
        }

        case WM_APP + 1:
            g_CurrentStep = WizardStep::FINISHED;
            InvalidateRect(hWnd, NULL, TRUE);
            break;

        case WM_DESTROY:
            if (g_hFontNormal) DeleteObject(g_hFontNormal);
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
    wc.lpszClassName = L"NullWireProSetupWizard";

    RegisterClassExW(&wc);

    int windowW = 800;
    int windowH = 530;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - windowW) / 2;
    int posY = (screenH - windowH) / 2;

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"NullWireProSetupWizard",
        L"NullWire Pro · Setup",
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
