#include "StillPicture.h"

#include "../common/Nv12.h"
#include "../common/SharedFrame.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace still
{
namespace
{
// A strong blur: average blocks of `block` pixels, then scale back up smoothly. Darkened, so
// white text reads on any picture.
void BlurNv12(const uint8_t* src, uint32_t w, uint32_t h, uint32_t block, std::vector<uint8_t>& out)
{
    const uint32_t sw = std::max(2u, (w / block) & ~1u), sh = std::max(2u, (h / block) & ~1u);
    std::vector<uint8_t> tiny(CamStudioNv12Bytes(sw, sh));
    const uint8_t* srcUv = src + static_cast<size_t>(w) * h;
    uint8_t* tinyUv = tiny.data() + static_cast<size_t>(sw) * sh;
    for (uint32_t y = 0; y < sh; ++y)
        for (uint32_t x = 0; x < sw; ++x)
        {
            const uint32_t x0 = x * w / sw, x1 = std::max(x0 + 1, (x + 1) * w / sw);
            const uint32_t y0 = y * h / sh, y1 = std::max(y0 + 1, (y + 1) * h / sh);
            uint64_t sum = 0;
            for (uint32_t yy = y0; yy < y1; ++yy)
                for (uint32_t xx = x0; xx < x1; ++xx) sum += src[static_cast<size_t>(yy) * w + xx];
            const uint64_t n = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
            // Darker (16 is black): 60 % of the way from black.
            tiny[static_cast<size_t>(y) * sw + x] = static_cast<uint8_t>(16 + (sum / n - 16) * 6 / 10);
        }
    for (uint32_t y = 0; y < sh / 2; ++y)
        for (uint32_t x = 0; x < sw / 2; ++x)
        {
            const uint32_t x0 = x * (w / 2) / (sw / 2), x1 = std::max(x0 + 1, (x + 1) * (w / 2) / (sw / 2));
            const uint32_t y0 = y * (h / 2) / (sh / 2), y1 = std::max(y0 + 1, (y + 1) * (h / 2) / (sh / 2));
            uint64_t su = 0, sv = 0;
            for (uint32_t yy = y0; yy < y1; ++yy)
                for (uint32_t xx = x0; xx < x1; ++xx)
                {
                    su += srcUv[static_cast<size_t>(yy) * w + 2 * xx];
                    sv += srcUv[static_cast<size_t>(yy) * w + 2 * xx + 1];
                }
            const uint64_t n = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
            tinyUv[static_cast<size_t>(y) * sw + 2 * x] = static_cast<uint8_t>(su / n);
            tinyUv[static_cast<size_t>(y) * sw + 2 * x + 1] = static_cast<uint8_t>(sv / n);
        }
    out.resize(CamStudioNv12Bytes(w, h));
    const Nv12Rect all{ 0, 0, static_cast<double>(sw), static_cast<double>(sh) };
    Nv12CropScaleBilinear(tiny.data(), static_cast<long>(sw), sw, sh, all, out.data(), w, h);
}

struct Canvas
{
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<ID2D1RenderTarget> target;

    bool Create(uint32_t w, uint32_t h)
    {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
        if (FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return false;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        return SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target));
    }

    // The bitmap's pixels, as NV12.
    bool ToNv12(uint32_t w, uint32_t h, std::vector<uint8_t>& nv12)
    {
        WICRect all{ 0, 0, static_cast<INT>(w), static_cast<INT>(h) };
        ComPtr<IWICBitmapLock> lock;
        if (FAILED(bitmap->Lock(&all, WICBitmapLockRead, &lock))) return false;
        UINT stride = 0, size = 0;
        BYTE* data = nullptr;
        lock->GetStride(&stride);
        if (FAILED(lock->GetDataPointer(&size, &data)) || !data) return false;
        nv12.resize(CamStudioNv12Bytes(w, h));
        BgraToNv12(data, stride, w, h, nv12.data(), nv12.data() + static_cast<size_t>(w) * h, w);
        return true;
    }
};
} // namespace

