#define CAMSTUDIO_ENGINE_EXPORTS
#include "EngineApi.h"
#include "CaptureEngine.h"
#include "Connector.h"
#include "Discovery.h"
#include "Json.h"
#include "PhoneLink.h"
#include "ScreenViewer.h"
#include "WindowSource.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <optional>
#include <memory>
#include <mutex>

namespace
{
std::mutex g_lock;
std::unique_ptr<CaptureEngine> g_engine; // created in cs_init, never at DLL load
// Keeps paired phones connected. Created once and only stopped in cs_shutdown: discovery
// callbacks may still call Wake() on it.
phone::Connector* g_connector = nullptr;
// The camera list as of the last cs_refresh_cameras. Indices change on every refresh; ids do not.
std::vector<CameraDevice> g_cameras;
std::vector<std::wstring> g_ids;                   // stable id per entry (cs_camera_id)
std::vector<int32_t> g_kinds;                      // 0 webcam, 1 phone camera, 2 phone screen
std::vector<phone::Endpoint> g_endpoints;          // parallel to phone entries in g_cameras
std::vector<FormatOption> g_formats;
std::wstring g_formatsId;                          // whose formats g_formats are
std::atomic<uint32_t> g_formatsRequest{ 0 };       // the newest cs_load_formats call
std::string g_phoneCaps;
std::wstring g_lastError;
std::wstring g_pairingCode;
std::atomic<int> g_pairingDecision{ -1 }; // -1 waiting, 0 declined, 1 accepted (PC user)
std::shared_ptr<phone::PhoneLink> g_streamingLink; // the phone the virtual camera shows, if any
std::map<int32_t, std::unique_ptr<ScreenViewer>> g_viewers; // screen windows, by cs_viewer_open id
int32_t g_nextViewer = 1;
std::vector<WindowInfo> g_windows; // the last cs_list_windows
int g_initCount = 0;

std::wstring Widen(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

int32_t CopyOut(const std::wstring& s, wchar_t* buffer, int32_t capacity)
{
    if (buffer && capacity > 0) wcsncpy_s(buffer, capacity, s.c_str(), _TRUNCATE);
    return static_cast<int32_t>(s.size());
}

int32_t CopyOut(const std::string& s, char* buffer, int32_t capacity)
{
    if (buffer && capacity > static_cast<int32_t>(s.size())) memcpy(buffer, s.c_str(), s.size() + 1);
    return static_cast<int32_t>(s.size()) + 1;
}

ImageSettings ToImage(const CsImage* in)
{
    ImageSettings s;
    if (!in) return s;
    s.orientation.rotation = ((in->rotation % 360) + 360) % 360;
    if (s.orientation.rotation % 90) s.orientation.rotation = 0;
    s.orientation.mirror = in->mirror != 0;
    s.orientation.flip = in->flip != 0;
    s.aspect = (in->aspect >= 0 && in->aspect <= 4) ? static_cast<CropAspect>(in->aspect) : CropAspect::Native;
    s.zoom = in->zoom;
    s.centerX = in->centerX;
    s.centerY = in->centerY;
    auto clamp100 = [](int32_t v) { return v < -100 ? -100 : v > 100 ? 100 : static_cast<int>(v); };
    s.color.brightness = clamp100(in->brightness);
    s.color.contrast = clamp100(in->contrast);
    s.color.saturation = clamp100(in->saturation);
    s.color.temperature = clamp100(in->temperature);
    return s;
}

bool HasHevcDecoder()
{
    static const bool has = [] {
        MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_HEVC };
        IMFActivate** a = nullptr;
        UINT32 n = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT, &in, nullptr, &a, &n))) return false;
        for (UINT32 i = 0; i < n; ++i) a[i]->Release();
        CoTaskMemFree(a);
        return n > 0;
    }();
    return has;
}

