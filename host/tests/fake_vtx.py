#!/usr/bin/env python3
"""A fake analog VTX for StickTime Pi's VTX control tests: TBS SmartAudio v1, v2, v2.1 and ImmersionRC
Tramp, on a TCP port (any OS: the host's --vtx-port tcp:127.0.0.1:PORT) or a pty (Linux: --vtx-port
tty:/dev/pts/N, which takes the host's real termios path).

Like the real one-wire line, every byte the host sends comes straight back (the echo; echo=False is a
missing RX wire). Quirks: OpenVTx-style reply lengths (len = payload), a 0x00 after replies (as in the
TBS spec's captures), V2.1 level lists both ways, clones that take a frame only with a 0x00 after it
(AKK/RDQ), unplugged (no answers, the echo stays), a VTX that ignores power commands, Tramp race lock.

    python host/tests/fake_vtx.py --tcp 5000 [--proto sa|tramp] [--version 1|2|2.1] [--style tbs|openvtx]
    python host/tests/fake_vtx.py --pty [...]          (prints the pty's path; Linux)

Tests import it: FakeVtx (the device; change it with set() / press()), TcpLink and PtyLink (the line).
Standard library only. Written for this project from the protocol facts in docs/RESEARCH.md."""
import argparse
import os
import select
import socket
import sys
import threading
import time

SA_GET, SA_POWER, SA_CHANNEL, SA_FREQ, SA_MODE = 1, 2, 3, 4, 5
MODE_FREQ, MODE_PIT, MODE_PIR, MODE_POR, MODE_UNLOCKED = 0x01, 0x02, 0x04, 0x08, 0x10
SA40 = [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725, 5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866,
        5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945, 5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880,
        5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0xD5) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def tramp_sum(f):
    return sum(f[1:14]) & 0xFF


