// main.cpp - Audio Enhancer
// Win32 window that hosts a WebView2 UI (ui/index.html, style.css, script.js)
// plus the WASAPI backend (audio_engine.h).
//
// Flow:
//   * Output list = every active playback device except the virtual cable (VB-Cable).
//   * Power ON  -> Windows default output is switched to the virtual cable, the engine
//                  captures it, applies effect + bass/treble + volume and plays it on the chosen output.
//   * Power OFF -> engine stops and the previous Windows default output is restored.
//   * Settings are saved to %LOCALAPPDATA%\AudioEnhancer\settings.ini
//
// Tray:   minimize and the X button hide the window into the system tray (the engine keeps running).
//         Left-click the tray icon = open, right-click = menu (Open / Enable / Effect / Start with Windows / Exit).
//         "Exit" in the tray menu is the only way to quit. Start hidden with:  AudioEnhancer.exe --tray
//
// Settings page: theme / mode / start-with-Windows / update preferences are stored in settings.ini.
// Updates:  checks https://github.com/mahfuz0712/audioEnhancer/releases (latest) on start and every 12 hours,
//           picks the installer for this OS (Windows: .exe), downloads it, verifies the SHA-256 and runs it.
//
// JS -> C++ (strings):  "ready"  "output|<deviceId>"  "effect|<Normal|3D|Balanced>"  "power|<1|0>"
//                       "volume|<0..100>"  "bass|<-10..10>"  "treble|<-10..10>"
// C++ -> JS (JSON):     {"type":"devices",...}  {"type":"state",...}  {"type":"power","value":bool}
//                       {"type":"status",...}

#include <windows.h>
#include <shellapi.h>

#ifndef ENDSESSION_CLOSEAPP
#define ENDSESSION_CLOSEAPP 0x00000001
#endif

#include <winhttp.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"
#include "audio_engine.h"
#include "version.h"

#define AE_WIDEN2(x) L##x
#define AE_WIDEN(x) AE_WIDEN2(x)
static const wchar_t *kAppVersion = AE_WIDEN(APP_VERSION_STR);

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

// ------------------------------------------------------------------ state
static HWND g_hwnd = nullptr;
static ICoreWebView2Controller *g_ctrl = nullptr;
static ICoreWebView2 *g_web = nullptr;

static AudioEngine g_engine;
static std::vector<AudioDevice> g_all;
static std::wstring g_cableId, g_outputId, g_prevDefault;

static int g_effect = 0;         // 0 Normal, 1 3D, 2 Balanced (same order as the Effect enum)
static int g_volume = 100;       // 0..100 %
static int g_bass = 0;           // -10..+10 dB
static int g_treble = 0;         // -10..+10 dB
static bool g_power = true;      // current on/off state
static bool g_savedPower = true; // what the user last chose (persisted)
static bool g_running = false;
static bool g_booted = false;
static bool g_dirty = false; // settings changed -> save on next timer tick

static const UINT_PTR TIMER_ID = 1;

// ---- tray / lifecycle
static const UINT WM_TRAY = WM_APP + 1;
static const UINT TRAY_UID = 1;
enum
{
    ID_TRAY_OPEN = 1001,
    ID_TRAY_POWER = 1002,
    ID_TRAY_EFFECT0 = 1010, // 1010..1012 = Normal / 3D / Balanced
    ID_TRAY_AUTOSTART = 1020,
    ID_TRAY_EXIT = 1099
};
static const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *kRunValue = L"AudioEnhancer";

// ---- settings page + updates
static const UINT WM_UPDATE_MSG = WM_APP + 3;    // worker thread -> UI thread: lParam = new std::wstring(json)
static const UINT WM_UPDATE_LAUNCH = WM_APP + 4; // worker thread -> UI thread: run the downloaded installer
static std::wstring g_theme = L"default";        // see ui/style.css for the theme ids
static std::wstring g_mode = L"system";          // system | light | dark
static bool g_checkUpdates = true;

struct UpdateInfo
{
    std::wstring tag, url, name, sha256;
};
static std::mutex g_updMutex;
static UpdateInfo g_upd;       // newest release found by the last check
static std::wstring g_updFile; // downloaded installer
static std::wstring g_notifiedTag;
static std::atomic<bool> g_updBusy(false);
static ULONGLONG g_lastUpdateCheck = 0;

static UINT g_wmTaskbarCreated = 0;   // Explorer restarted -> re-add the tray icon
static UINT g_wmShowApp = 0;          // sent by a second instance: "bring the window up"
static HICON g_trayIconOn = nullptr;  // enhancer running
static HICON g_trayIconOff = nullptr; // enhancer off (dimmed)
static HANDLE g_mutex = nullptr;
static bool g_trayAdded = false;
static bool g_quitting = false;
static bool g_startHidden = false;
static bool g_trayHintShown = false; // the "still running in the tray" balloon is shown only once

static void updateTray();  // defined with the tray code below
static bool isAutostart(); // defined with the tray code below
static void setAutostart(bool on);

// ------------------------------------------------------------------ helpers
static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static bool isCable(const AudioDevice &d)
{
    return d.name.find(L"CABLE") != std::wstring::npos ||
           d.name.find(L"VB-Audio") != std::wstring::npos;
}

// VB-Cable Driver Pack 45 installs two devices: "CABLE Input" (stereo) and "CABLE In 16ch".
static bool is16ch(const AudioDevice &d)
{
    return d.name.find(L"16ch") != std::wstring::npos || d.name.find(L"16 ch") != std::wstring::npos;
}

static const AudioDevice *findById(const std::wstring &id)
{
    if (id.empty())
        return nullptr;
    for (const auto &d : g_all)
        if (d.id == id)
            return &d;
    return nullptr;
}

static const AudioDevice *findOutput(const std::wstring &id)
{
    const AudioDevice *d = findById(id);
    return (d && !isCable(*d)) ? d : nullptr;
}

static bool sameList(const std::vector<AudioDevice> &a, const std::vector<AudioDevice> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].name != b[i].name)
            return false;
    return true;
}

