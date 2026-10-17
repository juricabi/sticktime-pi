#!/usr/bin/env python3
"""Boot the StickTime Pi image under QEMU (-M raspi3b) and check it (pios-image.md section 7).

    python3 image/test/boot_test.py out/sticktime-pi-<ver>.img.xz [--work DIR] [--qemu PATH] [--quick]

Works with QEMU on Linux (CI: qemu-system-aarch64 >= 8.2) and with QEMU for Windows. Stdlib only:
the image's FAT partition is read and written by a small FAT32 module here (kernel, initramfs and
dtb out, a CI sticktime.txt in), the dtb is edited by a small FDT module (the PL011 is taken from
the Bluetooth serdev so it gives a real serial console), and the checks run as user pi on that
serial console (QEMU chardev socket), so no ssh client or sshpass is needed. SSH itself is checked
too: its banner on the forwarded port, and a password login when an ssh client that can take the
password from SSH_ASKPASS is there.

The boots:
  1. first boot (with 'resize'): resize, first-boot units, sticktime-setup applies the CI settings
     (ssh on, a password, hostname sticktime-ci, a WiFi network); then the checks; then the test
     edits sticktime.txt (hostname, video = hdmi) and reboots
  2. sticktime-setup sees the video change, rewrites config.txt and reboots once by itself
  3. no second reboot (the guard), the new hostname and config; then ssh = off and a reboot
  4. ssh is off, the web port still answers
--quick stops after boot 1. Exit status 0 when every check passed. A JSON report and the serial
logs go to the work folder.
"""
import argparse
import datetime
import json
import lzma
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
# Windows: $QEMU, else the installer's default folder, else qemu-system-aarch64 on the PATH
WIN_QEMU = os.environ.get("QEMU") or r"C:\Program Files\qemu\qemu-system-aarch64.exe"
DTB = "bcm2710-rpi-3-b-plus.dtb"
CI_HOST1, CI_HOST2 = "sticktime-ci", "stci-two"
# the emulated SD host (sdhost-bcm2835, mmc0) reporting an error. Some recover; a boot that then
# fails with one of these in its log failed because QEMU's card emulation stalled, not the image
SD_FLAKE = r"sdhost-bcm2835 [0-9a-f]+\.mmc: (fsm \d+, hsts 0000010[0-9a-f]|PIO read timeout|.*timeout)"
# a boot that went wrong (fail at once instead of waiting for the timeout)
BOOT_FAILED = (r"Kernel panic|end Kernel panic|You are in emergency mode|Give root password for maintenance|"
               r"No init found|\(initramfs\) |EXT4-fs error")


def log(msg):
    print(f"[{datetime.datetime.now():%H:%M:%S}] {msg}", flush=True)


