#!/usr/bin/env python3
"""Tests of the web server and every route of docs/WEB_API.md, against the real C server
(host/tests/web_test_main.c: web_start() plus a fake frame loop) on a temporary base folder with
fake helper programs (sticktime-wifi, sticktime-update, the power command).

    python host/tests/web_api_test.py [--exe PATH] [--keep] [-k NAME]... [--no-slow]

--exe     the web_test_main program (default: the newest build/*/web_test_main[.exe] or
          build/agent-web/obj*/web_test_main[.exe])
--keep    keep the temporary base folder (its server.log has the server's log)
-k NAME   run only the tests whose name contains NAME (repeatable)
--no-slow skip the 15 s idle-timeout tests
--tmp DIR make the base folder in DIR (e.g. a mounted vfat image, like the Pi's boot partition)

Runs on Windows (helpers are "python <libexec>/<name>.py") and on Linux, the Pi or an arm64 VM
(helpers are executable scripts). Python 3.11+, standard library only. Exit status 0 when all pass.
"""
import argparse
import glob
import gzip
import http.client
import json
import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WIN = os.name == "nt"
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fake_vtx  # noqa: E402  (a fake SmartAudio / Tramp VTX for /api/vtx)

VTX_DEV = fake_vtx.FakeVtx("sa", 3)
VTX_LINK = None                          # started in main()
EXE = ".exe" if WIN else ""

PORT = 0
BASE = ""
MAX_UPLOAD = 1 << 20                     # the server's max_upload (songs), small to test 413

SECRETS = ["secret-pass-123", "ghp_TOPSECRET42"]
SETTINGS = """\
# StickTime Pi settings (web_api_test)
# This comment must survive every save.

video = av          # composite to the VTX
tv = ntsc
volume = 70
wifi_ssid = Home
wifi_password = "secret-pass-123"
github_token = ghp_TOPSECRET42
my_custom_key = keep me   # unknown keys stay
this line has no equals sign
sd_write = game
"""
# ETX_EVT_* (etx.h) and APP_KEY_* (app.h) as the fake frame loop logs them
KEYS = {"enter": 0x202, "exit": 0x201, "next": 0x1E00, "prev": 0x1D00, "inc": 0x1E00, "dec": 0x1D00,
        "nextpage": 0x204, "prevpage": 0x203, "menu": 0x206, "up": -101, "down": -102}
CMD_RELOAD_GAME, CMD_RELOAD_SETTINGS, CMD_QUIT, CMD_SCREENSHOT, CMD_MUSIC_NEXT, CMD_SONGS_CHANGED = 1, 2, 4, 8, 16, 32
SLOT = "SCRIPTS/TOOLS/StickTimeTracks"


class Fail(Exception):
    pass


def ok(cond, msg="check failed"):
    if not cond:
        raise Fail(msg)


def eq(a, b, what=""):
    if a != b:
        raise Fail(f"{what}: {a!r} != {b!r}" if what else f"{a!r} != {b!r}")


# ---------------------------------------------------------------- HTTP helpers
class R:
    def __init__(self, status, headers, body, reason=""):
        self.status, self.headers, self.body, self.reason = status, headers, body, reason

    def h(self, name):
        return self.headers.get(name.lower())

    def json(self):
        try:
            return json.loads(self.body.decode("utf-8"))
        except Exception as e:
            raise Fail(f"not JSON ({e}): {self.body[:200]!r}")

    def __repr__(self):
        return f"<{self.status} {self.headers.get('content-type')} {self.body[:160]!r}>"


def hostport():
    return f"127.0.0.1:{PORT}"


def request(method, path, body=None, headers=None, host=None, xst=None, timeout=15):
    """one request on its own connection (http.client); X-StickTime: 1 on mutations unless xst=False"""
    h = {"Host": host if host is not None else hostport()}
    if xst is None:
        xst = method in ("POST", "PUT", "DELETE")
    if xst:
        h["X-StickTime"] = "1"
    h.update(headers or {})
    if isinstance(body, str):
        body = body.encode("utf-8")
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=timeout)
    try:
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        return R(r.status, {k.lower(): v for k, v in r.getheaders()}, data, r.reason)
    finally:
        c.close()


def get(path, **kw):
    return request("GET", path, **kw)


def post(path, body=b"", **kw):
    return request("POST", path, body, **kw)


def is_error(r, status, name):
    eq(r.status, status, f"status of {r!r}")
    ok((r.h("content-type") or "").startswith("application/json"), f"error is JSON: {r!r}")
    j = r.json()
    eq(j.get("error"), name, "error name")
    ok(isinstance(j.get("message"), str) and j["message"], "error has a message")
    eq(r.h("cache-control"), "no-store", "no-store on errors")
    eq(r.h("x-content-type-options"), "nosniff", "nosniff")
    ok("access-control-allow-origin" not in r.headers, "no CORS")
    return j


def is_json(r, status=200):
    eq(r.status, status, f"status of {r!r}")
    eq(r.h("content-type"), "application/json; charset=utf-8", "JSON type")
    eq(r.h("cache-control"), "no-store", "JSON is no-store")
    eq(r.h("x-content-type-options"), "nosniff", "nosniff")
    ok("access-control-allow-origin" not in r.headers, "no CORS")
    return r.json()


class Raw:
    """a raw socket for keep-alive, pipelining and malformed requests"""

    def __init__(self, timeout=10):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
        self.buf = b""

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("latin-1")
        self.s.sendall(data)

    def _fill(self):
        d = self.s.recv(65536)
        if not d:
            raise EOFError("closed")
        self.buf += d

    def head(self):
        while b"\r\n\r\n" not in self.buf:
            self._fill()
        i = self.buf.index(b"\r\n\r\n")
        lines = self.buf[:i].decode("latin-1").split("\r\n")
        self.buf = self.buf[i + 4:]
        parts = lines[0].split(" ", 2)
        ok(parts[0] == "HTTP/1.1", f"status line {lines[0]!r}")
        hd = {}
        for line in lines[1:]:
            k, v = line.split(":", 1)
            hd[k.strip().lower()] = v.strip()
        return int(parts[1]), hd

    def response(self, head_only=False):
        st, hd = self.head()
        if head_only or st in (204, 304) or 100 <= st < 200:
            n = 0
        elif "content-length" in hd:
            n = int(hd["content-length"])
        else:
            try:
                while True:
                    self._fill()
            except (EOFError, ConnectionError):
                pass
            n = len(self.buf)
        while len(self.buf) < n:
            self._fill()
        body, self.buf = self.buf[:n], self.buf[n:]
        return R(st, hd, body)

    def closed_within(self, seconds):
        """True when the server closes (EOF or reset) within seconds, with no more data"""
        self.s.settimeout(seconds)
        try:
            d = self.s.recv(65536)
            if d:
                self.buf += d
                return False
            return True
        except socket.timeout:
            return False
        except ConnectionError:
            return True

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def rq(method, path, extra="", body=b"", host=None, version="HTTP/1.1"):
    """the bytes of a request; extra = more header lines ending in CRLF"""
    h = f"{method} {path} {version}\r\nHost: {host or hostport()}\r\n{extra}"
    if isinstance(body, str):
        body = body.encode()
    if body or method in ("POST", "PUT"):
        h += f"Content-Length: {len(body)}\r\n"
    return h.encode("latin-1") + b"\r\n" + body


def raw_once(data, timeout=10):
    c = Raw(timeout)
    try:
        c.send(data)
        return c.response()
    finally:
        c.close()


# ---------------------------------------------------------------- the server's side files
def path(*parts):
    return os.path.join(BASE, *parts)


def sdp(rel):
    return path("sd", *rel.split("/"))


def write(p, data):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data.encode("utf-8") if isinstance(data, str) else data)


def read(p):
    with open(p, "rb") as f:
        return f.read()


def settings_text():
    return read(path("sticktime.txt")).decode("utf-8")


def events():
    try:
        return read(path("state", "events.log")).decode().splitlines()
    except FileNotFoundError:
        return []


def wait_for(pred, timeout=5.0, step=0.05):
    end = time.time() + timeout
    while True:
        v = pred()
        if v or time.time() > end:
            return v
        time.sleep(step)


def wait_event(mark, pred, timeout=3.0):
    """an events.log line after line index mark that pred accepts"""
    return wait_for(lambda: next((e for e in events()[mark:] if pred(e)), None), timeout)


def cmd_bits(mark):
    bits = 0
    for e in events()[mark:]:
        if e.startswith("cmd "):
            bits |= int(e.split()[1])
    return bits


def helper_log():
    """the helper calls in order: one file each (appends to one file race between processes on Windows)"""
    return [read(p).decode().strip() for p in sorted(glob.glob(path("state", "calls", "*.log")))]


def no_temp_files(rel_dir):
    names = os.listdir(sdp(rel_dir)) if rel_dir else os.listdir(path("sd"))
    ok(not [n for n in names if n.startswith(".st-tmp-")], f"temp files left in {rel_dir}: {names}")


def set_policy(p):
    r = post("/api/settings", f"sd_write = {p}\n")
    eq(r.status, 200, f"sd_write = {p}: {r!r}")


def sd(method, rel, body=None, query="", **kw):
    return request(method, "/api/sd/" + rel + ("?" + query if query else ""), body, **kw)


# ---------------------------------------------------------------- setup
HELPER = r'''#!{python}
# fake {name} for web_api_test.py: logs its arguments, answers like the real one
import json, os, sys, time
STATE = {state!r}
NAME = {name!r}
args = sys.argv[1:]
os.makedirs(os.path.join(STATE, "calls"), exist_ok=True)
with open(os.path.join(STATE, "calls", "%020d-%d.log" % (time.time_ns(), os.getpid())), "w") as f:
    f.write(NAME + " " + " ".join(args) + "\n")
def mode(default="ok"):
    try:
        with open(os.path.join(STATE, NAME + ".mode")) as f:
            return f.read().strip()
    except OSError:
        return default
if NAME == "sticktime-wifi" and args[:1] == ["scan"]:
    m = mode()
    now = int(time.time() * 1000)
    nets = [{{"ssid": "Home", "signal": 72, "security": "WPA2", "band": "2.4", "chan": 6, "known": True}},
            {{"ssid": "Caf\u00e9 \"<b>\"", "signal": 40, "security": "open", "band": "5", "chan": 36, "known": False}}]
    if m == "slow":
        time.sleep(6)
    if m == "nodev":
        print(json.dumps({{"networks": [], "at": now, "error": "no WiFi device"}}))
        sys.exit(2)
    if m == "garbage":
        print("this is not JSON")
        sys.exit(0)
    if m == "crash":
        sys.exit(1)
    res = {{"networks": nets, "at": now, "device": "wlan0"}}
    if m == "note":
        res["note"] = "the hotspot is on: the list may be old"
    print(json.dumps(res))
    sys.exit(0)
if NAME == "power":
    with open(os.path.join(STATE, "power.log"), "a") as f:
        f.write(" ".join(args) + "\n")
print(json.dumps({{"ok": True}}))
'''


def make_helper(name, libexec, state):
    text = HELPER.format(python=sys.executable, name=name, state=state)
    p = os.path.join(libexec, name + (".py" if WIN or name == "power" else ""))
    write(p, text)
    if not WIN:
        try:
            os.chmod(p, 0o755)
        except OSError:
            pass                       # vfat: the mount's fmask decides (0755 by default)
    return p


def make_base(base):
    write(os.path.join(base, "sticktime.txt"), SETTINGS)
    # the game's SD card
    s = lambda rel: os.path.join(base, "sd", *rel.split("/"))  # noqa: E731
    write(s("SCRIPTS/TOOLS/StickTime.lua"), "-- TNS|StickTime|TNE\nreturn { run = function() return 0 end }\n")
    write(s("SCRIPTS/TOOLS/Other.lua"), "return {}\n")
    write(s("SCRIPTS/TOOLS/readme.txt"), "not a tool\n")
    os.makedirs(s("SCRIPTS/TOOLS/Folder.lua"))
    write(s(SLOT + "/c1.txt"), "Track One\n1 0\n")
    write(s(SLOT + "/C5.TXT"), "Track Five\n1 0\n")
    os.makedirs(s("SCRIPTS/TOOLS/StickTimeBW"))
    write(s("MODELS/model1.yml"), "name: x\n")
    write(s(".st-tmp-1-1"), "stale temp at the root")
    write(s("SCRIPTS/.st-tmp-2-2"), "stale temp in a folder")
    write(s("SCRIPTS/x.tmp~"), "plat's temp: kept, hidden")
    # the web UI
    w = lambda rel: os.path.join(base, "web", *rel.split("/"))  # noqa: E731
    write(w("index.html"), "<!doctype html><title>StickTime Pi</title>settings app\n")
    app_js = "// app.js\n" + "console.log('x');\n" * 200
    write(w("app.js"), app_js)
    write(w("app.js.gz"), gzip.compress(app_js.encode()))
    write(w("style.css"), "body{color:red}\n")
    write(w("pi-sd.js"), "// pi-sd.js shim\n")
    write(w(".hidden"), "secret dot file\n")
    os.makedirs(w("sub"))
    write(w("sub2/index.html"), "<p>sub2</p>\n")
    write(w("img.png"), b"\x89PNG\r\n\x1a\n")
    # the emulator page and its files
    e = lambda rel: os.path.join(base, "editor", *rel.split("/"))  # noqa: E731
    write(e("index.html"), "<!doctype html><head><script src=\"pi-sd.js\"></script></head><body>emulator</body>\n")
    write(e("tracks/index.json"), '{"tracks":[]}\n')
    write(e("sdcard/manifest.json"), '{"version":"1.7.3","files":[]}\n')
    write(e("sdcard/SCRIPTS/TOOLS/StickTime.lua"), "-- staged release\n")
    # helpers and state
    state = os.path.join(base, "state")
    os.makedirs(state, exist_ok=True)
    libexec = os.path.join(base, "libexec")
    for n in ("sticktime-wifi", "sticktime-update", "power"):
        make_helper(n, libexec, state)