static const wchar_t *effectName(int e)
{
    return e == 1 ? L"3D" : (e == 2 ? L"Balanced" : L"Normal");
}

static int effectFromName(const std::wstring &n)
{
    return n == L"3D" ? 1 : (n == L"Balanced" ? 2 : 0);
}

static const wchar_t *kindOf(int ff)
{
    switch (ff)
    {
    case 3:
    case 5:
        return L"headphones";
    case 9:
        return L"display";
    case 7:
    case 8:
        return L"digital";
    default:
        return L"speakers";
    }
}

static const wchar_t *labelOf(int ff)
{
    switch (ff)
    {
    case 1:
        return L"Speakers";
    case 2:
        return L"Line out";
    case 3:
        return L"Headphones";
    case 5:
        return L"Headset";
    case 7:
    case 8:
        return L"Digital audio";
    case 9:
        return L"HDMI / DisplayPort";
    default:
        return L"Audio device";
    }
}

static std::wstring jsonEsc(const std::wstring &s)
{
    std::wstring o;
    for (wchar_t c : s)
    {
        switch (c)
        {
        case L'"':
            o += L"\\\"";
            break;
        case L'\\':
            o += L"\\\\";
            break;
        case L'\n':
            o += L"\\n";
            break;
        case L'\r':
            o += L"\\r";
            break;
        case L'\t':
            o += L"\\t";
            break;
        default:
            if (c < 0x20)
            {
                wchar_t b[8];
                swprintf(b, 8, L"\\u%04x", (unsigned)c);
                o += b;
            }
            else
            {
                o += c;
            }
        }
    }
    return o;
}

static std::wstring exeDir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p = buf;
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : p.substr(0, k);
}

// ------------------------------------------------------------------ settings (settings.ini)
static std::wstring settingsPath()
{
    wchar_t local[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0)
        return L"";
    const std::wstring base = std::wstring(local) + L"\\AudioEnhancer";
    CreateDirectoryW(base.c_str(), nullptr);
    return base + L"\\settings.ini";
}

static void applyToneToEngine()
{
    g_engine.setEffect(g_effect);
    g_engine.setVolume(g_volume);
    g_engine.setBass(g_bass);
    g_engine.setTreble(g_treble);
}

static bool validThemeId(const std::wstring &t)
{
    if (t.empty() || t.size() > 24)
        return false;
    for (wchar_t c : t)
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-'))
            return false;
    return true;
}

static bool validMode(const std::wstring &m)
{
    return m == L"system" || m == L"light" || m == L"dark";
}

static void loadSettings()
{
    const std::wstring p = settingsPath();
    if (p.empty())
        return;
    const wchar_t *sec = L"Audio";
    g_effect = clampInt((int)GetPrivateProfileIntW(sec, L"Effect", 0, p.c_str()), 0, 2);
    g_volume = clampInt((int)GetPrivateProfileIntW(sec, L"Volume", 100, p.c_str()), 0, 100);
    // bass / treble are stored with +100 offset because GetPrivateProfileInt cannot read negatives
    g_bass = clampInt((int)GetPrivateProfileIntW(sec, L"Bass", 100, p.c_str()) - 100, -10, 10);
    g_treble = clampInt((int)GetPrivateProfileIntW(sec, L"Treble", 100, p.c_str()) - 100, -10, 10);
    g_savedPower = GetPrivateProfileIntW(sec, L"Power", 1, p.c_str()) != 0;
    g_power = g_savedPower;
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(sec, L"Output", L"", buf, 512, p.c_str());
    g_outputId = buf;
    g_trayHintShown = GetPrivateProfileIntW(L"App", L"TrayHint", 0, p.c_str()) != 0;
    g_checkUpdates = GetPrivateProfileIntW(L"App", L"CheckUpdates", 1, p.c_str()) != 0;
    wchar_t t[64] = {};
    GetPrivateProfileStringW(L"App", L"Theme", L"default", t, 64, p.c_str());
    if (validThemeId(t))
        g_theme = t;
    GetPrivateProfileStringW(L"App", L"Mode", L"system", t, 64, p.c_str());
    if (validMode(t))
        g_mode = t;
}

