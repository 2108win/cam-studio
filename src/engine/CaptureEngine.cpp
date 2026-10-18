#include "CaptureEngine.h"
#include "FrameWriter.h"
#include "PhoneLink.h"
#include "VideoDecoder.h"
#include "WindowSource.h"

#include <mfvirtualcamera.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr ULONGLONG kPreviewIntervalMs = 33; // ~30 fps is plenty for a preview
// Source closed after this long with no app on the camera and no preview on
// screen (an app switching cameras or a quick glance should not reopen it).
// MFCreateVirtualCamera exists on Windows 11 only (mfsensorgroup.dll, build 22000). Looked up at
// run time so the engine loads on Windows 10, where a DirectShow source filter (src/dshow) reads
// the frames instead and no camera is registered here (docs/windows10.md).
using CreateVirtualCameraFn = HRESULT(STDAPICALLTYPE*)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess,
                                                      LPCWSTR, LPCWSTR, const GUID*, ULONG, IMFVirtualCamera**);
CreateVirtualCameraFn VirtualCameraApi()
{
    static const CreateVirtualCameraFn fn = []() -> CreateVirtualCameraFn {
        // CAMSTUDIO_FORCE_DSHOW=1: the Windows 10 path on any Windows, for testing.
        if (GetEnvironmentVariableW(L"CAMSTUDIO_FORCE_DSHOW", nullptr, 0)) return nullptr;
        const HMODULE m = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? reinterpret_cast<CreateVirtualCameraFn>(GetProcAddress(m, "MFCreateVirtualCamera")) : nullptr;
    }();
    return fn;
}

constexpr ULONGLONG kIdleCloseMs = 5000;
// An app that stopped streaming this recently still counts as using the camera: apps restart
// their stream when they change mode (Discord does), and rebuilding the camera then drops it.
constexpr ULONGLONG kConsumerGraceMs = 5000;
// How long the last picture stays on the camera while the next source opens (refreshed while
// it is still opening). After that, or once the source is reported lost, apps show "no signal".
constexpr uint32_t kHoldMs = 10000;

