// PC side of docs/protocol.md: TCP connection to a phone, handshake and
// pairing (section 3), then sealed records (section 2).
#pragma once

#include "Crypto.h"
#include "Discovery.h"
#include "Json.h"

#include <atomic>
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
    bool Connected() const { return m_socket != kInvalid; }

    const std::string& DeviceId() const { return m_deviceId; }
    const std::string& DeviceName() const { return m_deviceName; }
    const std::string& Os() const { return m_os; }

    bool SendControl(const json::Value& message);
    // Waits up to timeoutMs. Returns false on timeout (timedOut) or when
    // the connection is gone or tampered with.
    bool Receive(Message& message, int timeoutMs, bool& timedOut);
    void Close();

    // Asks for caps and waits for them (other control messages are skipped).
    // Generous: the phone may test-open hidden cameras before answering.
    bool RequestCaps(json::Value& caps, std::string& error, int timeoutMs = 20000);

private:
    static constexpr uintptr_t kInvalid = ~uintptr_t(0);

    bool SendRecord(uint8_t type, const uint8_t* body, size_t size);
    bool ReadExact(uint8_t* dst, size_t size, int timeoutMs, bool& timedOut);
    bool SendHandshake(const json::Value& v);
    bool ReadHandshake(json::Value& v, int timeoutMs);

    uintptr_t m_socket = kInvalid;
    std::recursive_mutex m_sendLock; // Close() may run inside SendControl
    crypto::Bytes m_upKey, m_downKey;
    uint64_t m_sendCounter = 0;
    uint64_t m_receiveCounter = 0;
    std::string m_deviceId, m_deviceName, m_os;
};

// This PC's identity and pairing keys (DPAPI-protected files under
// %LOCALAPPDATA%\CamStudio).
std::string ClientId();
std::string ClientName();
crypto::Bytes LoadPairKey(const std::string& deviceId);
void SavePairKey(const std::string& deviceId, const crypto::Bytes& key);
} // namespace phone
