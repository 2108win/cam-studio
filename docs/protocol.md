# Giao thức Cam Studio v1

Đây là hợp đồng **duy nhất** giữa app điện thoại (Android Kotlin, iOS Swift) và Cam Studio trên PC (C++). Khi đổi giao thức thì sửa file này trước, rồi sửa cả ba bên theo.

- **Vai trò:** điện thoại là **máy chủ**, PC là **máy khách**.
- **Đường truyền:** Wi‑Fi trong nhà là mặc định. USB (adb cho Android, usbmux cho iPhone) chỉ là một đường ống TCP khác, chạy y nguyên giao thức này (mục 1.2).
- **Mã hóa:** mọi dữ liệu sau bước bắt tay đều được mã hóa.

---

## 1. Tìm nhau trong mạng (DNS‑SD / mDNS)

Điện thoại quảng bá dịch vụ:

| Trường | Giá trị |
|---|---|
| Loại dịch vụ | `_camstudio._tcp` |
| Cổng | **47321** nếu cổng này trống, nếu không thì một cổng trống bất kỳ. Dùng cổng cố định để địa chỉ "Thêm theo IP" vẫn đúng sau khi mở lại app |
| TXT `v` | `1` (phiên bản giao thức) |
| TXT `id` | UUID cố định của máy, sinh lần đầu và lưu lại |
| TXT `name` | tên máy người dùng đặt, ví dụ "iPhone của Lan" |
| TXT `model` | ví dụ `iPhone16,1`, `Redmi 9T` |
| TXT `os` | `ios` hoặc `android` |

PC duyệt `_camstudio._tcp` và đưa mỗi máy vào danh sách camera, ví dụ "iPhone của Lan (Wi‑Fi)".

**Khi mDNS không chạy:** nhiều mạng trong nhà chặn mDNS, ví dụ router cách ly máy khách, mạng 2.4/5 GHz tách riêng, mesh hoặc mạng khách. Khi đó dùng cách thủ công:
- **Trên điện thoại:** màn hình app luôn hiện `IP:cổng` và một mã QR chứa `camstudio://<ip>:<port>?id=<deviceId>`.
- **Trên PC:** chọn "Thêm theo địa chỉ IP" rồi nhập địa chỉ đó.

**iOS:** `Info.plist` bắt buộc có `NSLocalNetworkUsageDescription` và `NSBonjourServices = ["_camstudio._tcp"]`. Thiếu thì việc quảng bá thất bại mà không báo lỗi gì.

### 1.1 Chia sẻ màn hình (iPhone, Android)

Trên iPhone, màn hình được chia sẻ bằng một tiện ích ReplayKit (Broadcast Upload Extension) chạy trong tiến trình riêng.
- **Người dùng bật trên iPhone:** bấm nút trong app, hoặc giữ nút Ghi màn hình trong Trung tâm điều khiển. PC không bật được.
- **Dịch vụ riêng:** trong lúc chia sẻ, tiện ích quảng bá một dịch vụ `_camstudio._tcp` thứ hai, PC đưa nó vào danh sách như một nguồn riêng:

| Trường | Giá trị |
|---|---|
| Cổng | **47322** nếu trống, nếu không thì một cổng trống bất kỳ |
| TXT `id` | `<deviceId>:screen`, để PC coi là nguồn khác với camera |
| TXT `name` | tên máy + ` · Màn hình`, ví dụ "iPhone · Màn hình" |
| TXT `src` | `screen` |

- **`hello` vẫn dùng `deviceId` gốc:** khóa ghép đôi của PC gắn với id này, nên một PC đã ghép với app thì kết nối thẳng vào màn hình.
- **Tiện ích không ghép đôi được:** nó không có màn hình để hỏi. Nếu PC gửi `pairStart`, tiện ích trả `error` với `code = "notPaired"`.
- **`caps`:** chỉ có một ống kính `{"id": "screen", "facing": "screen", "kind": "screen"}`, không có điều khiển. `modes` gồm kích thước gốc của ReplayKit (888×1920 trên iPhone 14) và các bản thu nhỏ.
- **Hướng xoay:** khung luôn là khung dọc của màn hình. Khi xoay máy, `orientation` trong `state` đổi theo, giống camera.
- **Màn hình đứng yên vẫn có khung:** ReplayKit chỉ gửi khung khi màn hình thay đổi. Vì vậy tiện ích gửi lại khung cuối vài lần mỗi giây (PC ngắt nguồn im lặng quá 6 giây), và gửi ngay một khung khi bắt đầu phát hoặc khi được xin `keyframe`.

