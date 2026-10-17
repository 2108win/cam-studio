#include "PhoneLink.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dpapi.h>
#include <objbase.h>

#include <fstream>
#include <sstream>

#pragma comment(lib, "ws2_32")

namespace phone
{
namespace
{
constexpr uint8_t kHandshake = 0x01;
constexpr uint8_t kSealed = 0x02;
constexpr uint32_t kMaxHandshake = 64 * 1024;
constexpr uint32_t kMaxSealed = 16 * 1024 * 1024;

struct WinsockInit
{
    WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
};

std::wstring DataDir()
{
    wchar_t path[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    std::wstring dir = std::wstring(path) + L"\\CamStudio";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\pairings").c_str(), nullptr);
    return dir;
}

std::wstring PairFile(const std::string& deviceId)
{
    // File name from a hash: device ids are arbitrary text from the network.
    const crypto::Bytes id = crypto::Str(deviceId);
    const std::string h = crypto::ToHex(crypto::Sha256({ &id }));
    return DataDir() + L"\\pairings\\" + std::wstring(h.begin(), h.begin() + 32) + L".key";
}

void PutU32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void PutU64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i)); }
uint32_t GetU32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t GetU64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p[i]; return v; }
} // namespace

// ---- identity & pairing store ---------------------------------------------------------

std::string ClientId()
{
    static const std::string id = [] {
        const std::wstring file = DataDir() + L"\\client-id.txt";
        std::ifstream in(file);
        std::string s;
        if (in >> s && s.size() >= 32) return s;
        GUID g;
        CoCreateGuid(&g);
        char buf[40];
        snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        std::ofstream(file) << buf;
        return std::string(buf);
    }();
    return id;
}

std::string ClientName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = ARRAYSIZE(name);
    if (!GetComputerNameW(name, &n)) return "PC";
    const int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, -1, s.data(), len, nullptr, nullptr);
    return s;
}

