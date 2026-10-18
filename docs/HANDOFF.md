# Handoff: where the work stands

> **Tóm tắt (tiếng Việt):** ngày 2026-10-18 nhiều phiên AI làm song song trên Mac. Toàn bộ đã gộp
> vào nhánh **`integration/2026-10-18`** trên GitHub (chưa gộp vào `main`). Phần lớn mã **chưa biên
> dịch trên Windows** và **chưa thử trên máy thật**. File này ghi phần nào từ đâu, đã kiểm tra tới
> đâu, anh/chị đã quyết định những gì, và việc tiếp theo theo thứ tự. Đọc CLAUDE.md trước, rồi đọc
> file này.

Written 2026-10-18 14:40 (+07) on the user's Mac, updated 14:50 after the merge. For AI agents and
people picking the work up on any machine.

**How to use this file:**
- CLAUDE.md explains how the project works and what was learned the hard way.
- This file says where unfinished work lives and what to do next.
- When you finish a step below, update this file in the same commit: the table, the next steps, the date.
- Delete this file once everything is merged, built on Windows and tested on the devices.

**Start from `integration/2026-10-18`.** It holds everything below and is on GitHub
(`git fetch && git checkout integration/2026-10-18`).
- `main` on GitHub is still `ef2173d`.
- The feature branches were not pushed. They live only in the git repository on the user's Mac
  (`/Users/win_lax/aCode/cam-studio`).

## 1. Branch map

```
main  ef2173d  "Add Xcode workspace and schemes"            (GitHub main as of 2026-10-18 morning)
 └─ 8708494  Base: USB transport (copy of the main checkout's uncommitted USB work)
     └─ bd470d5 · 8f8cfa5 · a8ec345   claude/vigorous-einstein-c615b3   screen over USB, probe-safe phone servers
         ├─ cd72d5b                    claude/ios-background             iPhone: camera in PiP, Camera/Screen parts, auto/busy/ping
         ├─ 01113e2 · 8fd6f4a          claude/cool-mcnulty-4e2cb6        Android: CameraService, Camera/Screen parts, auto/busy/ping
         └─ a8c72c1 … f0f236a          claude/nifty-hawking-2631de       PC: many sources, size rule, screen window, window source, camera always on
 └─ e20769e                            claude/testflight-tweaks          TestFlight archive/upload tweaks (another session's, committed as found)

integration/2026-10-18 = claude/nifty-hawking-2631de
                         + merge claude/ios-background       (8997cc6)
                         + merge claude/cool-mcnulty-4e2cb6  (23d0add)
                         + merge claude/testflight-tweaks    (916e49a; ios/Info.plist keeps both keys)
                         + this file
```

**Worktrees on the Mac:** each branch is checked out at `.claude/worktrees/<suffix>`. Do not
edit another session's worktree.

**The three feature branches do not overlap:**
- `ios-background` touches only `ios/`;
- `cool-mcnulty` touches only `android/`;
- `nifty-hawking` touches `src/`, `app/`, `tools/`, `CMakeLists.txt`, `README.md`,
  `docs/protocol.md` and `CLAUDE.md`.

Merging them together should therefore be conflict-free. Each branch's own `CLAUDE.md` and commit
messages hold the details. The PC session documented its part in CLAUDE.md on its branch, in the
sections "PC: many sources…" and "PC work of 2026-10-18 to build and test on Windows".

**The main checkout on the Mac** (`/Users/win_lax/aCode/cam-studio`) is now on
`integration/2026-10-18`. Before the merge it held two kinds of uncommitted work:
- **The USB transport.** It was identical to commit `8708494`, so it was dropped.
- **The TestFlight session's four files.** They are now commit `e20769e`.

