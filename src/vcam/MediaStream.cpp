#include "MediaStream.h"
#include "MediaSource.h"

namespace
{
HRESULT CreateNv12Type(const VideoFormat& f, IMFMediaType** out)
{
    ComPtr<IMFMediaType> type;
    RETURN_IF_FAILED(MFCreateMediaType(&type));
    RETURN_IF_FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, f.width));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_SAMPLE_SIZE, static_cast<UINT32>(CamStudioNv12Bytes(f.width, f.height))));
    RETURN_IF_FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, f.width, f.height));
    RETURN_IF_FAILED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, f.fpsNum, f.fpsDen));
    RETURN_IF_FAILED(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    const uint64_t bitrate = CamStudioNv12Bytes(f.width, f.height) * 8 * f.fpsNum / (f.fpsDen ? f.fpsDen : 1);
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate > UINT32_MAX ? UINT32_MAX : bitrate)));
    *out = type.Detach();
    return S_OK;
}

HRESULT SetStreamCategory(IMFAttributes* attrs, DWORD streamId)
{
    RETURN_IF_FAILED(attrs->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
    RETURN_IF_FAILED(attrs->SetUINT32(MF_DEVICESTREAM_STREAM_ID, streamId));
    RETURN_IF_FAILED(attrs->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    RETURN_IF_FAILED(attrs->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));
    return S_OK;
}
} // namespace

MediaStream::~MediaStream()
{
    Shutdown();
}

HRESULT MediaStream::RuntimeClassInitialize(MediaSource* source, DWORD streamId,
                                            const std::vector<VideoFormat>& formats,
                                            FrameReader* reader)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, source);
    RETURN_HR_IF_NULL(E_INVALIDARG, reader);
    if (formats.empty()) return E_INVALIDARG;

    m_parent = source;
    m_streamId = streamId;
    m_reader = reader;

    std::vector<IMFMediaType*> types;
    for (const auto& f : formats)
    {
        IMFMediaType* t = nullptr;
        HRESULT hr = CreateNv12Type(f, &t);
        if (FAILED(hr))
        {
            for (auto* p : types) p->Release();
            return hr;
        }
        types.push_back(t);
    }

    HRESULT hr = MFCreateStreamDescriptor(m_streamId, static_cast<DWORD>(types.size()), types.data(), &m_descriptor);
    if (SUCCEEDED(hr))
    {
        ComPtr<IMFMediaTypeHandler> handler;
        hr = m_descriptor->GetMediaTypeHandler(&handler);
        if (SUCCEEDED(hr)) hr = handler->SetCurrentMediaType(types[0]);
    }
    for (auto* p : types) p->Release();
    RETURN_IF_FAILED(hr);

    RETURN_IF_FAILED(SetStreamCategory(m_descriptor.Get(), m_streamId));
    RETURN_IF_FAILED(MFCreateAttributes(&m_attributes, 4));
    RETURN_IF_FAILED(SetStreamCategory(m_attributes.Get(), m_streamId));
    RETURN_IF_FAILED(MFCreateEventQueue(&m_eventQueue));

    m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!m_timer) m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!m_timer) return HRESULT_FROM_WIN32(GetLastError());

    m_worker = std::thread(&MediaStream::WorkerLoop, this);
    return S_OK;
}

// ---- IMFMediaEventGenerator ------------------------------------------------

IFACEMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    return m_eventQueue->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    return m_eventQueue->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    // GetEvent may block, so do not hold the lock while waiting.
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard lk(m_lock);
        RETURN_IF_FAILED(CheckShutdownLocked());
        queue = m_eventQueue;
    }
    return queue->GetEvent(flags, event);
}

IFACEMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* pv)
{
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    return m_eventQueue->QueueEventParamVar(type, ext, status, pv);
}

// ---- IMFMediaStream --------------------------------------------------------

IFACEMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source)
{
    RETURN_HR_IF_NULL(E_POINTER, source);
    *source = nullptr;
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    return m_parent.CopyTo(source);
}

IFACEMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor)
{
    RETURN_HR_IF_NULL(E_POINTER, descriptor);
    *descriptor = nullptr;
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    return m_descriptor.CopyTo(descriptor);
}

