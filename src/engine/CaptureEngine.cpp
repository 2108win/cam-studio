#include "CaptureEngine.h"
#include "FrameWriter.h"
#include "PhoneLink.h"
#include "VideoDecoder.h"

#include <mfvirtualcamera.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr ULONGLONG kPreviewIntervalMs = 33; // ~30 fps is plenty for a preview

inline uint8_t Clamp255(int v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

double QpcMs()
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / freq;
}

std::wstring Widen(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
} // namespace

std::vector<FormatOption> BuildFormatOptions(const std::vector<NativeFormat>& formats)
{
    std::vector<FormatOption> options;
    for (const auto& f : formats)
    {
        if (!f.width || !f.height || !f.fpsNum) continue;
        auto same = std::find_if(options.begin(), options.end(), [&](const FormatOption& o) {
            return o.native.width == f.width && o.native.height == f.height &&
                   std::fabs(o.native.Fps() - f.Fps()) < 0.01;
        });
        if (same == options.end()) options.push_back({ f, false, {} });
        else if (SubtypeRank(f.subtype) > SubtypeRank(same->native.subtype)) same->native = f;
    }
    std::sort(options.begin(), options.end(), [](const FormatOption& a, const FormatOption& b) {
        if (a.native.Pixels() != b.native.Pixels()) return a.native.Pixels() > b.native.Pixels();
        return a.native.Fps() > b.native.Fps();
    });
    if (!options.empty()) options.front().largest = true;
    return options;
}

HRESULT ReadCameraFormats(const CameraDevice& cam, std::vector<FormatOption>& options)
{
    options.clear();
    ComPtr<IMFMediaSource> source;
    HRESULT hr = cam.activate->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr)) return hr;
    std::vector<NativeFormat> formats;
    hr = GetNativeFormats(source.Get(), formats);
    source->Shutdown();
    cam.activate->ShutdownObject();
    if (SUCCEEDED(hr)) options = BuildFormatOptions(formats);
    return hr;
}

// ---- Pipeline: shared by every source -------------------------------------------
//
// Owns the virtual camera registration and the shared-memory writer, and
// runs orient -> crop/zoom -> adjust -> publish on each NV12 frame.

class CaptureEngine::Pipeline
{
public:
    explicit Pipeline(CaptureEngine& engine) : m_engine(engine) {}
    ~Pipeline() { End(); }

    // Registers the virtual camera for this source size and marks the
    // session running. Called again (after End) when the size changes.
    HRESULT Begin(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen, const GUID& subtype)
    {
        m_width = width;
        m_height = height;
        m_interval100ns = fpsNum ? 1e7 * fpsDen / fpsNum : 333333.0;
        {
            std::lock_guard lk(m_engine.m_lock);
            m_layout = ComputeLayout(width, height, m_engine.EffectiveImageLocked());
        }
        HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
            MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser,
            CAMSTUDIO_VCAM_FRIENDLY_NAME, CAMSTUDIO_VCAM_CLSID_STRING, nullptr, 0, &m_vcam);
        if (FAILED(hr)) { m_failure = L"Không tạo được camera ảo (chạy scripts\\register.ps1 bằng quyền Admin)"; return hr; }
        MFSetAttributeSize(m_vcam.Get(), CAMSTUDIO_ATTR_NATIVE_SIZE, m_layout.outWidth, m_layout.outHeight);
        MFSetAttributeRatio(m_vcam.Get(), CAMSTUDIO_ATTR_NATIVE_FPS, fpsNum, fpsDen);
        hr = m_vcam->Start(nullptr);
        if (FAILED(hr)) { m_failure = L"Không bật được camera ảo"; return hr; }

