# Handoff: unfinished work

> **Tóm tắt (2026-10-30):** bản 0.1.11 đã đăng trên GitHub (PC + Android). iOS 0.1.0
> (20261021.2339) đã lên TestFlight. Còn một lỗi micro phía PC (mục 3) và các bài thử cần người
> cầm máy. Bản PC nhẹ và mượt hơn (2026-10-30) đã commit, chưa phát hành.

For AI agents picking the work up on any machine. Read CLAUDE.md first ("Windows build and the
Redmi over USB" says what was verified). Update this file in the same commit as each step; delete
it once everything is tested.

## 1. What is not verified yet

| Piece | Built | Tested on a device |
|---|---|---|
| **Windows 10 DirectShow filter** (x64+x86), engine run-time API pick | yes | **published in v0.1.6**; Win11 forced path verified; **no real Win10 PC** yet |
| **iPhone re-pair on key mismatch** (PC `pair` only on clean close) | yes | in v0.1.6; iPhone streamed 2K/60 with the saved key (no mismatch hit); iOS `Store` compiled on the Mac 2026-10-21 (app + screen extension, CryptoTests pass); the re-pair itself not yet hit on a device |
| USB: Redmi over adb, engine starts/stops adb | yes | yes |
| USB: iPhone through the Apple service on Windows | yes | **no** |
| Webcam found again after unplug/replug | yes | **no** |
| Probe-safe phone servers | yes | iPhone yes, Redmi yes |
| Screen over USB ("… · Màn hình (USB)") | yes | **no** (the Redmi needs PIN + consent) |
| PC phone screen in its own window, "Dùng làm camera ảo" | yes | **no** |
| Android `CameraService` (camera runs in the background) | yes | yes (screen off, locked) |
| Android UI split Camera / Màn hình | yes | camera part only |
| auto/busy/ping | yes | auto yes; busy (two PCs) **no** |
| PC connector, seamless switch, size "Tự động" | yes | yes (with `camstudio-verify`, not Discord) |
| PC window as the camera source | yes | yes (test window) |
| **"Tự căn khung" + Smart Zoom** (`AutoFrame.h`, `FaceDetect.cpp`, switch in the Camera tab), 2026-10-19 | **compiled on Windows 2026-10-19** (engine + app, 0 warnings) | `framing-check` 37/0, `frame-check` 375/0, protocol ALL PASS; **OBSBOT: follows a face** (1.1× at 1080p, "nét"), Redmi with nobody in view: "Không thấy ai". Fix needed first: FaceTracker wants `RelativeTime` on each frame, else E_FAIL every frame |
| PC camera always on, automatic source pick | yes | yes |
| PC UI 2026-10-19: device cards, camera bar under the preview, colour presets, (i) help, **Khởi động lại** | yes | dev build: dark + light theme, 960 px window, presets, restart (visible, hidden `--tray`, and during a Discord call: Discord then needs its camera off/on once, picture back at once) |
| PC design rules (spacing, radii, one status palette, screen-reader names), 2026-10-20 | yes | dev build: dark + light theme, menus, Settings |
| PC icons: bundled Fluent UI System Icons font (`tools/make_icon_font.py`) | yes | dev build on Win11; **not seen on Windows 10** |
| Phone mic → PC → VB-Audio "CABLE Input" (Android `MicCapture`, PC `AudioOut`, protocol 5.1) | yes | fake phone tone → WAV: 440 Hz, 48 kHz, no gaps, live off; Redmi over USB: sound with the screen off and locked, denied → camera fine and the reason shown; **not yet: the PC-triggered permission prompt, the WASAPI path into CABLE (VB-Cable not installed), a hidden window in a real call, iOS (not written)** |
| PC "Nền": blur / image behind the person (Windows ML, PP-HumanSeg, in Assets/Models) | yes | OBSBOT, empty room: blurred whole, image mode replaces it; 1–2 ms a look; 3.1 → 5.4 ms a frame at 1080p100. Installed 0.1.8, the user in frame: sharp person and hair, room blurred. **Not yet: a phone camera, Windows 10** |
| PC "Làm mịn da" (faces, guided filter) | yes | `frame-check` 379/0 (4 new); installed 0.1.8 at 70 on the user's face: smoother skin, glasses and eyes sharp, no ellipse edge, "1 khuôn mặt" |
| PC: a hot phone drops to 1080p30 / 720p30 (`state.thermal`) | yes | fake phone `--thermal serious`: 720p60 → 720p30, warning; "Quên ghép đôi" on the fake phone: key gone, camera moved to the webcam; **not on a real hot phone** |
| PC snapshot (PNG) and recording (MP4 H.264 + mic AAC) of the camera's picture | yes | OBSBOT: PNG 1920×1080, MP4 4.4 s and 17.7 s with the window hidden (~50 fps, sound track); **not tried: a phone source, paused while recording, no microphone, Windows 10** |
| PC phone-app QR ("+" › Tải app cho điện thoại…, empty preview) | yes | 16/16 codes (versions 1–10, Vietnamese text) read back by zxing-cpp; **not scanned with a phone camera** |
| PC phone-app QR: two codes (Android release page, iPhone TestFlight `join/h3NKXEjD`), 2026-10-28 | yes (Windows, 2026-10-29) | dialog: both codes decoded by zxing-cpp from a screenshot, the TestFlight page answers "Join the Cam Studio beta"; **the empty-preview row (no source) not seen** |
| PC pause picture (blurred last picture + text, card, own image, no signal) | yes | blurred card seen through `camstudio-verify --dump` (29 fps, 0 no-signal), resume live; **card / own image / Windows 10 path not looked at** |
| PC global shortcuts (P pause, N next, C snapshot, R record, B effects, with Ctrl+Alt) | yes | hidden: with `camstudio-verify --hold` on the camera (no frame lost) and in standby (snapshot woke the webcam); a taken combination (Zalo's Ctrl+Alt+S) shows the warning. **Not in a real Discord call** |
| PC source list: working sources first, "Chưa kết nối" below; right-click "Quên ghép đôi…" / "Xóa khỏi danh sách"; add by IP without a port | yes | order, menu, add + remove a test IP; **"Quên ghép đôi" not run** (needs re-pairing a real phone) |
| iPhone PiP after leaving the app (`BackgroundCamera.swift`, voip) | yes | **no** |
| Installer with adb | yes | yes (installed since 0.1.4) |
| Discord over Wi‑Fi (Redmi), screen off | yes | yes |
| Automatic updates, PC (`Updater.cs`) | yes | yes (0.1.4 → test 0.1.5); "Không" at UAC **no** |
| Automatic updates, Android (`AppUpdater.kt`) | yes | 0.1.4 installed; update path **no** (next release) |
| PC lighter and smoother (2026-10-30, CLAUDE.md "Light and smooth"): even 30 fps preview, SSE2 colour, effects capped to the camera's rate, tray trims | yes (Windows, dev build) | OBSBOT: preview 30 fps, blur + reader 140 % → 28 % CPU, tray (Task Manager) 38 → 24 MB; `camstudio-verify` live, 0 no-signal. **Not yet: in a release, a phone source, Windows 10, the preview looked at by a person** |
| PC Settings: "by Win Lã" link under the version (`Assets/WinLa.gif`, `MakerLink_Click`), 2026-10-24 | yes (Windows, 2026-10-29) | dev build: logo shows, the GIF has no loop extension (28 frames once), the link opens winlax.id.vn; **with Windows animations off: not looked at** |

## 2. The user's decisions (do not ask again)

- Bundle adb in the Windows installer.
- iPhone in the background: Picture in Picture plus the `voip` background mode. The App Review risk
  is accepted.
- Several sources: stay connected, switch seamlessly (not all streaming at once).
- Virtual camera size "Tự động, ưu tiên nét nhất":
  - no app on it: the source's native size;
  - during a call: keep the size, fit with black bars;
  - after the call: native again.
- Both: the phone screen in its own PC window, and a PC app window as the camera source.
- The virtual camera exists for as long as the app runs ("no signal" without a source).
- iPhone screen sharing stays on ReplayKit broadcast. No AirPlay receiver (clean-room rule).
- "Lật ngang" applies to every source, a PC window included (it mirrors its text): keep it so.
- Git: only `main` (see CLAUDE.md, Ground rules).

## 3. Next steps (need someone at the phone or PC)

- **Phone mic, still open (code review 2026-10-21, on the Mac):** `RunPhone` reads `audio` only
  from later messages, not from the first `started` (`CaptureEngine.cpp` ~1575: add
  `audioFrom(v)` there). So a mic denied at start shows on the PC only after a `set` (the mic
  button toggled), and a phone recording at 44.1/16 kHz would play at 48 kHz. Also: "Tạm dừng
  hình" (`cs_switch_none`) stops the phone's sound too, in a call.

- **v0.1.11 published** (2026-10-29, tag on e2a4572, setup sha256 e3fa551e…, APK 12757a4a…,
  versionCode 13, no Android code change): two phone-app QR codes, "by Win Lã" in Settings.
  Installed on the user's PC with `/LOG`: succeeded, no rollback, installed files match the
  build, settings untouched (the size was already "Tự động"), virtual camera 181/181 live. No
  phone tested (Redmi not plugged in, iPhone app closed). The phone-mic bug above is still in it.
- **v0.1.10 published** (2026-10-21, tag on 91dae8f, setup sha256 10de9da0…, APK baf1f5ec…): Thành phần bổ sung (Apple Devices, VB-CABLE), Windows accent +
  Settings › Màu nhấn, bigger centred icons, one card per phone, crash log. Tested on the user's
  PC: winget install of Apple Devices, iPhone over USB (~130 ms vs ~185 ms Wi‑Fi), VB-CABLE
  install through the app, live accent change. Not clicked through: the in-app Apple Devices
  flow end to end (the same steps were run by hand). **iPhone "Tắt màn hình"** (black cover,
  auto after 60 s) is in TestFlight build 0.1.0 (20261021.2339), uploaded from the Mac; untested
  on the device (PiP with the cover above it).
- **v0.1.9 published** (2026-10-21, tag on 07218c5): `CamStudio-Setup-0.1.9.exe` (sha256
  d9b48899…) and `CamStudio-0.1.9-android.apk` (versionCode 11, sha256 1fa84ed7…, the same APK
  tested on the Redmi). Redmi over USB: pairing survived the upgrade; mic denied → camera fine,
  reason shown; mic allowed → sound, also screen off and locked. Left: with the mic denied, the
  PC's request popping the prompt and the sound starting once allowed; VB-Cable (CABLE Input →
  CABLE Output); Android 14+ service rules; the installed 0.1.8 updating itself to 0.1.9.
  The Redmi's "Chạy nền" was switched on by a stray adb tap during the test.
- **v0.1.8 published** (2026-10-20, tag on d7ac5cd): both digests match; the installed 0.1.7
  saw it within seconds and updated, but the install rolled back half-done (adb.exe locked by a
  test loop of ours, see CLAUDE.md Gotchas); reinstalled with `/LOG`: succeeded, 0.1.8 running.
  The installer now skips an unchanged adb (next release). The Redmi should offer 0.1.8 too.
- **v0.1.7 published** (2026-10-20, tag on 9bdd907): installed on the user's PC; GitHub
  `releases/latest` is v0.1.7, both digests match. Installed 0.1.6 PCs and the Redmi update
  themselves from it (not yet watched on a device: the Redmi's update path is still untested).

- **"Tự căn khung" on Windows:** built and seen working 2026-10-19 (OBSBOT, dev build run from `app/bin`). Still to try: the
  OBSBOT and a phone at 4K: it follows a face, goes wide after ~4 s with nobody, the hint shows
  the crop size. Measure "Chi tiết › Cam Studio dùng" (the whole app; the tracker has its own
  thread) and `processMs` with it off and on, at 4K and 1080p (framed means a crop-scale every
  frame). With it on, the window hidden and no app on the camera: no detector work. Check: screens
  and windows are never framed, tap-to-focus lands right while framed, a Discord call.

0. **v0.1.5 is published** (2026-10-18, the new PC layout): the user's PC updated itself 0.1.4 ->
   0.1.5 (UAC, relaunch, not elevated). The Redmi still runs 0.1.4: opening the app there should show
   "Có bản mới 0.1.5". Check the confirmations and that the app restarts as 0.1.5 (needs the PIN).
1. **Redmi, unlocked:** "Chia sẻ màn hình" → on the PC: "Redmi 9T · Màn hình (USB)", the screen
   window, "Dùng làm camera ảo"; stop/start sharing while an app holds the camera.
2. **Unplug tests** while Discord (or `camstudio-verify --hold`) holds the camera: the phone cable,
   the OBSBOT (replug into another port).
3. **Discord call:** turn the phone, switch sources, pause. **Seen 2026-10-19:** the phone turned
   portrait → landscape in a call keeps the portrait camera (fitted, `PlanOutput` with
   `consumerHolds`), so Discord shows a letterboxed/cropped landscape picture. A new size needs a
   rebuild, which Discord does not survive. **The user chose a fixed 1080p** (Settings, like Camo).
   Verified 2026-10-19 (Redmi, dev build): with no app on it, a portrait picture keeps the camera
   at 1920×1080 (no rebuild); in Discord portrait shows with side bars, landscape fills the tile,
   the camera never drops. The size had been found back at "Tự động" once (cause unknown). A new size applies
   only once Discord has let go for 5 s (`kConsumerGraceMs`): toggling the camera off and on
   quickly keeps the old size, which is what confused the user first.
4. **Two PCs** with one phone: the second shows "Bận với PC khác".
5. **iPhone:** Apple Devices on Windows. PiP and "Tắt màn hình": install TestFlight build 20261021.2339
   or `scripts/ios-run.sh` on the Mac (ask first: it replaces the TestFlight install).

## 4. Pitfalls

- iOS signing: `ios/Signing.local.xcconfig` is git-ignored (team `2TLZSD8YW7`, bundle id
  `vn.camstudio.phonewl`). Never sign a new bundle id with team `8VVXM8DN6B` (the employer).
- One PC per phone: a test from the Mac drops the user's Windows PC. Ask first.
- The iPhone is the user's own phone. Ask before launching apps on it or installing a build.
- The Redmi locks with a PIN after a while: never enter it, never change its screen settings.
- Don't run `usb-check hello` while the PC streams: a valid hello replaces the PC's session.
- `docs/camo-gap-analysis.md`: keep the clean-room rule in mind before using it.
