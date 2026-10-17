#pragma once

#include "Common.h"
#include "FrameReader.h"
#include "MediaStream.h"

class MediaSource
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
          IMFGetService,
          IKsControl,
          IMFSampleAllocatorControl>
{
public:
    MediaSource() = default;
    ~MediaSource() override;

    // activateAttributes carries the CAMSTUDIO_ATTR_* values set by the host.
    HRESULT RuntimeClassInitialize(IMFAttributes* activateAttributes);

    // IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* pv) override;

    // IMFMediaSource
    IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
    IFACEMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    IFACEMETHODIMP Pause() override;
    IFACEMETHODIMP Shutdown() override;
    IFACEMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* startPos) override;
    IFACEMETHODIMP Stop() override;

    // IMFMediaSourceEx
    IFACEMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    IFACEMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    IFACEMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    IFACEMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

    // IKsControl
    IFACEMETHODIMP KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID data, ULONG dataLength, ULONG* bytesReturned) override;
    IFACEMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLength, LPVOID data, ULONG dataLength, ULONG* bytesReturned) override;
    IFACEMETHODIMP KsEvent(PKSEVENT event, ULONG eventLength, LPVOID data, ULONG dataLength, ULONG* bytesReturned) override;

    // IMFSampleAllocatorControl
    IFACEMETHODIMP SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) override;
    IFACEMETHODIMP GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) override;

    bool IsShutdown() const
    {
        std::lock_guard lk(m_lock);
        return m_state == State::Shutdown;
    }

    // The formats we publish: native first, then downscales of the same aspect.
    static std::vector<VideoFormat> BuildFormats(uint32_t w, uint32_t h, uint32_t fpsNum, uint32_t fpsDen);

private:
    enum class State { Invalid, Stopped, Started, Shutdown };

    HRESULT CheckShutdownLocked() const;
    HRESULT CreateSourceAttributes(IMFAttributes* activateAttributes);

    mutable std::mutex m_lock;
    State m_state = State::Invalid;

    ComPtr<IMFMediaEventQueue> m_eventQueue;
    ComPtr<IMFPresentationDescriptor> m_pd;
    ComPtr<IMFAttributes> m_attributes;
    ComPtr<MediaStream> m_stream;
    FrameReader m_reader;
};
