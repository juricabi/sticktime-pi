# StickTime Pi: plan

## 1. Goal

A Raspberry Pi 4 that acts like a virtual quad. It runs the StickTime FPV simulator and sends the
picture over 5.8 GHz to ordinary analog FPV goggles (Pi composite out → VTX), or to an HDMI screen.
The pilot flies with their own radio: ELRS/Crossfire over WiFi, USB joystick, or Bluetooth.

```
radio ─ ELRS/XF WiFi joystick (UDP 11000) ─┐
radio ─ USB joystick (EdgeTX HID) ─────────┼─▶ Pi 4: sticktime-pi (C, static) runs StickTime.lua
ELRS BLE joystick / gamepad (Bluetooth) ───┘          │ DRM/KMS page flips
                                                       ├─ AV (3.5 mm jack) ─▶ 5.8 GHz VTX ─▶ analog goggles
                                                       └─ or HDMI ─▶ monitor / HDMI-in goggles
phone or PC browser ── http://sticktime.local ─▶ settings, WiFi, sticks, tracks, updates
```

## 2. Requirements (from the owner, 2026-10-16)

1. **A custom image to flash and try.** Lightweight: only what this needs.
2. **Video output options:** AV/composite to a VTX, or HDMI. NTSC/PAL, 4:3/16:9.
3. **WiFi network** setup.
4. **Inputs:** the owner's ELRS/Crossfire WiFi joystick protocol, plus USB and Bluetooth.
5. **Low latency**, end to end.
6. **Updatable to new versions** without reflashing.
7. **Tracks like on the web:** custom tracks, the track editor and the gallery.
8. **Web page with WiFi and joystick settings** (and the rest of the settings).
9. Private repo for now, open-sourceable later: keep it clean, documented, GPL-2.0.

## 3. Architecture

### 3.1 The host (`host/`)

One static C program, `sticktime-pi`. Modules:

| File | Job |
|---|---|
| `main.c` | options, settings, main loop, frame pacing, restart of the Lua state |
| `etx.c` | Lua state, EdgeTX globals and functions, `io` on a real folder (the "SD card"), `loadScript`, `STICKTIME_TEST` |
| `lcd.c` | RGB565 frame buffer and the drawing calls: a literal port of `engine.js` `colorLcd()` |
| `font.c` | Roboto through stb_truetype, glyph cache per (px, bold), the engine.js metrics |
| `video_drm.c` | KMS: pick the connector (Composite / HDMI-A, **accept "unknown" status**), mode, two dumb buffers, page flip + event |
| `video_png.c` | headless: write frames as PNG (tests, screenshots) |
| `input_evdev.c` | joysticks, gamepads and keyboards from `/dev/input/event*` (hotplug via inotify) |
| `input_wifi.c` | ELRS/Crossfire WiFi joystick: a C port of `JoystickEngine.cs` (beacons, `/udpcontrol` POST on a thread, frames, placeholder filter, source lock) |
| `nav.c` | the sticks drive the menus when not flying; a switch channel pauses |
| `web.c` | small HTTP/1.1 server: static files + JSON API |
| `settings.c` | read/write `sticktime.txt` |

Rules:
- Draw exactly like `engine.js`. The game's layout and its tests assume it.
- Nothing blocks the frame loop: HTTP activation, updates, big file writes and scripts go to other threads or processes.
- No allocation in the steady-state frame path (the glyph cache warms up once).

### 3.2 Frame loop and latency

```
per frame:
  sleep until  next_vblank − (render_p95 + margin)      ← "late latching": render just in time
  drain inputs (UDP, evdev) non-blocking                 ← the newest sample wins, no smoothing
  clock = monotonic ms → getTime() = clock / 10
  STICKTIME_DT = display time from the last frame's refresh to this one's (ms)
  run(event)  → Lua draws into the RGB565 buffer
  copy/scale into the back dumb buffer (x-scale for non-square composite pixels)
  drmModePageFlip(event) → read flip event (vblank timestamp, sequence)
  update render_p95; when idle, step the Lua GC (lua_gc STEP) after the flip
```

- Learn the vblank phase from page-flip event timestamps. Margin about 1.5 ms. On a missed flip, back off.
- With rendering of about 3 ms, the stick sample is about 5 ms older than scanout start, instead of up to one whole frame.
- Process setup: `SCHED_FIFO` priority for the main thread, `mlockall`, cpufreq governor
  `performance` (set by the setup script), no swap.
