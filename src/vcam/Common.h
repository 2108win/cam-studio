#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>
#include <propvarutil.h>
#include <ole2.h>
// After ole2/propvarutil (they pull in cguid.h, which breaks under INITGUID)
// and before ks/ksmedia, so the KS GUIDs we use get selectany definitions.
#include <initguid.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <strsafe.h>

#include <cstdint>
#include <mutex>

#include "../common/SharedFrame.h"

using Microsoft::WRL::ComPtr;

// Frame Server runs us as LocalService in session 0, so file logging into the
// project folder is not possible. Read these with DebugView (enable
// "Capture Global Win32").
inline void VcamLog(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    StringCchCopyW(buf, ARRAYSIZE(buf), L"[CamStudioVCam] ");
    size_t prefix = wcslen(buf);
    va_list args;
    va_start(args, fmt);
    StringCchVPrintfW(buf + prefix, ARRAYSIZE(buf) - prefix, fmt, args);
    va_end(args);
    StringCchCatW(buf, ARRAYSIZE(buf), L"\n");
    OutputDebugStringW(buf);
}

#define RETURN_IF_FAILED(expr)                                                   \
    do {                                                                         \
        HRESULT hr__ = (expr);                                                   \
        if (FAILED(hr__)) {                                                      \
            VcamLog(L"%hs:%d 0x%08X %hs", __FUNCTION__, __LINE__, hr__, #expr);   \
            return hr__;                                                         \
        }                                                                        \
    } while (0)

#define RETURN_HR_IF_NULL(hr, p) do { if ((p) == nullptr) return (hr); } while (0)

extern HMODULE g_hModule;
