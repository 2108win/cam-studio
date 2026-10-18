#include "SourceFilter.h"

#include "../common/Nv12.h"

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

// ---- media type helpers ----------------------------------------------------------------

void FreeMediaTypeFields(AM_MEDIA_TYPE& mt)
{
    if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
    if (mt.pUnk) mt.pUnk->Release();
    mt.pUnk = nullptr;
}

HRESULT CopyMediaTypeFields(AM_MEDIA_TYPE& dst, const AM_MEDIA_TYPE& src)
{
    dst = src;
    dst.pbFormat = nullptr;
    if (src.cbFormat && src.pbFormat)
    {
        dst.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src.cbFormat));
        if (!dst.pbFormat) return E_OUTOFMEMORY;
        memcpy(dst.pbFormat, src.pbFormat, src.cbFormat);
    }
    else
    {
        dst.cbFormat = 0;
    }
    if (dst.pUnk) dst.pUnk->AddRef();
    return S_OK;
}

AM_MEDIA_TYPE* AllocMediaType(const AM_MEDIA_TYPE& src)
{
    auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!mt) return nullptr;
    if (FAILED(CopyMediaTypeFields(*mt, src)))
    {
        CoTaskMemFree(mt);
        return nullptr;
    }
    return mt;
}

void DeleteMediaType(AM_MEDIA_TYPE* mt)
{
    if (!mt) return;
    FreeMediaTypeFields(*mt);
    CoTaskMemFree(mt);
}

namespace
{
constexpr wchar_t kPinName[] = L"Capture";
constexpr REFERENCE_TIME kMinInterval = 166667;   // 60 fps
constexpr REFERENCE_TIME kMaxInterval = 2000000;  // 5 fps
constexpr uint32_t kFourccs[] = { kFourccNV12, kFourccYUY2, kFourccI420 };
constexpr uint32_t kStandardSizes[][2] = { { 1920, 1080 }, { 1280, 720 }, { 960, 540 }, { 640, 480 }, { 640, 360 }, { 320, 240 } };

bool IsPartial(const AM_MEDIA_TYPE* mt)
{
    return !mt || mt->majortype == GUID_NULL || mt->subtype == GUID_NULL || mt->formattype == GUID_NULL;
}

// NV12 (contiguous, pitch w) -> YUY2 (pitch 2w). Chroma rows are shared by two luma rows.
void Nv12ToYuy2(const uint8_t* nv12, uint32_t w, uint32_t h, uint8_t* dst)
{
    const uint8_t* uvPlane = nv12 + static_cast<size_t>(w) * h;
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* yr = nv12 + static_cast<size_t>(y) * w;
        const uint8_t* uv = uvPlane + static_cast<size_t>(y / 2) * w;
        uint8_t* d = dst + static_cast<size_t>(y) * w * 2;
        for (uint32_t x = 0; x < w; x += 2, d += 4)
        {
            d[0] = yr[x];
            d[1] = uv[x];
            d[2] = yr[x + 1];
            d[3] = uv[x + 1];
        }
    }
}

// NV12 -> I420 (Y, then U, then V, each plane packed).
void Nv12ToI420(const uint8_t* nv12, uint32_t w, uint32_t h, uint8_t* dst)
{
    const size_t lumaBytes = static_cast<size_t>(w) * h;
    memcpy(dst, nv12, lumaBytes);
    const uint8_t* uv = nv12 + lumaBytes;
    uint8_t* u = dst + lumaBytes;
    uint8_t* v = u + lumaBytes / 4;
    const size_t pairs = lumaBytes / 4;
    for (size_t i = 0; i < pairs; ++i)
    {
        u[i] = uv[2 * i];
        v[i] = uv[2 * i + 1];
    }
}

class PinEnumerator final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumPins>
{
public:
    HRESULT RuntimeClassInitialize(IPin* pin)
    {
        m_pin = pin;
        return S_OK;
    }
    IFACEMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) override
    {
        if (!pins || (count > 1 && !fetched)) return E_POINTER;
        ULONG n = 0;
        if (count && m_index == 0)
        {
            m_pin.CopyTo(&pins[0]);
            m_index = 1;
            n = 1;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override
    {
        m_index += count;
        return m_index > 1 ? S_FALSE : S_OK;
    }
    IFACEMETHODIMP Reset() override
    {
        m_index = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumPins** out) override
    {
        if (!out) return E_POINTER;
        ComPtr<PinEnumerator> e;
        RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<PinEnumerator>(&e, m_pin.Get()));
        e->m_index = m_index;
        return e.CopyTo(out);
    }

private:
    ComPtr<IPin> m_pin;
    ULONG m_index = 0;
};