IFACEMETHODIMP MediaStream::RequestSample(IUnknown* token)
{
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    if (m_state != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;

    // Delivered asynchronously by the worker so that pacing to the frame
    // rate never blocks Frame Server's calling thread.
    m_pendingTokens.emplace_back(token);
    m_cv.notify_one();
    return S_OK;
}

// ---- IMFMediaStream2 -------------------------------------------------------

IFACEMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state)
{
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    if (state == m_state) return S_OK;

    switch (state)
    {
    case MF_STREAM_STATE_PAUSED:
        if (m_state != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
        m_state = MF_STREAM_STATE_PAUSED;
        return S_OK;
    case MF_STREAM_STATE_RUNNING:
        return StartLocked(nullptr, false);
    case MF_STREAM_STATE_STOPPED:
        return StopLocked(false);
    default:
        return MF_E_INVALID_STATE_TRANSITION;
    }
}

IFACEMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state)
{
    RETURN_HR_IF_NULL(E_POINTER, state);
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    *state = m_state;
    return S_OK;
}

// ---- Control from MediaSource ---------------------------------------------

HRESULT MediaStream::Start(IMFMediaType* mediaType)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, mediaType);
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    return StartLocked(mediaType, true);
}

HRESULT MediaStream::Stop(bool sendEvent)
{
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    return StopLocked(sendEvent);
}

void MediaStream::Shutdown()
{
    {
        std::lock_guard lk(m_lock);
        if (m_shutdown) return;
        m_shutdown = true;
        m_quit = true;
        m_state = MF_STREAM_STATE_STOPPED;
        m_pendingTokens.clear();
        if (m_reader) m_reader->SetActive(false);
        if (m_eventQueue) m_eventQueue->Shutdown();
    }
    m_cv.notify_all();
    if (m_timer)
    {
        // Wake the worker if it is pacing.
        LARGE_INTEGER due{};
        due.QuadPart = -1;
        SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
    }
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) m_worker.join();

    std::lock_guard lk(m_lock);
    m_eventQueue.Reset();
    m_parent.Reset();
    m_allocator.Reset();
    m_currentType.Reset();
    m_descriptor.Reset();
    m_attributes.Reset();
    if (m_timer) { CloseHandle(m_timer); m_timer = nullptr; }
}

HRESULT MediaStream::SetSampleAllocator(IMFVideoSampleAllocator* allocator)
{
    std::lock_guard lk(m_lock);
    RETURN_IF_FAILED(CheckShutdownLocked());
    if (m_state == MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
    m_allocator = allocator;
    return S_OK;
}

// ---- Internals -------------------------------------------------------------

HRESULT MediaStream::CheckShutdownLocked() const
{
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (!m_eventQueue) return E_UNEXPECTED;
    return S_OK;
}

HRESULT MediaStream::StartLocked(IMFMediaType* mediaType, bool sendEvent)
{
    bool typeChanged = false;
    if (mediaType)
    {
        BOOL same = FALSE;
        if (m_currentType) m_currentType->Compare(mediaType, MF_ATTRIBUTES_MATCH_ALL_ITEMS, &same);
        if (!same)
        {
            m_currentType = mediaType;
            typeChanged = true;
        }
    }
    if (!m_currentType)
    {
        ComPtr<IMFMediaTypeHandler> handler;
        RETURN_IF_FAILED(m_descriptor->GetMediaTypeHandler(&handler));
        RETURN_IF_FAILED(handler->GetCurrentMediaType(&m_currentType));
        typeChanged = true;
    }

    if (m_state != MF_STREAM_STATE_RUNNING || typeChanged)
    {
        if (!m_allocator)
        {
            RETURN_IF_FAILED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&m_allocator)));
        }
        m_allocator->UninitializeSampleAllocator();
        RETURN_IF_FAILED(m_allocator->InitializeSampleAllocator(10, m_currentType.Get()));

        UINT32 num = 30, den = 1;
        RETURN_IF_FAILED(MFGetAttributeSize(m_currentType.Get(), MF_MT_FRAME_SIZE, &m_width, &m_height));
        MFGetAttributeRatio(m_currentType.Get(), MF_MT_FRAME_RATE, &num, &den);
        m_interval100ns = (num > 0) ? static_cast<LONGLONG>(10'000'000) * (den ? den : 1) / num : 333333;
        m_lastDelivery100ns = 0;
        VcamLog(L"Stream start %ux%u @ %u/%u", m_width, m_height, num, den);
    }

    if (sendEvent)
    {
        RETURN_IF_FAILED(m_eventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
    }
    m_state = MF_STREAM_STATE_RUNNING;
    m_reader->SetActive(true);
    m_cv.notify_one();
    return S_OK;
}

