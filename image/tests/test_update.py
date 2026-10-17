"""Tests of sticktime-update (image/rootfs/usr/lib/sticktime/) against fake_github.py.

    python image/tests/test_update.py            (from the repository root; -v for names)

Pure functions are tested in-process; commands run the real script in a subprocess against a
temporary "SD card" (base folder, settings, /run folder), as on the Pi. A power cut is an
os._exit() at a named step (STICKTIME_UPDATE_CRASH_AT), followed by 'boot'. After every scenario
the player's saves, tracks and ghosts must be byte for byte what they were.

The real StickTime-1.7.2-sdcard.zip is used as the model when it is in
build/agent-updater/fixture/ (gh release download v1.7.2 -R juricabi/sticktime); otherwise the
zips are synthetic with the same shape."""
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
from unittest import mock
import warnings
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
LIB = os.path.join(REPO, "image", "rootfs", "usr", "lib", "sticktime")
SCRIPT = os.path.join(LIB, "sticktime-update")
PATCHER = os.path.join(REPO, "tools", "patch_editor.py")
FIXTURE = os.path.join(REPO, "build", "agent-updater", "fixture", "StickTime-1.7.2-sdcard.zip")
sys.path.insert(0, LIB)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools"))

from fake_github import FakeGitHub  # noqa: E402
from stupdate import common, gamezip, net, app as appmod, game as gamemod, system as sysmod  # noqa: E402
import make_app_package  # noqa: E402

GAME = "juricabi/sticktime"
APPR = "juricabi/sticktime-pi"
TOKEN = "github_pat_TESTTOKEN0123456789"
SCHEMA = {"state", "message", "progress", "game", "app", "checked_at", "error"}


# ---------------------------------------------------------------- builders

def _zinfo(name, mode):
    zi = zipfile.ZipInfo(name, (2026, 10, 14, 9, 23, 16))
    zi.create_system = 3
    zi.external_attr = mode << 16
    zi.compress_type = zipfile.ZIP_STORED if name.endswith("/") else zipfile.ZIP_DEFLATED
    return zi


def _synthetic_entries():
    pad = lambda n: ("-- " + "x" * 70 + "\n") * n
    lua = ('local toolName = "TNS|StickTime|TNE"\n--[[ ======\n  StickTime v1.7.2  -  a real 3D FPV quad simulator\n]]\n'
           + pad(200) + "return { run = function() return 0 end }\n").encode()
    bw = ('local toolName = "TNS|StickTime BW|TNE"\n--[[\n  StickTime BW v1.7.2  -  loader\n]]\n' + pad(5)).encode()
    lite = ('local toolName = "TNS|StickTime Lite|TNE"\n--[[\n  StickTime Lite v1.7.2  -  loader\n]]\n' + pad(5)).encode()
    luac = b"\x1bLua\x53\x00\x19\x93\r\n\x1a\n" + bytes(range(256)) * 4
    return [
        ("SCRIPTS/", None), ("SCRIPTS/TOOLS/", None), ("SCRIPTS/TOOLS/StickTimeLite/", None),
        ("SCRIPTS/TOOLS/StickTimeLite/core.lua", pad(30).encode()),
        ("SCRIPTS/TOOLS/StickTimeLite/game.luac", luac),
        ("SCRIPTS/TOOLS/StickTimeLite/t1.txt", b"track one\n"),
        ("SCRIPTS/TOOLS/StickTimeTracks/", None),
        ("SCRIPTS/TOOLS/StickTimeTracks/README.txt", b"Your own tracks: c1.txt to c8.txt\n"),
        ("SCRIPTS/TOOLS/StickTimeLite.lua", lite),
        ("SCRIPTS/TOOLS/StickTimeBW/", None),
        ("SCRIPTS/TOOLS/StickTimeBW/core.lua", pad(40).encode()),
        ("SCRIPTS/TOOLS/StickTimeBW/game.luac", luac[::-1]),
        ("SCRIPTS/TOOLS/StickTimeGhosts/", None),
        ("SCRIPTS/TOOLS/StickTimeGhosts/README.txt", b"Ghosts of your best laps.\n"),
        ("SCRIPTS/TOOLS/StickTimeBW.lua", bw),
        ("SCRIPTS/TOOLS/StickTime.lua", lua),
    ]


def _model_entries():
    if os.path.isfile(FIXTURE):
        with zipfile.ZipFile(FIXTURE) as z:
            return [(i.filename, None if i.filename.endswith("/") else z.read(i)) for i in z.infolist()]
    return _synthetic_entries()


MODEL = _model_entries()


def game_zip(version, add=None, drop=(), change=None, raw_entries=None):
    """a release zip like StickTime's for version (the loaders' headers say it); add {name: bytes}
    (exact zip names), drop names, change {name: fn(bytes) -> bytes}"""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data in (raw_entries or MODEL):
            if name in drop:
                continue
            if data is None:
                z.writestr(_zinfo(name, 0o40755), b"")
                continue
            if name.endswith((".lua",)) and b"v1.7.2" in data[:600]:
                data = data.replace(b"v1.7.2", ("v" + version).encode(), 1)
            if change and name in change:
                data = change[name](data)
            z.writestr(_zinfo(name, 0o100644), data)
        for name, data in (add or {}).items():
            z.writestr(_zinfo(name, 0o100644), data)
    return buf.getvalue()


def zip_raw_name(name, data=b"x"):
    """the model zip plus an entry stored under exactly name (ZipInfo() would fix up a backslash or a NUL)"""
    buf = io.BytesIO(game_zip("1.7.2"))
    with zipfile.ZipFile(buf, "a") as z:
        zi = _zinfo("SCRIPTS/TOOLS/placeholder.txt", 0o100644)
        zi.filename = name
        z.writestr(zi, data)
    return buf.getvalue()


def zip_member(zbytes, name):
    with zipfile.ZipFile(io.BytesIO(zbytes)) as z:
        return z.read(name)


def sim_html(version, extra=""):
    return ("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<title>StickTime</title>\n"
            "<link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">\n"
            "<link rel=\"stylesheet\" href=\"https://fonts.googleapis.com/css2?family=Chakra+Petch&display=swap\">\n"
            "<link rel=\"apple-touch-icon\" href=\"apple-touch-icon.png\">\n</head>\n<body>\n"
            "<button id=\"tracksBtn\"></button><button id=\"edConnect\"></button><button id=\"edMineBtn\"></button>"
            "<button id=\"edGalBtn\"></button><button id=\"radioBtn\"></button>\n"
            "<script type=\"text/plain\" id=\"lua-color\">-- StickTime v%s</script>\n"
            "<script>const GAME_FILES = 'sdcard/'; const GALLERY_FILES = 'tracks/';"
            " if (window.showDirectoryPicker) {}%s</script>\n</body>\n</html>\n" % (version, extra)).encode()


