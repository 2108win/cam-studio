// Verifies step 1 end to end, the way a consumer app (Zoom, OBS...) sees it:
//   1. the "Cam Studio" virtual camera exists,
//   2. its largest format equals the physical camera's native resolution,
//   3. 30 frames can be read at that size, and they are live video rather
//      than the "no signal" pattern.
//
//   camstudio-verify [--device <idx|name>] [--frames N]
// Exit code 0 = PASS.

#include "../common/MfDevices.h"

#include <cstdio>
#include <io.h>
#include <fcntl.h>

using Microsoft::WRL::ComPtr;

namespace
{
int Fail(const wchar_t* msg)
{
    fwprintf(stderr, L"FAIL: %s\n", msg);
    return 1;
}

// Fraction of luma pixels equal to the "no signal" background (0x30).
double NoSignalRatio(const uint8_t* y, long pitch, uint32_t w, uint32_t h)
{
    uint64_t hits = 0, total = 0;
    for (uint32_t row = 0; row < h; row += 8)
    {
        const uint8_t* p = y + static_cast<size_t>(row) * pitch;
        for (uint32_t x = 0; x < w; x += 8, ++total)
            if (p[x] == 0x30) ++hits;
    }
    return total ? static_cast<double>(hits) / total : 1.0;
}

// Writes an NV12 frame as a 24-bit BMP (BT.601 limited range) so the colours
// can be checked by eye.
bool SaveNv12AsBmp(const std::wstring& path, const uint8_t* nv12, uint32_t w, uint32_t h)
{
    const uint32_t rowBytes = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> pixels(static_cast<size_t>(rowBytes) * h);
    const uint8_t* uvPlane = nv12 + static_cast<size_t>(w) * h;
    for (uint32_t y = 0; y < h; ++y)
    {
        uint8_t* out = pixels.data() + static_cast<size_t>(h - 1 - y) * rowBytes; // bottom-up
        const uint8_t* uv = uvPlane + static_cast<size_t>(y / 2) * w;
        for (uint32_t x = 0; x < w; ++x)
        {
            const int c = nv12[static_cast<size_t>(y) * w + x] - 16;
            const int d = uv[x & ~1u] - 128;
            const int e = uv[(x & ~1u) + 1] - 128;
            auto clamp = [](int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
            out[x * 3 + 0] = clamp((298 * c + 516 * d + 128) >> 8);           // B
            out[x * 3 + 1] = clamp((298 * c - 100 * d - 208 * e + 128) >> 8); // G
            out[x * 3 + 2] = clamp((298 * c + 409 * e + 128) >> 8);           // R
        }
    }

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = static_cast<LONG>(h);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biSizeImage = static_cast<DWORD>(pixels.size());
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);
    return true;
}
} // namespace

