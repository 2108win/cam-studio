# Cam Studio so với Camo Studio: tính năng còn thiếu và lộ trình

Ngày 2026-10-18. Bản Camo được xem: **Camo Studio 2.9.0 (build 25534) cho macOS**, cài ở `/Applications/Camo Studio.app`.

## Phạm vi (clean-room)

Theo quy tắc trong `CLAUDE.md`, Camo chỉ được nghiên cứu ở **mức kiến trúc**:

- **Đã xem:** cấu trúc gói ứng dụng, `Info.plist`, tên các App Intent, tên chuỗi giao diện và tên file tài nguyên. Cùng với đó là các trang công khai của Reincubate (nguồn ở cuối).
- **Không làm:**
  - không dịch ngược file thực thi hay framework (class-dump, Ghidra, Hopper, `strings`);
  - không cài skill "REA" hay công cụ dịch ngược nào;
  - không đụng tới cơ chế cấp phép của Camo Pro.
- **Không mang dữ liệu của Camo vào repo:** LUT `.cube`, `presets.json`, `devices.json`, file `.camoscene`, ảnh nền, prompt nền AI. Nếu Cam Studio cần preset hay LUT, ta tự làm từ đầu.

"Có được tính năng như Camo Pro" ở đây nghĩa là **tự xây tính năng tương đương**, không phải mở khóa Camo.

## Kết luận chính

1. **Camo thu tiền gần như chỉ ở chất lượng hình.** Trang giá chính thức hiện nay:
   - **Bản miễn phí:** tối đa 720p, 30 fps.
   - **Bản Pro:** từ 1080p tới 4K, tới 60 fps, ghi hình độ phân giải siêu cao, dùng được trên 2 máy.

   Các bài review cũ (2023) lại xếp xóa phông, mọi ống kính, đèn flash và việc bỏ watermark vào Pro. Hai nguồn này mâu thuẫn nhau; tài liệu này lấy trang giá hiện tại làm chuẩn.
2. **Cam Studio đã miễn phí đúng phần Camo thu tiền:** độ phân giải gốc (4K30 HEVC, 1080p60, webcam 1080p100), mọi ống kính (kể cả ống MIUI giấu đi), đèn flash, và không có watermark.
3. **Vậy khoảng cách nằm ở tính năng, không ở gói cước:** hiệu ứng nền, tự căn khung, bộ lọc màu, ghi hình, âm thanh, cảnh/lớp, phát trực tiếp, điều khiển từ bên ngoài.

## Kiến trúc Camo trên Mac (mức cao) và tương đương trên Windows

| Thành phần trong gói Camo | Vai trò | Cam Studio trên Windows |
|---|---|---|
| CoreMediaIO camera system extension | camera ảo | `IMFVirtualCamera` + `src/vcam` (đã có) |
| `XPCServices/DAL Proxy.xpc` | camera cho app cũ dùng DAL | Frame Server đã phục vụ cả app DirectShow (Discord), không cần thêm |
| `PlugIns/ReincubateCamoAudio.driver` (HAL plugin) | **micro ảo**: tiếng từ điện thoại hoặc đã trộn | Windows cần **driver âm thanh có chữ ký** (khó hơn nhiều so với HAL plugin trên Mac) |
| `SwiftyMobileDevice`, `USBKit` | iPhone qua cáp (usbmuxd) | `src/engine/UsbTransport.cpp` (đã viết, chưa build trên Windows) |
| `default.metallib`, `FilterKit`, `CocoaLUTSwift` | hiệu ứng trên GPU, LUT 3D | chưa có; trên Windows sẽ là shader D3D11 |
| `CamoStreamer`, `CamoChat` | phát RTMP nhiều nền tảng, xem chat | chưa có |
| Kiểu file `.camoscene`, thư mục cảnh mẫu | cảnh, lớp, overlay | chưa có |
| `Metadata.appintents` (~30 intent) | Phím tắt macOS điều khiển lens, zoom, preset, overlay… | chưa có |
| `CamoStreamDeckPlugin` | nút Stream Deck | chưa có |
| `Helpers/camo-mcp` | cho trợ lý AI (như Claude) điều khiển app qua MCP | chưa có |
| `devices.json` | thông tin riêng từng mẫu webcam (ống kính, tính năng tắt) | chưa có |
| `Sparkle.framework` | tự cập nhật | chưa có |

Bản Camo cho Windows có thể khác bản Mac (ví dụ, theo blog 2.8, trộn âm thanh có trên Windows). Muốn so trên Windows thì xem release notes của Camo cho Windows.

