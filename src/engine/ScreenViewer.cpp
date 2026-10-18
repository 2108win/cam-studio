#include "ScreenViewer.h"
#include "VideoDecoder.h" // windows.h, Media Foundation
#include "../common/Nv12.h"
#include "PhoneLink.h"

#include <objbase.h>

#include <algorithm>

namespace
{
constexpr uint64_t kDrawIntervalMs = 30; // a window shows ~30 pictures a second at most

std::wstring Widen(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

struct ScreenMode
{
    uint32_t width = 0, height = 0, fps = 30;
    bool hevc = false;
};

// The screen's full size (the biggest mode), H.264 when offered (it always decodes on Windows),
// at most 30 fps: a window does not need more.
bool PickMode(const json::Value& caps, ScreenMode& out)
{
    bool found = false;
    for (const auto& m : caps["modes"].Items())
    {
        bool h264 = false, hevc = false;
        for (const auto& c : m["codecs"].Items())
        {
            h264 |= c.Str() == "h264";
            hevc |= c.Str() == "hevc";
        }
        if (!h264 && !hevc) continue;
        const uint32_t w = static_cast<uint32_t>(m["width"].Int());
        const uint32_t h = static_cast<uint32_t>(m["height"].Int());
        if (found && static_cast<uint64_t>(w) * h <= static_cast<uint64_t>(out.width) * out.height) continue;
        int fps = 0, lowest = 0;
        for (const auto& f : m["fps"].Items())
        {
            const int v = f.Int();
            if (v <= 30 && v > fps) fps = v;
            if (!lowest || v < lowest) lowest = v;
        }
        out = { w, h, static_cast<uint32_t>(fps ? fps : lowest ? lowest : 30), !h264 };
        found = w && h;
    }
    return found;
}
} // namespace

ScreenViewer::ScreenViewer(std::shared_ptr<phone::PhoneLink> link, const phone::Endpoint& endpoint)
    : m_link(std::move(link)), m_endpoint(endpoint)
{
    m_thread = std::thread(&ScreenViewer::Run, this);
}

ScreenViewer::~ScreenViewer()
{
    m_quit = true;
    if (m_connecting) m_link->Abort(); // a connect can take up to ~12 s
    if (m_thread.joinable()) m_thread.join();
}

void ScreenViewer::SetSize(uint32_t maxWidth, uint32_t maxHeight)
{
    m_maxW = std::min<uint32_t>(maxWidth, 7680);
    m_maxH = std::min<uint32_t>(maxHeight, 7680);
}

bool ScreenViewer::Copy(uint32_t* dst, size_t capacity, uint32_t& width, uint32_t& height, uint64_t& frameId) const
{
    std::lock_guard lk(m_lock);
    width = m_width;
    height = m_height;
    frameId = m_frameId;
    if (m_pixels.empty() || !dst || capacity < m_pixels.size()) return false;
    memcpy(dst, m_pixels.data(), m_pixels.size() * sizeof(uint32_t));
    return true;
}

ScreenViewer::State ScreenViewer::GetState(uint32_t& screenWidth, uint32_t& screenHeight, std::wstring& error) const
{
    std::lock_guard lk(m_lock);
    screenWidth = m_screenW;
    screenHeight = m_screenH;
    error = m_error;
    return m_state;
}

void ScreenViewer::SetState(State state, const std::wstring& error)
{
    std::lock_guard lk(m_lock);
    m_state = state;
    m_error = error;
}

void ScreenViewer::Run()
{
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); // the decoder MFT
    while (!m_quit)
    {
        SetState(State::Connecting, L"");
        std::wstring why;
        Stream(why);
        if (m_quit) break;
        SetState(State::Waiting, why.empty() ? L"Mất kết nối với điện thoại" : why);
        for (int i = 0; i < 20 && !m_quit; ++i) Sleep(100);
    }
    if (com) CoUninitialize();
}

