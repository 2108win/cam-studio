// Media Foundation helpers for enumerating cameras and their native formats.
// Used by the host app and the verification tool.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <cwctype>
#include <memory>
#include <string>
#include <vector>

#include "SharedFrame.h"

// The MF_SOURCE_READER_* stream constants are enums with negative values.
inline constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
inline constexpr DWORD kCurrentType = static_cast<DWORD>(MF_SOURCE_READER_CURRENT_TYPE_INDEX);

namespace phone { class PhoneLink; }

struct CameraDevice
{
    std::wstring name;
    std::wstring symbolicLink;
    Microsoft::WRL::ComPtr<IMFActivate> activate;

    // Phone camera (docs/protocol.md): activate is null, frames come
    // through this authenticated connection instead.
    std::shared_ptr<phone::PhoneLink> phone;

    bool IsPhone() const { return phone != nullptr; }
};

enum class LensFacing : int
{
    None = 0, // webcam: not applicable
    Back = 1,
    Front = 2,
    External = 3,
};

struct NativeFormat
{
    DWORD index = 0;
    GUID subtype{};
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 fpsNum = 0;
    UINT32 fpsDen = 1;

    // Phones: which camera on the phone this mode belongs to.
    std::string phoneCamera;
    LensFacing lens = LensFacing::None;

    double Fps() const { return fpsDen ? static_cast<double>(fpsNum) / fpsDen : 0.0; }
    uint64_t Pixels() const { return static_cast<uint64_t>(width) * height; }
};

inline std::wstring SubtypeName(const GUID& g)
{
    if (g == MFVideoFormat_NV12) return L"NV12";
    if (g == MFVideoFormat_YUY2) return L"YUY2";
    if (g == MFVideoFormat_MJPG) return L"MJPG";
    if (g == MFVideoFormat_RGB24) return L"RGB24";
    if (g == MFVideoFormat_RGB32) return L"RGB32";
    if (g == MFVideoFormat_H264) return L"H264";
    if (g == MFVideoFormat_HEVC) return L"HEVC";
    if (g == MFVideoFormat_I420) return L"I420";
    if (g == MFVideoFormat_L8) return L"L8";
    wchar_t buf[16];
    swprintf_s(buf, L"%.4hs", reinterpret_cast<const char*>(&g.Data1));
    return buf;
}

inline std::vector<CameraDevice> EnumerateCameras()
{
    std::vector<CameraDevice> result;
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1))) return result;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count))) return result;

    for (UINT32 i = 0; i < count; ++i)
    {
        CameraDevice d;
        d.activate.Attach(devices[i]);
        WCHAR* s = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(d.activate->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &len)))
        {
            d.name = s;
            CoTaskMemFree(s);
        }
        if (SUCCEEDED(d.activate->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &len)))
        {
            d.symbolicLink = s;
            CoTaskMemFree(s);
        }
        result.push_back(std::move(d));
    }
    CoTaskMemFree(devices);
    return result;
}

inline std::wstring ToLower(std::wstring s)
{
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// Windows lists software cameras as "<name> (Windows Virtual Camera)".
inline bool IsOurVirtualCamera(const CameraDevice& d)
{
    const std::wstring ours = CAMSTUDIO_VCAM_FRIENDLY_NAME;
    return d.name == ours || d.name.rfind(ours + L" (", 0) == 0;
}

// Lists the native formats of the first video stream of a media source.
inline HRESULT GetNativeFormats(IMFMediaSource* source, std::vector<NativeFormat>& out,
                                Microsoft::WRL::ComPtr<IMFMediaTypeHandler>* handlerOut = nullptr)
{
    out.clear();
    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> pd;
    HRESULT hr = source->CreatePresentationDescriptor(&pd);
    if (FAILED(hr)) return hr;
    BOOL selected = FALSE;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor> sd;
    hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IMFMediaTypeHandler> handler;
    hr = sd->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;

    DWORD count = 0;
    hr = handler->GetMediaTypeCount(&count);
    if (FAILED(hr)) return hr;
    for (DWORD i = 0; i < count; ++i)
    {
        Microsoft::WRL::ComPtr<IMFMediaType> type;
        if (FAILED(handler->GetMediaTypeByIndex(i, &type))) continue;
        NativeFormat f;
        f.index = i;
        type->GetGUID(MF_MT_SUBTYPE, &f.subtype);
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &f.width, &f.height);
        MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &f.fpsNum, &f.fpsDen);
        out.push_back(f);
    }
    if (handlerOut) *handlerOut = handler;
    return S_OK;
}

// Rank of a pixel format when two formats tie on size and frame rate.
// Uncompressed formats avoid a decode step.
inline int SubtypeRank(const GUID& g)
{
    if (g == MFVideoFormat_NV12) return 4;
    if (g == MFVideoFormat_YUY2) return 3;
    if (g == MFVideoFormat_MJPG) return 2;
    if (g == MFVideoFormat_H264 || g == MFVideoFormat_HEVC) return 0; // need a decoder; avoid if possible
    return 1;
}

// "Native resolution": the largest frame size, then the highest frame rate,
// then the cheapest pixel format to convert.
inline const NativeFormat* PickLargest(const std::vector<NativeFormat>& formats)
{
    const NativeFormat* best = nullptr;
    for (const auto& f : formats)
    {
        if (f.width == 0 || f.height == 0) continue;
        if (!best) { best = &f; continue; }
        if (f.Pixels() != best->Pixels()) { if (f.Pixels() > best->Pixels()) best = &f; continue; }
        if (f.Fps() != best->Fps()) { if (f.Fps() > best->Fps()) best = &f; continue; }
        if (SubtypeRank(f.subtype) > SubtypeRank(best->subtype)) best = &f;
    }
    return best;
}

// Picks a device by index ("2") or case-insensitive name substring
// ("obsbot"). With an empty selector it takes the first camera that is not
// our own virtual camera.
inline const CameraDevice* SelectCamera(const std::vector<CameraDevice>& cams, const std::wstring& selector)
{
    if (selector.empty())
    {
        for (const auto& c : cams) if (!IsOurVirtualCamera(c)) return &c;
        return nullptr;
    }
    bool numeric = !selector.empty();
    for (wchar_t ch : selector) if (!iswdigit(ch)) { numeric = false; break; }
    if (numeric)
    {
        size_t i = static_cast<size_t>(_wtoi(selector.c_str()));
        return i < cams.size() ? &cams[i] : nullptr;
    }
    const std::wstring needle = ToLower(selector);
    for (const auto& c : cams)
        if (!IsOurVirtualCamera(c) && ToLower(c.name).find(needle) != std::wstring::npos) return &c;
    return nullptr;
}
