#!/usr/bin/env python3
"""Run the headless game scenarios (host/tests/scenarios/*.lua) against a sticktime-pi build.

  python host/tests/run_scenarios.py [--bin PATH] [--game StickTime.lua] [--sizes 640x480,1280x720]
                                     [--only name,name] [--out DIR] [--quick]

Each scenario runs in a fresh temporary base folder (sd/ with the game, no settings file, so the
host writes the template) at each render size. Screenshots go to OUT/<size>/<scenario>/.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SIBLING = os.path.abspath(os.path.join(ROOT, "..", "sticktime"))
EXE = ".exe" if os.name == "nt" else ""


def default_bin():
    for d in ("native-edgetx", "x86_64-linux-edgetx"):
        p = os.path.join(ROOT, "build", d, "sticktime-pi" + EXE)
        if os.path.exists(p):
            return p
    return os.path.join(ROOT, "build", "native-edgetx", "sticktime-pi" + EXE)


def make_base(tmp, game):
    tools = os.path.join(tmp, "base", "sd", "SCRIPTS", "TOOLS")
    os.makedirs(os.path.join(tools, "StickTimeTracks"))
    os.makedirs(os.path.join(tools, "StickTimeGhosts"))
    shutil.copy(game, os.path.join(tools, "StickTime.lua"))
    os.makedirs(os.path.join(tmp, "state"))
    return os.path.join(tmp, "base")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=default_bin())
    ap.add_argument("--game", default=os.path.join(SIBLING, "sdcard", "SCRIPTS", "TOOLS", "StickTime.lua"))
    ap.add_argument("--sizes", default="640x480,1280x720")
    ap.add_argument("--only", default="")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "scenario-shots"))
    ap.add_argument("--quick", action="store_true", help="skip long_run")
    ap.add_argument("--timeout", type=int, default=1800)
    a = ap.parse_args()
    scen_dir = os.path.join(HERE, "scenarios")
    names = sorted(f[:-4] for f in os.listdir(scen_dir) if f.endswith(".lua") and f != "lib.lua")
    if a.only:
        names = [n for n in names if n in a.only.split(",")]
    if a.quick:
        names = [n for n in names if n != "long_run"]
    env = dict(os.environ)
    env["STICKTIME_GALLERY_TRACK"] = os.path.join(SIBLING, "tracks", "multigp-2017-utt.txt")
    results = []
    for size in a.sizes.split(","):
        for n in names:
            with tempfile.TemporaryDirectory(prefix="stpi-scen-") as tmp:
                base = make_base(tmp, a.game)
                out = os.path.join(a.out, size, n)
                os.makedirs(out, exist_ok=True)
                cmd = [a.bin, "--scenario", os.path.join(scen_dir, n + ".lua"), "--base", base,
                       "--settings", os.path.join(tmp, "sticktime.txt"), "--fonts", os.path.join(ROOT, "host", "fonts"),
                       "--state", os.path.join(tmp, "state"), "--png-dir", out, "--size", size]
                t0 = time.time()
                try:
                    r = subprocess.run(cmd, capture_output=True, text=True, timeout=a.timeout, env=env)
                    ok, log = r.returncode == 0, r.stdout + r.stderr
                except subprocess.TimeoutExpired as e:
                    ok, log = False, "TIMEOUT\n" + (e.stdout or "") + (e.stderr or "") if isinstance(e.stdout, str) else "TIMEOUT"
                dt = time.time() - t0
                results.append((size, n, ok, dt))
                with open(os.path.join(out, "log.txt"), "w", encoding="utf-8") as f:
                    f.write(log)
                print(f"{'PASS' if ok else 'FAIL'} {size:10s} {n:18s} {dt:6.1f} s")
                if not ok:
                    print("    " + "\n    ".join(log.strip().splitlines()[-15:]))
    failed = [r for r in results if not r[2]]
    print(f"{len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
