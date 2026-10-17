#!/usr/bin/env python3
"""VTX control end to end: the real program (headless: --video none --no-inputs --no-audio) and the fake
VTX of host/tests/fake_vtx.py, through the web API. The line is TCP (any OS: --vtx-port tcp:...) or, with
--pty on Linux, a pty pair (--vtx-port tty:/dev/pts/N: the program's real termios path).

    python host/tests/vtx_live_test.py [--exe PATH] [--pty] [--keep]

Covers: finding a SmartAudio 2.1 VTX and setting it from the settings; POST /api/vtx (band/channel,
power, pit mode, a frequency) and its checks; a button press set back; unplugged and plugged back in; the
cable pulled (TCP); loading a VTX table, using its custom band, downloading it, a broken one, back to the
built-in one; the VTX swapped for a Tramp one (auto finds it); send-only; off; the status page's line.
Standard library only; exit status 0 when all pass."""
import argparse
import glob
import http.client
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import fake_vtx  # noqa: E402

EXE = ".exe" if os.name == "nt" else ""
PORT = 0

TABLE = {
    "description": "StickTime Pi test table",
    "version": "1.0",
    "vtx_table": {
        "bands_list": [
            {"name": "BOSCAM_A", "letter": "A", "is_factory_band": True, "frequencies": [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725]},
            {"name": "BOSCAM_B", "letter": "B", "is_factory_band": True, "frequencies": [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866]},
            {"name": "CUSTOM", "letter": "X", "is_factory_band": False, "frequencies": [5600, 5620, 5640, 0, 5680, 5700, 5720, 5740]},
        ],
        "powerlevels_list": [{"value": 14, "label": "25"}, {"value": 20, "label": "100"}, {"value": 26, "label": "400"}],
    },
}


class Fail(Exception):
    pass


def ok(cond, msg):
    if not cond:
        raise Fail(msg)


def req(method, path, body=None, timeout=10):
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=timeout)
    h = {"Host": "127.0.0.1"}
    if method != "GET":
        h["X-StickTime"] = "1"
    if isinstance(body, str):
        body = body.encode()
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, data


def get_json(path):
    st, data = req("GET", path)
    ok(st == 200, f"GET {path}: {st} {data[:200]!r}")
    return json.loads(data)


def post(path, body, expect=200):
    st, data = req("POST", path, body)
    ok(st == expect, f"POST {path} {body!r}: {st} {data[:300]!r}")
    return json.loads(data) if data else None


def vtx():
    return get_json("/api/vtx")


def wait_for(pred, what, timeout=20.0):
    t0, last = time.time(), None
    while time.time() - t0 < timeout:
        try:
            last = pred()
        except (OSError, ValueError, http.client.HTTPException):
            last = None
        if last:
            return last
        time.sleep(0.2)
    try:
        v = vtx()
        info = f"state={v['state']} message={v['message']!r} error={v['error']!r}"
    except Exception as e:  # noqa: BLE001
        info = repr(e)
    raise Fail(f"timed out waiting for {what} ({info})")


def ready(proto=None):
    def p():
        v = vtx()
        return v if v["state"] == "ready" and v["in_sync"] and (proto is None or v["protocol"] == proto) else None
    return p


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def find_exe():
    c = [p for p in glob.glob(os.path.join(ROOT, "build", "*", "sticktime-pi" + EXE)) if os.path.isfile(p)]
    if not c:
        raise SystemExit("no sticktime-pi found: build it (host/build.sh) or pass --exe")
    return max(c, key=os.path.getmtime)


def settings_file(base):
    with open(os.path.join(base, "sticktime.txt"), encoding="utf-8") as f:
        return f.read()


