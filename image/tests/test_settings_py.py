"""Tests of image/rootfs/usr/lib/sticktime/settings.py, the Python twin of host/src/settings.c.

    python image/tests/test_settings_py.py            (from anywhere; -v for names)

The shared vectors (host/tests/settings_vectors.txt) are run exactly as host/tests/test_settings.c
runs them, so a case that passes in both proves the two parsers agree byte for byte. The rest
mirrors the C test (template, load/save, random round trips) plus what only Python has: the str
API with surrogate escapes, update_file() and the command line."""
import importlib.util
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
LIB = os.path.join(REPO, "image", "rootfs", "usr", "lib", "sticktime")
MODULE = os.path.join(LIB, "settings.py")
VECTORS = os.path.join(REPO, "host", "tests", "settings_vectors.txt")

_spec = importlib.util.spec_from_file_location("sticktime_settings", MODULE)
st = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(st)


def S(b):
    """vector bytes -> the str the API takes"""
    return b.decode("utf-8", "surrogateescape")


def B(s):
    return s.encode("utf-8", "surrogateescape")


def show(b):
    return repr(bytes(b))[2:-1] if b is not None else "(None)"


# ------------------------------------------------------------------ the vector runner
class VectorError(Exception):
    pass


def unesc(p):
    out, i, n = bytearray(), 0, len(p)
    while i < n:
        c = p[i]
        if c != 0x5C:
            out.append(c)
            i += 1
            continue
        i += 1
        if i >= n:
            raise VectorError("dangling backslash")
        e = p[i:i + 1]
        if e == b"x":
            h = p[i + 1:i + 3]
            if len(h) != 2:
                raise VectorError("bad \\x escape")
            out.append(int(h, 16))
            i += 3
            continue
        m = {b"\\": 0x5C, b"n": 0x0A, b"r": 0x0D, b"t": 0x09, b"0": 0x00, b"s": 0x20}
        if e not in m:
            raise VectorError(f"unknown escape \\{e!r}")
        out.append(m[e])
        i += 1
    return bytes(out)


def key_value(p):
    """(key, value or None) from "KEY = VALUE" / "KEY =" / "KEY" """
    sp = p.find(b" ")
    key = unesc(p if sp < 0 else p[:sp])
    if sp < 0:
        return key, None
    rest = p[sp:]
    if rest == b" =":
        return key, b""
    if not rest.startswith(b" = "):
        raise VectorError(f"expected ' = ' in {p!r}")
    return key, unesc(rest[3:])


def each_list(s):
    return [(B(k), B(v)) for k, v in s.each()]


