// A stand-in phone for testing the PC side without a real device. Speaks the
// phone side of docs/protocol.md: TCP server, handshake + pairing (auto
// accepts on the "phone" side; the PC user still confirms), sealed records,
// caps with one 1280x720@30 H.264 mode, and a moving test pattern encoded
// with the Windows H.264 encoder MFT.
//
//   camstudio-fake-phone [--port 47321] [--thermal serious|critical] [--audio] [--stall MS]
// --stall: every 3 s the stream stops for MS ms, then the held frames go out in a bunch (a Wi‑Fi
// stall): the PC's "stats" (printed) should show queueMs rise, and the PC show the newest frame.
// --audio: the caps offer a microphone; asked for it ("audio" in start, or "set"), the phone sends
// a 440 Hz tone as AUDIO records (48 kHz mono 16-bit, 10 ms each), as the real apps send their mic.
// --thermal: 5 s into a stream, "state" says the phone is that hot (the PC should then ask for
// 30 fps or less, see MainWindow.Thermal.cs); the caps offer 30 and 60 fps for that.
// Then in Cam Studio: "Thêm điện thoại theo địa chỉ IP…" -> 127.0.0.1:47321

#include "../src/engine/Crypto.h"
#include "../src/engine/Json.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <initguid.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

#pragma comment(lib, "ws2_32")

using Microsoft::WRL::ComPtr;
using crypto::Bytes;

namespace
{
constexpr uint32_t kWidth = 1280, kHeight = 720, kFps = 30;
const char* kDeviceId = "FAKE-PHONE-0001";

SOCKET g_sock = INVALID_SOCKET;
std::mutex g_sendLock;
Bytes g_sendKey, g_recvKey;
uint64_t g_sendCounter = 0, g_recvCounter = 0;
std::atomic<bool> g_streaming{ false }, g_forceKey{ false };

void PutU32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void PutU64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i)); }
uint32_t GetU32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t GetU64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p[i]; return v; }