// "No source": nothing to open (CaptureEngine::Switch with an empty CameraDevice).
bool IsNoSource(const CameraDevice& cam)
{
    return !cam.IsPhone() && !cam.IsPcWindow() && !cam.activate;
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
// runs orient -> crop/zoom -> fit -> adjust -> publish on each NV12 frame.
// It outlives a source: Switch() hands the next source the same camera.

class CaptureEngine::Pipeline
{
public:
    explicit Pipeline(CaptureEngine& engine) : m_engine(engine), m_dshow(VirtualCameraApi() == nullptr)
    {
        m_writer.UseLocal(m_dshow);
    }
    ~Pipeline() { End(); }

    // Windows 10: frames go to the DirectShow filter's channel, no camera is registered.
    bool DirectShow() const { return m_dshow; }

    // A source (a new one, or the same one again) delivers width x height frames from now on.
    // Publishes the virtual camera for it, or keeps the one already published when an app is
    // using it (PlanOutput): apps using it (Discord, Zoom…) never lose it.
    HRESULT Begin(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen, const GUID& subtype)
    {
        ++m_begins;
        m_width = width;
        m_height = height;
        m_fpsNum = fpsNum ? fpsNum : 30;
        m_fpsDen = fpsNum ? fpsDen : 1;
        m_interval100ns = fpsNum ? 1e7 * fpsDen / fpsNum : 333333.0;
        m_firstTs = -1;
        m_received = m_dropped = 0;
        Relayout();
        if (m_broken) return m_brokenHr;

        std::lock_guard lk(m_engine.m_lock);
        auto& s = m_engine.m_status;
        s.state = State::Running;
        s.error.clear();
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
            RemoveCamera(m_vcam.Get());
            m_vcam->Shutdown();
            m_vcam.Reset();
        }
        m_vcamWidth = m_vcamHeight = 0;
    }

    // Unregisters the camera outright. Without this a DirectShow app that
    // keeps its graph after the camera went away (Discord does) holds the old
    // instance, and Frame Server refuses the next one with E_ACCESSDENIED.
    static void RemoveCamera(IMFVirtualCamera* vcam)
    {
        const HRESULT hr = vcam->Remove();
        wchar_t msg[96];
        swprintf_s(msg, L"[CamStudioEngine] IMFVirtualCamera::Remove 0x%08X\n", static_cast<unsigned>(hr));
        OutputDebugStringW(msg);
    }

    // The virtual camera is registered (it outlives a lost source). Windows 10: the channel is open.
    bool Live() const { return m_dshow ? m_vcamWidth != 0 : m_vcam != nullptr; }
    // How many times a source started (Begin): tells whether a run got that far.
    uint32_t Begins() const { return m_begins; }
    // The camera had to be rebuilt and could not be: nothing more can be published.
    bool Broken() const { return m_broken; }

    // Some app is streaming from the virtual camera right now (Frame Server
    // started our source's stream). Cheap: the mapping is looked up at most
    // twice a second until it exists.
    bool ConsumerActive()
    {
        m_writer.TryOpen();
        return m_writer.ConsumerActive();
    }

    const std::wstring& Failure() const { return m_failure; }

    // Keeps the last picture on the camera while the next source opens (see FrameWriter::HoldFor).
    void Hold() { m_writer.HoldFor(kHoldMs); }
    void ReleaseHold() { m_writer.ReleaseHold(); }

    // Picks up a change of picture shape (settings, the phone turned), of the size setting, or
    // an app letting go of the camera. Called on every frame, and by Run in standby; cheap when
    // nothing changed.
    void Tick()
    {
        const uint32_t gen = m_engine.m_layoutGen;
        const ULONGLONG now = GetTickCount64();
        if (gen == m_layoutGenSeen && now - m_lastRelayout < 1000) return;
        m_layoutGenSeen = gen;
        m_lastRelayout = now;
        if (m_width) Relayout();
    }

    // One source frame. timestamp100ns is on the source's own clock.
    void Frame(const uint8_t* data, long pitch, int64_t timestamp100ns)
    {
        Count(timestamp100ns);
        Tick();
        if (m_broken) return;
        m_writer.TryOpen();
        Publish(data, pitch);
    }

    // A frame that arrived but is not worth converting (see WillPublish): counted only.
    void Count(int64_t timestamp100ns)
    {
        // Dropped = frames the timeline says should have arrived minus those
        // that did; counting per gap would also count jitter as drops.
        if (m_firstTs < 0) m_firstTs = timestamp100ns;
        ++m_received;
        const double missing = std::floor((timestamp100ns - m_firstTs) / m_interval100ns + 1.5) - static_cast<double>(m_received);
        m_dropped = missing > 0 ? static_cast<uint64_t>(missing) : 0;

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

    // Whether the next frame will go anywhere (an app, the 1-a-second keep-fresh write, the
    // preview). A window source checks this before reading the frame back from the GPU.
    bool WillPublish()
    {
        m_writer.TryOpen();
        return m_writer.ConsumerActive() || (m_writer.IsOpen() && GetTickCount64() - m_lastWriteTick >= 1000) ||
               m_engine.PreviewWanted();
    }

    double LastFps() const { return m_lastFps; }

private:
    // Applies the size rule and computes the layout for the current settings.
    void Relayout()
    {
        ImageSettings image;
        OutputSize mode;
        {
            std::lock_guard lk(m_engine.m_lock);
            image = m_engine.EffectiveImageLocked();
            mode = static_cast<OutputSize>(m_engine.m_outputSize.load());
        }
        const FrameLayout natural = ComputeLayout(m_width, m_height, image);
        const ULONGLONG now = GetTickCount64();
        const bool consumer = ConsumerActive();
        if (consumer) m_consumerSeen = now;
        const bool holds = consumer || (m_consumerSeen && now - m_consumerSeen < kConsumerGraceMs);
        OutputPlan plan = PlanOutput(mode, natural.pictureWidth, natural.pictureHeight,
                                     Live() ? m_vcamWidth : 0, Live() ? m_vcamHeight : 0, holds);
        // A rebuild that was refused a moment ago (an app still holds the old camera): stay
        // fitted for a while instead of trying again on every frame.
        if (plan.rebuild && Live() && now < m_noRebuildUntil) plan = { m_vcamWidth, m_vcamHeight, false };
        if (plan.rebuild) Register(plan.width, plan.height);

        m_image = image;
        m_layout = Live() ? ComputeLayout(m_width, m_height, image, m_vcamWidth, m_vcamHeight) : natural;
        std::lock_guard lk(m_engine.m_lock);
        auto& s = m_engine.m_status;
        s.width = m_layout.outWidth;
        s.height = m_layout.outHeight;
        s.pictureWidth = m_layout.pictureWidth;
        s.pictureHeight = m_layout.pictureHeight;
    }

    // Publishes the virtual camera at this size, in place of the current one.
    void Register(uint32_t width, uint32_t height)
    {
        const uint32_t oldW = m_vcamWidth, oldH = m_vcamHeight;
        HRESULT hr = Create(width, height);
        if (FAILED(hr) && oldW && hr == E_ACCESSDENIED && SUCCEEDED(Create(oldW, oldH)))
        {
            // An app (DirectShow) still holds the camera we just removed: put back the size it
            // had, keep fitting into it, and try the new size later.
            m_noRebuildUntil = GetTickCount64() + 30000;
            hr = S_OK;
        }
        m_broken = FAILED(hr);
        m_brokenHr = hr;
    }

    HRESULT Create(uint32_t width, uint32_t height)
    {
        if (m_dshow)
        {
            // No camera to register: the channel the DirectShow filter reads is ours. It takes
            // this size unless an app streams from it right now (then its size stays, and the
            // picture is fitted into it, like a camera an app holds).
            uint32_t w = 0, h = 0;
            const HRESULT lhr = m_writer.OpenLocal(width, height, w, h);
            if (FAILED(lhr))
            {
                m_failure = L"Không mở được bộ nhớ chia sẻ của camera ảo";
                return lhr;
            }
            m_vcamWidth = w;
            m_vcamHeight = h;
            return S_OK;
        }
        End();
        HRESULT hr = E_FAIL;
        // E_ACCESSDENIED: an app still holds a camera instance we removed a
        // moment ago. Retry a few times with a fresh registration.
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            if (attempt) Sleep(500);
            hr = VirtualCameraApi()(MFVirtualCameraType_SoftwareCameraSource,
                MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser,
                CAMSTUDIO_VCAM_FRIENDLY_NAME, CAMSTUDIO_VCAM_CLSID_STRING, nullptr, 0, &m_vcam);
            if (FAILED(hr)) { m_failure = L"Không tạo được camera ảo (hãy cài lại Cam Studio bằng file CamStudio-Setup)"; return hr; }
            MFSetAttributeSize(m_vcam.Get(), CAMSTUDIO_ATTR_NATIVE_SIZE, width, height);
            MFSetAttributeRatio(m_vcam.Get(), CAMSTUDIO_ATTR_NATIVE_FPS, m_fpsNum, m_fpsDen);
            hr = m_vcam->Start(nullptr);
            if (SUCCEEDED(hr)) break;
            RemoveCamera(m_vcam.Get());
            m_vcam->Shutdown();
            m_vcam.Reset();
            if (hr != E_ACCESSDENIED) break;
        }
        if (FAILED(hr))
        {
            // An app (DirectShow, e.g. Discord) still holds an old instance of
            // the camera: closing it, or a fresh Frame Server, clears this.
            m_failure = hr == E_ACCESSDENIED
                ? L"Một ứng dụng (thường là Discord) vẫn giữ camera ảo cũ. Tắt camera trong ứng dụng đó rồi bật lại, hoặc khởi động lại dịch vụ camera"
                : L"Không bật được camera ảo";
            return hr;
        }
        m_vcamWidth = width;
        m_vcamHeight = height;
        m_consumerSeen = 0;
        return S_OK;
    }

    void Publish(const uint8_t* data, long pitch)
    {
        const double t0 = QpcMs();
        const FrameLayout& l = m_layout;
        const ImageSettings& image = m_image;

        // Nobody is watching (no app on the virtual camera, preview hidden):
        // skip the per-frame work, just keep the source flowing. One frame a
        // second still goes to the camera, so an app that opens it gets a
        // picture at once rather than "no signal" until the next frame of a
        // slow source (a still phone screen sends ~4 a second).
        const ULONGLONG tick = GetTickCount64();
        const bool toCamera = m_writer.ConsumerActive() || (m_writer.IsOpen() && tick - m_lastWriteTick >= 1000);
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

        // The picture alone (no bars): what the preview shows.
        const uint8_t* picY = nullptr;
        const uint8_t* picUv = nullptr;
        long picPitch = 0;
        if (l.fitted)
        {
            // Scaled into its rectangle of a frame at the camera's size. The bars are painted
            // once per layout; frames only write the rectangle.
            const size_t bytes = CamStudioNv12Bytes(l.outWidth, l.outHeight);
            const uint32_t rect[6] = { l.outWidth, l.outHeight, l.destX, l.destY, l.destWidth, l.destHeight };
            if (m_fit.size() != bytes || !std::equal(rect, rect + 6, m_barsFor))
            {
                m_fit.resize(bytes);
                Nv12FillBars(m_fit.data(), l.outWidth, l.outHeight, l.destX, l.destY, l.destWidth, l.destHeight);
                std::copy(rect, rect + 6, m_barsFor);
            }
            uint8_t* dstY = m_fit.data() + static_cast<size_t>(l.destY) * l.outWidth + l.destX;
            uint8_t* dstUv = m_fit.data() + static_cast<size_t>(l.outWidth) * l.outHeight +
                             static_cast<size_t>(l.destY / 2) * l.outWidth + l.destX;
            Nv12CropScaleBilinearTo(frame, framePitch, l.orientedWidth, l.orientedHeight, l.source,
                                    dstY, dstUv, static_cast<long>(l.outWidth), l.destWidth, l.destHeight);
            if (!image.color.IsIdentity())
                Nv12AdjustPlanes(dstY, dstUv, static_cast<long>(l.outWidth), dstY, dstUv, static_cast<long>(l.outWidth),
                                 l.destWidth, l.destHeight, image.color); // the bars stay black
            frame = m_fit.data();
            framePitch = static_cast<long>(l.outWidth);
            picY = dstY;
            picUv = dstUv;
            picPitch = framePitch;
        }
        else
        {
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
            picY = frame;
            picUv = frame + static_cast<size_t>(framePitch) * l.outHeight;
            picPitch = framePitch;
        }

        if (toCamera)
        {
            m_writer.Write(frame, framePitch, l.outWidth, l.outHeight);
            m_lastWriteTick = tick;
        }
        if (toPreview) m_engine.UpdatePreview(picY, picUv, picPitch, l.destWidth, l.destHeight);
        m_workMs += QpcMs() - t0;
        ++m_workFrames;
    }

    CaptureEngine& m_engine;
    const bool m_dshow;
    ComPtr<IMFVirtualCamera> m_vcam;
    uint32_t m_vcamWidth = 0, m_vcamHeight = 0; // size the virtual camera was published with
    uint32_t m_fpsNum = 30, m_fpsDen = 1;
    FrameWriter m_writer;
    std::wstring m_failure;
    bool m_broken = false;
    HRESULT m_brokenHr = S_OK;
    uint32_t m_width = 0, m_height = 0;           // the source's frames
    uint32_t m_begins = 0;
    FrameLayout m_layout;
    ImageSettings m_image;
    uint32_t m_layoutGenSeen = ~0u;
    ULONGLONG m_lastRelayout = 0;
    ULONGLONG m_consumerSeen = 0;                 // last time an app was streaming
    ULONGLONG m_noRebuildUntil = 0;
    std::vector<uint8_t> m_oriented, m_output, m_fit;
    uint32_t m_barsFor[6] = {};                   // the layout m_fit's bars were painted for
    double m_interval100ns = 333333.0;
    double m_workMs = 0;
    unsigned m_workFrames = 0;
    int64_t m_firstTs = -1;
    uint64_t m_received = 0, m_dropped = 0;
    unsigned m_frames = 0;
    ULONGLONG m_lastWriteTick = 0;
    ULONGLONG m_statStart = GetTickCount64();
    double m_lastFps = 0;
};

