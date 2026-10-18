// camstudio-dshow-consumer: opens the "Cam Studio" camera through DirectShow
// (ksproxy), the way Discord's capture code does, and keeps the graph running
// without reacting when the camera goes away. Used to reproduce the virtual
// camera getting refused (0x80070005) after a source switch.
//
//   camstudio-dshow-consumer [--seconds N] [--grab [--fourcc YUY2]]
//
// --grab: no window; the frames go through a SampleGrabber into a null renderer, and once a
// second the picture is measured (mean and spread of luma, change since the last look). Checks
// the Windows 10 DirectShow filter (src/dshow) delivers real frames, not "no signal".
#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// qedit.h is not in the SDK any more; the SampleGrabber still ships with Windows.
// {C1F400A0-3F08-11d3-9F0B-006008039E37}
static const GUID CLSID_SampleGrabberQ = { 0xC1F400A0, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };
// {C1F400A4-3F08-11d3-9F0B-006008039E37}
static const GUID CLSID_NullRendererQ = { 0xC1F400A4, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };
struct ISampleGrabberCBQ;
MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabberQ : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCBQ*, long) = 0;
};

#pragma comment(lib, "strmiids")

using Microsoft::WRL::ComPtr;

static ComPtr<IBaseFilter> FindCamStudio(std::wstring& name)
{
    ComPtr<ICreateDevEnum> devEnum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum)))) return nullptr;
    ComPtr<IEnumMoniker> monikers;
    if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) != S_OK) return nullptr;
    ComPtr<IMoniker> m;
    while (monikers->Next(1, &m, nullptr) == S_OK)
    {
        ComPtr<IPropertyBag> bag;
        if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))))
        {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR &&
                std::wstring(v.bstrVal).rfind(L"Cam Studio", 0) == 0)
            {
                name = v.bstrVal;
                VariantClear(&v);
                ComPtr<IBaseFilter> filter;
                if (SUCCEEDED(m->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter)))) return filter;
            }
            VariantClear(&v);
        }
        m.Reset();
    }
    return nullptr;
}

int wmain(int argc, wchar_t** argv)
{
    int seconds = 120;
    bool grab = false;
    std::wstring fourcc; // --fourcc NV12|YUY2|I420: ask the grabber for that format (--grab)
    for (int i = 1; i < argc; ++i)
    {
        if (std::wstring(argv[i]) == L"--seconds" && i + 1 < argc) seconds = _wtoi(argv[++i]);
        else if (std::wstring(argv[i]) == L"--grab") grab = true;
        else if (std::wstring(argv[i]) == L"--fourcc" && i + 1 < argc) fourcc = argv[++i];
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring name;
    ComPtr<IBaseFilter> camera = FindCamStudio(name);
    if (!camera) { wprintf(L"Cam Studio not found in DirectShow\n"); return 1; }
    wprintf(L"Opened %s via DirectShow\n", name.c_str());

    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> builder;
    CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
    CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder));
    builder->SetFiltergraph(graph.Get());
    graph->AddFilter(camera.Get(), L"Cam Studio");
    ComPtr<ISampleGrabberQ> grabber;
    ComPtr<IBaseFilter> grabberFilter, sink;
    if (grab)
    {
        CoCreateInstance(CLSID_SampleGrabberQ, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&grabberFilter));
        CoCreateInstance(CLSID_NullRendererQ, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sink));
        if (!grabberFilter || !sink || FAILED(grabberFilter.As(&grabber))) { wprintf(L"SampleGrabber not available\n"); return 1; }
        AM_MEDIA_TYPE want{};
        want.majortype = MEDIATYPE_Video;
        want.formattype = FORMAT_VideoInfo;
        if (fourcc.size() == 4)
        {
            char c[4] = { static_cast<char>(fourcc[0]), static_cast<char>(fourcc[1]), static_cast<char>(fourcc[2]), static_cast<char>(fourcc[3]) };
            uint32_t code = 0;
            memcpy(&code, c, 4);
            want.subtype = { code, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
        }
        grabber->SetMediaType(&want);
        grabber->SetBufferSamples(TRUE);
        graph->AddFilter(grabberFilter.Get(), L"Grabber");
        graph->AddFilter(sink.Get(), L"Null");
    }
    // Render to the default video renderer (a small window), like a preview; or through the grabber.
    HRESULT hr = builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, camera.Get(), grabberFilter.Get(), sink.Get());
    wprintf(L"RenderStream 0x%08X\n", static_cast<unsigned>(hr));
    uint32_t grabW = 0, grabH = 0;
    if (grab && SUCCEEDED(hr))
    {
        AM_MEDIA_TYPE mt{};
        if (SUCCEEDED(grabber->GetConnectedMediaType(&mt)) && mt.formattype == FORMAT_VideoInfo && mt.pbFormat)
        {
            const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
            grabW = static_cast<uint32_t>(vih->bmiHeader.biWidth);
            grabH = static_cast<uint32_t>(vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight : vih->bmiHeader.biHeight);
            wprintf(L"Connected %u x %u fourcc %.4hs\n", grabW, grabH, reinterpret_cast<const char*>(&vih->bmiHeader.biCompression));
            if (mt.pbFormat) CoTaskMemFree(mt.pbFormat);
        }
    }
    ComPtr<IMediaControl> control;
    graph.As(&control);
    hr = control->Run();
    wprintf(L"Run 0x%08X\n", static_cast<unsigned>(hr));
    fflush(stdout);

    ComPtr<IMediaEvent> events;
    graph.As(&events);
    const ULONGLONG start = GetTickCount64();
    std::vector<unsigned char> frame, previous;
    ULONGLONG lastLook = 0;
    while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000)
    {
        if (grab && grabW && GetTickCount64() - lastLook >= 1000)
        {
            lastLook = GetTickCount64();
            long size = 0;
            if (SUCCEEDED(grabber->GetCurrentBuffer(&size, nullptr)) && size > 0)
            {
                frame.resize(static_cast<size_t>(size));
                grabber->GetCurrentBuffer(&size, reinterpret_cast<long*>(frame.data()));
                // Luma: the first w*h bytes of NV12/I420, every other byte of YUY2.
                const size_t n = static_cast<size_t>(grabW) * grabH;
                double sum = 0, sq = 0, diff = 0;
                for (size_t i = 0; i < n && i < frame.size(); ++i)
                {
                    const double v = frame[i];
                    sum += v;
                    sq += v * v;
                    if (previous.size() == frame.size()) diff += std::fabs(v - previous[i]);
                }
                const double mean = sum / n, sd = std::sqrt(std::max(0.0, sq / n - mean * mean));
                wprintf(L"[%6.1f s] frame %ld bytes, luma mean %.1f spread %.1f, change since last look %.1f\n",
                        (lastLook - start) / 1000.0, size, mean, sd, previous.empty() ? 0.0 : diff / n);
                fflush(stdout);
                previous = frame;
            }
            else
            {
                wprintf(L"[%6.1f s] no frame yet\n", (lastLook - start) / 1000.0);
                fflush(stdout);
            }
        }
        long code = 0;
        LONG_PTR p1 = 0, p2 = 0;
        if (events && events->GetEvent(&code, &p1, &p2, 200) == S_OK)
        {
            wprintf(L"[%6.1f s] graph event 0x%X (%lld, %lld)\n", (GetTickCount64() - start) / 1000.0, code,
                static_cast<long long>(p1), static_cast<long long>(p2));
            fflush(stdout);
            events->FreeEventParams(code, p1, p2);
        }
        // Keep holding the graph whatever happens: that is the behaviour under test.
    }
    control->Stop();
    return 0;
}