class Case:
    def __init__(self, name, line, fail):
        self.name, self.line, self.fail = name, line, fail
        self.inp = bytearray()
        self.s = None
        self.parsed = self.check_parse_warn = self.modified = False
        self.block = ""
        self.expect = bytearray()
        self.each_expect = []

    def check(self, cond, msg):
        if not cond:
            self.fail(self.line, self.name, msg)

    def ensure_parsed(self):
        if self.parsed:
            return
        self.s = st.parse(bytes(self.inp))
        self.parsed = self.check_parse_warn = True

    def close_block(self):
        s = self.s
        if self.block == "warn":
            w = B(s.warnings())
            self.check(w == bytes(self.expect), f'warnings "{show(w)}", expected "{show(self.expect)}"')
        elif self.block == "out":
            o = s.serialize()
            self.check(o == bytes(self.expect), f'serialized "{show(o)}", expected "{show(self.expect)}"')
        elif self.block == "each":
            got = each_list(s)
            self.check(got == self.each_expect, f"each {got!r}, expected {self.each_expect!r}")
        self.block = ""
        self.expect = bytearray()
        self.each_expect = []

    def directive(self, d, p):
        if d in ("in", "inrep"):
            self.check(not self.parsed, "in after the parse")
            if d == "in":
                self.inp += unesc(p)
            else:
                n, _, text = p.partition(b" ")
                self.inp += unesc(text) * int(n)
            return
        self.ensure_parsed()
        blk = "out" if d == "outrep" else d
        if self.block and self.block != blk:
            self.close_block()
        if self.check_parse_warn:
            self.check_parse_warn = False
            if d != "warn":
                self.check(self.s.warnings() == "", f'unexpected warnings "{self.s.warnings()!r}"')
        s = self.s
        if d == "warn":
            self.block = "warn"
            self.expect += unesc(p) + b"\n"
        elif d == "nowarn":
            self.check(s.warnings() == "", f"unexpected warnings {s.warnings()!r}")
        elif d == "out":
            self.block = "out"
            self.expect += unesc(p)
        elif d == "outrep":
            self.block = "out"
            n, _, text = p.partition(b" ")
            self.expect += unesc(text) * int(n)
        elif d == "each":
            self.block = "each"
            k, v = key_value(p)
            if v is None:
                raise VectorError("each needs KEY = VALUE")
            self.each_expect.append((k, v))
        elif d == "noeach":
            self.check(s.each() == [], f"each gave {s.each()!r}")
        elif d in ("raw", "get"):
            k, v = key_value(p)
            got = s.raw(S(k)) if d == "raw" else s.get(S(k))
            if v is None:
                self.check(got is None, f"{d}({k!r}) = {got!r}, expected not set")
            else:
                self.check(got is not None and B(got) == v, f'{d}({show(k)}) = "{show(B(got)) if got is not None else None}", expected "{show(v)}"')
        elif d == "rawlen":
            k, v = key_value(p)
            got = s.raw(S(k))
            self.check(got is not None and len(B(got)) == int(v), f"rawlen({k!r}) = {len(B(got)) if got is not None else -1}")
        elif d in ("int", "num", "bool"):
            parts = p.decode().split()
            if d == "bool":
                k, dflt, eq, exp = parts
                got = s.get_bool(k, int(dflt) != 0)
                self.check(got == (int(exp) != 0), f"get_bool({k}, {dflt}) = {got}")
            elif d == "int":
                k, dflt, mn, mx, eq, exp = parts
                got = s.get_int(k, int(dflt), int(mn), int(mx))
                self.check(got == int(exp), f"get_int({k}, {dflt}, {mn}, {mx}) = {got}, expected {exp}")
            else:
                k, dflt, mn, mx, eq, exp = parts
                got = s.get_num(k, float(dflt), float(mn), float(mx))
                self.check(abs(got - float(exp)) < 1e-9, f"get_num({k}, {dflt}, {mn}, {mx}) = {got}, expected {exp}")
        elif d in ("set", "set!"):
            k, v = key_value(p)
            if v is None:
                raise VectorError("set needs KEY = VALUE")
            ok = s.set(S(k), S(v))
            self.check(ok == (d == "set"), f'set({show(k)}, "{show(v)}") = {ok}')
            self.modified = True
        elif d == "del":
            k, _ = key_value(p)
            self.check(s.set(S(k), None), f"set({k!r}, None) failed")
            self.modified = True
        elif d in ("valid", "invalid"):
            k, v = key_value(p)
            if v is None:
                raise VectorError(f"{d} needs KEY = VALUE")
            m = st.validate(S(k), S(v))
            if d == "valid":
                self.check(m is None, f'validate({k!r}, "{show(v)}"): {m}')
            else:
                self.check(bool(m), f'validate({k!r}, "{show(v)}") passed')
        else:
            raise VectorError(f"unknown directive {d}")

    def finish(self):
        self.ensure_parsed()
        if self.check_parse_warn:
            self.check(self.s.warnings() == "", f"unexpected warnings {self.s.warnings()!r}")
            self.check_parse_warn = False
        self.close_block()
        s = self.s
        t1 = s.serialize()
        s2 = st.parse(t1)
        t2 = s2.serialize()
        self.check(t1 == t2, "serialize(parse(serialize)) differs")
        self.check(each_list(s) == each_list(s2), "the reparsed settings differ")
        w1, w2 = s.warnings(), s2.warnings()
        if w1.startswith("NUL bytes removed\n"):
            w1 = w1[len("NUL bytes removed\n"):]
        self.check(w1 == w2, f"reparsed warnings {w2!r} vs {w1!r}")
        for k in st.KEYS:
            self.check(s.get(k.key) == s2.get(k.key), f"reparsed {k.key} differs")
        if not self.modified:
            clean = bytes(self.inp).replace(b"\0", b"")
            self.check(t1 == clean, f'round trip "{show(t1)}", input "{show(clean)}"')


