// "Nền" (background effects): finds the person in the picture with a small segmentation model
// (PP-HumanSeg, OpenCV Zoo, Apache 2.0) through Windows ML, on its own thread, ~15 times a
// second; the capture thread blends every frame with the latest mask (BlendBackground).
// Nothing runs (no thread, no model) until the first frame is offered: off costs nothing.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Segmenter
{
public:
    Segmenter() = default;
    ~Segmenter();
    Segmenter(const Segmenter&) = delete;
    Segmenter& operator=(const Segmenter&) = delete;

    static constexpr uint32_t kSize = 192;     // the model's input, square
    static constexpr double kIntervalMs = 66;  // ~15 a second

    void SetModel(const std::wstring& path);
    // Capture thread: the last job is done and kIntervalMs passed.
    bool Due(double nowMs);
    // A kSize x kSize RGB sample of this NV12 picture (planes y / uv, pitch bytes a row).
    void Offer(const uint8_t* y, const uint8_t* uv, long pitch, uint32_t w, uint32_t h, double nowMs);
    // The newest mask (kSize x kSize, 0 background .. 255 person, smoothed over time), when it
    // changed since the last call.
    bool TakeMask(std::vector<uint8_t>& mask);
    // Starts over (another source: the old mask is someone else).
    void Reset();
    // 1 works or not tried yet; 0 Windows ML or the model is missing; -1 it failed and gave up.
    int State() const { return m_unsupported ? 0 : m_gaveUp ? -1 : 1; }
    double InferMs() const { return m_inferMs; }

private:
    void Worker();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    bool m_quit = false;
    bool m_busy = false;
    std::wstring m_model;
    std::atomic<bool> m_unsupported{ false };
    std::atomic<bool> m_gaveUp{ false };
    std::atomic<double> m_inferMs{ 0 };
    double m_lastOfferMs = -1e9;

    std::vector<float> m_job;      // 1 x 3 x kSize x kSize, normalised RGB
    std::vector<float> m_smooth;   // the person probability, averaged over time
    std::vector<uint8_t> m_result;
    bool m_hasResult = false;
    bool m_reset = false;
};

// Blends the background of an NV12 picture region (planes y / uv, pitch) with a blurred copy of
// itself (strength 0..100) or with `image` (NV12 of the same w x h), using mask (Segmenter::kSize
// square, 255 = person). Rows run in parallel.
struct BackgroundScratch
{
    std::vector<uint8_t> tiny;   // the box-averaged picture the blur is made from
    std::vector<uint8_t> tinyTmp;
    std::vector<uint8_t> alpha;  // the mask at half the picture's size
    uint32_t alphaW = 0, alphaH = 0;
};
void BlendBackground(uint8_t* y, uint8_t* uv, long pitch, uint32_t w, uint32_t h, const std::vector<uint8_t>& mask,
                     bool maskChanged, int strength, const uint8_t* image, BackgroundScratch& scratch);
