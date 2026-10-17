// H.264 / HEVC (Annex-B) -> NV12 with the Media Foundation decoder MFT.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <vector>

class VideoDecoder
{
public:
    // nv12 points at the visible picture: `height` luma rows of `pitch`
    // bytes, then height/2 interleaved UV rows, contiguous.
    using FrameCallback = std::function<void(const uint8_t* nv12, long pitch, uint32_t width, uint32_t height)>;

    HRESULT Init(bool hevc, uint32_t width, uint32_t height);
    void Reset() { m_mft.Reset(); }

    // Feeds one access unit (Annex-B). Calls onFrame for every decoded
    // frame. A failure means the stream needs a new keyframe.
    HRESULT Decode(const uint8_t* data, size_t size, int64_t time100ns, const FrameCallback& onFrame);

private:
    HRESULT SelectOutputType();
    HRESULT Drain(const FrameCallback& onFrame);

    Microsoft::WRL::ComPtr<IMFTransform> m_mft;
    bool m_providesSamples = false;
    DWORD m_outputSize = 0;
    uint32_t m_codedWidth = 0, m_codedHeight = 0; // decoder surface (e.g. 1920x1088)
    uint32_t m_width = 0, m_height = 0;           // visible picture (e.g. 1920x1080)
    LONG m_stride = 0;
    std::vector<uint8_t> m_staging;
};
