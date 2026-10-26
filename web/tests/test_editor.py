#!/usr/bin/env python3
"""StickTime's real emulator page (../sticktime/web/simulator.html) patched by tools/patch_editor.py
and served at /editor/ by the mock Pi (web/tests/mock_server.py) with web/pi-sd.js and /api/sd/,
in headless Chromium at http://sticktime.local (not a secure context, like the Pi: no native folder
picker, no crypto.subtle).

Covered: the page connects by itself to "StickTime Pi"; the #tracks/#mine/#open/#install/#emulator
links; My tracks lists the Pi's slots (any case on disk); Send to radio PUTs c1.txt (with the
X-StickTime header); Remove DELETEs a slot and leaves the ghost; the gallery comes from
/editor/tracks/index.json; Install reads the staged release (manifest from
sticktime/tools/sd_manifest.py), hashes with the SHA-256 fill-in, writes the game's files and never
touches StickTime.dat or c*.txt; the fill-in against hashlib; the API's refusals shown as the
editor's messages; the Pi unreachable -> Reconnect radio -> back; the radio wording in the Pi's.

    python web/tests/test_editor.py          (or: python -m pytest web/tests/test_editor.py)
    STP_BROWSER=firefox|webkit python web/tests/test_editor.py     (see browsers.py)
"""
import base64
import hashlib
import json
import os
import pathlib
import random
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
ST = REPO.parent / "sticktime"
SHOTS = REPO / "build" / "agent-webui" / "shots"
sys.dont_write_bytecode = True                  # (no __pycache__ in the repo)
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools"))
import browsers  # noqa: E402
import mock_server  # noqa: E402
import patch_editor  # noqa: E402
from playwright.sync_api import sync_playwright  # noqa: E402

HOST = "sticktime.local"                # the Pi's name, for requests made here
PAGE_HOST = browsers.HOST               # what the browser opens (see browsers.py)
SUFFIX = "" if browsers.NAME == "chromium" else "-" + browsers.NAME
TOOLS = "SCRIPTS/TOOLS"
SLOTS = TOOLS + "/StickTimeTracks"


def track(src, name):
    """a gallery track under another name (the id is the numbers': renaming keeps it valid)"""
    lines = (ST / "tracks" / src).read_text(encoding="utf-8").replace("\r\n", "\n").split("\n")
    return name + "\n" + lines[1] + "\n"


SLOT2 = track("multigp-2018-rq.txt", "Pi Slot Two")
SLOT3 = track("multigp-2019-gq.txt", "Pi Slot Three")
GALLERY = {"tracks": [{"file": "pi-gallery.txt", "name": "Pi Gallery UTT", "by": "Tester", "about": "From the Pi's gallery file.",
                       "text": track("multigp-2017-utt.txt", "Pi Gallery UTT"), "was": []}]}
SAVE = b"\x00\x01StickTime save\xff"
GHOST = b"ghost of c2"
OLD_GAME = b"-- StickTime v1.7.1\n-- an older game\nreturn { run = function() return 0 end }\n"
READY = "window.sim && window.sim.editor && window.sim.engine && window.sim.editor.radio().state === 'on' && !window.sim.editor.radio().busy"


def sha(b):
    return hashlib.sha256(b).hexdigest()


class EditorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (ST / "web" / "simulator.html").is_file():
            raise unittest.SkipTest("../sticktime is not checked out next to this repo")
        cls.tmp = pathlib.Path(tempfile.mkdtemp(prefix="sticktime-editor-test-"))
        subprocess.run([sys.executable, str(ST / "tools" / "sd_manifest.py"), str(cls.tmp / "sdcard")], check=True, capture_output=True)
        cls.manifest = json.loads((cls.tmp / "sdcard" / "manifest.json").read_text(encoding="utf-8"))
        html = patch_editor.patch((ST / "web" / "simulator.html").read_bytes().decode("utf-8"))
        cls.sd = cls.tmp / "sd"
        cls.sd.mkdir()
        cls.pi = mock_server.MockPi(sd=cls.sd, simulator_html=html, sdcard=cls.tmp / "sdcard",
                                    gallery=json.dumps(GALLERY).encode())
        cls.srv, cls.port = mock_server.serve(cls.pi)
        cls.base = f"http://{PAGE_HOST}:{cls.port}/editor/"
        cls.pw = sync_playwright().start()
        cls.br = browsers.launch(cls.pw)
        SHOTS.mkdir(parents=True, exist_ok=True)

    @classmethod
    def tearDownClass(cls):
        cls.br.close()
        cls.pw.stop()
        cls.srv.shutdown()
        cls.pi.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        # the Pi's SD folder afresh: an older game, a save, a track in c2, its ghost
        shutil.rmtree(self.sd)
        (self.sd / SLOTS).mkdir(parents=True)
        (self.sd / TOOLS / "StickTimeGhosts").mkdir()
        (self.sd / TOOLS / "StickTime.lua").write_bytes(OLD_GAME)
        (self.sd / TOOLS / "StickTime.dat").write_bytes(SAVE)
        (self.sd / SLOTS / "c2.txt").write_text(SLOT2, encoding="utf-8", newline="\n")
        (self.sd / TOOLS / "StickTimeGhosts" / "c2.txt").write_bytes(GHOST)
        self.pi.file_values.pop("sd_write", None)
        self.pi.sdcard = self.tmp / "sdcard"
        self.pi.fail.clear()
        with self.pi.lock:
            self.pi.requests.clear()
        self.errors, self.dialogs, self.answer = [], [], True
        self.ctx = self.br.new_context(viewport={"width": 1400, "height": 1000})
        self.page = self.ctx.new_page()
        self.page.on("pageerror", lambda e: self.errors.append(str(e)))
        self.page.on("dialog", self.on_dialog)

    def tearDown(self):
        self.ctx.close()
        self.assertEqual(self.errors, [], "no script errors on the page")
        for r in self.pi.requests:
            if r["method"] != "GET":
                self.assertEqual(r["headers"].get("x-sticktime"), "1", f"{r['method']} {r['path']} without X-StickTime")

    def on_dialog(self, d):
        self.dialogs.append(d.message)
        d.accept() if self.answer else d.dismiss()

    # helpers
    def open(self, view="#tracks"):
        self.page.goto(self.base + view)
        self.page.wait_for_function(READY, timeout=30000)
        return self.page.evaluate("sim.editor.radio()")

    def idle(self):
        self.page.wait_for_function("!sim.editor.radio().busy", timeout=20000)
        return self.page.evaluate("sim.editor.radio()")

    def mutations(self):
        return [(r["method"], r["path"], r["status"]) for r in self.pi.requests if r["method"] != "GET"]

    def rows(self):
        return self.page.evaluate("[...document.querySelectorAll('#edSlotList tbody tr')].map((r) => [...r.cells].map((c) => c.textContent.replace(/\\s+/g, ' ').trim()))")

    # ---------------------------------------------------------------- connecting
    def test_connects_by_itself(self):
        r = self.open("#tracks")
        self.assertEqual((r["ok"], r["state"], r["at"], r["name"], r["game"]), (True, "on", "card", "StickTime Pi", "sdcard/"))
        p = self.page
        self.assertEqual(p.evaluate("window.isSecureContext"), browsers.SECURE, "served like the Pi: not a secure context")
        self.assertTrue(p.is_visible("#editor"), "#tracks opens the track editor")
        self.assertIn("StickTime Pi", p.inner_text("#edRadioLineText"))
        self.assertEqual(p.inner_text("#edSendText"), "Send to radio")
        self.assertEqual(p.evaluate("typeof crypto.subtle.digest"), "function", "SHA-256 filled in where crypto.subtle is missing")
        self.assertIn(("GET", "/editor/pi-sd.js"), [(x["method"], x["path"]) for x in self.pi.requests])
        self.assertFalse(any("fonts.g" in x["path"] for x in self.pi.requests))
        p.screenshot(path=str(SHOTS / f"editor-tracks{SUFFIX}.png"))

    def test_views(self):
        self.open("#emulator")
        p = self.page
        self.assertTrue(p.is_hidden("#editor") and p.is_hidden("#radioPanel"), "#emulator: just the emulator (connected all the same)")
        p.evaluate("location.hash = '#mine'")
        p.wait_for_selector("#edMine", state="visible")
        self.assertTrue(p.is_visible("#editor"))
        p.evaluate("location.hash = '#open'")
        p.wait_for_selector("#edGallery", state="visible")
        p.evaluate("location.hash = '#install'")
        p.wait_for_selector("#radioPanel", state="visible")
        self.assertIn("StickTime Pi", p.inner_text("#rpTitle"))
        self.assertNotIn("over USB", p.inner_text("#rpTitle"), "the Install panel's title in the Pi's words")
        # each link makes its whole view, whatever the last one left open
        p.evaluate("location.hash = '#tracks'")
        p.wait_for_selector("#radioPanel", state="hidden")
        p.wait_for_selector("#edGrid", state="visible")
        self.assertTrue(p.is_hidden("#edGallery") and p.is_hidden("#edMine") and p.is_visible("#edSlot"))
        p.evaluate("location.hash = '#mine'")
        p.wait_for_selector("#edMine", state="visible")
        p.evaluate("location.hash = '#nonsense'")
        p.wait_for_selector("#edGrid", state="visible")
        self.assertTrue(p.is_hidden("#edMine"), "an unknown view is #tracks")
        p.evaluate("location.hash = '#open'")
        p.wait_for_selector("#edGallery", state="visible")
        p.evaluate("location.hash = '#emulator'")
        p.wait_for_selector("#editor", state="hidden")
        self.assertEqual(p.evaluate("sim.editor.radio().state"), "on", "still connected")

    def test_redirect_and_files(self):
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}/editor", headers={"Host": HOST})

        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *a, **k):
                return None
        try:
            urllib.request.build_opener(NoRedirect).open(req)
            self.fail("no redirect")
        except urllib.error.HTTPError as e:
            self.assertEqual((e.code, e.headers["Location"]), (301, "/editor/"))
        html = urllib.request.urlopen(f"http://127.0.0.1:{self.port}/editor/").read().decode()
        self.assertEqual(html.count('<script src="pi-sd.js"></script>'), 1)
        self.assertLess(html.index("pi-sd.js"), html.index("</head>"))
        self.assertNotIn("fonts.googleapis.com", html)
        self.assertIn("const GAME_FILES = 'sdcard/';", html, "Install stays on")

    # ---------------------------------------------------------------- tracks
    def test_my_tracks_lists_the_pi(self):
        (self.sd / SLOTS / "C3.TXT").write_text(SLOT3, encoding="utf-8")         # FAT: any case
        self.open("#mine")
        rows = self.rows()
        self.assertEqual(len(rows), 8)
        self.assertIn("Pi Slot Two", rows[1][-1], f"c2 on the Pi: {rows[1]}")
        self.assertIn("Pi Slot Three", rows[2][-1], f"C3.TXT found as c3: {rows[2]}")
        self.assertEqual(rows[0][-1], "—")
        self.page.screenshot(path=str(SHOTS / f"editor-mine{SUFFIX}.png"))

    def test_send_to_radio(self):
        self.open("#tracks")
        p = self.page
        p.select_option("#edSlot", "1")
        reloads = self.pi.reloads
        p.click("#edSend")
        self.idle()
        text = p.evaluate("sim.editor.file()")
        self.assertEqual((self.sd / SLOTS / "c1.txt").read_text(encoding="utf-8"), text, "c1.txt is the editor's file")
        self.assertIn(("PUT", "/api/sd/SCRIPTS/TOOLS/StickTimeTracks/c1.txt", 200), self.mutations())
        # (StickTime 1.10.0's built-in tracks have scenery: its copy is a format 3 track, which the
        # card's older game doesn't list, and the editor says so)
        sent = "Sent to the Pi as track 9 (c1.txt). "
        self.assertIn(p.inner_text("#edStatus"), (sent + "The game on the Pi has it in its track list once it is back in its menus.",
                      sent + "\"My Meadow\" is a track with scenery: StickTime 1.7.1 on the card doesn't list it, 1.10.0 and later do. Install updates it."))
        self.assertGreater(self.pi.reloads, reloads, "the game reloads after a slot changes")
        self.assertIn("c1:", p.inner_text("#edRadioLineText"))
        # the same track into c2 (Pi Slot Two there): asked first, in the Pi's words; No keeps it
        self.answer = False
        p.select_option("#edSlot", "2")
        self.dialogs.clear()
        p.click("#edSend")
        self.idle()
        self.assertEqual((self.sd / SLOTS / "c2.txt").read_text(encoding="utf-8"), SLOT2)

    def test_send_replaces_after_asking(self):
        self.open("#tracks")
        p = self.page
        # a track that is on the Pi nowhere: shuffle the seed
        p.click("#edShuffle")
        p.select_option("#edSlot", "2")
        self.dialogs.clear()
        p.click("#edSend")
        self.idle()
        self.assertTrue(self.dialogs and "on the Pi holds" in self.dialogs[0] and "radio" not in self.dialogs[0], self.dialogs)
        self.assertEqual((self.sd / SLOTS / "c2.txt").read_text(encoding="utf-8"), p.evaluate("sim.editor.file()"))

    def test_remove_from_pi(self):
        self.open("#mine")
        p = self.page
        p.click('[data-sdel="r2"]')
        self.idle()
        self.assertEqual(self.dialogs, ['Remove "Pi Slot Two" (track 10) from the Pi? Its best times stay in the Pi\'s save until a different track is put in that slot.'])
        self.assertFalse((self.sd / SLOTS / "c2.txt").exists())
        self.assertIn(("DELETE", "/api/sd/SCRIPTS/TOOLS/StickTimeTracks/c2.txt", 204), self.mutations())
        self.assertEqual((self.sd / TOOLS / "StickTimeGhosts" / "c2.txt").read_bytes(), GHOST, "the ghost stays")
        self.assertEqual(p.inner_text("#edMineStatus"), "Removed c2.txt from the Pi. The game on the Pi picks it up once it is back in its menus.")

    def test_copy_all_to_pi(self):
        self.open("#mine")
        p = self.page
        p.evaluate(f"sim.writeSD('/SCRIPTS/TOOLS/StickTimeTracks/c4.txt', {SLOT3!r})")
        p.click("#edMineBtn")
        p.click("#edMineBtn")
        self.idle()
        p.click("#edAllTo")
        self.idle()
        self.assertEqual((self.sd / SLOTS / "c4.txt").read_text(encoding="utf-8"), SLOT3)
        self.assertIn("Sent c4 to the Pi.", p.inner_text("#edMineStatus"))

    def test_gallery_from_the_pi(self):
        self.open("#open")
        p = self.page
        p.wait_for_function("document.getElementById('edCards').textContent.includes('Pi Gallery UTT')", timeout=10000)
        self.assertIn(("GET", "/editor/tracks/index.json"), [(x["method"], x["path"]) for x in self.pi.requests])
        p.screenshot(path=str(SHOTS / f"editor-gallery{SUFFIX}.png"))

    def test_a_folder_named_like_a_slot(self):
        (self.sd / SLOTS / "c5.txt").mkdir()
        self.open("#mine")
        rows = self.rows()
        self.assertEqual(rows[4][-1], "—", "a folder isn't a track")

    # ---------------------------------------------------------------- install
    def test_install_from_staged_release(self):
        # the Lite's save lives in its game folder, beside a file an older release left there; a B&W save
        (self.sd / TOOLS / "StickTimeLite").mkdir()
        (self.sd / TOOLS / "StickTimeLite" / "data.txt").write_bytes(b"lite save\x00\xff")
        (self.sd / TOOLS / "StickTimeLite" / "old.lua").write_bytes(b"-- an older release's\n")
        (self.sd / TOOLS / "StickTimeBW.dat").write_bytes(b"BW save")
        self.open("#install")
        p = self.page
        p.wait_for_selector("#edEditions [data-ed]", timeout=20000)
        self.idle()
        self.assertIn(f"StickTime {self.manifest['version']} (staged on this Pi) onto StickTime Pi", p.inner_text("#edInstallHead"))
        self.assertIn("will be updated (1.7.1 on the Pi)", p.inner_text("#edEditions"))
        p.screenshot(path=str(SHOTS / f"editor-install{SUFFIX}.png"))
        before = {n: ((self.sd / n).read_bytes(), (self.sd / n).stat().st_mtime_ns) for n in
                  (TOOLS + "/StickTime.dat", SLOTS + "/c2.txt", TOOLS + "/StickTimeGhosts/c2.txt",
                   TOOLS + "/StickTimeLite/data.txt", TOOLS + "/StickTimeBW.dat")}
        p.check('#edEditions [data-ed="bw"]')
        p.check('#edEditions [data-ed="lite"]')
        with self.pi.lock:
            self.pi.requests.clear()
        p.click("#edInstallGo")
        self.idle()
        st = p.inner_text("#rpStatus")
        self.assertIn(f"StickTime {self.manifest['version']} is on the Pi: Color, B&W and Lite.", st)
        self.assertIn("Removed StickTimeLite/old.lua.", st)
        self.assertFalse((self.sd / TOOLS / "StickTimeLite" / "old.lua").exists())
        self.assertNotIn("Eject", st)
        for f in self.manifest["files"]:
            disk = self.sd / self.manifest["base"] / f["path"]
            self.assertTrue(disk.is_file(), f["path"])
            self.assertEqual(sha(disk.read_bytes()), f["sha256"], f["path"])
        for n, (data, mt) in before.items():
            self.assertEqual(((self.sd / n).read_bytes(), (self.sd / n).stat().st_mtime_ns), (data, mt), f"{n} untouched")
        for m, path, status in self.mutations():
            self.assertNotRegex(path.lower(), r"(\.dat|/c\d+\.txt|data\.txt)$", f"Install never writes the player's files: {m} {path}")
            self.assertLess(status, 400, f"{m} {path} -> {status}")
        # the staged files were fetched and hashed in the page (no crypto.subtle on http)
        self.assertTrue(any(x["path"] == "/editor/sdcard/manifest.json" for x in self.pi.requests))
        p.click("#edRefresh")
        self.idle()
        p.wait_for_function("document.getElementById('edEditions').innerText.split('up to date').length - 1 === 3", timeout=20000)

    def test_install_nothing_staged(self):
        self.pi.sdcard = None
        self.open("#install")
        p = self.page
        p.wait_for_function("document.getElementById('rpInstWait').textContent.includes('staged')", timeout=20000)
        self.assertIn("No StickTime release is staged on this Pi", p.inner_text("#rpInstWait"))
        self.assertTrue(p.is_hidden("#edInstallPanel"))

    def test_install_refused_by_policy(self):
        self.pi.file_values["sd_write"] = "tracks"
        self.open("#install")
        p = self.page
        p.wait_for_selector("#edEditions [data-ed]", timeout=20000)
        self.idle()
        p.click("#edInstallGo")
        self.idle()
        self.assertEqual(p.inner_text("#rpStatus"), "The StickTime Pi: This Pi lets the track editor write only its track slots (c1.txt to c8.txt).")
        self.assertEqual((self.sd / TOOLS / "StickTime.lua").read_bytes(), OLD_GAME)

    # ---------------------------------------------------------------- errors
    def test_refusal_shows_as_editor_message(self):
        self.pi.file_values["sd_write"] = "off"
        self.open("#tracks")
        p = self.page
        p.select_option("#edSlot", "1")
        p.click("#edSend")
        self.idle()
        self.assertEqual(p.inner_text("#edStatus"), "The StickTime Pi: This Pi doesn't let the web page change its SD card (sd_write = off in sticktime.txt).")
        self.assertFalse((self.sd / SLOTS / "c1.txt").exists())
        self.assertEqual(self.page.evaluate("sim.editor.radio().state"), "on", "a refusal keeps the connection")

    def test_disk_full_and_bad_names(self):
        self.open("#tracks")
        p = self.page
        self.pi.fail["/api/sd/SCRIPTS/TOOLS/StickTimeTracks/c1.txt"] = (507, "QuotaExceededError", "The SD card is full.")
        p.select_option("#edSlot", "1")
        p.click("#edSend")
        self.idle()
        self.assertEqual(p.inner_text("#edStatus"), "The StickTime Pi: The SD card is full.")
        # the shim itself, against the API: names and kinds as Chrome reports them
        out = p.evaluate("""async () => {
          const root = await showDirectoryPicker(), r = {};
          const name = async (f) => { try { await f(); return 'ok'; } catch (e) { return e.name; } };
          r.dotdot = await name(() => root.getDirectoryHandle('..'));
          r.slash = await name(() => root.getFileHandle('a/b'));
          r.missing = await name(() => root.getFileHandle('nope.txt'));
          r.kind = await name(() => root.getFileHandle('SCRIPTS'));
          r.trailingDot = await name(() => root.getFileHandle('c1.txt.', { create: true }));
          r.notEmpty = await name(() => root.removeEntry('SCRIPTS'));
          r.recursive = await name(() => root.removeEntry('SCRIPTS', { recursive: true }));
          r.tempName = await name(() => root.getFileHandle('.st-tmp-1'));
          r.empty = await name(() => root.getFileHandle(''));
          r.list = [];
          for await (const h of root.values()) r.list.push(h.kind + ':' + h.name);
          const tools = await (await root.getDirectoryHandle('scripts')).getDirectoryHandle('TOOLS');   // any case
          r.same = await tools.isSameEntry(await (await root.getDirectoryHandle('SCRIPTS')).getDirectoryHandle('tools'));
          r.resolved = await root.resolve(tools);
          r.keys = [];
          for await (const k of tools.keys()) r.keys.push(k);
          // writes: seek, write past the end (zeros between), truncate, then one PUT on close
          const f = await (await tools.getDirectoryHandle('StickTimeTracks')).getFileHandle('c6.txt', { create: true });
          const w = await f.createWritable();
          await w.write('abc');
          await w.write({ type: 'write', position: 5, data: new Uint8Array([65]) });
          await w.seek(1);
          await w.write(new Blob(['Z']));
          await w.truncate(7);
          await w.close();
          r.closedWrite = await name(() => w.write('x'));
          const back = new Uint8Array(await (await f.getFile()).arrayBuffer());
          r.bytes = Array.from(back);
          const k = await f.createWritable({ keepExistingData: true });
          await k.seek(6);
          await k.write('!');
          await k.abort();                                  // nothing sent
          r.afterAbort = (await f.getFile()).size;
          return r;
        }""")
        self.assertEqual(out, {"dotdot": "TypeError", "slash": "TypeError", "missing": "NotFoundError", "kind": "TypeMismatchError",
                               "trailingDot": "TypeError", "notEmpty": "InvalidModificationError", "recursive": "NoModificationAllowedError",
                               "tempName": "TypeError", "empty": "TypeError", "list": ["directory:SCRIPTS"], "same": True,
                               "resolved": ["scripts", "TOOLS"], "keys": ["StickTime.dat", "StickTime.lua", "StickTimeGhosts", "StickTimeTracks"],
                               "closedWrite": "TypeError", "bytes": [97, 90, 99, 0, 0, 65, 0], "afterAbort": 7})
        self.assertTrue((self.sd / TOOLS / "StickTime.lua").is_file(), "the refused recursive delete removed nothing")
        puts = [m for m in self.mutations() if m[0] == "PUT" and m[1].endswith("/c6.txt")]
        self.assertEqual(len(puts), 1, "one PUT per close(), none for abort()")

    def test_unreachable_then_reconnect(self):
        self.open("#tracks")
        p = self.page
        p.route("**/api/sd/**", lambda route: route.abort())
        p.select_option("#edSlot", "1")
        p.click("#edSend")
        r = self.idle()
        self.assertEqual(r["state"], "ask")
        self.assertEqual(p.inner_text("#edSendText"), "Reconnect radio")
        self.assertEqual(p.inner_text("#edStatus"), "The StickTime Pi doesn't answer: is it on, and this device on its WiFi? Then Reconnect radio.")
        p.unroute("**/api/sd/**")
        p.click("#edSend")                                    # Reconnect radio
        r = self.idle()
        self.assertEqual(r["state"], "on")
        p.click("#edSend")
        self.idle()
        self.assertTrue((self.sd / SLOTS / "c1.txt").is_file())

    def test_empty_sd_is_not_a_card(self):
        shutil.rmtree(self.sd / "SCRIPTS")
        self.page.goto(self.base + "#tracks")
        self.page.wait_for_function("window.sim && window.sim.editor && window.sim.engine", timeout=30000)
        # connecting by itself reports in the hidden Install panel: the editor's own line says it too
        self.page.wait_for_function("document.getElementById('edStatus').textContent.includes('SCRIPTS')", timeout=20000)
        self.assertEqual(self.page.evaluate("sim.editor.radio().state"), "off")
        st = self.page.inner_text("#edStatus")
        self.assertIn("The StickTime Pi's SD folder has no SCRIPTS folder", st)
        self.assertNotIn("radio", st.replace("Connect radio", ""))
        self.assertIn("Connect radio", self.page.inner_text("#edSendText"))

    def test_busy_server_is_retried(self):
        self.open("#tracks")
        p = self.page
        self.pi.fail["/api/sd/SCRIPTS/TOOLS/StickTimeTracks/c3.txt"] = (503, "Busy", "Too many clients.", 2)   # twice, then fine
        p.select_option("#edSlot", "3")
        p.click("#edSend")
        self.idle()
        self.assertEqual((self.sd / SLOTS / "c3.txt").read_text(encoding="utf-8"), p.evaluate("sim.editor.file()"))
        self.assertIn("Sent to the Pi as track 11", p.inner_text("#edStatus"))

    def test_gallery_missing_or_broken(self):
        for body in (None, b"{not json", json.dumps({"tracks": []}).encode()):
            self.pi.gallery = body
            self.page.goto("about:blank")                       # a new load: the page asks for the gallery once per load
            self.open("#open")
            p = self.page
            p.wait_for_function("document.getElementById('edCards').children.length > 3", timeout=10000)
            cards = p.inner_text("#edCards")
            self.assertNotIn("Pi Gallery UTT", cards, "the page's own gallery")
            self.assertIn("MultiGP", cards)
        self.pi.gallery = json.dumps(GALLERY).encode()

    def test_api_guards(self):
        def call(method, path, headers, body=b""):
            req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", data=body if method != "GET" else None, method=method, headers=headers)
            try:
                with urllib.request.urlopen(req) as r:
                    return r.status, r.read()
            except urllib.error.HTTPError as e:
                return e.code, json.loads(e.read() or b"{}")
        slot = "/api/sd/SCRIPTS/TOOLS/StickTimeTracks/c7.txt"
        st, j = call("PUT", slot, {"Host": HOST}, b"x")
        self.assertEqual((st, j["error"]), (403, "Forbidden"), "a PUT without X-StickTime")
        st, j = call("PUT", slot, {"Host": HOST, "X-StickTime": "1", "Origin": "http://evil.example"}, b"x")
        self.assertEqual((st, j["error"]), (403, "Forbidden"), "a PUT from another site")
        st, j = call("GET", "/api/sd/?list", {"Host": "evil.example"})
        self.assertEqual(st, 421, "DNS rebinding: a foreign Host")
        st, _ = call("PUT", slot, {"Host": HOST + ":80", "X-StickTime": "1", "Origin": f"http://{HOST}:80"}, b"x")
        self.assertEqual(st, 200)
        self.assertEqual((self.sd / SLOTS / "c7.txt").read_bytes(), b"x")
        with self.pi.lock:
            self.pi.requests.clear()                            # (tearDown checks the page's own requests)

    # ---------------------------------------------------------------- SHA-256 fill-in
    def test_sha256_fill_in(self):
        if browsers.SECURE:                     # (WebKit at 127.0.0.1 has its own: hide it, so the fill-in is the one tested)
            self.page.add_init_script("Object.defineProperty(Crypto.prototype, 'subtle', { get() { return undefined; }, configurable: true });")
        self.open("#emulator")
        self.assertTrue(self.page.evaluate("Object.prototype.hasOwnProperty.call(crypto, 'subtle')"), "pi-sd.js's own crypto.subtle")
        rnd = random.Random(7)
        cases = [b"", b"abc", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", b"a" * 1000000]
        cases += [bytes(rnd.randrange(256) for _ in range(n)) for n in (1, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 1000, 65537)]
        b64 = [base64.b64encode(c).decode() for c in cases]
        got = self.page.evaluate("""async (list) => {
          const hex = (b) => Array.from(new Uint8Array(b), (x) => x.toString(16).padStart(2, '0')).join('');
          const out = [];
          for (const s of list) {
            const bin = atob(s), u = new Uint8Array(bin.length);
            for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
            out.push(hex(await crypto.subtle.digest('SHA-256', u.buffer)));
          }
          // views with an offset, a DataView, the algorithm as an object, and what it refuses
          const big = new Uint8Array([9, 9, 97, 98, 99, 9]);
          out.push(hex(await crypto.subtle.digest('sha-256', big.subarray(2, 5))));
          out.push(hex(await crypto.subtle.digest({ name: 'SHA-256' }, new DataView(big.buffer, 2, 3))));
          out.push(await crypto.subtle.digest('SHA-1', big).then(() => 'ok', (e) => e.name));
          out.push(await crypto.subtle.digest('SHA-256', 'abc').then(() => 'ok', (e) => e.name));
          return out;
        }""", b64)
        want = [sha(c) for c in cases] + [sha(b"abc"), sha(b"abc"), "NotSupportedError", "TypeError"]
        self.assertEqual(got, want)


if __name__ == "__main__":
    unittest.main(verbosity=2)
