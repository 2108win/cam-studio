// NV12 copy/scale helpers shared by the host and the media source.
#pragma once

#include <algorithm>
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

// Applies colour adjustments to a w x h picture whose planes may sit anywhere
// (e.g. a rectangle inside a bigger frame). src == dst adjusts in place.
inline void Nv12AdjustPlanes(const uint8_t* srcY, const uint8_t* srcUv, long srcPitch,
                             uint8_t* dstY, uint8_t* dstUv, long dstPitch, uint32_t w, uint32_t h,
                             const Nv12ColorAdjust& c)
{
    // Multipliers are 1.7 fixed point (128 = 1.0) so (x-128)*mul fits int16.
    const int16_t contrast = static_cast<int16_t>(128 + c.contrast * 102 / 100);    // 0.2x .. 1.8x
    const int16_t saturation = static_cast<int16_t>(128 + c.saturation * 128 / 100); // 0x .. 2x
    const int16_t brightness = static_cast<int16_t>(c.brightness * 64 / 100);        // -64 .. +64
    const int16_t warm = static_cast<int16_t>(c.temperature * 30 / 100);              // chroma shift

    const int16_t lumaAdd[2] = { static_cast<int16_t>(128 + brightness), static_cast<int16_t>(128 + brightness) };
    nv12detail::AdjustRows(srcY, srcPitch, dstY, dstPitch, w, h, contrast, lumaAdd);

    // Warmer = less blue (U down), more red (V up).
    const int16_t chromaAdd[2] = { static_cast<int16_t>(128 - warm), static_cast<int16_t>(128 + warm) };
    nv12detail::AdjustRows(srcUv, srcPitch, dstUv, dstPitch, w, h / 2, saturation, chromaAdd);
}

