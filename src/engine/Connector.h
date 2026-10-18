// Keeps every paired phone connected in the background, so picking a phone (or switching to it
// while an app uses the virtual camera) starts streaming at once: no handshake, caps known.
//
// One thread, woken every 3 s and when the network list changes. It never runs on the UI thread.
// For each phone it may use (Wi‑Fi with a saved pairing key, or on a cable), it connects with
// hello "auto" (a phone in use by another PC answers "busy" and keeps that PC), asks for caps
// once, then keeps the idle link alive: reads what the phone sends and pings every 15 s
// (docs/protocol.md 3.1, 4.1). A failure, "busy" or a dropped link waits 30 s before the next try.
// While anyone else uses a link (streaming, a screen window, pairing) it leaves it alone.
#pragma once

#include "Discovery.h"
#include "PhoneLink.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace phone
{
// A phone source's connection, as the camera menu shows it (CsLinkState in EngineApi.h).
enum class LinkState : int32_t
{
    Idle = 0,         // not connected, nothing to try (yet)
    Connected = 1,    // connected (idle and ready, or in use)
    Connecting = 2,
    NeedsPairing = 3, // never paired with this PC (or the phone forgot it): pick it to pair
    Busy = 4,         // another PC has the phone
    Unreachable = 5,  // the last try failed (error says why); tried again later
    Elsewhere = 6,    // the same phone is connected another way (Wi‑Fi / cable)
};

// Stable id of a phone source: "usb:<serial>:<port>" on a cable, "<ip>:<port>" on the network.
std::string EndpointKey(const Endpoint& e);

class Connector
{
public:
    ~Connector() { Stop(); }

    // delayMs: the first look waits this long (started with Windows: let it boot first);
    // beforeFirst runs on the connector's thread just before it (starts discovery).
    void Start(uint32_t delayMs = 0, std::function<void()> beforeFirst = nullptr);
    void Stop();
    // After Stop(): forgets every phone, closing the idle links.
    void Clear();
    // The phone list may have changed: look now rather than at the next tick.
    void Wake();

    // The link for this endpoint, the same object every time for the same EndpointKey.
    std::shared_ptr<PhoneLink> Link(const Endpoint& e);
    LinkState State(const std::string& key, std::string* error = nullptr) const;
    // Changes whenever a state changes: the app refreshes its list then.
    uint32_t Generation() const { return m_generation; }

private:
    struct Entry
    {
        Endpoint endpoint;
        std::shared_ptr<PhoneLink> link;
        LinkState state = LinkState::Idle;
        std::string error;
        uint64_t retryAt = 0;        // GetTickCount64() before which no new attempt is made
        bool listed = false;         // in the latest KnownPhones()
        bool capsFetched = false;    // caps asked for once in this run
        uint64_t connectedSince = 0; // the idle link's current connection
        unsigned drops = 0;          // connections lost in a row (each one doubles the wait)
    };

    void Run(uint32_t delayMs, std::function<void()> beforeFirst);
    void Service(const std::string& key);
    void Drain(PhoneLink& link);
    void Attempt(const std::string& key, const Endpoint& e, PhoneLink& link);
    // Another endpoint of the same phone and port is connected (or, for a cable phone never
    // connected yet, a Wi‑Fi one of the same name is in use): leave this one alone.
    bool CoveredElsewhere(const std::string& key, const Endpoint& e, const PhoneLink& link) const;
    void Set(const std::string& key, LinkState state, const std::string& error, uint64_t retryAt);

    mutable std::mutex m_lock;
    std::condition_variable m_wake;
    bool m_stop = false;
    bool m_woken = false;
    std::thread m_thread;
    std::map<std::string, Entry> m_entries;
    std::shared_ptr<PhoneLink> m_current; // being serviced: Stop() cuts its attempt short
    std::atomic<uint32_t> m_generation{ 1 };
};
} // namespace phone