**Android:** dùng cùng cổng, cùng TXT, cùng `caps` và cùng lỗi `notPaired`. Chỉ khác cách lấy màn hình:
- **Cách lấy màn hình:** MediaProjection, chạy ngay trong app, trong một foreground service loại `mediaProjection`.
- **Bật:** người dùng bấm "Chia sẻ màn hình sang máy tính" trong app rồi đồng ý trong hộp thoại của hệ thống. Mỗi lần chia sẻ phải đồng ý lại (Android 14 trở lên).
- **Tắt:** bấm "Dừng" trong thông báo, bấm lại nút trong app, hoặc dùng nút dừng của hệ thống.
- **Chạy nền:** vẫn chạy khi đang mở ứng dụng khác.
- **Hướng xoay:** `modes` ưu tiên khung ngang, vì nhiều bộ mã hóa Android chỉ nhận tới 1920×1088. Ví dụ Redmi 9T: 1920×880 và 1280×592.
  - Khi hình màn hình và khung khác chiều (màn hình dọc trong khung ngang, hoặc ngược lại), app xoay hình một phần tư vòng theo chiều kim đồng hồ và báo `orientation = 270` để PC xoay lại.
  - Khi cùng chiều, `orientation = 0`.
- **Kích thước:** mọi kích thước đều là bội số của 16.

### 1.2 Qua cáp USB

Cáp chỉ thay đường ống TCP. Bắt tay, ghép đôi, mã hóa và mọi bản ghi giữ nguyên như qua Wi‑Fi. App điện thoại **không cần sửa gì**:
- máy chủ của app nghe trên mọi giao diện mạng;
- đường ống qua cáp đi vào cổng của app trên chính điện thoại (loopback).

**PC tìm điện thoại trên cáp** (không có mDNS, nên không có TXT):

| | Android | iPhone |
|---|---|---|
| Dịch vụ trên PC | adb server, `127.0.0.1:5037` | usbmuxd: trên Windows là Apple Mobile Device Service `127.0.0.1:27015` (cài cùng Apple Devices hoặc iTunes); trên Mac là `/var/run/usbmuxd` |
| Danh sách, tự cập nhật | `host:track-devices`, rồi `host:devices-l` | `Listen`: các tin `Attached` / `Detached` |
| Chỉ lấy cáp | bỏ serial dạng `ip:port`, `…._adb-tls-…`, `emulator-…` | chỉ `ConnectionType = "USB"` (iPhone đồng bộ qua Wi‑Fi có `"Network"`) |
| Tên máy | `shell:settings get global device_name`, giống app (`Store.deviceName`) | `DeviceName` từ lockdownd (cổng 62078), hỏi `GetValue` không cần phiên: không ghép đôi, iPhone không hiện gì |
| Mở đường ống | `host:transport:<serial>` rồi `tcp:47321` | `Connect` với `DeviceID` và `PortNumber` = cổng **đảo byte** (thứ tự mạng, đọc như số little‑endian: 47321 → 55736) |
| App chưa mở | `FAIL` cho `tcp:` | kết quả `Number = 3` |

- **Khung tin của adb:** mỗi yêu cầu là 4 chữ số hex độ dài rồi tới nội dung; trả lời là `OKAY`, hoặc `FAIL` kèm 4 chữ số hex độ dài và lý do.
- **Khung tin của usbmuxd:** đầu 16 byte little‑endian (độ dài kể cả đầu, version `1`, loại `8` = plist, tag) rồi plist XML.
- **Khung tin của lockdownd:** `u32` big‑endian độ dài rồi plist XML.

**Chỉ dùng cổng cố định:** qua cáp PC chỉ biết cổng 47321 (camera) và 47322 (màn hình). Nếu app phải lùi về cổng ngẫu nhiên (mục 1) thì không kết nối qua cáp được.

**Danh tính:**
- PC gọi nguồn là `"<tên máy> (USB)"` và giữ nó theo serial (adb) hoặc UDID (iPhone).
- `deviceId` vẫn lấy từ `hello`, nên khóa ghép đôi qua Wi‑Fi dùng luôn cho cáp, không phải ghép lại.
- Tên chỉ được đưa vào danh sách khi đã đọc xong, để không đổi trong lúc máy còn cắm (app trên PC tìm lại camera theo tên).

**Khi mất kết nối:**
- PC kết nối lại qua đúng cáp đó, theo serial hoặc UDID. `DeviceID` của usbmux đổi mỗi lần cắm, nên được tra lại theo UDID mỗi lần.
- Rút cáp: PC chờ (trạng thái Waiting), camera ảo vẫn giữ. PC **không** tự chuyển sang Wi‑Fi.

**iPhone khóa màn hình hoặc app chạy nền:**
- Lúc đầu, kết nối vẫn được nhận nhưng app không trả lời `hello`. PC chờ 8 giây rồi báo "mở khóa điện thoại và mở app".
- Sau khi iOS thu hồi cổng nghe, `Connect` trả `3`.

