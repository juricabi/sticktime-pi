# Research notes (verified facts, with sources)

Everything here was checked against source code or documentation while StickTime Pi was written
(2026-10-16). Items marked **VERIFY ON HARDWARE** are still open.

## 1. Composite (AV) video on the Raspberry Pi 4

- **KMS config that works on Raspberry Pi OS Trixie** (kernel 6.12.75+rpt-rpi-v8, image 2025-12-04):
  - `/boot/firmware/config.txt`: `dtoverlay=vc4-kms-v3d,composite`
  - `/boot/firmware/cmdline.txt`: append `vc4.tv_norm=NTSC` (or `PAL`, ...)
  - The console, DRM apps and `cvlc` show a picture; `kmsprint` lists an active CRTC on `Composite-1`.
  - **The connector reports `DRM_MODE_UNKNOWNCONNECTION`, not connected.** wlroots/labwc treat it
    as disconnected and show black. **Our DRM code must accept "unknown" connectors.**
  - Source: https://github.com/raspberrypi/trixie-feedback/issues/66
- `vc4.tv_norm` values: `NTSC`, `NTSC-J`, `NTSC-443`, `PAL`, `PAL-M`, `PAL-N`, `PAL60`, `SECAM`.
  Default NTSC. Source: https://github.com/raspberrypi/documentation/pull/3537/files
- **On the Pi 4 (and Pi 5), enabling composite disables HDMI.** Both can't run at once, so
  switching AV ↔ HDMI means editing config.txt and rebooting. Composite isn't available on the Pi 400.
  The same docs change mentions `enable_tvout=1` for Pi 4/5. The working Trixie report above used
  only the overlay and `tv_norm`. **VERIFY ON HARDWARE** whether `enable_tvout=1` is needed too.
- A **regression in kernel 6.6** gave black composite on the Pi 4 (CRTC never attached).
  Fine on 6.1, and works again on 6.12 per the Trixie report. Pin a known-good image and kernel.
  Source: https://github.com/raspberrypi/linux/issues/5814
- Mode strings seen in the wild: `video=Composite-1:720x480@60ie` (NTSC), `video=Composite-1:720x576@50ie` (PAL).
  Lakka uses `video=Composite-1:720x480@60e` and `720x576@50e`, and ships 240p on the Pi.
  Source: https://lakka.tv/articles/2024/05/02/rpi-composite/
- **VERIFY ON HARDWARE:**
  - With 480i, does a DRM page flip land at every field (60/s) or only every frame (30/s)?
    If it's 30/s, try a progressive 240p/288p mode for 60 progressive updates (lower latency, no combing).
  - Which mode list the connector offers on Trixie (`kmsprint -m`).
- **3.5 mm TRRS jack pinout** (Pi 2/3/4): tip = left audio, ring 1 = right audio, ring 2 = ground,
  **sleeve = composite video**. Camcorder-style cables often carry video on the *red* plug.
  To feed a VTX: sleeve → VTX video in, ring 2 → VTX ground. The audio pins are free for a VTX audio pad later.
  Source: https://pinouts.ru/HeadsetsHeadphones/Raspberry_pi_composite_vi.shtml