// Phone capabilities -> the format list: per lens (in the phone's order),
// biggest first, every listed frame rate. HEVC when both ends support it
// (better quality per Mbps over Wi‑Fi), else H.264.
std::vector<FormatOption> PhoneFormats(const json::Value& caps)
{
    std::map<std::string, std::pair<std::wstring, LensFacing>> lenses;
    std::vector<std::string> order;
    for (const auto& l : caps["lenses"].Items())
    {
        const std::string facing = l["facing"].Str();
        lenses[l["id"].Str()] = { Widen(l["label"].Str(l["id"].Str())),
            facing == "front" ? LensFacing::Front : facing == "external" ? LensFacing::External : LensFacing::Back };
        order.push_back(l["id"].Str());
    }

    std::vector<FormatOption> out;
    for (const std::string& lensId : order)
    {
        bool first = true;
        for (const auto& m : caps["modes"].Items())
        {
            if (m["lens"].Str() != lensId) continue;
            bool h264 = false, hevc = false;
            for (const auto& c : m["codecs"].Items())
            {
                h264 |= c.Str() == "h264";
                hevc |= c.Str() == "hevc";
            }
            // H.264 first: it always decodes on Windows. HEVC only when the
            // phone offers nothing else and the HEVC extension is installed.
            const bool useHevc = hevc && !h264 && HasHevcDecoder();
            if (!h264 && !useHevc) continue;
            std::vector<int> fpsList;
            for (const auto& f : m["fps"].Items()) fpsList.push_back(f.Int());
            std::sort(fpsList.rbegin(), fpsList.rend());
            for (int fps : fpsList)
            {
                FormatOption o;
                o.native.index = static_cast<DWORD>(out.size());
                o.native.width = static_cast<UINT32>(m["width"].Int());
                o.native.height = static_cast<UINT32>(m["height"].Int());
                o.native.fpsNum = static_cast<UINT32>(fps);
                o.native.fpsDen = 1;
                o.native.subtype = useHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
                o.native.phoneCamera = lensId;
                o.native.lens = lenses[lensId].second;
                o.lensLabel = lenses[lensId].first;
                o.largest = first; // modes are listed biggest first
                first = false;
                out.push_back(o);
            }
        }
    }
    return out;
}

bool ValidCamera(int32_t camera) { return camera >= 0 && camera < static_cast<int32_t>(g_cameras.size()); }

const phone::Endpoint& EndpointOf(int32_t camera)
{
    const size_t webcams = g_cameras.size() - g_endpoints.size();
    return g_endpoints[static_cast<size_t>(camera) - webcams];
}
} // namespace

CS_API int32_t cs_init()
{
    return cs_init_delayed(0);
}

CS_API int32_t cs_init_delayed(int32_t delayMs)
{
    std::lock_guard lk(g_lock);
    if (g_initCount++ == 0)
    {
        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr))
        {
            g_initCount = 0;
            return hr;
        }
        g_engine = std::make_unique<CaptureEngine>();
        if (!g_connector) g_connector = new phone::Connector; // lives until the process ends
        phone::SetOnChange([] { g_connector->Wake(); });
        // Discovery starts on the connector's thread, after the delay (it may start adb).
        g_connector->Start(static_cast<uint32_t>(std::max(0, delayMs)), [] { phone::StartDiscovery(); });
    }
    return S_OK;
}

CS_API void cs_shutdown()
{
    std::unique_ptr<CaptureEngine> engine;
    std::map<int32_t, std::unique_ptr<ScreenViewer>> viewers;
    {
        std::lock_guard lk(g_lock);
        if (g_initCount == 0 || --g_initCount > 0) return;
        engine = std::move(g_engine);
        viewers.swap(g_viewers);
        g_cameras.clear();
        g_ids.clear();
        g_kinds.clear();
        g_endpoints.clear();
        g_formats.clear();
        g_streamingLink.reset();
    }
    engine.reset(); // joins the capture thread
    viewers.clear(); // and the screen windows' threads
    g_connector->Stop();
    g_connector->Clear(); // closes the idle links
    phone::StopDiscovery();
    MFShutdown();
}