static void saveSettings()
{
    const std::wstring p = settingsPath();
    if (p.empty())
        return;
    const wchar_t *sec = L"Audio";
    WritePrivateProfileStringW(sec, L"Effect", std::to_wstring(g_effect).c_str(), p.c_str());
    WritePrivateProfileStringW(sec, L"Volume", std::to_wstring(g_volume).c_str(), p.c_str());
    WritePrivateProfileStringW(sec, L"Bass", std::to_wstring(g_bass + 100).c_str(), p.c_str());
    WritePrivateProfileStringW(sec, L"Treble", std::to_wstring(g_treble + 100).c_str(), p.c_str());
    WritePrivateProfileStringW(sec, L"Power", g_savedPower ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(sec, L"Output", g_outputId.c_str(), p.c_str());
    WritePrivateProfileStringW(L"App", L"TrayHint", g_trayHintShown ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"App", L"CheckUpdates", g_checkUpdates ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"App", L"Theme", g_theme.c_str(), p.c_str());
    WritePrivateProfileStringW(L"App", L"Mode", g_mode.c_str(), p.c_str());
}

// ------------------------------------------------------------------ C++ -> JS
static void postJson(const std::wstring &json)
{
    if (g_web)
        g_web->PostWebMessageAsJson(json.c_str());
}

static void postStatus(const wchar_t *state, const std::wstring &title, const std::wstring &text)
{
    postJson(L"{\"type\":\"status\",\"state\":\"" + std::wstring(state) + L"\",\"title\":\"" +
             jsonEsc(title) + L"\",\"text\":\"" + jsonEsc(text) + L"\"}");
    updateTray(); // keep the tray tooltip in sync with the status
}

static void postPower()
{
    postJson(std::wstring(L"{\"type\":\"power\",\"value\":") + (g_power ? L"true" : L"false") + L"}");
}

static void postSettings()
{
    postJson(L"{\"type\":\"settings\",\"theme\":\"" + jsonEsc(g_theme) + L"\",\"mode\":\"" + jsonEsc(g_mode) +
             L"\",\"autostart\":" + (isAutostart() ? L"true" : L"false") +
             L",\"checkUpdates\":" + (g_checkUpdates ? L"true" : L"false") + L",\"version\":\"" + kAppVersion +
             L"\"}");
}

static void postState()
{
    postJson(L"{\"type\":\"state\",\"effect\":\"" + std::wstring(effectName(g_effect)) +
             L"\",\"volume\":" + std::to_wstring(g_volume) + L",\"bass\":" + std::to_wstring(g_bass) +
             L",\"treble\":" + std::to_wstring(g_treble) + L"}");
}

static void postDevices()
{
    std::wstring j = L"{\"type\":\"devices\",\"selected\":\"" + jsonEsc(g_outputId) + L"\",\"devices\":[";
    bool first = true;
    for (const auto &d : g_all)
    {
        if (isCable(d))
            continue;
        if (!first)
            j += L",";
        first = false;
        j += L"{\"id\":\"" + jsonEsc(d.id) + L"\",\"name\":\"" + jsonEsc(d.name) +
             L"\",\"description\":\"" + labelOf(d.formFactor) + L"\",\"kind\":\"" +
             kindOf(d.formFactor) + L"\"}";
    }
    j += L"]}";
    postJson(j);
}

static void postCurrentStatus()
{
    if (g_running)
    {
        const AudioDevice *o = findById(g_outputId);
        std::wstring text = o ? o->name : std::wstring();
        text += L" \u00B7 ";
        text += effectName(g_effect);
        postStatus(L"active", L"Audio Enhancer Active", text);
    }
    else
    {
        postStatus(L"idle", L"Audio Enhancer Off", L"Turn it on to enhance your system audio");
    }
}

// ------------------------------------------------------------------ backend logic
static void refreshDevices(bool force)
{
    std::vector<AudioDevice> now = enumerateOutputDevices();
    const bool changed = !sameList(now, g_all);
    g_all = now;

    // Prefer the normal stereo cable over the 16-channel one.
    g_cableId.clear();
    for (const auto &d : g_all)
        if (isCable(d) && !is16ch(d))
        {
            g_cableId = d.id;
            break;
        }
    if (g_cableId.empty())
        for (const auto &d : g_all)
            if (isCable(d))
            {
                g_cableId = d.id;
                break;
            }

    if (!findOutput(g_outputId))
    {
        g_outputId.clear();
        for (const auto &d : g_all)
            if (!isCable(d) && d.isDefault)
            {
                g_outputId = d.id;
                break;
            }
        if (g_outputId.empty())
            for (const auto &d : g_all)
                if (!isCable(d))
                {
                    g_outputId = d.id;
                    break;
                }
    }

    if (changed || force)
        postDevices();
}

// Put the Windows default output back to a real device.
static void restoreDefault()
{
    std::wstring target;
    if (findOutput(g_prevDefault))
        target = g_prevDefault;
    else if (findOutput(g_outputId))
        target = g_outputId;
    else
        for (const auto &d : g_all)
            if (!isCable(d))
            {
                target = d.id;
                break;
            }
    if (!target.empty())
        setDefaultOutput(target);
}

static bool startEngine()
{
    if (g_running)
        return true;
    if (g_cableId.empty())
    {
        postStatus(L"error", L"Virtual audio device not found",
                   L"Install VB-Cable (vb-audio.com/Cable), restart your PC and open the app again.");
        return false;
    }
    if (!findOutput(g_outputId))
    {
        postStatus(L"error", L"No output device", L"Connect a speaker or headphones, then turn the enhancer on.");
        return false;
    }

    g_prevDefault = getDefaultOutputId();
    if (g_prevDefault == g_cableId)
        g_prevDefault.clear();
    setDefaultOutput(g_cableId);

    applyToneToEngine();
    std::wstring err;
    if (!g_engine.start(g_cableId, g_outputId, err))
    {
        restoreDefault();
        postStatus(L"error", L"Could not start", err);
        return false;
    }
    g_running = true;
    postCurrentStatus();
    return true;
}

static void stopEngine()
{
    if (!g_running)
        return;
    g_engine.stop();
    g_running = false;
    restoreDefault();
    postCurrentStatus();
}

static void setPower(bool on)
{
    g_power = on;
    if (on)
    {
        if (!startEngine())
            g_power = false;
    }
    else
    {
        stopEngine();
    }
    postPower();
}

static void changeOutput(const std::wstring &id)
{
    if (!findOutput(id))
        return;
    g_outputId = id;
    g_dirty = true;
    if (!g_running)
    {
        postCurrentStatus();
        return;
    }
    // Keep the Windows default on the virtual cable, just move the engine to the new output.
    g_engine.stop();
    applyToneToEngine();
    std::wstring err;
    if (!g_engine.start(g_cableId, g_outputId, err))
    {
        g_running = false;
        restoreDefault();
        g_power = false;
        postPower();
        postStatus(L"error", L"Could not switch output", err);
    }
    else
    {
        postCurrentStatus();
    }
}

// ------------------------------------------------------------------ updates (GitHub Releases)
static const wchar_t *kLatestApiUrl = L"https://api.github.com/repos/mahfuz0712/audioEnhancer/releases/latest";
static const wchar_t *kDownloadPrefix = L"https://github.com/mahfuz0712/audioEnhancer/releases/download/";

// Which release file belongs to this OS. The app is Windows-only today (WASAPI + WebView2);
// a Linux build would return L".deb" here (and use the matching install command).
static const wchar_t *platformAssetExtension() { return L".exe"; }

static void postUpd(const std::wstring &json, bool notify = false)
{
    if (g_hwnd)
        PostMessageW(g_hwnd, WM_UPDATE_MSG, notify ? 1 : 0, (LPARAM) new std::wstring(json));
}

static void updEvt(const wchar_t *state, const std::wstring &extra = L"")
{
    postUpd(L"{\"type\":\"update\",\"state\":\"" + std::wstring(state) + L"\",\"current\":\"" + kAppVersion + L"\"" +
            extra + L"}");
}

static void updError(const std::wstring &msg)
{
    updEvt(L"error", L",\"message\":\"" + jsonEsc(msg) + L"\"");
}

struct HttpReq
{
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    ~HttpReq()
    {
        if (req)
            WinHttpCloseHandle(req);
        if (con)
            WinHttpCloseHandle(con);
        if (ses)
            WinHttpCloseHandle(ses);
    }
};

// Opens the request and reads the response headers. Redirects (GitHub -> CDN) are followed automatically.
static bool httpStart(const std::wstring &url, const wchar_t *accept, HttpReq &h, DWORD &status, DWORD &len)
{
    wchar_t host[256] = {}, path[2048] = {}, extra[1024] = {};
    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
        return false;

    const std::wstring ua = std::wstring(L"AudioEnhancer/") + kAppVersion;
    h.ses = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.ses)
        h.ses = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.ses)
        return false;
    WinHttpSetTimeouts(h.ses, 15000, 15000, 20000, 60000);
    h.con = WinHttpConnect(h.ses, host, uc.nPort, 0);
    if (!h.con)
        return false;
    const std::wstring target = std::wstring(path) + extra;
    h.req = WinHttpOpenRequest(h.con, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!h.req)
        return false;
    const std::wstring hdr = std::wstring(L"Accept: ") + accept + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(h.req, hdr.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(h.req, nullptr))
        return false;
    DWORD sz = sizeof(DWORD);
    status = 0;
    WinHttpQueryHeaders(h.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sz, WINHTTP_NO_HEADER_INDEX);
    sz = sizeof(DWORD);
    len = 0;
    WinHttpQueryHeaders(h.req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &len, &sz, WINHTTP_NO_HEADER_INDEX);
    return true;
}