**adb trên Windows:**
- Bộ cài kèm `adb.exe` trong `App\adb`.
- PC chỉ chạy adb server khi chưa có server nào **và** Windows thấy một giao diện ADB, tức có điện thoại bật Gỡ lỗi USB đang cắm. Giao diện ADB thuộc lớp `{F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}`, cũng là lớp adb dùng để tìm máy.
- PC tắt server do chính nó chạy khi không còn điện thoại nào trên adb quá một phút (và Windows không còn thấy giao diện ADB), hoặc khi thoát. Lý do: adb quét USB mỗi giây. Server do công cụ khác chạy thì để nguyên.
- PC không bao giờ chạy `adb.exe` khi đã có server: adb khác phiên bản sẽ tắt server mà công cụ khác đang dùng.

**Chia sẻ màn hình qua cáp** (mục 1.1): cùng đường ống, tới cổng **47322**.
- **Trong danh sách:** với mỗi điện thoại sẵn sàng trên cáp, PC liệt kê thêm `"<tên máy> · Màn hình (USB)"`, giữ theo serial/UDID và cổng.
  - Android còn chờ cho phép gỡ lỗi USB thì không liệt kê.
  - PC **không** dò cổng 47322 để biết điện thoại có đang chia sẻ hay không: app cũ sẽ bỏ phiên đang phát (mục 3.1). Nguồn luôn có trong danh sách.
- **Khi chưa chia sẻ:** không có gì nghe trên 47322, nên adb trả `FAIL` cho `tcp:47322`, usbmux trả `3`. PC báo "chưa chia sẻ màn hình" và chỉ cách bật.
- **Danh tính:** `hello` mang `deviceId` gốc như qua Wi‑Fi, nên khóa ghép đôi của camera dùng luôn. Tiện ích và service không ghép đôi được (`notPaired`).
- **Khi dừng chia sẻ:** PC chờ (Waiting) và thử lại qua đúng cáp đó. Bật chia sẻ lại thì hình tự quay lại.

**Tường lửa:** PC chỉ kết nối ra ngoài (ra điện thoại), nên Windows không hiện hộp thoại tường lửa.

---

## 2. Khung dữ liệu trên TCP

Mỗi **bản ghi** có dạng:

```
u32  length        // big-endian, số byte theo sau (tối đa 16 MiB)
u8   type
...  body          // length - 1 byte
```

**Trước khi bắt tay xong**, chỉ dùng `type = 0x01` (HANDSHAKE), và body là JSON UTF‑8 không mã hóa.

**Sau khi bắt tay xong**, mọi bản ghi đều có `type = 0x02` (SEALED). Body là:

```
u64  counter       // big-endian, tăng 1 mỗi bản ghi theo từng chiều, bắt đầu từ 0
...  ciphertext    // AES-256-GCM(plaintext), tag 16 byte nối cuối
```

- **Nonce GCM (12 byte):** `u32 direction || u64 counter`, cả hai big‑endian. `direction` là `1` cho chiều PC → điện thoại và `2` cho chiều điện thoại → PC.
- **AAD (dữ liệu xác thực kèm) của GCM:** 9 byte `u8 type(=0x02) || u64 counter`.
- **Tag:** 16 byte, nằm **cuối** ciphertext.
- **Counter:** phải đúng bằng counter trước đó cộng 1. Bản ghi đầu tiên mỗi chiều là 0. Sai counter thì đóng kết nối.
- **Giới hạn kích thước:** trước khi xác thực xong, một bản ghi tối đa **64 KiB**; sau khi xong tối đa **16 MiB**. Vượt giới hạn thì đóng kết nối.

**Plaintext** sau khi giải mã:

```
u8   kind
i64  ptsMicros     // thời điểm khung trên đồng hồ của điện thoại, µs (0 nếu không dùng)
u32  flags
...  payload
```

| kind | Tên | Chiều | payload |
|---|---|---|---|
| 1 | CONTROL | hai chiều | JSON (mục 4) |
| 2 | VIDEO_CONFIG | ĐT → PC | Thông tin khởi tạo bộ giải mã: SPS/PPS/VPS dạng **Annex‑B** (có mã bắt đầu `00 00 00 01`) |
| 3 | VIDEO_FRAME | ĐT → PC | Một khung đã nén, **Annex‑B**. `flags & 1` = khung khóa |

**iOS:** VideoToolbox xuất khung dạng AVCC (mỗi NAL có 4 byte độ dài ở đầu), còn SPS/PPS/VPS nằm trong `CMFormatDescription`. App iOS phải đổi sang Annex‑B trước khi gửi:
- thay 4 byte độ dài bằng `00 00 00 01`;
- gửi VIDEO_CONFIG mỗi khi `CMFormatDescription` thay đổi.

