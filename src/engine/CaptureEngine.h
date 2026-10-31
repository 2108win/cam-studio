// Runs the virtual camera on a worker thread:
//   source (webcam, phone, or a window on this PC) -> NV12 -> orient -> crop/zoom (or automatic
//     framing) -> fit -> adjust -> shared memory -> virtual camera
// and keeps a small BGRA copy of the latest frame for the preview. Switch() moves to another
// source while the virtual camera stays registered, so apps using it never lose it.
#pragma once

#include "../common/MfDevices.h"
#include "ImageSettings.h"
#include "AudioOut.h"
#include "Recorder.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace phone { class PhoneLink; }

// One entry of the resolution picker: a distinct size + frame rate, backed
// by the camera's cheapest-to-convert native subtype for that combination.
struct FormatOption
{
    NativeFormat native;
    bool largest = false;      // the camera's (or this lens') native, biggest mode
    std::wstring lensLabel;    // phones: e.g. "Camera sau · góc rộng"
};

std::vector<FormatOption> BuildFormatOptions(const std::vector<NativeFormat>& formats);
HRESULT ReadCameraFormats(const CameraDevice& cam, std::vector<FormatOption>& options);

class CaptureEngine
{
public:
    // Waiting: the source dropped (phone app hidden, cable pulled, camera
    // taken by another app) but the virtual camera stays up and the engine
    // keeps retrying; `error` says what it is waiting for.
    enum class State { Stopped, Starting, Running, Failed, Waiting };

    struct Status
    {
        State state = State::Stopped;
        uint32_t width = 0;          // virtual camera output
        uint32_t height = 0;
        uint32_t cameraWidth = 0;    // source frame
        uint32_t cameraHeight = 0;
        GUID cameraSubtype{};        // MJPG/NV12 for webcams, H264/HEVC for phones
        double fps = 0.0;
        double processMs = 0.0;      // average per-frame work in our pipeline
        uint64_t framesCaptured = 0;
        uint64_t framesDropped = 0;
        bool sharedMemory = false;   // Frame Server has loaded our media source
        bool consumerActive = false; // some app is streaming from the camera
        int sourceRotation = 0;      // phones: rotation the phone asked for
        bool standby = false;        // virtual camera up, source closed until something needs it
        uint32_t pictureWidth = 0;   // the picture inside the output: smaller than width x height
        uint32_t pictureHeight = 0;  //   when it is fitted with black bars
        uint32_t sourceId = 0;       // which Start()/Switch() the frame counts belong to
        bool noSource = false;       // the camera is up with no source: apps see "no signal"
        double lagMs = 0.0;          // phones: how late the newest frame arrived in the last second
        uint64_t framesSkipped = 0;  //   and frames not shown because a newer one had arrived
        std::wstring error;
    };

    ~CaptureEngine() { Stop(); }

    // A window that gets msg when a preview frame is ready (or the state changed). One message
    // waits at a time: the next is posted once CopyPreview has been called. Set once, before any
    // source runs.
    void SetNotify(HWND window, UINT msg)
    {
        m_notifyMsg = msg;
        m_notifyWindow = window;
    }

    // Both return the id the new source's status will carry (Status::sourceId).
    uint32_t Start(const CameraDevice& cam, const NativeFormat& format, const ImageSettings& image);
    // Moves to another source, or another mode of the same one, keeping the virtual camera: an
    // app using it sees the last picture frozen until the new source's first frame, never "no
    // signal". Starts when nothing runs. An empty CameraDevice is "no source": the camera stays
    // registered (at format's size if it must be published now) and shows "no signal".
    uint32_t Switch(const CameraDevice& cam, const NativeFormat& format);
    void Stop();
    Status GetStatus() const;

    // Applies on the next frame. Returns true when the picture changes shape (rotation 0<->90,
    // crop aspect): the virtual camera then follows the size rule (SetOutputSize).
    bool SetImage(const ImageSettings& image);

    // "Kích thước camera ảo". Auto: the source's own size while no app uses the camera; an app
    // using it keeps its size and other pictures are fitted inside with black bars, until it
    // lets go. Fixed sizes fit every source.
    void SetOutputSize(OutputSize size)
    {
        m_outputSize = static_cast<int>(size);
        ++m_layoutGen;
    }

    // "Chụp ảnh": the next frame the camera shows (or what it shows now with no source) as a PNG.
    // Blocks up to 4 s: a closed source (standby) opens first.
    HRESULT Snapshot(const std::wstring& path);
    // "Ghi hình": what the camera shows, from now until StopRecording, to an MP4.
    HRESULT StartRecording(const std::wstring& path, bool microphone);
    HRESULT StopRecording() { return m_recorder.Stop(); }
    const Recorder& Recording() const { return m_recorder; }

