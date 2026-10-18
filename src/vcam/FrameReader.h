#pragma once

#include "Common.h"

// Reader side of the shared-memory frame channel. Lives in Frame Server.
class FrameReader
{
public:
    FrameReader() = default;
    ~FrameReader();
    FrameReader(const FrameReader&) = delete;
    FrameReader& operator=(const FrameReader&) = delete;

    // Creates (or opens) the global mapping sized for the native format. `local`: the Windows 10
    // channel instead (CAMSTUDIO_LOCAL_MAPPING_NAME, sized once for any picture; the size given
    // is only what a fresh mapping starts with).
    HRESULT Open(uint32_t nativeWidth, uint32_t nativeHeight, bool local = false);
    // Opens the mapping only if it exists (the host runs): nothing is created.
    bool OpenExisting(bool local);

    // The picture the host writes now (local channel: it changes between streams).
    uint32_t Width() const { return m_header ? m_header->width : 0; }
    uint32_t Height() const { return m_header ? m_header->height : 0; }
    void Close();

    HANDLE FrameEvent() const { return m_event; }

    // Number of the newest frame the host published (0 = none yet), and of
    // the last one Fill() delivered: equal means nothing new to send.
    LONG64 LatestFrame() const { return m_header ? (m_header->latest >> 2) : 0; }
    LONG64 DeliveredFrame() const { return m_delivered; }

    // Copies the latest host frame into an NV12 destination of any size,
    // scaled to fit (black bars when the shape differs). Draws a "no signal"
    // pattern if the host is not writing. Returns true when a live frame was delivered.
    bool Fill(uint8_t* dst, LONG pitch, uint32_t width, uint32_t height, LONGLONG now100ns);

    void SetActive(bool active);

private:
    void DrawNoSignal(uint8_t* dst, LONG pitch, uint32_t width, uint32_t height, LONGLONG now100ns);

    HANDLE m_mapping = nullptr;
    HANDLE m_event = nullptr;
    SharedFrameHeader* m_header = nullptr;
    bool m_active = false;
    LONG64 m_delivered = 0;
};