Still untracked there: `docs/camo-gap-analysis.md`, from another session ("Camo Studio reverse
engineering analysis"). It is not part of any branch. Before you use anything in it, keep the
clean-room rule in CLAUDE.md in mind.

## 2. What each piece does and how far it got

"Built" means it compiled somewhere. "Tested" means it ran against a real device.

| Piece | Branch | Built | Tested |
|---|---|---|---|
| USB transport: adb (Android) and usbmux (iPhone) tunnels to the phone app, background device watchers, iPhone name from lockdownd (`src/engine/UsbTransport.*`, `tools/usb_check.cpp`, `fake-pc --usb`) | `8708494` | `UsbTransport.cpp` with clang on the Mac. The PC engine side (`Discovery`, `PhoneLink::Dial`/`Reconnect`, names "… (USB)") **no** | iPhone 14 over the cable from the Mac: `usb-check`, `fake-pc --usb` (four lenses 1080p30, 4K30 HEVC 25 Mbps, all frames decoded), unplug/replug. **Android over adb: never. Windows Apple Mobile Device Service: never** |
| adb bundled in the installer, started only with an Android phone plugged in, stopped after 1 min idle or on exit | `8708494` | no | no |
| Webcam found again after unplug/replug (`FindCameraAgain`) | `8708494` | no | no |
| Phone servers replace a session only after the new connection's `hello` (probe-safe) | `bd470d5` | iOS yes; Android no | iPhone: 40 s stream undisturbed by probes |
| Screen over USB: PC lists "… · Màn hình (USB)" on port 47322 | `bd470d5` | PC no | `usb-check` on the iPhone; the screen extension over USB not run |
| iPhone camera keeps streaming in a PiP window after leaving the app: voip background mode, multitasking camera access, `BackgroundCamera.swift`; interruptions reported to the PC | `cd72d5b` | yes (unsigned `xcodebuild`) | **not on the iPhone** (the user postponed) |
| iPhone UI split into Camera and Màn hình parts | `cd72d5b` | yes | no |
| Phone side of auto/busy/ping (iOS) | `cd72d5b` | yes | no |
| Android `CameraService` (foreground service of type camera, wake and Wi‑Fi locks), Camera and Màn hình cards, auto/busy/ping | `01113e2`, `8fd6f4a` | **no** (no JDK/SDK on the Mac) | no |
| PC portable picture code: size rule `PlanOutput`, fit with black bars, BGRA ⇄ NV12 (`tools/frame_check.cpp`) | `a8c72c1` | yes, clang `-arch x86_64`, 307 checks, ASan/UBSan | — |
| PC background connector: auto-connect paired and cable phones, link leases, ping, stable source ids, cached caps | `65cef43` | no | no |
| PC seamless switching on one virtual camera, size rule "Tự động", size setting | `49553e4`, `1bcab06` | no | no |
| PC phone screen in its own window (`ScreenViewer`, `ScreenWindow`) | `2b577a6` | no | no |
| PC window of another app as the camera source (Windows.Graphics.Capture, `WindowSource`) | `1e290e3` | no | no |
| PC: virtual camera lives as long as the app, "no signal" without a source, automatic source pick by priority | `3af1289` | no | no |
| Docs: protocol 3.1/3.5/4.1 (auto, busy, ping), README, CLAUDE.md for the PC work | `f0f236a` | — | — |

**Protocol changes (docs/protocol.md on the PC branch is the reference):**
- **Probe-safe phones:** a phone replaces its session only after the new connection's `hello`.
- **hello `"auto": true`:** sent when the PC connects by itself. A phone held by another PC answers
  `error` `code:"busy"` and keeps its session. The same PC (same `clientId`) may replace its own
  dead session.
- **`ping`:** the PC sends it every 15 s on idle links. A phone that has seen a ping closes the
  session after 45 s of silence.
- **USB:** section 1.2 of protocol.md.

## 3. Decisions the user made (do not ask again)

All on 2026-10-18:
- **adb:** bundle it in the Windows installer (from the SDK's platform-tools, with `NOTICE.txt`).
- **Android over USB:** test on Windows, not on the Mac.
- **iPhone in the background:** Picture in Picture plus the `voip` background mode. The App Review
  question this may raise is accepted.
- **Several sources:** stay connected and switch seamlessly. Not "all streaming at once".
- **Virtual camera size "Tự động, ưu tiên nét nhất":**
  - no app on the camera: the source's native size;
  - an app using it: keep the size and fit the picture with black bars;
  - when the app lets go: back to native.
  - Fixed 4K/1080p/720p are options.
- **Window features:** do both. The phone's screen in its own PC window (to share that window in
  Discord/Zoom), and a PC app window as the virtual camera's source.
- **The virtual camera exists for as long as the app runs.** It shows "no signal" without a source
  and uses a source as soon as one is ready. Toggling it off only pauses the picture. (The user
  asked this through the Android session.)
- **iPhone screen sharing stays on ReplayKit broadcast.** An AirPlay receiver or QuickTime-style
  USB capture on Windows would need reverse-engineered Apple protocols, which goes against the
  clean-room rule. The broadcast stores nothing on the phone.
- **Language:** user-facing text in Vietnamese; CLAUDE.md and this file in English.

## 4. Next steps, in order

1. **Done on 2026-10-18:** everything merged into `integration/2026-10-18` and pushed.
   - At merge time the PC session was still having an agent review its uncompiled code. If
     `claude/nifty-hawking-2631de` (or any other branch above) gets new commits, merge them into
     `integration/2026-10-18` and push again.
   - Merge into `main` only once the Windows build works.
2. **Windows: build and fix.** Run `scripts\build.ps1`. Then `camstudio-protocol-test`,
   `camstudio-frame-check`, `camstudio-usb-check list`. Then the test list in CLAUDE.md, section
   "PC work of 2026-10-18 to build and test on Windows". Also:
   - USB: a Redmi via adb and the iPhone via Apple Devices/iTunes;
   - unplug while Discord uses the camera;
   - the OBSBOT unplugged and plugged into another port.
3. **Android:** build with `scripts\build-android.ps1` on Windows, or install JDK 17 and Android SDK 36
   on the Mac (ask first). On the Redmi, check:
   - the camera keeps streaming with the app hidden and the screen off;
   - "Chạy nền" off;
   - pairing only while the app is visible;
   - two PCs: the second gets "busy".
4. **iPhone:** `scripts/ios-run.sh`. This installs over the TestFlight build, because the bundle id
   `vn.camstudio.phonewl` is the same: **ask the user first**. While `fake-pc --usb` streams, check:
   - leaving the app shows the PiP window and frames keep coming;
   - a lens switch from the PC while in PiP;
   - "Tiếp tục phát khi rời app" off gives error `background` as before;
   - a second PC with `auto` gets `busy`.

   If the camera does not run in PiP, remove the voip mode and keep the old behaviour.
5. **Docs:**
   - Update CLAUDE.md "Status" and "Open work" with what was verified.
   - Update README (Vietnamese) for the new PC features if anything changed during the fixes.
   - Shrink or delete this file.

## 5. Pitfalls for agents on this repo

- **Several Claude sessions share this repository** (the main checkout plus worktrees).
  - Check `git worktree list` and `git status` before touching anything.
  - Never `git checkout .`, `git reset` or `git clean` in the main checkout without looking:
    it holds other sessions' uncommitted work.
  - Never use a bare `git stash`.
- **Only Windows builds the PC side** (engine, vcam, WinUI app). On a Mac, the portable C++
  (`UsbTransport.cpp`, `tools/frame_check.cpp`) compiles with clang. Mark anything else
  "not compiled".
- **This Mac has no JDK and no Android SDK.**
- **iOS signing:**
  - `ios/Signing.local.xcconfig` is git-ignored. It holds team `2TLZSD8YW7` (the user's own paid
    team) and bundle id `vn.camstudio.phonewl`. Copy it into a new worktree.
  - Never sign a new bundle id with team `8VVXM8DN6B` (the user's employer).
  - Bundle id `vn.camstudio.phone` belongs to another account.
- **One PC per phone.** Any test from the Mac drops the user's Windows PC. Older phone builds drop
  the session on any connection, even a probe. Ask before testing.
- **The iPhone is the user's own phone** (UDID `00008110-0012401C3A86401E`). They may be using it,
  for example playing a game. Ask before launching apps on it, and before installing a build.
- **Every PC change of 2026-10-18 is unverified.** When something misbehaves on Windows, suspect
  first that day's commits on `nifty-hawking` and `8708494`.
