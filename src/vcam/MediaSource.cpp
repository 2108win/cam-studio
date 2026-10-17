#include "MediaSource.h"

#include <algorithm>

namespace
{
constexpr DWORD kStreamId = 0;
constexpr uint32_t kDefaultWidth = 1920;
constexpr uint32_t kDefaultHeight = 1080;
} // namespace

MediaSource::~MediaSource()
{
    Shutdown();
}

std::vector<VideoFormat> MediaSource::BuildFormats(uint32_t w, uint32_t h, uint32_t fpsNum, uint32_t fpsDen)
{
    // Every size is offered at the native rate, plus 30 fps when the native
    // rate is higher: many conferencing apps reject or mishandle >30 fps.
    const bool add30 = fpsDen && fpsNum / fpsDen > 31;
    std::vector<VideoFormat> formats;
    auto add = [&](uint32_t fw, uint32_t fh) {
        formats.push_back({ fw, fh, fpsNum, fpsDen });
        if (add30) formats.push_back({ fw, fh, 30, 1 });
    };

    add(w, h); // native first

    // Smaller sizes with the native aspect ratio, for apps that only accept
    // common resolutions. They are scaled from the native frame.
    static const uint32_t heights[] = { 2160, 1440, 1080, 720, 540, 480, 360 };
    for (uint32_t th : heights)
    {
        if (th >= h) continue;
        uint32_t tw = static_cast<uint32_t>((static_cast<uint64_t>(th) * w + h / 2) / h);
        tw = (tw + 1) & ~1u;
        if (tw < 160) continue;
        add(tw, th);
    }
    return formats;
}

HRESULT MediaSource::RuntimeClassInitialize(IMFAttributes* activateAttributes)
{
    std::lock_guard lk(m_lock);
    if (m_state != State::Invalid) return MF_E_ALREADY_INITIALIZED;

    uint32_t w = kDefaultWidth, h = kDefaultHeight, fn = 30, fd = 1;
    if (activateAttributes)
    {
        UINT32 a = 0, b = 0;
        if (SUCCEEDED(MFGetAttributeSize(activateAttributes, CAMSTUDIO_ATTR_NATIVE_SIZE, &a, &b)) && a && b)
        {
            w = a & ~1u;
            h = b & ~1u;
        }
        if (SUCCEEDED(MFGetAttributeRatio(activateAttributes, CAMSTUDIO_ATTR_NATIVE_FPS, &a, &b)) && a && b)
        {
            fn = a;
            fd = b;
        }
    }
    VcamLog(L"Initialize native %ux%u @ %u/%u", w, h, fn, fd);

    RETURN_IF_FAILED(m_reader.Open(w, h));
    RETURN_IF_FAILED(CreateSourceAttributes(activateAttributes));
    RETURN_IF_FAILED(MFCreateEventQueue(&m_eventQueue));

    RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<MediaStream>(
        &m_stream, this, kStreamId, BuildFormats(w, h, fn, fd), &m_reader));

    ComPtr<IMFStreamDescriptor> sd;
    RETURN_IF_FAILED(m_stream->GetStreamDescriptor(&sd));
    IMFStreamDescriptor* sds[] = { sd.Get() };
    RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, sds, &m_pd));

    m_state = State::Stopped;
    return S_OK;
}

HRESULT MediaSource::CreateSourceAttributes(IMFAttributes* activateAttributes)
{
    RETURN_IF_FAILED(MFCreateAttributes(&m_attributes, 4));
    if (activateAttributes)
    {
        RETURN_IF_FAILED(activateAttributes->CopyAllItems(m_attributes.Get()));
    }

    // A legacy profile that accepts every media type, so profile-aware apps
    // (e.g. the Windows Camera app) still see all of our formats. Optional:
    // if any step fails we log it and run without profiles.
    ComPtr<IMFSensorProfileCollection> profiles;
    ComPtr<IMFSensorProfile> profile;
    HRESULT hr = MFCreateSensorProfileCollection(&profiles);
    if (SUCCEEDED(hr)) hr = MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile);
    if (SUCCEEDED(hr)) hr = profile->AddProfileFilter(kStreamId, L"((RES==;FRT==;SUT==))");
    if (SUCCEEDED(hr)) hr = profiles->AddProfile(profile.Get());
    if (SUCCEEDED(hr)) hr = m_attributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.Get());
    if (FAILED(hr)) VcamLog(L"Sensor profile skipped (0x%08X)", hr);
    return S_OK;
}

HRESULT MediaSource::CheckShutdownLocked() const
{
    if (m_state == State::Shutdown) return MF_E_SHUTDOWN;
    if (!m_eventQueue || !m_stream) return E_UNEXPECTED;
    return S_OK;
}

// ---- IMFMediaEventGenerator ------------------------------------------------

IFACEMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    return m_eventQueue->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    return m_eventQueue->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard lk(m_lock);
        RETURN_IF_FAILED(CheckShutdownLocked());
        queue = m_eventQueue;
    }
    return queue->GetEvent(flags, event);
}

IFACEMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* pv)
{
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    return m_eventQueue->QueueEventParamVar(type, ext, status, pv);
}

// ---- IMFMediaSource --------------------------------------------------------

IFACEMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** pd)
{
    RETURN_HR_IF_NULL(E_POINTER, pd);
    *pd = nullptr;
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    return m_pd->Clone(pd);
}

IFACEMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics)
{
    RETURN_HR_IF_NULL(E_POINTER, characteristics);
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

IFACEMETHODIMP MediaSource::Pause()
{
    return MF_E_INVALID_STATE_TRANSITION;
}

IFACEMETHODIMP MediaSource::Shutdown()
{
    ComPtr<MediaStream> stream;
    {
        std::lock_guard lk(m_lock);
        if (m_state == State::Shutdown) return S_OK;
        m_state = State::Shutdown;
        if (m_eventQueue) m_eventQueue->Shutdown();
        stream = std::move(m_stream);
    }
    // Outside the source lock: this joins the stream's worker thread.
    if (stream) stream->Shutdown();

    std::lock_guard lk(m_lock);
    m_eventQueue.Reset();
    m_pd.Reset();
    m_attributes.Reset();
    m_reader.Close();
    return S_OK;
}

IFACEMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* startPos)
{
    if (!pd || !startPos) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;

    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    if (m_state == State::Invalid) return MF_E_INVALID_STATE_TRANSITION;

    DWORD count = 0;
    RETURN_IF_FAILED(pd->GetStreamDescriptorCount(&count));
    if (count != 1) return E_INVALIDARG;

    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> sd;
    RETURN_IF_FAILED(pd->GetStreamDescriptorByIndex(0, &selected, &sd));

    BOOL wasSelected = FALSE;
    ComPtr<IMFStreamDescriptor> ours;
    RETURN_IF_FAILED(m_pd->GetStreamDescriptorByIndex(0, &wasSelected, &ours));

    PROPVARIANT startTime;
    RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &startTime));

    if (selected)
    {
        RETURN_IF_FAILED(m_pd->SelectStream(0));

        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> type;
        RETURN_IF_FAILED(sd->GetMediaTypeHandler(&handler));
        RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));

        ComPtr<IUnknown> unk;
        RETURN_IF_FAILED(m_stream.As(&unk));
        RETURN_IF_FAILED(m_eventQueue->QueueEventParamUnk(wasSelected ? MEUpdatedStream : MENewStream,
            GUID_NULL, S_OK, unk.Get()));
        RETURN_IF_FAILED(m_stream->Start(type.Get()));
    }
    else if (wasSelected)
    {
        RETURN_IF_FAILED(m_pd->DeselectStream(0));
        RETURN_IF_FAILED(m_stream->Stop(false));
    }

    RETURN_IF_FAILED(m_eventQueue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &startTime));
    m_state = State::Started;
    return S_OK;
}

IFACEMETHODIMP MediaSource::Stop()
{
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    if (m_state != State::Started) return MF_E_INVALID_STATE_TRANSITION;

    RETURN_IF_FAILED(m_stream->Stop(true));
    RETURN_IF_FAILED(m_pd->DeselectStream(0));

    PROPVARIANT stopTime;
    RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &stopTime));
    RETURN_IF_FAILED(m_eventQueue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &stopTime));
    m_state = State::Stopped;
    return S_OK;
}

// ---- IMFMediaSourceEx ------------------------------------------------------

IFACEMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes)
{
    RETURN_HR_IF_NULL(E_POINTER, attributes);
    *attributes = nullptr;
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    // Must hand out a reference to our store, not a copy.
    return m_attributes.CopyTo(attributes);
}

IFACEMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes)
{
    RETURN_HR_IF_NULL(E_POINTER, attributes);
    *attributes = nullptr;
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    if (streamId != m_stream->Id()) return MF_E_NOT_FOUND;
    IMFAttributes* a = m_stream->Attributes();
    if (!a) return E_UNEXPECTED;
    a->AddRef();
    *attributes = a;
    return S_OK;
}

IFACEMETHODIMP MediaSource::SetD3DManager(IUnknown*)
{
    // System-memory samples only for now.
    return E_NOTIMPL;
}

// ---- IMFGetService ---------------------------------------------------------

IFACEMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* object)
{
    RETURN_HR_IF_NULL(E_POINTER, object);
    *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// ---- IKsControl ------------------------------------------------------------
// No camera controls yet. ERROR_SET_NOT_FOUND mimics an AVStream driver that
// has no handler for the requested property set.

IFACEMETHODIMP MediaSource::KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID, ULONG, ULONG* bytesReturned)
{
    if (!property || propertyLength < sizeof(KSPROPERTY)) return E_INVALIDARG;
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* bytesReturned)
{
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* bytesReturned)
{
    if (bytesReturned) *bytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ---- IMFSampleAllocatorControl ---------------------------------------------

IFACEMETHODIMP MediaSource::SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator)
{
    RETURN_HR_IF_NULL(E_POINTER, allocator);
    ComPtr<MediaStream> stream;
    {
        std::lock_guard lk(m_lock);
        RETURN_IF_FAILED(CheckShutdownLocked());
        if (outputStreamId != m_stream->Id()) return MF_E_NOT_FOUND;
        stream = m_stream;
    }
    ComPtr<IMFVideoSampleAllocator> videoAllocator;
    RETURN_IF_FAILED(allocator->QueryInterface(IID_PPV_ARGS(&videoAllocator)));
    return stream->SetSampleAllocator(videoAllocator.Get());
}

IFACEMETHODIMP MediaSource::GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage)
{
    RETURN_HR_IF_NULL(E_POINTER, inputStreamId);
    RETURN_HR_IF_NULL(E_POINTER, usage);
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    if (outputStreamId != m_stream->Id()) return MF_E_NOT_FOUND;
    *inputStreamId = outputStreamId;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}