CS_API int32_t cs_refresh_cameras()
{
    // Cheap enough for the UI thread: the phone list is kept current in the background.
    std::lock_guard lk(g_lock);
    g_cameras.clear();
    g_ids.clear();
    g_kinds.clear();
    g_endpoints.clear();
    for (auto& c : EnumerateCameras())
    {
        if (IsOurVirtualCamera(c)) continue; // never loop our own output back
        g_ids.push_back(L"cam:" + (c.symbolicLink.empty() ? c.name : ToLower(c.symbolicLink)));
        g_kinds.push_back(0);
        g_cameras.push_back(std::move(c));
    }
    for (const phone::Endpoint& e : g_connector ? phone::KnownPhones() : std::vector<phone::Endpoint>())
    {
        CameraDevice d;
        d.name = Widen(e.name.empty() ? e.host : e.name) +
                 (e.Usb() ? L" (USB)" : e.manual ? L" (" + Widen(e.host) + L")" : L" (Wi‑Fi)");
        d.phone = g_connector->Link(e); // the same link every time: it may be connected already
        g_ids.push_back(L"phone:" + Widen(phone::EndpointKey(e)));
        g_kinds.push_back(e.screen ? 2 : 1);
        g_cameras.push_back(std::move(d));
        g_endpoints.push_back(e);
    }
    if (g_connector) g_connector->Wake();
    return static_cast<int32_t>(g_cameras.size());
}

CS_API int32_t cs_camera_name(int32_t camera, wchar_t* buffer, int32_t capacity)
{
    std::lock_guard lk(g_lock);
    if (!ValidCamera(camera)) return -1;
    return CopyOut(g_cameras[camera].name, buffer, capacity);
}

CS_API int32_t cs_camera_id(int32_t camera, wchar_t* buffer, int32_t capacity)
{
    std::lock_guard lk(g_lock);
    if (!ValidCamera(camera)) return -1;
    return CopyOut(g_ids[camera], buffer, capacity);
}

CS_API int32_t cs_camera_kind(int32_t camera)
{
    std::lock_guard lk(g_lock);
    if (!ValidCamera(camera)) return -1;
    return g_kinds[camera];
}

CS_API int32_t cs_camera_state(int32_t camera, wchar_t* error, int32_t capacity)
{
    phone::Endpoint endpoint;
    {
        std::lock_guard lk(g_lock);
        if (!ValidCamera(camera)) return -1;
        if (!g_cameras[camera].IsPhone())
        {
            CopyOut(std::wstring(), error, capacity);
            return static_cast<int32_t>(phone::LinkState::Connected); // a webcam is always there
        }
        endpoint = EndpointOf(camera);
    }
    std::string why;
    const phone::LinkState state = g_connector ? g_connector->State(phone::EndpointKey(endpoint), &why) : phone::LinkState::Idle;
    CopyOut(Widen(why), error, capacity);
    return static_cast<int32_t>(state);
}

CS_API uint32_t cs_sources_generation()
{
    return g_connector ? g_connector->Generation() : 0;
}