class MediaTypeEnumerator final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumMediaTypes>
{
public:
    HRESULT RuntimeClassInitialize(std::vector<AM_MEDIA_TYPE>&& types)
    {
        m_types = std::move(types);
        return S_OK;
    }
    ~MediaTypeEnumerator()
    {
        for (auto& t : m_types) FreeMediaTypeFields(t);
    }
    IFACEMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** out, ULONG* fetched) override
    {
        if (!out || (count > 1 && !fetched)) return E_POINTER;
        ULONG n = 0;
        while (n < count && m_index < m_types.size())
        {
            out[n] = AllocMediaType(m_types[m_index]);
            if (!out[n]) break;
            ++n;
            ++m_index;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override
    {
        m_index += count;
        return m_index > m_types.size() ? S_FALSE : S_OK;
    }
    IFACEMETHODIMP Reset() override
    {
        m_index = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumMediaTypes** out) override
    {
        if (!out) return E_POINTER;
        std::vector<AM_MEDIA_TYPE> copy;
        for (const auto& t : m_types)
        {
            AM_MEDIA_TYPE c{};
            if (FAILED(CopyMediaTypeFields(c, t))) break;
            copy.push_back(c);
        }
        ComPtr<MediaTypeEnumerator> e;
        RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<MediaTypeEnumerator>(&e, std::move(copy)));
        e->m_index = m_index;
        return e.CopyTo(out);
    }

private:
    std::vector<AM_MEDIA_TYPE> m_types;
    size_t m_index = 0;
};
} // namespace

// ---- OutputPin -------------------------------------------------------------------------

HRESULT OutputPin::RuntimeClassInitialize(SourceFilter* filter)
{
    m_filter = filter;
    m_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    return m_timer ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

OutputPin::~OutputPin()
{
    StopWorker();
    if (m_timer) CloseHandle(m_timer);
}

void OutputPin::Orphan()
{
    StopWorker();
    std::lock_guard lk(m_lock);
    m_filter = nullptr;
}

static FILTER_STATE StateOf(const SourceFilter* f)
{
    return f ? f->State() : State_Stopped;
}

std::vector<PinFormat> OutputPin::Offered()
{
    std::vector<uint32_t> sizes[2]; // [0] widths, [1] heights, in order
    auto add = [&](uint32_t w, uint32_t h) {
        for (size_t i = 0; i < sizes[0].size(); ++i)
            if (sizes[0][i] == w && sizes[1][i] == h) return;
        sizes[0].push_back(w);
        sizes[1].push_back(h);
    };
    // The host's picture first (its own size is the best quality), when the host runs.
    m_reader.OpenExisting(true);
    if (m_reader.Width() >= 16 && m_reader.Height() >= 16) add(m_reader.Width() & ~1u, m_reader.Height() & ~1u);
    for (const auto& s : kStandardSizes) add(s[0], s[1]);

    std::vector<PinFormat> out;
    for (size_t i = 0; i < sizes[0].size(); ++i)
        for (uint32_t fourcc : kFourccs) out.push_back({ fourcc, sizes[0][i], sizes[1][i] });
    return out;
}

bool OutputPin::Parse(const AM_MEDIA_TYPE& mt, PinFormat& f, REFERENCE_TIME& interval)
{
    if (mt.majortype != MEDIATYPE_Video) return false;
    uint32_t fourcc = 0;
    for (uint32_t c : kFourccs)
        if (mt.subtype == FourccSubtype(c)) fourcc = c;
    if (!fourcc) return false;
    const BITMAPINFOHEADER* bmi = nullptr;
    REFERENCE_TIME avg = 0;
    if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER) && mt.pbFormat)
    {
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt.pbFormat);
        bmi = &vih->bmiHeader;
        avg = vih->AvgTimePerFrame;
    }
    else if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2) && mt.pbFormat)
    {
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER2*>(mt.pbFormat);
        bmi = &vih->bmiHeader;
        avg = vih->AvgTimePerFrame;
    }
    else
    {
        return false;
    }
    const LONG w = bmi->biWidth;
    const LONG h = bmi->biHeight < 0 ? -bmi->biHeight : bmi->biHeight;
    if (w < 16 || h < 16 || (w & 1) || (h & 1) || w > 8192 || h > 8192) return false;
    if (bmi->biCompression != fourcc) return false;
    f.fourcc = fourcc;
    f.width = static_cast<uint32_t>(w);
    f.height = static_cast<uint32_t>(h);
    interval = avg > 0 ? std::clamp<REFERENCE_TIME>(avg, kMinInterval, kMaxInterval) : 333333;
    return true;
}

