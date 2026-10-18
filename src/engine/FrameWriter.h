#pragma once

#include "../common/SharedFrame.h"
#include "../common/Nv12.h"

#include <mfapi.h>

// Writer side of the shared-memory channel. The mapping is created by the
// media source inside Frame Server, so it only exists once some app has
// opened the virtual camera. Until then Write() is a cheap no-op.
class FrameWriter
{
public:
    ~FrameWriter() { Close(); }

    bool IsOpen() const { return m_header != nullptr; }
    bool ConsumerActive() const { return m_header && m_header->readerActive > 0; }
    uint32_t Width() const { return m_header ? m_header->width : 0; }
    uint32_t Height() const { return m_header ? m_header->height : 0; }

    void TryOpen()
    {
        if (m_header) return;
        const ULONGLONG now = GetTickCount64();
        if (now - m_lastAttempt < 500) return;
        m_lastAttempt = now;

        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, CAMSTUDIO_MAPPING_NAME);
        if (!mapping) return;
        auto* header = static_cast<SharedFrameHeader*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
        if (!header || header->magic != CAMSTUDIO_MAGIC || header->version != CAMSTUDIO_VERSION ||
            header->slots != CAMSTUDIO_SLOTS)
        {
            if (header) UnmapViewOfFile(header);
            CloseHandle(mapping);
            return;
        }
        m_mapping = mapping;
        m_header = header;
        m_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, CAMSTUDIO_EVENT_NAME);
    }

    void Close()
    {
        if (m_header) { UnmapViewOfFile(m_header); m_header = nullptr; }
        if (m_mapping) { CloseHandle(m_mapping); m_mapping = nullptr; }
        if (m_event) { CloseHandle(m_event); m_event = nullptr; }
    }

    // Publishes one NV12 frame. Scales only if the mapping was sized for a
    // different format (e.g. left over from an earlier session).
    void Write(const uint8_t* src, long srcPitch, uint32_t w, uint32_t h)
    {
        if (!m_header) return;

        // A slot that is neither the one readers are told to use nor being read.
        const LONG64 latest = m_header->latest;
        const uint32_t latestSlot = latest ? static_cast<uint32_t>(latest & 3) : CAMSTUDIO_SLOTS;
        uint32_t slot = CAMSTUDIO_SLOTS;
        for (uint32_t s = 0; s < CAMSTUDIO_SLOTS && slot == CAMSTUDIO_SLOTS; ++s)
            if (s != latestSlot && m_header->slotReaders[s] == 0) slot = s;
        for (uint32_t s = 0; s < CAMSTUDIO_SLOTS && slot == CAMSTUDIO_SLOTS; ++s)
            if (s != latestSlot) slot = s; // every other slot busy: the reader's seqlock catches it

        InterlockedIncrement64(&m_header->slotSeq[slot]); // odd: writing
        Nv12Scale(src, srcPitch, w, h, CamStudioSlot(m_header, slot), static_cast<long>(m_header->stride),
                  m_header->width, m_header->height);
        InterlockedIncrement64(&m_header->slotSeq[slot]); // even: done
        InterlockedExchange64(&m_header->writeTime100ns, MFGetSystemTime());
        InterlockedExchange64(&m_header->latest, (((latest >> 2) + 1) << 2) | slot);
        m_released = false;
        if (m_event) SetEvent(m_event);
    }

    // Keeps the latest frame good for readers for `ms` more, so an app keeps showing it (frozen)
    // instead of "no signal" while the next source opens. Nothing is copied: the picture is
    // already in its slot, only its time moves (ahead of now: readers check the age with a
    // signed difference). The next Write() or ReleaseHold() sets the real time again.
    void HoldFor(uint32_t ms)
    {
        // After a loss the old picture stays gone: retries must not bring it back frozen.
        if (!m_header || !m_header->latest || m_released) return;
        const LONG64 until = MFGetSystemTime() + static_cast<LONG64>(ms) * 10000 - CAMSTUDIO_STALE_100NS;
        if (until > m_header->writeTime100ns) InterlockedExchange64(&m_header->writeTime100ns, until);
    }

    // The source is gone: the held frame ages from now, and readers show "no signal" in 2 s.
    void ReleaseHold()
    {
        m_released = true; // until the next frame is written
        if (!m_header) return;
        const LONG64 now = MFGetSystemTime();
        if (m_header->writeTime100ns > now) InterlockedExchange64(&m_header->writeTime100ns, now);
    }

private:
    HANDLE m_mapping = nullptr;
    HANDLE m_event = nullptr;
    SharedFrameHeader* m_header = nullptr;
    ULONGLONG m_lastAttempt = 0;
    bool m_released = false; // ReleaseHold since the last Write: HoldFor does nothing
};
