#!/usr/bin/env python3
"""Integration scenarios for the WiFi joystick receiver: the C probe (test_wifi_live with arguments,
which runs input_wifi.c + the vendored core, third_party/elrs-wifi, over real sockets) against fake
modules (fake_elrs.py), all on loopback. Standard library only (Python 3.11+).

    python host/tests/wifi_live_test.py                 all scenarios
    python host/tests/wifi_live_test.py two_modules -v  some, with every probe event printed
    python host/tests/wifi_live_test.py --list
    python host/tests/wifi_live_test.py --probe build/aarch64-edgetx/test_wifi_live

The probe defaults to build/native-edgetx/test_wifi_live[.exe] (build.sh). Ports start at
--base-port (27500), never 11000/80, so a real module on the LAN can't interfere. Modules are
127.0.0.2, .3, ... (all of 127/8 is loopback on Linux and Windows); beacons are unicast to
127.0.0.1, since broadcasts on loopback are unreliable on Windows.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import fake_elrs  # noqa: E402
from fake_elrs import ELRS_CENTER, ELRS_HIGH, ELRS_LOW, FakeModule  # noqa: E402

VERBOSE = False
# A blocking call in wifi_poll (a connect, a POST) would show up as seconds; an emulated or busy
# machine has scheduling stalls of tens of ms, so the hard limit is in between (p99 is the real check).
POLL_MAX_US = 200000


class Failed(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Failed(msg)


# ---------------------------------------------------------------- the probe process
class Probe:
    def __init__(self, exe, port, http_port, *extra, name="probe"):
        self.name = name
        cmd = [exe, "--port", str(port), "--http-port", str(http_port), "--stdin", "--duration-ms", "300000",
               *[str(x) for x in extra]]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, encoding="utf-8", errors="replace", bufsize=1)
        self.t0 = time.monotonic()
        self.ev = []
        self.err = []
        self.done = False
        self.cv = threading.Condition()
        threading.Thread(target=self._read_out, daemon=True).start()
        threading.Thread(target=self._read_err, daemon=True).start()

    def _read_out(self):
        for line in self.p.stdout:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            e["wall"] = time.monotonic()
            if VERBOSE:
                print(f"    [{self.name}] {line.rstrip()}")
            with self.cv:
                self.ev.append(e)
                self.cv.notify_all()
        with self.cv:
            self.done = True
            self.cv.notify_all()

    def _read_err(self):
        for line in self.p.stderr:
            self.err.append(line.rstrip())

    def mark(self):
        with self.cv:
            return len(self.ev)

    def wait(self, pred, timeout, start=0):
        end = time.monotonic() + timeout
        with self.cv:
            i = start
            while True:
                while i < len(self.ev):
                    if pred(self.ev[i]):
                        return self.ev[i]
                    i += 1
                left = end - time.monotonic()
                if left <= 0 or self.done:
                    return None
                self.cv.wait(left)

    def events(self, kind, start=0):
        with self.cv:
            return [e for e in self.ev[start:] if e["ev"] == kind]

    def logs(self, sub, start=0):
        return [e for e in self.events("log", start) if sub in e["msg"]]

    def wait_log(self, sub, timeout, start=0):
        return self.wait(lambda e: e["ev"] == "log" and sub in e["msg"], timeout, start)

    def wait_state(self, state, timeout, start=0, module=None):
        return self.wait(lambda e: e["ev"] == "state" and e["state"].startswith(state) and
                         (module is None or e["module"] == module), timeout, start)

    def wait_frame(self, pred, timeout, start=0):
        return self.wait(lambda e: e["ev"] == "frame" and pred(e), timeout, start)

    def wait_stats(self, pred, timeout, start=0):
        return self.wait(lambda e: e["ev"] == "stats" and pred(e), timeout, start)

    def last_state(self):
        s = self.events("state")
        return s[-1]["state"] if s else None

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def close(self, timeout=10):
        """quit; returns the "poll" event (the poll-time statistics) or None"""
        if self.p.poll() is None:
            try:
                self.cmd("quit")
            except OSError:
                pass
            try:
                self.p.wait(timeout)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.wait(lambda e: e["ev"] == "exit", 2)
        polls = self.events("poll")
        return polls[-1] if polls else None

    def kill(self):
        self.p.kill()
        self.p.wait()

    def dump(self, n=40):
        with self.cv:
            tail = self.ev[-n:]
        print(f"    --- last {len(tail)} events of {self.name}:")
        for e in tail:
            print("     ", {k: v for k, v in e.items() if k != "wall"})


# ---------------------------------------------------------------- a scenario's resources
class Ctx:
    def __init__(self, exe, base):
        self.exe = exe
        self.base = base            # UDP port of the probe; base+1.. for the modules' web servers
        self.fakes, self.probes, self.socks = [], [], []

    def fake(self, ip="127.0.0.2", http=1, kind="elrs", beacon_interval=1.0, start=True, first_beacon=0.1, **knobs):
        m = FakeModule(ip=ip, http_port=self.base + http if http else 0, kind=kind, stream_port=self.base,
                       beacon_to=("127.0.0.1", self.base), beacon_interval=beacon_interval,
                       first_beacon=first_beacon)
        for k, v in knobs.items():
            if not hasattr(m, k):
                raise AttributeError(k)
            setattr(m, k, v)
        self.fakes.append(m)
        return m.start() if start else m

    def probe(self, *extra, http=1, name="probe", listening=True):
        """start a probe; by default wait until it listens, so no beacon is lost before the bind"""
        p = Probe(self.exe, self.base, self.base + http, *extra, name=name)
        self.probes.append(p)
        if listening and not p.wait(lambda e: e["ev"] == "state" and e["proto"] != "stopped", 3):
            raise Failed(f"{name} did not start")
        return p

    def udp(self, ip="127.0.0.1", port=0):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((ip, port))
        self.socks.append(s)
        return s

    def cleanup(self):
        for p in self.probes:
            p.close(5)
        for m in self.fakes:
            m.stop()
        for s in self.socks:
            s.close()


SCENARIOS = []


def scenario(fn):
    SCENARIOS.append(fn)
    return fn


def ip_of(e):
    return e.get("src")


# ---------------------------------------------------------------- scenarios
@scenario
def discovery_elrs(c):
    """ELRS beacon -> one POST with the right request -> streaming, values mapped; no beacons or POSTs while streaming"""
    vals = [ELRS_HIGH, ELRS_LOW, ELRS_CENTER, 24570, 8216] + [ELRS_CENTER] * 11
    p = c.probe()
    m = c.fake("127.0.0.2", values=vals)
    check(p.wait_log("Discovery beacon from ELRS module 127.0.0.2 - activating", 3), "no beacon log")
    s = p.wait_state("streaming", 4, module="ELRS 127.0.0.2")
    check(s, f"not streaming (last state {p.last_state()})")
    check(p.wait_log("Activation POST -> 127.0.0.2: 200 OK", 2), "no POST result log")
    post = m.posts[0]
    check(post["method"] == "POST" and post["path"] == "/udpcontrol", f"request {post['method']} {post['path']}")
    check(post["headers"].get("Content-Type") == "application/x-www-form-urlencoded", "Content-Type")
    check(post["body"] == "action=joystick_begin&interval=4000&channels=16", f"body {post['body']!r}")
    check(post["headers"].get("Host") == f"127.0.0.2:{c.base + 1}", f"Host {post['headers'].get('Host')}")
    check(post["client"] == "127.0.0.1", f"POST came from {post['client']}")
    f = p.wait_frame(lambda e: e["n"] == 16, 2)
    check(f, "no 16-channel frame")
    check(f["raw"][:5] == vals[:5], f"raw {f['raw'][:5]}")
    check(f["val"][:5] == [1024, -1024, 0, 511, -511], f"val {f['val'][:5]}")
    check(f["name"] == "ELRS 127.0.0.2", f"name {f['name']}")
    beacons = m.sent_beacons
    st = p.wait_stats(lambda e: e["frames"] > 100, 3, p.mark())
    check(st and 200 <= st["hz"] <= 300, f"rate {st and st['hz']} Hz (expected ~250)")
    time.sleep(2.5)
    check(m.npost() == 1, f"{m.npost()} POSTs (expected 1: no re-POST while streaming)")
    check(m.sent_beacons == beacons, "an ELRS module must not beacon while streaming")
    check(len([e for e in p.events("state") if e["state"] == "streaming"]) == 1, "streaming state flickered")
    return f"{st['hz']:.0f} Hz, jitter {st['jitter']:.2f} ms"


@scenario
def crossfire(c):
    """Crossfire beacon -> POST -> ~90 Hz; its beacons while streaming cause no new POST; overshoot clamps"""
    p = c.probe()
    m = c.fake("127.0.0.2", kind="xf", beacon_interval=0.7)
    check(p.wait_log("Discovery beacon from Crossfire module 127.0.0.2 - activating", 3), "no beacon log")
    check(p.wait_state("streaming", 4, module="Crossfire 127.0.0.2"), f"not streaming ({p.last_state()})")
    st = p.wait_stats(lambda e: e["frames"] > 50, 3, p.mark())
    check(st and 75 <= st["hz"] <= 100, f"rate {st and st['hz']} Hz (expected ~90)")
    b0 = m.beacons_while_active
    time.sleep(2.5)
    check(m.beacons_while_active - b0 >= 3, "the fake XF module should keep beaconing")
    check(m.npost() == 1, f"{m.npost()} POSTs (beacons from the streaming module must be ignored)")
    k = p.mark()
    m.overshoot = True
    f = p.wait_frame(lambda e: e["raw"][0] == 32767, 2, k)
    check(f and f["val"][0] == 1024 and f["n"] == 16, f"overshoot frame {f}")
    return f"{st['hz']:.0f} Hz, jitter {st['jitter']:.2f} ms"


@scenario
def explicit_ip(c):
    """explicit module IP, no beacons: POST right after the bind, streaming; other modules' frames ignored"""
    m = c.fake("127.0.0.2", beacons_enabled=False)
    other = c.fake("127.0.0.3", http=2, beacons_enabled=False)
    p = c.probe("--module-ip", "127.0.0.2")
    lg = p.wait_log("Activation POST -> 127.0.0.2: 200 OK", 3)
    check(lg, "no POST")
    check(lg["t"] < 1000, f"POST result only {lg['t']} ms after start")
    check(p.wait_state("streaming", 3, module="WiFi 127.0.0.2"), f"not streaming ({p.last_state()})")
    # another module streams to us too (someone POSTed it from this host): filtered silently
    status, body = fake_elrs.post_udpcontrol("127.0.0.3", c.base + 2, dict(action="joystick_begin", interval=4000,
                                                                           channels=16), source_ip="127.0.0.1")
    check(status == 200, f"manual POST to the other module: {status} {body}")
    st = p.wait_stats(lambda e: e["dropped_other"] > 100, 4)
    check(st, "frames of the other module were not counted as filtered")
    check(not p.logs("Ignoring 2nd source"), "explicit mode filters before the lock logic: no warning")
    check(not p.logs("locked to 127.0.0.3"), "locked to the wrong module")
    check(m.npost() == 1 and other.npost() == 1, f"POSTs: {m.npost()} / {other.npost()}")


