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

**Icon app:** giống logo bản PC và Android (máy quay trắng trên nền xanh), nằm ở [Resources/Assets.xcassets](Resources/Assets.xcassets). Muốn vẽ lại thì chạy `swift scripts/make-icon-ios.swift` từ thư mục gốc. Script vẽ theo đúng tỉ lệ của `scripts/make-icon.ps1`. Bản build bằng xtool chưa có icon này.

## Đưa lên TestFlight

Cần **tài khoản Apple Developer Program trả phí**. Personal Team (tài khoản miễn phí) không upload được.

1. Đăng nhập tài khoản đó trong Xcode › Settings › Accounts.
2. Ghi team và bundle id vào `ios/Signing.local.xcconfig` (file này không đưa lên git):
   ```
   DEVELOPMENT_TEAM = ABCDE12345
   CAMSTUDIO_BUNDLE_ID = com.tenban.camstudio
   ```
   - **Đổi team và bundle id cùng một lần sửa.** Xcode tự đăng ký bundle id trên team đang ký, và id đã đăng ký thì không chuyển sang tài khoản khác được. Ghi id mới mà team vẫn là team cũ, chỉ cần chạy một script là id đó thuộc về team cũ. `vn.camstudio.phone` đã mất theo cách này. Lần đầu build một id trên một team, cả hai script hiện tên team và hỏi lại trước khi đăng ký. Trả lời `y` thì script ghi dòng `// confirmed: <team> <bundle id>` vào file này và không hỏi lại nữa.
   - Bundle id mặc định `vn.camstudio.phone` chỉ dùng để build vào máy. Tiện ích chia sẻ màn hình tự lấy id `<bundle id>.screen`.
   - Đổi team hay bundle id thì iPhone coi đây là app mới: PC phải ghép đôi lại.
   - `scripts/ios-run.sh` cũng đọc file này, nên bản build vào máy dùng luôn id mới.
3. Archive:
   ```bash
   scripts/ios-archive.sh
   ```
   - Script tạo project, archive bản Release, rồi mở bản archive trong **Xcode › Organizer**.
   - Lần đầu, Xcode tự đăng ký hai bundle id (app và tiện ích) trên tài khoản.
   - Số build mặc định lấy theo ngày giờ, ví dụ `20261017.2230`. Mỗi lần upload phải dùng số lớn hơn lần trước. Có thể tự đặt bằng `--build N`, nhưng sau khi đã upload một bản theo ngày giờ thì số nhỏ như `7` sẽ bị từ chối.
   - Muốn kiểm tra bản Release biên dịch được mà chưa có tài khoản: `scripts/ios-archive.sh --unsigned`. Bản này không upload được.