CS_API int32_t cs_load_formats(int32_t camera)
{
    CameraDevice cam;
    phone::Endpoint endpoint;
    std::wstring id;
    // A phone's load can take long (pairing): a newer call (another camera picked meanwhile) wins.
    const uint32_t request = ++g_formatsRequest;
    {
        std::lock_guard lk(g_lock);
        if (!ValidCamera(camera)) return E_INVALIDARG;
        cam = g_cameras[camera];
        id = g_ids[camera];
        if (cam.IsPhone()) endpoint = EndpointOf(camera);
        g_lastError.clear();
    }

    std::vector<FormatOption> formats;
    std::string capsText;
    if (!cam.IsPhone())
    {
        const HRESULT hr = ReadCameraFormats(cam, formats);
        if (FAILED(hr))
        {
            std::lock_guard lk(g_lock);
            g_lastError = L"Không đọc được định dạng của camera";
            return hr;
        }
    }
    else
    {
        // Usually instant: the connector (or an earlier load) already has the phone's caps, and
        // they are kept across reconnects. Otherwise connect now, pairing if needed.
        phone::PhoneLink& link = *cam.phone;
        capsText = link.Caps();
        // Held until the end of this branch: connecting, pairing and asking for caps read the
        // link, and the connector (or anyone else) must stay off it meanwhile.
        std::optional<phone::LeaseGuard> lease;
        if (capsText.empty())
        {
            // A connector attempt in progress is cut short: this one may pair.
            if (!link.Lease(30000))
            {
                std::lock_guard lk(g_lock);
                g_lastError = L"Điện thoại đang bận, hãy thử lại sau giây lát";
                return E_FAIL;
            }
            lease.emplace(&link);
            capsText = link.Caps(); // the connector may have just got them
        }
        if (capsText.empty())
        {
            json::Value caps;
            std::string error;
            auto connect = [&] {
                g_pairingDecision = -1;
                return link.Connect(endpoint,
                    [](const std::string& code) {
                        std::lock_guard lk(g_lock);
                        g_pairingCode = code.empty() ? L"" : Widen(code.substr(0, 3) + " " + code.substr(3));
                    },
                    [] {
                        // Wait for the PC user's answer in the pairing dialog.
                        for (int i = 0; i < 1200 && g_pairingDecision < 0; ++i) Sleep(100);
                        return g_pairingDecision == 1;
                    },
                    error);
            };
            bool ok = link.Connected() || connect();
            if (ok && !link.RequestCaps(caps, error))
            {
                // The old connection may be dead (phone app restarted): one fresh try.
                link.Close();
                ok = connect() && link.RequestCaps(caps, error);
            }
            if (!ok)
            {
                link.Close();
                std::lock_guard lk(g_lock);
                g_lastError = Widen(error);
                return E_FAIL;
            }
            capsText = caps.Dump();
        }
        json::Value caps;
        try
        {
            caps = json::Value::Parse(capsText);
        }
        catch (const std::exception&)
        {
            std::lock_guard lk(g_lock);
            g_lastError = L"Thông tin camera của điện thoại bị hỏng";
            return E_FAIL;
        }
        formats = PhoneFormats(caps);
        if (formats.empty())
        {
            std::lock_guard lk(g_lock);
            g_lastError = L"Điện thoại không có chế độ quay nào phù hợp";
            return E_FAIL;
        }
    }

    std::lock_guard lk(g_lock);
    const int32_t count = static_cast<int32_t>(formats.size());
    if (request != g_formatsRequest) return count; // superseded: the newer call's table stays
    g_formats = std::move(formats);
    g_formatsId = id;
    g_phoneCaps = capsText;
    return count;
}

CS_API int32_t cs_get_format(int32_t index, CsFormat* format)
{
    std::lock_guard lk(g_lock);
    if (!format || index < 0 || index >= static_cast<int32_t>(g_formats.size())) return E_INVALIDARG;
    const FormatOption& o = g_formats[index];
    *format = {};
    format->width = o.native.width;
    format->height = o.native.height;
    format->fpsNum = o.native.fpsNum;
    format->fpsDen = o.native.fpsDen;
    format->fourcc = o.native.subtype.Data1;
    format->isNative = o.largest ? 1 : 0;
    format->lens = static_cast<int32_t>(o.native.lens);
    wcsncpy_s(format->lensLabel, o.lensLabel.c_str(), _TRUNCATE);
    return S_OK;
}

CS_API int32_t cs_last_error(wchar_t* buffer, int32_t capacity)
{
    std::lock_guard lk(g_lock);
    return CopyOut(g_lastError, buffer, capacity);
}

CS_API int32_t cs_pairing_code(wchar_t* buffer, int32_t capacity)
{
    std::lock_guard lk(g_lock);
    return CopyOut(g_pairingCode, buffer, capacity);
}

CS_API void cs_pairing_answer(int32_t accept)
{
    g_pairingDecision = accept ? 1 : 0;
}

CS_API void cs_add_phone(const wchar_t* host, int32_t port)
{
    if (!host || port <= 0 || port > 65535) return;
    phone::AddManual(Narrow(host), static_cast<uint16_t>(port));
}

namespace
{
// The id a phone source's pairing key is saved under: the hello id once connected, else the TXT
// id (a screen advertises "<id>:screen", its hello says <id>).
std::string PairingId(const CameraDevice& cam, const phone::Endpoint& e)
{
    std::string id = cam.phone ? cam.phone->DeviceId() : std::string();
    if (id.empty()) id = e.deviceId;
    if (id.size() > 7 && id.compare(id.size() - 7, 7, ":screen") == 0) id.resize(id.size() - 7);
    return id;
}
} // namespace