@scenario
def post_failure_retry(c):
    """POSTs answered 500 twice: logged, no hint, retried only every 5 s; the third succeeds -> streaming"""
    p = c.probe()
    m = c.fake("127.0.0.2", beacon_interval=1.0, http_fail_first=2)
    check(p.wait_log("Activation POST -> 127.0.0.2: 500", 3), "no 500 log")
    check(p.wait_state("streaming", 14, module="ELRS 127.0.0.2"), f"not streaming ({p.last_state()})")
    t = m.post_times()
    check(len(t) == 3, f"{len(t)} POSTs (expected 3)")
    gaps = [b - a for a, b in zip(t, t[1:])]
    check(all(4.8 <= g <= 6.5 for g in gaps), f"POST gaps {gaps} (throttle 5 s, beacons every 1 s)")
    check(len(p.logs("Activation POST -> 127.0.0.2: 500")) == 2, "two 500 results")
    check(len(p.logs("Discovery beacon from ELRS")) >= 8, "beacons in between are logged (throttled)")
    check(not p.logs("!! Activated OK"), "a failed POST must not start the 'no data' hint")
    return "gaps " + ", ".join(f"{g:.2f}" for g in gaps) + " s"


@scenario
def post_timeout(c):
    """a module that never answers: the POST times out after 5 s on the worker; the frame loop never blocks"""
    p = c.probe()
    m = c.fake("127.0.0.2", beacon_interval=1.0, http_delay=10)
    check(p.wait_log("Discovery beacon from ELRS module 127.0.0.2", 3), "no beacon")
    st = p.wait_state("activating 127.0.0.2", 1)
    check(st, "no 'activating' status")
    lg = p.wait_log("Activation failed for 127.0.0.2: timed out after 5 s", 8)
    check(lg, "no timeout log")
    dt = (lg["wall"] - m.post_times()[0])
    check(4.8 <= dt <= 5.8, f"timed out after {dt:.2f} s")
    time.sleep(0.5)
    poll = p.close()
    check(poll and poll["p99_us"] <= 2000, f"poll p99 {poll and poll['p99_us']} us")
    check(poll["max_us"] < POLL_MAX_US, f"poll max {poll['max_us']} us")
    return f"timeout after {dt:.2f} s, poll p99 {poll['p99_us']} us, max {poll['max_us']} us"