HRESULT OutputPin::Build(const PinFormat& f, REFERENCE_TIME interval, AM_MEDIA_TYPE& mt)
{
    mt = {};
    auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!vih) return E_OUTOFMEMORY;
    *vih = {};
    vih->AvgTimePerFrame = interval;
    vih->dwBitRate = static_cast<DWORD>(std::min<uint64_t>(UINT32_MAX, static_cast<uint64_t>(f.SampleBytes()) * 8 * 10000000 / interval));
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = static_cast<LONG>(f.width);
    vih->bmiHeader.biHeight = static_cast<LONG>(f.height);
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = static_cast<WORD>(f.BitsPerPixel());
    vih->bmiHeader.biCompression = f.fourcc;
    vih->bmiHeader.biSizeImage = f.SampleBytes();
    mt.majortype = MEDIATYPE_Video;
    mt.subtype = FourccSubtype(f.fourcc);
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = f.SampleBytes();
    mt.formattype = FORMAT_VideoInfo;
    mt.cbFormat = sizeof(VIDEOINFOHEADER);
    mt.pbFormat = reinterpret_cast<BYTE*>(vih);
    return S_OK;
}

HRESULT OutputPin::NegotiateAllocator(IMemInputPin* input, uint32_t sampleBytes)
{
    ALLOCATOR_PROPERTIES wanted{};
    input->GetAllocatorRequirements(&wanted); // optional
    ComPtr<IMemAllocator> allocator;
    if (FAILED(input->GetAllocator(&allocator)))
        RETURN_IF_FAILED(CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&allocator)));
    ALLOCATOR_PROPERTIES props{};
    props.cBuffers = std::max<long>(wanted.cBuffers, 4);
    props.cbBuffer = std::max<long>(wanted.cbBuffer, static_cast<long>(sampleBytes));
    props.cbAlign = std::max<long>(wanted.cbAlign, 1);
    props.cbPrefix = wanted.cbPrefix;
    ALLOCATOR_PROPERTIES actual{};
    RETURN_IF_FAILED(allocator->SetProperties(&props, &actual));
    if (actual.cbBuffer < static_cast<long>(sampleBytes)) return E_FAIL;
    RETURN_IF_FAILED(input->NotifyAllocator(allocator.Get(), FALSE));
    m_allocator = allocator;
    return S_OK;
}

IFACEMETHODIMP OutputPin::Connect(IPin* receiver, const AM_MEDIA_TYPE* pmt)
{
    if (!receiver) return E_POINTER;
    std::lock_guard lk(m_lock);
    if (m_connected) return VFW_E_ALREADY_CONNECTED;
    if (StateOf(m_filter) != State_Stopped) return VFW_E_NOT_STOPPED;

    // Candidates, best first: a complete type the caller gave, else the current format and
    // then everything offered (narrowed by a partial type).
    std::vector<AM_MEDIA_TYPE> candidates;
    auto push = [&](const PinFormat& f, REFERENCE_TIME interval) {
        if (pmt)
        {
            if (pmt->majortype != GUID_NULL && pmt->majortype != MEDIATYPE_Video) return;
            if (pmt->subtype != GUID_NULL && pmt->subtype != FourccSubtype(f.fourcc)) return;
            if (pmt->formattype != GUID_NULL && pmt->formattype != FORMAT_VideoInfo) return;
        }
        AM_MEDIA_TYPE mt{};
        if (SUCCEEDED(Build(f, interval, mt))) candidates.push_back(mt);
    };
    if (!IsPartial(pmt))
    {
        AM_MEDIA_TYPE mt{};
        PinFormat f;
        REFERENCE_TIME interval = 0;
        if (!Parse(*pmt, f, interval)) return VFW_E_TYPE_NOT_ACCEPTED;
        if (SUCCEEDED(CopyMediaTypeFields(mt, *pmt))) candidates.push_back(mt);
    }
    else
    {
        if (m_formatSet) push(m_format, m_interval);
        for (const PinFormat& f : Offered()) push(f, m_formatSet ? m_interval : 333333);
    }

    HRESULT hr = VFW_E_NO_ACCEPTABLE_TYPES;
    for (AM_MEDIA_TYPE& mt : candidates)
    {
        PinFormat f;
        REFERENCE_TIME interval = 0;
        if (hr != S_OK && Parse(mt, f, interval) && receiver->ReceiveConnection(this, &mt) == S_OK)
        {
            ComPtr<IMemInputPin> input;
            if (SUCCEEDED(receiver->QueryInterface(IID_PPV_ARGS(&input))) && SUCCEEDED(NegotiateAllocator(input.Get(), f.SampleBytes())))
            {
                m_input = input;
                m_connected = receiver;
                m_format = f;
                m_interval = interval;
                m_formatSet = true;
                hr = S_OK;
            }
            else
            {
                receiver->Disconnect();
            }
        }
        FreeMediaTypeFields(mt);
    }
    return hr;
}