        std::lock_guard lk(m_engine.m_lock);
        auto& s = m_engine.m_status;
        s.state = State::Running;
        s.width = m_layout.outWidth;
        s.height = m_layout.outHeight;
        s.cameraWidth = width;
        s.cameraHeight = height;
        s.cameraSubtype = subtype;
        m_engine.Notify();
        return S_OK;
    }

    void End()
    {
        m_writer.Close();
        if (m_vcam)
        {
            m_vcam->Stop();
            m_vcam->Shutdown();
            m_vcam.Reset();
        }
    }

    const std::wstring& Failure() const { return m_failure; }

    // True when the effective output size no longer matches this session
    // (e.g. the phone was turned): the caller rebuilds the virtual camera.
    bool SizeChanged() const
    {
        std::lock_guard lk(m_engine.m_lock);
        const FrameLayout l = ComputeLayout(m_width, m_height, m_engine.EffectiveImageLocked());
        return l.outWidth != m_layout.outWidth || l.outHeight != m_layout.outHeight;
    }

    // One source frame. timestamp100ns is on the source's own clock.
    void Frame(const uint8_t* data, long pitch, int64_t timestamp100ns)
    {
        // Dropped = frames the timeline says should have arrived minus those
        // that did; counting per gap would also count jitter as drops.
        if (m_firstTs < 0) m_firstTs = timestamp100ns;
        ++m_received;
        const double missing = std::floor((timestamp100ns - m_firstTs) / m_interval100ns + 1.5) - static_cast<double>(m_received);
        m_dropped = missing > 0 ? static_cast<uint64_t>(missing) : 0;

        m_writer.TryOpen();
        Publish(data, pitch);

        ++m_frames;
        const ULONGLONG now = GetTickCount64();
        if (now - m_statStart >= 1000)
        {
            std::lock_guard lk(m_engine.m_lock);
            auto& s = m_engine.m_status;
            s.fps = m_frames * 1000.0 / (now - m_statStart);
            s.framesCaptured += m_frames;
            s.framesDropped = m_dropped;
            s.processMs = m_workFrames ? m_workMs / m_workFrames : 0;
            s.sharedMemory = m_writer.IsOpen();
            s.consumerActive = m_writer.ConsumerActive();
            m_lastFps = s.fps;
            m_frames = 0;
            m_workMs = 0;
            m_workFrames = 0;
            m_statStart = now;
        }
    }

    double LastFps() const { return m_lastFps; }

private:
    void Publish(const uint8_t* data, long pitch)
    {
        const double t0 = QpcMs();
        ImageSettings image;
        {
            std::lock_guard lk(m_engine.m_lock);
            image = m_engine.EffectiveImageLocked();
        }
        const FrameLayout l = ComputeLayout(m_width, m_height, image);
        if (l.outWidth != m_layout.outWidth || l.outHeight != m_layout.outHeight)
            return; // size change pending: the session is being rebuilt

        // Nobody is watching (no app on the virtual camera, preview hidden):
        // skip all per-frame work, just keep the source flowing.
        const bool toCamera = m_writer.ConsumerActive();
        const bool toPreview = m_engine.PreviewDue();
        if (!toCamera && !toPreview) return;

        const uint8_t* frame = data;
        uint8_t* owned = nullptr; // set once the frame lives in our own buffer
        long framePitch = pitch;
        if (l.needsOrient)
        {
            m_oriented.resize(CamStudioNv12Bytes(l.orientedWidth, l.orientedHeight));
            Nv12Orient(data, pitch, m_width, m_height, m_oriented.data(), image.orientation);
            frame = owned = m_oriented.data();
            framePitch = static_cast<long>(l.orientedWidth);
        }
        if (l.needsResample)
        {
            m_output.resize(CamStudioNv12Bytes(l.outWidth, l.outHeight));
            Nv12CropScaleBilinear(frame, framePitch, l.orientedWidth, l.orientedHeight, l.source,
                m_output.data(), l.outWidth, l.outHeight);
            frame = owned = m_output.data();
            framePitch = static_cast<long>(l.outWidth);
        }
        if (!image.color.IsIdentity())
        {
            // In place on our own buffer, or copy-and-adjust from the source's.
            if (!owned)
            {
                m_output.resize(CamStudioNv12Bytes(l.outWidth, l.outHeight));
                owned = m_output.data();
            }
            Nv12Adjust(frame, framePitch, owned, l.outWidth, l.outHeight, image.color);
            frame = owned;
            framePitch = static_cast<long>(l.outWidth);
        }

        if (toCamera) m_writer.Write(frame, framePitch, l.outWidth, l.outHeight);
        if (toPreview) m_engine.UpdatePreview(frame, framePitch, l.outWidth, l.outHeight);
        m_workMs += QpcMs() - t0;
        ++m_workFrames;
    }

    CaptureEngine& m_engine;
    ComPtr<IMFVirtualCamera> m_vcam;
    FrameWriter m_writer;
    std::wstring m_failure;
    uint32_t m_width = 0, m_height = 0;
    FrameLayout m_layout;
    std::vector<uint8_t> m_oriented, m_output;
    double m_interval100ns = 333333.0;
    double m_workMs = 0;
    unsigned m_workFrames = 0;
    int64_t m_firstTs = -1;
    uint64_t m_received = 0, m_dropped = 0;
    unsigned m_frames = 0;
    ULONGLONG m_statStart = GetTickCount64();
    double m_lastFps = 0;
};

