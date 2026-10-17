// Finds Cam Studio phones on the home network (DNS-SD _camstudio._tcp,
// docs/protocol.md section 1) and remembers phones added by IP.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace phone
{
struct Endpoint
{
    std::string host;      // IPv4 address
    uint16_t port = 0;
    std::string deviceId;  // TXT id (empty for manual entries until connected)
    std::string name;      // UTF-8, TXT name or service instance name
    std::string model;
    std::string os;        // ios | android
    bool manual = false;   // added by IP
};

// Starts browsing in the background (idempotent).
void StartDiscovery();
void StopDiscovery();

// Phones currently visible plus manual ones, de-duplicated by deviceId.
std::vector<Endpoint> KnownPhones();

// "Add by IP": remembered for this run and in %LOCALAPPDATA%\CamStudio\phones.txt.
void AddManual(const std::string& host, uint16_t port);
} // namespace phone
