# Handoff: unfinished work

> **Tóm tắt:** việc ngày 2026-10-18 đã gộp hết vào `main`. Phần lớn **chưa biên dịch trên Windows**
> và **chưa thử trên máy thật**. Việc tiếp theo ở mục 3.

For AI agents picking the work up on any machine. Read CLAUDE.md first.

When you finish a step, update this file in the same commit. Delete it once everything is built
and tested.

## 1. What is not verified yet

All of it is on `main`. Details are in CLAUDE.md and the commit messages.
The uncompiled PC code was reviewed by reading (engine and app); the bugs found are fixed in
8357bd0 and 3a25aa9. No compile errors were spotted, but nothing replaces a Windows build.

| Piece | Built | Tested on a device |
|---|---|---|
| USB transport, adb and usbmux (`src/engine/UsbTransport.*`) | the portable part with clang on the Mac; engine side no | iPhone over the cable from the Mac: yes. Android/adb: never. Windows Apple service: never |
| adb in the installer, started and stopped by the engine | no | no |
| Webcam found again after unplug/replug | no | no |
| Phone servers replace a session only after `hello` (probe-safe) | iOS yes, Android no | iPhone yes |
| Screen over USB ("… · Màn hình (USB)") | PC no | partly (`usb-check`) |
| iPhone camera in Picture in Picture after leaving the app (`BackgroundCamera.swift`, voip mode) | yes | **no** |
| iPhone and Android UI split into Camera / Màn hình | iOS yes, Android no | no |
| Android `CameraService` (camera keeps running in the background) | **no** | no |
| auto/busy/ping (both phones, PC) | iOS yes, others no | no |
| PC size rule, fit with bars, BGRA⇄NV12 (`tools/frame_check.cpp`) | yes, 307 checks on the Mac | — |
| PC connector (auto-connect), seamless switch, size "Tự động" | no | no |
| PC phone screen in its own window | no | no |
| PC app window as the camera source | no | no |
| PC virtual camera always on while the app runs, automatic source pick | no | no |

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

## 3. Next steps

1. **Windows:**
   - Build with `scripts\build.ps1` and fix what does not compile.
   - Run `camstudio-protocol-test`, `camstudio-frame-check`, `camstudio-usb-check list`.
   - Then the test list in CLAUDE.md, section "PC work of 2026-10-18 to build and test on Windows".
   - Also test USB (a Redmi via adb, the iPhone via Apple Devices/iTunes) and unplugging the OBSBOT.
2. **Android:**
   - Build with `scripts\build-android.ps1` on Windows, or install JDK 17 and Android SDK on the
     Mac (ask first).
   - On the Redmi: the camera keeps streaming with the app hidden; two PCs give "busy".
3. **iPhone:**
   - `scripts/ios-run.sh`. **Ask first:** it replaces the TestFlight install (same bundle id).
   - While `fake-pc --usb` streams, leaving the app must show the PiP window with frames still
     coming.
   - If it does not work, remove the voip mode.
4. **Docs:** update CLAUDE.md "Status" with what was verified, then shrink or delete this file.

## 4. Pitfalls

- The PC side builds only on Windows. On the Mac only the portable C++ compiles
  (`UsbTransport.cpp`, `tools/frame_check.cpp`). The Mac has no JDK or Android SDK.
- iOS signing:
  - `ios/Signing.local.xcconfig` is git-ignored: team `2TLZSD8YW7` (the user's own),
    bundle id `vn.camstudio.phonewl`.
  - Never sign a new bundle id with team `8VVXM8DN6B` (the employer).
- One PC per phone. A test from the Mac drops the user's Windows PC. Ask first.
- The iPhone is the user's own phone and they may be using it. Ask before launching apps on it or
  installing a build.
- `docs/camo-gap-analysis.md` may exist untracked, from another session. Keep the clean-room rule
  in mind before using it.