    // "Nền": the person stays sharp; the background is blurred (strength 0..100) or replaced by
    // an image. Cameras only (never a screen or a window), and only on frames that go somewhere.
    enum class BackgroundMode : int { Off = 0, Blur = 1, Image = 2 };
    void SetBackground(BackgroundMode mode, int strength, const std::wstring& image, const std::wstring& model);
    // "Micro điện thoại" (AudioOut.h): ask a phone camera for its microphone and play it into
    // target (an output device's endpoint id, or "wav:<path>"); empty: off. Applies at once.
    void SetPhoneAudio(const std::wstring& target);
    // An app uses a microphone now: a phone camera source stays open with nobody on the camera
    // (a call with the camera off still needs the phone's sound). See SourceWanted.
    void SetAudioWanted(bool wanted) { m_audioWanted = wanted; }
    struct PhoneAudioState
    {
        bool asked = false;     // the phone streams and was asked for its sound
        bool receiving = false; // sound arrived in the last second
        std::wstring problem;   // why not ("" when fine)
    };
    PhoneAudioState GetPhoneAudio() const;

    // "Làm mịn da" (SkinSmooth.h): 0 off, 1..100. Cameras only.
    void SetSmoothing(int strength) { m_smoothing = std::clamp(strength, 0, 100); }

    struct BackgroundState
    {
        int available = 1;    // 1 works or not tried, 0 Windows ML / the model missing, -1 gave up
        bool active = false;  // blending frames now
        double inferMs = 0;   // the model's last run
        int faces = -1;       // skin smoothing: faces in the last look (-1 off, -2 no face tracker)
    };
    BackgroundState GetBackground() const;

    // What apps see while the picture is paused (cs_set_pause_picture): the camera's own "no
    // signal", a card with text, the last picture blurred with the text, or an image file.
    // Applies whenever the camera has no source; the app sets NoSignal unless it paused.
    enum class PauseKind : int { NoSignal = 0, Card = 1, Blur = 2, Image = 3 };
    void SetPausePicture(PauseKind kind, const std::wstring& text, const std::wstring& path);

    // "Tự căn khung" (AutoFrame.h): webcams and phone cameras are zoomed and moved to keep the
    // faces in the shot. Screens and windows are never framed.
    void SetAutoFrame(bool on) { m_autoFrame = on; }

    struct Framing
    {
        bool active = false;       // framing moves the picture now (on, a camera, frames flowing)
        int available = 1;         // 1 face detection works, 0 Windows has none, -1 it kept failing
        bool cameraSource = false; // the source is a camera
        int faces = 0;             // in the last detection
        double zoom = 1.0;         // the view shown now
        double centerX = 0.5;
        double centerY = 0.5;
        double maxZoom = 1.0;      // how far framing may zoom into this picture
    };
    Framing GetFraming() const;

    void SetPreviewEnabled(bool enabled) { m_previewEnabled = enabled; }

    // Largest preview size worth producing (the panel's size in pixels).
    void SetPreviewSize(uint32_t maxWidth, uint32_t maxHeight)
    {
        m_previewMaxW = std::clamp<uint32_t>(maxWidth, 64, 3840);
        m_previewMaxH = std::clamp<uint32_t>(maxHeight, 64, 3840);
    }

    // Latest preview frame (BGRA, top-down). frameId increases per update.
    bool CopyPreview(uint32_t* dst, size_t capacity, uint32_t& width, uint32_t& height, uint64_t& frameId) const;

    // Phones: latest "state" message (JSON) while streaming.
    std::string PhoneState() const;

private:
    class Pipeline;
    friend class Pipeline;

    // How one attempt at running the source ended.
    enum class Outcome
    {
        Quit,   // Stop() was called
        Lost,   // the source went away: retry while the virtual camera stays up
        Fatal,  // retrying cannot help (missing decoder, lens refused…)
        Idle,   // nobody needed a picture for a while: source closed, back to standby
        Switch, // Switch() was called: run the next source on the same virtual camera
    };

    // A source loop should return now.
    bool Interrupted() const { return m_quit || m_switch; }

    // A picture is needed: an app streams from the virtual camera, or the
    // preview is on screen. Otherwise the source stays closed (standby).
    bool SourceWanted(Pipeline& pipeline);
    // In a source loop: false once nothing has wanted a picture for kIdleCloseMs.
    bool StillWanted(Pipeline& pipeline, ULONGLONG& idleSince);
    void SetStandby(bool standby);
    int ExpectedRotationLocked(const CameraDevice& cam, const NativeFormat& format) const;

