#!/usr/bin/env python3
"""Make the app package the updater installs: sticktime-pi-app-<ver>.tar.gz (+ .sha256).

    python tools/make_app_package.py --bin build/aarch64-edgetx/sticktime-pi --ver 0.2.1 [--out dist]
        [--gpu build/aarch64-edgetx/sticktime-gpu]
    python tools/make_app_package.py --lib-dir DIR      (only lib/'s files, for image/build.sh)

It holds bin/sticktime-pi (and bin/sticktime-gpu, the GPU drawing helper, when given), web/ (without
web/tests/), fonts/ when the updater installs them, lib/
(the system scripts, see lib_files), VERSION and SHA256SUMS. The updater's own check
(stupdate.app.validate_app_tar) must accept it, else this fails. The release workflow
(.github/workflows/image.yml) and the boot test's update check use it.
"""
import argparse
import hashlib
import io
import os
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
LIB = os.path.join(ROOT, "image", "rootfs", "usr", "lib", "sticktime")
sys.path.insert(0, LIB)
from stupdate import app  # noqa: E402  (the updater decides what a package may hold)


def _read(path):
    with open(path, "rb") as f:
        return f.read()


def _lf(b):
    return b.replace(b"\r\n", b"\n")      # a Windows checkout: a #! line with \r doesn't run


def lib_files():
    """lib/: the system scripts exactly as the image installs them in /usr/lib/sticktime, which the
    updater syncs from here (stupdate/system.py): image/rootfs/usr/lib/sticktime, tools/patch_editor.py
    and the default settings file made from host/src/settings_keys.h. {rel: bytes}"""
    out = {}
    for d, dirs, names in os.walk(LIB):
        dirs[:] = sorted(x for x in dirs if x != "__pycache__" and not x.startswith("."))
        for n in sorted(names):
            if n.startswith(".") or n.endswith(".pyc"):
                continue
            rel = os.path.relpath(os.path.join(d, n), LIB).replace(os.sep, "/")
            out[rel] = _lf(_read(os.path.join(d, n)))
    out["patch_editor.py"] = _lf(_read(os.path.join(ROOT, "tools", "patch_editor.py")))
    with tempfile.TemporaryDirectory() as tmp:
        t = os.path.join(tmp, "sticktime.txt")
        subprocess.run([sys.executable, "-I", os.path.join(LIB, "sticktime-setup"), "extract-template",
                        os.path.join(ROOT, "host", "src", "settings_keys.h"), t], check=True)
        out["sticktime.txt.default"] = _lf(_read(t))
    return out


def _aarch64(b):
    return len(b) >= 64 and b[:4] == b"\x7fELF" and b[4] == 2 and b[5] == 1 and int.from_bytes(b[18:20], "little") == 183


def build(binary, ver, mtime, lib_extra=None, gpu=None):
    """lib_extra: {rel: bytes or None} changes to lib/ (the boot test's sync check); gpu: the GPU helper"""
    files = {"bin/sticktime-pi": _read(binary)}
    if gpu:
        files["bin/sticktime-gpu"] = _read(gpu)
        if not _aarch64(files["bin/sticktime-gpu"]):
            raise ValueError("%s isn't a 64-bit ARM program" % gpu)
    lib = lib_files()
    for rel, b in (lib_extra or {}).items():
        if b is None:
            lib.pop(rel, None)
        else:
            lib[rel] = b
    files.update({"lib/" + rel: b for rel, b in lib.items()})
    web = os.path.join(ROOT, "web")
    for d, dirs, names in os.walk(web):
        rel = os.path.relpath(d, ROOT).replace(os.sep, "/")
        if rel == "web/tests" or rel.startswith("web/tests/") or "__pycache__" in rel:
            continue                       # the web UI's own tests stay in the repo
        for n in sorted(names):
            if not n.startswith("."):
                files[rel + "/" + n] = _read(os.path.join(d, n))
    if "fonts" in app.TOP_DIRS:            # fonts/ only when the updater installs it; the image has them anyway
        fd = os.path.join(ROOT, "host", "fonts")
        for n in sorted(os.listdir(fd)):
            files["fonts/" + n] = _read(os.path.join(fd, n))
    files["VERSION"] = (ver + "\n").encode()
    files["SHA256SUMS"] = "".join(f"{hashlib.sha256(b).hexdigest()}  {p}\n" for p, b in sorted(files.items())).encode()
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.USTAR_FORMAT) as tf:
        for p, b in sorted(files.items()):
            ti = tarfile.TarInfo(p)
            ti.size, ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = len(b), mtime, 0, 0, "root", "root"
            ti.mode = 0o755 if p.startswith("bin/") or (p.startswith("lib/") and b.startswith(b"#!")) else 0o644
            tf.addfile(ti, io.BytesIO(b))
    data = buf.getvalue()
    pkg = app.validate_app_tar(data, ver)  # raises when the updater would refuse it
    return data, len(pkg.files)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", help="the static aarch64 sticktime-pi, built with VERSION=<ver>")
    ap.add_argument("--ver", help="the version (digits and dots)")
    ap.add_argument("--out", default="dist", help="output folder")
    ap.add_argument("--lib-dir", help="only write lib/'s files into this folder (image/build.sh)")
    ap.add_argument("--gpu", help="the GPU drawing helper sticktime-gpu (aarch64, glibc), for bin/")
    a = ap.parse_args()
    if a.lib_dir:
        for rel, b in lib_files().items():
            p = os.path.join(a.lib_dir, *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(b)
            os.chmod(p, 0o755 if b.startswith(b"#!") else 0o644)
        return 0
    if not a.bin or not a.ver:
        ap.error("--bin and --ver are needed (or --lib-dir)")
    mtime = int(os.environ.get("SOURCE_DATE_EPOCH") or 0) or 1700000000
    data, n = build(a.bin, a.ver, mtime, gpu=a.gpu)
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