## Bảng đối chiếu

Ký hiệu: **Có**, **Một phần**, **Chưa**. Gói Camo "không rõ" là chỗ các nguồn công khai không ghi rõ. Công sức: **S** ≤ 1 ngày, **M** vài ngày, **L** 1–2 tuần, **XL** lâu hơn.

### Nguồn hình và chất lượng

| Tính năng Camo | Gói Camo | Cam Studio | Nên làm ở đâu | Công sức |
|---|---|---|---|---|
| Điện thoại iOS/Android làm camera | Free | **Có** | — | — |
| Webcam, capture card, DSLR | Free | **Có** (Media Foundation) | — | — |
| Trên 720p, tới 4K | Pro | **Có**, miễn phí, đúng độ phân giải gốc | — | — |
| 60 fps | Pro | **Có** (1080p60; webcam tới 100) | — | — |
| Qua cáp USB | không rõ | **Một phần**: đã viết, chưa build trên Windows | Open work mục 4 | M |
| HDR / 10-bit | không rõ | **Một phần**: iPhone gửi HEVC 10-bit HLG; PC chưa kiểm HEVC từ đầu tới cuối | PC | S–M |
| Smart Zoom (quay 4K, cắt ra 1080p nét) | không rõ | **Một phần**: PC đã có zoom số 1–4× và chọn tâm | PC: tùy chọn "nhận 4K, phát 1080p" | S |
| Thu cửa sổ/màn hình PC | không rõ | **Chưa** (chỉ có màn hình điện thoại) | PC: Windows.Graphics.Capture | M |

### Điều khiển camera

| Tính năng Camo | Gói Camo | Cam Studio | Nên làm ở đâu | Công sức |
|---|---|---|---|---|
| Mọi ống kính | bài cũ ghi Pro | **Có**, cả ống MIUI giấu đi | — | — |
| Đèn flash | bài cũ ghi Pro | **Có** | — | — |
| Lấy nét, phơi sáng, ISO, cân bằng trắng tay | không rõ | **Có** (iPhone đã kiểm; bảng nâng cao trên Redmi chưa kiểm) | Open work mục 2 | — |
| Chống rung | Camo: chỉ iOS | **Có** (iOS) | — | — |
| Lật, xoay, cắt khung, tỉ lệ | không rõ | **Có** | — | — |
| Làm tối màn hình điện thoại ("screen curtain") | không rõ | **Có** (app tự làm tối) | — | — |
| Bù ngược sáng (chống bóng đen khi sau lưng sáng) | không rõ | **Chưa** | điện thoại: đo sáng theo mặt / bù phơi sáng | S |

### Hiệu ứng nền

| Tính năng Camo | Gói Camo | Cam Studio | Nên làm ở đâu | Công sức |
|---|---|---|---|---|
| Xóa phông (có thanh độ mạnh) | trang tổng quan: Free | **Một phần**: iPhone qua bảng hiệu ứng hệ thống (PC không chỉnh độ mạnh được) và chế độ Điện ảnh; Android và webcam: chưa | điện thoại trước (xem lộ trình), webcam trên PC | M–L |
| Thay nền bằng ảnh/video | trang tổng quan: Free | **Một phần**: iPhone iOS 18 qua bảng hệ thống; còn lại chưa | như trên | M–L |
| Làm mờ riêng tư (mờ mọi thứ trừ người) | không rõ | **Chưa** | như trên | M |
| Phông xanh thật (chroma key, tự hiệu chỉnh) | không rõ | **Chưa** | PC (rẻ, nhưng chỉ có ích khi có nền thay thế) | S–M |
| Spotlight (làm nổi người, tối xung quanh) | không rõ | **Chưa** | cần tách người, giống xóa phông | M |
| Nền AI tạo theo kiểu | không rõ | **Chưa** | **không nên**: cần máy chủ hoặc mô hình tạo ảnh lớn | — |

### Khung hình

| Tính năng Camo | Gói Camo | Cam Studio | Nên làm ở đâu | Công sức |
|---|---|---|---|---|
| Tự căn khung (theo mặt, có hoặc không zoom) | không rõ | **Chưa** cho Android/webcam; iPhone có Center Stage qua bảng hệ thống | PC: `Windows.Media.FaceAnalysis.FaceTracker` lái zoom/tâm đã có, chạy cho mọi nguồn | M |
| Center Stage của Apple | không rõ | **Một phần** (bảng hệ thống) | iPhone: bật/tắt trực tiếp từ PC qua `set` | S |
| Điều khiển zoom/pan tay | không rõ | **Có** | — | — |