class FakeVtx:
    """The device. Thread-safe: the line threads call hear(), tests call set()/press()/state()."""

    def __init__(self, proto="sa", version=3, style="tbs", tail_zero=True, echo=True, need_trail=False,
                 delay=0.03, levels=(14, 20, 26)):
        self.lock = threading.RLock()
        self.proto, self.version, self.style = proto, version, style
        self.tail_zero, self.echo, self.need_trail, self.delay = tail_zero, echo, need_trail, delay
        self.plugged, self.ignore_power = True, False
        # SmartAudio: on A1 by channel, unlocked, the lowest power
        self.channel, self.freq, self.mode = 0, 5865, MODE_UNLOCKED
        self.levels = list(levels)
        self.power = 7 if version == 1 else 0
        self.dbm = self.levels[0]
        # Tramp
        self.tfreq, self.tpower, self.tmin, self.tmax, self.tmaxp = 5740, 25, 5600, 5950, 600
        self.tpit, self.tcontrol, self.temp = 0, 0, 31
        self.took = []                       # (cmd, value): what it was told
        self.heard = 0                       # bytes
        self.buf = bytearray()

    # ---------------------------------------------------------------- for tests
    def set(self, **kw):
        with self.lock:
            for k, v in kw.items():
                if not hasattr(self, k):
                    raise AttributeError(k)
                setattr(self, k, v)

    def press(self, channel=None, power=None, dbm=None, freq=None, pit=None):
        """someone presses the VTX's button"""
        with self.lock:
            if channel is not None:
                self.channel, self.freq = channel, SA40[channel]
                self.mode &= ~MODE_FREQ
            if power is not None:
                self.power = self.tpower = power
            if dbm is not None:
                self.dbm = dbm
            if freq is not None:
                self.tfreq = self.freq = freq
            if pit is not None:
                self.tpit = 1 if pit else 0
                self.mode = (self.mode | MODE_PIT) if pit else (self.mode & ~MODE_PIT)

    def count(self, cmd, value=None):
        with self.lock:
            return sum(1 for c, v in self.took if c == cmd and (value is None or v == value))

    def state(self):
        with self.lock:
            return {"channel": self.channel, "freq": self.freq, "mode": self.mode, "power": self.power, "dbm": self.dbm,
                    "pit": bool(self.mode & MODE_PIT) if self.proto == "sa" else bool(self.tpit),
                    "tfreq": self.tfreq, "tpower": self.tpower}

    # ---------------------------------------------------------------- the wire
    def hear(self, data):
        """bytes from the host -> [(seconds_later, bytes)] for the host: the echo at once, replies later"""
        out = []
        with self.lock:
            self.heard += len(data)
            if self.echo:
                out.append((0.0, bytes(data)))
            self.buf += data
            replies = self._tramp() if self.proto == "tramp" else self._sa()
            if not self.plugged:
                replies = []
            out += [(self.delay, r) for r in replies]
            if len(self.buf) > 4096:
                del self.buf[:-64]
        return out

    def _sa(self):
        out, b = [], self.buf
        while True:
            i = b.find(b"\xaa\x55")
            if i < 0:
                del b[:max(0, len(b) - 1)]
                return out
            del b[:i]
            if len(b) < 5:
                return out
            n = b[3]
            if n > 32:
                del b[:1]
                continue
            if len(b) < 5 + n:
                return out
            if crc8(b[:4 + n]) != b[4 + n] or not b[2] & 1:
                del b[:1]
                continue
            if self.need_trail:
                if len(b) < 6 + n:
                    return out                    # wait for the byte after the frame
                if b[5 + n] != 0:
                    del b[:5 + n]                 # this clone ignores a frame without its 0x00
                    continue
            frame = bytes(b[:5 + n])
            del b[:5 + n]
            if self.plugged:
                r = self._sa_cmd(frame[2] >> 1, frame[4:4 + n])
                if r:
                    out.append(r)

    def _sa_reply(self, cmd, payload):
        payload = bytes(payload)
        ln = len(payload) if self.style == "openvtx" else len(payload) + 1
        body = bytes([cmd, ln]) + payload
        r = b"\xaa\x55" + body + bytes([crc8(body)])
        if self.tail_zero and self.style != "openvtx":
            r += b"\x00"
        return r

    def _sa_cmd(self, cmd, p):
        v = p[0] if p else 0
        if cmd == SA_GET:
            self.took.append((SA_GET, 0))
            code = {1: 0x01, 2: 0x09, 3: 0x11}[self.version]
            pl = [self.channel, self.power, self.mode, self.freq >> 8, self.freq & 0xFF]
            if self.version == 3:
                if self.style == "openvtx":
                    pl += [self.dbm, len(self.levels)] + self.levels
                else:
                    pl += [self.dbm, len(self.levels), 0] + self.levels
            return self._sa_reply(code, pl)
        if cmd == SA_POWER:
            self.took.append((SA_POWER, v))
            if not self.ignore_power:
                if self.version == 3 and v & 0x80:
                    d = v & 0x7F
                    if d == 0:
                        self.mode |= MODE_PIT             # 0 dBm: pit mode now (the dBm figure stays)
                    else:
                        lower = [x for x in self.levels if x <= d]
                        self.dbm = lower[-1] if lower else self.levels[0]
                        self.mode &= ~MODE_PIT
                else:
                    self.power = v
                    if self.version == 3 and v < len(self.levels):
                        self.dbm = self.levels[v]
                        self.mode &= ~MODE_PIT
            return self._sa_reply(SA_POWER, [v, 1])
        if cmd == SA_CHANNEL:
            self.took.append((SA_CHANNEL, v))
            if v < 40:
                self.channel, self.freq = v, SA40[v]
                self.mode &= ~MODE_FREQ
            return self._sa_reply(SA_CHANNEL, [v, 1])
        if cmd == SA_FREQ and len(p) >= 2:
            f = p[0] << 8 | p[1]
            self.took.append((SA_FREQ, f))
            if not f & 0xC000:
                self.freq = f
                self.mode |= MODE_FREQ
            return self._sa_reply(SA_FREQ, [p[0], p[1], 1])
        if cmd == SA_MODE:
            self.took.append((SA_MODE, v))
            if self.version >= 2:
                flags = (MODE_PIR if v & 1 else 0) | (MODE_POR if v & 2 else 0)
                self.mode = (self.mode & ~(MODE_PIR | MODE_POR)) | flags
                if v & 4:
                    self.mode &= ~MODE_PIT
                elif flags and self.version == 3:
                    self.mode |= MODE_PIT                 # 2.1: at once; 2.0: at the next power-up
                if v & 8:
                    self.mode |= MODE_UNLOCKED
            return self._sa_reply(SA_MODE, [v, 1])
        return None

    def _tramp(self):
        out, b = [], self.buf
        while True:
            i = b.find(b"\x0f")
            if i < 0:
                b.clear()
                return out
            del b[:i]
            if len(b) < 16:
                return out
            f = bytes(b[:16])
            if tramp_sum(f) != f[14] or f[15] != 0:
                del b[:1]
                continue
            del b[:16]
            if self.plugged:
                r = self._tramp_cmd(f)
                if r:
                    out.append(r)

    def _tramp_cmd(self, f):
        cmd, v = chr(f[1]), f[2] | f[3] << 8
        self.took.append((cmd, v))
        r = bytearray(16)
        r[0], r[1] = 0x0F, f[1]

        def u16(at, x):
            r[at], r[at + 1] = x & 0xFF, (x >> 8) & 0xFF
        if cmd == "r":
            u16(2, self.tmin), u16(4, self.tmax), u16(6, self.tmaxp)
        elif cmd == "v":
            u16(2, self.tfreq), u16(4, self.tpower)
            r[6], r[7] = self.tcontrol, self.tpit
            u16(8, self.tpower)
        elif cmd == "s":
            u16(6, self.temp & 0xFFFF)
        else:
            if cmd == "F" and not self.tcontrol & 1:
                self.tfreq = v
            elif cmd == "P" and not self.tcontrol & 1 and not self.ignore_power:
                self.tpower = v
            elif cmd == "I":
                self.tpit = 0 if v else 1                 # 0 = pit on
            return None                                   # set commands get no answer
        r[14] = tramp_sum(r)
        return bytes(r)