CS_API int32_t cs_phone_flags(int32_t camera)
{
    std::lock_guard lk(g_lock);
    if (!ValidCamera(camera) || !g_cameras[camera].IsPhone()) return 0;
    const phone::Endpoint& e = EndpointOf(camera);
    return (e.manual ? CS_PHONE_MANUAL : 0) | (phone::HasPairKey(PairingId(g_cameras[camera], e)) ? CS_PHONE_PAIRED : 0);
}

CS_API void cs_forget_phone(int32_t camera, int32_t what)
{
    phone::Endpoint endpoint;
    std::vector<std::shared_ptr<phone::PhoneLink>> links;
    {
        std::lock_guard lk(g_lock);
        if (!ValidCamera(camera) || !g_cameras[camera].IsPhone()) return;
        endpoint = EndpointOf(camera);
        const std::string id = PairingId(g_cameras[camera], endpoint);
        if (what & CS_PHONE_PAIRED)
        {
            phone::ForgetPairKey(id);
            // Every route to this phone (Wi‑Fi, cable, its screen) still holds a session.
            for (int32_t i = 0; i < static_cast<int32_t>(g_cameras.size()); ++i)
                if (g_cameras[i].IsPhone() && g_cameras[i].phone &&
                    (i == camera || (!id.empty() && PairingId(g_cameras[i], EndpointOf(i)) == id)))
                    links.push_back(g_cameras[i].phone);
        }
    }
    for (auto& link : links) link->Abort(); // the lease holder closes it
    if ((what & CS_PHONE_MANUAL) && endpoint.manual) phone::RemoveManual(endpoint.host, endpoint.port);
    if (g_connector) g_connector->Wake();
}

namespace
{
int32_t StartOrSwitch(int32_t camera, int32_t format, const CsImage* image, bool keepCamera)
{
    CameraDevice cam;
    NativeFormat fmt;
    CaptureEngine* engine;
    {
        std::lock_guard lk(g_lock);
        if (!g_engine) return E_UNEXPECTED;
        if (!ValidCamera(camera)) return E_INVALIDARG;
        if (g_ids[camera] != g_formatsId || format < 0 || format >= static_cast<int32_t>(g_formats.size()))
            return E_INVALIDARG;
        cam = g_cameras[camera];
        fmt = g_formats[format].native;
        engine = g_engine.get();
        g_streamingLink = cam.phone;
    }
    uint32_t id = 0;
    if (keepCamera)
    {
        engine->SetImage(ToImage(image));
        id = engine->Switch(cam, fmt);
    }
    else
    {
        id = engine->Start(cam, fmt, ToImage(image));
    }
    return static_cast<int32_t>(id & 0x7FFFFFFF);
}
} // namespace

CS_API int32_t cs_start(int32_t camera, int32_t format, const CsImage* image)
{
    return StartOrSwitch(camera, format, image, false);
}

CS_API int32_t cs_switch(int32_t camera, int32_t format, const CsImage* image)
{
    return StartOrSwitch(camera, format, image, true);
}

CS_API int32_t cs_switch_none(int32_t width, int32_t height)
{
    CaptureEngine* engine = nullptr;
    {
        std::lock_guard lk(g_lock);
        if (!g_engine) return E_UNEXPECTED;
        engine = g_engine.get();
        g_streamingLink.reset();
    }
    NativeFormat fmt;
    fmt.width = width >= 2 && width <= 7680 ? static_cast<UINT32>(width) & ~1u : 1920;
    fmt.height = height >= 2 && height <= 7680 ? static_cast<UINT32>(height) & ~1u : 1080;
    fmt.fpsNum = 30;
    fmt.fpsDen = 1;
    fmt.subtype = MFVideoFormat_NV12;
    return static_cast<int32_t>(engine->Switch(CameraDevice{}, fmt) & 0x7FFFFFFF);
}

CS_API void cs_stop()
{
    if (g_engine) g_engine->Stop();
    std::lock_guard lk(g_lock);
    g_streamingLink.reset();
}

CS_API int32_t cs_set_image(const CsImage* image)
{
    return g_engine && g_engine->SetImage(ToImage(image)) ? 1 : 0;
}

