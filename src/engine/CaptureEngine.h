// Runs the virtual camera on a worker thread:
//   source (webcam, phone, or a window on this PC) -> NV12 -> orient -> crop/zoom -> fit -> adjust
//     -> shared memory -> virtual camera
// and keeps a small BGRA copy of the latest frame for the preview. Switch() moves to another
// source while the virtual camera stays registered, so apps using it never lose it.
#pragma once

#include "../common/MfDevices.h"
#include "ImageSettings.h"

#include <algorithm>
#include <atomic>
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
        std::wstring error;
    };

    ~CaptureEngine() { Stop(); }

    void SetNotify(HWND window, UINT msg) { m_notifyWindow = window; m_notifyMsg = msg; }

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
    bool PreviewDue();
    bool PreviewWanted() const; // PreviewDue without taking the slot
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
    std::atomic<uint32_t> m_previewMaxW{ 1280 };
    std::atomic<uint32_t> m_previewMaxH{ 720 };

    mutable std::mutex m_lock;
    Status m_status;
    ImageSettings m_image;
    int m_sourceRotation = 0;
    std::string m_phoneState;
    std::vector<uint32_t> m_preview;
    uint32_t m_previewW = 0;
    uint32_t m_previewH = 0;
    uint64_t m_previewId = 0;
    ULONGLONG m_lastPreviewTick = 0;

    // The source running now, and the one Switch() asked for (under m_lock).
    CameraDevice m_camera;
    NativeFormat m_format;
    CameraDevice m_nextCamera;
    NativeFormat m_nextFormat;
    uint32_t m_sourceSeq = 0;  // last id handed out by Start()/Switch()
    uint32_t m_nextSourceId = 0;
    std::shared_ptr<phone::PhoneLink> m_blocking;

    // Last orientation each phone source reported (camera name + lens), so the
    // virtual camera published in standby already has the right shape and is
    // not rebuilt (dropping the app that just opened it) when the phone starts.
    std::map<std::wstring, int> m_rotationMemory;

    HWND m_notifyWindow = nullptr;
    UINT m_notifyMsg = 0;
};
