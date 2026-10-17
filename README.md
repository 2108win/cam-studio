# Cam Studio

Một app giống Camo Studio: lấy hình từ camera ngoài và đưa ra một **camera ảo** ("Cam Studio") ở **độ phân giải gốc** của camera. Zoom, Teams, OBS và trình duyệt đều dùng được camera ảo này.

**Nguồn hình:**
- **Webcam USB / capture card:** đã chạy.
- **Điện thoại Android qua Wi‑Fi:** đã chạy với Redmi 9T, dùng được cả 4 ống kính: siêu rộng 0.6×, chính 1×, macro và camera trước.
- **iPhone qua Wi‑Fi:** đã chạy trên iPhone 14 (iOS 27.2) với "PC giả" trên Mac (mọi ống kính, tới 4K và HDR, điện ảnh). Chưa thử với app PC Windows.
- **Điện thoại qua USB:** dùng chung giao thức, sẽ làm sau.

## Cài đặt (bản phát hành)

Tải file ở trang [Releases](https://github.com/2108win/cam-studio/releases):

- **Windows 11:**
  - Tải `CamStudio-<phiên bản>-win-x64.zip` và giải nén.
  - Bấm đúp `Install.cmd`, rồi bấm Yes khi Windows hỏi quyền Admin.
  - Mở **Cam Studio** từ Start menu.
  - Gỡ cài đặt trong Settings › Apps.
- **Android 8 trở lên:**
  - Chép `CamStudio-<phiên bản>-android.apk` sang điện thoại rồi mở file để cài.
  - Điện thoại sẽ hỏi quyền cài app từ nguồn ngoài, hãy cho phép.

Đóng gói bản mới bằng `scripts\package-release.ps1`. Kết quả nằm trong `build\dist`.

## Điện thoại làm webcam (Wi‑Fi)

- **Chạy trên PC:** [CamStudio.exe](app/) tự tìm điện thoại trong cùng mạng Wi‑Fi (mDNS `_camstudio._tcp`). Nếu mạng chặn mDNS, chọn **Thêm điện thoại theo địa chỉ IP…** rồi nhập địa chỉ hiện trên app điện thoại.
- **Ghép đôi lần đầu:** PC và điện thoại cùng hiện một **mã 6 số**. Bấm **Đồng ý** trên điện thoại nếu hai mã giống nhau. Sau lần này, kết nối tự xác thực, và mọi dữ liệu đều được mã hóa AES‑256‑GCM.
- **Tận dụng hết camera của điện thoại:**
  - Điện thoại tự báo **mọi ống kính**: góc rộng, siêu rộng, tele, camera trước, và camera ảo tự đổi ống kính.
  - Kèm theo mọi độ phân giải/fps mà bộ mã hóa phần cứng hỗ trợ, cùng HDR, chống rung và các chỉnh tay.
  - PC dựng bảng điều khiển (icon điện thoại ở thanh công cụ) từ đúng những gì máy đó có.
  - **iPhone:** có thêm nút mở bảng hiệu ứng hệ thống (Xóa phông, Ánh sáng studio, Center Stage). Chế độ Điện ảnh có từ iOS 26.
- **Xoay điện thoại:** hướng ảnh và khung dọc/ngang trên PC tự đổi theo.
- **Mạng yếu:** bitrate tự giảm để hình không bị trễ dồn.

| Phần | Thư mục | Build |
|---|---|---|
| Đặc tả giao thức (chung cho cả ba bên) | [docs/protocol.md](docs/protocol.md) | — |
| App Android (Kotlin, Camera2 + MediaCodec) | [android/](android/) | Windows: `scripts\build-android.ps1` → `build\CamStudioPhone.apk`. Mac/Linux: `cd android && ./gradlew assembleRelease` |
| App iPhone (Swift, AVFoundation + VideoToolbox) | [ios/](ios/) | Trên Mac: `scripts/ios-run.sh` (Xcode + XcodeGen), hoặc trên Windows (WSL) bằng xtool. Xem [ios/README.md](ios/README.md) |
| PC giả để thử app điện thoại trên Mac | [tools/fake_pc.swift](tools/fake_pc.swift) | `scripts/fake-pc.sh --lens all` |
| Kiểm tra mật mã khớp giữa các bên | [docs/protocol-vectors.json](docs/protocol-vectors.json) | `build\Release\camstudio-protocol-test.exe` (PC), `gradlew testDebugUnitTest` (Android), scheme `CryptoTestsMac` trong Xcode (iOS, chạy ngay trên Mac) |

**Cài app Android:**
- Bật **Gỡ lỗi USB** rồi chạy `adb install build\CamStudioPhone.apk`.
- Hoặc chép file APK sang điện thoại rồi mở để cài.

## Kiến trúc

```
webcam USB ──► CamStudioEngine.dll ──(shared memory Global\, NV12)──► CamStudioVCam.dll ──► Zoom / OBS / Teams
               chụp ở chế độ đã chọn                                   (Frame Server nạp,
               MJPG/YUY2 → NV12                                         chạy trong session 0)
               xoay/lật → cắt khung/zoom
               MFCreateVirtualCamera
                    ▲
                    │ C API (P/Invoke)
               CamStudio.exe (WinUI 3, C#)
```

- **`app/`**: giao diện **WinUI 3** (C#, .NET 10, Windows App SDK đóng gói kèm, không cần cài thêm).
  - **Phong cách Windows 11:** nền Mica, thanh tiêu đề riêng, các thẻ cài đặt giống app Settings.
  - **Giao diện:** Sáng/Tối theo Windows, hoặc chọn tay.
- **`src/engine`**: `CamStudioEngine.dll`, phần xử lý camera viết bằng C++, có C API ở [EngineApi.h](src/engine/EngineApi.h).
  - Chạy ở luồng riêng, khóa chế độ native đã chọn trước khi tạo source reader.
  - Xoay/lật, rồi cắt theo tỉ lệ và zoom số (bilinear).
  - Đăng ký camera ảo (lifetime `Session`) theo đúng kích thước đầu ra.
- **`src/vcam`**: camera ảo dạng Media Foundation media source (COM, dùng WRL), dựa theo [mẫu VirtualCamera](https://github.com/microsoft/Windows-Camera/tree/master/Samples/VirtualCamera) (MIT) của Microsoft.
  - **Định dạng cung cấp:** kích thước đầu ra trước tiên, sau đó các cỡ nhỏ hơn cùng tỉ lệ khung hình. Mỗi cỡ có fps gốc, và có thêm bản 30 fps nếu fps gốc cao hơn 30.
- **`src/verify`**: mở camera ảo theo cách một app bình thường mở. Nó kiểm tra định dạng lớn nhất có bằng độ phân giải gốc của camera thật không, và đọc 30 khung hình để chắc đó là hình thật chứ không phải màn hình "no signal".
- **`src/common/SharedFrame.h`**: quy ước dùng chung giữa engine và DLL camera ảo: tên object, seqlock, DACL.

## Build

Cần:
- Visual Studio 2022 Build Tools (workload C++, Windows SDK 10.0.22000 trở lên)
- .NET SDK 10
- Internet cho lần build đầu, để tải gói NuGet Windows App SDK

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1        # C++ (CMake) rồi app WinUI
powershell -ExecutionPolicy Bypass -File scripts\make-icon.ps1    # chỉ khi muốn vẽ lại icon
```

App được build ra `app\bin\x64\Release\net10.0-windows10.0.22621.0\win-x64\CamStudio.exe`.

## Cài đặt (một lần, cần quyền Admin)

```powershell
powershell -ExecutionPolicy Bypass -File scripts\register.ps1
```

Script sẽ dừng dịch vụ Frame Server, nên app nào đang dùng camera (Camo, Teams, trình duyệt…) sẽ bị ngắt. Sau đó nó copy DLL camera ảo vào `C:\Program Files\CamStudio` và đăng ký COM vào HKLM. Log được ghi ở `build\register.log`.

Chỉ khi sửa `src/vcam` mới cần chạy lại `register.ps1`. Gỡ cài đặt bằng `scripts\unregister.ps1`.

## Chạy

```powershell
CamStudio.exe                                         # mở app
CamStudio.exe --device obsbot                         # chọn sẵn camera và tự bật camera ảo
CamStudio.exe --tray                                  # chạy ẩn ở khay, bật lại camera lần trước
build\Release\camstudio-verify.exe --device obsbot    # kiểm tra camera ảo (khi đang ở chế độ gốc, không xoay/cắt)
```

**Trong app:**

- **Thanh trên:**
  - Chọn **camera** và **độ phân giải** (mặc định là chế độ gốc). Đổi là áp dụng ngay; app đang dùng camera ảo sẽ mất hình khoảng một giây trong lúc camera ảo được tạo lại.
  - Bên phải là công tắc **Camera ảo** kèm trạng thái.
- **Thanh công cụ dưới ảnh:** mỗi chức năng là một icon, di chuột vào để xem tên. Icon nào có chấm nhỏ hoặc được tô màu là đang khác mặc định.

  | Icon | Chức năng |
  |---|---|
  | Lật ngang, Lật dọc | Bật/tắt, áp dụng ngay |
  | Xoay (0°/90°/180°/270°) | Mỗi lần bấm xoay thêm 90°. 90° và 270° đổi sang khung dọc, nên camera ảo tự khởi động lại |
  | Tỉ lệ khung | Gốc, 16:9, 4:3, 1:1, 9:16. Cắt ở giữa, không méo hình; camera ảo tự khởi động lại |
  | Thu phóng | Zoom số 1–4×, giữ nguyên độ phân giải đầu ra |
  | Chỉnh ảnh | Độ sáng, tương phản, độ bão hòa, nhiệt độ màu. Xử lý bằng SIMD, áp dụng cho cả camera ảo |
  | Thông tin | Chế độ gốc, đầu ra, fps thực tế, khung bị rơi, thời gian xử lý, CPU/RAM, ứng dụng đang dùng camera |
  | Cài đặt | Giao diện Sáng/Tối, Khởi động cùng Windows, đóng cửa sổ thì chạy ở khay hay thoát hẳn |

- **Trên ảnh xem trước:**
  - Cuộn chuột để thu phóng.
  - Khi đang phóng to, kéo để di chuyển khung.
  - Nhấp đúp để đặt lại.
- **Đóng cửa sổ (X):** mặc định app thu xuống khay hệ thống và camera ảo vẫn chạy. Chuột phải vào biểu tượng ở khay để Bật/Tắt camera ảo, Lật ngang, **Khởi động cùng Windows** hoặc **Thoát**.

**Tiết kiệm tài nguyên:**
- **Không ai xem thì không xử lý:** khi không có app nào dùng camera ảo và cửa sổ đang ẩn hoặc thu nhỏ, app chỉ giữ cho camera chạy, không xử lý từng khung (đo được khoảng 1–2% của một nhân CPU).
- **Xem trước vừa đủ:** ảnh xem trước chỉ được tạo đúng bằng kích thước đang hiển thị, tối đa 30 fps, và được chép thẳng vào bộ nhớ ảnh.
- **Không chuyển đổi thừa:** với camera xuất sẵn NV12, khung đi thẳng mà không qua bộ chuyển đổi.
- **Chỉ một bản chạy:** mở app lần nữa thì cửa sổ đang chạy sẽ hiện lên.
- **Lưu cài đặt:** mọi lựa chọn được lưu ở `HKCU\Software\CamStudio`.

## Gỡ lỗi

DLL camera ảo chạy trong Frame Server (LocalService) nên không ghi được file log. Để xem log, chạy [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview) bằng quyền Admin, bật **Capture Global Win32** và lọc theo `[CamStudioVCam]`.