def run_vectors(fail):
    """runs every case; fail(line, case, message) for each failed check. Returns the number of cases."""
    with open(VECTORS, "rb") as f:
        text = f.read()
    case, ncases = None, 0
    for lineno, line in enumerate(text.split(b"\n"), 1):
        if line.endswith(b"\r"):
            line = line[:-1]
        if not line or line.startswith(b"#"):
            continue
        d, colon, p = line.partition(b":")
        if not colon:
            fail(lineno, "", f"no ':' in {line!r}")
            continue
        if p.startswith(b" "):
            p = p[1:]
        d = d.decode()
        if d == "case":
            if case:
                case.finish()
            case = Case(p.decode(), lineno, fail)
            ncases += 1
            continue
        if not case:
            fail(lineno, "", "directive outside a case")
            continue
        case.line = lineno
        try:
            case.directive(d, p)
        except VectorError as e:
            fail(lineno, case.name, str(e))
    if case:
        case.finish()
    return ncases


class TestVectors(unittest.TestCase):
    def test_vectors(self):
        failures = []
        n = run_vectors(lambda line, name, msg: failures.append(f"settings_vectors.txt:{line} [{name}] {msg}"))
        self.assertGreaterEqual(n, 50)
        self.assertEqual(failures, [], "\n" + "\n".join(failures[:40]))


# ------------------------------------------------------------------ the rest of the API
class TestKeys(unittest.TestCase):
    def test_table(self):
        self.assertGreaterEqual(len(st.KEYS), 38)
        names = [k.key for k in st.KEYS]
        self.assertEqual(len(names), len(set(names)))
        for k in st.KEYS:
            self.assertIs(st.key(k.key), k)
            self.assertIs(st.key(k.key.upper()), k)
            self.assertTrue(k.help)
            self.assertFalse(k.live and k.reboot)
            self.assertIn(k.kind, (st.STRING, st.ENUM, st.INT, st.NUM, st.BOOL, st.SECRET))
            self.assertIsNone(st.validate(k.key, k.default), k.key)
            if k.kind == st.ENUM:
                self.assertIn("|", k.choices)
                self.assertIn(k.default, k.choices.split("|"))
            if k.kind == st.INT:
                lo, hi = (int(x) for x in k.choices.split("|"))
                self.assertTrue(lo <= int(k.default) <= hi)

    def test_matches_header(self):
        spec = importlib.util.spec_from_file_location("check_settings_keys", os.path.join(REPO, "host", "tests", "check_settings_keys.py"))
        check = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(check)
        self.assertEqual(check.problems(), [])

    def test_lookup_and_validate(self):
        self.assertIsNone(st.key("nope"))
        self.assertIsNone(st.key(""))
        self.assertIsNone(st.key(None))
        self.assertIsNone(st.key("vide\u212a"))                 # no Unicode case folding (C lowers ASCII only)
        self.assertIsNone(st.key("stic\u212a_menus"))
        self.assertIs(st.key("Stick_Menus"), st.key("stick_menus"))
        self.assertIsNone(st.validate("video", None))
        self.assertIsNone(st.validate("not_a_key", "anything"))
        self.assertIsNone(st.validate(None, "x"))
        self.assertEqual(st.validate("tv", "x"), "must be one of ntsc, pal, pal60, ntsc-j, pal-m, pal-n, ntsc-443, secam")
        self.assertEqual(st.validate("margin", "x"), "must be a whole number from 0 to 20")
        self.assertEqual(st.validate("ssh", "maybe"), "must be on or off")
        self.assertEqual(st.validate("x", "a\nb"), "must not contain line breaks")
        self.assertEqual(st.validate("x", "a\0b"), "must not contain NUL characters")
        self.assertEqual(st.validate("password", "\ud800"), "must be valid UTF-8")
        self.assertIsNone(st.validate("wifi_ssid", b"Caf\xe9"))      # bytes are fine too
        self.assertIsNone(st.validate("wifi_password", "p\u00e4ssw\u00f6"))   # 8 bytes, 6 characters
        self.assertIsNotNone(st.validate("wifi_password", "p\u00e4ss\u00f6"))


