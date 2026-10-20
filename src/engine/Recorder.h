// "Ghi hình": what the virtual camera shows, to an MP4 (H.264, the GPU's encoder when there is
// one), with the default microphone (AAC) when asked. Nothing exists until Start; frames come
// from the capture thread (Push), the microphone is read on its own thread.
#pragma once

#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Recorder
{
public:
    ~Recorder() { Stop(); }

    // width x height (even) at up to fps frames a second. microphone: also the default microphone;
    // without one (none, or blocked in Windows' privacy settings) the video is recorded alone.
    HRESULT Start(const std::wstring& path, uint32_t width, uint32_t height, uint32_t fps, bool microphone);
    // One frame of the camera (NV12, any size: fitted into the recording's, bars around). Frames
    // closer together than the frame rate allows are skipped. Any thread.
    void Push(const uint8_t* nv12, long pitch, uint32_t w, uint32_t h);
    // Finishes the file (it is playable only after this).
    HRESULT Stop();

    bool Active() const { return m_active; }
    int64_t ElapsedMs() const;
    bool HasAudio() const { return m_hasAudio; }
    std::wstring AudioProblem() const;

private:
    HRESULT AddAudio(IMFSinkWriter* writer);
    void ReadMicrophone();
    LONGLONG Now100ns() const; // since Start

    std::mutex m_lock; // the sink writer, from the capture and microphone threads
    Microsoft::WRL::ComPtr<IMFSinkWriter> m_writer;
    DWORD m_videoStream = 0;
    DWORD m_audioStream = 0;
    uint32_t m_width = 0, m_height = 0;
    LONGLONG m_frameInterval = 333333; // 100 ns
    LONGLONG m_lastVideo = -1;
    std::vector<uint8_t> m_fit;        // a frame of another size, fitted
    LARGE_INTEGER m_start{};
    LARGE_INTEGER m_freq{};
    std::atomic<bool> m_active{ false };
    std::atomic<bool> m_hasAudio{ false };

    Microsoft::WRL::ComPtr<IMFMediaSource> m_micSource;
    Microsoft::WRL::ComPtr<IMFSourceReader> m_mic;
    uint32_t m_audioRate = 0, m_audioBlock = 0; // samples a second, bytes per sample frame
    std::thread m_micThread;
    std::atomic<bool> m_micStop{ false };
    std::atomic<bool> m_micDone{ true }; // the microphone's thread has left its loop
    mutable std::mutex m_problemLock;
    std::wstring m_audioProblem;
};