@scenario
def refused(c):
    """explicit IP with no web server: 'connection refused', a retry every ~5 s, the frame loop unaffected"""
    p = c.probe("--module-ip", "127.0.0.4", http=9)
    check(p.wait_log("Activation failed for 127.0.0.4: connection refused", 4), "no refused log")
    check(p.wait(lambda e: len(p.logs("Activation failed for 127.0.0.4")) >= 2, 8), "no retry")
    f = p.logs("Activation failed for 127.0.0.4")
    gap = f[1]["t"] - f[0]["t"]
    check(4800 <= gap <= 6500, f"retry after {gap} ms")
    with p.cv:
        k = p.ev.index(f[1])
    check(p.wait_state("connecting to 127.0.0.4", 1, k), f"status {p.last_state()}")
    poll = p.close()
    check(poll and poll["p99_us"] <= 2000 and poll["max_us"] < POLL_MAX_US, f"poll {poll}")
    return f"retry after {gap} ms, poll p99 {poll['p99_us']} us"


@scenario
def http_errors(c):
    """404 and a dropped connection are failures with clear logs; the explicit target is re-POSTed"""
    m = c.fake("127.0.0.2", beacons_enabled=False, http_status=404)
    p = c.probe("--module-ip", "127.0.0.2")
    check(p.wait_log("Activation POST -> 127.0.0.2: 404 Not Found", 3), "no 404 log")
    m.http_status, m.http_drop = 200, True
    check(p.wait_log("Activation failed for 127.0.0.2: closed without a reply", 8), "no 'closed' log")
    m.http_drop = False
    check(p.wait_state("streaming", 8), f"not streaming after the module recovered ({p.last_state()})")
    check(m.npost() == 3, f"{m.npost()} POSTs")


