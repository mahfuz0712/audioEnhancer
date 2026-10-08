// setup.cpp - Audio Enhancer installer (single exe, needs admin - see setup.manifest)
//
//   AudioEnhancer-Setup.exe              -> install
//   uninstall.exe /uninstall             -> (copy of this exe placed in the install folder)
//
// What it does:
//   1. Copies the embedded app files to  C:\Program Files\Audio Enhancer
//   2. Creates Start Menu + Desktop shortcuts and an "Apps & features" uninstall entry
//   3. If no virtual audio cable is present: downloads the official VB-Cable driver pack
//      from vb-audio.com, runs its silent installer, and offers a reboot.
//
// NOTE: Windows always shows its own "Install this device software?" prompt for the
//       driver. That prompt cannot be skipped by any installer - click "Install" once.

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <urlmon.h>

#include <string>
#include <thread>
#include <vector>

#include "audio_engine.h"  // enumerateOutputDevices() - used to detect an installed cable

// Official download (VB-Audio, driver pack 45). Update this if VB-Audio publishes a newer pack.
static const wchar_t* kCableUrl = L"https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip";
static const wchar_t* kAppName = L"Audio Enhancer";
static const wchar_t* kAppVersion = L"1.0.0";
static const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AudioEnhancer";

enum { RES_EXE = 101, RES_LOADER, RES_HTML, RES_CSS, RES_JS, RES_ICON };
enum { WM_STATUS = WM_APP + 1, WM_FINISHED = WM_APP + 2 };

static HWND g_wnd = nullptr, g_label = nullptr, g_bar = nullptr;
static std::thread g_worker;
static bool g_finished = false, g_ok = false, g_needReboot = false;
static std::wstring g_error, g_warning;

// ------------------------------------------------------------------ helpers
static std::wstring programFilesDir() {
    wchar_t b[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, b);
    return b;
}

static std::wstring installDir() { return programFilesDir() + L"\\" + kAppName; }

static std::wstring specialDir(int csidl) {
    wchar_t b[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, csidl, nullptr, 0, b);
    return b;
}

static std::wstring selfPath() {
    wchar_t b[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    return b;
}

static void makeDirs(const std::wstring& path) {
    for (size_t i = 3; i < path.size(); ++i)  // skip "C:\"
        if (path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    CreateDirectoryW(path.c_str(), nullptr);
}

static void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring full = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(full);
            else { SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(full.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static std::wstring findFile(const std::wstring& dir, const std::wstring& name) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring full = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) found = findFile(full, name);
        else if (_wcsicmp(n.c_str(), name.c_str()) == 0) found = full;
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static bool writeResource(int id, const std::wstring& path) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    const DWORD size = SizeofResource(nullptr, r);
    const void* data = LockResource(g);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

// Runs a command line and waits. Returns the exit code, (DWORD)-1 if it could not start,
// (DWORD)-2 on timeout.
static DWORD runProcess(const std::wstring& cmdline, const std::wstring& workDir, bool hidden, DWORD timeoutMs) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring cl = cmdline;  // CreateProcessW needs a writable buffer
    if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE, hidden ? CREATE_NO_WINDOW : 0, nullptr,
                        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi))
        return (DWORD)-1;
    DWORD code = (DWORD)-2;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static bool createShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& workDir) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl)))
        return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(workDir.c_str());
    sl->SetDescription(kAppName);
    sl->SetIconLocation(target.c_str(), 0);
    bool ok = false;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
        pf->Release();
    }
    sl->Release();
    return ok;
}

static void regSetStr(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

static bool cableInstalled() {
    for (const auto& d : enumerateOutputDevices())
        if (d.name.find(L"CABLE") != std::wstring::npos || d.name.find(L"VB-Audio") != std::wstring::npos)
            return true;
    return false;
}

static bool appIsRunning() { return FindWindowW(L"AudioEnhancerWnd", nullptr) != nullptr; }

static void rebootNow() {
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(tok);
    }
    ExitWindowsEx(EWX_REBOOT, 0x80040002);  // planned, application / installation
}

// ------------------------------------------------------------------ install worker
static void step(int percent, const std::wstring& text) {
    PostMessageW(g_wnd, WM_STATUS, (WPARAM)percent, (LPARAM) new std::wstring(text));
}