bool RenderCard(const std::wstring& text, uint32_t w, uint32_t h, const uint8_t* background, uint32_t bw, uint32_t bh,
                std::vector<uint8_t>& nv12)
{
    if (w < 2 || h < 2) return false;
    Canvas c;
    if (!c.Create(w, h)) return false;

    // The background: the last picture blurred (copied into the bitmap first), else a dark gradient.
    if (background && bw >= 2 && bh >= 2)
    {
        std::vector<uint8_t> fitted(CamStudioNv12Bytes(w, h));
        Nv12Scale(background, static_cast<long>(bw), bw, bh, fitted.data(), static_cast<long>(w), w, h);
        std::vector<uint8_t> blurred;
        BlurNv12(fitted.data(), w, h, std::max(16u, w / 40), blurred);
        WICRect all{ 0, 0, static_cast<INT>(w), static_cast<INT>(h) };
        ComPtr<IWICBitmapLock> lock;
        if (SUCCEEDED(c.bitmap->Lock(&all, WICBitmapLockWrite, &lock)))
        {
            UINT stride = 0, size = 0;
            BYTE* data = nullptr;
            lock->GetStride(&stride);
            if (SUCCEEDED(lock->GetDataPointer(&size, &data)) && data)
            {
                std::vector<uint32_t> bgra(static_cast<size_t>(w) * h);
                Nv12ToBgra(blurred.data(), blurred.data() + static_cast<size_t>(w) * h, static_cast<long>(w), w, h, bgra.data(), w, h);
                for (uint32_t y = 0; y < h; ++y) memcpy(data + static_cast<size_t>(y) * stride, bgra.data() + static_cast<size_t>(y) * w, w * 4);
            }
        }
        else background = nullptr;
    }

    ID2D1RenderTarget* t = c.target.Get();
    t->BeginDraw();
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    if (!background)
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        const D2D1_GRADIENT_STOP s[2] = { { 0.f, D2D1::ColorF(0x23262E) }, { 1.f, D2D1::ColorF(0x111216) } };
        t->CreateGradientStopCollection(s, 2, &stops);
        ComPtr<ID2D1LinearGradientBrush> fill;
        t->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(fw, fh)), stops.Get(), &fill);
        t->FillRectangle(D2D1::RectF(0, 0, fw, fh), fill.Get());
    }

    // A pause sign (a ring with two bars) above the text, sized from the frame's shorter side.
    const float unit = std::min(fw, fh) / 1080.f;
    ComPtr<ID2D1SolidColorBrush> white, soft;
    t->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.95f), &white);
    t->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.16f), &soft);
    const float cx = fw / 2, cy = fh / 2 - 70 * unit, r = 64 * unit;
    t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), soft.Get());
    const float barW = 14 * unit, barH = 50 * unit, gap = 12 * unit;
    t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - gap / 2 - barW, cy - barH / 2, cx - gap / 2, cy + barH / 2), 4 * unit, 4 * unit), white.Get());
    t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx + gap / 2, cy - barH / 2, cx + gap / 2 + barW, cy + barH / 2), 4 * unit, 4 * unit), white.Get());

    ComPtr<IDWriteFactory> dw;
    if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
    {
        // Segoe UI Variable on Windows 11; DirectWrite falls back to Segoe UI on Windows 10.
        ComPtr<IDWriteTextFormat> format;
        if (SUCCEEDED(dw->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, 64 * unit, L"vi-vn", &format)))
        {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            const float top = cy + r + 36 * unit;
            t->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
                        D2D1::RectF(fw * 0.08f, top, fw * 0.92f, fh), white.Get());
        }
    }
    if (FAILED(t->EndDraw())) return false;
    return c.ToNv12(w, h, nv12);
}

bool LoadImageFill(const std::wstring& path, uint32_t w, uint32_t h, std::vector<uint8_t>& nv12)
{
    if (w < 2 || h < 2 || path.empty()) return false;
    Canvas c;
    if (!c.Create(w, h)) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converted;
    if (FAILED(c.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(c.wic->CreateFormatConverter(&converted)) ||
        FAILED(converted->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    ComPtr<ID2D1Bitmap> image;
    if (FAILED(c.target->CreateBitmapFromWicBitmap(converted.Get(), &image))) return false;
    const D2D1_SIZE_F size = image->GetSize();
    if (size.width < 1 || size.height < 1) return false;
    // Fill: the image's shorter side meets the frame, the rest is cropped evenly.
    const float scale = std::max(w / size.width, h / size.height);
    const float dw = size.width * scale, dh = size.height * scale;
    const float x = (w - dw) / 2, y = (h - dh) / 2;
    c.target->BeginDraw();
    c.target->Clear(D2D1::ColorF(0x000000));
    c.target->DrawBitmap(image.Get(), D2D1::RectF(x, y, x + dw, y + dh), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (FAILED(c.target->EndDraw())) return false;
    return c.ToNv12(w, h, nv12);
}
} // namespace still
