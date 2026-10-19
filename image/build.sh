#!/usr/bin/env bash
# StickTime Pi: build the flashable SD card image from Raspberry Pi OS Lite (64-bit).
#
#   sudo [NAME=value ...] image/build.sh
#
# Runs on Linux as root: a GitHub ubuntu-24.04 runner (with sudo) or the arm64 build VM. It edits the
# official image from the outside: loop-mount, change files and unit links, copy the payload onto
# the FAT partition, unmount, compress. Nothing runs inside the image (no chroot, no qemu-user), so
# it works the same on x86_64 and arm64 hosts. Safe to run again: every run starts from the cached,
# sha256-checked base image, and leftovers of a killed run (mounts, loop devices) are cleaned first.
#
# Inputs (environment):
#   BASE=2026-04-21        base image id from image/base.env (default: BASE_DEFAULT there)
#   BASE_XZ=FILE           a local copy of that base .img.xz (still checked against its sha256)
#   VER=0.1.0              app version for sticktime/VERSION (digits and dots; default: the git tag
#                          vX.Y.Z at HEAD, else the last tag, else 0.0.0)
#   LABEL=...              the name part of the output files (default: VER, plus -dev.<sha> off a tag)
#   BIN=FILE               the static aarch64 sticktime-pi (default: a placeholder that only answers
#                          /api/status, so the image can be built and boot-tested without it)
#   GPU_BIN=FILE           the GPU drawing helper sticktime-gpu (aarch64, glibc), put next to it (optional:
#                          without it drawing = gpu draws on the CPU)
#   GAME_ZIP=FILE          StickTime-<ver>-sdcard.zip (default: download GAME_VERSION's release asset)
#   GAME_VERSION=1.7.2     the StickTime release to ship (default: GAME_VERSION_DEFAULT in base.env)
#   GAME_SHA256=hex        the zip's sha256 (default: the pin in base.env for the default version)
#   EDITOR_HTML=FILE       StickTime's web/simulator.html of that version (default: download at the tag)
#   GALLERY_JSON=FILE      the track gallery index.json (default: download; optional)
#   ALLOW_NO_EDITOR=1      build without editor/ when the page can't be fetched or patched
#   EXTRA_MB=0             grow the root partition by this much (only needed for package installs)
#   XZ_LEVEL=6             xz preset (0-9); KEEP_IMG=1 also keeps the raw .img
#   OUT=out  CACHE=build/image-cache  WORK=$OUT/work      folders (WORK must not be on a FAT/NTFS mount)
#   OFFLINE=1              never download: everything must be given or already in CACHE
#
# Output: $OUT/sticktime-pi-$LABEL.img.xz, .img.xz.sha256 (sha256sum format) and .info (what went in).
set -Eeuo pipefail
umask 022

die() { echo "build.sh: error: $*" >&2; exit 1; }
say() { echo "== $*"; }
warn() { echo "build.sh: warning: $*" >&2; }

[ "$(id -u)" = 0 ] || die "run as root (sudo image/build.sh): it loop-mounts the image"
[ "$(uname -s)" = Linux ] || die "Linux only (loop devices, ext4, vfat)"

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMG_DIR=$REPO/image
LIB_SRC=$IMG_DIR/rootfs/usr/lib/sticktime
# shellcheck source=base.env
. "$IMG_DIR/base.env"

BASE=${BASE:-$BASE_DEFAULT}
BASE_ID=${BASE//-/_}
eval "BASE_URL=\${BASE_${BASE_ID}_URL:-}"
eval "BASE_SHA256=\${BASE_${BASE_ID}_SHA256:-}"
eval "BASE_SIZE=\${BASE_${BASE_ID}_SIZE:-}"
eval "BASE_KERNEL=\${BASE_${BASE_ID}_KERNEL:-?}"
[ -n "$BASE_URL" ] && [ -n "$BASE_SHA256" ] || die "BASE=$BASE is not in image/base.env"

OUT=${OUT:-$REPO/out}
CACHE=${CACHE:-$REPO/build/image-cache}     # build/ and out/ are in .gitignore
WORK=${WORK:-$OUT/work}
XZ_LEVEL=${XZ_LEVEL:-6}
EXTRA_MB=${EXTRA_MB:-0}
OFFLINE=${OFFLINE:-0}
mkdir -p "$OUT" "$CACHE" "$WORK"
OUT=$(cd "$OUT" && pwd); CACHE=$(cd "$CACHE" && pwd); WORK=$(cd "$WORK" && pwd)
[[ "$XZ_LEVEL" =~ ^[0-9]e?$ ]] || die "XZ_LEVEL must be 0-9"
[[ "$EXTRA_MB" =~ ^[0-9]+$ ]] || die "EXTRA_MB must be a number of MiB"

need() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 || die "missing tool: $c (apt-get install $(pkg_of "$c"))"; done; }
pkg_of() { case $1 in fsck.vfat) echo dosfstools;; e2fsck|resize2fs) echo e2fsprogs;; sfdisk|losetup|blkid) echo fdisk util-linux;;
  xz) echo xz-utils;; python3) echo python3;; *) echo "$1";; esac; }