### Màu sắc

| Tính năng Camo | Gói Camo | Cam Studio | Nên làm ở đâu | Công sức |
|---|---|---|---|---|
| Sáng, tương phản, bão hòa, nhiệt độ màu | không rõ | **Có** | — | — |
| Hue, gamma, độ nét, vibrance | không rõ | **Chưa** | PC, cùng chỗ `Nv12ColorAdjust` | S |
| Preset (lưu bộ chỉnh màu, đổi nhanh) | không rõ | **Chưa** | PC: JSON trong cài đặt app | S |
| Bộ lọc LUT 3D (`.cube`) có thanh độ mạnh | không rõ | **Chưa** | PC: shader D3D11, chỉ chạy khi có app đang dùng camera | M |

### Âm thanh

| Tính năng Camo | Gói Camo | Cam Studio | Nên làm ở đâu | Công sức |
|---|---|---|---|---|
| Micro điện thoại thành micro trên máy tính | không rõ | **Chưa** | gửi tiếng qua giao thức thì dễ; **micro ảo trên Windows cần driver có chữ ký** | XL |
| Lọc ồn | không rõ | **Chưa** | điện thoại: iOS voice processing, Android `NoiseSuppressor` (gần như miễn phí) | S (khi đã có tiếng) |
| Thu tiếng hệ thống, trộn tiếng | không rõ | **Chưa** | PC (WASAPI loopback) | M |

### Ghi hình và chụp ảnh

| Tính năng Camo | Gói Camo | Cam Studio | Nên làm ở đâu | Công sức |
|---|---|---|---|---|
| Ghi hình H.264/HEVC | Pro: ghi độ phân giải siêu cao | **Chưa** | PC, hai kiểu:<br>• **Ghi luồng gốc từ điện thoại:** đóng gói lại H.264/HEVC đã nhận vào `.mp4`, không mã hóa lại, gần như 0% CPU, giữ nguyên 4K. File **không có** cắt khung, zoom/pan, xoay hay chỉnh màu của PC; hướng điện thoại phải ghi vào ma trận xoay của MP4; chất lượng theo bitrate tự thích ứng.<br>• **Ghi đúng hình camera ảo đang phát** (và mọi webcam): phải mã hóa, dùng bộ mã hóa phần cứng Media Foundation | M |
| Chụp ảnh khung hình (PNG) | không rõ | **Chưa** | PC: lấy khung NV12 → WIC PNG. Có thể thêm: điện thoại chụp ảnh tĩnh độ phân giải đầy đủ | S |
| Ghi ngang + dọc cùng lúc | không rõ | **Chưa** | **không nên** (lệch khỏi hướng tối giản) | — |

### Cảnh, lớp, phát trực tiếp

| Tính năng Camo | Gói Camo | Cam Studio | Nên làm ở đâu | Công sức |
|---|---|---|---|---|
| Cảnh, lớp (chữ, ảnh, web, chat, PiP, mặt nạ, biến) | không rõ | **Chưa** | **không nên làm đầy đủ**: Cam Studio đã chạy trong OBS. Tối đa: một lớp chữ/ảnh (bảng tên, logo) | S–M (bản tối thiểu), XL (đầy đủ) |
| Hai nguồn cùng lúc (PiP, cạnh nhau) | không rõ | **Chưa** | PC, cần ghép hai luồng: nặng hơn hẳn | L |
| Phát RTMP (YouTube, Twitch, Facebook, Instagram), nhiều nền tảng, chat | không rõ | **Chưa** | **không nên**: để OBS lo | — |

### Điều khiển từ bên ngoài và cài đặt

| Tính năng Camo | Gói Camo | Cam Studio | Nên làm ở đâu | Công sức |
|---|---|---|---|---|
| Phím tắt hệ thống / URL scheme | không rõ | **Chưa** | PC: `camstudio://` hoặc CLI nhỏ qua named pipe (đổi nguồn, ống kính, zoom, preset, bật/tắt camera ảo) và phím tắt toàn cục | S–M |
| Stream Deck | không rõ | **Chưa** | dựa trên CLI/URL scheme ở trên | M |
| MCP cho trợ lý AI | không rõ | **Chưa** | máy chủ MCP nhỏ dựa trên cùng lệnh điều khiển | S (khi đã có CLI) |
| Tự cập nhật | — | **Chưa** | PC: hỏi GitHub Releases khi mở app, tải Setup.exe; hoặc manifest winget | S–M |
| Watermark | bản Free hiện không có | **Không có** | — | — |