# ================================================================ FAT32 (read, and rewrite a file)
class Fat32:
    """the FAT32 file system at byte `offset` of an image file. Reads files; replaces the content of
    an existing file (new clusters, both FATs updated, FSInfo free count set to 'unknown')."""

    def __init__(self, path, offset):
        self.path, self.off = path, offset
        with open(path, "rb") as f:
            f.seek(offset)
            bs = f.read(512)
        if bs[510:512] != b"\x55\xaa":
            raise ValueError("no FAT boot sector")
        self.bps = struct.unpack_from("<H", bs, 11)[0]
        self.spc = bs[13]
        rsv = struct.unpack_from("<H", bs, 14)[0]
        self.nfats = bs[16]
        if struct.unpack_from("<H", bs, 22)[0] != 0:
            raise ValueError("FAT12/16 is not supported (Raspberry Pi OS uses FAT32)")
        fatsz = struct.unpack_from("<I", bs, 36)[0]
        tot = struct.unpack_from("<H", bs, 19)[0] or struct.unpack_from("<I", bs, 32)[0]
        self.root = struct.unpack_from("<I", bs, 44)[0]
        self.fsinfo = struct.unpack_from("<H", bs, 48)[0]
        self.csize = self.bps * self.spc
        self.fat_off = offset + rsv * self.bps
        self.fat_bytes = fatsz * self.bps
        self.data_off = self.fat_off + self.nfats * self.fat_bytes
        self.nclus = (tot - rsv - self.nfats * fatsz) // self.spc
        with open(path, "rb") as f:
            f.seek(self.fat_off)
            self.fat = bytearray(f.read(self.fat_bytes))

    def ent(self, c):
        return struct.unpack_from("<I", self.fat, 4 * c)[0] & 0x0FFFFFFF

    def set_ent(self, c, v):
        old = struct.unpack_from("<I", self.fat, 4 * c)[0]
        struct.pack_into("<I", self.fat, 4 * c, (old & 0xF0000000) | (v & 0x0FFFFFFF))

    def chain(self, c):
        out = []
        while 2 <= c < 0x0FFFFFF8:
            if len(out) > self.nclus:
                raise ValueError("FAT chain loop")
            out.append(c)
            c = self.ent(c)
        return out

    def cl_off(self, c):
        return self.data_off + (c - 2) * self.csize

    def read_chain(self, c, size=None):
        parts = []
        with open(self.path, "rb") as f:
            for x in self.chain(c):
                f.seek(self.cl_off(x))
                parts.append(f.read(self.csize))
        b = b"".join(parts)
        return b if size is None else b[:size]

    def entries(self, c):
        """(name, attr, cluster, size, absolute offset of the 32-byte entry)"""
        cl = self.chain(c)
        d = self.read_chain(c)
        lfn = []
        for i in range(0, len(d), 32):
            e = d[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            if e[11] == 0x0F:
                lfn.insert(0, (e[1:11] + e[14:26] + e[28:32]).decode("utf-16le", "replace").split("\x00")[0])
                continue
            if e[11] & 0x08:                      # volume label
                lfn = []
                continue
            base, ext = e[0:8].decode("latin-1").rstrip(), e[8:11].decode("latin-1").rstrip()
            if e[12] & 0x08:
                base = base.lower()
            if e[12] & 0x10:
                ext = ext.lower()
            name = "".join(lfn) or (base + ("." + ext if ext else ""))
            lfn = []
            clus = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            absoff = self.cl_off(cl[i // self.csize]) + i % self.csize
            yield name, e[11], clus, size, absoff

    def find(self, path):
        c = self.root
        parts = [p for p in path.strip("/").split("/") if p]
        for n, part in enumerate(parts):
            for name, attr, clus, size, off in self.entries(c):
                if name.lower() == part.lower():
                    if n == len(parts) - 1:
                        return name, attr, clus, size, off
                    if not attr & 0x10:
                        raise FileNotFoundError(path)
                    c = clus
                    break
            else:
                raise FileNotFoundError(path)
        raise FileNotFoundError(path)

    def read(self, path):
        _n, attr, clus, size, _o = self.find(path)
        if attr & 0x10:
            raise IsADirectoryError(path)
        return self.read_chain(clus, size) if size else b""

    def listdir(self, path="/"):
        c = self.root if path.strip("/") == "" else self.find(path)[2]
        return [n for n, *_ in self.entries(c) if n not in (".", "..")]

    def write(self, path, data):
        """replace an existing file's content"""
        _n, attr, clus, _size, off = self.find(path)
        if attr & 0x10:
            raise IsADirectoryError(path)
        for c in self.chain(clus):
            self.set_ent(c, 0)
        need = (len(data) + self.csize - 1) // self.csize
        free = []
        c = 2
        while len(free) < need and c < self.nclus + 2:
            if self.ent(c) == 0:
                free.append(c)
            c += 1
        if len(free) < need:
            raise OSError("the FAT partition is full")
        for a, b in zip(free, free[1:]):
            self.set_ent(a, b)
        if free:
            self.set_ent(free[-1], 0x0FFFFFFF)
        now = time.localtime()
        ftime = (now.tm_hour << 11) | (now.tm_min << 5) | (now.tm_sec // 2)
        fdate = ((max(now.tm_year, 1980) - 1980) << 9) | (now.tm_mon << 5) | now.tm_mday
        first = free[0] if free else 0
        with open(self.path, "r+b") as f:
            for i, c in enumerate(free):
                f.seek(self.cl_off(c))
                f.write(data[i * self.csize:(i + 1) * self.csize].ljust(self.csize, b"\0"))
            f.seek(off + 20)
            f.write(struct.pack("<H", first >> 16))
            f.seek(off + 22)
            f.write(struct.pack("<HHH", ftime, fdate, first & 0xFFFF))
            f.write(struct.pack("<I", len(data)))
            for k in range(self.nfats):
                f.seek(self.fat_off + k * self.fat_bytes)
                f.write(self.fat)
            if self.fsinfo and self.fsinfo != 0xFFFF:
                fi = self.off + self.fsinfo * self.bps
                f.seek(fi)
                sec = f.read(512)
                if sec[0:4] == b"RRaA" and sec[484:488] == b"rrAa":
                    f.seek(fi + 488)
                    f.write(struct.pack("<II", 0xFFFFFFFF, 0xFFFFFFFF))    # free count / next free: unknown


def partitions(path):
    with open(path, "rb") as f:
        mbr = f.read(512)
    if mbr[510:512] != b"\x55\xaa":
        raise ValueError(f"{path}: no MBR partition table")
    out = []
    for i in range(4):
        e = mbr[446 + 16 * i:446 + 16 * (i + 1)]
        if e[4]:
            out.append({"type": e[4], "start": struct.unpack_from("<I", e, 8)[0], "sectors": struct.unpack_from("<I", e, 12)[0]})
    return out


# ================================================================ FDT (device tree blob) editing
class Fdt:
    """parse a .dtb into nodes, change properties, write it back (version 17 layout)"""

    def __init__(self, data):
        (magic, total, off_struct, off_strings, off_rsv, ver, _lcv, self.boot_cpu, size_strings,
         size_struct) = struct.unpack_from(">10I", data, 0)
        if magic != 0xD00DFEED:
            raise ValueError("not a device tree blob")
        strings = data[off_strings:off_strings + size_strings]
        self.rsv = []
        p = off_rsv
        while True:
            a, s = struct.unpack_from(">QQ", data, p)
            p += 16
            if a == 0 and s == 0:
                break
            self.rsv.append((a, s))
        self.root = None
        stack = []
        p = off_struct
        while True:
            tok = struct.unpack_from(">I", data, p)[0]
            p += 4
            if tok == 1:
                e = data.index(b"\0", p)
                node = {"name": data[p:e].decode(), "props": [], "children": []}
                p = (e + 4) & ~3
                if stack:
                    stack[-1]["children"].append(node)
                else:
                    self.root = node
                stack.append(node)
            elif tok == 2:
                stack.pop()
            elif tok == 3:
                ln, no = struct.unpack_from(">II", data, p)
                p += 8
                name = strings[no:strings.index(b"\0", no)].decode()
                stack[-1]["props"].append([name, data[p:p + ln]])
                p = (p + ln + 3) & ~3
            elif tok == 4:
                continue
            elif tok == 9:
                break
            else:
                raise ValueError(f"bad FDT token {tok}")

    def node(self, path):
        n = self.root
        for part in [x for x in path.split("/") if x]:
            for ch in n["children"]:
                if ch["name"] == part:
                    n = ch
                    break
            else:
                return None
        return n

    def set_string(self, path, prop, value):
        n = self.node(path)
        if n is None:
            return False
        v = value.encode() + b"\0"
        for pr in n["props"]:
            if pr[0] == prop:
                pr[1] = v
                return True
        n["props"].append([prop, v])
        return True

    def get(self, path, prop):
        n = self.node(path)
        for pr in (n["props"] if n else []):
            if pr[0] == prop:
                return pr[1]
        return None

    def dump(self):
        strings, sidx = bytearray(), {}
        out = bytearray()

        def sref(name):
            if name not in sidx:
                sidx[name] = len(strings)
                strings.extend(name.encode() + b"\0")
            return sidx[name]

        def emit(n):
            out.extend(struct.pack(">I", 1))
            nm = n["name"].encode() + b"\0"
            out.extend(nm + b"\0" * ((4 - len(nm) % 4) % 4))
            for name, val in n["props"]:
                out.extend(struct.pack(">III", 3, len(val), sref(name)))
                out.extend(val + b"\0" * ((4 - len(val) % 4) % 4))
            for ch in n["children"]:
                emit(ch)
            out.extend(struct.pack(">I", 2))
        emit(self.root)
        out.extend(struct.pack(">I", 9))
        rsv = b"".join(struct.pack(">QQ", a, s) for a, s in self.rsv) + struct.pack(">QQ", 0, 0)
        off_rsv = 40
        off_struct = off_rsv + len(rsv)
        off_strings = off_struct + len(out)
        total = off_strings + len(strings)
        hdr = struct.pack(">10I", 0xD00DFEED, total, off_struct, off_strings, off_rsv, 17, 16, self.boot_cpu,
                          len(strings), len(out))
        return hdr + rsv + bytes(out) + bytes(strings)


# ================================================================ helpers
def free_port(preferred=0):
    for port in ([preferred] if preferred else []) + [0]:
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", port))
            return s.getsockname()[1]
        except OSError:
            continue
        finally:
            s.close()
    raise OSError("no free port")


def ppm_to_png(ppm_path, png_path):
    data = open(ppm_path, "rb").read()
    m = re.match(rb"P6\s+(?:#[^\n]*\n\s*)*(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        raise ValueError("not a binary PPM")
    w, h, mx = int(m.group(1)), int(m.group(2)), int(m.group(3))
    px = data[m.end():m.end() + w * h * 3]
    raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(png_path, "wb").write(png)
    nonblack = sum(1 for i in range(0, len(px), 3 * 97) if px[i:i + 3] != b"\0\0\0")
    return w, h, nonblack


def http_get(port, path, timeout=10):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", headers={"Host": f"127.0.0.1:{port}"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read(1 << 20)


def ssh_banner(port, timeout=10):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
            s.settimeout(timeout)
            return s.recv(256).decode("latin-1", "replace").strip()
    except OSError as e:
        return f"error: {e}"


def ssh_password_login(port, password, work, cmd="hostname"):
    """(ok, output or reason): log in as pi with the password through SSH_ASKPASS (OpenSSH >= 8.4)"""
    ssh = shutil.which("ssh")
    if not ssh:
        return None, "no ssh client"
    if os.name == "nt":
        # no CR LF after the password (ssh would send the CR): set /p prints without a line end
        askpass = os.path.join(work, "askpass.cmd")
        with open(askpass, "w", newline="\r\n") as f:
            f.write(f'@<nul set /p "={password}"\n')
    else:
        askpass = os.path.join(work, "askpass.sh")
        with open(askpass, "w", newline="\n") as f:
            f.write(f"#!/bin/sh\necho '{password}'\n")
        os.chmod(askpass, 0o700)
    env = dict(os.environ, SSH_ASKPASS=askpass, SSH_ASKPASS_REQUIRE="force", DISPLAY=os.environ.get("DISPLAY", ":0"))
    kh = os.path.join(work, "known_hosts")
    argv = [ssh, "-p", str(port), "-o", "StrictHostKeyChecking=no", "-o", f"UserKnownHostsFile={kh}",
            "-o", "PreferredAuthentications=password,keyboard-interactive", "-o", "PubkeyAuthentication=no",
            "-o", "NumberOfPasswordPrompts=1", "-o", "ConnectTimeout=20", "pi@127.0.0.1", cmd]
    try:
        r = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=90, stdin=subprocess.DEVNULL)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, f"ssh did not run: {e}"
    if r.returncode == 0:
        return True, r.stdout.strip()
    return False, (r.stderr.strip().splitlines() or [f"exit {r.returncode}"])[-1]


class Console:
    """the guest's serial console through QEMU's chardev socket: wait for text, log in, run commands"""

    def __init__(self, port):
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.sock = None
        for _ in range(100):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                time.sleep(0.2)
        if self.sock is None:
            raise OSError("cannot connect to the serial port")
        self.sock.settimeout(None)
        self.alive = True
        threading.Thread(target=self._reader, daemon=True).start()
        self.n = 0

    def _reader(self):
        while True:
            try:
                d = self.sock.recv(65536)
            except OSError:
                d = b""
            if not d:
                self.alive = False
                return
            with self.lock:
                self.buf.extend(d)

    def mark(self):
        with self.lock:
            return len(self.buf)

    def text(self, start=0):
        with self.lock:
            return bytes(self.buf[start:]).decode("utf-8", "replace")

    def wait(self, pattern, timeout, start=0, proc=None):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            m = rx.search(self.text(start))
            if m:
                return m
            if proc is not None and proc.poll() is not None:
                return None
            time.sleep(0.2)
        return None

    def send(self, s):
        self.sock.sendall(s.encode("utf-8"))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def login(self, user, password, timeout=180):
        end = time.time() + timeout
        while time.time() < end:
            m0 = self.mark()
            self.send("\r")
            if self.wait(r"login: ?$", 8, m0) or self.wait(r"login:", 1, m0):
                m1 = self.mark()
                self.send(user + "\r")
                if not self.wait(r"[Pp]assword: ?", 20, m1):
                    continue
                m2 = self.mark()
                self.send(password + "\r")
                m = self.wait(r"(\$ ?$|Login incorrect)", 40, m2)
                if m and "incorrect" not in m.group(0):
                    self.run("stty -echo cols 250 2>/dev/null; export TERM=dumb SYSTEMD_PAGER= SYSTEMD_COLORS=0 PAGER=cat "
                             "SYSTEMD_URLIFY=0 LC_ALL=C", 20)
                    return True
                if m:
                    return False
            elif self.wait(r"\$ ?$", 1, m0):
                return True                     # still logged in from before
        return False

    def run(self, cmd, timeout=60):
        """(exit status, output) of a shell command (None, partial output on a timeout)"""
        self.n += 1
        n = self.n
        start = self.mark()
        self.send(f"printf '\\n%s_%s\\n' __B {n}; {cmd}; printf '\\n%s_%s_%s\\n' __E {n} $?\r")
        m = self.wait(rf"__E_{n}_(\d+)", timeout, start)
        t = self.text(start).replace("\r\n", "\n").replace("\r", "")
        b = t.find(f"__B_{n}\n")
        e = t.find(f"\n__E_{n}_")
        if not m:
            return None, t[b + len(f"__B_{n}\n"):] if b >= 0 else t
        return int(m.group(1)), (t[b + len(f"__B_{n}\n"):e] if b >= 0 and e >= 0 else "").strip("\n")

    def sudo(self, cmd, password, timeout=60):
        return self.run(f"printf '%s\\n' '{password}' | sudo -S -p '' sh -c '{cmd}'", timeout)


class Monitor:
    def __init__(self, port):
        self.port = port

    def cmd(self, line, timeout=20):
        try:
            with socket.create_connection(("127.0.0.1", self.port), timeout=timeout) as s:
                s.settimeout(timeout)
                buf = b""
                end = time.time() + timeout
                while b"(qemu)" not in buf and time.time() < end:
                    buf += s.recv(4096)
                s.sendall(line.encode() + b"\n")
                buf = b""
                while buf.count(b"(qemu)") < 1 and time.time() < end:
                    d = s.recv(4096)
                    if not d:
                        break
                    buf += d
                return buf.decode("utf-8", "replace")
        except OSError as e:
            return f"error: {e}"


# ================================================================ the test
class BootTest:
    def __init__(self, a):
        self.a = a
        self.work = os.path.abspath(a.work)
        os.makedirs(self.work, exist_ok=True)
        self.img = os.path.join(self.work, "test.img")
        self.results = []
        self.boots = []
        self.password = a.password
        self.proc = None
        self.console = None
        self.qemu = a.qemu or (WIN_QEMU if os.name == "nt" and os.path.exists(WIN_QEMU) else shutil.which("qemu-system-aarch64"))
        if not self.qemu or not os.path.exists(self.qemu) and not shutil.which(self.qemu):
            raise SystemExit("qemu-system-aarch64 not found (--qemu PATH)")
        self.icount = a.icount
        self.ssh_port = free_port(a.ssh_port)
        self.http_port = free_port(a.http_port)

    # ---- results
    def check(self, name, ok, detail=""):
        ok = bool(ok)
        self.results.append({"check": name, "ok": ok, "detail": str(detail)[:2000]})
        log(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {str(detail)[:300]}" if detail else ""))
        return ok

    def info(self, name, detail):
        self.results.append({"check": name, "ok": None, "detail": str(detail)[:4000]})
        log(f"INFO  {name}: {str(detail)[:600]}")

    # ---- preparation
    def prepare(self):
        src = self.a.image
        log(f"image {src}")
        if src.endswith(".xz"):
            # bounded output per call: a trimmed (zeroed) gigabyte compresses to a few kilobytes
            dec = lzma.LZMADecompressor()
            with open(src, "rb") as f, open(self.img, "wb") as o:
                while not dec.eof:
                    chunk = b""
                    if dec.needs_input:
                        chunk = f.read(1 << 20)
                        if not chunk:
                            raise SystemExit(f"{src}: the xz data ends early")
                    o.write(dec.decompress(chunk, max_length=1 << 26))
        else:
            shutil.copyfile(src, self.img)
        size = os.path.getsize(self.img)
        p2 = 1
        while p2 < size:
            p2 <<= 1
        with open(self.img, "r+b") as f:
            f.truncate(p2)                   # QEMU wants a power-of-2 SD card (hw/sd/sd.c)
        log(f"SD card {size} -> {p2} bytes")
        parts = partitions(self.img)
        self.check("partition table: FAT boot + Linux root", len(parts) >= 2 and parts[0]["type"] in (0x0B, 0x0C)
                   and parts[1]["type"] == 0x83, parts)
        self.fat = Fat32(self.img, parts[0]["start"] * 512)
        q = os.path.join(self.work, "boot")
        os.makedirs(q, exist_ok=True)
        kdir = self.a.kernel_dir
        for name in ("kernel8.img", "initramfs8", DTB):
            if kdir and os.path.exists(os.path.join(kdir, name)):
                shutil.copyfile(os.path.join(kdir, name), os.path.join(q, name))
            else:
                with open(os.path.join(q, name), "wb") as f:
                    f.write(self.fat.read(name))
        fdt = Fdt(open(os.path.join(q, DTB), "rb").read())
        if fdt.set_string("/soc/serial@7e201000/bluetooth", "status", "disabled"):
            open(os.path.join(q, DTB), "wb").write(fdt.dump())
            again = Fdt(open(os.path.join(q, DTB), "rb").read())
            self.check("dtb: PL011 freed from the Bluetooth serdev", again.get("/soc/serial@7e201000/bluetooth", "status") == b"disabled\0")
        else:
            self.info("dtb", "no /soc/serial@7e201000/bluetooth node (left as it is)")
        self.kernel, self.initrd, self.dtb = (os.path.join(q, n) for n in ("kernel8.img", "initramfs8", DTB))
        # what the image carries on its FAT
        ver = self.fat.read("sticktime/VERSION").decode().strip()
        gv = self.fat.read("sticktime/sd/SCRIPTS/TOOLS/GAME_VERSION").decode().strip()
        self.info("image contents", f"app {ver}, game {gv}, sticktime/: {sorted(self.fat.listdir('sticktime'))}")
        cmdline = self.fat.read("cmdline.txt").decode()
        config = self.fat.read("config.txt").decode()
        self.check("cmdline.txt baked (one line, resize, quiet, tv norm)", cmdline.count("\n") == 1 and " resize" in cmdline
                   and "vc4.tv_norm=NTSC" in cmdline and " quiet" in cmdline, cmdline.strip())
        self.check("config.txt block baked (composite)", "\ndtoverlay=vc4-kms-v3d,composite" in config and "\nenable_tvout=1" in config)
        self.check("no cloud-init seed files on the FAT", not any(n in self.fat.listdir("/") for n in ("user-data", "meta-data", "network-config")))
        self.orig_cmdline = cmdline
        # the CI settings: ssh on with a password, a hostname, a WiFi network (no WiFi under QEMU)
        txt = self.fat.read("sticktime.txt").decode("utf-8")
        for k, v in (("ssh", "on"), ("password", self.password), ("hostname", CI_HOST1), ("wifi_ssid", "CI Net"),
                     ("wifi_password", "ci-wifi-pass")):
            txt, n = re.subn(rf"(?m)^{k} =.*$", f"{k} = {v}", txt)
            if n != 1:
                raise SystemExit(f"sticktime.txt has no single '{k} =' line")
        self.fat.write("sticktime.txt", txt.encode("utf-8"))
        self.check("CI sticktime.txt written to the FAT", f"hostname = {CI_HOST1}" in self.fat.read("sticktime.txt").decode())

    # ---- QEMU
    def launch(self, n, first):
        if self.console:
            self.console.close()
        ser = free_port()
        self.mon_port = free_port()
        serial_log = os.path.join(self.work, f"serial-{n}.log")
        append = ("console=tty1 root=/dev/mmcblk0p2 rootfstype=ext4 fsck.repair=yes rootwait"
                  + (" resize" if first else "")
                  + " watchdog_disarm=0 systemd.watchdog_sec=off loglevel=6 dwc_otg.lpm_enable=0 sticktime.ci=1"
                  " console=ttyAMA1,115200")
        # -icount: the guest's clock follows its instructions, so a stalled host (an antivirus reading the
        # 4 GB card file, a busy CI runner) can't make the SD driver time out; slower (one host thread)
        icount = ["-icount", "shift=auto"] if self.icount else []
        argv = [self.qemu, "-M", "raspi3b", "-m", "1G"] + icount + ["-kernel", self.kernel, "-initrd", self.initrd, "-dtb", self.dtb,
                "-drive", f"file={self.img},if=sd,format=raw", "-append", append,
                "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{self.ssh_port}-:22,hostfwd=tcp:127.0.0.1:{self.http_port}-:80",
                "-device", "usb-net,netdev=n0", "-device", "usb-kbd",
                "-display", "none", "-no-reboot",
                "-chardev", f"socket,id=s0,host=127.0.0.1,port={ser},server=on,wait=off,logfile={serial_log.replace(os.sep, '/')}",
                "-serial", "chardev:s0",
                "-monitor", f"tcp:127.0.0.1:{self.mon_port},server=on,wait=off"]
        log(f"boot {n}: starting QEMU (ssh {self.ssh_port}, http {self.http_port}{', icount' if self.icount else ''})")
        self.qlog = open(os.path.join(self.work, f"qemu-{n}.log"), "wb")
        # cwd = the work folder: monitor commands then take bare file names (HMP splits a path at spaces)
        self.proc = subprocess.Popen(argv, stdout=self.qlog, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, cwd=self.work)
        self.t0 = time.time()
        self.console = Console(ser)
        return self.console

    def wait_qemu_exit(self, timeout):
        try:
            self.proc.wait(timeout)
            return True
        except subprocess.TimeoutExpired:
            return False

    def stop(self):
        if self.proc and self.proc.poll() is None:
            Monitor(self.mon_port).cmd("quit", 5)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(10)
        if self.console:
            self.console.close()
            self.console = None

    def wait_boot(self, n, expect_setup_reboot=False, timeout=None):
        """until 'STICKTIME-CI: ready' (or QEMU's exit when setup reboots); returns a dict of times"""
        c = self.console
        timeout = timeout or (self.a.first_timeout if n == 1 else self.a.timeout)
        t = {"boot": n}
        bad = BOOT_FAILED
        end = self.t0 + timeout
        seen_setup = False
        login_at = None
        while time.time() < end:
            txt = c.text()
            if login_at is None and re.search(r"login: ?$", txt):
                login_at = time.time()
            elif login_at and time.time() - login_at > self.a.ready_grace:
                # a login prompt but no marker (sticktime-ci.service broken?): go on, the check fails
                t["ready_missing"] = True
                t["login_s"] = round(login_at - self.t0, 1)
                break
            if not seen_setup:
                m = re.search(r"STICKTIME-CI: setup done hostname=(\S+) reboot=(\d)[^\n]*", txt)
                if m:
                    seen_setup = True
                    t["setup_s"] = round(time.time() - self.t0, 1)
                    t["setup_line"] = m.group(0).strip()
                    t["setup_reboot"] = m.group(2) == "1"
            m = re.search(r"STICKTIME-CI: ready[^\n]*", txt)
            if m:
                t["ready_s"] = round(time.time() - self.t0, 1)
                t["ready_line"] = m.group(0).strip()
                break
            if re.search(bad, txt):
                t["error"] = re.search(bad, txt).group(0)
                break
            if self.proc.poll() is not None:
                t["exit_s"] = round(time.time() - self.t0, 1)
                t["exited"] = True
                break
            time.sleep(1)
        else:
            t["timeout"] = True
        self.boots.append(t)
        log(f"boot {n}: {t}")
        return t

    # ---- checks over the console
    def login(self):
        # -icount (after QEMU's SD controller flaked) can keep the console busy with boot messages for
        # minutes after 'ready'
        return self.console.login("pi", self.password, timeout=600 if self.icount else 180)

    def sh(self, cmd, timeout=60):
        rc, out = self.console.run(cmd, timeout)
        return rc, out

    def app_version_seen(self, want, timeout=180):
        """wait until the running host's /api/status reports version want; the last version seen"""
        end, seen = time.time() + timeout, None
        while time.time() < end:
            try:
                code, body = http_get(self.http_port, "/api/status")
                if code == 200:
                    seen = json.loads(body).get("version")
                    if seen == want:
                        return seen
            except (OSError, ValueError, urllib.error.URLError):
                pass
            time.sleep(3)
        return seen

    def update_checks(self, n):
        """the web page's Updates end to end, on the real OS (noexec /run, FAT, the service restart): a
        newer app from a fake GitHub on this machine (10.0.2.2 from the guest), then a rollback"""
        sys.path.insert(0, os.path.join(REPO, "image", "tests"))
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import fake_github           # noqa: E402
        import make_app_package      # noqa: E402
        ver2 = self.a.update_ver
        rc, out = self.sh("cat /boot/firmware/sticktime/VERSION")
        ver1 = out.strip()
        # its system scripts differ from the image's: a new file and a changed one (stupdate/system.py)
        lib = make_app_package.lib_files()
        extra = {"boot-test.txt": b"from the update\n", "stboot.py": lib["stboot.py"] + b"\n# boot test\n"}
        try:
            data, _n = make_app_package.build(self.a.update_bin, ver2, 1700000000, lib_extra=extra)
        except Exception as e:                       # noqa: BLE001
            self.check(f"boot {n}: app package {ver2} built", False, e)
            return
        scripts = ("cat /usr/lib/sticktime/boot-test.txt 2>/dev/null; grep -c '^# boot test' /usr/lib/sticktime/stboot.py;"
                   " test -f /var/lib/sticktime/system/image.json && echo image-kept")
        gh = fake_github.FakeGitHub(advertise="10.0.2.2").start()
        try:
            gh.add_release("juricabi/sticktime-pi", "v" + ver2, f"sticktime-pi-app-{ver2}.tar.gz", data)
            upd = ("env STICKTIME_GITHUB_API=%s STICKTIME_GITHUB_RAW=%s STICKTIME_PAGES=%s /usr/lib/sticktime/sticktime-update"
                   " --base /boot/firmware/sticktime --settings /boot/firmware/sticktime.txt --state /run/sticktime"
                   % (gh.api, gh.raw, gh.pages))
            rc, out = self.console.sudo(upd + " apply app", self.password, timeout=600)
            self.info(f"boot {n}: sticktime-update apply app", out[-1500:])
            rc2, v = self.sh("cat /boot/firmware/sticktime/VERSION")
            self.check(f"boot {n}: app update {ver1} -> {ver2} installed", v.strip() == ver2, f"VERSION {v.strip()!r}; {out[-600:]}")
            seen = self.app_version_seen(ver2)
            self.check(f"boot {n}: the updated app runs (status says {ver2})", seen == ver2, f"status version {seen!r}")
            rc2, s = self.sh(scripts)
            self.check(f"boot {n}: the update's system scripts are in /usr/lib/sticktime",
                       s.split() == ["from", "the", "update", "1", "image-kept"], s)
            rc2, s = self.sh("cat /run/sticktime/update.json")
            self.check(f"boot {n}: the web page is told what applies after a reboot",
                       "Some system changes apply after a reboot" in s, s[-600:])
            # the host runs the set's own sync (lib/ on the FAT, python -I -B) at every start: at boot and
            # after the update's restart, which then had nothing left to do
            end, jl, syncs = time.time() + 60, "", 0
            while time.time() < end:
                rc2, jl = self.console.sudo("journalctl -b -u sticktime.service --no-pager -o cat | grep -c "
                                            "\"system scripts are up to date\"", self.password, timeout=60)
                last = (jl or "").strip().splitlines()[-1:]
                syncs = int(last[0]) if last and last[0].isdigit() else 0
                if syncs >= 2:
                    break
                time.sleep(3)
            self.check(f"boot {n}: the host's start sync ran at boot and after the update (up to date)", syncs >= 2, jl)
            rc, out = self.console.sudo(upd + " rollback app", self.password, timeout=600)
            self.info(f"boot {n}: sticktime-update rollback app", out[-1500:])
            rc2, v = self.sh("cat /boot/firmware/sticktime/VERSION")
            self.check(f"boot {n}: rollback to {ver1}", v.strip() == ver1, f"VERSION {v.strip()!r}; {out[-600:]}")
            seen = self.app_version_seen(ver1)
            self.check(f"boot {n}: the old app runs again (status says {ver1})", seen == ver1, f"status version {seen!r}")
            rc2, s = self.sh(scripts)
            self.check(f"boot {n}: the rollback brought the image's system scripts back", s.split() == ["0", "image-kept"], s)
        finally:
            gh.stop()

    def guest_checks_first(self):
        """False when the login fails: every later command would only be typed into the login prompt"""
        c, pw = self.console, self.password
        if not self.check("serial login as pi with the password from sticktime.txt", self.login()):
            self.tail_log(len(self.boots))
            return False
        c.sudo("dmesg -n 1", pw)                    # no kernel messages between our command outputs
        rc, out = self.sh("hostname")
        self.check("hostname from sticktime.txt", out.strip() == CI_HOST1, out)
        rc, out = self.sh("systemctl is-enabled userconfig.service; systemctl is-enabled sshswitch.service")
        self.check("userconfig and sshswitch masked", out.split() == ["masked", "masked"], out)
        rc, out = self.sh("systemctl --failed --plain --no-legend --no-pager")
        self.check("no failed units", rc == 0 and out.strip() == "", out or "(none)")
        rc, out = self.sh("systemctl is-system-running")
        self.check("system state running", out.strip() == "running", out)
        rc, out = self.sh("systemctl is-active sticktime-setup.service sticktime.service sticktime-hotspot.service "
                          "sticktime-bt.service sticktime-update-repair.service")
        self.check("StickTime units active", out.split() == ["active"] * 5, out)
        rc, out = self.sh("cd /boot/firmware/sticktime/sd/SCRIPTS/TOOLS && ls StickTime.lua StickTimeBW.lua StickTimeLite.lua "
                          "StickTimeBW/core.lua StickTimeLite/core.lua 2>&1")
        self.check("all three game editions on the card (color, BW, Lite)", rc == 0 and "No such" not in out, out)
        rc, out = self.sh("cat /run/sticktime/setup.json")
        try:
            rep = json.loads(out)
        except ValueError:
            rep = {}
        self.check("sticktime-setup ran (setup.json)", rep.get("mode") == "boot" and rep.get("failed") == [], out[:500])
        self.check("setup did not reboot on the first boot (config baked in)", rep.get("reboot") is False)
        rc, out = self.sh("grep -E '^(password|ssh|hostname) =' /boot/firmware/sticktime.txt")
        self.check("password cleared from sticktime.txt", "password =\n" in out + "\n" and pw not in out, out)
        rc, out = self.sh("getent passwd pi; sudo -n true 2>&1 | head -n 1")
        self.check("user pi has bash", out.splitlines()[0].endswith(":/bin/bash") if out else False, out)
        rc, out = self.sh("cat /etc/machine-id")
        self.check("first boot completed (machine-id set)", re.fullmatch(r"[0-9a-f]{32}", out.strip()) is not None, out)
        rc, out = self.sh("df -B1 --output=size / | tail -n 1; lsblk -bno SIZE /dev/mmcblk0p2")
        nums = [int(x) for x in out.split() if x.isdigit()]
        self.check("root partition and file system grown to the card", len(nums) == 2 and nums[0] > 3_300_000_000
                   and nums[1] > 3_400_000_000, out)
        rc, out = self.sh("cat /boot/firmware/cmdline.txt")
        self.check("initramfs rewrote cmdline.txt (new PARTUUID, no resize), our tokens kept",
                   " resize" not in out and "root=PARTUUID=" in out and "vc4.tv_norm=NTSC" in out
                   and out.split("root=PARTUUID=")[1][:8] != self.orig_cmdline.split("root=PARTUUID=")[1][:8], out)
        rc, out = self.sh("systemctl is-active cloud-init-main.service cloud-init-local.service; test -e /etc/cloud/cloud-init.disabled && echo disabled")
        self.check("cloud-init did not run", "active\n" not in out.replace("inactive", "") and out.strip().endswith("disabled"), out)
        rc, out = self.sh("swapon --noheadings --show; echo swaps=$(wc -l < /proc/swaps)")
        self.check("no swap", out.strip().endswith("swaps=1"), out)
        rc, out = self.sh("systemctl get-default; systemctl is-enabled apt-daily.timer man-db.timer NetworkManager-wait-online.service 2>&1 | tr '\\n' ' '")
        self.check("boot trims (multi-user, timers off)", out.startswith("multi-user.target") and "enabled" not in out.split("\n", 1)[-1].replace("disabled", ""), out)
        rc, out = self.sh("ls /etc/NetworkManager/system-connections/; stat -c '%a %U %n' /etc/NetworkManager/system-connections/* 2>/dev/null")
        modes = [ln for ln in out.splitlines() if re.match(r"^\d{3} ", ln)]
        self.check("NM profiles written, 0600 root", "sticktime-wifi.nmconnection" in out and "sticktime-hotspot.nmconnection" in out
                   and modes and all(ln.startswith("600 root") for ln in modes), out)
        rc, out = c.sudo("cat /var/lib/NetworkManager/NetworkManager.state; grep -o cfg80211.ieee80211_regdom=[A-Z]* /boot/firmware/cmdline.txt", pw)
        self.check("WiFi radio enabled for NM, country in cmdline.txt", "WirelessEnabled=true" in out and "regdom=US" in out, out)
        rc, out = self.sh("journalctl -b -u sticktime-hotspot.service --no-pager -o cat | tail -n 5")
        self.check("hotspot helper quiet without a WiFi device", "no WiFi device" in out or "waiting" in out, out)
        rc, out = self.sh("journalctl -b -u sticktime-bt.service --no-pager -o cat | tail -n 5")
        self.check("bluetooth helper quiet without an adapter", "Traceback" not in out, out or "(nothing logged)")
        rc, out = self.sh("journalctl -b -u sticktime-setup.service --no-pager -o cat | tail -n 15")
        self.info("sticktime-setup log", out)
        rc, out = self.sh("cat /etc/hostname; grep 127.0.1.1 /etc/hosts")
        self.check("/etc/hostname and /etc/hosts", out.split()[0] == CI_HOST1 and out.split()[-1] == CI_HOST1 if out.split() else False, out)
        # the app
        rc, out = self.sh("systemctl show -p ActiveState -p SubState -p NRestarts -p MainPID sticktime.service")
        self.info("sticktime.service", out.replace("\n", " "))
        st0 = dict(ln.split("=", 1) for ln in out.splitlines() if "=" in ln)
        placeholder = False
        try:
            code, body = http_get(self.http_port, "/api/status")
            j = json.loads(body)
            placeholder = bool(j.get("placeholder"))
            self.check("http://127.0.0.1:%d/api/status answers" % self.http_port, code == 200 and "version" in j,
                       body[:300].decode("utf-8", "replace"))
        except (OSError, ValueError, urllib.error.URLError) as e:
            self.check("http /api/status answers", False, e)
        self.placeholder = placeholder
        c.sudo("systemctl kill -s KILL sticktime.service", pw)
        time.sleep(1)
        ok = False
        for _ in range(60):
            rc, out = self.sh("systemctl show -p ActiveState -p NRestarts -p MainPID sticktime.service")
            st1 = dict(ln.split("=", 1) for ln in out.splitlines() if "=" in ln)
            if st1.get("ActiveState") == "active" and int(st1.get("NRestarts", "0") or 0) > int(st0.get("NRestarts", "0") or 0) \
                    and st1.get("MainPID") not in ("0", st0.get("MainPID")):
                ok = True
                break
            time.sleep(1)
        self.check("sticktime.service restarts after it is killed", ok, out.replace("\n", " "))
        rc, out = self.sh("journalctl -b -u sticktime.service --no-pager -o cat | tail -n 12")
        self.info("sticktime.service log", out)
        # ssh
        banner = ssh_banner(self.ssh_port)
        self.check("ssh answers on the forwarded port", banner.startswith("SSH-2.0-"), banner)
        # sshd + PAM accept pi's password: an ssh client inside the guest (askpass, no tty), so it
        # works wherever the test runs; then the same from this machine through the forwarded port
        rc, out = self.sh(f"printf '#!/bin/sh\\necho %s\\n' '{pw}' > /tmp/ap && chmod 700 /tmp/ap && "
                          "SSH_ASKPASS=/tmp/ap SSH_ASKPASS_REQUIRE=force DISPLAY=:0 setsid -w ssh -o StrictHostKeyChecking=no "
                          "-o UserKnownHostsFile=/dev/null -o PreferredAuthentications=password -o PubkeyAuthentication=no "
                          "-o LogLevel=ERROR pi@127.0.0.1 hostname < /dev/null; rm -f /tmp/ap", 120)
        self.check("ssh password login as pi (inside the guest)", out.strip().splitlines()[-1:] == [CI_HOST1], out)
        ok, why = ssh_password_login(self.ssh_port, pw, self.work)
        if ok is None:
            self.info("ssh password login from this machine", f"not tried: {why}")
        elif os.name == "nt" and not ok:
            # Windows ssh clients can't always run an askpass script; the check above covers sshd
            self.info("ssh password login from this machine", f"askpass did not work here: {why}")
        else:
            self.check("ssh password login as pi from this machine (forwarded port)", ok and why.strip() == CI_HOST1, why)
        rc, out = self.sh("systemd-analyze time; systemd-analyze blame --no-pager | head -n 15")
        self.info("systemd-analyze (first boot)", out)
        self.screenshot("first-boot")
        return True

    def screenshot(self, name):
        ppm = os.path.join(self.work, f"screen-{name}.ppm").replace(os.sep, "/")
        r = Monitor(self.mon_port).cmd(f"screendump screen-{name}.ppm", 30)   # relative to QEMU's cwd
        size, end = -1, time.time() + 30
        while time.time() < end:                     # written by QEMU after the monitor answered
            if os.path.exists(ppm) and os.path.getsize(ppm) > 15 and os.path.getsize(ppm) == size:
                break
            size = os.path.getsize(ppm) if os.path.exists(ppm) else -1
            time.sleep(1)
        if os.path.exists(ppm):
            try:
                w, h, nonblack = ppm_to_png(ppm, ppm[:-4] + ".png")
                os.unlink(ppm)
                self.info(f"screenshot {name}", f"{ppm[:-4]}.png {w}x{h}, {nonblack} sampled non-black pixels")
            except (OSError, ValueError) as e:
                self.info(f"screenshot {name}", f"cannot convert: {e}")
        else:
            self.info(f"screenshot {name}", f"no screendump: {r.strip()[:200]}")

    def edit_settings(self, changes):
        pw = self.password
        expr = "; ".join(f"s/^{k} =.*$/{k} = {v}/" for k, v in changes.items())
        rc, out = self.console.sudo(f"sed -i \"{expr}\" /boot/firmware/sticktime.txt && sync", pw)
        rc2, out2 = self.sh("grep -E '^(" + "|".join(changes) + ") =' /boot/firmware/sticktime.txt")
        return rc == 0 and all(f"{k} = {v}" in out2 for k, v in changes.items())

    def reboot_guest(self):
        self.console.sudo("systemctl reboot", self.password, timeout=10)
        ok = self.wait_qemu_exit(300)
        self.check("guest reboot ends QEMU (-no-reboot)", ok)
        if not ok:
            self.stop()

    def run(self):
        started = time.time()
        try:
            # ---- boot 1 (QEMU's emulated SD controller sometimes times out under a busy host: then
            # the card reads garbage; that is QEMU, not the image, so boot 1 starts over from a fresh copy)
            base = len(self.results)
            for attempt in range(1, self.a.retries + 2):
                notes = [r for r in self.results[base:] if r["check"].startswith("boot 1 attempt")]
                self.results = self.results[:base] + notes          # prepare()'s checks once, not per try
                self.prepare()
                self.launch(1, first=True)
                t = self.wait_boot(1)
                flake = re.search(SD_FLAKE, self.console.text()) if "error" in t else None
                if not flake or "ready_s" in t or attempt > self.a.retries:
                    break
                self.info(f"boot 1 attempt {attempt}", f"QEMU's SD controller timed out ({flake.group(0)}): starting over"
                          " with -icount")
                self.icount = True                     # from now on, for every boot
                self.stop()
                self.boots.pop()
            ok = self.check("first boot reaches 'ready'", "ready_s" in t, t)
            if not ok and not t.get("ready_missing"):
                self.tail_log(1)
                return self.finish(started)
            self.check("boot 1: sticktime-setup marker", "setup_s" in t, t.get("setup_line", ""))
            self.check("boot 1: no failed units at ready", "failed=none" in t.get("ready_line", ""), t.get("ready_line", ""))
            if not self.guest_checks_first() or self.a.quick:
                return self.finish(started)
            # ---- boot 2: video change -> setup rewrites config.txt and reboots once
            self.check("settings edited for boot 2 (hostname, video = hdmi)",
                       self.edit_settings({"hostname": CI_HOST2, "video": "hdmi"}))
            self.reboot_guest()
            self.launch(2, first=False)
            t = self.wait_boot(2)
            self.check("boot 2: setup applied the video change and rebooted once by itself",
                       t.get("setup_reboot") is True and t.get("exited") is True, t)
            if not t.get("exited"):
                self.stop()
            # ---- boot 3: no second reboot
            self.launch(3, first=False)
            t = self.wait_boot(3)
            self.check("boot 3: ready without another setup reboot", "ready_s" in t and t.get("setup_reboot") is False, t)
            if "ready_s" in t or t.get("ready_missing"):
                c, pw = self.console, self.password
                if not self.check("boot 3: serial login", self.login()):
                    self.tail_log(3)
                    return self.finish(started)
                c.sudo("dmesg -n 1", pw)
                rc, out = self.sh("hostname")
                self.check("boot 3: new hostname applied", out.strip() == CI_HOST2, out)
                rc, out = self.sh("grep -E '^(dtoverlay=vc4|enable_tvout)' /boot/firmware/config.txt")
                self.check("boot 3: config.txt switched to HDMI", out.split() == ["dtoverlay=vc4-kms-v3d", "enable_tvout=0"], out)
                rc, out = self.sh("cat /var/lib/sticktime/boot-config.json; cat /run/sticktime/setup.json")
                self.check("boot 3: reboot guard cleared after the clean boot", '"last_ok"' in out and '"reboot": false' in out, out[:400])
                rc, out = self.sh("systemctl --failed --plain --no-legend --no-pager")
                self.check("boot 3: no failed units", out.strip() == "", out or "(none)")
                if self.a.update_bin:
                    self.update_checks(3)
                self.check("settings edited for boot 4 (ssh = off)", self.edit_settings({"ssh": "off"}))
                self.reboot_guest()
                # ---- boot 4: ssh off
                self.launch(4, first=False)
                t = self.wait_boot(4)
                if self.check("boot 4: ready", "ready_s" in t, t) or t.get("ready_missing"):
                    if not self.check("boot 4: serial login", self.login()):
                        self.tail_log(4)
                        return self.finish(started)
                    rc, out = self.sh("systemctl is-enabled ssh.service; systemctl is-active ssh.service")
                    self.check("boot 4: ssh disabled and stopped", out.split()[:2] == ["disabled", "inactive"], out)
                    banner = ssh_banner(self.ssh_port, 5)
                    self.check("boot 4: no ssh banner on the forwarded port", not banner.startswith("SSH-"), banner)
                    # the program may still be starting: on a fast runner the checks above take ~8 s
                    code, err, end = None, None, time.time() + 120
                    while time.time() < end:
                        try:
                            code, _ = http_get(self.http_port, "/api/status")
                            if code == 200:
                                break
                        except (OSError, urllib.error.URLError) as e:
                            err = e
                        time.sleep(3)
                    self.check("boot 4: web still answers", code == 200, err)
                    self.screenshot("boot-4")
            return self.finish(started)
        finally:
            self.stop()

    def tail_log(self, n):
        try:
            txt = open(os.path.join(self.work, f"serial-{n}.log"), encoding="utf-8", errors="replace").read()
            self.info(f"serial-{n}.log (tail)", txt[-3000:])
        except OSError:
            pass

    def finish(self, started):
        failed = [r for r in self.results if r["ok"] is False]
        report = {"image": self.a.image, "qemu": self.qemu, "boots": self.boots, "results": self.results,
                  "passed": sum(1 for r in self.results if r["ok"]), "failed": len(failed),
                  "minutes": round((time.time() - started) / 60, 1), "placeholder_app": getattr(self, "placeholder", None)}
        with open(os.path.join(self.work, "report.json"), "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
        log(f"boots: " + "; ".join(f"#{b['boot']}: setup {b.get('setup_s', '-')} s, ready {b.get('ready_s', '-')} s"
                                   + (f", exit {b['exit_s']} s" if "exit_s" in b else "") for b in self.boots))
        log(f"{report['passed']} passed, {len(failed)} failed in {report['minutes']} min; report: {self.work}/report.json")
        for r in failed:
            log(f"  FAILED: {r['check']}: {r['detail'][:300]}")
        return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", help="sticktime-pi-<ver>.img.xz or .img")
    ap.add_argument("--work", default=os.path.join(REPO, "build", "boot-test"), help="work folder (needs ~4.5 GB)")
    ap.add_argument("--qemu", help="qemu-system-aarch64 to use")
    ap.add_argument("--kernel-dir", help="take kernel8.img/initramfs8/dtb from here instead of the image")
    ap.add_argument("--password", default="ci-Pass-123", help="the pi password the test sets")
    ap.add_argument("--ssh-port", type=int, default=10022)
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--first-timeout", type=int, default=1800, help="seconds for the first boot")
    ap.add_argument("--timeout", type=int, default=1200, help="seconds for the later boots")
    ap.add_argument("--ready-grace", type=int, default=600, help="seconds after the login prompt to wait for the ready marker")
    ap.add_argument("--retries", type=int, default=2, help="start boot 1 over this often after an SD emulation timeout")
    ap.add_argument("--icount", action="store_true", help="run QEMU with -icount from the start (else only after a timeout)")
    ap.add_argument("--quick", action="store_true", help="only the first boot and its checks")
    ap.add_argument("--update-bin", help="a static aarch64 sticktime-pi built with VERSION=--update-ver: boot 3 updates "
                    "the app to it from a fake GitHub, then rolls back (the Updates page, end to end)")
    ap.add_argument("--update-ver", help="the version --update-bin was built with (higher than the image's)")
    a = ap.parse_args()
    if not os.path.exists(a.image):
        ap.error(f"{a.image} not found")
    sys.exit(BootTest(a).run())


if __name__ == "__main__":
    main()
