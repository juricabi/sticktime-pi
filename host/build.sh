#!/bin/sh
# Build sticktime-pi and its unit tests.
#
#   host/build.sh [target] [what]
#     target: native (default: this machine), aarch64 (the Pi: static, musl), x86_64-linux (static)
#     what:   all (default: the program + tests), prog, tests, test (build and run the tests), clean
#
# Environment:
#   CC       compiler (default: "zig cc" when zig is on the PATH, else cc)
#   NUMS     edgetx (default: EdgeTX's Lua numbers: 32-bit integers, single floats, FLOORN2I)
#            ref    (32-bit integers, double floats: like fengari, for comparing with engine.js)
#   OPT      optimization flags (default -O2)
#   VERSION  the version string compiled in (default: VERSION file, else 0.0.0-dev)
#   JOBS     parallel compiles (default 8)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
cd "$HERE"   # relative paths in flags and object lists: the checkout path may contain spaces
TARGET=${1:-native}
WHAT=${2:-all}
NUMS=${NUMS:-edgetx}
OPT=${OPT:--O2}
JOBS=${JOBS:-8}
if [ -z "$VERSION" ]; then
  if [ -f "$ROOT/VERSION" ]; then VERSION=$(tr -d ' \r\n' < "$ROOT/VERSION"); else VERSION=0.0.0-dev; fi
fi

if [ -z "$CC" ]; then
  if command -v zig >/dev/null 2>&1; then CC="zig cc"; else CC=cc; fi
fi
TFLAGS=""
EXE=""
LIBS="-lm"
case "$TARGET" in
  native)
    case "$(uname -s)" in
      MINGW*|MSYS*|CYGWIN*) EXE=.exe; LIBS="-lm -lws2_32 -lwinmm" ;;
      *) LIBS="-lm -lpthread" ;;
    esac ;;
  aarch64)
    case "$CC" in *zig*) TFLAGS="-target aarch64-linux-musl" ;; esac
    TFLAGS="$TFLAGS -static"; LIBS="-lm -lpthread" ;;
  x86_64-linux)
    case "$CC" in *zig*) TFLAGS="-target x86_64-linux-musl" ;; esac
    TFLAGS="$TFLAGS -static"; LIBS="-lm -lpthread" ;;
  *) echo "unknown target $TARGET" >&2; exit 2 ;;
esac

case "$NUMS" in
  edgetx) LUAFLAGS="-DLUA_32BITS -DLUA_FLOORN2I=1 -DLUA_COMPAT_5_2" ;;
  ref)    LUAFLAGS="-DLUA_INT_TYPE=LUA_INT_INT -DLUA_FLOAT_TYPE=LUA_FLOAT_DOUBLE -DLUA_COMPAT_5_2" ;;
  *) echo "unknown NUMS $NUMS" >&2; exit 2 ;;
esac
case "$TARGET" in native) [ -n "$EXE" ] || LUAFLAGS="$LUAFLAGS -DLUA_USE_POSIX" ;; *) LUAFLAGS="$LUAFLAGS -DLUA_USE_POSIX" ;; esac

OUT="../build/$TARGET-$NUMS"
mkdir -p "$OUT/obj/lua" "$OUT/obj/src" "$OUT/obj/tests"

if [ "$WHAT" = clean ]; then rm -rf "$OUT"; echo "removed $OUT"; exit 0; fi

CFLAGS="$TFLAGS $OPT -g -std=gnu99 $LUAFLAGS -Ithird_party/lua -Ithird_party -Isrc -DSTP_VERSION=\"$VERSION\""
WARN="-Wall -Wextra -Wno-unused-parameter"

LUA_SRC="lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm
  lundump lvm lzio lauxlib lbaselib lbitlib lcorolib ldblib liolib lmathlib loslib lstrlib ltablib lutf8lib loadlib linit"
