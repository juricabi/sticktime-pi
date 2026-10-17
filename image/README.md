# image/: the StickTime Pi SD card image

`image/build.sh` turns the official Raspberry Pi OS Lite (64-bit) image into the StickTime Pi image:
flash it, put the card in a Raspberry Pi, and it boots straight into the simulator (composite video by
default), with its settings in `sticktime.txt` on the card's boot partition.

## Build

On Linux, as root (a GitHub `ubuntu-24.04` runner, any Debian/Ubuntu with e2fsprogs 1.47 or newer,
or an arm64 machine; nothing runs inside the image, so the host's architecture doesn't matter):

```sh
sudo apt-get install dosfstools e2fsprogs fdisk xz-utils curl python3 zerofree
sudo BIN=build/aarch64-edgetx/sticktime-pi VER=0.1.0 image/build.sh
# -> out/sticktime-pi-0.1.0.img.xz, .img.xz.sha256, .info
```

Everything is optional; the main knobs (all in the header of `build.sh`):

| Variable | Default | What |
|---|---|---|
| `BIN` | a placeholder | the static aarch64 `sticktime-pi` (`host/build.sh aarch64 prog`). The placeholder only answers `/api/status`, so the image can be built and boot-tested before the binary exists |
| `VER`, `LABEL` | from the git tag | `VER` (digits and dots) goes into `sticktime/VERSION` for the updater; `LABEL` names the files |
| `BASE` | `BASE_DEFAULT` in `base.env` | which pinned Raspberry Pi OS image to start from |
| `GAME_ZIP` / `GAME_VERSION`, `GAME_SHA256` | the pin in `base.env` | the StickTime release (`StickTime-<ver>-sdcard.zip`) shipped on the card |
| `EDITOR_HTML`, `GALLERY_JSON` | downloaded | StickTime's `web/simulator.html` at that tag (patched by `tools/patch_editor.py`) and the track gallery |
| `OFFLINE=1` | | never download; take everything from the variables above or `CACHE` |
| `EXTRA_MB`, `XZ_LEVEL`, `KEEP_IMG` | 0, 6, 0 | grow the root partition; xz preset; keep the raw `.img` too |
| `OUT`, `CACHE`, `WORK` | `out/`, `build/image-cache/`, `out/work/` | `WORK` must be a Linux file system (loop devices) |

The base image is downloaded once into `CACHE` and checked against its sha256 every run. A run
starts from that file each time, so it can be repeated; a run that was killed leaves mounts or loop
devices behind, and the next run removes them first. Any failure unmounts and detaches everything
(`trap`). It takes about 6 minutes in the emulated arm64 build VM (xz -2), and a few minutes on a
GitHub runner.

## What the build changes

Host-side edits of the mounted image, no chroot, no packages (pios-image.md 6.2):

- **No first-boot questions**: user `pi` (uid 1000) gets `/bin/bash`, its password stays locked
  until `password =` in `sticktime.txt` sets it; `userconfig`, `sshswitch`, `rpi-eeprom-update`
  masked; `rename_user.conf` removed; default target `multi-user`.
- **cloud-init off** (`/etc/cloud/cloud-init.disabled`, seed files removed from the FAT, netplan
  generator masked). Flash with "No" to Imager's OS customisation; `sticktime.txt` does that job.
- **Less boot work**: apt/man-db/dpkg/e2scrub timers, cron and NetworkManager-wait-online off; no
  swap (`/etc/rpi/swap.conf.d`); CPU governor `performance` (udev rule, cmdline, tmpfiles mask).
- **Our files** from `image/rootfs/` (installed as they are, CRLF stripped, `#!` files 0755):
  helpers in `/usr/lib/sticktime/`, units in `/etc/systemd/system/`, NetworkManager without WiFi
  power save, an avahi `_http._tcp` record, a 48 MB journal cap, `/run/sticktime` via tmpfiles.