static std::wstring utf8ToWide(const std::string &s)
{
    if (s.empty())
        return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static bool httpGetString(const std::wstring &url, std::wstring &out, DWORD &status)
{
    HttpReq h;
    DWORD len = 0;
    if (!httpStart(url, L"application/vnd.github+json", h, status, len))
        return false;
    std::string data;
    for (;;)
    {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(h.req, &avail) || avail == 0)
            break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(h.req, &chunk[0], avail, &got) || got == 0)
            break;
        data.append(chunk.data(), got);
        if (data.size() > 4 * 1024 * 1024)
            break; // a release description is never this big
    }
    out = utf8ToWide(data);
    return true;
}

static bool httpDownload(const std::wstring &url, const std::wstring &path, std::wstring &err,
                         const std::function<void(int)> &progress)
{
    HttpReq h;
    DWORD status = 0, len = 0;
    if (!httpStart(url, L"application/octet-stream", h, status, len))
    {
        err = L"Could not reach GitHub. Check your internet connection.";
        return false;
    }
    if (status != 200)
    {
        err = L"The download failed (HTTP " + std::to_wstring(status) + L").";
        return false;
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        err = L"Could not write the installer to the temp folder.";
        return false;
    }
    bool ok = true;
    ULONGLONG total = 0;
    int lastPct = -1;
    std::vector<char> buf(64 * 1024);
    for (;;)
    {
        DWORD got = 0;
        if (!WinHttpReadData(h.req, buf.data(), (DWORD)buf.size(), &got))
        {
            ok = false;
            break;
        }
        if (got == 0)
            break;
        DWORD wr = 0;
        if (!WriteFile(f, buf.data(), got, &wr, nullptr) || wr != got)
        {
            ok = false;
            break;
        }
        total += got;
        if (len > 0)
        {
            const int pct = (int)(total * 100 / len);
            if (pct != lastPct)
            {
                lastPct = pct;
                progress(pct);
            }
        }
    }
    CloseHandle(f);
    if (!ok || (len > 0 && total != len))
    {
        DeleteFileW(path.c_str());
        err = L"The download was interrupted. Please try again.";
        return false;
    }
    return true;
}

static bool sha256File(const std::wstring &path, std::wstring &hex)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0)
    {
        std::vector<UCHAR> buf(64 * 1024);
        DWORD got = 0;
        ok = true;
        while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got > 0)
            if (BCryptHashData(hash, buf.data(), got, 0) < 0)
            {
                ok = false;
                break;
            }
        UCHAR digest[32] = {};
        if (ok && BCryptFinishHash(hash, digest, 32, 0) >= 0)
        {
            hex.clear();
            for (UCHAR b : digest)
            {
                wchar_t t[4];
                swprintf(t, 4, L"%02x", (unsigned)b);
                hex += t;
            }
        }
        else
        {
            ok = false;
        }
    }
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

// "v1.0.10" -> {1,0,10}
static std::vector<int> parseVersion(const std::wstring &s)
{
    std::vector<int> v;
    size_t i = 0;
    while (i < s.size())
    {
        if (s[i] >= L'0' && s[i] <= L'9')
        {
            int n = 0;
            while (i < s.size() && s[i] >= L'0' && s[i] <= L'9')
                n = n * 10 + (s[i++] - L'0');
            v.push_back(n);
        }
        else if (!v.empty() && s[i] != L'.')
        {
            break; // "1.0.1-beta": stop at the suffix
        }
        else
        {
            i++;
        }
    }
    return v;
}

static int compareVersions(const std::wstring &a, const std::wstring &b)
{
    const std::vector<int> x = parseVersion(a), y = parseVersion(b);
    for (size_t i = 0; i < (x.size() > y.size() ? x.size() : y.size()); i++)
    {
        const int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q)
            return p < q ? -1 : 1;
    }
    return 0;
}

// Reads the string value of "key" found between [from, to) in a JSON text (just enough JSON for GitHub's reply).
static bool jsonGet(const std::wstring &j, const std::wstring &key, size_t from, size_t to, std::wstring &out)
{
    const std::wstring pat = L"\"" + key + L"\"";
    size_t p = j.find(pat, from);
    if (p == std::wstring::npos || p >= to)
        return false;
    p += pat.size();
    while (p < j.size() && (j[p] == L':' || j[p] == L' ' || j[p] == L'\n' || j[p] == L'\r' || j[p] == L'\t'))
        p++;
    if (p >= j.size() || j[p] != L'"')
        return false; // null / number
    p++;
    out.clear();
    while (p < j.size() && j[p] != L'"')
    {
        if (j[p] == L'\\' && p + 1 < j.size())
        {
            const wchar_t c = j[++p];
            if (c == L'n')
                out += L'\n';
            else if (c == L't')
                out += L'\t';
            else if (c == L'r')
                ; // drop
            else if (c == L'u' && p + 4 < j.size())
            {
                out += (wchar_t)wcstol(j.substr(p + 1, 4).c_str(), nullptr, 16);
                p += 4;
            }
            else
                out += c;
        }
        else
        {
            out += j[p];
        }
        p++;
    }
    return true;
}