def patched(html):
    import importlib.util
    spec = importlib.util.spec_from_file_location("pe_test", PATCHER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.patch(html.decode()).encode()


def gallery(n=3, tag="a"):
    return json.dumps({"tracks": [{"file": "t%d.txt" % i, "name": "Track %s%d" % (tag, i), "by": "Tester",
                                   "text": "Track %s%d\n1 %d 42 10\n" % (tag, i, 100000 + i), "was": [i + 1]}
                                  for i in range(n)]}, indent=1).encode()


def fake_elf(machine=183, cls=2, etype=2, payload=b"sticktime-pi"):
    h = b"\x7fELF" + bytes([cls, 1, 1, 0]) + b"\0" * 8 + etype.to_bytes(2, "little") + machine.to_bytes(2, "little")
    return h + b"\0" * (64 - len(h)) + payload


def app_tgz(version, web=None, sums_override=None, extra=None, drop=(), elf=None, compat=None, tweak=None,
            members=None):
    files = {"VERSION": (version + "\n").encode(), "bin/sticktime-pi": elf or fake_elf(payload=b"host " + version.encode()),
             "web/index.html": ("<!doctype html><title>StickTime Pi %s</title>" % version).encode(),
             "web/app.js": ("console.log('app %s');" % version).encode(),
             "web/pi-sd.js": ("/* pi-sd %s */" % version).encode()}
    if web:
        files.update(web)
    if compat is not None:
        files["compat.json"] = json.dumps(compat).encode()
    if extra:
        files.update(extra)
    for d in drop:
        files.pop(d, None)
    sums = "".join("%s  %s\n" % (hashlib.sha256(b).hexdigest(), p) for p, b in sorted(files.items()) if p != "SHA256SUMS")
    files["SHA256SUMS"] = sums_override if sums_override is not None else sums.encode()
    if tweak:
        tweak(files)
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tf:
        for d in ("bin", "web"):
            ti = tarfile.TarInfo(d)
            ti.type = tarfile.DIRTYPE
            ti.mode = 0o755
            tf.addfile(ti)
        for p, b in sorted(files.items()):
            ti = tarfile.TarInfo(p)
            ti.size = len(b)
            ti.mode = 0o755 if p.startswith("bin/") else 0o644
            tf.addfile(ti, io.BytesIO(b))
        for ti, b in members or []:
            tf.addfile(ti, io.BytesIO(b) if b is not None else None)
    return buf.getvalue()


def body(version):
    return ("StickTime is a 3D FPV sim.\n\n### New in %s\n- **Better gates** for %s.\n\nEarlier changes: "
            "[all releases](https://github.com/juricabi/sticktime/releases).\n\n**Faster firmware**" % (version, version))


PLAYER = {
    "StickTime.dat": b"FPVSIM2 lay=3 best1=12345 vol=7\n",
    "FPVSim.dat": b"FPVSIM legacy\r\n",
    "StickTimeTracks/c1.txt": b"My Track\n1 123456 7 20\n",
    "StickTimeTracks/c8.txt": b"Last slot\r\n1 654321 9 5\r\n",
    "StickTimeGhosts/c1.txt": bytes(range(256)) * 8,
    "StickTimeGhosts/c12.txt": b"ghost 12\n",
    "StickTimeLite/data.txt": b"lite save\n",       # the Lite's save, inside its game folder
    "StickTimeBW.dat": b"BW save\n",
}


def editions_of(zbytes):
    """the B&W and Lite editions' files of a release zip as the updater installs them {rel: bytes}"""
    gz = gamezip.validate_game_zip(zbytes, common.header_version(zip_member(zbytes, "SCRIPTS/TOOLS/StickTime.lua")))
    m, files = gamezip.manifest(gz)
    return {f["path"]: files[f["path"]] for f in m["files"] if f["edition"] in ("bw", "lite")}


# ---------------------------------------------------------------- the device

class Device:
    """a temporary SD card + /run folder, factory state: game 1.7.2 (no slots) with its B&W and Lite
    editions as the image installs them (editions=False: an image before 0.2, color only), app 0.1.0"""

    def __init__(self, root, gh, editions=True, factory_zip=None):
        self.root = root
        self.gh = gh
        self.base = os.path.join(root, "boot", "sticktime")
        self.settings = os.path.join(root, "boot", "sticktime.txt")
        self.state = os.path.join(root, "run")
        self.libdir = os.path.join(root, "usr-lib-sticktime")
        self.tools = os.path.join(self.base, "sd", "SCRIPTS", "TOOLS")
        os.makedirs(self.tools)
        os.makedirs(self.state)
        z = factory_zip or game_zip("1.7.2")
        self.factory_lua = zip_member(z, "SCRIPTS/TOOLS/StickTime.lua")
        self.put("sd/SCRIPTS/TOOLS/StickTime.lua", self.factory_lua)
        self.put("sd/SCRIPTS/TOOLS/GAME_VERSION", b"1.7.2\n")
        self.factory_editions = editions_of(z) if editions else {}
        for rel, b in self.factory_editions.items():
            self.put("sd/SCRIPTS/TOOLS/" + rel, b)
        # the player edited this one; StickTimeGhosts/README.txt is missing
        self.tracks_readme = b"My notes: c1 is the park, c8 the garage.\r\n"
        self.put("sd/SCRIPTS/TOOLS/StickTimeTracks/README.txt", self.tracks_readme)
        for rel, b in PLAYER.items():
            self.put("sd/SCRIPTS/TOOLS/" + rel, b)
        self.factory_page = patched(sim_html("1.7.2"))
        self.put("editor/index.html", self.factory_page)
        self.put("editor/tracks/index.json", gallery(2, "factory"))
        self.put("bin/sticktime-pi", fake_elf(payload=b"host 0.1.0"))
        self.put("web/index.html", b"<!doctype html><title>StickTime Pi 0.1.0</title>")
        self.put("web/app.js", b"console.log('app 0.1.0');")
        self.put("VERSION", b"0.1.0\n")
        self.write_settings()
        self.player0 = self.player()

    def write_settings(self, auto="off", token=""):
        with open(self.settings, "w", newline="\n") as f:
            f.write("# StickTime Pi settings\nvideo = av\nauto_update = %s\ngithub_token = %s\n" % (auto, token))

    def p(self, rel):
        return os.path.join(self.base, *rel.split("/"))

    def put(self, rel, data):
        p = self.p(rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    def get(self, rel):
        try:
            with open(self.p(rel), "rb") as f:
                return f.read()
        except OSError:
            return None

    def exists(self, rel):
        return os.path.lexists(self.p(rel))

    def player(self):
        out = {}
        for rel in PLAYER:
            out[rel] = self.get("sd/SCRIPTS/TOOLS/" + rel)
        return out

    def editions(self):
        """the B&W and Lite editions' files in sd/ (loaders, everything in their folders but the
        player's data.txt) {rel: bytes}"""
        out = {}
        for loader, folder in (("StickTimeBW.lua", "StickTimeBW"), ("StickTimeLite.lua", "StickTimeLite")):
            if self.exists("sd/SCRIPTS/TOOLS/" + loader):
                out[loader] = self.get("sd/SCRIPTS/TOOLS/" + loader)
            d = self.p("sd/SCRIPTS/TOOLS/" + folder)
            for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
                if folder + "/" + n not in PLAYER:
                    out[folder + "/" + n] = self.get("sd/SCRIPTS/TOOLS/%s/%s" % (folder, n))
        return out

    def env(self, **kw):
        e = dict(os.environ)
        for k in list(e):
            if k.startswith("STICKTIME_"):
                del e[k]
        e.update({
            "STICKTIME_GITHUB_API": self.gh.api, "STICKTIME_GITHUB_RAW": self.gh.raw, "STICKTIME_PAGES": self.gh.pages,
            "STICKTIME_PATCH_EDITOR": PATCHER, "STICKTIME_HTTP_TIMEOUT": "5", "STICKTIME_TIME_WAIT": "0",
            "STICKTIME_RESTART_CMD": json.dumps([sys.executable, "-c",
                                                 "open(r'%s','a').write('r')" % os.path.join(self.state, "restarted")]),
            "STICKTIME_RELOAD_URL": self.gh.base + "/reload",
            "STICKTIME_STATUS_URL": self.gh.base + "/status",
            "STICKTIME_IMAGE_VERSION_FILE": os.path.join(self.root, "image-version"),
            "STICKTIME_VAR": os.path.join(self.root, "var"),
            # the system scripts (/usr/lib/sticktime) and systemctl, which logs what it was asked
            "STICKTIME_LIBDIR": self.libdir,
            "STICKTIME_SYSTEMCTL": json.dumps([sys.executable, "-c", "import sys; open(r'%s', 'a').write(' '.join("
                                               "sys.argv[1:]) + chr(10))" % os.path.join(self.state, "systemctl.log")]),
        })
        e.update({k: str(v) for k, v in kw.items()})
        return e

    def argv(self, *args):
        return [sys.executable, "-I", SCRIPT, "--base", self.base, "--settings", self.settings,
                "--state", self.state] + list(args)

    def run(self, *args, **env):
        r = subprocess.run(self.argv(*args), env=self.env(**env), capture_output=True, timeout=120)
        self.last = r
        return r.returncode

    def run_fast(self, *args, **env):
        """the same command inside this process, ten times faster, for the long loops (never with a
        STICKTIME_UPDATE_CRASH_AT point: that one ends the process)"""
        from stupdate import cli
        saved = dict(os.environ)
        out, err = io.StringIO(), io.StringIO()
        try:
            os.environ.clear()
            os.environ.update(self.env(**env))
            common.FAULTS = common._Faults()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = cli.main(self.argv(*args)[3:])
        finally:
            os.environ.clear()
            os.environ.update(saved)
            common.FAULTS = common._Faults()
        self.last = subprocess.CompletedProcess(args, rc, out.getvalue().encode(), err.getvalue().encode())
        return rc

    def out(self):
        return (self.last.stdout + self.last.stderr).decode("utf-8", "replace")

    def status(self):
        with open(os.path.join(self.state, "update.json"), "rb") as f:
            return json.loads(f.read())

    def slots(self):
        d = self.p("game")
        return sorted(os.listdir(d)) if os.path.isdir(d) else []

    def restarts(self):
        try:
            with open(os.path.join(self.state, "restarted")) as f:
                return len(f.read())
        except OSError:
            return 0

    def tmp_files(self):
        out = []
        for root, _, names in os.walk(self.base):
            out += [os.path.join(root, n) for n in names if n.endswith(".updtmp")]
        return out


class FakeServerCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gh = FakeGitHub().start()

    @classmethod
    def tearDownClass(cls):
        cls.gh.stop()

    def setUp(self):
        self.gh.reset()
        self.gh.gallery = gallery(3)
        self.tmp = tempfile.mkdtemp(prefix="stupd-")
        self.dev = Device(self.tmp, self.gh)
        self.releases = {}
        self.rels = {}
        self.apps = {}

    def tearDown(self):
        # the player's files are never touched, whatever happened
        self.assertEqual(self.dev.player(), self.dev.player0, "saves/tracks/ghosts changed")
        shutil.rmtree(self.tmp, ignore_errors=True)

    def release(self, version, zbytes=None, page=True, **kw):
        z = zbytes if zbytes is not None else game_zip(version)
        files = {"sdcard/SCRIPTS/TOOLS/StickTime.lua": zip_member(z, "SCRIPTS/TOOLS/StickTime.lua")} if zbytes is None else {}
        if page:
            files["web/simulator.html"] = page if isinstance(page, bytes) else sim_html(version)
        files.update(kw.pop("files", {}))
        kw.setdefault("body", body(version))
        rel, commit = self.gh.add_release(GAME, "v" + version, "StickTime-%s-sdcard.zip" % version, z, files=files, **kw)
        self.releases[version] = (z, commit)
        self.rels[version] = rel
        return z

    def asset_id(self, version):
        return self.rels[version]["assets"][0]["id"]

    def app_release(self, version, tgz=None, private=False, token=None, sums=None, **kw):
        """a StickTime Pi release; sums=True adds a matching SHA256SUMS asset, bytes a given one"""
        t = tgz if tgz is not None else app_tgz(version)
        self.gh.repo(APPR, private=private, token=token)
        extra = {}
        if sums is True:
            extra["SHA256SUMS"] = ("%s  sticktime-pi-app-%s.tar.gz\n" % (hashlib.sha256(t).hexdigest(), version)).encode()
        elif sums is not None:
            extra["SHA256SUMS"] = sums
        kw.setdefault("body", body(version))
        self.gh.add_release(APPR, "v" + version, "sticktime-pi-app-%s.tar.gz" % version, t, extra_assets=extra, **kw)
        self.apps[version] = t
        return t

    def assert_app(self, version, dev=None):
        d = dev or self.dev
        self.assertEqual(d.get("VERSION"), (version + "\n").encode())
        if version == "0.1.0":
            self.assertEqual(d.get("bin/sticktime-pi"), fake_elf(payload=b"host 0.1.0"))
            self.assertEqual(d.get("web/index.html"), b"<!doctype html><title>StickTime Pi 0.1.0</title>")
            self.assertFalse(d.exists("web/pi-sd.js"))
        else:
            with tarfile.open(fileobj=io.BytesIO(self.apps[version]), mode="r:gz") as tf:
                for m in tf:
                    if m.isreg():
                        self.assertEqual(d.get(m.name), tf.extractfile(m).read(), m.name)
            rec = json.loads(d.get("APP_RELEASE.json"))
            self.assertEqual((rec["version"], rec["digest"]), (version, hashlib.sha256(self.apps[version]).hexdigest()))
        self.assertEqual(self.inspect(d)[1].verify_installed(), [])

    def lua(self, version):
        return zip_member(self.releases[version][0], "SCRIPTS/TOOLS/StickTime.lua")

    def assert_game(self, version, page_version=None, dev=None):
        d = dev or self.dev
        self.assertEqual(d.get("sd/SCRIPTS/TOOLS/StickTime.lua"),
                         self.lua(version) if version in self.releases else d.factory_lua)
        self.assertEqual(d.get("sd/SCRIPTS/TOOLS/GAME_VERSION"), (version + "\n").encode())
        # the B&W and Lite editions are the same release's (and the Lite's data.txt is still there)
        self.assertEqual(d.editions(), editions_of(self.releases[version][0]) if version in self.releases
                         else d.factory_editions)
        pv = page_version or version
        want = d.factory_page if pv == "factory" else patched(sim_html(pv))
        self.assertEqual(d.get("editor/index.html"), want)
        self.assertEqual(d.tmp_files(), [])

    def inspect(self, dev=None):
        d = dev or self.dev
        cfg = common.Config(base=d.base, state=d.state, env={"STICKTIME_PATCH_EDITOR": PATCHER})
        return gamemod.Game(cfg, None, None), appmod.App(cfg, None, None, {})

    def assert_consistent(self, dev=None):
        """whatever happened, the card is in one piece: complete slots only, the runtime copies follow
        the active slot, the staged release is whole or absent, the app set verifies, no leftovers,
        and the player's files are untouched"""
        d = dev or self.dev
        g, a = self.inspect(d)
        self.assertEqual(d.tmp_files(), [])
        for s in g.slots():
            self.assertTrue(s.valid, "%s is a broken slot" % s.name)
        lua = d.get("sd/SCRIPTS/TOOLS/StickTime.lua")
        self.assertIsNotNone(lua)
        self.assertEqual(d.get("sd/SCRIPTS/TOOLS/GAME_VERSION"), (common.header_version(lua) + "\n").encode())
        inst = g.installed()
        if inst.slot is None:
            self.assertEqual(d.editions(), d.factory_editions, "no slot: the editions are the image's")
        if inst.state == "slot":
            act = inst.slot
            if act.editions is not None:
                # never a mix: the editions in sd/ are exactly the active slot's
                self.assertEqual(d.editions(), {rel: act.read(rel) for rel in act.sd_files() if rel != "StickTime.lua"},
                                 "the editions follow %s" % act.name)
            if act.has("simulator.html"):
                try:
                    want = patched(act.read("simulator.html"))
                except ValueError:
                    want = None              # a page the patcher doesn't know: the previous one stays
                if want is not None:
                    self.assertEqual(d.get("editor/index.html"), want)
            elif act.has("editor.html"):
                self.assertEqual(d.get("editor/index.html"), act.read("editor.html"))
        if d.exists("editor/sdcard/manifest.json"):
            self.assert_staged_ok(dev=d)
        self.assertEqual(a.verify_installed(), [])
        for n in ("bin/sticktime-pi", "web/index.html", "VERSION"):
            self.assertTrue(d.exists(n), n)
        self.assertFalse(d.exists("update/staging"))
        self.assertFalse(d.exists("update/journal.json"))
        self.assertEqual([n for n in os.listdir(d.base) if n.endswith(".swap")], [])
        self.assertEqual(d.player(), d.player0, "saves/tracks/ghosts changed")
        self.assertEqual(d.get("sd/SCRIPTS/TOOLS/StickTimeTracks/README.txt"), d.tracks_readme)
        st = d.status() if os.path.exists(os.path.join(d.state, "update.json")) else None
        if st is not None:
            self.assert_schema(st)
        return inst

    def assert_schema(self, st):
        self.assertEqual(set(st), SCHEMA)
        self.assertEqual(set(st["game"]), {"installed", "latest", "staged", "notes"})
        self.assertEqual(set(st["app"]), {"installed", "latest", "notes"})
        self.assertIn(st["state"], ("idle", "checking", "downloading", "installing", "done", "error"))
        self.assertTrue(0 <= st["progress"] <= 1)
        self.assertIsInstance(st["checked_at"], int)
        for k in ("message", "error"):
            self.assertIsInstance(st[k], str)

    def assert_failed(self, rc, want_rc, *words, dev=None):
        d = dev or self.dev
        self.assertEqual(rc, want_rc, d.out())
        st = d.status()
        self.assert_schema(st)
        self.assertEqual(st["state"], "error")
        self.assertTrue(st["error"])
        for w in words:
            self.assertIn(w, st["message"])
        return st

    def assert_staged_ok(self, version=None, dev=None):
        d = dev or self.dev
        m = json.loads(d.get("editor/sdcard/manifest.json"))
        self.assertEqual(gamezip.man_problem(m), "")
        if version:
            self.assertEqual(m["version"], version)
        for f in m["files"]:
            b = d.get("editor/sdcard/SCRIPTS/TOOLS/" + f["path"])
            self.assertIsNotNone(b, f["path"])
            self.assertEqual(hashlib.sha256(b).hexdigest(), f["sha256"])
            self.assertEqual(len(b), f["size"])
        return m


# ---------------------------------------------------------------- pure functions

class TestVersions(unittest.TestCase):
    def test_vkey(self):
        v = common.vkey
        self.assertEqual(v("1.6"), v("1.6.0"))
        self.assertEqual(v("1.6.0.0"), v("1.6"))
        self.assertLess(v("1.6"), v("1.6.1"))
        self.assertLess(v("1.6.9"), v("1.7"))
        self.assertLess(v("1.9"), v("1.10"))
        self.assertGreater(v("2.0"), v("1.99.99.99"))
        self.assertEqual(v("01.2"), v("1.2"))

    def test_tag_re(self):
        ok = ["v1.5", "v1.6.1", "v1.7.2", "v10.0.0.1"]
        no = ["1.5", "v1", "v1.2.3.4.5", "v1.2-beta", "v1.2 ", "V1.2", "v1..2", "release-1.2", ""]
        for t in ok:
            self.assertTrue(common.TAG_RE.fullmatch(t), t)
        for t in no:
            self.assertFalse(common.TAG_RE.fullmatch(t), t)

    def test_header_version(self):
        h = common.header_version
        self.assertEqual(h(b'local toolName = "TNS|StickTime|TNE"\n--[[\n  StickTime v1.7.2  -  x'), "1.7.2")
        self.assertEqual(h(b"  StickTime BW v1.6  - "), "1.6")
        self.assertIsNone(h(b"StickTime v"))
        self.assertIsNone(h(b"x" * 600 + b"StickTime v1.2.3"))   # only the first 600 bytes
        self.assertIsNone(h(b""))
        self.assertIsNone(h(None))


def _rel(tag, assets, **kw):
    r = {"tag_name": tag, "draft": False, "prerelease": False, "id": 1, "published_at": "2026-10-14T09:12:29Z",
         "body": "", "assets": assets}
    r.update(kw)
    return r


def _asset(name, size=100, state="uploaded", digest="sha256:" + "a" * 64, aid=5):
    return {"name": name, "size": size, "state": state, "digest": digest, "id": aid,
            "browser_download_url": "https://github.com/x/" + name, "url": "https://api.github.com/a/%d" % aid,
            "updated_at": "2026-10-14T09:23:30Z"}


class TestPickRelease(unittest.TestCase):
    F = "StickTime-{ver}-sdcard.zip"

    def pick(self, rels, want=None):
        return net.pick_release(rels, self.F, 8 * common.MIB, want)

    def test_highest_version_not_latest(self):
        rels = [_rel("v1.6.9", [_asset("StickTime-1.6.9-sdcard.zip")], published_at="2026-10-15T00:00:00Z"),
                _rel("v1.7.2", [_asset("StickTime-1.7.2-sdcard.zip")]),
                _rel("v1.10", [_asset("StickTime-1.10-sdcard.zip")])]
        best, pending = self.pick(rels)
        self.assertEqual(best.version, "1.10")
        self.assertIsNone(pending)

    def test_skips(self):
        rels = [_rel("v2.0", [_asset("StickTime-2.0-sdcard.zip")], draft=True),
                _rel("v1.9", [_asset("StickTime-1.9-sdcard.zip")], prerelease=True),
                _rel("v1.8", [_asset("StickTime-1.8.0-sdcard.zip")]),          # name must use the tag's version
                _rel("latest", [_asset("StickTime-latest-sdcard.zip")]),
                _rel("v1.7.9", [_asset("StickTime-1.7.9-sdcard.zip", size=0)]),
                _rel("v1.7.8", [_asset("StickTime-1.7.8-sdcard.zip", size=9 * common.MIB)]),
                _rel("v1.7.7", [_asset("StickTime-1.7.7-sdcard.zip", digest="md5:abc")]),
                _rel("v1.7.6", [_asset("StickTime-1.7.6-sdcard.zip", state="starter")]),
                _rel("v" + "1" * 40 + ".0", [_asset("x")]),
                "garbage", None, _rel(None, []),
                _rel("v1.7.2", [_asset("StickTime-1.7.2-sdcard.zip")])]
        best, pending = self.pick(rels)
        self.assertEqual(best.version, "1.7.2")
        self.assertEqual(pending, "1.8")

    def test_pending_and_null_digest(self):
        rels = [_rel("v1.7.3", []), _rel("v1.7.2", [_asset("StickTime-1.7.2-sdcard.zip", digest=None)])]
        best, pending = self.pick(rels)
        self.assertEqual((best.version, best.digest, pending), ("1.7.2", None, "1.7.3"))

    def test_nothing(self):
        self.assertEqual(self.pick([]), (None, None))
        self.assertEqual(self.pick({"message": "Not Found"}), (None, None))
        self.assertEqual(self.pick([_rel("v1.7.3", [])]), (None, "1.7.3"))

    def test_same_version_two_tags(self):
        rels = [_rel("v1.6", [_asset("StickTime-1.6-sdcard.zip", aid=1)], published_at="2026-01-01T00:00:00Z"),
                _rel("v1.6.0", [_asset("StickTime-1.6.0-sdcard.zip", aid=2)], published_at="2026-02-01T00:00:00Z")]
        self.assertEqual(self.pick(rels)[0].asset_id, 2)

    def test_want(self):
        rels = [_rel("v1.7.2", [_asset("StickTime-1.7.2-sdcard.zip")]), _rel("v1.6", [_asset("StickTime-1.6-sdcard.zip")])]
        self.assertEqual(self.pick(rels, "1.6.0")[0].tag, "v1.6")
        self.assertEqual(self.pick(rels, "1.5"), (None, None))

    def test_notes(self):
        self.assertEqual(net.notes_of(body("1.7.3")), "### New in 1.7.3\n- **Better gates** for 1.7.3.")
        self.assertEqual(net.notes_of("Just text\r\n"), "Just text")
        self.assertEqual(net.notes_of(None), "")
        self.assertEqual(len(net.notes_of("### New in 9\n" + "x" * 9000)), 4000)

    def test_iso_time(self):
        self.assertEqual(net.iso_time("1970-01-01T00:01:00Z"), 60)
        self.assertIsNone(net.iso_time("yesterday"))
        self.assertIsNone(net.iso_time(None))


class TestGameZip(unittest.TestCase):
    def ok(self, z, v="1.7.2"):
        return gamezip.validate_game_zip(z, v)

    def bad(self, z, v="1.7.2", why=None):
        with self.assertRaises(common.UpdateError) as cm:
            gamezip.validate_game_zip(z, v, asset_id=77)
        self.assertEqual(cm.exception.code, "bad_zip")
        self.assertIn("asset 77", cm.exception.message)
        if why:
            self.assertIn(why, cm.exception.message)

    def test_model_and_versions(self):
        gz = self.ok(game_zip("1.7.2"))
        self.assertEqual(gz.version, "1.7.2")
        self.assertIn("StickTimeBW/game.luac", gz.files)
        self.ok(game_zip("1.6"), "1.6.0")         # 1.6 == 1.6.0
        self.bad(game_zip("1.7.3"), "1.7.2", "says 1.7.3")

    def test_real_release_zip(self):
        if not os.path.isfile(FIXTURE):
            self.skipTest("no fixture zip")
        with open(FIXTURE, "rb") as f:
            data = f.read()
        self.assertEqual(hashlib.sha256(data).hexdigest(), "572d19a844b579e09b6c8072c22cde2a4b545480e9fc644fa39a057d35e47b00")
        gz = self.ok(data)
        self.assertEqual(len(gz.files), 26)
        m, files = gamezip.manifest(gz)
        self.assertEqual(gamezip.man_problem(m), "")
        self.assertEqual(sorted(m["editions"]), ["bw", "color", "lite"])
        self.assertEqual(len(m["files"]), 26)

    def test_unsafe_names(self):
        for name in ["SCRIPTS/TOOLS/../x.lua", "../SCRIPTS/TOOLS/x.lua", "/SCRIPTS/TOOLS/x.lua", "C:/SCRIPTS/TOOLS/x.lua", "SCRIPTS/TOOLS/x:y.lua", "SCRIPTS/TOOLS/.hidden", "SCRIPTS/TOOLS//x.lua",
                     "SCRIPTS/TOOLS/./x.lua", "SCRIPTS/TOOLS/caf\u00e9.lua", "SCRIPTS/TOOLS/a b.lua",
                     "SCRIPTS/TOOLS/" + "a" * 65, "SCRIPTS/x.lua", "RADIO/x.yml", "x.lua"]:
            self.bad(game_zip("1.7.2", add={name: b"x"}), why=None)
        # names as stored in the zip, which ZipInfo() would quietly change
        bs = "\\"
        for name in ["SCRIPTS" + bs + "TOOLS" + bs + "x.lua", "SCRIPTS/TOOLS" + bs + ".." + bs + ".." + bs + "x.lua",
                     "SCRIPTS/TOOLS/x\x00.lua", "SCRIPTS/TOOLS/x.lua\x00../../evil"]:
            self.bad(zip_raw_name(name), why="unsafe name")

    def test_folders(self):
        self.ok(game_zip("1.7.2", raw_entries=MODEL + [("SCRIPTS/TOOLS/Extra/", None)]))
        self.bad(game_zip("1.7.2", raw_entries=MODEL + [("IMAGES/", None)]), why="outside")

    def test_player_files_never_shipped(self):
        for name in ["StickTimeTracks/c1.txt", "StickTimeGhosts/C12.TXT", "StickTime.dat", "FPVSim.DAT",
                     "StickTimeLite/data.txt", "StickTimeLite/DATA.TXT", "Other/data.txt"]:
            self.bad(game_zip("1.7.2", add={"SCRIPTS/TOOLS/" + name: b"x"}), why="player")

    def test_limits(self):
        self.bad(game_zip("1.7.2", add={"SCRIPTS/TOOLS/big.txt": b"x" * (1048576 + 1)}), why="bytes")
        self.ok(game_zip("1.7.2", add={"SCRIPTS/TOOLS/big.txt": b"x" * 1048576}))
        many = {"SCRIPTS/TOOLS/f%d.txt" % i: b"x" for i in range(260)}
        self.bad(game_zip("1.7.2", add=many), why="entries")
        total = {"SCRIPTS/TOOLS/b%d.txt" % i: os.urandom(1048576) for i in range(9)}
        self.bad(game_zip("1.7.2", add=total), why="8 MB")

    def test_duplicates(self):
        self.bad(game_zip("1.7.2", add={"SCRIPTS/TOOLS/stICKtime.LUA": b"x"}), why="twice")
        # the same name twice (zipfile warns while writing it; the zip is meant to be bad)
        buf = io.BytesIO()
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with zipfile.ZipFile(buf, "w") as z:
                for name, data in MODEL:
                    if data is not None:
                        z.writestr(name, data)
                z.writestr("SCRIPTS/TOOLS/StickTimeBW/core.lua", b"again")
        self.bad(buf.getvalue(), why="twice")

    def test_required_and_header(self):
        self.bad(game_zip("1.7.2", drop=("SCRIPTS/TOOLS/StickTime.lua",)), why="no SCRIPTS/TOOLS/StickTime.lua")
        lua = "SCRIPTS/TOOLS/StickTime.lua"
        self.bad(game_zip("1.7.2", change={lua: lambda b: b.replace(b"TNS|StickTime|TNE", b"TNS|Other|TNE")}), why="isn't the StickTime tool")
        self.bad(game_zip("1.7.2", change={lua: lambda b: b[:600]}), why="only")
        self.bad(game_zip("1.7.2", change={lua: lambda b: b.replace(b"StickTime v1.7.2", b"StickTime ver")}), why="no version")
        # only StickTime.lua is required (v1.5 had no READMEs)
        self.ok(game_zip("1.7.2", drop=tuple(n for n, _ in MODEL if n.endswith("README.txt"))))
        only = [(n, d) for n, d in MODEL if n in ("SCRIPTS/", "SCRIPTS/TOOLS/", lua)]
        gz = self.ok(game_zip("1.7.2", raw_entries=only))
        m, _ = gamezip.manifest(gz)
        self.assertEqual(list(m["editions"]), ["color"])
        self.assertEqual(gamezip.man_problem(m), "")

    def test_entry_kinds(self):
        def with_info(mod):
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, "w") as z:
                for name, data in MODEL:
                    zi = _zinfo(name, 0o40755 if data is None else 0o100644)
                    z.writestr(zi, data or b"")
                zi = _zinfo("SCRIPTS/TOOLS/link.lua", 0o120777)
                mod(zi)
                z.writestr(zi, b"StickTime.lua")
            return buf.getvalue()
        self.bad(with_info(lambda zi: None), why="link")
        self.ok(with_info(lambda zi: setattr(zi, "external_attr", 0o100644 << 16)))
        b = bytearray(with_info(lambda zi: setattr(zi, "external_attr", 0o100644 << 16)))
        # the encrypted flag, in the central directory entry of link.lua
        i = b.rfind(b"PK\x01\x02")
        b[i + 8] |= 1
        self.bad(bytes(b), why="encrypted")
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            for name, data in MODEL:
                zi = _zinfo(name, 0o100644)
                if data is not None and name.endswith("t1.txt"):
                    zi.compress_type = zipfile.ZIP_BZIP2
                z.writestr(zi, data or b"")
        if any(n.endswith("t1.txt") for n, _ in MODEL):
            self.bad(buf.getvalue(), why="compression")

    def test_damaged(self):
        z = bytearray(game_zip("1.7.2"))
        lua = zip_member(bytes(z), "SCRIPTS/TOOLS/StickTime.lua")
        self.bad(b"", why="not a zip")
        self.bad(b"PK\x03\x04garbage", why="not a zip")
        self.bad(bytes(z[: len(z) // 2]), why="not a zip")
        with zipfile.ZipFile(io.BytesIO(bytes(z))) as zz:
            off = zz.getinfo("SCRIPTS/TOOLS/StickTime.lua").header_offset
        # a flipped byte in StickTime.lua's compressed data: inflate or the CRC check fails
        z[off + 30 + len("SCRIPTS/TOOLS/StickTime.lua") + 100] ^= 0xFF
        self.bad(bytes(z), why="StickTime.lua")
        self.assertTrue(lua)

    def test_unknown_files(self):
        # forward compatibility: safe unknown files pass, but never reach the manifest
        gz = self.ok(game_zip("1.7.2", add={"SCRIPTS/TOOLS/NewThing/x.bin": b"x", "SCRIPTS/TOOLS/StickTimeBW/x.png": b"x"}))
        m, files = gamezip.manifest(gz)
        self.assertNotIn("NewThing/x.bin", files)
        self.assertNotIn("StickTimeBW/x.png", files)
        self.assertEqual(gamezip.man_problem(m), "")

    def test_edition_version_mismatch(self):
        gz = self.ok(game_zip("1.7.2", change={"SCRIPTS/TOOLS/StickTimeBW.lua": lambda b: b.replace(b"BW v1.7.2", b"BW v1.7.1")}))
        m, files = gamezip.manifest(gz)
        self.assertNotIn("bw", m["editions"])
        self.assertFalse(any(p.startswith("StickTimeBW") for p in files))
        self.assertEqual(gamezip.man_problem(m), "")

    def test_manifest_format(self):
        gz = self.ok(game_zip("1.7.2"))
        m, files = gamezip.manifest(gz)
        self.assertEqual(list(m), ["version", "base", "editions", "keep", "files"])
        self.assertEqual(m["keep"], ["StickTimeLite/data.txt"])
        self.assertEqual(m["editions"]["color"], {"label": "Color", "loader": "StickTime.lua", "folder": None})
        mb = gamezip.manifest_bytes(m)
        self.assertTrue(mb.endswith(b"}\n") and b"\r" not in mb)
        for f in m["files"]:
            self.assertEqual(f["sha256"], hashlib.sha256(files[f["path"]]).hexdigest())
            self.assertEqual(f["edition"], gamezip.edition_of(f["path"]))

    def test_man_problem(self):
        gz = self.ok(game_zip("1.7.2"))
        good, _ = gamezip.manifest(gz)
        mp = gamezip.man_problem
        self.assertEqual(mp(good), "")

        def mut(fn):
            m = json.loads(json.dumps(good))
            fn(m)
            return mp(m)
        self.assertEqual(mp(None), "not a list of files")
        self.assertEqual(mp([]), "not a list of files")
        self.assertEqual(mut(lambda m: m.pop("keep")), "not a list of files")
        self.assertEqual(mut(lambda m: m.update(version="1")), "its version or its folder")
        self.assertEqual(mut(lambda m: m.update(version="v1.2")), "its version or its folder")
        self.assertEqual(mut(lambda m: m.update(base="SCRIPTS")), "its version or its folder")
        self.assertEqual(mut(lambda m: m.update(keep=[1])), "its list of the player's files")
        self.assertEqual(mut(lambda m: m["editions"].update(x={"label": "X", "loader": "StickTime.lua", "folder": None})), "the x edition")
        self.assertEqual(mut(lambda m: m["editions"]["bw"].update(folder="Other")), "the bw edition")
        self.assertEqual(mut(lambda m: m["files"].append(dict(m["files"][0]))), "a file without a name, or twice")
        self.assertEqual(mut(lambda m: m["files"].append({"path": "StickTimeTracks/c1.txt", "size": 1, "sha256": "a" * 64, "edition": "shared"})),
                         "StickTimeTracks/c1.txt is the player's own")
        self.assertEqual(mut(lambda m: m["files"].append({"path": "StickTimeTracks/x.txt", "size": 1, "sha256": "a" * 64, "edition": "shared"})),
                         "StickTimeTracks/x.txt isn't one of the game's files")
        self.assertIn("size", mut(lambda m: m["files"][0].update(size=1048577)))
        self.assertIn("size", mut(lambda m: m["files"][0].update(size=True)))
        self.assertIn("size", mut(lambda m: m["files"][0].update(sha256="A" * 64)))
        self.assertEqual(mut(lambda m: m.update(files=[f for f in m["files"] if f["path"] != "StickTimeBW.lua"])), "StickTimeBW.lua is missing")

    def test_gallery_ok(self):
        g = gamezip.gallery_ok
        self.assertEqual(g(gallery(3)), "")
        self.assertEqual(g(b"<html>"), "not JSON")
        self.assertEqual(g(b'\xff\xfe'), "not JSON")
        self.assertEqual(g(b"[]"), "no list of tracks")
        self.assertEqual(g(b'{"tracks":[]}'), "0 tracks")
        self.assertEqual(g(json.dumps({"tracks": [{"text": "x"}] * 501}).encode()), "501 tracks")
        self.assertEqual(g(b'{"tracks":["x"]}'), "a track that isn't an object")
        self.assertEqual(g(json.dumps({"tracks": [{"text": "x" * 6000}]}).encode()), "")    # 1.8.0's bigger tracks
        self.assertEqual(g(json.dumps({"tracks": [{"text": "x" * 6001}]}).encode()), "a track over 6000 characters")
        self.assertEqual(g(b'{"tracks":[{"text":5}]}'), "a track's text")
        self.assertEqual(g(b'{"tracks":[{"text":"a","by":5}]}'), "a track's by")
        self.assertEqual(g(b'{"tracks":[{"text":"a","was":[0]}]}'), "a track's earlier ids")
        self.assertEqual(g(b'{"tracks":[{"text":"a","was":"1"}]}'), "a track's earlier ids")
        self.assertEqual(g(b'{"tracks":[{"name":"no text"}]}'), "no track with a text")

    def test_page_problem(self):
        pp = gamezip.page_problem
        self.assertEqual(pp(sim_html("1.7.3"), "1.7.3"), "")
        self.assertEqual(pp(b"\n<!DOCTYPE HTML>" + sim_html("1.7.3")[15:], "1.7.3"), "")
        self.assertEqual(pp(b"<html>" + sim_html("1.7.3"), "1.7.3"), "not an HTML page")
        self.assertEqual(pp(sim_html("1.7.3"), "1.7.4"), "not StickTime 1.7.4's page")
        self.assertEqual(pp(sim_html("1.7.3") + b"\xff", "1.7.3"), "not UTF-8")


class TestPreflight(unittest.TestCase):
    """the start test of a new binary before it is installed: it runs from STICKTIME_VAR (Pi OS mounts
    /run noexec, so 0.1.0's test from the state folder always failed with "Permission denied")"""
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="stpi-preflight-")
        self.cfg = common.Config(base=self.tmp, state=os.path.join(self.tmp, "run"),
                                 env={"STICKTIME_UPDATE_PREFLIGHT": "on", "STICKTIME_VAR": os.path.join(self.tmp, "var")})
        self.app = appmod.App(self.cfg, None, None, {})
        self.pkg = appmod.AppPackage("0.2.1", {appmod.BIN: b"\x7fELF usage: [--version]"}, {})

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def preflight(self, run):
        with mock.patch.object(appmod.subprocess, "run", run):
            self.app._preflight(self.pkg)

    def test_runs_from_the_var_folder(self):
        seen = []
        def run(argv, **kw):
            seen.append((argv, open(argv[0], "rb").read()))
            return subprocess.CompletedProcess(argv, 0, b"sticktime-pi 0.2.1\n")
        self.preflight(run)
        self.assertEqual(seen[0][0][1:], ["--version"])
        self.assertEqual(os.path.dirname(seen[0][0][0]), os.path.join(self.tmp, "var", "preflight"))
        self.assertEqual(seen[0][1], self.pkg.files[appmod.BIN])
        self.assertFalse(os.path.exists(seen[0][0][0]), "removed afterwards")
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "run", "preflight")), "nothing in the state folder")

    def test_a_folder_that_cant_run_programs_skips_the_test(self):
        def run(argv, **kw):
            raise PermissionError(13, "Permission denied", argv[0])
        self.preflight(run)                       # no UpdateError: the update goes on

    def test_a_binary_for_another_machine_is_refused(self):
        def run(argv, **kw):
            raise OSError(8, "Exec format error", argv[0])
        with self.assertRaises(common.UpdateError) as c:
            self.preflight(run)
        self.assertIn("can't run on this Pi", str(c.exception))

    def test_the_wrong_version_is_refused(self):
        with self.assertRaises(common.UpdateError) as c:
            self.preflight(lambda argv, **kw: subprocess.CompletedProcess(argv, 0, b"sticktime-pi 0.2.0\n"))
        self.assertIn("start test", str(c.exception))

    def test_a_crash_is_refused(self):
        with self.assertRaises(common.UpdateError):
            self.preflight(lambda argv, **kw: subprocess.CompletedProcess(argv, -11, b""))


