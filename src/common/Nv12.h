// NV12 copy/scale helpers shared by the host and the media source.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include <emmintrin.h> // SSE2, always available on x64

// Brightness / contrast / saturation / colour temperature, each -100..100.
struct Nv12ColorAdjust
{
    int brightness = 0;
    int contrast = 0;
    int saturation = 0;
    int temperature = 0; // + warmer (more red), - cooler (more blue)

    bool IsIdentity() const { return !brightness && !contrast && !saturation && !temperature; }
};

namespace nv12detail
{
// out = (in - 128) * mul / 128 + add, per 16-bit lane, saturated to 0..255.
// `add` may differ per lane (U and V offsets alternate in the chroma plane).
inline void AdjustRows(const uint8_t* src, long srcPitch, uint8_t* dst, long dstPitch,
                       uint32_t bytes, uint32_t rows, int16_t mul, const int16_t add[2])
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i bias = _mm_set1_epi16(128);
    const __m128i m = _mm_set1_epi16(mul);
    const __m128i a = _mm_setr_epi16(add[0], add[1], add[0], add[1], add[0], add[1], add[0], add[1]);
    for (uint32_t y = 0; y < rows; ++y)
    {
        const uint8_t* s = src + static_cast<size_t>(y) * srcPitch;
        uint8_t* d = dst + static_cast<size_t>(y) * dstPitch;
        uint32_t x = 0;
        for (; x + 16 <= bytes; x += 16)
        {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + x));
            __m128i lo = _mm_sub_epi16(_mm_unpacklo_epi8(v, zero), bias);
            __m128i hi = _mm_sub_epi16(_mm_unpackhi_epi8(v, zero), bias);
            lo = _mm_add_epi16(_mm_srai_epi16(_mm_mullo_epi16(lo, m), 7), a);
            hi = _mm_add_epi16(_mm_srai_epi16(_mm_mullo_epi16(hi, m), 7), a);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + x), _mm_packus_epi16(lo, hi));
        }
        for (; x < bytes; ++x)
        {
            const int v = ((s[x] - 128) * mul >> 7) + add[x & 1];
            d[x] = static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}
} // namespace nv12detail

// Applies colour adjustments from src (pitch srcPitch) into a contiguous dst
// of the same size. src == dst (with srcPitch == w) adjusts in place.
inline void Nv12Adjust(const uint8_t* src, long srcPitch, uint8_t* dst, uint32_t w, uint32_t h,
                       const Nv12ColorAdjust& c)
{
    // Multipliers are 1.7 fixed point (128 = 1.0) so (x-128)*mul fits int16.
    const int16_t contrast = static_cast<int16_t>(128 + c.contrast * 102 / 100);    // 0.2x .. 1.8x
    const int16_t saturation = static_cast<int16_t>(128 + c.saturation * 128 / 100); // 0x .. 2x
    const int16_t brightness = static_cast<int16_t>(c.brightness * 64 / 100);        // -64 .. +64
    const int16_t warm = static_cast<int16_t>(c.temperature * 30 / 100);              // chroma shift

    const int16_t lumaAdd[2] = { static_cast<int16_t>(128 + brightness), static_cast<int16_t>(128 + brightness) };
    nv12detail::AdjustRows(src, srcPitch, dst, static_cast<long>(w), w, h, contrast, lumaAdd);

    // Warmer = less blue (U down), more red (V up).
    const int16_t chromaAdd[2] = { static_cast<int16_t>(128 - warm), static_cast<int16_t>(128 + warm) };
    nv12detail::AdjustRows(src + static_cast<size_t>(srcPitch) * h, srcPitch,
                           dst + static_cast<size_t>(w) * h, static_cast<long>(w), w, h / 2, saturation, chromaAdd);
}

// Orientation applied by the host before frames reach the virtual camera.
// Rotation is clockwise and happens first; mirror (left-right) and flip
// (top-bottom) then apply to the rotated image, i.e. what the user sees.
struct Nv12Orientation
{
    int rotation = 0; // 0, 90, 180 or 270
    bool mirror = false;
    bool flip = false;

    bool IsIdentity() const { return rotation == 0 && !mirror && !flip; }
    bool SwapsSize() const { return rotation == 90 || rotation == 270; }
};