bool SendAll(const uint8_t* p, size_t n)
{
    while (n > 0)
    {
        const int s = send(g_sock, reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
        if (s <= 0) return false;
        p += s;
        n -= s;
    }
    return true;
}

bool RecvAll(uint8_t* p, size_t n)
{
    while (n > 0)
    {
        const int r = recv(g_sock, reinterpret_cast<char*>(p), static_cast<int>(n), 0);
        if (r <= 0) return false;
        p += r;
        n -= r;
    }
    return true;
}

bool SendHandshake(const json::Value& v)
{
    const std::string body = v.Dump();
    std::vector<uint8_t> rec(5 + body.size());
    PutU32(rec.data(), static_cast<uint32_t>(body.size() + 1));
    rec[4] = 0x01;
    memcpy(rec.data() + 5, body.data(), body.size());
    return SendAll(rec.data(), rec.size());
}

bool ReadHandshake(json::Value& v)
{
    uint8_t head[5];
    if (!RecvAll(head, 5) || head[4] != 0x01) return false;
    const uint32_t len = GetU32(head);
    if (len < 1 || len > 65536) return false;
    std::string body(len - 1, '\0');
    if (!RecvAll(reinterpret_cast<uint8_t*>(body.data()), body.size())) return false;
    v = json::Value::Parse(body);
    return true;
}

bool SendSealed(uint8_t kind, const uint8_t* payload, size_t size, int64_t ptsUs = 0, uint32_t flags = 0)
{
    std::lock_guard lk(g_sendLock);
    std::vector<uint8_t> plain(13 + size);
    plain[0] = kind;
    PutU64(plain.data() + 1, static_cast<uint64_t>(ptsUs));
    PutU32(plain.data() + 9, flags);
    memcpy(plain.data() + 13, payload, size);
    const uint64_t counter = g_sendCounter++;
    const Bytes sealed = crypto::Seal(g_sendKey, crypto::kDirDown, counter, plain.data(), plain.size());
    std::vector<uint8_t> rec(4 + 1 + 8 + sealed.size());
    PutU32(rec.data(), static_cast<uint32_t>(1 + 8 + sealed.size()));
    rec[4] = 0x02;
    PutU64(rec.data() + 5, counter);
    memcpy(rec.data() + 13, sealed.data(), sealed.size());
    return SendAll(rec.data(), rec.size());
}

bool SendControl(const json::Value& v)
{
    const std::string s = v.Dump();
    return SendSealed(1, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

bool ReceiveControl(json::Value& v)
{
    uint8_t head[13];
    if (!RecvAll(head, 13) || head[4] != 0x02) return false;
    const uint32_t len = GetU32(head);
    const uint64_t counter = GetU64(head + 5);
    if (counter != g_recvCounter++) return false;
    std::vector<uint8_t> sealed(len - 9);
    if (!RecvAll(sealed.data(), sealed.size())) return false;
    Bytes plain;
    if (!crypto::Open(g_recvKey, crypto::kDirUp, counter, sealed.data(), sealed.size(), plain)) return false;
    if (plain[0] != 1) { v = json::Value(); return true; }
    v = json::Value::Parse(std::string(plain.begin() + 13, plain.end()));
    return true;
}

std::wstring PairFile(const std::string& clientId)
{
    return L"fake-phone-" + std::wstring(clientId.begin(), clientId.end()) + L".key";
}

bool Handshake()
{
    json::Value hello;
    if (!ReadHandshake(hello) || hello["type"].Str() != "hello") return false;
    const std::string clientId = hello["clientId"].Str();
    const Bytes commit = crypto::FromBase64(hello["commit"].Str());
    printf("PC \"%s\" (%s) connected\n", hello["clientName"].Str().c_str(), clientId.c_str());

    std::ifstream kf(PairFile(clientId), std::ios::binary);
    Bytes kPair((std::istreambuf_iterator<char>(kf)), std::istreambuf_iterator<char>());
    crypto::EcdhKey key;
    SendHandshake(json::Value::MakeObject()
        .Set("type", "hello").Set("v", 1).Set("deviceId", kDeviceId).Set("name", "Fake Phone")
        .Set("model", "Test Rig").Set("os", "android").Set("pub", crypto::Base64(key.PublicKey()))
        .Set("paired", !kPair.empty()));

    json::Value reveal;
    if (!ReadHandshake(reveal)) return false;
    const Bytes pcPub = crypto::FromBase64(reveal["pub"].Str());
    if (!crypto::ConstantTimeEqual(crypto::Sha256({ &pcPub }), commit)) { printf("commitment mismatch\n"); return false; }
    const Bytes z = key.Agree(pcPub);
    const Bytes t = crypto::Transcript(clientId, kDeviceId, pcPub, key.PublicKey());

    json::Value next;
    if (!ReadHandshake(next)) return false;
    if (next["type"].Str() == "pairStart")
    {
        printf("Pairing code: %s  (fake phone accepts; confirm on the PC)\n", crypto::Sas(t).c_str());
        SendHandshake(json::Value::MakeObject().Set("type", "pair").Set("accepted", true));
        kPair = crypto::PairKey(z, t);
        if (!ReadHandshake(next)) return false;
    }
    if (next["type"].Str() != "auth" || kPair.empty() ||
        !crypto::ConstantTimeEqual(crypto::FromBase64(next["mac"].Str()), crypto::PcMac(kPair, t)))
    {
        printf("PC authentication failed\n");
        return false;
    }
    std::ofstream(PairFile(clientId), std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(kPair.data()), kPair.size());
    SendHandshake(json::Value::MakeObject().Set("type", "auth").Set("mac", crypto::Base64(crypto::PhoneMac(kPair, t))));
    g_recvKey = crypto::UpKey(z, kPair, t);
    g_sendKey = crypto::DownKey(z, kPair, t);
    g_sendCounter = g_recvCounter = 0;
    printf("Authenticated, channel encrypted\n");
    return true;
}

// The --thermal level, sent in "state" once a stream has run 5 s.
std::string g_thermal;
int g_stallMs = 0;                      // --stall
bool g_audioCap = false;                // --audio
std::atomic<bool> g_audioOn{ false };   // the PC asked for the microphone

// The "microphone": a 440 Hz tone, 10 ms a record, while the PC wants it.
void AudioLoop()
{
    std::vector<int16_t> chunk(480);
    uint64_t n = 0;
    ULONGLONG next = GetTickCount64();
    while (g_streaming)
    {
        if (g_audioOn)
        {
            for (auto& s : chunk) s = static_cast<int16_t>(8000 * std::sin(2 * 3.14159265358979 * 440 * static_cast<double>(n++) / 48000));
            SendSealed(4, reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size() * 2, static_cast<int64_t>(n * 1000000 / 48000));
        }
        next += 10;
        const ULONGLONG now = GetTickCount64();
        if (next > now) Sleep(static_cast<DWORD>(next - now));
    }
}

json::Value Caps()
{
    json::Value fps = json::Value::MakeArray();
    fps.Push(30);
    if (!g_thermal.empty()) fps.Push(60);
    json::Value audio = json::Value::MakeArray();
    audio.Push(48000);
    json::Value codecs = json::Value::MakeArray();
    codecs.Push("h264");
    json::Value modes = json::Value::MakeArray();
    modes.Push(json::Value::MakeObject().Set("lens", "cam0").Set("width", kWidth).Set("height", kHeight)
        .Set("fps", fps).Set("codecs", codecs).Set("hdr", false));
    json::Value focusModes = json::Value::MakeArray();
    focusModes.Push("continuous").Push("manual");
    json::Value controls = json::Value::MakeObject()
        .Set("focus", json::Value::MakeObject().Set("modes", focusModes))
        .Set("torch", true);
    json::Value lenses = json::Value::MakeArray();
    lenses.Push(json::Value::MakeObject().Set("id", "cam0").Set("facing", "back").Set("kind", "wide")
        .Set("label", "Camera giả · góc rộng").Set("zoom", json::Value::MakeObject().Set("min", 1).Set("max", 4))
        .Set("controls", controls));
    return json::Value::MakeObject().Set("type", "caps")
        .Set("device", json::Value::MakeObject().Set("name", "Fake Phone").Set("os", "android"))
        .Set("lenses", lenses).Set("modes", modes)
        .Set("effects", json::Value::MakeObject().Set("cinematic", "none"))
        .Set("audio", g_audioCap ? json::Value::MakeObject().Set("format", "pcm16").Set("channels", 1).Set("rates", audio) : json::Value());
}

// Moving test pattern: colour gradient, a sweeping white box, frame counter bars.
void DrawFrame(std::vector<uint8_t>& nv12, uint32_t n)
{
    uint8_t* y = nv12.data();
    uint8_t* uv = y + kWidth * kHeight;
    for (uint32_t r = 0; r < kHeight; ++r)
        for (uint32_t c = 0; c < kWidth; ++c)
            y[r * kWidth + c] = static_cast<uint8_t>(40 + (c * 160 / kWidth));
    for (uint32_t r = 0; r < kHeight / 2; ++r)
        for (uint32_t c = 0; c < kWidth / 2; ++c)
        {
            uv[r * kWidth + 2 * c] = static_cast<uint8_t>(64 + c * 128 / (kWidth / 2));
            uv[r * kWidth + 2 * c + 1] = static_cast<uint8_t>(64 + r * 128 / (kHeight / 2));
        }
    const uint32_t bx = (n * 12) % (kWidth - 160);
    for (uint32_t r = 280; r < 440; ++r) memset(y + r * kWidth + bx, 235, 160);
    for (uint32_t b = 0; b < 16; ++b)
        if (n & (1u << b))
            for (uint32_t r = 20; r < 60; ++r) memset(y + r * kWidth + 40 + b * 70, 235, 50);
}

void StreamLoop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const ULONGLONG streamStart = GetTickCount64();
    ULONGLONG lastState = 0;
    MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_NV12 }, out{ MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, &count);
    if (!count) { printf("no H.264 encoder\n"); return; }
    ComPtr<IMFTransform> enc;
    acts[0]->ActivateObject(IID_PPV_ARGS(&enc));
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);

    ComPtr<IMFMediaType> ot, it;
    MFCreateMediaType(&ot);
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    ot->SetUINT32(MF_MT_AVG_BITRATE, 4000000);
    ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, kWidth, kHeight);
    MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, kFps, 1);
    ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    HRESULT hr = enc->SetOutputType(0, ot.Get(), 0);
    MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, kWidth, kHeight);
    MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, kFps, 1);
    if (SUCCEEDED(hr)) hr = enc->SetInputType(0, it.Get(), 0);
    if (FAILED(hr)) { printf("encoder setup failed 0x%08lx\n", hr); return; }
    ComPtr<ICodecAPI> api;
    enc.As(&api);
    if (api)
    {
        VARIANT v{};
        v.vt = VT_UI4; v.ulVal = kFps;
        api->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
        v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
        api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    MFT_OUTPUT_STREAM_INFO info{};
    enc->GetOutputStreamInfo(0, &info);
    std::vector<uint8_t> nv12(kWidth * kHeight * 3 / 2);
    const int64_t frame100ns = 10000000 / kFps;
    ULONGLONG next = GetTickCount64();
    ULONGLONG lastStall = next;
    for (uint32_t n = 0; g_streaming; ++n)
    {
        DrawFrame(nv12, n);
        if (g_forceKey.exchange(false) && api)
        {
            VARIANT v{};
            v.vt = VT_UI4; v.ulVal = 1;
            api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        }
        ComPtr<IMFMediaBuffer> buf;
        MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &buf);
        BYTE* p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        memcpy(p, nv12.data(), nv12.size());
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(nv12.size()));
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(buf.Get());
        s->SetSampleTime(n * frame100ns);
        s->SetSampleDuration(frame100ns);
        enc->ProcessInput(0, s.Get(), 0);

        for (;;)
        {
            ComPtr<IMFMediaBuffer> ob;
            MFCreateMemoryBuffer(info.cbSize ? info.cbSize : 2 * 1024 * 1024, &ob);
            ComPtr<IMFSample> os;
            MFCreateSample(&os);
            os->AddBuffer(ob.Get());
            MFT_OUTPUT_DATA_BUFFER od{};
            od.pSample = os.Get();
            DWORD status = 0;
            hr = enc->ProcessOutput(0, 1, &od, &status);
            if (od.pEvents) od.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) break;
            ComPtr<IMFMediaBuffer> cb;
            os->ConvertToContiguousBuffer(&cb);
            BYTE* d = nullptr;
            DWORD len = 0;
            cb->Lock(&d, nullptr, &len);
            const bool key = MFGetAttributeUINT32(os.Get(), MFSampleExtension_CleanPoint, 0) != 0;
            LONGLONG t = 0;
            os->GetSampleTime(&t);
            if (!SendSealed(3, d, len, t / 10, key ? 1 : 0)) g_streaming = false;
            cb->Unlock();
        }
        if (!g_thermal.empty() && GetTickCount64() - streamStart > 5000 && GetTickCount64() - lastState > 1000)
        {
            lastState = GetTickCount64();
            SendControl(json::Value::MakeObject().Set("type", "state").Set("thermal", g_thermal).Set("battery", 80).Set("charging", false));
        }
        next += 1000 / kFps;
        const ULONGLONG now = GetTickCount64();
        if (next > now) Sleep(static_cast<DWORD>(next - now));
        if (g_stallMs > 0 && now - lastStall > 3000)
        {
            lastStall = now;
            Sleep(static_cast<DWORD>(g_stallMs)); // the frames due meanwhile follow back to back
        }
    }
    CoUninitialize();
}
} // namespace