@scenario
def quiet_then_returns(c):
    """the stream stops: not live after 0.5 s, lock released after 3 s; it resumes without a POST;
    a power cycle beacons again and is re-activated"""
    p = c.probe()
    m = c.fake("127.0.0.2", beacon_interval=1.0)
    check(p.wait_state("streaming", 4), f"not streaming ({p.last_state()})")
    time.sleep(0.3)
    k = p.mark()
    m.mode = "silent"
    t_silent = time.monotonic()
    st = p.wait_stats(lambda e: e["present"] and not e["live"], 2.5, k)
    check(st, "the source still counted as live after 500 ms of silence (or released too early)")
    q = p.wait_log("Source 127.0.0.2 went quiet - releasing lock", 5, k)
    check(q, "no 'went quiet'")
    dt = q["wall"] - t_silent          # the last frame may have left a few ms before t_silent
    check(2.95 <= dt <= 3.6, f"released {dt:.3f} s after the last frame")
    check(p.wait_state("listening", 1, k), f"status after release: {p.last_state()}")
    # the module never stopped streaming in its own mind: frames come back, relock, no POST
    k = p.mark()
    m.mode = "normal"
    check(p.wait_state("streaming", 2, k), "no relock when the frames came back")
    check(m.npost() == 1, f"{m.npost()} POSTs (expected no new one)")
    # power cycle: silence, then beacons again -> a new POST
    k = p.mark()
    m.stop()
    check(p.wait_log("went quiet", 5, k), "no release after power off")
    m.start()
    check(p.wait_state("streaming", 5, k), "not streaming after the power cycle")
    check(m.npost() == 2 and len(p.logs("Activation POST -> 127.0.0.2: 200 OK")) == 2,
          "the power-cycled module must be POSTed again")
    return f"released after {dt:.2f} s"


@scenario
def crossfire_placeholders(c):
    """placeholder frames: never a source, one warning, a status that says why; corrupt frames
    mid-stream are dropped without flicker"""
    p = c.probe()
    m = c.fake("127.0.0.2", kind="xf", beacon_interval=0.7, mode="placeholder")
    s = p.wait_state("no stick data from 127.0.0.2", 4)
    check(s, f"status {p.last_state()}")
    check(s["detail"] == "module found, but it is sending no stick data", f"detail {s['detail']}")
    st = p.wait_stats(lambda e: e["dropped_filter"] > 100, 3)
    check(st and st["applied"] == 0 and not st["present"], f"placeholders applied? {st}")
    check(len(p.logs("is streaming placeholder frames - no stick data")) == 1, "warn once")
    check(not p.events("frame") and not [e for e in p.events("state") if e["state"] == "streaming"], "streamed")
    check(not p.logs("!! Activated OK"), "the network hint must not show for a placeholder module")
    # real data with every 10th frame corrupt
    k = p.mark()
    m.corrupt_every = 10
    m.values = [1000 * (i + 1) for i in range(16)]
    m.mode = "normal"
    check(p.wait_state("streaming", 2, k), "no streaming after real data")
    p.wait_stats(lambda e: True, 2, p.mark())          # a window that may still hold placeholders
    s1 = p.wait_stats(lambda e: True, 2, p.mark())
    s2 = p.wait_stats(lambda e: True, 2, p.mark())
    check(s1 and s2, "no stats")
    frames = p.events("frame", k)
    check(frames and all(f["raw"] == m.values for f in frames), "a corrupt frame was applied")
    check(len([e for e in p.events("state", k) if e["state"] == "streaming"]) == 1, "state flicker")
    bad = s2["dropped_filter"] - s1["dropped_filter"]
    check(4 <= bad <= 15, f"{bad} corrupt frames dropped in 1 s (expected ~9)")
    check(70 <= s2["hz"] <= 100, f"rate {s2['hz']}")
    return f"{s2['hz']:.0f} Hz applied, {bad} corrupt frames/s dropped"


