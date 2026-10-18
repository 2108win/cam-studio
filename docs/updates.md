# Tự động cập nhật

Nguồn: bản phát hành mới nhất trên GitHub (`releases/latest`, bỏ qua bản nháp và pre-release).
Tên file phải đúng như `scripts\package-release.ps1` tạo ra:
`CamStudio-Setup-<x.y.z>.exe` và `CamStudio-<x.y.z>-android.apk`, tag `v<x.y.z>`.

## PC (`app/Services/Updater.cs`, `app/MainWindow.Update.cs`)

- Kiểm tra 3 giây sau khi người dùng tự mở app, 90 giây sau khi khởi động cùng Windows, rồi 6 giờ
  một lần. Một yêu cầu nhỏ, có ETag.
- Có bản mới: tải ngầm vào `%LOCALAPPDATA%\CamStudio\updates`, kiểm tra kích thước và SHA-256 mà
  GitHub ghi cho file (`digest`). Sai hoặc thiếu mã thì bỏ. Kiểm tra lại ngay trước khi chạy.
- Khi nào cài:
  - **Không bao giờ khi có app đang dùng camera** (bộ cài khởi động lại dịch vụ camera của Windows,
    mọi app camera sẽ mất hình).
  - Tự cài: chỉ khi người dùng vừa tự mở app, bộ cài đã tải sẵn, và không ai dùng camera. App báo
    trước 15 giây ("đóng thông báo để để sau").
  - Còn lại: thanh thông báo, mục "Cập nhật lên x.y.z" ở khay, bóng thông báo, nút trong Cài đặt.
    Có app đang dùng camera thì hỏi lại trước khi cài.
  - Khởi động cùng Windows: không bao giờ bật hộp hỏi quyền lúc mới bật máy.
- Cài: chạy bộ cài với `/SILENT /SUPPRESSMSGBOXES /NORESTART /RELAUNCH=show|tray` (không dùng
  "runas": Inno tự xin quyền nên mở lại app bằng quyền thường). Bộ cài mở lại app (`[Run]`
  `RelaunchAfterUpdate`). Người dùng bấm "Không" ở UAC: app vẫn chạy và báo "Chưa cập nhật".
- "Tự động cập nhật" tắt: chỉ báo có bản mới, không tự tải.
- Chỉ bản đã cài (`InstallLocation` trong khóa gỡ cài đặt) mới tự cài. Bản chạy từ `app\bin` chỉ
  kiểm tra và tải.
- Thử mà không cần GitHub: `CAMSTUDIO_UPDATE_FEED=http://127.0.0.1:<port>/latest` (chỉ nhận
  loopback), giả lập JSON của GitHub.

## Android

APK tải về phải cùng chữ ký với bản đang cài. Hiện mọi bản đều ký bằng `~/.android/debug.keystore`
của máy Windows này (SHA-256 chứng chỉ `7b48cd77…a14cf`): luôn build bản phát hành trên máy này,
hoặc chép keystore đó sang máy khác.

## iPhone

TestFlight / App Store tự cập nhật; app không cần làm gì.

## Phát hành một bản

1. Tăng `<Version>` trong `app/CamStudio.App.csproj` và `versionName`/`versionCode` trong
   `android/app/build.gradle.kts`.
2. `scripts\package-release.ps1` → `build\dist`.
3. Tạo release `v<x.y.z>` trên GitHub, đính kèm hai file. App sẽ tự thấy trong vòng 6 giờ.
