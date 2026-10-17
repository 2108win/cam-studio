# Cam Studio cho iPhone

App Swift (AVFoundation + VideoToolbox + Network.framework), cài đặt đúng theo [docs/protocol.md](../docs/protocol.md).

## Build trên Mac

Cần Xcode 26 trở lên (đã dùng Xcode 27) và XcodeGen: `brew install xcodegen`.

**Cách nhanh:** iPhone đã ghép với Mac (cắm cáp lần đầu, bấm **Tin cậy**, bật **Chế độ nhà phát triển**). Sau đó chạy:

```bash
scripts/ios-run.sh
```

Script này làm lần lượt:
1. chạy `CryptoTests` ngay trên Mac;
2. tạo project bằng XcodeGen rồi build;
3. cài app lên iPhone và mở app.

Lần đầu, script hỏi **Team ID**: xem trong Xcode › Settings › Accounts, cột Team. Team ID được lưu vào `ios/Signing.local.xcconfig`, file này không đưa lên git. Lý do: chọn Team trong Xcode thì lần chạy `xcodegen` sau sẽ mất. Tài khoản Apple miễn phí cũng được, nhưng app chỉ chạy 7 ngày.

**Mở bằng Xcode:**

```bash
cd ios
xcodegen            # tạo CamStudio.xcodeproj từ project.yml
open CamStudio.xcodeproj
```

- **Không cho Xcode sửa Info.plist.** Xcode 27 có lúc tự chuyển các khóa trong [Info.plist](Info.plist) sang build settings và ghi đè file. Khi đó lần `xcodegen` sau sẽ làm mất mô tả quyền Camera và Mạng cục bộ: iOS chặn camera và mDNS. Nếu lỡ bị, chạy `git checkout ios/Info.plist`.
- **Kiểm tra mật mã:** chọn scheme **CryptoTestsMac** rồi ⌘U, hoặc chạy lệnh dưới đây. Test phải PASS, nghĩa là phần mật mã khớp từng byte với PC và Android:
  ```bash
  xcodebuild test -project CamStudio.xcodeproj -scheme CryptoTestsMac -destination platform=macOS
  ```

## Thử trên Mac, không cần PC Windows

[tools/fake_pc.swift](../tools/fake_pc.swift) là một "PC giả" chạy trên Mac. Nó làm đúng việc của app PC:
- tìm điện thoại qua mDNS, ghép đôi, đọc caps;
- phát từng ống kính, gửi `set`, giải mã video bằng VideoToolbox;
- lưu khung đầu và khung cuối của mỗi ống kính thành PNG, đã xoay đúng chiều, trong `build/fake-pc-out/`.

Lần đầu chạy, bấm **Đồng ý** trên iPhone nếu mã 6 số giống mã PC giả in ra.

```bash
scripts/fake-pc.sh --lens all --seconds 4
scripts/fake-pc.sh --lens back-wide --width 3840 --height 2160 --codec hevc --bitrate 25000
scripts/fake-pc.sh --lens back-dualWide --set '{"cinematic":true}'
scripts/fake-pc.sh --lens front-trueDepth --effects     # mở bảng hiệu ứng hệ thống khi đang phát
```

Công cụ này chỉ theo đúng giao thức, nên về nguyên tắc cũng thử được app Android. Tuy vậy, chưa ai chạy nó với Android.

## Build trên Windows (WSL + xtool)