// Applies colour adjustments from src (pitch srcPitch) into a contiguous dst
// of the same size. src == dst (with srcPitch == w) adjusts in place.
inline void Nv12Adjust(const uint8_t* src, long srcPitch, uint8_t* dst, uint32_t w, uint32_t h,
                       const Nv12ColorAdjust& c)
{
    Nv12AdjustPlanes(src, src + static_cast<size_t>(srcPitch) * h, srcPitch,
                     dst, dst + static_cast<size_t>(w) * h, static_cast<long>(w), w, h, c);
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

// Bilinear-scales the `r` region of src (sw x sh, contiguous planes) into a
// dw x dh picture whose planes start at dstY / dstUv with dstPitch bytes per
// row: a whole frame, or a rectangle inside a bigger one (fit with bars).
// Used for digital zoom and fitting; 8-bit fixed-point weights.
inline void Nv12CropScaleBilinearTo(const uint8_t* src, long srcPitch, uint32_t sw, uint32_t sh,
                                    const Nv12Rect& r, uint8_t* dstY, uint8_t* dstUv, long dstPitch,
                                    uint32_t dw, uint32_t dh)
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
        uint8_t* d = dstY + static_cast<size_t>(y) * dstPitch;
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
    taps(xs, dw / 2, r.x / 2, r.w / 2, sw / 2);
    taps(ys, dh / 2, r.y / 2, r.h / 2, sh / 2);
    for (uint32_t y = 0; y < dh / 2; ++y)
    {
        const uint8_t* a = srcUv + static_cast<size_t>(ys[y].i0) * srcPitch;
        const uint8_t* b = srcUv + static_cast<size_t>(ys[y].i1) * srcPitch;
        const uint32_t wy = ys[y].w1;
        uint8_t* d = dstUv + static_cast<size_t>(y) * dstPitch;
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

// Same into a contiguous dw x dh frame.
inline void Nv12CropScaleBilinear(const uint8_t* src, long srcPitch, uint32_t sw, uint32_t sh,
                                  const Nv12Rect& r, uint8_t* dst, uint32_t dw, uint32_t dh)
{
    Nv12CropScaleBilinearTo(src, srcPitch, sw, sh, r, dst, dst + static_cast<size_t>(dw) * dh,
                            static_cast<long>(dw), dw, dh);
}

// Black in limited-range video (what the virtual camera and the preview assume).
constexpr uint8_t kNv12BlackY = 16;
constexpr uint8_t kNv12BlackUv = 128;

// Paints everything of a contiguous w x h frame outside the rectangle
// (rx, ry, rw, rh) black: the bars around a picture fitted inside the frame.
// The rectangle's position and size are even. The picture inside is left alone.
inline void Nv12FillBars(uint8_t* frame, uint32_t w, uint32_t h, uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh)
{
    auto plane = [](uint8_t* p, uint32_t rowBytes, uint32_t rows, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t v) {
        for (uint32_t y = 0; y < rows; ++y)
        {
            uint8_t* row = p + static_cast<size_t>(y) * rowBytes;
            if (y < y0 || y >= y1)
            {
                memset(row, v, rowBytes);
                continue;
            }
            memset(row, v, x0);
            memset(row + x1, v, rowBytes - x1);
        }
    };
    plane(frame, w, h, rx, ry, rx + rw, ry + rh, kNv12BlackY);
    // UV rows are w bytes too (interleaved pairs at half the height).
    plane(frame + static_cast<size_t>(w) * h, w, h / 2, rx, ry / 2, rx + rw, (ry + rh) / 2, kNv12BlackUv);
}

// BGRA (8 bits per channel, top-down, pitch bytes per row) -> NV12, BT.601
// limited range like the rest of the pipeline. w and h are even; chroma is the
// average of each 2x2 block.
inline void BgraToNv12(const uint8_t* bgra, size_t pitch, uint32_t w, uint32_t h,
                       uint8_t* dstY, uint8_t* dstUv, size_t dstPitch)
{
    for (uint32_t y = 0; y < h; y += 2)
    {
        const uint8_t* r0 = bgra + static_cast<size_t>(y) * pitch;
        const uint8_t* r1 = r0 + pitch;
        uint8_t* y0 = dstY + static_cast<size_t>(y) * dstPitch;
        uint8_t* y1 = y0 + dstPitch;
        uint8_t* uv = dstUv + static_cast<size_t>(y / 2) * dstPitch;
        for (uint32_t x = 0; x < w; x += 2)
        {
            const uint8_t* p[4] = { r0 + x * 4, r0 + x * 4 + 4, r1 + x * 4, r1 + x * 4 + 4 };
            int sb = 0, sg = 0, sr = 0;
            for (int i = 0; i < 4; ++i)
            {
                const int b = p[i][0], g = p[i][1], r = p[i][2];
                sb += b;
                sg += g;
                sr += r;
                const uint8_t luma = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                (i < 2 ? y0 : y1)[x + (i & 1)] = luma;
            }
            // Averages with rounding; the sums of four fit easily in int.
            const int b = (sb + 2) >> 2, g = (sg + 2) >> 2, r = (sr + 2) >> 2;
            uv[x] = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            uv[x + 1] = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

namespace nv12detail
{
// One BGRA pixel from Y, U, V (BT.601 limited range).
inline uint32_t BgraPixel(int y, int u, int v)
{
    auto clamp = [](int x) { return static_cast<uint32_t>(x < 0 ? 0 : x > 255 ? 255 : x); };
    const int c = y - 16, d = u - 128, e = v - 128;
    const uint32_t r = clamp((298 * c + 409 * e + 128) >> 8);
    const uint32_t g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
    const uint32_t b = clamp((298 * c + 516 * d + 128) >> 8);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}
} // namespace nv12detail

// The plain version of Nv12ToBgra below, kept as its reference (frame-check compares the two).
inline void Nv12ToBgraScalar(const uint8_t* yPlane, const uint8_t* uvPlane, long pitch, uint32_t w, uint32_t h,
                             uint32_t* out, uint32_t pw, uint32_t ph)
{
    for (uint32_t y = 0; y < ph; ++y)
    {
        const uint32_t sy = static_cast<uint32_t>(static_cast<uint64_t>(y) * h / ph);
        const uint8_t* row = yPlane + static_cast<size_t>(sy) * pitch;
        const uint8_t* uv = uvPlane + static_cast<size_t>(sy / 2) * pitch;
        uint32_t* o = out + static_cast<size_t>(y) * pw;
        for (uint32_t x = 0; x < pw; ++x)
        {
            const uint32_t sx = static_cast<uint32_t>(static_cast<uint64_t>(x) * w / pw);
            o[x] = nv12detail::BgraPixel(row[sx], uv[sx & ~1u], uv[(sx & ~1u) + 1]);
        }
    }
}

// NV12 (w x h, planes at y / uv with pitch bytes per row) -> BGRA of pw x ph,
// nearest-neighbour (pw == w and ph == h converts 1:1). BT.601 limited range.
// Used for on-screen pictures (the preview, a phone screen's window). Each output row's samples
// are gathered first, then converted with SSE2 8 pixels at a time in 32-bit sums: the same
// output as Nv12ToBgraScalar, bit for bit.
inline void Nv12ToBgra(const uint8_t* yPlane, const uint8_t* uvPlane, long pitch, uint32_t w, uint32_t h,
                       uint32_t* out, uint32_t pw, uint32_t ph)
{
    thread_local std::vector<uint32_t> columns;
    thread_local std::vector<uint8_t> line; // one output row's Y, U and V samples
    columns.resize(pw);
    for (uint32_t x = 0; x < pw; ++x) columns[x] = static_cast<uint32_t>(static_cast<uint64_t>(x) * w / pw);
    line.resize(static_cast<size_t>(pw) * 3);
    uint8_t* ly = line.data();
    uint8_t* lu = ly + pw;
    uint8_t* lv = lu + pw;
    const __m128i zero = _mm_setzero_si128();
    const __m128i k16 = _mm_set1_epi16(16), k128 = _mm_set1_epi16(128);
    const __m128i one = _mm_set1_epi16(1);
    const __m128i round = _mm_set1_epi32(128);
    const __m128i kR = _mm_setr_epi16(298, 409, 298, 409, 298, 409, 298, 409);      // (c, e)
    const __m128i kB = _mm_setr_epi16(298, 516, 298, 516, 298, 516, 298, 516);      // (c, d)
    const __m128i kG1 = _mm_setr_epi16(298, -100, 298, -100, 298, -100, 298, -100); // (c, d)
    const __m128i kG2 = _mm_setr_epi16(-208, 128, -208, 128, -208, 128, -208, 128); // (e, 1): + rounding
    const __m128i alpha = _mm_set1_epi8(static_cast<char>(0xFF));
    // Four 32-bit sums >> 8 for each half, packed to 8 bytes clamped to 0..255.
    auto channel = [](__m128i lo, __m128i hi) {
        const __m128i v16 = _mm_packs_epi32(_mm_srai_epi32(lo, 8), _mm_srai_epi32(hi, 8));
        return _mm_packus_epi16(v16, v16);
    };

    uint32_t lastSy = UINT32_MAX;
    for (uint32_t y = 0; y < ph; ++y)
    {
        const uint32_t sy = static_cast<uint32_t>(static_cast<uint64_t>(y) * h / ph);
        uint32_t* o = out + static_cast<size_t>(y) * pw;
        if (sy == lastSy)
        {
            memcpy(o, o - pw, static_cast<size_t>(pw) * 4); // scaled up: the same source row again
            continue;
        }
        lastSy = sy;
        const uint8_t* row = yPlane + static_cast<size_t>(sy) * pitch;
        const uint8_t* uv = uvPlane + static_cast<size_t>(sy / 2) * pitch;
        const uint32_t* cols = columns.data();
        if (pw == w)
            memcpy(ly, row, pw);
        else
            for (uint32_t x = 0; x < pw; ++x) ly[x] = row[cols[x]];
        for (uint32_t x = 0; x < pw; ++x)
        {
            const uint32_t sx = cols[x] & ~1u;
            lu[x] = uv[sx];
            lv[x] = uv[sx + 1];
        }
        uint32_t x = 0;
        for (; x + 8 <= pw; x += 8)
        {
            auto load = [&](const uint8_t* p) {
                return _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p + x)), zero);
            };
            const __m128i c = _mm_sub_epi16(load(ly), k16);
            const __m128i d = _mm_sub_epi16(load(lu), k128);
            const __m128i e = _mm_sub_epi16(load(lv), k128);
            const __m128i ceLo = _mm_unpacklo_epi16(c, e), ceHi = _mm_unpackhi_epi16(c, e);
            const __m128i cdLo = _mm_unpacklo_epi16(c, d), cdHi = _mm_unpackhi_epi16(c, d);
            const __m128i e1Lo = _mm_unpacklo_epi16(e, one), e1Hi = _mm_unpackhi_epi16(e, one);
            const __m128i r = channel(_mm_add_epi32(_mm_madd_epi16(ceLo, kR), round), _mm_add_epi32(_mm_madd_epi16(ceHi, kR), round));
            const __m128i b = channel(_mm_add_epi32(_mm_madd_epi16(cdLo, kB), round), _mm_add_epi32(_mm_madd_epi16(cdHi, kB), round));
            const __m128i g = channel(_mm_add_epi32(_mm_madd_epi16(cdLo, kG1), _mm_madd_epi16(e1Lo, kG2)),
                                      _mm_add_epi32(_mm_madd_epi16(cdHi, kG1), _mm_madd_epi16(e1Hi, kG2)));
            const __m128i bg = _mm_unpacklo_epi8(b, g); // B G pairs
            const __m128i ra = _mm_unpacklo_epi8(r, alpha);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(o + x), _mm_unpacklo_epi16(bg, ra));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(o + x + 4), _mm_unpackhi_epi16(bg, ra));
        }
        for (; x < pw; ++x) o[x] = nv12detail::BgraPixel(ly[x], lu[x], lv[x]);
    }
}

