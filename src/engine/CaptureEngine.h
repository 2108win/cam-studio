// Runs one capture session on a worker thread:
//   source (webcam or phone) -> NV12 -> orient -> crop/zoom -> adjust
//     -> shared memory -> virtual camera
// and keeps a small BGRA copy of the latest frame for the preview.
#pragma once

#include "../common/MfDevices.h"
#include "ImageSettings.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    enum class State { Stopped, Starting, Running, Failed };

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
        std::wstring error;
    };

    ~CaptureEngine() { Stop(); }

    void SetNotify(HWND window, UINT msg) { m_notifyWindow = window; m_notifyMsg = msg; }

    void Start(const CameraDevice& cam, const NativeFormat& format, const ImageSettings& image);
    void Stop();
    Status GetStatus() const;

    // Applies on the next frame. If the new settings change the output size
    // (rotation 0<->90, crop aspect) the session restarts; returns true then.
    bool SetImage(const ImageSettings& image);

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

    void Run(CameraDevice cam, NativeFormat format);
    void RunWebcam(Pipeline& pipeline, const CameraDevice& cam, const NativeFormat& format);
    void RunPhone(Pipeline& pipeline, const CameraDevice& cam, const NativeFormat& format);
    void Fail(const std::wstring& what, HRESULT hr);
    void Notify() const;
    void UpdatePreview(const uint8_t* nv12, long pitch, uint32_t w, uint32_t h);
    bool PreviewDue();
    void StartLocked();
    ImageSettings EffectiveImageLocked() const; // user settings + phone orientation

    std::thread m_thread;
    std::atomic<bool> m_quit{ false };
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

    // Kept to restart the same session when the output size changes.
    CameraDevice m_camera;
    NativeFormat m_format;

    HWND m_notifyWindow = nullptr;
    UINT m_notifyMsg = 0;
};
