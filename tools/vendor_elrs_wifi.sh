#!/bin/sh
# Vendor the ELRS / Crossfire WiFi joystick protocol core into host/third_party/elrs-wifi/.
#
#   tools/vendor_elrs_wifi.sh [path-to-linux-repo]   copy the core from a checkout of
#                                                    github.com/juricabi/elrs-crossfire-wifi-joystick-linux
#                                                    (default: ../elrs-crossfire-wifi-joystick-linux)
#   tools/vendor_elrs_wifi.sh --check                verify the vendored files against VENDOR (no network):
#                                                    exit 1 on any difference
#
# The core lives in that repo (core/, MIT) and is shared with the Linux app; it is copied here unmodified,
# so a fix goes upstream first and is then vendored again with this script. The fake module the tests
# use (host/tests/fake_elrs.py) comes along. VENDOR records the commit and the SHA-256 of every copied
# file (the commit goes into README.md's "Commit" row too); --check (CI) makes sure nobody edited them here.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR=host/third_party/elrs-wifi
# upstream path -> path in this repo; the order is VENDOR's order
FILES="core/wifi_proto.h:$DIR/wifi_proto.h
core/wifi_proto.c:$DIR/wifi_proto.c
core/wifi_net.h:$DIR/wifi_net.h
core/wifi_net.c:$DIR/wifi_net.c
core/tests/t.h:$DIR/tests/t.h
core/tests/test_wifi_proto.c:$DIR/tests/test_wifi_proto.c
core/tests/test_wifi_net.c:$DIR/tests/test_wifi_net.c
LICENSE:$DIR/LICENSE
tests/fake_elrs.py:host/tests/fake_elrs.py"
# ours in $DIR, not vendored: never compared, never removed
OWN="$DIR/README.md $DIR/VENDOR"

die() { echo "vendor_elrs_wifi: $*" >&2; exit 1; }

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum < "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 < "$1" | cut -d' ' -f1
  elif command -v openssl >/dev/null 2>&1; then openssl dgst -sha256 < "$1" | sed 's/^.*= *//'
  else die "no sha256sum, shasum or openssl"
  fi
}

# the files in $DIR, as paths from the repo root
dir_files() { (cd "$ROOT" && find "$DIR" -type f | sort); }