// Copies or nearest-neighbour scales an NV12 image into separate Y and UV destinations (a
// rectangle inside a larger frame, say). The source planes are contiguous: the UV plane starts
// right after `sh` luma rows of `srcPitch` bytes.
inline void Nv12ScaleTo(const uint8_t* src, long srcPitch, uint32_t sw, uint32_t sh,
                        uint8_t* dst, uint8_t* dstUv, long dstPitch, uint32_t dw, uint32_t dh)
{
    const uint8_t* srcUv = src + static_cast<size_t>(srcPitch) * sh;

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

// Same, with contiguous destination planes (the UV plane right after `dh` rows of `dstPitch`).
inline void Nv12Scale(const uint8_t* src, long srcPitch, uint32_t sw, uint32_t sh,
                      uint8_t* dst, long dstPitch, uint32_t dw, uint32_t dh)
{
    Nv12ScaleTo(src, srcPitch, sw, sh, dst, dst + static_cast<size_t>(dstPitch) * dh, dstPitch, dw, dh);
}

// Scales an NV12 picture into a frame of another size keeping its shape: centred, black bars
// around (even position and size). Same size: a plain copy.
inline void Nv12FitInto(const uint8_t* src, long srcPitch, uint32_t sw, uint32_t sh,
                        uint8_t* dst, long dstPitch, uint32_t dw, uint32_t dh)
{
    if (!sw || !sh || !dw || !dh) return;
    if (sw == dw && sh == dh)
    {
        Nv12Scale(src, srcPitch, sw, sh, dst, dstPitch, dw, dh);
        return;
    }
    uint32_t rw = dw, rh = dh;
    if (static_cast<uint64_t>(sw) * dh > static_cast<uint64_t>(dw) * sh) rh = static_cast<uint32_t>(static_cast<uint64_t>(dw) * sh / sw);
    else rw = static_cast<uint32_t>(static_cast<uint64_t>(dh) * sw / sh);
    rw = std::max(2u, rw & ~1u);
    rh = std::max(2u, rh & ~1u);
    const uint32_t rx = ((dw - rw) / 2) & ~1u;
    const uint32_t ry = ((dh - rh) / 2) & ~1u;
    uint8_t* dstUv = dst + static_cast<size_t>(dstPitch) * dh;
    if (rw != dw || rh != dh)
    {
        // Bars: every row outside the rectangle, and the margins of the rows inside it.
        for (uint32_t y = 0; y < dh; ++y)
        {
            uint8_t* row = dst + static_cast<size_t>(y) * dstPitch;
            if (y < ry || y >= ry + rh) { memset(row, kNv12BlackY, dw); continue; }
            memset(row, kNv12BlackY, rx);
            memset(row + rx + rw, kNv12BlackY, dw - rx - rw);
        }
        for (uint32_t y = 0; y < dh / 2; ++y)
        {
            uint8_t* row = dstUv + static_cast<size_t>(y) * dstPitch;
            if (y < ry / 2 || y >= (ry + rh) / 2) { memset(row, kNv12BlackUv, dw); continue; }
            memset(row, kNv12BlackUv, rx);
            memset(row + rx + rw, kNv12BlackUv, dw - rx - rw);
        }
    }
    Nv12ScaleTo(src, srcPitch, sw, sh, dst + static_cast<size_t>(ry) * dstPitch + rx,
                dstUv + static_cast<size_t>(ry / 2) * dstPitch + rx, dstPitch, rw, rh);
}
