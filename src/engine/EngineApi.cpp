#define CAMSTUDIO_ENGINE_EXPORTS
#include "EngineApi.h"
#include "CaptureEngine.h"
#include "Discovery.h"
#include "Json.h"
#include "PhoneLink.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace
{
std::mutex g_lock;
std::unique_ptr<CaptureEngine> g_engine; // created in cs_init, never at DLL load
std::vector<CameraDevice> g_cameras;
std::vector<phone::Endpoint> g_endpoints;          // parallel to phone entries in g_cameras
std::map<std::string, std::shared_ptr<phone::PhoneLink>> g_links; // by host:port, reused
std::vector<FormatOption> g_formats;
int32_t g_formatsCamera = -1;
std::string g_phoneCaps;
std::wstring g_lastError;
std::wstring g_pairingCode;
std::atomic<int> g_pairingDecision{ -1 }; // -1 waiting, 0 declined, 1 accepted (PC user)
int32_t g_streamingCamera = -1;
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

std::string EndpointKey(const phone::Endpoint& e) { return e.host + ":" + std::to_string(e.port); }
} // namespace

CS_API int32_t cs_init()
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
        phone::StartDiscovery();
    }
    return S_OK;
}

CS_API void cs_shutdown()
{
    std::unique_ptr<CaptureEngine> engine;
    {
        std::lock_guard lk(g_lock);
        if (g_initCount == 0 || --g_initCount > 0) return;
        engine = std::move(g_engine);
        g_cameras.clear();
        g_formats.clear();
        g_links.clear();
    }
    engine.reset(); // joins the capture thread
    phone::StopDiscovery();
    MFShutdown();
}

CS_API int32_t cs_refresh_cameras()
{
    std::lock_guard lk(g_lock);
    // Keep the loaded formats and the streaming source valid across a
    // refresh: remember them by name and re-find them in the new list.
    auto nameAt = [](int32_t i) { return i >= 0 && i < static_cast<int32_t>(g_cameras.size()) ? g_cameras[i].name : std::wstring(); };
    const std::wstring formatsName = nameAt(g_formatsCamera);
    const std::wstring streamingName = nameAt(g_streamingCamera);
    g_cameras.clear();
    g_endpoints.clear();
    for (auto& c : EnumerateCameras())
        if (!IsOurVirtualCamera(c)) g_cameras.push_back(std::move(c)); // never loop our own output back
    for (const phone::Endpoint& e : phone::KnownPhones())
    {
        CameraDevice d;
        d.name = Widen(e.name.empty() ? e.host : e.name) + (e.manual ? L" (" + Widen(e.host) + L")" : L" (Wi‑Fi)");
        auto& link = g_links[EndpointKey(e)];
        if (!link) link = std::make_shared<phone::PhoneLink>();
        d.phone = link;
        g_cameras.push_back(std::move(d));
        g_endpoints.push_back(e);
    }
    auto find = [](const std::wstring& name) {
        for (size_t i = 0; i < g_cameras.size(); ++i)
            if (!name.empty() && g_cameras[i].name == name) return static_cast<int32_t>(i);
        return int32_t(-1);
    };
    g_formatsCamera = find(formatsName);
    if (g_formatsCamera < 0) g_formats.clear();
    g_streamingCamera = find(streamingName);
    return static_cast<int32_t>(g_cameras.size());
}

CS_API int32_t cs_camera_name(int32_t camera, wchar_t* buffer, int32_t capacity)
{
    std::lock_guard lk(g_lock);
    if (camera < 0 || camera >= static_cast<int32_t>(g_cameras.size())) return -1;
    return CopyOut(g_cameras[camera].name, buffer, capacity);
}

CS_API int32_t cs_camera_kind(int32_t camera)
{
    std::lock_guard lk(g_lock);
    if (camera < 0 || camera >= static_cast<int32_t>(g_cameras.size())) return -1;
    return g_cameras[camera].IsPhone() ? 1 : 0;
}

CS_API int32_t cs_load_formats(int32_t camera)
{
    CameraDevice cam;
    phone::Endpoint endpoint;
    {
        std::lock_guard lk(g_lock);
        if (camera < 0 || camera >= static_cast<int32_t>(g_cameras.size())) return E_INVALIDARG;
        cam = g_cameras[camera];
        if (cam.IsPhone())
        {
            const size_t webcams = g_cameras.size() - g_endpoints.size();
            endpoint = g_endpoints[static_cast<size_t>(camera) - webcams];
        }
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
        // While this phone streams, its connection belongs to the capture
        // thread: reuse the capabilities read before.
        const auto state = g_engine ? g_engine->GetStatus().state : CaptureEngine::State::Stopped;
        const bool streaming = g_streamingCamera == camera &&
            (state == CaptureEngine::State::Running || state == CaptureEngine::State::Waiting);
        json::Value caps;
        if (streaming && !g_phoneCaps.empty())
        {
            caps = json::Value::Parse(g_phoneCaps);
        }
        else
        {
            std::string error;
            auto connect = [&] {
                g_pairingDecision = -1;
                return cam.phone->Connect(endpoint,
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
            bool ok = cam.phone->Connected() || connect();
            if (ok && !cam.phone->RequestCaps(caps, error))
            {
                // The old connection may be dead (phone app restarted): one fresh try.
                cam.phone->Close();
                ok = connect() && cam.phone->RequestCaps(caps, error);
            }
            if (!ok)
            {
                cam.phone->Close();
                std::lock_guard lk(g_lock);
                g_lastError = Widen(error);
                return E_FAIL;
            }
        }
        capsText = caps.Dump();
        formats = PhoneFormats(caps);
        if (formats.empty())
        {
            std::lock_guard lk(g_lock);
            g_lastError = L"Điện thoại không có chế độ quay nào phù hợp";
            return E_FAIL;
        }
    }

    std::lock_guard lk(g_lock);
    g_formats = std::move(formats);
    g_formatsCamera = camera;
    g_phoneCaps = capsText;
    return static_cast<int32_t>(g_formats.size());
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

CS_API int32_t cs_start(int32_t camera, int32_t format, const CsImage* image)
{
    CameraDevice cam;
    NativeFormat fmt;
    CaptureEngine* engine;
    {
        std::lock_guard lk(g_lock);
        if (!g_engine) return E_UNEXPECTED;
        if (camera < 0 || camera >= static_cast<int32_t>(g_cameras.size())) return E_INVALIDARG;
        if (camera != g_formatsCamera || format < 0 || format >= static_cast<int32_t>(g_formats.size()))
            return E_INVALIDARG;
        cam = g_cameras[camera];
        fmt = g_formats[format].native;
        engine = g_engine.get();
        g_streamingCamera = camera;
    }
    engine->Start(cam, fmt, ToImage(image));
    return S_OK;
}

CS_API void cs_stop()
{
    if (g_engine) g_engine->Stop();
    g_streamingCamera = -1;
}

CS_API int32_t cs_set_image(const CsImage* image)
{
    return g_engine && g_engine->SetImage(ToImage(image)) ? 1 : 0;
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
        if (g_streamingCamera < 0 || g_streamingCamera >= static_cast<int32_t>(g_cameras.size())) return E_FAIL;
        link = g_cameras[g_streamingCamera].phone;
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