@scenario
def rate_1000us(c):
    """--interval-us 1000: the POST asks for it and ~1 kHz arrives"""
    p = c.probe("--interval-us", "1000")
    m = c.fake("127.0.0.2")
    check(p.wait_state("streaming", 4), f"not streaming ({p.last_state()})")
    check(m.posts[0]["body"] == "action=joystick_begin&interval=1000&channels=16", m.posts[0]["body"])
    p.wait_stats(lambda e: True, 2, p.mark())          # skip the window with the start in it
    rates = []
    for _ in range(3):
        st = p.wait_stats(lambda e: True, 2, p.mark())
        check(st, "no stats")
        rates.append(st["hz"])
    avg = sum(rates) / len(rates)
    check(750 <= avg <= 1150, f"rate {rates} Hz (expected ~1000)")
    check(st["dropped_filter"] == 0 and st["malformed"] == 0, f"drops {st}")
    poll = p.close()
    return f"{avg:.0f} Hz (windows {', '.join(f'{r:.0f}' for r in rates)}), jitter {st['jitter']:.2f} ms, poll p99 {poll['p99_us']} us"


@scenario
def interval_channels(c):
    """--interval-us 2000 --channels 8: the request carries them, frames have 8 channels at ~500 Hz"""
    p = c.probe("--interval-us", "2000", "--channels", "8")
    m = c.fake("127.0.0.2")
    check(p.wait_state("streaming", 4), f"not streaming ({p.last_state()})")
    check(m.posts[0]["body"] == "action=joystick_begin&interval=2000&channels=8", m.posts[0]["body"])
    check(p.wait_frame(lambda e: e["n"] == 8 and len(e["val"]) == 8, 2), "no 8-channel frame")
    p.wait_stats(lambda e: True, 2, p.mark())
    st = p.wait_stats(lambda e: True, 2, p.mark())
    check(st and 400 <= st["hz"] <= 600, f"rate {st and st['hz']} (expected ~500)")
    check(m.state()["channels"] == 8 and m.state()["interval_us"] == 2000, f"module state {m.state()}")
    return f"{st['hz']:.0f} Hz"


@scenario
def two_modules(c):
    """two modules beacon: both POSTed, lock to the first frame's source, one warning about the
    other; when the first goes quiet the other takes over"""
    p = c.probe()
    # the replies are late, so the second beacon is seen before the first module streams (a module
    # that beacons while we are already locked is ignored, like the reference: Beacon B8)
    a = c.fake("127.0.0.2", beacon_interval=1.0, http_delay=0.3)
    b = c.fake("127.0.0.3", beacon_interval=1.0, http_delay=0.3, first_beacon=0.15)
    s = p.wait_state("streaming", 5)
    check(s, f"not streaming ({p.last_state()})")
    first = s["module"].split()[-1]
    second = "127.0.0.3" if first == "127.0.0.2" else "127.0.0.2"
    check(p.wait_log(f"Ignoring 2nd source {second} (locked to {first})", 4), "no 2nd-source warning")
    check(a.npost() == 1 and b.npost() == 1, f"POSTs {a.npost()} / {b.npost()}")
    time.sleep(1.5)
    check(len(p.logs("Ignoring 2nd source")) == 1, "warned more than once")
    k = p.mark()
    (a if first == "127.0.0.2" else b).mode = "silent"
    t = time.monotonic()
    s = p.wait_state("streaming", 5, k, module=f"ELRS {second}")
    check(s, "no handover")
    dt = s["wall"] - t
    check(2.95 <= dt <= 3.6, f"handover after {dt:.3f} s")
    check(p.logs(f"Joystick source locked to {second}", k), "no lock log")
    return f"locked to {first}, handover after {dt:.2f} s"


