// Phones on a USB cable (docs/protocol.md 1.2). The protocol does not change: the platform's own
// USB service opens a TCP tunnel to the phone app's port, and PhoneLink runs over that socket
// exactly as over Wi‑Fi (same pairing, same encryption).
//   Android: the adb server (127.0.0.1:5037): "host:transport:<serial>", then "tcp:<port>".
//   iPhone:  usbmuxd: "Connect". On Windows that is the Apple Mobile Device Service
//            (127.0.0.1:27015, installed with Apple Devices or iTunes); on a Mac /var/run/usbmuxd.
// Plain sockets, nothing Windows-only outside #ifdef _WIN32: tools/usb_check.cpp builds this file
// on a Mac too and runs it against the real usbmuxd.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace usb
{
using Socket = intptr_t; // SOCKET on Windows, a file descriptor elsewhere
constexpr Socket kNoSocket = -1;

enum class Kind { Adb, Usbmux };

// The phone app's fixed ports (docs/protocol.md 1, 1.1), the only ones reachable over a cable.
constexpr uint16_t kCameraPort = 47321;
constexpr uint16_t kScreenPort = 47322; // only while the phone shares its screen

struct Device
{
    Kind kind = Kind::Adb;
    std::string serial;  // adb serial, or the iPhone's UDID
    std::string state;   // adb: "device", "unauthorized", "authorizing"; iPhones: "device"
    std::string name;    // the name the phone app also shows (Android device_name, iPhone name); may be empty
    std::string model;   // adb "model:", iPhone ProductType ("iPhone14,7")
    uint32_t muxId = 0;  // usbmux DeviceID: a new one each time the cable is plugged in
};

// One-shot listings of phones on a cable (network adb and Wi‑Fi-synced iPhones left out).
// False, with a message for the user, when the service is not running.
bool ListAdb(std::vector<Device>& out, std::string& error);
bool ListUsbmux(std::vector<Device>& out, std::string& error);

// A connected, blocking socket that is a tunnel to the phone's TCP port, or kNoSocket and a
// message for the user (Vietnamese, UTF-8). Takes a few milliseconds.
Socket OpenAdb(const std::string& serial, uint16_t port, std::string& error);
Socket OpenUsbmux(const std::string& udid, uint16_t port, std::string& error);
void Close(Socket s);

// The phone's own name: Android "settings get global device_name" over adb; the iPhone's name
// from lockdownd, asked without a session (no pairing, nothing shown on the phone). Empty when
// it cannot be read.
std::string AdbDeviceName(const std::string& serial);
std::string IphoneName(uint32_t muxId, std::string* productType = nullptr);

// Keeps the list of phones on a cable current in a background thread per service. adb
// ("host:track-devices") and usbmuxd ("Listen") push every change, so nothing is polled while
// they run; when a service is missing it is looked for again every few seconds.
class Watcher
{
public:
    ~Watcher() { Stop(); }
    // startAdb runs when no adb server answers. It may start one (Windows: only while an Android
    // phone with USB debugging is plugged in) and returns true if it did. stopIdleAdb runs after
    // a minute with no phone on adb; it may stop the server (it scans USB every second) and
    // returns true if it did.
    void Start(std::function<bool()> startAdb, std::function<bool()> stopIdleAdb = nullptr);
    void Stop();
    // Phones ready to list. A phone is added once its name is known, so the name never changes
    // while it stays plugged in (the app finds cameras again by name).
    std::vector<Device> Devices() const;

private:
    void RunAdb();
    void RunUsbmux();
    void UpdateAdb();
    bool Pause(int ms); // false once Stop() was called
    bool Register(Socket& slot, Socket s);
    void Unregister(Socket& slot, Socket s);

    std::function<bool()> m_startAdb, m_stopIdleAdb;
    std::thread m_adbThread, m_usbmuxThread;
    mutable std::mutex m_lock;
    std::condition_variable m_wake;
    bool m_stop = false;
    Socket m_adbSocket = kNoSocket;    // the open watch sockets, shut down by Stop()
    Socket m_usbmuxSocket = kNoSocket;
    std::vector<Device> m_adb, m_usbmux;
    std::map<std::string, std::string> m_adbNames; // serial -> name, looked up once
};

#ifdef _WIN32
// Starts the adb server when an Android phone with USB debugging is plugged in and none runs:
// adb.exe from the installer (App\adb), the Android SDK, or PATH. At most one try every 30 s.
bool StartAdbServer();
// Stops the server StartAdbServer started, if any (never one that something else started).
void StopAdbServer();
// StopAdbServer once no phone with USB debugging is plugged in any more. True if it stopped it.
bool StopIdleAdbServer();
#endif
} // namespace usb
