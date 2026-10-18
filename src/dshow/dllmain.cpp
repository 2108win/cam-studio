// COM entry points and registration of the DirectShow source filter (src/dshow/SourceFilter.h).
#include "SourceFilter.h"

#include <wrl/module.h>

#include <string>

HMODULE g_hModule = nullptr;

namespace
{
class FilterFactory final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IClassFactory>
{
public:
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        RETURN_HR_IF_NULL(E_POINTER, ppv);
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        ComPtr<SourceFilter> filter;
        RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<SourceFilter>(&filter));
        return filter->QueryInterface(riid, ppv);
    }
    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        auto& module = Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule();
        lock ? module.IncrementObjectCount() : module.DecrementObjectCount();
        return S_OK;
    }
};

// {860BB310-5D01-11d0-BD3B-00A0C911CE86}: CLSID_VideoInputDeviceCategory.
const wchar_t kCategoryInstance[] =
    L"Software\\Classes\\CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\" CAMSTUDIO_DSHOW_CLSID_STRING;
const wchar_t kClsidKey[] = L"Software\\Classes\\CLSID\\" CAMSTUDIO_DSHOW_CLSID_STRING;

bool Elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

LSTATUS SetString(HKEY key, const wchar_t* name, const std::wstring& value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

// The filter's merit, pin and media types for the filter mapper (FilterData under the category
// instance, which intelligent-connect and some apps read).
HRESULT RegisterWithMapper(bool unregister)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComPtr<IFilterMapper2> mapper;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mapper));
    if (SUCCEEDED(hr))
    {
        if (unregister)
        {
            hr = mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, CAMSTUDIO_DSHOW_CLSID_STRING, CLSID_CamStudioDShow);
        }
        else
        {
            const GUID subtype = FourccSubtype(kFourccNV12);
            REGPINTYPES types[] = { { &MEDIATYPE_Video, &subtype } };
            REGFILTERPINS pins[1] = {};
            pins[0].strName = const_cast<LPWSTR>(L"Capture");
            pins[0].bRendered = FALSE;
            pins[0].bOutput = TRUE;
            pins[0].bZero = FALSE;
            pins[0].bMany = FALSE;
            pins[0].clsConnectsToFilter = &CLSID_NULL;
            pins[0].strConnectsToPin = nullptr;
            pins[0].nMediaTypes = 1;
            pins[0].lpMediaType = types;
            REGFILTER2 rf{};
            rf.dwVersion = 1;
            rf.dwMerit = MERIT_DO_NOT_USE; // a capture device: picked by name, never by intelligent connect
            rf.cPins = 1;
            rf.rgPins = pins;
            hr = mapper->RegisterFilter(CLSID_CamStudioDShow, CAMSTUDIO_DSHOW_FRIENDLY_NAME, nullptr,
                                        &CLSID_VideoInputDeviceCategory, CAMSTUDIO_DSHOW_CLSID_STRING, &rf);
        }
    }
    mapper.Reset();
    if (SUCCEEDED(init)) CoUninitialize();
    return hr;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow()
{
    // Stay loaded: the streaming thread runs our code, and an unload under it would crash the app.
    return S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule();
    RETURN_HR_IF_NULL(E_POINTER, ppv);
    *ppv = nullptr;
    if (clsid != CLSID_CamStudioDShow) return CLASS_E_CLASSNOTAVAILABLE;
    ComPtr<FilterFactory> factory = Microsoft::WRL::Make<FilterFactory>();
    RETURN_HR_IF_NULL(E_OUTOFMEMORY, factory.Get());
    return factory->QueryInterface(riid, ppv);
}

// Machine-wide (HKLM) from an elevated regsvr32 (the installer); for the current user (HKCU)
// otherwise, which is enough to develop and test without elevation. The keys are created here
// first so the filter mapper's writes (through HKCR) land in the same hive.
STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_hModule, path, ARRAYSIZE(path))) return HRESULT_FROM_WIN32(GetLastError());
    const HKEY root = Elevated() ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;

    HKEY clsidKey = nullptr;
    LSTATUS st = RegCreateKeyExW(root, kClsidKey, 0, nullptr, 0, KEY_WRITE, nullptr, &clsidKey, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    SetString(clsidKey, nullptr, L"Cam Studio DirectShow Camera");
    HKEY inproc = nullptr;
    st = RegCreateKeyExW(clsidKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc, nullptr);
    if (st == ERROR_SUCCESS)
    {
        SetString(inproc, nullptr, path);
        st = SetString(inproc, L"ThreadingModel", L"Both");
        RegCloseKey(inproc);
    }
    RegCloseKey(clsidKey);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    // The capture-device entry apps enumerate (ICreateDevEnum).
    HKEY instance = nullptr;
    st = RegCreateKeyExW(root, kCategoryInstance, 0, nullptr, 0, KEY_WRITE, nullptr, &instance, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    SetString(instance, L"CLSID", CAMSTUDIO_DSHOW_CLSID_STRING);
    st = SetString(instance, L"FriendlyName", CAMSTUDIO_DSHOW_FRIENDLY_NAME);
    RegCloseKey(instance);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);

    RegisterWithMapper(false); // FilterData; the entry above works without it
    return S_OK;
}

STDAPI DllUnregisterServer()
{
    RegisterWithMapper(true);
    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
    {
        if (root == HKEY_LOCAL_MACHINE && !Elevated()) continue;
        RegDeleteTreeW(root, kCategoryInstance);
        RegDeleteTreeW(root, kClsidKey);
    }
    return S_OK;
}