// ---- Engine control ------------------------------------------------------------------

uint32_t CaptureEngine::Start(const CameraDevice& cam, const NativeFormat& format, const ImageSettings& image)
{
    Stop();
    uint32_t id = 0;
    {
        std::lock_guard lk(m_lock);
        m_camera = cam;
        m_format = format;
        m_image = image;
        m_sourceRotation = ExpectedRotationLocked(cam, format);
        m_phoneState.clear();
        id = ++m_sourceSeq;
    }
    StartLocked();
    std::lock_guard lk(m_lock);
    m_status.sourceId = id;
    return id;
}

uint32_t CaptureEngine::Switch(const CameraDevice& cam, const NativeFormat& format)
{
    bool running = false;
    ImageSettings image;
    {
        std::lock_guard lk(m_lock);
        running = m_thread.joinable() && m_status.state != State::Failed && m_status.state != State::Stopped;
        image = m_image;
    }
    if (!running) return Start(cam, format, image);

    std::shared_ptr<phone::PhoneLink> blocking;
    uint32_t id = 0;
    {
        std::lock_guard lk(m_lock);
        m_nextCamera = cam;
        m_nextFormat = format;
        id = m_nextSourceId = ++m_sourceSeq;
        m_switch = true;
        blocking = m_blocking;
    }
    // The source may be stuck reconnecting to a phone that is gone (up to ~12 s): cut it short.
    if (blocking) blocking->Abort();
    return id;
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
    m_switch = false;
    m_thread = std::thread(&CaptureEngine::Run, this, cam, format);
    Notify();
}

