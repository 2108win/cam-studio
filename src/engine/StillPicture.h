// Still pictures for the virtual camera while the picture is paused (cs_set_pause_picture):
// a card with text, the last picture blurred with the text on it, or an image file. Rendered
// once per setting and camera size (Direct2D, DirectWrite, WIC), never per frame.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace still
{
// NV12 w x h (even sizes). background: the last picture (NV12 bw x bh) to blur behind the text,
// or null for a plain dark card.
bool RenderCard(const std::wstring& text, uint32_t w, uint32_t h, const uint8_t* background, uint32_t bw, uint32_t bh,
                std::vector<uint8_t>& nv12);

// An image file (PNG, JPEG, BMP…) scaled to fill w x h, centred and cropped. False if unreadable.
bool LoadImageFill(const std::wstring& path, uint32_t w, uint32_t h, std::vector<uint8_t>& nv12);

// "Chụp ảnh": an NV12 frame (contiguous w x h) as a PNG file.
HRESULT SavePng(const std::wstring& path, const uint8_t* nv12, uint32_t w, uint32_t h);
} // namespace still