crypto::Bytes LoadPairKey(const std::string& deviceId)
{
    std::ifstream f(PairFile(deviceId), std::ios::binary);
    std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (blob.empty()) return {};
    DATA_BLOB in{ static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(blob.data()) };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    crypto::Bytes key(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return key;
}

void SavePairKey(const std::string& deviceId, const crypto::Bytes& key)
{
    DATA_BLOB in{ static_cast<DWORD>(key.size()), const_cast<BYTE*>(key.data()) };
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Cam Studio pairing", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
    std::ofstream(PairFile(deviceId), std::ios::binary | std::ios::trunc).write(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
}

// ---- connection --------------------------------------------------------------------------

bool PhoneLink::Connect(const Endpoint& endpoint, const PairingCallback& onPairing, const PairingConfirm& confirm, std::string& error)
{
    return ConnectImpl(endpoint, onPairing, confirm, true, error);
}

bool PhoneLink::Reconnect(std::string& error)
{
    Endpoint endpoint = m_endpoint;
    if (!m_deviceId.empty())
    {
        for (const Endpoint& e : KnownPhones())
            if (e.deviceId == m_deviceId) { endpoint = e; break; }
    }
    if (endpoint.host.empty()) { error = "Chưa từng kết nối điện thoại này"; return false; }
    return ConnectImpl(endpoint, nullptr, nullptr, false, error);
}

bool PhoneLink::ConnectImpl(const Endpoint& endpoint, const PairingCallback& onPairing, const PairingConfirm& confirm,
                            bool allowPairing, std::string& error)
{
    static WinsockInit winsock;
    Close();
    m_endpoint = endpoint;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addr = nullptr;
    if (getaddrinfo(endpoint.host.c_str(), std::to_string(endpoint.port).c_str(), &hints, &addr) != 0)
    {
        error = "Địa chỉ không hợp lệ: " + endpoint.host;
        return false;
    }
    SOCKET s = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    // Connect with a timeout: unreachable phones should not hang the app.
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    connect(s, addr->ai_addr, static_cast<int>(addr->ai_addrlen));
    freeaddrinfo(addr);
    fd_set w;
    FD_ZERO(&w);
    FD_SET(s, &w);
    timeval tv{ 4, 0 };
    int so = 0, len = sizeof(so);
    if (select(0, nullptr, &w, nullptr, &tv) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &len) != 0 || so != 0)
    {
        closesocket(s);
        error = "Không kết nối được tới điện thoại " + endpoint.host + ":" + std::to_string(endpoint.port) +
                ". Kiểm tra app Cam Studio trên điện thoại đang mở và cùng mạng Wi‑Fi.";
        return false;
    }
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&noDelay), sizeof(noDelay));
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&rcvbuf), sizeof(rcvbuf));
    m_socket = s;

    auto fail = [&](const std::string& msg) { error = msg; Close(); return false; };

    // 1. hello with commitment to our ephemeral key (section 3.1)
    crypto::EcdhKey key;
    const std::string clientId = ClientId();
    const crypto::Bytes commit = crypto::Sha256({ &key.PublicKey() });
    if (!SendHandshake(json::Value::MakeObject()
            .Set("type", "hello").Set("v", 1)
            .Set("clientId", clientId).Set("clientName", ClientName())
            .Set("commit", crypto::Base64(commit))))
        return fail("Mất kết nối với điện thoại");

    json::Value hello;
    if (!ReadHandshake(hello, 8000) || hello["type"].Str() != "hello") return fail("Điện thoại không trả lời đúng giao thức");
    if (hello["v"].Int() != 1) return fail("Phiên bản app trên điện thoại không khớp với Cam Studio trên PC");
    m_deviceId = hello["deviceId"].Str();
    m_deviceName = hello["name"].Str(endpoint.name);
    m_os = hello["os"].Str();
    const crypto::Bytes phonePub = crypto::FromBase64(hello["pub"].Str());
    if (phonePub.size() != 65 || m_deviceId.empty()) return fail("Dữ liệu bắt tay không hợp lệ");

    // 2. reveal our key, derive Z and the transcript
    if (!SendHandshake(json::Value::MakeObject().Set("type", "reveal").Set("pub", crypto::Base64(key.PublicKey()))))
        return fail("Mất kết nối với điện thoại");
    crypto::Bytes z, t;
    try
    {
        z = key.Agree(phonePub);
        t = crypto::Transcript(clientId, m_deviceId, key.PublicKey(), phonePub);
    }
    catch (const std::exception&)
    {
        return fail("Khóa của điện thoại không hợp lệ");
    }

    // 3. pairing (3.2) or straight to authentication (3.3)
    crypto::Bytes kPair = LoadPairKey(m_deviceId);
    bool newPairing = false;
    if (kPair.empty() || !hello["paired"].Bool())
    {
        if (!allowPairing) return fail("Điện thoại cần ghép đôi lại: chọn lại điện thoại trong danh sách camera");
        // The old key, if any, stays until a new pairing succeeds.
        if (!SendHandshake(json::Value::MakeObject().Set("type", "pairStart"))) return fail("Mất kết nối với điện thoại");
        if (onPairing) onPairing(crypto::Sas(t));
        json::Value reply;
        const bool got = ReadHandshake(reply, 125000);
        auto done = [&](const std::string& msg) { if (onPairing) onPairing(""); return fail(msg); };
        if (!got) return done("Hết thời gian chờ xác nhận ghép đôi trên điện thoại");
        if (reply["type"].Str() == "error") return done(reply["message"].Str("Điện thoại từ chối ghép đôi"));
        if (reply["type"].Str() != "pair" || !reply["accepted"].Bool()) return done("Đã từ chối ghép đôi trên điện thoại");
        // The PC user must agree too (protocol 3.2).
        if (!confirm || !confirm()) return done("Đã từ chối ghép đôi trên máy tính");
        if (onPairing) onPairing("");
        kPair = crypto::PairKey(z, t);
        newPairing = true;
    }

    if (!SendHandshake(json::Value::MakeObject().Set("type", "auth").Set("mac", crypto::Base64(crypto::PcMac(kPair, t)))))
        return fail("Mất kết nối với điện thoại");
    json::Value auth;
    if (!ReadHandshake(auth, 8000) || auth["type"].Str() != "auth")
        return fail(newPairing ? "Ghép đôi thất bại" : "Điện thoại không nhận ra máy tính này. Hãy ghép đôi lại.");
    if (!crypto::ConstantTimeEqual(crypto::FromBase64(auth["mac"].Str()), crypto::PhoneMac(kPair, t)))
        return fail("Điện thoại xác thực sai: có thể không phải thiết bị đã ghép đôi");
    if (newPairing) SavePairKey(m_deviceId, kPair);

    // 4. session keys
    m_upKey = crypto::UpKey(z, kPair, t);
    m_downKey = crypto::DownKey(z, kPair, t);
    m_sendCounter = m_receiveCounter = 0;
    return true;
}

void PhoneLink::Close()
{
    std::lock_guard lk(m_sendLock);
    if (m_socket != kInvalid)
    {
        closesocket(static_cast<SOCKET>(m_socket));
        m_socket = kInvalid;
    }
    m_upKey.clear();
    m_downKey.clear();
}

// ---- records -------------------------------------------------------------------------------

bool PhoneLink::SendRecord(uint8_t type, const uint8_t* body, size_t size)
{
    if (m_socket == kInvalid) return false;
    std::vector<uint8_t> buf(5 + size);
    PutU32(buf.data(), static_cast<uint32_t>(1 + size));
    buf[4] = type;
    memcpy(buf.data() + 5, body, size);
    const char* p = reinterpret_cast<const char*>(buf.data());
    size_t left = buf.size();
    while (left > 0)
    {
        const int n = send(static_cast<SOCKET>(m_socket), p, static_cast<int>(left), 0);
        if (n <= 0) return false;
        p += n;
        left -= n;
    }
    return true;
}

