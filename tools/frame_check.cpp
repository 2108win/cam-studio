// Checks the portable picture code: the virtual camera size rule (PlanOutput), the "fit" layout
// (ComputeLayout with a fixed output), moving the view (SetLayoutView), black bars, pitch-aware
// scale/adjust into a rectangle, and BGRA <-> NV12. Builds on Windows (camstudio-frame-check.exe) and on a Mac. Nv12.h uses SSE2,
// so on Apple silicon build for x86_64 (runs under Rosetta):
//   clang++ -std=c++20 -arch x86_64 -O1 -Wall -Wextra -o build/frame-check tools/frame_check.cpp && build/frame-check
#include "../src/engine/ImageSettings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

std::string Size(uint32_t w, uint32_t h) { return std::to_string(w) + "x" + std::to_string(h); }

size_t Nv12Bytes(uint32_t w, uint32_t h) { return static_cast<size_t>(w) * h * 3 / 2; }

// ---- the size rule -------------------------------------------------------------------------

void TestPlan()
{
    auto expect = [](const char* name, OutputPlan p, uint32_t w, uint32_t h, bool rebuild) {
        Check(p.width == w && p.height == h && p.rebuild == rebuild,
              std::string("plan ") + name + ": got " + Size(p.width, p.height) + (p.rebuild ? " rebuild" : " keep") +
              ", want " + Size(w, h) + (rebuild ? " rebuild" : " keep"));
    };
    // Tự động: nobody uses the camera -> the source's own size.
    expect("auto first", PlanOutput(OutputSize::Auto, 1920, 1080, 0, 0, false), 1920, 1080, true);
    expect("auto same", PlanOutput(OutputSize::Auto, 1920, 1080, 1920, 1080, true), 1920, 1080, false);
    expect("auto new size, free", PlanOutput(OutputSize::Auto, 1080, 1440, 1920, 1080, false), 1080, 1440, true);
    // In a call: keep the size, the new picture is fitted.
    expect("auto new size, in use", PlanOutput(OutputSize::Auto, 1080, 1440, 1920, 1080, true), 1920, 1080, false);
    // Even the first publish ignores a consumer: there is no camera to keep yet.
    expect("auto first, in use", PlanOutput(OutputSize::Auto, 3840, 2160, 0, 0, true), 3840, 2160, true);
    // Fixed sizes.
    expect("1080p portrait picture", PlanOutput(OutputSize::R1080, 1080, 1920, 0, 0, false), 1920, 1080, true);
    expect("1080p kept", PlanOutput(OutputSize::R1080, 3840, 2160, 1920, 1080, false), 1920, 1080, false);
    expect("4K from 1080p, free", PlanOutput(OutputSize::R2160, 1920, 1080, 1920, 1080, false), 3840, 2160, true);
    expect("720p from 1080p, in use", PlanOutput(OutputSize::R720, 1920, 1080, 1920, 1080, true), 1920, 1080, false);
    expect("back to auto, free", PlanOutput(OutputSize::Auto, 1280, 720, 1920, 1080, false), 1280, 720, true);
}

// ---- fit layout ----------------------------------------------------------------------------

