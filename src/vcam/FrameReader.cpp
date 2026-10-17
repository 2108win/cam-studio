#include "FrameReader.h"
#include "../common/Nv12.h"

#include <sddl.h>
#include <vector>

FrameReader::~FrameReader()
{
    Close();
}

HRESULT FrameReader::Open(uint32_t nativeWidth, uint32_t nativeHeight)
{
    if (m_header) return S_OK;

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            CAMSTUDIO_OBJECT_SDDL, SDDL_REVISION_1, &sd, nullptr))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };

    const uint64_t bytes = CamStudioMappingBytes(nativeWidth, nativeHeight);
    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
        static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), CAMSTUDIO_MAPPING_NAME);
    const DWORD mapErr = GetLastError();
    m_event = CreateEventW(&sa, FALSE, FALSE, CAMSTUDIO_EVENT_NAME);
    LocalFree(sd);

    if (!m_mapping || !m_event)
    {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        VcamLog(L"CreateFileMapping/CreateEvent failed 0x%08X", hr);
        Close();
        return hr;
    }

    m_header = static_cast<SharedFrameHeader*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!m_header)
    {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        Close();
        return hr;
    }

    if (mapErr != ERROR_ALREADY_EXISTS || m_header->magic != CAMSTUDIO_MAGIC)
    {
        // First opener initialises the header. The host waits for magic.
        m_header->version = CAMSTUDIO_VERSION;
        m_header->width = nativeWidth;
        m_header->height = nativeHeight;
        m_header->stride = nativeWidth;
        m_header->slots = CAMSTUDIO_SLOTS;
        m_header->frameBytes = CamStudioNv12Bytes(nativeWidth, nativeHeight);
        m_header->latest = 0;
        m_header->writeTime100ns = 0;
        m_header->readerActive = 0;
        for (uint32_t s = 0; s < CAMSTUDIO_SLOTS; ++s)
        {
            m_header->slotSeq[s] = 0;
            m_header->slotReaders[s] = 0;
        }
        MemoryBarrier();
        m_header->magic = CAMSTUDIO_MAGIC;
    }

    VcamLog(L"Shared memory ready %ux%u (existing=%d)", m_header->width, m_header->height,
        mapErr == ERROR_ALREADY_EXISTS);
    return S_OK;
}

void FrameReader::Close()
{
    SetActive(false);
    if (m_header) { UnmapViewOfFile(m_header); m_header = nullptr; }
    if (m_mapping) { CloseHandle(m_mapping); m_mapping = nullptr; }
    if (m_event) { CloseHandle(m_event); m_event = nullptr; }
}

void FrameReader::SetActive(bool active)
{
    if (!m_header || active == m_active) return;
    m_active = active;
    InterlockedAdd(&m_header->readerActive, active ? 1 : -1);
}

bool FrameReader::Fill(uint8_t* dst, LONG pitch, uint32_t width, uint32_t height, LONGLONG now100ns)
{
    if (m_header && m_header->slots == CAMSTUDIO_SLOTS)
    {
        const uint32_t sw = m_header->width;
        const uint32_t sh = m_header->height;

        // "No signal" only when the host really stopped (or never wrote):
        // racing the writer just means trying the newer frame.
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const LONG64 latest = m_header->latest;
            if (latest == 0) break;                                              // host never wrote
            if (now100ns - m_header->writeTime100ns > CAMSTUDIO_STALE_100NS) break; // host stopped
            const uint32_t slot = static_cast<uint32_t>(latest & 3);
            if (slot >= CAMSTUDIO_SLOTS) break;

            InterlockedIncrement(&m_header->slotReaders[slot]);
            const LONG64 seq1 = m_header->slotSeq[slot];
            bool ok = false;
            if (!(seq1 & 1))
            {
                Nv12Scale(CamStudioSlot(m_header, slot), sw, sw, sh, dst, pitch, width, height);
                MemoryBarrier();
                ok = m_header->slotSeq[slot] == seq1;
            }
            InterlockedDecrement(&m_header->slotReaders[slot]);
            if (ok)
            {
                m_delivered = latest >> 2;
                return true;
            }
        }
        // Still racing after several tries (several readers at a very high
        // rate): a possibly torn copy of the latest frame beats a grey flash.
        const LONG64 latest = m_header->latest;
        if (latest != 0 && now100ns - m_header->writeTime100ns <= CAMSTUDIO_STALE_100NS)
        {
            Nv12Scale(CamStudioSlot(m_header, static_cast<uint32_t>(latest & 3) % CAMSTUDIO_SLOTS), sw, sw, sh,
                      dst, pitch, width, height);
            m_delivered = latest >> 2;
            return true;
        }
    }

    DrawNoSignal(dst, pitch, width, height, now100ns);
    return false;
}

void FrameReader::DrawNoSignal(uint8_t* dst, LONG pitch, uint32_t width, uint32_t height, LONGLONG now100ns)
{
    // Dark grey with a sweeping bar so a stuck pipeline is easy to spot.
    const uint32_t barW = (width / 16) & ~1u;
    const uint32_t barX = static_cast<uint32_t>((now100ns / 100000) % (width + barW)) & ~1u;
    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t* row = dst + static_cast<size_t>(y) * pitch;
        memset(row, 0x30, width);
        if (barX < width)
        {
            uint32_t w = (barX + barW > width) ? width - barX : barW;
            memset(row + barX, 0xB0, w);
        }
    }
    uint8_t* uv = dst + static_cast<size_t>(height) * pitch;
    for (uint32_t y = 0; y < height / 2; ++y)
    {
        memset(uv + static_cast<size_t>(y) * pitch, 0x80, width);
    }
}
