#include "UsbTransport.h"

#include "Json.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cfgmgr32.h>
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "cfgmgr32")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace usb
{
namespace
{
constexpr uint16_t kAdbPort = 5037;
constexpr uint16_t kLockdownPort = 62078;
constexpr uint32_t kMaxPlist = 1024 * 1024;

// ---- sockets -------------------------------------------------------------------------------

#ifdef _WIN32
constexpr uint16_t kUsbmuxPort = 27015; // Apple Mobile Device Service
SOCKET Native(Socket s) { return static_cast<SOCKET>(s); }
void EnsureWinsock()
{
    static const bool started = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    (void)started;
}
#else
int Native(Socket s) { return static_cast<int>(s); }
void EnsureWinsock() {}
#endif

Socket Adopt(decltype(socket(0, 0, 0)) s)
{
#ifdef _WIN32
    return s == INVALID_SOCKET ? kNoSocket : static_cast<Socket>(s);
#else
    if (s < 0) return kNoSocket;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); // a closed tunnel must not kill us
#endif
    return static_cast<Socket>(s);
#endif
}

void Shutdown(Socket s)
{
#ifdef _WIN32
    shutdown(Native(s), SD_BOTH);
#else
    shutdown(Native(s), SHUT_RDWR);
#endif
}

Socket ConnectLoopback(uint16_t port)
{
    EnsureWinsock();
    const Socket s = Adopt(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s == kNoSocket) return kNoSocket;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(Native(s), reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
    {
        Close(s);
        return kNoSocket;
    }
    int one = 1;
    setsockopt(Native(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    return s;
}

Socket ConnectUsbmuxd()
{
#ifdef _WIN32
    return ConnectLoopback(kUsbmuxPort);
#else
    const Socket s = Adopt(socket(AF_UNIX, SOCK_STREAM, 0));
    if (s == kNoSocket) return kNoSocket;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, "/var/run/usbmuxd", sizeof(a.sun_path) - 1);
    if (connect(Native(s), reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
    {
        Close(s);
        return kNoSocket;
    }
    return s;
#endif
}

// timeoutMs < 0 waits forever (the watchers; Stop() shuts their socket down).
bool WaitReadable(Socket s, int timeoutMs)
{
#ifdef _WIN32
    fd_set r;
    FD_ZERO(&r);
    FD_SET(Native(s), &r);
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    return select(0, &r, nullptr, nullptr, timeoutMs < 0 ? nullptr : &tv) == 1;
#else
    pollfd p{ Native(s), POLLIN, 0 };
    return poll(&p, 1, timeoutMs) == 1;
#endif
}

bool RecvAll(Socket s, void* dst, size_t size, int timeoutMs)
{
    char* p = static_cast<char*>(dst);
    while (size > 0)
    {
        if (!WaitReadable(s, timeoutMs)) return false;
        const auto n = recv(Native(s), p, static_cast<int>(size), 0);
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool SendAll(Socket s, const void* data, size_t size)
{
    const char* p = static_cast<const char*>(data);
    while (size > 0)
    {
#ifdef MSG_NOSIGNAL
        const auto n = send(Native(s), p, static_cast<int>(size), MSG_NOSIGNAL);
#else
        const auto n = send(Native(s), p, static_cast<int>(size), 0);
#endif
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

void PutLE32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
uint32_t GetLE32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

bool Contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

// ---- adb (the adb server's "smart socket" protocol) ------------------------------------------

// A block: 4 hex digits of length, then that many bytes.
bool AdbReadBlock(Socket s, std::string& out, int timeoutMs)
{
    char hex[5] = {};
    if (!RecvAll(s, hex, 4, timeoutMs)) return false;
    char* end = nullptr;
    const unsigned long n = strtoul(hex, &end, 16);
    if (end != hex + 4) return false;
    out.assign(n, '\0');
    return n == 0 || RecvAll(s, out.data(), n, timeoutMs);
}

// Sends one request; the server answers OKAY, or FAIL and a block with the reason.
bool AdbSend(Socket s, const std::string& request, std::string& failure, int timeoutMs = 5000)
{
    char head[8];
    snprintf(head, sizeof(head), "%04x", static_cast<unsigned>(request.size()));
    char status[4];
    if (!SendAll(s, head, 4) || !SendAll(s, request.data(), request.size()) || !RecvAll(s, status, 4, timeoutMs))
    {
        failure = "closed";
        return false;
    }
    if (memcmp(status, "OKAY", 4) == 0) return true;
    if (memcmp(status, "FAIL", 4) != 0 || !AdbReadBlock(s, failure, timeoutMs)) failure = "bad reply";
    return false;
}

// "host:transport:<serial>" on a fresh connection: the socket then talks to that phone's adbd.
Socket AdbTransport(const std::string& serial, std::string& failure)
{
    const Socket s = ConnectLoopback(kAdbPort);
    if (s == kNoSocket)
    {
        failure = "no adb server";
        return kNoSocket;
    }
    if (!AdbSend(s, "host:transport:" + serial, failure))
    {
        Close(s);
        return kNoSocket;
    }
    return s;
}

std::string AdbShell(const std::string& serial, const std::string& command)
{
    std::string failure, out;
    const Socket s = AdbTransport(serial, failure);
    if (s == kNoSocket) return {};
    if (AdbSend(s, "shell:" + command, failure))
    {
        char buf[512];
        while (out.size() < 4096 && WaitReadable(s, 3000))
        {
            const auto n = recv(Native(s), buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
    }
    Close(s);
    return out;
}

// Network adb ("192.168.1.5:5555", "adb-XXXX._adb-tls-connect._tcp") and emulators are not cables.
bool IsCable(const std::string& serial)
{
    return serial.find(':') == std::string::npos && !Contains(serial, "._adb-tls") && serial.rfind("emulator-", 0) != 0;
}

std::string Trim(std::string s)
{
    const char* ws = " \t\r\n";
    s.erase(0, s.find_first_not_of(ws));
    s.erase(s.find_last_not_of(ws) + 1);
    return s;
}

// ---- XML property lists (usbmuxd and lockdownd) ----------------------------------------------

std::string XmlEscape(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out += c;
    }
    return out;
}

// One dict of strings and integers, which is all the requests need.
std::string PlistDict(const std::vector<std::pair<std::string, std::string>>& strings,
                      const std::vector<std::pair<std::string, int64_t>>& integers = {})
{
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                    "<plist version=\"1.0\">\n<dict>\n";
    for (const auto& [k, v] : strings) x += "\t<key>" + XmlEscape(k) + "</key>\n\t<string>" + XmlEscape(v) + "</string>\n";
    for (const auto& [k, v] : integers) x += "\t<key>" + XmlEscape(k) + "</key>\n\t<integer>" + std::to_string(v) + "</integer>\n";
    return x + "</dict>\n</plist>\n";
}

void AppendUtf8(std::string& out, unsigned long cp)
{
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

// Reads the XML plists usbmuxd and lockdownd send into a json::Value: dict, array, string,
// integer, real, true, false; data and date stay as their text.
class PlistReader
{
public:
    explicit PlistReader(const std::string& xml) : m_s(xml) {}

    json::Value Read()
    {
        std::string tag;
        while (NextTag(tag))
            if (tag != "plist" && tag != "/plist") return Element(tag);
        return {};
    }

private:
    // The next tag's name without attributes: "dict", "/dict", "true/" (self-closing)...
    bool NextTag(std::string& name)
    {
        while (true)
        {
            const size_t open = m_s.find('<', m_p);
            if (open == std::string::npos) return false;
            if (m_s.compare(open, 4, "<!--") == 0)
            {
                const size_t end = m_s.find("-->", open);
                if (end == std::string::npos) return false;
                m_p = end + 3;
                continue;
            }
            const size_t close = m_s.find('>', open);
            if (close == std::string::npos) return false;
            m_p = close + 1;
            if (open + 1 < m_s.size() && (m_s[open + 1] == '?' || m_s[open + 1] == '!')) continue; // <?xml?>, <!DOCTYPE>
            std::string t = m_s.substr(open + 1, close - open - 1);
            const bool selfClosing = !t.empty() && t.back() == '/';
            if (selfClosing) t.pop_back();
            const size_t space = t.find_first_of(" \t\r\n");
            if (space != std::string::npos) t.resize(space);
            name = selfClosing ? t + "/" : t;
            return true;
        }
    }

    // Text up to the next tag, entities decoded.
    std::string Text()
    {
        const size_t end = m_s.find('<', m_p);
        const std::string raw = m_s.substr(m_p, end == std::string::npos ? std::string::npos : end - m_p);
        m_p = end == std::string::npos ? m_s.size() : end;
        std::string out;
        for (size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i] != '&') { out += raw[i]; continue; }
            const size_t semi = raw.find(';', i);
            if (semi == std::string::npos) { out += raw[i]; continue; }
            const std::string e = raw.substr(i + 1, semi - i - 1);
            if (e == "amp") out += '&';
            else if (e == "lt") out += '<';
            else if (e == "gt") out += '>';
            else if (e == "quot") out += '"';
            else if (e == "apos") out += '\'';
            else if (!e.empty() && e[0] == '#')
                AppendUtf8(out, e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? strtoul(e.c_str() + 2, nullptr, 16) : strtoul(e.c_str() + 1, nullptr, 10));
            else out += "&" + e + ";";
            i = semi;
        }
        return out;
    }

    // Text content of a <tag>...</tag> whose opening tag was just read.
    std::string Content()
    {
        std::string text = Text(), closing;
        NextTag(closing);
        return text;
    }

    json::Value Element(const std::string& tag)
    {
        if (++m_depth > 32) return {}; // malformed or hostile input
        json::Value v;
        std::string t;
        if (tag == "dict")
        {
            v = json::Value::MakeObject();
            while (NextTag(t) && t != "/dict")
            {
                if (t != "key" && t != "key/") continue;
                const std::string key = t == "key" ? Content() : std::string();
                if (!NextTag(t)) break;
                v.Set(key, Element(t));
            }
        }
        else if (tag == "array")
        {
            v = json::Value::MakeArray();
            while (NextTag(t) && t != "/array") v.Push(Element(t));
        }
        else if (tag == "dict/") v = json::Value::MakeObject();
        else if (tag == "array/") v = json::Value::MakeArray();
        else if (tag == "true/") v = true;
        else if (tag == "false/") v = false;
        else if (tag == "string/" || tag == "data/") v = std::string();
        else if (tag == "integer") v = static_cast<int64_t>(strtoll(Content().c_str(), nullptr, 10));
        else if (tag == "real") v = strtod(Content().c_str(), nullptr);
        else if (tag == "string" || tag == "data" || tag == "date" || tag == "key") v = Content();
        --m_depth;
        return v;
    }

    const std::string& m_s;
    size_t m_p = 0;
    int m_depth = 0;
};

json::Value ParsePlist(const std::string& xml)
{
    try
    {
        return PlistReader(xml).Read();
    }
    catch (const std::exception&)
    {
        return {};
    }
}

// ---- usbmuxd -----------------------------------------------------------------------------

// Message: 16-byte little-endian header (length including header, version 1, type 8 = plist,
// tag), then an XML plist.
bool UsbmuxSend(Socket s, const std::string& messageType, const std::vector<std::pair<std::string, int64_t>>& integers = {})
{
    std::vector<std::pair<std::string, int64_t>> fields = integers;
    fields.emplace_back("kLibUSBMuxVersion", 3);
    const std::string body = PlistDict({ { "MessageType", messageType }, { "ProgName", "CamStudio" }, { "ClientVersionString", "CamStudio" } }, fields);
    uint8_t head[16];
    PutLE32(head, static_cast<uint32_t>(16 + body.size()));
    PutLE32(head + 4, 1);
    PutLE32(head + 8, 8);
    PutLE32(head + 12, 1);
    return SendAll(s, head, sizeof(head)) && SendAll(s, body.data(), body.size());
}

bool UsbmuxRead(Socket s, json::Value& out, int timeoutMs)
{
    uint8_t head[16];
    if (!RecvAll(s, head, sizeof(head), timeoutMs)) return false;
    const uint32_t length = GetLE32(head);
    if (length < 16 || length - 16 > kMaxPlist) return false;
    std::string body(length - 16, '\0');
    if (!body.empty() && !RecvAll(s, body.data(), body.size(), 5000)) return false;
    out = ParsePlist(body);
    return out.IsObject();
}

// Asks usbmuxd to connect to `port` on the phone; on success the socket becomes the tunnel.
// Returns usbmuxd's result code (0 = connected, 3 = refused), or -1 if usbmuxd is not there.
int UsbmuxConnect(uint32_t muxId, uint16_t port, Socket& tunnel)
{
    tunnel = kNoSocket;
    const Socket s = ConnectUsbmuxd();
    if (s == kNoSocket) return -1;
    // PortNumber is in network byte order, read as a little-endian integer.
    const uint16_t swapped = static_cast<uint16_t>((port >> 8) | (port << 8));
    json::Value reply;
    if (!UsbmuxSend(s, "Connect", { { "DeviceID", muxId }, { "PortNumber", swapped } }) || !UsbmuxRead(s, reply, 5000))
    {
        Close(s);
        return -1;
    }
    const int result = reply["Number"].Int(-1);
    if (result == 0) tunnel = s;
    else Close(s);
    return result;
}

bool DeviceFromProperties(const json::Value& props, uint32_t muxId, Device& d)
{
    if (props["ConnectionType"].Str("USB") != "USB") return false; // Wi‑Fi sync lists iPhones as "Network"
    d.kind = Kind::Usbmux;
    d.serial = props["SerialNumber"].Str();
    d.state = "device";
    d.muxId = muxId;
    return !d.serial.empty();
}

// lockdownd frames: u32 big-endian length, then an XML plist.
bool LockdownGet(Socket s, const std::string& key, std::string& value)
{
    const std::string body = PlistDict({ { "Label", "CamStudio" }, { "Request", "GetValue" }, { "Key", key } });
    const uint32_t n = static_cast<uint32_t>(body.size());
    const uint8_t head[4] = { uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n) };
    uint8_t len[4];
    if (!SendAll(s, head, 4) || !SendAll(s, body.data(), body.size()) || !RecvAll(s, len, 4, 3000)) return false;
    const uint32_t size = (uint32_t(len[0]) << 24) | (uint32_t(len[1]) << 16) | (uint32_t(len[2]) << 8) | len[3];
    if (size == 0 || size > kMaxPlist) return false;
    std::string reply(size, '\0');
    if (!RecvAll(s, reply.data(), size, 3000)) return false;
    value = ParsePlist(reply)["Value"].Str();
    return !value.empty();
}

const char* kAppNotOpen = "Không kết nối được tới app Cam Studio trên điện thoại qua cáp USB. Hãy mở app trên điện thoại.";
} // namespace

void Close(Socket s)
{
    if (s == kNoSocket) return;
#ifdef _WIN32
    closesocket(Native(s));
#else
    close(Native(s));
#endif
}

// ---- adb -----------------------------------------------------------------------------------

bool ListAdb(std::vector<Device>& out, std::string& error)
{
    out.clear();
    const Socket s = ConnectLoopback(kAdbPort);
    if (s == kNoSocket)
    {
        error = "adb chưa chạy";
        return false;
    }
    std::string failure, text;
    const bool ok = AdbSend(s, "host:devices-l", failure) && AdbReadBlock(s, text, 5000);
    Close(s);
    if (!ok)
    {
        error = "adb không trả lời: " + failure;
        return false;
    }
    // "<serial>   device product:lime_global model:M2010J19SG device:lime transport_id:1"
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::vector<std::string> words;
        size_t p = start;
        while (p < end)
        {
            const size_t w = text.find_first_not_of(" \t\r", p);
            if (w == std::string::npos || w >= end) break;
            size_t e = text.find_first_of(" \t\r", w);
            if (e == std::string::npos || e > end) e = end;
            words.push_back(text.substr(w, e - w));
            p = e;
        }
        start = end + 1;
        if (words.size() < 2 || !IsCable(words[0])) continue;
        Device d;
        d.kind = Kind::Adb;
        d.serial = words[0];
        d.state = words[1];
        if (d.state != "device" && d.state != "unauthorized" && d.state != "authorizing") continue; // offline, recovery...
        for (size_t i = 2; i < words.size(); ++i)
            if (words[i].rfind("model:", 0) == 0) d.model = words[i].substr(6);
        out.push_back(d);
    }
    return true;
}

Socket OpenAdb(const std::string& serial, uint16_t port, std::string& error)
{
    std::string failure;
    const Socket s = AdbTransport(serial, failure);
    if (s == kNoSocket)
    {
        if (failure == "no adb server")
            error = "Chưa chạy được adb để kết nối điện thoại Android qua cáp USB.";
        else if (Contains(failure, "unauthorized"))
            error = "Trên điện thoại, chọn \"Cho phép\" trong hộp thoại \"Cho phép gỡ lỗi USB?\" rồi thử lại.";
        else if (Contains(failure, "offline") || Contains(failure, "authorizing"))
            error = "Điện thoại đang kết nối lại qua cáp USB. Thử lại sau giây lát.";
        else
            error = "Không thấy điện thoại trên cáp USB. Cắm lại cáp, và bật \"Gỡ lỗi USB\" trong Tùy chọn cho nhà phát triển.";
        return kNoSocket;
    }
    // adbd connects to the port on the phone itself; FAIL ("closed") when nothing listens there.
    if (!AdbSend(s, "tcp:" + std::to_string(port), failure, 8000))
    {
        Close(s);
        error = kAppNotOpen;
        return kNoSocket;
    }
    return s;
}

std::string AdbDeviceName(const std::string& serial)
{
    // The same name the Android app uses (Store.deviceName): Settings.Global.DEVICE_NAME.
    const std::string name = Trim(AdbShell(serial, "settings get global device_name"));
    return name == "null" || Contains(name, "rror") || name.size() > 100 ? std::string() : name;
}

// ---- usbmux --------------------------------------------------------------------------------

bool ListUsbmux(std::vector<Device>& out, std::string& error)
{
    out.clear();
    const Socket s = ConnectUsbmuxd();
    if (s == kNoSocket)
    {
#ifdef _WIN32
        error = "Để dùng iPhone qua cáp USB, hãy cài app Apple Devices (Microsoft Store) hoặc iTunes trên máy tính.";
#else
        error = "usbmuxd không chạy";
#endif
        return false;
    }
    json::Value reply;
    const bool ok = UsbmuxSend(s, "ListDevices") && UsbmuxRead(s, reply, 5000);
    Close(s);
    if (!ok)
    {
        error = "Dịch vụ thiết bị Apple không trả lời";
        return false;
    }
    for (const json::Value& entry : reply["DeviceList"].Items())
    {
        Device d;
        if (DeviceFromProperties(entry["Properties"], static_cast<uint32_t>(entry["DeviceID"].Int()), d)) out.push_back(d);
    }
    return true;
}

Socket OpenUsbmux(const std::string& udid, uint16_t port, std::string& error)
{
    // The DeviceID changes every time the cable is plugged in: look it up by UDID each time.
    std::vector<Device> devices;
    if (!ListUsbmux(devices, error)) return kNoSocket;
    const Device* phone = nullptr;
    for (const Device& d : devices)
        if (d.serial == udid) phone = &d;
    if (!phone)
    {
        error = "Không thấy iPhone trên cáp USB. Cắm lại cáp và mở khóa iPhone.";
        return kNoSocket;
    }
    Socket tunnel = kNoSocket;
    const int result = UsbmuxConnect(phone->muxId, port, tunnel);
    if (result == 0) return tunnel;
    if (result == 3)
        error = "Không kết nối được tới app Cam Studio trên iPhone qua cáp USB. Hãy mở app; nếu iPhone hỏi "
                "\"Tin cậy máy tính này?\", chọn Tin cậy.";
    else if (result < 0)
        error = "Dịch vụ thiết bị Apple không trả lời";
    else
        error = "iPhone từ chối kết nối qua cáp USB (mã " + std::to_string(result) + ")";
    return kNoSocket;
}

std::string IphoneName(uint32_t muxId, std::string* productType)
{
    Socket s = kNoSocket;
    if (UsbmuxConnect(muxId, kLockdownPort, s) != 0) return {};
    std::string name;
    LockdownGet(s, "DeviceName", name);
    if (productType) LockdownGet(s, "ProductType", *productType);
    Close(s);
    return name;
}

// ---- watcher -------------------------------------------------------------------------------

void Watcher::Start(std::function<bool()> startAdb, std::function<bool()> stopIdleAdb)
{
    Stop();
    {
        std::lock_guard lk(m_lock);
        m_stop = false;
    }
    m_startAdb = std::move(startAdb);
    m_stopIdleAdb = std::move(stopIdleAdb);
    m_adbThread = std::thread([this] { RunAdb(); });
    m_usbmuxThread = std::thread([this] { RunUsbmux(); });
}

void Watcher::Stop()
{
    {
        std::lock_guard lk(m_lock);
        m_stop = true;
        // Wakes the blocking reads; each thread closes its own socket.
        if (m_adbSocket != kNoSocket) Shutdown(m_adbSocket);
        if (m_usbmuxSocket != kNoSocket) Shutdown(m_usbmuxSocket);
    }
    m_wake.notify_all();
    if (m_adbThread.joinable()) m_adbThread.join();
    if (m_usbmuxThread.joinable()) m_usbmuxThread.join();
    std::lock_guard lk(m_lock);
    m_adb.clear();
    m_usbmux.clear();
}

std::vector<Device> Watcher::Devices() const
{
    std::lock_guard lk(m_lock);
    std::vector<Device> all = m_adb;
    all.insert(all.end(), m_usbmux.begin(), m_usbmux.end());
    return all;
}

bool Watcher::Pause(int ms)
{
    std::unique_lock lk(m_lock);
    return !m_wake.wait_for(lk, std::chrono::milliseconds(ms), [this] { return m_stop; });
}

bool Watcher::Register(Socket& slot, Socket s)
{
    std::lock_guard lk(m_lock);
    if (m_stop) return false;
    slot = s;
    return true;
}

void Watcher::Unregister(Socket& slot, Socket s)
{
    {
        std::lock_guard lk(m_lock);
        slot = kNoSocket;
    }
    Close(s);
}

void Watcher::RunAdb()
{
    while (true)
    {
        const Socket s = ConnectLoopback(kAdbPort);
        if (s == kNoSocket)
        {
            // No adb server: maybe start one (only worth it with a phone plugged in), else look again.
            const bool started = m_startAdb && m_startAdb();
            if (!Pause(started ? 300 : 3000)) return;
            continue;
        }
        if (!Register(m_adbSocket, s))
        {
            Close(s);
            return;
        }
        std::string failure, block;
        // The server sends the device list now and again after every change.
        if (AdbSend(s, "host:track-devices", failure))
        {
            while (true)
            {
                bool none;
                {
                    std::lock_guard lk(m_lock);
                    none = m_adb.empty();
                }
                // No phone for a minute: let the server go if we started it. Its socket then closes.
                if (none && m_stopIdleAdb && !WaitReadable(s, 60000))
                {
                    m_stopIdleAdb();
                    continue;
                }
                if (!AdbReadBlock(s, block, -1)) break;
                UpdateAdb();
            }
        }
        Unregister(m_adbSocket, s);
        {
            std::lock_guard lk(m_lock);
            m_adb.clear(); // the server went away (killed, replaced by another adb version...)
        }
        if (!Pause(1000)) return;
    }
}

void Watcher::UpdateAdb()
{
    std::vector<Device> devices;
    std::string error;
    if (!ListAdb(devices, error)) return;
    for (Device& d : devices)
    {
        if (d.state != "device") continue; // unauthorized: nothing can be asked yet
        auto it = m_adbNames.find(d.serial);
        if (it == m_adbNames.end())
        {
            std::string name = AdbDeviceName(d.serial);
            if (name.empty()) name = d.model; // "Redmi_9T" style
            for (char& c : name)
                if (c == '_') c = ' ';
            it = m_adbNames.emplace(d.serial, name).first;
        }
        d.name = it->second;
    }
    std::lock_guard lk(m_lock);
    m_adb = std::move(devices);
}

void Watcher::RunUsbmux()
{
    while (true)
    {
        const Socket s = ConnectUsbmuxd();
        if (s == kNoSocket)
        {
            // No Apple Mobile Device Service (Windows without Apple Devices/iTunes): look again later.
            if (!Pause(5000)) return;
            continue;
        }
        if (!Register(m_usbmuxSocket, s))
        {
            Close(s);
            return;
        }
        std::map<uint32_t, Device> attached;
        json::Value reply, message;
        if (UsbmuxSend(s, "Listen") && UsbmuxRead(s, reply, 5000) && reply["Number"].Int(-1) == 0)
        {
            // "Attached" for every phone already there, then one message per change.
            while (UsbmuxRead(s, message, -1))
            {
                const std::string type = message["MessageType"].Str();
                const uint32_t id = static_cast<uint32_t>(message["DeviceID"].Int());
                if (type == "Attached")
                {
                    Device d;
                    if (!DeviceFromProperties(message["Properties"], id, d)) continue;
                    // lockdownd may not answer in the first moments after the cable goes in.
                    for (int attempt = 0; attempt < 3 && d.name.empty(); ++attempt)
                    {
                        d.name = IphoneName(id, &d.model);
                        if (d.name.empty() && !Pause(700)) break;
                    }
                    attached[id] = d;
                }
                else if (type == "Detached")
                {
                    attached.erase(id);
                }
                else
                {
                    continue;
                }
                std::vector<Device> list;
                for (const auto& [_, d] : attached) list.push_back(d);
                std::lock_guard lk(m_lock);
                m_usbmux = std::move(list);
            }
        }
        Unregister(m_usbmuxSocket, s);
        {
            std::lock_guard lk(m_lock);
            m_usbmux.clear();
        }
        if (!Pause(2000)) return;
    }
}

// ---- starting adb (Windows) ------------------------------------------------------------------

#ifdef _WIN32
namespace
{
// The interface class every phone adb can see registers (AdbWinApi's ANDROID_USB_CLASS_ID).
const GUID kAdbInterface = { 0xF72FE0D4, 0xCBCB, 0x407D, { 0x88, 0x14, 0x9E, 0xD6, 0x73, 0xD0, 0xDD, 0x6B } };
std::atomic<bool> g_startedAdb{ false };
std::atomic<ULONGLONG> g_lastStart{ 0 };

bool AdbPhonePlugged()
{
    ULONG size = 0;
    GUID guid = kAdbInterface;
    if (CM_Get_Device_Interface_List_SizeW(&size, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return false;
    return size > 1; // an empty list is just its terminating NUL
}

bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring FindAdb()
{
    // 1. Shipped with the installer: App\adb\adb.exe, next to CamStudioEngine.dll.
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&FindAdb), &self) &&
        GetModuleFileNameW(self, path, MAX_PATH))
    {
        std::wstring dir = path;
        dir.resize(dir.find_last_of(L'\\') + 1);
        if (FileExists(dir + L"adb\\adb.exe")) return dir + L"adb\\adb.exe";
    }
    // 2. The Android SDK (developer machines), 3. PATH.
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH))
    {
        const std::wstring sdk = std::wstring(path) + L"\\Android\\Sdk\\platform-tools\\adb.exe";
        if (FileExists(sdk)) return sdk;
    }
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, path, nullptr)) return path;
    return {};
}
} // namespace

bool StartAdbServer()
{
    // Without a phone that has USB debugging on, adb would find nothing: don't run it.
    if (!AdbPhonePlugged()) return false;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = g_lastStart;
    if (last && now - last < 30000) return false;
    g_lastStart = now;
    const std::wstring adb = FindAdb();
    if (adb.empty()) return false;
    std::wstring command = L"\"" + adb + L"\" start-server";
    const std::wstring dir = adb.substr(0, adb.find_last_of(L'\\'));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(adb.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
        return false;
    // "start-server" returns once the server answers; the server keeps running on its own.
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_startedAdb = true;
    return true;
}

void StopAdbServer()
{
    if (!g_startedAdb.exchange(false)) return;
    const Socket s = ConnectLoopback(kAdbPort);
    if (s == kNoSocket) return;
    std::string failure;
    AdbSend(s, "host:kill", failure, 2000);
    Close(s);
}

bool StopIdleAdbServer()
{
    if (!g_startedAdb || AdbPhonePlugged()) return false;
    StopAdbServer();
    return true;
}
#endif
} // namespace usb