void ExpectFit(const char* name, const FrameLayout& l, uint32_t fitW, uint32_t fitH)
{
    const std::string n = std::string("fit ") + name + ": ";
    Check(l.fitted, n + "fitted");
    Check(l.outWidth == fitW && l.outHeight == fitH, n + "output " + Size(l.outWidth, l.outHeight));
    Check(l.needsResample, n + "resamples");
    Check(l.destX % 2 == 0 && l.destY % 2 == 0 && l.destWidth % 2 == 0 && l.destHeight % 2 == 0,
          n + "even rect " + std::to_string(l.destX) + "," + std::to_string(l.destY) + " " + Size(l.destWidth, l.destHeight));
    Check(l.destX + l.destWidth <= fitW && l.destY + l.destHeight <= fitH, n + "rect inside the output");
    // Centred within 2 px, and one side fills the output.
    const int dx = static_cast<int>(fitW - l.destWidth) - 2 * static_cast<int>(l.destX);
    const int dy = static_cast<int>(fitH - l.destHeight) - 2 * static_cast<int>(l.destY);
    Check(dx >= 0 && dx <= 2 && dy >= 0 && dy <= 2, n + "centred");
    Check(l.destWidth == fitW || l.destHeight == fitH, n + "touches two sides");
    // Aspect kept (within the 2 px rounding).
    const double want = static_cast<double>(l.pictureWidth) / l.pictureHeight;
    const double got = static_cast<double>(l.destWidth) / l.destHeight;
    const double tolerance = 2.0 / std::min(l.destWidth, l.destHeight) * want + 1e-9;
    Check(std::fabs(got - want) <= tolerance, n + "aspect " + std::to_string(got) + " vs " + std::to_string(want));
}

void TestFitLayout()
{
    ImageSettings plain;
    {
        const FrameLayout l = ComputeLayout(1920, 1080, plain);
        Check(!l.fitted && l.outWidth == 1920 && l.outHeight == 1080 && l.pictureWidth == 1920 &&
              l.destWidth == 1920 && l.destHeight == 1080 && l.destX == 0 && l.destY == 0, "no fit: picture = output");
        Check(!l.needsResample && !l.needsOrient, "no fit: straight copy");
        const FrameLayout same = ComputeLayout(1920, 1080, plain, 1920, 1080);
        Check(!same.fitted && !same.needsResample, "fit to the same size is no fit");
    }
    {
        // Phone held upright (sensor 1920x1080 turned 90) into a call's 1080p camera: pillarbox.
        ImageSettings turned;
        turned.orientation.rotation = 90;
        const FrameLayout l = ComputeLayout(1920, 1080, turned, 1920, 1080);
        ExpectFit("portrait into 1080p", l, 1920, 1080);
        Check(l.pictureWidth == 1080 && l.pictureHeight == 1920, "portrait picture size");
        Check(l.destHeight == 1080 && l.destWidth == 608 && l.destX == 656 && l.destY == 0,
              "portrait rect " + std::to_string(l.destX) + "," + std::to_string(l.destY) + " " + Size(l.destWidth, l.destHeight));
        Check(l.needsOrient, "portrait still rotates");
    }
    {
        // Same aspect, smaller: scaled up to fill, no bars.
        const FrameLayout l = ComputeLayout(1280, 720, plain, 1920, 1080);
        ExpectFit("720p into 1080p", l, 1920, 1080);
        Check(l.destX == 0 && l.destY == 0 && l.destWidth == 1920 && l.destHeight == 1080, "720p fills 1080p");
    }
    {
        const FrameLayout l = ComputeLayout(1440, 1080, plain, 1920, 1080);
        ExpectFit("4:3 into 16:9", l, 1920, 1080);
        Check(l.destX == 240 && l.destWidth == 1440, "4:3 rect");
    }
    {
        // Letterbox: wide into tall.
        const FrameLayout l = ComputeLayout(1920, 1080, plain, 1080, 1440);
        ExpectFit("16:9 into 3:4", l, 1080, 1440);
        Check(l.destX == 0 && l.destWidth == 1080 && l.destHeight == 608 && l.destY == 416,
              "letterbox rect " + std::to_string(l.destY) + " " + Size(l.destWidth, l.destHeight));
    }
    {
        // Crop to 1:1 first, then fit; zoom only moves the source rectangle.
        ImageSettings square;
        square.aspect = CropAspect::R1x1;
        square.zoom = 2.0;
        square.centerX = 0.3;
        const FrameLayout natural = ComputeLayout(1920, 1080, square);
        const FrameLayout l = ComputeLayout(1920, 1080, square, 1280, 720);
        ExpectFit("1:1 zoomed into 720p", l, 1280, 720);
        Check(l.destWidth == 720 && l.destHeight == 720 && l.destX == 280, "1:1 rect");
        Check(l.source.x == natural.source.x && l.source.y == natural.source.y && l.source.w == natural.source.w &&
              l.source.h == natural.source.h, "fit leaves the zoom region alone");
    }
    {
        // Odd output sizes are made even.
        const FrameLayout l = ComputeLayout(1920, 1080, plain, 1001, 601);
        ExpectFit("odd output", l, 1000, 600);
    }
    {
        // Tiny pictures still give a valid rectangle.
        const FrameLayout l = ComputeLayout(2, 200, plain, 1920, 1080);
        ExpectFit("sliver", l, 1920, 1080);
    }
}