VIDEO_CONFIG có thể kèm NAL khác ngoài VPS/SPS/PPS. Ví dụ, HEVC 10‑bit HLG của iPhone có thêm SEI (loại 39). Bên nhận không được giả định trong đó chỉ có tham số bộ giải mã.
| 4 | AUDIO_FRAME | ĐT → PC | (dành cho sau này) |

---

## 3. Bắt tay và ghép đôi

**Mật mã dùng chung**, có sẵn trên cả ba nền tảng:

| Thành phần | Thuật toán | Android | iOS | Windows |
|---|---|---|---|---|
| Trao đổi khóa | ECDH P‑256 | `KeyAgreement("ECDH")` | CryptoKit `P256.KeyAgreement` | BCrypt `ECDH_P256` |
| Dẫn xuất khóa | HKDF‑SHA256 | | | |
| Xác thực | HMAC‑SHA256 | | | |
| Mã hóa | AES‑256‑GCM | | | |

**Quy ước byte**, bắt buộc. Ba bên viết riêng nên chỉ cần lệch 1 byte là không ghép được:

- **Khóa công khai:** dạng không nén **đúng 65 byte** `04 || X || Y`, với X và Y mỗi số **đúng 32 byte big‑endian**.
  - Java: `BigInteger.toByteArray()` có thể trả 31 hoặc 33 byte, nên phải cắt hoặc đệm về 32.
  - Windows: `BCRYPT_ECCPUBLIC_BLOB` = header 8 byte + X + Y (big‑endian), bỏ header rồi thêm `04` ở đầu.
  - Trong JSON thì mã base64 chuẩn (RFC 4648 có dấu `=`).
- **Bí mật ECDH `Z`:** **tọa độ X của điểm chung, 32 byte big‑endian**.
  - Windows: `BCryptDeriveKey(..., BCRYPT_KDF_RAW_SECRET)` trả về dạng **little‑endian**, nên phải đảo ngược byte.
- **Chuỗi** (`clientId`, `deviceId`…): giữ nguyên **đúng byte UTF‑8 nhận được** trong JSON, không chuẩn hóa hoa/thường.
  - Lý do: UUID của Swift là chữ hoa, của Java/C# là chữ thường. Mỗi bên tự sinh id của mình một lần, sau đó luôn dùng nguyên văn.
- **`LP(x)`** = `u32 big‑endian độ dài || x`.
- **HKDF:** HKDF‑SHA256 theo RFC 5869 (extract rồi expand), **tự cài bằng HMAC‑SHA256** trên cả ba nền tảng. Android không có HKDF sẵn, nên không dùng API HKDF của nền tảng nào.

### 3.1 Trình tự

```
PC → ĐT  {"type":"hello","v":1,"clientId":"<uuid PC>","clientName":"<tên PC>",
          "commit":"<base64 SHA256(pcPub)>","auto":true}       // "auto" chỉ khi PC tự kết nối
ĐT → PC  {"type":"hello","v":1,"deviceId":"<uuid ĐT>","name":"…","model":"…","os":"ios|android",
          "pub":"<base64 phonePub>","paired":true|false}
PC → ĐT  {"type":"reveal","pub":"<base64 pcPub>"}
```

- **Kiểm tra cam kết:** điện thoại kiểm tra `SHA256(pcPub) == commit`.
- **Vì sao phải cam kết trước:** PC đã chốt khóa của mình trước khi thấy khóa điện thoại, nên kẻ đứng giữa không thể chọn khóa sao cho mã so sánh trùng nhau.
- **Bản ghi phiên (transcript):** `T = SHA256( LP("camstudio-v1") || LP(clientId) || LP(deviceId) || LP(pcPub) || LP(phonePub) )`.
- **Bí mật chung:** `Z = ECDH(pcPriv, phonePub)`, là tọa độ X dài 32 byte (xem quy ước byte ở trên).
- **Mỗi lúc một PC:** điện thoại chỉ phục vụ một PC. Kết nối mới chỉ thay phiên đang chạy **sau khi đã gửi `hello` hợp lệ**:
  - "hợp lệ" là đúng `type`, `v = 1`, có `clientId` và `commit` base64;
  - phiên cũ đóng ngay trước khi điện thoại trả `hello`, kể cả khi bắt tay sau đó thất bại (ví dụ `notPaired`).
