# Cam Studio trên Windows 10

Windows 11 (build 22000 trở lên) có API camera ảo (`MFCreateVirtualCamera`): mọi ứng dụng đều thấy
"Cam Studio". Windows 10 không có API này, nên ở đó Cam Studio dùng một **bộ lọc DirectShow**
(`src/dshow`, DLL `CamStudioDShow64.dll` và `CamStudioDShow32.dll`) thay cho camera ảo của hệ thống.

## Cùng một dòng khung hình

- Trên Windows 11, Frame Server tạo bộ nhớ chia sẻ `Global\CamStudioVCam_Frames_v2`; app ghi vào đó.
- Trên Windows 10, app tự tạo `Local\CamStudioVCam_Frames_v2` (cùng cấu trúc `SharedFrameHeader`,
  3 ô NV12). Bộ nhớ này được cấp một lần đủ cho ảnh 4K (`CAMSTUDIO_LOCAL_MAX_PIXELS`); `width/height`
  trong header là kích thước app đang ghi, chỉ đổi khi không có ứng dụng nào đang đọc (luật kích
  thước như cũ: đang gọi thì giữ, ảnh khác được lồng viền đen).
- Bộ lọc DirectShow chạy **trong** ứng dụng dùng camera, đọc các ô đó, co giãn giữ tỉ lệ về kích
  thước ứng dụng chọn, và báo `readerActive` để app mở nguồn (standby vẫn hoạt động).

Engine tìm `MFCreateVirtualCamera` lúc chạy (`VirtualCameraApi()` trong `CaptureEngine.cpp`); không có
thì vào chế độ DirectShow. Đặt `CAMSTUDIO_FORCE_DSHOW=1` để thử chế độ này trên Windows 11.

## Bộ lọc DirectShow

- Một chân ra loại CAPTURE; định dạng NV12, YUY2, I420; kích thước ảnh hiện tại của app rồi
  1920×1080, 1280×720, 960×540, 640×480, 640×360, 320×240; 5–60 fps theo ứng dụng yêu cầu.
- Viết không dùng base classes của DirectShow (SDK không có). Thanh ghi: `regsvr32`. Chạy với quyền
  admin thì đăng ký HKLM (installer); không thì HKCU (để thử khi phát triển).
- Cần cả bản 32-bit: ứng dụng 32-bit chỉ nạp được bộ lọc 32-bit. `scripts\build.ps1` build thêm
  `build-x86\Release\CamStudioDShow.dll`.
- Ứng dụng chỉ dùng Media Foundation để bắt hình (Camera của Windows, Teams mới…) **không thấy** bộ
  lọc DirectShow trên Windows 10. Discord, OBS, Zoom, Skype, Chrome/Edge (bản dùng DirectShow) thì thấy.

## Installer

`MinVersion=10.0.17763` (Windows 10 1809). Windows 11: đăng ký `CamStudioVCam.dll` như trước.
Windows 10: đăng ký hai DLL DirectShow (64 và 32-bit), không đăng ký camera Media Foundation.
App WinUI: `TargetPlatformMinVersion` 10.0.17763.0 (Windows App SDK hỗ trợ từ 1809).

## Giới hạn

- Chỉ Windows 64-bit (x64). Windows 10 32-bit không được hỗ trợ (app .NET/WinUI chỉ build x64).
- Windows on ARM: chưa; camera ảo của Windows 11 đòi DLL ARM64 (Frame Server là tiến trình ARM64).
- Nguồn "Cửa sổ trên máy tính" cần Windows 10 1903 trở lên (Windows.Graphics.Capture).
- Chưa thử trên máy Windows 10 thật; trên Windows 11 đã thử bằng `CAMSTUDIO_FORCE_DSHOW=1` và
  `camstudio-dshow-consumer` (xem CLAUDE.md).
