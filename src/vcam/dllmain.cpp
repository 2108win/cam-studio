#include "Activate.h"

#include <wrl/module.h>

HMODULE g_hModule = nullptr;

namespace
{
class ActivateFactory
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IClassFactory>
{
public:
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        RETURN_HR_IF_NULL(E_POINTER, ppv);
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        ComPtr<Activate> activate;
        RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<Activate>(&activate));
        return activate->QueryInterface(riid, ppv);
    }

    IFACEMETHODIMP LockServer(BOOL lock) override
    {
        auto& module = Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule();
        lock ? module.IncrementObjectCount() : module.DecrementObjectCount();
        return S_OK;
    }
};

const wchar_t kClsidKey[] = L"Software\\Classes\\CLSID\\" CAMSTUDIO_VCAM_CLSID_STRING;
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
    // Stay loaded: the stream worker thread runs our code, and unloading
    // under it would crash Frame Server. The cost is one resident DLL.
    return S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    // Make sure the WRL module exists so object counting works.
    Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule();
    RETURN_HR_IF_NULL(E_POINTER, ppv);
    *ppv = nullptr;
    if (clsid != CLSID_CamStudioVCam) return CLASS_E_CLASSNOTAVAILABLE;
    ComPtr<ActivateFactory> factory = Microsoft::WRL::Make<ActivateFactory>();
    RETURN_HR_IF_NULL(E_OUTOFMEMORY, factory.Get());
    return factory->QueryInterface(riid, ppv);
}

// Registration goes to HKLM: Frame Server runs as LocalService and cannot
// see per-user (HKCU) COM registrations. Requires an elevated regsvr32.
STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_hModule, path, ARRAYSIZE(path))) return HRESULT_FROM_WIN32(GetLastError());

    HKEY clsidKey = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kClsidKey, 0, nullptr, 0, KEY_WRITE, nullptr, &clsidKey, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    const wchar_t name[] = L"Cam Studio Virtual Camera";
    RegSetValueExW(clsidKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(name), sizeof(name));

    HKEY inproc = nullptr;
    st = RegCreateKeyExW(clsidKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc, nullptr);
    if (st == ERROR_SUCCESS)
    {
        RegSetValueExW(inproc, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path),
            static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
        const wchar_t model[] = L"Both";
        st = RegSetValueExW(inproc, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(model), sizeof(model));
        RegCloseKey(inproc);
    }
    RegCloseKey(clsidKey);
    return HRESULT_FROM_WIN32(st);
}

STDAPI DllUnregisterServer()
{
    LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey);
    return (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(st);
}
