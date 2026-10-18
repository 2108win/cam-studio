// DirectShow source filter "Cam Studio" for Windows 10, which has no virtual camera API
// (docs/windows10.md). It runs inside the app that uses the camera and reads the host's frames
// from the local shared-memory channel (CAMSTUDIO_LOCAL_MAPPING_NAME), the same NV12 slots the
// Windows 11 media source reads. Written without the DirectShow base classes (not in the SDK):
// one output pin of category CAPTURE, NV12 / YUY2 / I420, any of a few sizes, paced at the rate
// the app asked for. Apps that capture through DirectShow see it like a webcam: Discord, OBS,
// Zoom, older Chrome builds, Skype… Media Foundation apps on Windows 10 cannot see DirectShow
// filters at all; nothing can be done about that short of a driver.
#pragma once

#include "../vcam/Common.h"
#include "../vcam/FrameReader.h"

#include <dshow.h>
#include <dvdmedia.h>
#include <wrl/implements.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

// {7D2B9C41-3A6E-4F58-B0D1-9E4C2F6A8B13}
#define CAMSTUDIO_DSHOW_CLSID_STRING L"{7D2B9C41-3A6E-4F58-B0D1-9E4C2F6A8B13}"
inline constexpr GUID CLSID_CamStudioDShow =
    { 0x7d2b9c41, 0x3a6e, 0x4f58, { 0xb0, 0xd1, 0x9e, 0x4c, 0x2f, 0x6a, 0x8b, 0x13 } };

// The same name as the Windows 11 camera: apps show one "Cam Studio" either way.
#define CAMSTUDIO_DSHOW_FRIENDLY_NAME L"Cam Studio"

