// Automatic framing ("Tự căn khung"): from the faces found in the picture, the digital zoom and
// centre that keep the people in a head-and-shoulders shot, moved there smoothly. Pure logic, no
// Windows: the faces come from FaceDetect.cpp; tools/framing_check.cpp tests this file.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// A face, as a fraction of the oriented frame (0..1 on each axis).
struct FaceRect
{
    double x = 0, y = 0, w = 0, h = 0;
};

// The oriented frame, and the picture shown at zoom 1 (its aspect crop), in pixels.
struct FramingGeometry
{
    double frameWidth = 0, frameHeight = 0;
    double pictureWidth = 0, pictureHeight = 0;
};

// A digital zoom and its centre (0..1 of the oriented frame), as in ImageSettings.
struct FramingView
{
    double zoom = 1.0;
    double centerX = 0.5;
    double centerY = 0.5;
};

// "Smart Zoom": how far framing may zoom into this picture. A third of a 4K picture still has
// 720 lines, what meeting apps send; smaller pictures get 1.5x (a little soft, still useful).
inline double FramingMaxZoom(const FramingGeometry& g)
{
    const double shortSide = std::min(g.pictureWidth, g.pictureHeight);
    return std::clamp(shortSide / 720.0, 1.5, 3.0);
}

// Keeps a view's centre where the whole view stays inside the frame.
inline FramingView ClampView(FramingView v, const FramingGeometry& g)
{
    if (g.frameWidth <= 0 || g.frameHeight <= 0) return v;
    const double fx = std::min(1.0, g.pictureWidth / v.zoom / g.frameWidth);
    const double fy = std::min(1.0, g.pictureHeight / v.zoom / g.frameHeight);
    v.centerX = std::clamp(v.centerX, fx / 2, 1 - fx / 2);
    v.centerY = std::clamp(v.centerY, fy / 2, 1 - fy / 2);
    return v;
}

// The shot for these faces: everyone in it, about three face heights tall (head and shoulders),
// the faces a little above the middle. Faces much smaller than the largest are left out: a
// poster, a TV, someone far behind.
inline FramingView ComposeShot(const std::vector<FaceRect>& faces, const FramingGeometry& g)
{
    double largest = 0;
    for (const FaceRect& f : faces) largest = std::max(largest, f.h * g.frameHeight);
    if (largest <= 0) return {};

    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, faceW = 0;
    for (const FaceRect& f : faces)
    {
        if (f.h * g.frameHeight < 0.4 * largest) continue;
        x0 = std::min(x0, f.x * g.frameWidth);
        y0 = std::min(y0, f.y * g.frameHeight);
        x1 = std::max(x1, (f.x + f.w) * g.frameWidth);
        y1 = std::max(y1, (f.y + f.h) * g.frameHeight);
        faceW = std::max(faceW, f.w * g.frameWidth);
    }
    const double needW = std::max(faceW * 3.2, (x1 - x0) + faceW * 2.2);
    const double needH = std::max(largest * 3.0, (y1 - y0) + largest * 2.0);

    FramingView v;
    v.zoom = std::clamp(std::min(g.pictureWidth / needW, g.pictureHeight / needH), 1.0, FramingMaxZoom(g));
    const double viewH = g.pictureHeight / v.zoom;
    v.centerX = (x0 + x1) / 2 / g.frameWidth;
    v.centerY = ((y0 + y1) / 2 + 0.10 * viewH) / g.frameHeight; // faces at 40% from the top
    return ClampView(v, g);
}

class AutoFramer
{
public:
    // Back to the whole picture at once: a new source, a new shape, or framing just turned on.
    void Reset()
    {
        m_started = false;
        m_faces.clear();
        m_faceCount = 0;
        m_newFaces = false;
    }

    // A detection result (may be empty). Empty results keep the shot for a while: a face is
    // often missed for a moment (turned away, a hand in front).
    void OnFaces(const std::vector<FaceRect>& faces, double nowSec)
    {
        m_faceCount = static_cast<int>(faces.size());
        if (faces.empty()) return;
        m_faces = faces;
        m_lastFaceSec = nowSec;
        m_newFaces = true;
    }

