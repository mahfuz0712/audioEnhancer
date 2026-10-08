#pragma once
// audio_engine.h - WASAPI backend
//   * enumerateOutputDevices(): dynamic list of active playback devices
//   * getDefaultOutputId() / setDefaultOutput(): read / change the Windows default output
//   * AudioEngine: captures audio (loopback) from a "source" device (the virtual cable),
//     applies the effect, and plays it on the real output device.

#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "dsp.h"

struct AudioDevice
{
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
    int formFactor = -1; // Windows EndpointFormFactor: 1 speakers, 3 headphones, 5 headset, 9 HDMI...
};

// GUIDs defined here so we don't depend on how the MinGW headers/libs define them.
namespace aeguid
{
    static const GUID CLSID_Enumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    static const GUID IID_Enumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
    static const GUID IID_Client = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
    static const GUID IID_Render = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
    static const GUID IID_Capture = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
    // Undocumented (but widely used) policy-config interface, used to change the default device.
    static const GUID CLSID_PolicyConfig = {0x870AF99C, 0x171D, 0x4F9E, {0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9}};
    static const GUID IID_PolicyConfig = {0xF8679F50, 0x850A, 0x41CF, {0x9C, 0x72, 0x43, 0x0F, 0x29, 0x02, 0x90, 0xC8}};
    static const PROPERTYKEY PKEY_FriendlyName =
        {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
    static const PROPERTYKEY PKEY_FormFactor =
        {{0x1DA5D803, 0xD492, 0x4EDD, {0x8C, 0x23, 0xE0, 0xC0, 0xFF, 0xEE, 0x7F, 0x0E}}, 0};
} // namespace aeguid

// Only SetDefaultEndpoint is called; the other slots just keep the vtable layout correct.
struct IAePolicyConfig : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

template <class T>
inline void aeRelease(T *&p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

// Call from a thread that has called CoInitializeEx.
inline std::vector<AudioDevice> enumerateOutputDevices()
{
    std::vector<AudioDevice> result;

    IMMDeviceEnumerator *en = nullptr;
    if (FAILED(CoCreateInstance(aeguid::CLSID_Enumerator, nullptr, CLSCTX_ALL,
                                aeguid::IID_Enumerator, (void **)&en)))
        return result;

    std::wstring defaultId;
    IMMDevice *def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)))
    {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id)))
        {
            defaultId = id;
            CoTaskMemFree(id);
        }
        aeRelease(def);
    }

    IMMDeviceCollection *col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)))
    {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i)
        {
            IMMDevice *d = nullptr;
            if (FAILED(col->Item(i, &d)))
                continue;

            AudioDevice ad;
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id)))
            {
                ad.id = id;
                CoTaskMemFree(id);
            }

            IPropertyStore *ps = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)))
            {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(aeguid::PKEY_FriendlyName, &pv)) &&
                    pv.vt == VT_LPWSTR && pv.pwszVal)
                    ad.name = pv.pwszVal;
                PropVariantClear(&pv);

                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(aeguid::PKEY_FormFactor, &pv)) && pv.vt == VT_UI4)
                    ad.formFactor = (int)pv.ulVal;
                PropVariantClear(&pv);
                aeRelease(ps);
            }
            if (ad.name.empty())
                ad.name = L"Unknown device";
            ad.isDefault = (ad.id == defaultId);
            result.push_back(ad);
            aeRelease(d);
        }
        aeRelease(col);
    }
    aeRelease(en);
    return result;
}

inline std::wstring getDefaultOutputId()
{
    std::wstring result;
    IMMDeviceEnumerator *en = nullptr;
    if (FAILED(CoCreateInstance(aeguid::CLSID_Enumerator, nullptr, CLSCTX_ALL,
                                aeguid::IID_Enumerator, (void **)&en)))
        return result;
    IMMDevice *def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)))
    {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id)))
        {
            result = id;
            CoTaskMemFree(id);
        }
        aeRelease(def);
    }
    aeRelease(en);
    return result;
}