4. Trên [App Store Connect](https://appstoreconnect.apple.com), vào **Apps › +  › New App** và chọn bundle id ở bước 2. Tên app phải chưa ai dùng trên App Store.
5. Trong Organizer, chọn bản archive, bấm **Distribute App › TestFlight & App Store** rồi **Upload**. Xcode tự lo chứng chỉ phân phối.

   Cách nhanh hơn: `scripts/ios-archive.sh --upload` archive xong là upload luôn, bằng tài khoản đang đăng nhập trong Xcode. Chứng chỉ phân phối do Apple quản lý trên cloud, máy Mac không cần có sẵn.
6. **Mã hóa:** [Info.plist](Info.plist) có `ITSAppUsesNonExemptEncryption = NO`, nên App Store Connect không hỏi về mã hóa (export compliance / App Encryption Documentation) cho từng bản build nữa. Khóa này nghĩa là app chỉ dùng mã hóa có sẵn của iOS: kết nối tới PC được mã hóa hoàn toàn bằng CryptoKit (ECDH P‑256, AES‑GCM, SHA‑256/HMAC; HKDF viết trên HMAC của CryptoKit). Nếu sau này app tự cài một thuật toán mã hóa riêng thì phải sửa lại khóa này.

App có sẵn [Resources/PrivacyInfo.xcprivacy](Resources/PrivacyInfo.xcprivacy) cho cả app và tiện ích:
- không theo dõi người dùng, không thu thập dữ liệu;
- khai báo `UserDefaults` với lý do CA92.1.

App Store Connect từ chối bản upload nếu thiếu file này.

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
scripts/fake-pc.sh --screen --lens all                  # màn hình đang chia sẻ (bật chia sẻ trên iPhone trước)
```

Công cụ này chỉ theo đúng giao thức, nên về nguyên tắc cũng thử được app Android. Tuy vậy, chưa ai chạy nó với Android.

## Build trên Windows (WSL + xtool)

[xtool](https://xtool.sh) build app iOS từ gói SwiftPM ([Package.swift](Package.swift) + [xtool.yml](xtool.yml)) mà không cần Mac. Hai cách build dùng chung mã nguồn và chung [Info.plist](Info.plist).

1. Trong Ubuntu (WSL), chạy `bash /mnt/d/winlax/job/cam-studio/scripts/setup-xtool.sh` để cài Swift, usbmuxd và xtool.
2. Tải **Xcode.xip** từ https://developer.apple.com/download/all/?q=Xcode (cần đăng nhập Apple ID) vào ổ D:.
3. Chạy `xtool setup`, tự đăng nhập Apple ID, rồi nhập đường dẫn tới Xcode.xip, ví dụ `/mnt/d/Downloads/Xcode_27.xip`.
4. Build: `cd /mnt/d/winlax/job/cam-studio/ios && xtool dev build`. Cắm iPhone và chạy `xtool dev` để cài lên máy. iPhone cần được nối vào WSL qua USBIPD, xem [hướng dẫn của xtool](https://xtool.sh/documentation/xtool/installation-linux).

Với tài khoản Apple miễn phí: app chạy được 7 ngày rồi phải cài lại, và xtool thêm tiền tố vào bundle id (ví dụ `XTL-1234.vn.camstudio.phone`).

Bản xtool chỉ có app chính, **không có chia sẻ màn hình**: tiện ích ReplayKit chỉ build bằng Xcode trên Mac.

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
- **Chia sẻ màn hình sang PC:** trong app, bấm **Chia sẻ màn hình sang máy tính** rồi **Bắt đầu phát**. Cũng bật được từ Trung tâm điều khiển: giữ nút Ghi màn hình, chọn Cam Studio.
  - **Trên PC:** màn hình hiện thành một nguồn riêng, **"iPhone · Màn hình"**, chọn như một camera.
  - **Dùng app khác thoải mái:** khác với camera, việc chia sẻ vẫn chạy khi bạn chuyển sang app khác. Xoay máy thì hình trên PC xoay theo.
  - **Ghép đôi trước:** PC phải ghép đôi với iPhone (qua camera) ít nhất một lần, vì phần chia sẻ màn hình không tự hiện hộp thoại ghép đôi được.
  - **Cách hoạt động:** đây là một tiện ích ReplayKit ([CamStudioScreen/](CamStudioScreen/)) chạy riêng, cổng 47322. Nó dùng chung deviceId và khóa ghép đôi với app qua Keychain Sharing. Chi tiết ở [docs/protocol.md](../docs/protocol.md) mục 1.1.
  - **Dừng:** chạm dấu đỏ ở góc màn hình rồi chọn Dừng.
- **Điện ảnh (xóa phông có chuyển nét):** chỉ có trên iOS 26 với iPhone hỗ trợ.
  - Bật/tắt ngay lúc đang phát, hiệu ứng áp thẳng vào hình gửi sang PC.
  - Chỉ một số ống kính hỗ trợ. Trên iPhone 14 là `back-dualWide` và `front-trueDepth`, ở 1080p 30 fps. Caps báo riêng cho từng ống kính.
  - API này nằm gọn trong [Cinematic.swift](CamStudio/Cinematic.swift). Nếu SDK của bạn chưa có API này, file tự bỏ qua và PC sẽ báo là không hỗ trợ.

## Giới hạn

- **Phải để app mở khi phát camera:** iOS dừng camera của app chạy nền, nên app tự làm tối màn hình để đỡ tốn pin. Nút **Tắt màn hình** làm màn hình đen hẳn, camera vẫn phát (chạm để bật lại); 1 phút không chạm thì màn hình cũng tự đen. Chia sẻ màn hình thì không cần.
- **Chia sẻ màn hình:**
  - nội dung có bản quyền (Netflix…) hiện màn đen;
  - iPhone hiện dấu đỏ suốt lúc chia sẻ;
  - không có âm thanh, không điều khiển iPhone từ PC được;
  - cài lại app thì buổi chia sẻ đang chạy bị dừng.
- **Tên máy:** iOS chỉ cho app biết tên chung "iPhone", nên PC hiện "iPhone (Wi‑Fi)" chứ không phải tên bạn đặt.
- **Đã chạy thật** trên iPhone 14 (iOS 27.2) với PC giả trên Mac. Các phần đã thử:
  - tìm qua mDNS và theo IP, ghép đôi, đọc caps;
  - mọi ống kính; 1080p30, 1080p60, 4K30 HEVC, HDR 10‑bit;
  - zoom, đèn, lấy nét, phơi sáng, cân bằng trắng (tự động và thủ công);
  - điện ảnh, bảng hiệu ứng hệ thống, xoay máy, ẩn app rồi mở lại;
  - chia sẻ màn hình: 888×1920 khoảng 30 fps (H.264 và HEVC), chuyển sang app khác, xoay ngang cả hai chiều, màn hình đứng yên, camera chạy song song.

  **App PC Windows thật:** đã thấy PC ghép đôi và phát 4K từ camera iPhone. Các nút điều khiển và chia sẻ màn hình trên PC thật chưa kiểm tra.