need losetup mount umount blkid xz sha256sum python3 fsck.vfat e2fsck sed awk find install
python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' || die "python3 3.11 or newer is needed"
# the image's ext4 has orphan_file: e2fsprogs older than 1.47 can't check it (Ubuntu 22.04)
E2V=$(e2fsck -V 2>&1 | awk 'NR==1{print $2}')
python3 - "$E2V" <<'PY' || die "e2fsprogs $E2V is too old: 1.47 or newer is needed (orphan_file); use ubuntu-24.04"
import re, sys
m = re.match(r"(\d+)\.(\d+)", sys.argv[1])
sys.exit(0 if m and (int(m.group(1)), int(m.group(2))) >= (1, 47) else 1)
PY

# ---- version -------------------------------------------------------------------------------------
git_tag_ver() { git -C "$REPO" describe --tags --exact-match --match 'v[0-9]*' HEAD 2>/dev/null | sed 's/^v//'; }
if [ -z "${VER:-}" ]; then
  VER=$(git_tag_ver || true)
  if [ -z "$VER" ]; then
    VER=$(git -C "$REPO" describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null | sed 's/^v//' || true)
    VER=${VER:-0.0.0}
    SHA=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || true)
    LABEL=${LABEL:-$VER-dev${SHA:+.$SHA}}
  fi