// ---- NV12 bars, scale and adjust into a rectangle --------------------------------------------

std::vector<uint8_t> Pattern(uint32_t w, uint32_t h, unsigned seed)
{
    std::vector<uint8_t> f(Nv12Bytes(w, h));
    for (size_t i = 0; i < f.size(); ++i) f[i] = static_cast<uint8_t>((i * 31 + seed * 17 + (i / w) * 7) % 220 + 20);
    return f;
}

void TestBars()
{
    const uint32_t w = 64, h = 36, rx = 8, ry = 4, rw = 20, rh = 10;
    std::vector<uint8_t> f(Nv12Bytes(w, h), 77);
    Nv12FillBars(f.data(), w, h, rx, ry, rw, rh);
    bool lumaOk = true, chromaOk = true;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            const bool inside = x >= rx && x < rx + rw && y >= ry && y < ry + rh;
            lumaOk &= f[y * w + x] == (inside ? 77 : kNv12BlackY);
        }
    const uint8_t* uv = f.data() + w * h;
    for (uint32_t y = 0; y < h / 2; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            const bool inside = x >= rx && x < rx + rw && y >= ry / 2 && y < (ry + rh) / 2;
            chromaOk &= uv[y * w + x] == (inside ? 77 : kNv12BlackUv);
        }
    Check(lumaOk, "bars: luma black outside, untouched inside");
    Check(chromaOk, "bars: chroma neutral outside, untouched inside");

    std::vector<uint8_t> full(Nv12Bytes(w, h), 77);
    Nv12FillBars(full.data(), w, h, 0, 0, w, h);
    Check(std::all_of(full.begin(), full.end(), [](uint8_t v) { return v == 77; }), "bars: a full rectangle paints nothing");
}

void TestScaleIntoRect()
{
    const uint32_t sw = 96, sh = 54;
    const std::vector<uint8_t> src = Pattern(sw, sh, 1);
    const Nv12Rect r{ 4.5, 2.0, 80.0, 45.0 };
    const uint32_t dw = 40, dh = 22;

    std::vector<uint8_t> plain(Nv12Bytes(dw, dh));
    Nv12CropScaleBilinear(src.data(), sw, sw, sh, r, plain.data(), dw, dh);

    // The same picture at (10, 6) inside a 64x36 frame full of sentinels.
    const uint32_t fw = 64, fh = 36, x0 = 10, y0 = 6;
    std::vector<uint8_t> frame(Nv12Bytes(fw, fh), 3);
    uint8_t* dstY = frame.data() + y0 * fw + x0;
    uint8_t* dstUv = frame.data() + fw * fh + (y0 / 2) * fw + x0;
    Nv12CropScaleBilinearTo(src.data(), sw, sw, sh, r, dstY, dstUv, fw, dw, dh);

    bool same = true, outsideUntouched = true;
    for (uint32_t y = 0; y < fh; ++y)
        for (uint32_t x = 0; x < fw; ++x)
        {
            const bool inside = x >= x0 && x < x0 + dw && y >= y0 && y < y0 + dh;
            const uint8_t v = frame[y * fw + x];
            if (inside) same &= v == plain[(y - y0) * dw + (x - x0)];
            else outsideUntouched &= v == 3;
        }
    for (uint32_t y = 0; y < fh / 2; ++y)
        for (uint32_t x = 0; x < fw; ++x)
        {
            const bool inside = x >= x0 && x < x0 + dw && y >= y0 / 2 && y < (y0 + dh) / 2;
            const uint8_t v = frame[fw * fh + y * fw + x];
            if (inside) same &= v == plain[dw * dh + (y - y0 / 2) * dw + (x - x0)];
            else outsideUntouched &= v == 3;
        }
    Check(same, "scale into a rectangle = scale into its own frame");
    Check(outsideUntouched, "scale into a rectangle writes nothing outside it");
}