void ScreenViewer::Stream(std::wstring& why)
{
    phone::PhoneLink& link = *m_link;
    // One reader per link. The connector gives way at once; when the virtual camera streams
    // this screen, the window waits until it moves to another source.
    for (int waited = 0; !link.Lease(200); ++waited)
    {
        if (m_quit) return;
        if (waited == 5) SetState(State::Waiting, L"Màn hình này đang là nguồn của camera ảo. Chọn nguồn khác cho camera ảo để xem ở đây.");
    }
    phone::LeaseGuard lease(&link);
    SetState(State::Connecting, L"");

    std::string error;
    if (!link.Connected())
    {
        // A link that connected before knows where to find the phone again (its IP may have
        // changed); a new one uses the address it was listed with. Never pairs: the screen
        // cannot (docs/protocol.md 1.1), pairing happens with the camera.
        m_connecting = true;
        const bool ok = !m_quit && (link.DeviceId().empty() ? link.ConnectKnown(m_endpoint, error) : link.Reconnect(error));
        m_connecting = false;
        if (!ok)
        {
            why = Widen(error);
            return;
        }
    }

    json::Value caps;
    try
    {
        const std::string text = link.Caps();
        if (!text.empty()) caps = json::Value::Parse(text);
    }
    catch (const std::exception&)
    {
    }
    m_connecting = true; // closing the window cuts this short too
    const bool haveCaps = caps.IsObject() || (!m_quit && link.RequestCaps(caps, error, 10000));
    m_connecting = false;
    if (!haveCaps)
    {
        link.Close();
        why = Widen(error);
        return;
    }
    ScreenMode mode;
    if (!PickMode(caps, mode))
    {
        why = L"Điện thoại không có chế độ chia sẻ màn hình nào phù hợp";
        return;
    }
    const uint32_t kbps = std::clamp<uint32_t>(
        static_cast<uint32_t>(static_cast<uint64_t>(mode.width) * mode.height * mode.fps / 8000), 2000, 40000);
    if (!link.SendControl(json::Value::MakeObject()
            .Set("type", "start").Set("lens", "screen")
            .Set("width", mode.width).Set("height", mode.height).Set("fps", mode.fps)
            .Set("codec", mode.hevc ? "hevc" : "h264").Set("bitrateKbps", kbps)))
    {
        link.Close();
        why = L"Mất kết nối với điện thoại";
        return;
    }

    auto stateFrom = [&](const json::Value& v) {
        if (!v["orientation"].IsNull()) m_rotation = ((v["orientation"].Int() % 360) + 360) % 360;
    };

    json::Value started;
    const uint64_t deadline = GetTickCount64() + 10000;
    while (!m_quit)
    {
        phone::Message m;
        bool timedOut = false;
        if (!link.Receive(m, 500, timedOut))
        {
            if (timedOut && GetTickCount64() < deadline) continue;
            link.Close();
            why = timedOut ? L"Điện thoại không bắt đầu chia sẻ màn hình" : L"Mất kết nối với điện thoại";
            return;
        }
        if (m.kind != phone::Kind::Control) continue;
        json::Value v;
        try { v = m.Json(); } catch (const std::exception&) { continue; }
        if (v["type"].Str() == "error")
        {
            why = Widen(v["message"].Str("Điện thoại báo lỗi"));
            return;
        }
        if (v["type"].Str() == "started") { started = v; stateFrom(v); break; }
    }
    if (m_quit)
    {
        link.SendControl(json::Value::MakeObject().Set("type", "stop"));
        return;
    }

    const uint32_t width = static_cast<uint32_t>(started["width"].Int(static_cast<int>(mode.width)));
    const uint32_t height = static_cast<uint32_t>(started["height"].Int(static_cast<int>(mode.height)));
    VideoDecoder decoder;
    if (FAILED(decoder.Init(started["codec"].Str() == "h264" ? false : mode.hevc, width, height)))
    {
        link.SendControl(json::Value::MakeObject().Set("type", "stop"));
        why = mode.hevc ? L"Không có bộ giải mã HEVC (cài \"HEVC Video Extensions\" từ Microsoft Store)" : L"Không tạo được bộ giải mã video";
        return;
    }
    SetState(State::Showing, L"");

    crypto::Bytes config;
    bool waitKeyframe = true;
    std::vector<uint8_t> unit;
    uint32_t lostFrames = 0, frames = 0;
    uint64_t lastFrame = GetTickCount64();
    uint64_t lastStats = lastFrame;
    while (!m_quit)
    {
        phone::Message m;
        bool timedOut = false;
        const bool got = link.Receive(m, 500, timedOut);
        const uint64_t now = GetTickCount64();
        if (!got && !timedOut)
        {
            link.Close();
            why = L"Mất kết nối với điện thoại";
            return;
        }
        if (now - lastFrame > 6000)
        {
            // The phone re-sends a still screen several times a second: silence means it stopped.
            link.SendControl(json::Value::MakeObject().Set("type", "stop"));
            link.Close();
            why = L"Điện thoại ngừng chia sẻ màn hình";
            return;
        }
        if (got && m.kind == phone::Kind::Control)
        {
            json::Value v;
            try { v = m.Json(); } catch (const std::exception&) { continue; }
            const std::string type = v["type"].Str();
            if (type == "error") { why = Widen(v["message"].Str("Điện thoại báo lỗi")); return; }
            if (type == "stopped") { why = L"Điện thoại đã dừng chia sẻ màn hình"; return; }
            if (type == "state" || type == "started") stateFrom(v);
        }
        else if (got && m.kind == phone::Kind::VideoConfig)
        {
            config = m.payload;
        }
        else if (got && m.kind == phone::Kind::VideoFrame)
        {
            lastFrame = now;
            if (waitKeyframe && !m.Keyframe()) continue;
            const uint8_t* data = m.payload.data();
            size_t size = m.payload.size();
            if (m.Keyframe() && !config.empty())
            {
                unit.assign(config.begin(), config.end());
                unit.insert(unit.end(), m.payload.begin(), m.payload.end());
                data = unit.data();
                size = unit.size();
            }
            const HRESULT hr = decoder.Decode(data, size, m.ptsUs * 10, [&](const uint8_t* nv12, long pitch, uint32_t w, uint32_t h) {
                ++frames;
                Show(nv12, pitch, w, h);
            });
            if (FAILED(hr))
            {
                ++lostFrames;
                waitKeyframe = true;
                link.SendControl(json::Value::MakeObject().Set("type", "keyframe"));
            }
            else
            {
                waitKeyframe = false;
            }
        }
        if (now - lastStats >= 1000)
        {
            // For the phone's bitrate adaptation (docs/protocol.md 6).
            link.SendControl(json::Value::MakeObject()
                .Set("type", "stats").Set("recvFps", frames * 1000.0 / static_cast<double>(now - lastStats))
                .Set("decodeMs", 0.0).Set("queueMs", 0).Set("lost", static_cast<int>(lostFrames)));
            frames = lostFrames = 0;
            lastStats = now;
        }
    }
    link.SendControl(json::Value::MakeObject().Set("type", "stop"));
}