// ---- Engine control ------------------------------------------------------------------

void CaptureEngine::Start(const CameraDevice& cam, const NativeFormat& format, const ImageSettings& image)
{
    Stop();
    {
        std::lock_guard lk(m_lock);
        m_camera = cam;
        m_format = format;
        m_image = image;
        m_sourceRotation = 0;
        m_phoneState.clear();
    }
    StartLocked();
}

void CaptureEngine::StartLocked()
{
    CameraDevice cam;
    NativeFormat format;
    {
        std::lock_guard lk(m_lock);
        m_status = Status{};
        m_status.state = State::Starting;
        m_preview.clear();
        m_previewW = m_previewH = 0;
        cam = m_camera;
        format = m_format;
    }
    m_quit = false;
    m_thread = std::thread(&CaptureEngine::Run, this, cam, format);
    Notify();
}

void CaptureEngine::Stop()
{
    m_quit = true;
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard lk(m_lock);
    if (m_status.state != State::Failed) m_status.state = State::Stopped;
    m_status.fps = 0;
    m_status.sharedMemory = m_status.consumerActive = false;
}

CaptureEngine::Status CaptureEngine::GetStatus() const
{
    std::lock_guard lk(m_lock);
    Status s = m_status;
    s.sourceRotation = m_sourceRotation;
    return s;
}

std::string CaptureEngine::PhoneState() const
{
    std::lock_guard lk(m_lock);
    return m_phoneState;
}

ImageSettings CaptureEngine::EffectiveImageLocked() const
{
    ImageSettings s = m_image;
    s.orientation.rotation = (s.orientation.rotation + m_sourceRotation) % 360;
    return s;
}

bool CaptureEngine::SetImage(const ImageSettings& image)
{
    bool restart = false;
    {
        std::lock_guard lk(m_lock);
        const bool live = m_status.state == State::Running || m_status.state == State::Starting;
        const ImageSettings before = EffectiveImageLocked();
        m_image = image;
        if (live && m_status.cameraWidth)
        {
            const FrameLayout now = ComputeLayout(m_status.cameraWidth, m_status.cameraHeight, before);
            const FrameLayout next = ComputeLayout(m_status.cameraWidth, m_status.cameraHeight, EffectiveImageLocked());
            restart = now.outWidth != next.outWidth || now.outHeight != next.outHeight;
        }
    }
    if (restart)
    {
        // The virtual camera's size is fixed per session: publish a new one.
        m_quit = true;
        if (m_thread.joinable()) m_thread.join();
        StartLocked();
    }
    return restart;
}

bool CaptureEngine::PreviewDue()
{
    if (!m_previewEnabled) return false;
    const ULONGLONG now = GetTickCount64();
    if (now - m_lastPreviewTick < kPreviewIntervalMs) return false;
    m_lastPreviewTick = now;
    return true;
}

bool CaptureEngine::CopyPreview(uint32_t* dst, size_t capacity, uint32_t& width, uint32_t& height, uint64_t& frameId) const
{
    std::lock_guard lk(m_lock);
    width = m_previewW;
    height = m_previewH;
    frameId = m_previewId;
    if (m_preview.empty()) return false;
    if (!dst || capacity < m_preview.size()) return false; // caller learns the size first
    memcpy(dst, m_preview.data(), m_preview.size() * sizeof(uint32_t));
    return true;
}