void TestAdjustRect()
{
    const uint32_t fw = 64, fh = 36, x0 = 12, y0 = 4, rw = 32, rh = 20;
    std::vector<uint8_t> frame = Pattern(fw, fh, 2);
    Nv12FillBars(frame.data(), fw, fh, x0, y0, rw, rh);

    // The rectangle on its own, adjusted with the old contiguous function.
    std::vector<uint8_t> alone(Nv12Bytes(rw, rh));
    for (uint32_t y = 0; y < rh; ++y) memcpy(&alone[y * rw], &frame[(y0 + y) * fw + x0], rw);
    for (uint32_t y = 0; y < rh / 2; ++y) memcpy(&alone[rw * rh + y * rw], &frame[fw * fh + (y0 / 2 + y) * fw + x0], rw);
    Nv12ColorAdjust c;
    c.brightness = 40;
    c.contrast = -20;
    c.saturation = 30;
    c.temperature = 25;
    std::vector<uint8_t> expected(alone.size());
    Nv12Adjust(alone.data(), rw, expected.data(), rw, rh, c);

    uint8_t* y = frame.data() + y0 * fw + x0;
    uint8_t* uv = frame.data() + fw * fh + (y0 / 2) * fw + x0;
    Nv12AdjustPlanes(y, uv, fw, y, uv, fw, rw, rh, c); // in place

    bool same = true, barsBlack = true;
    for (uint32_t yy = 0; yy < fh; ++yy)
        for (uint32_t x = 0; x < fw; ++x)
        {
            const bool inside = x >= x0 && x < x0 + rw && yy >= y0 && yy < y0 + rh;
            const uint8_t v = frame[yy * fw + x];
            if (inside) same &= v == expected[(yy - y0) * rw + (x - x0)];
            else barsBlack &= v == kNv12BlackY;
        }
    for (uint32_t yy = 0; yy < fh / 2; ++yy)
        for (uint32_t x = 0; x < fw; ++x)
        {
            const bool inside = x >= x0 && x < x0 + rw && yy >= y0 / 2 && yy < (y0 + rh) / 2;
            const uint8_t v = frame[fw * fh + yy * fw + x];
            if (inside) same &= v == expected[rw * rh + (yy - y0 / 2) * rw + (x - x0)];
            else barsBlack &= v == kNv12BlackUv;
        }
    Check(same, "adjust in place on a rectangle = adjust the rectangle alone");
    Check(barsBlack, "adjust on a rectangle leaves the bars black");
}

// ---- BGRA <-> NV12 ---------------------------------------------------------------------------

struct Rgb { int r, g, b; };

std::vector<uint8_t> Solid(uint32_t w, uint32_t h, size_t pitch, Rgb c)
{
    std::vector<uint8_t> img(pitch * h, 0xEE); // pitch padding stays 0xEE
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t* p = &img[y * pitch + x * 4];
            p[0] = static_cast<uint8_t>(c.b);
            p[1] = static_cast<uint8_t>(c.g);
            p[2] = static_cast<uint8_t>(c.r);
            p[3] = 255;
        }
    return img;
}