class Server:
    def __init__(self, exe, base, port):
        self.exe, self.base, self.port = exe, base, port
        self.p = None

    def start(self):
        env = dict(os.environ)
        env["STICKTIME_PYTHON"] = sys.executable
        env["STICKTIME_POWER_CMD"] = f'"{sys.executable}" "{os.path.join(self.base, "libexec", "power.py")}"'
        if VTX_LINK:
            env["STP_VTX_PORT"] = f"tcp:127.0.0.1:{VTX_LINK.port}"      # VTX control against the fake VTX
        self.log = open(os.path.join(self.base, "server.log"), "wb")
        self.p = subprocess.Popen([self.exe, self.base, str(self.port), "sticktime", str(MAX_UPLOAD)],
                                  stdout=subprocess.PIPE, stderr=self.log, env=env)
        ready = threading.Event()

        def reader():
            for line in self.p.stdout:
                if line.strip() == b"READY":
                    ready.set()
        threading.Thread(target=reader, daemon=True).start()
        if not ready.wait(15):
            self.kill()
            raise SystemExit(f"the server didn't start: see {os.path.join(self.base, 'server.log')}")

    def stop(self):
        """the graceful way (web_stop): the fake loop sees <state>/stop. Returns the exit code."""
        write(os.path.join(self.base, "state", "stop"), "")
        try:
            return self.p.wait(15)
        except subprocess.TimeoutExpired:
            self.kill()
            return None

    def kill(self):
        if self.p and self.p.poll() is None:
            self.p.kill()
            self.p.wait()
        if getattr(self, "log", None):
            self.log.close()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def find_exe():
    cands = glob.glob(os.path.join(ROOT, "build", "*", "web_test_main" + EXE))
    cands += glob.glob(os.path.join(ROOT, "build", "agent-web", "obj*", "web_test_main" + EXE))
    cands = [c for c in cands if os.path.isfile(c)]
    if not cands:
        raise SystemExit("no web_test_main found: build it (host/build.sh) or pass --exe")
    return max(cands, key=os.path.getmtime)


TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


# ================================================================ pages
@test
def static_files():
    r = get("/")
    eq(r.status, 200)
    eq(r.h("content-type"), "text/html; charset=utf-8")
    eq(r.body, read(path("web", "index.html")))
    eq(r.h("cache-control"), "no-cache")
    eq(r.h("x-content-type-options"), "nosniff")
    etag = r.h("etag")
    ok(etag and etag.startswith('"') and etag.endswith('"'), f"ETag {etag!r}")
    eq(get("/index.html").h("etag"), etag, "same file, same tag")
    for p, t in (("/style.css", "text/css; charset=utf-8"), ("/pi-sd.js", "text/javascript; charset=utf-8"),
                 ("/img.png", "image/png"), ("/sub2/", "text/html; charset=utf-8"),
                 ("/sub2/index.html", "text/html; charset=utf-8")):
        r = get(p)
        eq(r.status, 200, p)
        eq(r.h("content-type"), t, p)
    # 304 for If-None-Match: the tag, a weak one, a list, "*"
    for inm in (etag, "W/" + etag, f'"nope", {etag}', "*"):
        r = get("/", headers={"If-None-Match": inm})
        eq(r.status, 304, f"If-None-Match {inm}")
        eq(r.body, b"")
        eq(r.h("etag"), etag)
    eq(get("/", headers={"If-None-Match": '"other"'}).status, 200)
    # a changed file gets a new tag
    old = get("/style.css").h("etag")
    write(path("web", "style.css"), "body{color:blue;margin:0}\n")
    r = get("/style.css", headers={"If-None-Match": old})
    eq(r.status, 200)
    ok(r.h("etag") != old, "a new tag")
    eq(r.body, b"body{color:blue;margin:0}\n")
    # HEAD: the headers only
    r = request("HEAD", "/")
    eq(r.status, 200)
    eq(r.body, b"")
    eq(int(r.h("content-length")), len(read(path("web", "index.html"))))


@test
def static_gzip_sibling():
    r = get("/app.js", headers={"Accept-Encoding": "gzip, deflate"})
    eq(r.status, 200)
    eq(r.h("content-encoding"), "gzip")
    eq(r.h("vary"), "Accept-Encoding")
    eq(gzip.decompress(r.body), read(path("web", "app.js")))
    plain = get("/app.js", headers={"Accept-Encoding": "identity"})
    eq(plain.h("content-encoding"), None)
    eq(plain.body, read(path("web", "app.js")))
    ok(plain.h("etag") != r.h("etag"), "gzip and plain have different tags")
    eq(get("/app.js", headers={"Accept-Encoding": "gzip;q=0"}).h("content-encoding"), None, "q=0 refuses gzip")


@test
def static_not_found_and_safety():
    for p in ("/nope.html", "/.hidden", "/sub/", "/sub", "/sub2", "/web/index.html", "/sub2/.x"):
        is_error(get(p), 404, "NotFound")
    for p in ("/../sticktime.txt", "/%2e%2e/sticktime.txt", "/..%2fsticktime.txt", "/sub2/..%5c..%5csticktime.txt",
              "/a%00b", "/C:/x", "//etc/passwd", "/%2fetc%2fpasswd", "/x%"):
        r = get(p)
        ok(r.status in (400, 404), f"{p}: {r!r}")
        ok(b"wifi_password" not in r.body, f"{p} leaked the settings")
    is_error(post("/"), 405, "MethodNotAllowed")
    eq(post("/").h("allow"), "GET, HEAD")
    # favicon: 204 without one, the file once there is one
    r = get("/favicon.ico")
    eq(r.status, 204)
    write(path("web", "favicon.ico"), b"\x00\x00\x01\x00")
    r = get("/favicon.ico")
    eq(r.status, 200)
    eq(r.h("content-type"), "image/x-icon")
    os.remove(path("web", "favicon.ico"))


@test
def editor_routes():
    r = get("/editor")
    eq(r.status, 301)
    eq(r.h("location"), "/editor/")
    eq(get("/editor?x=1&y=2").h("location"), "/editor/?x=1&y=2")
    eq(get("/editor?" + "q" * 3000).h("location"), "/editor/", "a huge query is dropped")
    for p in ("/editor/", "/editor/index.html"):
        r = get(p)
        eq(r.status, 200, p)
        eq(r.h("content-type"), "text/html; charset=utf-8")
        eq(r.body, read(path("editor", "index.html")))
        ok(r.h("etag"), "ETag")
    r = get("/editor/pi-sd.js")
    eq(r.status, 200)
    eq(r.h("content-type"), "text/javascript; charset=utf-8")
    eq(r.body, read(path("web", "pi-sd.js")))
    r = get("/editor/tracks/index.json")
    eq(r.status, 200)
    eq(r.h("content-type"), "application/json; charset=utf-8")
    eq(get("/editor/sdcard/manifest.json").status, 200)
    r = get("/editor/sdcard/SCRIPTS/TOOLS/StickTime.lua")
    eq(r.body, b"-- staged release\n")
    is_error(get("/editor/nope.js"), 404, "NotFound")
    is_error(get("/editor/../sticktime.txt"), 400, "BadRequest")
    is_error(post("/editor/"), 405, "MethodNotAllowed")
    # nothing staged / no gallery: 404 (the page falls back to its own)
    shutil.move(path("editor", "sdcard"), path("sdcard.off"))
    shutil.move(path("editor", "tracks"), path("tracks.off"))
    try:
        is_error(get("/editor/sdcard/manifest.json"), 404, "NotFound")
        is_error(get("/editor/tracks/index.json"), 404, "NotFound")
    finally:
        shutil.move(path("sdcard.off"), path("editor", "sdcard"))
        shutil.move(path("tracks.off"), path("editor", "tracks"))


# ================================================================ Host / CSRF
@test
def host_check():
    good = ["sticktime.local", "STICKTIME.LOCAL:8080", "sticktime", "sticktime.local.", "localhost", "localhost:1",
            "10.0.0.5", "127.0.0.1:80", "[::1]", "[fe80::1]:80"]
    for h in good:
        eq(get("/api/status", host=h).status, 200, f"Host {h}")
    bad = ["evil.com", "sticktime.local.evil.com", "evil.sticktime.local", "sticktime.locals", "1.2.3.4.5", "256.1.1.1",
           "localhost.evil.com", "[::1", "sticktime.local:99999", "a@sticktime.local", "", "sticktime.local:"]
    for h in bad:
        r = get("/api/status", host=h)
        is_error(r, 421, "Forbidden")
    is_error(get("/", host="evil.com"), 421, "Forbidden")
    # HTTP/1.1 needs a Host; HTTP/1.0 may leave it out; two are refused
    is_error(raw_once(b"GET /api/status HTTP/1.1\r\n\r\n"), 400, "BadRequest")
    eq(raw_once(b"GET /api/status HTTP/1.0\r\n\r\n").status, 200)
    is_error(raw_once(rq("GET", "/api/status", f"Host: {hostport()}\r\n")), 400, "BadRequest")


@test
def csrf_check():
    for m, p in (("POST", "/api/game/reload"), ("PUT", "/api/sd/" + SLOT + "/c2.txt"), ("DELETE", "/api/sd/" + SLOT + "/c1.txt"),
                 ("POST", "/api/settings")):
        is_error(request(m, p, b"x" if m == "PUT" else b"", xst=False), 403, "Forbidden")
        is_error(request(m, p, b"", xst=False, headers={"X-StickTime": "0"}), 403, "Forbidden")
    ok(os.path.exists(sdp(SLOT + "/c1.txt")), "a refused DELETE deleted")
    mark = len(events())
    eq(post("/api/game/reload").status, 200)
    eq(post("/api/game/reload", xst=False, headers={"X-StickTime-SD": "1"}).status, 200, "X-StickTime-SD works too")
    # Origin: when present it must match Host
    is_error(post("/api/game/reload", headers={"Origin": "http://evil.com"}), 403, "Forbidden")
    is_error(post("/api/game/reload", headers={"Origin": "null"}), 403, "Forbidden")
    is_error(post("/api/game/reload", headers={"Origin": f"http://127.0.0.1:{PORT + 1}"}), 403, "Forbidden")
    eq(post("/api/game/reload", headers={"Origin": f"http://{hostport()}"}).status, 200)
    eq(post("/api/game/reload", host="sticktime.local", headers={"Origin": "http://sticktime.local"}).status, 200)
    eq(post("/api/game/reload", host="sticktime.local:80", headers={"Origin": "http://STICKTIME.local"}).status, 200)
    eq(get("/api/status", headers={"Origin": "http://evil.com"}).status, 200, "reads don't care")
    ok(wait_for(lambda: cmd_bits(mark) & CMD_RELOAD_GAME), "reload reached the frame loop")
    # never any CORS: a preflight gets a plain 405
    r = request("OPTIONS", "/api/sd/x", headers={"Origin": "http://evil.com", "Access-Control-Request-Method": "PUT"})
    is_error(r, 405, "MethodNotAllowed")