class TestTemplate(unittest.TestCase):
    def test_template(self):
        s = st.parse(st.TEMPLATE)
        self.assertEqual(s.warnings(), "")
        self.assertEqual(s.each(), [(k.key, k.default) for k in st.KEYS])    # every key once, in order
        self.assertEqual(s.serialize(), st.TEMPLATE.encode())
        for k in st.KEYS:
            self.assertEqual(s.get(k.key), k.default)
        self.assertTrue(s.set("video", "hdmi"))
        t = s.serialize().decode()
        self.assertEqual(t, st.TEMPLATE.replace("\nvideo = av\n", "\nvideo = hdmi\n", 1))
        self.assertEqual(s.as_dict(effective=False)["video"], "hdmi")
        d = s.as_dict()
        self.assertEqual(d["video"], "hdmi")
        self.assertEqual(len(d), len(st.KEYS))

    def test_new_and_null(self):
        s = st.new()
        self.assertEqual(s.warnings(), "")
        self.assertEqual(s.warning_list(), [])
        self.assertEqual(s.get("video"), "av")
        self.assertIsNone(s.raw("video"))
        self.assertTrue(s.set("video", "hdmi"))
        for bad in ("", "   ", None, "a b", "a=b", ".x", "-x", "k\u00e9y", "a\0b"):
            self.assertFalse(s.set(bad, "x"), repr(bad))
        long_key = "k" * 64
        self.assertTrue(s.set(long_key, "x"))
        self.assertEqual(s.raw(long_key), "x")
        self.assertEqual(s.raw(long_key.upper()), "x")
        self.assertFalse(s.set(long_key + "k", "x"))
        self.assertIsNone(s.raw(long_key + "k"))
        self.assertEqual(s.get(long_key + "k"), "")
        self.assertIsNone(s.raw(""))
        self.assertIsNone(s.raw(None))
        self.assertEqual(s.get(None), "")
        self.assertIsNone(s.raw(" video"))                     # lookups don't trim
        self.assertTrue(s.set(" \tTV\t ", "pal"))              # set does
        self.assertEqual(s.serialize(), f"video = hdmi\n{long_key} = x\ntv = pal\n".encode())
        self.assertEqual(st.parse(b"").serialize(), b"")
        self.assertEqual(st.parse(None).serialize(), b"")
        self.assertEqual(st.parse(bytearray(b"a = 1")).raw("a"), "1")
        self.assertEqual(st.parse("wifi_ssid = Caf\u00e9\n").raw("wifi_ssid"), "Caf\u00e9")
        s = st.parse("a = 1\nb = \ud800\n")                     # not even surrogate-escapable: kept, flagged
        self.assertEqual(s.raw("a"), "1")
        self.assertEqual(s.warnings(), "line 2: not UTF-8\n")
        self.assertEqual(s.serialize(), b"a = 1\nb = \xed\xa0\x80\n")

    def test_str_api(self):
        s = st.parse(b"wifi_ssid = Caf\xe9\nx = 1\n")
        self.assertEqual(s.warnings(), "line 1: not UTF-8\n")
        self.assertEqual(s.raw("wifi_ssid"), "Caf\udce9")      # surrogate escape of the stray byte
        self.assertTrue(s.set("x", "2"))
        self.assertEqual(s.serialize(), b"wifi_ssid = Caf\xe9\nx = 2\n")
        self.assertTrue(s.set("y", "Caf\udce9"))               # and back to the same byte
        self.assertTrue(s.serialize().endswith(b"y = Caf\xe9\n"))
        self.assertTrue(s.set("z", b"raw \xff bytes"))
        self.assertEqual(s.raw("z"), "raw \udcff bytes")
        before = s.serialize()
        self.assertFalse(s.set("x", "\ud800"))                 # not encodable
        self.assertFalse(s.set("x", "a\0b"))                   # C strings can't hold it
        self.assertFalse(s.set("x", "a\rb"))
        self.assertFalse(s.set("x", "\n"))
        self.assertEqual(s.serialize(), before)
        self.assertEqual(s.items(), s.each())

    def test_typed_getters(self):
        s = st.parse(b"margin = 15\nvolume = abc\nf = 1e999\nn = -1e999\nstats = yes\nlatency = 0.1\n")
        self.assertEqual(s.get_int("margin", 0, 0, 10), 10)
        self.assertEqual(s.get_int("volume", 1, 0, 100), 70)       # invalid: the default, not the argument
        self.assertEqual(s.get_num("f", 1.0, 0.0, 5.0), 5.0)       # inf is clamped (as strtod's HUGE_VAL)
        self.assertEqual(s.get_num("n", 1.0, -2.0, 5.0), -2.0)
        self.assertEqual(s.get_num("latency", 9.0, 0.0, 1.0), 0.1)
        self.assertTrue(s.get_bool("stats", False))
        self.assertEqual(s.get("stats"), "on")
        self.assertIs(s.get_bool("nokey", None), None)


