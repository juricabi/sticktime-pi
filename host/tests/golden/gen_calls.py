#!/usr/bin/env python3
# Writes the hand-designed call files in calls/ (fuzz.js writes the fuzz ones). Each file covers one
# group of lcd calls and its edge cases; see README.md for the format. Rerun ref.js after a change.
import itertools
import math
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'calls')


def rgb(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def RGB(r, g, b, low=0):
    return '0x%08X' % (((rgb(r, g, b) << 16) | 0x8000 | low) & 0xFFFFFFFF)


def named(i, low=0):
    return '0x%X' % (((i << 16) | low) & 0xFFFFFFFF)


def write(name, lines):
    with open(os.path.join(OUT, name + '.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def background(L, w=640, h=480, step=8):
    # a varied background so blending sees many destination colors
    k = 0
    for y in range(0, h, step):
        for x in range(0, w, 80):
            c = (k * 2654435761) & 0xFFFF
            L.append('drawFilledRectangle %d %d 80 %d 0x%04X8000' % (x, y, step, c))
            k += 1


# ---------------------------------------------------------------- colors
L = ['# colorOf(flags): named colors are index << 16 (31 of them; past the table: black), the 0x8000',
     '# flag means the top 16 bits are RGB565, and the low flag bits never change the color.',
     '# clear without an argument is named color 5 (COLOR_THEME_SECONDARY3).',
     'clear', 'snap', 'clear 0', 'snap', 'clear 0x1E0000', 'snap', 'clear 0x1F0000', 'snap', 'clear 0xFFFF0000', 'snap',
     'clear 0xF8008000', 'snap', 'clear 0x07E08005', 'snap', 'clear 0x001F83FF', 'snap',
     '# SMLSIZE + white + SHADOWED as the game passes it (negative as an int32)',
     'clear -31872', 'snap', 'clear 0x8000', 'snap', 'clear 0xFFFF8000', 'snap', 'clear 0x00057FFF', 'snap',
     '# every named color, and 31 / 0xFFFF past the end, with flag bits in the low half']
lows = [0, 1, 0x80, 0x300, 0x7FFF, 0x5F04]
for i in range(33):
    L.append('drawFilledRectangle %d 0 14 100 %s' % (i * 14, named(i if i < 32 else 0xFFFF, lows[i % 6])))
L.append('# RGB colors with flag bits')
for i in range(32):
    r, g, b = (i * 37) & 255, (i * 91 + 13) & 255, (255 - i * 8) & 255
    L.append('drawFilledRectangle %d 100 15 50 %s' % (i * 15, RGB(r, g, b, lows[i % 6])))
L += ['drawFilledRectangle 0 150 480 20 0xFFFF8000', 'drawFilledRectangle 0 170 480 20 0x00008000',
      'drawFilledRectangle 0 190 480 20 0x7BEF8000',
      '# defaults: drawFilledRectangle / drawPoint / drawLine without a color are named color 0 (black)',
      'drawFilledRectangle 10 215 50 20', 'drawPoint 70 220', 'drawLine 80 220 120 220 255']
write('colors', L)

# ---------------------------------------------------------------- rects
L = ['# fillRect: negative sizes flip, clipping, opacity 0..15 (blend565 with a = round((15 - opa) * 255 / 15)),',
     '# the Lua API masks the opacity with & 15. drawRectangle: four fillRects of thickness t.']
background(L)
L.append('snap')
L.append('# every opacity of a few colors over the background')
cols = [RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255), RGB(255, 255, 255), RGB(0, 0, 0), named(9), named(26)]
for j, c in enumerate(cols):
    for opa in range(16):
        L.append('drawFilledRectangle %d %d 28 18 %s %d' % (opa * 30, j * 20, c, opa))
L.append('# opacities outside 0..15 (masked by the API)')
for k, opa in enumerate([16, 17, 31, 32, -1, -15, -16, 255, 0x7FFFFFFF, -2147483648]):
    L.append('drawFilledRectangle %d 145 28 18 %s %d' % (k * 30, RGB(255, 200, 0), opa))
L.append('snap')
L += ['# sizes: negative (flip), zero, one pixel, clipped on every side',
      'drawFilledRectangle 50 180 -20 -10 0xF8008000', 'drawFilledRectangle 60 170 20 -10 0x07E08000',
      'drawFilledRectangle 90 170 -20 10 0x001F8000', 'drawFilledRectangle 100 170 0 10 0xFFFF8000',
      'drawFilledRectangle 100 170 10 0 0xFFFF8000', 'drawFilledRectangle 120 170 1 1 0xFFFF8000',
      'drawFilledRectangle 125 170 -1 -1 0xFFFF8000', 'drawFilledRectangle -10 200 30 30 0xF8008000',
      'drawFilledRectangle 470 200 30 30 0xF8008000', 'drawFilledRectangle 630 200 30 30 0xF8008000',
      'drawFilledRectangle 200 -10 30 30 0xF8008000', 'drawFilledRectangle 200 262 30 30 0xF8008000',
      'drawFilledRectangle 240 470 30 30 0xF8008000', 'drawFilledRectangle 479 271 1 1 0x07E08000',
      'drawFilledRectangle 480 272 1 1 0x07E08000', 'drawFilledRectangle 639 479 1 1 0x07E08000',
      'drawFilledRectangle 640 480 1 1 0x07E08000', 'drawFilledRectangle 481 100 -2 5 0x07E08000',
      'drawFilledRectangle 641 100 -2 5 0x07E08000', 'drawFilledRectangle 300 273 5 -2 0x07E08000',
      'drawFilledRectangle 300 481 5 -2 0x07E08000', 'drawFilledRectangle -5 -5 5 5 0x07E08000',
      'drawFilledRectangle -5 -5 6 6 0x07E08000', 'drawFilledRectangle 0 0 -5 -5 0x07E08000',
      '# int32 extremes (JS computes with doubles: no overflow)',
      'drawFilledRectangle 2147483647 0 2147483647 10 0xFFE08000',
      'drawFilledRectangle -2147483648 230 2147483647 5 0xFFE08000',
      'drawFilledRectangle 2147483647 236 -2147483648 3 0xFFE08000',
      'drawFilledRectangle 400 -2147483648 3 2147483647 0xFFE08000',
      'drawFilledRectangle 405 -2147483648 3 -2147483648 0xFFE08000',
      'drawFilledRectangle 410 2147483647 3 -2147483648 0xFFE08000',
      'drawFilledRectangle -2147483648 -2147483648 -2147483648 -2147483648 0xFFE08000',
      'drawFilledRectangle 0 250 2147483647 2 0xF81F8000 7',
      'drawFilledRectangle 2147483647 255 -2147483647 2 0xF81F8000 3',
      'snap', 'clear 0x10000',
      '# drawRectangle: default thickness 1, 0 (nothing), thick, thicker than half, negative; colors, opacity']
for k, t in enumerate([None, 1, 0, 2, 5, 9, 10, 15, -1, -3]):
    x, y = 10 + (k % 5) * 90, 10 + (k // 5) * 60
    L.append('drawRectangle %d %d 40 20 %s%s' % (x, y, named(9), '' if t is None else ' %d' % t))
L += ['drawRectangle 10 130 -40 20 0xF8008000 2', 'drawRectangle 100 130 40 -20 0xF8008000 2',
      'drawRectangle 190 130 -40 -20 0xF8008000 3', 'drawRectangle 250 130 1 1 0xF8008000',
      'drawRectangle 260 130 2 2 0xF8008000 1', 'drawRectangle 270 130 3 3 0xF8008000 2',
      'drawRectangle 300 120 50 30 0x001F8000 4 7', 'drawRectangle 310 125 50 30 0x07E08000 4 12',
      'drawRectangle 320 130 50 30 0xF8008000 4 15', 'drawRectangle 330 135 50 30 0xFFE08000 4 16',
      'drawRectangle 340 140 50 30 0xFFE08000 4 -1', 'drawRectangle -10 -10 30 30 0xFFFF8000 3',
      'drawRectangle 460 260 30 30 0xFFFF8000 3', 'drawRectangle 620 460 30 30 0xFFFF8000 3',
      'drawRectangle 100 200 50 30', 'drawRectangle 160 200 50 30 0x8000',
      'drawRectangle 0 0 480 272 0xF81F8000', 'drawRectangle 0 0 640 480 0x07FF8000 2',
      'drawRectangle -2147483648 -2147483648 2147483647 2147483647 0xFFFF8000 2147483647',
      'drawRectangle 2147483647 2147483647 -2147483648 -2147483648 0xFFFF8000 -2147483648',
      'drawRectangle 200 220 2147483647 20 0xFFE08000 3', 'drawRectangle 200 245 -2147483648 20 0xFFE08000 3']
write('rects', L)

# ---------------------------------------------------------------- lines
L = ['# lcd.drawLine: an end beyond the right / bottom edge (x > LCD_W or y > LCD_H) draws nothing;',
     '# otherwise Liang-Barsky clipping in doubles, Math.round of the ends, Bresenham with an 8-bit',
     '# pattern (bit x & 7 on shallow lines, y & 7 on steep ones).',
     '# every pattern, horizontal (x & 7)']
for p in range(256):
    L.append('drawLine 0 %d 479 %d %d 0x%04X8000' % (p, p, p, ((p * 2654435761) >> 8) & 0xFFFF | 0x0841))
L += ['snap', 'clear', '# every pattern, vertical (y & 7), and steep diagonals']
for p in range(256):
    x = p * 2 if p < 240 else p * 2 - 470
    L.append('drawLine %d 0 %d 271 %d 0x%04X8000' % (x, x if p < 240 else x + 100, p, (p * 40503) & 0xFFFF))
L += ['snap', 'clear', '# octants from one center, ends that land on .5 after clipping, pattern & 255']
cx, cy = 240, 136
for a in range(0, 360, 7):
    x = cx + int(round(130 * math.cos(math.radians(a))))
    y = cy + int(round(130 * math.sin(math.radians(a))))
    L.append('drawLine %d %d %d %d %d %s' % (cx, cy, x, y, 255 if a % 3 else 0x55, named(17 + a % 13)))
L += ['drawLine -1 0 1 1 255 0xFFFF8000', 'drawLine 0 -1 1 1 255 0xFFFF8000', 'drawLine -3 5 3 6 255 0xFFFF8000',
      'drawLine 5 -3 6 3 255 0xFFFF8000', 'drawLine -1 10 2 11 255 0xFFFF8000', 'drawLine 20 -7 21 7 255 0xFFFF8000',
      'drawLine 30 40 30 40 255 0xFFFF8000', 'drawLine 32 40 32 40 0x55 0xFFFF8000', 'drawLine 33 40 33 40 0x55 0xFFFF8000',
      'drawLine 40 50 60 50 0x1FF 0xFFFF8000', 'drawLine 40 52 60 52 0x100 0xFFFF8000', 'drawLine 40 54 60 54 -1 0xFFFF8000',
      'drawLine 40 56 60 56 -2147483648 0xFFFF8000', 'drawLine 40 58 60 58 0x7FFFFFFF 0xFFFF8000',
      'drawLine 40 60 60 60 255', 'drawLine 40 62 60 62 0x33 0xF8008000',
      'snap', 'clear',
      '# the API rejection: x > 480 / 640 or y > 272 / 480 (each screen rejects different ones)',
      'drawLine 0 10 480 10 255 0xFFFF8000', 'drawLine 0 12 481 12 255 0xFFFF8000', 'drawLine 0 14 640 14 255 0xFFFF8000',
      'drawLine 0 16 641 16 255 0xFFFF8000', 'drawLine 481 18 0 18 255 0xFFFF8000', 'drawLine 641 20 0 20 255 0xFFFF8000',
      'drawLine 10 0 10 272 255 0xFFFF8000', 'drawLine 12 0 12 273 255 0xFFFF8000', 'drawLine 14 0 14 480 255 0xFFFF8000',
      'drawLine 16 0 16 481 255 0xFFFF8000', 'drawLine 18 273 18 0 255 0xFFFF8000', 'drawLine 20 481 20 0 255 0xFFFF8000',
      'drawLine 480 272 0 0 255 0xF8008000', 'drawLine 640 480 0 0 255 0x07E08000', 'drawLine 2147483647 0 0 0 255 0xFFFF8000',
      'drawLine 0 0 0 2147483647 255 0xFFFF8000',
      '# negative ends are clipped, never rejected',
      'drawLine -100 30 100 30 255 0xFFE08000', 'drawLine -1000 -1000 300 200 255 0xFFE08000',
      'drawLine 300 -50 -50 300 255 0xFFE08000', 'drawLine -2147483648 40 200 40 255 0xFFE08000',
      'drawLine -2147483648 -2147483648 470 260 255 0x07FF8000',
      'drawLine -2147483648 -2147483648 -2147483648 -2147483648 255 0x07FF8000',
      'drawLine 100 -2147483648 110 270 255 0x07FF8000', 'drawLine -2147483648 270 479 -2147483648 255 0xF81F8000',
      'drawLine -5 -5 -1 -1 255 0xFFFF8000', 'drawLine -10 100 -1 120 255 0xFFFF8000', 'drawLine 50 -10 70 -1 255 0xFFFF8000',
      '# along the edges',
      'drawLine 0 0 479 0 255 0x001F8000', 'drawLine 0 271 479 271 255 0x001F8000', 'drawLine 479 0 479 271 255 0x001F8000',
      'drawLine 0 0 0 271 255 0x001F8000', 'drawLine 639 0 639 479 255 0x001F8000', 'drawLine 0 479 639 479 255 0x001F8000',
      '# long shallow / steep lines crossing everything',
      'drawLine -300 100 900 101 255 0xFFFF8000', 'drawLine 200 -500 201 700 255 0xFFFF8000',
      'drawLine -700 -3 480 250 0x0F 0xFFFF8000', 'drawLine 479 271 -2000 270 255 0xF8008000',
      'drawLine 3 3 -3 -3 255 0xF8008000',
      'snap', 'clear',
      '# drawLineWithClipping (engine.js clips to the screen only) and drawTriangle: no rejection',
      'drawLineWithClipping 0 0 700 500 0 100 0 100 255 0xFFFF8000',
      'drawLineWithClipping -50 200 900 210 0 0 0 0 0x0F 0xF8008000',
      'drawLineWithClipping 10 10 20 20 0 0 0 0 255', 'drawTriangle 10 10 600 50 100 400 0x07E08000',
      'drawTriangle -100 -100 2000 100 300 3000 0xF8008000', 'drawTriangle 200 200 200 200 200 200 0xFFFF8000',
      'drawTriangle 250 100 300 100 275 100', 'drawTriangle -2147483648 0 2147483647 271 0 2147483647 0x001F8000']
write('lines', L)

# ---------------------------------------------------------------- triangles
L = ['# drawFilledTriangle: Adafruit GFX fill, one hline per row, truncating division (Math.trunc),',
     '# rows outside the screen skipped; opacity & 15.']
background(L)
L.append('snap')
shapes = [((0, 0, 40, 0, 20, 30), RGB(255, 0, 0)), ((0, 30, 40, 30, 20, 0), RGB(0, 255, 0)),
          ((0, 0, 0, 30, 40, 15), RGB(0, 0, 255)), ((40, 0, 40, 30, 0, 15), RGB(255, 255, 0)),
          ((0, 0, -7, 13, 9, 30), RGB(255, 0, 255)), ((3, 0, 37, 7, -11, 29), RGB(0, 255, 255)),
          ((0, 10, 40, 10, 20, 10), RGB(255, 255, 255)), ((40, 12, 0, 12, 25, 12), RGB(255, 128, 0)),
          ((5, 5, 5, 5, 5, 5), RGB(255, 255, 255)), ((0, 0, 10, 10, 30, 30), RGB(128, 255, 0)),
          ((10, 0, 10, 15, 10, 30), RGB(0, 128, 255)), ((0, 0, 1, 30, 2, 0), RGB(255, 0, 128))]
for k, (a, c) in enumerate(shapes):
    ox, oy = 10 + (k % 6) * 75, 20 + (k // 6) * 45
    L.append('drawFilledTriangle %d %d %d %d %d %d %s' % (a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy, a[4] + ox, a[5] + oy, c))
L.append('# the 6 corner orders of one triangle')
for k, perm in enumerate(itertools.permutations([(0, 0), (33, 11), (9, 37)])):
    ox, oy = 10 + k * 75, 120
    L.append('drawFilledTriangle %s %s' % (' '.join('%d %d' % (p[0] + ox, p[1] + oy) for p in perm), named(20 + k)))
L.append('# opacities 0..15 (and outside, masked)')
for k, opa in enumerate(list(range(16)) + [16, -1, 31]):
    ox, oy = 5 + (k % 16) * 29, 170 if k < 16 else 205
    L.append('drawFilledTriangle %d %d %d %d %d %d %s %d' % (ox, oy, ox + 40, oy + 10, ox + 5, oy + 30, RGB(255, 255, 255), opa))
L += ['snap', 'clear',
      '# off-screen corners: rows outside the screen, x far left / right, huge values with few rows',
      'drawFilledTriangle -100000 10 100000 20 240 60 0xF8008000',
      'drawFilledTriangle 240 -5000 100 5000 400 300 0x07E08000 5',
      'drawFilledTriangle -2147483648 70 2147483647 90 50 100 0x001F8000',
      'drawFilledTriangle 2147483647 100 -2147483648 100 10 130 0xFFE08000',
      'drawFilledTriangle -2147483648 140 -2147483648 160 2147483647 160 0xF81F8000 3',
      'drawFilledTriangle 100 -1000000 300 1000000 -200 100 0x07FF8000 9',
      'drawFilledTriangle 0 -2000 479 271 200 2000 0xFFFF8000 12', 'drawFilledTriangle 0 271 479 271 240 272 0xF8008000',
      'drawFilledTriangle 0 272 479 272 240 300 0xF8008000', 'drawFilledTriangle 0 -1 479 -1 240 -30 0xF8008000',
      'drawFilledTriangle 0 0 479 0 240 -30 0xFFE08000', 'drawFilledTriangle 600 470 700 479 620 500 0xFFE08000',
      'drawFilledTriangle 10 200 10 200 10 200', 'drawFilledTriangle -5 -5 -5 -5 -5 -5 0xFFFF8000',
      'drawFilledTriangle 470 250 490 260 475 280 0xFFFF8000', 'drawFilledTriangle -3 250 4 260 -2 280 0xFFFF8000',
      'drawFilledTriangle 1000 0 1000 100 1100 50 0xFFFF8000', 'drawFilledTriangle -1000 0 -1000 100 -1100 50 0xFFFF8000']
write('tris', L)

# ---------------------------------------------------------------- circles
L = ['# drawFilledCircle: rows dy = -r..r of half width floor(sqrt(r*r - dy*dy)); drawCircle: 360 points',
     '# rounded from cx + r cos(a), cy + r sin(a). drawPoint: inside the screen only.']
for r in range(-2, 15):
    L.append('drawFilledCircle %d 20 %d %s' % (15 + (r + 2) * 27, r, named(17 + (r + 2) % 14)))
for r in range(0, 21):
    L.append('drawCircle %d 70 %d %s' % (12 + r * 22, r, named(17 + r % 14)))
L += ['drawCircle 100 140 -10 0xFFFF8000', 'drawCircle 140 140 -1 0xFFFF8000', 'drawCircle 160 140 1 0xFFFF8000',
      'drawCircle 180 140 2 0xFFFF8000', 'drawCircle 240 160 50 0xF8008000', 'drawCircle 240 160 51 0x07E08000',
      'drawCircle 240 160 52 0x001F8000', 'drawCircle 240 160 100 0xFFE08000', 'drawCircle 400 200 120',
      'drawFilledCircle 0 0 30 0xF8008000', 'drawFilledCircle 480 272 30 0x07E08000', 'drawFilledCircle 640 480 30 0x001F8000',
      'drawFilledCircle -20 136 25 0xFFFF8000', 'drawFilledCircle 240 -20 25 0xFFFF8000', 'drawFilledCircle 240 300 40 0xFFFF8000',
      'drawFilledCircle 600 300 45', 'drawCircle 0 0 40 0xFFFF8000', 'drawCircle 479 271 40 0xFFFF8000',
      'drawCircle 639 479 40 0xFFFF8000', 'drawCircle -40 100 41 0xFFFF8000',
      'snap', 'clear',
      'drawFilledCircle 240 136 300 0x18E38000', 'drawFilledCircle -100000 136 100100 0xF8008000',
      'drawFilledCircle 240 100136 100000 0x07E08000', 'drawFilledCircle 240 136 -2147483648 0xFFFF8000',
      'drawFilledCircle 2147483647 2147483647 5 0xFFFF8000', 'drawFilledCircle -2147483648 -2147483648 5 0xFFFF8000',
      '# r * r beyond 2^53: the doubles round (engine.js loops all 2r + 1 rows, so not 2^31)',
      'drawFilledCircle 240 100000100 100000000 0xF81F8000', 'drawFilledCircle -99999800 136 100000000 0x07FF8000',
      'drawCircle 240 136 1000000 0xFFFF8000', 'drawCircle 240 136 2147483647 0xFFFF8000',
      'drawCircle 240 136 -2147483648 0xFFFF8000', 'drawCircle -1000 136 1240 0xFFE08000',
      'drawCircle 2147483647 -2147483648 3 0xFFE08000',
      'snap', 'clear', '# points',
      'drawPoint 0 0 0xFFFF8000', 'drawPoint 479 271 0xFFFF8000', 'drawPoint 480 271 0xF8008000', 'drawPoint 479 272 0xF8008000',
      'drawPoint 639 479 0xFFFF8000', 'drawPoint 640 0 0xF8008000', 'drawPoint 0 480 0xF8008000', 'drawPoint -1 0 0xF8008000',
      'drawPoint 0 -1 0xF8008000', 'drawPoint 100 100', 'drawPoint 101 100 0x50000', 'drawPoint -2147483648 2147483647 0xFFFF8000',
      'drawPoint 2147483647 0 0xFFFF8000', 'drawPoint 200 200 0x1F0000']
write('circles', L)

# ---------------------------------------------------------------- invert + gauge
L = ['# invertRect: XOR 0xFFFF inside the rect clipped to the screen (negative sizes do nothing).',
     '# drawGauge: a DISABLED box, then round(w * clamp(n / d, 0, 1)) in the color (d = 0: n / 0 is',
     '# +-Infinity; 0 / 0 is NaN and draws no bar).']
background(L)
L += ['invertRect 10 10 100 50', 'invertRect 60 30 100 50', 'invertRect 200 10 50 50', 'invertRect 200 10 50 50',
      'invertRect 300 10 -50 50', 'invertRect 300 10 50 -50', 'invertRect -10 100 30 30', 'invertRect 470 100 30 30',
      'invertRect 630 100 30 30', 'invertRect 100 262 30 30', 'invertRect 100 470 30 30', 'invertRect 0 0 0 0',
      'invertRect -2147483648 140 2147483647 5', 'invertRect 2147483647 150 2147483647 5',
      'invertRect 400 -2147483648 5 2147483647', 'invertRect 410 0 3 2147483647',
      'invertRect -2147483648 -2147483648 -2147483648 -2147483648',
      'snap', 'clear 0x30000']
y = 2
for (w, h, n, d, c) in [(100, 8, 1, 4, RGB(255, 0, 0)), (100, 8, 3, 4, RGB(0, 255, 0)), (100, 8, 7, 4, RGB(0, 0, 255)),
                        (100, 8, 4, 4, RGB(255, 255, 0)), (100, 8, 0, 4, RGB(255, 0, 255)), (100, 8, -1, 4, RGB(0, 255, 255)),
                        (100, 8, 5, 0, RGB(255, 255, 255)), (100, 8, -5, 0, RGB(255, 255, 255)), (100, 8, 0, 0, RGB(255, 255, 255)),
                        (100, 8, 1, -4, RGB(255, 128, 0)), (100, 8, -1, -4, RGB(255, 128, 0)), (5, 8, 1, 2, RGB(255, 128, 0)),
                        (3, 8, 1, 2, RGB(128, 255, 0)), (-5, 8, 1, 2, RGB(255, 0, 0)), (-100, 8, 1, 3, RGB(255, 0, 0)),
                        (100, -8, 1, 2, RGB(0, 255, 0)), (0, 8, 1, 2, RGB(0, 255, 0)), (100, 8, 1, 3, RGB(0, 0, 255)),
                        (100, 8, 2, 3, RGB(0, 0, 255)), (7, 8, 1, 2, None), (2147483647, 8, 1, 2, RGB(255, 255, 255)),
                        (-2147483648, 8, 1, 2, RGB(255, 255, 255)), (100, 8, 2147483647, -2147483648, RGB(255, 255, 255)),
                        (100, 8, -2147483648, -2147483648, RGB(255, 0, 0)), (100, 8, 2147483647, 2147483647, RGB(0, 255, 0))]:
    x = 200 if w < 0 else 20
    L.append('drawGauge %d %d %d %d %d %d%s' % (x, y, w, h, n, d, '' if c is None else ' ' + c))
    y += 10
L += ['drawGauge 300 10 100 20 1 2 0x50000', 'drawGauge -50 40 100 20 3 4 0xF8008000',
      'drawGauge 450 40 100 20 3 4 0xF8008000', 'drawGauge 620 40 100 20 1 4 0xF8008000']
write('invert_gauge', L)

# ---------------------------------------------------------------- text
CROAT = '\\xc4\\x8c\\xc4\\x87\\xc5\\xbe\\xc5\\xa1\\xc4\\x91 Track \\xc3\\x9cn\\xc3\\xafcode'
EMOJI = '\\xf0\\x9f\\x98\\x80!'
TXT = ['abc', 'StickTime', '', '\\xc2\\xb0/s', 'Hoop Forest', CROAT, EMOJI, 'x\\ty\\nz']
L = ['# drawText with the stub font (7.3 px a code point, nothing drawn): alignment (RIGHT x - w, CENTER',
     '# x - w / 2, VCENTER y - h / 2, Math.round) shows in the INVERS box (x - 1, y, w + 2, lh). Every',
     '# size index (flags >> 8 & 15; 8..15 are 16 px), UTF-8 counted by code point.',
     'adv 7.3', 'clear 0x10000']
al = [0, 0x08, 0x04, 0x0C, 0x02, 0x06, 0x0A, 0x0E]
y = 2
for k, s in enumerate(TXT):
    for j, a in enumerate(al):
        x = 30 + j * 58
        L.append('drawText %d %d "%s" 0x%08X' % (x, y, s, ((17 + (k + j) % 13) << 16) | a | 0x01))
    y += 26 if k % 2 else 22
L += ['snap', 'clear 0x10000']
y = 0
for i in range(16):
    L.append('drawText 20 %d "Wx" 0x%08X' % (y, (i << 8) | 0x01 | ((17 + i % 13) << 16)))
    L.append('drawText 300 %d "Wx" 0x%08X' % (y, (i << 8) | 0x01 | 0x08 | 0x02 | 0xF8008000))
    y += 16
L += ['snap', 'clear 0x10000',
      '# SHADOWED (draws nothing with the stub), INVERS with RGB and named colors, defaults, edges',
      'drawText 10 10 "abc" 0x80', 'drawText 10 30 "abc" 0x81', 'drawText 10 50 "abc" 0xFFFF8381', 'drawText 10 70 "abc"',
      'drawText 10 90 "abc" 0x07E08001', 'drawText 10 110 "" 0x01', 'drawText 30 110 "" 0x05', 'drawText 50 110 "" 0x09',
      'drawText 0 130 "abcdef" 0x09', 'drawText -3 150 "abcdef" 0x05', 'drawText 480 170 "abcdef" 0x09',
      'drawText 479 190 "abcdef" 0x01', 'drawText 640 210 "abcdef" 0x09', 'drawText 300 -5 "abcdef" 0x03',
      'drawText 300 280 "abcdef" 0x03', 'drawText 300 485 "abcdef" 0x03', 'drawText 100 262 "abc" 0x01',
      'drawText -2147483648 10 "abc" 0x0F', 'drawText 2147483647 10 "abc" 0x0F', 'drawText 200 -2147483648 "abc" 0x03',
      'drawText 200 2147483647 "abc" 0x03', 'drawText 200 240 "%s" 0x0D' % ('W' * 120),
      'drawText 0 255 "%s" 0x01' % ('i' * 80), 'drawText 100 200 "quote \\" backslash \\\\ done" 0x01',
      '# sizeText at every size']
for i in range(16):
    for s in ['', 'a', 'abc', CROAT, EMOJI]:
        L.append('sizeText "%s" 0x%X' % (s, i << 8))
L += ['sizeText "abc"', 'sizeText "abc" 0xFFFF8580', 'sizeText "abc" -1']
write('text', L)

# ---------------------------------------------------------------- text with halves
L = ['# drawText where CENTER / VCENTER land on halves: Math.round takes them up, also when negative',
     '# (C round() would take them away from zero). 2.5 px a code point: 1 -> w 3, 2 -> 5, 3 -> 8.',
     'adv 2.5', 'clear 0x10000']
y = 0
for k, s in enumerate(['a', 'ab', 'abc', 'abcd', 'abcdefg']):
    for j, x in enumerate([2, 1, 0, 3, 100, 101]):
        L.append('drawText %d %d "%s" 0x%08X' % (x + j * 60 if x > 3 else x + j * 3, y + j * 3, s, 0x05 | ((17 + j) << 16)))
    y += 22
L += ['# negative halves near the left edge: x - w / 2 = -5.5 -> -5 (the box from -6 shows up to x 9)',
      'drawText 2 120 "abcdef" 0xF8008005', 'drawText 1 140 "abcdef" 0x07E08005', 'drawText 0 160 "abcdefgh" 0x001F8005',
      '# VCENTER of an odd line height near the top: XXL (64 px, lh 75 on std) 10 - 37.5 = -27.5 -> -27',
      'drawText 100 10 "ab" 0xF8008603', 'drawText 140 0 "ab" 0x07E08603', 'drawText 180 37 "ab" 0x001F8603',
      'drawText 220 38 "ab" 0xFFE08603', 'drawText 260 -1 "abc" 0xF81F8203', 'drawText 300 3 "abc" 0x07FF8203',
      'drawText 340 2 "a" 0xFFFF8303', 'drawText 380 4 "a" 0xFFFF8503', 'drawText 420 5 "a" 0xFFFF8703',
      'drawText 0 0 "abc" 0xFFFF800F', 'drawText 1 200 "abc" 0xFFFF800F']
write('text_half', L)

# ---------------------------------------------------------------- numbers / timers
L = ['# drawNumber: (v / 10).toFixed(1) for PREC1, (v / 100).toFixed(2) for PREC2, else String(v);',
     '# drawTimer: [-]MM:SS (minutes not limited). With the stub font the frames only show the widths;',
     '# test_lcd_golden.c checks the strings themselves against the ones engine.js measured.',
     'adv 6.5', 'clear 0x10000']
vals = [0, 1, -1, 5, -5, 9, -9, 10, -10, 12, -12, 15, -15, 99, -99, 100, -100, 999, 1000, -1000, 12345, -12345,
        65535, -65536, 2147483647, -2147483648, 2147483646, -2147483647]
y = 0
for k, v in enumerate(vals):
    for j, fl in enumerate([0x00, 0x10, 0x20, 0x30, 0x20 | 0x08, 0x30 | 0x04]):
        x = 10 + j * 78 + (70 if fl & 0x08 else 0)
        L.append('drawNumber %d %d %d 0x%08X' % (x, y, v, fl | 0x01 | ((17 + (k + j) % 13) << 16) | (0x300 if k % 2 else 0)))
    y += 9 if k % 2 == 0 else 10
    if y > 250:
        L += ['snap', 'clear 0x10000']
        y = 0
L += ['drawNumber 10 260 42', 'snap', 'clear 0x10000']
tim = [0, 1, 59, 60, 61, 599, 600, 3599, 3600, 5999, 6000, 359999, -1, -59, -60, -61, -3599, -6000,
       2147483647, -2147483648, 2147483646, -2147483647]
y = 0
for k, v in enumerate(tim):
    L.append('drawTimer 10 %d %d 0x%08X' % (y, v, 0x01 | ((17 + k % 13) << 16)))
    L.append('drawTimer 400 %d %d 0x%08X' % (y, v, 0x09 | 0x2000 | ((17 + k % 13) << 16)))
    y += 12
L.append('drawTimer 200 0 125')
write('numbers', L)
print('wrote the hand-designed call files to', OUT)