// Writes src (w x h, srcPitch) into a contiguous dst of the oriented size:
// (h x w) for 90/270, otherwise (w x h). w and h must be even.
inline void Nv12Orient(const uint8_t* src, long srcPitch, uint32_t w, uint32_t h,
                       uint8_t* dst, const Nv12Orientation& o)
{
    const uint32_t ow = o.SwapsSize() ? h : w;
    const uint32_t oh = o.SwapsSize() ? w : h;

    // Maps an output position (after undoing mirror/flip) to a source one,
    // for a plane of size sw x sh whose rotated size is dw x dh.
    auto source = [&](uint32_t x, uint32_t y, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                      uint32_t& sx, uint32_t& sy) {
        if (o.mirror) x = dw - 1 - x;
        if (o.flip) y = dh - 1 - y;
        switch (o.rotation)
        {
        case 90:  sx = y;          sy = sh - 1 - x; break;
        case 180: sx = sw - 1 - x; sy = sh - 1 - y; break;
        case 270: sx = sw - 1 - y; sy = x;          break;
        default:  sx = x;          sy = y;          break;
        }
    };

    for (uint32_t y = 0; y < oh; ++y)
    {
        uint8_t* d = dst + static_cast<size_t>(y) * ow;
        for (uint32_t x = 0; x < ow; ++x)
        {
            uint32_t sx, sy;
            source(x, y, w, h, ow, oh, sx, sy);
            d[x] = src[static_cast<size_t>(sy) * srcPitch + sx];
        }
    }

    const uint8_t* srcUv = src + static_cast<size_t>(srcPitch) * h;
    uint8_t* dstUv = dst + static_cast<size_t>(ow) * oh;
    const uint32_t cw = w / 2, ch = h / 2, ocw = ow / 2, och = oh / 2;
    for (uint32_t y = 0; y < och; ++y)
    {
        uint8_t* d = dstUv + static_cast<size_t>(y) * ow;
        for (uint32_t x = 0; x < ocw; ++x)
        {
            uint32_t sx, sy;
            source(x, y, cw, ch, ocw, och, sx, sy);
            const uint8_t* s = srcUv + static_cast<size_t>(sy) * srcPitch + sx * 2;
            d[x * 2] = s[0];
            d[x * 2 + 1] = s[1];
        }
    }
}

// A source rectangle in pixels; fractional so zoom/pan move smoothly.
struct Nv12Rect
{
    double x = 0, y = 0, w = 0, h = 0;
};

// Bilinear-scales the `r` region of src (sw x sh) into a contiguous dst of
// dw x dh. Used for digital zoom; 8-bit fixed-point weights.
inline void Nv12CropScaleBilinear(const uint8_t* src, long srcPitch, uint32_t sw, uint32_t sh,
                                  const Nv12Rect& r, uint8_t* dst, uint32_t dw, uint32_t dh)
{
    struct Tap { uint32_t i0, i1; uint32_t w1; }; // w1 in 0..256
    auto taps = [](std::vector<Tap>& out, uint32_t n, double start, double len, uint32_t limit) {
        out.resize(n);
        const double step = len / n;
        for (uint32_t i = 0; i < n; ++i)
        {
            double f = start + (i + 0.5) * step - 0.5;
            if (f < 0) f = 0;
            if (f > limit - 1) f = limit - 1;
            const uint32_t i0 = static_cast<uint32_t>(f);
            out[i] = { i0, i0 + 1 < limit ? i0 + 1 : i0, static_cast<uint32_t>((f - i0) * 256 + 0.5) };
        }
    };

    thread_local std::vector<Tap> xs, ys;

    // Luma.
    taps(xs, dw, r.x, r.w, sw);
    taps(ys, dh, r.y, r.h, sh);
    for (uint32_t y = 0; y < dh; ++y)
    {
        const uint8_t* a = src + static_cast<size_t>(ys[y].i0) * srcPitch;
        const uint8_t* b = src + static_cast<size_t>(ys[y].i1) * srcPitch;
        const uint32_t wy = ys[y].w1;
        uint8_t* d = dst + static_cast<size_t>(y) * dw;
        for (uint32_t x = 0; x < dw; ++x)
        {
            const Tap& t = xs[x];
            const uint32_t top = a[t.i0] * (256 - t.w1) + a[t.i1] * t.w1;
            const uint32_t bot = b[t.i0] * (256 - t.w1) + b[t.i1] * t.w1;
            d[x] = static_cast<uint8_t>((top * (256 - wy) + bot * wy + 32768) >> 16);
        }
    }

    // Chroma (interleaved UV at half resolution).
    const uint8_t* srcUv = src + static_cast<size_t>(srcPitch) * sh;
    uint8_t* dstUv = dst + static_cast<size_t>(dw) * dh;
    taps(xs, dw / 2, r.x / 2, r.w / 2, sw / 2);
    taps(ys, dh / 2, r.y / 2, r.h / 2, sh / 2);
    for (uint32_t y = 0; y < dh / 2; ++y)
    {
        const uint8_t* a = srcUv + static_cast<size_t>(ys[y].i0) * srcPitch;
        const uint8_t* b = srcUv + static_cast<size_t>(ys[y].i1) * srcPitch;
        const uint32_t wy = ys[y].w1;
        uint8_t* d = dstUv + static_cast<size_t>(y) * dw;
        for (uint32_t x = 0; x < dw / 2; ++x)
        {
            const Tap& t = xs[x];
            for (int c = 0; c < 2; ++c)
            {
                const uint32_t top = a[t.i0 * 2 + c] * (256 - t.w1) + a[t.i1 * 2 + c] * t.w1;
                const uint32_t bot = b[t.i0 * 2 + c] * (256 - t.w1) + b[t.i1 * 2 + c] * t.w1;
                d[x * 2 + c] = static_cast<uint8_t>((top * (256 - wy) + bot * wy + 32768) >> 16);
            }
        }
    }
}

