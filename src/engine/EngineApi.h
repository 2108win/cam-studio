// Flat C API of CamStudioEngine.dll, consumed by the WinUI app via P/Invoke.
// All structs use natural alignment (pack 8); strings are UTF-16 unless
// noted; JSON is UTF-8.
#pragma once

#include <cstdint>

#ifdef CAMSTUDIO_ENGINE_EXPORTS
#define CS_API extern "C" __declspec(dllexport)
#else
#define CS_API extern "C" __declspec(dllimport)
#endif

struct CsFormat
{
    uint32_t width;
    uint32_t height;
    uint32_t fpsNum;
    uint32_t fpsDen;
    uint32_t fourcc;   // native subtype, e.g. 'MJPG', 'NV12', 'H264', 'HEVC'
    int32_t isNative;  // the camera's (or this lens') largest mode
    int32_t lens;      // phones: 1 back, 2 front, 3 external; 0 for webcams
    wchar_t lensLabel[64];
};

struct CsImage
{
    int32_t rotation; // 0, 90, 180, 270 (clockwise)
    int32_t mirror;
    int32_t flip;
    int32_t aspect;   // CropAspect: 0 native, 1 16:9, 2 4:3, 3 1:1, 4 9:16
    double zoom;      // 1 .. 4
    double centerX;   // 0 .. 1
    double centerY;
    int32_t brightness;  // -100 .. 100
    int32_t contrast;
    int32_t saturation;
    int32_t temperature; // + warmer, - cooler
};

struct CsStatus
{
    int32_t state;            // 0 stopped, 1 starting, 2 running, 3 failed,
                              // 4 waiting (source lost, virtual camera kept, retrying; see error)
    uint32_t width;           // virtual camera output
    uint32_t height;
    uint32_t cameraWidth;     // source frame
    uint32_t cameraHeight;
    uint32_t cameraFourcc;
    double fps;
    double processMs;
    uint64_t framesCaptured;
    uint64_t framesDropped;
    int32_t sharedMemory;
    int32_t consumerActive;
    int32_t sourceRotation;   // phones: rotation applied from the phone's orientation
    int32_t standby;          // running, but the source is closed until an app opens the camera
                              // (or the preview is on screen): nothing captured, decoded or sent
    wchar_t error[256];
};

CS_API int32_t cs_init();
CS_API void cs_shutdown();

// Cameras: webcams, then phones (found on Wi‑Fi, added by IP, or on a USB
// cable). Our own virtual camera is excluded. Returns the count.
CS_API int32_t cs_refresh_cameras();
// Copies the name; returns its length, or -1 for a bad index.
CS_API int32_t cs_camera_name(int32_t camera, wchar_t* buffer, int32_t capacity);
// A camera's stable id, the same across refreshes (indices are not): "cam:<symbolic link>" for
// webcams, "phone:<ip>:<port>" or "phone:usb:<serial>:<port>" for phones. Returns its length.
CS_API int32_t cs_camera_id(int32_t camera, wchar_t* buffer, int32_t capacity);
// 0 webcam, 1 phone camera, 2 phone screen (shared screen: shown in its own window by default).
CS_API int32_t cs_camera_kind(int32_t camera);

// Phones are kept connected in the background (paired ones on Wi‑Fi, every phone on a cable), so
// picking one starts at once. Their state, for the camera menu:
enum CsLinkState
{
    CS_LINK_IDLE = 0,          // not connected
    CS_LINK_CONNECTED = 1,     // connected (webcams always report this)
    CS_LINK_CONNECTING = 2,
    CS_LINK_NEEDS_PAIRING = 3, // pick it to pair
    CS_LINK_BUSY = 4,          // another PC has the phone
    CS_LINK_UNREACHABLE = 5,   // the last try failed (error says why), tried again later
    CS_LINK_ELSEWHERE = 6,     // the same phone is connected another way (Wi‑Fi / cable)
};
// Returns a CsLinkState and copies the last error (may be empty).
CS_API int32_t cs_camera_state(int32_t camera, wchar_t* error, int32_t capacity);
// Changes whenever a phone's state changes or phones come and go: refresh the list then.
CS_API uint32_t cs_sources_generation();

// Reads a camera's formats (best first). For phones the connector usually has them already
// (instant); otherwise this connects and, the first time, pairs: poll cs_pairing_code
// meanwhile. Blocking; call it off the UI thread. Returns the count, or <0 (HRESULT) on error;
// the message is then in cs_last_error.
CS_API int32_t cs_load_formats(int32_t camera);
CS_API int32_t cs_get_format(int32_t index, CsFormat* format);
CS_API int32_t cs_last_error(wchar_t* buffer, int32_t capacity);

// While a phone is waiting for the user to confirm pairing: the 6-digit
// code to show. Returns its length (0 when no pairing is in progress).
CS_API int32_t cs_pairing_code(wchar_t* buffer, int32_t capacity);
// The PC user's answer to the pairing code (1 = codes match, accept).
// Pairing needs a yes on both the phone and the PC (protocol 3.2).
CS_API void cs_pairing_answer(int32_t accept);

// "Add by IP" for networks where discovery does not work.
CS_API void cs_add_phone(const wchar_t* host, int32_t port);

// Starts capture + virtual camera for a camera and an index from the last
// cs_load_formats() of that camera. Returns an HRESULT.
CS_API int32_t cs_start(int32_t camera, int32_t format, const CsImage* image);
CS_API void cs_stop();

// Live picture settings. Returns 1 if the session restarted (size change).
CS_API int32_t cs_set_image(const CsImage* image);

CS_API void cs_get_status(CsStatus* status);
CS_API void cs_set_preview_enabled(int32_t enabled);
// Largest preview worth producing: the preview panel's size in pixels.
CS_API void cs_set_preview_size(int32_t maxWidth, int32_t maxHeight);

// Copies the latest preview frame (BGRA, top-down). Always reports its
// size and id; copies only when capacity (in pixels) is enough. Returns 1
// when pixels were copied.
CS_API int32_t cs_copy_preview(uint32_t* pixels, int32_t capacity, uint32_t* width, uint32_t* height, uint64_t* frameId);

// Phones (UTF-8 JSON, docs/protocol.md): the capabilities from the last
// cs_load_formats, and the latest "state" while streaming. Return the
// length needed (call with capacity 0 to size the buffer).
CS_API int32_t cs_phone_caps(char* buffer, int32_t capacity);
CS_API int32_t cs_phone_state(char* buffer, int32_t capacity);
// Sends a control message ("set", "showSystemEffects"…) to the streaming phone.
CS_API int32_t cs_phone_send(const char* json);
