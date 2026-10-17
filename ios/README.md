# Cam Studio cho iPhone

App Swift (AVFoundation + VideoToolbox + Network.framework), cài đặt đúng theo [docs/protocol.md](../docs/protocol.md).

## Build trên Mac

Cần Xcode 15 trở lên. Muốn dùng chế độ **Điện ảnh** thì cần Xcode 26 và iPhone chạy iOS 26.

```bash
brew install xcodegen
cd ios
xcodegen            # tạo CamStudio.xcodeproj từ project.yml
open CamStudio.xcodeproj
```

Trong Xcode:

1. Chọn target **CamStudio** → **Signing & Capabilities** → **Team**. Tài khoản Apple miễn phí cũng được, nhưng app chỉ chạy được 7 ngày.
2. Cắm iPhone, chọn iPhone làm đích chạy, rồi bấm **Run**.
3. **Kiểm tra mật mã trước tiên:** chọn **Product → Test** (⌘U). Test `CryptoTests` phải PASS, nghĩa là phần mật mã khớp từng byte với PC và Android. Nếu test này lỗi thì ghép đôi sẽ không chạy được.

## Build trên Windows (WSL + xtool)

[xtool](https://xtool.sh) build app iOS từ gói SwiftPM ([Package.swift](Package.swift) + [xtool.yml](xtool.yml)) mà không cần Mac. Hai cách build dùng chung mã nguồn và chung [Info.plist](Info.plist).

1. Trong Ubuntu (WSL), chạy `bash /mnt/d/winlax/job/cam-studio/scripts/setup-xtool.sh` để cài Swift, usbmuxd và xtool.
2. Tải **Xcode.xip** từ https://developer.apple.com/download/all/?q=Xcode (cần đăng nhập Apple ID) vào ổ D:.
3. Chạy `xtool setup`, tự đăng nhập Apple ID, rồi nhập đường dẫn tới Xcode.xip, ví dụ `/mnt/d/Downloads/Xcode_27.xip`.
4. Build: `cd /mnt/d/winlax/job/cam-studio/ios && xtool dev build`. Cắm iPhone và chạy `xtool dev` để cài lên máy. iPhone cần được nối vào WSL qua USBIPD, xem [hướng dẫn của xtool](https://xtool.sh/documentation/xtool/installation-linux).

Với tài khoản Apple miễn phí: app chạy được 7 ngày rồi phải cài lại, và xtool thêm tiền tố vào bundle id (ví dụ `XTL-1234.vn.camstudio.phone`).

Test `CryptoTests` vẫn chạy trên Mac (Xcode) như trước.

## Lần đầu mở app

iPhone sẽ hỏi hai quyền: **Camera** và **Mạng cục bộ**. Phải cho phép cả hai: thiếu quyền Mạng cục bộ thì PC sẽ không tìm thấy iPhone.

## Tính năng

- **Ống kính:** iPhone báo tất cả ống kính cho PC:
  - camera ảo nhiều ống kính, tự đổi ống kính khi zoom;
  - góc rộng, siêu rộng, tele;
  - camera trước TrueDepth.
- **Chế độ quay:** mọi độ phân giải và fps mà bộ mã hóa phần cứng hỗ trợ, tối đa khoảng 4096×2176.
- **HDR:** HDR HLG 10‑bit (HEVC Main10) trên máy có hỗ trợ.
- **Điều khiển từ PC:** zoom, lấy nét, phơi sáng (ISO, tốc độ màn trập, bù sáng), cân bằng trắng theo Kelvin, đèn flash, chống rung (kể cả điện ảnh), tăng sáng khi thiếu sáng.
- **Hiệu ứng hệ thống** (Xóa phông, Ánh sáng studio, Center Stage, Thay nền): Apple không cho app tự bật các hiệu ứng này. Từ PC, bấm "Mở hiệu ứng hệ thống trên điện thoại"; bảng hiệu ứng sẽ hiện trên iPhone, bạn bật ở đó, và hiệu ứng áp dụng thẳng vào hình gửi sang PC.
- **Điện ảnh (xóa phông có chuyển nét):** chỉ có trên iOS 26 với iPhone hỗ trợ. API này nằm gọn trong [Cinematic.swift](CamStudio/Cinematic.swift). Nếu SDK của bạn chưa có API này, file tự bỏ qua và PC sẽ báo là không hỗ trợ.

## Giới hạn

- **Phải để app mở khi đang phát:** iOS dừng camera của app chạy nền, nên app tự làm tối màn hình để đỡ tốn pin.
- **Đã build được** bằng xtool trên Windows (iOS SDK 27.1, Swift 6.4.1), không lỗi, không cảnh báo. **Chưa chạy thử trên iPhone** và chưa chạy test `CryptoTests` (test này cần Xcode trên Mac).
