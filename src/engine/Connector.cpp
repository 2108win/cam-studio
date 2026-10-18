#include "Connector.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace phone
{
namespace
{
constexpr auto kTick = std::chrono::seconds(3);
constexpr uint64_t kRetryMs = 30000;       // after a failure or "busy"
constexpr uint64_t kPairRetryMs = 300000;  // the phone wants pairing: only the user can do that
constexpr uint64_t kPingMs = 15000;        // docs/protocol.md 4.1
constexpr uint64_t kSteadyMs = 300000;     // a link up this long resets the drop count

// "<deviceId>:screen" (the shared screen's TXT id) -> "<deviceId>": the pairing key's id.
std::string BaseId(std::string id)
{
    const std::string suffix = ":screen";
    if (id.size() > suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0)
        id.resize(id.size() - suffix.size());
    return id;
}

// The phone server behind an endpoint: its device id and which server (camera or screen). Two
// endpoints with the same identity reach the same server (over Wi‑Fi and over a cable), and
// the phone serves one connection per PC there. Empty while unknown (a cable phone never
// connected: no TXT record, the id comes from hello).
std::string Identity(const Endpoint& e, const PhoneLink& link)
{
    const std::string id = e.deviceId.empty() ? link.DeviceId() : BaseId(e.deviceId);
    if (id.empty()) return {};
    return id + (e.screen ? "|screen" : "|camera");
}
} // namespace

std::string EndpointKey(const Endpoint& e)
{
    return (e.Usb() ? "usb:" + e.serial : e.host) + ":" + std::to_string(e.port);
}

void Connector::Start(uint32_t delayMs, std::function<void()> beforeFirst)
{
    std::lock_guard lk(m_lock);
    if (m_thread.joinable()) return;
    m_stop = false;
    m_thread = std::thread(&Connector::Run, this, delayMs, std::move(beforeFirst));
}

void Connector::Stop()
{
    std::shared_ptr<PhoneLink> current;
    {
        std::lock_guard lk(m_lock);
        m_stop = true;
        current = m_current;
    }
    if (current) current->Abort(); // an attempt in progress ends at once
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void Connector::Clear()
{
    std::map<std::string, Entry> entries;
    {
        std::lock_guard lk(m_lock);
        entries.swap(m_entries);
        ++m_generation;
    }
    // Links nobody else holds close here (~PhoneLink).
}

void Connector::Wake()
{
    {
        std::lock_guard lk(m_lock);
        m_woken = true;
    }
    m_wake.notify_all();
}

std::shared_ptr<PhoneLink> Connector::Link(const Endpoint& e)
{
    std::lock_guard lk(m_lock);
    Entry& entry = m_entries[EndpointKey(e)];
    if (!entry.link)
    {
        entry.link = std::make_shared<PhoneLink>();
        entry.endpoint = e;
        ++m_generation;
    }
    return entry.link;
}

LinkState Connector::State(const std::string& key, std::string* error) const
{
    std::lock_guard lk(m_lock);
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return LinkState::Idle;
    if (it->second.link && it->second.link->Connected()) return LinkState::Connected;
    if (error) *error = it->second.error;
    // Connected by someone else and closed since: the next tick says what it is now.
    return it->second.state == LinkState::Connected ? LinkState::Idle : it->second.state;
}

void Connector::Set(const std::string& key, LinkState state, const std::string& error, uint64_t retryAt)
{
    std::lock_guard lk(m_lock);
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return;
    Entry& e = it->second;
    e.retryAt = retryAt;
    if (e.state == state && e.error == error) return;
    e.state = state;
    e.error = error;
    ++m_generation;
}

void Connector::Run(uint32_t delayMs, std::function<void()> beforeFirst)
{
    if (delayMs)
    {
        std::unique_lock lk(m_lock);
        m_wake.wait_for(lk, std::chrono::milliseconds(delayMs), [this] { return m_stop; });
        if (m_stop) return;
    }
    if (beforeFirst) beforeFirst();
    for (;;)
    {
        // The phones to look after: everything listed now, plus links still open from before.
        const std::vector<Endpoint> phones = KnownPhones();
        std::vector<std::string> keys;
        {
            std::lock_guard lk(m_lock);
            if (m_stop) break;
            std::map<std::string, bool> before;
            for (auto& [key, entry] : m_entries)
            {
                before[key] = entry.listed;
                entry.listed = false;
            }
            bool changed = false;
            for (const Endpoint& e : phones)
            {
                const std::string key = EndpointKey(e);
                Entry& entry = m_entries[key];
                if (!entry.link) entry.link = std::make_shared<PhoneLink>();
                const bool back = !before[key] || entry.endpoint.host != e.host || entry.endpoint.port != e.port ||
                                  entry.endpoint.ready != e.ready;
                if (back && e.ready)
                {
                    // The phone (re)appeared: its app was opened again, the cable plugged in or
                    // allowed. Try now, not after the wait from earlier failures.
                    entry.retryAt = 0;
                    entry.drops = 0;
                }
                changed |= back || entry.endpoint.name != e.name;
                entry.endpoint = e;
                entry.listed = true;
            }
            for (const auto& [key, entry] : m_entries)
            {
                changed |= before[key] && !entry.listed; // gone from the list
                keys.push_back(key);
            }
            if (changed) ++m_generation;
        }

        for (const std::string& key : keys)
        {
            {
                std::lock_guard lk(m_lock);
                if (m_stop) break;
            }
            Service(key);
        }

        std::unique_lock lk(m_lock);
        m_wake.wait_for(lk, kTick, [this] { return m_stop || m_woken; });
        m_woken = false;
        if (m_stop) break;
    }
}

void Connector::Service(const std::string& key)
{
    Endpoint e;
    std::shared_ptr<PhoneLink> link;
    bool listed = false;
    uint64_t retryAt = 0;
    LinkState state = LinkState::Idle;
    {
        std::lock_guard lk(m_lock);
        const auto it = m_entries.find(key);
        if (it == m_entries.end() || !it->second.link) return;
        e = it->second.endpoint;
        link = it->second.link;
        listed = it->second.listed;
        retryAt = it->second.retryAt;
        state = it->second.state;
    }

    // Someone else reads this link now (streaming, a screen window, pairing): they keep it alive.
    if (!link->TryLease(true)) return;
    LeaseGuard lease(link.get());
    struct Current
    {
        Connector& c;
        Current(Connector& connector, std::shared_ptr<PhoneLink> l) : c(connector)
        {
            std::lock_guard lk(c.m_lock);
            c.m_current = std::move(l);
        }
        ~Current()
        {
            std::lock_guard lk(c.m_lock);
            c.m_current.reset();
        }
    } current(*this, link); // destroyed before the lease is given back

    const uint64_t now = GetTickCount64();
    if (link->Connected())
    {
        Drain(*link);
        if (link->Connected())
        {
            if (now - link->LastSendTick() >= kPingMs) link->SendControl(json::Value::MakeObject().Set("type", "ping"));
            {
                std::lock_guard lk(m_lock);
                Entry& entry = m_entries[key];
                if (!entry.connectedSince) entry.connectedSince = now;
                if (now - entry.connectedSince > kSteadyMs) entry.drops = 0;
            }
            Set(key, LinkState::Connected, "", 0);
            return;
        }
    }
    if (link->Preempted()) return; // given up for someone who needs it now: not a failure

    if (state == LinkState::Connected)
    {
        // The link dropped (phone app closed, Wi‑Fi gone, or another PC took the phone). Wait
        // before the next try, longer each time: a phone app from before "auto" existed hands
        // itself to every new connection, and two PCs would otherwise take it from each other.
        link->Close();
        unsigned drops = 0;
        {
            std::lock_guard lk(m_lock);
            Entry& entry = m_entries[key];
            entry.connectedSince = 0;
            drops = entry.drops = std::min(entry.drops + 1, 5u);
        }
        Set(key, LinkState::Idle, "", now + (kRetryMs << (drops - 1)));
        return;
    }

    if (!listed || now < retryAt) return;
    if (!e.ready)
    {
        Set(key, LinkState::Idle, "", 0);
        return;
    }
    if (CoveredElsewhere(key, e, *link))
    {
        Set(key, LinkState::Elsewhere, "", 0);
        return;
    }
    if (!e.Usb())
    {
        // Wi‑Fi: only phones this PC has paired with (the key is stored by the camera's id; the
        // shared screen uses the same one). A manual entry is known by id once it connected.
        const std::string id = BaseId(e.deviceId.empty() ? link->DeviceId() : e.deviceId);
        if (id.empty())
        {
            Set(key, LinkState::Idle, "", 0);
            return;
        }
        if (LoadPairKey(id).empty())
        {
            Set(key, LinkState::NeedsPairing, "", 0);
            return;
        }
    }
    Attempt(key, e, *link);
}

// Reads what an idle link has: the phone may send caps again, or the tail of a stream.
void Connector::Drain(PhoneLink& link)
{
    for (int i = 0; i < 64 && link.Readable(); ++i)
    {
        Message m;
        bool timedOut = false;
        if (!link.Receive(m, 1000, timedOut)) return; // closed (Receive closed it), or a stall
        if (m.kind != Kind::Control) continue;
        try
        {
            const json::Value v = m.Json();
            if (v["type"].Str() == "caps") link.SetCaps(v.Dump());
        }
        catch (const std::exception&)
        {
        }
    }
}

void Connector::Attempt(const std::string& key, const Endpoint& e, PhoneLink& link)
{
    Set(key, LinkState::Connecting, "", 0);
    bool fetchCaps = false;
    {
        std::lock_guard lk(m_lock);
        fetchCaps = !m_entries[key].capsFetched || link.Caps().empty();
    }
    std::string error;
    // A connect (and the caps after it) may be cut short by anyone who needs the link now.
    link.SetAttempting(true);
    bool ok = link.ConnectAuto(e, error);
    // Caps once per run (and whenever the phone sends them again): enough to pick the source
    // at once. The phone may test-open hidden cameras before answering, so this can take a while.
    if (ok && fetchCaps)
    {
        json::Value caps;
        ok = link.RequestCaps(caps, error);
        if (ok)
        {
            std::lock_guard lk(m_lock);
            m_entries[key].capsFetched = true;
        }
    }
    link.SetAttempting(false);
    const uint64_t now = GetTickCount64();
    if (link.Preempted())
    {
        if (!ok) link.Close();
        Set(key, LinkState::Idle, "", 0);
        return;
    }
    if (!ok)
    {
        link.Close();
        const std::string code = link.LastErrorCode();
        if (code == "busy") Set(key, LinkState::Busy, error, now + kRetryMs);
        else if (code == "pair") Set(key, LinkState::NeedsPairing, error, now + kPairRetryMs);
        else Set(key, LinkState::Unreachable, error, now + kRetryMs);
        return;
    }
    {
        std::lock_guard lk(m_lock);
        m_entries[key].connectedSince = now;
    }
    Set(key, LinkState::Connected, "", 0);
}

bool Connector::CoveredElsewhere(const std::string& key, const Endpoint& e, const PhoneLink& link) const
{
    const std::string id = Identity(e, link);
    std::lock_guard lk(m_lock);
    for (const auto& [otherKey, other] : m_entries)
    {
        if (otherKey == key || !other.link || other.endpoint.screen != e.screen) continue;
        const std::string otherId = Identity(other.endpoint, *other.link);
        // A cable phone never connected has no id yet: a Wi‑Fi phone of the same name is it.
        const bool same = !id.empty() ? otherId == id
                                      : e.Usb() && !other.endpoint.Usb() && !e.name.empty() && other.endpoint.name == e.name;
        if (!same) continue;
        // In use that way (streaming, a screen window): connecting here would take it away.
        if (other.link->InUse()) return true;
        // Connected that way already. The cable is preferred: it takes over an idle Wi‑Fi link
        // (the phone lets the same PC replace its own session).
        if (other.link->Connected() && !(e.Usb() && !other.endpoint.Usb())) return true;
    }
    return false;
}
} // namespace phone