void CaptureEngine::Notify() const
{
    if (m_notifyWindow) PostMessageW(m_notifyWindow, m_notifyMsg, 0, 0);
}

void CaptureEngine::Fail(const std::wstring& what, HRESULT hr)
{
    wchar_t buf[64] = {};
    if (hr) swprintf_s(buf, L" (0x%08X)", static_cast<unsigned>(hr));
    {
        std::lock_guard lk(m_lock);
        m_status.state = State::Failed;
        m_status.error = what + buf;
    }
    Notify();
}

// Downscaled NV12 -> BGRA (BT.601 limited range) for the on-screen preview.
void CaptureEngine::UpdatePreview(const uint8_t* nv12, long pitch, uint32_t w, uint32_t h)
{
    // Only as many pixels as the preview panel actually shows.
    const double scale = std::min({ 1.0, static_cast<double>(m_previewMaxW) / w, static_cast<double>(m_previewMaxH) / h });
    const uint32_t pw = std::max<uint32_t>(2, static_cast<uint32_t>(w * scale));
    const uint32_t ph = std::max<uint32_t>(2, static_cast<uint32_t>(h * scale));

    thread_local std::vector<uint32_t> buf;
    thread_local std::vector<uint32_t> columns;
    buf.resize(static_cast<size_t>(pw) * ph);
    columns.resize(pw);
    for (uint32_t x = 0; x < pw; ++x) columns[x] = static_cast<uint32_t>(static_cast<uint64_t>(x) * w / pw);

    const uint8_t* uvPlane = nv12 + static_cast<size_t>(pitch) * h;
    for (uint32_t y = 0; y < ph; ++y)
    {
        const uint32_t sy = static_cast<uint32_t>(static_cast<uint64_t>(y) * h / ph);
        const uint8_t* row = nv12 + static_cast<size_t>(sy) * pitch;
        const uint8_t* uv = uvPlane + static_cast<size_t>(sy / 2) * pitch;
        uint32_t* out = buf.data() + static_cast<size_t>(y) * pw;
        for (uint32_t x = 0; x < pw; ++x)
        {
            const uint32_t sx = columns[x];
            const int c = row[sx] - 16;
            const int d = uv[sx & ~1u] - 128;
            const int e = uv[(sx & ~1u) + 1] - 128;
            const uint8_t r = Clamp255((298 * c + 409 * e + 128) >> 8);
            const uint8_t g = Clamp255((298 * c - 100 * d - 208 * e + 128) >> 8);
            const uint8_t b = Clamp255((298 * c + 516 * d + 128) >> 8);
            out[x] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
        }
    }
    {
        std::lock_guard lk(m_lock);
        m_preview.swap(buf);
        m_previewW = pw;
        m_previewH = ph;
        ++m_previewId;
    }
    Notify();
}

// ---- Sources ------------------------------------------------------------------------------

void CaptureEngine::Run(CameraDevice cam, NativeFormat format)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        Fail(L"Không khởi tạo được COM", E_FAIL);
        return;
    }
    {
        Pipeline pipeline(*this);
        if (cam.IsPhone()) RunPhone(pipeline, cam, format);
        else RunWebcam(pipeline, cam, format);
    }
    CoUninitialize();
    Notify();
}

