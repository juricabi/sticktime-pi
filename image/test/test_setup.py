#!/usr/bin/env python3
"""Unit tests of the image's system helpers: sticktime-setup, sticktime-wifi, sticktime-hotspot,
sticktime-bt and their shared code (stlib.py, stboot.py).

    python3 image/test/test_setup.py [-v] [-k PATTERN]

Stdlib only; runs on Linux (CI) and Windows. The helpers work on a fake root directory (the same
--root/set_root the image build uses) with fake nmcli, systemctl, iw, rfkill, chpasswd, usermod and
bluetoothctl on PATH: each logs its arguments (and chpasswd its stdin) to a JSON-lines file and
answers from a JSON state file, so every test can say what the system looks like and check what
the helper did.
"""
import importlib.machinery
import importlib.util
import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout

sys.dont_write_bytecode = True          # no __pycache__ next to the helpers in image/rootfs
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
LIB = os.path.join(REPO, "image", "rootfs", "usr", "lib", "sticktime")
HEADER = os.path.join(REPO, "host", "src", "settings_keys.h")
sys.path.insert(0, LIB)

import stboot  # noqa: E402
import stlib as st  # noqa: E402


def load_helper(name):
    """an extensionless helper script as a module"""
    path = os.path.join(LIB, name)
    loader = importlib.machinery.SourceFileLoader(name.replace("-", "_"), path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


setup = load_helper("sticktime-setup")
wifi = load_helper("sticktime-wifi")
hotspot = load_helper("sticktime-hotspot")
bt = load_helper("sticktime-bt")

TEMPLATE = setup.extract_template(HEADER)

# config.txt of the 2026-04-21 image (verified copy), with its [cm4]/[cm5]/[all] sections
STOCK_CONFIG = """# For more options and information see
# http://rptl.io/configtxt
# Some settings may impact device functionality. See link above for details

# Uncomment some or all of these to enable the optional hardware interfaces
#dtparam=i2c_arm=on
#dtparam=i2s=on
#dtparam=spi=on

# Enable audio (loads snd_bcm2835)
dtparam=audio=on

# Additional overlays and parameters are documented
# /boot/firmware/overlays/README

# Automatically load overlays for detected cameras
camera_auto_detect=1

# Automatically load overlays for detected DSI displays
display_auto_detect=1

# Automatically load initramfs files, if found
auto_initramfs=1

# Enable DRM VC4 V3D driver
dtoverlay=vc4-kms-v3d
max_framebuffers=2

# Don't have the firmware create an initial video= setting in cmdline.txt.
# Use the kernel's default instead.
disable_fw_kms_setup=1

# Run in 64-bit mode
arm_64bit=1

# Disable compensation for displays with overscan
disable_overscan=1

# Run as fast as firmware / board allows
arm_boost=1

[cm4]
# Enable host mode on the 2711 built-in XHCI USB controller.
# This line should be removed if the legacy DWC2 controller is required
# (e.g. for USB device mode) or if USB support is not required.
otg_mode=1

[cm5]
dtoverlay=dwc2,dr_mode=host

[all]
"""
STOCK_CMDLINE = ("console=serial0,115200 console=tty1 root=PARTUUID=7d23366b-02 rootfstype=ext4 fsck.repair=yes "
                 "rootwait resize\n")
ISO3166 = "# comment\nAD\tAndorra\nDE\tGermany\nGB\tBritain (UK)\nHR\tCroatia\nUS\tUnited States\n"

FAKE_CMD = r'''#!/usr/bin/env python3
import json, os, sys
name = os.path.basename(sys.argv[0])
args = sys.argv[1:]
try:
    state = json.load(open(os.environ["FAKE_STATE"], encoding="utf-8"))
except (KeyError, OSError, ValueError):
    state = {}
entry = {"cmd": name, "argv": args}
if name == "chpasswd":
    entry["stdin"] = sys.stdin.read()
with open(os.environ["FAKE_LOG"], "a", encoding="utf-8") as f:
    f.write(json.dumps(entry) + "\n")
def esc(s):
    return s.replace("\\", "\\\\").replace(":", "\\:")
rc, out = 0, ""
a = [x for x in args]
if name == "nmcli":
    while a and a[0] in ("-t", "-w", "-f"):
        if a[0] in ("-w", "-f"):
            a = a[2:]
        else:
            a = a[1:]
    nm = state.get("nm", {})
    if not nm.get("running", True) and a[:1] != ["--offline"]:
        sys.stderr.write("Error: NetworkManager is not running.\n")
        sys.exit(8)
    if a[:1] == ["general"]:
        out = "running\n"
    elif a[:1] == ["device"] and len(a) == 1:
        out = "".join(":".join(esc(x) for x in d) + "\n" for d in nm.get("devices", []))
    elif a[:3] == ["device", "wifi", "list"]:
        rescan = "yes" in a[a.index("--rescan") + 1:a.index("--rescan") + 2] if "--rescan" in a else False
        if nm.get("scan_fail") or (rescan and nm.get("rescan_fail")):
            sys.stderr.write("Error: scanning not allowed\n")
            rc = 1
        else:
            out = nm.get("scan", "")
    elif a[:2] == ["connection", "up"]:
        prof = a[a.index("id") + 1] if "id" in a else ""
        ok = nm.get("up_ok", True)
        if isinstance(ok, dict):
            ok = ok.get(prof, True)
        if not ok:
            sys.stderr.write("Error: Connection activation failed: no network with SSID found\n")
            rc = 4
elif name == "systemctl":
    sc = state.get("systemctl", {})
    if a[:1] == ["is-enabled"]:
        v = sc.get("is-enabled " + a[1], "disabled")
        out = v + "\n"
        rc = 0 if v.startswith("enabled") else 1
    rc = sc.get("rc " + " ".join(a), rc)
elif name == "chpasswd":
    rc = state.get("chpasswd_rc", 0)
    if rc:
        sys.stderr.write("chpasswd: (user pi) pam_chauthtok() failed\n")
elif name == "iw":
    if a[:1] == ["dev"] and "station" in a:
        if state.get("stations") is None:
            rc = 1
        else:
            out = "".join(f"Station 00:11:22:33:44:{i:02x} (on wlan0)\n\tinactive time: 10 ms\n" for i in range(state["stations"]))
elif name == "bluetoothctl":
    b = state.get("bt", {})
    if not args:
        # an interactive session: one command per stdin line, each logged as "session <command>";
        # answers from state["bt"]["session"] (a list: one answer per call, the last one repeats)
        sess, used = b.get("session", {}), {}
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            with open(os.environ["FAKE_LOG"], "a", encoding="utf-8") as f:
                f.write(json.dumps({"cmd": name, "argv": ["session"] + line.split()}) + "\n")
            if line == "quit":
                break
            dflt = {"agent NoInputNoOutput": "Agent registered", "default-agent": "Default agent request successful"}
            r = sess.get(line, dflt.get(line, ""))
            if isinstance(r, list):
                i = used.get(line, 0)
                used[line] = i + 1
                r = r[min(i, len(r) - 1)] if r else ""
            sys.stdout.write(r + "\n")
            sys.stdout.flush()
        sys.exit(0)
    a = [x for x in a if not x.startswith("--agent") and x != "NoInputNoOutput"]
    if a[:1] == ["--timeout"]:
        a = a[2:]
    key = " ".join(a)
    out = b.get(key, "")
    rc = b.get("rc " + key, 0)
sys.stdout.write(out)
sys.exit(rc)
'''
FAKES = ("nmcli", "systemctl", "iw", "rfkill", "chpasswd", "usermod", "bluetoothctl", "journalctl")
PYCACHE = os.path.join(LIB, "__pycache__")
_PYC_BEFORE = set(os.listdir(PYCACHE)) if os.path.isdir(PYCACHE) else None


def tearDownModule():
    """remove the bytecode this run made next to the helpers (helper subprocesses write it)"""
    if not os.path.isdir(PYCACHE):
        return
    for n in set(os.listdir(PYCACHE)) - (_PYC_BEFORE or set()):
        if n.startswith(("stlib.", "stboot.", "sticktime-")):
            try:
                os.unlink(os.path.join(PYCACHE, n))
            except OSError:
                pass
    if _PYC_BEFORE is None and not os.listdir(PYCACHE):
        os.rmdir(PYCACHE)


class Env(unittest.TestCase):
    """a fake root plus fake commands; st's root points at it"""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="sticktime-test-")
        self.root = os.path.join(self.tmp, "root")
        self.bin = os.path.join(self.tmp, "bin")
        os.makedirs(self.bin)
        for n in FAKES:
            path = os.path.join(self.bin, n)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(FAKE_CMD)
            os.chmod(path, 0o755)
        self.log_path = os.path.join(self.tmp, "fake.log")
        self.state_path = os.path.join(self.tmp, "fake.json")
        self.state = {"nm": {"running": True, "devices": [["wlan0", "wifi", "disconnected", "--"]]},
                      "systemctl": {"is-enabled ssh.service": "disabled"}}
        self.save_state()
        self.old_env = dict(os.environ)
        os.environ["PATH"] = self.bin + os.pathsep + os.environ.get("PATH", "")
        os.environ["FAKE_LOG"] = self.log_path
        os.environ["FAKE_STATE"] = self.state_path
        os.environ.pop("STICKTIME_NOREBOOT", None)
        self.make_root()
        st.set_root(self.root)
        # background helpers are recorded, not started (no stray processes after a test)
        self.spawned = []
        self.real_spawn = st.spawn_background
        st.spawn_background = lambda argv: self.spawned.append([os.path.basename(a) for a in argv]) or True

    def tearDown(self):
        st.spawn_background = self.real_spawn
        st.set_root("/")
        os.environ.clear()
        os.environ.update(self.old_env)
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ---- fixtures
    def w(self, rel, data, mode=None):
        path = os.path.join(self.root, rel.lstrip("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data.encode("utf-8") if isinstance(data, str) else data)
        if mode is not None:
            os.chmod(path, mode)
        return path

    def r(self, rel, binary=False):
        path = os.path.join(self.root, rel.lstrip("/"))
        with open(path, "rb") as f:
            b = f.read()
        return b if binary else b.decode("utf-8")

    def exists(self, rel):
        return os.path.lexists(os.path.join(self.root, rel.lstrip("/")))      # a dangling link counts

    def make_root(self):
        self.w("/boot/firmware/config.txt", STOCK_CONFIG)
        self.w("/boot/firmware/cmdline.txt", STOCK_CMDLINE)
        self.w("/boot/firmware/sticktime.txt", TEMPLATE)
        self.w("/usr/lib/sticktime/sticktime.txt.default", TEMPLATE)
        self.w("/etc/hostname", "raspberrypi\n")
        self.w("/etc/hosts", "127.0.0.1\tlocalhost\n::1\t\tlocalhost ip6-localhost ip6-loopback\nff02::1\t\tip6-allnodes\n\n"
                             "127.0.1.1\t\traspberrypi\n")
        self.w("/etc/passwd", "root:x:0:0:root:/root:/bin/bash\npi:x:1000:1000::/home/pi:/usr/sbin/nologin\n")
        self.w("/var/lib/NetworkManager/NetworkManager.state", "[main]\nWirelessEnabled=false\n")
        self.w("/var/lib/systemd/rfkill/platform-fe215040.serial:bluetooth", "0\n")
        self.w("/var/lib/systemd/rfkill/platform-fe300000.mmcnr:wlan", "1\n")
        self.w("/usr/share/zoneinfo/iso3166.tab", ISO3166)
        self.w("/proc/cmdline", "console=tty1 root=/dev/mmcblk0p2\n")
        self.w("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "ondemand\n")
        self.w("/dev/kmsg", "")
        self.w("/usr/lib/systemd/system/ssh.service", "[Unit]\nDescription=OpenBSD Secure Shell server\n\n[Service]\n"
                                                      "ExecStart=/usr/sbin/sshd -D\n\n[Install]\nWantedBy=multi-user.target\n"
                                                      "Alias=sshd.service\n")
        os.makedirs(os.path.join(self.root, "etc/NetworkManager/system-connections"), exist_ok=True)

    def save_state(self):
        with open(self.state_path, "w", encoding="utf-8") as f:
            json.dump(self.state, f)

    def calls(self, cmd=None):
        try:
            with open(self.log_path, encoding="utf-8") as f:
                out = [json.loads(ln) for ln in f if ln.strip()]
        except OSError:
            out = []
        return [c for c in out if cmd is None or c["cmd"] == cmd]

    def argvs(self, cmd):
        return [" ".join(c["argv"]) for c in self.calls(cmd)]

    def clear_calls(self):
        try:
            os.unlink(self.log_path)
        except OSError:
            pass

    def settings(self, **kv):
        """the template with some values changed (via the real writer)"""
        sf = st.SettingsFile(TEMPLATE.encode("utf-8"))
        for k, v in kv.items():
            sf.set(k, v)
        self.w("/boot/firmware/sticktime.txt", sf.serialize())

    def run_setup(self, mode="boot", no_reboot=False):
        err = io.StringIO()
        with redirect_stderr(err):
            c, failed = setup.apply(mode, no_reboot)
        self.last_stderr = err.getvalue()
        return c, failed

    def bake(self):
        with redirect_stderr(io.StringIO()):
            setup.apply("bake")


# ================================================================ the settings file
class TestSettingsFile(Env):
    def test_template_extracted_from_header(self):
        self.assertTrue(TEMPLATE.startswith("# StickTime Pi settings\n"))
        vals = st.SettingsFile(TEMPLATE.encode()).values
        for k, v in {"video": "av", "tv": "ntsc", "hostname": "sticktime", "hotspot": "auto", "ssh": "off",
                     "password": "", "wifi_country": "US", "hotspot_password": "sticktime", "bluetooth": "on"}.items():
            self.assertEqual(vals[k], v, k)
        self.assertNotIn("\r", TEMPLATE)

    def test_extract_template_errors(self):
        bad = os.path.join(self.tmp, "bad.h")
        with open(bad, "w") as f:
            f.write("#define OTHER \"x\"\n")
        with self.assertRaises(ValueError):
            setup.extract_template(bad)
        with open(bad, "w") as f:
            f.write('#define SETTINGS_TEMPLATE_TEXT \\\n"video = av\\n" \\\n"tv = pal\\n"\n')
        with self.assertRaises(ValueError):          # no hostname/ssh/... lines
            setup.extract_template(bad)
        with open(bad, "w") as f:
            f.write('#define SETTINGS_TEMPLATE_TEXT \\\n"# \\"q\\" \\x41\\101\\n" \\\n"video = av\\ntv = ntsc\\nhostname = h\\n'
                    'wifi_ssid =\\nhotspot = auto\\nssh = off\\npassword =\\n"\n#define NEXT 1\n')
        t = setup.extract_template(bad)
        self.assertTrue(t.startswith('# "q" AA\n'))
        self.assertIn("password =\n", t)

    def test_missing_file_writes_template(self):
        os.unlink(os.path.join(self.root, "boot/firmware/sticktime.txt"))
        s = st.load_settings()
        self.assertFalse(s.exists)
        self.assertEqual(self.r("/boot/firmware/sticktime.txt"), TEMPLATE)
        self.assertEqual(s.get("video"), "av")
        self.assertTrue(any("missing" in w for w in s.warnings))

    def test_missing_template_uses_fallback(self):
        os.unlink(os.path.join(self.root, "usr/lib/sticktime/sticktime.txt.default"))
        os.unlink(os.path.join(self.root, "boot/firmware/sticktime.txt"))
        s = st.load_settings()
        self.assertEqual(s.get("hostname"), "sticktime")
        self.assertIn("hostname = sticktime", self.r("/boot/firmware/sticktime.txt"))

    def test_empty_and_whitespace_file(self):
        for data in (b"", b"\xef\xbb\xbf", b"\r\n \t\n"):
            self.w("/boot/firmware/sticktime.txt", data)
            s = st.load_settings()
            self.assertFalse(s.exists)
            self.assertEqual(self.r("/boot/firmware/sticktime.txt"), TEMPLATE)

    def test_crlf_bom_parsed_and_kept(self):
        data = "\ufeffhostname = Pi-Box \r\nssh = on\r\n# c\r\npassword = \"a # b\"  # note\r\n".encode("utf-8")
        self.w("/boot/firmware/sticktime.txt", data)
        s = st.load_settings()
        self.assertEqual(s.get("hostname"), "Pi-Box")
        self.assertEqual(s.get("password"), "a # b")
        self.assertTrue(s.bool("ssh"))
        st.update_settings({"password": ""})
        out = self.r("/boot/firmware/sticktime.txt", binary=True)
        self.assertTrue(out.startswith(b"\xef\xbb\xbf"))
        self.assertEqual(out.count(b"\r\n"), 4)
        self.assertNotIn(b"a # b", out)
        self.assertIn(b"password =  # note\r\n", out)
        self.assertIn(b"hostname = Pi-Box \r\n", out)      # untouched lines stay byte for byte

    def test_corrupt_file_backed_up_and_replaced(self):
        junk = bytes(range(256)) * 20
        self.w("/boot/firmware/sticktime.txt", junk)
        s = st.load_settings()
        self.assertFalse(s.exists)
        self.assertEqual(self.r("/boot/firmware/sticktime.txt"), TEMPLATE)
        backups = [n for n in os.listdir(os.path.join(self.root, "boot/firmware")) if n.startswith("sticktime.txt.broken-")]
        self.assertEqual(len(backups), 1)
        self.assertEqual(self.r("/boot/firmware/" + backups[0], binary=True), junk)

    def test_huge_or_keyless_file_is_corrupt(self):
        self.w("/boot/firmware/sticktime.txt", b"video = hdmi\n" + b"#" * (1024 * 1024 + 10) + b"\n")
        self.assertFalse(st.load_settings().exists)
        self.w("/boot/firmware/sticktime.txt", b"this is not a settings file\nnor is this\n")
        self.assertFalse(st.load_settings().exists)

    def test_latin1_file_warns_but_reads(self):
        self.w("/boot/firmware/sticktime.txt", "hostname = caf\xe9\nssh = on\n".encode("latin-1"))
        s = st.load_settings()
        self.assertTrue(s.exists)
        self.assertTrue(s.bool("ssh"))
        self.assertTrue(any("UTF-8" in w for w in s.warnings))

    def test_parser_rules(self):
        sf = st.SettingsFile(b"Video = HDMI\nssid_no_eq\n = nokey\nwifi_ssid = My Net # comment\nwifi_ssid = Later#notcomment\n"
                             b"wifi_password = \"  lead # \\\"x\\\" \\\\ \"\nhotspot_ssid = \"unterminated\n"
                             b"  # indented comment\nempty =\n")
        self.assertEqual(sf.values["video"], "HDMI")
        self.assertEqual(sf.values["wifi_ssid"], "Later#notcomment")
        self.assertEqual(sf.values["wifi_password"], '  lead # "x" \\ ')
        self.assertEqual(sf.values["hotspot_ssid"], "unterminated")
        self.assertEqual(sf.values["empty"], "")
        self.assertEqual(len(sf.warnings), 3)
        for w in ("no '='", "no key", "closing quote"):
            self.assertTrue(any(w in x for x in sf.warnings), w)

    def test_set_quotes_and_keeps_comments(self):
        sf = st.SettingsFile(b"a = 1   # keep me\nb = \"q\" # also\nc = 3\n")
        sf.set("a", "x # y")
        sf.set("b", " lead")
        sf.set("c", "")
        sf.set("d", "#start")
        out = sf.serialize().decode()
        self.assertEqual(out, 'a = "x # y"   # keep me\nb = " lead" # also\nc =\nd = "#start"\n')
        again = st.SettingsFile(out.encode()).values
        self.assertEqual(again, {"a": "x # y", "b": " lead", "c": "", "d": "#start"})
        with self.assertRaises(ValueError):
            sf.set("bad key", "x")
        with self.assertRaises(ValueError):
            sf.set("k", "line\nbreak")

    def test_bool_and_enum_fallbacks(self):
        self.settings(ssh="maybe", video="vga", bluetooth="OFF")
        s = st.load_settings()
        self.assertFalse(s.bool("ssh"))
        self.assertFalse(s.bool("bluetooth"))
        self.assertEqual(s.enum("video", ("av", "hdmi")), "av")
        self.assertEqual(len(s.warnings), 2)

    def test_update_settings_appends_new_key(self):
        st.update_settings({"wifi_hidden": "on"})
        txt = self.r("/boot/firmware/sticktime.txt")
        self.assertTrue(txt.startswith(TEMPLATE))
        self.assertTrue(txt.endswith("wifi_hidden = on\n"))


# ================================================================ NetworkManager keyfiles
def kf_unescape(v):
    out, i = [], 0
    while i < len(v):
        if v[i] == "\\" and i + 1 < len(v):
            out.append({"s": " ", "n": "\n", "t": "\t", "r": "\r", "\\": "\\"}.get(v[i + 1], v[i + 1]))
            i += 2
        else:
            out.append(v[i])
            i += 1
    return "".join(out)


def kf_parse(text):
    """GKeyFile: {section: {key: raw value}} (trailing spaces cut, as GKeyFile does)"""
    secs, cur = {}, None
    for ln in text.split("\n"):
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        m = re.match(r"^\[(.+)\]$", ln)
        if m:
            cur = secs.setdefault(m.group(1), {})
            continue
        k, _, v = ln.partition("=")
        cur[k.strip()] = v.lstrip(" ").rstrip(" ")
    return secs


def nm_ssid_bytes(raw):
    """NM's keyfile reader: digits/';'/spaces only = a byte list, else a string (escapes)"""
    if re.fullmatch(r"[0-9; ]+", raw) and raw.strip():
        return bytes(int(x) for x in raw.replace(" ", "").split(";") if x)
    return kf_unescape(raw).replace("\\;", ";").encode("utf-8")


class TestKeyfiles(Env):
    SSIDS = ["Home", "My Network", "semi;colon", "hash#tag", "eq=uals", " lead", "trail ", "  ", "1234", "12 34",
             "back\\slash", "Ünïcödé 🚁", "x" * 32, "a", "tab\tin"]
    PSKS = ["secret123", " leading space", "trailing space ", "back\\slash#;=", "\"quoted\"", "x" * 63,
            "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef"]

    def test_ssid_round_trip(self):
        for s in self.SSIDS:
            kf = kf_parse(st.keyfile_client(s, "secret123"))
            self.assertEqual(nm_ssid_bytes(kf["wifi"]["ssid"]), s.encode("utf-8"), repr(s))

    def test_plain_ssid_readable(self):
        self.assertIn("ssid=My Network\n", st.keyfile_client("My Network", ""))
        self.assertIn("ssid=49;50;51;52;\n", st.keyfile_client("1234", ""))

    def test_psk_round_trip(self):
        for p in self.PSKS:
            self.assertIsNone(st.psk_problem(p), p)
            kf = kf_parse(st.keyfile_client("Home", p))
            self.assertEqual(kf_unescape(kf["wifi-security"]["psk"]), p, repr(p))

    def test_psk_rules(self):
        self.assertIn("at least 8", st.psk_problem("short"))
        self.assertIn("at most 63", st.psk_problem("y" * 64))          # 64 but not hex
        self.assertIn("at most 63", st.psk_problem("y" * 65))
        self.assertIn("ASCII", st.psk_problem("pässwörd123"))
        self.assertIn("ASCII", st.psk_problem("tab\there123"))
        self.assertIsNone(st.psk_problem("A" * 64))

    def test_ssid_rules(self):
        self.assertIsNotNone(st.ssid_problem(""))
        self.assertIsNotNone(st.ssid_problem("x" * 33))
        self.assertIsNotNone(st.ssid_problem("é" * 17))              # 34 bytes
        self.assertIsNone(st.ssid_problem("é" * 16))

    def test_open_and_hidden_client(self):
        kf = kf_parse(st.keyfile_client("Cafe", "", hidden=True))
        self.assertNotIn("wifi-security", kf)
        self.assertEqual(kf["wifi"]["hidden"], "true")
        self.assertEqual(kf["wifi"]["band"], "bg")
        self.assertEqual(kf["connection"]["id"], "sticktime-wifi")
        self.assertFalse(kf["connection"]["id"].startswith("netplan-"))

    def test_hotspot_profile(self):
        kf = kf_parse(st.keyfile_hotspot("StickTime", "sticktime"))
        self.assertEqual(kf["wifi"]["mode"], "ap")
        self.assertEqual(kf["ipv4"]["method"], "shared")
        self.assertEqual(kf["ipv4"]["address1"], "10.42.0.1/24")
        self.assertEqual(kf["connection"]["autoconnect"], "false")
        self.assertEqual(kf["wifi-security"]["proto"], "rsn;")

    def test_uuid_stable(self):
        self.assertEqual(kf_parse(st.keyfile_client("A", ""))["connection"]["uuid"],
                         kf_parse(st.keyfile_client("B", "password1"))["connection"]["uuid"])

    @unittest.skipIf(os.name == "nt", "POSIX permissions")
    def test_keyfile_mode_600(self):
        st.write_keyfile("sticktime-wifi", st.keyfile_client("Home", "secret123"))
        mode = stat.S_IMODE(os.stat(os.path.join(self.root, "etc/NetworkManager/system-connections/sticktime-wifi.nmconnection")).st_mode)
        self.assertEqual(mode, 0o600)
        os.chmod(os.path.join(self.root, "etc/NetworkManager/system-connections/sticktime-wifi.nmconnection"), 0o644)
        st.write_keyfile("sticktime-wifi", st.keyfile_client("Home", "secret123"))     # same content: mode fixed
        mode = stat.S_IMODE(os.stat(os.path.join(self.root, "etc/NetworkManager/system-connections/sticktime-wifi.nmconnection")).st_mode)
        self.assertEqual(mode, 0o600)

    def test_nm_split(self):
        self.assertEqual(st.nm_split(r"*:My\:Net:72:WPA2:6:2437 MHz"), ["*", "My:Net", "72", "WPA2", "6", "2437 MHz"])
        self.assertEqual(st.nm_split(r"a\\:b"), ["a\\", "b"])
        self.assertEqual(st.nm_split(""), [""])
        self.assertEqual(st.nm_split("x:"), ["x", ""])


# ================================================================ config.txt and cmdline.txt
class TestBootFiles(unittest.TestCase):
    def test_block_appended_and_stock_lines_commented(self):
        new, warn = stboot.config_txt(STOCK_CONFIG, "av", "ntsc", "auto")
        self.assertEqual(warn, [])
        lines = new.split("\n")
        self.assertIn("#dtoverlay=vc4-kms-v3d" + stboot.NOTE, lines)
        self.assertEqual([ln for ln in lines if ln.startswith("dtoverlay=vc4")], ["dtoverlay=vc4-kms-v3d,composite,noaudio"])
        self.assertIn("enable_tvout=1", lines)
        self.assertIn("dtparam=audio=on", lines)
        self.assertIn("auto_initramfs=1", lines)          # kept: the first boot's resize needs the initramfs
        self.assertIn("otg_mode=1", lines)
        self.assertIn("dtoverlay=dwc2,dr_mode=host", lines)  # [cm5] keeps its own overlay
        b = lines.index(stboot.BEGIN)
        self.assertEqual(lines[b + 1], "[all]")
        self.assertEqual(lines[-2], stboot.END)
        self.assertTrue(new.endswith("\n"))
        for key in ("camera_auto_detect=0", "display_auto_detect=0", "disable_splash=1", "disable_overscan=1"):
            self.assertEqual(sum(1 for ln in lines if ln == key), 1, key)

    def test_idempotent(self):
        for video in ("av", "hdmi"):
            for audio in stboot.AUDIO_OUTS:
                a, _ = stboot.config_txt(STOCK_CONFIG, video, "pal", audio)
                b, _ = stboot.config_txt(a, video, "pal", audio)
                self.assertEqual(a, b, (video, audio))
                c, _ = stboot.config_txt(b, "hdmi" if video == "av" else "av", "pal", audio)
                d, _ = stboot.config_txt(c, video, "pal", audio)
                self.assertEqual(a, d, "switching back gives the same file")

    def test_hdmi_and_audio(self):
        new, _ = stboot.config_txt(STOCK_CONFIG, "hdmi", "ntsc", "auto")
        self.assertIn("\ndtoverlay=vc4-kms-v3d\n", new)
        self.assertIn("\nenable_tvout=0\n", new)
        self.assertIn("\ndtparam=audio=on\n", new)
        new, _ = stboot.config_txt(STOCK_CONFIG, "hdmi", "ntsc", "jack")
        self.assertIn("\ndtoverlay=vc4-kms-v3d,noaudio\n", new)
        new, _ = stboot.config_txt(STOCK_CONFIG, "hdmi", "ntsc", "off")
        self.assertIn("\ndtparam=audio=off\n", new)
        new, warn = stboot.config_txt(STOCK_CONFIG, "av", "ntsc", "hdmi")
        self.assertEqual(len(warn), 1)
        self.assertIn("\ndtparam=audio=on\n", new)

    def test_sections_and_broken_markers(self):
        old = "[pi4]\ndisable_overscan=0\ndtoverlay=vc4-fkms-v3d\n[all]\n" + stboot.BEGIN + "\njunk=1\n"
        new, _ = stboot.config_txt(old, "av", "ntsc", "auto")
        self.assertIn("[pi4]\ndisable_overscan=0\n", new)          # not ours: inside [pi4]
        self.assertIn("#dtoverlay=vc4-fkms-v3d", new)              # any vc4 overlay, any section
        self.assertNotIn("junk=1", new)                            # an unterminated block runs to the end
        self.assertEqual(new.count(stboot.BEGIN), 1)
        orphan = "dtparam=x=1\n" + stboot.END + "\n"
        new, _ = stboot.config_txt(orphan, "av", "ntsc", "auto")
        self.assertEqual(new.count(stboot.END), 1)

    def test_crlf_config_kept(self):
        new, _ = stboot.config_txt(STOCK_CONFIG.replace("\n", "\r\n"), "av", "ntsc", "auto")
        self.assertNotIn("\n", new.replace("\r\n", ""))
        self.assertIn("\r\nenable_tvout=1\r\n", new)

    def test_cmdline(self):
        new = stboot.cmdline_txt(STOCK_CMDLINE, "ntsc", "US")
        self.assertEqual(new, STOCK_CMDLINE.rstrip("\n") + " quiet logo.nologo loglevel=3 vt.global_cursor_default=0 consoleblank=0 "
                         "cpufreq.default_governor=performance vc4.tv_norm=NTSC cfg80211.ieee80211_regdom=US\n")
        self.assertEqual(stboot.cmdline_txt(new, "ntsc", "US"), new)
        pal = stboot.cmdline_txt(new, "pal60", "")
        self.assertIn("vc4.tv_norm=PAL", pal.split())
        self.assertNotIn("regdom", pal)
        self.assertEqual(pal.count("\n"), 1)

    def test_cmdline_keeps_foreign_tokens(self):
        old = ("console=serial0,115200 console=tty1 root=PARTUUID=abcdef12-02 rootfstype=ext4 quiet quiet loglevel=7 "
               "loglevel=1 systemd.run=/boot/firstrun.sh systemd.run_success_action=reboot video=HDMI-A-1:1280x720@60D "
               "cfg80211.ieee80211_regdom=GB fsck.repair=yes rootwait\r\n")
        new = stboot.cmdline_txt(old, "pal", "DE")
        toks = new.split()
        self.assertEqual(toks.count("quiet"), 1)
        self.assertEqual([t for t in toks if t.startswith("loglevel=")], ["loglevel=3"])
        self.assertIn("systemd.run=/boot/firstrun.sh", toks)
        self.assertIn("video=HDMI-A-1:1280x720@60D", toks)
        self.assertIn("cfg80211.ieee80211_regdom=DE", toks)
        self.assertNotIn("resize", toks)                       # never re-added
        self.assertTrue(new.endswith("rootwait logo.nologo vt.global_cursor_default=0 consoleblank=0 "
                                     "cpufreq.default_governor=performance vc4.tv_norm=PAL\n"))
        self.assertEqual(toks.index("root=PARTUUID=abcdef12-02"), 2)
        self.assertNotIn("\r", new)

    def test_cmdline_unusable_left_alone(self):
        # the jack: the firmware's snd_bcm2835.enable_headphones=0 (composite on) is overridden by ours,
        # which comes later in the line; HDMI-only sound drops it again
        j = stboot.cmdline_txt(STOCK_CMDLINE, "ntsc", "US", jack=True)
        self.assertEqual(j.split().count("snd_bcm2835.enable_headphones=1"), 1)
        self.assertEqual(stboot.cmdline_txt(j, "ntsc", "US", jack=True), j)
        self.assertNotIn("snd_bcm2835", stboot.cmdline_txt(j, "ntsc", "US", jack=False))
        self.assertTrue(stboot.jack_audio("av", "auto") and stboot.jack_audio("av", "hdmi") and stboot.jack_audio("hdmi", "both"))
        self.assertFalse(stboot.jack_audio("hdmi", "hdmi"))
        self.assertIsNone(stboot.cmdline_txt("", "ntsc", "US"))
        self.assertIsNone(stboot.cmdline_txt("console=tty1 quiet\n", "ntsc", "US"))
        self.assertEqual(stboot.cmdline_txt("root=/dev/mmcblk0p2\nrootwait\n", "ntsc", "").count("\n"), 1)

    def test_video_signature(self):
        a, _ = stboot.config_txt(STOCK_CONFIG, "av", "ntsc", "auto")
        b, _ = stboot.config_txt(STOCK_CONFIG, "av", "ntsc", "off")       # audio only
        c, _ = stboot.config_txt(STOCK_CONFIG, "hdmi", "ntsc", "auto")
        cmd = stboot.cmdline_txt(STOCK_CMDLINE, "ntsc", "US")
        self.assertEqual(stboot.video_signature(a, cmd), stboot.video_signature(b, cmd))
        self.assertNotEqual(stboot.video_signature(a, cmd), stboot.video_signature(c, cmd))
        self.assertNotEqual(stboot.video_signature(a, cmd), stboot.video_signature(a, stboot.cmdline_txt(cmd, "pal", "US")))
        self.assertEqual(stboot.video_signature(a, cmd), stboot.video_signature(a, stboot.cmdline_txt(cmd, "ntsc", "HR")))

    def test_vtx_uart_overlay(self):
        off, _ = stboot.config_txt(STOCK_CONFIG, "av", "ntsc", "auto")
        on, warn = stboot.config_txt(STOCK_CONFIG, "av", "ntsc", "auto", "uart3")
        self.assertEqual(warn, [])
        self.assertNotIn("dtoverlay=uart", off)
        lines = on.split("\n")
        self.assertEqual([ln for ln in lines if ln.startswith("dtoverlay=uart")], ["dtoverlay=uart3"])
        self.assertLess(lines.index(stboot.BEGIN), lines.index("dtoverlay=uart3"))
        self.assertEqual(lines.index("dtoverlay=uart3"), lines.index(stboot.END) - 1)    # inside our [all] block
        self.assertNotIn("ctsrts", on)                                                    # GPIO4/5 only
        # idempotent; another port replaces it; off removes it, giving the file without VTX control back
        self.assertEqual(stboot.config_txt(on, "av", "ntsc", "auto", "uart3")[0], on)
        u5, _ = stboot.config_txt(on, "hdmi", "pal", "jack", "uart5")
        self.assertEqual([ln for ln in u5.split("\n") if ln.startswith("dtoverlay=uart")], ["dtoverlay=uart5"])
        self.assertEqual(stboot.config_txt(u5, "av", "ntsc", "auto")[0], off)
        _, warn = stboot.config_txt(STOCK_CONFIG, "av", "ntsc", "auto", "uart1")
        self.assertEqual(len(warn), 1)
        # the overlay needs a reboot: it is part of the signature (a user's own uart line too, a commented one not)
        cmd = stboot.cmdline_txt(STOCK_CMDLINE, "ntsc", "US")
        self.assertNotEqual(stboot.video_signature(off, cmd), stboot.video_signature(on, cmd))
        self.assertNotEqual(stboot.video_signature(on, cmd), stboot.video_signature(stboot.config_txt(on, "av", "ntsc", "auto", "uart4")[0], cmd))
        self.assertEqual(stboot.video_signature(off, cmd), stboot.video_signature(off + "#dtoverlay=uart3\n", cmd))
        self.assertNotEqual(stboot.video_signature(off, cmd), stboot.video_signature(off + "dtoverlay=uart2\n", cmd))
        self.assertEqual(stboot.video_signature(on, cmd), stboot.video_signature(on.replace("dtoverlay=uart3", "dtoverlay = uart3  # x"), cmd))


# ================================================================ sticktime-setup
class TestSetup(Env):
    def baked(self, **kv):
        if kv:
            self.settings(**kv)
        self.bake()
        self.clear_calls()

    def test_bake_writes_files_only(self):
        self.bake()
        self.assertEqual(self.r("/etc/hostname"), "sticktime\n")
        self.assertIn("127.0.1.1\tsticktime\n", self.r("/etc/hosts"))
        self.assertIn(stboot.BEGIN, self.r("/boot/firmware/config.txt"))
        self.assertIn("cfg80211.ieee80211_regdom=US", self.r("/boot/firmware/cmdline.txt"))
        self.assertTrue(self.exists("/etc/NetworkManager/system-connections/sticktime-hotspot.nmconnection"))
        self.assertFalse(self.exists("/etc/NetworkManager/system-connections/sticktime-wifi.nmconnection"))
        self.assertEqual(self.calls(), [])                       # no commands at build time
        self.assertFalse(self.exists("/run/sticktime/setup.json"))

    def test_first_boot_after_bake_changes_nothing(self):
        self.baked()
        cfg, cmd = self.r("/boot/firmware/config.txt"), self.r("/boot/firmware/cmdline.txt")
        c, failed = self.run_setup()
        self.assertEqual(failed, [])
        self.assertFalse(c.report["reboot"])
        self.assertEqual(self.r("/boot/firmware/config.txt"), cfg)
        self.assertEqual(self.r("/boot/firmware/cmdline.txt"), cmd)
        self.assertFalse([a for a in self.argvs("systemctl") if a.endswith("reboot")])
        rep = json.loads(self.r("/run/sticktime/setup.json"))
        self.assertEqual(rep["hostname"], "sticktime")
        self.assertIn("setup done hostname=sticktime reboot=0", self.r("/dev/kmsg"))
        self.assertFalse(self.exists("/dev/console"))            # no CI marker without sticktime.ci=1

    def test_initramfs_partuuid_change_is_not_a_reboot(self):
        self.baked()
        cmd = self.r("/boot/firmware/cmdline.txt").replace("7d23366b", "1234abcd").replace(" resize", "")
        self.w("/boot/firmware/cmdline.txt", cmd)
        c, _ = self.run_setup()
        self.assertFalse(c.report["reboot"])
        self.assertEqual(self.r("/boot/firmware/cmdline.txt"), cmd)

    def test_hostname(self):
        for given, want in (("Race-Pi", "race-pi"), ("bad_name!", "sticktime"), ("-x", "sticktime"),
                            ("a" * 64, "sticktime"), ("", "sticktime"), ("pi2", "pi2")):
            self.settings(hostname=given)
            c, _ = self.run_setup("bake")
            self.assertEqual(self.r("/etc/hostname"), want + "\n", given)
            hosts = self.r("/etc/hosts")
            self.assertEqual(len(re.findall(r"^127\.0\.1\.1\s", hosts, re.M)), 1)
            self.assertIn(f"127.0.1.1\t{want}\n", hosts)
            self.assertIn("127.0.0.1\tlocalhost", hosts)
            if want != given.lower() and given:
                self.assertTrue(any("hostname" in w for w in c.warnings))

    def test_hosts_duplicates_and_missing(self):
        self.w("/etc/hosts", "127.0.0.1 localhost\n127.0.1.1 a\n127.0.1.1 b\n")
        self.run_setup("bake")
        self.assertEqual(self.r("/etc/hosts").count("127.0.1.1"), 1)
        self.w("/etc/hosts", "127.0.0.1 localhost")
        self.run_setup("bake")
        self.assertEqual(self.r("/etc/hosts"), "127.0.0.1 localhost\n127.0.1.1\tsticktime\n")
        os.unlink(os.path.join(self.root, "etc/hosts"))
        self.run_setup("bake")
        self.assertIn("127.0.0.1\tlocalhost", self.r("/etc/hosts"))

    def test_password_set_cleared_never_logged(self):
        pw = "S3cr#t: pass\\word"
        self.baked(password=pw)
        out = io.StringIO()
        with redirect_stdout(out):
            c, failed = self.run_setup()
        self.assertEqual(failed, [])
        ch = self.calls("chpasswd")
        self.assertEqual(len(ch), 1)
        self.assertEqual(ch[0]["argv"], [])                   # never on the command line
        self.assertEqual(ch[0]["stdin"], f"pi:{pw}\n")
        self.assertEqual(st.load_settings().get("password"), "")
        self.assertIn("password =\n", self.r("/boot/firmware/sticktime.txt"))
        everything = self.last_stderr + out.getvalue() + self.r("/run/sticktime/setup.json") + self.r("/dev/kmsg")
        self.assertNotIn(pw, everything)
        self.assertNotIn("S3cr", everything)
        self.assertEqual(self.argvs("usermod"), ["-s /bin/bash pi"])
        # the next boot: nothing to set
        self.clear_calls()
        self.run_setup()
        self.assertEqual(self.calls("chpasswd"), [])

    def test_password_failure_keeps_it(self):
        self.state["chpasswd_rc"] = 1
        self.save_state()
        self.baked(password="secret123")
        c, failed = self.run_setup()
        self.assertEqual(st.load_settings().get("password"), "secret123")
        self.assertTrue(any("could not set the password" in w for w in c.warnings))
        self.assertNotIn("secret123", self.last_stderr)

    WANTS = "/etc/systemd/system/multi-user.target.wants/ssh.service"
    ALIAS = "/etc/systemd/system/sshd.service"

    def test_ssh_on_off(self):
        self.baked(ssh="on")
        self.run_setup()
        # the links 'systemctl enable' would make, made without asking PID 1; then a start job
        self.assertTrue(self.exists(self.WANTS) and self.exists(self.ALIAS))
        if os.name != "nt":
            self.assertEqual(os.readlink(os.path.join(self.root, self.WANTS.lstrip("/"))), "/usr/lib/systemd/system/ssh.service")
        self.assertFalse([a for a in self.argvs("systemctl") if a.startswith(("enable", "disable"))])
        self.assertIn("start --no-block ssh.service", self.argvs("systemctl"))
        self.assertTrue(self.calls("usermod"))
        self.clear_calls()
        c, _ = self.run_setup()                             # enabled already: nothing to do
        self.assertFalse([a for a in self.argvs("systemctl") if "ssh.service" in a])
        self.assertNotIn("ssh on", c.done)
        self.settings(ssh="off")
        self.run_setup()
        self.assertFalse(self.exists(self.WANTS) or self.exists(self.ALIAS))
        self.assertIn("stop --no-block ssh.service", self.argvs("systemctl"))

    def test_ssh_masked_or_missing_unit(self):
        os.unlink(os.path.join(self.root, "usr/lib/systemd/system/ssh.service"))
        self.baked(ssh="on")
        c, failed = self.run_setup()
        self.assertEqual(failed, [])
        self.assertFalse(self.exists(self.WANTS))
        self.assertIn("cannot enable ssh.service", self.last_stderr)

    def test_unit_install_parsing(self):
        self.w("/usr/lib/systemd/system/x.service", "[Unit]\nX=1\n[Install]\nWantedBy=a.target b.target\n"
                                                    "RequiredBy=c.target\nAlias=y.service\n")
        self.assertEqual(st.unit_install("x.service"), {"WantedBy": ["a.target", "b.target"], "RequiredBy": ["c.target"],
                                                        "Alias": ["y.service"]})
        self.assertFalse(st.unit_enabled("x.service"))
        self.assertTrue(st.enable_unit("x.service"))
        self.assertTrue(st.unit_enabled("x.service"))
        for f in ("a.target.wants/x.service", "b.target.wants/x.service", "c.target.requires/x.service", "y.service"):
            self.assertTrue(self.exists("/etc/systemd/system/" + f), f)
        st.disable_unit("x.service")
        self.assertFalse(st.unit_enabled("x.service"))
        self.assertFalse(self.exists("/etc/systemd/system/y.service"))
        self.assertEqual(st.unit_install("nothing.service"), {"WantedBy": [], "RequiredBy": [], "Alias": []})

    def test_ssh_flag_file_from_pc(self):
        self.baked()
        self.w("/boot/firmware/ssh.txt", "")
        self.w("/boot/firmware/ssh", "")
        c, _ = self.run_setup()
        self.assertFalse(self.exists("/boot/firmware/ssh.txt"))
        self.assertFalse(self.exists("/boot/firmware/ssh"))
        self.assertTrue(st.load_settings().bool("ssh"))
        self.assertTrue(self.exists(self.WANTS))
        self.assertIn("start --no-block ssh.service", self.argvs("systemctl"))

    def test_wifi_profile_and_radio(self):
        self.baked(wifi_ssid="Home Net", wifi_password="secret123", wifi_country="hr")
        c, failed = self.run_setup()
        self.assertEqual(failed, [])
        kf = kf_parse(self.r("/etc/NetworkManager/system-connections/sticktime-wifi.nmconnection"))
        self.assertEqual(kf["wifi"]["ssid"], "Home Net")
        self.assertIn("reg set HR", self.argvs("iw"))
        self.assertIn("cfg80211.ieee80211_regdom=HR", self.r("/boot/firmware/cmdline.txt"))
        self.assertFalse(c.report["reboot"])                  # the country is no reason to reboot
        self.assertIn("WirelessEnabled=true", self.r("/var/lib/NetworkManager/NetworkManager.state"))
        self.assertIn("unblock wlan", self.argvs("rfkill"))
        if os.name != "nt":                                   # ':' in a name is a stream on NTFS
            self.assertEqual(self.r("/var/lib/systemd/rfkill/platform-fe300000.mmcnr:wlan"), "0\n")
        self.assertNotIn("secret123", self.last_stderr)

    def test_wifi_bad_values(self):
        self.baked(wifi_ssid="Home", wifi_password="short")
        st.write_keyfile("sticktime-wifi", st.keyfile_client("Old", "oldpassword"))
        c, failed = self.run_setup()
        self.assertEqual(failed, [])
        self.assertFalse(self.exists("/etc/NetworkManager/system-connections/sticktime-wifi.nmconnection"))
        self.assertIn("at least 8", c.report["wifi"]["error"])
        self.settings(wifi_ssid="", wifi_password="")
        st.write_keyfile("sticktime-wifi", st.keyfile_client("Old", "oldpassword"))
        self.run_setup()
        self.assertFalse(self.exists("/etc/NetworkManager/system-connections/sticktime-wifi.nmconnection"))

    def test_wifi_open_hidden_hex(self):
        self.baked(wifi_ssid="Cafe", wifi_password="", wifi_hidden="on")
        self.run_setup()
        kf = kf_parse(self.r("/etc/NetworkManager/system-connections/sticktime-wifi.nmconnection"))
        self.assertNotIn("wifi-security", kf)
        self.assertEqual(kf["wifi"]["hidden"], "true")
        hexpsk = "ab" * 32
        self.settings(wifi_ssid="Lab", wifi_password=hexpsk)
        self.run_setup()
        kf = kf_parse(self.r("/etc/NetworkManager/system-connections/sticktime-wifi.nmconnection"))
        self.assertEqual(kf["wifi-security"]["psk"], hexpsk)
        self.assertNotIn("hidden", kf["wifi"])

    def test_country_invalid(self):
        for cc in ("XX", "USA", "1", "u s"):
            self.settings(wifi_country=cc)
            self.clear_calls()
            c, _ = self.run_setup()
            self.assertNotIn("regdom", self.r("/boot/firmware/cmdline.txt"))
            self.assertEqual(self.calls("iw"), [])
            self.assertTrue(any("wifi_country" in w for w in c.warnings), cc)
        self.settings(wifi_country="")
        c, _ = self.run_setup()
        self.assertFalse(any("wifi_country" in w for w in c.warnings))

    def test_hotspot_profile_fallbacks(self):
        self.baked(hotspot_ssid="x" * 40, hotspot_password="short")
        c, _ = self.run_setup()
        kf = kf_parse(self.r("/etc/NetworkManager/system-connections/sticktime-hotspot.nmconnection"))
        self.assertEqual(kf["wifi"]["ssid"], "StickTime")
        self.assertEqual(kf["wifi-security"]["psk"], "sticktime")
        self.assertEqual(sum(1 for w in c.warnings if "hotspot" in w), 2)
        self.assertNotIn("short", " ".join(c.warnings))       # the bad password is not echoed

    def test_bluetooth_rfkill(self):
        self.baked(bluetooth="off")
        self.run_setup()
        self.assertIn("block bluetooth", self.argvs("rfkill"))
        nt = os.name == "nt"                                  # ':' in a name is a stream on NTFS
        if not nt:
            self.assertEqual(self.r("/var/lib/systemd/rfkill/platform-fe215040.serial:bluetooth"), "1\n")
        self.settings(bluetooth="on")
        self.run_setup()
        self.assertIn("unblock bluetooth", self.argvs("rfkill"))
        if not nt:
            self.assertEqual(self.r("/var/lib/systemd/rfkill/platform-fe215040.serial:bluetooth"), "0\n")

    def test_keep_logs(self):
        conf = "/etc/systemd/journald.conf.d/91-sticktime-keep-logs.conf"
        restart = "restart systemd-journald.service"
        self.baked()
        c, _ = self.run_setup()                             # off, the default: the journal stays in RAM
        self.assertFalse(self.exists(conf))
        self.assertNotIn(restart, self.argvs("systemctl"))
        self.assertIs(c.report["keep_logs"], False)
        self.settings(keep_logs="on")
        c, _ = self.run_setup()
        self.assertIn("Storage=persistent\nSystemMaxUse=50M\n", self.r(conf))
        self.assertIn(restart, self.argvs("systemctl"))
        self.assertEqual(self.argvs("journalctl"), ["--flush"])     # this boot's log so far goes to the card too
        self.assertIn("logs kept across restarts", c.done)
        self.clear_calls()
        c, _ = self.run_setup()                             # already so: journald isn't restarted again
        self.assertNotIn(restart, self.argvs("systemctl"))
        self.assertNotIn("logs kept across restarts", c.done)
        self.settings(keep_logs="off")
        c, _ = self.run_setup()
        self.assertFalse(self.exists(conf))
        self.assertIn(restart, self.argvs("systemctl"))
        self.assertIn("logs in RAM only", c.done)

    def test_governor(self):
        self.baked()
        self.run_setup()
        self.assertEqual(self.r("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"), "performance")
        shutil.rmtree(os.path.join(self.root, "sys/devices/system/cpu/cpufreq"))
        c, failed = self.run_setup()                         # QEMU: no cpufreq
        self.assertEqual(failed, [])

    def test_no_wifi_no_nm(self):
        self.state["nm"] = {"running": False}
        self.save_state()
        for f in ("iw", "rfkill"):
            os.unlink(os.path.join(self.bin, f))
        self.baked(wifi_ssid="Home", wifi_password="secret123")
        c, failed = self.run_setup()
        self.assertEqual(failed, [])
        self.assertTrue(self.exists("/etc/NetworkManager/system-connections/sticktime-wifi.nmconnection"))

    # ---- reboot once (E14)
    def test_video_change_reboots_once(self):
        self.baked()
        self.settings(video="hdmi")
        c, failed = self.run_setup()
        self.assertEqual(failed, [])
        self.assertTrue(c.report["reboot"])
        self.assertIn("--no-block reboot", self.argvs("systemctl"))
        self.assertIn("enable_tvout=0", self.r("/boot/firmware/config.txt"))
        guard = json.loads(self.r("/var/lib/sticktime/boot-config.json"))
        self.assertEqual(guard["count"], 1)
        self.assertIn("rebooting once", self.r("/dev/kmsg"))
        # the boot after the reboot: nothing changes, no reboot, the guard says so
        self.clear_calls()
        c, _ = self.run_setup()
        self.assertFalse(c.report["reboot"])
        self.assertNotIn("--no-block reboot", self.argvs("systemctl"))
        self.assertIn("last_ok", json.loads(self.r("/var/lib/sticktime/boot-config.json")))

    def test_reboot_guard_same_content(self):
        self.baked()
        old_cfg = self.r("/boot/firmware/config.txt")
        self.settings(video="hdmi")
        c, _ = self.run_setup()
        self.assertTrue(c.report["reboot"])
        # something put the old file back (a read-only card that "lost" the write, another tool)
        self.w("/boot/firmware/config.txt", old_cfg)
        self.clear_calls()
        c, _ = self.run_setup()
        self.assertFalse(c.report["reboot"])
        self.assertTrue(c.report["reboot_needed"])
        self.assertNotIn("--no-block reboot", self.argvs("systemctl"))
        self.assertTrue(any("not rebooting a second time" in w for w in c.warnings))

    def test_reboot_guard_limit(self):
        self.baked()
        reboots = 0
        for i in range(5):                    # a different video config at every boot, no clean boot between
            self.settings(video="hdmi" if i % 2 == 0 else "av", tv=["ntsc", "pal", "secam", "ntsc-j", "pal-m"][i])
            c, _ = self.run_setup()
            reboots += c.report["reboot"]
        self.assertEqual(reboots, setup.MAX_REBOOTS)

    def test_noreboot_token_and_env(self):
        self.baked()
        self.settings(video="hdmi")
        self.w("/proc/cmdline", "root=/dev/mmcblk0p2 sticktime.noreboot=1 sticktime.ci=1\n")
        c, _ = self.run_setup()
        self.assertFalse(c.report["reboot"])
        self.assertTrue(c.report["reboot_needed"])
        self.assertNotIn("--no-block reboot", self.argvs("systemctl"))
        self.assertIn("STICKTIME-CI: setup done", self.r("/dev/console"))
        self.assertNotIn("STICKTIME-CI", self.r("/dev/kmsg"))

    def test_apply_mode_never_reboots(self):
        self.baked()
        self.settings(video="hdmi")
        c, _ = self.run_setup("apply")
        self.assertFalse(c.report["reboot"])
        self.assertTrue(c.report["reboot_needed"])
        self.assertNotIn("--no-block reboot", self.argvs("systemctl"))
        self.assertFalse(self.exists("/var/lib/sticktime/boot-config.json"))

    def test_vtx_control_adds_its_uart_and_reboots_once(self):
        self.baked()
        cfg = self.r("/boot/firmware/config.txt")
        self.assertNotIn("dtoverlay=uart", cfg)
        # the default port is a single pin (gpio4): no overlay, nothing to reboot for
        self.settings(vtx="auto")
        c, failed = self.run_setup()
        self.assertEqual(failed, [])
        self.assertFalse(c.report["reboot"])
        self.assertEqual(self.r("/boot/firmware/config.txt"), cfg)
        self.assertEqual(c.report["video"]["vtx_uart"], "")
        # the two-pin mode: the UART's overlay, a reboot once
        self.settings(vtx="auto", vtx_port="uart3")
        c, failed = self.run_setup()
        self.assertTrue(c.report["reboot"])
        self.assertIn("\ndtoverlay=uart3\n", self.r("/boot/firmware/config.txt"))
        self.assertEqual(c.report["video"]["vtx_uart"], "uart3")
        self.clear_calls()
        c, _ = self.run_setup()                                  # the boot after: nothing to do
        self.assertFalse(c.report["reboot"])
        # protocol changes are no reason to reboot; the UART is
        self.settings(vtx="tramp", vtx_port="uart3", vtx_send_only="on", vtx_band="R")
        c, _ = self.run_setup()
        self.assertFalse(c.report["reboot"])
        self.settings(vtx="tramp", vtx_port="uart4")
        c, _ = self.run_setup()
        self.assertTrue(c.report["reboot"])
        self.assertIn("\ndtoverlay=uart4\n", self.r("/boot/firmware/config.txt"))
        self.assertNotIn("dtoverlay=uart3", self.r("/boot/firmware/config.txt"))
        # back to a single pin, or off: the overlay goes (GPIO8/9 free again)
        self.settings(vtx="tramp", vtx_port="gpio17")
        c, _ = self.run_setup()
        self.assertNotIn("dtoverlay=uart", self.r("/boot/firmware/config.txt"))
        self.settings(vtx="smartaudio", vtx_port="uart5")
        c, _ = self.run_setup("apply")
        self.assertIn("\ndtoverlay=uart5\n", self.r("/boot/firmware/config.txt"))
        self.settings(vtx="off", vtx_port="uart5")
        c, _ = self.run_setup("apply")
        self.assertNotIn("dtoverlay=uart", self.r("/boot/firmware/config.txt"))
        # a bad port name: the default (gpio4, no overlay), with a warning
        self.settings(vtx="smartaudio", vtx_port="uart9")
        c, _ = self.run_setup("apply")
        self.assertNotIn("dtoverlay=uart", self.r("/boot/firmware/config.txt"))
        self.assertTrue(any("vtx_port" in w for w in c.warnings), c.warnings)

    def test_audio_change_needs_no_reboot(self):
        self.baked()
        self.settings(audio_out="off")
        c, _ = self.run_setup()
        self.assertFalse(c.report["reboot"])
        self.assertIn("dtparam=audio=off", self.r("/boot/firmware/config.txt"))

    def test_missing_or_broken_boot_files(self):
        self.baked()
        os.unlink(os.path.join(self.root, "boot/firmware/config.txt"))
        self.w("/boot/firmware/cmdline.txt", "console=tty1 quiet\n")
        self.settings(video="hdmi")
        c, failed = self.run_setup()
        self.assertEqual(failed, [])
        self.assertFalse(self.exists("/boot/firmware/config.txt"))
        self.assertEqual(self.r("/boot/firmware/cmdline.txt"), "console=tty1 quiet\n")
        self.assertFalse(c.report["reboot"])
        self.assertEqual(sum(1 for w in c.warnings if "config.txt" in w or "cmdline.txt" in w), 2)

    def test_readonly_fat(self):
        self.baked()
        self.settings(video="hdmi")
        real = st.atomic_write

        def ro(path, data, mode=0o644):
            if path.startswith("/boot/firmware"):
                raise OSError(30, "Read-only file system")
            return real(path, data, mode)
        st.atomic_write = ro
        try:
            c, failed = self.run_setup()
        finally:
            st.atomic_write = real
        self.assertIn("boot-config", failed)
        self.assertFalse(c.report["reboot"])
        self.assertEqual(self.r("/etc/hostname"), "sticktime\n")   # the other steps still ran

    def test_one_broken_step_does_not_stop_the_rest(self):
        self.baked()
        self.settings(hostname="other", video="hdmi")
        real = setup.step_ssh
        setup.step_ssh = lambda c, s: 1 / 0
        try:
            c, failed = self.run_setup()
        finally:
            setup.step_ssh = real
        self.assertEqual(failed, ["ssh"])
        self.assertEqual(self.r("/etc/hostname"), "other\n")
        self.assertTrue(c.report["reboot"])

    def test_corrupt_settings_boot(self):
        self.baked()
        self.w("/boot/firmware/sticktime.txt", b"\x00\x01\x02" * 100)
        c, failed = self.run_setup()
        self.assertEqual(failed, [])
        self.assertEqual(self.r("/boot/firmware/sticktime.txt"), TEMPLATE)
        self.assertEqual(c.report["hostname"], "sticktime")
        self.assertFalse(c.report["reboot"])

    def test_cli_apply_json(self):
        self.baked()
        self.settings(video="hdmi")
        env = dict(os.environ)
        r = subprocess.run([sys.executable, "-I", "-B", os.path.join(LIB, "sticktime-setup"), "--root", self.root, "apply"],
                           capture_output=True, text=True, env=env, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        j = json.loads(r.stdout)
        self.assertTrue(j["ok"])
        self.assertTrue(j["reboot_needed"])
        r = subprocess.run([sys.executable, "-I", "-B", os.path.join(LIB, "sticktime-setup"), "--root", self.root, "nonsense"],
                           capture_output=True, text=True, env=env, timeout=60)
        self.assertEqual(r.returncode, 2)


# ================================================================ sticktime-wifi
SCAN = (r"*:Home:70:WPA2:6:2437 MHz" "\n"
        r" :Home:40:WPA2:36:5180 MHz" "\n"
        r" :My\:Colon\\Net:55:WPA1 WPA2:11:2462 MHz" "\n"
        r" ::80:WPA2:1:2412 MHz" "\n"
        r" :Cafe:30:--:6:2437 MHz" "\n"
        r" :ExpressLRS TX:90::1:2412 MHz" "\n"
        r" :Office:20:WPA2 802.1X:6:2437 MHz" "\n"
        r" :New:25:WPA3:6:2437 MHz" "\n"
        "garbage line\n")


class TestWifi(Env):
    def wifi(self, *args):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(io.StringIO()):
            rc = wifi.main(list(args))
        return rc, json.loads(out.getvalue())

    def test_parse_scan(self):
        nets = wifi.parse_scan(SCAN, "Home")
        names = [n["ssid"] for n in nets]
        self.assertEqual(names[0], "Home")                   # the active one first
        self.assertEqual(names.count("Home"), 1)
        home = nets[0]
        self.assertEqual(home["signal"], 70)
        self.assertTrue(home["active"] and home["known"])
        self.assertEqual(home["bands"], ["2.4", "5"])
        self.assertIn("My:Colon\\Net", names)
        self.assertNotIn("", names)                          # hidden networks are left out
        sec = {n["ssid"]: n["security"] for n in nets}
        self.assertEqual(sec["Cafe"], "open")
        self.assertEqual(sec["ExpressLRS TX"], "open")
        self.assertEqual(sec["Office"], "WPA2-Enterprise")
        self.assertEqual(sec["New"], "WPA3")
        self.assertEqual(wifi.parse_scan("", ""), [])

    def test_scan_writes_cache(self):
        self.state["nm"]["scan"] = SCAN
        self.save_state()
        rc, j = self.wifi("scan")
        self.assertEqual(rc, 0)
        self.assertEqual(j["device"], "wlan0")
        cache = json.loads(self.r("/run/sticktime/wifi-scan.json"))
        self.assertEqual(cache["networks"], j["networks"])
        self.assertTrue(any("--rescan yes" in a for a in self.argvs("nmcli")))

    def test_scan_no_device_and_no_nm(self):
        self.state["nm"]["devices"] = [["eth0", "ethernet", "connected", "Wired"]]
        self.save_state()
        rc, j = self.wifi("scan")
        self.assertEqual(rc, 2)
        self.assertEqual(j["error"], "no WiFi device")
        self.state["nm"] = {"running": False}
        self.save_state()
        rc, j = self.wifi("scan")
        self.assertEqual(rc, 1)
        self.assertIn("not running", j["error"])

    def test_scan_busy_falls_back(self):
        self.state["nm"]["scan"] = SCAN
        self.state["nm"]["rescan_fail"] = True
        self.save_state()
        rc, j = self.wifi("scan")
        self.assertEqual(rc, 0)
        self.assertTrue(j["networks"])
        self.state["nm"]["scan_fail"] = True
        self.save_state()
        rc, j = self.wifi("scan")                            # the last results, marked as cached
        self.assertEqual(rc, 0)
        self.assertTrue(j["cached"])

    def test_scan_in_ap_mode(self):
        self.state["nm"]["scan"] = SCAN
        self.save_state()
        self.wifi("scan")
        self.state["nm"]["devices"] = [["wlan0", "wifi", "connected", "sticktime-hotspot"]]
        self.state["nm"]["scan"] = ""
        self.save_state()
        self.clear_calls()
        rc, j = self.wifi("scan")
        self.assertEqual(rc, 0)
        self.assertTrue(j["cached"])
        self.assertIn("hotspot", j["note"])
        self.assertFalse(any("--rescan yes" in a for a in self.argvs("nmcli")))

    def test_apply(self):
        self.settings(wifi_ssid="Home", wifi_password="secret123", wifi_country="DE", wifi_module_ip="10.0.0.1")
        st.write_keyfile("sticktime-module", st.keyfile_module("ExpressLRS TX", "expresslrs"))
        rc, j = self.wifi("apply")
        self.assertEqual(rc, 0, j)
        self.assertTrue(j["ok"])
        self.assertTrue(self.exists("/etc/NetworkManager/system-connections/sticktime-wifi.nmconnection"))
        self.assertFalse(self.exists("/etc/NetworkManager/system-connections/sticktime-module.nmconnection"))
        self.assertEqual(st.load_settings().get("wifi_module_ip"), "")
        a = self.argvs("nmcli")
        self.assertIn("radio wifi on", a)
        self.assertIn("connection delete id sticktime-module", a)
        self.assertTrue(any(x.startswith("connection load ") for x in a))
        self.assertIn("reg set DE", self.argvs("iw"))
        self.assertGreater(st.held_until(), time.time())
        # no hotspot daemon to poke (no pid file): the connect runs as its own background helper
        self.assertTrue(any(a[-2:] == ["connect", "sticktime-wifi"] for a in self.spawned), self.spawned)

    def test_apply_hidden_flag(self):
        self.settings(wifi_ssid="Secret", wifi_password="secret123")
        rc, j = self.wifi("apply", "--hidden")
        self.assertEqual(rc, 0)
        self.assertEqual(st.load_settings().get("wifi_hidden"), "on")
        kf = kf_parse(self.r("/etc/NetworkManager/system-connections/sticktime-wifi.nmconnection"))
        self.assertEqual(kf["wifi"]["hidden"], "true")
        rc, j = self.wifi("apply")                         # no flag: stays hidden (not seen in a scan)
        self.assertEqual(st.load_settings().get("wifi_hidden"), "on")
        self.w("/run/sticktime/wifi-scan.json", json.dumps({"networks": [{"ssid": "Secret"}], "at": 1}))
        rc, j = self.wifi("apply")                         # seen by name: not hidden after all
        self.assertEqual(st.load_settings().get("wifi_hidden"), "off")
        self.assertNotIn("hidden", kf_parse(self.r("/etc/NetworkManager/system-connections/sticktime-wifi.nmconnection"))["wifi"])

    def test_apply_errors_and_offline(self):
        self.settings(wifi_ssid="Home", wifi_password="short")
        rc, j = self.wifi("apply")
        self.assertEqual(rc, 1)
        self.assertEqual(j["error"], "BadRequest")
        self.settings(wifi_ssid="Home", wifi_password="secret123")
        self.state["nm"] = {"running": False}
        self.save_state()
        rc, j = self.wifi("apply")
        self.assertEqual(rc, 0)
        self.assertIn("next boot", j["message"])
        self.settings(wifi_ssid="", wifi_password="")
        self.state["nm"] = {"running": True, "devices": []}
        self.save_state()
        rc, j = self.wifi("apply")
        self.assertIn("removed", j["message"])

    def test_module_ap(self):
        self.state["nm"]["scan"] = SCAN
        self.save_state()
        rc, j = self.wifi("module-ap", "elrs")
        self.assertEqual(rc, 0, j)
        self.assertEqual(j["module_ip"], "10.0.0.1")
        kf = kf_parse(self.r("/etc/NetworkManager/system-connections/sticktime-module.nmconnection"))
        self.assertEqual(kf["wifi"]["ssid"], "ExpressLRS TX")
        self.assertNotIn("wifi-security", kf)                  # the scan says it is open
        self.assertEqual(kf["connection"]["autoconnect"], "false")
        self.assertEqual(st.load_settings().get("wifi_module_ip"), "10.0.0.1")
        self.assertTrue(any(a[-2:] == ["connect", "sticktime-module"] for a in self.spawned), self.spawned)

    def test_module_ap_not_found_and_bad(self):
        self.state["nm"]["scan"] = r" :Home:70:WPA2:6:2437 MHz" "\n"
        self.save_state()
        rc, j = self.wifi("module-ap", "tbs")
        self.assertEqual(rc, 1)
        self.assertEqual(j["error"], "NotFound")
        rc, j = self.wifi("module-ap", "frsky")
        self.assertEqual(j["error"], "BadRequest")
        self.state["nm"]["scan"] = r" :tbs_crossfire_ABCDEF:60:WPA2:1:2412 MHz" "\n"
        self.save_state()
        rc, j = self.wifi("module-ap", "tbs")
        self.assertEqual(j["error"], "BadRequest")             # it has a password we don't know
        rc, j = self.wifi("module-ap", "tbs", "tbspassword")
        self.assertEqual(rc, 0, j)
        self.assertEqual(j["module_ip"], "192.168.4.1")

    def test_connect_falls_back_to_hotspot(self):
        self.state["nm"]["up_ok"] = {"sticktime-wifi": False}
        self.state["nm"]["devices"] = [["wlan0", "wifi", "connected", "sticktime-hotspot"]]
        self.save_state()
        rc, j = self.wifi("connect", "sticktime-wifi")
        self.assertEqual(rc, 1)
        a = self.argvs("nmcli")
        self.assertIn("connection down id sticktime-hotspot", a)
        self.assertTrue(a[-1].endswith("connection up id sticktime-hotspot"), a[-1])
        self.settings(hotspot="off")
        self.clear_calls()
        self.wifi("connect", "sticktime-wifi")
        self.assertFalse(any("up id sticktime-hotspot" in x for x in self.argvs("nmcli")))

    def test_usage_and_internal_errors(self):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(io.StringIO()):
            self.assertEqual(wifi.main([]), 2)
            self.assertEqual(wifi.main(["bogus"]), 2)
        real = wifi.scan
        wifi.scan = lambda rescan=True: 1 / 0
        try:
            rc, j = self.wifi("scan")
        finally:
            wifi.scan = real
        self.assertEqual(rc, 1)
        self.assertEqual(j["error"], "Internal")

    def test_cli_scan(self):
        self.state["nm"]["scan"] = SCAN
        self.save_state()
        r = subprocess.run([sys.executable, "-I", "-B", os.path.join(LIB, "sticktime-wifi"), "--root", self.root, "scan"],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads(r.stdout)["networks"][0]["ssid"], "Home")


# ================================================================ sticktime-hotspot policy
class TestHotspotPolicy(unittest.TestCase):
    def obs(self, conn="", stations=None, has_client=True, held=False):
        return {"connection": conn, "stations": stations, "has_client": has_client, "held": held}

    def test_no_client_profile_starts_ap_at_once(self):
        s = hotspot.new_state(0)
        self.assertEqual(hotspot.decide("auto", 0, s, self.obs(has_client=False)), "ap_up")

    def test_boot_wait(self):
        s = hotspot.new_state(0)
        self.assertIsNone(hotspot.decide("auto", 10, s, self.obs()))
        self.assertIsNone(hotspot.decide("auto", hotspot.BOOT_WAIT - 1, s, self.obs()))
        self.assertEqual(hotspot.decide("auto", hotspot.BOOT_WAIT, s, self.obs()), "ap_up")

    def test_connected_then_lost(self):
        s = hotspot.new_state(0)
        self.assertIsNone(hotspot.decide("auto", 5, s, self.obs("sticktime-wifi")))
        self.assertEqual(s["phase"], "client")
        self.assertIsNone(hotspot.decide("auto", 100, s, self.obs()))
        self.assertIsNone(hotspot.decide("auto", 100 + hotspot.LOST_WAIT - 1, s, self.obs()))
        self.assertEqual(hotspot.decide("auto", 100 + hotspot.LOST_WAIT, s, self.obs()), "ap_up")
        # back before the timeout resets it
        s = hotspot.new_state(0)
        hotspot.decide("auto", 5, s, self.obs("sticktime-wifi"))
        hotspot.decide("auto", 10, s, self.obs())
        hotspot.decide("auto", 20, s, self.obs("sticktime-wifi"))
        self.assertIsNone(hotspot.decide("auto", 20 + hotspot.LOST_WAIT, s, self.obs()))

    def test_any_other_connection_is_fine(self):
        s = hotspot.new_state(0)
        self.assertIsNone(hotspot.decide("auto", 999, s, self.obs("preconfigured")))
        self.assertIsNone(hotspot.decide("auto", 999, s, self.obs("sticktime-module")))

    def test_ap_never_cuts_off_users(self):
        s = hotspot.new_state(0)
        t = hotspot.BOOT_WAIT
        hotspot.decide("auto", t, s, self.obs())
        for i in range(1, 200):
            now = t + i * 30
            self.assertIsNone(hotspot.decide("auto", now, s, self.obs(hotspot.st.HOTSPOT_ID, stations=1)))
            self.assertIsNone(hotspot.decide("auto", now + 1, s, self.obs(hotspot.st.HOTSPOT_ID, stations=None)))

    def test_ap_idle_retries_client_with_backoff(self):
        s = hotspot.new_state(0)
        ap = hotspot.st.HOTSPOT_ID
        hotspot.decide("auto", 0, s, self.obs(ap, stations=0))
        tries = []
        now = 0
        while now < 4000:
            now += 5
            act = hotspot.decide("auto", now, s, self.obs(ap, stations=0))
            if act == "try_client":
                tries.append(now)
                hotspot.after_try(s, now, False)
                hotspot.decide("auto", now + 1, s, self.obs(ap, stations=0))
        gaps = [b - a for a, b in zip(tries, tries[1:])]
        self.assertGreaterEqual(tries[0], hotspot.RETRY_MIN)
        self.assertEqual(gaps, sorted(gaps))                  # slower and slower
        self.assertLessEqual(max(gaps), hotspot.RETRY_MAX + 10)
        self.assertEqual(s["backoff"], hotspot.RETRY_MAX)

    def test_ap_without_client_profile_stays(self):
        s = hotspot.new_state(0)
        for now in range(0, 5000, 50):
            self.assertIsNone(hotspot.decide("auto", now, s, self.obs(hotspot.st.HOTSPOT_ID, 0, has_client=False)))

    def test_try_now_and_hold(self):
        s = hotspot.new_state(0)
        s["try_now"] = True
        self.assertEqual(hotspot.decide("auto", 1, s, self.obs(hotspot.st.HOTSPOT_ID, stations=3)), "try_client")
        self.assertFalse(s["try_now"])
        self.assertIsNone(hotspot.decide("auto", 999, hotspot.new_state(0), self.obs(held=True)))

    def test_mode_on(self):
        s = hotspot.new_state(0)
        self.assertEqual(hotspot.decide("on", 0, s, self.obs("sticktime-wifi")), "ap_up")
        self.assertIsNone(hotspot.decide("on", 1, s, self.obs(hotspot.st.HOTSPOT_ID, 0)))
        s["try_now"] = True
        self.assertIsNone(hotspot.decide("on", 2, s, self.obs(hotspot.st.HOTSPOT_ID, 0)))

    def test_success_resets_backoff(self):
        s = hotspot.new_state(0)
        hotspot.after_try(s, 10, False)
        hotspot.after_try(s, 20, False)
        self.assertEqual(s["backoff"], hotspot.RETRY_MIN * 4)
        hotspot.after_try(s, 30, True)
        self.assertEqual(s["backoff"], hotspot.RETRY_MIN)
        self.assertEqual(s["phase"], "client")


class TestHotspotDaemon(Env):
    def test_off_exits(self):
        self.settings(hotspot="off")
        with redirect_stderr(io.StringIO()):
            self.assertEqual(hotspot.Daemon().run(), 0)
        self.assertEqual(self.calls("nmcli"), [])

    def test_observe(self):
        self.state["nm"]["devices"] = [["wlan0", "wifi", "connecting (getting IP configuration)", "sticktime-wifi"]]
        self.save_state()
        o = hotspot.observe("wlan0")
        self.assertEqual(o["connection"], "sticktime-wifi")
        self.assertFalse(o["has_client"])
        self.state["nm"]["devices"] = [["wlan0", "wifi", "connected", "sticktime-hotspot"]]
        self.state["stations"] = 2
        self.save_state()
        st.write_keyfile("sticktime-wifi", st.keyfile_client("Home", ""))
        o = hotspot.observe("wlan0")
        self.assertEqual(o["stations"], 2)
        self.assertTrue(o["has_client"])
        self.state["nm"]["devices"] = [["wlan0", "wifi", "disconnected", "--"]]
        self.save_state()
        self.assertEqual(hotspot.observe("wlan0")["connection"], "")

    def test_ap_up_failure_backs_off_quietly(self):
        self.state["nm"]["up_ok"] = False
        self.save_state()
        d = hotspot.Daemon()
        err = io.StringIO()
        with redirect_stderr(err):
            for _ in range(5):
                self.assertFalse(d.ap_up("wlan0"))
        self.assertEqual(d.ap_failures, 5)
        self.assertLessEqual(err.getvalue().count("did not start"), 2)
        self.state["nm"]["up_ok"] = True
        self.save_state()
        with redirect_stderr(io.StringIO()):
            self.assertTrue(d.ap_up("wlan0"))
        self.assertEqual(d.ap_failures, 0)


# ================================================================ sticktime-bt
class TestBluetooth(Env):
    def test_parse(self):
        devs = bt.parse_devices("\x1b[0;94m[bluetooth]\x1b[0m# Device AA:BB:CC:DD:EE:01 ELRS Joystick\n"
                                "Device aa:bb:cc:dd:ee:02\nDevice AA:BB:CC:DD:EE:03 Xbox Wireless Controller\nnoise\n")
        self.assertEqual(devs, [("AA:BB:CC:DD:EE:01", "ELRS Joystick"), ("AA:BB:CC:DD:EE:02", ""),
                                ("AA:BB:CC:DD:EE:03", "Xbox Wireless Controller")])
        info = bt.parse_info("Device AA:BB:CC:DD:EE:01 (public)\n\tName: ELRS Joystick\n\tIcon: input-gaming\n"
                             "\tPaired: yes\n\tTrusted: no\n\tConnected: no\n\tUUID: Human Interface Device (00001812-...)\n"
                             "\tUUID: Battery Service (0000180f-...)\n")
        self.assertTrue(info["paired"])
        self.assertFalse(info["trusted"])
        self.assertEqual(len(info["uuids"]), 2)

    def test_wanted(self):
        self.assertEqual(bt.wanted("ELRS Joystick"), "elrs")
        self.assertEqual(bt.wanted("ExpressLRS Joystick 2"), "elrs")
        self.assertEqual(bt.wanted("Pad", {"icon": "input-gaming"}), "gamepad")
        self.assertEqual(bt.wanted("x", {"class": "0x00002508"}), "gamepad")
        self.assertEqual(bt.wanted("8BitDo SN30 Pro"), "gamepad")
        self.assertIsNone(bt.wanted("Logitech K380", {"icon": "input-keyboard"}))
        self.assertIsNone(bt.wanted("Xbox Controller", {"icon": "input-keyboard"}))
        self.assertIsNone(bt.wanted("Galaxy S24", {"icon": "phone"}))
        self.assertIsNone(bt.wanted("", {}))

    def test_off_and_no_adapter(self):
        self.settings(bluetooth="off")
        with redirect_stderr(io.StringIO()):
            self.assertEqual(bt.Daemon().run(), 0)
        self.assertEqual(self.calls("bluetoothctl"), [])
        self.assertEqual(bt.adapters(), [])
        os.makedirs(os.path.join(self.root, "sys/class/bluetooth/hci0"))
        self.assertEqual(bt.adapters(), ["hci0"])

    def session(self):
        return [x[len("session "):] for x in self.argvs("bluetoothctl") if x.startswith("session ")]

    def test_pair_trust_connect(self):
        mac = "AA:BB:CC:DD:EE:01"
        self.state["bt"] = {"devices": f"Device {mac} ELRS Joystick\nDevice 11:22:33:44:55:66 Phone\n",
                            "devices Paired": "", f"info {mac}": "\tName: ELRS Joystick\n\tPaired: no\n\tConnected: no\n",
                            "session": {f"connect {mac}": "Connection successful", f"pair {mac}": "Pairing successful",
                                        f"trust {mac}": f"Changing {mac} trust succeeded"}}
        self.save_state()
        d = bt.Daemon()
        bt.wifi_joystick_live = lambda: False
        with redirect_stderr(io.StringIO()):
            self.assertTrue(d.tick())
        # the agent is registered (and made the default) before anything else: BlueZ 5.82 refuses a Just
        # Works pairing with "No agent available" when the pair command comes first
        self.assertEqual(self.session(), ["agent NoInputNoOutput", "default-agent", f"connect {mac}", f"pair {mac}",
                                          f"trust {mac}", "quit"])
        a = self.argvs("bluetoothctl")
        self.assertFalse(any("11:22:33:44:55:66" in x for x in a))    # a phone is not even looked at
        self.assertFalse(any("scan" in x for x in a))                 # connected: no scanning

    def test_connect_retries_after_radio_drops(self):
        # the Pi 4's shared WiFi/BT radio: "Connection Failed to be Established" (0x3e) on the first tries
        mac = "AA:BB:CC:DD:EE:01"
        drop = "Failed to connect: org.bluez.Error.Failed le-connection-abort-by-local"
        self.state["bt"] = {"devices": f"Device {mac} ELRS Joystick\n", f"info {mac}": "\tPaired: no\n\tConnected: no\n",
                            "session": {f"connect {mac}": [drop, drop, "Connection successful"],
                                        f"pair {mac}": "Pairing successful"}}
        self.save_state()
        bt.wifi_joystick_live = lambda: True
        with redirect_stderr(io.StringIO()):
            self.assertTrue(bt.Daemon().tick())
        self.assertEqual(self.session().count(f"connect {mac}"), 3)
        self.assertIn(f"pair {mac}", self.session())

    def test_already_paired_only_connects(self):
        mac = "AA:BB:CC:DD:EE:01"
        self.state["bt"] = {"devices": f"Device {mac} ELRS Joystick\n", "devices Paired": f"Device {mac} ELRS Joystick\n",
                            f"info {mac}": "\tPaired: yes\n\tTrusted: yes\n\tConnected: no\n",
                            "session": {f"connect {mac}": "Connection successful"}}
        self.save_state()
        bt.wifi_joystick_live = lambda: True
        with redirect_stderr(io.StringIO()):
            self.assertTrue(bt.Daemon().tick())
        self.assertNotIn(f"pair {mac}", self.session())
        self.assertNotIn(f"trust {mac}", self.session())

    def test_pair_failure_backoff(self):
        mac = "AA:BB:CC:DD:EE:01"
        self.state["bt"] = {"devices": f"Device {mac} ELRS Joystick\n", f"info {mac}": "\tPaired: no\n",
                            "session": {f"connect {mac}": "Connection successful",
                                        f"pair {mac}": "Failed to pair: org.bluez.Error.AuthenticationFailed"}}
        self.save_state()
        d = bt.Daemon()
        bt.wifi_joystick_live = lambda: True                           # streaming: no scans either
        log = io.StringIO()
        with redirect_stderr(log):
            d.tick()
            d.tick()                                                   # within the 10 s back-off: no new try
        self.assertEqual(self.session().count(f"pair {mac}"), 1)
        self.assertEqual(d.failures[mac], 1)
        self.assertIn("AuthenticationFailed", log.getvalue())
        self.assertFalse(any("scan" in x for x in self.argvs("bluetoothctl")))

    def test_radio_gives_up_after_tries(self):
        mac = "AA:BB:CC:DD:EE:01"
        self.state["bt"] = {"devices": f"Device {mac} ELRS Joystick\n", f"info {mac}": "\tPaired: no\n",
                            "session": {f"connect {mac}": "Failed to connect: org.bluez.Error.Failed"}}
        self.save_state()
        bt.wifi_joystick_live = lambda: True
        log = io.StringIO()
        with redirect_stderr(log):
            self.assertFalse(bt.Daemon().tick())
        self.assertEqual(self.session().count(f"connect {mac}"), bt.CONNECT_TRIES)
        self.assertNotIn(f"pair {mac}", self.session())
        self.assertIn("no connection after", log.getvalue())


# ================================================================ the image build: the game
class TestBuildGame(unittest.TestCase):
    """image/build.sh's step that puts StickTime from its release zip on the card: its Python, taken
    out of build.sh and run as the build runs it (python3 -I - ZIP SD/SCRIPTS/TOOLS VERSION)"""
    FIXTURE = os.path.join(REPO, "build", "agent-updater", "fixture", "StickTime-1.7.2-sdcard.zip")

    @classmethod
    def setUpClass(cls):
        with open(os.path.join(REPO, "image", "build.sh"), encoding="utf-8") as f:
            text = f.read().replace("\r\n", "\n")
        m = re.search(r'GAME_VERSION=\$\(python3 -I - "\$GAME_ZIP" "\$P/sd/SCRIPTS/TOOLS" "\$\{GAME_VERSION:-\}" '
                      r"<<'PY'\n(.*?)\nPY\n", text, re.S)
        assert m, "build.sh's game step not found"
        cls.code = m.group(1)

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="stbuild-game-")
        self.dst = os.path.join(self.tmp, "sd", "SCRIPTS", "TOOLS")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    @staticmethod
    def entries(version="1.7.2"):
        pad = ("-- " + "x" * 70 + "\n") * 200
        e = {"SCRIPTS/": None, "SCRIPTS/TOOLS/": None,
             "SCRIPTS/TOOLS/StickTime.lua": f'local toolName = "TNS|StickTime|TNE"\n--[[\n  StickTime v{version}  -  sim\n]]\n{pad}'.encode(),
             "SCRIPTS/TOOLS/StickTimeTracks/README.txt": b"Your tracks: c1.txt to c8.txt\n",
             "SCRIPTS/TOOLS/StickTimeGhosts/README.txt": b"Ghosts.\n"}
        for ed, name, files in (("BW", "StickTimeBW", ("core.lua", "color.lua", "game.luac", "why.lua")),
                                ("Lite", "StickTimeLite", ("core.lua", "color.lua", "game.luac", "why.lua", "q1.txt", "t1.txt"))):
            e[f"SCRIPTS/TOOLS/{name}.lua"] = f'local toolName = "TNS|StickTime {ed}|TNE"\n--[[\n  StickTime {ed} v{version}  -  loader\n]]\n'.encode()
            e[f"SCRIPTS/TOOLS/{name}/"] = None
            for n in files:
                e[f"SCRIPTS/TOOLS/{name}/{n}"] = f"-- {name}/{n}\n".encode() * 3
        return e

    def zip(self, entries):
        import zipfile
        p = os.path.join(self.tmp, "game.zip")
        with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
            for n, b in entries.items():
                z.writestr(n, b"" if b is None else b)
        return p

    def build(self, zpath, want=""):
        r = subprocess.run([sys.executable, "-I", "-", zpath, self.dst, want], input=self.code.encode(),
                           capture_output=True, timeout=60)
        return r.returncode, r.stdout.decode().strip(), r.stderr.decode()

    def tree(self):
        out = {}
        for root, _, names in os.walk(self.dst):
            for n in names:
                p = os.path.join(root, n)
                with open(p, "rb") as f:
                    out[os.path.relpath(p, self.dst).replace(os.sep, "/")] = f.read()
        return out

    def test_all_three_editions(self):
        e = self.entries()
        rc, out, err = self.build(self.zip(e), "1.7.2")
        self.assertEqual((rc, out), (0, "1.7.2"), err)
        want = {n[len("SCRIPTS/TOOLS/"):]: b for n, b in e.items() if b is not None}
        want["GAME_VERSION"] = b"1.7.2\n"
        self.assertEqual(self.tree(), want)
        self.assertIn("B&W edition: StickTimeBW.lua + StickTimeBW/ (4 files)", err)
        self.assertIn("Lite edition: StickTimeLite.lua + StickTimeLite/ (6 files)", err)
        self.assertNotIn("warning", err)

    def test_the_real_release_zip(self):
        if not os.path.isfile(self.FIXTURE):
            self.skipTest("no StickTime-1.7.2-sdcard.zip in build/agent-updater/fixture/")
        import zipfile
        rc, out, err = self.build(self.FIXTURE)
        self.assertEqual((rc, out), (0, "1.7.2"), err)
        t = self.tree()
        with zipfile.ZipFile(self.FIXTURE) as z:
            for i in z.infolist():
                if not i.is_dir():
                    self.assertEqual(t.pop(i.filename[len("SCRIPTS/TOOLS/"):]), z.read(i), i.filename)
        self.assertEqual(t, {"GAME_VERSION": b"1.7.2\n"}, "everything the zip has, nothing else")
        self.assertEqual(len(os.listdir(os.path.join(self.dst, "StickTimeBW"))), 4)
        self.assertEqual(len(os.listdir(os.path.join(self.dst, "StickTimeLite"))), 17)

    def test_an_older_zip_without_the_editions(self):
        e = {n: b for n, b in self.entries().items() if "StickTimeBW" not in n and "StickTimeLite" not in n}
        rc, out, err = self.build(self.zip(e))
        self.assertEqual((rc, out), (0, "1.7.2"), err)
        self.assertEqual(sorted(self.tree()), ["GAME_VERSION", "StickTime.lua", "StickTimeGhosts/README.txt",
                                               "StickTimeTracks/README.txt"])
        self.assertIn("warning", err)
        self.assertIn("no StickTimeBW.lua: no B&W edition", err)
        self.assertIn("no StickTimeLite.lua: no Lite edition", err)

    def test_an_edition_of_another_version_or_without_its_folder(self):
        e = self.entries()
        e["SCRIPTS/TOOLS/StickTimeBW.lua"] = e["SCRIPTS/TOOLS/StickTimeBW.lua"].replace(b"BW v1.7.2", b"BW v1.7.1")
        e = {n: b for n, b in e.items() if not n.startswith("SCRIPTS/TOOLS/StickTimeLite/")}
        rc, out, err = self.build(self.zip(e))
        self.assertEqual(rc, 0, err)
        self.assertIn("StickTimeBW.lua isn't StickTime 1.7.2's: no B&W edition", err)
        self.assertIn("no StickTimeLite/: no Lite edition", err)
        self.assertFalse([n for n in self.tree() if n.startswith(("StickTimeBW", "StickTimeLite"))])

    def test_never_a_save_or_odd_files(self):
        e = self.entries()
        e.update({"SCRIPTS/TOOLS/StickTimeLite/data.txt": b"a save shipped by mistake",
                  "SCRIPTS/TOOLS/StickTimeLite/DATA.TXT": b"again",
                  "SCRIPTS/TOOLS/StickTimeBW/x.dat": b"save", "SCRIPTS/TOOLS/StickTimeBW/c1.txt": b"a track",
                  "SCRIPTS/TOOLS/StickTimeLite/sub/x.lua": b"deeper", "SCRIPTS/TOOLS/StickTimeLite/x.png": b"png",
                  "SCRIPTS/TOOLS/StickTimeLite/a.b.lua": b"dots"})
        rc, out, err = self.build(self.zip(e))
        self.assertEqual(rc, 0, err)
        t = self.tree()
        for n in ("StickTimeLite/data.txt", "StickTimeLite/DATA.TXT", "StickTimeBW/x.dat", "StickTimeBW/c1.txt",
                  "StickTimeLite/sub/x.lua", "StickTimeLite/x.png", "StickTimeLite/a.b.lua"):
            self.assertNotIn(n, t)
            self.assertIn(f"SCRIPTS/TOOLS/{n} left out", err)
        self.assertIn("StickTimeLite/core.lua", t)

    def test_the_wrong_version_or_no_game(self):
        rc, _, err = self.build(self.zip(self.entries("1.7.3")), "1.7.2")
        self.assertNotEqual(rc, 0)
        self.assertIn("the script says v1.7.3, not 1.7.2", err)
        e = self.entries()
        del e["SCRIPTS/TOOLS/StickTime.lua"]
        rc, _, err = self.build(self.zip(e))
        self.assertNotEqual(rc, 0)
        self.assertIn("no usable SCRIPTS/TOOLS/StickTime.lua", err)


# ================================================================ the files in image/rootfs
class TestRootfsFiles(unittest.TestCase):
    ROOTFS = os.path.join(REPO, "image", "rootfs")

    def units(self):
        d = os.path.join(self.ROOTFS, "etc", "systemd", "system")
        return {n: open(os.path.join(d, n), encoding="utf-8").read() for n in os.listdir(d)}

    def test_units_reference_existing_helpers(self):
        for name, text in self.units().items():
            for m in re.finditer(r"^Exec\w*=-?(\S+)", text, re.M):
                path = m.group(1)
                if path.startswith("/usr/lib/sticktime/"):
                    helper = os.path.join(self.ROOTFS, path.lstrip("/"))
                    if os.path.basename(path) == "sticktime-update" and not os.path.exists(helper):
                        continue                                       # written by the updater's author
                    self.assertTrue(os.path.exists(helper), f"{name}: {path}")
            self.assertNotIn("\r", text, name)

    def test_main_unit(self):
        u = self.units()["sticktime.service"]
        self.assertIn("ExecStart=/boot/firmware/sticktime/bin/sticktime-pi --base /boot/firmware/sticktime "
                      "--settings /boot/firmware/sticktime.txt --libexec /usr/lib/sticktime --state /run/sticktime", u)
        for line in ("Restart=always", "TTYPath=/dev/tty1", "StartLimitIntervalSec=0", "Conflicts=getty@tty1.service",
                     "After=sticktime-setup.service"):
            self.assertIn(line, u)

    def test_setup_unit_order(self):
        u = self.units()["sticktime-setup.service"]
        for dep in ("NetworkManager.service", "sticktime.service", "avahi-daemon.service", "ssh.service", "bluetooth.service"):
            self.assertRegex(u, rf"(?m)^Before=.*\b{re.escape(dep)}")
        self.assertIn("Type=oneshot", u)
        self.assertIn("DefaultDependencies=no", u)

    def test_helpers_have_shebang_and_lf(self):
        for n in ("sticktime-setup", "sticktime-wifi", "sticktime-hotspot", "sticktime-bt", "sticktime-ci"):
            with open(os.path.join(LIB, n), "rb") as f:
                data = f.read()
            self.assertTrue(data.startswith(b"#!/usr/bin/python3 -I\n") or data.startswith(b"#!/bin/sh\n"), n)


if __name__ == "__main__":
    unittest.main()
