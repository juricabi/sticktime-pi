#!/usr/bin/env python3
"""The settings app and the patched emulator page against the REAL C server: host/tests/web_test_main.c
(web_start() of host/src/web.c + web_api.c with a fake frame loop) on a temporary base folder with
this repo's web/ files and fake helpers. The mock (mock_server.py) follows docs/WEB_API.md; this
checks that the page and the shim also work with what the server really sends.

    python web/tests/test_real_server.py [--exe PATH]

The program: --exe, $STP_WEB_TEST_MAIN, or the newest build/*/web_test_main[.exe] (any agent's
build dir, e.g. build/agent-webui/real/). Skipped when there is none. Standard library + Playwright.
"""
import glob
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
WEB = REPO / "web"
SIM = REPO.parent / "sticktime" / "web" / "simulator.html"
WIN = os.name == "nt"
EXE = ".exe" if WIN else ""
HOST = "sticktime.local"
SLOTS = "SCRIPTS/TOOLS/StickTimeTracks"
MAX_UPLOAD = 1 << 20
sys.dont_write_bytecode = True                  # (no __pycache__ in the repo)
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "host" / "tests"))
import fake_vtx  # noqa: E402  (a fake SmartAudio VTX for the VTX card: web_test_main's STP_VTX_PORT)
import patch_editor  # noqa: E402
from playwright.sync_api import sync_playwright  # noqa: E402

SETTINGS = """\
# StickTime Pi settings (test_real_server)
# this comment stays through every save

video = av          # composite to the VTX
tv = ntsc
volume = 70
wifi_ssid = Home
wifi_password = "secret-pass-123"
github_token = ghp_TOPSECRET42
my_custom_key = keep me
"""
# fake helpers: log the call; sticktime-wifi scan answers a list; sticktime-update writes the state file
HELPER = r'''#!{python}
import json, os, sys, time
STATE, NAME, args = {state!r}, {name!r}, sys.argv[1:]
with open(os.path.join(STATE, "calls.log"), "a") as f:
    f.write(NAME + " " + " ".join(args) + "\n")
if NAME == "sticktime-wifi" and args[:1] == ["scan"]:
    print(json.dumps({{"networks": [{{"ssid": "Home", "signal": 72, "security": "WPA2", "band": "2.4", "chan": 6, "known": True}},
                                   {{"ssid": "Café \"<b>\"", "signal": 40, "security": "", "band": "5", "chan": 36, "known": False}}],
                      "at": int(time.time() * 1000)}}))
    sys.exit(0)
if NAME == "sticktime-update":
    st = {{"state": "done", "message": "Updates available.", "progress": 1, "checked_at": int(time.time() * 1000), "error": "",
          "game": {{"installed": "1.7.2", "latest": "1.7.3", "staged": "1.7.3", "notes": "### New in 1.7.3\n- Riverside"}},
          "app": {{"installed": "test", "latest": "", "notes": ""}}}}
    tmp = os.path.join(STATE, "update.json.tmp")
    with open(tmp, "w") as f:
        json.dump(st, f)
    os.replace(tmp, os.path.join(STATE, "update.json"))
print(json.dumps({{"ok": True}}))
'''
# ETX_EVT_* (etx.h) and APP_KEY_* (app.h), as the fake loop logs them
KEYS = {"enter": 0x202, "exit": 0x201, "next": 0x1E00, "prev": 0x1D00, "inc": 0x1E00, "dec": 0x1D00,
        "nextpage": 0x204, "prevpage": 0x203, "menu": 0x206, "up": -101, "down": -102}


def find_exe():
    for a in sys.argv[1:]:
        if a.startswith("--exe="):
            return a[6:]
    if os.environ.get("STP_WEB_TEST_MAIN"):
        return os.environ["STP_WEB_TEST_MAIN"]
    cands = [c for c in glob.glob(str(REPO / "build" / "**" / ("web_test_main" + EXE)), recursive=True) if os.path.isfile(c)]
    return max(cands, key=os.path.getmtime) if cands else None


def track(src, name):
    """a gallery track under another name (renaming keeps it valid); a made-up one without ../sticktime"""
    f = REPO.parent / "sticktime" / "tracks" / src
    if not f.is_file():
        return name + "\n1 0\n"
    return name + "\n" + f.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n")[1] + "\n"