- **The FAT partition**: `sticktime.txt` (the template text `SETTINGS_TEMPLATE_TEXT` from
  `host/src/settings_keys.h`, extracted at build time so it can't drift from the host's), and
  `sticktime/` with `bin/sticktime-pi`, `sd/SCRIPTS/TOOLS/{StickTime.lua, StickTimeBW.lua, StickTimeBW/,
  StickTimeLite.lua, StickTimeLite/, StickTimeTracks/, StickTimeGhosts/, GAME_VERSION}` (all three
  editions from the release zip: the color one, and the B&W and Lite loaders with their folders' game files,
  never a save; an edition an older zip lacks, or whose loader is of another version, is left out with a
  warning; the web page's Game tab switches between them), `web/`, `editor/index.html` +
  `editor/tracks/index.json`, `fonts/` (Roboto + OFL.txt), `music/README.txt`, `VERSION`, `SHA256SUMS`.
- `config.txt` and `cmdline.txt` are generated from the default settings by the same code that runs
  at every boot (`sticktime-setup bake`), so a fresh card needs no reboot to apply them.

`machine-id` stays `uninitialized`, so the first boot still grows the root partition (initramfs
`resize` + `systemd-growfs-root`) and makes new ssh host keys.

## On the Pi

| Unit | Runs |
|---|---|
| `sticktime-setup.service` | every boot, before NetworkManager, avahi, ssh, bluetooth and the app: applies `sticktime.txt` (below) |
| `sticktime.service` | `/boot/firmware/sticktime/bin/sticktime-pi --base /boot/firmware/sticktime --settings /boot/firmware/sticktime.txt --libexec /usr/lib/sticktime --state /run/sticktime`; owns tty1 (getty@tty1 and autovt@tty1 masked), `Restart=always` with a backoff from 1 s to 30 s and no start limit, may use SCHED_FIFO and mlockall; `KillMode=process` so helpers it starts (an app update) outlive its restart |
| `sticktime-update-repair.service` | before the app: `sticktime-update repair` (finishes an update a power cut interrupted) |
| `sticktime-update.timer` → `.service` | 90 s after boot: `sticktime-update boot` (installs updates when `auto_update = on`); never periodically while flying |
| `sticktime-hotspot.service` | the hotspot policy (below) |
| `sticktime-bt.service` | `bluetooth = on`: pairs, trusts and connects "ELRS Joystick"/"ExpressLRS Joystick" and gamepads; quiet without an adapter; no scanning while the WiFi joystick streams (2.4 GHz air time) |
| `sticktime-ci.service` | only with `sticktime.ci=1` on the kernel command line (the QEMU test): prints `STICKTIME-CI: ready ...` |

### sticktime-setup

`/usr/lib/sticktime/sticktime-setup boot` (Python 3, stdlib) applies at every boot: hostname and
`/etc/hosts` (RFC 1123, else `sticktime`); the WiFi client profile `sticktime-wifi` (a NetworkManager
keyfile, 0600, 2.4 GHz, power save off; SSIDs with `;`, `#`, `=`, spaces, digits only or UTF-8 are
written so NM reads them back exactly; open networks; 64-hex PSKs; `wifi_hidden = on` for hidden
networks); the hotspot profile `sticktime-hotspot` (10.42.0.1/24, WPA2, channel 6); the WiFi country
(cmdline `cfg80211.ieee80211_regdom`, `iw reg set`, `WirelessEnabled=true`, rfkill unblock); the
Bluetooth radio on/off; ssh on/off (a `ssh` or `ssh.txt` file on the FAT means `ssh = on`, written
into `sticktime.txt`); the pi password (`chpasswd` over stdin, then cleared from `sticktime.txt`,
never logged); the CPU governor; the managed block at the end of `config.txt` (vc4 overlay with or
without `,composite`, `enable_tvout`, audio, overscan, boot-speed keys; the stock vc4/audio/detect
lines are commented out) and its own `cmdline.txt` tokens (quiet, `vc4.tv_norm`, the country; every
other token is kept as it is, one line, no CRLF).

- **Reboot once**: when the video part of `config.txt`/`cmdline.txt` changed, it reboots by itself,
  at most once for the same content and at most twice in a row without a clean boot in between
  (`/var/lib/sticktime/boot-config.json`). `sticktime.noreboot=1` on the kernel command line (or
  `STICKTIME_NOREBOOT=1`) turns the reboot off; the decision compares files, never `/proc/cmdline`,
  so QEMU's fixed `-append` can't loop.
- **A broken settings file**: missing or empty → the template is written; binary garbage, over 1 MB,
  or no keys at all → kept as `sticktime.txt.broken-<time>` and a fresh template written. CRLF, a
  BOM, Notepad's trailing spaces, quoted values with `#` are fine. One failing step never stops the
  others; the report is `/run/sticktime/setup.json`.
- `sticktime-setup apply [--json]` does the same while running and never reboots (it says
  `reboot_needed`); `bake --root DIR` writes only files (the image build).

### WiFi: sticktime-wifi and the hotspot

`sticktime-wifi` is run by the host for the web page: `scan` (JSON on stdout, cached in
`/run/sticktime/wifi-scan.json`; nmcli's `\:` escapes handled; in hotspot mode the networks seen
before the AP started), `apply [--hidden]` (rewrites the profile from `sticktime.txt`, connects in
the background, the hotspot comes back when that fails; ends a module-AP session),
`module-ap elrs|tbs [PASSWORD]` (joins the ELRS `ExpressLRS TX` / `expresslrs` or TBS
`tbs_crossfire_*` access point, module at 10.0.0.1 / 192.168.4.1, as a one-time connection).

**Hotspot policy** (`hotspot = auto`, `sticktime-hotspot run`): after boot, wait up to 45 s for the
network from `sticktime.txt` (none set: start the hotspot at once); not connected → the `StickTime`
access point (NM shared mode, 2.4 GHz, 10.42.0.1, DHCP). The Pi's WiFi chip can't scan or join while
it is an access point, so looking for the known network again means switching the AP off for up to
30 s: that happens only while **no station is connected** for a whole interval (2 min, doubling up
to 15 min). A phone or the ELRS module on the hotspot is never cut off. The web page's "join" tries
at once (SIGUSR1). A lost client connection (45 s) brings the hotspot back. `hotspot = on`: always
the AP; `off`: never.

## Testing

```sh
python3 image/test/test_setup.py          # unit tests, fake root + fake nmcli/systemctl/... (Linux, Windows)
python3 image/test/boot_test.py out/sticktime-pi-<ver>.img.xz [--quick]
```

`boot_test.py` boots the image under QEMU `-M raspi3b` (the Pi 4 machine has no USB or network in
QEMU) with the image's own kernel, initramfs and Pi 3 B+ dtb, exactly as pios-image.md section 7
says: power-of-2 SD card, PL011 freed from Bluetooth in the dtb (pure-Python FDT editor),
`watchdog_disarm=0 systemd.watchdog_sec=off` (QEMU resets at once when the watchdog is opened),
usb-net with ssh and http forwarded, the serial console on a socket, the monitor for a screenshot.
It writes a CI `sticktime.txt` (ssh on, a password) into the FAT with its own FAT32 code (no mtools
needed, works on Windows), runs the checks as `pi` on the serial console, and goes through four
boots: first boot, a settings change that makes setup reboot once, the boot after it (no second
reboot), and ssh turned off. QEMU for Windows works the same as qemu-system-aarch64 on Linux.

Under QEMU there is no DRM device, no WiFi and no Bluetooth chip; the image must (and does) boot
without failed units anyway. The program then draws on QEMU's framebuffer (`fb0`, 800x480), so the
screenshots (`<work>/screen-first-boot.png`, `screen-boot-4.png`) show the StickTime menu.
Composite video, WiFi and Bluetooth need a real Raspberry Pi.

Last run (2026-10-17, `sticktime-pi-0.1.0.img.xz` with the real program): 50/50 checks in 8 minutes
on Windows; boot 1 ready after 3 minutes (the first boot grows the root partition), later boots after
about 70 seconds.

## Changing the base image

`base.env` pins each base by URL, sha256 and size. Before making another one the default, check on
the new image (the build fails loudly on most of these): user `pi` is uid 1000 with
`/usr/sbin/nologin`; `userconfig.service` and cloud-init exist; `/etc/machine-id` is
`uninitialized`; `cmdline.txt` has ` resize` and the initramfs scripts `resize_early`/`set_partuuid`;
the partition table is FAT + ext4 (offsets are read from it, never assumed); e2fsprogs on the build
host can handle its ext4 features; and composite video on a real Pi 4 with that kernel.
