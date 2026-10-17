#!/usr/bin/env python3
"""Make the app package the updater installs: sticktime-pi-app-<ver>.tar.gz (+ .sha256).

    python tools/make_app_package.py --bin build/aarch64-edgetx/sticktime-pi --ver 0.2.1 [--out dist]

It holds bin/sticktime-pi, web/ (without web/tests/), fonts/ when the updater installs them, VERSION
and SHA256SUMS. The updater's own check (stupdate.app.validate_app_tar) must accept it, else this
fails. The release workflow (.github/workflows/image.yml) and the boot test's update check use it.
"""
import argparse
import hashlib
import io
import os
import sys
import tarfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(ROOT, "image", "rootfs", "usr", "lib", "sticktime"))
from stupdate import app  # noqa: E402  (the updater decides what a package may hold)


def build(binary, ver, mtime):
    files = {"bin/sticktime-pi": open(binary, "rb").read()}
    web = os.path.join(ROOT, "web")
    for d, dirs, names in os.walk(web):
        rel = os.path.relpath(d, ROOT).replace(os.sep, "/")
        if rel == "web/tests" or rel.startswith("web/tests/") or "__pycache__" in rel:
            continue                       # the web UI's own tests stay in the repo
        for n in sorted(names):
            if not n.startswith("."):
                files[rel + "/" + n] = open(os.path.join(d, n), "rb").read()
    if "fonts" in app.TOP_DIRS:            # fonts/ only when the updater installs it; the image has them anyway
        fd = os.path.join(ROOT, "host", "fonts")
        for n in sorted(os.listdir(fd)):
            files["fonts/" + n] = open(os.path.join(fd, n), "rb").read()
    files["VERSION"] = (ver + "\n").encode()
    files["SHA256SUMS"] = "".join(f"{hashlib.sha256(b).hexdigest()}  {p}\n" for p, b in sorted(files.items())).encode()
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.USTAR_FORMAT) as tf:
        for p, b in sorted(files.items()):
            ti = tarfile.TarInfo(p)
            ti.size, ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = len(b), mtime, 0, 0, "root", "root"
            ti.mode = 0o755 if p.startswith("bin/") else 0o644
            tf.addfile(ti, io.BytesIO(b))
    data = buf.getvalue()
    pkg = app.validate_app_tar(data, ver)  # raises when the updater would refuse it
    return data, len(pkg.files)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True, help="the static aarch64 sticktime-pi, built with VERSION=<ver>")
    ap.add_argument("--ver", required=True, help="the version (digits and dots)")
    ap.add_argument("--out", default="dist", help="output folder")
    a = ap.parse_args()
    mtime = int(os.environ.get("SOURCE_DATE_EPOCH") or 0) or 1700000000
    data, n = build(a.bin, a.ver, mtime)
    os.makedirs(a.out, exist_ok=True)
    name = f"sticktime-pi-app-{a.ver}.tar.gz"
    with open(os.path.join(a.out, name), "wb") as f:
        f.write(data)
    with open(os.path.join(a.out, name + ".sha256"), "w", encoding="ascii", newline="\n") as f:
        f.write(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
    print(f"{a.out}/{name}: {len(data)} bytes, {n} files, accepted by the updater")
    return 0


if __name__ == "__main__":
    sys.exit(main())