# ================================================================ status, sticks, log, screenshot
@test
def status_route():
    r = get("/api/status")
    j = is_json(r)
    ok(isinstance(j["version"], str) and j["version"], "version")     # STP_VERSION of the build
    eq(j["game_version"], "1.7.2")
    ok(isinstance(j["uptime_ms"], int) and j["uptime_ms"] >= 0)
    for k in ("fps", "latency_ms", "present_ms"):
        ok(isinstance(j[k], (int, float)), k)
    eq(j["frame_ms"]["avg"], 16.7)
    eq(j["run_ms"]["p95"], 4.0)
    eq(j["missed"], 3)
    g = j["game"]
    eq(g["state"], 1)
    eq(g["state_name"], "menu")
    eq(g["loaded"], True)
    eq(g["error"], 'boom: "x" <b>\n\tline 2 \u00e9', "the error string survives escaping")
    eq(g["lua_kb"], 812)
    ok(b"<b>" not in r.body, "< and > are escaped in JSON")
    eq(j["video"]["render_w"], 640)
    ok(j["video"]["desc"].startswith("Composite-1"))
    inp = j["input"]
    eq(inp["active"], 0)
    eq(inp["kind"], "wifi")
    eq(inp["live"], True)
    eq(inp["sticks"]["thr"], -1024)
    eq(inp["sticks"]["ail"], 100)
    ok(inp["sticks"]["pause"] in (None, 1024), "pause: null when unknown")
    eq(len(inp["sources"]), 2)
    eq(inp["sources"][0]["name"], "ELRS 10.0.0.1")
    eq(inp["sources"][1]["kind"], "keyboard")
    eq(j["wifi_joystick"]["state"], "streaming")
    eq(j["audio"]["device"], "jack")
    eq(j["audio"]["music"], "Song 3 (generated)")
    for k in ("ip", "iface", "ssid", "mode", "hostname"):
        ok(isinstance(j["network"][k], str), k)
    eq(j["network"]["hostname"], "sticktime")
    ok(j["system"]["cpu_temp"] is None or isinstance(j["system"]["cpu_temp"], (int, float)))
    for s in SECRETS:
        ok(s.encode() not in r.body, "no secrets")
    eq(request("HEAD", "/api/status").body, b"")
    is_error(post("/api/status"), 405, "MethodNotAllowed")
    eq(post("/api/status").h("allow"), "GET, HEAD")
    # network mode: from the hotspot daemon's state, else network.json
    write(path("state", "hotspot.json"), '{"mode":"auto","phase":"ap","connection":"sticktime-hotspot","at":1}')
    n = get("/api/status").json()["network"]
    eq((n["mode"], n["ssid"]), ("hotspot", "StickTime"))
    write(path("state", "hotspot.json"), '{"mode":"auto","phase":"client","connection":"sticktime-wifi","at":1}')
    n = get("/api/status").json()["network"]
    eq((n["mode"], n["ssid"]), ("client", "Home"))
    write(path("state", "network.json"), '{"mode":"client","ssid":"Caf\\u00e9"}')
    n = get("/api/status").json()["network"]
    eq((n["mode"], n["ssid"]), ("client", "Caf\u00e9"))
    write(path("state", "network.json"), "{broken")
    eq(get("/api/status").json()["network"]["mode"], "client", "a broken network.json is ignored")
    os.remove(path("state", "network.json"))
    os.remove(path("state", "hotspot.json"))


@test
def sticks_and_log():
    j = is_json(get("/api/sticks"))
    eq(j["active"], 0)
    eq(len(j["sources"]), 2)
    for s in j["sources"]:
        eq(len(s["ch"]), 16)
        ok(all(isinstance(v, int) for v in s["ch"]))
    eq(j["sources"][0]["nch"], 16)
    eq(j["sources"][0]["rate_hz"], 250.0)
    eq(j["sticks"]["ele"], -200)
    r = get("/api/log")
    eq(r.status, 200)
    eq(r.h("content-type"), "text/plain; charset=utf-8")
    eq(r.h("cache-control"), "no-store")
    ok(b"web: listening on port" in r.body, "the log has the server's start")


@test
def sticks_stream():
    c = Raw(5)
    try:
        c.send(rq("GET", "/api/sticks/stream"))
        st, hd = c.head()
        eq(st, 200)
        eq(hd.get("content-type"), "text/event-stream; charset=utf-8")
        eq(hd.get("cache-control"), "no-store")
        ok("content-length" not in hd, "a stream has no length")
        t0 = time.time()
        evs = []
        while len(evs) < 6:
            while b"\n\n" not in c.buf:
                c._fill()
            i = c.buf.index(b"\n\n")
            block, c.buf = c.buf[:i].decode(), c.buf[i + 2:]
            if block.startswith("data: "):
                evs.append(json.loads(block[6:]))
            else:
                ok(block.startswith("retry:") or block.startswith(":"), f"event block {block!r}")
        dt = time.time() - t0
        ok(dt < 2.0, f"6 events took {dt:.2f} s (about 33 ms each)")
        for e in evs:
            eq(len(e["sources"]), 2)
            eq(e["sticks"]["thr"], -1024)
        frames = {tuple(e["sources"][0]["ch"]) for e in evs}
        ok(len(frames) > 1, "the stream follows the published status")
    finally:
        c.close()
    r = request("HEAD", "/api/sticks/stream")
    eq((r.status, r.h("allow"), r.body), (405, "GET", b""), "HEAD: the headers of a 405, no body")


def png_info(b):
    ok(b[:8] == b"\x89PNG\r\n\x1a\n", "PNG signature")
    pos, idat, w, h, ct = 8, b"", 0, 0, -1
    while pos < len(b):
        n = struct.unpack(">I", b[pos:pos + 4])[0]
        typ, data = b[pos + 4:pos + 8], b[pos + 8:pos + 8 + n]
        eq(zlib.crc32(typ + data) & 0xFFFFFFFF, struct.unpack(">I", b[pos + 8 + n:pos + 12 + n])[0], "PNG CRC")
        if typ == b"IHDR":
            w, h, depth, ct = struct.unpack(">IIBB", data[:10])
            eq(depth, 8, "bit depth")
        elif typ == b"IDAT":
            idat += data
        elif typ == b"IEND":
            break
        pos += 12 + n
    bpp = {2: 3, 6: 4}[ct]
    pix = zlib.decompress(idat)
    eq(len(pix), h * (1 + w * bpp), "PNG data size")
    return w, h, tuple(pix[1:4])          # row 0, pixel 0 is unfiltered under every filter type