- **PC tự kết nối (`"auto": true`):** PC kết nối sẵn mọi điện thoại đã ghép đôi ở chế độ nền (mục 3.5) mà người dùng không bấm chọn. Kết nối như vậy **không được lấy điện thoại khỏi PC khác**:
  - Điện thoại đang có phiên **và** `clientId` của phiên đó **khác** `clientId` trong `hello` này → điện thoại không gửi `hello` của mình mà gửi **một** bản ghi HANDSHAKE (`0x01`, không mã hóa) rồi đóng kết nối này; phiên đang có giữ nguyên:
    ```
    ĐT → PC  {"type":"error","code":"busy","message":"Điện thoại đang kết nối với <tên PC kia>"}
    ```
  - **Cùng `clientId`** (chính PC đó kết nối lại, ví dụ kết nối cũ đã chết sau khi Wi‑Fi chập chờn hoặc PC ngủ dậy mà điện thoại chưa biết): thay phiên cũ như kết nối thường. Nếu không, PC sẽ "bận với chính mình" cho tới khi phiên cũ hết hạn.
  - Không có phiên nào: nhận như thường.
  - Không có `auto` (người dùng chọn điện thoại, hoặc PC nối lại nguồn đang phát): thay phiên đang có như trước.
  - Quy tắc này áp dụng cho cả máy chủ camera (47321) lẫn máy chủ màn hình (47322).
- **Kết nối không phải PC:** đóng trước khi gửi `hello`, gửi thứ khác, hoặc im lặng quá **10 giây**. Điện thoại đóng kết nối đó, còn phiên đang phát và dòng trạng thái trên app giữ nguyên. Vì vậy dò xem cổng có mở không (ví dụ qua cáp USB) không làm rớt hình.
- **App cũ:** các bản build trước thay đổi này (ngày 2026‑10‑18) bỏ phiên ngay khi nhận kết nối: mọi APK Android trước đó, và iOS TestFlight tới build 20261018.0743. Số phiên bản chưa đổi (Android 0.1.3, iOS 0.1.0), nên không dựa vào nó để phân biệt. Vì thế PC không dò cổng (mục 1.2).

### 3.2 Ghép đôi lần đầu (`paired == false`, hoặc PC không còn khóa ghép đôi)

Sau `reveal`, PC chọn một trong hai đường:
- **Có khóa ghép đôi với `deviceId` này và điện thoại báo `paired == true`:** gửi ngay `auth` (bước 3.3).
- **Ngược lại:** gửi `{"type":"pairStart"}` để bắt đầu ghép đôi.

1. **Hai bên cùng tính mã 6 số:** `SAS = (u32 big-endian của 4 byte đầu của SHA256("camstudio-sas" || T)) mod 1 000 000`, rồi viết đủ 6 chữ số (thêm số 0 ở đầu).
2. **Hiện mã và hỏi người dùng ở cả hai bên:** PC và điện thoại cùng hiện mã kèm nút **Đồng ý** / **Từ chối**.
   - **Phải đồng ý trên cả hai:** PC chỉ gửi `auth` (bước 3.3) sau khi người dùng bấm Đồng ý trên PC **và** điện thoại đã trả `pair accepted`.
   - **Vì sao cần cả hai:** nếu chỉ xác nhận trên điện thoại, một "điện thoại giả" trong mạng có thể tự đồng ý rồi đẩy hình vào camera ảo của PC.
3. **Khi người dùng bấm Đồng ý trên điện thoại:**
   ```
   ĐT → PC  {"type":"pair","accepted":true}
   ```
   Từ chối thì gửi `accepted:false` rồi đóng kết nối.
4. **Khóa ghép đôi lâu dài:** `K_pair = HKDF(ikm = Z, salt = T, info = "camstudio pair", 32 byte)`.
   - Điện thoại lưu khóa này theo `clientId`: Keychain trên iOS, EncryptedSharedPreferences/Keystore trên Android.
   - PC lưu theo `deviceId`, mã hóa bằng DPAPI của Windows.
5. Sau đó làm tiếp bước 3.3, dùng `K_pair` vừa tạo.

### 3.3 Xác thực hai chiều (mọi lần kết nối)

```
PC → ĐT  {"type":"auth","mac":"<base64 HMAC(K_pair, "pc" || T)>"}
ĐT → PC  {"type":"auth","mac":"<base64 HMAC(K_pair, "phone" || T)>"}
```

- **Ghép chuỗi:** dấu `||` ở đây là ghép byte thường; `"pc"` và `"phone"` là các byte ASCII.
- **Sai MAC:** bên nhận đóng kết nối ngay.
- **PC có khóa nhưng điện thoại báo `paired == false`:**
  - **Không xóa khóa cũ**, vì thông điệp này chưa được xác thực: bất kỳ ai trong mạng cũng có thể gửi nó để phá việc ghép đôi.
  - PC báo "Điện thoại yêu cầu ghép đôi lại" rồi làm bước 3.2.
  - Khóa mới **chỉ thay** khóa cũ khi bước 3.2 thành công.
- **Hỏi ghép đôi chỉ khi app đang mở:** điện thoại chỉ hiện hộp thoại ghép đôi lúc app đang hiện trên màn hình. Nếu không, nó trả `error` với `code = "background"`.

### 3.4 Khóa phiên

```
K_up   = HKDF(ikm = Z || K_pair, salt = T, info = "camstudio pc->phone", 32)
K_down = HKDF(ikm = Z || K_pair, salt = T, info = "camstudio phone->pc", 32)
```

