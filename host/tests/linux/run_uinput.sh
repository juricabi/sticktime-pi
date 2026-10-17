#!/bin/sh
# Build the uinput test of input_evdev as a static aarch64 binary and run it as root on an arm64
# Linux machine (a Pi, or a VM) over ssh. Uses a stand-in for input.c when it isn't written yet.
#
#   host/tests/linux/run_uinput.sh root@host [ssh options...]     e.g. root@127.0.0.1 -p 2222 -i key
#   host/tests/linux/run_uinput.sh                                 only build
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${OUT:-"$ROOT/build/uinput-test"}
mkdir -p "$OUT"
CC=${CC:-zig cc}
INPUT_C="$ROOT/host/src/input.c"
if [ ! -f "$INPUT_C" ]; then
  INPUT_C="$OUT/input_stub.c"
  cat > "$INPUT_C" <<'EOF'
#include <string.h>
#include "input.h"
void insrc_update(insrc_t *s, const int16_t *ch, int nch, int64_t now_us) {
  if (nch > INPUT_CH) nch = INPUT_CH;
  bool moved = false;
  for (int i = 0; i < nch; i++) { int d = ch[i] - s->ch[i]; if (d > 51 || d < -51) moved = true; s->ch[i] = ch[i]; }
  s->nch = (uint8_t)nch; s->last_us = now_us; if (moved) s->moved_us = now_us; s->updates++;
}
EOF
fi
# shellcheck disable=SC2086
$CC -target aarch64-linux-musl -static -O2 -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
  -I"$ROOT/host/src" "$HERE/test_evdev_uinput.c" "$ROOT/host/src/input_evdev.c" "$INPUT_C" \
  -o "$OUT/test_evdev_uinput" -lm
echo "built $OUT/test_evdev_uinput"
[ $# -ge 1 ] || exit 0
DEST=$1
shift
PORT=""
ARGS=""
while [ $# -gt 0 ]; do
  case "$1" in
    -p) PORT=$2; shift 2 ;;
    *) ARGS="$ARGS $1"; shift ;;
  esac
done
# shellcheck disable=SC2086
scp ${PORT:+-P $PORT} $ARGS "$OUT/test_evdev_uinput" "$DEST:/tmp/test_evdev_uinput"
# shellcheck disable=SC2086
ssh ${PORT:+-p $PORT} $ARGS "$DEST" "chmod +x /tmp/test_evdev_uinput; modprobe uinput 2>/dev/null; /tmp/test_evdev_uinput"