void TestBgraToNv12()
{
    const uint32_t w = 8, h = 4;
    const size_t pitch = w * 4 + 16; // GPU rows are often padded
    auto convert = [&](Rgb c, uint8_t& y, uint8_t& u, uint8_t& v) {
        const std::vector<uint8_t> img = Solid(w, h, pitch, c);
        std::vector<uint8_t> nv(Nv12Bytes(w, h));
        BgraToNv12(img.data(), pitch, w, h, nv.data(), nv.data() + w * h, w);
        y = nv[w + 3];
        u = nv[w * h + w + 2];
        v = nv[w * h + w + 3];
    };
    uint8_t y, u, v;
    convert({ 255, 255, 255 }, y, u, v);
    Check(y == 235 && u == 128 && v == 128, "white -> 235/128/128, got " + std::to_string(y) + "/" + std::to_string(u) + "/" + std::to_string(v));
    convert({ 0, 0, 0 }, y, u, v);
    Check(y == 16 && u == 128 && v == 128, "black -> 16/128/128");
    convert({ 255, 0, 0 }, y, u, v);
    Check(y == 82 && u == 90 && v == 240, "red -> 82/90/240, got " + std::to_string(y) + "/" + std::to_string(u) + "/" + std::to_string(v));
    convert({ 0, 0, 255 }, y, u, v);
    Check(y == 41 && u == 240 && v == 110, "blue -> 41/240/110, got " + std::to_string(y) + "/" + std::to_string(u) + "/" + std::to_string(v));

    // A 2x2 block half white, half black: luma per pixel, chroma neutral.
    std::vector<uint8_t> img = Solid(2, 2, 8, { 0, 0, 0 });
    for (int i = 0; i < 3; ++i) img[i] = 255; // top-left pixel white
    for (int i = 0; i < 3; ++i) img[8 + 4 + i] = 255; // bottom-right white
    std::vector<uint8_t> nv(Nv12Bytes(2, 2));
    BgraToNv12(img.data(), 8, 2, 2, nv.data(), nv.data() + 4, 2);
    Check(nv[0] == 235 && nv[1] == 16 && nv[2] == 16 && nv[3] == 235 && nv[4] == 128 && nv[5] == 128, "2x2 block: luma per pixel, chroma averaged");

    // Round trip for flat colours stays within a few levels (limited range quantisation).
    int worst = 0;
    for (int r = 0; r <= 255; r += 51)
        for (int g = 0; g <= 255; g += 51)
            for (int b = 0; b <= 255; b += 51)
            {
                const std::vector<uint8_t> src = Solid(w, h, pitch, { r, g, b });
                std::vector<uint8_t> n(Nv12Bytes(w, h));
                BgraToNv12(src.data(), pitch, w, h, n.data(), n.data() + w * h, w);
                std::vector<uint32_t> back(w * h);
                Nv12ToBgra(n.data(), n.data() + w * h, w, w, h, back.data(), w, h);
                const uint32_t p = back[5];
                worst = std::max({ worst, std::abs(static_cast<int>((p >> 16) & 255) - r),
                                   std::abs(static_cast<int>((p >> 8) & 255) - g), std::abs(static_cast<int>(p & 255) - b) });
                Check((p >> 24) == 255, "round trip: opaque");
            }
    Check(worst <= 4, "round trip within 4 levels, worst " + std::to_string(worst));
}

void TestNv12ToBgraScaled()
{
    // Left half black, right half white (luma only), chroma neutral.
    const uint32_t w = 16, h = 8;
    std::vector<uint8_t> nv(Nv12Bytes(w, h), 128);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) nv[y * w + x] = x < w / 2 ? 16 : 235;
    std::vector<uint32_t> out(4 * 2);
    Nv12ToBgra(nv.data(), nv.data() + w * h, w, w, h, out.data(), 4, 2);
    Check(out[0] == 0xFF000000u && out[1] == 0xFF000000u && out[2] == 0xFFFFFFFFu && out[3] == 0xFFFFFFFFu,
          "downscaled preview keeps the halves");
}

