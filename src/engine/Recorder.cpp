#include "Recorder.h"

#include "../common/Nv12.h"
#include "../common/SharedFrame.h"

#include <mfapi.h>
#include <mferror.h>
#include <mmdeviceapi.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace
{
// The default microphone (Windows' "communications"-independent default for recording).
HRESULT DefaultMicrophone(ComPtr<IMFMediaSource>& source)
{
    ComPtr<IMMDeviceEnumerator> devices;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices));
    if (FAILED(hr)) return hr;
    ComPtr<IMMDevice> device;
    hr = devices->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    if (FAILED(hr)) return hr;
    LPWSTR id = nullptr;
    hr = device->GetId(&id);
    if (FAILED(hr)) return hr;
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 2);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID);
    attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID, id);
    CoTaskMemFree(id);
    return MFCreateDeviceSource(attrs.Get(), &source);
}
} // namespace

LONGLONG Recorder::Now100ns() const
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<LONGLONG>((now.QuadPart - m_start.QuadPart) * 10'000'000.0 / m_freq.QuadPart);
}

int64_t Recorder::ElapsedMs() const
{
    return m_active ? Now100ns() / 10000 : 0;
}

std::wstring Recorder::AudioProblem() const
{
    std::lock_guard lk(m_problemLock);
    return m_audioProblem;
}

HRESULT Recorder::Start(const std::wstring& path, uint32_t width, uint32_t height, uint32_t fps, bool microphone)
{
    Stop();
    width &= ~1u;
    height &= ~1u;
    if (width < 16 || height < 16) return E_INVALIDARG;
    fps = std::clamp<uint32_t>(fps, 5, 60);
    {
        std::lock_guard lk(m_problemLock);
        m_audioProblem.clear();
    }

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 3);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE); // the GPU's H.264 encoder
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);       // a live source: never block it
    ComPtr<IMFSinkWriter> writer;
    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &writer);
    if (FAILED(hr)) return hr;

    // H.264 at about 0.1 bit per pixel and frame: 1080p30 ~ 6 Mbit/s, 4K30 ~ 25 Mbit/s.
    const uint32_t bitrate = std::clamp<uint32_t>(static_cast<uint32_t>(uint64_t(width) * height * fps / 10), 2'000'000, 40'000'000);
    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD video = 0;
    hr = writer->AddStream(out.Get(), &video);
    if (FAILED(hr)) return hr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = writer->SetInputMediaType(video, in.Get(), nullptr);
    if (FAILED(hr)) return hr;

    m_hasAudio = false;
    if (microphone)
    {
        const HRESULT audio = AddAudio(writer.Get());
        if (FAILED(audio))
        {
            std::lock_guard lk(m_problemLock);
            m_audioProblem = audio == E_ACCESSDENIED
                ? L"Không ghi được tiếng: Windows đang chặn micro (Cài đặt › Quyền riêng tư › Micro)."
                : L"Không ghi được tiếng: không mở được micro.";
        }
    }
    hr = writer->BeginWriting();
    if (FAILED(hr))
    {
        m_mic.Reset();
        m_micSource.Reset();
        return hr;
    }

    std::lock_guard lk(m_lock);
    m_writer = writer;
    m_videoStream = video;
    m_width = width;
    m_height = height;
    m_frameInterval = 10'000'000 / fps;
    m_lastVideo = -1;
    QueryPerformanceFrequency(&m_freq);
    QueryPerformanceCounter(&m_start);
    m_active = true;
    if (m_mic)
    {
        m_micStop = false;
        m_micDone = false;
        m_micThread = std::thread([this] { ReadMicrophone(); });
    }
    return S_OK;
}

