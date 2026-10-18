// Shared contract between the host app (writer) and the virtual camera media
// source (reader, loaded by the Windows Frame Server service in session 0).
#pragma once

#include <windows.h>
#include <guiddef.h>
#include <cmath>
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
// v2: triple-buffered (v1 had one buffer, and readers that met a write in
// progress showed "no signal" or a torn frame: flicker in Discord at 100 fps).
#define CAMSTUDIO_MAPPING_NAME   L"Global\\CamStudioVCam_Frames_v2"
#define CAMSTUDIO_EVENT_NAME     L"Global\\CamStudioVCam_NewFrame_v2"

// The media source (LocalService, which holds SeCreateGlobalPrivilege)
// creates the objects with this DACL so the unelevated host can open them:
// SYSTEM, LocalService and Administrators get full access, while interactive
// and authenticated users get read/write.
#define CAMSTUDIO_OBJECT_SDDL L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;IU)(A;;GRGW;;;AU)"

// Windows 10 has no virtual camera API: there the reader is a DirectShow source filter
// (src/dshow) inside the app that uses the camera, in the user's own session. The objects live
// in the session-local namespace (creating "Global\" ones needs SeCreateGlobalPrivilege, which
// neither the app nor the host has). One mapping, sized once for the largest picture: the header's
// width/height say what the host writes now (changed only while no reader streams), and
// frameBytes stays the slot stride, so a reader that raced the change sees a torn frame at worst.
#define CAMSTUDIO_LOCAL_MAPPING_NAME L"Local\\CamStudioVCam_Frames_v2"
#define CAMSTUDIO_LOCAL_EVENT_NAME   L"Local\\CamStudioVCam_NewFrame_v2"
constexpr uint32_t CAMSTUDIO_LOCAL_MAX_PIXELS = 4096u * 2304u; // 4K either way up fits

constexpr uint32_t CAMSTUDIO_MAGIC   = 0x31534D43; // "CMS1"
constexpr uint32_t CAMSTUDIO_VERSION = 2;

// Frames older than this are treated as "no signal".
constexpr int64_t CAMSTUDIO_STALE_100NS = 2'000'0000; // 2 s

// Frame slots. The writer fills a slot that is neither the latest one nor
// being read, then publishes it; a reader copies the latest slot. With three
// slots neither side waits for the other.
constexpr uint32_t CAMSTUDIO_SLOTS = 3;

#pragma pack(push, 8)
struct SharedFrameHeader
{
    uint32_t magic;          // CAMSTUDIO_MAGIC once the reader has initialised
    uint32_t version;        // CAMSTUDIO_VERSION
    uint32_t width;          // NV12 frame width the mapping was sized for
    uint32_t height;         // NV12 frame height
    uint32_t stride;         // bytes per luma row (== width)
    uint32_t slots;          // CAMSTUDIO_SLOTS
    uint64_t frameBytes;     // one slot: stride * height * 3 / 2
    // Latest complete frame: (frame number << 2) | slot. 0 = nothing written yet.
    volatile LONG64 latest;
    volatile LONG64 writeTime100ns; // MFGetSystemTime() of the last frame
    volatile LONG   readerActive;   // >0 while a stream is running
    uint32_t reserved1;
    // Per-slot seqlock (odd while the writer fills it): a reader that raced
    // the writer notices and retries with the newer latest slot.
    volatile LONG64 slotSeq[CAMSTUDIO_SLOTS];
    // Readers copying each slot right now: the writer avoids those slots.
    volatile LONG   slotReaders[CAMSTUDIO_SLOTS];
    uint32_t reserved2;
    uint8_t  pad[32];
    // CAMSTUDIO_SLOTS NV12 frames follow at offset sizeof(SharedFrameHeader).
};
#pragma pack(pop)

inline uint64_t CamStudioNv12Bytes(uint32_t w, uint32_t h)
{
    return static_cast<uint64_t>(w) * h * 3 / 2;
}

inline uint64_t CamStudioMappingBytes(uint32_t w, uint32_t h)
{
    return sizeof(SharedFrameHeader) + CAMSTUDIO_SLOTS * CamStudioNv12Bytes(w, h);
}

inline uint8_t* CamStudioSlot(SharedFrameHeader* h, uint32_t slot)
{
    return reinterpret_cast<uint8_t*>(h + 1) + slot * h->frameBytes;
}

inline uint64_t CamStudioLocalSlotBytes()
{
    return static_cast<uint64_t>(CAMSTUDIO_LOCAL_MAX_PIXELS) * 3 / 2;
}

inline uint64_t CamStudioLocalMappingBytes()
{
    return sizeof(SharedFrameHeader) + CAMSTUDIO_SLOTS * CamStudioLocalSlotBytes();
}

// First opener of a mapping: everything but the magic, then the magic (readers and the host
// wait for it). frameBytes is the slot stride: the frame itself, or the local channel's capacity.
inline void CamStudioInitHeader(SharedFrameHeader* h, uint32_t width, uint32_t height, uint64_t frameBytes)
{
    h->version = CAMSTUDIO_VERSION;
    h->width = width;
    h->height = height;
    h->stride = width;
    h->slots = CAMSTUDIO_SLOTS;
    h->frameBytes = frameBytes;
    h->latest = 0;
    h->writeTime100ns = 0;
    h->readerActive = 0;
    for (uint32_t s = 0; s < CAMSTUDIO_SLOTS; ++s)
    {
        h->slotSeq[s] = 0;
        h->slotReaders[s] = 0;
    }
    MemoryBarrier();
    h->magic = CAMSTUDIO_MAGIC;
}

// Local channel: a picture larger than the slots are sized for is written smaller, same shape.
inline void CamStudioClampLocalSize(uint32_t& w, uint32_t& h)
{
    const uint64_t pixels = static_cast<uint64_t>(w) * h;
    if (pixels <= CAMSTUDIO_LOCAL_MAX_PIXELS || !pixels) return;
    const double k = std::sqrt(static_cast<double>(CAMSTUDIO_LOCAL_MAX_PIXELS) / static_cast<double>(pixels));
    w = static_cast<uint32_t>(w * k) & ~1u;
    h = static_cast<uint32_t>(h * k) & ~1u;
}