fi
VER=${VER#v}
[[ "$VER" =~ ^[0-9]+(\.[0-9]+){1,3}$ ]] || die "VER=$VER: use digits and dots (1.2.3); put other words in LABEL"
LABEL=${LABEL:-$VER}
[[ "$LABEL" =~ ^[A-Za-z0-9._+-]+$ ]] || die "LABEL=$LABEL: letters, digits and . _ + - only"
NAME=sticktime-pi-$LABEL
say "StickTime Pi $VER ($NAME), base $BASE (kernel $BASE_KERNEL)"

# ---- cleanup of this run and of a killed earlier one --------------------------------------------
MNT=$WORK/mnt
LOOP=
cleanup() {
  local rc=$?
  set +e
  if mountpoint -q "$MNT/boot/firmware" 2>/dev/null; then umount "$MNT/boot/firmware" || umount -l "$MNT/boot/firmware"; fi
  if mountpoint -q "$MNT" 2>/dev/null; then umount -R "$MNT" || umount -lR "$MNT"; fi
  [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null
  # any other loop device still on our work image (a run killed with -9)
  if [ -e "$WORK/work.img" ]; then
    losetup --list -n -O NAME -j "$WORK/work.img" 2>/dev/null | while read -r d; do losetup -d "$d"; done
  fi
  rm -f "$OUT/$NAME.img.xz.part"
  [ $rc -ne 0 ] && echo "build.sh: FAILED (exit $rc); the work files are in $WORK" >&2
  return $rc
}
trap cleanup EXIT
trap 'exit 130' INT TERM
cleanup_old() {
  set +e
  if mountpoint -q "$MNT/boot/firmware" 2>/dev/null; then umount "$MNT/boot/firmware"; fi
  if mountpoint -q "$MNT" 2>/dev/null; then umount -R "$MNT"; fi
  if [ -e "$WORK/work.img" ]; then
    losetup --list -n -O NAME -j "$WORK/work.img" 2>/dev/null | while read -r d; do warn "detaching $d (left over)"; losetup -d "$d"; done
  fi
  set -e
}
cleanup_old

# ---- downloads (cached, checked) ------------------------------------------------------------------
fetch() {   # fetch URL DEST [SHA256]: download once into the cache; checks the sha256 when given
  local url=$1 dest=$2 sum=${3:-}
  if [ -s "$dest" ] && { [ -z "$sum" ] || echo "$sum  $dest" | sha256sum -c --status -; }; then return 0; fi
  [ "$OFFLINE" = 1 ] && die "OFFLINE=1 and $(basename "$dest") is not in $CACHE"
  need curl
  say "downloading $url"
  curl -fL --retry 5 --retry-delay 5 --connect-timeout 30 -o "$dest.part" "$url" || { rm -f "$dest.part"; return 1; }
  if [ -n "$sum" ] && ! echo "$sum  $dest.part" | sha256sum -c --status -; then
    rm -f "$dest.part"; die "sha256 mismatch for $url"
  fi
  mv -f "$dest.part" "$dest"
}

BASE_FILE=$CACHE/$(basename "$BASE_URL")
if [ -n "${BASE_XZ:-}" ]; then
  [ -f "$BASE_XZ" ] || die "BASE_XZ=$BASE_XZ not found"
  BASE_FILE=$BASE_XZ
  say "checking $BASE_FILE"
  echo "$BASE_SHA256  $BASE_FILE" | sha256sum -c --status - || die "BASE_XZ does not match BASE=$BASE's sha256"
else
  fetch "$BASE_URL" "$BASE_FILE" "$BASE_SHA256" || die "cannot download the base image"
fi
if [ -n "$BASE_SIZE" ] && [ "$(stat -c %s "$BASE_FILE")" != "$BASE_SIZE" ]; then die "$BASE_FILE has the wrong size"; fi

# the game (StickTime release), its emulator page and the track gallery
if [ -n "${GAME_ZIP:-}" ]; then
  [ -f "$GAME_ZIP" ] || die "GAME_ZIP=$GAME_ZIP not found"
  [ -n "${GAME_SHA256:-}" ] && { echo "$GAME_SHA256  $GAME_ZIP" | sha256sum -c --status - || die "GAME_ZIP does not match GAME_SHA256"; }
else
  GAME_VERSION=${GAME_VERSION:-$GAME_VERSION_DEFAULT}
  [[ "$GAME_VERSION" =~ ^[0-9]+(\.[0-9]+){1,3}$ ]] || die "GAME_VERSION=$GAME_VERSION is not a version"
  if [ -z "${GAME_SHA256:-}" ] && [ "$GAME_VERSION" = "$GAME_VERSION_DEFAULT" ]; then GAME_SHA256=$GAME_SHA256_DEFAULT; fi
  mkdir -p "$CACHE/game-$GAME_VERSION"
  GAME_ZIP=$CACHE/game-$GAME_VERSION/StickTime-$GAME_VERSION-sdcard.zip
  fetch "https://github.com/$GAME_REPO/releases/download/v$GAME_VERSION/StickTime-$GAME_VERSION-sdcard.zip" \
    "$GAME_ZIP" "${GAME_SHA256:-}" || die "cannot download StickTime $GAME_VERSION"
fi

PAY=$WORK/payload
rm -rf "$PAY"
mkdir -p "$PAY/sticktime"
P=$PAY/sticktime

say "game: $(basename "$GAME_ZIP")"
# the zip's three editions, as the emulator's Install and the updater put them on a card: the color
# SCRIPTS/TOOLS/StickTime.lua, and the B&W and Lite ones (StickTimeBW.lua + StickTimeBW/,
# StickTimeLite.lua + StickTimeLite/: on a color screen they run their core.lua in B&W colors, the
# web page's Game tab picks one); the tracks' and ghosts' folders with their READMEs. Never a save
# (*.dat, StickTimeLite/data.txt) or a track. The version comes from StickTime.lua's header, which
# must match GAME_VERSION when that was given; an edition whose loader says another version, or that
# an older zip doesn't have, is left out with a warning.
GAME_VERSION=$(python3 -I - "$GAME_ZIP" "$P/sd/SCRIPTS/TOOLS" "${GAME_VERSION:-}" <<'PY'
import os, re, sys, zipfile
src, dst, want = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    z = zipfile.ZipFile(src)
    bad = z.testzip()
except (zipfile.BadZipFile, OSError) as e:
    sys.exit(f"{src}: not a zip: {e}")
if bad:
    sys.exit(f"{src}: damaged member {bad}")
names = {i.filename: i for i in z.infolist()}
lua = names.get("SCRIPTS/TOOLS/StickTime.lua")
if lua is None or lua.file_size < 10000 or lua.file_size > 4 << 20:
    sys.exit(f"{src}: no usable SCRIPTS/TOOLS/StickTime.lua")
data = z.read(lua)
m = re.search(rb"StickTime v(\d+(?:\.\d+){1,3})", data[:600])
if not m:
    sys.exit(f"{src}: StickTime.lua has no version in its header")
ver = m.group(1).decode()
def key(v):                                   # 1.6 == 1.6.0, as the updater compares
    t = [int(x) for x in v.split(".")]
    while len(t) > 1 and t[-1] == 0:
        t.pop()
    return t
if want and key(ver) != key(want):
    sys.exit(f"{src}: the script says v{ver}, not {want}")
os.makedirs(os.path.join(dst, "StickTimeTracks"), exist_ok=True)
os.makedirs(os.path.join(dst, "StickTimeGhosts"), exist_ok=True)
with open(os.path.join(dst, "StickTime.lua"), "wb") as f:
    f.write(data)
# the B&W and Lite editions: the loader and the game's files in its folder (the names the emulator's
# Install and the updater accept: [A-Za-z0-9_-]+.lua|luac|txt, never the player's data.txt)
KEEP = re.compile(r"(?i)c\d+\.txt|.*\.dat|data\.txt")
for label, loader, folder in (("B&W", "StickTimeBW.lua", "StickTimeBW"), ("Lite", "StickTimeLite.lua", "StickTimeLite")):
    li = names.get("SCRIPTS/TOOLS/" + loader)
    if li is None or li.is_dir():
        print(f"build.sh: warning: {src} has no {loader}: no {label} edition on the card", file=sys.stderr)
        continue
    ld = z.read(li)
    lm = re.search(rb"StickTime(?: BW| Lite)? v(\d+(?:\.\d+){1,3})", ld[:600])
    if not lm or key(lm.group(1).decode()) != key(ver):
        print(f"build.sh: warning: {loader} isn't StickTime {ver}'s: no {label} edition on the card", file=sys.stderr)
        continue
    pre = "SCRIPTS/TOOLS/" + folder + "/"
    files = {}
    for n, i in names.items():
        rest = n[len(pre):]
        if not n.startswith(pre) or not rest or i.is_dir():
            continue
        if "/" in rest or not re.fullmatch(r"[A-Za-z0-9_-]+\.(lua|luac|txt)", rest) or KEEP.fullmatch(rest) or i.file_size > 1 << 20:
            print(f"build.sh: warning: {n} left out (not one of the game's files)", file=sys.stderr)
            continue
        files[rest] = z.read(i)
    if not files:
        print(f"build.sh: warning: {src} has no {folder}/: no {label} edition on the card", file=sys.stderr)
        continue
    os.makedirs(os.path.join(dst, folder), exist_ok=True)
    with open(os.path.join(dst, loader), "wb") as f:
        f.write(ld)
    for rest, b in sorted(files.items()):
        with open(os.path.join(dst, folder, rest), "wb") as f:
            f.write(b)
    print(f"== {label} edition: {loader} + {folder}/ ({len(files)} files)", file=sys.stderr)
for n, i in names.items():
    for d in ("StickTimeTracks/", "StickTimeGhosts/"):
        pre = "SCRIPTS/TOOLS/" + d
        rest = n[len(pre):]
        if n.startswith(pre) and rest and not i.is_dir() and "/" not in rest and re.fullmatch(r"[A-Za-z0-9_.-]+", rest) \
                and not re.fullmatch(r"(?i)c\d+\.txt|.*\.dat", rest):
            with open(os.path.join(dst, d, rest), "wb") as f:   # READMEs; never tracks or saves
                f.write(z.read(i))
with open(os.path.join(dst, "GAME_VERSION"), "w", newline="\n") as f:
    f.write(ver + "\n")
print(ver)
PY
) || die "the StickTime zip is not usable"
say "game version $GAME_VERSION"

GDIR=$CACHE/game-$GAME_VERSION
mkdir -p "$GDIR"
if [ -z "${EDITOR_HTML:-}" ]; then
  EDITOR_HTML=$GDIR/simulator.html
  fetch "https://raw.githubusercontent.com/$GAME_REPO/v$GAME_VERSION/web/simulator.html" "$EDITOR_HTML" \
    || { rm -f "$EDITOR_HTML"; EDITOR_HTML=; }
fi
mkdir -p "$P/editor"
if [ -n "$EDITOR_HTML" ] && [ -f "$EDITOR_HTML" ] && [ -f "$REPO/tools/patch_editor.py" ] \
    && python3 -I "$REPO/tools/patch_editor.py" "$EDITOR_HTML" "$P/editor/index.html"; then
  grep -q 'pi-sd.js' "$P/editor/index.html" || die "the patched editor page has no pi-sd.js"
  say "editor: $(stat -c %s "$P/editor/index.html") bytes"
else
  rm -f "$P/editor/index.html"
  [ "${ALLOW_NO_EDITOR:-0}" = 1 ] || die "no track editor page (EDITOR_HTML / tools/patch_editor.py); ALLOW_NO_EDITOR=1 builds without it"
  warn "building without the track editor page"
fi
if [ -z "${GALLERY_JSON:-}" ]; then
  GALLERY_JSON=$GDIR/index.json
  # the gallery changes without releases: fetch it fresh each build, fall back to a cached copy
  rm -f "$GALLERY_JSON.new"
  if [ "$OFFLINE" != 1 ] && command -v curl >/dev/null && curl -fsSL --retry 3 --connect-timeout 20 -o "$GALLERY_JSON.new" "$GALLERY_URL"; then
    mv -f "$GALLERY_JSON.new" "$GALLERY_JSON"
  else
    rm -f "$GALLERY_JSON.new"
  fi
fi
if [ -f "$GALLERY_JSON" ] && python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1],encoding="utf-8")); sys.exit(not (isinstance(d,dict) and isinstance(d.get("tracks"),list) and d["tracks"]))' "$GALLERY_JSON"; then
  mkdir -p "$P/editor/tracks"
  cp "$GALLERY_JSON" "$P/editor/tracks/index.json"
  say "gallery: $(python3 -I -c 'import json,sys; print(len(json.load(open(sys.argv[1],encoding="utf-8"))["tracks"]))' "$GALLERY_JSON") tracks"