void CaptureEngine::Stop()
{
    m_quit = true;
    std::shared_ptr<phone::PhoneLink> blocking;
    {
        std::lock_guard lk(m_lock);
        blocking = m_blocking;
    }
    if (blocking) blocking->Abort();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard lk(m_lock);
    if (m_status.state != State::Failed) m_status.state = State::Stopped;
    m_status.fps = 0;
    m_status.sharedMemory = m_status.consumerActive = false;
}

void CaptureEngine::SetBlocking(const std::shared_ptr<phone::PhoneLink>& link)
{
    std::lock_guard lk(m_lock);
    m_blocking = link;
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

int CaptureEngine::ExpectedRotationLocked(const CameraDevice& cam, const NativeFormat& format) const
{
    if (!cam.IsPhone()) return 0;
    const auto it = m_rotationMemory.find(cam.name + L"|" + Widen(format.phoneCamera));
    if (it != m_rotationMemory.end()) return it->second;
    // First time: phones are usually held upright. Their camera sensors are
    // mounted sideways (90), and an Android screen streams landscape (270).
    return format.phoneCamera == "screen" ? 270 : 90;
}

bool CaptureEngine::SourceWanted(Pipeline& pipeline)
{
    return m_previewEnabled || pipeline.ConsumerActive();
}

bool CaptureEngine::StillWanted(Pipeline& pipeline, ULONGLONG& idleSince)
{
    if (SourceWanted(pipeline))
    {
        idleSince = 0;
        return true;
    }
    const ULONGLONG now = GetTickCount64();
    if (!idleSince) idleSince = now;
    return now - idleSince < kIdleCloseMs;
}

void CaptureEngine::SetStandby(bool standby)
{
    {
        std::lock_guard lk(m_lock);
        if (m_status.standby == standby) return;
        m_status.standby = standby;
        if (standby)
        {
            m_status.fps = 0;
            m_status.consumerActive = false;
        }
    }
    Notify();
}

ImageSettings CaptureEngine::EffectiveImageLocked() const
{
    ImageSettings s = m_image;
    s.orientation.rotation = (s.orientation.rotation + m_sourceRotation) % 360;
    return s;
}

bool CaptureEngine::SetImage(const ImageSettings& image)
{
    std::lock_guard lk(m_lock);
    const bool live = m_status.state == State::Running || m_status.state == State::Starting || m_status.state == State::Waiting;
    const ImageSettings before = EffectiveImageLocked();
    m_image = image;
    ++m_layoutGen; // the pipeline applies it on the next frame (or in standby)
    if (!live || !m_status.cameraWidth) return false;
    const FrameLayout a = ComputeLayout(m_status.cameraWidth, m_status.cameraHeight, before);
    const FrameLayout b = ComputeLayout(m_status.cameraWidth, m_status.cameraHeight, EffectiveImageLocked());
    return a.pictureWidth != b.pictureWidth || a.pictureHeight != b.pictureHeight;
}

bool CaptureEngine::PreviewDue()
{
    if (!m_previewEnabled) return false;
    const ULONGLONG now = GetTickCount64();
    if (now - m_lastPreviewTick < kPreviewIntervalMs) return false;
    m_lastPreviewTick = now;
    return true;
}

bool CaptureEngine::PreviewWanted() const
{
    return m_previewEnabled && GetTickCount64() - m_lastPreviewTick >= kPreviewIntervalMs;
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

void CaptureEngine::Wait(const std::wstring& what, HRESULT hr)
{
    wchar_t buf[64] = {};
    if (hr) swprintf_s(buf, L" (0x%08X)", static_cast<unsigned>(hr));
    {
        std::lock_guard lk(m_lock);
        m_status.state = State::Waiting;
        m_status.error = what + buf;
        m_status.fps = 0;
    }
    Notify();
}

// Downscaled NV12 -> BGRA (BT.601 limited range) for the on-screen preview.
void CaptureEngine::UpdatePreview(const uint8_t* y, const uint8_t* uv, long pitch, uint32_t w, uint32_t h)
{
    // Only as many pixels as the preview panel actually shows.
    const double scale = std::min({ 1.0, static_cast<double>(m_previewMaxW) / w, static_cast<double>(m_previewMaxH) / h });
    const uint32_t pw = std::max<uint32_t>(2, static_cast<uint32_t>(w * scale));
    const uint32_t ph = std::max<uint32_t>(2, static_cast<uint32_t>(h * scale));

    thread_local std::vector<uint32_t> buf;
    buf.resize(static_cast<size_t>(pw) * ph);
    Nv12ToBgra(y, uv, pitch, w, h, buf.data(), pw, ph);
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
        CameraDevice webcam = cam; // found again after the webcam was unplugged
        bool lost = false;
        bool begun = false;  // the pipeline knows this source's size
        bool stuck = false;  // this source cannot run (Fatal while the camera is up): wait for another
        while (!m_quit)
        {
            if (m_switch)
            {
                // Another source on the same virtual camera. Apps using it keep the last picture,
                // frozen, until the new source's first frame.
                pipeline.Hold();
                std::lock_guard lk(m_lock);
                m_switch = false;
                cam = m_camera = m_nextCamera;
                format = m_format = m_nextFormat;
                m_sourceRotation = ExpectedRotationLocked(cam, format);
                ++m_layoutGen;
                m_phoneState.clear();
                m_status.sourceId = m_nextSourceId;
                m_status.framesCaptured = m_status.framesDropped = 0;
                m_status.fps = 0;
                m_status.error.clear();
                m_status.noSource = IsNoSource(cam);
                if (m_status.state == State::Waiting) m_status.state = pipeline.Live() ? State::Running : State::Starting;
                webcam = cam;
                lost = false;
                begun = false;
                stuck = false;
            }
            if (stuck)
            {
                for (int i = 0; i < 2 && !Interrupted(); ++i) Sleep(100);
                continue;
            }

            if (IsNoSource(cam))
            {
                // No source (none found yet, or the picture is turned off): the camera stays
                // registered, at the size it has, and apps that use it see "no signal". Only
                // whether an app streams from it is watched (the app looks for a source then).
                if (!pipeline.Live())
                {
                    const HRESULT hr = pipeline.Begin(format.width, format.height, format.fpsNum, format.fpsDen, MFVideoFormat_NV12);
                    if (FAILED(hr)) { Fail(pipeline.Failure(), hr); break; }
                }
                pipeline.ReleaseHold();
                SetStandby(false);
                {
                    std::lock_guard lk(m_lock);
                    m_status.noSource = true;
                    m_status.state = State::Running;
                    m_status.error.clear();
                    m_status.fps = 0;
                }
                Notify();
                while (!Interrupted())
                {
                    pipeline.Tick(); // a new size setting applies with no source too
                    const bool consumer = pipeline.ConsumerActive();
                    bool changed = false;
                    {
                        std::lock_guard lk(m_lock);
                        changed = m_status.consumerActive != consumer;
                        m_status.consumerActive = consumer;
                    }
                    if (changed) Notify();
                    Sleep(200);
                }
                continue;
            }

            // Standby: nothing needs a picture (no app on the camera, preview
            // hidden). The virtual camera is published so apps can find and
            // open it, but the source stays closed: no webcam light, no phone
            // encoding or Wi‑Fi traffic, no decoding. Opening it wakes us.
            if (!SourceWanted(pipeline))
            {
                if (!pipeline.Live() || !begun)
                {
                    // In this source's shape (a phone at its last orientation): nothing to
                    // rebuild when it starts.
                    const HRESULT hr = pipeline.Begin(format.width, format.height, format.fpsNum, format.fpsDen, format.subtype);
                    if (FAILED(hr)) { Fail(pipeline.Failure(), hr); break; }
                    begun = true;
                }
                {
                    std::lock_guard lk(m_lock);
                    if (m_status.state == State::Waiting) m_status.state = State::Running; // nothing to wait for now
                    m_status.error.clear();
                }
                SetStandby(true);
                for (int i = 0; i < 2 && !Interrupted() && !SourceWanted(pipeline); ++i) Sleep(100);
                pipeline.Tick(); // a new shape or size setting applies in standby too
                if (pipeline.Broken()) { Fail(pipeline.Failure(), E_FAIL); break; }
                continue;
            }
            SetStandby(false);

            std::wstring why;
            HRESULT hr = S_OK;
            Outcome outcome;
            const uint32_t begins = pipeline.Begins();
            if (cam.IsPhone())
            {
                outcome = RunPhone(pipeline, cam, format, why, hr);
            }
            else if (cam.IsPcWindow())
            {
                outcome = RunWindow(pipeline, cam, format, why, hr);
            }
            else if (lost && !FindCameraAgain(cam, webcam))
            {
                why = L"Camera đã bị rút ra. Cắm lại cáp để tiếp tục";
                outcome = Outcome::Lost;
            }
            else
            {
                outcome = RunWebcam(pipeline, webcam, format, why, hr);
            }
            // The pipeline knows this source's size only if the run got to Begin (otherwise
            // standby would mix the previous source's size with this one's rotation).
            if (pipeline.Begins() != begins) begun = true;
            if (outcome == Outcome::Switch) continue;
            lost = outcome == Outcome::Lost;
            if (outcome == Outcome::Quit || m_quit) break;
            if (outcome == Outcome::Idle) continue; // back to standby
            if (pipeline.Broken())
            {
                Fail(pipeline.Failure(), hr);
                break;
            }
            // The camera lives as long as the app, so a source that fails before it was ever
            // published publishes it anyway (apps see "no signal") and is waited for like a
            // source lost later. Only the camera itself failing ends the session.
            if (!pipeline.Live())
            {
                const HRESULT bhr = pipeline.Begin(format.width, format.height, format.fpsNum, format.fpsDen, format.subtype);
                if (FAILED(bhr))
                {
                    Fail(pipeline.Failure(), bhr);
                    break;
                }
            }
            pipeline.ReleaseHold();
            Wait(why, hr);
            if (outcome == Outcome::Fatal)
            {
                // A source picked while the camera is up cannot run (lens refused, no HEVC
                // decoder…): keep the camera (apps keep it selected) until another one is picked.
                stuck = true;
                continue;
            }
            // Keep the virtual camera up (apps that use it keep it selected and
            // show its "no signal" picture) and bring the source back by itself.
            for (int i = 0; i < 20 && !Interrupted(); ++i) Sleep(100);
        }
        pipeline.End();
    }
    CoUninitialize();
    Notify();
}

CaptureEngine::Outcome CaptureEngine::RunWebcam(Pipeline& pipeline, const CameraDevice& cam, const NativeFormat& format,
                                                std::wstring& why, HRESULT& hr)
{
    Outcome outcome = Outcome::Quit;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    UINT32 width = 0, height = 0;
    ULONGLONG idleSince = 0;

#define STEP(expr, what) do { hr = (expr); if (FAILED(hr)) { why = what; outcome = Outcome::Lost; goto cleanup; } } while (0)

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
    pipeline.Hold();

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
    if (FAILED(hr)) { why = pipeline.Failure(); outcome = Outcome::Fatal; goto cleanup; }

    // 4. Capture loop.
    while (!Interrupted())
    {
        // Nobody needs the picture any more: close the webcam (its light goes off).
        if (!StillWanted(pipeline, idleSince)) { outcome = Outcome::Idle; goto cleanup; }
        DWORD streamIndex = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        STEP(reader->ReadSample(kVideoStream, 0, &streamIndex, &flags, &ts, &sample),
             L"Mất tín hiệu camera (bị rút ra, hoặc app khác đang dùng)");
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))
        {
            why = L"Camera đã ngắt kết nối";
            hr = MF_E_END_OF_STREAM;
            outcome = Outcome::Lost;
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
        if (pipeline.Broken()) { why = pipeline.Failure(); outcome = Outcome::Fatal; goto cleanup; }
    }

cleanup:
#undef STEP
    reader.Reset();
    if (source) source->Shutdown();
    cam.activate->ShutdownObject();
    return m_quit ? Outcome::Quit : m_switch ? Outcome::Switch : outcome;
}


