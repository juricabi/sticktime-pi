# StickTime Pi

StickTime Pi turns a Raspberry Pi 4 into a "virtual quad". It runs the StickTime FPV simulator
(EdgeTX Lua tool, https://github.com/juricabi/sticktime) natively and sends the picture to analog FPV
goggles through a 5.8 GHz VTX wired to the Pi's composite (AV) output, or to an HDMI screen.
The sticks come from the pilot's own radio: ELRS/Crossfire WiFi joystick, USB joystick or Bluetooth.

The owner (juricabi) also wrote StickTime and https://github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Windows.
The repo is open source (GPL-2.0): keep it clean, no secrets or personal paths in git. Your own notes
go in `.local/` (ignored).

## Read first

- `docs/PLAN.md`: requirements, architecture, settings, image, updates, milestones. **Start here.**
- `docs/RESEARCH.md`: verified facts with sources (composite config, protocols, axis maps, EdgeTX API, Lua flags).
- `docs/DEVELOPMENT.md`: toolchains, every test suite, the image build and the QEMU boot test.
- `docs/WEB_API.md`: the web API contract. `docs/USER_GUIDE.md`: for end users.

Keep the sibling repos checked out next to this one. The plan points into them:
`../sticktime` (game, `web/src/engine.js` = the reference to port),
`../ELRS-Crossfire-WiFi-Joystick-Windows` (`JoystickEngine.cs` = the WiFi protocol's reference behaviour) and
`../elrs-crossfire-wifi-joystick-linux` (`core/` = the WiFi joystick protocol core, shared with the owner's
Linux app and vendored unmodified in `host/third_party/elrs-wifi/`: change it there, then
`tools/vendor_elrs_wifi.sh`; CI checks the copy with `--check`).

## Owner's requirements

1. Flashable SD image for a Raspberry Pi 4, lightweight: only what this needs.
2. Video: AV/composite to a VTX (NTSC/PAL, 4:3/16:9) or HDMI, selectable.
3. WiFi setup, plus a web page with WiFi, joystick and other settings.
4. Inputs: ELRS/Crossfire WiFi joystick (same protocol as the owner's Windows app), USB, Bluetooth.
5. **Low latency** everywhere.
6. Updatable to new versions (game and app) without reflashing.
7. Tracks like the web emulator: custom tracks c1–c8, the track editor, the gallery.

## Key decisions (see PLAN.md for why)

- Run the **unchanged** `StickTime.lua` in a native C host: a C port of the color path of
  `sticktime/web/src/engine.js` (EdgeTX API + firmware-faithful RGB565 drawing). Don't rewrite the game.
- Lua 5.3.6 with EdgeTX's number settings: `-DLUA_32BITS -DLUA_FLOORN2I=1 -DLUA_COMPAT_5_2`.
- Output straight to DRM/KMS (dumb buffers + page flip); input straight from evdev and UDP.
  No X, Wayland, SDL or browser on the device.
- Static aarch64 binary, so updates are one file and it runs on any Pi OS version.
- Image: Raspberry Pi OS Lite 64-bit, customized in GitHub Actions.
- Everything that changes (binary, game, web UI, settings, tracks, saves) lives on the FAT boot
  partition under `/boot/firmware/sticktime/`. Updates are folder swaps, and the user can edit from a PC.
- The system scripts (`/usr/lib/sticktime`) travel in app updates as `lib/` and are synced from there
  (`stupdate/system.py`). Put system changes in those scripts, not in image-only steps of `image/build.sh`,
  or they only reach people who reflash.

## Layout

```
host/         native host (C)
  src/        main, host, scenario, etx, lcd, font, video_drm/fb/png, input_evdev/wifi, nav, web, web_api,
              settings, audio, sfx, dsp, music, songs, sys, plat, app
  tests/      unit tests (test_*.c), linux/ (Linux-only), scenarios/, golden/, test drivers (*.py)
  third_party/          Lua 5.3.6, stb, minimp3, dr_libs, elrs-wifi = the WiFi joystick core (all unmodified, see its README)
  fonts/      Roboto Regular/Bold TTF (OFL), converted from sticktime/web/vendor/*.woff2
web/          the on-device web page + tests/
image/        image build (build.sh, base.env), rootfs/ (units, /usr/lib/sticktime helpers, updater), tests
tools/        patch_editor.py, compare_emulator.py, make_app_package.py, vendor_elrs_wifi.sh
.github/      CI workflows, issue templates
docs/         PLAN, RESEARCH, DEVELOPMENT, WEB_API, USER_GUIDE
```

## Conventions

- C (gnu99), `-Wall -Wextra` clean. No allocation in the steady-state frame loop.
- Draw exactly like `engine.js`; when in doubt, port it literally (the game is tested against it).
- Never block the frame loop. Network POSTs, updates and big writes go to a thread or helper process.
- Test headless first (PNG frame dumps, frame-time stats), then on the Pi over SSH.
- File writes on the FAT partition: temp file + fsync + rename.
