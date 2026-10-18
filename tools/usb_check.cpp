// Checks the USB transport (src/engine/UsbTransport.cpp) against real phones on a cable, on
// Windows (camstudio-usb-check.exe) and on a Mac:
//   clang++ -std=c++20 -Wall -Wextra -o build/usb-check tools/usb_check.cpp src/engine/UsbTransport.cpp
//
//   usb-check list                       phones on a cable, from adb and usbmuxd
//   usb-check watch [seconds]            the background watcher's list each time it changes
//   usb-check hello <serial|udid> [port] open a tunnel to the app and exchange "hello"
//                                        (docs/protocol.md 3.1): device id, name, paired
//   usb-check probe <serial|udid> [port] [close|silent|junk]
//                                        open a tunnel and never send "hello": close it at once,
//                                        stay silent, or send bytes that are not a record. Then
//                                        report when the phone closes it (silent: after ~10 s)
//
// "hello" is a real connection, so the phone drops the PC it is streaming to (one PC at a time).
// A probe must not: the session in progress keeps streaming (docs/protocol.md 1).
#include "../src/engine/Json.h"
#include "../src/engine/UsbTransport.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace
{
const char* KindName(usb::Kind k) { return k == usb::Kind::Adb ? "adb" : "usbmux"; }

void Print(const std::vector<usb::Device>& devices)
{
    if (devices.empty()) printf("  (none)\n");
    for (const usb::Device& d : devices)
        printf("  %-6s %-28s %-12s name=\"%s\" model=%s%s\n", KindName(d.kind), d.serial.c_str(), d.state.c_str(),
               d.name.c_str(), d.model.c_str(), d.kind == usb::Kind::Usbmux ? (" muxId=" + std::to_string(d.muxId)).c_str() : "");
}

int List()
{
    std::vector<usb::Device> adb, mux;
    std::string error;
    printf("adb:\n");
    if (usb::ListAdb(adb, error))
    {
        for (usb::Device& d : adb)
            if (d.state == "device") d.name = usb::AdbDeviceName(d.serial);
        Print(adb);
    }
    else printf("  %s\n", error.c_str());
    printf("usbmux:\n");
    if (usb::ListUsbmux(mux, error))
    {
        for (usb::Device& d : mux) d.name = usb::IphoneName(d.muxId, &d.model);
        Print(mux);
    }
    else printf("  %s\n", error.c_str());
    return 0;
}

int Watch(int seconds)
{
    usb::Watcher watcher;
    watcher.Start(nullptr);
    std::string last = "?";
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end)
    {
        const auto devices = watcher.Devices();
        std::string now;
        for (const auto& d : devices) now += d.serial + "/" + d.state + "/" + d.name + ";";
        if (now != last)
        {
            printf("[%s]\n", now.empty() ? "no phones" : "phones");
            Print(devices);
            fflush(stdout);
            last = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    watcher.Stop();
    printf("stopped\n");
    return 0;
}

bool SendAll(usb::Socket s, const std::string& data)
{
    size_t done = 0;
    while (done < data.size())
    {
        const auto n = send(static_cast<decltype(socket(0, 0, 0))>(s), data.data() + done, static_cast<int>(data.size() - done), 0);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

bool RecvAll(usb::Socket s, char* dst, size_t size)
{
    while (size > 0)
    {
        const auto n = recv(static_cast<decltype(socket(0, 0, 0))>(s), dst, static_cast<int>(size), 0);
        if (n <= 0) return false;
        dst += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

void SetReceiveTimeout(usb::Socket s, int seconds)
{
#ifdef _WIN32
    DWORD timeout = seconds * 1000;
#else
    timeval timeout{ seconds, 0 };
#endif
    setsockopt(static_cast<decltype(socket(0, 0, 0))>(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

// A tunnel to the app's port on the phone with this adb serial or UDID, or kNoSocket.
usb::Socket OpenTunnel(const std::string& id, uint16_t port)
{
    std::vector<usb::Device> adb, mux;
    std::string error;
    usb::ListAdb(adb, error);
    usb::ListUsbmux(mux, error);
    usb::Socket s = usb::kNoSocket;
    bool found = false;
    for (const auto& d : adb)
        if (d.serial == id) { found = true; s = usb::OpenAdb(id, port, error); }
    for (const auto& d : mux)
        if (d.serial == id) { found = true; s = usb::OpenUsbmux(id, port, error); }
    if (!found) printf("no phone %s on a cable\n", id.c_str());
    else if (s == usb::kNoSocket) printf("tunnel failed: %s\n", error.c_str());
    else printf("tunnel to port %u open\n", port);
    fflush(stdout);
    return s;
}

int Probe(const std::string& id, uint16_t port, const std::string& mode)
{
    const usb::Socket s = OpenTunnel(id, port);
    if (s == usb::kNoSocket) return 1;
    if (mode == "close")
    {
        usb::Close(s);
        printf("closed without a hello\n");
        return 0;
    }
    // "junk": the first 4 bytes read as a record length far above the handshake limit.
    if (mode == "junk" && !SendAll(s, "GET / HTTP/1.1\r\nHost: phone\r\n\r\n")) printf("send failed\n");
    const auto start = std::chrono::steady_clock::now();
    SetReceiveTimeout(s, 30);
    char byte;
    const auto n = recv(static_cast<decltype(socket(0, 0, 0))>(s), &byte, 1, 0);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    usb::Close(s);
    if (n > 0) printf("the phone sent data: it took this for a PC\n");
    else if (seconds >= 29) printf("still open after %.0f s\n", seconds);
    else printf("the phone closed it after %.1f s\n", seconds);
    return n > 0 ? 1 : 0;
}

int Hello(const std::string& id, uint16_t port)
{
    const usb::Socket s = OpenTunnel(id, port);
    if (s == usb::kNoSocket) return 1;
    // A suspended app (phone locked, app in the background) may leave the connection unanswered.
    SetReceiveTimeout(s, 10);

    // HANDSHAKE record: u32 length, type 0x01, JSON. The commitment is never checked: no "reveal" follows.
    const std::string body = json::Value::MakeObject()
        .Set("type", "hello").Set("v", 1).Set("clientId", "usb-check").Set("clientName", "usb-check")
        .Set("commit", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=").Dump();
    const uint32_t n = static_cast<uint32_t>(body.size() + 1);
    std::string record = { char(n >> 24), char(n >> 16), char(n >> 8), char(n), char(0x01) };
    record += body;
    char head[5];
    if (!SendAll(s, record) || !RecvAll(s, head, 5))
    {
        printf("no reply from the app in 10 s (phone locked, or the app in the background?)\n");
        usb::Close(s);
        return 1;
    }
    const uint32_t length = (uint32_t(uint8_t(head[0])) << 24) | (uint32_t(uint8_t(head[1])) << 16) | (uint32_t(uint8_t(head[2])) << 8) | uint8_t(head[3]);
    if (head[4] != 0x01 || length < 2 || length > 65536) { printf("not a HANDSHAKE record\n"); usb::Close(s); return 1; }
    std::string reply(length - 1, '\0');
    const bool ok = RecvAll(s, reply.data(), reply.size());
    usb::Close(s);
    if (!ok) { printf("reply cut short\n"); return 1; }
    const json::Value v = json::Value::Parse(reply);
    printf("hello: deviceId=%s name=\"%s\" model=\"%s\" os=%s paired=%s\n", v["deviceId"].Str().c_str(), v["name"].Str().c_str(),
           v["model"].Str().c_str(), v["os"].Str().c_str(), v["paired"].Bool() ? "true" : "false");
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string cmd = argc > 1 ? argv[1] : "list";
    if (cmd == "list") return List();
    if (cmd == "watch") return Watch(argc > 2 ? atoi(argv[2]) : 30);
    const uint16_t port = static_cast<uint16_t>(argc > 3 ? atoi(argv[3]) : usb::kCameraPort);
    if (cmd == "hello" && argc > 2) return Hello(argv[2], port);
    if (cmd == "probe" && argc > 2) return Probe(argv[2], port, argc > 4 ? argv[4] : "close");
    fprintf(stderr, "usage: usb-check list | watch [seconds] | hello <serial|udid> [port] | probe <serial|udid> [port] [close|silent|junk]\n");
    return 2;
}