- Inputs: ask ELRS for `interval=4000` (250 Hz; configurable, ELRS allows ≥ 1000 µs). USB HID polls at 1 ms.
- `STICKTIME_LAT`: try 0 first (frames show quickly), then the measured sample-to-mid-scanout time. Pick by feel and slow-motion video.
- `STICKTIME_DT`: the color game (after 1.8.0) steps its simulation by it. From the 10 ms `getTime()` ticks
  alone, 60 Hz frames step 0.6 ms unevenly on average and at times 3 ms (micro-judder in fast turns); with it
  the steps are even. `STICKTIME_AV = true` (set before the script loads when the output is composite): the
  game outlines its HUD text, and its Colors setting's Auto is the natural Summer look.
- A latency test mode: a white box appears the frame a chosen channel crosses 50%. Film the stick and the goggles at 240 fps.

### 3.3 Video modes

| Output | Mode | Render size (square pixels) | Copy |
|---|---|---|---|
| AV NTSC 4:3 | 720×480i | 640×480 | scale x 640→720 |
| AV NTSC 16:9 | 720×480i | 854×480 | scale x 854→720 |
| AV PAL 4:3 | 720×576i | 768×576 | scale x 768→720 |
| AV PAL 16:9 | 720×576i | 1024×576 | scale x 1024→720 |
| HDMI | 1280×720@60 if offered, else preferred | 1280×720 | 1:1 or nearest |

- Fonts by render height: 480 → `lrg` sizes (as the 800×480 MK3). 720 → `pc`. 1080 → `hd`.
  Other heights → scale `std` by H/272 × 0.78. Check legibility in real goggles: analog wants big text.
- The x-scale is a per-row LUT copy (about 0.5 ms on a Pi 4). Hardware plane scaling (HVS) is a later optimization.
- AV ↔ HDMI needs config.txt + reboot (Pi 4 limit). The setup script applies it at boot (see 3.7).
- **VERIFY ON HARDWARE**: flip rate in 480i (per field or per frame). Offer `av_mode = 240p` if needed.

### 3.4 Inputs and sticks

Normalized to channels CH1..CH16 (−1024..1024), then `stick_channels` (a channel per stick, e.g. `1 2 3 4`)
or else `channel_order` (default `AETR`) picks ail/ele/thr/rud for `getValue` (gamepads and the keyboard
already give ail/ele/thr/rud).

| Source | Detected by | Channels |
|---|---|---|
| ELRS/XF WiFi | beacon or `wifi_module_ip` | frame values 0..32767 → −1024..1024 |
| EdgeTX/OpenTX USB | name contains "EdgeTX"/"OpenTX" | ABS codes 0..7 = CH1..CH8 |
| ELRS BLE | name "ELRS Joystick" (4.x) / "ExpressLRS Joystick" (3.x) | 4.x: X=CH1, Y=CH2, RY=CH3, RZ=CH4, Z=CH5, RX=CH6; 3.x: RX=CH3, RY=CH4, RZ=CH6 (RESEARCH.md §3) |
| Gamepad | has BTN_GAMEPAD | mode-2 layout: thr = −ABS_Y, rud = ABS_X, ele = −ABS_RY, ail = ABS_RX |
| Other joystick | fallback | ABS codes in order |

- The newest active source wins (or a fixed `input = wifi|usb|bluetooth`). Per-axis invert and calibration come from the settings.
- **Menus without taking the goggles off** (states MENU, SETUP, PAUSED, DONE):
  pitch up/down = PREV/NEXT (repeat while held), roll right = ENTER, roll left = EXIT.
  Use deflection > 60% with hysteresis, Betaflight-OSD style.
  In SETUP edit mode, pitch = INC/DEC. The keyboard always works too (arrows, Enter, Esc, +/−).
- **Pause in flight**: a switch channel (`pause_channel`, default CH5) changing state sends EXIT.
  Gamepad: Start/Select buttons.
- The game asks for "ail ele thr rud" with `getFieldInfo`, so return ids 1..4.

### 3.5 Storage on the FAT boot partition (editable from a PC)

```
/boot/firmware/sticktime.txt              settings (comments explain every key)
/boot/firmware/sticktime/
  bin/sticktime-pi                        static aarch64 binary   (bin.prev/ kept for rollback)
  sd/SCRIPTS/TOOLS/StickTime.lua          the game (from a StickTime release), color edition
  sd/SCRIPTS/TOOLS/StickTimeBW.lua, StickTimeBW/, StickTimeLite.lua, StickTimeLite/   the B&W and Lite editions
  sd/SCRIPTS/TOOLS/StickTimeTracks/c1..c8.txt   your tracks (9–16)
  sd/SCRIPTS/TOOLS/StickTimeGhosts/       ghosts
  sd/SCRIPTS/TOOLS/StickTime.dat          bests and settings of the game (StickTimeBW.dat, StickTimeLite/data.txt)
  web/                                    the web UI, the StickTime editor page, the track gallery
  VERSION                                 app version; sd/.../GAME_VERSION for the game
```