@scenario
def restart(c):
    """the receiver restarts while the module streams: streaming again at once, without a POST"""
    p = c.probe(name="probe1")
    m = c.fake("127.0.0.2")
    check(p.wait_state("streaming", 4), f"not streaming ({p.last_state()})")
    p.close()
    t = time.monotonic()
    p2 = c.probe(name="probe2")
    s = p2.wait_state("streaming", 2)
    check(s, f"not streaming after the restart ({p2.last_state()})")
    dt = s["wall"] - t
    check(dt < 1.0, f"took {dt:.2f} s")
    check(m.npost() == 1, f"{m.npost()} POSTs")
    # stop/start commands: the port is free while stopped
    k = p2.mark()
    p2.cmd("stop")
    check(p2.wait_state("off", 2, k), "no 'off'")
    s = c.udp("0.0.0.0", c.base)
    s.close()
    c.socks.remove(s)
    p2.cmd("start")
    check(p2.wait_state("streaming", 2, k), "no streaming after start")
    return f"streaming {dt * 1000:.0f} ms after the restart"


@scenario
def port_in_use(c):
    """another program holds the UDP port: Error 'UDP port N in use'; once it is free, listening within ~5 s"""
    blocker = c.udp("0.0.0.0", c.base)
    p = c.probe(listening=False)
    s = p.wait_state(f"UDP port {c.base} in use", 3)
    check(s, f"status {p.last_state()}")
    check(p.wait_log(f"ERROR: cannot listen on UDP port {c.base}", 1), "no error log")
    t = time.monotonic()
    blocker.close()
    c.socks.remove(blocker)
    s = p.wait_state("listening", 7)
    check(s, f"status {p.last_state()}")
    dt = s["wall"] - t
    check(dt <= 5.5, f"recovered after {dt:.2f} s")
    check(len(p.logs("ERROR: cannot listen")) == 1, "the error was logged more than once")
    return f"recovered {dt:.2f} s after the port was freed"


@scenario
def second_client_steals(c):
    """another client POSTs the module: our stream stops, the lock is released, auto mode can't get it
    back (ELRS doesn't beacon while streaming); setting the module IP takes it back at once"""
    p = c.probe()
    m = c.fake("127.0.0.2")
    elsewhere = c.udp("127.0.0.1", 0)                  # "another PC": where the stolen stream lands
    m.stream_port_for["127.0.0.3"] = elsewhere.getsockname()[1]
    check(p.wait_state("streaming", 4), f"not streaming ({p.last_state()})")
    k = p.mark()
    status, _ = fake_elrs.post_udpcontrol("127.0.0.2", c.base + 1, dict(action="joystick_begin", interval=4000,
                                                                        channels=16), source_ip="127.0.0.3")
    check(status == 200 and m.state()["remote_ip"] == "127.0.0.3", f"steal failed: {status} {m.state()}")
    check(p.wait_log("went quiet", 4, k), "no release")
    time.sleep(2)
    check(m.npost() == 2, f"{m.npost()} POSTs (auto mode has no way to re-POST)")
    check(p.last_state() == "listening", p.last_state())
    k = p.mark()
    p.cmd("target 127.0.0.2")
    check(p.wait_log("Target module set to 127.0.0.2 - connecting...", 2, k), "no target log")
    s = p.wait_state("streaming", 2, k)
    check(s, f"explicit target did not take the stream back ({p.last_state()})")
    check(m.state()["remote_ip"] == "127.0.0.1", m.state())


@scenario
def garbage(c):
    """a module that sends garbage: nothing applied, counted as malformed, the receiver survives"""
    p = c.probe()
    m = c.fake("127.0.0.2", mode="garbage")
    st = p.wait_stats(lambda e: e["malformed"] > 200, 5)
    check(st, "garbage not counted")
    check(not p.events("frame") and st["applied"] == 0, "garbage was applied")
    k = p.mark()
    m.mode = "normal"
    check(p.wait_state("streaming", 2, k), "no streaming after garbage")
    return f"{st['malformed']} malformed datagrams"


