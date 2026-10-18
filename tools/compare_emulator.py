#!/usr/bin/env python3
"""Render the same game scenes with the browser emulator (headless Chromium, sticktime's
web/simulator.html) and with sticktime-pi (headless scenario), then compare the pictures.

  python tools/compare_emulator.py [--bin PATH] [--sizes 1280x720,800x480,480x272] [--out DIR]

Writes OUT/<size>/<scene>_{web,pi,diff,side}.png and prints the share of pixels that differ
(any channel by more than 48). Geometry should match closely; text differs a little (canvas vs
stb_truetype antialiasing) and the 3D scene can shift by a pixel here and there (the browser's
Lua uses doubles, the Pi's single floats like EdgeTX).
"""
import argparse
import base64
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
SIBLING = os.path.abspath(os.path.join(ROOT, "..", "sticktime"))
EXE = ".exe" if os.name == "nt" else ""
RADIO = {"1280x720": "pc", "1920x1080": "pc1080", "800x480": "mk3", "480x272": "tx16s", "480x320": "tx15", "320x240": "pa01"}

# scenes: a list of steps, each ("test", name, *args) | ("frames", n) | ("sticks", a, e, t, r)
SCENES = {
    "menu": [("frames", 120)],
    "settings": [("frames", 30), ("key", "next"), ("key", "next"), ("key", "next"), ("key", "next"), ("key", "next"),
                 ("key", "enter"), ("frames", 30)],
    "meadow_fly": [("test", "track", 1), ("test", "start", 2), ("frames", 330), ("sticks", 0, 120, 180, 0), ("frames", 90)],
    "bando_fly": [("test", "track", 7), ("test", "start", 3), ("frames", 30), ("sticks", 60, 200, 260, 30), ("frames", 150)],
    "pause": [("test", "track", 4), ("test", "start", 2), ("frames", 330), ("key", "exit"), ("frames", 20)],
}


def read_png(path):
    """minimal PNG reader for 8-bit RGB/RGBA non-interlaced files -> (w, h, rows of (r,g,b))"""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", chunk)
            assert depth == 8 and ctype in (2, 6) and inter == 0, (depth, ctype, inter)
            bpp = 3 if ctype == 2 else 4
        elif typ == b"IDAT":
            idat += chunk
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line[i:i + 3]) for i in range(0, stride, bpp))
        rows[-1] = list(rows[-1])
        prev = line
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + b"".join(px for px in row) for row in rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def web_render(size, outdir):
    from playwright.sync_api import sync_playwright
    page_url = "file:///" + os.path.join(SIBLING, "web", "simulator.html").replace("\\", "/")
    with sync_playwright() as p:
        b = p.chromium.launch()
        page = b.new_page(viewport={"width": 1600, "height": 1200})
        page.goto(page_url)
        page.wait_for_function("window.sim && window.sim.engine && window.sim.engine.frames > 2", timeout=30000)
        page.evaluate(f"sim.radio('{RADIO[size]}')")
        page.wait_for_function("window.sim.engine && window.sim.engine.frames > 0", timeout=30000)
        for name, steps in SCENES.items():
            page.evaluate("sim.pause(true); sim.boot(); sim.sticks(0, 0, -1, 0)")
            for st in steps:
                if st[0] == "frames":
                    # the Pi's frame interval; radio screens otherwise step 50 ms (the emulator's 20 fps)
                    page.evaluate(f"sim.step({st[1]}, 1000 / 60)")
                elif st[0] == "test":
                    page.evaluate(f"sim.test({', '.join(repr(x) for x in st[1:])})")
                elif st[0] == "key":
                    page.evaluate(f"sim.key('{st[1]}'); sim.step(1, 1000 / 60)")
                elif st[0] == "sticks":
                    page.evaluate(f"sim.sticks({st[1] / 1024}, {st[2] / 1024}, {st[3] / 1024}, {st[4] / 1024})")
            err = page.evaluate("sim.engine.error && sim.engine.error.msg")
            if err:
                print("browser error in", name, err)
            url = page.evaluate("sim.shot()")
            open(os.path.join(outdir, f"{name}_web.png"), "wb").write(base64.b64decode(url.split(",", 1)[1]))
        b.close()


