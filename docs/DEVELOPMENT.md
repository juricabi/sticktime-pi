# Developing StickTime Pi

How to build StickTime Pi, run every test suite, build the SD image and boot-test it. The design is in
[PLAN.md](PLAN.md), verified facts with sources in [RESEARCH.md](RESEARCH.md), the web API contract in
[WEB_API.md](WEB_API.md). Project rules are in [CLAUDE.md](../CLAUDE.md) (read it whether or not you use
an AI assistant).

## Checkouts

```
sticktime/        https://github.com/juricabi/sticktime   (the game; web/src/engine.js is the reference)
sticktime-pi/     this repo
```
Keep `sticktime` next to this repo: the tests run its `StickTime.lua` and compare against its emulator.

## Tools

| What | Needed for |
|---|---|
| **zig 0.15** (`zig cc`), or gcc/clang on Linux | everything in C; zig also cross-compiles the static aarch64 program from any OS |
| a POSIX shell (Linux, macOS, Git Bash on Windows) | `host/build.sh` |
| **Python 3.11+** | test drivers, the image helpers and the updater (the Pi runs Pi OS's Python 3.13) |
| Playwright + Chromium (`pip install playwright && playwright install chromium`) | web UI tests, `tools/compare_emulator.py` |
| Linux with root, `dosfstools e2fsprogs fdisk xz-utils curl python3 zerofree` | building the SD image |
| **QEMU** with `qemu-system-aarch64` (machine `raspi3b`) | the image boot test |
| an arm64 Linux machine (a Pi, an arm64 VM, a cloud runner) | running the Pi build's tests and the Linux-only tests |
| node + fengari | only to regenerate the drawing references (`host/tests/golden/README.md`) |

`host/build.sh` uses `zig cc` when zig is on the PATH, else `cc`; set `CC` to override.

## Build and unit tests

```sh
host/build.sh native test      # the program + unit tests for this machine, then runs them (exit code = result)
host/build.sh aarch64          # static Pi program + test binaries: build/aarch64-edgetx/
host/build.sh native clean     # remove build/native-edgetx/
NUMS=ref host/build.sh native  # Lua with double floats, like the browser (for comparisons with engine.js)
```
Unit tests are `host/tests/test_*.c` (drawing, fonts, the EdgeTX Lua API, settings, input, menu
navigation, video, web server, WiFi protocol, audio, motor sound, music). `test_lcd_golden` replays
recorded drawing calls and must match `engine.js` byte for byte; it skips when its references (in
`build/golden/`, made by `host/tests/golden/ref.js`) are missing.

Run the Pi build's tests on arm64 Linux the same way: copy `build/aarch64-edgetx/` and the repo, and
run each `test_*` from the repo root.

## Integration tests

```sh
python host/tests/run_scenarios.py                 # the real game, headless, scripted (--quick skips long_run, --only NAME)
python host/tests/web_api_test.py --exe build/native-edgetx/web_test_main      # the web API, real C server
python web/tests/test_real_server.py --exe=build/native-edgetx/web_test_main   # web page + editor in Chromium
python tools/compare_emulator.py                   # same scenes in the browser emulator and in StickTime Pi
python image/test/test_setup.py                    # boot-time setup helpers (settings, WiFi, config.txt, ...), the build's game step
python image/tests/test_update.py                  # the updater against a fake GitHub (~4 min)
python host/tests/check_settings_keys.py           # settings.py and settings_keys.h describe the same keys
python host/tests/vtx_live_test.py                 # VTX control: the real program + the fake VTX (--pty on Linux: termios)
```
(add `.exe` to the binaries on Windows.) Scenarios are Lua scripts in `host/tests/scenarios/`; the
scenario API is documented at the top of `host/src/scenario.c`, screenshots go to
`build/scenario-shots/`. `host/tests/fake_vtx.py` is a fake SmartAudio 1.0/2.0/2.1 and Tramp VTX (with the
one-wire echo and clone quirks) on TCP or a pty; the program takes it with `--vtx-port tcp:127.0.0.1:PORT` (or
`tty:/dev/pts/N`), `web_test_main` with `STP_VTX_PORT`, and `web_api_test.py` and `web/tests/test_real_server.py`
start one themselves. `compare_emulator.py` writes `build/compare/<size>/*_side.png`: look at the
pictures, not only the numbers (text antialiasing and single-pixel 3D edges differ a little).

### The program headless

```sh
build/native-edgetx/sticktime-pi --video none --no-inputs --no-audio --port 8099 \
    --base BASE --settings TMP/sticktime.txt --state TMP/state --fonts host/fonts
```
`BASE` holds `sd/` (a copy of `../sticktime/sdcard`), `web/` (this repo's `web/` without `tests/`) and,
for the editor, `editor/index.html` (`python tools/patch_editor.py ../sticktime/web/simulator.html BASE/editor/index.html`).
`--video png --png-dir DIR` writes frames as PNG. Mutating API requests need the header `X-StickTime: 1`.

### Linux-only tests (root, arm64 or x86_64 Linux)

- `host/tests/linux/test_evdev_uinput.c`: input devices made with uinput. `host/tests/linux/run_uinput.sh root@HOST`
  builds it static for aarch64 and runs it over ssh.
- `host/tests/linux/test_audio_alsa.c`: the ALSA output, best on `modprobe snd-aloop` (checks the samples
  that come back, underrun recovery, hotplug with `--hotplug`).
- `host/tests/linux/test_vtx_gpio.c`: the single-pin VTX line through the GPIO character device against `gpio-sim`
  (root; Debian's kernel lacks gpio-sim and irq_sim: build `drivers/gpio/gpio-sim.c` + `kernel/irq/irq_sim.c` out of tree;
  an emulated VM needs `--baud 50`). On a real Pi: `sticktime-pi --vtx-test gpio4` (with a VTX) and
  `sticktime-pi --vtx-loopback-test gpio4 gpio17` (pins 7 and 11 joined through 1 kΩ) print the line's timing.
- `host/tests/linux/test_video_drm.c`: the DRM/KMS and fbdev backends on a real KMS device (vc4 on a Pi) or
  vkms in a VM (`--recovery`, `--vt`, `--sys`; `--strict-timing` only on real hardware).

Build them like the unit tests, linked with the objects from `build/<target>/obj/` (see each file's header).

## The SD image

On Linux as root (details and all options: [image/README.md](../image/README.md)):
```sh
VERSION=0.1.0 host/build.sh aarch64 prog            # the version compiled in must match VER
sudo BIN=build/aarch64-edgetx/sticktime-pi VER=0.1.0 image/build.sh
# -> out/sticktime-pi-0.1.0.img.xz, .sha256, .info
```
Nothing runs inside the image, so the build host's architecture doesn't matter. On Windows, use an arm64
or x86_64 Linux VM (copy the repo with `git -c core.autocrlf=false archive HEAD`: CRLF line ends break
the shell scripts).

### Boot test (QEMU)

```sh
python -u image/test/boot_test.py out/sticktime-pi-0.1.0.img.xz --work build/boot     # ~8 min, 4 boots
```
It boots the image under QEMU `-M raspi3b` (QEMU's Pi 4 machine has no USB or network) four times: the
first boot with its one reboot, settings changes, ssh on and off. It checks the services, the web page,
the settings file and takes screenshots (`build/boot/screen-*.png` must show the StickTime menu). QEMU
is found via `--qemu`, `$QEMU` (Windows), or the PATH. When QEMU's emulated SD controller times out
during the first boot (seen with a virus scanner reading the image file), the test starts over with
`-icount shift=auto`.

QEMU can't test composite video, HVS scaling, the WiFi and Bluetooth chips, real audio or real latency:
test those on a Pi 4.

### Tips for an emulated arm64 VM (QEMU on Windows or x86 Linux)

- It is 10-20 times slower than a real core: run independent test parts in parallel.
- With `-machine virt`, keep 8 vCPUs or fewer: with more, QEMU switches to GICv3 and some UEFI builds hang.
- Debian's arm64 kernel has no vkms: build it out of tree from the kernel sources for the DRM test, and
  don't `rmmod vkms` after `/dev/fb0` was mmapped (it hangs in the kernel; a VM issue, not a Pi issue).

## CI

- `.github/workflows/build.yml` (push, pull request): the host on x86_64 with unit tests and scenarios,
  the static aarch64 program, the Python tests on 3.11 and 3.13, the web UI in headless Chromium.
- `.github/workflows/image.yml` (tags `v*`, manual): the app package for the updater, the SD image and its
  QEMU boot test, and on tags a GitHub release with all of it. The release notes: `docs/releases/<tag>.md`
  (what's new, how to update) when the tagged commit has it, then the install text and the image's info.
  Run by hand with a tag (`v0.2.4`), it publishes that release from the branch it runs on; the release
  makes the tag at the run's commit (a tag that exists must already be that commit).

## Layout

```
host/src/        the C program: main.c (CLI, frame loop), host.c (frame step, error screen, reloads, overlay,
                 audio feed), scenario.c (headless test API), etx.c (EdgeTX Lua API), lcd.c/font.c (drawing),
                 video*.c (drm/fb/png), input*.c, nav.c, wifi_proto.c, web.c/web_api.c, settings.c,
                 audio.c/sfx.c/dsp.c/music*.c/songs.c, sys.c, plat.c, app.c
host/tests/      unit tests (test_*.c), linux/, scenarios/, golden/, test drivers (*.py)
host/third_party/  Lua 5.3.6, stb, minimp3, dr_libs (unmodified, see its README)
host/fonts/      Roboto (OFL)
web/             the Pi's web page (index.html, app.js, style.css, pi-sd.js) and its tests/
image/           build.sh, base.env (pinned inputs), rootfs/ (systemd units, /usr/lib/sticktime helpers,
                 the updater stupdate/), test/ (setup and boot tests), tests/ (updater tests). The helpers
                 also go into every app package as lib/ (tools/make_app_package.py lib_files) and reach
                 running Pis through the updater's sync (stupdate/system.py); systemd units and other
                 files outside /usr/lib/sticktime still need a new image
tools/           patch_editor.py (makes the emulator page work on the Pi), compare_emulator.py
build/, out/     build output and images (ignored by git)
```

## Conventions

- C is gnu99 and `-Wall -Wextra` clean. No allocation in the steady-state frame loop; never block it
  (network, updates and big writes go to a thread or a helper process).
- Draw exactly like `engine.js`; when in doubt, port it literally. `StickTime.lua` itself is never changed.
- Writes on the FAT partition: temp file, fsync, rename.
- Paths may contain spaces: quote them in every script.