// Copies or nearest-neighbour scales an NV12 image. Planes are contiguous:
// the UV plane starts right after `height` luma rows of `pitch` bytes.
inline void Nv12Scale(const uint8_t* src, long srcPitch, uint32_t sw, uint32_t sh,
                      uint8_t* dst, long dstPitch, uint32_t dw, uint32_t dh)
{
    const uint8_t* srcUv = src + static_cast<size_t>(srcPitch) * sh;
    uint8_t* dstUv = dst + static_cast<size_t>(dstPitch) * dh;

    if (sw == dw && sh == dh)
    {
        for (uint32_t y = 0; y < dh; ++y)
            memcpy(dst + static_cast<size_t>(y) * dstPitch, src + static_cast<size_t>(y) * srcPitch, dw);
        for (uint32_t y = 0; y < dh / 2; ++y)
            memcpy(dstUv + static_cast<size_t>(y) * dstPitch, srcUv + static_cast<size_t>(y) * srcPitch, dw);
        return;
    }

    // thread_local so repeated calls at the same size do not reallocate.
    thread_local std::vector<uint32_t> xmap;
    thread_local std::vector<uint32_t> cxmap;
    if (xmap.size() != dw)
    {
        xmap.resize(dw);
        cxmap.resize(dw / 2);
    }
    for (uint32_t x = 0; x < dw; ++x) xmap[x] = static_cast<uint32_t>(static_cast<uint64_t>(x) * sw / dw);
    for (uint32_t x = 0; x < dw / 2; ++x)
        cxmap[x] = static_cast<uint32_t>(static_cast<uint64_t>(x) * (sw / 2) / (dw / 2)) * 2;

    for (uint32_t y = 0; y < dh; ++y)
    {
        const uint8_t* s = src + static_cast<size_t>(static_cast<uint64_t>(y) * sh / dh) * srcPitch;
        uint8_t* d = dst + static_cast<size_t>(y) * dstPitch;
        for (uint32_t x = 0; x < dw; ++x) d[x] = s[xmap[x]];
    }
    for (uint32_t y = 0; y < dh / 2; ++y)
    {
        const uint8_t* s = srcUv + static_cast<size_t>(static_cast<uint64_t>(y) * (sh / 2) / (dh / 2)) * srcPitch;
        uint8_t* d = dstUv + static_cast<size_t>(y) * dstPitch;
        for (uint32_t x = 0; x < dw / 2; ++x)
        {
            d[2 * x] = s[cxmap[x]];
            d[2 * x + 1] = s[cxmap[x] + 1];
        }
    }
}