// --reopen: behave like a video-call app (Discord) that, when the camera
// disappears, immediately looks it up again and reopens it. Logs each open
// and how it ended, so a virtual camera that cannot come back is visible.
int ReopenLoop(int seconds)
{
    const ULONGLONG start = GetTickCount64();
    auto at = [&] { return (GetTickCount64() - start) / 1000.0; };
    int opens = 0;
    while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000)
    {
        auto cams = EnumerateCameras();
        const CameraDevice* vcam = nullptr;
        for (const auto& c : cams) if (IsOurVirtualCamera(c)) vcam = &c;
        if (!vcam) { Sleep(20); continue; }
        ComPtr<IMFMediaSource> source;
        HRESULT hr = vcam->activate->ActivateObject(IID_PPV_ARGS(&source));
        ComPtr<IMFSourceReader> reader;
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
        if (FAILED(hr))
        {
            wprintf(L"[%6.1f s] open failed 0x%08X\n", at(), static_cast<unsigned>(hr));
            fflush(stdout);
            vcam->activate->ShutdownObject();
            Sleep(20);
            continue;
        }
        ++opens;
        int frames = 0;
        for (;;)
        {
            DWORD si = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            hr = reader->ReadSample(kVideoStream, 0, &si, &flags, &ts, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) break;
            if (sample && frames++ == 0) { wprintf(L"[%6.1f s] open #%d streaming\n", at(), opens); fflush(stdout); }
            if (GetTickCount64() - start >= static_cast<ULONGLONG>(seconds) * 1000) break;
        }
        wprintf(L"[%6.1f s] open #%d ended after %d frames (0x%08X)\n", at(), opens, frames, static_cast<unsigned>(hr));
        fflush(stdout);
        reader.Reset();
        source->Shutdown();
        vcam->activate->ShutdownObject();
    }
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);

    std::wstring selector;
    std::wstring dumpPath;
    int frameTarget = 30;
    int modeIndex = -1; // --mode: read this virtual camera format instead of the largest
    int holdSeconds = 0; // --hold: stay open like a video call, logging every 2 s
    int reopenSeconds = 0; // --reopen: reopen at once whenever the camera goes away (like Discord)
    UINT32 expectW = 0, expectH = 0;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--device" && i + 1 < argc) selector = argv[++i];
        else if (a == L"--frames" && i + 1 < argc) frameTarget = _wtoi(argv[++i]);
        else if (a == L"--dump" && i + 1 < argc) dumpPath = argv[++i];
        else if (a == L"--mode" && i + 1 < argc) modeIndex = _wtoi(argv[++i]);
        else if (a == L"--hold" && i + 1 < argc) holdSeconds = _wtoi(argv[++i]);
        else if (a == L"--reopen" && i + 1 < argc) reopenSeconds = _wtoi(argv[++i]);
        else if (a == L"--expect" && i + 1 < argc)
        {
            // For sources that are not a local webcam (phones): the size the
            // virtual camera must deliver, e.g. --expect 1920x1080.
            if (swscanf_s(argv[++i], L"%ux%u", &expectW, &expectH) != 2) return Fail(L"--expect wants WxH");
        }
        else
        {
            fwprintf(stderr, L"Usage: camstudio-verify [--device <index|name> | --expect WxH] [--mode N] [--frames N | --hold seconds | --reopen seconds] [--dump frame.bmp]\n");
            return 2;
        }
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION)))
        return Fail(L"MF init");

    if (reopenSeconds > 0)
    {
        const int rc = ReopenLoop(reopenSeconds);
        MFShutdown();
        CoUninitialize();
        return rc;
    }

    auto cams = EnumerateCameras();

    NativeFormat expected;
    if (expectW && expectH)
    {
        expected.width = expectW;
        expected.height = expectH;
        wprintf(L"Expected        : %ux%u\n", expectW, expectH);
    }
    else
    {
        // Expected: the physical camera's largest native format.
        const CameraDevice* physical = SelectCamera(cams, selector);
        if (!physical) return Fail(L"physical camera not found (use --device or --expect)");
        ComPtr<IMFMediaSource> src;
        if (FAILED(physical->activate->ActivateObject(IID_PPV_ARGS(&src)))) return Fail(L"cannot open physical camera");
        std::vector<NativeFormat> formats;
        GetNativeFormats(src.Get(), formats);
        const NativeFormat* best = PickLargest(formats);
        if (!best) return Fail(L"physical camera has no formats");
        expected = *best;
        physical->activate->ShutdownObject();
        wprintf(L"Physical camera : %s, native %ux%u @ %.2f fps\n", physical->name.c_str(),
            expected.width, expected.height, expected.Fps());
    }

    const CameraDevice* vcam = nullptr;
    for (const auto& c : cams) if (IsOurVirtualCamera(c)) vcam = &c;
    if (!vcam) return Fail(L"\"Cam Studio\" virtual camera not found (is CamStudio.exe running?)");

    ComPtr<IMFMediaSource> source;
    if (FAILED(vcam->activate->ActivateObject(IID_PPV_ARGS(&source)))) return Fail(L"cannot open virtual camera");

    std::vector<NativeFormat> formats;
    ComPtr<IMFMediaTypeHandler> handler;
    if (FAILED(GetNativeFormats(source.Get(), formats, &handler))) return Fail(L"cannot read virtual camera formats");
    wprintf(L"Virtual camera  : %zu formats\n", formats.size());
    for (const auto& f : formats)
        wprintf(L"    [%2u] %s %ux%u @ %.2f\n", f.index, SubtypeName(f.subtype).c_str(), f.width, f.height, f.Fps());

    const NativeFormat* largest = PickLargest(formats);
    if (!largest) return Fail(L"virtual camera has no formats");
    if (modeIndex >= 0)
    {
        // Read like an app that picked a smaller mode (Discord: 30 fps).
        auto it = std::find_if(formats.begin(), formats.end(), [&](const NativeFormat& f) { return static_cast<int>(f.index) == modeIndex; });
        if (it == formats.end()) return Fail(L"--mode: no such format index");
        largest = &*it;
        expected.width = it->width;
        expected.height = it->height;
        wprintf(L"Reading mode    : [%u] %ux%u @ %.2f\n", it->index, it->width, it->height, it->Fps());
    }
    if (largest->width != expected.width || largest->height != expected.height)
    {
        fwprintf(stderr, L"FAIL: largest virtual format %ux%u != native %ux%u\n",
            largest->width, largest->height, expected.width, expected.height);
        return 1;
    }

    ComPtr<IMFMediaType> type;
    handler->GetMediaTypeByIndex(largest->index, &type);
    if (FAILED(handler->SetCurrentMediaType(type.Get()))) return Fail(L"cannot select largest format");

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader))) return Fail(L"source reader");

    int got = 0, live = 0, sizeErrors = 0;
    LONGLONG prevTs = -1;            // sample timestamps: how even the cadence is
    double minGapMs = 1e9, maxGapMs = 0;
    const ULONGLONG start = GetTickCount64();
    ULONGLONG firstFrame = 0;
    ULONGLONG lastReport = start;
    int reportGot = 0, reportLive = 0;
    auto keepGoing = [&] {
        const ULONGLONG elapsed = GetTickCount64() - start;
        return holdSeconds > 0 ? elapsed < static_cast<ULONGLONG>(holdSeconds) * 1000
                               : got < frameTarget && elapsed < 20000;
    };
    while (keepGoing())
    {
        if (holdSeconds > 0 && GetTickCount64() - lastReport >= 2000)
        {
            wprintf(L"[%5.1f s] %d frames, %d live\n", (GetTickCount64() - start) / 1000.0, got - reportGot, live - reportLive);
            fflush(stdout);
            reportGot = got;
            reportLive = live;
            lastReport = GetTickCount64();
        }
        DWORD si = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(kVideoStream, 0, &si, &flags, &ts, &sample);
        if (FAILED(hr))
        {
            wprintf(L"[%5.1f s] ReadSample 0x%08X (camera gone for this app)\n", (GetTickCount64() - start) / 1000.0, hr);
            fflush(stdout);
            break;
        }
        if (flags & MF_SOURCE_READERF_ERROR) break;
        if (!sample) continue;
        if (!firstFrame) firstFrame = GetTickCount64();
        if (prevTs >= 0 && got > 5) // skip start-up
        {
            const double gap = (ts - prevTs) / 10000.0;
            minGapMs = std::min(minGapMs, gap);
            maxGapMs = std::max(maxGapMs, gap);
        }
        prevTs = ts;

        ComPtr<IMFMediaType> cur;
        reader->GetCurrentMediaType(kVideoStream, &cur);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (w != expected.width || h != expected.height) ++sizeErrors;

        ComPtr<IMFMediaBuffer> buf;
        sample->ConvertToContiguousBuffer(&buf);
        BYTE* data = nullptr;
        DWORD len = 0;
        if (buf && SUCCEEDED(buf->Lock(&data, nullptr, &len)))
        {
            if (len < CamStudioNv12Bytes(w, h)) ++sizeErrors;
            else
            {
                if (NoSignalRatio(data, static_cast<long>(w), w, h) < 0.5) ++live;
                if (!dumpPath.empty() && got == frameTarget - 1)
                    wprintf(L"Saved frame     : %s (%s)\n", dumpPath.c_str(),
                        SaveNv12AsBmp(dumpPath, data, w, h) ? L"ok" : L"failed");
            }
            buf->Unlock();
        }
        ++got;
    }
    const double secs = firstFrame ? (GetTickCount64() - firstFrame) / 1000.0 : 0.0;

    wprintf(L"Frames read     : %d (%d live, %d size errors), %.1f fps\n", got, live, sizeErrors,
        (got > 1 && secs > 0) ? (got - 1) / secs : 0.0);
    wprintf(L"No-signal frames: %d\n", got - live);
    if (maxGapMs > 0) wprintf(L"Frame interval  : %.1f .. %.1f ms\n", minGapMs, maxGapMs);

    reader.Reset();
    source->Shutdown();
    vcam->activate->ShutdownObject();
    MFShutdown();
    CoUninitialize();

    if (holdSeconds > 0) return 0;
    if (got < frameTarget) return Fail(L"not enough frames");
    if (sizeErrors) return Fail(L"frames had the wrong size");
    if (live < got / 2) return Fail(L"frames are the no-signal pattern: host is not feeding the camera");

    wprintf(L"PASS: virtual camera delivers native %ux%u\n", expected.width, expected.height);
    return 0;
}
