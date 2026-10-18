# Handoff: unfinished work

> **Tóm tắt:** ngày 2026-10-18 phần PC đã build và chạy trên Windows, Redmi qua cáp và Wi‑Fi (cả
> Discord) chạy tốt, có tự động cập nhật (0.1.4). Còn lại: phát hành 0.1.4 và các bài thử cần
> người cầm máy (mục 3).

For AI agents picking the work up on any machine. Read CLAUDE.md first ("Windows build and the
Redmi over USB" says what was verified). Update this file in the same commit as each step; delete
it once everything is tested.

## 1. What is not verified yet

| Piece | Built | Tested on a device |
|---|---|---|
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
| PC camera always on, automatic source pick | yes | yes |
| iPhone PiP after leaving the app (`BackgroundCamera.swift`, voip) | yes | **no** |
| Installer with adb | yes | built, **not installed** |
| Discord over Wi‑Fi (Redmi), screen off | yes | yes |
| Automatic updates, PC (`Updater.cs`) | yes | fake feed only; real install **no** |
| Automatic updates, Android (`AppUpdater.kt`) | yes | installed 0.1.4; update path **no** |

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
- Git: only `main` (see CLAUDE.md, Ground rules).

## 3. Next steps (need someone at the phone or PC)

0. **Release 0.1.4** (`build\dist`, built 2026-10-18): ask the user before publishing it on GitHub.
   Then test the real update: install 0.1.4 (UAC), serve a 0.1.5 build from a fake feed
   (`CAMSTUDIO_UPDATE_FEED`, docs/updates.md), open the app: 15 s notice, UAC, silent setup, the app
   opens again not elevated; "Không" at UAC keeps the app running. On the phone: a 0.1.5 release
   shows "Có bản mới", installs after the confirmations.
1. **Redmi, unlocked:** "Chia sẻ màn hình" → on the PC: "Redmi 9T · Màn hình (USB)", the screen
   window, "Dùng làm camera ảo"; stop/start sharing while an app holds the camera.
2. **Unplug tests** while Discord (or `camstudio-verify --hold`) holds the camera: the phone cable,
   the OBSBOT (replug into another port).
3. **Discord call:** turn the phone, switch sources, pause.
4. **Two PCs** with one phone: the second shows "Bận với PC khác".
5. **iPhone:** Apple Devices on Windows; `scripts/ios-run.sh` on the Mac for PiP (ask first: it
   replaces the TestFlight install).
6. Ask the user: should "Lật ngang" apply to a window source (it mirrors the text)?

## 4. Pitfalls

- iOS signing: `ios/Signing.local.xcconfig` is git-ignored (team `2TLZSD8YW7`, bundle id
  `vn.camstudio.phonewl`). Never sign a new bundle id with team `8VVXM8DN6B` (the employer).
- One PC per phone: a test from the Mac drops the user's Windows PC. Ask first.
- The iPhone is the user's own phone. Ask before launching apps on it or installing a build.
- The Redmi locks with a PIN after a while: never enter it, never change its screen settings.
- Don't run `usb-check hello` while the PC streams: a valid hello replaces the PC's session.
- `docs/camo-gap-analysis.md`: keep the clean-room rule in mind before using it.