    void Run(CameraDevice cam, NativeFormat format);
    Outcome RunWebcam(Pipeline& pipeline, const CameraDevice& cam, const NativeFormat& format, std::wstring& why, HRESULT& hr);
    Outcome RunPhone(Pipeline& pipeline, const CameraDevice& cam, const NativeFormat& format, std::wstring& why, HRESULT& hr);
    Outcome RunWindow(Pipeline& pipeline, const CameraDevice& cam, const NativeFormat& format, std::wstring& why, HRESULT& hr);
    void Fail(const std::wstring& what, HRESULT hr);
    void Wait(const std::wstring& what, HRESULT hr);
    void Notify() const;
    void UpdatePreview(const uint8_t* y, const uint8_t* uv, long pitch, uint32_t w, uint32_t h);
    // sourceFps: the source's nominal rate. The preview gets an even ~30 fps (every frame of a
    // source at 30 fps or less).
    bool PreviewDue(double sourceFps);
    bool PreviewWanted(double sourceFps) const; // PreviewDue without taking the slot
    void StartLocked();
    ImageSettings EffectiveImageLocked() const; // user settings + phone orientation
    // A phone link the source is blocked on while it reconnects: Switch()/Stop() abort it.
    void SetBlocking(const std::shared_ptr<phone::PhoneLink>& link);

    std::thread m_thread;
    std::atomic<bool> m_quit{ false };
    std::atomic<bool> m_switch{ false };   // m_next* is waiting to take over
    std::atomic<int> m_outputSize{ 0 };    // OutputSize
    std::atomic<uint32_t> m_layoutGen{ 0 }; // bumped when the picture's shape may change
    std::atomic<bool> m_previewEnabled{ true };
    std::atomic<bool> m_autoFrame{ false };
    std::atomic<uint32_t> m_previewMaxW{ 1280 };
    std::atomic<uint32_t> m_previewMaxH{ 720 };

    mutable std::mutex m_lock;
    Status m_status;
    Framing m_framing;
    ImageSettings m_image;
    int m_sourceRotation = 0;
    std::string m_phoneState;
    std::vector<uint32_t> m_preview;
    uint32_t m_previewW = 0;
    uint32_t m_previewH = 0;
    uint64_t m_previewId = 0;
    double m_previewDueMs = 0; // QPC time the next preview frame is due (capture thread only)

    // The source running now, and the one Switch() asked for (under m_lock).
    CameraDevice m_camera;
    NativeFormat m_format;
    CameraDevice m_nextCamera;
    NativeFormat m_nextFormat;
    uint32_t m_sourceSeq = 0;  // last id handed out by Start()/Switch()
    uint32_t m_nextSourceId = 0;
    std::shared_ptr<phone::PhoneLink> m_blocking;

    struct PausePicture
    {
        PauseKind kind = PauseKind::NoSignal;
        std::wstring text;
        std::wstring path;
        uint32_t gen = 0; // bumped on every change: the picture is rendered again
    };
    PausePicture m_pause; // under m_lock

    Recorder m_recorder;

    // Background effect settings (mode and strength read on every frame, the paths under m_lock).
    std::atomic<int> m_bgMode{ 0 };
    std::atomic<int> m_bgStrength{ 60 };
    std::atomic<int> m_smoothing{ 0 };

    // The phone's microphone (SetPhoneAudio); target and gen under m_lock.
    std::wstring m_audioTarget;
    std::atomic<uint32_t> m_audioGen{ 0 };
    std::atomic<bool> m_audioWanted{ false };
    std::atomic<bool> m_phoneCameraSource{ false }; // the source now is a phone's camera
    PhoneAudioState m_audioState; // under m_lock
    std::wstring m_bgImage, m_bgModel;
    uint32_t m_bgGen = 0;
    BackgroundState m_bgState; // under m_lock
    // A snapshot asked for: the next frame that goes to the camera is copied here.
    std::atomic<bool> m_snapWanted{ false };
    std::mutex m_snapLock;
    std::condition_variable m_snapReady;
    std::vector<uint8_t> m_snap;
    uint32_t m_snapW = 0, m_snapH = 0;
    void OfferSnapshot(const uint8_t* nv12, long pitch, uint32_t w, uint32_t h);

    // Last orientation each phone source reported (camera name + lens), so the
    // virtual camera published in standby already has the right shape and is
    // not rebuilt (dropping the app that just opened it) when the phone starts.
    std::map<std::wstring, int> m_rotationMemory;

    std::atomic<HWND> m_notifyWindow{ nullptr };
    std::atomic<UINT> m_notifyMsg{ 0 };
    mutable std::atomic<bool> m_notifyPending{ false }; // posted, CopyPreview not called since
};