- **Khóa tạm cho mỗi lần kết nối:** mỗi kết nối dùng một cặp khóa ECDH mới, nên lộ `K_pair` cũng không giải mã được các phiên cũ.
- **Sau bước này:** mọi bản ghi đều là SEALED (`type = 0x02`).

### 3.5 PC kết nối sẵn ở chế độ nền

Để chuyển nguồn tức thì, PC giữ sẵn kết nối tới các điện thoại (không phát hình):
- **Điện thoại nào:** điện thoại Wi‑Fi mà PC đã có khóa ghép đôi (tra theo TXT `id`, bỏ đuôi `:screen`), và mọi điện thoại cắm cáp. Không bao giờ ghép đôi ở chế độ này.
- **Cách kết nối:** `hello` có `"auto": true`, rồi `getCaps` một lần. PC lưu `caps` để khi người dùng chọn điện thoại thì có ngay danh sách chế độ.
- **Kết nối rảnh:** PC đọc những gì điện thoại gửi (ví dụ `caps` mới) và gửi `ping` mỗi 15 giây (mục 4.1).
- **Khi thất bại:** nhận `busy` hoặc lỗi thì 30 giây sau mới thử lại. Kết nối rảnh bị đóng thì chờ 30 giây, mỗi lần tiếp theo gấp đôi (tối đa 8 phút): app điện thoại cũ (trước `auto`) nhận mọi kết nối mới, nên hai PC sẽ giành nhau liên tục nếu thử lại ngay. Khi điện thoại xuất hiện lại (mDNS quảng bá lại, cắm cáp, cho phép gỡ lỗi USB) thì thử lại ngay.
- **Một điện thoại, hai đường:** cùng một máy chủ (cùng `deviceId` và cùng cổng camera/màn hình) qua Wi‑Fi và qua cáp chỉ được giữ một kết nối, ưu tiên cáp. Kết nối đang dùng (đang phát, cửa sổ màn hình) không bao giờ bị lấy.
- **Khi phát:** chính kết nối đó được dùng để phát (`start`), không phải bắt tay lại. Nếu kết nối rảnh đã chết mà PC chưa biết (không có gì trả lời 4 giây sau `start`), PC mở kết nối mới một lần.
- **TCP keepalive:** PC bật keepalive (10 giây im lặng, rồi mỗi 2 giây) trên kết nối Wi‑Fi, để biết điện thoại đã rời mạng.

---

## 4. Thông điệp điều khiển (CONTROL, JSON)

Mọi thông điệp đều có trường `"type"`. Bên nhận **bỏ qua** những trường không hiểu, nhờ vậy khi nâng phiên bản các bên cũ vẫn chạy được.

### 4.1 PC → điện thoại

| type | Trường | Ý nghĩa |
|---|---|---|
| `getCaps` | | Xin bảng khả năng camera (mục 5) |
| `start` | `lens`, `width`, `height`, `fps`, `codec` (`h264`, `hevc`, `hevc10`), `bitrateKbps`, `hdr` (bool), `stabilization` | Bắt đầu phát |
| `stop` | | Dừng phát; vẫn giữ kết nối |
| `set` | các trường điều khiển ở mục 4.3 | Đổi thông số khi đang phát |
| `keyframe` | | Xin một khung khóa (sau khi mất gói, hoặc khi bộ giải mã cần khởi tạo lại) |
| `stats` | `recvFps`, `decodeMs`, `queueMs`, `lost` | Gửi mỗi giây để điện thoại tự chỉnh bitrate |
| `showSystemEffects` | | Chỉ iOS: mở bảng hiệu ứng video của hệ thống (Xóa phông, Ánh sáng studio, Center Stage…) |
| `ping` | | Mỗi 15 giây trên kết nối rảnh (không phát hình). Điện thoại **không trả lời**. Điện thoại nào đã nhận một `ping` thì đóng phiên khi **45 giây** không nhận được bản ghi nào từ PC (bất kỳ bản ghi nào cũng tính). PC cũ không gửi `ping` nên không bị ảnh hưởng. Áp dụng cho cả cổng 47321 và 47322 |

### 4.2 Điện thoại → PC

