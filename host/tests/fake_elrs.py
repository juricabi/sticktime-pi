#!/usr/bin/env python3
"""Fake ELRS / TBS Crossfire WiFi joystick TX module, for testing without hardware.

Shared with sticktime-pi (host/tests/fake_elrs.py): keep the two copies the same.

Standard library only (Python 3.11+). Follows ExpressLRS src/lib/WIFI/wifiJoystick.cpp and
devWIFI.cpp (WebUdpControl), and what the owner's Windows app (JoystickEngine.cs) saw from TBS
Crossfire modules:

  beacons  ELRS  b"ELRS" u8 version(1) u16le port(11000) u8 name_len name, only while not streaming
                 (wifiJoystick.cpp Loop returns early while active), once more than 5 s passed since the
                 last one: so at once when a stream that lasted longer ends
           XF    b"VELOCIDRONE" (+ optional bytes), every ~8 s, also while streaming
  HTTP     POST /udpcontrol, form body or query: action=joystick_begin&interval=<us>&channels=<n>
           ELRS: the stream goes to the client that posted, always (a later POST from anyone
                 steals an active stream); interval/channels only change while idle: interval 0
                 or < 1000 -> 10000, channels as uint8, 0 -> 8, > 16 -> 16. Reply 200 "ok".
                 action=joystick_end stops the stream (StopSending: active = false; the beacons resume,
                 the first at once, see above) and gets 200 "ok" too, also while idle; any other action
                 gets no reply.
                 GET /udpcontrol -> 404 (the route is POST only).
           XF:   assumed: 16 channels every 11 ms, ignores interval, channels and joystick_end.
  stream   u8 type(1) u8 count, count x u16le, to (client ip, stream_port), from (ip, any port)

Several modules run on one host with their own loopback addresses (127.0.0.2, 127.0.0.3, ...):
Linux and Windows route all of 127.0.0.0/8 to loopback, so the receiver tells them apart by source
IP. On loopback, broadcasts are unreliable (Windows): beacon_to is a unicast address in tests.

As a library (host/tests/wifi_live_test.py):
    m = FakeModule(ip="127.0.0.2", http_port=27501, beacon_to=("127.0.0.1", 27500)).start()
    m.mode = "placeholder"; ...; m.stop()
    m.actions() == ["joystick_begin", "joystick_end"]; m.npost("joystick_end") == 1; m.beacon_times
As a program (it prints each request and the stream state it left), e.g. on a PC next to a real Pi
(no radio needed):
    python fake_elrs.py --ip 0.0.0.0 --http-port 80 --kind elrs --sweep
"""
import argparse
import http.client
import math
import random
import socket
import socketserver
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlencode, urlsplit

PLACEHOLDER = 0xF26A
# a corrupt frame captured live from a Crossfire module (ChannelParsingTests.CorruptFrame_MidStream)
CORRUPT = [62058, 62058, 62058, 56051, 556, 35904, 212, 11347]
OVERSHOOT = 0x816B           # a Crossfire stick past a wide endpoint
ELRS_CENTER = 16393          # EdgeTX 0 -> CRSF 992 -> (992 - 172) * 32767 // 1639
ELRS_LOW = 19                # EdgeTX -1024 -> CRSF 173
ELRS_HIGH = 32767            # EdgeTX +1024 -> CRSF 1811
XF_PERIOD = 0.011
MODES = ("normal", "placeholder", "silent", "garbage")


def elrs_beacon(name=b"Unified TX", port=11000, version=1):
    return b"ELRS" + struct.pack("<BHB", version, port, len(name)) + name


def channel_frame(values, ftype=1):
    return struct.pack("<BB", ftype, len(values)) + b"".join(struct.pack("<H", v & 0xFFFF) for v in values)


def edgetx_to_raw(ch):
    """What an ELRS module sends for an EdgeTX channel value (-1024..1024; +-1536 with wide limits)."""
    crsf = max(0, min(1984, 992 + int(ch * 4 / 5)))          # C truncation toward zero
    crsf = max(172, min(1811, crsf))
    return (crsf - 172) * 32767 // 1639


def to_int(s):
    """Arduino String::toInt(): the leading integer, else 0."""
    s = (s or "").strip()
    i = 1 if s[:1] in ("+", "-") else 0
    while i < len(s) and s[i].isdigit():
        i += 1
    try:
        return int(s[:i])
    except ValueError:
        return 0


def post_udpcontrol(ip, http_port, params, source_ip=None, timeout=5.0):
    """POST /udpcontrol like the Windows app, optionally from a given local address (a second
    client). Returns (status, body) or (None, error text)."""
    try:
        c = http.client.HTTPConnection(ip, http_port, timeout=timeout,
                                       source_address=(source_ip, 0) if source_ip else None)
        c.request("POST", "/udpcontrol", urlencode(params),
                  {"Content-Type": "application/x-www-form-urlencoded"})
        r = c.getresponse()
        body = r.read()
        c.close()
        return r.status, body
    except (OSError, http.client.HTTPException) as e:
        return None, str(e)