CaptureEngine::Outcome CaptureEngine::RunPhone(Pipeline& pipeline, const CameraDevice& cam, const NativeFormat& format,
                                               std::wstring& why, HRESULT& hr)
{
    phone::PhoneLink& link = *cam.phone;
    const bool hevc = format.subtype == MFVideoFormat_HEVC;
    const uint32_t fps = format.fpsDen ? format.fpsNum / format.fpsDen : 30;
    // ~0.1 bit per pixel per frame: 1080p30 ≈ 6 Mbps, 4K30 ≈ 25 Mbps.
    const uint32_t kbps = std::clamp<uint32_t>(
        static_cast<uint32_t>(static_cast<uint64_t>(format.width) * format.height * fps / 10000), 2000, 60000);
    auto interrupted = [&] { return m_quit ? Outcome::Quit : Outcome::Switch; };

    // The connection is gone: the next attempt reconnects.
    auto lost = [&](const std::wstring& what) {
        link.Close();
        why = what;
        return Outcome::Lost;
    };

    // The link is ours while this source runs: the connector (which keeps idle links alive)
    // and screen windows leave it alone. A connector attempt in progress gives way at once.
    bool told = false;
    for (int waited = 0; !link.Lease(200); ++waited)
    {
        if (Interrupted()) return interrupted();
        if (waited < 15) pipeline.Hold();
        // Someone else keeps it (the phone's screen open in its own window, or pairing): say so.
        else if (!told && link.InUse() && pipeline.Live())
        {
            told = true;
            pipeline.ReleaseHold();
            Wait(L"Nguồn này đang được dùng ở chỗ khác (cửa sổ màn hình riêng, hoặc đang ghép đôi)", S_OK);
        }
    }
    phone::LeaseGuard lease(&link);

    // After a lost connection (phone app closed and reopened, Wi‑Fi dropped):
    // reconnect quietly. Only an already-paired phone gets back in.
    auto reconnect = [&]() -> bool {
        std::string error;
        pipeline.Hold();
        SetBlocking(cam.phone); // Switch()/Stop() cut it short
        const bool ok = !Interrupted() && link.Reconnect(error);
        SetBlocking(nullptr);
        if (!ok) why = error.empty() ? L"Mất kết nối với điện thoại" : Widen(error);
        return ok && !Interrupted(); // a switch while it connected: the caller returns
    };
    auto sendStart = [&] {
        return link.SendControl(json::Value::MakeObject()
            .Set("type", "start").Set("lens", format.phoneCamera)
            .Set("width", format.width).Set("height", format.height).Set("fps", fps)
            .Set("codec", hevc ? "hevc" : "h264").Set("bitrateKbps", kbps));
    };

    // An idle link kept by the connector may have died without anyone noticing (the phone
    // left the Wi‑Fi): if it says nothing at all after "start", one fresh connection.
    bool reused = link.Connected();
    if (!reused && !reconnect()) return Interrupted() ? interrupted() : Outcome::Lost;
    if (!sendStart())
    {
        if (!reused) return lost(L"Mất kết nối với điện thoại");
        reused = false;
        link.Close();
        if (!reconnect()) return Interrupted() ? interrupted() : Outcome::Lost;
        if (!sendStart()) return lost(L"Mất kết nối với điện thoại");
    }

    auto stateFrom = [&](const json::Value& v) {
        std::lock_guard lk(m_lock);
        if (!v["orientation"].IsNull())
        {
            const int rotation = ((v["orientation"].Int() % 360) + 360) % 360;
            if (rotation != m_sourceRotation) ++m_layoutGen; // the phone was turned: new shape
            m_sourceRotation = rotation;
            m_rotationMemory[cam.name + L"|" + Widen(format.phoneCamera)] = m_sourceRotation;
        }
        if (v["type"].Str() == "state") m_phoneState = v.Dump();
    };
    // A phone "error": some only need the user to bring the app back, the
    // rest will not get better by asking again.
    auto phoneError = [&](const json::Value& v) {
        why = Widen(v["message"].Str("Điện thoại báo lỗi"));
        const std::string code = v["code"].Str();
        return code == "permission" || code == "unsupported" || code == "encoder" ? Outcome::Fatal : Outcome::Lost;
    };

    // Wait for "started" (or an error) before decoding anything.
    json::Value started;
    ULONGLONG sentAt = GetTickCount64();
    ULONGLONG deadline = sentAt + 10000;
    bool heard = false;
    while (!Interrupted())
    {
        pipeline.Hold();
        phone::Message m;
        bool timedOut = false;
        if (!link.Receive(m, 500, timedOut))
        {
            const ULONGLONG now = GetTickCount64();
            if (reused && !heard && (!timedOut || now - sentAt >= 4000))
            {
                // Silent or closed: the kept link was dead. Start over once on a new one.
                reused = false;
                link.Close();
                if (!reconnect()) return Interrupted() ? interrupted() : Outcome::Lost;
                if (!sendStart()) return lost(L"Mất kết nối với điện thoại");
                sentAt = GetTickCount64();
                deadline = sentAt + 10000;
                continue;
            }
            if (timedOut && now < deadline) continue;
            return lost(timedOut ? L"Điện thoại không bắt đầu phát (app trên điện thoại đang bị ẩn?)"
                                 : L"Mất kết nối với điện thoại");
        }
        heard = true;
        if (m.kind != phone::Kind::Control) continue;
        json::Value v;
        try { v = m.Json(); } catch (const std::exception&) { continue; }
        if (v["type"].Str() == "error") return phoneError(v);
        if (v["type"].Str() == "started") { started = v; stateFrom(v); break; }
    }
    if (Interrupted())
    {
        // Switched away before the phone answered: its "started" (and "stopped") for this start
        // would still come and be taken for the next start's. A fresh connection next time.
        link.SendControl(json::Value::MakeObject().Set("type", "stop"));
        link.Close();
        return interrupted();
    }

    uint32_t width = static_cast<uint32_t>(started["width"].Int(format.width));
    uint32_t height = static_cast<uint32_t>(started["height"].Int(format.height));
    VideoDecoder decoder;
    hr = decoder.Init(started["codec"].Str() == "h264" ? false : hevc, width, height);
    if (FAILED(hr))
    {
        why = hevc ? L"Không có bộ giải mã HEVC (cài \"HEVC Video Extensions\" từ Microsoft Store)" : L"Không tạo được bộ giải mã video";
        link.SendControl(json::Value::MakeObject().Set("type", "stop"));
        return Outcome::Fatal;
    }
    hr = pipeline.Begin(width, height, fps, 1, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    if (FAILED(hr))
    {
        why = pipeline.Failure();
        link.SendControl(json::Value::MakeObject().Set("type", "stop"));
        return Outcome::Fatal;
    }
    hr = S_OK;

    crypto::Bytes config;     // latest SPS/PPS(/VPS), prepended to keyframes
    bool waitKeyframe = true; // decoding starts at a keyframe
    uint32_t lostFrames = 0;
    double decodeMs = 0;
    unsigned decoded = 0;
    ULONGLONG lastFrame = GetTickCount64();
    ULONGLONG lastStats = lastFrame;
    std::vector<uint8_t> unit;
    Outcome outcome = Outcome::Quit;
    ULONGLONG idleSince = 0;

    while (!Interrupted())
    {
        // Nobody needs the picture any more: the phone stops encoding and sending.
        if (!StillWanted(pipeline, idleSince)) { outcome = Outcome::Idle; break; }
        phone::Message m;
        bool timedOut = false;
        const bool got = link.Receive(m, 500, timedOut);
        const ULONGLONG now = GetTickCount64();
        if (!got && !timedOut)
        {
            outcome = lost(L"Mất kết nối với điện thoại");
            break;
        }
        if (now - lastFrame > 6000)
        {
            // The app was hidden or the phone locked: the connection may be
            // half-dead too, so start over with a fresh one.
            link.SendControl(json::Value::MakeObject().Set("type", "stop"));
            outcome = lost(L"Điện thoại ngừng gửi hình (app trên điện thoại bị ẩn hoặc màn hình tắt?)");
            break;
        }
        // Until the first frame of a new source decodes, the camera keeps the previous picture.
        if (waitKeyframe) pipeline.Hold();

        if (got && m.kind == phone::Kind::Control)
        {
            json::Value v;
            try { v = m.Json(); } catch (const std::exception&) { continue; }
            const std::string type = v["type"].Str();
            if (type == "error") { outcome = phoneError(v); break; }
            if (type == "stopped") { why = L"Điện thoại đã dừng phát"; outcome = Outcome::Lost; break; }
            if (type == "caps") link.SetCaps(v.Dump());
            // A turn of the phone changes the picture's shape: the pipeline applies the size
            // rule on the next frame (fit while an app uses the camera, else a new size).
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
            const double t0 = QpcMs();
            const HRESULT dhr = decoder.Decode(data, size, m.ptsUs * 10, [&](const uint8_t* nv12, long pitch, uint32_t w, uint32_t h) {
                if (w != width || h != height)
                {
                    // The phone changed the stream's size: the pipeline must not read past the
                    // frame. Same camera rule as any new size (fit during a call).
                    width = w;
                    height = h;
                    if (FAILED(pipeline.Begin(width, height, fps, 1, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264))) return;
                }
                pipeline.Frame(nv12, pitch, m.ptsUs * 10);
            });
            decodeMs += QpcMs() - t0;
            ++decoded;
            if (FAILED(dhr))
            {
                // Corrupt or missing data: ask for a fresh keyframe.
                ++lostFrames;
                waitKeyframe = true;
                link.SendControl(json::Value::MakeObject().Set("type", "keyframe"));
            }
            else
            {
                waitKeyframe = false;
            }
            if (pipeline.Broken()) { why = pipeline.Failure(); outcome = Outcome::Fatal; break; }
        }

        if (now - lastStats >= 1000)
        {
            link.SendControl(json::Value::MakeObject()
                .Set("type", "stats").Set("recvFps", pipeline.LastFps())
                .Set("decodeMs", decoded ? decodeMs / decoded : 0.0)
                .Set("queueMs", 0).Set("lost", static_cast<int>(lostFrames)));
            lostFrames = 0;
            decodeMs = 0;
            decoded = 0;
            lastStats = now;
        }
    }

    if (link.Connected()) link.SendControl(json::Value::MakeObject().Set("type", "stop"));
    return m_quit ? Outcome::Quit : m_switch ? Outcome::Switch : outcome;
}

// A window on this PC. Its size changes while it is being resized: frames of another size are
// fitted (with bars) into the size the source started with, and once the window has kept a new
// size for a second the pipeline takes that size (the camera then follows the size rule).
CaptureEngine::Outcome CaptureEngine::RunWindow(Pipeline& pipeline, const CameraDevice& cam, const NativeFormat& format,
                                                std::wstring& why, HRESULT& hr)
{
    if (!IsWindow(cam.window))
    {
        why = L"Cửa sổ đã đóng. Hãy chọn nguồn khác";
        return Outcome::Fatal;
    }
    static_cast<void>(format); // the size comes from the frames
    WindowCapture capture;
    if (!capture.Open(cam.window, why)) return Outcome::Fatal;

    constexpr int64_t kInterval100ns = 333333; // the camera is published at 30 fps
    uint32_t srcW = 0, srcH = 0;               // what the pipeline gets
    uint32_t pendingW = 0, pendingH = 0;
    ULONGLONG pendingSince = 0;
    std::vector<uint8_t> content, frame;
    ULONGLONG idleSince = 0;
    ULONGLONG lastFrame = GetTickCount64();
    ULONGLONG lastTaken = 0;
    ULONGLONG lastRepeat = 0;
    bool haveLast = false; // `frame` holds the window's newest picture
    int64_t ts = 0;
    Outcome outcome = Outcome::Quit;
    hr = S_OK;

    auto onFrame = [&](const uint8_t* bgra, uint32_t pitch, uint32_t w, uint32_t h) {
        const uint32_t cw = w & ~1u, ch = h & ~1u;
        if (cw < 2 || ch < 2) return;
        const ULONGLONG now = GetTickCount64();
        if (!srcW || ((cw != srcW || ch != srcH) && pendingW == cw && pendingH == ch && now - pendingSince >= 1000))
        {
            srcW = cw;
            srcH = ch;
            hr = pipeline.Begin(srcW, srcH, 30, 1, MFVideoFormat_ARGB32);
            if (FAILED(hr)) return;
        }
        if (cw != srcW || ch != srcH)
        {
            if (pendingW != cw || pendingH != ch)
            {
                pendingW = cw;
                pendingH = ch;
                pendingSince = now;
            }
        }
        frame.resize(CamStudioNv12Bytes(srcW, srcH));
        uint8_t* y = frame.data();
        uint8_t* uv = frame.data() + static_cast<size_t>(srcW) * srcH;
        if (cw == srcW && ch == srcH)
        {
            BgraToNv12(bgra, pitch, cw, ch, y, uv, srcW);
        }
        else
        {
            content.resize(CamStudioNv12Bytes(cw, ch));
            BgraToNv12(bgra, pitch, cw, ch, content.data(), content.data() + static_cast<size_t>(cw) * ch, cw);
            const FrameLayout l = ComputeLayout(cw, ch, ImageSettings{}, srcW, srcH);
            Nv12FillBars(frame.data(), srcW, srcH, l.destX, l.destY, l.destWidth, l.destHeight);
            Nv12CropScaleBilinearTo(content.data(), static_cast<long>(cw), cw, ch,
                Nv12Rect{ 0, 0, static_cast<double>(cw), static_cast<double>(ch) },
                y + static_cast<size_t>(l.destY) * srcW + l.destX,
                uv + static_cast<size_t>(l.destY / 2) * srcW + l.destX,
                static_cast<long>(srcW), l.destWidth, l.destHeight);
        }
        haveLast = true;
        pipeline.Frame(frame.data(), static_cast<long>(srcW), ts);
    };

    while (!Interrupted())
    {
        // Nobody needs the picture any more: stop capturing (back to standby).
        if (!StillWanted(pipeline, idleSince)) { outcome = Outcome::Idle; break; }
        // The capture item's Closed event does not always come (seen on Windows 11 with a
        // WinForms window): ask Windows directly.
        if (!IsWindow(cam.window))
        {
            why = L"Cửa sổ đã đóng. Hãy chọn nguồn khác";
            outcome = Outcome::Fatal;
            break;
        }
        ULONGLONG now = GetTickCount64();
        // A window sends frames only when it changes, up to the screen's rate: take at most 30 a
        // second (waiting rather than dropping, so the newest frame of a burst is the one kept),
        // and read one back from the GPU only when it goes somewhere.
        if (now - lastTaken < 30)
        {
            Sleep(static_cast<DWORD>(30 - (now - lastTaken)));
            now = GetTickCount64();
        }
        const bool read = !srcW || pipeline.WillPublish();
        const WindowCapture::Result result = capture.Next(100, read, onFrame);
        if (result == WindowCapture::Result::Closed)
        {
            why = L"Cửa sổ đã đóng. Hãy chọn nguồn khác";
            outcome = Outcome::Fatal;
            break;
        }
        if (result == WindowCapture::Result::Error)
        {
            // Capture failed for now (e.g. the graphics device was reset): open it again.
            why = L"Không quay được cửa sổ, đang thử lại";
            outcome = Outcome::Lost;
            break;
        }
        if (FAILED(hr))
        {
            why = pipeline.Failure();
            outcome = Outcome::Fatal;
            break;
        }
        if (result == WindowCapture::Result::Frame)
        {
            lastFrame = now;
            lastTaken = now;
            ts += kInterval100ns;
            if (!read && srcW)
            {
                haveLast = false;
                pipeline.Count(ts); // counted, not converted: nobody watches
                pipeline.Tick();
            }
        }
        else if (srcW && now - lastFrame >= 500)
        {
            // A still window: the camera keeps showing its last picture. Written again once a
            // second when we have it: a camera rebuilt meanwhile (an app left) has none to hold.
            if (haveLast && now - lastRepeat >= 1000)
            {
                lastRepeat = now;
                ts += kInterval100ns;
                pipeline.Frame(frame.data(), static_cast<long>(srcW), ts);
            }
            else
            {
                pipeline.Hold();
                pipeline.Tick();
            }
        }
        if (pipeline.Broken()) { why = pipeline.Failure(); outcome = Outcome::Fatal; break; }
    }
    capture.Close();
    return m_quit ? Outcome::Quit : m_switch ? Outcome::Switch : outcome;
}
