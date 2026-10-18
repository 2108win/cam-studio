#include "Discovery.h"
#include "UsbTransport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>

#include <algorithm>
#include <fstream>
#include <map>

#pragma comment(lib, "dnsapi")

namespace phone
{
namespace
{
std::mutex g_lock;
std::map<std::wstring, Endpoint> g_found;   // by service instance name
std::vector<Endpoint> g_manual;
DNS_SERVICE_CANCEL g_browseCancel{};
bool g_browsing = false;
bool g_manualLoaded = false;
// On the heap: its threads must be joined in StopDiscovery, never by a static destructor
// while the DLL unloads.
usb::Watcher* g_usb = nullptr;

std::string Narrow(const wchar_t* w)
{
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void CALLBACK OnResolved(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
{
    std::wstring* key = static_cast<std::wstring*>(context);
    if (status == ERROR_SUCCESS && instance && instance->ip4Address)
    {
        Endpoint e;
        IN_ADDR addr;
        addr.S_un.S_addr = *instance->ip4Address;
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &addr, buf, sizeof(buf));
        e.host = buf;
        e.port = instance->wPort;
        e.name = Narrow(instance->pszInstanceName);
        if (const size_t dot = e.name.find("._camstudio"); dot != std::string::npos) e.name.resize(dot);
        for (DWORD i = 0; i < instance->dwPropertyCount; ++i)
        {
            const std::wstring k = instance->keys[i] ? instance->keys[i] : L"";
            const std::string v = Narrow(instance->values[i]);
            if (k == L"id") e.deviceId = v;
            else if (k == L"name" && !v.empty()) e.name = v;
            else if (k == L"model") e.model = v;
            else if (k == L"os") e.os = v;
        }
        std::lock_guard lk(g_lock);
        g_found[*key] = e;
    }
    if (instance) DnsServiceFreeInstance(instance);
    delete key;
}

void Resolve(const std::wstring& instanceName)
{
    auto* key = new std::wstring(instanceName);
    DNS_SERVICE_RESOLVE_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.QueryName = const_cast<PWSTR>(key->c_str());
    req.pResolveCompletionCallback = OnResolved;
    req.pQueryContext = key;
    DNS_SERVICE_CANCEL cancel{};
    if (DnsServiceResolve(&req, &cancel) != DNS_REQUEST_PENDING) delete key;
}

void CALLBACK OnBrowse(DWORD status, PVOID, PDNS_RECORD records)
{
    if (status == ERROR_SUCCESS)
    {
        for (PDNS_RECORD r = records; r; r = r->pNext)
        {
            if (r->wType != DNS_TYPE_PTR || !r->Data.PTR.pNameHost) continue;
            const std::wstring instance = r->Data.PTR.pNameHost;
            if (r->dwTtl == 0)
            {
                // Goodbye packet: the phone app closed.
                std::lock_guard lk(g_lock);
                g_found.erase(instance);
            }
            else
            {
                Resolve(instance);
            }
        }
    }
    if (records) DnsRecordListFree(records, DnsFreeRecordList);
}

std::wstring ManualFile()
{
    wchar_t path[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    std::wstring dir = std::wstring(path) + L"\\CamStudio";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\phones.txt";
}

void LoadManualLocked()
{
    if (g_manualLoaded) return;
    g_manualLoaded = true;
    std::ifstream f(ManualFile());
    std::string host;
    unsigned port = 0;
    while (f >> host >> port)
        if (port > 0 && port < 65536) g_manual.push_back({ host, static_cast<uint16_t>(port), "", host, "", "", true });
}
} // namespace

void StartDiscovery()
{
    std::lock_guard lk(g_lock);
    if (g_browsing) return;
    DNS_SERVICE_BROWSE_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.QueryName = const_cast<PWSTR>(L"_camstudio._tcp.local");
    req.pBrowseCallback = OnBrowse;
    g_browsing = DnsServiceBrowse(&req, &g_browseCancel) == DNS_REQUEST_PENDING;
    if (!g_usb)
    {
        g_usb = new usb::Watcher;
        g_usb->Start(usb::StartAdbServer, usb::StopIdleAdbServer);
    }
}

void StopDiscovery()
{
    usb::Watcher* watcher = nullptr;
    {
        std::lock_guard lk(g_lock);
        if (g_browsing) DnsServiceBrowseCancel(&g_browseCancel);
        g_browsing = false;
        std::swap(watcher, g_usb);
    }
    if (watcher)
    {
        watcher->Stop();
        delete watcher;
    }
    usb::StopAdbServer(); // only if we started it
}

std::vector<Endpoint> KnownPhones()
{
    std::lock_guard lk(g_lock);
    LoadManualLocked();
    std::vector<Endpoint> out;
    for (const auto& [_, e] : g_found) out.push_back(e);
    for (const Endpoint& m : g_manual)
    {
        // A manual entry that discovery also found shows once.
        const bool dup = std::any_of(out.begin(), out.end(), [&](const Endpoint& e) {
            return e.host == m.host && e.port == m.port;
        });
        if (!dup) out.push_back(m);
    }

    // Phones on a USB cable (docs/protocol.md 1.2). They have no TXT record: the name comes from
    // the phone (Android device_name, the iPhone's name) and the device id from "hello".
    std::vector<Endpoint> cable;
    std::vector<bool> ready; // the app can be reached (adb: USB debugging allowed)
    for (const usb::Device& d : g_usb ? g_usb->Devices() : std::vector<usb::Device>())
    {
        Endpoint e;
        e.transport = d.kind == usb::Kind::Adb ? Transport::Adb : Transport::Usbmux;
        e.serial = d.serial;
        e.port = usb::kCameraPort;
        e.os = d.kind == usb::Kind::Adb ? "android" : "ios";
        e.model = d.model;
        if (d.state != "device") e.name = "Điện thoại Android – chờ cho phép gỡ lỗi USB";
        else if (!d.name.empty()) e.name = d.name;
        else e.name = d.kind == usb::Kind::Adb ? "Điện thoại Android" : "iPhone";
        cable.push_back(e);
        ready.push_back(d.state == "device");
    }
    // Two phones with the same name: the app tells cameras apart by name, so add the serial's end.
    std::vector<bool> twin(cable.size());
    for (size_t i = 0; i < cable.size(); ++i)
        for (size_t j = 0; j < cable.size(); ++j)
            if (i != j && cable[i].name == cable[j].name) twin[i] = true;
    for (size_t i = 0; i < cable.size(); ++i)
    {
        Endpoint& camera = cable[i];
        if (twin[i]) camera.name += " · " + camera.serial.substr(camera.serial.size() > 4 ? camera.serial.size() - 4 : 0);
        out.push_back(camera);
        // The shared screen (docs/protocol.md 1.1) through the same cable, named like the Wi‑Fi one.
        // Listed for every phone ready on the cable: whether it is sharing is not checked, because a
        // test connection makes older phone apps drop the session in progress. Picking it while
        // the phone does not share its screen says so.
        if (!ready[i]) continue;
        Endpoint screen = camera;
        screen.port = usb::kScreenPort;
        screen.name += " · Màn hình";
        out.push_back(screen);
    }
    return out;
}

void AddManual(const std::string& host, uint16_t port)
{
    std::lock_guard lk(g_lock);
    LoadManualLocked();
    for (const Endpoint& m : g_manual)
        if (m.host == host && m.port == port) return;
    g_manual.push_back({ host, port, "", host, "", "", true });
    std::ofstream f(ManualFile(), std::ios::app);
    f << host << ' ' << port << '\n';
}
} // namespace phone