@scenario
def probe_expectations(c):
    """the probe alone (no stdin): exits 0 as soon as its expectations are met, 1 when time runs out"""
    c.fake("127.0.0.2", beacon_interval=0.5)
    base = [c.exe, "--port", str(c.base), "--http-port", str(c.base + 1)]

    def run(*args, timeout=15):
        t = time.monotonic()
        r = subprocess.run(base + list(args), capture_output=True, text=True, timeout=timeout)
        ev = [json.loads(x) for x in r.stdout.splitlines() if x.startswith("{")]
        return r.returncode, ev, time.monotonic() - t

    code, ev, dt = run("--expect-src", "127.0.0.2", "--expect-nch", "16", "--min-frames", "200",
                       "--min-rate", "200", "--max-rate", "300", "--duration-ms", "8000")
    ex = [e for e in ev if e["ev"] == "exit"]
    check(code == 0 and ex and ex[0]["reason"] == "expectations met", f"exit {code} {ex}")
    check(any(e["ev"] == "stats" and e["hz"] >= 200 for e in ev), "no rate in the output")
    check(any(e["ev"] == "poll" for e in ev), "no poll statistics")
    # the module still streams to this host: a restarted probe locks at once, but to the wrong expectation
    code2, ev2, dt2 = run("--expect-src", "127.0.0.9", "--duration-ms", "1500")
    ex2 = [e for e in ev2 if e["ev"] == "exit"]
    check(code2 == 1 and ex2 and "duration" in ex2[0]["reason"], f"exit {code2} {ex2}")
    check(any(e["ev"] == "frame" and e["src"] == "127.0.0.2" for e in ev2), "no frames in the second run")
    code3, _, _ = run("--bogus")
    check(code3 == 2, f"unknown argument: exit {code3}")
    return f"met after {dt:.1f} s"


@scenario
def request_change(c):
    """a new rate, then a new channel count, while an ELRS module streams: each restarts it (joystick_end,
    then joystick_begin with the new request), the lock is kept and the new rate / count arrives"""
    p = c.probe()
    m = c.fake("127.0.0.2")
    check(p.wait_state("streaming", 4, module="ELRS 127.0.0.2"), f"not streaming ({p.last_state()})")
    st = p.wait_stats(lambda e: e["frames"] > 100, 3, p.mark())
    check(st and 200 <= st["hz"] <= 300, f"rate {st and st['hz']} Hz before the change (expected ~250)")
    k = p.mark()
    p.cmd("interval 2000")
    check(p.wait_log("Restarting the stream of 127.0.0.2: 500 Hz, 16 channels", 2, k), "no restart log")
    check(p.wait_log("Stream end POST -> 127.0.0.2: 200", 3, k), "no joystick_end result")
    check(p.wait_log("Activation POST -> 127.0.0.2: 200 OK", 3, k), "no joystick_begin result")
    posts = [x for x in m.posts if x["method"] == "POST"]
    check([x["params"].get("action") for x in posts] == ["joystick_begin", "joystick_end", "joystick_begin"],
          f"POSTs {[x['body'] for x in posts]}")
    check(posts[1]["body"] == "action=joystick_end", f"end body {posts[1]['body']!r}")
    check(posts[2]["body"] == "action=joystick_begin&interval=2000&channels=16", f"begin body {posts[2]['body']!r}")
    check(m.state()["active"] and m.state()["interval_us"] == 2000, f"module state {m.state()}")
    p.wait_stats(lambda e: True, 2, p.mark())          # skip the window with the change in it
    st = p.wait_stats(lambda e: True, 2, p.mark())
    check(st and 400 <= st["hz"] <= 600, f"rate {st and st['hz']} Hz after the change (expected ~500)")
    check(st["interval_us"] == 2000 and st["channels"] == 16 and st["restarts"] == 1, f"probe {st}")
    check(st["posts"] == 2 and st["posts_ok"] == 2 and st["pending"] == 0, f"probe {st}")
    k2 = p.mark()
    p.cmd("channels 8")
    check(p.wait_log("Restarting the stream of 127.0.0.2: 500 Hz, 8 channels", 2, k2), "no restart log")
    check(p.wait_frame(lambda e: e["n"] == 8 and len(e["val"]) == 8, 3, k2), "no 8-channel frames after the restart")
    check(m.state()["channels"] == 8 and m.state()["interval_us"] == 2000, f"module state {m.state()}")
    time.sleep(0.5)
    check(m.npost() == 5, f"{m.npost()} POSTs (expected 5: begin, then end + begin twice)")
    check(not p.logs("restarting its stream", k), "16-channel frames still on their way restarted it once more")
    check(not [e for e in p.events("state", k) if e["state"] != "streaming"], "the lock was lost during a restart")
    return f"{st['hz']:.0f} Hz after the change"