// Makes 'id' the Windows default playback device (console + multimedia roles).
inline bool setDefaultOutput(const std::wstring &id)
{
    if (id.empty())
        return false;
    IAePolicyConfig *pc = nullptr;
    if (FAILED(CoCreateInstance(aeguid::CLSID_PolicyConfig, nullptr, CLSCTX_ALL,
                                aeguid::IID_PolicyConfig, (void **)&pc)))
        return false;
    HRESULT h1 = pc->SetDefaultEndpoint(id.c_str(), eConsole);
    HRESULT h2 = pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
    pc->Release();
    return SUCCEEDED(h1) && SUCCEEDED(h2);
}

class AudioEngine
{
public:
    ~AudioEngine() { stop(); }

    void setEffect(int e) { effect_.store(e); }
    void setVolume(int pct) { volume_.store(pct); } // 0..100
    void setBass(int db) { bass_.store(db); }       // about -12..+12 dB
    void setTreble(int db) { treble_.store(db); }   // about -12..+12 dB
    bool alive() const { return alive_.load(); }

    // Returns true on success. On failure 'err' holds a message.
    bool start(const std::wstring &sourceId, const std::wstring &outputId, std::wstring &err)
    {
        stop();
        stopFlag_ = false;
        alive_ = false;

        std::promise<std::wstring> prom;
        std::future<std::wstring> fut = prom.get_future();
        th_ = std::thread(&AudioEngine::worker, this, sourceId, outputId, std::move(prom));

        std::wstring r = fut.get();
        if (!r.empty())
        {
            th_.join();
            err = r;
            return false;
        }
        return true;
    }

    void stop()
    {
        stopFlag_ = true;
        if (th_.joinable())
            th_.join();
        alive_ = false;
    }

private:
    void worker(std::wstring srcId, std::wstring outId, std::promise<std::wstring> prom)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        timeBeginPeriod(1);

        IMMDeviceEnumerator *en = nullptr;
        IMMDevice *src = nullptr, *dst = nullptr;
        IAudioClient *cap = nullptr, *ren = nullptr;
        IAudioCaptureClient *capSvc = nullptr;
        IAudioRenderClient *renSvc = nullptr;

        auto cleanup = [&]()
        {
            if (cap)
                cap->Stop();
            if (ren)
                ren->Stop();
            aeRelease(capSvc);
            aeRelease(renSvc);
            aeRelease(cap);
            aeRelease(ren);
            aeRelease(src);
            aeRelease(dst);
            aeRelease(en);
            timeEndPeriod(1);
            CoUninitialize();
        };
        auto fail = [&](const wchar_t *msg, HRESULT hr)
        {
            wchar_t buf[256];
            swprintf(buf, 256, L"%ls (0x%08lX)", msg, (unsigned long)hr);
            prom.set_value(buf);
            cleanup();
        };

        HRESULT hr = CoCreateInstance(aeguid::CLSID_Enumerator, nullptr, CLSCTX_ALL,
                                      aeguid::IID_Enumerator, (void **)&en);
        if (FAILED(hr))
            return fail(L"Cannot create device enumerator", hr);

        hr = en->GetDevice(srcId.c_str(), &src);
        if (FAILED(hr))
            return fail(L"Source device not found", hr);
        hr = en->GetDevice(outId.c_str(), &dst);
        if (FAILED(hr))
            return fail(L"Output device not found", hr);

        hr = src->Activate(aeguid::IID_Client, CLSCTX_ALL, nullptr, (void **)&cap);
        if (FAILED(hr))
            return fail(L"Cannot open source device", hr);
        hr = dst->Activate(aeguid::IID_Client, CLSCTX_ALL, nullptr, (void **)&ren);
        if (FAILED(hr))
            return fail(L"Cannot open output device", hr);