- Composite on the Pi 4 is off by default because its clock affects other clocks ("slightly slows
  down the entire system"). Irrelevant for us. Source: https://github.com/raspberrypi/documentation/pull/2541

## 2. ExpressLRS WiFi joystick (and TBS Crossfire "VelociDrone Mobile" link)

From the ExpressLRS source (`src/lib/WIFI/wifiJoystick.cpp`, `devWIFI.cpp`, `wifiJoystick.h`) and the
owner's Windows app (`github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Windows`, `JoystickEngine.cs`):

- Port **UDP 11000** for both the beacon and the stream.
- **ELRS beacon**: every 5 s, broadcast to 255.255.255.255:11000. Payload:
  `'E','L','R','S'` (big-endian magic), `uint8 version` (=1), `uint16 port` (LE), `uint8 name_len`, `name`.
  **Crossfire beacon**: starts with ASCII `VELOCIDRONE` (~every 8 s).
- **Activation**: HTTP `POST http://<module-ip>/udpcontrol`, form body
  `action=joystick_begin&interval=<µs>&channels=<n>`. The module answers `ok` and starts streaming
  to the IP that sent the POST. The owner's app sends `interval=10000&channels=8`, throttles POSTs to
  one every 5 s per module, and runs them off the receive thread.
- **ELRS accepts `interval` ≥ 1000 µs** (below that, or 0, it falls back to 10000). So **lower
  latency is available: e.g. 4000 µs (250 Hz) or 2000 µs (500 Hz)**. Channels: 1–16 (0 → 8).
- **Channel frame**: `uint8 type` (=1), `uint8 count` (4–16; Crossfire always sends 16), then
  `count × uint16 LE`. ELRS maps CRSF 1000–2000 µs (`CRSF_CHANNEL_VALUE_STD_MIN..MAX`) to **0–32767**
  (constrained, so full stick = endpoint).
- **Crossfire quirks** (from the owner's app): a channel with no data holds `0xF26A` (62058). Drop any
  frame that contains it (all channels sit at it while there's no input, and corrupt frames mix it with
  garbage under fast stick moves). Values a bit above 32767 *without* the placeholder are real stick
  overshoot: clamp them. Drop a frame whose channels are *all* out of range.
  Broken TBS firmware: XF 6.42/6.48, WiFi module 3.20 (only placeholders). Works: XF 6.31/6.36, WiFi 2.17–3.10.
- **Source lock**: bind to the first module that sends real channel data; ignore other sources until
  the bound one has been silent for 3 s. With an explicit module IP, accept only that IP.
- There's **no stop command**: the module streams until it's powered off or its WiFi drops.
- Module WiFi is **2.4 GHz only**. Keep the Pi on 2.4 GHz too: 5 GHz WiFi channels sit next to the
  5.8 GHz video band.
- In AP mode the ELRS TX module is at `10.0.0.1` (SSID `ExpressLRS TX`, password `expresslrs`), and a
  TBS module at `192.168.4.1`. If the Pi joins the module's AP, POST to the gateway IP: no beacon wait.
  **VERIFY** whether the module beacons in AP mode.

## 3. ExpressLRS BLE joystick

From `src/lib/BLE/devBLE.cpp` and https://oscarliang.com/expresslrs-bluetooth-joystick/

- ESP32-based TX modules only. Start it from the ExpressLRS Lua script → "BLE Joystick". RF is
  turned off while it runs. Exiting the Lua script turns Bluetooth off. Range is about 3 m.
- Advertised name: **`ELRS Joystick`** in 4.x (`new BleGamepad("ELRS Joystick", "ELRS", 100)`),
  **`ExpressLRS Joystick`** in 3.x (`ELRSGamepad() : BleGamepad("ExpressLRS Joystick", "ELRS", 100)`).
  Pairs without a PIN (Just Works).
- HID joystick, 8 axes (each 0..32767: 3.x maps to that, 4.x to the library's default axes range 0..0x7FFF),
  buttons 1–8 = CH9–CH16 (pressed when ≥ 2000 µs).
  3.x keeps the library's defaults (16 buttons, 1 hat), 4.x sets 8 buttons and no hat.
- Axes: ELRS calls, in 3.3 through 4.x alike,
  `bleGamepad->setAxes(data[0], data[1], data[4], data[5], data[2], data[3], data[6], data[7])`
  (`src/lib/BLE/devBLE.cpp`, line 38 at tag 3.5.3, line 26 at 4.1.0; `data[i]` = CH(i+1)). What that means
  depends on the version of lemmingDev's ESP32-BLE-Gamepad it is built with:
  - **ELRS 3.x** (`src/targets/common.ini` 3.3.0–3.6.x: `lemmingdev/ESP32-BLE-Gamepad @ 0.5.2`): `BleGamepad.h` 0.5.2
    (GitHub tag `v0.5.2` and the PlatformIO registry package alike) is `setAxes(x, y, z, rZ, rX, rY, slider1, slider2)`
    → x=CH1, y=CH2, z=CH5, rZ=CH6, rX=CH3, rY=CH4: **ABS_X=CH1, ABS_Y=CH2, ABS_RX=CH3, ABS_RY=CH4, ABS_Z=CH5, ABS_RZ=CH6**.
    The library kept that order up to v0.7.1.
  - **ELRS 4.x** (4.0.x `common.ini`, 4.1.0 `esp32-tx.ini`: `@ 0.7.4`; master: `@ 0.7.5`): from v0.7.2 on the library
    has `setAxes(x, y, z, rX, rY, rZ, slider1, slider2)` → x=CH1, y=CH2, z=CH5, rX=CH6, rY=CH3, rZ=CH4:
    **ABS_X=CH1, ABS_Y=CH2, ABS_RY=CH3 (throttle), ABS_RZ=CH4 (yaw), ABS_Z=CH5, ABS_RX=CH6**.
    Verified on a real Pi with an ELRS 4.x module ("ELRS Joystick"): at rest X, Y centred, Z = 0, RX = 32767,
    RY = 19 (throttle low), RZ centred; with the 3.x table its yaw showed on CH6.
  - In both, the report descriptor declares X, Y, Z, Rz, Rx, Ry, Slider, Slider in that order and the report
    carries `_x, _y, _z, _rZ, _rX, _rY, _slider1, _slider2` in the same order; only the setter's argument
    order changed. So the device name tells the two apart, not the descriptor.
  - The two sliders are both HID usage Slider (0x36). Linux maps Slider to ABS_THROTTLE (`hidinput_configure_usage`:
    `HID_GD_SLIDER` → `map_abs(usage & 0xf)`); the second one is a duplicate, which kernels with
    `HID_QUIRK_INCREMENT_USAGE_ON_DUPLICATE` handling (6.12 here) **drop** ("Some usages could not be mapped") unless
    the device has that quirk; older kernels moved it to the next free code (ABS_RUDDER, else ABS_WHEEL). So CH7 is
    ABS_THROTTLE and CH8 usually doesn't reach Linux at all.
  - StickTime Pi: CH1..CH8 = X, Y, RY, RZ, Z, RX, THROTTLE, RUDDER (or WHEEL), and X, Y, RX, RY, Z, RZ, ... for a
    device named "ExpressLRS ..." (3.x). Any other layout: the `stick_channels` setting (a channel per stick).
- On Linux, BLE HID goes through BlueZ (HOGP) → uhid → a normal `/dev/input/event*` device.

## 4. EdgeTX USB joystick (radio plugged in, "USB Joystick (HID)")

From `radio/src/usb_joystick.cpp` (EdgeTX main):

- **Classic mode**: 8 axes, 11-bit (0–2048), **CH1..CH8 → X, Y, Z, Rx, Ry, Rz, Slider, Dial**
  (Linux ABS codes 0..7 in that order), plus 24 buttons = CH9..CH32 (on if the channel is > 0).
  The channels are the model's mixer outputs, so stick mode and AETR order are applied by the radio.
  Recommend a plain "sim" model: 100% weights, no expo.
- **Advanced mode** lets the user assign any channel to any axis or button.

## 5. StickTime and the EdgeTX API the game needs

- Run the built color script `sdcard/SCRIPTS/TOOLS/StickTime.lua` from the StickTime repo
  (`build.py` output; it includes the `STICKTIME_TEST` hooks). It returns `{ init = init, run = run }`.
  `run(event, touch)` is called every cycle. Returning nonzero means "close the tool".
- **Reference implementation to port: `web/src/engine.js` (color path)** in the StickTime repo.
  It mimics the firmware, and the game is tested against it. Highlights:
  - Globals: `LCD_W`, `LCD_H`, the color flags (`INVERS 0x01, VCENTER 0x02, CENTER 0x04, RIGHT 0x08,
    LEFT 0, SHADOWED 0x80, BLINK 0x1000, TIMEHOUR 0x2000, LEADING0 0x10, PREC1 0x20, PREC2 0x30,
    SOLID 0xff, DOTTED 0x55, BOLD 0x100, TINSIZE 0x200, SMLSIZE 0x300, MIDSIZE 0x400, DBLSIZE 0x500,
    XXLSIZE 0x600`), named theme colors as `index << 16`, the `EVT_*` constants, `PLAY_NOW`/`PLAY_BACKGROUND`.
  - Colors: `lcd.RGB(r,g,b)` returns `(rgb565 << 16) | 0x8000` as a signed 32-bit integer. A flags
    word with bit 0x8000 carries RGB565 in its top 16 bits; otherwise the top 16 bits index the named
    colors. Treat flags as `uint32_t`.
  - `lcd.drawLine` **draws nothing if any endpoint has `x > LCD_W` or `y > LCD_H`** (firmware quirk the
    game relies on and tests for), then Liang-Barsky clip + Bresenham with an 8-bit pattern.
  - `lcd.drawFilledRectangle(x,y,w,h,flags,opacity)`: opacity 0 (opaque) .. 15 (invisible), blended in RGB565.
  - `lcd.drawFilledTriangle`: Adafruit-GFX fill, one horizontal span per row, integer division truncating toward zero.
  - `getTime()` = 10 ms ticks (integer). `getValue('ail'|'ele'|'thr'|'rud' or field id)` = −1024..1024.
    `getFieldInfo(name)` → `{id, name, desc}` (ids 1..4). `getStickMode()` = 1..4.
  - `io.open/read/write/seek/close` with EdgeTX signatures (`io.read(f, n)`, `io.write(f, ...)`).
  - `loadScript(path, mode, env)`: a third argument leaves the chunk with a nil `_ENV` (EdgeTX quirk).
  - Text: Roboto, sizes per screen class (px for STD, BOLD, XXS, XS, L, XL, XXL, LXL):
    `std [16,16,9,13,24,32,64,48]` (480×272), `sml` (320×240), `lrg [22,22,12,18,33,44,88,66]` (800×480),
    `pc [32,…]` (1280×720), `hd [48,…]` (1920×1080). `lcd.sizeText` reports EdgeTX line heights
    (`LINE_H` table). Glyph metrics: ascent = round(px × 0.93), line height = round(px × 1.17),
    per-character advance summed as floats, no kerning, each glyph rasterized at an integer pen x.
- Lua: EdgeTX builds Lua 5.3 with **32-bit integers, single-precision floats, `LUA_FLOORN2I`**
  (floats are floored when converted to integers). Flags: `-DLUA_32BITS -DLUA_FLOORN2I=1 -DLUA_COMPAT_5_2`
  (see `tools/build_etxlua.sh` in the StickTime repo). Use `luaL_checkinteger` for API arguments
  (it floors floats under FLOORN2I). Never `luaL_checknumber` on flags: a float can't hold every 32-bit value.
- Game states (from `src/sticktime.lua`): `MENU 1, SETUP 2, COUNT 3, FLY 4, CRASHED 5, READY 6, PAUSED 7, DONE 8`.
  `STICKTIME_TEST.get()` returns `state` as its 16th value (define the global `STICKTIME_TEST = {}`
  before `init()`). Use it to switch the sticks between menu navigation and flying.
  Events the game handles: ENTER, EXIT, NEXT/PREV (+ _REPT), INC/DEC (+ _REPT), NEXT_PAGE/PREV_PAGE, TOUCH_TAP.
  - EXIT in flight pauses. The pause menu is Resume, Respawn, Restart, Settings, Main menu.
  - In READY, any stick move > 0.12 (or throttle > 0.3) starts flying, or it starts on its own after 2 s.
- Timing: the game smooths the 10 ms `getTime()` steps and runs physics in 80 Hz substeps on the
  measured frame time. Tested at 50, 20 and 16 ms frames.
  - Camera prediction: `STICKTIME_LAT` (seconds) overrides the display-delay prediction. 0 turns it off.
    The web emulator's PC mode sets 0 because frames show at once. Set it to the measured
    sample-to-display time, or 0.
  - After 1.8.0 (the color game): `STICKTIME_DT` (ms, read every frame) is the display interval, and the
    simulation steps by it instead of the smoothed ticks (pulled 1 % a frame towards the tick clock).
    `STICKTIME_AV = true` (read when the script loads): analog video. The HUD text gets a black outline
    instead of the drop shadow, and Settings → Colors Auto is Summer (else Classic). The save keeps it as
    `colors=` (0 Auto, 1 Classic, 2 Summer, 3 Evening, 4 Overcast).
- Saves: `StickTime.dat`. Ghosts: `StickTimeGhosts/c<N>.txt` (5 KB, written when a lap sets a new ghost).
  Custom tracks: `StickTimeTracks/c1.txt..c8.txt` (tracks 9–16), read at start ("restart the game after adding one").

## 6. VTX control: SmartAudio, Tramp, VTX tables and the Pi's UARTs

Checked 2026-10-17. Betaflight, INAV, ArduPilot, OpenVTx and ExpressLRS are GPL-3.0: they were read for protocol
facts and behaviour only; `host/src/vtx*.c` and `host/tests/fake_vtx.py` were written from these notes. (PX4's VTX
driver is BSD-3-Clause, but nothing was taken from it either.)

### SmartAudio (TBS)
Primary source: TBS "Unify Pro / SmartAudio" spec rev 09 (2018), https://www.team-blacksheep.com/tbs_smartaudio_rev09.pdf.
- Line: **4800 baud, 1 start + 2 stop bits**, logic high 0.9-3.3 V, low 0-0.5 V; one wire, half duplex, both sides
  inputs when idle; the Unify's software UART can be **±5 % off** (temperature). "The SmartAudio line need to be low
  before a frame is sent. If the host MCU can't handle this it can be done by sending a 0x00 dummy byte in front of
  the actual frame." The VTX answers every frame, usually within 100 ms.
- Frame `AA 55 <cmd> <len> <payload> <crc>`; host commands are sent as `(cmd << 1) | 1`: GET_SETTINGS 1, SET_POWER 2,
  SET_CHANNEL 3 (0..39: band A1 = 0 ... R8 = 39, bands A, B, E, F, R), SET_FREQ 4 (big-endian MHz; bit 15 = set the
  pit frequency, bit 14 = get it), SET_MODE 5 (v2+: bit 0 in-range pit, 1 out-range pit, 2 **leave** pit mode (the
  bit table calls it "running", but the commands use 0x04 to switch pit mode off; ExpressLRS's page notes this too),
  3 unlock). CRC-8, polynomial 0xD5, initial 0.
- **Every CRC example of the spec recomputed (test_vtx.c checks them):** host frames match only with the CRC over the
  whole frame including `AA 55` (GET_SETTINGS `AA 55 03 00 9F`, SET_POWER `AA 55 05 01 00 6B`, 2.1 power
  `AA 55 05 01 8E 2C`, SET_CHANNEL `AA 55 07 01 00 B8`, SET_FREQ 5865 `AA 55 09 02 16 E9 DC`, SET_MODE
  `AA 55 0B 01 0A 7B`); **replies only with the CRC over cmd + len + payload, without `AA 55`**, and their len counts
  the payload plus the CRC (V1 `AA 55 01 06 00 00 01 16 E9 4D`, V2.1 `AA 55 11 0C ... 1A 01`, SET_POWER
  `AA 55 02 03 00 01 0F`). Typos in the spec: the V2 reply says CRC 0xDA (0x0A by the maths and its own capture), the
  2.1 SET_POWER reply says 0x0F (0x6D), the version table says 0x10 for 2.1 (its example and every implementation use
  0x11). The spec's captures show a 0x00 after the replies (the line dropping to idle).
- Real devices differ: OpenVTx (a VTX-side implementation, https://github.com/OpenVTx/OpenVTx `src/src/smartAudio.c`)
  sends **len = payload only** and the CRC after it, no trailing byte, 10 ms after the request. Betaflight's reader
  (`src/main/io/vtx_smartaudio.c`, `saReceiveFrame`) reads len bytes then a CRC byte: with TBS framing that "CRC" is
  the 0x00 after the reply (a message followed by its own CRC-8 gives 0). **Our parser takes both** (the CRC where the
  spec puts it, else one byte later with the payload length the command must have) and tells our own echo from a
  reply by where the CRC starts (needed: host SET_FREQ 0x09 = the V2 GET_SETTINGS reply code).
- GET_SETTINGS reply: version in bits 7..3 of the command (0x01 v1, 0x09 v2, 0x11 v2.1); payload channel, power,
  operation mode (bit 0 tuned by frequency, 1 pit mode on, 2 in-range pit flag, 3 out-range pit flag, 4 unlocked),
  frequency; v2.1 adds the current dBm, a level count and the levels. TBS's example says count 3 and lists 4 values
  (0, 14, 20, 26: 0 dBm = pit mode); Betaflight, INAV and PX4 read count levels after the 0; OpenVTx sends the true
  count and no 0 (its table is 1, 2, 14, 20, 26 dBm). **We count what the frame holds** (at most count + 1) and drop 0.
- Power: v1 = the PA's DAC value (spec: 7, 16, 25, 40 for 25/200/500/800 mW), v2 = level 0..3, **v2.1 = dBm with the
  MSB set** (rounded down to a supported level; the reply then carries dBm). 2.1: **0 dBm = pit mode at once without
  the boot flag** (spec changelog; TBS Unify Pro32 manual p. 18, https://team-blacksheep.com/media/files/tbs-unify-pro32-manual.pdf);
  after it the VTX keeps reporting the previous dBm with the pit bit set (Betaflight's comment); any power command
  leaves pit mode. Out-range pit mode becomes in-range on 2.1. SmartAudio 2.0 can't switch pit mode on at once: the
  flags only make it boot into pit mode (Betaflight's `vtxSASetPitMode` refuses "on" below 2.1). ExpressLRS sends the
  old level numbers (no MSB) to a Pro32 HV (2.1) and that works: 2.1 VTXes take both.
- Quirks seen by other hosts: Betaflight's `USE_AKK_SMARTAUDIO` (and INAV's default "early AKK workaround") sends a
  0x00 **after** each frame ("AKK/RDQ SmartAudio devices can expect an extra byte"); INAV, ExpressLRS and PX4 send the
  leading 0x00; Betaflight's PR #7603 (always the 0x00 on soft serial) was reverted in #7789 because it broke a TBS 2.1
  device; INAV's `vtx_smartaudio_stopbits` says the TBS Sixty9 (2.1) needs **1 stop bit**; Betaflight adjusts the baud
  4800-4950, ArduPilot 4560-5040 (`AP_SmartAudio.h`); Betaflight waits 120 ms for a reply and polls 150 ms apart.
  ArduPilot's `VTX_OPTIONS` bits 4-6 (leading zero, 1 stop bit, ignore CRC) exist for the same reasons.
- The line: Betaflight drives SmartAudio push-pull and idles it **low with a pull-down** (`drivers/serial.h`,
  `SERIAL_PULL_SMARTAUDIO`; the Betaflight wiki says the Sparky2's Flexi port pull-ups make it unsuitable for the Unify
  Pro; a TBS Unify Nano needed an external pull-down, Betaflight PR #6380). ExpressLRS idles it high (input with
  pull-up) and only sends; ArduPilot pulls down.

### Tramp (ImmersionRC)
No ImmersionRC document exists (OpenVTx's README: "IMRC has not released a protocol standard"); every implementation
(Betaflight `src/main/io/vtx_tramp.c`, OpenVTx `src/src/tramp.c`, ArduPilot `AP_Tramp.cpp`, INAV, PX4) agrees on:
- **9600 baud 8N1**, 16-byte frames both ways: `0F <cmd> <12 bytes, 16-bit values little-endian from byte 2>
  <sum of bytes 1..13 mod 256> 00`; replies must end in 0x00.
- Queries answered with the same letter: `r` → min MHz (bytes 2-3), max MHz (4-5), max mW (6-7); `v` → MHz (2-3),
  configured mW (4-5), control mode (6, bit 0 = race lock), pit mode (7, 1 = on), actual mW (8-9); `s` → temperature
  in °C, signed (6-7). Set commands, not answered: `F` MHz, `P` mW, `I` pit mode with **0 = on, 1 = off**.
- Betaflight: 200 ms between requests, status every 1 s, up to 20 tries per change ("some VTX fail to respond to
  every request (like Matek FCHUB-VTX)"); it opens Tramp in plain half duplex (idle high). ArduPilot: a Foxeer Reaper
  Infinity answers at about **9400 baud** (it adjusts 9120-10080). OpenVTx always reports 0 °C (Betaflight takes a 0 as
  its own echo; we compare with the frame we sent instead) and, before it locks onto a protocol, sends frames by
  itself after 3 s.
- Default power table (Betaflight and its wiki): 25, 100, 200, 400, 600 mW. Logic level: **not documented**;
  ExpressLRS drives Tramp HV from a 3.3 V ESP32 (https://www.expresslrs.org/software/serialvtx/).

### ExpressLRS "Serial VTX" (https://www.expresslrs.org/software/serialvtx/)
One-way only, from a receiver's ESP32 UART TX to the SmartAudio or Tramp pad (firmware 3.5+). Tested with a TBS Unify
Pro32 HV (SmartAudio), an ImmersionRC Tramp HV and OpenVTx. "Loosely-defined protocols with edge cases,
misinterpretations, and shoddy cloned implementations"; pit mode found unpredictable; bands A, B, E, F, R, L only.

### VTX tables (Betaflight Configurator JSON)
- Schema `vtxconfig_schema-1.0.json` (https://github.com/betaflight/betaflight-configurator, `resources/jsonschema/`):
  `description` (string), `version` ("1.0"), `vtx_table.bands_list[]` of `name`, `letter`, **`is_factory_band`**
  (bool) and `frequencies` (integers), and `vtx_table.powerlevels_list[]` of `value` (integer) and `label` (string).
  The channel count is the longest frequency list. Limits in Betaflight (`drivers/vtx_table.h`) and the Configurator:
  8 bands, 8 channels, 8 levels, band name 8 characters, letter 1, label 3; a frequency of 0 blocks that slot.
- Meaning (Betaflight wiki, `docs/wiki/guides/current/VTX.md`): a **factory** band is set by band/channel (the VTX's
  own table; the frequencies are only shown), others by frequency; Tramp only knows frequencies. Power values:
  SmartAudio 1.0 7/16/25/40, 2.0 0/1/2/3, 2.1 dBm (e.g. 14/20/26), Tramp mW. PX4's VTX docs say the same
  (https://docs.px4.io/main/en/vtx/index.html: an index for SA v1/v2, dBm for SA2.1, mW for Tramp; "some VTXs enter pit
  mode when power is set to 0").
- Betaflight's SmartAudio channel number for a factory band is `(band position in the table - 1) * channels +
  (channel - 1)`, so a table's factory bands must be in the VTX's own order (A, B, E, F, R for the Unify).
- Ready-made tables: betaflight.com `static/resources/vtx_tables` and the makers' sites (RunCam, Matek, SpeedyBee).
  None states a license, so our tests use a table made for them.

### The Raspberry Pi 4's UARTs
- Overlays README, rpi-6.12.y (https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/arch/arm/boot/dts/overlays/README):
  `uart3` "Enable uart 3 on GPIOs 4-7. BCM2711 only." (TX GPIO4, RX GPIO5; CTS/RTS on 6/7 only with `ctsrts`), `uart4`
  GPIOs 8-11 (overlaps SPI0), `uart5` GPIOs 12-15 (`ctsrts` would take the console's 14/15). Header pins
  (https://pinout.xyz/pinout/uart): **uart3 TX pin 7, RX pin 29**; uart4 TX 24, RX 21; uart5 TX 32, RX 33. The overlays
  set TX without a pull and **RX with a pull-up** (`bcm2711-rpi-ds.dtsi`).
- `bcm2711.dtsi`: PL011s `serial@7e201400` (uart2), **`serial@7e201600` (uart3)**, `serial@7e201800` (uart4),
  `serial@7e201a00` (uart5), all disabled by default; bus 0x7e... = ARM 0xfe..., so the platform device is
  `fe201600.serial`. The ttyAMA number follows the `serialN` alias and has varied between releases: we find the tty by
  `/sys/class/tty/ttyAMA*/device/of_node` (or the device's name), never by its number.
- Bluetooth on the Pi 4 uses UART0 (PL011, GPIO30-33; `bcm2711-rpi-4-b.dts`); uart3/4/5 are separate PL011 blocks on
  other pins and don't touch it. The console UART is the mini UART on GPIO14/15.
- **Not 5 V tolerant**: "All UARTs operate at 3.3 V; connecting to 5 V systems causes damage"
  (raspberrypi/documentation `computers/configuration/interfaces.adoc`); inputs are "3.3V-tolerant", VIL ≤ 0.8 V,
  VIH ≥ 2.0 V, default drive 4 mA, 8 mA at most (`gpio-on-raspberry-pi.adoc`).
- Linux termios: custom baud rates through `TCSETS2` + `BOTHER` (the PL011 divides its clock finely); `TIOCSBRK` /
  `TIOCCBRK` hold TX low / release it. In raw mode a break or a framing error reads as 0x00 unless `IGNBRK` / `IGNPAR`
  (termios(3)); we set `IGNBRK`.

### Electrical design of the two-pin UART mode (our reasoning from the facts above; **VERIFY ON HARDWARE**)
- One wire, no single-wire mode on the PL011: **TX → 1 kΩ → pad, RX → 4.7 kΩ → pad**, ground shared (the AV cable's).
  (The single-pin mode below uses only the first: GPIO4 → 1 kΩ → pad; the same voltage check applies.)
  The PL011's TX idles high, so between frames the pad sits at 3.3 V through 1 kΩ: a replying VTX must sink 3.3 mA to
  send a 0 (any MCU pin can; a VTX with its own series resistor in that line might not, hence the hint to try
  2.2-4.7 kΩ: SmartAudio's 0.9 V high threshold leaves room). The Pi's RX reads the pad through 4.7 kΩ against its
  ~50 kΩ pull-up: a low pad reads about 0.3 V, under VIL 0.8 V. Everything sent comes back on RX: the software matches
  that echo against what it sent and drops it; a missing echo means a missing RX wire, a garbled one a bad line.
- Protection: on a 5 V pad the 4.7 kΩ keeps the current into RX's clamp near 0.25 mA, but with TX high 1 kΩ lets
  (5 - 3.3) V / 1 kΩ ≈ 1.7 mA flow into TX all the time: too much to call safe. Hence "measure the pad first"
  (0-0.5 V or 3.0-3.4 V: fine; 3.5 V or more: a bidirectional level shifter) in the user guide.
- **Idle high with a 0x00 before each frame, not TIOCSBRK idle low, by default**: the spec offers the 0x00 byte for
  hosts that can't hold the line low; INAV, ExpressLRS (tested on a TBS Pro32 HV) and PX4 do this. Holding TX low
  through 1 kΩ between frames would make a replying VTX fight the Pi's low output for every 1 bit: the same load as
  idling high, without the spec's blessing. Because a TBS 2.1 device once broke with Betaflight's always-0x00 change,
  detection also tries "line held low between frames, no 0x00" (Betaflight's pull-down way, emulated with TIOCSBRK
  around each frame), "no lead-in", "0x00 after the frame" (AKK/RDQ), "1 stop bit" (Sixty9) and 4600-5000 baud;
  Tramp at 9600, 9400 and 9800.
- Send-only (TX wire only) works like ExpressLRS: nothing is read; frames go 200 ms apart, because a SmartAudio VTX
  still answers on the wire.

### Single-pin soft serial (the default `vtx_port = gpio4`; our design, **VERIFY ON HARDWARE**)
- Why: one wire on one pin, like a flight controller. Betaflight's soft serial (`src/main/drivers/serial_softserial.c`,
  a timer-driven bit-banged UART) is the precedent: it is the usual way to run SmartAudio/Tramp on a pad without a
  free UART, and its `vtx_softserial_alt` option exists for exactly this line's 0x00 quirk. Linux has no single-wire
  UART mode, but the GPIO character device v2 (https://docs.kernel.org/userspace-api/gpio/chardev.html,
  `<linux/gpio.h>`, kernel 5.10+) gives what a soft UART needs: a requested line can be switched between input and
  output without letting go of it (`GPIO_V2_LINE_SET_CONFIG_IOCTL`), and an input reports both edges with the
  kernel's timestamp taken in the interrupt handler (`CLOCK_MONOTONIC` by default; `EVENT_CLOCK_HTE` needs a hardware
  timestamp engine, which the Pi 4 doesn't have). The line is found on the chip labelled `pinctrl-bcm2711` (also
  `pinctrl-bcm2835`, `pinctrl-rp1`) at the BCM number, else by its line name `GPIO<n>`; never by gpiochip number.
- Bias while listening: **pull-down for SmartAudio** (the spec: both sides inputs when idle, the line low before a
  frame; Betaflight's `SERIAL_PULL_SMARTAUDIO`), **pull-up for Tramp** (Betaflight opens Tramp as plain half duplex,
  open-drain with a pull-up: idle high). The BCM2711's internal pulls are 33-73 kΩ. While listening, the Pi's pin is
  an input, so a replying VTX drives against nothing but that pull: less load than the two-pin mode's idle-high TX.
- Timing: a bit is 208.3 µs at 4800 baud (SmartAudio), 104.2 µs at 9600 (Tramp). A UART samples each bit in its
  middle, so the sender's edges may be off by much less than half a bit; with the VTX's own ±5 % clock, an edge more
  than **a quarter of a bit** late (52 µs / 26 µs) is treated as a bad frame and the frame goes again. Each edge is
  scheduled from the frame's start (no drift), slept for with `clock_nanosleep(TIMER_ABSTIME)` to 60 µs before, then
  waited for by spinning on `CLOCK_MONOTONIC`, and driven with `GPIO_V2_LINE_SET_VALUES_IOCTL`, from the VTX thread
  pinned to core 1 (the frame loop has core 3, the audio thread core 2, most interrupts core 0) and raised to
  `SCHED_FIFO` (priority 40) only for the frame. Right after the last stop bit the pin is an input again (the VTX
  answers within 100 ms; OpenVTx after 10 ms). How late the edges were is shown on the web page and logged.
- Receiving: user-space latency doesn't matter because the kernel timestamps each edge. The decoder takes a falling
  edge as a start bit (a start shorter than half a bit is a glitch), re-measures the bit time on the byte's clean
  edges (the ±5 % clock), samples each bit in its middle, and counts a low stop bit as a framing error (the byte stays:
  the CRC decides). When a SmartAudio VTX lets go after its reply the pulled-down line falls and stays low: that
  "byte" of zeros without a stop bit is dropped (it is the 0x00 the TBS captures show after replies).
- Tested (2026-10-17) through the real character device against `gpio-sim` (Debian's arm64 kernel has neither
  gpio-sim nor irq_sim built: both were built out of tree for the test VM): line lookup by name, the request with
  edge events and bias, both biases, a 10-byte reply decoded from a "VTX" driving the line, frames sent and sampled,
  back to input after each frame, a busy line. The emulated VM is 10-20× slower than a Pi (its edges went out
  ~0.8 ms late), so that ran at 50 baud; the real timing at 4800/9600 baud is for the Pi: `sticktime-pi --vtx-test
  gpio4` and `--vtx-loopback-test gpio4 gpio17` (pins 7 and 11 joined through 1 kΩ) print it.
- When one pin isn't good enough on some Pi, the two-pin UART mode above is the fallback (hardware timing).