Writes: temp file + `fsync` + `rename` (safe against power loss on FAT).

### 3.6 Settings file (`sticktime.txt`, key = value)

```
# video
video = av              # av | hdmi
tv = ntsc               # ntsc | pal | pal60 | ntsc-j
aspect = 4:3            # 4:3 | 16:9 (what your goggles show)
# wifi (2.4 GHz recommended next to 5.8 GHz video)
wifi_ssid =
wifi_password =
wifi_country = HR
hotspot = auto          # auto: start "StickTime" AP when no known network | on | off
hotspot_password = sticktime
# sticks
input = auto            # auto | wifi | usb | bluetooth | keyboard
channel_order = AETR
stick_channels =        # e.g. 1 2 3 4: the channel of ail, ele, thr, rud (empty: channel_order)
wifi_module_ip =        # empty: find it by its beacon
wifi_interval_us = 4000
pause_channel = 5
invert =                # e.g. ail,thr
bluetooth = on          # auto-pair ELRS BLE Joystick / gamepads
# VTX control (SmartAudio / Tramp on a GPIO UART: host/src/vtx.c, user guide section 9)
vtx = off               # off | auto | smartaudio | tramp
vtx_port = gpio4        # one pin in software (vtx_soft.c): gpio4|17|22|23|27; two pins: uart3|4|5 (stboot.py adds dtoverlay=<uart>)
vtx_band =              # a VTX table letter, empty = leave it; vtx_channel 1-8; vtx_freq MHz overrides
vtx_power = 0           # 1-based VTX table level, 0 = leave it; vtx_pit, vtx_send_only = on | off
# system
hostname = sticktime
ssh = off
password =              # sets the 'pi' user's password (then cleared from this file)
auto_update = off
keep_logs = off         # on: the journal on the SD card (Storage=persistent, 50 MB), applied by sticktime-setup
```

### 3.7 Image (`image/`, built in GitHub Actions)

- Base: Raspberry Pi OS Lite 64-bit (`https://downloads.raspberrypi.com/raspios_lite_arm64_latest`,
  record the exact file and its sha256 in the release notes). It's Trixie, kernel 6.12: composite verified.
- Customize the official image on an ubuntu runner: enlarge, loop-mount, chroot through qemu-user-static.
  - Packages: whatever Lite lacks (check: `curl`, `ca-certificates`, `bluez` and `avahi-daemon` should already be there).
  - Hostname `sticktime`, so avahi gives `sticktime.local`. Create user `pi` with password locked
    (set via settings), so no first-boot user dialog grabs tty1 (`userconfig.service`).
  - systemd units: `sticktime.service` (Restart=always, conflicts with getty@tty1),
    `sticktime-setup.service` (oneshot, before NetworkManager and sticktime), `sticktime-bt.service`,
    `sticktime-update.service` + timer (when `auto_update = on`).
  - Disable what slows boot or wakes the CPU: apt-daily timers, man-db timer, and similar. Keep the first-boot resize.
  - cmdline.txt: `quiet loglevel=3 logo.nologo vt.global_cursor_default=0 consoleblank=0` (+ `vc4.tv_norm=…` from settings).
  - config.txt: our block between markers, rewritten by the setup script from the settings
    (`dtoverlay=vc4-kms-v3d[,composite]`, `disable_overscan=1`, ...).
- `sticktime-setup` (shell): on every boot it reads `sticktime.txt` and applies hostname, WiFi
  (NetworkManager connections, band bg), hotspot, ssh, password, cpufreq governor and video.
  When config.txt or cmdline.txt change, it reboots once.
- Output: `sticktime-pi-<ver>.img.xz` + sha256, a CI artifact; a release on tags.
- Later, if smaller and faster boot matter: Buildroot or Alpine diskless with the same static host.

### 3.8 Web UI (`web/`, served by the host on port 80)

Pages: **Status** (FPS, frame and latency numbers, input source and packet rate, IP, versions),
**Video**, **WiFi** (scan, join, hotspot), **Sticks** (live bars for all channels, source, order,
invert, calibration, pause switch, ELRS module IP and interval), **Tracks**, **Updates**, **System** (ssh, password, reboot).

JSON API (sketch): `GET /api/status`, `GET/POST /api/settings`, `GET /api/sticks` (or SSE stream),
`GET /api/wifi/scan`, `POST /api/wifi`, `GET/PUT/DELETE /api/sd/<path>` (inside sd/ only),
`POST /api/game/reload`, `GET /api/update/check`, `POST /api/update/apply`, `POST /api/reboot`.