static void finish(bool ok) {
    g_ok = ok;
    PostMessageW(g_wnd, WM_FINISHED, 0, 0);
}

static void installWorker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const std::wstring dir = installDir();
    step(5, L"Preparing...");
    makeDirs(dir + L"\\ui");

    // 1) application files
    step(10, L"Copying application files...");
    struct Item { int id; std::wstring rel; };
    const Item items[] = {
        {RES_EXE, L"\\AudioEnhancer.exe"},     {RES_LOADER, L"\\WebView2Loader.dll"},
        {RES_HTML, L"\\ui\\index.html"},       {RES_CSS, L"\\ui\\style.css"},
        {RES_JS, L"\\ui\\script.js"},          {RES_ICON, L"\\ui\\icon.png"},
    };
    for (const auto& it : items) {
        if (!writeResource(it.id, dir + it.rel)) {
            g_error = L"Could not write " + dir + it.rel + L"\n\nClose Audio Enhancer if it is running and try again.";
            CoUninitialize();
            return finish(false);
        }
    }
    CopyFileW(selfPath().c_str(), (dir + L"\\uninstall.exe").c_str(), FALSE);

    // 2) shortcuts + uninstall entry
    step(30, L"Creating shortcuts...");
    const std::wstring exe = dir + L"\\AudioEnhancer.exe";
    createShortcut(specialDir(CSIDL_COMMON_PROGRAMS) + L"\\" + kAppName + L".lnk", exe, dir);
    createShortcut(specialDir(CSIDL_COMMON_DESKTOPDIRECTORY) + L"\\" + kAppName + L".lnk", exe, dir);

    step(40, L"Registering uninstaller...");
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k,
                        nullptr) == ERROR_SUCCESS) {
        regSetStr(k, L"DisplayName", kAppName);
        regSetStr(k, L"DisplayVersion", kAppVersion);
        regSetStr(k, L"Publisher", kAppName);
        regSetStr(k, L"InstallLocation", dir);
        regSetStr(k, L"DisplayIcon", exe);
        regSetStr(k, L"UninstallString", L"\"" + dir + L"\\uninstall.exe\" /uninstall");
        DWORD one = 1;
        RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegCloseKey(k);
    }

    // 3) virtual audio cable
    step(50, L"Checking for the virtual audio device...");
    if (cableInstalled()) {
        step(95, L"Virtual audio device already installed.");
    } else {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring work = std::wstring(tmp) + L"AudioEnhancerSetup";
        removeTree(work);
        makeDirs(work);
        const std::wstring zip = work + L"\\vbcable.zip";
        const std::wstring extracted = work + L"\\pack";

        step(55, L"Downloading VB-Cable from vb-audio.com...");
        if (FAILED(URLDownloadToFileW(nullptr, kCableUrl, zip.c_str(), 0, nullptr))) {
            g_warning = L"The VB-Cable driver could not be downloaded (no internet?).\n"
                        L"Install it manually from https://vb-audio.com/Cable and restart your PC.";
        } else {
            step(70, L"Extracting driver package...");
            runProcess(L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" +
                           zip + L"' -DestinationPath '" + extracted + L"' -Force\"",
                       L"", true, 120000);

            const std::wstring setupExe = findFile(extracted, L"VBCABLE_Setup_x64.exe");
            if (setupExe.empty()) {
                g_warning = L"The downloaded VB-Cable package did not contain the installer.\n"
                            L"Install it manually from https://vb-audio.com/Cable and restart your PC.";
            } else {
                step(80, L"Installing the virtual audio driver - click \"Install\" in the Windows prompt...");
                const size_t slash = setupExe.find_last_of(L'\\');
                const DWORD code = runProcess(L"\"" + setupExe + L"\" -i -h", setupExe.substr(0, slash), false, 5 * 60 * 1000);
                if (code == (DWORD)-1 || code == (DWORD)-2)
                    g_warning = L"The VB-Cable installer did not finish. You can run it again from "
                                L"https://vb-audio.com/Cable.";
                else
                    g_needReboot = true;  // VB-Cable needs a restart to work properly
            }
        }
        removeTree(work);
    }

    step(100, L"Done.");
    CoUninitialize();
    finish(true);
}