IFACEMETHODIMP OutputPin::Disconnect()
{
    std::lock_guard lk(m_lock);
    if (StateOf(m_filter) != State_Stopped) return VFW_E_NOT_STOPPED;
    if (!m_connected) return S_FALSE;
    m_input.Reset();
    m_allocator.Reset();
    m_connected.Reset();
    return S_OK;
}

IFACEMETHODIMP OutputPin::ConnectedTo(IPin** pin)
{
    if (!pin) return E_POINTER;
    std::lock_guard lk(m_lock);
    *pin = nullptr;
    if (!m_connected) return VFW_E_NOT_CONNECTED;
    return m_connected.CopyTo(pin);
}

IFACEMETHODIMP OutputPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt)
{
    if (!pmt) return E_POINTER;
    std::lock_guard lk(m_lock);
    *pmt = {};
    if (!m_connected) return VFW_E_NOT_CONNECTED;
    return Build(m_format, m_interval, *pmt);
}

IFACEMETHODIMP OutputPin::QueryPinInfo(PIN_INFO* info)
{
    if (!info) return E_POINTER;
    info->pFilter = m_filter;
    if (info->pFilter) info->pFilter->AddRef();
    info->dir = PINDIR_OUTPUT;
    wcscpy_s(info->achName, kPinName);
    return S_OK;
}

IFACEMETHODIMP OutputPin::QueryDirection(PIN_DIRECTION* dir)
{
    if (!dir) return E_POINTER;
    *dir = PINDIR_OUTPUT;
    return S_OK;
}

IFACEMETHODIMP OutputPin::QueryId(LPWSTR* id)
{
    if (!id) return E_POINTER;
    *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(kPinName)));
    if (!*id) return E_OUTOFMEMORY;
    memcpy(*id, kPinName, sizeof(kPinName));
    return S_OK;
}

IFACEMETHODIMP OutputPin::QueryAccept(const AM_MEDIA_TYPE* pmt)
{
    if (!pmt) return E_POINTER;
    PinFormat f;
    REFERENCE_TIME interval = 0;
    return Parse(*pmt, f, interval) ? S_OK : S_FALSE;
}

IFACEMETHODIMP OutputPin::EnumMediaTypes(IEnumMediaTypes** out)
{
    if (!out) return E_POINTER;
    std::vector<AM_MEDIA_TYPE> types;
    {
        std::lock_guard lk(m_lock);
        if (m_formatSet)
        {
            AM_MEDIA_TYPE mt{};
            if (SUCCEEDED(Build(m_format, m_interval, mt))) types.push_back(mt);
        }
        for (const PinFormat& f : Offered())
        {
            if (m_formatSet && f.fourcc == m_format.fourcc && f.width == m_format.width && f.height == m_format.height) continue;
            AM_MEDIA_TYPE mt{};
            if (SUCCEEDED(Build(f, m_formatSet ? m_interval : 333333, mt))) types.push_back(mt);
        }
    }
    ComPtr<MediaTypeEnumerator> e;
    RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<MediaTypeEnumerator>(&e, std::move(types)));
    return e.CopyTo(out);
}

// IAMStreamConfig