class _Server(ThreadingHTTPServer):
    daemon_threads = True

    def server_bind(self):
        # HTTPServer.server_bind does a reverse lookup (getfqdn) that can stall for seconds on Windows
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


class FakeModule:
    def __init__(self, ip="127.0.0.2", http_port=8080, kind="elrs", stream_port=11000,
                 beacon_to=("127.0.0.1", 11000), beacon_interval=None, name=b"Unified TX",
                 first_beacon=0.05):
        if kind not in ("elrs", "xf"):
            raise ValueError("kind must be elrs or xf")
        self.ip, self.http_port, self.kind = ip, http_port, kind
        self.stream_port, self.beacon_to, self.name = stream_port, beacon_to, name
        self.beacon_interval = beacon_interval if beacon_interval is not None else (5.0 if kind == "elrs" else 8.0)
        self.first_beacon = first_beacon
        self.lock = threading.Lock()
        # protocol state (lost on stop = power off)
        self.active = False
        self.remote_ip = None
        self.interval_us = 10000
        self.channels = 8 if kind == "elrs" else 16
        # scenario knobs: change them any time
        self.values = [ELRS_CENTER] * 16      # raw values to stream (the first `channels`)
        self.mode = "normal"                  # normal | placeholder | silent | garbage
        self.corrupt_every = 0                # > 0: every Nth frame is CORRUPT (cut/padded to count)
        self.overshoot = False                # CH1 = 0x816B
        self.beacons_enabled = True
        self.beacon_suffix = b""              # XF: bytes after VELOCIDRONE
        self.http_status = 200                # reply to every POST /udpcontrol with this
        self.http_fail_first = 0              # the first N POSTs get http_fail_status instead
        self.http_fail_status = 500
        self.http_delay = 0.0                 # seconds before replying (timeout tests)
        self.http_drop = False                # close the connection without a reply
        self.stream_port_for = {}             # client ip -> port: on loopback, a stand-in for another PC
        self.xf_period = XF_PERIOD
        self.seed = 1234                      # garbage mode
        # observations
        self.posts = []                       # dicts: method, client, path, headers, body, params, t, status
        self.sent_frames = 0
        self.sent_beacons = 0
        self.sent_garbage = 0
        self.beacons_while_active = 0
        self.beacon_times = []                # time.monotonic() of each beacon sent
        self._stop = threading.Event()
        self._threads = []
        self._httpd = None
        self._udp = None

    # ---------------------------------------------------------------- lifecycle
    def start(self):
        self._stop = threading.Event()
        self._udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self._udp.bind((self.ip, 0))         # the source IP is the module's identity
        if self.http_port:
            self._httpd = _Server((self.ip, self.http_port), self._handler_class())
            self._spawn(lambda: self._httpd.serve_forever(poll_interval=0.05))
        self._spawn(self._beacon_loop)
        self._spawn(self._stream_loop)
        return self

    def stop(self):
        """Power off: silent, and the stream state is lost."""
        self._stop.set()
        if self._httpd:
            self._httpd.shutdown()
            self._httpd.server_close()
            self._httpd = None
        for t in self._threads:
            t.join(2)
        self._threads.clear()
        if self._udp:
            self._udp.close()
            self._udp = None
        with self.lock:
            self.active, self.remote_ip = False, None

    def _spawn(self, fn):
        t = threading.Thread(target=fn, daemon=True)
        t.start()
        self._threads.append(t)

    # ---------------------------------------------------------------- observations
    def post_times(self, status=None):
        with self.lock:
            return [p["t"] for p in self.posts if p["method"] == "POST" and (status is None or p["status"] == status)]

    def npost(self, action=None):
        """POSTs received; with action, only POST /udpcontrol with that action (e.g. "joystick_end")."""
        with self.lock:
            return sum(1 for p in self.posts if p["method"] == "POST" and
                       (action is None or (p["path"] == "/udpcontrol" and p["params"].get("action") == action)))

    def actions(self, client=None):
        """The action of each POST /udpcontrol, in order ("" when it had none); client: only from that IP."""
        with self.lock:
            return [p["params"].get("action", "") for p in self.posts if p["method"] == "POST" and
                    p["path"] == "/udpcontrol" and (client is None or p["client"] == client)]

    def state(self):
        with self.lock:
            return dict(active=self.active, remote_ip=self.remote_ip, interval_us=self.interval_us,
                        channels=self.channels)

    # ---------------------------------------------------------------- protocol
    def udpcontrol(self, client_ip, params):
        """True when a reply should be sent (ELRS sends none for an unknown action)."""
        action = params.get("action", "")
        with self.lock:
            if action == "joystick_begin":
                if self.kind == "elrs":
                    self.remote_ip = client_ip            # before the "already active" check
                    if not self.active:
                        iv = to_int(params.get("interval", ""))
                        self.interval_us = 10000 if iv < 1000 else iv
                        ch = to_int(params.get("channels", "")) & 0xFF      # a uint8_t parameter
                        self.channels = 8 if ch == 0 else min(ch, 16)
                        self.active = True
                else:
                    self.remote_ip = client_ip
                    self.active = True
                return True
            if action == "joystick_end":
                if self.kind == "elrs":
                    self.active = False
                return True
            return self.kind != "elrs"

    def _handler_class(self):
        mod = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def _record(self, method, path, body, params):
                # active/remote_ip: the stream state the request left (None: it did not reach udpcontrol)
                rec = dict(method=method, client=self.client_address[0], path=path, headers=dict(self.headers),
                           body=body, params=params, t=time.monotonic(), status=None, active=None, remote_ip=None)
                with mod.lock:
                    mod.posts.append(rec)
                return rec

            def do_POST(self):
                u = urlsplit(self.path)
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n).decode("latin-1") if n else ""
                params = {k: v[0] for k, v in parse_qs(u.query).items()}
                if (self.headers.get("Content-Type") or "").startswith("application/x-www-form-urlencoded"):
                    params.update({k: v[0] for k, v in parse_qs(body).items()})
                rec = self._record("POST", u.path, body, params)
                if u.path != "/udpcontrol":
                    return self._reply(rec, 404, b"Not found")
                if mod.http_delay and mod._stop.wait(mod.http_delay):
                    return                                 # powered off meanwhile
                with mod.lock:
                    fail = mod.http_fail_first > 0
                    if fail:
                        mod.http_fail_first -= 1
                if mod.http_drop:
                    self.close_connection = True
                    return
                if fail or mod.http_status != 200:
                    return self._reply(rec, mod.http_fail_status if fail else mod.http_status, b"error")
                answer = mod.udpcontrol(self.client_address[0], params)
                with mod.lock:
                    rec["active"], rec["remote_ip"] = mod.active, mod.remote_ip
                if answer:
                    self._reply(rec, 200, b"ok")
                else:
                    self.close_connection = True

            def do_GET(self):
                rec = self._record("GET", self.path, "", {})
                self._reply(rec, 404, b"Not found")       # /udpcontrol is POST only on ELRS

            def _reply(self, rec, code, body):
                rec["status"] = code
                self.send_response(code)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)
                self.close_connection = True

        return Handler

    # ---------------------------------------------------------------- loops
    def _send_beacon(self, pkt, active):
        if not self.beacons_enabled or not self.beacon_to:
            return
        try:
            self._udp.sendto(pkt, self.beacon_to)
            self.sent_beacons += 1
            self.beacon_times.append(time.monotonic())
            if active:
                self.beacons_while_active += 1
        except OSError:
            pass

    def _beacon_loop(self):
        if self.kind == "elrs":
            return self._elrs_beacon_loop()
        nxt = time.monotonic() + self.first_beacon
        while not self._stop.wait(max(0.0, nxt - time.monotonic())):
            nxt += self.beacon_interval
            with self.lock:
                active = self.active
            self._send_beacon(b"VELOCIDRONE" + self.beacon_suffix, active)

    def _elrs_beacon_loop(self):
        # wifiJoystick.cpp Loop: nothing while streaming; idle, a beacon once more than the interval passed since
        # the last one (the first one first_beacon after power on). So when a stream that lasted longer ends
        # (joystick_end), the next beacon goes out at once.
        due = time.monotonic() + self.first_beacon
        while True:
            now = time.monotonic()
            with self.lock:
                active = self.active
            if not active and now >= due:
                due = now + self.beacon_interval
                self._send_beacon(elrs_beacon(self.name), False)
            wait = 0.01 if active else min(0.01, max(0.0, due - time.monotonic()))
            if self._stop.wait(wait):
                return

    def _frame(self, n, count, rnd):
        if self.mode == "placeholder":
            return channel_frame([PLACEHOLDER] * count)
        if self.mode == "garbage":
            # never a valid frame or beacon: too short, a count outside 4..16, or fewer bytes than the count needs
            self.sent_garbage += 1
            k = rnd.randrange(0, 40)
            b = bytearray(rnd.randrange(256) for _ in range(k))
            if k >= 2 and rnd.random() < 0.5:
                b[1] = rnd.choice((0, 1, 2, 3, 17, 18, 255))
            elif k >= 2:
                b[1] = rnd.randrange(4, 17)
                del b[2 + 2 * b[1] - 1:]
            return bytes(b)
        if self.corrupt_every and n % self.corrupt_every == 0:
            return channel_frame((CORRUPT + [PLACEHOLDER] * 16)[:count])
        vals = list(self.values[:count]) + [ELRS_CENTER] * max(0, count - len(self.values))
        if self.overshoot:
            vals[0] = OVERSHOOT
        return channel_frame(vals)

    def _stream_loop(self):
        # frames are sent when due, so the average rate stays exact even where sleep() is coarse
        rnd = random.Random(self.seed)
        n = 0
        t0 = None
        period = None
        while not self._stop.is_set():
            with self.lock:
                active, dst, count = self.active, self.remote_ip, self.channels
                p = self.interval_us / 1e6 if self.kind == "elrs" else self.xf_period
            if not active or not dst or self.mode == "silent":
                t0 = None
                self._stop.wait(0.002)
                continue
            now = time.perf_counter()
            if t0 is None or p != period or now - (t0 + n * period) > 0.1:   # (re)start or fell far behind
                t0, n, period = now, 0, p
            while t0 + n * period <= now:
                n += 1
                try:
                    self._udp.sendto(self._frame(n, count, rnd), (dst, self.stream_port_for.get(dst, self.stream_port)))
                    self.sent_frames += 1
                except OSError:
                    pass
            d = t0 + n * period - time.perf_counter()
            if d > 0:
                time.sleep(min(d, 0.05))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default="0.0.0.0", help="bind address = module identity (0.0.0.0 on a real LAN)")
    ap.add_argument("--http-port", type=int, default=80, help="0 = no web server (refused)")
    ap.add_argument("--kind", choices=("elrs", "xf"), default="elrs")
    ap.add_argument("--stream-port", type=int, default=None,
                    help="where frames go (default: the --beacon-to port, like a real module, else 11000)")
    ap.add_argument("--beacon-to", default="255.255.255.255:11000", help="host:port, or '' for none")
    ap.add_argument("--beacon-interval", type=float, default=None, help="seconds (default 5 ELRS, 8 XF)")
    ap.add_argument("--name", default="Unified TX")
    ap.add_argument("--mode", choices=MODES, default="normal")
    ap.add_argument("--http-status", type=int, default=200)
    ap.add_argument("--fail-first", type=int, default=0, help="answer the first N POSTs with 500")
    ap.add_argument("--http-delay", type=float, default=0.0)
    ap.add_argument("--http-drop", action="store_true")
    ap.add_argument("--corrupt-every", type=int, default=0)
    ap.add_argument("--overshoot", action="store_true")
    ap.add_argument("--values", default="", help="comma-separated EdgeTX channel values (-1024..1024)")
    ap.add_argument("--sweep", action="store_true", help="move CH1..CH4 in slow sine sweeps")
    ap.add_argument("--duration", type=float, default=0, help="seconds, 0 = until Ctrl+C")
    a = ap.parse_args(argv)
    bt = None
    if a.beacon_to:
        h, p = a.beacon_to.rsplit(":", 1)
        bt = (h, int(p))
    sp = a.stream_port if a.stream_port is not None else bt[1] if bt else 11000
    m = FakeModule(ip=a.ip, http_port=a.http_port, kind=a.kind, stream_port=sp, beacon_to=bt,
                   beacon_interval=a.beacon_interval, name=a.name.encode("latin-1")[:255])
    m.mode, m.http_status, m.http_fail_first = a.mode, a.http_status, a.fail_first
    m.http_delay, m.http_drop, m.corrupt_every, m.overshoot = a.http_delay, a.http_drop, a.corrupt_every, a.overshoot
    if a.values:
        m.values = [edgetx_to_raw(int(v)) for v in a.values.split(",")][:16]
        m.values += [ELRS_CENTER] * (16 - len(m.values))
    m.start()
    print(f"fake {a.kind} module on {a.ip}:{a.http_port}, beacons -> {bt}; Ctrl+C to stop", flush=True)
    seen = 0
    end = time.monotonic() + a.duration if a.duration else None
    try:
        while end is None or time.monotonic() < end:
            time.sleep(0.02)
            if a.sweep:
                t = time.monotonic()
                m.values[:4] = [edgetx_to_raw(int(1024 * math.sin(t * (0.5 + i * 0.2)))) for i in range(4)]
            with m.lock:
                new = m.posts[seen:]
                seen = len(m.posts)
            for p in new:
                left = ""
                if p["active"] is not None:
                    left = f", streaming to {p['remote_ip']}" if p["active"] else ", idle (beacons)"
                print(f"{p['method']} {p['path']} from {p['client']}: {p['body']!r} -> {p['status']}{left}", flush=True)
    except KeyboardInterrupt:
        pass
    m.stop()
    print(f"sent {m.sent_frames} frames, {m.sent_beacons} beacons", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
