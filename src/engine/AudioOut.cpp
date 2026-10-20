#include "AudioOut.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr uint32_t kTargetMs = 60;  // what the buffer aims to hold
constexpr uint32_t kMaxMs = 150;    // more than this (clock drift, a burst): drop back to the target

// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), here so no GUIDs need defining.
const PROPERTYKEY kFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

void WriteWavHeader(FILE* f, uint32_t rate, uint32_t channels, uint32_t dataBytes)
{
    const uint32_t byteRate = rate * channels * 2, riff = 36 + dataBytes;
    const uint16_t format = 1, ch = static_cast<uint16_t>(channels), block = static_cast<uint16_t>(channels * 2), bits = 16;
    const uint32_t fmtSize = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtSize, 4, 1, f);
    fwrite(&format, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&block, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
}
} // namespace

AudioOut::AudioOut(std::wstring target) : m_target(std::move(target))
{
    if (m_target.rfind(L"wav:", 0) == 0) return; // written in Push, no thread
    m_thread = std::thread(&AudioOut::Run, this);
}

AudioOut::~AudioOut()
{
    {
        std::lock_guard lk(m_lock);
        m_quit = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_wav)
    {
        WriteWavHeader(m_wav, m_rate, m_channels, m_wavBytes);
        fclose(m_wav);
    }
}

void AudioOut::SetProblem(const std::wstring& p)
{
    std::lock_guard lk(m_lock);
    m_problem = p;
}

std::wstring AudioOut::Problem() const
{
    std::lock_guard lk(m_lock);
    return m_problem;
}

bool AudioOut::Receiving() const
{
    std::lock_guard lk(m_lock);
    return m_lastPush && GetTickCount64() - m_lastPush < 1000;
}

void AudioOut::Push(const uint8_t* pcm, size_t bytes, uint32_t rate, uint32_t channels)
{
    if (!rate || !channels || bytes < 2) return;
    const size_t samples = bytes / 2;
    std::lock_guard lk(m_lock);
    m_lastPush = GetTickCount64();
    if (m_target.rfind(L"wav:", 0) == 0)
    {
        if (!m_wav)
        {
            if (_wfopen_s(&m_wav, m_target.c_str() + 4, L"wb") != 0) m_wav = nullptr;
            if (m_wav) WriteWavHeader(m_wav, rate, channels, 0);
        }
        m_rate = rate;
        m_channels = channels;
        if (m_wav)
        {
            fseek(m_wav, 0, SEEK_END);
            fwrite(pcm, 2, samples, m_wav);
            m_wavBytes += static_cast<uint32_t>(samples * 2);
        }
        return;
    }
    if (rate != m_rate || channels != m_channels)
    {
        m_rate = rate;
        m_channels = channels;
        m_ring.clear(); // the render thread reopens the device in this format
    }
    const int16_t* s = reinterpret_cast<const int16_t*>(pcm);
    m_ring.insert(m_ring.end(), s, s + samples);
    // Too far behind (the device runs slower than the phone, or a burst after a stall).
    const size_t max = static_cast<size_t>(rate) * channels * kMaxMs / 1000;
    const size_t target = static_cast<size_t>(rate) * channels * kTargetMs / 1000;
    if (m_ring.size() > max) m_ring.erase(m_ring.begin(), m_ring.begin() + (m_ring.size() - target) / channels * channels);
    m_wake.notify_all();
}

void AudioOut::Run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    UINT32 bufferFrames = 0;
    uint32_t openRate = 0, openChannels = 0;
    bool primed = false; // the buffer reached its target since (re)opening or an underrun

    for (;;)
    {
        uint32_t rate = 0, channels = 0;
        {
            std::unique_lock lk(m_lock);
            if (!client) m_wake.wait_for(lk, std::chrono::milliseconds(200), [this] { return m_quit || m_rate; });
            if (m_quit) break;
            rate = m_rate;
            channels = m_channels;
        }
        if (!rate) continue;

        if (!client || rate != openRate || channels != openChannels)
        {
            if (client) client->Stop();
            client.Reset();
            render.Reset();
            openRate = openChannels = 0;
            ComPtr<IMMDeviceEnumerator> devices;
            ComPtr<IMMDevice> device;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices));
            if (SUCCEEDED(hr)) hr = devices->GetDevice(m_target.c_str(), &device);
            if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = static_cast<WORD>(channels);
            format.nSamplesPerSec = rate;
            format.wBitsPerSample = 16;
            format.nBlockAlign = static_cast<WORD>(channels * 2);
            format.nAvgBytesPerSec = rate * format.nBlockAlign;
            // Windows converts to the device's own format (rate, channels, float).
            if (SUCCEEDED(hr))
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    50 * 10000, 0, &format, nullptr);
            if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
            if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
            if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
            if (SUCCEEDED(hr)) hr = client->Start();
            if (FAILED(hr))
            {
                client.Reset();
                render.Reset();
                SetProblem(hr == static_cast<HRESULT>(0x80070490) /* not found */ || hr == E_NOTFOUND
                    ? L"Không thấy thiết bị phát đã chọn (đã gỡ VB-Audio Cable?)"
                    : L"Không mở được thiết bị phát tiếng");
                std::unique_lock lk(m_lock);
                m_wake.wait_for(lk, std::chrono::seconds(2), [this] { return m_quit; });
                continue;
            }
            SetProblem(L"");
            openRate = rate;
            openChannels = channels;
            primed = false;
        }

        WaitForSingleObject(event, 100);
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding)))
        {
            client.Reset(); // the device went away: reopen
            continue;
        }
        const UINT32 free = bufferFrames > padding ? bufferFrames - padding : 0;
        if (!free) continue;
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(free, &data))) continue;
        UINT32 filled = 0;
        {
            std::lock_guard lk(m_lock);
            const size_t have = m_ring.size() / channels;
            if (!primed && have >= rate * kTargetMs / 1000) primed = true;
            if (primed)
            {
                filled = static_cast<UINT32>(std::min<size_t>(free, have));
                int16_t* out = reinterpret_cast<int16_t*>(data);
                std::copy_n(m_ring.begin(), static_cast<size_t>(filled) * channels, out);
                m_ring.erase(m_ring.begin(), m_ring.begin() + static_cast<size_t>(filled) * channels);
                if (filled < free) primed = false; // ran dry: wait for the buffer again
            }
        }
        if (filled < free) memset(data + static_cast<size_t>(filled) * channels * 2, 0, static_cast<size_t>(free - filled) * channels * 2);
        render->ReleaseBuffer(free, 0);
    }
    if (client) client->Stop();
    render.Reset();
    client.Reset();
    CloseHandle(event);
    CoUninitialize();
}

std::vector<std::pair<std::wstring, std::wstring>> AudioOut::Outputs()
{
    std::vector<std::pair<std::wstring, std::wstring>> out;
    ComPtr<IMMDeviceEnumerator> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices)))) return out;
    ComPtr<IMMDeviceCollection> list;
    if (FAILED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) return out;
    UINT count = 0;
    list->GetCount(&count);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        if (FAILED(list->Item(i, &device))) continue;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) continue;
        std::wstring name;
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)))
        {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
            PropVariantClear(&v);
        }
        out.emplace_back(id, name);
        CoTaskMemFree(id);
    }
    return out;
}