CS_API void cs_set_output_size(int32_t mode)
{
    if (g_engine && mode >= 0 && mode <= 3) g_engine->SetOutputSize(static_cast<OutputSize>(mode));
}

CS_API void cs_set_auto_frame(int32_t enabled)
{
    if (g_engine) g_engine->SetAutoFrame(enabled != 0);
}

CS_API void cs_get_framing(CsFraming* out)
{
    if (!out) return;
    *out = {};
    out->zoom = out->maxZoom = 1.0;
    out->centerX = out->centerY = 0.5;
    out->available = 1;
    if (!g_engine) return;
    const CaptureEngine::Framing f = g_engine->GetFraming();
    out->zoom = f.zoom;
    out->centerX = f.centerX;
    out->centerY = f.centerY;
    out->maxZoom = f.maxZoom;
    out->active = f.active ? 1 : 0;
    out->faces = f.faces;
    out->available = f.available;
    out->cameraSource = f.cameraSource ? 1 : 0;
}

CS_API void cs_get_status(CsStatus* out)
{
    if (!out) return;
    *out = {};
    if (!g_engine) return;
    const CaptureEngine::Status s = g_engine->GetStatus();
    out->state = static_cast<int32_t>(s.state);
    out->width = s.width;
    out->height = s.height;
    out->cameraWidth = s.cameraWidth;
    out->cameraHeight = s.cameraHeight;
    out->cameraFourcc = s.cameraSubtype.Data1;
    out->fps = s.fps;
    out->processMs = s.processMs;
    out->framesCaptured = s.framesCaptured;
    out->framesDropped = s.framesDropped;
    out->sharedMemory = s.sharedMemory ? 1 : 0;
    out->consumerActive = s.consumerActive ? 1 : 0;
    out->sourceRotation = s.sourceRotation;
    out->standby = s.standby ? 1 : 0;
    out->pictureWidth = s.pictureWidth;
    out->pictureHeight = s.pictureHeight;
    out->sourceId = s.sourceId;
    out->noSource = s.noSource ? 1 : 0;
    wcsncpy_s(out->error, s.error.c_str(), _TRUNCATE);
}

CS_API void cs_set_preview_enabled(int32_t enabled)
{
    if (g_engine) g_engine->SetPreviewEnabled(enabled != 0);
}

CS_API void cs_set_preview_size(int32_t maxWidth, int32_t maxHeight)
{
    if (g_engine && maxWidth > 0 && maxHeight > 0)
        g_engine->SetPreviewSize(static_cast<uint32_t>(maxWidth), static_cast<uint32_t>(maxHeight));
}

CS_API int32_t cs_copy_preview(uint32_t* pixels, int32_t capacity, uint32_t* width, uint32_t* height, uint64_t* frameId)
{
    uint32_t w = 0, h = 0;
    uint64_t id = 0;
    bool copied = g_engine && g_engine->CopyPreview(pixels, capacity > 0 ? static_cast<size_t>(capacity) : 0, w, h, id);
    if (width) *width = w;
    if (height) *height = h;
    if (frameId) *frameId = id;
    return copied ? 1 : 0;
}

CS_API int32_t cs_phone_caps(char* buffer, int32_t capacity)
{
    std::lock_guard lk(g_lock);
    return CopyOut(g_phoneCaps, buffer, capacity);
}

CS_API int32_t cs_phone_state(char* buffer, int32_t capacity)
{
    const std::string state = g_engine ? g_engine->PhoneState() : std::string();
    return CopyOut(state, buffer, capacity);
}

CS_API int32_t cs_phone_send(const char* text)
{
    std::shared_ptr<phone::PhoneLink> link;
    {
        std::lock_guard lk(g_lock);
        link = g_streamingLink; // held, not looked up by index: refreshes may move it
    }
    if (!link || !text) return E_FAIL;
    try
    {
        return link->SendControl(json::Value::Parse(text)) ? S_OK : E_FAIL;
    }
    catch (const std::exception&)
    {
        return E_INVALIDARG;
    }
}

// ---- a phone's shared screen in its own window -------------------------------------------------

