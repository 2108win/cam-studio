// What the user set up for the picture (orientation, crop aspect, digital
// zoom) and the frame geometry that follows from it.
#pragma once

#include "../common/Nv12.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

enum class CropAspect : int
{
    Native = 0,
    R16x9 = 1,
    R4x3 = 2,
    R1x1 = 3,
    R9x16 = 4,
};

struct ImageSettings
{
    Nv12Orientation orientation;
    CropAspect aspect = CropAspect::Native;
    double zoom = 1.0;    // 1 .. 4
    double centerX = 0.5; // zoom centre, 0..1 of the oriented frame
    double centerY = 0.5;
    Nv12ColorAdjust color;
};

struct FrameLayout
{
    uint32_t orientedWidth = 0;  // after rotation
    uint32_t orientedHeight = 0;
    uint32_t outWidth = 0;       // what the virtual camera publishes
    uint32_t outHeight = 0;
    Nv12Rect source;             // region of the oriented frame that is shown
    bool needsOrient = false;
    bool needsResample = false;  // zoom, crop or fit active

    // The picture at its own size (what the output is when nothing is fitted).
    uint32_t pictureWidth = 0;
    uint32_t pictureHeight = 0;
    // Fitted: the output has another size (an app holds the virtual camera at
    // the previous source's size, or a fixed size was chosen). The picture is
    // scaled into this rectangle (even position and size, centred) and the
    // rest is black bars. Otherwise the rectangle is the whole output.
    bool fitted = false;
    uint32_t destX = 0, destY = 0, destWidth = 0, destHeight = 0;
};

// "Kích thước camera ảo": the source's own size (best quality), or a fixed one.
enum class OutputSize : int
{
    Auto = 0,
    R2160 = 1, // 3840 x 2160
    R1080 = 2, // 1920 x 1080
    R720 = 3,  // 1280 x 720
};

inline void FixedOutputSize(OutputSize s, uint32_t& w, uint32_t& h)
{
    switch (s)
    {
    case OutputSize::R2160: w = 3840; h = 2160; break;
    case OutputSize::R1080: w = 1920; h = 1080; break;
    case OutputSize::R720: w = 1280; h = 720; break;
    default: w = h = 0; break;
    }
}

// What the virtual camera should publish next.
struct OutputPlan
{
    uint32_t width = 0;
    uint32_t height = 0;
    bool rebuild = false; // publish a new virtual camera at this size (drops any app using it)
};

// The size rule. "Tự động": the picture's own size, but an app using the
// camera keeps the size it has (rebuilding would drop the camera in a call),
// and the picture is fitted into it until the app lets go. A fixed size works
// the same way when it changes. currentW/H is 0 when no camera is published.
// consumerHolds: an app streams from the camera now, or did a moment ago (an
// app restarting its stream briefly lets go).
inline OutputPlan PlanOutput(OutputSize mode, uint32_t pictureW, uint32_t pictureH,
                             uint32_t currentW, uint32_t currentH, bool consumerHolds)
{
    uint32_t w = pictureW, h = pictureH;
    if (mode != OutputSize::Auto) FixedOutputSize(mode, w, h);
    if (!currentW || !currentH) return { w, h, true };
    if (currentW == w && currentH == h) return { w, h, false };
    if (consumerHolds) return { currentW, currentH, false };
    return { w, h, true };
}

inline double AspectRatio(CropAspect a)
{
    switch (a)
    {
    case CropAspect::R16x9: return 16.0 / 9.0;
    case CropAspect::R4x3: return 4.0 / 3.0;
    case CropAspect::R1x1: return 1.0;
    case CropAspect::R9x16: return 9.0 / 16.0;
    default: return 0.0;
    }
}

// The picture size depends only on the camera size, rotation and aspect, so
// zoom and pan can change live without touching the virtual camera.
// fitWidth/fitHeight (both non-zero): the output has that size instead, with
// the picture scaled to fit inside it and black bars around (see FrameLayout).
inline FrameLayout ComputeLayout(uint32_t cameraWidth, uint32_t cameraHeight, const ImageSettings& s,
                                 uint32_t fitWidth = 0, uint32_t fitHeight = 0)
{
    FrameLayout l;
    const bool swap = s.orientation.SwapsSize();
    l.orientedWidth = swap ? cameraHeight : cameraWidth;
    l.orientedHeight = swap ? cameraWidth : cameraHeight;
    l.needsOrient = !s.orientation.IsIdentity();

    // Largest centred region with the requested aspect.
    double rw = l.orientedWidth, rh = l.orientedHeight;
    if (const double ratio = AspectRatio(s.aspect); ratio > 0)
    {
        if (rw / rh > ratio) rw = rh * ratio;
        else rh = rw / ratio;
    }
    l.outWidth = std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(rw)) & ~1u);
    l.outHeight = std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(rh)) & ~1u);

    // Zoom shrinks the shown region around the chosen centre, kept inside.
    const double zoom = std::clamp(s.zoom, 1.0, 4.0);
    const double vw = l.outWidth / zoom, vh = l.outHeight / zoom;
    const double cx = std::clamp(s.centerX, 0.0, 1.0) * l.orientedWidth;
    const double cy = std::clamp(s.centerY, 0.0, 1.0) * l.orientedHeight;
    l.source.w = vw;
    l.source.h = vh;
    l.source.x = std::clamp(cx - vw / 2, 0.0, l.orientedWidth - vw);
    l.source.y = std::clamp(cy - vh / 2, 0.0, l.orientedHeight - vh);
    if (zoom <= 1.0001)
    {
        // Plain crop: whole even pixels, so resampling is an exact copy.
        l.source.x = std::floor(l.source.x / 2) * 2;
        l.source.y = std::floor(l.source.y / 2) * 2;
    }

    l.needsResample = zoom > 1.0001 || l.outWidth != l.orientedWidth || l.outHeight != l.orientedHeight;

    l.pictureWidth = l.outWidth;
    l.pictureHeight = l.outHeight;
    l.destWidth = l.outWidth;
    l.destHeight = l.outHeight;
    if (fitWidth >= 2 && fitHeight >= 2 && (fitWidth != l.outWidth || fitHeight != l.outHeight))
    {
        fitWidth &= ~1u;
        fitHeight &= ~1u;
        const double scale = std::min(static_cast<double>(fitWidth) / l.pictureWidth,
                                      static_cast<double>(fitHeight) / l.pictureHeight);
        // Even and inside the output; the side that fills it exactly stays exact.
        auto even = [](double v, uint32_t limit) {
            const uint32_t n = static_cast<uint32_t>(std::lround(v / 2)) * 2;
            return std::clamp<uint32_t>(n, 2, limit);
        };
        l.destWidth = even(l.pictureWidth * scale, fitWidth);
        l.destHeight = even(l.pictureHeight * scale, fitHeight);
        l.destX = ((fitWidth - l.destWidth) / 2) & ~1u;
        l.destY = ((fitHeight - l.destHeight) / 2) & ~1u;
        l.outWidth = fitWidth;
        l.outHeight = fitHeight;
        l.fitted = true;
        l.needsResample = true;
    }
    return l;
}
