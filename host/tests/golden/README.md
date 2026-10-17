# Golden tests: lcd.c against engine.js

`host/tests/test_lcd_golden.c` replays drawing calls through `lcd.c` and compares every frame,
byte for byte, with what the web emulator's own `lcd` draws for the same calls
(`sticktime/web/src/engine.js`, run on fengari exactly as a Lua script would call it).
Both sides use the same stub font: every code point advances by a fixed number of pixels and
draws nothing. So text is checked for its geometry (widths, alignment, INVERS boxes, sizeText),
and the strings that drawNumber / drawTimer print are compared as strings.

Each call file is replayed on two screens: 480x272 with the `std` font sizes and 640x480 with
`lrg`, so the edge rules (x > LCD_W rejects a line, x = LCD_W clips it) meet different calls.

## Files

| File | What |
|---|---|
| `calls/*.txt` | the call files (checked in) |
| `gen_calls.py` | writes the hand-designed call files: colors, rects, lines, tris, circles, invert_gauge, text, text_half, numbers |
| `fuzz.js` | writes `calls/fuzz1.txt` .. `fuzz4.txt`: seeded random mixes of every call (the same files every run) |
| `ref.js` | replays the call files through engine.js and writes the references to `build/golden/` |
| `../test_lcd_golden.c` | replays them through lcd.c and compares |

## Call file format

One call per line: the lcd function name, then its arguments separated by spaces.

- Numbers: decimal or `0x` hex, anything that fits 32 bits (int32 or uint32: `0xF8008000` and
  `-134184960` are the same flags). They reach the function as Lua integers.
- Strings: `"..."` with the escapes `\\` `\"` `\n` `\r` `\t` and `\xHH` (any byte).
- A line starting with `#` is a comment; blank lines are ignored.
- `adv <px>` before the first call sets the stub advance (default 7.3; stored as a float, as
  the C font code keeps it).
- `snap` records the frame buffer at that point. The frame after the last call is always recorded.

Calls: `clear`, `drawPoint`, `drawLine`, `drawLineWithClipping`, `drawFilledRectangle`,
`drawRectangle`, `invertRect`, `drawFilledTriangle`, `drawTriangle`, `drawFilledCircle`,
`drawCircle`, `drawGauge`, `drawText`, `drawNumber`, `drawTimer`, `sizeText`. Optional arguments
can be left out, as in Lua (`clear` alone is named color 5, a missing color is black).

## References (`build/golden/`, not checked in)

- `<name>.<W>x<H>.bin`: the recorded frames, one after the other, each W x H RGB565
  little-endian, row by row.
- `<name>.<W>x<H>.txt`: one line per call with a result, in order: `L<line> sizeText <w> <h>`,
  and `L<line> drawText|drawNumber|drawTimer <hex of the UTF-8 string drawn>` (`-` when empty).
- `index.txt`: the FNV-1a hash of each call file and of engine.js. The test refuses a call file
  that changed since its references were made.

When a frame differs, the test prints the number of differing pixels and the first one, and
writes what lcd.c drew next to the reference as `<name>.<W>x<H>.got<frame>.bin`.

## Regenerating

From the repository root, with the sibling `sticktime` repo checked out next to this one:

```sh
npm install --prefix build/golden-node fengari@0.1.4    # once; outside the repo's sources
python3 host/tests/golden/gen_calls.py                   # only after changing gen_calls.py
node host/tests/golden/fuzz.js                           # only after changing fuzz.js
node host/tests/golden/ref.js --fengari build/golden-node/node_modules/fengari
host/build.sh test                                       # or just run test_lcd_golden
```

`ref.js` takes `--engine <engine.js>` (default `../sticktime/web/src/engine.js`), `--out <dir>`
(default `build/golden`) and `--calls <dir>`. Rerun it whenever engine.js or a call file changes;
the test says so when it notices. Without references the test prints SKIP and passes.

engine.js loops over every row of a triangle or a filled circle, also the invisible ones (lcd.c
skips them), so keep huge shapes within the limits `fuzz.js` uses, or the reference takes minutes.