class TestLoadSave(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="st_settings_")
        self.path = os.path.join(self.tmp, "sticktime.txt")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def read(self):
        with open(self.path, "rb") as f:
            return f.read()

    def test_load_save(self):
        s, exists = st.load(self.path)
        self.assertFalse(exists)
        self.assertEqual(s.warnings(), "")
        self.assertIsNone(s.load_error)
        self.assertEqual(s.raw("video"), "av")                # the template's lines
        self.assertTrue(s.set("wifi_ssid", ' My "Net" # 1 '))
        self.assertTrue(s.set("wifi_password", "p\u00e4ss \\ #word"))
        s.save(self.path)
        self.assertEqual(os.listdir(self.tmp), ["sticktime.txt"])   # no temp file left behind
        self.assertEqual(self.read(), s.serialize())
        s2, exists = st.load(self.path)
        self.assertTrue(exists)
        self.assertEqual(s2.raw("wifi_ssid"), ' My "Net" # 1 ')
        self.assertEqual(s2.raw("wifi_password"), "p\u00e4ss \\ #word")
        self.assertEqual(s2.warnings(), "")
        # saving where the folder is missing raises and leaves nothing
        with self.assertRaises(OSError):
            s2.save(os.path.join(self.tmp, "missing", "dir", "sticktime.txt"))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "missing")))
        self.assertEqual(sorted(os.listdir(self.tmp)), ["sticktime.txt"])

    def test_unreadable(self):
        d = os.path.join(self.tmp, "adir.txt")
        os.mkdir(d)
        s, exists = st.load(d)
        self.assertTrue(exists)
        self.assertTrue(s.warnings().startswith(f"cannot read {d}: "), s.warnings())
        self.assertTrue(s.warnings().endswith(" (using the defaults)\n"))
        self.assertIsNotNone(s.load_error)
        self.assertEqual(s.get("video"), "av")
        with self.assertRaises(OSError):
            st.update_file(d, {"video": "hdmi"})               # never replaces what it couldn't read
        self.assertTrue(os.path.isdir(d))
        # too big (1 MiB and more, as plat_read_file)
        with open(self.path, "wb") as f:
            f.write(b"# " + b"x" * (st.FILE_MAX - 3) + b"\n")
        s, exists = st.load(self.path)
        self.assertTrue(exists)
        self.assertIn("cannot read", s.warnings())
        with open(self.path, "wb") as f:
            f.write(b"# " + b"x" * (st.FILE_MAX - 4) + b"\n")
        s, exists = st.load(self.path)
        self.assertEqual(s.warnings(), "")

    def test_pc_file(self):
        pc = b"\xef\xbb\xbfvideo = hdmi\r\nwifi_ssid = \"Caf\xc3\xa9 #1\"\r\n\0\0"
        with open(self.path, "wb") as f:
            f.write(pc)
        s, exists = st.load(self.path)
        self.assertEqual(s.get("video"), "hdmi")
        self.assertEqual(s.raw("wifi_ssid"), "Caf\u00e9 #1")
        self.assertEqual(s.warnings(), "NUL bytes removed\n")
        self.assertTrue(s.set("tv", "pal"))
        s.save(self.path)
        self.assertEqual(self.read(), b"\xef\xbb\xbfvideo = hdmi\r\nwifi_ssid = \"Caf\xc3\xa9 #1\"\r\ntv = pal\r\n")

    def test_update_file(self):
        st.update_file(self.path, {"video": "HDMI", "wifi_ssid": " Home "})   # missing: the template + changes
        t = self.read().decode()
        self.assertIn("\nvideo = hdmi\n", t)
        self.assertIn('\nwifi_ssid = " Home "\n', t)
        self.assertTrue(t.startswith("# StickTime Pi settings\n"))
        with open(self.path, "wb") as f:
            f.write(b"# mine\r\npassword = secret # cleared at boot\r\nx = 1\r\n")
        st.save_values(self.path, {"password": "", "y": "2"})
        self.assertEqual(self.read(), b"# mine\r\npassword = # cleared at boot\r\nx = 1\r\ny = 2\r\n")
        st.update_file(self.path, {"x": None})
        self.assertEqual(self.read(), b"# mine\r\npassword = # cleared at boot\r\ny = 2\r\n")
        before = self.read()
        for bad in ({"bad key": "1"}, {"y": "3", "z": "a\nb"}):
            with self.assertRaises(ValueError):
                st.update_file(self.path, bad)
            self.assertEqual(self.read(), before)              # all or nothing
        self.assertEqual(os.listdir(self.tmp), ["sticktime.txt"])

    def test_cli(self):
        def run(*args):
            return subprocess.run([sys.executable, "-I", MODULE, "-f", self.path, *args],
                                  capture_output=True, text=True, encoding="utf-8")
        r = run("set", "video=hdmi", "wifi_ssid= x # y")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(run("get", "video", "wifi_ssid", "tv").stdout, "hdmi\n x # y\nntsc\n")
        self.assertEqual(run("raw", "wifi_ssid").stdout, " x # y\n")
        self.assertEqual(run("raw", "nothere").returncode, 1)
        r = run("set", "volume=500")
        self.assertEqual(r.returncode, 1)
        self.assertIn("volume must be a whole number from 0 to 100", r.stderr)
        self.assertEqual(run("set", "novalue").returncode, 2)
        self.assertEqual(run("warnings").stdout, "")
        d = json.loads(run("json").stdout)
        self.assertEqual(d["video"], "hdmi")
        self.assertEqual(d["hostname"], "sticktime")
        self.assertEqual(run("validate", "hostname", "my-pi").returncode, 0)
        r = run("validate", "hostname", "-x")
        self.assertEqual(r.returncode, 1)
        self.assertIn("hostname must be 1-63", r.stderr)
        self.assertEqual(run().returncode, 2)
        self.assertEqual(run("frobnicate").returncode, 2)


