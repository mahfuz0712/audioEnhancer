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
// JS -> C++ (strings):  "ready"  "output|<deviceId>"  "effect|<Normal|3D|Balanced>"  "power|<1|0>"
//                       "volume|<0..100>"  "bass|<-10..10>"  "treble|<-10..10>"
// C++ -> JS (JSON):     {"type":"devices",...}  {"type":"state",...}  {"type":"power","value":bool}
//                       {"type":"status",...}

#include <windows.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "WebView2.h"
#include "audio_engine.h"

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
}

static void postPower()
{
    postJson(std::wstring(L"{\"type\":\"power\",\"value\":") + (g_power ? L"true" : L"false") + L"}");
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
            g_booted = true;
            if (g_power && !startEngine())
                g_power = false;
        }
        else
        {
            postCurrentStatus();
        }
        postPower();
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

// ------------------------------------------------------------------ window
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE:
        if (g_ctrl)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_ctrl->put_Bounds(rc);
        }
        return 0;
    case WM_TIMER:
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
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

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
    return (int)m.wParam;
}