[xtool](https://xtool.sh) build app iOS từ gói SwiftPM ([Package.swift](Package.swift) + [xtool.yml](xtool.yml)) mà không cần Mac. Hai cách build dùng chung mã nguồn và chung [Info.plist](Info.plist).

1. Trong Ubuntu (WSL), chạy `bash /mnt/d/winlax/job/cam-studio/scripts/setup-xtool.sh` để cài Swift, usbmuxd và xtool.
2. Tải **Xcode.xip** từ https://developer.apple.com/download/all/?q=Xcode (cần đăng nhập Apple ID) vào ổ D:.
3. Chạy `xtool setup`, tự đăng nhập Apple ID, rồi nhập đường dẫn tới Xcode.xip, ví dụ `/mnt/d/Downloads/Xcode_27.xip`.
4. Build: `cd /mnt/d/winlax/job/cam-studio/ios && xtool dev build`. Cắm iPhone và chạy `xtool dev` để cài lên máy. iPhone cần được nối vào WSL qua USBIPD, xem [hướng dẫn của xtool](https://xtool.sh/documentation/xtool/installation-linux).

Với tài khoản Apple miễn phí: app chạy được 7 ngày rồi phải cài lại, và xtool thêm tiền tố vào bundle id (ví dụ `XTL-1234.vn.camstudio.phone`).

Test `CryptoTests` chạy trên Mac (scheme `CryptoTestsMac`).

## Lần đầu mở app

iPhone sẽ hỏi hai quyền: **Camera** và **Mạng cục bộ**. Phải cho phép cả hai: thiếu quyền Mạng cục bộ thì PC sẽ không tìm thấy iPhone.

## Tính năng

- **Ống kính:** iPhone báo tất cả ống kính cho PC, mỗi ống kính có id cố định:
  - camera ảo nhiều ống kính, tự đổi ống kính khi zoom (`back-dualWide`, `back-triple`…);
  - góc rộng, siêu rộng, tele (`back-wide`, `back-ultraWide`, `back-tele`);
  - camera trước: `front-trueDepth` nếu nó quay điện ảnh được, nếu không thì `front-wide`. Hai camera này cùng một cảm biến, nên chỉ báo một.
- **Chế độ quay:** mọi độ phân giải và fps mà bộ mã hóa phần cứng hỗ trợ, tối đa khoảng 4096×2176.
- **HDR:** HDR HLG 10‑bit (HEVC Main10) trên máy có hỗ trợ.
- **Điều khiển từ PC:** zoom, lấy nét, phơi sáng (ISO, tốc độ màn trập, bù sáng), cân bằng trắng theo Kelvin, đèn flash, chống rung (kể cả điện ảnh), tăng sáng khi thiếu sáng.
- **Hiệu ứng hệ thống** (Xóa phông, Ánh sáng studio, Center Stage, Thay nền): Apple không cho app tự bật các hiệu ứng này. Từ PC, bấm "Mở hiệu ứng hệ thống trên điện thoại"; bảng hiệu ứng sẽ hiện trên iPhone, bạn bật ở đó, và hiệu ứng áp dụng thẳng vào hình gửi sang PC.
- **Điện ảnh (xóa phông có chuyển nét):** chỉ có trên iOS 26 với iPhone hỗ trợ.
  - Bật/tắt ngay lúc đang phát, hiệu ứng áp thẳng vào hình gửi sang PC.
  - Chỉ một số ống kính hỗ trợ. Trên iPhone 14 là `back-dualWide` và `front-trueDepth`, ở 1080p 30 fps. Caps báo riêng cho từng ống kính.
  - API này nằm gọn trong [Cinematic.swift](CamStudio/Cinematic.swift). Nếu SDK của bạn chưa có API này, file tự bỏ qua và PC sẽ báo là không hỗ trợ.

## Giới hạn

- **Phải để app mở khi đang phát:** iOS dừng camera của app chạy nền, nên app tự làm tối màn hình để đỡ tốn pin.
- **Tên máy:** iOS chỉ cho app biết tên chung "iPhone", nên PC hiện "iPhone (Wi‑Fi)" chứ không phải tên bạn đặt.
- **Đã chạy thật** trên iPhone 14 (iOS 27.2) với PC giả trên Mac. Các phần đã thử:
  - tìm qua mDNS và theo IP, ghép đôi, đọc caps;
  - mọi ống kính; 1080p30, 1080p60, 4K30 HEVC, HDR 10‑bit;
  - zoom, đèn, lấy nét, phơi sáng, cân bằng trắng (tự động và thủ công);
  - điện ảnh, bảng hiệu ứng hệ thống, xoay máy, ẩn app rồi mở lại.

  **Chưa thử với app PC Windows thật.**