HRESULT MediaStream::StopLocked(bool sendEvent)
{
    m_state = MF_STREAM_STATE_STOPPED;
    m_pendingTokens.clear();
    m_reader->SetActive(false);
    if (sendEvent)
    {
        RETURN_IF_FAILED(m_eventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
    }
    return S_OK;
}

HRESULT MediaStream::DeliverLocked(IUnknown* token)
{
    ComPtr<IMFSample> sample;
    HRESULT hr = m_allocator->AllocateSample(&sample);
    if (FAILED(hr)) return hr; // MF_E_SAMPLEALLOCATOR_EMPTY is retried by the worker

    ComPtr<IMFMediaBuffer> buffer;
    RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));

    const LONGLONG now = MFGetSystemTime();
    ComPtr<IMF2DBuffer2> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d)))
    {
        BYTE* scanline0 = nullptr;
        BYTE* start = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        RETURN_IF_FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &start, &length));
        if (pitch > 0)
        {
            m_reader->Fill(scanline0, pitch, m_width, m_height, now);
        }
        buffer2d->Unlock2D();
    }
    else
    {
        BYTE* data = nullptr;
        DWORD maxLen = 0;
        RETURN_IF_FAILED(buffer->Lock(&data, &maxLen, nullptr));
        if (maxLen >= CamStudioNv12Bytes(m_width, m_height))
        {
            m_reader->Fill(data, static_cast<LONG>(m_width), m_width, m_height, now);
        }
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(CamStudioNv12Bytes(m_width, m_height)));
    }

    RETURN_IF_FAILED(sample->SetSampleTime(now));
    RETURN_IF_FAILED(sample->SetSampleDuration(m_interval100ns));
    if (token)
    {
        RETURN_IF_FAILED(sample->SetUnknown(MFSampleExtension_Token, token));
    }
    RETURN_IF_FAILED(m_eventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get()));
    m_lastDelivery100ns = now;
    return S_OK;
}

void MediaStream::PaceUntilNextFrame(LONGLONG interval)
{
    LONGLONG last;
    {
        std::lock_guard lk(m_lock);
        last = m_lastDelivery100ns;
    }
    if (last == 0) return;

    auto waitUntil = [&](LONGLONG target, HANDLE extra) {
        const LONGLONG now = MFGetSystemTime();
        if (target <= now) return;
        LARGE_INTEGER due{};
        due.QuadPart = -(target - now);
        SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
        HANDLE handles[2] = { m_timer, extra };
        WaitForMultipleObjects(extra ? 2 : 1, handles, FALSE, INFINITE);
    };

    // Never deliver faster than half an interval, and prefer to wake on a
    // fresh host frame so latency stays low. Give up waiting after 1.5x.
    waitUntil(last + interval / 2, nullptr);
    waitUntil(last + interval * 3 / 2, m_reader->FrameEvent());
}

void MediaStream::WorkerLoop()
{
    for (;;)
    {
        ComPtr<IUnknown> token;
        LONGLONG interval;
        {
            std::unique_lock lk(m_lock);
            m_cv.wait(lk, [&] {
                return m_quit || (m_state == MF_STREAM_STATE_RUNNING && !m_pendingTokens.empty());
            });
            if (m_quit) return;
            token = std::move(m_pendingTokens.front());
            m_pendingTokens.pop_front();
            interval = m_interval100ns;
        }

        PaceUntilNextFrame(interval);

        HRESULT hr;
        {
            std::lock_guard lk(m_lock);
            if (m_quit) return;
            if (m_state != MF_STREAM_STATE_RUNNING) continue;
            hr = DeliverLocked(token.Get());
            if (hr == MF_E_SAMPLEALLOCATOR_EMPTY)
            {
                // Downstream still holds every sample; try again shortly.
                m_pendingTokens.push_front(std::move(token));
            }
            else if (FAILED(hr))
            {
                VcamLog(L"DeliverLocked failed 0x%08X", hr);
                m_eventQueue->QueueEventParamVar(MEError, GUID_NULL, hr, nullptr);
            }
        }
        if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) Sleep(2);
    }
}