IFACEMETHODIMP OutputPin::SetFormat(AM_MEDIA_TYPE* pmt)
{
    if (!pmt) return E_POINTER;
    PinFormat f;
    REFERENCE_TIME interval = 0;
    if (!Parse(*pmt, f, interval)) return VFW_E_INVALIDMEDIATYPE;
    ComPtr<IPin> connected;
    {
        std::lock_guard lk(m_lock);
        if (m_connected)
        {
            if (StateOf(m_filter) != State_Stopped) return VFW_E_NOT_STOPPED;
            if (m_connected->QueryAccept(pmt) != S_OK) return VFW_E_INVALIDMEDIATYPE;
            connected = m_connected;
        }
        m_format = f;
        m_interval = interval;
        m_formatSet = true;
    }
    if (connected)
    {
        // Connected already: the graph connects us again with the new type.
        IFilterGraph* graph = m_filter ? m_filter->Graph() : nullptr;
        if (graph)
        {
            ComPtr<IFilterGraph2> g2;
            if (SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&g2)))) return g2->ReconnectEx(this, pmt);
            return graph->Reconnect(this);
        }
    }
    return S_OK;
}

IFACEMETHODIMP OutputPin::GetFormat(AM_MEDIA_TYPE** ppmt)
{
    if (!ppmt) return E_POINTER;
    AM_MEDIA_TYPE mt{};
    {
        std::lock_guard lk(m_lock);
        PinFormat f = m_format;
        REFERENCE_TIME interval = m_interval;
        if (!m_formatSet)
        {
            const auto offered = Offered();
            if (!offered.empty()) f = offered.front();
            interval = 333333;
        }
        RETURN_IF_FAILED(Build(f, interval, mt));
    }
    *ppmt = AllocMediaType(mt);
    FreeMediaTypeFields(mt);
    return *ppmt ? S_OK : E_OUTOFMEMORY;
}