// PCM from the microphone in, AAC out. Only rates and channel counts the AAC encoder takes.
HRESULT Recorder::AddAudio(IMFSinkWriter* writer)
{
    ComPtr<IMFMediaSource> source;
    HRESULT hr = DefaultMicrophone(source);
    if (FAILED(hr)) return hr;
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaType> pcm;
    MFCreateMediaType(&pcm);
    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pcm.Get());
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaType> actual;
    hr = reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &actual);
    if (FAILED(hr)) return hr;
    const UINT32 rate = MFGetAttributeUINT32(actual.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    const UINT32 channels = MFGetAttributeUINT32(actual.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
    if ((rate != 44100 && rate != 48000) || (channels != 1 && channels != 2)) return MF_E_INVALIDMEDIATYPE;

    ComPtr<IMFMediaType> aac;
    MFCreateMediaType(&aac);
    aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 20000); // 160 kbit/s
    aac->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    DWORD stream = 0;
    hr = writer->AddStream(aac.Get(), &stream);
    if (FAILED(hr)) return hr;
    hr = writer->SetInputMediaType(stream, actual.Get(), nullptr);
    if (FAILED(hr)) return hr;

    m_audioStream = stream;
    m_audioRate = rate;
    m_audioBlock = MFGetAttributeUINT32(actual.Get(), MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    m_micSource = source;
    m_mic = reader;
    m_hasAudio = true;
    return S_OK;
}

// The microphone's thread: its samples are timed by how many there were (no drift, no gaps),
// from when the first one arrived.
void Recorder::ReadMicrophone()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LONGLONG offset = -1;
    uint64_t frames = 0;
    while (!m_micStop)
    {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = m_mic->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
        {
            std::lock_guard lk(m_problemLock);
            m_audioProblem = L"Micro ngừng giữa chừng: phần sau không có tiếng.";
            break;
        }
        if (!sample) continue;
        DWORD bytes = 0;
        sample->GetTotalLength(&bytes);
        const uint64_t count = m_audioBlock ? bytes / m_audioBlock : 0;
        if (!count) continue;
        if (offset < 0) offset = Now100ns();
        const LONGLONG time = offset + static_cast<LONGLONG>(frames * 10'000'000 / m_audioRate);
        frames += count;
        sample->SetSampleTime(time);
        sample->SetSampleDuration(static_cast<LONGLONG>(count * 10'000'000 / m_audioRate));
        std::lock_guard lk(m_lock);
        if (m_writer) m_writer->WriteSample(m_audioStream, sample.Get());
    }
    m_micDone = true;
    CoUninitialize();
}

void Recorder::Push(const uint8_t* nv12, long pitch, uint32_t w, uint32_t h)
{
    if (!m_active) return;
    std::lock_guard lk(m_lock);
    if (!m_writer) return;
    const LONGLONG now = Now100ns();
    // A source faster than the recording (a 100 fps webcam): every other frame or so.
    if (m_lastVideo >= 0 && now - m_lastVideo < m_frameInterval * 8 / 10) return;

    const DWORD size = static_cast<DWORD>(CamStudioNv12Bytes(m_width, m_height));
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(size, &buffer))) return;
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) return;
    if (w == m_width && h == m_height) Nv12Scale(nv12, pitch, w, h, dst, static_cast<long>(m_width), m_width, m_height);
    else Nv12FitInto(nv12, pitch, w, h, dst, static_cast<long>(m_width), m_width, m_height); // another source's shape
    buffer->Unlock();
    buffer->SetCurrentLength(size);
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(now);
    sample->SetSampleDuration(m_frameInterval);
    if (SUCCEEDED(m_writer->WriteSample(m_videoStream, sample.Get()))) m_lastVideo = now;
}

HRESULT Recorder::Stop()
{
    if (!m_active && !m_writer) return S_FALSE;
    m_active = false;
    m_micStop = true;
    if (m_micThread.joinable())
    {
        // A sample comes every ~10 ms, so the loop sees the stop at once. A microphone that went
        // silent (unplugged) is shut down; one that still hangs is left behind, the file finishes.
        auto waitDone = [this](int ms) {
            for (int i = 0; i < ms / 10 && !m_micDone; ++i) Sleep(10);
            return m_micDone.load();
        };
        if (!waitDone(1000) && m_micSource) m_micSource->Shutdown();
        if (waitDone(1000)) m_micThread.join();
        else m_micThread.detach();
    }
    if (m_micDone)
    {
        m_mic.Reset();
        if (m_micSource) m_micSource->Shutdown();
        m_micSource.Reset();
    }
    ComPtr<IMFSinkWriter> writer;
    {
        std::lock_guard lk(m_lock);
        writer.Swap(m_writer);
    }
    return writer ? writer->Finalize() : S_FALSE;
}