    // Once per frame: the view to show now. nowSec is a steady clock in seconds.
    FramingView Step(const FramingGeometry& g, double nowSec)
    {
        if (!m_started)
        {
            m_started = true;
            m_view = m_target = FramingView{};
            m_lastSec = nowSec;
        }
        const double dt = std::clamp(nowSec - m_lastSec, 0.0, 0.25);
        m_lastSec = nowSec;

        if (!m_faces.empty() && nowSec - m_lastFaceSec > kWideAfterSec)
        {
            m_faces.clear(); // nobody for a while: show the whole room
            m_newFaces = true;
        }
        if (m_newFaces || g.pictureWidth != m_geometry.pictureWidth || g.pictureHeight != m_geometry.pictureHeight)
        {
            m_newFaces = false;
            m_geometry = g;
            const FramingView shot = m_faces.empty() ? ClampView(FramingView{}, g) : ComposeShot(m_faces, g);
            // Re-frame only when the people moved enough: a steady shot, not one that follows
            // every small movement.
            const double viewW = g.pictureWidth / m_target.zoom, viewH = g.pictureHeight / m_target.zoom;
            const bool moved = std::abs(shot.centerX - m_target.centerX) * g.frameWidth > kMoveFraction * viewW ||
                               std::abs(shot.centerY - m_target.centerY) * g.frameHeight > kMoveFraction * viewH;
            const bool zoomed = std::abs(std::log(shot.zoom / m_target.zoom)) > std::log(kZoomRatio);
            if (moved || zoomed) m_target = shot;
        }
        m_target = ClampView(m_target, g);

        // Ease towards the target, the same whatever the frame rate; zoom in log space.
        const double a = 1 - std::exp(-dt / kPanSeconds);
        const double b = 1 - std::exp(-dt / kZoomSeconds);
        m_view.zoom = std::exp(std::log(m_view.zoom) + (std::log(m_target.zoom) - std::log(m_view.zoom)) * b);
        m_view.centerX += (m_target.centerX - m_view.centerX) * a;
        m_view.centerY += (m_target.centerY - m_view.centerY) * a;
        m_view = ClampView(m_view, g);
        return m_view;
    }

    // Faces in the last detection result (0: nobody seen just now).
    int Faces() const { return m_faceCount; }
    FramingView Target() const { return m_target; }

    static constexpr double kWideAfterSec = 4.0; // no face this long: back to the whole picture
    static constexpr double kMoveFraction = 0.10; // of the view: smaller moves keep the shot
    static constexpr double kZoomRatio = 1.15;
    static constexpr double kPanSeconds = 0.6;    // time constants of the easing
    static constexpr double kZoomSeconds = 0.9;

private:
    bool m_started = false;
    std::vector<FaceRect> m_faces; // the last faces seen
    int m_faceCount = 0;
    bool m_newFaces = false;
    double m_lastFaceSec = 0;
    double m_lastSec = 0;
    FramingGeometry m_geometry;
    FramingView m_view, m_target;
};

// Size of the grey copy the face detector looks at: the long side at most maxLong pixels.
inline void DetectionSize(uint32_t width, uint32_t height, uint32_t maxLong, uint32_t& dw, uint32_t& dh)
{
    const double scale = std::min(1.0, static_cast<double>(maxLong) / std::max<uint32_t>(1, std::max(width, height)));
    dw = std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(width * scale)));
    dh = std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(height * scale)));
}

// Shrinks a luma plane to dw x dh (tightly packed): each pixel averages the 2x2 block at the
// centre of its area. Enough for a face detector, and cheap (four reads per pixel).
inline void DownscaleLuma(const uint8_t* src, long pitch, uint32_t w, uint32_t h, uint8_t* dst, uint32_t dw, uint32_t dh)
{
    if (w < 2 || h < 2 || !dw || !dh) return;
    for (uint32_t y = 0; y < dh; ++y)
    {
        const uint32_t sy = std::min<uint32_t>(h - 2, static_cast<uint32_t>((2ull * y + 1) * h / (2ull * dh)));
        const uint8_t* r0 = src + static_cast<size_t>(sy) * pitch;
        const uint8_t* r1 = r0 + pitch;
        uint8_t* out = dst + static_cast<size_t>(y) * dw;
        for (uint32_t x = 0; x < dw; ++x)
        {
            const uint32_t sx = std::min<uint32_t>(w - 2, static_cast<uint32_t>((2ull * x + 1) * w / (2ull * dw)));
            out[x] = static_cast<uint8_t>((r0[sx] + r0[sx + 1] + r1[sx] + r1[sx + 1] + 2) / 4);
        }
    }
}
