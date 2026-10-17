#include "VideoDecoder.h"

#include <mferror.h>
#include <mfidl.h>
#include <strmif.h>   // ICodecAPI
#include <initguid.h> // defines CODECAPI_AVLowLatencyMode in this file only
#include <codecapi.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

HRESULT VideoDecoder::Init(bool hevc, uint32_t width, uint32_t height)
{
    m_mft.Reset();
    const GUID subtype = hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, subtype };
    MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_NV12 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    // Synchronous decoders only; hardware MFTs are async and need an event loop.
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &activates, &count);
    if (FAILED(hr)) return hr;
    if (count == 0)
    {
        CoTaskMemFree(activates);
        return MF_E_TOPO_CODEC_NOT_FOUND; // e.g. HEVC Video Extensions not installed
    }
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_mft));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr)) return hr;

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(m_mft->GetAttributes(&attrs))) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(m_mft.As(&codecApi)))
    {
        VARIANT v;
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }

    ComPtr<IMFMediaType> type;
    hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return hr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    hr = m_mft->SetInputType(0, type.Get(), 0);
    if (FAILED(hr)) return hr;

    m_width = width;
    m_height = height;
    hr = SelectOutputType();
    if (FAILED(hr)) return hr;
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return S_OK;
}

HRESULT VideoDecoder::SelectOutputType()
{
    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> type;
        HRESULT hr = m_mft->GetOutputAvailableType(0, i, &type);
        if (FAILED(hr)) return hr; // MF_E_NO_MORE_TYPES: no NV12 offered
        GUID sub{};
        type->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub != MFVideoFormat_NV12) continue;
        hr = m_mft->SetOutputType(0, type.Get(), 0);
        if (FAILED(hr)) return hr;

        // The surface may be taller than the picture (1088 for 1080p):
        // the visible area is the minimum display aperture.
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &m_codedWidth, &m_codedHeight);
        UINT32 stride = 0;
        m_stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<LONG>(stride) : static_cast<LONG>(m_codedWidth);
        MFVideoArea area{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0)
        {
            m_width = static_cast<uint32_t>(area.Area.cx);
            m_height = static_cast<uint32_t>(area.Area.cy);
        }
        else
        {
            m_width = m_codedWidth;
            m_height = m_codedHeight;
        }

        MFT_OUTPUT_STREAM_INFO info{};
        m_mft->GetOutputStreamInfo(0, &info);
        m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        m_outputSize = info.cbSize;
        return S_OK;
    }
}

HRESULT VideoDecoder::Decode(const uint8_t* data, size_t size, int64_t time100ns, const FrameCallback& onFrame)
{
    if (!m_mft) return E_UNEXPECTED;

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
    if (FAILED(hr)) return hr;
    BYTE* dst = nullptr;
    buffer->Lock(&dst, nullptr, nullptr);
    memcpy(dst, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(time100ns);

    hr = m_mft->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING)
    {
        // Output is pending: drain it, then retry once.
        Drain(onFrame);
        hr = m_mft->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return hr;
    return Drain(onFrame);
}

HRESULT VideoDecoder::Drain(const FrameCallback& onFrame)
{
    for (;;)
    {
        MFT_OUTPUT_DATA_BUFFER out{};
        ComPtr<IMFSample> own;
        if (!m_providesSamples)
        {
            ComPtr<IMFMediaBuffer> b;
            MFCreateMemoryBuffer(m_outputSize ? m_outputSize : m_codedWidth * m_codedHeight * 3 / 2, &b);
            MFCreateSample(&own);
            own->AddBuffer(b.Get());
            out.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr = m_mft->ProcessOutput(0, 1, &out, &status);
        ComPtr<IMFSample> produced;
        produced.Attach(m_providesSamples ? out.pSample : nullptr);
        if (out.pEvents) out.pEvents->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
        {
            // First frame (or resolution change): the real output format is known now.
            hr = SelectOutputType();
            if (FAILED(hr)) return hr;
            continue;
        }
        if (FAILED(hr)) return hr;

        IMFSample* result = m_providesSamples ? produced.Get() : own.Get();
        ComPtr<IMFMediaBuffer> buf;
        if (!result || FAILED(result->ConvertToContiguousBuffer(&buf))) continue;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len))) continue;

        const LONG stride = m_stride > 0 ? m_stride : static_cast<LONG>(m_codedWidth);
        const size_t lumaBytes = static_cast<size_t>(stride) * m_codedHeight;
        if (len >= lumaBytes + lumaBytes / 2)
        {
            if (m_height == m_codedHeight)
            {
                onFrame(p, stride, m_width, m_height);
            }
            else
            {
                // UV starts after the full coded height: repack so it follows
                // the visible rows, as the rest of the pipeline expects.
                const size_t visibleLuma = static_cast<size_t>(stride) * m_height;
                m_staging.resize(visibleLuma + visibleLuma / 2);
                memcpy(m_staging.data(), p, visibleLuma);
                memcpy(m_staging.data() + visibleLuma, p + lumaBytes, visibleLuma / 2);
                onFrame(m_staging.data(), stride, m_width, m_height);
            }
        }
        buf->Unlock();
    }
}