// ---- moving the view (automatic framing) ---------------------------------------------------

void TestView()
{
    auto same = [](const Nv12Rect& a, const Nv12Rect& b) {
        return std::abs(a.x - b.x) < 1e-9 && std::abs(a.y - b.y) < 1e-9 && std::abs(a.w - b.w) < 1e-9 && std::abs(a.h - b.h) < 1e-9;
    };
    // SetLayoutView on a layout gives what ComputeLayout gives for the same zoom and centre.
    const double zooms[] = { 1.0, 1.37, 2.0, 3.5 };
    const double centres[][2] = { { 0.5, 0.5 }, { 0.1, 0.9 }, { 0.93, 0.07 } };
    for (double z : zooms)
        for (const auto& c : centres)
            for (int aspect = 0; aspect <= 4; ++aspect)
            {
                ImageSettings s;
                s.aspect = static_cast<CropAspect>(aspect);
                FrameLayout moved = ComputeLayout(3840, 2160, s);
                s.zoom = z;
                s.centerX = c[0];
                s.centerY = c[1];
                const FrameLayout want = ComputeLayout(3840, 2160, s);
                SetLayoutView(moved, z, c[0], c[1]);
                Check(same(moved.source, want.source) && moved.needsResample == want.needsResample,
                      "view matches ComputeLayout: zoom " + std::to_string(z) + " aspect " + std::to_string(aspect));
            }

    // Kept inside the frame at the edges.
    FrameLayout l = ComputeLayout(1920, 1080, ImageSettings{});
    SetLayoutView(l, 2.0, 0.0, 1.0);
    Check(l.source.x == 0 && std::abs(l.source.y - 540) < 1e-9 && l.source.w == 960 && l.source.h == 540, "view clamped to the corner");
    SetLayoutView(l, 2.0, 1.0, 0.0);
    Check(std::abs(l.source.x - 960) < 1e-9 && l.source.y == 0, "view clamped to the other corner");

    // Zoom 1 of an aspect crop: even whole pixels, so the copy stays exact.
    ImageSettings square;
    square.aspect = CropAspect::R1x1;
    l = ComputeLayout(1920, 1080, square);
    SetLayoutView(l, 1.0, 0.3011, 0.5);
    Check(static_cast<int>(l.source.x) % 2 == 0 && l.source.x == std::floor(l.source.x), "zoom 1 crop on even pixels");

    // needsResample follows the view both ways.
    l = ComputeLayout(1920, 1080, ImageSettings{});
    Check(!l.needsResample, "native picture needs no resample");
    SetLayoutView(l, 1.6, 0.5, 0.5);
    Check(l.needsResample, "framing zoom needs a resample");
    SetLayoutView(l, 1.0, 0.5, 0.5);
    Check(!l.needsResample, "back to zoom 1: no resample");

    // A fitted layout keeps its output and bars; only the source region moves.
    l = ComputeLayout(1080, 1920, ImageSettings{}, 1920, 1080);
    const FrameLayout before = l;
    SetLayoutView(l, 2.5, 0.4, 0.3);
    Check(l.fitted && l.needsResample && l.outWidth == before.outWidth && l.outHeight == before.outHeight &&
          l.destX == before.destX && l.destY == before.destY && l.destWidth == before.destWidth &&
          l.destHeight == before.destHeight, "fitted view keeps the bars");
    Check(std::abs(l.source.w - 1080 / 2.5) < 1e-9 && std::abs(l.source.h - 1920 / 2.5) < 1e-9, "fitted view size");
}
} // namespace

int main()
{
    TestPlan();
    TestFitLayout();
    TestBars();
    TestScaleIntoRect();
    TestAdjustRect();
    TestBgraToNv12();
    TestNv12ToBgraScaled();
    TestView();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
