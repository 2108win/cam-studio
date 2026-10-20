// "Micro điện thoại": the phone's microphone (AUDIO records, docs/protocol.md 5.1) played into an
// output device, typically VB-Audio's "CABLE Input", so a meeting app picks "CABLE Output" as its
// microphone. WASAPI shared mode on its own thread, a ~60 ms jitter buffer trimmed when it grows.
// "wav:<path>" as the target writes the sound to a WAV file instead (tests, no sound made).
#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class AudioOut
{
public:
    explicit AudioOut(std::wstring target);
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // 16-bit PCM, interleaved. A new rate or channel count reopens the device.
    void Push(const uint8_t* pcm, size_t bytes, uint32_t rate, uint32_t channels);
    // Why nothing plays ("" when it does).
    std::wstring Problem() const;
    // Sound arrived in the last second.
    bool Receiving() const;

    // Active output devices: (endpoint id, name).
    static std::vector<std::pair<std::wstring, std::wstring>> Outputs();

private:
    void Run();
    void SetProblem(const std::wstring& p);

    const std::wstring m_target;
    mutable std::mutex m_lock;
    std::condition_variable m_wake;
    std::deque<int16_t> m_ring;
    uint32_t m_rate = 0, m_channels = 1;
    bool m_quit = false;
    std::wstring m_problem;
    ULONGLONG m_lastPush = 0;
    std::thread m_thread;

    // "wav:" target
    FILE* m_wav = nullptr;
    uint32_t m_wavBytes = 0;
};