static std::wstring lowerCopy(std::wstring s)
{
    for (auto &c : s)
        c = (wchar_t)towlower(c);
    return s;
}

static void updateCheckWorker(bool silent)
{
    std::wstring body;
    DWORD status = 0;
    if (!httpGetString(kLatestApiUrl, body, status))
    {
        updError(L"Could not reach GitHub. Check your internet connection.");
        g_updBusy = false;
        return;
    }
    if (status == 404)
    { // no release published yet
        updEvt(L"latest");
        g_updBusy = false;
        return;
    }
    if (status == 403 || status == 429)
    {
        updError(L"GitHub is limiting requests right now. Please try again in a while.");
        g_updBusy = false;
        return;
    }
    std::wstring tag;
    if (status != 200 || !jsonGet(body, L"tag_name", 0, std::wstring::npos, tag))
    {
        updError(L"Could not read the release information (HTTP " + std::to_wstring(status) + L").");
        g_updBusy = false;
        return;
    }

    std::wstring shown = tag;
    if (!shown.empty() && (shown[0] == L'v' || shown[0] == L'V'))
        shown.erase(0, 1);

    if (compareVersions(tag, kAppVersion) <= 0)
    {
        updEvt(L"latest", L",\"latest\":\"" + jsonEsc(shown) + L"\"");
        g_updBusy = false;
        return;
    }

    // pick the release file for this OS (Windows: *.exe, preferably the one called "...Setup...")
    const std::wstring ext = platformAssetExtension();
    UpdateInfo best;
    ULONGLONG bestSize = 0;
    int bestScore = -1;
    size_t pos = 0;
    const size_t npos = std::wstring::npos;
    while ((pos = body.find(L"\"browser_download_url\"", pos)) != npos)
    {
        const size_t urlPos = pos;
        pos += 21;
        std::wstring url, name, digest;
        if (!jsonGet(body, L"browser_download_url", urlPos, npos, url))
            continue;
        size_t start = body.rfind(L"/releases/assets/", urlPos);
        if (start == npos)
            start = 0;
        jsonGet(body, L"name", start, urlPos, name);
        jsonGet(body, L"digest", start, urlPos, digest);
        ULONGLONG size = 0;
        const size_t sp = body.find(L"\"size\":", start);
        if (sp != npos && sp < urlPos)
            size = _wcstoui64(body.c_str() + sp + 7, nullptr, 10);

        const std::wstring ln = lowerCopy(name);
        if (ln.size() < ext.size() || ln.compare(ln.size() - ext.size(), ext.size(), ext) != 0)
            continue;
        const int score = 1 + (ln.find(L"setup") != npos ? 2 : 0);
        if (score > bestScore)
        {
            bestScore = score;
            best.url = url;
            best.name = name;
            best.tag = tag;
            best.sha256 = (digest.rfind(L"sha256:", 0) == 0) ? digest.substr(7) : L"";
            bestSize = size;
        }
    }

    std::wstring notes;
    jsonGet(body, L"body", 0, npos, notes);
    if (notes.size() > 1200)
        notes = notes.substr(0, 1200) + L"...";

    if (bestScore < 0)
    {
        updEvt(L"noasset", L",\"latest\":\"" + jsonEsc(shown) + L"\"");
        g_updBusy = false;
        return;
    }

    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(g_updMutex);
        g_upd = best;
        if (silent && g_notifiedTag != tag)
        {
            g_notifiedTag = tag;
            notify = true;
        }
    }
    postUpd(L"{\"type\":\"update\",\"state\":\"available\",\"current\":\"" + std::wstring(kAppVersion) +
                L"\",\"latest\":\"" + jsonEsc(shown) + L"\",\"name\":\"" + jsonEsc(best.name) + L"\",\"size\":" +
                std::to_wstring(bestSize) + L",\"notes\":\"" + jsonEsc(notes) + L"\"}",
            notify);
    g_updBusy = false;
}

static void updateDownloadWorker()
{
    UpdateInfo info;
    {
        std::lock_guard<std::mutex> lock(g_updMutex);
        info = g_upd;
    }
    // only ever download from this project's own GitHub releases
    if (info.url.rfind(kDownloadPrefix, 0) != 0)
    {
        updError(L"The update link is not a release of this project, so it was not downloaded.");
        g_updBusy = false;
        return;
    }
    std::wstring safe;
    for (wchar_t c : info.name)
        safe += ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' ||
                 c == L'-' || c == L'_')
                    ? c
                    : L'_';
    if (safe.empty())
        safe = L"AudioEnhancer-Setup.exe";

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"AudioEnhancerUpdate";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring file = dir + L"\\" + safe;

    updEvt(L"downloading", L",\"progress\":0");
    std::wstring err;
    const bool ok = httpDownload(info.url, file, err, [](int pct)
                                 { updEvt(L"downloading", L",\"progress\":" + std::to_wstring(pct)); });
    if (!ok)
    {
        updError(err);
        g_updBusy = false;
        return;
    }
    if (!info.sha256.empty())
    {
        std::wstring got;
        if (!sha256File(file, got) || lowerCopy(got) != lowerCopy(info.sha256))
        {
            DeleteFileW(file.c_str());
            updError(L"The downloaded file is damaged (checksum mismatch), so it was not started.");
            g_updBusy = false;
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_updMutex);
        g_updFile = file;
    }
    updEvt(L"installing");
    PostMessageW(g_hwnd, WM_UPDATE_LAUNCH, 0, 0);
    g_updBusy = false;
}

