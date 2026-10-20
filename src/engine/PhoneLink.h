// PC side of docs/protocol.md: TCP connection to a phone, handshake and
// pairing (section 3), then sealed records (section 2).
#pragma once

#include "Crypto.h"
#include "Discovery.h"
#include "Json.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace phone
{
enum class Kind : uint8_t
{
    Control = 1,
    VideoConfig = 2,
    VideoFrame = 3,
    Audio = 4, // the phone's microphone, 16-bit PCM (docs/protocol.md 5.1), only when asked
};

struct Message
{
    Kind kind{};
    int64_t ptsUs = 0;
    uint32_t flags = 0;
    crypto::Bytes payload;

    bool Keyframe() const { return flags & 1; }
    json::Value Json() const { return json::Value::Parse(std::string(payload.begin(), payload.end())); }
};

// One authenticated connection to a phone app's port (camera or shared screen).
//
// Threads: one reader at a time holds the lease (Lease/TryLease): the capture thread while it
// streams this source, a screen viewer, cs_load_formats while it connects, or the background
// connector while the link is idle. Only the lease holder connects, receives and closes.
// SendControl may run on any thread (the app's controls); Abort() too.
class PhoneLink
{
public:
    // Called during pairing with the 6-digit code to show, and with "" when
    // pairing is over.
    using PairingCallback = std::function<void(const std::string& code)>;
    // Blocks until the PC user accepts (true) or declines the code. Both
    // sides must accept (docs/protocol.md 3.2): otherwise a fake "phone" on
    // the network could pair itself and feed the virtual camera.
    using PairingConfirm = std::function<bool()>;

    PhoneLink() = default;
    ~PhoneLink() { Close(); }
    PhoneLink(const PhoneLink&) = delete;
    PhoneLink& operator=(const PhoneLink&) = delete;

    // Connects, authenticates (pairing first if needed). Blocking; on
    // failure `error` is a message for the user (Vietnamese, UTF-8).
    bool Connect(const Endpoint& endpoint, const PairingCallback& onPairing, const PairingConfirm& confirm, std::string& error);
    // The background connector: hello says "auto", so a phone in use by another PC answers
    // "busy" and keeps that PC (docs/protocol.md 3.1). Never pairs.
    bool ConnectAuto(const Endpoint& endpoint, std::string& error);
    // A connection the user asked for, to an already-paired phone: never pairs (a screen window).
    bool ConnectKnown(const Endpoint& endpoint, std::string& error);
    // Connects again to the last endpoint (re-resolved by device id, the
    // phone's IP may have changed). Only for an already-paired phone: never
    // starts pairing, since nobody is there to compare codes.
    bool Reconnect(std::string& error);
    bool Connected() const { return m_socket != kInvalid && !m_aborted; }
    // Why the last connect failed, for the connector: "busy" (another PC has the phone),
    // "pair" (pairing needed), or "" (anything else).
    const std::string& LastErrorCode() const { return m_errorCode; }

    // From the phone's hello; kept after the link closes. Any thread.
    std::string DeviceId() const;
    std::string DeviceName() const;

    bool SendControl(const json::Value& message);
    // Waits up to timeoutMs. Returns false on timeout (timedOut) or when
    // the connection is gone or tampered with.
    bool Receive(Message& message, int timeoutMs, bool& timedOut);
    // Something can be read now (a record, or the connection closing).
    bool Readable() const;
    // Lease holder: closes the socket.
    void Close();
    // Any thread: ends the connection without closing the socket under its reader (shutdown).
    // The reader wakes with an error and closes. Also cuts short a connect in progress.
    void Abort();

    // GetTickCount64() of the last record sent (the connector pings idle links).
    uint64_t LastSendTick() const { return m_lastSend; }

    // Asks for caps and waits for them (other control messages are skipped).
    // Generous: the phone may test-open hidden cameras before answering.
    bool RequestCaps(json::Value& caps, std::string& error, int timeoutMs = 20000);
    // The phone's last caps (JSON), kept across reconnects, so picking the source needs no
    // round trip. Any thread.
    std::string Caps() const;
    void SetCaps(const std::string& caps);

    // The lease (see above). TryLease(true) is the connector's: it gives way. While it marks
    // an attempt (SetAttempting: connecting, asking for caps), a Lease() from anyone else aborts
    // that attempt, and Preempted() then says so (not a failure). Reading an idle link is quick
    // and is waited for, so the idle connection survives.
    bool TryLease(bool background);
    bool Lease(int timeoutMs);
    void Unlease();
    void SetAttempting(bool on) { m_attempting = on; }
    bool Preempted() const { return m_preempted; }
    // Held by someone other than the connector (streaming, a viewer, pairing).
    bool InUse() const;

private:
    static constexpr uintptr_t kInvalid = ~uintptr_t(0);

    bool SendRecord(uint8_t type, const uint8_t* body, size_t size);
    bool ReadExact(uint8_t* dst, size_t size, int timeoutMs, bool& timedOut);
    bool SendHandshake(const json::Value& v);
    bool ReadHandshake(json::Value& v, int timeoutMs, bool* timedOut = nullptr);

    // repair: pair again although both sides have a key (they no longer match).
    bool ConnectImpl(const Endpoint& endpoint, const PairingCallback& onPairing, const PairingConfirm& confirm,
                     bool allowPairing, bool automatic, std::string& error, bool repair = false);

    std::atomic<uintptr_t> m_socket{ kInvalid };
    std::atomic<bool> m_aborted{ false };
    std::atomic<uint64_t> m_lastSend{ 0 };
    Endpoint m_endpoint; // last one Connect() was given
    std::recursive_mutex m_sendLock; // Close() may run inside SendControl
    crypto::Bytes m_upKey, m_downKey;
    uint64_t m_sendCounter = 0;
    uint64_t m_receiveCounter = 0;
    std::string m_errorCode;

    mutable std::mutex m_infoLock; // device identity and caps, read from other threads
    std::string m_deviceId, m_deviceName, m_os;
    std::string m_caps;

    mutable std::mutex m_leaseLock;
    std::condition_variable m_leaseFree;
    int m_leaseState = 0; // 0 free, 1 the connector, 2 anyone else
    std::atomic<bool> m_attempting{ false };
    std::atomic<bool> m_preempted{ false };
};

// Holds a link's lease for a scope (taken with Lease/TryLease beforehand).
class LeaseGuard
{
public:
    explicit LeaseGuard(PhoneLink* link) : m_link(link) {}
    ~LeaseGuard() { if (m_link) m_link->Unlease(); }
    LeaseGuard(const LeaseGuard&) = delete;
    LeaseGuard& operator=(const LeaseGuard&) = delete;

private:
    PhoneLink* m_link;
};

// This PC's identity and pairing keys (DPAPI-protected files under
// %LOCALAPPDATA%\CamStudio).
std::string ClientId();
std::string ClientName();
crypto::Bytes LoadPairKey(const std::string& deviceId);
void SavePairKey(const std::string& deviceId, const crypto::Bytes& key);
bool HasPairKey(const std::string& deviceId);
// "Quên ghép đôi": the next connection pairs again (6-digit code on both sides).
void ForgetPairKey(const std::string& deviceId);
} // namespace phone