class TestAppTar(unittest.TestCase):
    def bad(self, data, why, v="0.2.0"):
        with self.assertRaises(common.UpdateError) as cm:
            appmod.validate_app_tar(data, v, asset_id=9)
        self.assertEqual(cm.exception.code, "bad_package")
        self.assertIn(why, cm.exception.message)

    def test_good(self):
        pkg = appmod.validate_app_tar(app_tgz("0.2.0", compat={"min_image": "0.1.0"}), "0.2.0")
        self.assertEqual(pkg.version, "0.2.0")
        self.assertIn("web/pi-sd.js", pkg.files)
        self.assertEqual(pkg.compat["min_image"], "0.1.0")
        appmod.validate_app_tar(app_tgz("0.2"), "0.2.0")

    def test_dot_slash_names(self):
        files = {"VERSION": b"0.2.0\n", "bin/sticktime-pi": fake_elf()}
        sums = "".join("%s *./%s\n" % (hashlib.sha256(b).hexdigest(), p) for p, b in files.items())
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tf:
            ti = tarfile.TarInfo(".")
            ti.type = tarfile.DIRTYPE
            tf.addfile(ti)
            for p, b in list(files.items()) + [("SHA256SUMS", sums.encode())]:
                ti = tarfile.TarInfo("./" + p)
                ti.size = len(b)
                tf.addfile(ti, io.BytesIO(b))
        self.assertEqual(appmod.validate_app_tar(buf.getvalue(), "0.2.0").version, "0.2.0")

    def test_rejects(self):
        def member(name, typ=tarfile.REGTYPE, data=b"x", link=""):
            ti = tarfile.TarInfo(name)
            ti.type = typ
            ti.linkname = link
            ti.size = len(data) if typ == tarfile.REGTYPE else 0
            return ti, (data if typ == tarfile.REGTYPE else None)
        self.bad(app_tgz("0.2.0", members=[member("web/link", tarfile.SYMTYPE, link="/etc/passwd")]), "link")
        self.bad(app_tgz("0.2.0", members=[member("web/hard", tarfile.LNKTYPE, link="VERSION")]), "link")
        self.bad(app_tgz("0.2.0", members=[member("web/dev", tarfile.CHRTYPE)]), "link or a device")
        self.bad(app_tgz("0.2.0", members=[member("web/../../etc/x")]), "unsafe")
        self.bad(app_tgz("0.2.0", members=[member("/etc/x")]), "unsafe")
        self.bad(app_tgz("0.2.0", members=[member("web\\x")]), "unsafe")
        self.bad(app_tgz("0.2.0", members=[member("README.md")]), "unexpected file")
        self.bad(app_tgz("0.2.0", members=[member("etc/x")]), "unexpected file")
        self.bad(app_tgz("0.2.0", members=[member("etc", tarfile.DIRTYPE)]), "unexpected folder")
        self.bad(app_tgz("0.2.0", members=[member("web/APP.JS")]), "twice")      # FAT is case-insensitive
        self.bad(app_tgz("0.2.0", members=[member("WEB/x.js")]), "unexpected file")
        self.bad(app_tgz("0.2.0", drop=("VERSION",)), "no VERSION")
        self.bad(app_tgz("0.2.0", drop=("bin/sticktime-pi",)), "no bin/sticktime-pi")
        self.bad(app_tgz("0.2.0", tweak=lambda f: f.pop("SHA256SUMS")), "no SHA256SUMS")
        self.bad(app_tgz("0.3.0"), "VERSION says")
        self.bad(app_tgz("0.2.0", tweak=lambda f: f.update(VERSION=b"zero\n")), "VERSION says")
        self.bad(app_tgz("0.2.0", tweak=lambda f: f.update({"web/app.js": b"changed"})), "doesn't match SHA256SUMS")
        self.bad(app_tgz("0.2.0", tweak=lambda f: f.update({"web/new.js": b"new"})), "isn't in SHA256SUMS")
        self.bad(app_tgz("0.2.0", tweak=lambda f: f.update(SHA256SUMS=f["SHA256SUMS"] + b"%s  web/gone.js\n" % (b"a" * 64))), "which isn't in the file")
        self.bad(app_tgz("0.2.0", tweak=lambda f: f.update(SHA256SUMS=b"nonsense\n")), "SHA256SUMS")
        self.bad(app_tgz("0.2.0", elf=fake_elf(machine=62)), "aarch64")
        self.bad(app_tgz("0.2.0", elf=fake_elf(cls=1)), "64-bit")
        self.bad(app_tgz("0.2.0", elf=fake_elf(etype=1)), "executable")
        self.bad(app_tgz("0.2.0", elf=b"#!/bin/sh\necho hi\n"), "isn't a program")
        self.bad(app_tgz("0.2.0", extra={"compat.json": b"{"}), "compat.json")
        self.bad(app_tgz("0.2.0", extra={"compat.json": b"[1]"}), "compat.json")
        self.bad(b"not gzip at all", "not a .tar.gz")
        self.bad(app_tgz("0.2.0")[:200], "")

    def test_parse_sums(self):
        p = appmod.parse_sums
        self.assertEqual(p("%s  a/b\n%s *./c\n\n" % ("A" * 64, "b" * 64)), {"a/b": "a" * 64, "c": "b" * 64})
        with self.assertRaises(ValueError):
            p("%s  a\n%s  a\n" % ("a" * 64, "b" * 64))
        with self.assertRaises(ValueError):
            p("abc  a\n")