static void startUpdateCheck(bool silent)
{
    bool expected = false;
    if (!g_updBusy.compare_exchange_strong(expected, true))
        return;
    g_lastUpdateCheck = GetTickCount64();
    if (!silent)
        updEvt(L"checking");
    std::thread(updateCheckWorker, silent).detach();
}

static void startUpdateDownload()
{
    {
        std::lock_guard<std::mutex> lock(g_updMutex);
        if (g_upd.url.empty())
            return;
    }
    bool expected = false;
    if (!g_updBusy.compare_exchange_strong(expected, true))
        return;
    std::thread(updateDownloadWorker).detach();
}

// UI thread: run the downloaded installer (UAC asks for admin), then close ourselves so it can replace the files.
static void launchInstaller()
{
    std::wstring file;
    {
        std::lock_guard<std::mutex> lock(g_updMutex);
        file = g_updFile;
    }
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"open";
    sei.lpFile = file.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei))
    {
        g_quitting = true;
        DestroyWindow(g_hwnd); // stops the engine and puts the normal output device back
    }
    else
    {
        updError(L"The installer was not started (permission was declined).");
    }
}

static void openUrl(const std::wstring &u)
{
    static const wchar_t *allowed[] = {L"https://github.com/mahfuz0712/", L"https://vb-audio.com/",
                                       L"https://portfolio-mahfuz0712.vercel.app"};
    for (const wchar_t *a : allowed)
        if (u.rfind(a, 0) == 0)
        {
            ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
}

// First start of the engine after launch (also used when the app starts hidden in the tray).
static void bootEngine()
{
    if (g_booted)
        return;
    g_booted = true;
    if (g_power && !startEngine())
        g_power = false;
}

static void handleMessage(const std::wstring &m)
{
    const size_t bar = m.find(L'|');
    const std::wstring type = m.substr(0, bar);
    const std::wstring val = (bar == std::wstring::npos) ? L"" : m.substr(bar + 1);

    if (type == L"ready")
    {
        postState();
        refreshDevices(true);
        if (!g_booted)
        {
            bootEngine();
        }
        else
        {
            postCurrentStatus();
        }
        postPower();
        postSettings();
        if (g_checkUpdates)
            startUpdateCheck(true);
    }
    else if (type == L"theme")
    {
        if (validThemeId(val))
        {
            g_theme = val;
            g_dirty = true;
        }
    }
    else if (type == L"mode")
    {
        if (validMode(val))
        {
            g_mode = val;
            g_dirty = true;
        }
    }
    else if (type == L"autostart")
    {
        setAutostart(val == L"1");
        postSettings();
    }
    else if (type == L"checkupdates")
    {
        g_checkUpdates = (val == L"1");
        g_dirty = true;
    }
    else if (type == L"update")
    {
        if (val == L"check")
            startUpdateCheck(false);
        else if (val == L"install")
            startUpdateDownload();
    }
    else if (type == L"openurl")
    {
        openUrl(val);
    }
    else if (type == L"output")
    {
        changeOutput(val);
    }
    else if (type == L"effect")
    {
        g_effect = effectFromName(val);
        g_engine.setEffect(g_effect);
        g_dirty = true;
        if (g_running)
            postCurrentStatus();
    }
    else if (type == L"volume")
    {
        g_volume = clampInt(_wtoi(val.c_str()), 0, 100);
        g_engine.setVolume(g_volume);
        g_dirty = true;
    }
    else if (type == L"bass")
    {
        g_bass = clampInt(_wtoi(val.c_str()), -10, 10);
        g_engine.setBass(g_bass);
        g_dirty = true;
    }
    else if (type == L"treble")
    {
        g_treble = clampInt(_wtoi(val.c_str()), -10, 10);
        g_engine.setTreble(g_treble);
        g_dirty = true;
    }
    else if (type == L"power")
    {
        g_savedPower = (val == L"1");
        g_dirty = true;
        setPower(g_savedPower);
    }
}

// ------------------------------------------------------------------ WebView2 COM handlers
// Minimal handlers: they live for the whole process, so AddRef/Release are no-ops.
#define AE_COM_BOILERPLATE                                                \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override \
    {                                                                     \
        if (!ppv)                                                         \
            return E_POINTER;                                             \
        *ppv = this;                                                      \
        return S_OK;                                                      \
    }                                                                     \
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }               \
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

static std::wstring pageUrl()
{
    std::wstring p = exeDir() + L"\\ui\\index.html";
    std::wstring url = L"file:///";
    for (wchar_t c : p)
    {
        if (c == L'\\')
            url += L'/';
        else if (c == L' ')
            url += L"%20";
        else if (c == L'#')
            url += L"%23";
        else
            url += c;
    }
    return url;
}

struct MsgHandler : ICoreWebView2WebMessageReceivedEventHandler
{
    AE_COM_BOILERPLATE
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) override
    {
        LPWSTR msg = nullptr;
        if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg)
        {
            handleMessage(msg);
            CoTaskMemFree(msg);
        }
        return S_OK;
    }
};

struct CtrlHandler : ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
{
    AE_COM_BOILERPLATE
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller *ctrl) override;
};

struct EnvHandler : ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
{
    AE_COM_BOILERPLATE
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment *env) override;
};

static MsgHandler g_msgHandler;
static CtrlHandler g_ctrlHandler;
static EnvHandler g_envHandler;

