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

    // Creates (or opens) the global mapping sized for the native format.
    HRESULT Open(uint32_t nativeWidth, uint32_t nativeHeight);
    void Close();

    HANDLE FrameEvent() const { return m_event; }

    // Copies the latest host frame into an NV12 destination of any size,
    // scaling if needed. Draws a "no signal" pattern if the host is not
    // writing. Returns true when a live frame was delivered.
    bool Fill(uint8_t* dst, LONG pitch, uint32_t width, uint32_t height, LONGLONG now100ns);

    void SetActive(bool active);

private:
    void DrawNoSignal(uint8_t* dst, LONG pitch, uint32_t width, uint32_t height, LONGLONG now100ns);

    HANDLE m_mapping = nullptr;
    HANDLE m_event = nullptr;
    SharedFrameHeader* m_header = nullptr;
    bool m_active = false;
};
