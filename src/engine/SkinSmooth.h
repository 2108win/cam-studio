// "Làm mịn da": an edge-preserving smoothing (a self-guided filter, He et al. 2010) of the luma
// inside each face, faded out over an ellipse around it. Eyes, brows and the face's outline keep
// their edges (high local variance); even skin is evened out. Chroma is left alone.
// Header only, so the picture check can test it.
#pragma once

#include "AutoFrame.h" // FaceRect

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct SkinScratch
{
    std::vector<float> i, ii, a, b, tmp;
};

namespace skindetail
{
// Box mean of radius r over a w x h float plane, edges clamped (two 1-D passes, running sums).
inline void BoxMean(const float* src, float* dst, float* tmp, int w, int h, int r)
{
    for (int y = 0; y < h; ++y)
    {
        const float* s = src + static_cast<size_t>(y) * w;
        float* t = tmp + static_cast<size_t>(y) * w;
        double sum = 0;
        for (int x = -r; x <= r; ++x) sum += s[std::clamp(x, 0, w - 1)];
        for (int x = 0; x < w; ++x)
        {
            t[x] = static_cast<float>(sum / (2 * r + 1));
            sum += s[std::min(x + r + 1, w - 1)] - s[std::max(x - r, 0)];
        }
    }
    for (int x = 0; x < w; ++x)
    {
        double sum = 0;
        for (int y = -r; y <= r; ++y) sum += tmp[static_cast<size_t>(std::clamp(y, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y)
        {
            dst[static_cast<size_t>(y) * w + x] = static_cast<float>(sum / (2 * r + 1));
            sum += tmp[static_cast<size_t>(std::min(y + r + 1, h - 1)) * w + x] - tmp[static_cast<size_t>(std::max(y - r, 0)) * w + x];
        }
    }
}
} // namespace skindetail

// y: the luma plane of a w x h picture (pitch bytes a row). face: normalised to that picture.
// strength 1..100.
inline void SmoothFace(uint8_t* y, long pitch, uint32_t w, uint32_t h, const FaceRect& face, int strength, SkinScratch& s)
{
    using namespace skindetail;
    if (strength <= 0 || w < 16 || h < 16) return;
    // The face box, wider and taller (forehead, cheeks, chin), as the ellipse's bounds.
    const double cx = (face.x + face.w / 2) * w, cy = (face.y + face.h * 0.55) * h;
    const double rx = face.w * w * 0.75, ry = face.h * h * 0.85;
    const int x0 = std::max(0, static_cast<int>(cx - rx)), x1 = std::min(static_cast<int>(w), static_cast<int>(cx + rx));
    const int y0 = std::max(0, static_cast<int>(cy - ry)), y1 = std::min(static_cast<int>(h), static_cast<int>(cy + ry));
    const int rw = x1 - x0, rh = y1 - y0;
    if (rw < 8 || rh < 8) return;

    const size_t n = static_cast<size_t>(rw) * rh;
    s.i.resize(n);
    s.ii.resize(n);
    s.a.resize(n);
    s.b.resize(n);
    s.tmp.resize(n);
    for (int yy = 0; yy < rh; ++yy)
    {
        const uint8_t* row = y + static_cast<size_t>(y0 + yy) * pitch + x0;
        for (int xx = 0; xx < rw; ++xx)
        {
            const float v = row[xx];
            s.i[static_cast<size_t>(yy) * rw + xx] = v;
            s.ii[static_cast<size_t>(yy) * rw + xx] = v * v;
        }
    }
    // The radius follows the face's size; eps (what counts as "even") follows the strength.
    const int r = std::max(2, static_cast<int>(face.h * h / 45));
    const float sigma = 3.f + 0.30f * std::clamp(strength, 0, 100); // in luma steps
    const float eps = sigma * sigma;
    BoxMean(s.i.data(), s.a.data(), s.tmp.data(), rw, rh, r);   // a = mean I
    BoxMean(s.ii.data(), s.b.data(), s.tmp.data(), rw, rh, r);  // b = mean I²
    for (size_t k = 0; k < n; ++k)
    {
        const float mean = s.a[k];
        const float var = std::max(0.f, s.b[k] - mean * mean);
        const float ak = var / (var + eps);
        s.a[k] = ak;
        s.b[k] = mean - ak * mean;
    }
    BoxMean(s.a.data(), s.ii.data(), s.tmp.data(), rw, rh, r);  // mean a (reuses ii)
    BoxMean(s.b.data(), s.a.data(), s.tmp.data(), rw, rh, r);   // mean b
    const float amount = std::min(1.f, 0.35f + 0.65f * strength / 100.f);
    for (int yy = 0; yy < rh; ++yy)
    {
        uint8_t* row = y + static_cast<size_t>(y0 + yy) * pitch + x0;
        const double dy = (y0 + yy + 0.5 - cy) / ry;
        for (int xx = 0; xx < rw; ++xx)
        {
            const double dx = (x0 + xx + 0.5 - cx) / rx;
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d >= 1.0) continue;
            // Full inside 70 % of the ellipse, fading to nothing at its edge.
            const double t = std::clamp((1.0 - d) / 0.3, 0.0, 1.0);
            const float weight = amount * static_cast<float>(t * t * (3 - 2 * t));
            const size_t k = static_cast<size_t>(yy) * rw + xx;
            const float q = s.ii[k] * s.i[k] + s.a[k];
            const float v = s.i[k] + weight * (q - s.i[k]);
            row[xx] = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
        }
    }
}