# every module builds on every target; Linux-only ones compile to stubs elsewhere
SRC="plat app lcd font etx settings input nav video video_png video_drm video_fb input_evdev wifi_proto input_wifi
  web web_api web_vtx scenario sys host audio dsp sfx sfx_dsp music music_voices music_fx songs vtx_proto vtx_table vtx vtx_soft"

# compile "src.c obj.o flags" lines in parallel, only when the source (or a header) is newer
JOBLIST="$OUT/jobs.txt"
: > "$JOBLIST"
NEWEST_H=$(ls -t "$HERE"/src/*.h 2>/dev/null | head -1)
add() {  # add src obj flags
  if [ ! -f "$2" ] || [ "$1" -nt "$2" ] || { [ -n "$4" ] && [ -n "$NEWEST_H" ] && [ "$NEWEST_H" -nt "$2" ]; }; then
    printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$JOBLIST"
  fi
}
for f in $LUA_SRC; do add "$HERE/third_party/lua/$f.c" "$OUT/obj/lua/$f.o" "-w"; done
for f in $SRC; do
  [ -f "$HERE/src/$f.c" ] && add "$HERE/src/$f.c" "$OUT/obj/src/$f.o" "$WARN" h
done
if [ "$WHAT" != prog ]; then
  for t in "$HERE"/tests/*.c; do
    [ -f "$t" ] || continue
    n=$(basename "$t" .c)
    add "$t" "$OUT/obj/tests/$n.o" "$WARN" h
  done
fi
if [ -s "$JOBLIST" ]; then
  export CFLAGS CC
  # shellcheck disable=SC2016
  tr '\t' '\037' < "$JOBLIST" | xargs -P "$JOBS" -I{} sh -c '
    IFS=$(printf "\037"); set -- $1; unset IFS
    echo "  CC $(basename "$1")"
    $CC $CFLAGS $3 -c "$1" -o "$2"' _ {} || { echo "compile failed" >&2; exit 1; }
fi

LUA_OBJ=""
for f in $LUA_SRC; do LUA_OBJ="$LUA_OBJ $OUT/obj/lua/$f.o"; done
SRC_OBJ=""
for f in $SRC; do [ -f "$OUT/obj/src/$f.o" ] && [ "$f" != main ] && SRC_OBJ="$SRC_OBJ $OUT/obj/src/$f.o"; done

if [ "$WHAT" != tests ] && [ -f "$HERE/src/main.c" ]; then
  add "$HERE/src/main.c" "$OUT/obj/src/main.o" "$WARN" h
  if [ ! -f "$OUT/obj/src/main.o" ] || [ "$HERE/src/main.c" -nt "$OUT/obj/src/main.o" ] || [ "$NEWEST_H" -nt "$OUT/obj/src/main.o" ]; then
    echo "  CC main.c"; $CC $CFLAGS $WARN -c "$HERE/src/main.c" -o "$OUT/obj/src/main.o"
  fi
  echo "  LD sticktime-pi$EXE"
  # shellcheck disable=SC2086
  $CC $CFLAGS -o "$OUT/sticktime-pi$EXE" "$OUT/obj/src/main.o" $SRC_OBJ $LUA_OBJ $LIBS
fi

if [ "$WHAT" != prog ]; then
  for t in "$HERE"/tests/*.c; do
    [ -f "$t" ] || continue
    n=$(basename "$t" .c)
    echo "  LD $n$EXE"
    # shellcheck disable=SC2086
    $CC $CFLAGS -o "$OUT/$n$EXE" "$OUT/obj/tests/$n.o" $SRC_OBJ $LUA_OBJ $LIBS
  done
fi

if [ "$WHAT" = test ]; then
  fail=0
  # only test_*.c are unit tests; other programs in tests/ are tools (servers, renderers)
  for t in "$HERE"/tests/test_*.c; do
    [ -f "$t" ] || continue
    n=$(basename "$t" .c)
    if (cd "$ROOT" && "$HERE/$OUT/$n$EXE"); then echo "PASS $n"; else echo "FAIL $n"; fail=1; fi
  done
  exit $fail
fi
echo "built in $OUT"
