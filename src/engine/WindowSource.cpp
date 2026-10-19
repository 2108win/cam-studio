// Windows.Graphics.Capture for one window: a free-threaded frame pool (no dispatcher on the
// capture thread), the newest frame copied into a staging texture and mapped for the CPU.
// C++/WinRT includes live here and in FaceDetect.cpp only.
#include "WindowSource.h"

#include <unknwn.h> // before the C++/WinRT headers: classic COM interop
#include <inspectable.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <atomic>

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "dwmapi")

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

std::vector<WindowInfo> ListCaptureWindows()
{
    std::vector<WindowInfo> out;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto& list = *reinterpret_cast<std::vector<WindowInfo>*>(param);
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || hwnd == GetShellWindow()) return TRUE;
        if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId()) return TRUE; // Cam Studio's own windows
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;
        const int length = GetWindowTextLengthW(hwnd);
        if (length <= 0) return TRUE;
        std::wstring title(static_cast<size_t>(length) + 1, L'\0');
        title.resize(static_cast<size_t>(GetWindowTextW(hwnd, title.data(), length + 1)));
        if (!title.empty()) list.push_back({ hwnd, title });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return out;
}

void CaptureWindowSize(HWND window, uint32_t& width, uint32_t& height)
{
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(window, &r);
    width = std::max<uint32_t>(2, static_cast<uint32_t>(std::max<LONG>(0, r.right - r.left)) & ~1u);
    height = std::max<uint32_t>(2, static_cast<uint32_t>(std::max<LONG>(0, r.bottom - r.top)) & ~1u);
}

// What the capture callbacks touch. They run on the thread pool and may still be running when
// Close() revokes them, so they share this instead of pointing into Impl.
struct CaptureSignal
{
    HANDLE frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> closed{ false };
    ~CaptureSignal()
    {
        if (frameEvent) CloseHandle(frameEvent);
    }
};

struct WindowCapture::Impl
{
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device{ nullptr };
    wgc::GraphicsCaptureItem item{ nullptr };
    wgc::Direct3D11CaptureFramePool pool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    winrt::event_token frameToken{};
    winrt::event_token closedToken{};
    std::shared_ptr<CaptureSignal> signal = std::make_shared<CaptureSignal>();
    winrt::Windows::Graphics::SizeInt32 poolSize{};
    winrt::com_ptr<ID3D11Texture2D> staging;
    uint32_t stagingW = 0, stagingH = 0;
};

WindowCapture::WindowCapture() = default;

WindowCapture::~WindowCapture()
{
    Close();
}

bool WindowCapture::Open(HWND window, std::wstring& error)
{
    Close();
    try
    {
        if (!wgc::GraphicsCaptureSession::IsSupported())
        {
            error = L"Windows trên máy này không quay được cửa sổ (cần Windows 10 1903 trở lên)";
            return false;
        }
        auto impl = std::make_unique<Impl>();
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, impl->d3d.put(), nullptr, impl->context.put());
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, impl->d3d.put(), nullptr, impl->context.put());
        if (FAILED(hr))
        {
            error = L"Không tạo được Direct3D để quay cửa sổ";
            return false;
        }
        const auto dxgi = impl->d3d.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        impl->device = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        const auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                      winrt::put_abi(impl->item)));

        impl->poolSize = impl->item.Size();
        impl->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl->device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, impl->poolSize);
        std::shared_ptr<CaptureSignal> signal = impl->signal;
        impl->frameToken = impl->pool.FrameArrived([signal](const auto&, const auto&) { SetEvent(signal->frameEvent); });
        impl->closedToken = impl->item.Closed([signal](const auto&, const auto&) {
            signal->closed = true;
            SetEvent(signal->frameEvent);
        });
        impl->session = impl->pool.CreateCaptureSession(impl->item);
        // No yellow border around the window where Windows allows it (Windows 11).
        using winrt::Windows::Foundation::Metadata::ApiInformation;
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired"))
        {
            try
            {
                wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
                impl->session.IsBorderRequired(false);
            }
            catch (const winrt::hresult_error&)
            {
                // The border stays; capture works the same.
            }
        }
        impl->session.StartCapture();
        m = std::move(impl);
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        error = L"Không quay được cửa sổ này (" + std::wstring(e.message()) + L")";
        return false;
    }
}

WindowCapture::Result WindowCapture::Next(int timeoutMs, bool read, const FrameCallback& onFrame)
{
    if (!m || m->signal->closed) return Result::Closed;
    const DWORD wait = WaitForSingleObject(m->signal->frameEvent, static_cast<DWORD>(timeoutMs));
    if (m->signal->closed) return Result::Closed;
    if (wait != WAIT_OBJECT_0) return Result::Timeout;
    try
    {
        // Only the newest frame matters: release older ones at once (the pool holds two).
        wgc::Direct3D11CaptureFrame frame{ nullptr };
        for (auto next = m->pool.TryGetNextFrame(); next; next = m->pool.TryGetNextFrame())
        {
            if (frame) frame.Close();
            frame = next;
        }
        if (!frame) return Result::Timeout;
        const auto size = frame.ContentSize();
        if (read && size.Width >= 2 && size.Height >= 2)
        {
            const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (!m->staging || m->stagingW != desc.Width || m->stagingH != desc.Height)
            {
                D3D11_TEXTURE2D_DESC s = desc;
                s.Usage = D3D11_USAGE_STAGING;
                s.BindFlags = 0;
                s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                s.MiscFlags = 0;
                s.MipLevels = 1;
                s.ArraySize = 1;
                s.SampleDesc = { 1, 0 };
                m->staging = nullptr;
                winrt::check_hresult(m->d3d->CreateTexture2D(&s, nullptr, m->staging.put()));
                m->stagingW = desc.Width;
                m->stagingH = desc.Height;
            }
            // The content may be smaller than the pool's texture (the window shrank).
            const uint32_t w = std::min<uint32_t>(static_cast<uint32_t>(size.Width), desc.Width);
            const uint32_t h = std::min<uint32_t>(static_cast<uint32_t>(size.Height), desc.Height);
            const D3D11_BOX box{ 0, 0, 0, w, h, 1 };
            m->context->CopySubresourceRegion(m->staging.get(), 0, 0, 0, 0, texture.get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(m->context->Map(m->staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
            {
                onFrame(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, w, h);
                m->context->Unmap(m->staging.get(), 0);
            }
        }
        frame.Close();
        if (size.Width >= 2 && size.Height >= 2 && (size.Width != m->poolSize.Width || size.Height != m->poolSize.Height))
        {
            // Resized: frames from now on at the window's new size.
            m->poolSize = size;
            m->pool.Recreate(m->device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        }
        return Result::Frame;
    }
    catch (const winrt::hresult_error&)
    {
        // Only the item's Closed event means the window is gone.
        return m->signal->closed ? Result::Closed : Result::Error;
    }
}

void WindowCapture::Close()
{
    if (!m) return;
    try
    {
        if (m->pool) m->pool.FrameArrived(m->frameToken);
        if (m->item) m->item.Closed(m->closedToken);
        if (m->session) m->session.Close();
        if (m->pool) m->pool.Close();
    }
    catch (const winrt::hresult_error&)
    {
    }
    m.reset(); // the signal lives on while a callback still holds it

}