| type | Trường | Ý nghĩa |
|---|---|---|
| `caps` | mục 5 | Trả lời `getCaps`; gửi lại khi khả năng thay đổi |
| `started` | `lens`, `width`, `height`, `fps`, `codec`, `hdr`, `orientation` | Đã bắt đầu phát với thông số **thực tế**. `orientation` = số độ **PC phải xoay ảnh theo chiều kim đồng hồ** để hình đứng thẳng (0/90/180/270). Điện thoại tính từ hướng cảm biến và hướng cầm máy, rồi gửi lại trong `state` mỗi khi người dùng xoay máy |
| `state` | trạng thái hiện tại của mọi trường ở mục 4.3, cộng `iso`, `exposureMs`, `lensPosition`, `wbKelvin`, `thermal` (`nominal`/`fair`/`serious`/`critical`), `battery` (0–100), `charging` | Gửi khi có thay đổi, tối đa 4 lần/giây |
| `stopped` | `reason` | Đã dừng: do PC, app bị ẩn, máy quá nóng… |
| `error` | `code`, `message` (tiếng Việt, để hiện cho người dùng) | Lỗi |
| `bitrate` | `kbps` | Bitrate hiện tại sau khi tự thích ứng |

### 4.3 Các trường điều khiển (`set` và `state`)

| Trường | Kiểu | Ghi chú |
|---|---|---|
| `lens` | string | id ống kính trong `caps.lenses` |
| `zoom` | number | Hệ số zoom của ống kính hoặc camera ảo hiện tại. Với camera ảo nhiều ống kính, máy tự chuyển ống kính theo `switchOver` |
| `focusMode` | `auto`, `continuous`, `manual` | |
| `focus` | 0..1 | Vị trí ống kính khi `manual`: 0 = gần nhất |
| `focusPoint` / `exposurePoint` | `[x, y]` từ 0 đến 1 | Chạm để lấy nét / đo sáng |
| `exposureMode` | `auto`, `manual` | |
| `iso`, `exposureMs` | number | Khi `manual`, giới hạn theo `caps` |
| `exposureBias` | number (EV) | |
| `wbMode` | `auto`, `manual` | |
| `wbKelvin`, `wbTint` | number | |
| `torch` | 0..1 | 0 = tắt |
| `stabilization` | string | Một giá trị có trong `caps` |
| `hdr` | bool | |
| `cinematic` | bool | Chỉ khi `caps.effects.cinematic == "app"` |
| `lowLightBoost` | bool | |

---

## 5. Bảng khả năng (`caps`)

Điện thoại liệt kê **mọi thứ phần cứng hỗ trợ**. PC dựng nút điều khiển từ đúng bảng này, không dựa vào điều gì khác.

```jsonc
{
  "type": "caps",
  "device": { "name": "iPhone của Lan", "model": "iPhone16,1", "os": "ios", "osVersion": "18.2" },
  "lenses": [
    {
      "id": "back-triple",              // id ổn định do app đặt
      "facing": "back",                 // back | front | external | screen
      "kind": "virtual",                // wide | ultraWide | telephoto | trueDepth | virtual | external | screen
      "label": "Camera sau (tự đổi ống kính)",
      "members": ["back-ultra", "back-wide", "back-tele"],   // chỉ có ở camera ảo
      "zoom": { "min": 1.0, "max": 125.0, "switchOver": [2.0, 6.0], "displayMultiplier": 0.5 },
      "focalLength35mm": 24,
      "fovDegrees": 77.0,
      "sensorOrientation": 90,
      "cinematic": true,                // ống kính này quay điện ảnh được (xem dưới)
      "controls": { /* như mục "controls" bên dưới, nhưng riêng cho ống kính này */ }
    },
    { "id": "back-wide", "facing": "back", "kind": "wide", "label": "Góc rộng 1×", "zoom": { "min": 1, "max": 10 } },
    { "id": "back-ultra", "facing": "back", "kind": "ultraWide", "label": "Siêu rộng 0.5×" },
    { "id": "back-tele", "facing": "back", "kind": "telephoto", "label": "Tele 3×" },
    { "id": "front", "facing": "front", "kind": "trueDepth", "label": "Camera trước" }
  ],
  "modes": [
    // một dòng cho mỗi (ống kính, kích thước); fps là các mức có thể chọn
    { "lens": "back-triple", "width": 3840, "height": 2160, "fps": [24, 25, 30, 60],
      "codecs": ["h264", "hevc", "hevc10"], "hdr": true, "binned": false }
  ],
  "controls": {
    "focus": { "modes": ["continuous", "auto", "manual"], "point": true },
    "exposure": { "modes": ["auto", "manual"], "point": true,
                  "isoMin": 32, "isoMax": 3072, "msMin": 0.014, "msMax": 1000, "biasMin": -8, "biasMax": 8 },
    "whiteBalance": { "modes": ["auto", "manual"], "kelvinMin": 2000, "kelvinMax": 10000 },
    "torch": true,
    "stabilization": ["off", "standard", "cinematic", "cinematicExtended"],
    "lowLightBoost": false
  },
  "effects": {
    // "app"    = app tự bật/tắt được (dùng `set`)
    // "system" = do hệ điều hành điều khiển; người dùng bật ở bảng hệ thống (`showSystemEffects`)
    // "none"   = máy không hỗ trợ
    "cinematic": "app",
    "portrait": "system",
    "studioLight": "system",
    "centerStage": "system",
    "backgroundReplacement": "system",
    "bokehExtension": "none"            // Android: Camera2/CameraX extension
  },
  "encoder": { "maxWidth": 3840, "maxHeight": 2160, "maxBitrateKbps": 100000 }
}
```

