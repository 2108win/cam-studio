// Face detection for automatic framing: Windows.Media.FaceAnalysis.FaceTracker on its own
// thread, fed a small grey copy of the picture a few times a second. Nothing runs (no thread,
// no tracker) until the first frame is offered, so framing turned off costs nothing.
#pragma once

#include "AutoFrame.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class FaceDetector
{
public:
    FaceDetector() = default;
    ~FaceDetector();
    FaceDetector(const FaceDetector&) = delete;
    FaceDetector& operator=(const FaceDetector&) = delete;

    // Capture thread. A frame is worth offering now: the last one is done and kIntervalMs passed.
    bool Due(double nowMs);
    // Copies a small grey version of this luma plane for the detector. generation tags the
    // result: TakeResult drops results of another generation (a frame from before a rotation).
    void Offer(const uint8_t* luma, long pitch, uint32_t width, uint32_t height, uint32_t generation, double nowMs);
    // The newest finished detection of this generation, once.
    bool TakeResult(uint32_t generation, std::vector<FaceRect>& faces);
    // False once the tracker could not run (e.g. Windows N without the Media Feature Pack).
    bool Available() const { return !m_failed; }

    static constexpr double kIntervalMs = 200;
    static constexpr uint32_t kMaxSide = 640; // long side of the grey copy

private:
    void Worker();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    bool m_quit = false;
    bool m_busy = false; // a frame waits or is being looked at
    std::atomic<bool> m_failed{ false };
    double m_lastOfferMs = -1e9;

    std::vector<uint8_t> m_scratch; // capture thread: the grey copy being made
    std::vector<uint8_t> m_job;     // handed to the worker
    uint32_t m_jobWidth = 0, m_jobHeight = 0, m_jobGeneration = 0;

    std::vector<FaceRect> m_result;
    uint32_t m_resultGeneration = 0;
    bool m_hasResult = false;
};