class TestRandom(unittest.TestCase):
    ALPHA = b' \t#"\\=abZ0\r\n\x00\xef\xbb\xbf\xc3\xa9\x80\xff_.-x'

    def test_roundtrip_any_bytes(self):
        rnd = random.Random(12345)
        bad = 0
        for _ in range(5000):
            n = rnd.randrange(200)
            text = bytes(rnd.choice(self.ALPHA) if rnd.random() < 0.75 else rnd.randrange(256) for _ in range(n))
            if st.parse(text).serialize() != text.replace(b"\0", b""):
                bad += 1
        self.assertEqual(bad, 0)

    def test_set_any_value(self):
        rnd = random.Random(54321)
        keys = ("password", "wifi_ssid", "x", "github_token", "zz.y-1")
        bad = []
        for _ in range(5000):
            text = bytes(rnd.choice(self.ALPHA) for _ in range(rnd.randrange(60)))
            s = st.parse(text)
            v = bytes(c for c in (rnd.choice(self.ALPHA) if rnd.random() < 0.5 else rnd.randrange(256)
                                  for _ in range(rnd.randrange(40))) if c not in b"\0\r\n")
            k = rnd.choice(keys)
            if not s.set(k, v):
                bad.append(("set", text, v))
                continue
            o = s.serialize()
            s2 = st.parse(o)
            r = s2.raw(k)
            if r is None or B(r) != v:
                bad.append((text, v, o, r))
            if s2.set(k, "second") and st.parse(s2.serialize()).raw(k) != "second":
                bad.append(("second", text, v))
        self.assertEqual(bad[:5], [])


class TestBig(unittest.TestCase):
    def test_big_file(self):
        """~1 MiB (a paste, a corrupted card): linear, and the same answers as the C test"""
        import time
        text = "".join(f"k{i % 40000} = {i}\n" for i in range(120000)) + "video = hdmi\n"
        t0 = time.monotonic()
        s = st.parse(text)
        e = s.each()
        self.assertEqual(len(e), 40001)
        self.assertEqual(e[-1], ("video", "hdmi"))
        self.assertEqual(s.raw("k0"), "80000")
        self.assertEqual(s.raw("k39999"), "119999")
        self.assertTrue(s.set("k1", "x"))
        self.assertEqual(s.raw("k1"), "x")
        self.assertLess(time.monotonic() - t0, 30)


class TestCompat(unittest.TestCase):
    """the interfaces other helpers look for (stupdate/common.py, stlib.py)"""

    def test_shapes(self):
        tmp = tempfile.mkdtemp(prefix="st_settings_")
        try:
            p = os.path.join(tmp, "sticktime.txt")
            with open(p, "w") as f:
                f.write("auto_update = YES\ngithub_token = tok\n")
            obj = st.load(p)
            self.assertIsInstance(obj, tuple)
            d = dict(obj[0].items())                       # what common._as_dict does
            self.assertEqual(d, {"auto_update": "YES", "github_token": "tok"})
            self.assertFalse(hasattr(obj[0], "values"))    # (common.py would prefer a values() dict)
            self.assertTrue(callable(getattr(st, "update_file")))
            self.assertIs(st.save_values, st.update_file)
        finally:
            shutil.rmtree(tmp)


if __name__ == "__main__":
    unittest.main()