check() {
  [ -f "$ROOT/$DIR/VENDOR" ] || die "$DIR/VENDOR is missing: run tools/vendor_elrs_wifi.sh"
  bad=0
  listed=""
  n=0
  # "<sha256>  <path from the repo root>" lines; the rest (comments, source, commit) is information
  while IFS= read -r line; do
    case "$line" in
      ""|\#*|source\ *|commit\ *) continue ;;
    esac
    sum=${line%%  *}
    path=${line#*  }
    if [ "$sum" = "$line" ] || [ -z "$path" ] || [ "${#sum}" -ne 64 ]; then
      echo "VENDOR: unreadable line: $line" >&2; bad=1; continue
    fi
    case "$path" in
      /*|*..*) echo "VENDOR: bad path: $path" >&2; bad=1; continue ;;
    esac
    n=$((n + 1))
    listed="$listed
$path"
    if [ ! -f "$ROOT/$path" ]; then
      echo "missing: $path" >&2; bad=1
    elif [ "$(sha256 "$ROOT/$path")" != "$sum" ]; then
      echo "changed: $path (edit it upstream, then vendor it again)" >&2; bad=1
    fi
  done < "$ROOT/$DIR/VENDOR"
  [ "$n" -gt 0 ] || { echo "VENDOR lists no files" >&2; bad=1; }
  # a file VENDOR doesn't know is a difference too (it would be built, but nobody vendored it)
  for f in $(dir_files); do
    case " $OWN " in *" $f "*) continue ;; esac
    if ! printf '%s\n' "$listed" | grep -Fqx -- "$f"; then
      echo "not in VENDOR: $f" >&2; bad=1
    fi
  done
  commit=$(sed -n 's/^commit //p' "$ROOT/$DIR/VENDOR")
  [ -n "$commit" ] || { echo "VENDOR names no commit" >&2; bad=1; }
  # shellcheck disable=SC2016  # the backticks are Markdown's
  readme=$(sed -n 's/^| Commit | `\([^`]*\)`.*/\1/p' "$ROOT/$DIR/README.md" 2>/dev/null)
  if [ "$readme" != "$commit" ]; then
    echo "$DIR/README.md names commit '$readme', VENDOR '$commit'" >&2; bad=1
  fi
  if [ "$bad" -ne 0 ]; then
    echo "$DIR differs from VENDOR" >&2
    exit 1
  fi
  echo "$DIR matches VENDOR ($n files, commit $commit)"
}

vendor() {
  SRC=$1
  [ -d "$SRC" ] || die "no such directory: $SRC"
  SRC=$(cd "$SRC" && pwd)
  [ -f "$SRC/core/wifi_proto.h" ] && [ -f "$SRC/core/wifi_net.h" ] ||
    die "$SRC is not a checkout of elrs-crossfire-wifi-joystick-linux (no core/wifi_proto.h)"
  commit=$(git -C "$SRC" rev-parse HEAD 2>/dev/null) || die "$SRC is not a git checkout (the commit goes into VENDOR)"
  froms=$(printf '%s\n' "$FILES" | cut -d: -f1 | tr '\n' ' ')
  # shellcheck disable=SC2086
  if [ -n "$(git -C "$SRC" status --porcelain -- $froms)" ]; then
    echo "vendor_elrs_wifi: WARNING: $SRC has uncommitted changes in the vendored files: VENDOR says -dirty;" >&2
    echo "  commit them upstream and vendor again before committing here" >&2
    commit="$commit-dirty"
  fi
  # every source first, so a missing one changes nothing here
  for from in $froms; do
    [ -f "$SRC/$from" ] || die "missing in $SRC: $from"
  done
  mkdir -p "$ROOT/$DIR/tests"
  # files vendored before that upstream no longer has
  tos=$(printf '%s\n' "$FILES" | cut -d: -f2)
  for f in $(dir_files); do
    case " $OWN " in *" $f "*) continue ;; esac
    if ! printf '%s\n' "$tos" | grep -Fqx -- "$f"; then rm -f "$ROOT/$f"; echo "  removed $f"; fi
  done
  tmp="$ROOT/$DIR/VENDOR.tmp"
  {
    echo "# $DIR: written by tools/vendor_elrs_wifi.sh, do not edit (see README.md). Paths from the repo root."
    echo "source github.com/juricabi/elrs-crossfire-wifi-joystick-linux"
    echo "commit $commit"
  } > "$tmp"
  printf '%s\n' "$FILES" | while IFS=: read -r from to; do
    cp "$SRC/$from" "$ROOT/$to.tmp"
    mv "$ROOT/$to.tmp" "$ROOT/$to"
    printf '%s  %s\n' "$(sha256 "$ROOT/$to")" "$to" >> "$tmp"
    echo "  $from -> $to"
  done
  mv "$tmp" "$ROOT/$DIR/VENDOR"
  readme="$ROOT/$DIR/README.md"
  if [ -f "$readme" ]; then
    sed "s/^| Commit | .*/| Commit | \`$commit\` (also in \`VENDOR\`, with the SHA-256 of every file copied) |/" "$readme" > "$readme.tmp"
    mv "$readme.tmp" "$readme"
    grep -q "^| Commit | \`$commit\`" "$readme" || echo "vendor_elrs_wifi: WARNING: README.md has no \"| Commit |\" row to update" >&2
  fi
  echo "vendored commit $commit; now: host/build.sh native test"
}

case "${1:-}" in
  --check) [ $# -eq 1 ] || die "--check takes no other argument"; check ;;
  -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0" ;;
  -*) die "unknown option $1 (see --help)" ;;
  *) [ $# -le 1 ] || die "one path at most (see --help)"; vendor "${1:-$ROOT/../elrs-crossfire-wifi-joystick-linux}" ;;
esac
