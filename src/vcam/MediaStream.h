#pragma once

#include "Common.h"
#include "FrameReader.h"

#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>

class MediaSource;

struct VideoFormat
{
    uint32_t width;
    uint32_t height;
    uint32_t fpsNum;
    uint32_t fpsDen;
};

class MediaStream
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>>
{
public:
    MediaStream() = default;
    ~MediaStream() override;

    HRESULT RuntimeClassInitialize(MediaSource* source, DWORD streamId,
                                   const std::vector<VideoFormat>& formats,
                                   FrameReader* reader);

    // IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* pv) override;

    // IMFMediaStream
    IFACEMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    IFACEMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    IFACEMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    HRESULT Start(IMFMediaType* mediaType);
    HRESULT Stop(bool sendEvent);
    void Shutdown();
    HRESULT SetSampleAllocator(IMFVideoSampleAllocator* allocator);
    IMFAttributes* Attributes() const { return m_attributes.Get(); }
    DWORD Id() const { return m_streamId; }

private:
    HRESULT CheckShutdownLocked() const;
    HRESULT StartLocked(IMFMediaType* mediaType, bool sendEvent);
    HRESULT StopLocked(bool sendEvent);
    HRESULT DeliverLocked(IUnknown* token);
    void WorkerLoop();
    void PaceUntilNextFrame(LONGLONG interval100ns);

    mutable std::mutex m_lock;
    std::condition_variable m_cv;
    std::thread m_worker;
    std::deque<ComPtr<IUnknown>> m_pendingTokens; // nullptr tokens allowed
    bool m_quit = false;

    ComPtr<IMFMediaSource> m_parent;
    ComPtr<IMFMediaEventQueue> m_eventQueue;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<IMFStreamDescriptor> m_descriptor;
    ComPtr<IMFVideoSampleAllocator> m_allocator;
    ComPtr<IMFMediaType> m_currentType;
    FrameReader* m_reader = nullptr;

    DWORD m_streamId = 0;
    bool m_shutdown = false;
    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    LONGLONG m_interval100ns = 333333;
    LONGLONG m_lastDelivery100ns = 0;
    HANDLE m_timer = nullptr;
};