        // Both sides use float32 / stereo / 48 kHz. Windows converts to/from the
        // device's native format for us (AUTOCONVERTPCM + SRC_DEFAULT_QUALITY).
        const UINT32 kRate = 48000;
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 2 * sizeof(float);
        wf.nAvgBytesPerSec = kRate * wf.nBlockAlign;
        wf.cbSize = 0;

        const DWORD conv = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        const REFERENCE_TIME bufDur = 1000000; // 100 ms (100-ns units)

        hr = cap->Initialize(AUDCLNT_SHAREMODE_SHARED, conv | AUDCLNT_STREAMFLAGS_LOOPBACK,
                             bufDur, 0, &wf, nullptr);
        if (FAILED(hr))
            return fail(L"Cannot start loopback capture", hr);
        hr = ren->Initialize(AUDCLNT_SHAREMODE_SHARED, conv, bufDur, 0, &wf, nullptr);
        if (FAILED(hr))
            return fail(L"Cannot start output stream", hr);

        UINT32 renFrames = 0;
        ren->GetBufferSize(&renFrames);

        hr = cap->GetService(aeguid::IID_Capture, (void **)&capSvc);
        if (FAILED(hr))
            return fail(L"Capture service error", hr);
        hr = ren->GetService(aeguid::IID_Render, (void **)&renSvc);
        if (FAILED(hr))
            return fail(L"Render service error", hr);

        // Small pre-buffer of silence so the output doesn't underrun immediately.
        {
            const UINT32 pre = kRate * 30 / 1000;
            BYTE *p = nullptr;
            if (SUCCEEDED(renSvc->GetBuffer(pre, &p)))
                renSvc->ReleaseBuffer(pre, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        hr = cap->Start();
        if (FAILED(hr))
            return fail(L"Capture start failed", hr);
        hr = ren->Start();
        if (FAILED(hr))
            return fail(L"Output start failed", hr);

        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        Spatializer fx((float)kRate);
        ToneControl tone((float)kRate);
        alive_ = true;
        prom.set_value(L""); // tell start() everything is OK

        bool ok = true;
        while (!stopFlag_ && ok)
        {
            Sleep(5);

            UINT32 pkt = 0;
            while (ok && SUCCEEDED(capSvc->GetNextPacketSize(&pkt)) && pkt > 0)
            {
                BYTE *data = nullptr;
                UINT32 n = 0;
                DWORD flags = 0;
                if (FAILED(capSvc->GetBuffer(&data, &n, &flags, nullptr, nullptr)))
                {
                    ok = false;
                    break;
                }

                UINT32 pad = 0;
                if (FAILED(ren->GetCurrentPadding(&pad)))
                { // device removed / disconnected
                    capSvc->ReleaseBuffer(n);
                    ok = false;
                    break;
                }

                const UINT32 freeFrames = renFrames > pad ? renFrames - pad : 0;
                const UINT32 toWrite = n < freeFrames ? n : freeFrames;

                if (toWrite > 0)
                {
                    BYTE *out = nullptr;
                    if (SUCCEEDED(renSvc->GetBuffer(toWrite, &out)))
                    {
                        float *of = reinterpret_cast<float *>(out);
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                            std::memset(of, 0, toWrite * wf.nBlockAlign);
                        else
                            std::memcpy(of, data, toWrite * wf.nBlockAlign);

                        fx.process(of, toWrite, static_cast<Effect>(effect_.load()));
                        tone.process(of, toWrite, bass_.load(), treble_.load(), volume_.load());
                        renSvc->ReleaseBuffer(toWrite, 0);
                    }
                }
                capSvc->ReleaseBuffer(n);
            }
        }

        alive_ = false;
        cleanup();
    }

    std::thread th_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> alive_{false};
    std::atomic<int> effect_{0};
    std::atomic<int> volume_{100};
    std::atomic<int> bass_{0};
    std::atomic<int> treble_{0};
};