// Checks automatic framing (src/engine/AutoFrame.h): the shot for some faces, the steady target,
// easing, going wide when nobody is seen, the "Smart Zoom" limit, and the grey copy for the face
// detector. No Windows code: builds on Windows (camstudio-framing-check.exe) and on a Mac:
//   clang++ -std=c++20 -O1 -Wall -Wextra -o build/framing-check tools/framing_check.cpp && build/framing-check
#include "../src/engine/AutoFrame.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        printf("FAIL  %s\n", what.c_str());
    }
}

std::string Num(double v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

const FramingGeometry k4K{ 3840, 2160, 3840, 2160 };
const FramingGeometry k1080{ 1920, 1080, 1920, 1080 };
const FramingGeometry kPortrait{ 1080, 1920, 1080, 1920 };

// A face of this height (fraction of the frame), centred at (cx, cy), about 3:4.
FaceRect Face(const FramingGeometry& g, double cx, double cy, double h)
{
    const double w = h * 0.75 * g.frameHeight / g.frameWidth;
    return { cx - w / 2, cy - h / 2, w, h };
}

// Runs the framer from t0 for `seconds` at `fps`, with a detection every 0.2 s from `faces`.
// Returns the last view; t0 moves on.
FramingView Run(AutoFramer& f, const FramingGeometry& g, double& t0, double seconds, double fps,
                const std::function<std::vector<FaceRect>(double)>& faces)
{
    FramingView v;
    double nextDetect = t0;
    const int frames = static_cast<int>(std::lround(seconds * fps));
    for (int i = 1; i <= frames; ++i)
    {
        const double t = t0 + i / fps;
        if (t >= nextDetect)
        {
            f.OnFaces(faces(t), t);
            nextDetect += 0.2;
        }
        v = f.Step(g, t);
    }
    t0 += seconds;
    return v;
}

// The shown region in frame pixels.
struct Region { double x, y, w, h; };
Region Shown(const FramingView& v, const FramingGeometry& g)
{
    const double w = g.pictureWidth / v.zoom, h = g.pictureHeight / v.zoom;
    return { v.centerX * g.frameWidth - w / 2, v.centerY * g.frameHeight - h / 2, w, h };
}

bool Inside(const FaceRect& f, const Region& r, const FramingGeometry& g)
{
    const double x0 = f.x * g.frameWidth, y0 = f.y * g.frameHeight;
    const double x1 = (f.x + f.w) * g.frameWidth, y1 = (f.y + f.h) * g.frameHeight;
    return x0 >= r.x && y0 >= r.y && x1 <= r.x + r.w && y1 <= r.y + r.h;
}

void TestNobody()
{
    AutoFramer f;
    double t = 0;
    const FramingView v = Run(f, k4K, t, 5, 30, [](double) { return std::vector<FaceRect>{}; });
    Check(v.zoom == 1.0 && v.centerX == 0.5 && v.centerY == 0.5, "nobody: stays on the whole picture");
    Check(f.Faces() == 0, "nobody: 0 faces");
}

void TestOneFace()
{
    AutoFramer f;
    double t = 0;
    const FaceRect face = Face(k4K, 0.5, 0.45, 0.15);
    const FramingView v = Run(f, k4K, t, 6, 30, [&](double) { return std::vector<FaceRect>{ face }; });
    // Three face heights tall: 2160 / (3 * 324) = 2.22.
    Check(std::abs(v.zoom - 2160.0 / (3 * 0.15 * 2160)) < 0.02, "one face: head and shoulders, zoom " + Num(v.zoom));
    const Region r = Shown(v, k4K);
    Check(Inside(face, r, k4K), "one face: inside the shot");
    const double faceY = (face.y + face.h / 2) * k4K.frameHeight;
    Check(std::abs((faceY - r.y) / r.h - 0.40) < 0.01, "one face: 40% from the top, got " + Num((faceY - r.y) / r.h));
    Check(std::abs(v.centerX - 0.5) < 0.005, "one face: centred across");
    Check(f.Faces() == 1, "one face: 1 face");
}

void TestSteadyAndMove()
{
    AutoFramer f;
    double t = 0;
    Run(f, k4K, t, 5, 30, [&](double) { return std::vector<FaceRect>{ Face(k4K, 0.5, 0.45, 0.15) }; });
    const FramingView settled = f.Target();
    // Small movements (1% of the frame, slightly different sizes): the shot stays.
    int n = 0;
    Run(f, k4K, t, 5, 30, [&](double) {
        const double d = (n++ % 2 ? 0.01 : -0.01);
        return std::vector<FaceRect>{ Face(k4K, 0.5 + d, 0.45 - d, 0.15 + d / 2) };
    });
    const FramingView still = f.Target();
    Check(still.zoom == settled.zoom && still.centerX == settled.centerX && still.centerY == settled.centerY,
          "small movements keep the shot");
    // A real move (a quarter of the frame): the shot follows.
    const FramingView v = Run(f, k4K, t, 6, 30, [&](double) { return std::vector<FaceRect>{ Face(k4K, 0.75, 0.45, 0.15) }; });
    Check(std::abs(v.centerX - 0.75) < 0.01, "a big move: the shot follows, centreX " + Num(v.centerX));
}

void TestGoesWide()
{
    AutoFramer f;
    double t = 0;
    Run(f, k4K, t, 5, 30, [&](double) { return std::vector<FaceRect>{ Face(k4K, 0.5, 0.45, 0.15) }; });
    const auto none = [](double) { return std::vector<FaceRect>{}; };
    FramingView v = Run(f, k4K, t, 3, 30, none);
    Check(v.zoom > 2.0, "missed for 3 s: the shot holds, zoom " + Num(v.zoom));
    v = Run(f, k4K, t, 5, 30, none);
    Check(std::abs(v.zoom - 1) < 0.02 && std::abs(v.centerY - 0.5) < 0.01, "nobody for 8 s: back to the whole picture, zoom " + Num(v.zoom));
    // Someone comes back.
    v = Run(f, k4K, t, 6, 30, [&](double) { return std::vector<FaceRect>{ Face(k4K, 0.3, 0.45, 0.15) }; });
    Check(v.zoom > 2.0 && std::abs(v.centerX - 0.3) < 0.01, "someone again: framed again");
}

void TestSmartZoomLimit()
{
    Check(FramingMaxZoom(k4K) == 3.0, "4K: up to 3x (720 lines left)");
    Check(FramingMaxZoom(k1080) == 1.5, "1080p: 1.5x");
    Check(FramingMaxZoom(FramingGeometry{ 1280, 720, 1280, 720 }) == 1.5, "720p: at least 1.5x");
    Check(std::abs(FramingMaxZoom(FramingGeometry{ 2560, 1440, 2560, 1440 }) - 2.0) < 1e-9, "1440p: 2x");
    Check(FramingMaxZoom(kPortrait) == 1.5, "portrait 1080x1920: by its short side");

    for (const FramingGeometry* g : { &k4K, &k1080 })
    {
        AutoFramer f;
        double t = 0;
        const FramingView v = Run(f, *g, t, 8, 30, [&](double) { return std::vector<FaceRect>{ Face(*g, 0.5, 0.5, 0.04) }; });
        Check(std::abs(v.zoom - FramingMaxZoom(*g)) < 0.01, "a far face: zoom stops at the limit, " + Num(v.zoom));
    }
}

void TestEdges()
{
    AutoFramer f;
    double t = 0;
    const FaceRect face = Face(k4K, 0.04, 0.08, 0.12);
    const FramingView v = Run(f, k4K, t, 6, 30, [&](double) { return std::vector<FaceRect>{ face }; });
    const Region r = Shown(v, k4K);
    Check(r.x >= -1e-6 && r.y >= -1e-6 && r.x + r.w <= k4K.frameWidth + 1e-6 && r.y + r.h <= k4K.frameHeight + 1e-6,
          "a face at the corner: the shot stays inside the frame");
}

void TestGroupAndSmallFaces()
{
    // Two people side by side: both in the shot.
    AutoFramer f;
    double t = 0;
    const FaceRect a = Face(k4K, 0.30, 0.45, 0.14), b = Face(k4K, 0.70, 0.47, 0.13);
    FramingView v = Run(f, k4K, t, 6, 30, [&](double) { return std::vector<FaceRect>{ a, b }; });
    const Region r = Shown(v, k4K);
    Check(Inside(a, r, k4K) && Inside(b, r, k4K), "two people: both in the shot");
    Check(v.zoom > 1.0 && v.zoom < 2.0, "two people: wider than one, zoom " + Num(v.zoom));

    // A small face far behind (a poster, a TV) does not pull the shot wide.
    AutoFramer alone, withPoster;
    double t1 = 0, t2 = 0;
    const FaceRect me = Face(k4K, 0.4, 0.45, 0.15), poster = Face(k4K, 0.9, 0.2, 0.04);
    const FramingView v1 = Run(alone, k4K, t1, 6, 30, [&](double) { return std::vector<FaceRect>{ me }; });
    const FramingView v2 = Run(withPoster, k4K, t2, 6, 30, [&](double) { return std::vector<FaceRect>{ me, poster }; });
    Check(std::abs(v1.zoom - v2.zoom) < 1e-9 && std::abs(v1.centerX - v2.centerX) < 1e-9, "a small face far away is left out");
}

void TestPortrait()
{
    AutoFramer f;
    double t = 0;
    const FaceRect face = Face(kPortrait, 0.5, 0.4, 0.12);
    const FramingView v = Run(f, kPortrait, t, 6, 30, [&](double) { return std::vector<FaceRect>{ face }; });
    Check(v.zoom > 1.0 && v.zoom <= 1.5 + 1e-9, "portrait: zooms within its limit, " + Num(v.zoom));
    Check(Inside(face, Shown(v, kPortrait), kPortrait), "portrait: the face in the shot");
}

void TestFrameRate()
{
    const auto one = [](double) { return std::vector<FaceRect>{ Face(k4K, 0.6, 0.45, 0.15) }; };
    AutoFramer a, b;
    double ta = 0, tb = 0;
    const FramingView va = Run(a, k4K, ta, 1.0, 30, one);
    const FramingView vb = Run(b, k4K, tb, 1.0, 60, one);
    Check(std::abs(va.zoom - vb.zoom) / va.zoom < 0.01 && std::abs(va.centerX - vb.centerX) < 0.005,
          "30 and 60 fps ease alike: " + Num(va.zoom) + " vs " + Num(vb.zoom));
    Check(va.zoom > 1.2 && va.zoom < 2.1, "after 1 s the zoom is on its way, " + Num(va.zoom));
}

void TestResetAndShape()
{
    AutoFramer f;
    double t = 0;
    Run(f, k4K, t, 5, 30, [&](double) { return std::vector<FaceRect>{ Face(k4K, 0.5, 0.45, 0.15) }; });
    f.Reset();
    const FramingView v = f.Step(k4K, t + 0.033);
    Check(v.zoom == 1.0 && v.centerX == 0.5, "reset: the whole picture at once");
    Check(f.Faces() == 0, "reset: no faces");
}

void TestDetectionImage()
{
    uint32_t w = 0, h = 0;
    DetectionSize(3840, 2160, 640, w, h);
    Check(w == 640 && h == 360, "detection size 4K");
    DetectionSize(1080, 1920, 640, w, h);
    Check(w == 360 && h == 640, "detection size portrait");
    DetectionSize(320, 240, 640, w, h);
    Check(w == 320 && h == 240, "detection size: small frames stay");
    DetectionSize(880, 1920, 640, w, h);
    Check(w == 292 && h == 640, "detection size: even sides (NV12), got " + std::to_string(w) + "x" + std::to_string(h));
    DetectionSize(1281, 721, 2000, w, h);
    Check(w == 1280 && h == 720, "detection size: odd small frame made even");

    // Constant grey stays constant; a gradient stays a gradient.
    const uint32_t sw = 1920, sh = 1080;
    const long pitch = sw + 64; // padded rows
    std::vector<uint8_t> src(static_cast<size_t>(pitch) * sh, 77);
    std::vector<uint8_t> dst(640 * 360);
    DownscaleLuma(src.data(), pitch, sw, sh, dst.data(), 640, 360);
    bool flat = true;
    for (uint8_t v : dst) flat = flat && v == 77;
    Check(flat, "downscale: flat grey");

    for (uint32_t y = 0; y < sh; ++y)
        for (uint32_t x = 0; x < sw; ++x) src[y * pitch + x] = static_cast<uint8_t>(x * 255 / (sw - 1));
    DownscaleLuma(src.data(), pitch, sw, sh, dst.data(), 640, 360);
    bool gradient = true;
    for (uint32_t y = 0; y < 360; ++y)
        for (uint32_t x = 0; x < 640; ++x)
        {
            const double centre = (x + 0.5) * sw / 640;
            const double want = centre * 255 / (sw - 1);
            gradient = gradient && std::abs(dst[y * 640 + x] - want) <= 2;
            if (x) gradient = gradient && dst[y * 640 + x] >= dst[y * 640 + x - 1];
        }
    Check(gradient, "downscale: gradient kept");

    // Odd sizes stay inside the source.
    std::vector<uint8_t> odd(7 * 5, 200), out(3 * 2);
    DownscaleLuma(odd.data(), 7, 7, 5, out.data(), 3, 2);
    Check(out[0] == 200 && out[5] == 200, "downscale: odd sizes");
}
} // namespace

int main()
{
    TestNobody();
    TestOneFace();
    TestSteadyAndMove();
    TestGoesWide();
    TestSmartZoomLimit();
    TestEdges();
    TestGroupAndSmallFaces();
    TestPortrait();
    TestFrameRate();
    TestResetAndShape();
    TestDetectionImage();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