bool PhoneLink::ReadExact(uint8_t* dst, size_t size, int timeoutMs, bool& timedOut)
{
    timedOut = false;
    ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    bool extended = false;
    size_t got = 0;
    while (got < size)
    {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            // Nothing arrived: a plain timeout. Part of a record arrived:
            // give the rest 10 s before calling the connection dead.
            if (got == 0) { timedOut = true; return false; }
            if (extended) return false;
            extended = true;
            deadline = now + 10000;
            continue;
        }
        fd_set r;
        FD_ZERO(&r);
        FD_SET(static_cast<SOCKET>(m_socket), &r);
        const ULONGLONG wait = deadline - now;
        timeval tv{ static_cast<long>(wait / 1000), static_cast<long>((wait % 1000) * 1000) };
        const int ready = select(0, &r, nullptr, nullptr, &tv);
        if (ready == 0) continue; // re-checked against the deadline above
        if (ready < 0) return false;
        const int n = recv(static_cast<SOCKET>(m_socket), reinterpret_cast<char*>(dst + got), static_cast<int>(size - got), 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

bool PhoneLink::SendHandshake(const json::Value& v)
{
    const std::string text = v.Dump();
    return SendRecord(kHandshake, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool PhoneLink::ReadHandshake(json::Value& v, int timeoutMs)
{
    if (m_socket == kInvalid) return false;
    uint8_t head[5];
    bool timedOut = false;
    if (!ReadExact(head, 5, timeoutMs, timedOut)) return false;
    const uint32_t length = GetU32(head);
    if (length < 1 || length > kMaxHandshake || head[4] != kHandshake) return false;
    std::string body(length - 1, '\0');
    if (!ReadExact(reinterpret_cast<uint8_t*>(body.data()), body.size(), 8000, timedOut)) return false;
    try
    {
        v = json::Value::Parse(body);
        return v.IsObject();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool PhoneLink::SendControl(const json::Value& message)
{
    std::lock_guard lk(m_sendLock);
    if (m_upKey.empty()) return false;
    const std::string text = message.Dump();
    std::vector<uint8_t> plain(13 + text.size(), 0);
    plain[0] = static_cast<uint8_t>(Kind::Control);
    memcpy(plain.data() + 13, text.data(), text.size());
    const uint64_t counter = m_sendCounter++;
    const crypto::Bytes sealed = crypto::Seal(m_upKey, crypto::kDirUp, counter, plain.data(), plain.size());
    std::vector<uint8_t> body(8 + sealed.size());
    PutU64(body.data(), counter);
    memcpy(body.data() + 8, sealed.data(), sealed.size());
    if (SendRecord(kSealed, body.data(), body.size())) return true;
    Close(); // dead connection: the next use reconnects
    return false;
}

bool PhoneLink::Receive(Message& message, int timeoutMs, bool& timedOut)
{
    timedOut = false;
    if (m_socket == kInvalid || m_downKey.empty()) return false;
    // Any failure other than a plain timeout ends the connection, so
    // Connected() turns false and the next use reconnects.
    auto broken = [&] { Close(); return false; };
    uint8_t head[13];
    if (!ReadExact(head, 13, timeoutMs, timedOut)) return timedOut ? false : broken();
    const uint32_t length = GetU32(head);
    if (head[4] != kSealed || length < 1 + 8 + 16 || length > kMaxSealed) return broken();
    const uint64_t counter = GetU64(head + 5);
    if (counter != m_receiveCounter) return broken(); // replayed or reordered
    std::vector<uint8_t> sealed(length - 9);
    bool t2 = false;
    if (!ReadExact(sealed.data(), sealed.size(), 10000, t2)) return broken();
    crypto::Bytes plain;
    if (!crypto::Open(m_downKey, crypto::kDirDown, counter, sealed.data(), sealed.size(), plain) || plain.size() < 13) return broken();
    ++m_receiveCounter;
    message.kind = static_cast<Kind>(plain[0]);
    message.ptsUs = static_cast<int64_t>(GetU64(plain.data() + 1));
    message.flags = GetU32(plain.data() + 9);
    message.payload.assign(plain.begin() + 13, plain.end());
    return true;
}

bool PhoneLink::RequestCaps(json::Value& caps, std::string& error, int timeoutMs)
{
    if (!SendControl(json::Value::MakeObject().Set("type", "getCaps")))
    {
        error = "Mất kết nối với điện thoại";
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline)
    {
        Message m;
        bool timedOut = false;
        if (!Receive(m, 1000, timedOut))
        {
            if (timedOut) continue;
            error = "Mất kết nối với điện thoại";
            return false;
        }
        if (m.kind != Kind::Control) continue;
        try
        {
            json::Value v = m.Json();
            if (v["type"].Str() == "caps") { caps = v; return true; }
        }
        catch (const std::exception&)
        {
        }
    }
    error = "Điện thoại không gửi thông tin camera";
    return false;
}
} // namespace phone