HRESULT STDMETHODCALLTYPE CtrlHandler::Invoke(HRESULT hr, ICoreWebView2Controller *ctrl)
{
    if (FAILED(hr) || !ctrl)
    {
        MessageBoxW(g_hwnd, L"Could not create the WebView2 control.", L"Audio Enhancer", MB_ICONERROR);
        return hr;
    }
    g_ctrl = ctrl;
    g_ctrl->AddRef();
    g_ctrl->get_CoreWebView2(&g_web);

    ICoreWebView2Settings *st = nullptr;
    if (SUCCEEDED(g_web->get_Settings(&st)) && st)
    {
        st->put_AreDefaultContextMenusEnabled(FALSE);
        st->put_IsZoomControlEnabled(FALSE);
        st->put_IsStatusBarEnabled(FALSE);
        st->Release();
    }

    EventRegistrationToken tok;
    g_web->add_WebMessageReceived(&g_msgHandler, &tok);

    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_ctrl->put_Bounds(rc);

    g_web->Navigate(pageUrl().c_str());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE EnvHandler::Invoke(HRESULT hr, ICoreWebView2Environment *env)
{
    if (FAILED(hr) || !env)
    {
        MessageBoxW(g_hwnd,
                    L"WebView2 Runtime is not available.\nInstall it from: "
                    L"https://developer.microsoft.com/microsoft-edge/webview2/",
                    L"Audio Enhancer", MB_ICONERROR);
        return hr;
    }
    env->CreateCoreWebView2Controller(g_hwnd, &g_ctrlHandler);
    return S_OK;
}

static bool initWebView()
{
    HMODULE loader = LoadLibraryW((exeDir() + L"\\WebView2Loader.dll").c_str());
    if (!loader)
    {
        MessageBoxW(g_hwnd, L"WebView2Loader.dll not found next to the exe.", L"Audio Enhancer", MB_ICONERROR);
        return false;
    }
    typedef HRESULT(__stdcall * CreateEnvFn)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
                                             ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
    CreateEnvFn createEnv =
        (CreateEnvFn)(void *)GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!createEnv)
    {
        MessageBoxW(g_hwnd, L"Invalid WebView2Loader.dll.", L"Audio Enhancer", MB_ICONERROR);
        return false;
    }

    // Keep WebView2's profile in %LOCALAPPDATA% (the exe folder may not be writable).
    std::wstring userData;
    wchar_t local[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0)
    {
        std::wstring base = std::wstring(local) + L"\\AudioEnhancer";
        CreateDirectoryW(base.c_str(), nullptr);
        userData = base + L"\\WebView2";
        CreateDirectoryW(userData.c_str(), nullptr);
    }

    HRESULT hr = createEnv(nullptr, userData.empty() ? nullptr : userData.c_str(), nullptr, &g_envHandler);
    if (FAILED(hr))
    {
        MessageBoxW(g_hwnd, L"Could not start WebView2. Is the WebView2 Runtime installed?",
                    L"Audio Enhancer", MB_ICONERROR);
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ tray
static bool isAutostart()
{
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

static void setAutostart(bool on)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    if (on)
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring v = L"\"" + std::wstring(exe) + L"\" --tray";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE *)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

static void fillTip(NOTIFYICONDATAW &nid)
{
    std::wstring tip = L"Audio Enhancer";
    if (g_running)
    {
        tip += L" - Active\n";
        const AudioDevice *o = findById(g_outputId);
        if (o)
        {
            tip += o->name;
            tip += L" \u00B7 ";
        }
        tip += effectName(g_effect);
    }
    else
    {
        tip += L" - Off";
    }
    lstrcpynW(nid.szTip, tip.c_str(), (int)(sizeof(nid.szTip) / sizeof(wchar_t)));
}

static bool addTray()
{
    if (g_trayAdded || !g_hwnd)
        return g_trayAdded;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_running ? g_trayIconOn : g_trayIconOff;
    fillTip(nid);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) == TRUE;
    return g_trayAdded;
}

static void removeTray()
{
    if (!g_trayAdded)
        return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

static void updateTray()
{
    if (!g_trayAdded)
        return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_TIP | NIF_ICON;
    nid.hIcon = g_running ? g_trayIconOn : g_trayIconOff; // dimmed icon when off
    fillTip(nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void showTrayBalloon(const wchar_t *title, const wchar_t *text)
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, title, 64);
    lstrcpynW(nid.szInfo, text, 256);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void showTrayHint()
{
    showTrayBalloon(L"Audio Enhancer is still running",
                    L"Click the tray icon to open it again. Right-click it and choose Exit to quit.");
}

static void hideToTray()
{
    ShowWindow(g_hwnd, SW_HIDE);
    if (g_ctrl)
        g_ctrl->put_IsVisible(FALSE); // no rendering while hidden
    if (!g_trayHintShown)
    {
        g_trayHintShown = true;
        g_dirty = true;
        showTrayHint();
    }
}

static void showMainWindow()
{
    ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
    if (g_ctrl)
        g_ctrl->put_IsVisible(TRUE);
    SetForegroundWindow(g_hwnd);
}

static void setEffectFromTray(int e)
{
    g_effect = clampInt(e, 0, 2);
    g_engine.setEffect(g_effect);
    g_dirty = true;
    postState(); // update the dropdown in the UI
    if (g_running)
        postCurrentStatus();
}

static void showTrayMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    HMENU fx = CreatePopupMenu();
    for (int i = 0; i < 3; i++)
        AppendMenuW(fx, MF_STRING, ID_TRAY_EFFECT0 + i, effectName(i));
    CheckMenuRadioItem(fx, 0, 2, (UINT)g_effect, MF_BYPOSITION);

    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open Audio Enhancer");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_power ? MF_CHECKED : 0), ID_TRAY_POWER, L"Enabled");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)fx, L"Audio Effect");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (isAutostart() ? MF_CHECKED : 0), ID_TRAY_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetMenuDefaultItem(menu, ID_TRAY_OPEN, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // required, otherwise the menu does not close when clicking elsewhere
    const UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0,
                                          hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu); // also destroys the submenu

    switch (cmd)
    {
    case ID_TRAY_OPEN:
        showMainWindow();
        break;
    case ID_TRAY_POWER:
        g_savedPower = !g_power;
        g_dirty = true;
        setPower(g_savedPower); // also updates the toggle in the UI
        break;
    case ID_TRAY_EFFECT0:
    case ID_TRAY_EFFECT0 + 1:
    case ID_TRAY_EFFECT0 + 2:
        setEffectFromTray((int)cmd - ID_TRAY_EFFECT0);
        break;
    case ID_TRAY_AUTOSTART:
        setAutostart(!isAutostart());
        postSettings(); // keep the Settings page toggle in sync
        break;
    case ID_TRAY_EXIT:
        g_quitting = true;
        DestroyWindow(hwnd);
        break;
    }
}

// ------------------------------------------------------------------ window
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // registered (non-constant) messages
    if (g_wmTaskbarCreated && msg == g_wmTaskbarCreated)
    {
        g_trayAdded = false; // Explorer restarted, the old icon is gone
        addTray();
        return 0;
    }
    if (g_wmShowApp && msg == g_wmShowApp)
    {
        showMainWindow();
        return 0;
    }

    switch (msg)
    {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED)
        {
            if (g_trayAdded)
                hideToTray(); // e.g. "Show desktop" minimized us
            return 0;
        }
        if (g_ctrl)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_ctrl->put_Bounds(rc);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE && g_trayAdded)
        {
            hideToTray(); // minimize = go to the tray, no taskbar button
            return 0;
        }
        break;
    case WM_CLOSE:
        if (!g_quitting && g_trayAdded)
        {
            hideToTray(); // X = go to the tray, Exit is in the tray menu
            return 0;
        }
        break; // no tray -> normal close
    case WM_UPDATE_MSG:
    {
        std::wstring *json = (std::wstring *)lp;
        if (json)
        {
            postJson(*json);
            delete json;
        }
        if (wp == 1 && g_trayAdded && !IsWindowVisible(hwnd))
        { // found a new version while hidden in the tray
            std::wstring text;
            {
                std::lock_guard<std::mutex> lock(g_updMutex);
                text = L"Version " + g_upd.tag + L" is available. Open Audio Enhancer > Settings to update.";
            }
            showTrayBalloon(L"Audio Enhancer update available", text.c_str());
        }
        return 0;
    }
    case WM_UPDATE_LAUNCH:
        launchInstaller();
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp))
        {
        case WM_LBUTTONUP:
            showMainWindow();
            break;
        case WM_RBUTTONUP:
            showTrayMenu(hwnd);
            break;
        }
        return 0;
    case WM_QUERYENDSESSION:
        // An installer (Restart Manager) asks us to close for an update: X must really close now, not hide.
        if (lp & ENDSESSION_CLOSEAPP)
            g_quitting = true;
        return TRUE;
    case WM_ENDSESSION:
        if (wp)
        { // Windows is shutting down / logging off / installer update: give the sound device back first
            saveSettings();
            stopEngine();
            removeTray();
            if (lp & ENDSESSION_CLOSEAPP)
            { // closed by an installer -> really exit
                g_quitting = true;
                DestroyWindow(hwnd);
            }
        }
        return 0;
    case WM_TIMER:
        if (!g_trayAdded)
            addTray(); // Explorer was not ready yet (e.g. start with Windows)
        if (g_running && !g_engine.alive())
        { // e.g. Bluetooth headphones disconnected
            g_engine.stop();
            g_running = false;
            restoreDefault();
            g_power = false;
            postPower();
            postStatus(L"error", L"Output disconnected",
                       L"The selected output stopped. Reconnect it and turn the enhancer on.");
        }
        refreshDevices(false); // auto-detect newly connected / removed devices
        if (g_startHidden && !g_booted)
            bootEngine(); // hidden start: do not wait for the page to say "ready"
        if (g_checkUpdates && g_booted && g_lastUpdateCheck != 0 &&
            GetTickCount64() - g_lastUpdateCheck > 12ULL * 60 * 60 * 1000)
            startUpdateCheck(true); // the app can stay in the tray for days: look again every 12 hours
        if (g_dirty)
        {
            saveSettings();
            g_dirty = false;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        saveSettings();
        stopEngine(); // restores the previous Windows default output
        removeTray();
        if (g_web)
        {
            g_web->Release();
            g_web = nullptr;
        }
        if (g_ctrl)
        {
            g_ctrl->Close();
            g_ctrl->Release();
            g_ctrl = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow)
{
    const bool startHidden = wcsstr(GetCommandLineW(), L"--tray") != nullptr;

    // Single instance: the app can sit hidden in the tray, so a second launch must not start another engine.
    g_wmShowApp = RegisterWindowMessageW(L"AudioEnhancer_ShowWindow");
    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\AudioEnhancer_SingleInstance");
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (!startHidden)
        { // user launched it again -> bring the running one up
            AllowSetForegroundWindow(ASFW_ANY);
            HWND other = FindWindowW(L"AudioEnhancerWnd", nullptr);
            if (other)
                PostMessageW(other, g_wmShowApp, 0, 0);
        }
        CloseHandle(g_mutex);
        return 0;
    }
    g_startHidden = startHidden;
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    loadSettings();
    applyToneToEngine();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AudioEnhancerWnd";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1)); // from src/app.rc
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);
    g_trayIconOn = wc.hIconSm; // resource 1 = normal icon, resource 2 = dimmed "off" icon (src/app.rc)
    g_trayIconOff = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(2), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                      GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_trayIconOff)
        g_trayIconOff = g_trayIconOn;

    // 700x640 CSS pixels, scaled for the screen DPI and limited to the visible work area.
    HDC dc = GetDC(nullptr);
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    const double scale = dpi / 96.0;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, (LONG)(700 * scale), (LONG)(640 * scale)};
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left;
    int h = r.bottom - r.top;

    RECT wa = {0, 0, 1280, 720};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (h > wa.bottom - wa.top)
        h = wa.bottom - wa.top;
    if (w > wa.right - wa.left)
        w = wa.right - wa.left;
    const int x = wa.left + ((wa.right - wa.left) - w) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Audio Enhancer", style, x, y, w, h, nullptr, nullptr,
                             hInst, nullptr);
    addTray(); // if Explorer is not ready yet, the timer / TaskbarCreated adds it later
    if (g_startHidden)
    {
        ShowWindow(g_hwnd, SW_HIDE); // autostart: go straight to the tray
    }
    else
    {
        ShowWindow(g_hwnd, nShow);
        UpdateWindow(g_hwnd);
    }

    refreshDevices(false);
    SetTimer(g_hwnd, TIMER_ID, 2500, nullptr);

    if (!initWebView())
        return 1;

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    if (g_mutex)
        CloseHandle(g_mutex);
    return (int)m.wParam;
}