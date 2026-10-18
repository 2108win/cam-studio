// A phone's shared screen (docs/protocol.md 1.1) shown in a window of its own, so a meeting app
// can share exactly that window. Not the virtual camera: its own thread, the screen's link (port
// 47322) and decoder, the picture turned upright from "state", and BGRA at the window's size.
// It runs alongside the virtual camera and does not follow the main window's standby.
#pragma once

#include "Discovery.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace phone { class PhoneLink; }

class ScreenViewer
{
public:
    enum class State : int32_t
    {
        Connecting = 1,
        Showing = 2,
        Waiting = 3, // lost or not reachable (error says why); tried again every 2 s
    };

    ScreenViewer(std::shared_ptr<phone::PhoneLink> link, const phone::Endpoint& endpoint);
    ~ScreenViewer(); // stops the phone's stream; the link stays connected for the connector
    ScreenViewer(const ScreenViewer&) = delete;
    ScreenViewer& operator=(const ScreenViewer&) = delete;

    // The window's picture area in pixels. 0 x 0 (minimized): frames are decoded, not drawn.
    void SetSize(uint32_t maxWidth, uint32_t maxHeight);

    // Latest picture (BGRA, top-down), at most the window's size with the screen's aspect.
    // Always reports its size and id; copies only when capacity (pixels) is enough.
    bool Copy(uint32_t* dst, size_t capacity, uint32_t& width, uint32_t& height, uint64_t& frameId) const;

    // The screen upright at its own size (0 x 0 until the first frame), and the state.
    State GetState(uint32_t& screenWidth, uint32_t& screenHeight, std::wstring& error) const;

private:
    void Run();
    void Stream(std::wstring& why);
    void Show(const uint8_t* nv12, long pitch, uint32_t width, uint32_t height);
    void SetState(State state, const std::wstring& error);

    std::shared_ptr<phone::PhoneLink> m_link;
    phone::Endpoint m_endpoint;
    std::thread m_thread;
    std::atomic<bool> m_quit{ false };
    std::atomic<bool> m_connecting{ false }; // in a connect: the destructor aborts it
    std::atomic<uint32_t> m_maxW{ 0 }, m_maxH{ 0 };
    std::atomic<int> m_rotation{ 0 };        // from the phone's "started"/"state"

    // Viewer thread only.
    std::vector<uint8_t> m_oriented, m_scaled;
    std::vector<uint32_t> m_work;
    uint64_t m_lastShown = 0;

    mutable std::mutex m_lock;
    State m_state = State::Connecting;
    std::wstring m_error;
    std::vector<uint32_t> m_pixels;
    uint32_t m_width = 0, m_height = 0;
    uint32_t m_screenW = 0, m_screenH = 0;
    uint64_t m_frameId = 0;
};
