// Shared contract between the host app (writer) and the virtual camera media
// source (reader, loaded by the Windows Frame Server service in session 0).
#pragma once

#include <windows.h>
#include <guiddef.h>
#include <cstdint>

// COM class of the virtual camera media source.
// {72F5B2FD-C48E-449B-95CE-E8F13BF200CB}
#define CAMSTUDIO_VCAM_CLSID_STRING L"{72F5B2FD-C48E-449B-95CE-E8F13BF200CB}"
inline constexpr GUID CLSID_CamStudioVCam =
    { 0x72f5b2fd, 0xc48e, 0x449b, { 0x95, 0xce, 0xe8, 0xf1, 0x3b, 0xf2, 0x00, 0xcb } };

// Attributes the host sets on IMFVirtualCamera. Frame Server forwards them to
// the media source's IMFActivate, so the source knows the native format
// before it publishes its media types.
// UINT64 packed like MF_MT_FRAME_SIZE (width << 32 | height).
inline constexpr GUID CAMSTUDIO_ATTR_NATIVE_SIZE =
    { 0x7274f18f, 0x32c3, 0x4935, { 0xa7, 0xdf, 0xae, 0x15, 0xff, 0xc5, 0x8f, 0xdc } };
// UINT64 packed like MF_MT_FRAME_RATE (numerator << 32 | denominator).
inline constexpr GUID CAMSTUDIO_ATTR_NATIVE_FPS =
    { 0xa653111f, 0xe52f, 0x433f, { 0x82, 0x8f, 0x0a, 0xc1, 0x65, 0xd9, 0x65, 0xf2 } };

#define CAMSTUDIO_VCAM_FRIENDLY_NAME L"Cam Studio"

// Kernel object names. "Global\" is required: the reader runs in session 0
// and cannot see the host's session-local namespace.
#define CAMSTUDIO_MAPPING_NAME   L"Global\\CamStudioVCam_Frames_v1"
#define CAMSTUDIO_EVENT_NAME     L"Global\\CamStudioVCam_NewFrame_v1"

// The media source (LocalService, which holds SeCreateGlobalPrivilege)
// creates the objects with this DACL so the unelevated host can open them:
// SYSTEM, LocalService and Administrators get full access, while interactive
// and authenticated users get read/write.
#define CAMSTUDIO_OBJECT_SDDL L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;IU)(A;;GRGW;;;AU)"

constexpr uint32_t CAMSTUDIO_MAGIC   = 0x31534D43; // "CMS1"
constexpr uint32_t CAMSTUDIO_VERSION = 1;

// Frames older than this are treated as "no signal".
constexpr int64_t CAMSTUDIO_STALE_100NS = 2'000'0000; // 2 s

#pragma pack(push, 8)
struct SharedFrameHeader
{
    uint32_t magic;          // CAMSTUDIO_MAGIC once the reader has initialised
    uint32_t version;        // CAMSTUDIO_VERSION
    uint32_t width;          // NV12 frame width the mapping was sized for
    uint32_t height;         // NV12 frame height
    uint32_t stride;         // bytes per luma row (== width)
    uint32_t reserved0;
    uint64_t frameBytes;     // stride * height * 3 / 2
    // Seqlock: the writer sets it odd, copies the frame, then sets it even.
    // Readers retry if it was odd or changed during their copy.
    volatile LONG64 sequence;
    volatile LONG64 writeTime100ns; // MFGetSystemTime() of the last frame
    volatile LONG   readerActive;   // >0 while a stream is running
    uint32_t reserved1;
    uint8_t  pad[64];
    // NV12 payload follows at offset sizeof(SharedFrameHeader).
};
#pragma pack(pop)

inline uint64_t CamStudioNv12Bytes(uint32_t w, uint32_t h)
{
    return static_cast<uint64_t>(w) * h * 3 / 2;
}

inline uint64_t CamStudioMappingBytes(uint32_t w, uint32_t h)
{
    return sizeof(SharedFrameHeader) + CamStudioNv12Bytes(w, h);
}