@test
def screenshot():
    mark = len(events())
    r = get("/api/screenshot.png")
    eq(r.status, 200, repr(r))
    eq(r.h("content-type"), "image/png")
    eq(r.h("cache-control"), "no-store")
    w, h, rgb = png_info(r.body)
    eq((w, h), (64, 48))
    ok(rgb[0] == 0 and rgb[1] == 0 and rgb[2] >= 248, f"pixel 0,0 is blue: {rgb}")
    ok(cmd_bits(mark) & CMD_SCREENSHOT, "the frame loop was asked")
    # a frame loop that doesn't answer: 503 Busy after about 500 ms
    write(path("state", "no_shot"), "")
    try:
        time.sleep(0.05)
        t0 = time.time()
        r = get("/api/screenshot.png")
        dt = time.time() - t0
        j = is_error(r, 503, "Busy")
        eq(r.h("retry-after"), "1")
        ok(0.4 < dt < 3, f"gave up after {dt:.2f} s")
        ok(j["message"])
    finally:
        os.remove(path("state", "no_shot"))
    # several at once all get a picture
    res = [None] * 4

    def one(i):
        res[i] = get("/api/screenshot.png").status
    ts = [threading.Thread(target=one, args=(i,)) for i in range(4)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    eq(res, [200] * 4)


# ================================================================ settings
@test
def settings_get():
    r = get("/api/settings")
    j = is_json(r)
    vals, keys = j["values"], j["keys"]
    names = [k["key"] for k in keys]
    for k in ("video", "tv", "sd_write", "wifi_password", "hotspot_password", "password", "github_token", "volume"):
        ok(k in names, f"{k} in keys")
    for k in keys:
        ok(k["key"] in vals, f"value of {k['key']}")
        for f in ("kind", "default", "choices", "live", "reboot", "help"):
            ok(f in k, f"{k['key']}.{f}")
        if k["kind"] == "secret":
            eq(vals[k["key"]], "", f"{k['key']} is never sent")
            ok(isinstance(vals[k["key"] + "_set"], bool), f"{k['key']}_set")
            eq(k["default"], "", "no secret defaults either")
    eq(vals["wifi_password_set"], True)
    eq(vals["github_token_set"], True)
    eq(vals["password_set"], False)
    eq(vals["hotspot_password_set"], False)
    for s in SECRETS:
        ok(s.encode() not in r.body, f"secret {s} leaked")
    video = next(k for k in keys if k["key"] == "video")
    eq(video["kind"], "enum")
    eq(video["choices"], ["av", "hdmi"])
    eq(video["reboot"], True)
    eq(video["default"], "av")
    vol = next(k for k in keys if k["key"] == "volume")
    eq((vol["kind"], vol["min"], vol["max"], vol["live"]), ("int", 0, 100, True))
    eq(vals["video"], "av")
    eq(vals["tv"], "ntsc")
    eq(vals["hdmi_mode"], "auto", "defaults for keys the file lacks")
    n = SETTINGS.splitlines().index("this line has no equals sign") + 1
    ok(f"line {n}: no '='" in j["warnings"], f"warnings {j['warnings']!r}")


@test
def settings_post():
    before = settings_text()
    mark = len(events())
    r = post("/api/settings", "tv = pal\n")
    j = is_json(r)
    eq(j, {"ok": True, "saved": ["tv"], "reboot": True, "restart": False})
    t = settings_text()
    ok("tv = pal" in t, t)
    for line in before.splitlines():
        if not line.startswith("tv ="):
            ok(line in t, f"line kept: {line!r}")
    eq(get("/api/settings").json()["values"]["tv"], "pal")
    # an unchanged value saves nothing
    eq(post("/api/settings", "tv = pal\n").json()["saved"], [])
    # nothing is saved when any value is bad, and every bad field is named
    r = post("/api/settings", "volume = 55\ntv = mars\nvideo = vga\n")
    j = is_error(r, 400, "BadRequest")
    eq(sorted(j["fields"]), ["tv", "video"])
    ok("must be one of" in j["fields"]["tv"], j["fields"]["tv"])
    eq(settings_text(), t, "nothing saved")
    # live key: the frame loop is told; CRLF, BOM and comments in the body are fine
    mark = len(events())
    r = post("/api/settings", "\ufeff# a comment\r\nvolume = 55\r\n\r\n")
    eq(is_json(r)["saved"], ["volume"])
    ok(wait_for(lambda: cmd_bits(mark) & CMD_RELOAD_SETTINGS), "RELOAD_SETTINGS")
    ok("# This comment must survive every save." in settings_text())
    ok("video = av          # composite to the VTX" in settings_text(), "inline comment kept")
    # form encoding
    r = post("/api/settings", "volume=60&tv=ntsc&music_source=songs", headers={"Content-Type": "application/x-www-form-urlencoded"})
    eq(sorted(is_json(r)["saved"]), ["music_source", "tv", "volume"])
    v = get("/api/settings").json()["values"]
    eq((v["volume"], v["tv"], v["music_source"]), ("60", "ntsc", "songs"))
    is_error(post("/api/settings", "a=%zz", headers={"Content-Type": "application/x-www-form-urlencoded"}), 400, "BadRequest")
    j = is_error(post("/api/settings", "tv=pal%0Ax", headers={"Content-Type": "application/x-www-form-urlencoded"}), 400, "BadRequest")
    ok("tv" in j["fields"])
    j = is_error(post("/api/settings", "Bad%20Key=1", headers={"Content-Type": "application/x-www-form-urlencoded"}), 400, "BadRequest")
    ok("bad key" in j["fields"], j)
    # unknown keys are saved and kept; an empty value clears to the default
    eq(is_json(post("/api/settings", "new_thing = 42\n"))["saved"], ["new_thing"])
    ok("new_thing = 42" in settings_text() and "my_custom_key = keep me" in settings_text())
    eq(is_json(post("/api/settings", "volume =\n"))["saved"], ["volume"])
    eq(get("/api/settings").json()["values"]["volume"], "70", "cleared to the default")
    # quoting: a value with " #" in it survives
    post("/api/settings", 'hotspot_ssid = "Pi # one"\n')
    eq(get("/api/settings").json()["values"]["hotspot_ssid"], "Pi # one")
    # an empty body changes nothing; a huge one is refused
    eq(is_json(post("/api/settings", ""))["saved"], [])
    is_error(post("/api/settings", "x = " + "y" * 70000), 413, "PayloadTooLarge")
    j = is_error(post("/api/settings", "volume = " + "9" * 1100 + "\n"), 400, "BadRequest")
    ok("volume" in j["fields"])


@test
def settings_secrets():
    # an empty secret means "unchanged"; <key>_set (echoed from GET) is ignored
    r = post("/api/settings", "wifi_password =\nwifi_password_set = false\ngithub_token =\n")
    eq(is_json(r)["saved"], [])
    ok('wifi_password = "secret-pass-123"' in settings_text() or "wifi_password = secret-pass-123" in settings_text())
    # a new value: saved, never echoed
    r = post("/api/settings", "wifi_password = new-secret-456\n")
    eq(is_json(r)["saved"], ["wifi_password"])
    ok(b"new-secret-456" not in r.body)
    g = get("/api/settings")
    ok(b"new-secret-456" not in g.body)
    eq(g.json()["values"]["wifi_password_set"], True)
    ok("new-secret-456" in settings_text())
    # validated like the rest
    j = is_error(post("/api/settings", "wifi_password = short\n"), 400, "BadRequest")
    ok("wifi_password" in j["fields"])
    # _clear empties it, also next to an empty line for the same key (what the app sends)
    r = post("/api/settings", "wifi_password =\nwifi_password_clear = 1\n")
    eq(is_json(r)["saved"], ["wifi_password"])
    eq(get("/api/settings").json()["values"]["wifi_password_set"], False)
    ok("new-secret-456" not in settings_text())
    # _clear = 0 does nothing; _clear wins over a value in the same body
    eq(is_json(post("/api/settings", "github_token_clear = 0\n"))["saved"], [])
    r = post("/api/settings", "github_token_clear = 1\ngithub_token = ghp_other_value\n")
    eq(is_json(r)["saved"], ["github_token"])
    eq(get("/api/settings").json()["values"]["github_token_set"], False)
    ok("ghp_other_value" not in settings_text())
    # put the originals back for the other tests
    post("/api/settings", "wifi_password = secret-pass-123\ngithub_token = ghp_TOPSECRET42\n")


@test
def settings_restart_and_hostname():
    mark = len(events())
    r = post("/api/settings", "margin = 5\n")
    eq(is_json(r), {"ok": True, "saved": ["margin"], "reboot": False, "restart": True})
    ok(wait_for(lambda: cmd_bits(mark) & CMD_QUIT, 3), "a restart key restarts the service")
    post("/api/settings", "margin = 0\n")
    # a new hostname is accepted at once, and the old one until a reboot
    eq(get("/api/status", host="racer.local").status, 421)
    r = post("/api/settings", "hostname = racer\n")
    eq(is_json(r)["reboot"], True)
    eq(get("/api/status", host="racer.local").status, 200)
    eq(get("/api/status", host="RACER").status, 200)
    eq(get("/api/status", host="sticktime.local").status, 200)
    eq(get("/api/system").json()["hostname"], "racer")
    is_error(post("/api/settings", "hostname = -bad-\n"), 400, "BadRequest")
    post("/api/settings", "hostname = sticktime\n")


# ================================================================ the SD card
@test
def sd_stat_list_read():
    j = is_json(sd("GET", "", query="stat"))
    eq((j["name"], j["kind"]), ("", "directory"))
    eq(is_json(get("/api/sd?stat"))["kind"], "directory")
    j = is_json(sd("GET", "", query="list"))
    names = [e["name"] for e in j["entries"]]
    eq(names, sorted(names, key=lambda n: (n.lower(), n)), "sorted by name")
    ok("SCRIPTS" in names and "MODELS" in names, names)
    ok(not [n for n in names if n.startswith(".st-tmp-")], "no temp files")
    ok(not os.path.exists(sdp(".st-tmp-1-1")) and not os.path.exists(sdp("SCRIPTS/.st-tmp-2-2")),
       "stale temp files are removed at start")
    ok(os.path.exists(sdp("SCRIPTS/x.tmp~")), "plat's temp files are not ours to remove")
    sub = [e["name"] for e in is_json(sd("GET", "SCRIPTS", query="list"))["entries"]]
    eq(sub, ["TOOLS"], "temp files hidden")
    tracks = is_json(sd("GET", SLOT, query="list"))["entries"]
    e = next(x for x in tracks if x["name"] == "c1.txt")
    eq((e["kind"], e["size"]), ("file", 14))
    ok(abs(e["mtime"] - os.path.getmtime(sdp(SLOT + "/c1.txt")) * 1000) < 2500, "mtime in ms")
    tools = is_json(sd("GET", "SCRIPTS/TOOLS/", query="list"))["entries"]
    eq(next(x for x in tools if x["name"] == "Folder.lua")["kind"], "directory")
    is_error(sd("GET", SLOT + "/c1.txt", query="list"), 409, "TypeMismatchError")
    # read
    r = sd("GET", SLOT + "/c1.txt")
    eq(r.status, 200)
    eq(r.body, b"Track One\n1 0\n")
    eq(r.h("content-type"), "application/octet-stream")
    eq(r.h("cache-control"), "no-store")
    ok(r.h("x-mtime-ms").isdigit(), "X-Mtime-Ms")
    ok(r.h("last-modified").endswith(" GMT"), "Last-Modified")
    is_error(sd("GET", SLOT), 409, "TypeMismatchError")
    is_error(sd("GET", "SCRIPTS/nope.txt"), 404, "NotFoundError")
    is_error(sd("GET", "nope/c1.txt"), 404, "NotFoundError")
    is_error(sd("GET", SLOT + "/c1.txt/x"), 409, "TypeMismatchError")
    is_error(sd("GET", "nope", query="stat"), 404, "NotFoundError")
    r = request("HEAD", "/api/sd/" + SLOT + "/c1.txt")
    eq((r.status, r.body, r.h("content-length")), (200, b"", "14"))
    is_error(request("PATCH", "/api/sd/x"), 405, "MethodNotAllowed")


@test
def sd_case_insensitive():
    r = sd("GET", "scripts/tools/sticktimetracks/C1.TXT")
    eq(r.body, b"Track One\n1 0\n")
    j = is_json(sd("GET", "Scripts/Tools/StickTimeTracks/C1.Txt", query="stat"))
    eq(j["name"], "c1.txt", "stat says the name on disk")
    eq(is_json(sd("GET", "scripts/TOOLS/sticktimetracks/c5.txt", query="stat"))["name"], "C5.TXT")
    eq(sd("GET", SLOT.lower(), query="list").status, 200)
    set_policy("game")
    # a write keeps the spelling on disk and never makes a second file
    eq(sd("PUT", SLOT.upper() + "/c5.txt", "Track Five v2\n1 0\n").status, 200)
    names = os.listdir(sdp(SLOT))
    ok("C5.TXT" in names and "c5.txt" not in names, names)
    eq(read(sdp(SLOT + "/C5.TXT")), b"Track Five v2\n1 0\n")
    no_temp_files(SLOT)


@test
def sd_put_and_policy():
    files = {
        SLOT + "/c1.txt": ("game", "tracks", "all"),
        "scripts/tools/sticktimetracks/C8.TXT": ("game", "tracks", "all"),
        SLOT + "/c9.txt": ("all",),
        SLOT + "/c1.dat": ("all",),
        SLOT + "/c10.txt": ("all",),
        "SCRIPTS/TOOLS/StickTime.lua": ("game", "all"),
        "SCRIPTS/TOOLS/StickTimeBW.lua": ("game", "all"),
        "SCRIPTS/TOOLS/stickTimeLite.LUA": ("game", "all"),
        "SCRIPTS/TOOLS/StickTimeBW/main.lua": ("game", "all"),
        "SCRIPTS/TOOLS/StickTimeBW/a-b_C.luac": ("game", "all"),
        "SCRIPTS/TOOLS/StickTimeBW/x.new": ("game", "all"),
        "SCRIPTS/TOOLS/StickTimeBW/notes.txt": ("game", "all"),
        "SCRIPTS/TOOLS/StickTimeBW/data.txt": ("all",),
        "SCRIPTS/TOOLS/StickTimeBW/x.dat": ("all",),
        "SCRIPTS/TOOLS/StickTimeBW/a.b.lua": ("all",),
        "SCRIPTS/TOOLS/StickTimeBW/a%20b.lua": ("all",),
        SLOT + "/README.txt": ("game", "all"),
        "SCRIPTS/TOOLS/Other.lua": ("all",),
        "SCRIPTS/TOOLS/StickTime.dat": ("all",),
        "MODELS/model1.yml": ("all",),
        "new.txt": ("all",),
    }
    for pol in ("game", "tracks", "all", "off"):
        set_policy(pol)
        for rel, allowed in files.items():
            mark = len(events())
            body = f"{pol} {rel}\n".encode()
            r = sd("PUT", rel, body)
            if pol in allowed:
                j = is_json(r)
                eq(j["size"], len(body), rel)
                ok(isinstance(j["mtime"], int) and j["mtime"] > 1e12, "mtime in ms")
                got = sd("GET", rel)
                eq(got.body, body, f"{pol} {rel} read back")
                game = pol != "all" or allowed != ("all",)
                if game:
                    ok(wait_event(mark, lambda e: e == "sd_dirty"), f"sd_dirty after {rel}")
            else:
                j = is_error(r, 403, "NoModificationAllowedError")
                ok(len(j["message"]) > 20, "says what is allowed")
                if pol == "off":
                    ok("off" in j["message"], j["message"])
    set_policy("game")
    # a non-game write leaves the flag alone
    set_policy("all")
    time.sleep(0.1)
    mark = len(events())
    eq(sd("PUT", "MODELS/model2.yml", "x").status, 200)
    time.sleep(0.3)
    ok("sd_dirty" not in events()[mark:], "no reload for other files")
    set_policy("game")
    # the policy is checked before the folder: a game path whose folder is missing is 404
    is_error(sd("PUT", "SCRIPTS/TOOLS/StickTimeLite/main.lua", "x"), 404, "NotFoundError")
    for d in (SLOT, "SCRIPTS/TOOLS/StickTimeBW", "SCRIPTS/TOOLS", "SCRIPTS", ""):
        no_temp_files(d)


@test
def sd_put_limits_and_errors():
    set_policy("game")
    slot = SLOT + "/c2.txt"
    eq(sd("PUT", slot, b"t" * 65536).status, 200, "64 KB is fine for a slot")
    j = is_error(sd("PUT", slot, b"t" * 65537), 413, "QuotaExceededError")
    ok("64" in j["message"])
    eq(len(read(sdp(slot))), 65536, "the refused body changed nothing")
    eq(sd("PUT", "SCRIPTS/TOOLS/StickTime.lua", b"l" * (2 << 20)).status, 200, "2 MiB for other files")
    is_error(sd("PUT", "SCRIPTS/TOOLS/StickTime.lua", b"l" * ((2 << 20) + 1)), 413, "QuotaExceededError")
    eq(sd("PUT", slot, b"").status, 200, "an empty file")
    eq(read(sdp(slot)), b"")
    set_policy("all")
    is_error(sd("PUT", "nope/x.txt", "x"), 404, "NotFoundError")
    is_error(sd("PUT", "nope/deeper/x.txt", "x"), 404, "NotFoundError")
    is_error(sd("PUT", SLOT + "/c1.txt/x.txt", "x"), 409, "TypeMismatchError")
    is_error(sd("PUT", "SCRIPTS/TOOLS", "x"), 409, "TypeMismatchError")
    is_error(sd("PUT", "", "x"), 409, "TypeMismatchError")
    # unicode names, listed back as UTF-8
    eq(sd("PUT", "MODELS/caf%C3%A9%20%E2%82%AC.txt", "u").status, 200)
    names = [e["name"] for e in sd("GET", "MODELS", query="list").json()["entries"]]
    ok("caf\u00e9 \u20ac.txt" in names, names)
    # no Content-Length, chunked
    r = raw_once(f"PUT /api/sd/MODELS/z.txt HTTP/1.1\r\nHost: {hostport()}\r\nX-StickTime: 1\r\n\r\n".encode())
    is_error(r, 411, "BadRequest")
    r = raw_once(f"PUT /api/sd/MODELS/z.txt HTTP/1.1\r\nHost: {hostport()}\r\nX-StickTime: 1\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n".encode())
    is_error(r, 411, "BadRequest")
    eq(r.h("connection"), "close")
    ok(not os.path.exists(sdp("MODELS/z.txt")))
    set_policy("game")


@test
def sd_put_is_atomic():
    set_policy("all")
    rel = "MODELS/big.bin"
    old, new = b"a" * 300000, b"b" * 300000
    eq(sd("PUT", rel, old).status, 200)
    # a PUT in progress: readers see the old file whole; then the new one whole
    c = Raw(20)
    try:
        c.send(f"PUT /api/sd/{rel} HTTP/1.1\r\nHost: {hostport()}\r\nX-StickTime: 1\r\nContent-Length: {len(new)}\r\n\r\n".encode())
        c.send(new[:150000])
        time.sleep(0.3)
        eq(sd("GET", rel).body, old, "old file while writing")
        ok([n for n in os.listdir(sdp("MODELS")) if n.startswith(".st-tmp-")], "a temp file while writing")
        eq([e["name"] for e in sd("GET", "MODELS", query="list").json()["entries"] if e["name"].startswith(".st")], [],
           "the temp file is never listed")
        c.send(new[150000:])
        eq(c.response().status, 200)
    finally:
        c.close()
    eq(sd("GET", rel).body, new)
    no_temp_files("MODELS")
    # the client goes away mid-body: the old file stays, the temp file goes
    c = Raw(20)
    c.send(f"PUT /api/sd/{rel} HTTP/1.1\r\nHost: {hostport()}\r\nX-StickTime: 1\r\nContent-Length: {len(old)}\r\n\r\n".encode())
    c.send(old[:100000])
    time.sleep(0.2)
    c.close()
    ok(wait_for(lambda: not [n for n in os.listdir(sdp("MODELS")) if n.startswith(".st-tmp-")]), "temp file removed")
    eq(read(sdp(rel)), new, "an aborted PUT changes nothing")
    # two writers at once: one of the two whole files wins
    bodies = [b"1" * 200000, b"2" * 200000]
    res = [0, 0]

    def put(i):
        res[i] = sd("PUT", rel, bodies[i]).status
    ts = [threading.Thread(target=put, args=(i,)) for i in (0, 1)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    eq(res, [200, 200])
    ok(read(sdp(rel)) in bodies, "one whole body")
    # readers while two writers replace the file again and again: every read is one whole version
    versions = bodies + [b"3" * 250000, b"4" * 150000]
    stop, seen, bad, codes = threading.Event(), [], [], []

    def reader():
        while not stop.is_set():
            b = sd("GET", rel).body
            (seen if b in versions else bad).append(len(b))

    def writer(v):
        for _ in range(6):
            codes.append(sd("PUT", rel, v).status)
    rs = [threading.Thread(target=reader) for _ in range(2)]
    ws = [threading.Thread(target=writer, args=(v,)) for v in versions[2:]]
    [t.start() for t in rs + ws]
    [t.join() for t in ws]
    stop.set()
    [t.join() for t in rs]
    eq(codes, [200] * 12, "every write")
    ok(seen and not bad, f"torn reads: {bad[:5]} ({len(seen)} good)")
    no_temp_files("MODELS")
    set_policy("game")


@test
def sd_create():
    set_policy("game")
    d = "SCRIPTS/TOOLS/StickTimeGhosts"
    j = is_json(sd("POST", d, query="create=directory"), 201)
    eq((j["name"], j["kind"]), ("StickTimeGhosts", "directory"))
    ok(os.path.isdir(sdp(d)))
    eq(is_json(sd("POST", d.lower(), query="create=directory"), 200)["name"], "StickTimeGhosts", "existed: 200, name on disk")
    is_error(sd("POST", d, query="create=file"), 409, "TypeMismatchError")
    # a file: made empty, never truncated when it is there
    slot = SLOT + "/c3.txt"
    if os.path.exists(sdp(slot)):
        os.remove(sdp(slot))
    mark = len(events())
    j = is_json(sd("POST", slot, query="create=file"), 201)
    eq((j["name"], j["kind"], j["size"]), ("c3.txt", "file", 0))
    ok(wait_event(mark, lambda e: e == "sd_dirty"), "sd_dirty after a new slot file")
    sd("PUT", slot, "Track Three\n1 0\n")
    j = is_json(sd("POST", "scripts/tools/sticktimetracks/C3.TXT", query="create=file"), 200)
    eq((j["name"], j["size"]), ("c3.txt", 16))
    eq(read(sdp(slot)), b"Track Three\n1 0\n", "not truncated")
    is_error(sd("POST", slot, query="create=directory"), 409, "TypeMismatchError")
    is_error(sd("POST", slot, query="create=bogus"), 400, "BadRequest")
    is_error(sd("POST", slot), 400, "BadRequest")
    is_error(sd("POST", "SCRIPTS/TOOLS/StickTimeLite/x.lua", query="create=file"), 404, "NotFoundError")
    # the policy, for what would be made (before a missing folder is reported)
    is_error(sd("POST", "Foo", query="create=directory"), 403, "NoModificationAllowedError")
    is_error(sd("POST", "MODELS/x.txt", query="create=file"), 403, "NoModificationAllowedError")
    is_error(sd("POST", "MODELS/x/y.txt", query="create=file"), 403, "NoModificationAllowedError")
    set_policy("tracks")
    is_error(sd("POST", "SCRIPTS/TOOLS/StickTimeLite", query="create=directory"), 403, "NoModificationAllowedError")
    is_error(sd("POST", "SCRIPTS/TOOLS/StickTimeGhosts/README.txt", query="create=file"), 403, "NoModificationAllowedError")
    eq(sd("POST", "SCRIPTS/TOOLS/StickTimeGhosts", query="create=directory").status, 200, "there already: nothing to refuse")
    eq(sd("POST", SLOT, query="create=directory").status, 200)
    set_policy("off")
    is_error(sd("POST", SLOT + "/c4.txt", query="create=file"), 403, "NoModificationAllowedError")
    eq(sd("POST", SLOT + "/c3.txt", query="create=file").status, 200, "an existing file under off")
    is_error(sd("POST", SLOT + "/c3.txt", query="create=directory"), 409, "TypeMismatchError")
    set_policy("game")
    eq(sd("POST", "SCRIPTS/TOOLS/StickTimeLite", query="create=directory").status, 201, "a game folder")
    eq(sd("POST", "SCRIPTS/TOOLS/StickTimeGhosts/README.txt", query="create=file").status, 201)
    set_policy("all")
    eq(sd("POST", "Foo", query="create=directory").status, 201)
    eq(sd("POST", "Foo/bar.txt", query="create=file").status, 201)
    is_error(sd("POST", "Foo/bar.txt/x", query="create=file"), 409, "TypeMismatchError")
    eq(is_json(sd("POST", "", query="create=directory"))["kind"], "directory", "the root is there")
    set_policy("game")


@test
def sd_delete():
    set_policy("game")
    slot = SLOT + "/c4.txt"
    write(sdp(slot), "Track Four\n1 0\n")
    mark = len(events())
    r = sd("DELETE", "scripts/tools/sticktimetracks/C4.TXT")
    eq((r.status, r.body), (204, b""))
    ok(not os.path.exists(sdp(slot)))
    ok(wait_event(mark, lambda e: e == "sd_dirty"), "sd_dirty after a delete")
    is_error(sd("DELETE", slot), 404, "NotFoundError")
    is_error(sd("DELETE", ""), 403, "NoModificationAllowedError")
    is_error(sd("DELETE", "MODELS/model1.yml"), 403, "NoModificationAllowedError")
    is_error(sd("DELETE", "SCRIPTS/TOOLS/Other.lua"), 403, "NoModificationAllowedError")
    # folders: only when empty, unless recursive with sd_write = all
    is_error(sd("DELETE", SLOT), 409, "InvalidModificationError")
    is_error(sd("DELETE", SLOT, query="recursive=1"), 403, "NoModificationAllowedError")
    ok(os.path.exists(sdp(SLOT + "/c1.txt")), "nothing deleted")
    eq(sd("DELETE", "SCRIPTS/TOOLS/StickTimeLite").status, 204, "an empty game folder")
    set_policy("tracks")
    is_error(sd("DELETE", "SCRIPTS/TOOLS/StickTime.lua"), 403, "NoModificationAllowedError")
    set_policy("all")
    os.makedirs(sdp("Tree/a/b"), exist_ok=True)
    write(sdp("Tree/a/b/f.txt"), "x")
    write(sdp("Tree/g.txt"), "y")
    is_error(sd("DELETE", "Tree"), 409, "InvalidModificationError")
    eq(sd("DELETE", "tree", query="recursive=1").status, 204)
    ok(not os.path.exists(sdp("Tree")))
    is_error(sd("DELETE", "Tree"), 404, "NotFoundError")
    set_policy("off")
    is_error(sd("DELETE", SLOT + "/c1.txt"), 403, "NoModificationAllowedError")
    set_policy("game")


@test
def sd_bad_paths():
    set_policy("all")
    bad = ["..", "../sticktime.txt", "%2e%2e", "%2E%2E/sticktime.txt", "SCRIPTS/..", "SCRIPTS/%2e%2e/%2e%2e/sticktime.txt",
           ".", "SCRIPTS/./TOOLS", "%2e", "a%2fb", "..%2fsticktime.txt", "%2f%2e%2e%2fsticktime.txt", "..%5csticktime.txt",
           "%5c", "a\\b", "..\\sticktime.txt", "a%00b", "%00", "C:", "C%3A", "C%3a%5cWindows", "c:/windows", "/etc/passwd",
           "SCRIPTS//TOOLS", "a%01", "tab%09x", "nl%0Ax", "del%7F", "c1.txt.", "c1.txt%20", "...", "x%22y", 'x"y', "x*y",
           "x%2Ay", "x%3Cy", "x<y", "x%3Ey", "x%3Fy", "x%7Cy", "x|y", "x:y", "%FF", "%C0%AF", "%E0%80%AF", "%ED%A0%80",
           "%E2%82", "%80", "a%", "a%zz", "a%2", "a" * 256, "/".join(["b" * 255] * 4) + "/cc", "a/" * 600,
           ".st-tmp-1-1", "SCRIPTS/.st-tmp-5-5", "x.tmp~"]
    for p in bad:
        for m in ("GET", "PUT", "DELETE", "POST"):
            r = sd(m, p, b"x" if m == "PUT" else b"", query="create=file" if m == "POST" else "")
            is_error(r, 400, "TypeError")
    ok(b"wifi_password" in read(path("sticktime.txt")), "the settings are intact")
    # 255 bytes per component and 1024 in all are fine
    eq(sd("PUT", "M" + "x" * 250 + ".txt", "ok").status, 200)
    eq(sd("DELETE", "M" + "x" * 250 + ".txt").status, 204)
    deep = "/".join(["d" * 200] * 4)
    eq(sd("GET", deep, query="stat").status, 404, "a long path that isn't there")
    # Windows device names: never written (the card is read on PCs)
    for p in ("con", "NUL.txt", "MODELS/com1", "aux.lua", "LPT9.x"):
        is_error(sd("PUT", p, "x"), 400, "TypeError")
        r = sd("GET", p)
        if WIN:
            is_error(r, 400, "TypeError")
        else:
            is_error(r, 404, "NotFoundError")
    eq(sd("PUT", "console.txt", "x").status, 200)
    sd("DELETE", "console.txt")
    set_policy("game")


@test
def sd_links_not_followed():
    outside = path("outside")
    os.makedirs(outside, exist_ok=True)
    write(os.path.join(outside, "secret.txt"), "outside the SD card")
    link = sdp("LinkDir")
    try:
        if WIN:
            if subprocess.run(["cmd", "/c", "mklink", "/J", link, outside], capture_output=True).returncode:
                print("    (no junctions here: skipped)")
                return
        else:
            try:
                os.symlink(outside, link)
                os.symlink(os.path.join(outside, "secret.txt"), sdp("link.txt"))
            except OSError:
                print("    (no symlinks on this file system, e.g. vfat: skipped)")
                return
        set_policy("all")
        is_error(sd("GET", "LinkDir/secret.txt"), 404, "NotFoundError")
        is_error(sd("GET", "linkdir/secret.txt"), 404, "NotFoundError")
        is_error(sd("GET", "LinkDir", query="list"), 404, "NotFoundError")
        is_error(sd("PUT", "LinkDir/new.txt", "x"), 404, "NotFoundError")
        ok(not os.path.exists(os.path.join(outside, "new.txt")), "nothing written through the link")
        names = [e["name"] for e in sd("GET", "", query="list").json()["entries"]]
        ok("LinkDir" not in names and "link.txt" not in names, f"links are not listed: {names}")
        if not WIN:
            is_error(sd("GET", "link.txt"), 404, "NotFoundError")
            is_error(sd("PUT", "link.txt", "x"), 404, "NotFoundError")
            eq(read(os.path.join(outside, "secret.txt")), b"outside the SD card")
        is_error(sd("DELETE", "LinkDir", query="recursive=1"), 404, "NotFoundError")
        ok(os.path.exists(os.path.join(outside, "secret.txt")), "nothing deleted through the link")
    finally:
        set_policy("game")
        if WIN:
            if os.path.exists(link):
                os.rmdir(link)
        else:
            for p in (link, sdp("link.txt")):
                if os.path.lexists(p):
                    os.remove(p)


@test
def sd_read_limit():
    big = sdp("MODELS/huge.bin")
    with open(big, "wb") as f:
        f.truncate((16 << 20) + 1)
    try:
        is_error(sd("GET", "MODELS/huge.bin"), 413, "NotReadableError")
        eq(sd("GET", "MODELS/huge.bin", query="stat").json()["size"], (16 << 20) + 1)
        with open(big, "r+b") as f:
            f.truncate(16 << 20)
        r = sd("GET", "MODELS/huge.bin", timeout=60)
        eq((r.status, len(r.body)), (200, 16 << 20))
    finally:
        os.remove(big)


# ================================================================ songs
@test
def songs():
    j = is_json(get("/api/songs"))
    eq(j["songs"], [])
    ok(isinstance(j["free_mb"], int) and j["free_mb"] > 0, j)
    mark = len(events())
    r = request("PUT", "/api/songs/track1.mp3", b"ID3" + b"\x00" * 1000)
    eq(is_json(r), {"size": 1003})
    ok(wait_for(lambda: cmd_bits(mark) & CMD_SONGS_CHANGED), "SONGS_CHANGED")
    eq(read(path("music", "track1.mp3")), b"ID3" + b"\x00" * 1000)
    request("PUT", "/api/songs/B%20side.ogg", b"OggS")
    write(path("music", "notes.txt"), "not a song")
    j = get("/api/songs").json()
    eq([s["name"] for s in j["songs"]], ["B side.ogg", "track1.mp3"])
    eq(j["songs"][1]["size"], 1003)
    ok(j["songs"][1]["mtime"] > 1e12)
    r = get("/api/songs/track1.mp3")
    eq((r.status, r.h("content-type")), (200, "audio/mpeg"))
    # the same name in another case replaces it
    eq(request("PUT", "/api/songs/TRACK1.MP3", b"ID3v2").status, 200)
    ok(sorted(os.listdir(path("music"))) == ["B side.ogg", "notes.txt", "track1.mp3"], os.listdir(path("music")))
    eq(read(path("music", "track1.mp3")), b"ID3v2")
    for bad in ("x.txt", "noext", "a%2fb.mp3", "..%2fx.mp3", "%2e%2e", ".hidden.mp3", "con.mp3", "a" * 117 + ".mp3",
                "x%3A.mp3", "x.mp3.", "sub/x.mp3", ".st-tmp-1-1.mp3"):
        is_error(request("PUT", "/api/songs/" + bad, b"x"), 400, "BadRequest")
    eq(request("PUT", "/api/songs/" + "a" * 116 + ".mp3", b"x").status, 200, "120 bytes is fine")
    request("DELETE", "/api/songs/" + "a" * 116 + ".mp3")
    j = is_error(request("PUT", "/api/songs/big.wav", b"w" * (MAX_UPLOAD + 1)), 413, "PayloadTooLarge")
    ok("MB" in j["message"])
    eq(request("PUT", "/api/songs/big.wav", b"w" * MAX_UPLOAD).status, 200, "max_upload exactly")
    r = raw_once(f"PUT /api/songs/x.mp3 HTTP/1.1\r\nHost: {hostport()}\r\nX-StickTime: 1\r\n\r\n".encode())
    is_error(r, 411, "BadRequest")
    mark = len(events())
    eq(request("DELETE", "/api/songs/Big.WAV").status, 204)
    ok(wait_for(lambda: cmd_bits(mark) & CMD_SONGS_CHANGED))
    is_error(request("DELETE", "/api/songs/big.wav"), 404, "NotFound")
    is_error(request("POST", "/api/songs/x.mp3"), 405, "MethodNotAllowed")
    is_error(post("/api/songs"), 405, "MethodNotAllowed")
    mark = len(events())
    eq(is_json(post("/api/music/next")), {"ok": True})
    ok(wait_for(lambda: cmd_bits(mark) & CMD_MUSIC_NEXT))


# ================================================================ game and remote
@test
def remote_keys():
    mark = len(events())
    for name in KEYS:
        eq(is_json(post("/api/key", name)), {"ok": True}, name)
    want = [f"key {v}" for v in KEYS.values()]
    ok(wait_for(lambda: [e for e in events()[mark:] if e.startswith("key ")] == want), "keys in order")
    mark = len(events())
    eq(post("/api/key", "  ENTER\r\n").status, 200)
    eq(post("/api/key", "key = exit\n").status, 200)
    eq(post("/api/key", "key=up", headers={"Content-Type": "application/x-www-form-urlencoded"}).status, 200)
    ok(wait_for(lambda: [e for e in events()[mark:] if e.startswith("key ")] == ["key 514", "key 513", "key -101"]))
    for bad in ("", "bogus", "enter enter", "ent\x00er", "e" * 40):
        is_error(post("/api/key", bad), 400, "BadRequest")
    is_error(post("/api/key", "e" * 5000), 413, "PayloadTooLarge")
    is_error(get("/api/key"), 405, "MethodNotAllowed")
    mark = len(events())
    eq(is_json(post("/api/game/reload")), {"ok": True})
    ok(wait_for(lambda: cmd_bits(mark) & CMD_RELOAD_GAME))


@test
def tools_list():
    write(sdp("SCRIPTS/TOOLS/Named.lua"), "local x = 1 -- TNS|My Tool|TNE\nreturn {}\n")
    write(sdp("SCRIPTS/TOOLS/Broken.lua"), "-- TNS|half\n|TNE\n")
    write(sdp("SCRIPTS/TOOLS/Big.lua"), "-- TNS|Big One|TNE\n" + "-" * 5000)
    j = is_json(get("/api/tools"))
    eq(j["current"], "/SCRIPTS/TOOLS/StickTime.lua")
    tools = j["tools"]
    paths = [t["path"] for t in tools]
    eq(paths, sorted(paths, key=lambda p: (p.lower(), p)), "sorted")
    by = {t["path"]: t["name"] for t in tools}
    eq(by.get("/SCRIPTS/TOOLS/Other.lua"), "Other", "no TNS: the file name")
    eq(by.get("/SCRIPTS/TOOLS/Named.lua"), "My Tool", "the TNS name")
    eq(by.get("/SCRIPTS/TOOLS/Broken.lua"), "Broken", "a TNS over two lines is not a name")
    eq(by.get("/SCRIPTS/TOOLS/Big.lua"), "Big One", "a big file's first KB")
    ok("/SCRIPTS/TOOLS/StickTime.lua" in by)
    ok(all(p.lower().endswith(".lua") for p in paths), "only .lua files")
    ok("/SCRIPTS/TOOLS/Folder.lua" not in by, "folders are not tools")
    for f in ("Named.lua", "Broken.lua", "Big.lua"):
        os.remove(sdp("SCRIPTS/TOOLS/" + f))
    post("/api/settings", "script = /SCRIPTS/TOOLS/Other.lua\n")
    eq(get("/api/tools").json()["current"], "/SCRIPTS/TOOLS/Other.lua")
    post("/api/settings", "script = /SCRIPTS/TOOLS/StickTime.lua\n")


# ================================================================ WiFi
def clear_scan(mode="ok"):
    for f in ("wifi-scan.json", "sticktime-wifi.mode"):
        try:
            os.remove(path("state", f))
        except FileNotFoundError:
            pass
    if mode != "ok":
        write(path("state", "sticktime-wifi.mode"), mode)


def scans():
    return len([x for x in helper_log() if x == "sticktime-wifi scan"])


@test
def wifi_scan():
    clear_scan()
    n0 = scans()
    eq(get("/api/wifi/scan?cached=1").status, 404, "cached=1 before any scan")
    eq(scans(), n0, "cached=1 never scans")
    r = get("/api/wifi/scan")
    j = is_json(r)
    eq([n["ssid"] for n in j["networks"]], ["Home", 'Caf\u00e9 "<b>"'])
    eq(j["networks"][0]["signal"], 72)
    ok(j["at"] > 1e12)
    eq(scans(), n0 + 1)
    # ?cached=1: the last results, never a scan (a scan interrupts the WiFi joystick)
    eq(get("/api/wifi/scan?cached=1").json(), j)
    eq(scans(), n0 + 1, "cached=1 never scans")
    # cached for 10 s
    eq(get("/api/wifi/scan").json(), j)
    eq(scans(), n0 + 1, "the cache answered")
    # an old cache is refreshed
    write(path("state", "wifi-scan.json"), json.dumps({"networks": [], "at": int(time.time() * 1000) - 60000}))
    eq(len(get("/api/wifi/scan").json()["networks"]), 2)
    eq(scans(), n0 + 2)
    # a note from the helper is passed on
    clear_scan("note")
    ok("hotspot" in get("/api/wifi/scan").json().get("note", ""), "note")
    # the first scan takes long: 503 Busy, then the answer
    clear_scan("slow")
    r = get("/api/wifi/scan")
    is_error(r, 503, "Busy")
    eq(r.h("retry-after"), "1")
    r = get("/api/wifi/scan")                       # waits for the same scan, doesn't start another
    if r.status == 503:
        time.sleep(2.5)
        r = get("/api/wifi/scan")
    eq(len(is_json(r)["networks"]), 2)
    eq(len([x for x in helper_log() if x == "sticktime-wifi scan"]), n0 + 4, "one scan for all the waiting requests")
    # failures: the helper's message, else what went wrong
    clear_scan("nodev")
    j = is_error(get("/api/wifi/scan"), 500, "Internal")
    ok("no WiFi device" in j["message"], j["message"])
    clear_scan("garbage")
    ok("JSON" in is_error(get("/api/wifi/scan"), 500, "Internal")["message"])
    clear_scan("crash")
    ok("exit code 1" in is_error(get("/api/wifi/scan"), 500, "Internal")["message"])
    # a failed rescan still answers with an old cache
    clear_scan("crash")
    write(path("state", "wifi-scan.json"), json.dumps({"networks": [{"ssid": "Old"}], "at": int(time.time() * 1000) - 60000}))
    eq(get("/api/wifi/scan").json()["networks"], [{"ssid": "Old"}])
    # no helper at all
    clear_scan()
    hp = os.path.join(path("libexec"), "sticktime-wifi" + (".py" if WIN else ""))
    os.rename(hp, hp + ".off")
    try:
        ok("installed" in is_error(get("/api/wifi/scan"), 500, "Internal")["message"])
    finally:
        os.rename(hp + ".off", hp)
    clear_scan()
    is_error(post("/api/wifi/scan"), 405, "MethodNotAllowed")


@test
def wifi_join():
    n0 = len(helper_log())
    body = "ssid = Racing Net\npassword = hunter22pw\ncountry = de\nhidden = 1\n"
    r = post("/api/wifi", body)
    eq(is_json(r), {"ok": True, "applied": True})
    ok(b"hunter22pw" not in r.body)
    t = settings_text()
    ok("wifi_ssid = Racing Net" in t, t)
    ok("hunter22pw" in t and "wifi_country = DE" in t and "wifi_hidden = on" in t, t)
    ok(wait_for(lambda: "sticktime-wifi apply" in helper_log()[n0:]), "apply ran")
    g = get("/api/settings")
    ok(b"hunter22pw" not in g.body)
    eq(g.json()["values"]["wifi_password_set"], True)
    # not hidden: the key goes; an open network: the password goes
    r = post("/api/wifi", "ssid = Open Cafe\npassword =\n", headers={"Content-Type": "text/plain"})
    eq(r.status, 200)
    t = settings_text()
    ok("wifi_hidden" not in t and "hunter22pw" not in t and "wifi_ssid = Open Cafe" in t, t)
    ok("wifi_country = DE" in t, "a missing country is left alone")
    # form encoding
    r = post("/api/wifi", "ssid=Home&password=secret-pass-123&country=US",
             headers={"Content-Type": "application/x-www-form-urlencoded"})
    eq(r.status, 200)
    # bad values: every field named, nothing saved
    before = settings_text()
    for b, fields in (("password = short\n", ["password", "ssid"]),
                      ("ssid = " + "s" * 33 + "\n", ["ssid"]),
                      ("ssid = x\npassword = " + "p" * 64 + "\n", ["password"]),
                      ("ssid = x\ncountry = USA\n", ["country"]),
                      ("ssid = x\ncountry = 1A\n", ["country"])):
        j = is_error(post("/api/wifi", b), 400, "BadRequest")
        eq(sorted(j["fields"]), fields, b)
    eq(post("/api/wifi", "ssid = x\npassword = " + "a1" * 32 + "\n").status, 200, "64 hex digits are a PSK")
    post("/api/wifi", "ssid = Home\npassword = secret-pass-123\ncountry = US\n")
    ok("secret-pass-123" in settings_text())
    ok(before != "", "")
    is_error(post("/api/wifi", "x" * 5000), 413, "PayloadTooLarge")
    is_error(get("/api/wifi"), 405, "MethodNotAllowed")


@test
def wifi_module_ap():
    n0 = len(helper_log())
    r = post("/api/wifi/module-ap", "elrs")
    eq(is_json(r), {"ok": True, "applied": True, "module_ip": "10.0.0.1"})
    ok("wifi_module_ip = 10.0.0.1" in settings_text())
    ok(wait_for(lambda: "sticktime-wifi module-ap elrs" in helper_log()[n0:]))
    eq(post("/api/wifi/module-ap", "TBS\n").json()["module_ip"], "192.168.4.1")
    ok(wait_for(lambda: "sticktime-wifi module-ap tbs" in helper_log()[n0:]))
    is_error(post("/api/wifi/module-ap", "frsky"), 400, "BadRequest")
    is_error(post("/api/wifi/module-ap", ""), 400, "BadRequest")
    post("/api/settings", "wifi_module_ip =\n")


# ================================================================ updates and the system
@test
def updates():
    try:
        os.remove(path("state", "update.json"))
    except FileNotFoundError:
        pass
    j = is_json(get("/api/update"))
    eq(j["state"], "idle")
    eq(j["app"]["installed"], get("/api/status").json()["version"])
    eq(j["game"]["installed"], "1.7.2")
    st = {"state": "downloading", "message": "Downloading 1.7.3", "progress": 0.5,
          "game": {"installed": "1.7.2", "latest": "1.7.3", "staged": "", "notes": "fixes"},
          "app": {"installed": "0.1.0", "latest": "0.2.0", "notes": ""}, "checked_at": 1760000000000, "error": ""}
    write(path("state", "update.json"), json.dumps(st))
    eq(is_json(get("/api/update")), st, "the state file as it is")
    write(path("state", "update.json"), '{"state":"checki')
    eq(get("/api/update").json()["state"], "idle", "a half-written file is not sent")
    write(path("state", "update.json"), "[1,2]")
    eq(get("/api/update").json()["state"], "idle", "only an object")
    os.remove(path("state", "update.json"))
    n0 = len(helper_log())
    eq(is_json(post("/api/update/check"), 202), {"ok": True})
    eq(is_json(post("/api/update/apply", "game"), 202), {"ok": True})
    eq(post("/api/update/apply", "APP\n").status, 202)
    eq(post("/api/update/rollback", "game").status, 202)
    eq(post("/api/update/rollback", "what = app").status, 202)
    want = ["sticktime-update check", "sticktime-update apply game", "sticktime-update apply app",
            "sticktime-update rollback game", "sticktime-update rollback app"]
    ok(wait_for(lambda: sorted(helper_log()[n0:]) == sorted(want), 5), f"helper calls {helper_log()[n0:]}")
    for p in ("/api/update/apply", "/api/update/rollback"):
        is_error(post(p, "both"), 400, "BadRequest")
        is_error(post(p, ""), 400, "BadRequest")
        is_error(post(p, "game; rm -rf /"), 400, "BadRequest")
    is_error(post("/api/update"), 405, "MethodNotAllowed")
    is_error(get("/api/update/check"), 405, "MethodNotAllowed")
    hp = os.path.join(path("libexec"), "sticktime-update" + (".py" if WIN else ""))
    os.rename(hp, hp + ".off")
    try:
        ok("isn't installed" in is_error(post("/api/update/check"), 500, "Internal")["message"])
    finally:
        os.rename(hp + ".off", hp)


@test
def system_routes():
    j = is_json(get("/api/system"))
    eq(j["hostname"], "sticktime")
    eq(j["ssh"], False)
    eq(j["password_set"], False)
    ok(j["disk"]["total_mb"] > 0 and 0 <= j["disk"]["free_mb"] <= j["disk"]["total_mb"], j["disk"])
    for k in ("os", "kernel", "model"):
        ok(isinstance(j[k], str), k)
    if not WIN:
        ok(j["kernel"], "uname")
    post("/api/settings", "ssh = on\npassword = pi-login-99\n")
    j = get("/api/system").json()
    eq((j["ssh"], j["password_set"]), (True, True))
    ok(b"pi-login-99" not in get("/api/system").body)
    post("/api/settings", "ssh = off\npassword_clear = 1\n")
    try:
        os.remove(path("state", "power.log"))
    except FileNotFoundError:
        pass
    power = lambda: read(path("state", "power.log")).decode().split() if os.path.exists(path("state", "power.log")) else []  # noqa: E731
    eq(is_json(post("/api/reboot"), 202), {"ok": True})
    ok(wait_for(lambda: power() == ["reboot"], 5), f"power {power()}")
    eq(is_json(post("/api/shutdown"), 202), {"ok": True})
    ok(wait_for(lambda: power() == ["reboot", "poweroff"], 5), f"power {power()}")
    mark = len(events())
    eq(is_json(post("/api/restart"), 202), {"ok": True})
    ok(wait_for(lambda: cmd_bits(mark) & CMD_QUIT, 3), "restart quits the loop")
    for p in ("/api/reboot", "/api/shutdown", "/api/restart"):
        is_error(get(p), 405, "MethodNotAllowed")
        is_error(post(p, xst=False), 403, "Forbidden")
    is_error(get("/api/nope"), 404, "NotFound")
    is_error(get("/api"), 404, "NotFound")
    is_error(post("/api/nope"), 404, "NotFound")


# ================================================================ HTTP itself
@test
def http_malformed():
    cases = [
        (b"GARBAGE\r\n\r\n", 400),
        (b"get / HTTP/1.1\r\nHost: x\r\n\r\n", 400),
        (b"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505),
        (b"GET / HTTP/1.1x\r\nHost: x\r\n\r\n", 400),
        (b"GET / http/1.1\r\nHost: x\r\n\r\n", 400),
        (b"GET  / HTTP/1.1\r\nHost: x\r\n\r\n", 400),
        (b"GET / HTTP/1.1 \r\nHost: x\r\n\r\n", 400),
        (b"GET http://x/ HTTP/1.1\r\nHost: x\r\n\r\n", 400),
        (b"GET * HTTP/1.1\r\nHost: x\r\n\r\n", 400),
        (b"GET /a#b HTTP/1.1\r\nHost: x\r\n\r\n", 400),
        (b"GET /\x80 HTTP/1.1\r\nHost: x\r\n\r\n", 400),
        (b"GET / HTTP/1.1\r\nHost: localhost\r\n folded\r\n\r\n", 400),
        (b"GET / HTTP/1.1\r\nHost: localhost\r\nNoColon\r\n\r\n", 400),
        (b"GET / HTTP/1.1\r\nHost: localhost\r\n: empty\r\n\r\n", 400),
        (b"GET / HTTP/1.1\r\nHost: localhost\r\nBad Name: x\r\n\r\n", 400),
        (b"GET / HTTP/1.1\r\nHost: localhost\r\nX: a\rb\r\n\r\n", 400),
        (b"GET / HTTP/1.1\r\nHost: localhost\r\nX: a\x00b\r\n\r\n", 400),
        (b"GET / HTTP/1.1\r\nHost: localhost\r\nX: a\x01b\r\n\r\n", 400),
        (b"POST /api/key HTTP/1.1\r\nHost: localhost\r\nContent-Length: abc\r\n\r\n", 400),
        (b"POST /api/key HTTP/1.1\r\nHost: localhost\r\nContent-Length: -1\r\n\r\n", 400),
        (b"POST /api/key HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab", 400),
        (b"POST /api/key HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1234567890123456\r\n\r\n", 400),
        (b"GET / HTTP/1.1\r\nHost: localhost\r\nExpect: 200-ok\r\n\r\n", 417),
    ]
    for data, status in cases:
        c = Raw(5)
        try:
            c.send(data)
            r = c.response()
            j = is_error(r, status, "BadRequest")
            ok(j["message"])
            eq(r.h("connection"), "close", f"{data!r} closes")
            ok(c.closed_within(3), f"{data!r}: the server closed")
        finally:
            c.close()
    # the same Content-Length twice is fine
    r = raw_once(b"POST /api/key HTTP/1.1\r\nHost: localhost\r\nX-StickTime: 1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nenter")
    eq(r.status, 200)
    # blank lines before a request are skipped; LF-only line ends work
    eq(raw_once(b"\r\n\r\nGET /api/sticks HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n").status, 200)
    eq(raw_once(b"GET /api/sticks HTTP/1.1\nHost: localhost\nConnection: close\n\n").status, 200)


@test
def http_limits():
    # headers over 16 KB: 431 and close
    big = b"GET / HTTP/1.1\r\nHost: localhost\r\nX-Big: " + b"a" * 17000 + b"\r\n\r\n"
    r = raw_once(big)
    is_error(r, 431, "BadRequest")
    eq(r.h("connection"), "close")
    # a request line over 16 KB
    is_error(raw_once(b"GET /" + b"a" * 17000 + b" HTTP/1.1\r\nHost: localhost\r\n\r\n"), 431, "BadRequest")
    # just under the limit is fine
    ok_req = b"GET /api/sticks HTTP/1.1\r\nHost: localhost\r\nX-Big: " + b"a" * 15000 + b"\r\nConnection: close\r\n\r\n"
    eq(raw_once(ok_req).status, 200)
    # more than 100 header lines
    many = b"GET / HTTP/1.1\r\nHost: localhost\r\n" + b"".join(b"X-%d: y\r\n" % i for i in range(101)) + b"\r\n"
    is_error(raw_once(many), 431, "BadRequest")
    # long values the server keeps a copy of are cut, never overflow
    r = raw_once(rq("GET", "/", "If-None-Match: " + '"x",' * 1000 + "\r\nAccept-Encoding: " + "gzip," * 500 + "\r\n"))
    eq(r.status, 200)
    r = raw_once(rq("POST", "/api/game/reload", "X-StickTime: 1\r\nOrigin: http://" + "a" * 1000 + "\r\n"))
    is_error(r, 403, "Forbidden")
    is_error(raw_once(rq("GET", "/api/status", host="a" * 300)), 400, "BadRequest")


@test
def http_keepalive_and_pipelining():
    c = Raw(10)
    try:
        for i in range(5):
            c.send(rq("GET", "/api/sticks"))
            r = c.response()
            eq(r.status, 200)
            eq(r.h("connection"), "keep-alive")
        # three at once, answered in order
        c.send(rq("GET", "/api/sticks") + rq("GET", "/nope") + rq("POST", "/api/key", "X-StickTime: 1\r\n", "enter") +
               rq("HEAD", "/") + rq("GET", "/api/log"))
        rs = [c.response(head_only=(i == 3)) for i in range(5)]
        eq([r.status for r in rs], [200, 404, 200, 200, 200])
        eq(rs[2].json(), {"ok": True})
        ok(int(rs[3].h("content-length")) > 0 and rs[3].body == b"")
        eq(rs[4].h("content-type"), "text/plain; charset=utf-8")
        # Connection: close
        c.send(rq("GET", "/api/sticks", "Connection: close\r\n"))
        r = c.response()
        eq(r.h("connection"), "close")
        ok(c.closed_within(3), "closed after Connection: close")
    finally:
        c.close()
    # HTTP/1.0 closes unless asked to keep alive
    c = Raw(10)
    try:
        c.send(b"GET /api/sticks HTTP/1.0\r\nHost: localhost\r\n\r\n")
        eq(c.response().h("connection"), "close")
        ok(c.closed_within(3))
    finally:
        c.close()
    c = Raw(10)
    try:
        c.send(b"GET /api/sticks HTTP/1.0\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n")
        eq(c.response().h("connection"), "keep-alive")
        c.send(b"GET /api/sticks HTTP/1.0\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n")
        eq(c.response().status, 200)
    finally:
        c.close()
    # an unread body is read away (keep-alive); a big one closes the connection
    c = Raw(10)
    try:
        c.send(rq("POST", "/api/game/reload", "X-StickTime: 1\r\n", b"z" * 100000))
        r = c.response()
        eq((r.status, r.h("connection")), (200, "keep-alive"))
        c.send(rq("GET", "/api/sticks"))
        eq(c.response().status, 200, "the next request after a drained body")
    finally:
        c.close()
    c = Raw(30)
    try:
        c.send(f"POST /api/game/reload HTTP/1.1\r\nHost: localhost\r\nX-StickTime: 1\r\nContent-Length: {3 << 20}\r\n\r\n".encode())
        r = c.response()
        eq((r.status, r.h("connection")), (200, "close"))
    finally:
        c.close()


@test
def http_expect_continue():
    set_policy("game")
    c = Raw(10)
    try:
        body = b"Track Six\n1 0\n"
        c.send(f"PUT /api/sd/{SLOT}/c6.txt HTTP/1.1\r\nHost: localhost\r\nX-StickTime: 1\r\nExpect: 100-continue\r\n"
               f"Content-Length: {len(body)}\r\n\r\n".encode())
        st, hd = c.head()
        eq(st, 100, "100 Continue before the body")
        c.send(body)
        r = c.response()
        eq(r.status, 200)
        eq(read(sdp(SLOT + "/c6.txt")), body)
        # refused before the body: no 100, and the connection closes (the body never comes)
        c2 = Raw(10)
        try:
            c2.send(f"PUT /api/sd/MODELS/x.yml HTTP/1.1\r\nHost: localhost\r\nX-StickTime: 1\r\nExpect: 100-continue\r\n"
                    "Content-Length: 5\r\n\r\n".encode())
            r = c2.response()
            is_error(r, 403, "NoModificationAllowedError")
            eq(r.h("connection"), "close")
        finally:
            c2.close()
        c.send(rq("GET", "/api/sticks"))
        eq(c.response().status, 200, "the first connection is still good")
    finally:
        c.close()


def open_held(n):
    """n keep-alive connections, each known to hold a worker (it answered one request)"""
    held = []
    for _ in range(n):
        for attempt in range(40):
            c = Raw(10)
            c.send(rq("GET", "/api/sticks"))
            r = c.response()
            if r.status == 200:
                held.append(c)
                break
            c.close()
            time.sleep(0.1)          # a worker of an earlier test is still finishing
        else:
            raise Fail("couldn't open a connection")
    return held


@test
def http_client_limit():
    held = open_held(16)
    try:
        t0 = time.time()
        r = raw_once(rq("GET", "/api/status"))
        j = is_error(r, 503, "Busy")
        eq(r.h("retry-after"), "1")
        ok(time.time() - t0 < 2, "answered at once")
        ok(j["message"])
        # a held connection still works
        held[0].send(rq("GET", "/api/sticks"))
        eq(held[0].response().status, 200)
        # one goes: room again
        held.pop().close()
        ok(wait_for(lambda: raw_once(rq("GET", "/api/sticks", "Connection: close\r\n")).status == 200, 3), "room after a close")
    finally:
        for c in held:
            c.close()
    time.sleep(0.5)
    # SSE streams count as clients
    streams = []
    try:
        while len(streams) < 16:
            c = Raw(10)
            c.send(rq("GET", "/api/sticks/stream"))
            st, hd = c.head()
            if st == 200:
                streams.append(c)
                continue
            c.close()
            ok(st == 503, f"stream {st}")
            time.sleep(0.1)             # a worker from above is still finishing
        is_error(raw_once(rq("GET", "/api/status")), 503, "Busy")
    finally:
        for c in streams:
            c.close()
    ok(wait_for(lambda: raw_once(rq("GET", "/api/sticks", "Connection: close\r\n")).status == 200, 3), "streams end with their clients")


@test
def vtx_control():
    """/api/vtx and /api/vtx/table against the fake VTX (web_test_main runs VTX control on STP_VTX_PORT)"""
    tpath = path("vtxtable.json")
    if os.path.exists(tpath):
        os.unlink(tpath)

    def vtx():
        return is_json(get("/api/vtx"))

    # off (the file has no vtx key): the built-in table, nothing found
    v = vtx()
    eq(v["state"], "off", "state")
    eq(v["enabled"], False, "enabled")
    eq(v["found"], False, "found")
    eq(v["power"], None, "power")
    eq(v["table"]["source"], "default", "table source")
    eq([b["letter"] for b in v["table"]["bands"]], ["A", "B", "E", "F", "R"], "bands")
    eq(v["table"]["bands"][4]["frequencies"][0], 5658, "R1")
    eq(v["settings"], {"band": "", "channel": 1, "freq": 0, "power": 0, "pit": False}, "settings")
    eq(is_json(get("/api/status"))["vtx"], {"state": "off", "text": "off"}, "status line")
    r = request("HEAD", "/api/vtx")
    eq((r.status, r.body), (200, b""), "HEAD")
    r = request("PUT", "/api/vtx", b"x")
    is_error(r, 405, "MethodNotAllowed")
    eq(r.h("allow"), "GET, HEAD, POST", "Allow")
    is_error(request("POST", "/api/vtx/table", b"{}"), 405, "MethodNotAllowed")
    is_error(post("/api/vtx", b"power = 1\n", xst=False), 403, "Forbidden")
    # turning it on: a reboot (its UART overlay); the thread re-reads the settings and finds the VTX
    mark = len(events())
    r = is_json(post("/api/settings", b"vtx = auto\nvtx_band = R\nvtx_channel = 1\nvtx_power = 1\n"))
    eq(r["reboot"], False, "off -> auto on one pin (gpio4): no overlay, no reboot")
    eq(is_json(post("/api/settings", b"vtx_port = uart3\n"))["reboot"], True, "two pins: the UART overlay needs a reboot")
    eq(is_json(post("/api/settings", b"vtx_port = uart3\n"))["reboot"], False, "the same again")
    eq(is_json(post("/api/settings", b"vtx_port = gpio22\n"))["reboot"], False, "back to one pin")
    ok(wait_event(mark, lambda e: e == f"cmd {CMD_RELOAD_SETTINGS}"), "settings reload posted")
    ok(wait_for(lambda: vtx()["state"] == "ready", 15), f"found: {vtx()['message']}")
    v = vtx()
    eq((v["protocol"], v["version"], v["band"], v["channel"], v["freq"], v["in_sync"]), ("smartaudio", "2.1", "R", 1, 5658, True), "found")
    eq(v["power"], {"level": 1, "label": "25", "value": 14, "mw": 25}, "power")
    eq(VTX_DEV.state()["channel"], 32, "the VTX on R1")
    eq(len(v["table"]["powers"]), 3, "the 2.1 VTX's own levels")
    ok("echo_ok" in v["stats"] and v["stats"]["replies"] > 0, v["stats"])
    st = is_json(get("/api/status"))["vtx"]
    eq(st["state"], "ready", "status state")
    ok("SmartAudio 2.1" in st["text"] and "R1 5658 MHz" in st["text"] and "25 mW" in st["text"], st["text"])
    r = is_json(post("/api/settings", b"vtx = smartaudio\n"))
    eq(r["reboot"], False, "a protocol change needs no reboot")
    # POST /api/vtx: saved like settings, applied
    r = is_json(post("/api/vtx", b"band = e\nchannel = 3\npower = 3\npit = off\n"))
    eq(sorted(r["saved"]), ["vtx_band", "vtx_channel", "vtx_freq", "vtx_pit", "vtx_power"], "saved (new lines too)")
    ok(wait_for(lambda: VTX_DEV.state()["channel"] == 18 and VTX_DEV.state()["dbm"] == 26, 10), f"E3 at 26 dBm: {VTX_DEV.state()}")
    txt = settings_text()
    ok("vtx_band = E\n" in txt and "vtx_channel = 3\n" in txt and "vtx_power = 3\n" in txt, txt)
    ok("This comment must survive every save." in txt, "comments kept")
    r = is_json(post("/api/vtx", b"vtx_freq = 5900\n"))
    eq(r["saved"], ["vtx_freq"], "the settings' names work too")
    ok(wait_for(lambda: VTX_DEV.state()["freq"] == 5900, 10), "5900 MHz")
    eq(is_json(post("/api/vtx", b"band = R\n"))["saved"], ["vtx_band", "vtx_freq"], "a band clears the frequency")
    ok(wait_for(lambda: VTX_DEV.state()["channel"] == 34, 10), "R3")
    before = settings_text()
    r = post("/api/vtx", b"band = Q\nchannel = 9\nfreq = 7000\npower = 4\npit = sometimes\n")
    is_error(r, 400, "BadRequest")
    eq(sorted(r.json()["fields"]), ["band", "channel", "freq", "pit", "power"], "fields")
    ok("(A, B, E, F, R)" in r.json()["fields"]["band"], r.json()["fields"]["band"])
    ok("1 to 3" in r.json()["fields"]["power"], r.json()["fields"]["power"])
    eq(settings_text(), before, "nothing saved")
    is_error(post("/api/vtx", b"nothing = here\n"), 400, "BadRequest")
    is_error(post("/api/vtx", b"band = A\n" * 1000), 413, "PayloadTooLarge")
    # the table: download, upload a Betaflight JSON, use it, a broken one, back to the built-in one
    r = get("/api/vtx/table")
    eq(r.status, 200, "GET table")
    ok("attachment" in (r.h("content-disposition") or ""), r.h("content-disposition"))
    j = r.json()
    eq(j["version"], "1.0", "version")
    eq(j["vtx_table"]["bands_list"][0]["is_factory_band"], True, "factory")
    eq([p["value"] for p in j["vtx_table"]["powerlevels_list"]], [14, 20, 26], "levels")
    table = {"description": "Test", "version": "1.0", "vtx_table": {
        "bands_list": [{"name": "RACEBAND", "letter": "R", "is_factory_band": True, "frequencies": [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]},
                       {"name": "LOWRACE", "letter": "L", "is_factory_band": False, "frequencies": [5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621]}],
        "powerlevels_list": [{"value": 14, "label": "25 "}, {"value": 20, "label": "100"}]}}
    r = is_json(request("PUT", "/api/vtx/table", json.dumps(table).encode()))
    eq((r["bands"], r["channels"], r["powers"], r["description"]), (2, 8, 2, "Test"), "PUT")
    stored = json.loads(read(tpath))
    eq(stored["vtx_table"]["powerlevels_list"][0]["label"], "25", "stored normalized")
    ok(wait_for(lambda: vtx()["table"]["source"] == "file", 5), "the file's table in use")
    is_json(post("/api/vtx", b"band = L\nchannel = 8\npower = 2\n"))
    ok(wait_for(lambda: VTX_DEV.state()["freq"] == 5621 and VTX_DEV.state()["dbm"] == 20, 10), f"L8: {VTX_DEV.state()}")
    r = request("PUT", "/api/vtx/table", b'{"version": "1.0", "vtx_table": {"bands_list": [{"letter": "A"}]}}')
    is_error(r, 400, "BadRequest")
    ok("no frequencies" in r.json()["message"], r.json()["message"])
    is_error(request("PUT", "/api/vtx/table", b"not json"), 400, "BadRequest")
    is_error(request("PUT", "/api/vtx/table", b" " * (64 * 1024 + 1)), 413, "PayloadTooLarge")
    eq(json.loads(read(tpath))["description"], "Test", "a refused table leaves the file alone")
    is_ok = request("DELETE", "/api/vtx/table")
    eq(is_ok.status, 200, "DELETE")
    ok(not os.path.exists(tpath), "the file is gone")
    eq(request("DELETE", "/api/vtx/table").status, 200, "DELETE again")
    ok(wait_for(lambda: vtx()["table"]["source"] == "default" and "Band L" in vtx()["error"], 5), "band L is gone")
    # a broken file on the card: the built-in table and an error that says so
    write(tpath, "{ broken")
    is_json(post("/api/vtx", b"band = R\nchannel = 1\n"))
    ok(wait_for(lambda: "vtxtable.json can't be used" in vtx()["table"]["error"] and vtx()["state"] == "error", 5), vtx()["table"])
    os.unlink(tpath)
    # off again
    is_json(post("/api/settings", b"vtx = off\n"))
    ok(wait_for(lambda: vtx()["state"] == "off", 5), "off")


def slow_idle():
    """15 s timeouts, in parallel: idle connection, slow headers, keep-alive idle, stalled body"""
    set_policy("game")
    write(sdp(SLOT + "/c7.txt"), "Track Seven\n1 0\n")
    res = {}

    def idle():
        c = Raw(30)
        t0 = time.time()
        res["idle"] = (c.closed_within(25), time.time() - t0, c.buf)
        c.close()

    def slowloris():
        c = Raw(30)
        t0 = time.time()
        c.send(b"GET / HTTP/1.1\r\nHost: localhost\r\n")
        try:
            i = 0
            while time.time() - t0 < 25 and not select.select([c.s], [], [], 1.5)[0]:
                c.send(b"X-%d: y\r\n" % i)        # a header line every 1.5 s, never the end
                i += 1
            c.s.settimeout(10)
            r = c.response()
            res["slow"] = (r.status, time.time() - t0, r.h("connection"))
        except Exception as e:  # noqa: BLE001
            res["slow"] = (repr(e), time.time() - t0, None)
        c.close()

    def keepalive():
        c = Raw(30)
        c.send(rq("GET", "/api/sticks"))
        c.response()
        t0 = time.time()
        res["ka"] = (c.closed_within(25), time.time() - t0)
        c.close()

    def stalled_body():
        c = Raw(30)
        c.send(f"PUT /api/sd/{SLOT}/c7.txt HTTP/1.1\r\nHost: localhost\r\nX-StickTime: 1\r\nContent-Length: 100\r\n\r\n0123456789".encode())
        t0 = time.time()
        res["body"] = (c.closed_within(25), time.time() - t0, c.buf)
        c.close()

    ts = [threading.Thread(target=f) for f in (idle, slowloris, keepalive, stalled_body)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    closed, dt, buf = res["idle"]
    ok(closed and buf == b"" and 13.5 < dt < 18, f"idle connection: {res['idle']}")
    st, dt, conn = res["slow"]
    ok(st == 408 and 13.5 < dt < 19 and conn == "close", f"slow headers: {res['slow']}")
    closed, dt = res["ka"]
    ok(closed and 13.5 < dt < 18, f"keep-alive idle: {res['ka']}")
    closed, dt, buf = res["body"]
    ok(closed and 13.5 < dt < 18 and buf == b"", f"stalled body: {res['body']}")
    eq(read(sdp(SLOT + "/c7.txt")), b"Track Seven\n1 0\n", "a stalled PUT changes nothing")
    no_temp_files(SLOT)


# ================================================================ main
def main():
    global PORT, BASE
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("-k", action="append", default=[])
    ap.add_argument("--no-slow", action="store_true")
    ap.add_argument("--tmp")
    a = ap.parse_args()
    exe = os.path.abspath(a.exe) if a.exe else find_exe()
    BASE = tempfile.mkdtemp(prefix="stp-web-", dir=a.tmp)
    make_base(BASE)
    PORT = free_port()
    print(f"server {exe}\nbase   {BASE}\nport   {PORT}")
    global VTX_LINK
    VTX_LINK = fake_vtx.TcpLink(VTX_DEV)
    srv = Server(exe, BASE, PORT)
    srv.start()
    tests = list(TESTS)
    if not a.no_slow:
        tests.append(slow_idle)
    if a.k:
        tests = [t for t in tests if any(k in t.__name__ for k in a.k)]
    failed = []
    t_all = time.time()
    try:
        for t in tests:
            t0 = time.time()
            try:
                t()
                print(f"ok    {t.__name__} ({time.time() - t0:.1f} s)")
            except Exception as e:  # noqa: BLE001
                failed.append(t.__name__)
                kind = "" if isinstance(e, Fail) else type(e).__name__ + ": "
                print(f"FAIL  {t.__name__}: {kind}{e}")
                if not isinstance(e, Fail):
                    import traceback
                    traceback.print_exc()
            if srv.p.poll() is not None:
                failed.append("server died")
                print(f"FAIL  the server exited ({srv.p.returncode})")
                break
        code = srv.stop() if srv.p.poll() is None else srv.p.returncode
        if code != 0:
            failed.append("graceful stop")
            print(f"FAIL  web_stop: exit code {code}")
        else:
            print("ok    graceful stop (web_stop)")
    finally:
        srv.kill()
        if failed:
            try:
                tail = read(os.path.join(BASE, "server.log")).decode("utf-8", "replace").splitlines()[-25:]
                print("server.log (end):\n  " + "\n  ".join(tail))
            except OSError:
                pass
        if not a.keep and not failed:
            shutil.rmtree(BASE, ignore_errors=True)
        else:
            print(f"kept {BASE}")
    print(f"{len(tests) - len([f for f in failed if f in [t.__name__ for t in tests]])}/{len(tests)} tests passed"
          f" in {time.time() - t_all:.0f} s" + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
