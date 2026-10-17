// camstudio-dshow-consumer: opens the "Cam Studio" camera through DirectShow
// (ksproxy), the way Discord's capture code does, and keeps the graph running
// without reacting when the camera goes away. Used to reproduce the virtual
// camera getting refused (0x80070005) after a source switch.
//
//   camstudio-dshow-consumer [--seconds N]
#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

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
    for (int i = 1; i < argc; ++i)
        if (std::wstring(argv[i]) == L"--seconds" && i + 1 < argc) seconds = _wtoi(argv[++i]);

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
    // Render to the default video renderer (a small window), like a preview.
    HRESULT hr = builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, camera.Get(), nullptr, nullptr);
    wprintf(L"RenderStream 0x%08X\n", static_cast<unsigned>(hr));
    ComPtr<IMediaControl> control;
    graph.As(&control);
    hr = control->Run();
    wprintf(L"Run 0x%08X\n", static_cast<unsigned>(hr));
    fflush(stdout);

    ComPtr<IMediaEvent> events;
    graph.As(&events);
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000)
    {
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