// FOURCC subtypes (uuids.h has some of them under names that vary between SDKs).
inline constexpr GUID FourccSubtype(uint32_t fourcc)
{
    return { fourcc, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
}
inline constexpr uint32_t kFourccNV12 = 0x3231564E; // 'NV12'
inline constexpr uint32_t kFourccYUY2 = 0x32595559; // 'YUY2'
inline constexpr uint32_t kFourccI420 = 0x30323449; // 'I420'

struct PinFormat
{
    uint32_t fourcc = kFourccNV12;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t SampleBytes() const { return fourcc == kFourccYUY2 ? width * height * 2 : width * height * 3 / 2; }
    uint32_t BitsPerPixel() const { return fourcc == kFourccYUY2 ? 16 : 12; }
};

// AM_MEDIA_TYPE helpers (no base classes).
void FreeMediaTypeFields(AM_MEDIA_TYPE& mt);
HRESULT CopyMediaTypeFields(AM_MEDIA_TYPE& dst, const AM_MEDIA_TYPE& src);
AM_MEDIA_TYPE* AllocMediaType(const AM_MEDIA_TYPE& src);
void DeleteMediaType(AM_MEDIA_TYPE* mt);

class SourceFilter;

class OutputPin final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IPin, IAMStreamConfig, IKsPropertySet, IQualityControl>
{
public:
    HRESULT RuntimeClassInitialize(SourceFilter* filter);
    ~OutputPin();

    // Filter state. The worker starts in Pause (a live source delivers while paused; the renderer
    // holds the sample until Run) and ends in Stop.
    HRESULT Activate();
    void Run(REFERENCE_TIME start, IReferenceClock* clock);
    void Inactive();
    bool Connected() const { return m_connected != nullptr; }
    // The filter is going away while something still holds the pin.
    void Orphan();

    // IPin
    IFACEMETHODIMP Connect(IPin* receiver, const AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    IFACEMETHODIMP Disconnect() override;
    IFACEMETHODIMP ConnectedTo(IPin** pin) override;
    IFACEMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    IFACEMETHODIMP QueryDirection(PIN_DIRECTION* dir) override;
    IFACEMETHODIMP QueryId(LPWSTR* id) override;
    IFACEMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP EnumMediaTypes(IEnumMediaTypes** out) override;
    IFACEMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    IFACEMETHODIMP EndOfStream() override { return E_UNEXPECTED; }
    IFACEMETHODIMP BeginFlush() override { return E_UNEXPECTED; }
    IFACEMETHODIMP EndFlush() override { return E_UNEXPECTED; }
    IFACEMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return E_UNEXPECTED; }

    // IAMStreamConfig
    IFACEMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    IFACEMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
    IFACEMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** ppmt, BYTE* caps) override;

    // IKsPropertySet: the pin category, which capture graph builders look for.
    IFACEMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP Get(REFGUID set, DWORD id, LPVOID instance, DWORD instanceBytes, LPVOID data, DWORD dataBytes, DWORD* returned) override;
    IFACEMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override;

    // IQualityControl
    IFACEMETHODIMP Notify(IBaseFilter*, Quality) override { return S_OK; }
    IFACEMETHODIMP SetSink(IQualityControl*) override { return S_OK; }

private:
    // Everything offered, in order of preference: the host's current picture size first.
    std::vector<PinFormat> Offered();
    static bool Parse(const AM_MEDIA_TYPE& mt, PinFormat& f, REFERENCE_TIME& interval);
    static HRESULT Build(const PinFormat& f, REFERENCE_TIME interval, AM_MEDIA_TYPE& mt);
    HRESULT NegotiateAllocator(IMemInputPin* input, uint32_t sampleBytes);
    void Worker();
    void StopWorker();

    SourceFilter* m_filter = nullptr; // owns this pin (no reference back: no cycle)
    std::mutex m_lock;                // connection and format
    ComPtr<IPin> m_connected;
    ComPtr<IMemInputPin> m_input;
    ComPtr<IMemAllocator> m_allocator;
    PinFormat m_format;                   // current (SetFormat, or the connection's)
    REFERENCE_TIME m_interval = 333333;   // 30 fps
    bool m_formatSet = false;

    FrameReader m_reader;
    std::vector<uint8_t> m_nv12;      // for the conversions
    std::thread m_thread;
    std::atomic<bool> m_quit{ false };
    std::atomic<bool> m_running{ false };
    HANDLE m_timer = nullptr;
    std::mutex m_clockLock;
    ComPtr<IReferenceClock> m_clock;
    REFERENCE_TIME m_start = 0;
};

class SourceFilter final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::ChainInterfaces<IBaseFilter, IMediaFilter, IPersist>>
{
public:
    HRESULT RuntimeClassInitialize();
    ~SourceFilter();

    FILTER_STATE State() const { return m_state; }
    IFilterGraph* Graph() const { return m_graph; }

    // IPersist
    IFACEMETHODIMP GetClassID(CLSID* id) override;
    // IMediaFilter
    IFACEMETHODIMP Stop() override;
    IFACEMETHODIMP Pause() override;
    IFACEMETHODIMP Run(REFERENCE_TIME start) override;
    IFACEMETHODIMP GetState(DWORD, FILTER_STATE* state) override;
    IFACEMETHODIMP SetSyncSource(IReferenceClock* clock) override;
    IFACEMETHODIMP GetSyncSource(IReferenceClock** clock) override;
    // IBaseFilter
    IFACEMETHODIMP EnumPins(IEnumPins** out) override;
    IFACEMETHODIMP FindPin(LPCWSTR id, IPin** pin) override;
    IFACEMETHODIMP QueryFilterInfo(FILTER_INFO* info) override;
    IFACEMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override;
    IFACEMETHODIMP QueryVendorInfo(LPWSTR*) override { return E_NOTIMPL; }

private:
    std::mutex m_lock;
    ComPtr<OutputPin> m_pin;
    std::atomic<FILTER_STATE> m_state{ State_Stopped };
    IFilterGraph* m_graph = nullptr; // not referenced, per the DirectShow rules
    ComPtr<IReferenceClock> m_clock;
    wchar_t m_name[128] = L"";
};