class _Sender:
    """sends (time, bytes) in time order on one connection"""

    def __init__(self, write):
        self.write, self.q, self.cv, self.alive = write, [], threading.Condition(), True
        threading.Thread(target=self._run, daemon=True).start()

    def add(self, items):
        now = time.monotonic()
        with self.cv:
            for dt, data in items:
                self.q.append((now + dt, data))
            self.q.sort(key=lambda x: x[0])
            self.cv.notify()

    def stop(self):
        with self.cv:
            self.alive = False
            self.cv.notify()

    def _run(self):
        while True:
            with self.cv:
                while self.alive and (not self.q or self.q[0][0] > time.monotonic()):
                    self.cv.wait(None if not self.q else max(0.0, self.q[0][0] - time.monotonic()))
                if not self.alive:
                    return
                _, data = self.q.pop(0)
            try:
                self.write(data)
            except OSError:
                return


class TcpLink:
    """the line as a TCP server on 127.0.0.1 (one host at a time); .vtx can be swapped for another device"""

    def __init__(self, vtx, port=0):
        self.vtx = vtx
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(4)
        self.port = self.srv.getsockname()[1]
        self.conns, self.alive, self.accepted = [], True, 0
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while self.alive:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.accepted += 1
            self.conns.append(c)
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    def _serve(self, c):
        snd = _Sender(c.sendall)
        try:
            while self.alive:
                d = c.recv(4096)
                if not d:
                    break
                snd.add(self.vtx.hear(d))
        except OSError:
            pass
        finally:
            snd.stop()
            try:
                c.close()
            except OSError:
                pass

    def cut(self):
        """the cable is pulled: every connection closes (the host connects again)"""
        for c in list(self.conns):
            try:
                c.shutdown(socket.SHUT_RDWR)
                c.close()
            except OSError:
                pass
        self.conns.clear()

    def close(self):
        self.alive = False
        self.cut()
        self.srv.close()


class PtyLink:
    """the line as a pty pair (Linux): the host opens .path like a serial port"""

    def __init__(self, vtx):
        import tty
        self.vtx = vtx
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)                            # the host sets raw mode too; this is for before it opens
        self.path = os.ttyname(self.slave)
        self.alive = True
        self.snd = _Sender(lambda d: os.write(self.master, d))
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while self.alive:
            r, _, _ = select.select([self.master], [], [], 0.2)
            if not r:
                continue
            try:
                d = os.read(self.master, 4096)
            except OSError:
                time.sleep(0.05)                          # nobody has the slave open right now
                continue
            if d:
                self.snd.add(self.vtx.hear(d))

    def close(self):
        self.alive = False
        self.snd.stop()
        for fd in (self.master, self.slave):
            try:
                os.close(fd)
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser(description="A fake SmartAudio / Tramp VTX for tests.")
    ap.add_argument("--tcp", type=int, help="listen on 127.0.0.1:PORT")
    ap.add_argument("--pty", action="store_true", help="make a pty and print its path (Linux)")
    ap.add_argument("--proto", choices=("sa", "tramp"), default="sa")
    ap.add_argument("--version", choices=("1", "2", "2.1"), default="2.1")
    ap.add_argument("--style", choices=("tbs", "openvtx"), default="tbs")
    ap.add_argument("--no-echo", action="store_true")
    ap.add_argument("--need-trail", action="store_true")
    a = ap.parse_args()
    vtx = FakeVtx(a.proto, {"1": 1, "2": 2, "2.1": 3}[a.version], a.style, echo=not a.no_echo, need_trail=a.need_trail)
    if a.pty:
        link = PtyLink(vtx)
        print(link.path, flush=True)
    else:
        link = TcpLink(vtx, a.tcp or 0)
        print(f"tcp:127.0.0.1:{link.port}", flush=True)
    try:
        last = None
        while True:
            time.sleep(1)
            s = vtx.state()
            if s != last:
                print(s, flush=True)
                last = s
    except KeyboardInterrupt:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