CS_API int32_t cs_viewer_open(int32_t camera)
{
    std::lock_guard lk(g_lock);
    if (!g_engine || !ValidCamera(camera) || g_kinds[camera] != 2) return E_INVALIDARG;
    const int32_t id = g_nextViewer++;
    g_viewers[id] = std::make_unique<ScreenViewer>(g_cameras[camera].phone, EndpointOf(camera));
    return id;
}

CS_API void cs_viewer_set_size(int32_t viewer, int32_t maxWidth, int32_t maxHeight)
{
    std::lock_guard lk(g_lock);
    const auto it = g_viewers.find(viewer);
    if (it != g_viewers.end())
        it->second->SetSize(static_cast<uint32_t>(std::max(0, maxWidth)), static_cast<uint32_t>(std::max(0, maxHeight)));
}

CS_API int32_t cs_viewer_copy(int32_t viewer, uint32_t* pixels, int32_t capacity, uint32_t* width, uint32_t* height, uint64_t* frameId)
{
    uint32_t w = 0, h = 0;
    uint64_t id = 0;
    bool copied = false;
    {
        std::lock_guard lk(g_lock);
        const auto it = g_viewers.find(viewer);
        if (it != g_viewers.end())
            copied = it->second->Copy(pixels, capacity > 0 ? static_cast<size_t>(capacity) : 0, w, h, id);
    }
    if (width) *width = w;
    if (height) *height = h;
    if (frameId) *frameId = id;
    return copied ? 1 : 0;
}

CS_API int32_t cs_viewer_state(int32_t viewer, uint32_t* screenWidth, uint32_t* screenHeight, wchar_t* error, int32_t capacity)
{
    uint32_t w = 0, h = 0;
    std::wstring why;
    int32_t state = -1;
    {
        std::lock_guard lk(g_lock);
        const auto it = g_viewers.find(viewer);
        if (it != g_viewers.end()) state = static_cast<int32_t>(it->second->GetState(w, h, why));
    }
    if (screenWidth) *screenWidth = w;
    if (screenHeight) *screenHeight = h;
    CopyOut(why, error, capacity);
    return state;
}

CS_API void cs_viewer_close(int32_t viewer)
{
    std::unique_ptr<ScreenViewer> closing;
    {
        std::lock_guard lk(g_lock);
        const auto it = g_viewers.find(viewer);
        if (it == g_viewers.end()) return;
        closing = std::move(it->second);
        g_viewers.erase(it);
    }
    closing.reset(); // joins its thread (a connect in progress is aborted)
}

// ---- a window on this PC as the camera's source --------------------------------------------------

CS_API int32_t cs_list_windows()
{
    std::vector<WindowInfo> windows = ListCaptureWindows();
    std::lock_guard lk(g_lock);
    g_windows = std::move(windows);
    return static_cast<int32_t>(g_windows.size());
}

CS_API int32_t cs_window_title(int32_t index, wchar_t* buffer, int32_t capacity)
{
    std::lock_guard lk(g_lock);
    if (index < 0 || index >= static_cast<int32_t>(g_windows.size())) return -1;
    return CopyOut(g_windows[index].title, buffer, capacity);
}

CS_API int64_t cs_window_handle(int32_t index)
{
    std::lock_guard lk(g_lock);
    if (index < 0 || index >= static_cast<int32_t>(g_windows.size())) return 0;
    return reinterpret_cast<int64_t>(g_windows[index].hwnd);
}

CS_API int32_t cs_switch_window(int64_t handle, const CsImage* image)
{
    const HWND hwnd = reinterpret_cast<HWND>(handle);
    if (!hwnd || !IsWindow(hwnd)) return E_INVALIDARG;
    CameraDevice cam;
    cam.window = hwnd;
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    cam.name = title;
    NativeFormat fmt;
    CaptureWindowSize(hwnd, fmt.width, fmt.height);
    fmt.fpsNum = 30;
    fmt.fpsDen = 1;
    fmt.subtype = MFVideoFormat_ARGB32;
    CaptureEngine* engine = nullptr;
    {
        std::lock_guard lk(g_lock);
        if (!g_engine) return E_UNEXPECTED;
        engine = g_engine.get();
        g_streamingLink.reset();
    }
    engine->SetImage(ToImage(image));
    return static_cast<int32_t>(engine->Switch(cam, fmt) & 0x7FFFFFFF);
}