**`controls` theo từng ống kính:** mỗi phần tử trong `lenses` có thể có `controls` riêng, vì mỗi ống kính có giới hạn ISO, phơi sáng, lấy nét và chống rung khác nhau. Nếu có thì `lenses[i].controls` được ưu tiên hơn `controls` chung ở cấp trên.

**`cinematic` theo từng ống kính:** khi `effects.cinematic == "app"`, chỉ những ống kính không ghi `"cinematic": false` mới quay điện ảnh được. Không có trường này nghĩa là được.
- **Trên PC:** chỉ hiện nút điện ảnh cho những ống kính đó.
- **Khi không bật được** (ví dụ chế độ đang phát không có định dạng điện ảnh): điện thoại để `cinematic: false` trong `state`, **không** gửi `error`, vì PC dừng phát khi nhận `error`.

**Quy tắc ghi `modes`:**
- **Lọc theo bộ mã hóa:** chỉ ghi các kích thước mà camera xuất được **và** bộ mã hóa video phần cứng nhận được.
- **Lọc theo tốc độ:** chỉ ghi các fps mà cả hai bên đều đạt được.
- **Ảnh tĩnh không tính:** độ phân giải cảm biến chụp ảnh tĩnh (ví dụ 48 MP) không phải là chế độ video, nên không ghi vào đây.

---

## 6. Thích ứng với mạng

- **Theo dõi hàng đợi:** điện thoại theo dõi hàng đợi gửi của mình.
- **Mạng nghẽn:** khi hàng đợi vượt khoảng 150 ms, điện thoại:
  1. bỏ các khung không phải khung khóa đang chờ gửi;
  2. giảm bitrate 30%, mức sàn là 1 Mbps;
  3. gửi `bitrate` để PC biết.
- **Mạng ổn định trở lại:** sau 5 giây ổn định, tăng bitrate 10% mỗi lần, không vượt quá mức PC đã yêu cầu.
- **PC mất gói hoặc giải mã lỗi:** PC gửi `keyframe`, rồi bỏ các khung cho tới khi nhận được khung khóa tiếp theo.
- **Hình luôn mới:** PC chỉ giữ khung mới nhất (không xếp hàng), nên khi mạng yếu hình sẽ mờ đi chứ không bị trễ dồn.

---

## 7. Mã lỗi (`error.code`)

| code | Khi nào |
|---|---|
| `permission` | Chưa có quyền Camera, hoặc quyền Mạng cục bộ trên iOS |
| `background` | App bị ẩn: hệ điều hành chặn dùng camera khi app chạy nền, hoặc PC xin ghép đôi khi app không hiện trên màn hình (chỉ hỏi ghép đôi khi app đang hiện) |
| `busy` | Khi đang phát: app khác đang dùng camera. Trong bắt tay (thay cho `hello`): điện thoại đang kết nối với PC khác và `hello` này có `auto` (mục 3.1) |
| `unsupported` | Thông số yêu cầu không có trong `caps` |
| `thermal` | Máy quá nóng nên đã giảm chất lượng hoặc dừng |
| `encoder` | Bộ mã hóa phần cứng bị lỗi |
| `notPaired` | Nơi nhận kết nối không tự ghép đôi được (tiện ích chia sẻ màn hình của iPhone): ghép đôi trong app trước |

---

## Phụ lục A. Bộ dữ liệu kiểm thử

[protocol-vectors.json](protocol-vectors.json) chứa đầu vào cố định (khóa riêng, clientId, deviceId) và **mọi giá trị trung gian** của mục 3:
- `pcPublic`, `phonePublic`, `commit`
- `Z`, `T`, `SAS`
- `K_pair`, `macPc`, `macPhone`
- `K_up`, `K_down`
- một bản ghi SEALED hoàn chỉnh: PC → ĐT, counter 0, `{"type":"getCaps"}`

**Cách tạo:** sinh bằng [tools/protocol_vectors.py](../tools/protocol_vectors.py), viết thuần Python không dùng thư viện mật mã nào. Kết quả được đối chiếu với ECDH và AES‑GCM của JDK bằng [tools/VerifyVectors.java](../tools/VerifyVectors.java).

**Bắt buộc:** mỗi bên cài đặt phải có unit test tái tạo **đúng từng byte** các giá trị này:
- C++ (BCrypt)
- Kotlin (javax.crypto)
- Swift (CryptoKit)

Lưu ý `deviceId` trong bộ thử viết chữ hoa, đúng như Swift sinh ra, để kiểm tra quy tắc "giữ nguyên byte".