def run(exe, use_pty, keep):
    global PORT
    base = tempfile.mkdtemp(prefix="stp-vtx-")
    os.makedirs(os.path.join(base, "sd", "SCRIPTS", "TOOLS"))
    os.makedirs(os.path.join(base, "web"))
    with open(os.path.join(base, "web", "index.html"), "w") as f:
        f.write("<!doctype html><title>StickTime Pi</title>\n")
    with open(os.path.join(base, "sticktime.txt"), "w", newline="\n") as f:
        f.write("# vtx_live_test\nvideo = av\nvtx = auto\nvtx_band = R\nvtx_channel = 1\nvtx_power = 1\n")
    dev = fake_vtx.FakeVtx("sa", 3)
    link = fake_vtx.PtyLink(dev) if use_pty else fake_vtx.TcpLink(dev)
    spec = "tty:" + link.path if use_pty else f"tcp:127.0.0.1:{link.port}"
    PORT = free_port()
    log = open(os.path.join(base, "program.log"), "wb")
    cmd = [exe, "--video", "none", "--no-inputs", "--no-audio", "--port", str(PORT), "--base", base,
           "--settings", os.path.join(base, "sticktime.txt"), "--state", os.path.join(base, "state"),
           "--fonts", os.path.join(ROOT, "host", "fonts"), "--rt-prio", "0", "--cpu", "-1", "--vtx-port", spec]
    print("program:", " ".join(cmd))
    p = subprocess.Popen(cmd, stdout=log, stderr=log)
    passed, failed = [], []

    def step(name, fn):
        t0 = time.time()
        try:
            fn()
            passed.append(name)
            print(f"ok    {name} ({time.time() - t0:.1f} s)")
        except Exception as e:  # noqa: BLE001
            failed.append(name)
            print(f"FAIL  {name}: {type(e).__name__ if not isinstance(e, Fail) else ''} {e}")

    try:
        wait_for(lambda: req("GET", "/api/vtx")[0] == 200, "the web server", 30)

        def found():
            v = wait_for(ready("smartaudio"), "SmartAudio 2.1 found and set")
            s = dev.state()
            ok(v["version"] == "2.1", v["version"])
            ok(s["channel"] == 32 and s["dbm"] == 14, f"the VTX: {s}")
            ok(v["band"] == "R" and v["channel"] == 1 and v["freq"] == 5658, f"status: {v['band']}{v['channel']} {v['freq']}")
            ok(v["power"]["level"] == 1 and v["power"]["mw"] == 25, v["power"])
            ok(v["table"]["source"] == "default" and len(v["table"]["powers"]) == 3, v["table"]["source"])
            ok(v["stats"]["echo_ok"] > 0 and v["stats"]["replies"] > 0, v["stats"])
            ok(v["device"] == (link.path if use_pty else spec), v["device"])
            st = get_json("/api/status")
            ok(st["vtx"]["state"] == "ready" and "SmartAudio 2.1" in st["vtx"]["text"] and "R1 5658 MHz" in st["vtx"]["text"], st["vtx"])
        step("found and set from the settings", found)

        def set_band_power():
            r = post("/api/vtx", "band = F\nchannel = 4\npower = 3\n")
            ok(set(r["saved"]) >= {"vtx_band", "vtx_channel", "vtx_power"}, r)
            wait_for(lambda: dev.state()["channel"] == 27 and dev.state()["dbm"] == 26, "F4 at 26 dBm on the VTX")
            v = wait_for(ready(), "in sync")
            ok(v["band"] == "F" and v["channel"] == 4 and v["power"]["level"] == 3, v)
            txt = settings_file(base)
            ok("vtx_band = F" in txt and "vtx_channel = 4" in txt and "vtx_power = 3" in txt, txt)
        step("POST /api/vtx band, channel, power", set_band_power)

        def button():
            dev.press(channel=5, dbm=20)
            wait_for(lambda: dev.state()["channel"] == 27 and dev.state()["dbm"] == 26, "the Pi setting it back")
        step("a button press is set back", button)

        def pit():
            post("/api/vtx", "pit = on\n")
            wait_for(lambda: dev.state()["pit"], "pit mode on the VTX")
            wait_for(lambda: vtx()["pit"], "pit mode in the status")
            post("/api/vtx", "pit = off\n")
            wait_for(lambda: not dev.state()["pit"], "pit mode off")
        step("pit mode", pit)

        def freq():
            post("/api/vtx", "freq = 5810\n")               # (5800 is F4, where it is: nothing to do then)
            wait_for(lambda: dev.state()["freq"] == 5810 and dev.state()["mode"] & fake_vtx.MODE_FREQ, "5810 MHz by frequency")
            v = wait_for(ready(), "in sync")
            ok(v["freq"] == 5810 and v["freq_mode"] and v["band"] == "", v["freq"])
            post("/api/vtx", "band = F\nchannel = 4\n")             # a band again: by channel, the frequency setting cleared
            wait_for(lambda: dev.state()["channel"] == 27 and not dev.state()["mode"] & fake_vtx.MODE_FREQ, "F4 by channel")
            ok("vtx_freq = 0" in settings_file(base), "vtx_freq cleared")
        step("a frequency instead of band/channel", freq)

        def checks():
            st, data = req("POST", "/api/vtx", "band = Z\npower = 9\nchannel = 0\npit = maybe\nfreq = 123\n")
            j = json.loads(data)
            ok(st == 400 and set(j["fields"]) == {"band", "power", "channel", "pit", "freq"}, f"{st} {j}")
            ok("A, B, E, F, R" in j["fields"]["band"], j["fields"]["band"])
            st, _ = req("POST", "/api/vtx", "")
            ok(st == 400, f"empty: {st}")
            c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
            c.request("POST", "/api/vtx", body=b"power = 1\n", headers={"Host": "127.0.0.1"})
            ok(c.getresponse().status == 403, "without X-StickTime")
            c.close()
        step("POST /api/vtx checks", checks)

        def unplug():
            dev.set(plugged=False)
            wait_for(lambda: vtx()["state"] in ("searching", "not_found") and not vtx()["found"], "lost", 30)
            dev.set(plugged=True, channel=0, freq=5865, mode=fake_vtx.MODE_UNLOCKED, dbm=14)   # power-cycled
            wait_for(ready("smartaudio"), "found again", 30)
            wait_for(lambda: dev.state()["channel"] == 27 and dev.state()["dbm"] == 26, "set again")
        step("unplugged and plugged back in", unplug)

        if not use_pty:
            def cable():
                link.cut()
                time.sleep(0.5)
                dev.press(channel=1)
                wait_for(lambda: dev.state()["channel"] == 27, "reconnected and set back", 30)
                ok(link.accepted >= 2, link.accepted)
            step("the line closes and opens again", cable)

        def table():
            st, data = req("PUT", "/api/vtx/table", json.dumps(TABLE))
            ok(st == 200, f"PUT: {st} {data!r}")
            j = json.loads(data)
            ok(j["bands"] == 3 and j["powers"] == 3, j)
            wait_for(lambda: vtx()["table"]["source"] == "file", "the file's table in use")
            post("/api/vtx", "band = X\nchannel = 2\npower = 2\n")
            wait_for(lambda: dev.state()["freq"] == 5620 and dev.state()["mode"] & fake_vtx.MODE_FREQ, "X2 by frequency")
            v = wait_for(ready(), "in sync")
            ok(v["band"] == "X" and v["power"]["label"] == "100", v)
            st, data = req("POST", "/api/vtx", "channel = 4\n")
            ok(st == 400 and "isn't available" in data.decode(), f"{st} {data!r}")
            st, data = req("GET", "/api/vtx/table")
            ok(st == 200 and json.loads(data)["vtx_table"]["bands_list"][2]["letter"] == "X", data[:100])
            st, data = req("PUT", "/api/vtx/table", '{"vtx_table": {"bands_list": []}}')
            ok(st == 400 and b"no bands" in data, f"{st} {data!r}")
            st, data = req("DELETE", "/api/vtx/table")
            ok(st == 200, st)
            wait_for(lambda: vtx()["table"]["source"] == "default", "the built-in table again")
            wait_for(lambda: vtx()["state"] == "error" and "Band X" in vtx()["error"], "band X is gone")
            post("/api/vtx", "band = R\nchannel = 2\npower = 3\n")
            wait_for(lambda: dev.state()["channel"] == 33, "R2")
        step("VTX table: load, use, download, broken, default", table)

        def tramp():
            t = fake_vtx.FakeVtx("tramp")
            link.vtx = t
            v = wait_for(ready("tramp"), "the Tramp VTX found (auto)", 40)
            ok(t.state()["tfreq"] == 5695 and t.state()["tpower"] == 200, t.state())
            ok(v["freq"] == 5695 and v["limits"]["max_power"] == 600 and v["temp_c"] in (None, 31), v)
            wait_for(lambda: vtx()["temp_c"] == 31, "its temperature")
            t.press(freq=5800)
            wait_for(lambda: t.state()["tfreq"] == 5695, "set back")
        step("swapped for a Tramp VTX", tramp)

        def send_only():
            t = link.vtx
            r = post("/api/settings", "vtx = tramp\nvtx_send_only = on\nvtx_band = F\nvtx_channel = 1\n")
            ok(r["reboot"] is False, r)
            wait_for(lambda: t.state()["tfreq"] == 5740, "sent 5740 MHz")
            v = wait_for(lambda: vtx() if vtx()["state"] == "send_only" and "can't show" in vtx()["message"] else None, "send-only")
            ok(v["send_only"] and not v["found"], v)
        step("send-only", send_only)

        def off():
            r = post("/api/settings", "vtx = off\n")
            wait_for(lambda: vtx()["state"] == "off", "off")
            ok(get_json("/api/status")["vtx"]["text"] == "off", "status line")
            r = post("/api/settings", "vtx = auto\nvtx_send_only = off\n")
            ok(r["reboot"] is False, f"off -> on with one pin (gpio4) needs no reboot: {r}")
            r = post("/api/settings", "vtx_port = uart5\n")
            ok(r["reboot"] is True, f"the two-pin mode needs its UART overlay: {r}")
            r = post("/api/settings", "vtx = off\n")
            ok(r["reboot"] is False, f"off: {r}")
        step("off, and on again (reboot flag)", off)
    finally:
        p.terminate()
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
        log.close()
        link.close()
        if failed:
            with open(os.path.join(base, "program.log"), "rb") as f:
                tail = f.read().decode("utf-8", "replace").splitlines()[-30:]
            print("program.log (end):\n  " + "\n  ".join(tail))
        if keep or failed:
            print("kept", base)
        else:
            shutil.rmtree(base, ignore_errors=True)
    print(f"{len(passed)}/{len(passed) + len(failed)} passed" + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe")
    ap.add_argument("--pty", action="store_true", help="a pty instead of TCP (Linux)")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    exe = os.path.abspath(a.exe) if a.exe else find_exe()
    return run(exe, a.pty, a.keep)


if __name__ == "__main__":
    sys.exit(main())