**Tracks like the web**: serve StickTime's own `web/simulator.html` (from the matching StickTime
release) with an injected `pi-sd.js`. It replaces `window.showDirectoryPicker()` with a "directory
handle" backed by `/api/sd/`, so the editor's existing **Connect radio / Send to radio / My tracks**
(and even Install) work against the Pi's SD folder, unchanged. Check `editor.js` for the exact File
System Access calls it uses. Serve the gallery (`tracks/*.txt|json` + `index.json` from
`tools/gallery.py`) next to it. After a track changes, reload the game at the next menu.

### 3.9 Updates

- **Game**: latest release of `juricabi/sticktime`. Its `StickTime-<ver>-sdcard.zip` gives
  `StickTime.lua` and the B&W and Lite editions (loader + folder), kept and installed as one set (a slot,
  never the player's saves: `*.dat`, `StickTimeLite/data.txt`). Take `web/simulator.html` and `tracks/` from the same tag
  (`raw.githubusercontent.com/juricabi/sticktime/<tag>/…`).
- **App**: releases of this repo: `sticktime-pi-app-<ver>.tar.gz` with `bin/`, `web/`, `fonts/`, `lib/`,
  `VERSION`, `SHA256SUMS`. `lib/` is the system scripts (`/usr/lib/sticktime`: the boot setup, the WiFi,
  hotspot and Bluetooth helpers, this updater), exactly as the image installs them. The updater swaps and
  rolls `lib/` back with the rest of the set; `stupdate/system.py` then copies it into `/usr/lib/sticktime`
  (checked against `SHA256SUMS`, compiled, written atomically, and put back if the updater no longer starts).
  The host runs the installed set's own copy (`lib/sticktime-update sync`) at every start, so a set that an
  older updater installed still brings its scripts. Before the first change the image's own files are kept
  in `/var/lib/sticktime/system/image/`; a set without `lib/` (0.2.1 and older) brings them back. So a fix in
  the system scripts reaches a running Pi with an app update; keep system changes in these scripts (the boot
  setup can change `/etc` and the boot files) rather than in image-only steps of `image/build.sh`.
- Updater (`sticktime-update`, Python 3 from Pi OS): check, download to `.staging/`, verify, swap
  folders with `rename`, keep `.prev/`, restart the service. The web UI shows "update available"
  and installs on a button (or `auto_update = on`, checked at boot when online).
- OS updates: rare. Reflash, or `apt full-upgrade` over ssh for experts.
- Releases of a private fork need a token (`github_token` in the settings); the public repo needs none.

### 3.10 CI (`.github/workflows/`)

- `build.yml` (push/PR): build the host for x86_64 (tests) and aarch64 (static, cross gcc or zig cc);
  run headless tests (render menu + every track to PNG, frame-time budget, Lua errors fail the build);
  upload artifacts.
- `image.yml` (tag or manual): build the image, upload it, attach it to the release with the app tarball.

## 4. Milestones

| # | Milestone | Done when |
|---|---|---|
| M1 | Host runs the game headless on x86 | menu + all 8 tracks render to PNG matching the web emulator's look; no Lua errors; frame times logged |
| M2 | Real Pi, stock Pi OS Lite, deploy over SSH | picture on composite → VTX → goggles and on HDMI; keyboard + USB radio fly it; measured frame time and flip rate (480i question answered) |
| M3 | WiFi + menus by sticks | ELRS module found by beacon and by IP; 250 Hz stream; Crossfire placeholder handling; fly and navigate menus with goggles on; pause switch |
| M4 | Web UI | status, settings, live sticks, WiFi join/hotspot from a phone; tracks via the StickTime editor + gallery |
| M5 | Image in CI | flash → boots to the sim with no keyboard, AV by default; settings file works from a PC |
| M6 | Updates + Bluetooth | game and app update from GitHub with rollback; ELRS BLE auto-pairs |
| M7 | Polish | latency test mode + numbers in README; 240p option; motor sound on the jack's audio pins to a VTX audio pad; read-only root option |

## 5. Open questions for the owner

- Goggles: 4:3 or 16:9? NTSC or PAL preferred?
- Which ELRS TX module (ESP32-based → BLE works)? Crossfire too?
- Pi 4 RAM size? Also target the Pi 5 or Zero 2 W later? (The Pi 5 has composite on test pads only.)
- VTX power: separate 7–12 V, or 5 V from the Pi? Keep 25 mW (EU 5.8 GHz limit).