else
  warn "no track gallery (the editor page then shows its built-in one)"
fi

# the app: binary (or the placeholder), web UI, fonts, music folder, VERSION, SHA256SUMS
mkdir -p "$P/bin" "$P/web" "$P/fonts" "$P/music"
if [ -n "${BIN:-}" ]; then
  [ -f "$BIN" ] || die "BIN=$BIN not found"
  python3 -I - "$BIN" <<'PY' || die "BIN is not a 64-bit ARM Linux program"
import sys
b = open(sys.argv[1], "rb").read(64)
ok = len(b) == 64 and b[:4] == b"\x7fELF" and b[4] == 2 and b[5] == 1 and int.from_bytes(b[18:20], "little") == 183
sys.exit(0 if ok else 1)
PY
  cp "$BIN" "$P/bin/sticktime-pi"
  BIN_DESC="$(basename "$BIN") sha256 $(sha256sum "$BIN" | cut -d' ' -f1)"
  # the GPU drawing helper (drawing = gpu), next to it: without it that setting draws on the CPU
  if [ -n "${GPU_BIN:-}" ]; then
    [ -f "$GPU_BIN" ] || die "GPU_BIN=$GPU_BIN not found"
    cp "$GPU_BIN" "$P/bin/sticktime-gpu"
    chmod 0755 "$P/bin/sticktime-gpu"
    BIN_DESC="$BIN_DESC, $(basename "$GPU_BIN") sha256 $(sha256sum "$GPU_BIN" | cut -d' ' -f1)"
  fi
else
  warn "no BIN given: the image gets a PLACEHOLDER app (it answers /api/status, draws nothing)"
  cat > "$P/bin/sticktime-pi" <<'SH'
