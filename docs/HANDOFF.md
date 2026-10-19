# Handoff: unfinished work

> **Tóm tắt:** ngày 2026-10-18 phần PC đã build và chạy trên Windows, Redmi qua cáp và Wi‑Fi (cả
> Discord) chạy tốt, có tự động cập nhật; bản 0.1.5 (giao diện mới) đã đăng trên GitHub. iOS
> 0.1.0 (20261018.1756) đã lên TestFlight. Còn lại: các bài thử cần người cầm máy (mục 3).

For AI agents picking the work up on any machine. Read CLAUDE.md first ("Windows build and the
Redmi over USB" says what was verified). Update this file in the same commit as each step; delete
it once everything is tested.

## 1. What is not verified yet

| Piece | Built | Tested on a device |
|---|---|---|
| **Windows 10 DirectShow filter** (x64+x86), engine run-time API pick | yes | **published in v0.1.6**; Win11 forced path verified; **no real Win10 PC** yet |
| **iPhone re-pair on key mismatch** (PC `pair` only on clean close) | yes | in v0.1.6; iPhone streamed 2K/60 with the saved key (no mismatch hit); iOS `Store` **not compiled** (needs Mac) |
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
| iPhone PiP after leaving the app (`BackgroundCamera.swift`, voip) | yes | **no** |
| Installer with adb | yes | built, **not installed** |
| Discord over Wi‑Fi (Redmi), screen off | yes | yes |
| Automatic updates, PC (`Updater.cs`) | yes | yes (0.1.4 → test 0.1.5); "Không" at UAC **no** |
| Automatic updates, Android (`AppUpdater.kt`) | yes | 0.1.4 installed; update path **no** (next release) |

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
5. **iPhone:** Apple Devices on Windows. PiP: install TestFlight build 20261018.1756 (it has PiP)
   or `scripts/ios-run.sh` on the Mac (ask first: it replaces the TestFlight install).

## 4. Pitfalls

- iOS signing: `ios/Signing.local.xcconfig` is git-ignored (team `2TLZSD8YW7`, bundle id
  `vn.camstudio.phonewl`). Never sign a new bundle id with team `8VVXM8DN6B` (the employer).
- One PC per phone: a test from the Mac drops the user's Windows PC. Ask first.
- The iPhone is the user's own phone. Ask before launching apps on it or installing a build.
- The Redmi locks with a PIN after a while: never enter it, never change its screen settings.
- Don't run `usb-check hello` while the PC streams: a valid hello replaces the PC's session.
- `docs/camo-gap-analysis.md`: keep the clean-room rule in mind before using it.