@scenario
def request_crossfire(c):
    """Crossfire ignores the rate and channel count: a change is logged once and never sent (no joystick_end)"""
    p = c.probe()
    m = c.fake("127.0.0.2", kind="xf", beacon_interval=0.7)
    check(p.wait_state("streaming", 4, module="Crossfire 127.0.0.2"), f"not streaming ({p.last_state()})")
    k = p.mark()
    p.cmd("interval 2000")
    check(p.wait_log("Crossfire module 127.0.0.2 ignores the rate and channel count", 2, k), "no log")
    p.cmd("channels 8")
    time.sleep(1.5)
    check(m.npost() == 1, f"{m.npost()} POSTs (expected only the first joystick_begin)")
    check(not [x for x in m.posts if x["params"].get("action") == "joystick_end"], "Crossfire got a joystick_end")
    check(len(p.logs("ignores the rate and channel count", k)) == 1, "logged more than once")
    st = p.wait_stats(lambda e: True, 2, p.mark())
    check(st and 75 <= st["hz"] <= 100, f"rate {st and st['hz']} Hz (expected ~90)")
    check(st["restarts"] == 0 and st["interval_us"] == 2000 and st["channels"] == 8, f"probe {st}")
    f = p.wait_frame(lambda e: True, 2, p.mark())
    check(f and f["n"] == 16, f"frame {f}")
    return f"{st['hz']:.0f} Hz"


@scenario
def leftover_request(c):
    """a module still streaming another app's request (8 channels at 100 Hz) to this host, which ELRS keeps
    until it is restarted: restarted once with ours (16 channels at 250 Hz)"""
    m = c.fake("127.0.0.2", beacons_enabled=False)
    status, body = fake_elrs.post_udpcontrol("127.0.0.2", c.base + 1, dict(action="joystick_begin", interval=10000,
                                                                           channels=8), source_ip="127.0.0.1")
    check(status == 200 and m.state()["channels"] == 8, f"the other app's POST: {status} {body} {m.state()}")
    p = c.probe()
    check(p.wait_log("127.0.0.2 streams 8 channels, 16 asked for (another app's request?) - restarting its stream", 3),
          "no restart for the leftover request")
    check(p.wait_frame(lambda e: e["n"] == 16, 3), "no 16-channel frames after the restart")
    check(m.state()["channels"] == 16 and m.state()["interval_us"] == 4000, f"module state {m.state()}")
    p.wait_stats(lambda e: True, 2, p.mark())
    st = p.wait_stats(lambda e: True, 2, p.mark())
    check(st and 200 <= st["hz"] <= 300, f"rate {st and st['hz']} Hz (expected ~250)")
    check(st["restarts"] == 1 and m.npost() == 3, f"restarts {st['restarts']}, {m.npost()} POSTs")
    check(len(p.logs("restarting its stream")) == 1, "restarted more than once")
    return f"{st['hz']:.0f} Hz after the restart"


# ---------------------------------------------------------------- runner
def find_probe():
    for d in ("native-edgetx", "native-ref"):
        for exe in ("test_wifi_live.exe", "test_wifi_live"):
            p = os.path.join(ROOT, "build", d, exe)
            if os.path.isfile(p):
                return p
    return None


def main():
    global VERBOSE
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="scenarios to run (default: all)")
    ap.add_argument("--probe", help="the test_wifi_live executable")
    ap.add_argument("--base-port", type=int, default=27500)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every probe event")
    a = ap.parse_args()
    VERBOSE = a.verbose
    if a.list:
        for i, fn in enumerate(SCENARIOS):
            print(f"{fn.__name__:24} {fn.__doc__.split(chr(10))[0]}")
        return 0
    exe = a.probe or find_probe()
    if not exe or not os.path.isfile(exe):
        print("no probe: build it (host/build.sh) or pass --probe", file=sys.stderr)
        return 2
    exe = os.path.abspath(exe)
    unknown = [n for n in a.names if n not in [f.__name__ for f in SCENARIOS]]
    if unknown:
        print("unknown scenario(s): " + ", ".join(unknown), file=sys.stderr)
        return 2
    fails = 0
    t_all = time.monotonic()
    for i, fn in enumerate(SCENARIOS):
        if a.names and fn.__name__ not in a.names:
            continue
        c = Ctx(exe, a.base_port + 10 * i)
        t = time.monotonic()
        try:
            note = fn(c)
            print(f"PASS {fn.__name__:24} {time.monotonic() - t:5.1f} s  {note or ''}", flush=True)
        except Exception as e:  # noqa: BLE001 (a failed scenario must not stop the others)
            fails += 1
            print(f"FAIL {fn.__name__:24} {time.monotonic() - t:5.1f} s  {e}", flush=True)
            if not isinstance(e, Failed):
                traceback.print_exc()
            for p in c.probes:
                p.dump()
        finally:
            c.cleanup()
    print(f"{'all passed' if not fails else f'{fails} failed'} in {time.monotonic() - t_all:.0f} s")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