#!/bin/sh
# PLACEHOLDER: this image was built without the StickTime Pi binary (image/build.sh BIN=...).
# It only answers http://<pi>/api/status, so the image can be booted and tested; it draws nothing.
echo "sticktime-pi: placeholder (this image has no StickTime Pi binary)" >&2
if [ -w /dev/tty1 ]; then
  printf '\033[2J\033[H\n  StickTime Pi: this image has no app binary (placeholder build).\n' > /dev/tty1 2>/dev/null
fi
exec python3 -I - "$@" <<'PY'
import http.server, json, os, socketserver, sys, time
args = sys.argv[1:]
def opt(name, default):
    return args[args.index(name) + 1] if name in args[:-1] else default
base = opt("--base", "/boot/firmware/sticktime")
port = int(opt("--port", "80"))
def read(p):
    try:
        with open(os.path.join(base, p), encoding="utf-8", errors="replace") as f:
            return f.read().strip()
    except OSError:
        return ""
t0 = time.time()
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.split("?")[0] == "/api/status":
            body = json.dumps({"version": read("VERSION"), "game_version": read("sd/SCRIPTS/TOOLS/GAME_VERSION"),
                               "placeholder": True, "uptime_ms": int((time.time() - t0) * 1000),
                               "game": {"state": 0, "state_name": "unknown", "loaded": False,
                                        "error": "placeholder: this image has no StickTime Pi binary"}}).encode()
            code, ctype = 200, "application/json; charset=utf-8"
        else:
            body, code, ctype = b"StickTime Pi: placeholder build, no app binary in this image.\n", 503, "text/plain"
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a):
        pass
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
S(("", port), H).serve_forever()
PY
SH
  chmod 0755 "$P/bin/sticktime-pi"
  BIN_DESC="PLACEHOLDER (no app binary)"
fi
# web UI (not its tests)
(cd "$REPO/web" && find . -type f ! -path './tests/*' ! -name '.*' ! -path '*/__pycache__/*' -print0) |
  while IFS= read -r -d '' f; do install -D -m 0644 "$REPO/web/$f" "$P/web/$f"; done