def write(p, data):
    p = pathlib.Path(p)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data.encode("utf-8") if isinstance(data, str) else data)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class RealServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = find_exe()
        cls.exe = cls.exe and os.path.abspath(cls.exe)
        if not cls.exe:
            raise unittest.SkipTest("no web_test_main: build it (host/build.sh, or see the report of the webui agent)")
        cls.base = pathlib.Path(tempfile.mkdtemp(prefix="sticktime-real-"))
        b = cls.base
        write(b / "sticktime.txt", SETTINGS)
        write(b / "sd" / "SCRIPTS/TOOLS/StickTime.lua", "-- TNS|StickTime|TNE\nreturn { run = function() return 0 end }\n")
        write(b / "sd" / "SCRIPTS/TOOLS/Other.lua", "return {}\n")
        write(b / "sd" / SLOTS / "c1.txt", track("multigp-2017-utt.txt", "Track One"))
        write(b / "sd" / SLOTS / "C5.TXT", track("multigp-2018-rq.txt", "Track Five"))
        write(b / "sd" / "SCRIPTS/TOOLS/StickTime.dat", b"\x00save")
        (b / "music").mkdir()
        (b / "web").mkdir()
        for f in ("index.html", "app.js", "style.css", "pi-sd.js"):
            shutil.copy(WEB / f, b / "web" / f)
        cls.editor = SIM.is_file()
        if cls.editor:
            write(b / "editor" / "index.html", patch_editor.patch(SIM.read_bytes().decode("utf-8")))
        state = b / "state"
        state.mkdir()
        for n in ("sticktime-wifi", "sticktime-update", "power"):
            p = b / "libexec" / (n + (".py" if WIN or n == "power" else ""))
            write(p, HELPER.format(python=sys.executable, name=n, state=str(state)))
            if not WIN:
                os.chmod(p, 0o755)
        cls.vtx = fake_vtx.FakeVtx("sa", 3)
        cls.vtx_link = fake_vtx.TcpLink(cls.vtx)
        env = dict(os.environ, STICKTIME_PYTHON=sys.executable,
                   STICKTIME_POWER_CMD=f'"{sys.executable}" "{b / "libexec" / "power.py"}"',
                   STP_VTX_PORT=f"tcp:127.0.0.1:{cls.vtx_link.port}")
        cls.port = free_port()
        cls.log = open(b / "server.log", "wb")
        cls.proc = subprocess.Popen([cls.exe, str(b), str(cls.port), "sticktime", str(MAX_UPLOAD)], stdout=subprocess.PIPE, stderr=cls.log, env=env)
        ready = threading.Event()
        threading.Thread(target=lambda: [ready.set() for line in cls.proc.stdout if line.strip() == b"READY"], daemon=True).start()
        if not ready.wait(15):
            cls.proc.kill()
            raise RuntimeError("the server didn't start: " + str(b / "server.log"))
        cls.url = f"http://{HOST}:{cls.port}/"
        cls.pw = sync_playwright().start()
        cls.br = cls.pw.chromium.launch(args=[f"--host-resolver-rules=MAP {HOST} 127.0.0.1"])

    @classmethod
    def tearDownClass(cls):
        cls.br.close()
        cls.pw.stop()
        write(cls.base / "state" / "stop", "")
        try:
            cls.proc.wait(15)
        except subprocess.TimeoutExpired:
            cls.proc.kill()
        cls.log.close()
        cls.vtx_link.close()
        if "--keep" not in sys.argv:
            shutil.rmtree(cls.base, ignore_errors=True)

    def setUp(self):
        self.errors, self.answer = [], True
        self.ctx = self.br.new_context(viewport={"width": 1280, "height": 900}, accept_downloads=True)
        p = self.page = self.ctx.new_page()
        p.on("console", lambda m: m.type == "error" and not m.text.startswith("Failed to load resource") and self.errors.append(m.text))
        p.on("pageerror", lambda e: self.errors.append("pageerror: " + str(e)))
        p.on("dialog", lambda d: d.accept() if self.answer else d.dismiss())

    def tearDown(self):
        self.ctx.close()
        self.assertEqual(self.errors, [], "no errors on the page")

    def open(self, tab):
        self.page.goto(self.url + "#" + tab)
        self.page.wait_for_function("window.stp && stp.S.values && stp.status && stp.forms.every((f) => f.fields.size || f.spec === '*')", timeout=15000)
        return self.page

    def tab(self, name):
        self.page.click(f'#tabs a[data-page="{name}"]')
        self.page.wait_for_selector(f"#p-{name}", state="visible")

    def server_side(self, tab):
        """what the server says, for a failure message: the tab's API answer and the end of its log"""
        import http.client
        out = []
        for path in {"system": ["/api/system", "/api/log"]}.get(tab, ["/api/status"]):
            try:
                c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
                c.request("GET", path, headers={"Host": f"{HOST}:{self.port}"})
                r = c.getresponse()
                out.append(f"GET {path}: {r.status} {r.read()[:600]!r}")
            except Exception as e:
                out.append(f"GET {path}: {e!r}")
        try:
            out.append("server.log: " + (self.base / "server.log").read_text(errors="replace")[-3000:])
        except OSError as e:
            out.append(f"server.log: {e}")
        return "\n".join(out)

    def events(self):
        try:
            return (self.base / "state" / "events.log").read_text().splitlines()
        except OSError:
            return []

    def calls(self):
        try:
            return (self.base / "state" / "calls.log").read_text().splitlines()
        except OSError:
            return []

    def until(self, fn, timeout=8.0, what="the condition"):
        end = time.time() + timeout
        while not fn():
            if time.time() > end:
                self.fail("timed out waiting for " + what)
            self.page.wait_for_timeout(50)

    def confirm(self):
        self.page.wait_for_selector("#confirmDlg[open]")
        self.page.click("#confirmYes")

    # ---------------------------------------------------------------- the settings app
    def test_every_tab(self):
        p = self.open("status")
        p.wait_for_function("!document.getElementById('shotImg').hidden", timeout=8000)
        self.assertEqual(p.evaluate("document.getElementById('shotImg').naturalWidth"), 64, "the screenshot PNG of the fake loop")
        self.assertEqual(p.inner_text("[data-f=fps]"), "59.9")
        self.assertEqual(p.inner_text("[data-f=state]"), "In the menu")
        self.assertTrue(p.is_visible("#gameError"))
        self.assertIn('boom: "x" <b>', p.inner_text("#gameErrorText"), "the game's error as text")
        self.assertIsNone(p.query_selector("#gameErrorText b"))
        loaded = {
            "sticks": "document.querySelectorAll('#srcCards .src-card').length === 2",
            "video": "document.querySelectorAll('form[data-form=video] .field').length === 8",
            "wifi": "document.getElementById('scanNote').textContent.includes('Press Scan')",   # no scan yet: none at opening
            "sound": "document.querySelector('#songList li.empty') !== null",
            "tracks": "document.querySelectorAll('#slotList li').length === 8",
            "game": "document.querySelectorAll('#toolList label').length === 2",
            "updates": "document.getElementById('updMsg').textContent !== '–'",
            "system": "document.querySelector('[data-s=os]').textContent !== '–'",
        }
        for t, cond in loaded.items():
            self.tab(t)
            try:
                p.wait_for_function(cond, timeout=10000)
            except Exception as e:
                raise AssertionError(f"the {t} tab didn't load ({cond}): {e}\n{self.server_side(t)}") from None
            got = p.evaluate("(t) => stp.forms.filter((f) => f.spec !== '*' && f.el.closest('.page').dataset.page === t).map((f) => [f.spec, [...f.fields.keys()].join(',')])", t)
            for spec, keys in got:
                self.assertEqual(keys, spec, f"{t}: every key of the form is in the server's metadata")
        self.assertTrue(p.is_hidden("#moreCard"), "every key the server knows has its place on a page")
        self.tab("sticks")
        self.assertEqual(p.eval_on_selector_all("#srcCards .src-card:nth-child(2) .bar", "els => els.map((e) => e.classList.contains('off'))"), [False] * 4 + [True] * 12)
        self.assertIn(p.inner_text("#mixBars .bar:nth-child(5) output"), ("0", "1024"), "a pause of null")
        self.tab("tracks")
        rows = p.eval_on_selector_all("#slotList li", "els => els.map((e) => e.innerText.replace(/\\s+/g, ' ').trim())")
        self.assertEqual(rows[0], "c1 Track One track 9 Download")
        self.assertEqual(rows[4], "c5 Track Five track 13 Download")
        self.tab("wifi")
        p.click("#scanBtn")
        p.wait_for_function("document.querySelectorAll('#netList li').length === 2", timeout=10000)
        self.assertEqual(p.eval_on_selector_all("#netList .ssid", "els => els.map((e) => e.textContent)"), ["Home", 'Café "<b>"'])
        self.tab("system")
        if WIN:
            self.assertIn("not a Pi", p.inner_text("[data-s=os]"))
        else:
            self.assertNotEqual(p.inner_text("[data-s=os]"), "–", "the OS from /etc/os-release")
        txt = p.input_value("#fileText")
        self.assertIn("# wifi_password is set", txt)
        self.assertNotIn("secret-pass-123", p.content())
        self.assertNotIn("ghp_TOPSECRET42", p.content())

    def test_vtx_card(self):
        """the VTX card with the real server, its VTX thread and the fake VTX behind it"""
        p = self.open("video")
        p.wait_for_selector("#vtxPill:has-text('Off')")
        p.check('input[name="f-vtx-vtx"][value="auto"]')
        p.click('form[data-form="vtx"] .savebar button[type=submit]')
        p.wait_for_selector("#vtxPill:has-text('Ready')", timeout=15000)
        self.assertTrue(p.is_hidden("#rebootBanner"))                  # one pin (the default): no overlay, no reboot
        self.assertIn("vtx = auto", (self.base / "sticktime.txt").read_text())
        self.assertIn("SmartAudio 2.1", p.inner_text("#vtxDev"))
        p.select_option("#f-vtx-vtx_port", "uart4")                     # two pins: the UART's overlay needs a boot
        p.click('form[data-form="vtx"] .savebar button[type=submit]')
        p.wait_for_selector("#rebootBanner", state="visible")
        p.click("#rebootLater")
        p.select_option("#f-vtx-vtx_port", "gpio4")
        p.click('form[data-form="vtx"] .savebar button[type=submit]')
        self.until(lambda: "vtx_port = gpio4" in (self.base / "sticktime.txt").read_text(), what="back to one pin")
        p.select_option("#vtxBand", "R")
        self.until(lambda: self.vtx.state()["channel"] == 32, what="R1 on the VTX")
        p.click("#vtxChan label:has(input[value='3'])")
        self.until(lambda: self.vtx.state()["channel"] == 34, what="R3 on the VTX")
        p.click("#vtxPower label:has(input[value='3'])")
        self.until(lambda: self.vtx.state()["dbm"] == 26, what="26 dBm on the VTX")
        p.wait_for_selector("#vtxNow:has-text('R3 · 5732 MHz')", timeout=10000)
        p.wait_for_selector("#vtxPower input[value='3'] + span.now", timeout=10000)
        text = (self.base / "sticktime.txt").read_text()
        self.assertIn("vtx_band = R", text)
        self.assertIn("vtx_channel = 3", text)
        self.assertIn("this comment stays through every save", text)
        self.vtx.press(channel=0)                                       # the VTX's button: the Pi sets it back
        self.until(lambda: self.vtx.state()["channel"] == 34, timeout=10, what="set back after a button press")
        table = {"description": "Real server test", "version": "1.0", "vtx_table": {
            "bands_list": [{"name": "LOWRACE", "letter": "L", "is_factory_band": False, "frequencies": [5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621]}],
            "powerlevels_list": [{"value": 14, "label": "25"}, {"value": 20, "label": "100"}]}}
        p.set_input_files("#vtxFile", [{"name": "t.json", "mimeType": "application/json", "buffer": json.dumps(table).encode()}])
        p.wait_for_selector("#vtxTableLine:has-text('loaded (Real server test)')", timeout=10000)
        self.assertTrue((self.base / "vtxtable.json").is_file())
        p.select_option("#vtxBand", "L")
        self.until(lambda: self.vtx.state()["freq"] == 5436, what="L3 by frequency")
        with p.expect_download() as dl:
            p.click("#vtxDownload")
        self.assertEqual(json.loads(pathlib.Path(dl.value.path()).read_text(encoding="utf-8"))["description"], "Real server test")
        p.click("#vtxDefault")
        self.confirm()
        p.wait_for_selector("#vtxTableLine:has-text('built in')", timeout=10000)
        self.assertFalse((self.base / "vtxtable.json").exists())
        self.tab("status")
        p.wait_for_selector("[data-f=vtx]:has-text('SmartAudio 2.1')", timeout=10000)
        self.tab("video")
        p.check('input[name="f-vtx-vtx"][value="off"]')
        p.click('form[data-form="vtx"] .savebar button[type=submit]')
        p.wait_for_selector("#vtxPill:has-text('Off')", timeout=10000)

    def test_settings_saved_and_refused(self):
        p = self.open("video")
        p.select_option("#f-video-tv", "pal")
        p.click('form[data-form="video"] .savebar button[type=submit]')
        p.wait_for_selector("#rebootBanner", state="visible")
        text = (self.base / "sticktime.txt").read_text()
        self.assertIn("tv = pal", text)
        self.assertIn("# this comment stays through every save", text)
        self.assertIn("video = av          # composite to the VTX", text)
        self.assertIn("my_custom_key = keep me", text)
        # the server's own validation, shown per field; nothing saved
        self.tab("sticks")
        p.fill("#f-sticks-wifi_module_ip", "10.0.0")           # the page doesn't check IPs: the server does
        p.click('form[data-form="sticks"] .savebar button[type=submit]')
        p.wait_for_function("document.getElementById('f-sticks-wifi_module_ip-err').textContent.length > 3", timeout=8000)
        self.assertNotIn("wifi_module_ip = 10.0.0", (self.base / "sticktime.txt").read_text())
        p.fill("#f-sticks-wifi_module_ip", "")
        # the channel order: a list of all 24 orders
        opts = p.eval_on_selector_all("#f-sticks-channel_order option", "els => els.map((e) => e.value)")
        self.assertEqual((len(opts), len(set(opts)), opts[:2]), (24, 24, ["AETR", "TAER"]))
        p.select_option("#f-sticks-channel_order", "TAER")
        p.click('form[data-form="sticks"] .savebar button[type=submit]')
        self.until(lambda: "channel_order = TAER" in (self.base / "sticktime.txt").read_text(), what="the save")
        p.wait_for_function("document.getElementById('f-sticks-channel_order').value === 'TAER'", timeout=8000)
        # a secret: set, then cleared
        self.tab("updates")
        gt = 'form[data-form="updates"] .field:has(#f-updates-github_token-help)'
        self.assertEqual(p.inner_text(f"{gt} .state"), "Set")
        p.click(f"{gt} button:has-text('Clear')")
        p.click('form[data-form="updates"] .savebar button[type=submit]')
        p.wait_for_selector(f"{gt} .state:has-text('Not set')", timeout=8000)
        self.assertNotIn("ghp_TOPSECRET42", (self.base / "sticktime.txt").read_text())
        # the Sound form saves by itself
        self.tab("sound")
        p.click("#f-sound-music")
        self.until(lambda: "music = on" in (self.base / "sticktime.txt").read_text(), what="music = on")

    def test_stick_mapping_and_source(self):
        """Stick mapping by Detect on the real server's stick stream, and the notice for a fixed stick source"""
        move = self.base / "state" / "stick_move"
        settings = lambda: (self.base / "sticktime.txt").read_text()   # noqa: E731
        p = self.open("sticks")
        p.wait_for_function("document.querySelectorAll('#srcCards .src-card').length === 2", timeout=10000)
        fld = 'form[data-form="sticks"] .field:has(#f-sticks-stick_channels-help)'
        sels = ["#f-sticks-stick_channels", "#f-sticks-stick_channels-ele", "#f-sticks-stick_channels-thr", "#f-sticks-stick_channels-rud"]
        try:
            p.click(f"{fld} button:has-text('Detect')")
            p.wait_for_selector("#detectDlg[open]")
            for ch, nxt in ((6, "ELEVATOR"), (2, "THROTTLE"), (3, "YAW"), (4, None)):
                move.write_text(str(ch))
                if nxt:
                    p.wait_for_selector(f"#detectAsk:has-text('{nxt}')", timeout=10000)
                else:
                    p.wait_for_selector("#detectDlg", state="hidden", timeout=10000)
        finally:
            move.unlink(missing_ok=True)
        self.assertEqual([p.input_value(s) for s in sels], ["6", "2", "3", "4"])
        p.click('form[data-form="sticks"] .savebar button[type=submit]')
        self.until(lambda: "stick_channels = 6 2 3 4" in settings(), what="the mapping saved")
        p.wait_for_function("document.querySelector('form[data-form=sticks] .savebar').hidden", timeout=8000)
        # back to the channel order: the key is cleared
        p.click(f"{fld} button:has-text('Use the channel order')")
        p.click('form[data-form="sticks"] .savebar button[type=submit]')
        self.until(lambda: "stick_channels = 6" not in settings(), what="the mapping cleared")
        # the form is filled again from the saved file: a change made before that would be undone
        p.wait_for_function("document.querySelector('form[data-form=sticks] .savebar').hidden", timeout=8000)
        # Stick source = Bluetooth while the WiFi joystick sends: the page says it is ignored; Use auto
        p.select_option("#f-sticks-input", "bluetooth")
        p.click('form[data-form="sticks"] .savebar button[type=submit]')
        self.until(lambda: "input = bluetooth" in settings(), what="input = bluetooth")
        p.wait_for_selector("#inputWarnSticks", state="visible", timeout=8000)
        self.assertIn("Stick source is set to Bluetooth: ELRS 10.0.0.1 (WiFi) is ignored.", p.inner_text("#inputWarnSticks"))
        p.click("#inputWarnSticks button:has-text('Use auto')")
        self.until(lambda: "input = auto" in settings(), what="input = auto")
        p.wait_for_selector("#inputWarnSticks", state="hidden", timeout=8000)

    def test_keys_scripts_reload(self):
        # the B&W and Lite editions as the image installs them: listed by their names, with what they are
        tools = self.base / "sd" / "SCRIPTS/TOOLS"
        write(tools / "StickTimeBW.lua", 'local toolName = "TNS|StickTime BW|TNE"\n-- StickTime BW v1.7.2\nreturn {}\n')
        write(tools / "StickTimeLite.lua", 'local toolName = "TNS|StickTime Lite|TNE"\n-- StickTime Lite v1.7.2\nreturn {}\n')
        self.addCleanup(lambda: [os.remove(tools / n) for n in ("StickTimeBW.lua", "StickTimeLite.lua")])
        p = self.open("game")
        p.wait_for_selector("#toolList label:has-text('StickTime Lite')")
        names = p.eval_on_selector_all("#toolList label", "els => els.map((e) => [e.querySelector('b').textContent, "
                                       "(e.querySelector('.tdesc') || {}).textContent || ''])")
        self.assertEqual([(n, d.split(":")[0]) for n, d in names],
                         [("Other", ""), ("StickTime", "Color edition"), ("StickTime BW", "B&W edition"), ("StickTime Lite", "Lite edition")])
        n = len(self.events())
        order = ["enter", "exit", "next", "prev", "inc", "dec", "nextpage", "prevpage", "menu", "up", "down"]
        for k in order:
            p.click(f'.keypad [data-key="{k}"]')
            p.wait_for_timeout(60)
        self.until(lambda: len([e for e in self.events()[n:] if e.startswith("key ")]) == len(order), what="the keys")
        self.assertEqual([e for e in self.events()[n:] if e.startswith("key ")], [f"key {KEYS[k]}" for k in order])
        p.click("#toolList label:has-text('Other')")
        self.until(lambda: "script = /SCRIPTS/TOOLS/Other.lua" in (self.base / "sticktime.txt").read_text(), what="the script")
        n = len(self.events())
        p.click("#p-game [data-action=reload-game]")
        self.until(lambda: any(e.startswith("cmd ") for e in self.events()[n:]), what="the reload command")

    def test_songs(self):
        p = self.open("sound")
        p.set_input_files("#songInput", [{"name": "Lap.mp3", "mimeType": "audio/mpeg", "buffer": b"ID3" + bytes(5000)},
                                         {"name": "Huge.ogg", "mimeType": "audio/ogg", "buffer": b"OggS" + bytes(MAX_UPLOAD + 10)},
                                         {"name": "aux.wav", "mimeType": "audio/wav", "buffer": b"RIFF"}])
        p.wait_for_selector("#uploads li .st.bad:has-text('too big')", timeout=10000)
        self.until(lambda: (self.base / "music" / "Lap.mp3").is_file() and (self.base / "music" / "aux_.wav").is_file(), what="the uploads")
        p.wait_for_function("[...document.querySelectorAll('#songList .name')].map((e) => e.textContent).join() === 'aux_.wav,Lap.mp3'")
        self.assertRegex(p.inner_text("#songFree"), r"^\d+ MB free$")
        p.click('#songList button[aria-label="Delete Lap.mp3"]')
        self.confirm()
        self.until(lambda: not (self.base / "music" / "Lap.mp3").exists(), what="the delete")

    def test_wifi_and_updates(self):
        p = self.open("wifi")
        p.wait_for_selector("#scanBtn")
        p.click("#scanBtn")
        p.wait_for_function("document.querySelectorAll('#netList li').length === 2", timeout=10000)
        p.click("#netList li:nth-child(1) button")
        p.fill("#joinPass", "hunter2hunter2")
        p.click("#joinGo")
        p.wait_for_selector("#joinDlg", state="hidden")
        self.until(lambda: any(c.startswith("sticktime-wifi apply") for c in self.calls()), what="the WiFi helper")
        self.assertIn('wifi_password = hunter2hunter2', (self.base / "sticktime.txt").read_text())
        self.tab("sticks")
        p.click('[data-modap="elrs"]')
        self.confirm()
        self.until(lambda: any(c.startswith("sticktime-wifi module-ap elrs") for c in self.calls()), what="module-ap")
        self.until(lambda: "wifi_module_ip = 10.0.0.1" in (self.base / "sticktime.txt").read_text())
        self.tab("updates")
        p.click("#updCheck")
        p.wait_for_selector("#updGame [data-u=latest]:has-text('1.7.3 (new)')", timeout=15000)
        self.assertIn("sticktime-update check", self.calls())
        self.assertEqual(p.eval_on_selector_all("#updGame .notes > *", "els => els.map((e) => e.tagName)"), ["H3", "UL"])

    # ---------------------------------------------------------------- the emulator page and pi-sd.js
    def test_editor_install_of_all_three_editions(self):
        # the editor's Install of the release the updater staged (editor/sdcard/), with sd_write = game:
        # the color, B&W and Lite editions are written, a file an older release left in a folder goes,
        # and the player's saves (the Lite's data.txt inside its folder, StickTimeBW.dat) stay
        mk = REPO.parent / "sticktime" / "tools" / "sd_manifest.py"
        if not self.editor or not mk.is_file():
            self.skipTest("../sticktime is not checked out next to this repo")
        import hashlib
        b, tools = self.base, self.base / "sd" / "SCRIPTS/TOOLS"
        staged = b / "editor" / "sdcard"
        subprocess.run([sys.executable, str(mk), str(staged)], check=True, capture_output=True)
        man = json.loads((staged / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(sorted(man["editions"]), ["bw", "color", "lite"])
        color = (tools / "StickTime.lua").read_bytes()
        saves = {"StickTimeLite/data.txt": b"lite save\x00\xff", "StickTimeBW.dat": b"BW save"}
        for rel, data in saves.items():
            write(tools / rel, data)
        write(tools / "StickTimeLite" / "old.lua", "-- left by an older release\n")

        def restore():
            shutil.rmtree(staged, ignore_errors=True)
            for n in ("StickTimeBW", "StickTimeLite"):
                shutil.rmtree(tools / n, ignore_errors=True)
            for n in ("StickTimeBW.lua", "StickTimeLite.lua", "StickTimeBW.dat", "StickTimeTracks/README.txt", "StickTimeGhosts/README.txt"):
                if (tools / n).exists():
                    os.remove(tools / n)
            write(tools / "StickTime.lua", color)
        self.addCleanup(restore)
        p = self.page
        p.goto(self.url + "editor/#install")
        p.wait_for_selector("#edEditions [data-ed]", timeout=30000)
        p.wait_for_function("!sim.editor.radio().busy", timeout=20000)
        self.assertIn(f"StickTime {man['version']} (staged on this Pi) onto StickTime Pi", p.inner_text("#edInstallHead"))
        for e in ("color", "bw", "lite"):
            p.check(f'#edEditions [data-ed="{e}"]')
        p.click("#edInstallGo")
        p.wait_for_function("!sim.editor.radio().busy && document.getElementById('rpStatus').textContent.includes('StickTime')", timeout=60000)
        st = p.inner_text("#rpStatus")
        self.assertIn(f"StickTime {man['version']} is on the Pi: Color, B&W and Lite.", st)
        self.assertIn("Removed StickTimeLite/old.lua", st)
        for f in man["files"]:
            disk = tools / f["path"]
            self.assertTrue(disk.is_file(), f["path"])
            self.assertEqual(hashlib.sha256(disk.read_bytes()).hexdigest(), f["sha256"], f["path"])
        for rel, data in saves.items():
            self.assertEqual((tools / rel).read_bytes(), data, f"{rel} untouched")
        self.assertFalse((tools / "StickTimeLite" / "old.lua").exists())
        self.until(lambda: "sd_dirty" in self.events(), what="the game told to reload")
        # the server itself refuses the player's data.txt to the page, whatever the page does
        res = p.evaluate("""async () => {
          const tools = await (await (await showDirectoryPicker()).getDirectoryHandle('SCRIPTS')).getDirectoryHandle('TOOLS');
          const lite = await tools.getDirectoryHandle('StickTimeLite');
          const name = async (f) => { try { await f(); return 'ok'; } catch (e) { return e.name; } };
          return [await name(async () => { const w = await (await lite.getFileHandle('data.txt')).createWritable(); await w.write('x'); await w.close(); }),
                  await name(() => lite.removeEntry('data.txt'))];
        }""")
        self.assertEqual(res, ["NoModificationAllowedError", "NoModificationAllowedError"])
        self.assertEqual((tools / "StickTimeLite" / "data.txt").read_bytes(), saves["StickTimeLite/data.txt"])

    def test_editor_on_the_real_server(self):
        if not self.editor:
            self.skipTest("../sticktime is not checked out next to this repo")
        p = self.page
        p.goto(self.url + "editor/#mine")
        p.wait_for_function("window.sim && window.sim.editor && sim.editor.radio().state === 'on' && !sim.editor.radio().busy", timeout=30000)
        r = p.evaluate("sim.editor.radio()")
        self.assertEqual((r["name"], r["at"]), ("StickTime Pi", "card"))
        rows = p.evaluate("[...document.querySelectorAll('#edSlotList tbody tr')].map((r) => r.cells[r.cells.length - 1].textContent.trim())")
        self.assertIn("Track One", rows[0])
        self.assertIn("Track Five", rows[4], "C5.TXT found as c5")
        # Send to radio into c2, then Remove it
        p.evaluate("location.hash = '#tracks'")
        p.wait_for_selector("#edSlot", state="visible")
        p.select_option("#edSlot", "2")
        p.click("#edSend")
        p.wait_for_function("!sim.editor.radio().busy && document.getElementById('edStatus').textContent.includes('c2.txt')", timeout=15000)
        disk = self.base / "sd" / SLOTS / "c2.txt"
        self.assertEqual(disk.read_text(encoding="utf-8"), p.evaluate("sim.editor.file()"))
        self.until(lambda: "sd_dirty" in self.events(), what="the game told to reload")
        p.evaluate("location.hash = '#mine'")
        p.wait_for_function("!sim.editor.radio().busy && document.querySelector('[data-sdel=\"r2\"]')", timeout=15000)
        p.click('[data-sdel="r2"]')
        p.wait_for_function("!sim.editor.radio().busy && document.getElementById('edMineStatus').textContent.includes('c2.txt')", timeout=15000)
        self.assertFalse(disk.exists())
        self.assertTrue((self.base / "sd" / "SCRIPTS/TOOLS/StickTime.dat").is_file())
        # what the server refuses comes back as the editor's message
        res = p.evaluate("""async () => {
          const root = await showDirectoryPicker(), out = {};
          const name = async (f) => { try { await f(); return 'ok'; } catch (e) { return e.name + ': ' + e.message; } };
          const tools = await (await root.getDirectoryHandle('SCRIPTS')).getDirectoryHandle('TOOLS');
          out.dat = await name(async () => { const w = await (await tools.getFileHandle('StickTime.dat')).createWritable(); await w.write('x'); await w.close(); });
          out.missing = await name(() => tools.getFileHandle('nope.lua'));
          out.kind = await name(() => tools.getDirectoryHandle('StickTime.lua'));
          out.bad = await name(() => tools.getFileHandle('a:b'));
          return out;
        }""")
        self.assertTrue(res["dat"].startswith("NoModificationAllowedError: This Pi lets the web page write only"), res["dat"])
        self.assertTrue(res["missing"].startswith("NotFoundError"), res["missing"])
        self.assertTrue(res["kind"].startswith("TypeMismatchError"), res["kind"])
        self.assertTrue(res["bad"].startswith("TypeError"), res["bad"])
        self.assertEqual((self.base / "sd" / "SCRIPTS/TOOLS/StickTime.dat").read_bytes(), b"\x00save")


if __name__ == "__main__":
    unittest.main(verbosity=2, argv=[a for a in sys.argv if not a.startswith("--exe=") and a != "--keep"])