void CaptureEngine::RunWebcam(Pipeline& pipeline, const CameraDevice& cam, const NativeFormat& format)
{
    HRESULT hr = S_OK;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    UINT32 width = 0, height = 0;

#define STEP(expr, what) do { hr = (expr); if (FAILED(hr)) { Fail(what, hr); goto cleanup; } } while (0)

    // 1. Lock the camera to the chosen native format before the source
    //    reader exists, so the reader cannot renegotiate a smaller one.
    STEP(cam.activate->ActivateObject(IID_PPV_ARGS(&source)), L"Không mở được camera (app khác đang dùng độc quyền?)");
    {
        std::vector<NativeFormat> formats;
        ComPtr<IMFMediaTypeHandler> handler;
        STEP(GetNativeFormats(source.Get(), formats, &handler), L"Không đọc được định dạng của camera");
        ComPtr<IMFMediaType> nativeType;
        STEP(handler->GetMediaTypeByIndex(format.index, &nativeType), L"Định dạng không còn tồn tại");
        STEP(handler->SetCurrentMediaType(nativeType.Get()), L"Camera từ chối định dạng đã chọn");
    }

    // 2. Decode/convert to NV12 at the same size (e.g. MJPG -> NV12).
    {
        ComPtr<IMFAttributes> attrs;
        STEP(MFCreateAttributes(&attrs, 2), L"MFCreateAttributes");
        // Only insert the decoder/converter when the camera is not NV12
        // already; for NV12 cameras the frames pass straight through.
        if (format.subtype != MFVideoFormat_NV12)
            attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        STEP(MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), &reader), L"Không tạo được source reader");

        ComPtr<IMFMediaType> nv12;
        STEP(MFCreateMediaType(&nv12), L"MFCreateMediaType");
        nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, format.width, format.height);
        MFSetAttributeRatio(nv12.Get(), MF_MT_FRAME_RATE, format.fpsNum, format.fpsDen);
        STEP(reader->SetCurrentMediaType(kVideoStream, nullptr, nv12.Get()), L"Không chuyển được sang NV12");

        ComPtr<IMFMediaType> outType;
        STEP(reader->GetCurrentMediaType(kVideoStream, &outType), L"GetCurrentMediaType");
        MFGetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, &width, &height);
    }

    // 3. Virtual camera.
    hr = pipeline.Begin(width, height, format.fpsNum, format.fpsDen, format.subtype);
    if (FAILED(hr)) { Fail(pipeline.Failure(), hr); goto cleanup; }

    // 4. Capture loop.
    while (!m_quit)
    {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        STEP(reader->ReadSample(kVideoStream, 0, &streamIndex, &flags, &ts, &sample), L"Mất tín hiệu camera");
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))
        {
            Fail(L"Camera đã ngắt kết nối", MF_E_END_OF_STREAM);
            goto cleanup;
        }
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, &buffer))) continue;
        ComPtr<IMF2DBuffer> buffer2d;
        BYTE* data = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&data, &pitch)))
        {
            if (pitch > 0) pipeline.Frame(data, pitch, ts);
            buffer2d->Unlock2D();
        }
        else
        {
            DWORD len = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &len)))
            {
                if (len >= CamStudioNv12Bytes(width, height)) pipeline.Frame(data, static_cast<long>(width), ts);
                buffer->Unlock();
            }
        }
    }

cleanup:
#undef STEP
    pipeline.End();
    reader.Reset();
    if (source) source->Shutdown();
    cam.activate->ShutdownObject();
}