class TestMisc(unittest.TestCase):
    def test_parse_settings(self):
        t = "\ufeff# c\r\nauto_update = on   # comment\r\ngithub_token = \"ab#c \\\" d\"\nAUTO_UPDATE=off\nbroken line\n"
        d = common.parse_settings(t)
        self.assertEqual(d["auto_update"], "off")            # a later line wins
        self.assertEqual(d["github_token"], 'ab#c " d')
        self.assertEqual(common.parse_settings("x = a#b\n")["x"], "a#b")
        self.assertTrue(common.truthy("On") and common.truthy("yes") and common.truthy("1") and not common.truthy("off"))

    def test_read_settings_module(self):
        tmp = tempfile.mkdtemp()
        try:
            p = os.path.join(tmp, "s.txt")
            with open(p, "w") as f:
                f.write("auto_update = on\ngithub_token = tok\n")
            import types
            fake = types.ModuleType("settings")
            fake.load = lambda path: {"auto_update": "on", "github_token": "from-module"}
            sys.modules["settings"] = fake
            try:
                self.assertEqual(common.read_settings(p)["github_token"], "from-module")
                fake2 = types.ModuleType("settings")

                class S:
                    def get(self, k):
                        return {"auto_update": "on"}.get(k)
                fake2.load = lambda path: (S(), True)
                sys.modules["settings"] = fake2
                self.assertEqual(common.read_settings(p)["auto_update"], "on")
                fake3 = types.ModuleType("settings")

                def boom(path):
                    raise RuntimeError("broken")
                fake3.load = boom
                sys.modules["settings"] = fake3
                with contextlib.redirect_stderr(io.StringIO()) as err:
                    self.assertEqual(common.read_settings(p)["github_token"], "tok")   # falls back to the file
                self.assertIn("settings.py: broken", err.getvalue())
            finally:
                del sys.modules["settings"]
            self.assertEqual(common.read_settings(os.path.join(tmp, "missing.txt")), {} if not os.path.isfile(os.path.join(LIB, "settings.py")) else common.read_settings(os.path.join(tmp, "missing.txt")))
        finally:
            shutil.rmtree(tmp)

    def test_read_settings_real_module(self):
        if not os.path.isfile(os.path.join(LIB, "settings.py")):
            self.skipTest("settings.py isn't there yet")
        tmp = tempfile.mkdtemp()
        try:
            p = os.path.join(tmp, "sticktime.txt")
            with open(p, "wb") as f:
                f.write(b"\xef\xbb\xbf# mine\r\nauto_update = YES   # comment\r\ngithub_token = \"github_pat_x#y\"\r\n")
            d = common.read_settings(p)
            self.assertEqual(d["auto_update"], "on")              # effective value, normalized
            self.assertEqual(d["github_token"], "github_pat_x#y")
            self.assertTrue(common.truthy(d["auto_update"]))
            # missing file: the defaults (auto_update off, no token)
            d = common.read_settings(os.path.join(tmp, "missing.txt"))
            self.assertFalse(common.truthy(d.get("auto_update", "off")))
            self.assertEqual(d.get("github_token", ""), "")
            # garbage value: the default
            with open(p, "w") as f:
                f.write("auto_update = maybe\n")
            self.assertEqual(common.read_settings(p)["auto_update"], "off")
        finally:
            shutil.rmtree(tmp)

    def test_host_ok(self):
        cfg = common.Config(base="x", state="y", env={"STICKTIME_GITHUB_API": "http://127.0.0.1:9/api"})
        ok = ["https://api.github.com/repos/x", "https://github.com/a/b/releases/download/v1/x.zip",
              "https://release-assets.githubusercontent.com/x?sig=1", "https://raw.githubusercontent.com/a/b/c",
              "https://juricabi.github.io/sticktime/tracks/index.json", "http://127.0.0.1:9/cdn/1"]
        no = ["http://github.com/x", "https://evil.example/x", "https://github.com.evil.example/x",
              "https://githubusercontent.com.evil/x", "ftp://github.com/x", "http://127.0.0.1:10/x", "file:///etc/passwd",
              "https://evilgithubusercontent.com/x"]
        for u in ok:
            self.assertTrue(net.host_ok(u, cfg), u)
        for u in no:
            self.assertFalse(net.host_ok(u, cfg), u)

    def test_elf(self):
        self.assertEqual(appmod.elf_problem(fake_elf()), "")
        self.assertEqual(appmod.elf_problem(fake_elf(etype=3)), "")
        self.assertNotEqual(appmod.elf_problem(fake_elf()[:30]), "")

    def test_status_schema(self):
        tmp = tempfile.mkdtemp()
        try:
            cfg = common.Config(base=tmp, state=tmp, env={})
            with open(os.path.join(tmp, "update.json"), "w") as f:
                f.write('{"state":"bogus","extra":1,"game":{"installed":5,"latest":"1.2","x":1},"checked_at":"x"}')
            st = common.Status(cfg)
            st.set("checking", "m", 2.5)
            with open(os.path.join(tmp, "update.json"), "rb") as f:
                d = json.loads(f.read())
            self.assertEqual(set(d), SCHEMA)
            self.assertEqual(set(d["game"]), {"installed", "latest", "staged", "notes"})
            self.assertEqual(set(d["app"]), {"installed", "latest", "notes"})
            self.assertEqual((d["state"], d["progress"], d["game"]["latest"], d["game"]["installed"], d["checked_at"]),
                             ("checking", 1.0, "1.2", "", 0))
        finally:
            shutil.rmtree(tmp)

    def test_lock(self):
        tmp = tempfile.mkdtemp()
        try:
            a, b = common.Lock(os.path.join(tmp, "l")), common.Lock(os.path.join(tmp, "l"))
            self.assertTrue(a.acquire())
            self.assertFalse(b.acquire())
            a.release()
            self.assertTrue(b.acquire())
            b.release()
        finally:
            shutil.rmtree(tmp)


def dead_api():
    """an address where nothing listens: connection refused"""
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return "http://127.0.0.1:%d/api" % port


LUA_REL = "sd/SCRIPTS/TOOLS/StickTime.lua"


# ---------------------------------------------------------------- the game, end to end