// ------------------------------------------------------------------ uninstall
static int runUninstall() {
    if (appIsRunning()) {
        MessageBoxW(nullptr, L"Please close Audio Enhancer first, then run the uninstaller again.", kAppName,
                    MB_ICONINFORMATION);
        return 1;
    }
    if (MessageBoxW(nullptr, L"Remove Audio Enhancer from this computer?\n\n"
                             L"(The VB-Cable virtual audio driver is a separate program and will stay installed.)",
                    kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return 0;

    const std::wstring dir = installDir();  // fixed path on purpose - never derived from the exe location
    DeleteFileW((specialDir(CSIDL_COMMON_PROGRAMS) + L"\\" + kAppName + L".lnk").c_str());
    DeleteFileW((specialDir(CSIDL_COMMON_DESKTOPDIRECTORY) + L"\\" + kAppName + L".lnk").c_str());
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, KEY_WOW64_64KEY, 0);

    // This exe is running from the install folder, so let a detached cmd delete the folder after we exit.
    std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 > nul & rmdir /s /q \"" + dir + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                       nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    MessageBoxW(nullptr, L"Audio Enhancer was removed.", kAppName, MB_ICONINFORMATION);
    return 0;
}

// ------------------------------------------------------------------ progress window
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_STATUS: {
            std::wstring* t = reinterpret_cast<std::wstring*>(lp);
            SetWindowTextW(g_label, t->c_str());
            SendMessageW(g_bar, PBM_SETPOS, wp, 0);
            delete t;
            return 0;
        }
        case WM_FINISHED:
            g_finished = true;
            DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE:
            if (!g_finished) return 0;  // don't allow closing in the middle of an install
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    if (wcsstr(GetCommandLineW(), L"/uninstall")) {
        const int rc = runUninstall();
        CoUninitialize();
        return rc;
    }

    if (appIsRunning()) {
        MessageBoxW(nullptr, L"Please close Audio Enhancer first, then run setup again.", kAppName, MB_ICONINFORMATION);
        return 1;
    }
    if (MessageBoxW(nullptr,
                    L"This will install Audio Enhancer.\n\n"
                    L"If the virtual audio device (VB-Cable) is not installed yet, setup will download it from "
                    L"vb-audio.com and install it. Windows will ask you to confirm the driver, and a restart "
                    L"is needed afterwards.\n\nContinue?",
                    L"Audio Enhancer Setup", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        CoUninitialize();
        return 0;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AESetupWnd";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    HDC dc = GetDC(nullptr);
    const double s = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, dc);
    auto S = [s](int v) { return (int)(v * s); };

    const DWORD style = WS_CAPTION | WS_SYSMENU;
    RECT r = {0, 0, S(460), S(120)};
    AdjustWindowRect(&r, style, FALSE);
    const int w = r.right - r.left, h = r.bottom - r.top;
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"Audio Enhancer Setup", style,
                            (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                            nullptr, nullptr, hInst, nullptr);

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_label = CreateWindowExW(0, L"STATIC", L"Starting...", WS_CHILD | WS_VISIBLE | SS_LEFT, S(20), S(18), S(420),
                              S(40), g_wnd, nullptr, hInst, nullptr);
    SendMessageW(g_label, WM_SETFONT, (WPARAM)font, TRUE);
    g_bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, S(20), S(70), S(420), S(18), g_wnd,
                            nullptr, hInst, nullptr);
    SendMessageW(g_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    ShowWindow(g_wnd, SW_SHOW);
    UpdateWindow(g_wnd);

    g_worker = std::thread(installWorker);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_worker.joinable()) g_worker.join();

    if (!g_ok) {
        MessageBoxW(nullptr, (L"Setup failed.\n\n" + g_error).c_str(), L"Audio Enhancer Setup", MB_ICONERROR);
    } else {
        std::wstring msg = L"Audio Enhancer was installed successfully.\n";
        if (!g_warning.empty()) msg += L"\n" + g_warning + L"\n";
        if (g_needReboot) {
            msg += L"\nA restart is required to finish installing the virtual audio driver.\nRestart now?";
            if (MessageBoxW(nullptr, msg.c_str(), L"Audio Enhancer Setup", MB_YESNO | MB_ICONINFORMATION) == IDYES)
                rebootNow();
        } else {
            MessageBoxW(nullptr, msg.c_str(), L"Audio Enhancer Setup", MB_OK | MB_ICONINFORMATION);
        }
    }
    CoUninitialize();
    return g_ok ? 0 : 1;
}
