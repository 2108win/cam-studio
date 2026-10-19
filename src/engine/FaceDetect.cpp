// FaceTracker (Windows.Media.FaceAnalysis) for automatic framing. Its C++/WinRT includes stay in
// this file, like WindowSource.cpp's. The worker runs in the multithreaded apartment, where
// blocking on an async call with .get() is allowed.
#include "FaceDetect.h"

#include <unknwn.h> // before the C++/WinRT headers: classic COM interop

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.FaceAnalysis.h>

#include <cstring>

#pragma comment(lib, "windowsapp")

namespace
{
// Raw access to a locked SoftwareBitmap's pixels (documented interop interface).
struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable) IMemoryBufferByteAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

namespace imaging = winrt::Windows::Graphics::Imaging;
namespace faces = winrt::Windows::Media::FaceAnalysis;
} // namespace

FaceDetector::~FaceDetector()
{
    {
        std::lock_guard lk(m_mutex);
        m_quit = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

bool FaceDetector::Due(double nowMs)
{
    if (m_failed || nowMs - m_lastOfferMs < kIntervalMs) return false;
    std::lock_guard lk(m_mutex);
    return !m_busy;
}

void FaceDetector::Offer(const uint8_t* luma, long pitch, uint32_t width, uint32_t height, uint32_t generation, double nowMs)
{
    m_lastOfferMs = nowMs;
    uint32_t w = 0, h = 0;
    DetectionSize(width, height, kMaxSide, w, h);
    m_scratch.resize(static_cast<size_t>(w) * h);
    DownscaleLuma(luma, pitch, width, height, m_scratch.data(), w, h);
    {
        std::lock_guard lk(m_mutex);
        if (m_busy) return;
        m_job.swap(m_scratch);
        m_jobWidth = w;
        m_jobHeight = h;
        m_jobGeneration = generation;
        m_busy = true;
        if (!m_thread.joinable()) m_thread = std::thread(&FaceDetector::Worker, this);
    }
    m_wake.notify_one();
}

bool FaceDetector::TakeResult(uint32_t generation, std::vector<FaceRect>& out)
{
    std::lock_guard lk(m_mutex);
    if (!m_hasResult) return false;
    m_hasResult = false;
    if (m_resultGeneration != generation) return false;
    out.swap(m_result);
    return true;
}

void FaceDetector::Worker()
{
    bool apartment = false;
    faces::FaceTracker tracker{ nullptr };
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
        if (!faces::FaceTracker::IsSupported() ||
            !faces::FaceTracker::IsBitmapPixelFormatSupported(imaging::BitmapPixelFormat::Gray8))
            m_failed = true;
        else
            tracker = faces::FaceTracker::CreateAsync().get();
    }
    catch (...)
    {
        m_failed = true;
    }

    std::vector<uint8_t> pixels;
    for (;;)
    {
        uint32_t w = 0, h = 0, generation = 0;
        {
            std::unique_lock lk(m_mutex);
            m_wake.wait(lk, [this] { return m_quit || m_busy; });
            if (m_quit) break;
            pixels.swap(m_job);
            w = m_jobWidth;
            h = m_jobHeight;
            generation = m_jobGeneration;
        }

        std::vector<FaceRect> found;
        bool ok = false;
        if (!m_failed)
        {
            try
            {
                imaging::SoftwareBitmap bitmap(imaging::BitmapPixelFormat::Gray8, static_cast<int32_t>(w), static_cast<int32_t>(h));
                {
                    imaging::BitmapBuffer buffer = bitmap.LockBuffer(imaging::BitmapBufferAccessMode::Write);
                    const imaging::BitmapPlaneDescription plane = buffer.GetPlaneDescription(0);
                    winrt::Windows::Foundation::IMemoryBufferReference reference = buffer.CreateReference();
                    uint8_t* data = nullptr;
                    uint32_t capacity = 0;
                    winrt::check_hresult(reference.as<IMemoryBufferByteAccess>()->GetBuffer(&data, &capacity));
                    for (uint32_t y = 0; y < h; ++y)
                    {
                        const size_t at = static_cast<size_t>(plane.StartIndex) + static_cast<size_t>(y) * plane.Stride;
                        if (at + w > capacity) break;
                        memcpy(data + at, pixels.data() + static_cast<size_t>(y) * w, w);
                    }
                    reference.Close();
                    buffer.Close();
                }
                const winrt::Windows::Media::VideoFrame frame = winrt::Windows::Media::VideoFrame::CreateWithSoftwareBitmap(bitmap);
                for (const faces::DetectedFace& face : tracker.ProcessNextFrameAsync(frame).get())
                {
                    const imaging::BitmapBounds b = face.FaceBox();
                    found.push_back({ static_cast<double>(b.X) / w, static_cast<double>(b.Y) / h,
                                      static_cast<double>(b.Width) / w, static_cast<double>(b.Height) / h });
                }
                frame.Close();
                ok = true;
            }
            catch (...)
            {
                m_failed = true; // framing then reports itself unavailable
            }
        }

        std::lock_guard lk(m_mutex);
        m_busy = false;
        if (ok)
        {
            m_result.swap(found);
            m_resultGeneration = generation;
            m_hasResult = true;
        }
    }
    tracker = nullptr;
    if (apartment) winrt::uninit_apartment();
}