int main(int argc, char** argv)
{
    int port = 47321;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::string(argv[i]) == "--port") port = atoi(argv[i + 1]);
        if (std::string(argv[i]) == "--thermal") g_thermal = argv[i + 1];
        if (std::string(argv[i]) == "--stall") g_stallMs = atoi(argv[i + 1]);
    }
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--audio") g_audioCap = true;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    MFStartup(MF_VERSION);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 1) != 0)
    {
        printf("cannot listen on 127.0.0.1:%d\n", port);
        return 1;
    }
    printf("Fake phone listening on 127.0.0.1:%d\n", port);
    fflush(stdout);

    for (;;)
    {
        g_sock = accept(listener, nullptr, nullptr);
        if (g_sock == INVALID_SOCKET) break;
        BOOL nd = TRUE;
        setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&nd), sizeof(nd));
        std::thread streamer, sound;
        try
        {
            if (Handshake())
            {
                json::Value msg;
                while (ReceiveControl(msg))
                {
                    const std::string type = msg["type"].Str();
                    if (type == "getCaps") SendControl(Caps());
                    else if (type == "start")
                    {
                        printf("start: lens=%s %dx%d@%d %s\n", msg["lens"].Str().c_str(), msg["width"].Int(), msg["height"].Int(),
                            msg["fps"].Int(), msg["codec"].Str().c_str());
                        g_streaming = false;
                        if (streamer.joinable()) streamer.join();
                        if (sound.joinable()) sound.join();
                        g_audioOn = g_audioCap && msg["audio"].Bool();
                        printf("start: audio %s\n", g_audioOn ? "on" : "off");
                        SendControl(json::Value::MakeObject().Set("type", "started").Set("lens", "cam0")
                            .Set("width", kWidth).Set("height", kHeight).Set("fps", kFps).Set("codec", "h264").Set("orientation", 0)
                            .Set("audio", g_audioOn ? json::Value::MakeObject().Set("on", true).Set("rate", 48000).Set("channels", 1)
                                                    : json::Value::MakeObject().Set("on", false)));
                        g_streaming = true;
                        streamer = std::thread(StreamLoop);
                        sound = std::thread(AudioLoop);
                    }
                    else if (type == "stop") g_streaming = false;
                    else if (type == "keyframe") g_forceKey = true;
                    else if (type == "stats") printf("stats: %s\n", msg.Dump().c_str());
                    else if (type == "set")
                    {
                        printf("set: %s\n", msg.Dump().c_str());
                        if (!msg["audio"].IsNull() && g_audioCap)
                        {
                            g_audioOn = msg["audio"].Bool();
                            SendControl(json::Value::MakeObject().Set("type", "state").Set("audio",
                                g_audioOn.load() ? json::Value::MakeObject().Set("on", true).Set("rate", 48000).Set("channels", 1)
                                                 : json::Value::MakeObject().Set("on", false)));
                        }
                    }
                    fflush(stdout);
                }
            }
        }
        catch (const std::exception& e)
        {
            printf("error: %s\n", e.what());
        }
        g_streaming = false;
        if (streamer.joinable()) streamer.join();
        if (sound.joinable()) sound.join();
        closesocket(g_sock);
        printf("PC disconnected\n");
        fflush(stdout);
    }
    return 0;
}