class TestGameCheckApply(FakeServerCase):
    def test_check_stages_without_installing(self):
        z = self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("check"), 10, d.out())
        st = d.status()
        self.assert_schema(st)
        self.assertEqual((st["state"], st["error"]), ("done", ""))
        self.assertEqual((st["game"]["installed"], st["game"]["latest"], st["game"]["staged"]), ("1.7.2", "1.7.3", "1.7.3"))
        self.assertEqual(st["game"]["notes"], "### New in 1.7.3\n- **Better gates** for 1.7.3.")
        self.assertIn("StickTime 1.7.3 is available (1.7.2 installed)", st["message"])
        self.assertGreater(st["checked_at"], 1.7e12)
        self.assert_staged_ok("1.7.3")
        self.assertEqual(d.get("editor/sdcard/SCRIPTS/TOOLS/StickTime.lua"), zip_member(z, "SCRIPTS/TOOLS/StickTime.lua"))
        # a check installs nothing
        self.assertEqual(d.get(LUA_REL), d.factory_lua)
        self.assertEqual(d.get("editor/index.html"), d.factory_page)
        self.assertEqual(d.slots(), [])
        # the gallery follows the StickTime site
        self.assertEqual(d.get("editor/tracks/index.json"), self.gh.gallery)
        self.assertEqual(self.gh.count(r"^/api/repos/juricabi/sticktime/"), 1)
        self.assertEqual(self.gh.count(r"releases/latest"), 0)
        # cached for 10 minutes: no API call and no second download
        self.gh.clear()
        self.assertEqual(d.run("check", "game"), 10)
        self.assertEqual((self.gh.count(r"^/api/"), self.gh.count(r"^/cdn/")), (0, 0))
        self.assertEqual(d.run("check", "--game", "--no-cache", "--json"), 10)
        self.assertEqual(self.gh.count(r"^/api/"), 1)
        self.assertEqual(json.loads(d.last.stdout)["game"]["latest"], "1.7.3")
        self.assert_consistent()

    def test_check_up_to_date(self):
        self.release("1.7.2")
        d = self.dev
        self.assertEqual(d.run("check", "game"), 0, d.out())
        st = d.status()
        self.assertIn("StickTime 1.7.2 is up to date", st["message"])
        self.assertEqual((st["game"]["latest"], st["game"]["staged"]), ("1.7.2", "1.7.2"))
        self.assert_consistent()

    def test_apply_game(self):
        z = self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.3")
        slots = d.slots()
        self.assertEqual(len(slots), 2, slots)
        self.assertTrue(slots[0].startswith("0000-1.7.2-"), slots)         # the image's game, kept for a rollback
        self.assertEqual(slots[1], "0001-1.7.3-" + hashlib.sha256(z).hexdigest()[:8])
        slot = json.loads(d.get("game/%s/SLOT.json" % slots[1]))
        self.assertEqual((slot["version"], slot["tag"], slot["asset_sha256"], slot["commit"]),
                         ("1.7.3", "v1.7.3", hashlib.sha256(z).hexdigest(), self.releases["1.7.3"][1]))
        self.assertEqual(d.get("game/%s/release.zip" % slots[1]), z)
        st = d.status()
        self.assertEqual((st["state"], st["error"], st["progress"]), ("done", "", 1))
        self.assertEqual((st["game"]["installed"], st["game"]["latest"], st["game"]["staged"]), ("1.7.3", "1.7.3", "1.7.3"))
        self.assertIn("StickTime 1.7.3 is installed", st["message"])
        # READMEs: the player's stays, a missing one comes from the release
        self.assertEqual(d.get("sd/SCRIPTS/TOOLS/StickTimeTracks/README.txt"), d.tracks_readme)
        self.assertEqual(d.get("sd/SCRIPTS/TOOLS/StickTimeGhosts/README.txt"),
                         zip_member(z, "SCRIPTS/TOOLS/StickTimeGhosts/README.txt"))
        # the saves, tracks and ghosts were backed up before the version changed
        b = os.listdir(d.p("backup"))
        self.assertEqual(len(b), 1)
        self.assertTrue(b[0].endswith("-1.7.2-to-1.7.3"), b)
        for rel, data in PLAYER.items():
            self.assertEqual(d.get("backup/%s/%s" % (b[0], rel)), data, rel)
        # the host is asked to reload the game
        self.assertEqual([p for p, _, _ in self.gh.posts], ["/reload"])
        self.assertEqual(self.gh.posts[0][1].get("x-sticktime"), "1")
        self.assertEqual(d.get("editor/tracks/index.json"), self.gh.gallery)
        self.assert_staged_ok("1.7.3")
        self.assertEqual(self.gh.count(r"^/api/repos/juricabi/sticktime/"), 3)     # list, tag, tree
        self.assertEqual(self.gh.count(r"^/cdn/"), 1)
        self.assertEqual(self.gh.count(r"releases/latest"), 0)
        self.assertEqual(self.gh.count(r"/raw/juricabi/sticktime/%s/web/simulator.html" % self.releases["1.7.3"][1]), 1)
        self.assert_consistent()
        # again: up to date, nothing downloaded or written
        self.gh.clear()
        self.assertEqual(d.run("apply", "--game"), 0, d.out())
        self.assertIn("StickTime 1.7.3 is up to date", d.status()["message"])
        self.assertEqual(self.gh.count(r"^/cdn/"), 0)
        self.assertEqual(d.slots(), slots)
        self.assertEqual(self.gh.posts, [])
        self.assert_consistent()

    def test_highest_version_not_github_latest(self):
        now = time.time()
        self.release("1.7.3", created=now)              # what /releases/latest would say (newest commit)
        self.release("1.7.10", created=now - 86400)
        self.release("1.8.0", draft=True)
        self.release("1.9.0", prerelease=True)
        self.gh.add_release(GAME, "v2.0.0", None)        # published, its zip not attached yet
        d = self.dev
        self.assertEqual(d.run("check", "game"), 10, d.out())
        self.assertIn("StickTime 2.0.0 is being published", d.status()["message"])
        self.assertEqual(d.status()["game"]["latest"], "1.7.10")
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.10")
        self.assertEqual(self.gh.count(r"releases/latest"), 0)
        self.assert_consistent()

    def test_only_pending_release(self):
        self.gh.add_release(GAME, "v1.7.4", None)
        d = self.dev
        self.assertEqual(d.run("check", "game"), 0, d.out())
        self.assertIn("StickTime 1.7.4 is being published", d.status()["message"])
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assertEqual(d.status()["state"], "done")
        self.assertEqual(d.get(LUA_REL), d.factory_lua)
        self.gh.remove_release(GAME, "v1.7.4")
        self.assertEqual(d.run("check", "game", "--no-cache"), 0, d.out())
        self.assertIn("No StickTime release", d.status()["message"])
        self.assert_consistent()

    def test_same_version_is_a_noop(self):
        self.release("1.7.2")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assertIn("StickTime 1.7.2 is up to date", d.status()["message"])
        self.assertEqual(d.slots(), [])
        self.assertEqual(d.get(LUA_REL), d.factory_lua)
        self.assertEqual(d.get("editor/index.html"), d.factory_page)
        self.assert_staged_ok("1.7.2")
        # --force reinstalls it from GitHub (no backup: the version doesn't change)
        self.assertEqual(d.run("apply", "game", "--force"), 0, d.out())
        self.assertEqual(len(d.slots()), 2)
        self.assert_game("1.7.2")
        self.assertFalse(d.exists("backup"))
        self.assert_consistent()

    def test_downgrade_refused_unless_forced(self):
        self.release("1.7.1")
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.gh.remove_release(GAME, "v1.7.3")           # the newest release was deleted
        self.assert_failed(d.run("apply", "game", "--no-cache"), 8, "--force")
        self.assert_game("1.7.3")
        # never automatic, and not an error then
        self.assertEqual(d.run("apply", "game", "--auto", "--no-cache"), 0, d.out())
        self.assertIn("newer than the newest release", d.status()["message"])
        self.assert_game("1.7.3")
        self.assert_failed(d.run("apply", "game", "--version", "1.7.1"), 8)
        self.assert_failed(d.run("apply", "game", "--version", "9.9"), 3, "9.9")
        self.assertEqual(d.run("apply", "game", "--version", "1.7.1", "--force"), 0, d.out())
        self.assert_game("1.7.1")
        self.assertEqual(len(os.listdir(d.p("backup"))), 2)
        self.assert_consistent()

    def test_custom_game_is_never_replaced_automatically(self):
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        custom = self.lua("1.7.3") + b"-- my own change\n"          # a dev build copied from a PC
        d.put(LUA_REL, custom)
        self.release("1.7.4")
        self.assertEqual(d.run("check", "game", "--no-cache"), 10, d.out())
        self.assertEqual(d.run("apply", "game", "--auto", "--no-cache", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assertIn("Your own StickTime.lua", d.status()["message"])
        self.assertEqual(d.get(LUA_REL), custom)
        for cmd in ("boot", "repair"):
            self.assertEqual(d.run(cmd), 0, d.out())
            self.assertEqual(d.get(LUA_REL), custom)
        # a manual install replaces it, and keeps a copy in the backup
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.4")
        backups = sorted(os.listdir(d.p("backup")))
        self.assertEqual(d.get("backup/%s/StickTime.lua" % backups[-1]), custom)
        self.assert_consistent()

    def test_custom_copy_put_back_by_apply_of_same_release(self):
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        d.put(LUA_REL, self.lua("1.7.3").replace(b"StickTime v1.7.3", b"StickTime v1.7.3-dev", 1))
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.3")
        self.assertEqual(self.gh.count(r"^/cdn/"), 1)      # from the kept slot, not downloaded again
        self.assert_consistent()

    def test_page_install_of_the_staged_release(self):
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("check", "game"), 10, d.out())
        # the emulator page's Install copies the staged StickTime.lua into sd/
        d.put(LUA_REL, d.get("editor/sdcard/SCRIPTS/TOOLS/StickTime.lua"))
        self.assertEqual(d.run("check", "game"), 0, d.out())
        self.assertEqual(d.status()["game"]["installed"], "1.7.3")
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assertEqual(d.get(LUA_REL), self.lua("1.7.3"))
        # apply makes it a slot, so the editor page follows
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.3")
        self.assert_consistent()

    def test_auto_update_replaces_a_page_installed_release(self):
        # the page's Install put the staged 1.7.3 there (a release, not the player's own file)
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game", "--version", "1.7.3"), 0, d.out())
        self.release("1.7.4")
        self.assertEqual(d.run("check", "game", "--no-cache"), 10, d.out())
        d.put(LUA_REL, d.get("editor/sdcard/SCRIPTS/TOOLS/StickTime.lua"))
        # the boot leaves the player's choice alone
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assertEqual(d.get(LUA_REL), self.lua("1.7.4"))
        # a check stages the next release first: the copy is still known as a release, not the player's own
        self.release("1.7.5")
        self.assertEqual(d.run("check", "game", "--no-cache"), 10, d.out())
        self.assertNotIn("Your own", d.status()["message"])
        self.assertEqual(d.run("apply", "game", "--auto", "--no-cache", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assert_game("1.7.5")
        self.assertEqual(d.status()["game"]["installed"], "1.7.5")
        self.assert_consistent()

    def test_re_release_same_version_new_digest(self):
        z1 = self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        # the tag moved and the zip was replaced, a minute ago
        z2 = game_zip("1.7.3", change={"SCRIPTS/TOOLS/StickTime.lua": lambda b: b + b"-- fixed\n"})
        self.assertNotEqual(z1, z2)
        self.release("1.7.3", zbytes=z2, files={"sdcard/SCRIPTS/TOOLS/StickTime.lua": zip_member(z2, "SCRIPTS/TOOLS/StickTime.lua")},
                     updated=time.time() - 60)
        self.assertEqual(d.run("check", "game", "--no-cache"), 10, d.out())
        self.assertIn("StickTime 1.7.3 is available (1.7.3 installed)", d.status()["message"])
        # auto waits an hour: tags and zips are often replaced right after publishing
        self.assertEqual(d.run("apply", "game", "--auto", "--no-cache"), 0, d.out())
        self.assertIn("less than an hour ago", d.status()["message"])
        self.assertEqual(d.get(LUA_REL), zip_member(z1, "SCRIPTS/TOOLS/StickTime.lua"))
        self.assertEqual(d.run("apply", "game", "--auto", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assert_game("1.7.3")
        self.assertEqual(d.get(LUA_REL), zip_member(z2, "SCRIPTS/TOOLS/StickTime.lua"))
        self.assertEqual(d.slots()[-1], "0002-1.7.3-" + hashlib.sha256(z2).hexdigest()[:8])
        self.assert_consistent()

    def test_new_release_waits_an_hour_in_auto_mode(self):
        self.release("1.7.3", published=time.time() - 120)
        d = self.dev
        self.assertEqual(d.run("apply", "game", "--auto"), 0, d.out())
        self.assertIn("less than an hour ago", d.status()["message"])
        self.assertEqual(d.get(LUA_REL), d.factory_lua)
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.3")

    def test_annotated_tag_and_null_digest(self):
        self.release("1.7.3", annotated=True, digest=None)
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.3")
        self.assertEqual(self.gh.count(r"/git/tags/"), 1)
        # no digest, and the tag's files don't match: refused
        self.release("1.7.4", digest=None, files={"sdcard/SCRIPTS/TOOLS/StickTime.lua": b"something else"})
        self.assert_failed(d.run("apply", "game", "--no-cache"), 5, "checksum")
        # no digest and a truncated tree: nothing to verify against, refused
        self.release("1.7.5", digest=None, truncated_tree=True)
        self.assert_failed(d.run("apply", "game", "--no-cache"), 5)
        self.assert_game("1.7.3")
        self.assert_consistent()

    def test_truncated_tree_with_digest(self):
        self.release("1.7.3", truncated_tree=True)
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.3")


class TestGameRollback(FakeServerCase):
    def test_rollback_and_retention(self):
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        s1 = d.slots()
        self.assertEqual(d.run("rollback", "game"), 0, d.out())
        self.assert_game("1.7.2", "factory")
        self.assertIn("Went back to StickTime 1.7.2", d.status()["message"])
        self.assertTrue(d.exists("game/%s/DISABLED" % s1[1]))
        self.assertEqual(len(self.gh.posts), 2)                      # reloaded twice
        self.assert_consistent()
        self.assert_failed(d.run("rollback", "--game"), 8, "nothing to go back to")
        self.assert_game("1.7.2", "factory")
        # the rolled-back release isn't installed again automatically, but on request it is
        self.assertEqual(d.run("check", "game"), 10, d.out())
        self.assertEqual(d.run("apply", "game", "--auto", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assertIn("rolled back on this Pi", d.status()["message"])
        self.assert_game("1.7.2", "factory")
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.3")
        self.assertEqual([n[:4] for n in d.slots()], ["0000", "0001", "0002"])
        # retention: the image's, the active, the one before and the newest disabled one
        self.release("1.7.4")
        self.assertEqual(d.run("apply", "game", "--no-cache"), 0, d.out())
        self.assertEqual([n[:4] for n in d.slots()], ["0000", "0001", "0002", "0003"])
        self.release("1.7.5")
        self.assertEqual(d.run("apply", "game", "--no-cache"), 0, d.out())
        self.assertEqual([n[:4] for n in d.slots()], ["0000", "0001", "0003", "0004"])
        self.assert_game("1.7.5")
        self.assertLessEqual(len(os.listdir(d.p("backup"))), 5)
        # rollback goes to the one before
        self.assertEqual(d.run("rollback", "game"), 0, d.out())
        self.assert_game("1.7.4")
        self.assert_consistent()

    def test_rollback_from_a_custom_copy(self):
        # a dev build copied from a PC: rollback puts back the release the updater installed
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        custom = self.lua("1.7.3") + b"-- experiment\n"
        d.put(LUA_REL, custom)
        self.assertEqual(d.run("rollback", "game"), 0, d.out())
        self.assert_game("1.7.3")
        self.assertIn("Went back to StickTime 1.7.3", d.status()["message"])
        backups = sorted(os.listdir(d.p("backup")))
        self.assertEqual(d.get("backup/%s/StickTime.lua" % backups[-1]), custom)
        self.assert_consistent()

    def test_game_min_of_the_app(self):
        # compat.json of the installed StickTime Pi: older games are refused, even with --force
        self.release("1.7.3")
        d = self.dev
        d.put("compat.json", b'{"min_image":"0.1.0","game_min":"1.7.4"}')
        self.assert_failed(d.run("apply", "game"), 8, "needs StickTime 1.7.4")
        self.assert_failed(d.run("apply", "game", "--force"), 8, "needs StickTime 1.7.4")
        self.assertEqual(d.run("apply", "game", "--auto", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assertIn("needs StickTime 1.7.4", d.status()["message"])
        self.assertEqual(d.get(LUA_REL), d.factory_lua)
        self.release("1.7.4")
        self.assertEqual(d.run("apply", "game", "--no-cache"), 0, d.out())
        self.assert_game("1.7.4")
        d.put("compat.json", b'{"game_min": "not a version"}')         # ignored
        self.assertEqual(d.run("apply", "game", "--version", "1.7.3", "--force"), 0, d.out())
        self.assert_game("1.7.3")

    def test_rollback_with_nothing_kept(self):
        d = self.dev
        self.assert_failed(d.run("rollback", "game"), 8, "nothing to go back to")
        self.assert_failed(d.run("rollback", "app"), 8, "nothing to go back to")
        self.assertEqual(d.get(LUA_REL), d.factory_lua)
        self.assert_consistent()

    def test_rollback_offline(self):
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assertEqual(d.run("rollback", "game", STICKTIME_GITHUB_API=dead_api(), STICKTIME_GITHUB_RAW=dead_api(),
                               STICKTIME_PAGES=dead_api()), 0, d.out())
        self.assert_game("1.7.2", "factory")


def tweaked_zip(version):
    """a release whose editions changed more than their headers: the Lite's t1.txt gone, a t9.txt new,
    the B&W's game.luac other bytes"""
    return game_zip(version, drop=("SCRIPTS/TOOLS/StickTimeLite/t1.txt",),
                    add={"SCRIPTS/TOOLS/StickTimeLite/t9.txt": b"Track nine\n"},
                    change={"SCRIPTS/TOOLS/StickTimeBW/game.luac": lambda b: b + b"\x00new"})


def no_editions_zip(version):
    """a release with only the color game (as StickTime before its B&W editions)"""
    return game_zip(version, drop=tuple(n for n, _ in MODEL if n.startswith(("SCRIPTS/TOOLS/StickTimeBW", "SCRIPTS/TOOLS/StickTimeLite"))))


class TestGameEditions(FakeServerCase):
    """StickTime.lua, StickTimeBW.lua + StickTimeBW/ and StickTimeLite.lua + StickTimeLite/ are one game:
    installed, updated and rolled back together, and the player's files among them (StickTimeLite/data.txt,
    StickTimeBW.dat) are never touched"""

    BW, LITE = "sd/SCRIPTS/TOOLS/StickTimeBW", "sd/SCRIPTS/TOOLS/StickTimeLite"

    def test_update_and_rollback(self):
        z = self.release("1.7.3", zbytes=tweaked_zip("1.7.3"))
        d = self.dev
        self.assertEqual(sorted(d.factory_editions)[:1], ["StickTimeBW.lua"])
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.3")
        self.assertFalse(d.exists(self.LITE + "/t1.txt"), "a file the release doesn't have goes")
        self.assertEqual(d.get(self.LITE + "/t9.txt"), b"Track nine\n")
        self.assertEqual(d.get(self.BW + "/game.luac"), zip_member(z, "SCRIPTS/TOOLS/StickTimeBW/game.luac"))
        self.assertEqual(d.get(self.LITE + "/data.txt"), PLAYER["StickTimeLite/data.txt"])
        # the slot keeps the editions, the image's game (slot 0000) its own
        snap, new = (json.loads(d.get("game/%s/SLOT.json" % s)) for s in d.slots())
        self.assertEqual(sorted(new["editions"]), ["bw", "lite"])
        self.assertIn("StickTimeLite/t9.txt", new["editions"]["lite"])
        self.assertIn("StickTimeBW.lua", new["editions"]["bw"])
        self.assertEqual(json.loads(d.get("update/editions.json"))["files"]["StickTimeLite/t9.txt"]["sha256"],
                         hashlib.sha256(b"Track nine\n").hexdigest())
        self.assertEqual(sorted(r for rels in snap["editions"].values() for r in rels), sorted(d.factory_editions))
        for m in (snap, new):
            self.assertFalse([r for r in m["files"] if "data.txt" in r.lower() or r.lower().endswith(".dat")])
        # the backup before the version change has the saves, data.txt among them, and no game files
        b = sorted(os.listdir(d.p("backup")))[-1]
        self.assertEqual(d.get("backup/%s/StickTimeLite/data.txt" % b), PLAYER["StickTimeLite/data.txt"])
        self.assertEqual(d.get("backup/%s/StickTimeBW.dat" % b), PLAYER["StickTimeBW.dat"])
        self.assertFalse(d.exists("backup/%s/StickTimeLite/core.lua" % b))
        self.assertFalse(d.exists("backup/%s/StickTimeBW.lua" % b))
        self.assert_consistent()
        # rollback: all three go back to the image's together; data.txt stays the player's
        self.assertEqual(d.run("rollback", "game"), 0, d.out())
        self.assert_game("1.7.2", "factory")
        self.assertTrue(d.exists(self.LITE + "/t1.txt"))
        self.assertFalse(d.exists(self.LITE + "/t9.txt"))
        self.assert_consistent()
        # and on request the update again, then once more back (from a release to a release)
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.3")
        self.release("1.7.4")
        self.assertEqual(d.run("apply", "game", "--no-cache"), 0, d.out())
        self.assert_game("1.7.4")
        self.assertEqual(d.get(self.LITE + "/t1.txt"), editions_of(self.releases["1.7.4"][0])["StickTimeLite/t1.txt"])
        self.assertEqual(d.run("rollback", "game"), 0, d.out())
        self.assert_game("1.7.3")
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assert_game("1.7.3")
        self.assert_consistent()

    def test_an_image_without_editions_and_a_release_without_them(self):
        # an image from before the editions shipped: the first update brings them, a rollback takes them away
        old = Device(os.path.join(self.tmp, "old"), self.gh, editions=False)
        self.assertEqual(old.editions(), {})
        z = self.release("1.7.3")
        self.assertEqual(old.run("apply", "game"), 0, old.out())
        self.assert_game("1.7.3", dev=old)
        self.assertEqual(sorted(old.editions()), sorted(editions_of(z)))
        self.assertEqual(json.loads(old.get("game/%s/SLOT.json" % old.slots()[0]))["editions"], {})
        self.assertEqual(old.run("rollback", "game"), 0, old.out())
        self.assert_game("1.7.2", "factory", dev=old)
        self.assertEqual(old.editions(), {})
        self.assertFalse(old.exists("sd/SCRIPTS/TOOLS/StickTimeBW"), "the emptied folder goes")
        self.assertEqual(os.listdir(old.p("sd/SCRIPTS/TOOLS/StickTimeLite")), ["data.txt"], "the save stays, and its folder")
        self.assert_consistent(old)
        # a release with the color game only: the editions it lacks go too (never data.txt), and come back
        self.release("1.7.4", zbytes=no_editions_zip("1.7.4"))
        d = self.dev
        self.assertEqual(d.run("apply", "game", "--version", "1.7.4"), 0, d.out())
        self.assert_game("1.7.4")
        self.assertEqual(d.editions(), {})
        self.assertEqual(d.get(self.LITE + "/data.txt"), PLAYER["StickTimeLite/data.txt"])
        self.assert_consistent()
        self.assertEqual(d.run("rollback", "game"), 0, d.out())
        self.assert_game("1.7.2", "factory")
        self.assert_consistent()

    def test_own_edition_files(self):
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        mine = b"-- my own B&W build\n" + d.get(self.BW + "/core.lua")
        d.put(self.BW + "/core.lua", mine)
        bw_loader = d.get("sd/SCRIPTS/TOOLS/StickTimeBW.lua")
        # the boot leaves it, and the rest of its edition, as it is
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assertEqual(d.get(self.BW + "/core.lua"), mine)
        # an automatic update installs the new release but leaves that edition whole
        self.release("1.7.4")
        self.assertEqual(d.run("apply", "game", "--auto", "--no-cache", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assertIn("StickTime 1.7.4 is installed", d.status()["message"])
        self.assertIn("The B&W edition stays as it is: it holds files of your own", d.status()["message"])
        self.assertEqual(d.get(LUA_REL), self.lua("1.7.4"))
        self.assertEqual((d.get(self.BW + "/core.lua"), d.get("sd/SCRIPTS/TOOLS/StickTimeBW.lua")), (mine, bw_loader))
        new = editions_of(self.releases["1.7.4"][0])
        self.assertEqual({r: b for r, b in d.editions().items() if r.startswith("StickTimeLite")},
                         {r: b for r, b in new.items() if r.startswith("StickTimeLite")})
        # without the file of one's own the boot puts the release's back
        os.unlink(d.p(self.BW + "/core.lua"))
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assert_game("1.7.4")
        self.assert_consistent()
        # a manual install replaces such a file, and keeps a copy in the backup
        d.put(self.BW + "/core.lua", mine)
        self.release("1.7.5")
        self.assertEqual(d.run("apply", "game", "--no-cache"), 0, d.out())
        self.assert_game("1.7.5")
        b = sorted(os.listdir(d.p("backup")))[-1]
        self.assertEqual(d.get("backup/%s/StickTimeBW/core.lua" % b), mine)
        self.assertFalse(d.exists("backup/%s/StickTimeBW.lua" % b), "a release's file isn't backed up")
        self.assert_consistent()
        # a file of one's own that no release has stays where it is, whatever happens
        d.put(self.LITE + "/notes.txt", b"my notes\n")
        self.assertEqual(d.run("rollback", "game"), 0, d.out())
        self.assertEqual(d.get(self.LITE + "/notes.txt"), b"my notes\n")
        os.unlink(d.p(self.LITE + "/notes.txt"))
        self.assert_game("1.7.4")
        self.assert_consistent()

    def test_editions_from_the_pages_install(self):
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.release("1.7.4")
        self.assertEqual(d.run("check", "game", "--no-cache"), 10, d.out())
        m = self.assert_staged_ok("1.7.4")
        self.assertEqual(sorted(m["editions"]), ["bw", "color", "lite"])
        # the page's Install writes the staged B&W and Lite editions (not the color one)
        staged = {f["path"]: d.get("editor/sdcard/SCRIPTS/TOOLS/" + f["path"]) for f in m["files"] if f["edition"] in ("bw", "lite")}
        for rel, b in staged.items():
            d.put("sd/SCRIPTS/TOOLS/" + rel, b)
        # the boot leaves the player's choice alone
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assertEqual(d.editions(), staged)
        self.assertEqual(d.get(LUA_REL), self.lua("1.7.3"))
        # a newer release staged: those files are still known as a release's, so an automatic update
        # replaces them like ours (no "files of your own", no backup of them)
        self.release("1.7.5")
        self.assertEqual(d.run("check", "game", "--no-cache"), 10, d.out())
        self.assertEqual(d.run("apply", "game", "--auto", "--no-cache", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assertNotIn("of your own", d.status()["message"])
        self.assert_game("1.7.5")
        for b in os.listdir(d.p("backup")):
            self.assertFalse(d.exists("backup/%s/StickTimeBW.lua" % b))
        self.assert_consistent()

    def test_damaged_edition_files(self):
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        slot = d.slots()[-1]
        # the slot's copy damaged (a FAT error): the boot repairs it from the slot's release zip
        p = "game/%s/StickTimeLite/core.lua" % slot
        good = d.get(p)
        d.put(p, good[:-10] + b"\0" * 10)
        # and the copy in sd/ cut short by a power cut
        d.put(self.BW + "/core.lua", d.get(self.BW + "/core.lua")[:100])
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assertEqual(d.get(p), good)
        self.assertIn("repaired StickTime 1.7.3 from its release zip", d.status()["message"])
        self.assert_game("1.7.3")
        self.assert_consistent()
        # the slot's copy damaged and its zip too: the slot isn't used, the image's game is (all three)
        d.put(p, good[:-10] + b"\0" * 10)
        z = d.get("game/%s/release.zip" % slot)
        d.put("game/%s/release.zip" % slot, z[:-50] + b"\0" * 50)
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assertTrue(d.exists("game/%s/DISABLED" % slot))
        self.assert_game("1.7.2", "factory")
        self.assert_consistent()

    def test_slot_editions_are_checked(self):
        # SLOT.json may only list the editions' own game files, each of them in the slot
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        g = self.inspect()[0]
        act = g.active(g.slots())
        self.assertEqual(act.sd_files()[0], "StickTime.lua")
        self.assertEqual(len(act.sd_files()), 1 + len(editions_of(self.releases["1.7.3"][0])))
        path = os.path.join(act.dir, "SLOT.json")
        with open(path, "rb") as f:
            good = json.loads(f.read())
        bad = [
            lambda m: m["editions"].update(color=["StickTime.lua"]),
            lambda m: m["editions"]["lite"].append("StickTimeLite/data.txt"),
            lambda m: m["editions"]["lite"].append("StickTimeLite/nothere.lua"),
            lambda m: m["editions"]["bw"].remove("StickTimeBW.lua"),
            lambda m: m["editions"]["bw"].append("StickTime.lua"),
            lambda m: m.update(editions=["bw"]),
        ]
        for n, change in enumerate(bad):
            with self.subTest(n):
                m = json.loads(json.dumps(good))
                change(m)
                with open(path, "w") as f:
                    json.dump(m, f)
                self.assertFalse(gamemod.Slot(g.cfg.game_dir, act.name).valid)
        with open(path, "w") as f:
            json.dump(good, f)
        self.assertTrue(gamemod.Slot(g.cfg.game_dir, act.name).valid)
        # a slot from before the editions were kept: valid, and the editions on the card are left alone
        m = dict(good)
        del m["editions"]
        with open(path, "w") as f:
            json.dump(m, f)
        d.put(self.BW + "/core.lua", b"-- anything\n")
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assertEqual(d.get(self.BW + "/core.lua"), b"-- anything\n")
        self.assertEqual(d.get(LUA_REL), self.lua("1.7.3"))


class TestGameFailures(FakeServerCase):
    def assert_unchanged(self):
        d = self.dev
        self.assertEqual(d.get(LUA_REL), d.factory_lua)
        self.assertEqual(d.get("editor/index.html"), d.factory_page)
        self.assertEqual(d.slots(), [])
        self.assert_consistent()

    def test_bad_digest(self):
        self.release("1.7.3")
        d = self.dev
        self.gh.fault(r"^/cdn/", corrupt=True, times=2)
        self.assert_failed(d.run("apply", "game"), 5, "checksum", "asset %d" % self.asset_id("1.7.3"))
        self.assertEqual(self.gh.count(r"^/cdn/"), 2)                # tried again once
        self.assertFalse(os.path.exists(os.path.join(d.state, "check-game.json")))   # the list is asked again
        self.assert_unchanged()
        # once only (a CDN cache during a re-upload): the second download is right
        self.gh.clear()
        self.gh.fault(r"^/cdn/", corrupt=True, times=1)
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.3")

    def test_truncated_and_dropped_downloads(self):
        self.release("1.7.3")
        d = self.dev
        self.gh.fault(r"^/cdn/", truncate=1000, times=2)
        self.assert_failed(d.run("apply", "game"), 3, "broke off")
        self.assert_unchanged()
        self.gh.clear()
        self.gh.fault(r"^/cdn/", drop=True, times=2)
        self.assert_failed(d.run("apply", "game"), 3)
        self.assert_unchanged()
        self.assertEqual(os.listdir(os.path.join(d.state, "dl")) if os.path.isdir(os.path.join(d.state, "dl")) else [], [])
        self.gh.clear()
        self.gh.fault(r"^/cdn/", truncate=1000, times=1)
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_game("1.7.3")

    def test_bad_zips(self):
        d = self.dev
        cases = [
            ("zip slip", game_zip("1.7.3", add={"SCRIPTS/TOOLS/../../evil.lua": b"x"})),
            ("absolute path", game_zip("1.7.3", add={"/etc/evil": b"x"})),
            ("backslashes", zip_raw_name("SCRIPTS\\TOOLS\\..\\..\\evil.lua").replace(b"v1.7.2", b"v1.7.3")),
            ("a save in the release", game_zip("1.7.3", add={"SCRIPTS/TOOLS/StickTime.dat": b"x"})),
            ("a track in the release", game_zip("1.7.3", add={"SCRIPTS/TOOLS/StickTimeTracks/c3.txt": b"x"})),
            ("header says 1.7.2", game_zip("1.7.2")),
            ("no StickTime.lua", game_zip("1.7.3", drop=("SCRIPTS/TOOLS/StickTime.lua",))),
            ("not a zip", b"<!doctype html><title>not a zip</title>"),
            ("cut short", game_zip("1.7.3")[:-300]),
        ]
        for why, z in cases:
            with self.subTest(why):
                self.gh.reset()
                self.gh.gallery = gallery(3)
                self.release("1.7.3", zbytes=z)
                self.assert_failed(d.run("apply", "game", "--no-cache"), 5, "asset %d" % self.asset_id("1.7.3"))
                self.assertEqual(d.status()["error"], d.status()["message"])
                self.assert_unchanged()
                for root, _, names in os.walk(self.tmp):
                    self.assertNotIn("evil.lua", names)
                    self.assertNotIn("evil", names)
                dl = os.path.join(d.state, "dl")
                self.assertEqual(os.listdir(dl) if os.path.isdir(dl) else [], [], "the bad download stays")
                # a check doesn't stage it either
                self.assertEqual(d.run("check", "game", "--no-cache"), 10, d.out())
                self.assertIn("Couldn't prepare it for the editor's Install", d.status()["message"])
                self.assertFalse(d.exists("editor/sdcard/manifest.json"))

    def test_offline(self):
        self.release("1.7.3")
        d = self.dev
        self.assert_failed(d.run("check", "game", STICKTIME_GITHUB_API=dead_api()), 3, "Couldn't connect")
        self.assert_failed(d.run("apply", "game", STICKTIME_GITHUB_API="https://sticktime-test.invalid/api"), 3, "No internet")
        self.assert_unchanged()

    def test_rate_limit(self):
        self.release("1.7.3")
        d = self.dev
        self.gh.fault(r"^/api/", ratelimit=True, times=1)
        st = self.assert_failed(d.run("check", "game"), 4, "limit", "Try again after")
        with open(os.path.join(d.state, "ratelimit.json"), "rb") as f:
            rl = json.loads(f.read())
        self.assertGreater(rl["reset"], time.time() + 800)
        self.assertIn(time.strftime("%H:%M", time.localtime(rl["reset"])), st["message"])
        # until then, no request goes to the API (the limit is shared by the whole home network)
        self.gh.clear()
        self.assert_failed(d.run("check", "game", "--no-cache"), 4)
        self.assert_failed(d.run("apply", "game"), 4)
        self.assertEqual(self.gh.count(r"^/api/"), 0)
        self.assert_unchanged()
        # once the reset time has passed, it asks again
        with open(os.path.join(d.state, "ratelimit.json"), "w") as f:
            json.dump({"reset": int(time.time()) - 1}, f)
        self.assertEqual(d.run("check", "game"), 10, d.out())
        # a secondary limit: 403 + Retry-After without remaining 0
        self.gh.fault(r"^/api/", status=403, headers={"Retry-After": "120", "X-RateLimit-Remaining": "37"},
                      body=b'{"message":"You have exceeded a secondary rate limit."}', times=1)
        self.assert_failed(d.run("check", "game", "--no-cache"), 4)

    def test_rate_limit_in_the_middle_of_an_install(self):
        # the list costs 1 call, the install 2 more (tag, tree): the limit runs out after the download
        self.release("1.7.3")
        d = self.dev
        for quota in (1, 2):
            with self.subTest(quota=quota):
                try:
                    os.unlink(os.path.join(d.state, "ratelimit.json"))
                except OSError:
                    pass
                self.gh.clear()
                self.gh.api_quota = quota
                self.assert_failed(d.run("apply", "game", "--no-cache"), 4, "Try again after")
                # downloaded once: the verified zip is kept in /run for the next try
                self.assertEqual(self.gh.count(r"^/cdn/"), 1 if quota == 1 else 0)
                self.assert_unchanged()
        os.unlink(os.path.join(d.state, "ratelimit.json"))
        self.gh.clear()
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assertEqual(self.gh.count(r"^/cdn/"), 0)
        self.assert_game("1.7.3")

    def test_raw_and_cdn_429_are_retried(self):
        self.release("1.7.3")
        d = self.dev
        self.gh.fault(r"^/raw/", status=429, headers={"Retry-After": "1", "Content-Type": "text/plain"},
                      body=b"429: This request was rate-limited", times=1)
        self.gh.fault(r"^/cdn/", status=429, headers={"Retry-After": "1"}, times=1)
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assertEqual(self.gh.count(r"^/raw/"), 2)
        self.assertEqual(self.gh.count(r"^/cdn/"), 2)
        self.assert_game("1.7.3")
        self.assertFalse(os.path.exists(os.path.join(d.state, "ratelimit.json")))   # not the API's limit

    def test_server_answers(self):
        self.release("1.7.3")
        d = self.dev
        cases = [
            (dict(delay=3), 3, "didn't answer in time"),
            (dict(html=True), 3, "web page instead"),
            (dict(status=500), 3, "GitHub had a problem"),
            (dict(status=200, body=b"{not json"), 3, "broken JSON"),
            (dict(status=200, body=b'{"message":"not a list"}'), 3, "looks wrong"),
            (dict(status=200, body=b"[" + b" " * (3 << 20) + b"]"), 5, "more than"),
            (dict(status=404), 3, "Not found"),
            (dict(status=418), 3, "418"),
        ]
        for fault, rc, words in cases:
            with self.subTest(fault=list(fault)):
                self.gh.clear()
                self.gh.fault(r"/releases\?", **fault)
                self.assert_failed(d.run("check", "game", "--no-cache", STICKTIME_HTTP_TIMEOUT="1"), rc, words)
        self.assert_unchanged()

    def test_redirects_elsewhere_are_refused(self):
        self.release("1.7.3")
        d = self.dev
        for target in ("https://evil.example/x.zip", "http://github.com/x.zip", "https://github.com.evil.example/x",
                       "file:///etc/passwd"):
            with self.subTest(target):
                self.gh.clear()
                self.gh.fault(r"^/dl/", redirect=target)
                self.assert_failed(d.run("apply", "game"), 3)
        self.assert_unchanged()

    def test_disk_full_precheck(self):
        self.release("1.7.3")
        d = self.dev
        self.assert_failed(d.run("apply", "game", STICKTIME_UPDATE_FREE_MB="5"), 6, "free")
        self.assert_unchanged()
        # check still works; staging is skipped with a note
        self.assertEqual(d.run("check", "game", STICKTIME_UPDATE_FREE_MB="5"), 10, d.out())
        self.assertIn("Couldn't prepare it", d.status()["message"])
        self.assertFalse(d.exists("editor/sdcard"))

    def test_read_only_card(self):
        self.release("1.7.3")
        d = self.dev
        self.assert_failed(d.run("apply", "game", STICKTIME_UPDATE_EROFS="1"), 6, "read-only", "Reboot")
        self.assert_unchanged()
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assert_failed(d.run("rollback", "game", STICKTIME_UPDATE_EROFS="1"), 6, "read-only")
        self.assert_game("1.7.3")

    def test_page_missing_in_release(self):
        self.release("1.7.3", page=False)
        d = self.dev
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assertIn("no track editor page", d.status()["message"])
        self.assertEqual(d.get(LUA_REL), self.lua("1.7.3"))
        self.assertEqual(d.get("editor/index.html"), d.factory_page)
        self.assert_consistent()

    def test_page_tampered(self):
        self.release("1.7.3")
        d = self.dev
        self.gh.fault(r"^/raw/", corrupt=True)
        self.assert_failed(d.run("apply", "game"), 5, "doesn't match")
        self.assert_unchanged()

    def test_page_wrong_version_or_unpatchable(self):
        d = self.dev
        self.release("1.7.3", page=sim_html("1.7.2"))
        self.assertEqual(d.run("apply", "game"), 0, d.out())
        self.assertIn("isn't usable", d.status()["message"])
        self.assertEqual(d.get(LUA_REL), self.lua("1.7.3"))
        self.assertEqual(d.get("editor/index.html"), d.factory_page)
        odd = b"<!doctype html>\n<html><head></head><body>StickTime v1.7.4 without the editor's hooks</body></html>\n"
        self.release("1.7.4", page=odd)
        self.assertEqual(d.run("apply", "game", "--no-cache"), 0, d.out())
        self.assertIn("couldn't be prepared", d.status()["message"])
        self.assertEqual(d.get("editor/index.html"), d.factory_page)
        self.assertEqual(d.get("game/%s/simulator.html" % d.slots()[-1]), odd)      # kept for a newer patcher
        self.assert_consistent()

    def test_patcher_missing_then_back(self):
        self.release("1.7.3")
        d = self.dev
        nowhere = os.path.join(self.tmp, "no-patch_editor.py")
        self.assertEqual(d.run("apply", "game", STICKTIME_PATCH_EDITOR=nowhere), 0, d.out())
        self.assertIn("couldn't be prepared", d.status()["message"])
        self.assertEqual(d.get(LUA_REL), self.lua("1.7.3"))
        self.assertEqual(d.get("editor/index.html"), d.factory_page)
        # the next boot (with patch_editor.py there, e.g. after an app update) makes the page
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assert_game("1.7.3")
        self.assert_consistent()

    def test_patcher_from_the_apps_lib(self):
        # no STICKTIME_PATCH_EDITOR: the installed app's lib/patch_editor.py first, else the repo's
        self.release("1.7.3")
        d = self.dev
        d.put("lib/patch_editor.py", b"def patch(html):\n    return html.replace('</head>', '<!-- lib -->\\n</head>', 1)\n")
        self.assertEqual(d.run("apply", "game", STICKTIME_PATCH_EDITOR=""), 0, d.out())
        self.assertIn(b"<!-- lib -->", d.get("editor/index.html"))
        # the app's lib/ is gone again (a rollback): the next boot makes the page with the other one
        os.unlink(d.p("lib/patch_editor.py"))
        self.assertEqual(d.run("boot", STICKTIME_PATCH_EDITOR=""), 0, d.out())
        self.assert_game("1.7.3")

    def test_gallery_problems(self):
        d = self.dev
        factory = d.get("editor/tracks/index.json")
        for n, (g, ctype) in enumerate([(b"<html>", "text/html"), (None, None), (b'{"tracks":[]}', None),
                                        (json.dumps({"tracks": [{"text": "x" * 6001}]}).encode(), None),
                                        (gallery(3), "text/html; charset=utf-8")]):
            with self.subTest(n):
                self.gh.gallery = g
                self.gh.gallery_type = ctype or "application/json; charset=utf-8"
                self.release("1.7.%d" % (3 + n))
                self.assertEqual(d.run("apply", "game", "--no-cache"), 0, d.out())
                self.assertEqual(d.get("editor/tracks/index.json"), factory)
                self.assert_game("1.7.%d" % (3 + n))
        self.gh.gallery, self.gh.gallery_type = gallery(5, "new"), "application/json"
        self.assertEqual(d.run("check", "game"), 0, d.out())
        self.assertEqual(d.get("editor/tracks/index.json"), self.gh.gallery)


def small_zip(version):
    """a release with few files (the loops below run the updater once per write)"""
    return game_zip(version, raw_entries=_synthetic_entries())


class TestGamePowerCuts(FakeServerCase):
    """os._exit() at every step of an install (a power cut or a kill), then the boot repair: the card
    holds the old game or the new one, never a mix, and a second apply finishes the job"""

    OLD, NEW = "old", "new"
    FIRST = [("after_download", OLD), ("after_verify", OLD), ("slot_partial", OLD), ("before_slot_json", OLD),
             ("after_slot_json", OLD), ("after_snapshot", OLD),          # the first three hit the snapshot slot
             ("after_backup", NEW), ("reconcile_editions", NEW), ("reconcile_lua", NEW), ("reconcile_page", NEW),
             ("after_reconcile", NEW), ("stage_cleared", NEW), ("stage_files", NEW), ("after_stage", NEW)]
    # (reconcile_editions: the first edition file is written, StickTime.lua is still the old one)
    LATER = [("after_download", OLD), ("after_verify", OLD), ("slot_partial", OLD), ("before_slot_json", OLD),
             ("after_slot_json", NEW), ("after_backup", NEW), ("reconcile_editions", NEW), ("reconcile_lua", NEW),
             ("reconcile_page", NEW), ("after_reconcile", NEW), ("stage_cleared", NEW), ("stage_files", NEW),
             ("after_stage", NEW)]

    def cut(self, name, point, prepare=None, cmd=("apply", "game")):
        dv = Device(os.path.join(self.tmp, name), self.gh)
        if prepare:
            prepare(dv)
        self.gh.clear()
        rc = dv.run(*cmd, STICKTIME_UPDATE_CRASH_AT=point)
        self.assertEqual(rc, 99, "%s: no cut (%s)" % (point, dv.out()))
        self.assertEqual(dv.run_fast("boot"), 0, dv.out())
        return dv

    def test_first_update(self):
        self.release("1.7.3")
        for point, want in self.FIRST:
            with self.subTest(point):
                dv = self.cut("f-" + point, point)
                self.assert_consistent(dv)
                if want == self.OLD:
                    self.assert_game("1.7.2", "factory", dev=dv)
                else:
                    self.assert_game("1.7.3", dev=dv)
                self.assertEqual(dv.run_fast("apply", "game"), 0, dv.out())
                self.assert_game("1.7.3", dev=dv)
                self.assert_consistent(dv)

    def test_later_update(self):
        self.release("1.7.3")
        self.release("1.7.4")
        for point, want in self.LATER:
            with self.subTest(point):
                # 1.7.3 installed (so slots exist), then the cut while going to 1.7.4
                dv = Device(os.path.join(self.tmp, "l-" + point), self.gh)
                self.assertEqual(dv.run_fast("apply", "game", "--version", "1.7.3"), 0, dv.out())
                self.gh.clear()
                self.assertEqual(dv.run("apply", "game", "--no-cache", STICKTIME_UPDATE_CRASH_AT=point), 99, dv.out())
                self.assertEqual(dv.run_fast("boot"), 0, dv.out())
                self.assert_consistent(dv)
                self.assert_game("1.7.3" if want == self.OLD else "1.7.4", dev=dv)
                self.assertEqual(dv.run_fast("apply", "game"), 0, dv.out())
                self.assert_game("1.7.4", dev=dv)
                self.assert_consistent(dv)

    def test_rollback_cut(self):
        self.release("1.7.3")

        def installed(dv):
            self.assertEqual(dv.run_fast("apply", "game"), 0, dv.out())
        for point in ("rollback_marked", "reconcile_editions", "reconcile_lua"):
            with self.subTest(point):
                dv = self.cut("rb-" + point, point, installed, ("rollback", "game"))
                self.assert_consistent(dv)
                self.assert_game("1.7.2", "factory", dev=dv)

    def test_check_staging_cut(self):
        self.release("1.7.3")
        for point in ("stage_cleared", "stage_files"):
            with self.subTest(point):
                dv = self.cut("c-" + point, point, cmd=("check", "game"))
                self.assert_consistent(dv)
                self.assert_game("1.7.2", "factory", dev=dv)
                self.assertFalse(dv.exists("editor/sdcard/manifest.json"))     # nothing to restage from yet
                self.assertEqual(dv.run_fast("check", "game"), 10, dv.out())
                self.assert_staged_ok("1.7.3", dev=dv)

    def test_cut_while_staging_after_an_install_restages_the_active_release(self):
        self.release("1.7.3")

        def installed(dv):
            self.assertEqual(dv.run_fast("apply", "game"), 0, dv.out())
            self.release("1.7.4")
        dv = self.cut("s", "stage_files", installed, ("check", "game", "--no-cache"))
        self.assert_consistent(dv)
        self.assert_staged_ok("1.7.3", dev=dv)


class TestGameDiskFull(FakeServerCase):
    """ENOSPC at every single write of an install: once (the space came back, so the cleanup works) and
    for good (then the boot repair tidies up). The card always holds one whole game."""

    def loop(self, once):
        z = self.release("1.7.3", zbytes=small_zip("1.7.3"))
        new = zip_member(z, "SCRIPTS/TOOLS/StickTime.lua")
        n, failed = 0, 0
        while True:
            # (the small game's editions on the card too: every write of theirs is hit as well)
            dv = Device(os.path.join(self.tmp, "%s%d" % ("o" if once else "s", n)), self.gh, factory_zip=small_zip("1.7.2"))
            env = {"STICKTIME_UPDATE_ENOSPC_AFTER": str(n)}
            if once:
                env["STICKTIME_UPDATE_ENOSPC_ONCE"] = "1"
            rc = dv.run_fast("apply", "game", **env)
            hit = "No space left" in dv.out() or rc == 6
            self.assertIn(rc, (0, 6), dv.out())
            if rc == 6:
                failed += 1
                st = dv.status()
                self.assertEqual(st["state"], "error")
                self.assertIn("full", st["message"])
            if not once:
                self.assertEqual(dv.run_fast("boot"), 0, dv.out())
            self.assert_consistent(dv)
            self.assertEqual(dv.get(LUA_REL), new if rc == 0 else dv.factory_lua, "write %d" % (n + 1))
            if rc == 0 and not hit:
                break
            n += 1
            self.assertLess(n, 300)
        self.assertGreater(failed, 10)

    def test_enospc_once(self):
        self.loop(True)

    def test_enospc_for_good(self):
        self.loop(False)


# ---------------------------------------------------------------- the app, end to end

class TestApp(FakeServerCase):
    def test_apply_and_rollback(self):
        t = self.app_release("0.2.0", sums=True)
        d = self.dev
        self.assertEqual(d.run("check", "app"), 10, d.out())
        self.assertEqual(d.status()["app"]["latest"], "0.2.0")
        self.assertEqual(d.run("apply", "app"), 0, d.out())
        self.assert_app("0.2.0")
        self.assertEqual(d.restarts(), 1)
        st = d.status()
        self.assertEqual((st["state"], st["app"]["installed"], st["app"]["latest"]), ("done", "0.2.0", "0.2.0"))
        self.assertTrue(st["app"]["notes"].startswith("### New in 0.2.0"))
        # the previous set is kept for a rollback
        self.assertEqual(d.get("bin.prev/sticktime-pi"), fake_elf(payload=b"host 0.1.0"))
        self.assertEqual(d.get("web.prev/index.html"), b"<!doctype html><title>StickTime Pi 0.1.0</title>")
        self.assertEqual(d.get("VERSION.prev"), b"0.1.0\n")
        self.assertEqual(self.gh.count(r"^/cdn/"), 2)                  # the archive and the SHA256SUMS asset
        self.assert_consistent()
        # up to date: nothing downloaded, no restart
        self.gh.clear()
        self.assertEqual(d.run("apply", "app"), 0, d.out())
        self.assertIn("up to date", d.status()["message"])
        self.assertEqual((self.gh.count(r"^/cdn/"), d.restarts()), (0, 1))
        self.assertEqual(d.run("check", "app"), 0, d.out())
        # rollback swaps the sets back
        self.assertEqual(d.run("rollback", "app"), 0, d.out())
        self.assert_app("0.1.0")
        self.assertEqual(d.restarts(), 2)
        self.assertIn("Went back to StickTime Pi 0.1.0", d.status()["message"])
        self.assert_consistent()
        # a rolled-back release isn't reinstalled automatically, but on request it is
        self.assertEqual(d.run("check", "app", "--no-cache"), 10, d.out())
        self.assertEqual(d.run("apply", "app", "--auto", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assertIn("rolled back on this Pi", d.status()["message"])
        self.assert_app("0.1.0")
        self.assertEqual(d.run("apply", "app"), 0, d.out())
        self.assert_app("0.2.0")
        self.assertEqual(d.restarts(), 3)
        self.assert_consistent()
        self.assertEqual(t, self.apps["0.2.0"])

    def test_fonts_kept_unless_the_release_has_them(self):
        d = self.dev
        d.put("fonts/Roboto-Regular.ttf", b"font 0.1.0")
        # a release without fonts/ leaves the card's alone, and so does its rollback
        self.app_release("0.2.0", sums=True)
        self.assertEqual(d.run("apply", "app"), 0, d.out())
        self.assert_app("0.2.0")
        self.assertEqual(d.get("fonts/Roboto-Regular.ttf"), b"font 0.1.0")
        self.assertFalse(d.exists("fonts.prev"))
        self.assertEqual(d.run("rollback", "app"), 0, d.out())
        self.assert_app("0.1.0")
        self.assertEqual(d.get("fonts/Roboto-Regular.ttf"), b"font 0.1.0")
        self.assert_consistent()
        # a release with fonts/ swaps them like web/
        self.app_release("0.3.0", sums=True, tgz=app_tgz("0.3.0", extra={"fonts/Roboto-Regular.ttf": b"font 0.3.0"}))
        self.assertEqual(d.run("check", "app", "--no-cache"), 10, d.out())
        self.assertEqual(d.run("apply", "app"), 0, d.out())
        self.assert_app("0.3.0")
        self.assertEqual(d.get("fonts.prev/Roboto-Regular.ttf"), b"font 0.1.0")
        self.assertEqual(d.run("rollback", "app"), 0, d.out())
        self.assertEqual(d.get("fonts/Roboto-Regular.ttf"), b"font 0.1.0")
        self.assert_consistent()

    def test_private_repo_and_token(self):
        self.release("1.7.3")
        self.app_release("0.2.0", private=True, token=TOKEN)
        d = self.dev
        self.assert_failed(d.run("apply", "app"), 9, "github_token")
        # remembered: the next check costs no API call
        self.gh.clear()
        self.assert_failed(d.run("check", "app"), 9, "github_token")
        self.assertEqual(self.gh.count(r"^/api/"), 0)
        # check all: the game is still checked, the app's problem is only a note
        self.assertEqual(d.run("check"), 10, d.out())
        self.assertIn("github_token", d.status()["message"])
        self.assertEqual(d.status()["state"], "done")
        d.write_settings(token="github_pat_WRONG")
        self.assert_failed(d.run("apply", "app"), 9, "rejected")
        d.write_settings(token=TOKEN)
        self.gh.clear()
        self.assertEqual(d.run("apply", "app"), 0, d.out())
        self.assert_app("0.2.0")
        # the token goes to the API only: the asset comes through the API and its redirect drops it
        api = self.gh.headers_of(r"^/api/repos/juricabi/sticktime-pi/")
        self.assertTrue(api and all(h.get("authorization") == "Bearer " + TOKEN for h in api))
        self.assertEqual(self.gh.count(r"^/api/repos/juricabi/sticktime-pi/releases/assets/"), 1)
        cdn = self.gh.headers_of(r"^/cdn/")
        self.assertTrue(cdn and all("authorization" not in h for h in cdn))
        # never to the game's public repo, and never written anywhere
        self.gh.clear()
        self.assertEqual(d.run("check", "--no-cache"), 10, d.out())
        game = self.gh.headers_of(r"^/api/repos/juricabi/sticktime/") + self.gh.headers_of(r"^/(raw|pages|dl|cdn)/")
        self.assertTrue(game and all("authorization" not in h for h in game))
        self.assertNotIn(TOKEN, d.out())
        for root, _, names in os.walk(self.tmp):
            for n in names:
                if n != "sticktime.txt":
                    with open(os.path.join(root, n), "rb") as f:
                        self.assertNotIn(TOKEN.encode(), f.read(), n)

    def test_token_expiry_warning(self):
        self.app_release("0.2.0", private=True, token=TOKEN)
        self.gh.token_expiry = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(time.time() + 3 * 86400))
        d = self.dev
        d.write_settings(token=TOKEN)
        self.assertEqual(d.run("check", "app"), 10, d.out())
        self.assertIn("The GitHub token expires on", d.status()["message"])

    def test_bad_packages(self):
        d = self.dev

        def link(name="web/link", typ=tarfile.SYMTYPE, target="/etc/passwd"):
            ti = tarfile.TarInfo(name)
            ti.type, ti.linkname = typ, target
            return [(ti, None)]
        cases = [
            ("SHA256SUMS mismatch", app_tgz("0.2.0", tweak=lambda f: f.update({"web/app.js": b"changed"})), None),
            ("a file not in SHA256SUMS", app_tgz("0.2.0", tweak=lambda f: f.update({"web/x.js": b"x"})), None),
            ("x86-64 binary", app_tgz("0.2.0", elf=fake_elf(machine=62)), None),
            ("a symlink", app_tgz("0.2.0", members=link()), None),
            ("a hard link", app_tgz("0.2.0", members=link("web/h", tarfile.LNKTYPE, "VERSION")), None),
            ("path out of the folder", app_tgz("0.2.0", members=[(tarfile.TarInfo("web/../../x"), b"")]), None),
            ("VERSION says 0.3.0", app_tgz("0.3.0"), None),
            ("not gzip", b"\x1f\x8bnot really", None),
            ("the release's SHA256SUMS disagrees", None, ("0" * 64 + "  sticktime-pi-app-0.2.0.tar.gz\n").encode()),
        ]
        for why, tgz, sums in cases:
            with self.subTest(why):
                self.gh.reset()
                self.app_release("0.2.0", tgz=tgz, sums=sums)
                self.assert_failed(d.run("apply", "app", "--no-cache"), 5)
                self.assert_app("0.1.0")
                self.assertEqual(d.restarts(), 0)
                self.assert_consistent()
        # a bad digest twice: refused; once: fine
        self.gh.reset()
        self.app_release("0.2.0")
        self.gh.fault(r"^/cdn/", corrupt=True, times=2)
        self.assert_failed(d.run("apply", "app", "--no-cache"), 5, "checksum")
        self.assert_app("0.1.0")
        self.gh.clear()
        self.gh.fault(r"^/cdn/", truncate=500, times=1)
        self.assertEqual(d.run("apply", "app"), 0, d.out())
        self.assert_app("0.2.0")

    def test_image_too_old(self):
        d = self.dev
        with open(os.path.join(d.root, "image-version"), "w") as f:
            f.write("0.1.0\n")
        self.app_release("0.2.0", tgz=app_tgz("0.2.0", compat={"min_image": "0.5.0", "game_min": "1.7.0"}))
        self.assert_failed(d.run("apply", "app"), 8, "newer SD card image", "0.5.0")
        self.assert_app("0.1.0")
        with open(os.path.join(d.root, "image-version"), "w") as f:
            f.write("0.5.0\n")
        self.assertEqual(d.run("apply", "app"), 0, d.out())
        self.assert_app("0.2.0")
        self.assertEqual(json.loads(d.get("compat.json"))["min_image"], "0.5.0")
        self.assert_consistent()

    def test_versions(self):
        d = self.dev
        self.app_release("0.1.0", tgz=app_tgz("0.1.0"))
        self.assertEqual(d.run("apply", "app"), 0, d.out())                   # same version: nothing
        self.assertIn("up to date", d.status()["message"])
        self.assertEqual(d.restarts(), 0)
        self.gh.reset()
        self.app_release("0.0.9")
        self.assert_failed(d.run("apply", "app", "--no-cache"), 8, "--force")
        self.assertEqual(d.run("apply", "app", "--auto", "--no-cache", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assertIn("newer than the newest release", d.status()["message"])
        self.assert_app("0.1.0")
        self.assertEqual(d.run("apply", "app", "--force"), 0, d.out())
        self.assert_app("0.0.9")
        self.app_release("0.3", tgz=app_tgz("0.3"))
        self.assertEqual(d.run("apply", "app", "--version", "0.3.0"), 0, d.out())
        self.assert_app("0.3")

    def test_auto_restart_waits_for_the_menu(self):
        d = self.dev
        self.app_release("0.2.0")
        self.gh.host_state = "fly"
        self.assertEqual(d.run("apply", "app", "--auto", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assert_app("0.2.0")
        self.assertEqual(d.restarts(), 0)
        self.assertIn("next time StickTime Pi restarts", d.status()["message"])
        self.gh.host_state = "menu"
        self.app_release("0.3.0")
        self.assertEqual(d.run("apply", "app", "--auto", "--no-cache", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assertEqual(d.restarts(), 1)
        # a click on Install always restarts
        self.gh.host_state = "fly"
        self.app_release("0.4.0")
        self.assertEqual(d.run("apply", "app", "--no-cache"), 0, d.out())
        self.assertEqual(d.restarts(), 2)
        self.assert_consistent()

    def test_damaged_staging_is_swapped_back(self):
        d = self.dev
        self.app_release("0.2.0")
        self.assertEqual(d.run("apply", "app", STICKTIME_UPDATE_CRASH_AT="app_journal"), 99, d.out())
        stage = [n for n in os.listdir(d.p("update/staging"))][0]
        d.put("update/staging/%s/web/app.js" % stage, b"damaged by the power cut")
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assert_app("0.1.0")
        self.assertIn("damaged while installing", d.status()["message"])
        self.assertFalse(d.exists("bin.prev"))                 # the damaged set is gone, not kept for a rollback
        self.assert_consistent()
        self.assertEqual(d.run("apply", "app", "--auto", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assert_app("0.1.0")
        self.assertEqual(d.run("apply", "app"), 0, d.out())
        self.assert_app("0.2.0")

    def test_failures(self):
        d = self.dev
        self.app_release("0.2.0")
        self.assert_failed(d.run("apply", "app", STICKTIME_GITHUB_API=dead_api()), 3)
        self.assert_failed(d.run("apply", "app", STICKTIME_UPDATE_FREE_MB="1"), 6, "free")
        self.assert_failed(d.run("apply", "app", STICKTIME_UPDATE_EROFS="1"), 6, "read-only")
        self.gh.fault(r"^/api/", ratelimit=True, times=1)
        self.assert_failed(d.run("apply", "app", "--no-cache"), 4, "Try again after")
        self.assert_app("0.1.0")
        self.assertEqual(d.restarts(), 0)
        self.assert_consistent()


class TestAppPowerCuts(FakeServerCase):
    def test_apply_cut_at_every_step(self):
        self.app_release("0.2.0")
        points = ["app_verified", "app_staged", "app_journal"] + ["app_step_%d" % i for i in range(12)] + \
            ["app_swapped", "app_done"]
        hit = 0
        for point in points:
            with self.subTest(point):
                dv = Device(os.path.join(self.tmp, point), self.gh)
                rc = dv.run("apply", "app", STICKTIME_UPDATE_CRASH_AT=point)
                if rc == 0 and point.startswith("app_step_"):
                    continue                        # fewer steps than that
                self.assertEqual(rc, 99, dv.out())
                hit += 1
                self.assertEqual(dv.run_fast("boot"), 0, dv.out())
                self.assert_consistent(dv)
                if point in ("app_verified", "app_staged"):
                    self.assert_app("0.1.0", dev=dv)
                    self.assertEqual(dv.restarts(), 0)
                else:
                    # the journal was there: the boot finished the swap and restarted the service
                    self.assert_app("0.2.0", dev=dv)
                    self.assertEqual(dv.restarts(), 0 if point == "app_done" else 1)
                self.assertEqual(dv.run_fast("apply", "app"), 0, dv.out())
                self.assert_app("0.2.0", dev=dv)
                self.assert_consistent(dv)
        self.assertGreaterEqual(hit, 3 + 8 + 2)

    def test_killed_run_is_finished_by_the_next_command(self):
        # no reboot after a kill: the next command (not only the boot) finishes the swap first
        self.app_release("0.2.0")
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("apply", "app", STICKTIME_UPDATE_CRASH_AT="app_step_3"), 99, d.out())
        self.assertEqual(d.run("check", "game"), 10, d.out())
        self.assert_app("0.2.0")
        self.assertEqual(d.restarts(), 1)
        self.assert_consistent()

    def test_rollback_cut_at_every_step(self):
        self.app_release("0.2.0")
        for point in ["app_rollback_journal"] + ["app_step_%d" % i for i in range(16)]:
            with self.subTest(point):
                dv = Device(os.path.join(self.tmp, point), self.gh)
                self.assertEqual(dv.run_fast("apply", "app"), 0, dv.out())
                rc = dv.run("rollback", "app", STICKTIME_UPDATE_CRASH_AT=point)
                if rc == 0 and point.startswith("app_step_"):
                    continue
                self.assertEqual(rc, 99, dv.out())
                self.assertEqual(dv.run_fast("boot"), 0, dv.out())
                self.assert_consistent(dv)
                self.assert_app("0.1.0", dev=dv)


class TestAppDiskFull(FakeServerCase):
    def loop(self, once):
        self.app_release("0.2.0")
        n, failed = 0, 0
        while True:
            dv = Device(os.path.join(self.tmp, "%s%d" % ("o" if once else "s", n)), self.gh)
            env = {"STICKTIME_UPDATE_ENOSPC_AFTER": str(n)}
            if once:
                env["STICKTIME_UPDATE_ENOSPC_ONCE"] = "1"
            rc = dv.run_fast("apply", "app", **env)
            self.assertIn(rc, (0, 6), dv.out())
            hit = "No space left" in dv.out() or rc == 6
            failed += rc == 6
            if not once:
                self.assertEqual(dv.run_fast("boot"), 0, dv.out())
            self.assert_consistent(dv)
            v = dv.get("VERSION")
            self.assertIn(v, (b"0.1.0\n", b"0.2.0\n"))
            if rc == 0:
                self.assert_app("0.2.0", dev=dv)
            elif once:
                self.assert_app("0.1.0", dev=dv)       # the staging went, nothing was swapped
            if rc == 0 and not hit:
                break
            n += 1
            self.assertLess(n, 200)
        self.assertGreater(failed, 5)

    def test_enospc_once(self):
        self.loop(True)

    def test_enospc_for_good(self):
        self.loop(False)


# ---------------------------------------------------------------- the command line, the lock, the boot

class TestCommands(FakeServerCase):
    def test_usage(self):
        d = self.dev
        self.assertEqual(d.run("--self-test"), 0)
        self.assertRegex(d.last.stdout.decode(), r"^ok \d+\.\d+\.\d+")
        for args in ([], ["apply"], ["rollback"], ["apply", "game", "--app"], ["apply", "all", "--version", "1.7.3"],
                     ["rollback", "all"], ["apply", "everything"], ["frobnicate"]):
            with self.subTest(args):
                self.assertEqual(d.run(*args), 2, d.out())
        self.assertFalse(os.path.exists(os.path.join(d.state, "update.json")))      # a usage error changes nothing
        # --game/--app work like the word
        self.release("1.7.3")
        self.assertEqual(d.run("apply", "--game"), 0, d.out())
        self.assert_game("1.7.3")
        self.assertEqual(d.run("rollback", "--game"), 0, d.out())
        self.assert_game("1.7.2", "factory")

    def test_busy(self):
        self.release("1.7.3")
        d = self.dev
        lock = common.Lock(os.path.join(d.state, "update.lock"))
        self.assertTrue(lock.acquire())
        try:
            for args in (["check"], ["apply", "game"], ["rollback", "game"], ["boot"]):
                self.assertEqual(d.run(*args), 7, d.out())
            self.assertIn("Another update is running", d.out())
            # status never waits for the lock
            self.assertEqual(d.run("status"), 0, d.out())
            self.assert_schema(json.loads(d.last.stdout))
        finally:
            lock.release()
        self.assertEqual(self.gh.count(r"."), 0)
        self.assertEqual(d.get(LUA_REL), d.factory_lua)

    def test_status_command(self):
        d = self.dev
        # a run that died left "downloading": nothing holds the lock, so it's idle now
        with open(os.path.join(d.state, "update.json"), "w") as f:
            json.dump({"state": "downloading", "message": "Downloading…", "progress": 0.3, "unknown": 1}, f)
        self.assertEqual(d.run("status"), 0, d.out())
        st = json.loads(d.last.stdout)
        self.assert_schema(st)
        self.assertEqual((st["state"], st["game"]["installed"], st["app"]["installed"]), ("idle", "1.7.2", "0.1.0"))
        self.assertEqual(d.status(), st)
        os.unlink(os.path.join(d.state, "update.json"))
        self.assertEqual(d.run("status"), 0, d.out())
        self.assert_schema(json.loads(d.last.stdout))

    def test_progress_and_a_second_run(self):
        # bigger than one 64 KB read (Python 3.12+ reads that much at once), and slow enough (~2.5 s)
        # for a second updater to start on an emulated arm64 machine
        z = game_zip("1.7.3", add={"SCRIPTS/TOOLS/big.txt": os.urandom(256 * 1024)})
        self.release("1.7.3", zbytes=z, files={"sdcard/SCRIPTS/TOOLS/StickTime.lua": zip_member(z, "SCRIPTS/TOOLS/StickTime.lua")})
        d = self.dev
        self.gh.fault(r"^/cdn/", slow=0.01)
        p = subprocess.Popen(d.argv("apply", "game"), env=d.env(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        seen, busy = [], None
        t_end = time.time() + 90
        try:
            while p.poll() is None and time.time() < t_end:
                try:
                    st = d.status()
                except (OSError, ValueError):
                    st = None
                if st:
                    self.assert_schema(st)
                    seen.append((st["state"], st["progress"]))
                    if st["state"] == "downloading" and busy is None:
                        busy = d.run("check")
                time.sleep(0.03)
        finally:
            out, err = p.communicate(timeout=90)
        self.assertEqual(p.returncode, 0, err)
        self.assertEqual(busy, 7)
        dl = [pr for s, pr in seen if s == "downloading"]
        self.assertTrue(any(0.05 < pr < 0.45 for pr in dl), seen)
        progress = [pr for _, pr in seen]
        self.assertEqual(progress, sorted(progress), seen)
        self.assertEqual(seen[-1] if seen[-1][0] == "done" else ("done", 1), ("done", 1))
        self.assert_game("1.7.3")

    def test_boot_auto_off(self):
        self.release("1.7.3")
        d = self.dev
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assertEqual(self.gh.count(r"."), 0)                   # no network at all
        self.assertEqual(d.status()["state"], "idle")
        self.assertEqual(d.get(LUA_REL), d.factory_lua)
        self.assert_consistent()

    def test_boot_auto_on(self):
        self.release("1.7.3")
        self.app_release("0.2.0")
        d = self.dev
        d.write_settings(auto="on")
        self.assertEqual(d.run("boot", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assert_game("1.7.3")
        self.assert_app("0.2.0")
        self.assertEqual(d.restarts(), 1)
        st = d.status()
        self.assertEqual((st["state"], st["error"]), ("done", ""))
        self.assertIn("StickTime Pi 0.2.0 is installed", st["message"])
        self.assertIn("StickTime 1.7.3 is installed", st["message"])
        # the app first (a newer game may need a newer host), then the game
        paths = [p for m, p, _ in self.gh.requests if m == "GET"]
        first = lambda rx: next(i for i, x in enumerate(paths) if re.search(rx, x))
        self.assertLess(first(r"/sticktime-pi/releases"), first(r"/juricabi/sticktime/releases"))
        self.assert_consistent()
        # nothing new: a quiet boot
        self.assertEqual(d.run("boot", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assertEqual(d.restarts(), 1)

    def test_boot_auto_on_private_app_repo_and_offline(self):
        self.release("1.7.3")
        self.app_release("0.2.0", private=True, token=TOKEN)
        d = self.dev
        d.write_settings(auto="on")
        # no token: no app updates, which isn't an error; the game still updates
        self.assertEqual(d.run("boot", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assert_game("1.7.3")
        self.assertEqual(d.status()["state"], "done")
        # offline at boot (hotspot mode) is normal: idle, no error badge
        self.assertEqual(d.run("boot", STICKTIME_GITHUB_API=dead_api()), 0, d.out())
        st = d.status()
        self.assertEqual((st["state"], st["error"]), ("idle", ""))
        self.assertIn("No automatic update", st["message"])
        self.assert_consistent()

    def test_boot_auto_update_not_a_bool(self):
        # auto_update is on/off (settings_keys.h): anything else is the default, off
        self.release("1.7.3")
        d = self.dev
        d.write_settings(auto="check")
        self.assertEqual(d.run("boot"), 0, d.out())
        self.assertEqual(self.gh.count(r"."), 0)
        self.assertEqual(d.get(LUA_REL), d.factory_lua)
        d.write_settings(auto="yes")
        self.assertEqual(d.run("boot", STICKTIME_SETTLE_S="0"), 0, d.out())
        self.assert_game("1.7.3")

    def test_apply_all(self):
        self.release("1.7.3")
        d = self.dev
        # the app's archive is broken: the game still updates, the app's failure is reported
        self.app_release("0.2.0", tgz=app_tgz("0.2.0", elf=fake_elf(machine=62)))
        rc = d.run("apply", "all", STICKTIME_SETTLE_S="0")
        self.assertEqual(rc, 5, d.out())
        st = d.status()
        self.assertEqual(st["state"], "error")
        self.assertIn("aarch64", st["error"])
        self.assertIn("StickTime 1.7.3 is installed", st["message"])
        self.assert_game("1.7.3")
        self.assert_app("0.1.0")
        self.assert_consistent()

    def test_internal_error(self):
        d = self.dev
        # a bug in the updater (here: a method that raises): exit 1, the status says so, the lock is free
        wrapper = os.path.join(self.tmp, "buggy.py")
        with open(wrapper, "w") as f:
            f.write("import sys\nsys.path.insert(0, %r)\nfrom stupdate import cli, game\n"
                    "def boom(*a, **k):\n    raise RuntimeError('boom')\n"
                    "game.Game.check = boom\nsys.exit(cli.main(sys.argv[1:]))\n" % LIB)
        r = subprocess.run([sys.executable, "-I", wrapper, "--base", d.base, "--settings", d.settings,
                            "--state", d.state, "check", "game"], env=d.env(), capture_output=True, timeout=60)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn(b"RuntimeError: boom", r.stderr)
        st = d.status()
        self.assert_schema(st)
        self.assertEqual(st["state"], "error")
        self.assertIn("Internal error in the updater: boom", st["message"])
        self.release("1.7.3")
        self.assertEqual(d.run("check", "game"), 10, d.out())
        self.assert_consistent()


class TestReconcileRules(FakeServerCase):
    """which StickTime.lua in sd/ the updater may replace (in-process, against a real slot)"""

    def setUp(self):
        super().setUp()
        self.release("1.7.3")
        self.assertEqual(self.dev.run("apply", "game"), 0, self.dev.out())
        self.g = self.inspect()[0]
        self.slots = self.g.slots()
        self.new = self.lua("1.7.3")

    def damaged(self, b):
        return self.g._damaged(b, hashlib.sha256(b).hexdigest(), self.slots)

    def test_damaged(self):
        self.assertTrue(self.damaged(b""))
        self.assertTrue(self.damaged(self.new[:5000]))                  # under 10 KB
        self.assertTrue(self.damaged(self.new[:len(self.new) // 2]))    # cut short
        self.assertTrue(self.damaged(self.new[:-1] + b"Q"))             # same size, other bytes
        self.assertTrue(self.damaged(b"\0" * len(self.new)))            # what fsck may leave
        self.assertFalse(self.damaged(self.new))                        # a release's exact file
        self.assertFalse(self.damaged(self.dev.factory_lua))
        self.assertFalse(self.damaged(self.new + b"-- mine\n"))         # a longer file: someone's own
        other = self.new.replace(b"StickTime v1.7.3", b"StickTime v1.9.0", 1)
        self.assertFalse(self.damaged(other))                           # a version no slot has

    def test_replaceable(self):
        g, s = self.g, self.slots
        self.assertTrue(g._replaceable(None, s, False))
        self.assertTrue(g._replaceable(self.new, s, False))             # what we wrote
        self.assertFalse(g._replaceable(self.new + b"-- mine\n", s, False))
        self.assertTrue(g._replaceable(self.new + b"-- mine\n", s, True))
        # the image's release file, chosen on purpose after we wrote ours: left alone
        self.assertFalse(g._replaceable(self.dev.factory_lua, s, False))
        # unless it was what we were about to replace (a cut between the record and the file)
        g._remember_sd(hashlib.sha256(self.new).hexdigest(), hashlib.sha256(self.dev.factory_lua).hexdigest())
        self.assertTrue(g._replaceable(self.dev.factory_lua, s, False))
        # no record at all: any kept release is ours
        os.unlink(self.dev.p("update/sd.json"))
        self.assertTrue(g._replaceable(self.dev.factory_lua, s, False))
        self.assertFalse(g._replaceable(self.new + b"-- mine\n", s, False))


# ---------------------------------------------------------------- the system scripts (lib/)

_LIB = None


def image_lib():
    """the system scripts as the image installs them (and an app package's lib/ holds them)"""
    global _LIB
    if _LIB is None:
        _LIB = make_app_package.lib_files()
    return dict(_LIB)


def newer_lib():
    """a later version's lib/: a changed library and daemon, a new file"""
    lib = image_lib()
    lib["stlib.py"] = lib["stlib.py"] + b"\n# changed in 0.2.2\n"
    lib["sticktime-bt"] = lib["sticktime-bt"] + b"\n# changed in 0.2.2\n"
    lib["stnew.py"] = b"NEW = 1\n"
    return lib


class TestSystemSync(FakeServerCase):
    """stupdate/system.py: /usr/lib/sticktime (STICKTIME_LIBDIR) from the installed set's lib/"""

    def setUp(self):
        super().setUp()
        self.image = image_lib()
        self.put_tree(self.dev.libdir, self.image)

    @staticmethod
    def put_tree(root, files):
        for rel, b in files.items():
            p = os.path.join(root, *rel.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(b)

    def install_set(self, lib, version="0.2.2", tweak=None):
        """an installed app set with this lib/ (as the updater leaves it): files + SHA256SUMS"""
        d = self.dev
        d.put("VERSION", (version + "\n").encode())
        shutil.rmtree(d.p("lib"), ignore_errors=True)
        files = {"lib/" + rel: b for rel, b in lib.items()}
        files["VERSION"] = (version + "\n").encode()
        sums = "".join("%s  %s\n" % (hashlib.sha256(b).hexdigest(), p) for p, b in sorted(files.items()))
        for p, b in files.items():
            d.put(p, b)
        d.put("SHA256SUMS", sums.encode())
        if tweak:
            tweak(d)

    def libdir(self):
        return sysmod.tree(self.dev.libdir)

    def systemctl(self):
        try:
            with open(os.path.join(self.dev.state, "systemctl.log")) as f:
                return f.read().splitlines()
        except OSError:
            return []

    def test_no_lib_changes_nothing(self):
        d = self.dev
        self.assertEqual(d.run("sync"), 0, d.out())
        self.assertIn("up to date", d.out())
        self.assertEqual(self.libdir(), self.image)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "var", "system")))

    def test_install_keep_the_image_and_go_back(self):
        d, new = self.dev, newer_lib()
        self.install_set(new)
        os.makedirs(os.path.join(d.libdir, "__pycache__"))
        with open(os.path.join(d.libdir, "__pycache__", "stlib.cpython-313.pyc"), "wb") as f:
            f.write(b"stale")
        self.assertEqual(d.run("sync"), 0, d.out())
        self.assertEqual(self.libdir(), new)
        self.assertFalse(os.path.exists(os.path.join(d.libdir, "__pycache__")))
        self.assertIn("System scripts from StickTime Pi 0.2.2: updated sticktime-bt, stlib.py, stnew.py.", d.out())
        self.assertIn("Restarted sticktime-bt.service. Some changes apply after a reboot.", d.out())
        self.assertEqual(self.systemctl(), ["try-restart sticktime-bt.service"])
        if os.name != "nt":
            self.assertEqual(os.stat(os.path.join(d.libdir, "sticktime-bt")).st_mode & 0o777, 0o755)
            self.assertEqual(os.stat(os.path.join(d.libdir, "stnew.py")).st_mode & 0o777, 0o644)
        # the image's own files are kept, and what happened is noted
        var = os.path.join(self.tmp, "var", "system")
        self.assertEqual(sysmod.tree(os.path.join(var, "image")), self.image)
        state = json.load(open(os.path.join(var, "state.json")))
        self.assertEqual((state["changed"], state["reboot"]), (["sticktime-bt", "stlib.py", "stnew.py"], True))
        # again: nothing to do, nothing restarted
        self.assertEqual(d.run("sync"), 0, d.out())
        self.assertIn("up to date", d.out())
        self.assertEqual(len(self.systemctl()), 1)
        # a later set changes only what differs; the image copy stays the image's
        newer = dict(new, **{"stboot.py": new["stboot.py"] + b"# 0.2.3\n"})
        self.install_set(newer, "0.2.3")
        self.assertEqual(d.run("sync"), 0, d.out())
        self.assertIn("updated stboot.py.", d.out())
        self.assertEqual(len(self.systemctl()), 1)                    # not one of sticktime-bt's files
        self.assertEqual(sysmod.tree(os.path.join(var, "image")), self.image)
        # a set without lib/ (rolled back to 0.2.1): the image's files, and what an update added goes
        shutil.rmtree(d.p("lib"))
        self.assertEqual(d.run("sync"), 0, d.out())
        self.assertEqual(self.libdir(), self.image)
        self.assertIn("System scripts from the SD image: updated stboot.py, sticktime-bt, stlib.py; removed stnew.py.",
                      d.out())

    def test_refused_files_change_nothing(self):
        d = self.dev
        cases = {
            "doesn't match": lambda dv: dv.put("lib/stlib.py", b"# changed after the sums\n"),
            "is missing": lambda dv: os.unlink(dv.p("lib/stnew.py")),
            "no SHA256SUMS": lambda dv: os.unlink(dv.p("SHA256SUMS")),
        }
        for why, tweak in cases.items():
            self.install_set(newer_lib(), tweak=tweak)
            self.assertEqual(d.run("sync"), 5, why + ": " + d.out())
            self.assertIn(why, d.out())
            self.assertEqual(self.libdir(), self.image, why)
        broken = dict(newer_lib(), **{"stlib.py": b"def broken(:\n"})
        self.install_set(broken)
        self.assertEqual(d.run("sync"), 5, d.out())
        self.assertIn("don't compile (stlib.py:", d.out())
        noupd = newer_lib()
        del noupd["sticktime-update"]
        self.install_set(noupd)
        self.assertEqual(d.run("sync"), 5, d.out())
        self.assertIn("lib/ has no sticktime-update", d.out())
        self.assertEqual(self.libdir(), self.image)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "var", "system", "image.json")))
        self.assertEqual(self.systemctl(), [])

    def test_failed_self_test_puts_the_old_files_back(self):
        d = self.dev
        for rel, code in (("stupdate/cli.py", b"raise SystemExit('broken updater')\n"),
                          ("stlib.py", b"import no_such_module_here\n")):
            self.install_set(dict(newer_lib(), **{rel: code}))
            self.assertEqual(d.run("sync"), 5, d.out())
            self.assertIn("failed their test", d.out())
            self.assertEqual(self.libdir(), self.image, rel)
            self.assertEqual(self.systemctl(), [])

    def test_waits_for_a_running_update(self):
        d = self.dev
        self.install_set(newer_lib())
        lock = common.Lock(os.path.join(d.state, "update.lock"))
        self.assertTrue(lock.acquire())
        try:
            self.assertEqual(d.run("sync", "--wait", "0"), 7, d.out())
            self.assertIn("An update is running", d.out())
            self.assertEqual(self.libdir(), self.image)
        finally:
            lock.release()
        self.assertEqual(d.run("sync", "--wait", "0"), 0, d.out())
        self.assertEqual(self.libdir(), newer_lib())

    def test_app_update_brings_its_scripts_and_rollback_takes_them_back(self):
        d, new = self.dev, newer_lib()
        self.app_release("0.2.2", sums=True, tgz=app_tgz("0.2.2", extra={"lib/" + r: b for r, b in new.items()}))
        self.assertEqual(d.run("apply", "app"), 0, d.out())
        self.assert_app("0.2.2")
        self.assertEqual(self.libdir(), new)
        self.assertEqual(d.restarts(), 1)
        self.assertEqual(self.systemctl(), ["try-restart sticktime-bt.service"])
        self.assertEqual(d.status()["message"], "StickTime Pi 0.2.2 is installed; restarting… Some system changes "
                                                "apply after a reboot.")
        # the host's own sync at its next start finds nothing to do
        self.assertEqual(d.run("sync"), 0, d.out())
        self.assertIn("up to date", d.out())
        # back to 0.1.0, which has no lib/: the image's scripts
        self.assertEqual(d.run("rollback", "app"), 0, d.out())
        self.assert_app("0.1.0")
        self.assertEqual(self.libdir(), self.image)
        self.assertEqual(len(self.systemctl()), 2)
        # forward again: lib.prev went back to lib/ with the rest of the set
        self.assertEqual(d.run("rollback", "app"), 0, d.out())
        self.assert_app("0.2.2")
        self.assertEqual(self.libdir(), new)
        self.assert_consistent()

    def test_a_set_with_broken_scripts_still_installs_the_app(self):
        d = self.dev
        bad = dict(newer_lib(), **{"stboot.py": b"def broken(:\n"})
        self.app_release("0.2.2", sums=True, tgz=app_tgz("0.2.2", extra={"lib/" + r: b for r, b in bad.items()}))
        self.assertEqual(d.run("apply", "app"), 0, d.out())
        self.assert_app("0.2.2")
        self.assertEqual(self.libdir(), self.image)
        self.assertIn("The system scripts weren't updated: The system scripts of StickTime Pi 0.2.2 don't compile "
                      "(stboot.py:", d.status()["message"])
        self.assertEqual(d.restarts(), 1)

    def test_make_app_package_carries_the_image_scripts(self):
        elf = os.path.join(self.tmp, "sticktime-pi")
        with open(elf, "wb") as f:
            f.write(fake_elf(payload=b"--version 0.2.2"))
        data, _n = make_app_package.build(elf, "0.2.2", 1700000000)
        pkg = appmod.validate_app_tar(data, "0.2.2")
        self.assertEqual({p[4:]: b for p, b in pkg.files.items() if p.startswith("lib/")}, self.image)
        self.assertIn("sticktime.txt.default", self.image)
        self.assertIn("patch_editor.py", self.image)
        self.assertNotIn(b"\r\n", self.image["stlib.py"])
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
            modes = {m.name: m.mode for m in tf if m.name.startswith("lib/")}
        self.assertEqual((modes["lib/sticktime-update"], modes["lib/stlib.py"]), (0o755, 0o644))
        # the boot test's later version: a changed file and one fewer
        data, _n = make_app_package.build(elf, "0.2.2", 1700000000, lib_extra={"x.txt": b"x\n", "stboot.py": None})
        lib = {p[4:] for p in appmod.validate_app_tar(data, "0.2.2").files if p.startswith("lib/")}
        self.assertEqual(lib, (set(self.image) | {"x.txt"}) - {"stboot.py"})


if __name__ == "__main__":
    unittest.main()
