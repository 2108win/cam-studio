// A window on this PC as the virtual camera's source (like OBS "Window Capture"), through
// Windows.Graphics.Capture. Plain C++ here; every C++/WinRT include stays in WindowSource.cpp.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct WindowInfo
{
    HWND hwnd = nullptr;
    std::wstring title;
};

// Top-level windows worth offering: visible, titled, not cloaked (another virtual desktop, a
// suspended Store app), not tool windows, and not Cam Studio's own.
std::vector<WindowInfo> ListCaptureWindows();

// The window's size on screen without its shadow (what capture frames will be), even.
void CaptureWindowSize(HWND window, uint32_t& width, uint32_t& height);

class WindowCapture
{
public:
    // Error: capture failed for now (device lost, a transient WinRT error): reopen and go on.
    enum class Result { Frame, Timeout, Closed, Error };
    // BGRA rows, top-down, valid only during the call.
    using FrameCallback = std::function<void(const uint8_t* bgra, uint32_t pitch, uint32_t width, uint32_t height)>;

    WindowCapture();
    ~WindowCapture();
    WindowCapture(const WindowCapture&) = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;

    // Starts capturing. False, with a message for the user, if Windows cannot capture it.
    // Call on a thread with COM (MTA) initialised.
    bool Open(HWND window, std::wstring& error);
    // Waits up to timeoutMs for a new picture (a window sends one only when it changes). The
    // newest frame is read back from the GPU and passed to onFrame only when `read` is true
    // (nobody watching: released unread).
    Result Next(int timeoutMs, bool read, const FrameCallback& onFrame);
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
