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
    bool needsResample = false;  // zoom or crop active
};

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

// The output size depends only on the camera size, rotation and aspect, so
// zoom and pan can change live without touching the virtual camera.
inline FrameLayout ComputeLayout(uint32_t cameraWidth, uint32_t cameraHeight, const ImageSettings& s)
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
    return l;
}