IFACEMETHODIMP OutputPin::GetNumberOfCapabilities(int* count, int* size)
{
    if (!count || !size) return E_POINTER;
    std::lock_guard lk(m_lock);
    *count = static_cast<int>(Offered().size());
    *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

IFACEMETHODIMP OutputPin::GetStreamCaps(int index, AM_MEDIA_TYPE** ppmt, BYTE* caps)
{
    if (!ppmt || !caps) return E_POINTER;
    std::lock_guard lk(m_lock);
    const auto offered = Offered();
    if (index < 0 || static_cast<size_t>(index) >= offered.size()) return S_FALSE;
    const PinFormat& f = offered[static_cast<size_t>(index)];
    AM_MEDIA_TYPE mt{};
    RETURN_IF_FAILED(Build(f, 333333, mt));
    *ppmt = AllocMediaType(mt);
    FreeMediaTypeFields(mt);
    if (!*ppmt) return E_OUTOFMEMORY;

    auto* c = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(caps);
    *c = {};
    c->guid = FORMAT_VideoInfo;
    c->VideoStandard = 0;
    c->InputSize = { static_cast<LONG>(f.width), static_cast<LONG>(f.height) };
    c->MinCroppingSize = c->MaxCroppingSize = c->InputSize;
    c->CropGranularityX = c->CropGranularityY = 1;
    c->CropAlignX = c->CropAlignY = 1;
    c->MinOutputSize = c->MaxOutputSize = c->InputSize;
    c->OutputGranularityX = c->OutputGranularityY = 1;
    c->StretchTapsX = c->StretchTapsY = c->ShrinkTapsX = c->ShrinkTapsY = 0;
    c->MinFrameInterval = kMinInterval;
    c->MaxFrameInterval = kMaxInterval;
    c->MinBitsPerSecond = static_cast<LONG>(std::min<uint64_t>(LONG_MAX, static_cast<uint64_t>(f.SampleBytes()) * 8 * 10000000 / kMaxInterval));
    c->MaxBitsPerSecond = static_cast<LONG>(std::min<uint64_t>(LONG_MAX, static_cast<uint64_t>(f.SampleBytes()) * 8 * 10000000 / kMinInterval));
    return S_OK;
}

// IKsPropertySet

IFACEMETHODIMP OutputPin::Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD dataBytes, DWORD* returned)
{
    if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (returned) *returned = sizeof(GUID);
    if (!data) return S_OK;
    if (dataBytes < sizeof(GUID)) return E_UNEXPECTED;
    *static_cast<GUID*>(data) = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

IFACEMETHODIMP OutputPin::QuerySupported(REFGUID set, DWORD id, DWORD* support)
{
    if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (support) *support = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

// Streaming

HRESULT OutputPin::Activate()
{
    std::lock_guard lk(m_lock);
    if (!m_connected || !m_allocator) return VFW_E_NOT_CONNECTED;
    if (m_thread.joinable()) return S_OK;
    RETURN_IF_FAILED(m_reader.Open(m_format.width, m_format.height, true));
    RETURN_IF_FAILED(m_allocator->Commit());
    m_quit = false;
    m_reader.SetActive(true); // wakes the host: it opens its source for us
    m_thread = std::thread(&OutputPin::Worker, this);
    return S_OK;
}

void OutputPin::Run(REFERENCE_TIME start, IReferenceClock* clock)
{
    {
        std::lock_guard lk(m_clockLock);
        m_start = start;
        m_clock = clock;
    }
    m_running = true;
}

void OutputPin::StopWorker()
{
    m_quit = true;
    m_running = false;
    if (m_allocator) m_allocator->Decommit(); // GetBuffer returns, Receive is not waited for
    if (m_timer) CancelWaitableTimer(m_timer);
    if (m_thread.joinable()) m_thread.join();
    m_reader.SetActive(false);
}

void OutputPin::Inactive()
{
    StopWorker();
    std::lock_guard lk(m_clockLock);
    m_clock.Reset();
}

void OutputPin::Worker()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    PinFormat f;
    REFERENCE_TIME interval;
    ComPtr<IMemAllocator> allocator;
    ComPtr<IMemInputPin> input;
    {
        std::lock_guard lk(m_lock);
        f = m_format;
        interval = m_interval;
        allocator = m_allocator;
        input = m_input;
    }
    const uint32_t nv12Bytes = f.width * f.height * 3 / 2;
    if (f.fourcc != kFourccNV12) m_nv12.resize(nv12Bytes);

    auto waitUntil = [&](LONGLONG target, HANDLE extra) {
        const LONGLONG now = MFGetSystemTime();
        if (target <= now) return;
        LARGE_INTEGER due{};
        due.QuadPart = -(target - now);
        SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
        HANDLE handles[2] = { m_timer, extra };
        WaitForMultipleObjects(extra ? 2 : 1, handles, FALSE, 200);
    };

    LONGLONG due = 0;
    bool first = true;
    while (!m_quit)
    {
        // Paced like the Windows 11 media source: one frame per interval at the app's rate; a
        // source slower than that is waited for up to half an interval instead of repeated.
        const LONGLONG now = MFGetSystemTime();
        if (due == 0 || now - due > interval)
        {
            due = now;
        }
        else
        {
            waitUntil(due, nullptr);
            if (m_reader.LatestFrame() == m_reader.DeliveredFrame()) waitUntil(due + interval / 2, m_reader.FrameEvent());
        }
        due += interval;
        if (m_quit) break;

        ComPtr<IMediaSample> sample;
        const HRESULT got = allocator->GetBuffer(&sample, nullptr, nullptr, 0);
        if (FAILED(got))
        {
            if (m_quit) break;
            Sleep(5);
            continue;
        }
        BYTE* data = nullptr;
        if (FAILED(sample->GetPointer(&data)) || !data || sample->GetSize() < static_cast<long>(f.SampleBytes()))
        {
            Sleep(5);
            continue;
        }
        const LONGLONG stamp = MFGetSystemTime();
        if (f.fourcc == kFourccNV12)
        {
            m_reader.Fill(data, static_cast<LONG>(f.width), f.width, f.height, stamp);
        }
        else
        {
            m_reader.Fill(m_nv12.data(), static_cast<LONG>(f.width), f.width, f.height, stamp);
            if (f.fourcc == kFourccYUY2) Nv12ToYuy2(m_nv12.data(), f.width, f.height, data);
            else Nv12ToI420(m_nv12.data(), f.width, f.height, data);
        }
        sample->SetActualDataLength(static_cast<long>(f.SampleBytes()));
        sample->SetSyncPoint(TRUE);
        sample->SetPreroll(FALSE);
        sample->SetDiscontinuity(first ? TRUE : FALSE);
        sample->SetMediaTime(nullptr, nullptr);
        {
            // Stream time (the graph clock minus the run start): the renderer shows the frame at
            // once since it is "now". None while paused or without a clock.
            std::lock_guard lk(m_clockLock);
            REFERENCE_TIME t = 0;
            if (m_running && m_clock && SUCCEEDED(m_clock->GetTime(&t)))
            {
                REFERENCE_TIME start = t - m_start;
                REFERENCE_TIME end = start + interval;
                sample->SetTime(&start, &end);
            }
            else
            {
                sample->SetTime(nullptr, nullptr);
            }
        }
        const HRESULT hr = input->Receive(sample.Get());
        sample.Reset();
        if (hr == S_OK)
        {
            first = false;
        }
        else
        {
            // S_FALSE: the app is flushing or stopping, and asks for no more for now.
            if (m_quit) break;
            Sleep(10);
        }
    }
}

// ---- SourceFilter ------------------------------------------------------------------------

HRESULT SourceFilter::RuntimeClassInitialize()
{
    return Microsoft::WRL::MakeAndInitialize<OutputPin>(&m_pin, this);
}

SourceFilter::~SourceFilter()
{
    if (m_pin) m_pin->Orphan();
}

IFACEMETHODIMP SourceFilter::GetClassID(CLSID* id)
{
    if (!id) return E_POINTER;
    *id = CLSID_CamStudioDShow;
    return S_OK;
}

IFACEMETHODIMP SourceFilter::Stop()
{
    std::lock_guard lk(m_lock);
    if (m_state != State_Stopped)
    {
        m_pin->Inactive();
        m_state = State_Stopped;
    }
    return S_OK;
}

IFACEMETHODIMP SourceFilter::Pause()
{
    std::lock_guard lk(m_lock);
    if (m_state == State_Stopped && m_pin->Connected())
    {
        const HRESULT hr = m_pin->Activate();
        if (FAILED(hr)) return hr;
    }
    m_state = State_Paused;
    return S_OK;
}

IFACEMETHODIMP SourceFilter::Run(REFERENCE_TIME start)
{
    std::lock_guard lk(m_lock);
    if (m_state == State_Stopped && m_pin->Connected())
    {
        const HRESULT hr = m_pin->Activate();
        if (FAILED(hr)) return hr;
    }
    m_pin->Run(start, m_clock.Get());
    m_state = State_Running;
    return S_OK;
}

IFACEMETHODIMP SourceFilter::GetState(DWORD, FILTER_STATE* state)
{
    if (!state) return E_POINTER;
    *state = m_state;
    // A live source: nothing to cue while paused, so the graph does not wait for a sample.
    return m_state == State_Paused ? VFW_S_CANT_CUE : S_OK;
}

IFACEMETHODIMP SourceFilter::SetSyncSource(IReferenceClock* clock)
{
    std::lock_guard lk(m_lock);
    m_clock = clock;
    return S_OK;
}

IFACEMETHODIMP SourceFilter::GetSyncSource(IReferenceClock** clock)
{
    if (!clock) return E_POINTER;
    std::lock_guard lk(m_lock);
    return m_clock.CopyTo(clock);
}

IFACEMETHODIMP SourceFilter::EnumPins(IEnumPins** out)
{
    if (!out) return E_POINTER;
    ComPtr<PinEnumerator> e;
    RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<PinEnumerator>(&e, static_cast<IPin*>(m_pin.Get())));
    return e.CopyTo(out);
}

IFACEMETHODIMP SourceFilter::FindPin(LPCWSTR id, IPin** pin)
{
    if (!pin) return E_POINTER;
    *pin = nullptr;
    if (!id || wcscmp(id, kPinName) != 0) return VFW_E_NOT_FOUND;
    return m_pin.CopyTo(pin);
}

IFACEMETHODIMP SourceFilter::QueryFilterInfo(FILTER_INFO* info)
{
    if (!info) return E_POINTER;
    std::lock_guard lk(m_lock);
    wcscpy_s(info->achName, m_name);
    info->pGraph = m_graph;
    if (info->pGraph) info->pGraph->AddRef();
    return S_OK;
}

IFACEMETHODIMP SourceFilter::JoinFilterGraph(IFilterGraph* graph, LPCWSTR name)
{
    std::lock_guard lk(m_lock);
    m_graph = graph;
    if (name) wcsncpy_s(m_name, name, _TRUNCATE);
    else m_name[0] = L'\0';
    return S_OK;
}