// Upright, at most the window's size (never bigger than the screen itself), then BGRA.
void ScreenViewer::Show(const uint8_t* nv12, long pitch, uint32_t width, uint32_t height)
{
    Nv12Orientation o;
    o.rotation = m_rotation;
    const uint32_t fw = o.SwapsSize() ? height : width;
    const uint32_t fh = o.SwapsSize() ? width : height;
    {
        std::lock_guard lk(m_lock);
        m_screenW = fw;
        m_screenH = fh;
    }
    const uint32_t maxW = m_maxW, maxH = m_maxH;
    const uint64_t now = GetTickCount64();
    if (!maxW || !maxH || now - m_lastShown < kDrawIntervalMs) return; // minimized, or drawn just now
    m_lastShown = now;

    const uint8_t* frame = nv12;
    long framePitch = pitch;
    if (!o.IsIdentity())
    {
        m_oriented.resize(static_cast<size_t>(width) * height * 3 / 2);
        Nv12Orient(nv12, pitch, width, height, m_oriented.data(), o);
        frame = m_oriented.data();
        framePitch = static_cast<long>(fw);
    }

    const double scale = std::min({ 1.0, static_cast<double>(maxW) / fw, static_cast<double>(maxH) / fh });
    const uint32_t ow = std::max<uint32_t>(2, static_cast<uint32_t>(fw * scale) & ~1u);
    const uint32_t oh = std::max<uint32_t>(2, static_cast<uint32_t>(fh * scale) & ~1u);
    const uint8_t* y = frame;
    const uint8_t* uv = frame + static_cast<size_t>(framePitch) * fh;
    long p = framePitch;
    if (ow != fw || oh != fh)
    {
        // Bilinear, so text stays readable when the window is smaller than the screen.
        m_scaled.resize(static_cast<size_t>(ow) * oh * 3 / 2);
        Nv12CropScaleBilinear(frame, framePitch, fw, fh, Nv12Rect{ 0, 0, static_cast<double>(fw), static_cast<double>(fh) },
                              m_scaled.data(), ow, oh);
        y = m_scaled.data();
        uv = y + static_cast<size_t>(ow) * oh;
        p = static_cast<long>(ow);
    }
    m_work.resize(static_cast<size_t>(ow) * oh);
    Nv12ToBgra(y, uv, p, ow, oh, m_work.data(), ow, oh);
    std::lock_guard lk(m_lock);
    m_pixels.swap(m_work);
    m_width = ow;
    m_height = oh;
    ++m_frameId;
}