[ -f "$P/web/index.html" ] || die "web/index.html is missing"
install -m 0644 "$REPO"/host/fonts/*.ttf "$REPO/host/fonts/OFL.txt" "$P/fonts/"
# the system scripts as an app update carries them (the updater syncs /usr/lib/sticktime from lib/):
# the same files as the root filesystem gets below, which is checked there
python3 -I "$REPO/tools/make_app_package.py" --lib-dir "$P/lib" || die "cannot make lib/ (the system scripts)"
[ -f "$P/lib/sticktime-update" ] || die "lib/ has no sticktime-update"
cat > "$P/music/README.txt" <<'TXT'
Your own songs for StickTime Pi: put mp3, ogg or wav files here (from a PC, or on the web page
under Sound), then set music = on and music_source = songs in sticktime.txt or on the web page.
TXT
printf '%s\n' "$VER" > "$P/VERSION"
(cd "$P" && find bin web fonts lib -type f | LC_ALL=C sort | xargs -d '\n' sha256sum && sha256sum VERSION) > "$P/SHA256SUMS"

# the settings file and its default: SETTINGS_TEMPLATE_TEXT from host/src/settings_keys.h, so the
# template can't drift from the host's
python3 -I "$LIB_SRC/sticktime-setup" extract-template "$REPO/host/src/settings_keys.h" "$PAY/sticktime.txt" \
  || die "cannot extract SETTINGS_TEMPLATE_TEXT from host/src/settings_keys.h"
say "payload: $(du -sh "$PAY" | cut -f1)"

# ---- the image ------------------------------------------------------------------------------------
IMG=$WORK/work.img
say "unpacking $(basename "$BASE_FILE")"
rm -f "$IMG" "$IMG.part"
xz -dc -T0 "$BASE_FILE" > "$IMG.part"
mv "$IMG.part" "$IMG"

part_info() {   # part_info IMG -> "p1start p1sectors p1type p2start p2sectors p2type disksectors"
  python3 -I - "$1" <<'PY'
import os, struct, sys
f = open(sys.argv[1], "rb"); mbr = f.read(512)
if mbr[510:512] != b"\x55\xaa":
    sys.exit("no MBR partition table")
parts = []
for i in range(2):
    e = mbr[446 + 16 * i:446 + 16 * (i + 1)]
    parts += [struct.unpack("<I", e[8:12])[0], struct.unpack("<I", e[12:16])[0], e[4]]
print(*parts, os.path.getsize(sys.argv[1]) // 512)
PY
}
read -r P1S P1N P1T P2S P2N P2T DISKS < <(part_info "$IMG") || die "cannot read the partition table"
[ "$P1T" = 12 ] || [ "$P1T" = 11 ] || [ "$P1T" = 14 ] || die "partition 1 is not FAT (type $P1T)"
[ "$P2T" = 131 ] || die "partition 2 is not Linux (type $P2T)"
say "partitions: boot ${P1S}+${P1N}, root ${P2S}+${P2N} of $DISKS sectors"

if [ "$EXTRA_MB" -gt 0 ]; then
  need sfdisk resize2fs
  say "growing the root partition by $EXTRA_MB MiB"
  truncate -s +"${EXTRA_MB}M" "$IMG"
  echo ", +" | sfdisk --no-reread --no-tell-kernel -q -N 2 "$IMG" >/dev/null
fi

LOOP=$(losetup --find --show --partscan "$IMG")
command -v udevadm >/dev/null && udevadm settle 2>/dev/null || true
for _ in $(seq 1 60); do [ -b "${LOOP}p2" ] && [ -b "${LOOP}p1" ] && break; command -v partprobe >/dev/null && partprobe "$LOOP" 2>/dev/null; sleep 0.5; done
[ -b "${LOOP}p2" ] || die "no partition devices for $LOOP (no partscan support here?)"
[ "$(blkid -o value -s TYPE "${LOOP}p1")" = vfat ] || die "${LOOP}p1 is not vfat"
[ "$(blkid -o value -s TYPE "${LOOP}p2")" = ext4 ] || die "${LOOP}p2 is not ext4"
if [ "$EXTRA_MB" -gt 0 ]; then
  e2fsck -pf "${LOOP}p2" || [ $? -le 1 ] || die "e2fsck failed before resize2fs"
  resize2fs "${LOOP}p2"
fi

mkdir -p "$MNT"
mount "${LOOP}p2" "$MNT"
mount "${LOOP}p1" "$MNT/boot/firmware"   # Trixie mounts the FAT at /boot/firmware, not /boot
R=$MNT
B=$R/boot/firmware
[ -f "$R/etc/os-release" ] && [ -f "$B/config.txt" ] && [ -f "$B/cmdline.txt" ] || die "this does not look like Raspberry Pi OS"
grep -qx uninitialized "$R/etc/machine-id" || die "/etc/machine-id is not 'uninitialized': first-boot units would not run"
grep -q ' resize' "$B/cmdline.txt" || warn "cmdline.txt has no 'resize': the first boot won't grow the root partition"

# ---- 1. no first-boot questions: user pi exists, userconfig masked (pios-image.md 3.3) -------------
say "rootfs: first boot without questions"
sed -i -E 's#^(pi:[^:]*:1000:1000:[^:]*:/home/pi:)/usr/sbin/nologin$#\1/bin/bash#' "$R/etc/passwd"
grep -q '^pi:[^:]*:1000:1000:[^:]*:/home/pi:/bin/bash$' "$R/etc/passwd" || die "user pi (uid 1000) not found in /etc/passwd"
rm -f "$R/etc/ssh/sshd_config.d/rename_user.conf" "$R/etc/xdg/autostart/piwiz.desktop"
# the password stays locked ('!'): sticktime-setup sets it from sticktime.txt (password = ...)

UNITDIRS="$R/etc/systemd/system $R/usr/lib/systemd/system $R/lib/systemd/system"
unit_file() { for d in $UNITDIRS; do [ -e "$d/$1" ] && { echo "$d/$1"; return 0; }; done; return 1; }
sysctl_root() {   # systemctl --root, or the same links by hand when the host has no systemctl
  if command -v systemctl >/dev/null && systemctl --root="$R" "$@" >/dev/null 2>&1; then return 0; fi
  local verb=$1; shift
  local u
  for u in "$@"; do
    case $verb in
      mask) ln -sfn /dev/null "$R/etc/systemd/system/$u" ;;
      disable) find "$R/etc/systemd/system" -name "$u" -type l ! -lname /dev/null -delete ;;
      enable)
        local f; f=$(unit_file "$u") || { warn "no unit $u"; continue; }
        local t
        for t in $(sed -n 's/^WantedBy=//p' "$f"); do
          mkdir -p "$R/etc/systemd/system/$t.wants"; ln -sfn "${f#"$R"}" "$R/etc/systemd/system/$t.wants/$u"
        done ;;
      set-default) ln -sfn "/usr/lib/systemd/system/$u" "$R/etc/systemd/system/default.target" ;;
    esac
  done
}
for u in userconfig.service sshswitch.service rpi-eeprom-update.service; do
  sysctl_root disable "$u"
  sysctl_root mask "$u"
done
sysctl_root set-default multi-user.target       # no graphical.target (it pulls udisks2 even on Lite)
# the app owns tty1 (drm-kms.md 9.4); consoles on tty2-6 (Alt+F2) and SSH
sysctl_root mask getty@tty1.service autovt@tty1.service

# ---- 2. cloud-init and netplan off (3.4); Imager customisation is not used -----------------------
touch "$R/etc/cloud/cloud-init.disabled" 2>/dev/null || { mkdir -p "$R/etc/cloud" && touch "$R/etc/cloud/cloud-init.disabled"; }
rm -f "$B/user-data" "$B/meta-data" "$B/network-config"
mkdir -p "$R/etc/systemd/system-generators"
ln -sfn /dev/null "$R/etc/systemd/system-generators/netplan"

# ---- 3. less boot work (8.6) ----------------------------------------------------------------------
say "rootfs: boot trims"
for u in apt-daily.timer apt-daily-upgrade.timer man-db.timer dpkg-db-backup.timer e2scrub_all.timer \
         e2scrub_reap.service cron.service NetworkManager-wait-online.service; do
  sysctl_root disable "$u"
done
mkdir -p "$R/etc/tmpfiles.d"
# Pi OS tunes the ondemand governor, which we replace: its tmpfiles writes would fail
ln -sfn /dev/null "$R/etc/tmpfiles.d/raspberrypi-sys-mods-ondemand-governor.conf"

# Bluetooth joysticks: BlueZ's default LE connection interval is 30-50 ms, so an ELRS BLE joystick
# could send a report only every ~49 ms; 7.5-11.25 ms (the ESP32 accepts it) cut that to ~11 ms on a
# Pi 4. New pairings take these defaults (bluetoothd hands them to the kernel at start).
if [ -f "$R/etc/bluetooth/main.conf" ]; then
  python3 -I - "$R/etc/bluetooth/main.conf" <<'PY' || die "can't set the Bluetooth connection interval"
import re, sys
p = sys.argv[1]
want = {"MinConnectionInterval": "6", "MaxConnectionInterval": "9", "ConnectionLatency": "0"}
out, in_le, done = [], False, set()
for ln in open(p, encoding="utf-8").read().split("\n"):
    sec = re.match(r"^\s*\[(\w+)\]", ln)
    if sec:
        if in_le:
            out += [f"{k}={v}" for k, v in want.items() if k not in done]
            done.update(want)
        in_le = sec.group(1) == "LE"
    key = re.match(r"^\s*#?\s*(MinConnectionInterval|MaxConnectionInterval|ConnectionLatency)\s*=", ln)
    if in_le and key:
        out.append(f"{key.group(1)}={want[key.group(1)]}")
        done.add(key.group(1))
        continue
    out.append(ln)
if in_le:
    out += [f"{k}={v}" for k, v in want.items() if k not in done]
    done.update(want)
if not done:
    out += ["", "[LE]"] + [f"{k}={v}" for k, v in want.items()]
open(p, "w", encoding="utf-8").write("\n".join(out))
PY
  grep -q '^MinConnectionInterval=6$' "$R/etc/bluetooth/main.conf" || die "Bluetooth connection interval not set"
else
  warn "no /etc/bluetooth/main.conf: Bluetooth joysticks keep BlueZ's 30-50 ms connection interval"
fi

# ---- 4. our files: units, helpers, configs (image/rootfs) -----------------------------------------
say "rootfs: StickTime Pi system files"
(cd "$IMG_DIR/rootfs" && find . -type f ! -path '*/__pycache__/*' ! -name '*.pyc' ! -name '.gitkeep' ! -name '.*.swp' -print0) |
  while IFS= read -r -d '' f; do
    f=${f#./}
    mode=0644
    # programs: the helpers in /usr/lib/sticktime (no extension) and anything with a #! line
    if head -c 2 "$IMG_DIR/rootfs/$f" | grep -q '^#!'; then mode=0755; fi
    install -D -o root -g root -m "$mode" "$IMG_DIR/rootfs/$f" "$R/$f"
    # a Windows checkout may have CRLF: a #! line with \r does not run, and units would get \r too
    if grep -q $'\r' "$R/$f"; then sed -i 's/\r$//' "$R/$f"; fi
  done
for f in sticktime-setup sticktime-wifi sticktime-hotspot sticktime-bt sticktime-ci stlib.py stboot.py; do
  [ -f "$R/usr/lib/sticktime/$f" ] || die "image/rootfs is missing usr/lib/sticktime/$f"
done
[ -f "$R/usr/lib/sticktime/sticktime-update" ] || warn "sticktime-update is not in image/rootfs yet: no updates in this image"
# the updater re-patches editor/index.html after game updates (stupdate/game.py: patcher())
install -o root -g root -m 0644 "$REPO/tools/patch_editor.py" "$R/usr/lib/sticktime/patch_editor.py"
sed -i 's/\r$//' "$R/usr/lib/sticktime/patch_editor.py"
[ -f "$R/usr/lib/sticktime/settings.py" ] || warn "settings.py is not in image/rootfs yet: the helpers use their own writer"
install -m 0644 "$PAY/sticktime.txt" "$R/usr/lib/sticktime/sticktime.txt.default"
# the app set's lib/ must be what the image installed, else the first start "updates" the scripts
diff -r -x __pycache__ "$P/lib" "$R/usr/lib/sticktime" >/dev/null || die "lib/ differs from /usr/lib/sticktime"
UNITS="sticktime-setup.service sticktime.service sticktime-hotspot.service sticktime-bt.service sticktime-ci.service"
[ -f "$R/usr/lib/sticktime/sticktime-update" ] && UNITS="$UNITS sticktime-update-repair.service sticktime-update.timer"
# shellcheck disable=SC2086
sysctl_root enable $UNITS
for u in $UNITS; do
  t=$(sed -n 's/^WantedBy=//p' "$R/etc/systemd/system/$u" | head -n 1)
  [ -L "$R/etc/systemd/system/$t.wants/$u" ] || die "$u is not enabled ($t.wants)"
done
[ -L "$R/etc/systemd/system/userconfig.service" ] || die "userconfig.service is not masked"
# the initial clock of a Pi without RTC is this file's mtime (until NTP): the build time, not 2026-04
touch -d "@${SOURCE_DATE_EPOCH:-$(date +%s)}" "$R/var/lib/systemd/timesync/clock" 2>/dev/null || true

# ---- 5. the FAT partition: settings, config.txt/cmdline.txt, the app and the game ----------------
say "boot partition: sticktime.txt, sticktime/"
install -m 0644 "$PAY/sticktime.txt" "$B/sticktime.txt"
rm -rf "$B/sticktime"
mkdir -p "$B/sticktime"
cp -R "$PAY/sticktime/." "$B/sticktime/"
# config.txt block and cmdline.txt tokens from the default settings, by the same code as at every
# boot: a fresh card needs no reboot to apply them (E4)
python3 -I "$LIB_SRC/sticktime-setup" --root "$R" bake || die "sticktime-setup bake failed"
grep -q '^# >>> sticktime' "$B/config.txt" || die "config.txt has no sticktime block"
grep -q '^dtoverlay=vc4-kms-v3d,composite' "$B/config.txt" || die "config.txt: no composite overlay (default video = av)"
[ "$(grep -c '^dtoverlay=vc4-kms-v3d' "$B/config.txt")" = 1 ] || die "config.txt has more than one vc4 overlay line"
[ "$(wc -l < "$B/cmdline.txt")" = 1 ] && grep -q 'root=PARTUUID=' "$B/cmdline.txt" && grep -q ' resize' "$B/cmdline.txt" \
  && grep -q 'vc4.tv_norm=NTSC' "$B/cmdline.txt" || die "cmdline.txt is not right: $(cat "$B/cmdline.txt")"
[ "$(cat "$R/etc/hostname")" = sticktime ] || die "hostname is not sticktime"
[ -f "$R/etc/NetworkManager/system-connections/sticktime-hotspot.nmconnection" ] || die "no hotspot profile"
[ "$(stat -c %a "$R/etc/NetworkManager/system-connections/sticktime-hotspot.nmconnection")" = 600 ] || die "the hotspot profile is not 0600"
cp "$B/config.txt" "$WORK/config.txt"; cp "$B/cmdline.txt" "$WORK/cmdline.txt"
say "cmdline.txt: $(cat "$B/cmdline.txt")"
FAT_FREE=$(df -B1M --output=avail "$B" | tail -n 1 | tr -d ' ')
[ "$FAT_FREE" -ge 100 ] || die "only $FAT_FREE MiB left on the boot partition"

# ---- 6. hygiene and checks ------------------------------------------------------------------------
say "checks"
grep -qx uninitialized "$R/etc/machine-id" || die "machine-id changed"
ls "$R"/etc/ssh/ssh_host_*_key >/dev/null 2>&1 && die "ssh host keys in the image (they must be made at the first boot)"
[ -e "$R/etc/cloud/cloud-init.disabled" ] || die "cloud-init is not disabled"
find "$R/usr/lib/sticktime" -name '__pycache__' -prune -exec rm -rf {} +
sync
fstrim "$B" >/dev/null 2>&1 && TRIM_B=1 || TRIM_B=0
fstrim "$R" >/dev/null 2>&1 && TRIM_R=1 || TRIM_R=0
umount "$B"
umount "$R"
e2fsck -fn "${LOOP}p2" >/dev/null || die "e2fsck found problems on the root partition"
fsck.vfat -n "${LOOP}p1" >/dev/null || die "fsck.vfat found problems on the boot partition"
if [ "$TRIM_R" = 0 ] && command -v zerofree >/dev/null; then
  say "zerofree (fstrim did not work here): free blocks to zeros, so they compress"
  zerofree "${LOOP}p2"
fi
losetup -d "$LOOP"
LOOP=

# ---- 7. output ------------------------------------------------------------------------------------
say "compressing (xz -$XZ_LEVEL)"
xz -T0 "-$XZ_LEVEL" -c "$IMG" > "$OUT/$NAME.img.xz.part"
mv -f "$OUT/$NAME.img.xz.part" "$OUT/$NAME.img.xz"
(cd "$OUT" && sha256sum "$NAME.img.xz" > "$NAME.img.xz.sha256")
{
  echo "StickTime Pi image $NAME"
  echo "app version: $VER"
  echo "app binary: $BIN_DESC"
  echo "game: StickTime $GAME_VERSION ($(basename "$GAME_ZIP"), sha256 $(sha256sum "$GAME_ZIP" | cut -d' ' -f1))"
  echo "game editions: color$([ -f "$P/sd/SCRIPTS/TOOLS/StickTimeBW.lua" ] && echo ", B&W")$([ -f "$P/sd/SCRIPTS/TOOLS/StickTimeLite.lua" ] && echo ", Lite")"
  echo "editor page: $([ -f "$P/editor/index.html" ] && echo "yes, $(sha256sum "$P/editor/index.html" | cut -d' ' -f1)" || echo no)"
  echo "base: $(basename "$BASE_URL") (sha256 $BASE_SHA256, kernel $BASE_KERNEL)"
  echo "image: $(stat -c %s "$IMG") bytes raw, $(stat -c %s "$OUT/$NAME.img.xz") bytes xz, sha256 $(cut -d' ' -f1 < "$OUT/$NAME.img.xz.sha256")"
  echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(uname -m)"
} > "$OUT/$NAME.info"
if [ "${KEEP_IMG:-0}" = 1 ]; then mv -f "$IMG" "$OUT/$NAME.img"; else rm -f "$IMG"; fi
rm -rf "$PAY"
say "done: $OUT/$NAME.img.xz"
cat "$OUT/$NAME.info"