void CaptureEngine::RunPhone(Pipeline& pipeline, const CameraDevice& cam, const NativeFormat& format)
{
    phone::PhoneLink& link = *cam.phone;
    const bool hevc = format.subtype == MFVideoFormat_HEVC;
    const uint32_t fps = format.fpsDen ? format.fpsNum / format.fpsDen : 30;
    // ~0.1 bit per pixel per frame: 1080p30 ≈ 6 Mbps, 4K30 ≈ 25 Mbps.
    const uint32_t kbps = std::clamp<uint32_t>(
        static_cast<uint32_t>(static_cast<uint64_t>(format.width) * format.height * fps / 10000), 2000, 60000);

    if (!link.SendControl(json::Value::MakeObject()
            .Set("type", "start").Set("lens", format.phoneCamera)
            .Set("width", format.width).Set("height", format.height).Set("fps", fps)
            .Set("codec", hevc ? "hevc" : "h264").Set("bitrateKbps", kbps)))
    {
        Fail(L"Mất kết nối với điện thoại", 0);
        return;
    }

    auto stateFrom = [&](const json::Value& v) {
        std::lock_guard lk(m_lock);
        if (!v["orientation"].IsNull()) m_sourceRotation = ((v["orientation"].Int() % 360) + 360) % 360;
        if (v["type"].Str() == "state") m_phoneState = v.Dump();
    };

    // Wait for "started" (or an error) before decoding anything.
    json::Value started;
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (!m_quit)
    {
        phone::Message m;
        bool timedOut = false;
        if (!link.Receive(m, 500, timedOut))
        {
            if (timedOut && GetTickCount64() < deadline) continue;
            Fail(timedOut ? L"Điện thoại không bắt đầu phát" : L"Mất kết nối với điện thoại", 0);
            return;
        }
        if (m.kind != phone::Kind::Control) continue;
        json::Value v;
        try { v = m.Json(); } catch (const std::exception&) { continue; }
        if (v["type"].Str() == "error") { Fail(Widen(v["message"].Str("Điện thoại báo lỗi")), 0); return; }
        if (v["type"].Str() == "started") { started = v; stateFrom(v); break; }
    }
    if (m_quit) return;

    const uint32_t width = static_cast<uint32_t>(started["width"].Int(format.width));
    const uint32_t height = static_cast<uint32_t>(started["height"].Int(format.height));
    VideoDecoder decoder;
    HRESULT hr = decoder.Init(started["codec"].Str() == "h264" ? false : hevc, width, height);
    if (FAILED(hr))
    {
        Fail(hevc ? L"Không có bộ giải mã HEVC (cài \"HEVC Video Extensions\" từ Microsoft Store)" : L"Không tạo được bộ giải mã video", hr);
        link.SendControl(json::Value::MakeObject().Set("type", "stop"));
        return;
    }
    hr = pipeline.Begin(width, height, fps, 1, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    if (FAILED(hr))
    {
        Fail(pipeline.Failure(), hr);
        link.SendControl(json::Value::MakeObject().Set("type", "stop"));
        return;
    }

    crypto::Bytes config;     // latest SPS/PPS(/VPS), prepended to keyframes
    bool waitKeyframe = true; // decoding starts at a keyframe
    uint32_t lost = 0;
    double decodeMs = 0;
    unsigned decoded = 0;
    ULONGLONG lastFrame = GetTickCount64();
    ULONGLONG lastStats = lastFrame;
    std::vector<uint8_t> unit;

    while (!m_quit)
    {
        phone::Message m;
        bool timedOut = false;
        const bool got = link.Receive(m, 500, timedOut);
        const ULONGLONG now = GetTickCount64();
        if (!got && !timedOut)
        {
            Fail(L"Mất kết nối với điện thoại", 0);
            break;
        }
        if (now - lastFrame > 6000)
        {
            Fail(L"Điện thoại ngừng gửi hình (app trên điện thoại bị ẩn hoặc màn hình tắt?)", 0);
            break;
        }

        if (got && m.kind == phone::Kind::Control)
        {
            json::Value v;
            try { v = m.Json(); } catch (const std::exception&) { continue; }
            const std::string type = v["type"].Str();
            if (type == "error") { Fail(Widen(v["message"].Str("Điện thoại báo lỗi")), 0); break; }
            if (type == "stopped") { Fail(L"Điện thoại đã dừng phát", 0); break; }
            if (type == "state" || type == "started") stateFrom(v);
            // The phone was turned: rebuild the virtual camera in the new shape.
            if (pipeline.SizeChanged())
            {
                pipeline.End();
                if (FAILED(pipeline.Begin(width, height, fps, 1, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264)))
                {
                    Fail(pipeline.Failure(), 0);
                    break;
                }
            }
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
            const double t0 = QpcMs();
            hr = decoder.Decode(data, size, m.ptsUs * 10, [&](const uint8_t* nv12, long pitch, uint32_t, uint32_t) {
                pipeline.Frame(nv12, pitch, m.ptsUs * 10);
            });
            decodeMs += QpcMs() - t0;
            ++decoded;
            if (FAILED(hr))
            {
                // Corrupt or missing data: ask for a fresh keyframe.
                ++lost;
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
            link.SendControl(json::Value::MakeObject()
                .Set("type", "stats").Set("recvFps", pipeline.LastFps())
                .Set("decodeMs", decoded ? decodeMs / decoded : 0.0)
                .Set("queueMs", 0).Set("lost", static_cast<int>(lost)));
            lost = 0;
            decodeMs = 0;
            decoded = 0;
            lastStats = now;
        }
    }

    link.SendControl(json::Value::MakeObject().Set("type", "stop"));
    pipeline.End();
}