## Lộ trình đề xuất

Các việc trong "Open work" của `CLAUDE.md` vẫn đi trước: mục 1–4 (iPhone với PC thật, build `PhoneLink::Reconnect` và USB trên Windows, kiểm Redmi, TestFlight). Danh sách dưới đây xếp **sau** các việc đó. Nó giữ hai nguyên tắc của dự án: **tối giản**, và **không làm gì theo từng khung hình khi không ai dùng camera ảo**.

**Đợt A: nhỏ, giá trị cao, hợp hướng "nhẹ"**
1. **Ghi hình bằng cách đóng gói lại luồng từ điện thoại**, và chụp ảnh PNG. Đây là điểm Camo thu tiền ("ghi độ phân giải siêu cao"), mà Cam Studio làm được gần như không tốn CPU. Lưu ý: file là luồng gốc của điện thoại, không có cắt/zoom/màu của PC. Muốn ghi đúng hình camera ảo thì làm thêm đường mã hóa (cũng là đường cho webcam).
2. **Tự căn khung cho mọi nguồn** bằng `FaceTracker` của Windows, lái zoom và tâm sẵn có. Chỉ chạy khi có app đang dùng camera, vài lần mỗi giây là đủ.
3. **Smart Zoom:** nhận 4K từ điện thoại, phát 1080p đã cắt, nên zoom vẫn nét.
4. **Preset màu**, cùng hue/gamma/độ nét.
5. **Điều khiển từ ngoài:** URL scheme/CLI và phím tắt toàn cục. Sau đó MCP và Stream Deck dùng chung lớp lệnh này.
6. **Bật/tắt Center Stage trên iPhone thẳng từ PC** (`AVCaptureDevice.centerStageControlMode = .app`), thay vì chỉ mở bảng hệ thống. Xóa phông, Ánh sáng studio và Thay nền của hệ thống thì app chỉ đọc được, không bật được: muốn PC điều khiển (và có thanh độ mạnh) thì đi đường tách người bằng Vision ở mục 7.

**Đợt B: lớn hơn, cần cân nhắc**

7. **Xóa phông, thay nền, làm mờ riêng tư cho Android và webcam.**
   - **Ưu tiên chạy trên điện thoại** cho nguồn điện thoại: Android có MediaPipe Selfie Segmentation (giấy phép MIT/Apache, kiểm lại trong model card); iPhone có Vision `VNGeneratePersonSegmentationRequest`, cho phép PC chỉnh độ mạnh. PC vẫn nhẹ, và lệnh đi qua `set`/`caps` sẵn có.
   - **Webcam thì phải chạy trên PC:** ONNX Runtime + DirectML với một mô hình tách người có giấy phép MIT/Apache, chỉ chạy khi có app đang dùng camera.
   - Đây cũng là mục "AI background blur" trong Open work mục 5.
8. **Bộ lọc LUT 3D** bằng shader D3D11; ta tự làm vài LUT mẫu.
9. **Micro từ điện thoại:**
   - **Bước 1:** thêm loại bản ghi âm thanh vào giao thức (AAC hoặc Opus), lọc ồn ngay trên điện thoại.
   - **Bước 2 (khó):** micro ảo trên Windows cần driver âm thanh có chữ ký. Cần quyết định riêng trước khi làm.

**Không nên làm** (để OBS lo, hoặc trái với hướng tối giản và "không qua máy chủ"): trình soạn cảnh đầy đủ, phát RTMP và chat, nền do AI tạo, ghi ngang và dọc cùng lúc.

## Nguồn

- Trang giá Camo: [camo.com/pricing](https://camo.com/pricing)
- Tổng quan Camo: [camo.com/support/camo/camo-overview](https://camo.com/support/camo/camo-overview)
- Release notes Camo: [camo.com/support/camo/release-notes](https://camo.com/support/camo/release-notes)
- Giới thiệu Camo Studio 2.8: [camo.com/blog/camo-studio-2-8](https://camo.com/blog/camo-studio-2-8)
- Bài TidBITS về Camo Studio 2.9: [tidbits.com/?p=75706](https://tidbits.com/?p=75706)
- Review iMore (giá và phân chia Free/Pro thời trước): [imore.com](https://www.imore.com/reincubate-camo-studio-review-turn-your-iphone-webcam-your-dreams)