def pi_render(binary, size, outdir):
    # no STICKTIME_DT, like the browser: the same simulation steps on both sides (the color game
    # steps by the host's frame interval when it has one)
    lines = ["frame_dt(false)"]
    for name, steps in SCENES.items():
        lines.append("reload()")
        lines.append("sticks(0, 0, -1024, 0)")
        for st in steps:
            if st[0] == "frames":
                lines.append(f"frames({st[1]})")
            elif st[0] == "test":
                lines.append(f"test({', '.join(repr(x) if isinstance(x, str) else str(x) for x in st[1:])})")
            elif st[0] == "key":
                lines.append(f"key('{st[1]}') frames(1)")
            elif st[0] == "sticks":
                lines.append(f"sticks({st[1]}, {st[2]}, {st[3]}, {st[4]})")
        lines.append(f"if err() then log('pi error in {name}: ' .. err()) end")
        lines.append(f"shot('{name}_pi')")
    lines.append("return true")
    with tempfile.TemporaryDirectory(prefix="stpi-cmp-") as tmp:
        tools = os.path.join(tmp, "base", "sd", "SCRIPTS", "TOOLS")
        os.makedirs(tools)
        shutil.copy(os.path.join(SIBLING, "sdcard", "SCRIPTS", "TOOLS", "StickTime.lua"), tools)
        scen = os.path.join(tmp, "cmp.lua")
        open(scen, "w").write("\n".join(lines) + "\n")
        os.makedirs(os.path.join(tmp, "state"))
        with open(os.path.join(tmp, "sticktime.txt"), "w") as f:
            f.write("show_ip = off\n")              # the Pi's own overlay: not in the browser
        # the browser's PC screen shows frames at once: STICKTIME_LAT = 0 (our default "latency = 0")
        r = subprocess.run([binary, "--scenario", scen, "--base", os.path.join(tmp, "base"),
                            "--settings", os.path.join(tmp, "sticktime.txt"), "--fonts", os.path.join(ROOT, "host", "fonts"),
                            "--state", os.path.join(tmp, "state"), "--png-dir", outdir, "--size", size],
                           capture_output=True, text=True, timeout=1200)
        if r.returncode:
            print(r.stdout, r.stderr)
            raise SystemExit("pi render failed")


def compare(outdir, name):
    wa, ha, a = read_png(os.path.join(outdir, f"{name}_web.png"))
    wb, hb, b = read_png(os.path.join(outdir, f"{name}_pi.png"))
    if (wa, ha) != (wb, hb):
        return f"size differs: web {wa}x{ha}, pi {wb}x{hb}"
    diff, rows, side = 0, [], []
    for y in range(ha):
        row = []
        for x in range(wa):
            pa, pb = a[y][x], b[y][x]
            d = max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2]))
            if d > 48:
                diff += 1
                row.append(b"\xff\x00\xff")
            else:
                g = (pa[0] + pa[1] + pa[2]) // 6
                row.append(bytes((g, g, g)))
        rows.append(row)
        side.append(a[y] + b[y])
    write_png(os.path.join(outdir, f"{name}_diff.png"), wa, ha, rows)
    write_png(os.path.join(outdir, f"{name}_side.png"), wa * 2, ha, side)
    return f"{100.0 * diff / (wa * ha):6.2f}% pixels differ"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "native-edgetx", "sticktime-pi" + EXE))
    ap.add_argument("--sizes", default="1280x720,800x480,480x272")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "compare"))
    a = ap.parse_args()
    for size in a.sizes.split(","):
        out = os.path.join(a.out, size)
        os.makedirs(out, exist_ok=True)
        web_render(size, out)
        pi_render(a.bin, size, out)
        for name in SCENES:
            print(f"{size:10s} {name:12s} {compare(out, name)}")


if __name__ == "__main__":
    sys.exit(main())
