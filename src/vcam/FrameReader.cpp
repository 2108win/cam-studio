#include "FrameReader.h"
#include "../common/Nv12.h"

#include <sddl.h>
#include <vector>

FrameReader::~FrameReader()
{
    Close();
}

HRESULT FrameReader::Open(uint32_t nativeWidth, uint32_t nativeHeight, bool local)
{
    if (m_header) return S_OK;

    // The global channel is created by Frame Server (LocalService) with a DACL the unelevated
    // host can open. The local one lives in the user's own session: default security.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!local && !ConvertStringSecurityDescriptorToSecurityDescriptorW(
            CAMSTUDIO_OBJECT_SDDL, SDDL_REVISION_1, &sd, nullptr))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
    SECURITY_ATTRIBUTES* psa = local ? nullptr : &sa;

    const uint64_t bytes = local ? CamStudioLocalMappingBytes() : CamStudioMappingBytes(nativeWidth, nativeHeight);
    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, psa, PAGE_READWRITE,
        static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes),
        local ? CAMSTUDIO_LOCAL_MAPPING_NAME : CAMSTUDIO_MAPPING_NAME);
    const DWORD mapErr = GetLastError();
    m_event = CreateEventW(psa, FALSE, FALSE, local ? CAMSTUDIO_LOCAL_EVENT_NAME : CAMSTUDIO_EVENT_NAME);
    if (sd) LocalFree(sd);

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

    if (mapErr == ERROR_ALREADY_EXISTS && m_header->magic != CAMSTUDIO_MAGIC)
    {
        // Someone else created it a moment ago and is still filling the header.
        for (int i = 0; i < 100 && m_header->magic != CAMSTUDIO_MAGIC; ++i) Sleep(1);
    }
    if (mapErr != ERROR_ALREADY_EXISTS || m_header->magic != CAMSTUDIO_MAGIC)
    {
        // First opener initialises the header. The host waits for magic.
        uint32_t w = nativeWidth, h = nativeHeight;
        if (local) CamStudioClampLocalSize(w, h);
        CamStudioInitHeader(m_header, w, h, local ? CamStudioLocalSlotBytes() : CamStudioNv12Bytes(w, h));
    }

    VcamLog(L"Shared memory ready %ux%u (existing=%d)", m_header->width, m_header->height,
        mapErr == ERROR_ALREADY_EXISTS);
    return S_OK;
}

bool FrameReader::OpenExisting(bool local)
{
    if (m_header) return true;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, local ? CAMSTUDIO_LOCAL_MAPPING_NAME : CAMSTUDIO_MAPPING_NAME);
    if (!mapping) return false;
    auto* header = static_cast<SharedFrameHeader*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!header || header->magic != CAMSTUDIO_MAGIC || header->version != CAMSTUDIO_VERSION || header->slots != CAMSTUDIO_SLOTS)
    {
        if (header) UnmapViewOfFile(header);
        CloseHandle(mapping);
        return false;
    }
    m_mapping = mapping;
    m_header = header;
    m_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, local ? CAMSTUDIO_LOCAL_EVENT_NAME : CAMSTUDIO_EVENT_NAME);
    return true;
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
        if (!sw || !sh || CamStudioNv12Bytes(sw, sh) > m_header->frameBytes)
        {
            DrawNoSignal(dst, pitch, width, height, now100ns);
            return false;
        }

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
                Nv12FitInto(CamStudioSlot(m_header, slot), sw, sw, sh, dst, pitch, width, height);
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
            Nv12FitInto(CamStudioSlot(m_header, static_cast<uint32_t>(latest & 3) % CAMSTUDIO_SLOTS), sw, sw, sh,
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
