// Finds Cam Studio phones on the home network (DNS-SD _camstudio._tcp,
// docs/protocol.md section 1), remembers phones added by IP, and lists phones on a
// USB cable (adb, usbmuxd: docs/protocol.md 1.2).
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace phone
{
// How the PC reaches a phone: TCP over the network, or a tunnel through a USB cable.
enum class Transport : uint8_t
{
    Network,
    Adb,     // Android: through the adb server
    Usbmux,  // iPhone: through usbmuxd (Apple Mobile Device Service)
};

struct Endpoint
{
    std::string host;      // IPv4 address (network only)
    uint16_t port = 0;     // the phone app's port (USB: the fixed 47321)
    std::string deviceId;  // TXT id (empty for manual entries until connected)
    std::string name;      // UTF-8, TXT name or service instance name
    std::string model;
    std::string os;        // ios | android
    bool manual = false;   // added by IP
    Transport transport = Transport::Network;
    std::string serial;    // USB: adb serial or the iPhone's UDID
    bool screen = false;   // the phone's shared screen (TXT src=screen, or the cable's port 47322)
    bool ready = true;     // reachable now (false: Android on a cable waiting for "allow USB debugging")

    bool Usb() const { return transport != Transport::Network; }
};

// Starts browsing in the background (idempotent).
void StartDiscovery();
void StopDiscovery();

// Phones currently visible on the network, manual ones, then phones on a USB cable. Cheap: the
// USB list is kept current in the background, nothing is asked here.
std::vector<Endpoint> KnownPhones();

// "Add by IP": remembered for this run and in %LOCALAPPDATA%\CamStudio\phones.txt.
void AddManual(const std::string& host, uint16_t port);
void RemoveManual(const std::string& host, uint16_t port);

// Called (on a discovery thread, keep it short) when a phone appears, changes or leaves the
// network list. Cable changes are not reported: the connector looks every 3 s anyway.
void SetOnChange(std::function<void()> onChange);
} // namespace phone
