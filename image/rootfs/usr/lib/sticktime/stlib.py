"""Shared code of the StickTime Pi system helpers (sticktime-setup, -wifi, -hotspot, -bt).

Python 3.11+ standard library only. Every absolute path goes through p(), so the unit tests
(image/test/test_setup.py) run the helpers against a fake root directory (--root DIR or
STICKTIME_ROOT) with fake nmcli/systemctl/iw/... on PATH. Secrets (passwords, PSKs) are never
logged and never passed on a command line.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import uuid

ROOT = os.environ.get("STICKTIME_ROOT") or "/"
PROG = os.path.basename(sys.argv[0] or "sticktime")

BOOT = "/boot/firmware"
SETTINGS = BOOT + "/sticktime.txt"
BASE = BOOT + "/sticktime"
STATE = "/run/sticktime"
VARLIB = "/var/lib/sticktime"
LIBEXEC = "/usr/lib/sticktime"
TEMPLATE = LIBEXEC + "/sticktime.txt.default"   # written by image/build.sh from host/src/settings_keys.h
NM_DIR = "/etc/NetworkManager/system-connections"
NM_STATE = "/var/lib/NetworkManager/NetworkManager.state"
WIFI_ID = "sticktime-wifi"          # never "netplan-*": NM would mirror it into /etc/netplan (pios-image.md 4.1)
HOTSPOT_ID = "sticktime-hotspot"
MODULE_ID = "sticktime-module"
HOLD_FILE = STATE + "/wifi-hold"    # sticktime-wifi asks the hotspot daemon to leave wlan0 alone until then
UUID_NS = uuid.UUID("6d1f2b9e-5c3a-4f0e-9b7d-2a1c8e4f6b30")


def set_root(root):
    global ROOT
    ROOT = root or "/"


def p(path):
    """path inside the (possibly fake) root"""
    if ROOT in ("", "/"):
        return path
    return os.path.join(ROOT, path.lstrip("/"))


def log(msg):
    print(f"{PROG}: {msg}", file=sys.stderr, flush=True)


def kmsg(msg):
    """a line on the kernel log (the journal has it, from early boot on)"""
    try:
        with open(p("/dev/kmsg"), "a", encoding="utf-8") as f:
            f.write(f"<5>{PROG}: {msg}\n")
    except OSError:
        pass


def console(msg):
    """a line on the system console. Only for the CI markers (sticktime.ci=1): Raspberry Pi OS's
    console log level 3 (/etc/sysctl.d/98-rpi.conf) hides kernel log lines, the console doesn't"""
    try:
        with open(p("/dev/console"), "a", encoding="utf-8") as f:
            f.write(f"{PROG}: {msg}\n")
    except OSError:
        pass


def now_ms():
    return int(time.time() * 1000)


# ---- processes ---------------------------------------------------------------------------------

class Result:
    def __init__(self, rc, out="", err=""):
        self.returncode, self.stdout, self.stderr = rc, out, err

    @property
    def ok(self):
        return self.returncode == 0


def _resolve(argv):
    # Windows (unit tests only): the fake commands are extensionless Python scripts
    if os.name != "nt" or os.sep in argv[0] or "/" in argv[0]:
        return argv
    for d in os.environ.get("PATH", "").split(os.pathsep):
        f = os.path.join(d, argv[0])
        if os.path.isfile(f):
            with open(f, "rb") as fh:
                if fh.readline().startswith(b"#!") :
                    return [sys.executable, f] + list(argv[1:])
    return argv


def run(argv, input=None, timeout=30, quiet=False):
    """run a command, never raise: missing command -> rc 127, timeout -> rc 124"""
    try:
        r = subprocess.run(_resolve(list(argv)), input=input, capture_output=True, text=True,
                           timeout=timeout, encoding="utf-8", errors="replace")
        res = Result(r.returncode, r.stdout, r.stderr)
    except FileNotFoundError:
        res = Result(127, "", f"{argv[0]}: not found")
    except subprocess.TimeoutExpired:
        res = Result(124, "", f"{argv[0]}: timed out after {timeout} s")
    except OSError as e:
        res = Result(126, "", f"{argv[0]}: {e}")
    if not res.ok and not quiet:
        err = (res.stderr or res.stdout).strip().splitlines()
        log(f"{argv[0]} {argv[1] if len(argv) > 1 else ''}: exit {res.returncode}"
            + (f": {err[-1][:200]}" if err else ""))
    return res


def have(cmd):
    if shutil.which(cmd) is not None:
        return True
    # Windows (unit tests only): which() wants an extension, the fake commands have none
    return os.name == "nt" and any(os.path.isfile(os.path.join(d, cmd))
                                   for d in os.environ.get("PATH", "").split(os.pathsep) if d)


def spawn_background(argv):
    """start a detached helper (survives our exit; output to the journal via inherited stderr)"""
    try:
        subprocess.Popen(_resolve(list(argv)), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         start_new_session=True, close_fds=True)
        return True
    except OSError as e:
        log(f"cannot start {argv[0]}: {e}")
        return False


# ---- files -------------------------------------------------------------------------------------

def read_bytes(path):
    try:
        with open(p(path), "rb") as f:
            return f.read()
    except OSError:
        return None


def read_text(path, default=None):
    b = read_bytes(path)
    return default if b is None else b.decode("utf-8", "replace")


def fsync_dir(d):
    try:
        fd = os.open(d, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def atomic_write(path, data, mode=0o644):
    """temp file in the same directory + fsync + rename + fsync(dir): safe on FAT against power loss"""
    if isinstance(data, str):
        data = data.encode("utf-8")
    full = p(path)
    d = os.path.dirname(full) or "."
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, f".{os.path.basename(full)}.tmp{os.getpid()}")
    try:
        os.unlink(tmp)
    except OSError:
        pass
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), mode)
    try:
        os.write(fd, data)
        os.fsync(fd)
    finally:
        os.close(fd)
    try:
        os.chmod(tmp, mode)            # the umask may have cut bits; FAT ignores it
    except OSError:
        pass
    os.replace(tmp, full)
    fsync_dir(d)


def write_if_changed(path, data, mode=0o644):
    """True when the file was (re)written"""
    if isinstance(data, str):
        data = data.encode("utf-8")
    if read_bytes(path) == data:
        try:
            if os.name != "nt" and (os.stat(p(path)).st_mode & 0o777) != mode and not path.startswith(BOOT):
                os.chmod(p(path), mode)
        except OSError:
            pass
        return False
    atomic_write(path, data, mode)
    return True


def remove(path):
    try:
        os.unlink(p(path))
        return True
    except FileNotFoundError:
        return False
    except OSError as e:
        log(f"cannot remove {path}: {e}")
        return False


def write_json(path, obj, mode=0o644):
    atomic_write(path, json.dumps(obj, indent=1, sort_keys=True) + "\n", mode)


def read_json(path, default=None):
    b = read_bytes(path)
    if b is None:
        return default
    try:
        return json.loads(b.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return default


def cmdline_tokens():
    """the running kernel's command line (/proc/cmdline), as a list"""
    return (read_text("/proc/cmdline", "") or "").split()


# ---- systemd unit links (what 'systemctl enable/disable' writes, without asking PID 1) ----------

UNIT_DIRS = ("/etc/systemd/system", "/usr/lib/systemd/system", "/lib/systemd/system")
ETC_UNITS = "/etc/systemd/system"


def unit_file(name):
    """the unit's file path (inside the root, as the system sees it) or None; masked = None"""
    for d in UNIT_DIRS:
        f = f"{d}/{name}"
        full = p(f)
        if os.path.islink(full) and os.readlink(full) == "/dev/null":
            return None
        if os.path.isfile(full) and not (d == ETC_UNITS and os.path.islink(full)):
            return f
    return None


def unit_install(name):
    """{"WantedBy": [...], "RequiredBy": [...], "Alias": [...]} from the unit's [Install] section"""
    out = {"WantedBy": [], "RequiredBy": [], "Alias": []}
    f = unit_file(name)
    text = read_text(f, "") if f else ""
    sec = None
    for ln in text.splitlines():
        ln = ln.strip()
        if ln.startswith("["):
            sec = ln
            continue
        if sec == "[Install]" and "=" in ln:
            k, _, v = ln.partition("=")
            if k.strip() in out:
                out[k.strip()] += v.split()
    return out


def _link(target, path):
    full = p(path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    try:
        if os.path.lexists(full):
            os.unlink(full)
        os.symlink(target, full)
    except (OSError, NotImplementedError):
        if os.name != "nt":
            raise
        with open(full, "w") as fh:          # Windows test runs without symlink rights
            fh.write(target)


def unit_enabled(name):
    inst = unit_install(name)
    return any(os.path.lexists(p(f"{ETC_UNITS}/{t}.wants/{name}")) for t in inst["WantedBy"]) or \
        any(os.path.lexists(p(f"{ETC_UNITS}/{t}.requires/{name}")) for t in inst["RequiredBy"])


def enable_unit(name):
    """the links of [Install] (WantedBy, RequiredBy, Alias) in /etc/systemd/system; False when the
    unit has no file (or is masked)"""
    f = unit_file(name)
    if f is None:
        log(f"cannot enable {name}: no unit file (or masked)")
        return False
    inst = unit_install(name)
    for t in inst["WantedBy"]:
        _link(f, f"{ETC_UNITS}/{t}.wants/{name}")
    for t in inst["RequiredBy"]:
        _link(f, f"{ETC_UNITS}/{t}.requires/{name}")
    for a in inst["Alias"]:
        _link(f, f"{ETC_UNITS}/{a}")
    return True


def disable_unit(name):
    inst = unit_install(name)
    for t in inst["WantedBy"]:
        remove(f"{ETC_UNITS}/{t}.wants/{name}")
    for t in inst["RequiredBy"]:
        remove(f"{ETC_UNITS}/{t}.requires/{name}")
    for a in inst["Alias"]:
        full = p(f"{ETC_UNITS}/{a}")
        if os.path.islink(full) or (os.name == "nt" and os.path.isfile(full)):
            remove(f"{ETC_UNITS}/{a}")


# ---- sticktime.txt -----------------------------------------------------------------------------
# The format of host/src/settings.h (and settings.py, its Python twin):
#  - UTF-8, optional BOM, LF or CRLF; blank lines and lines starting with '#' are comments
#  - "key = value": the key trimmed and lower-cased, the value trimmed
#  - unquoted: a '#' after a space or tab starts a comment; "double quoted": \" and \\ escapes
#  - a later line with the same key wins; lines without '=' are ignored (a warning)

_KEY_RE = re.compile(r"^[A-Za-z0-9_.\-]+$")

# what the helpers need when the template file is missing (keep in step with settings_keys.h)
FALLBACK_DEFAULTS = {
    "video": "av", "tv": "ntsc", "aspect": "4:3", "wifi_ssid": "", "wifi_password": "",
    "wifi_country": "US", "hotspot": "auto", "hotspot_ssid": "StickTime", "hotspot_password": "sticktime",
    "bluetooth": "on", "audio_out": "auto", "sound": "on", "music": "off", "hostname": "sticktime",
    "ssh": "off", "password": "", "auto_update": "off", "wifi_module_ip": "",
}
SECRETS = ("wifi_password", "hotspot_password", "password", "github_token", "wifi_module_password")

FALLBACK_TEMPLATE = (
    "# StickTime Pi settings (minimal fallback: the full template was missing)\n"
    "# Edit this file on a PC or on the web page: http://sticktime.local\n"
    + "".join(f"{k} = {v}\n" for k, v in FALLBACK_DEFAULTS.items())
)


def _parse_value(raw):
    """(value, comment_start or -1, quoted) from the text after '='"""
    s = raw.strip(" \t")
    if s.startswith('"'):
        out, i = [], 1
        while i < len(s):
            c = s[i]
            if c == "\\" and i + 1 < len(s) and s[i + 1] in '"\\':
                out.append(s[i + 1])
                i += 2
                continue
            if c == '"':
                return "".join(out), True, True
            out.append(c)
            i += 1
        return "".join(out), True, False        # unterminated: take the rest
    # a '#' that follows a space or tab starts a comment (also right after "= ")
    lead = raw[: len(raw) - len(raw.lstrip(" \t"))]
    text = lead + s
    m = re.search(r"[ \t]#", text)
    if m:
        text = text[: m.start()]
    return text.strip(" \t"), False, True


class SettingsFile:
    """sticktime.txt, kept line by line so a rewrite changes only the values it must"""

    def __init__(self, data=b""):
        self.bom = data.startswith(b"\xef\xbb\xbf")
        if self.bom:
            data = data[3:]
        self.crlf = b"\r\n" in data
        self.binary = b"\x00" in data
        text = data.decode("utf-8", "replace")
        self.bad_utf8 = text.count("\ufffd") - data.decode("utf-8", "ignore").count("\ufffd")
        self.lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        if self.lines and self.lines[-1] == "":
            self.lines.pop()
        self.values, self.where, self.warnings = {}, {}, []
        nkeys = 0
        for n, line in enumerate(self.lines):
            st = line.strip(" \t\ufeff")
            if not st or st.startswith("#"):
                continue
            if "=" not in st:
                self.warnings.append(f"line {n + 1}: no '='")
                continue
            k, _, rest = line.partition("=")
            k = k.strip(" \t\ufeff").lower()
            if not k:
                self.warnings.append(f"line {n + 1}: no key before '='")
                continue
            v, quoted, closed = _parse_value(rest)
            if quoted and not closed:
                self.warnings.append(f"line {n + 1}: missing closing quote")
            self.values[k] = v
            self.where.setdefault(k, []).append(n)
            nkeys += 1
        self.nkeys = nkeys

    def looks_corrupt(self, size):
        """binary garbage (fsck damage, a wrong file): NUL bytes, huge, mostly non-text, or no keys at all"""
        if self.binary or size > 1024 * 1024:
            return True
        text = "\n".join(self.lines)
        ctrl = sum(1 for c in text if ord(c) < 32 and c not in "\t\n")
        if size and (self.bad_utf8 + ctrl) > max(8, size // 20):
            return True
        content = [ln for ln in self.lines if ln.strip(" \t\ufeff") and not ln.strip(" \t\ufeff").startswith("#")]
        return bool(content) and self.nkeys == 0

    @staticmethod
    def quote_if_needed(v):
        if v == "":
            return ""
        if (v != v.strip(" \t") or v.startswith(('"', "#")) or re.search(r"[ \t]#", v)
                or "\\" in v and v.startswith('"')):
            return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
        return v

    def set(self, key, value):
        """change one value in place (keeping an inline comment); a new key goes at the end"""
        key = key.lower()
        if not _KEY_RE.match(key) or "\n" in value or "\r" in value:
            raise ValueError(f"bad setting {key!r}")
        q = self.quote_if_needed(value)
        if key in self.where:
            idx = self.where[key]
            n = idx[-1]
            line = self.lines[n]
            k, _, rest = line.partition("=")
            _v, quoted, closed = _parse_value(rest)
            comment = ""
            if not quoted:
                m = re.search(r"[ \t]+#.*$", rest)
                if m:
                    comment = m.group(0)
            elif closed:
                # keep what follows the closing quote ("..." # comment)
                s = rest.lstrip(" \t")
                i, esc = 1, False
                while i < len(s):
                    if esc:
                        esc = False
                    elif s[i] == "\\":
                        esc = True
                    elif s[i] == '"':
                        break
                    i += 1
                tail = s[i + 1:]
                if tail.strip().startswith("#"):
                    comment = tail if tail.startswith((" ", "\t")) else " " + tail
            self.lines[n] = f"{k.rstrip()} = {q}".rstrip() + (comment if q or comment else "")
            if q == "" and comment:
                self.lines[n] = f"{k.rstrip()} =" + comment
            # earlier duplicates are dead (the last one wins): leave them, they are the user's text
        else:
            self.lines.append(f"{key} = {q}".rstrip())
            self.where[key] = [len(self.lines) - 1]
        self.values[key] = value

    def serialize(self):
        nl = "\r\n" if self.crlf else "\n"
        body = nl.join(self.lines) + nl
        return (b"\xef\xbb\xbf" if self.bom else b"") + body.encode("utf-8")


def template_text():
    t = read_text(TEMPLATE)
    return t if t and "=" in t else FALLBACK_TEMPLATE


def defaults():
    d = dict(FALLBACK_DEFAULTS)
    d.update(SettingsFile(template_text().encode("utf-8")).values)
    return d


class Settings:
    """the effective settings: file values over the template's defaults"""

    def __init__(self, values, sfile=None, exists=True, warnings=()):
        self.values, self.file, self.exists, self.warnings = values, sfile, exists, list(warnings)

    def get(self, key):
        return self.values.get(key, "")

    def bool(self, key):
        v = self.get(key).strip().lower()
        if v in ("on", "yes", "true", "1"):
            return True
        if v in ("off", "no", "false", "0"):
            return False
        d = defaults().get(key, "off").lower() in ("on", "yes", "true", "1")
        if v:
            self.warnings.append(f"{key} = {v!r} is not on/off: using {'on' if d else 'off'}")
        return d

    def enum(self, key, choices):
        v = self.get(key).strip().lower()
        if v in choices:
            return v
        d = defaults().get(key, choices[0])
        if v:
            self.warnings.append(f"{key} = {v!r} is not one of {', '.join(choices)}: using {d}")
        return d


def load_settings(repair=True):
    """read sticktime.txt; a missing, empty or corrupt file is replaced by the template (corrupt: kept
    next to it as sticktime.txt.broken-<time>). Never raises."""
    d = defaults()
    data = read_bytes(SETTINGS)
    warnings = []
    if data is None or not data.strip(b" \t\r\n\xef\xbb\xbf"):
        why = "missing" if data is None else "empty"
        warnings.append(f"{SETTINGS} was {why}: wrote the default settings")
        if repair:
            try:
                atomic_write(SETTINGS, template_text())
            except OSError as e:
                warnings.append(f"cannot write {SETTINGS}: {e}")
        return Settings(d, SettingsFile(template_text().encode("utf-8")), exists=False, warnings=warnings)
    sf = SettingsFile(data)
    if sf.looks_corrupt(len(data)):
        stamp = time.strftime("%Y%m%d-%H%M%S")
        bad = f"{SETTINGS}.broken-{stamp}"
        warnings.append(f"{SETTINGS} looked corrupt: kept it as {os.path.basename(bad)}, wrote the defaults")
        if repair:
            try:
                os.replace(p(SETTINGS), p(bad))
            except OSError:
                try:
                    shutil.copyfile(p(SETTINGS), p(bad))
                except OSError as e:
                    warnings.append(f"cannot back up {SETTINGS}: {e}")
            try:
                atomic_write(SETTINGS, template_text())
            except OSError as e:
                warnings.append(f"cannot write {SETTINGS}: {e}")
        return Settings(d, SettingsFile(template_text().encode("utf-8")), exists=False, warnings=warnings)
    if sf.bad_utf8:
        warnings.append(f"{SETTINGS} is not UTF-8 (save it as UTF-8): some characters were replaced")
    warnings += sf.warnings
    vals = dict(d)
    vals.update(sf.values)
    return Settings(vals, sf, exists=True, warnings=warnings)


def _settings_module():
    """settings.py (the host's Python twin, written next to us) or None"""
    here = os.path.dirname(os.path.realpath(__file__))
    if not os.path.isfile(os.path.join(here, "settings.py")):
        return None
    sys.path.insert(0, here)
    try:
        import settings as mod                                      # noqa: E402
        return mod
    except Exception as e:                                           # noqa: BLE001  (a broken twin: our writer)
        log(f"settings.py: {type(e).__name__}: {e}")
        return None
    finally:
        if sys.path and sys.path[0] == here:
            sys.path.pop(0)


def _update_with_module(mod, path, changes):
    """settings.py's API is the C one (settings.h): load/parse, set, serialize/save. Duck-typed,
    because it is written in parallel; False when it offers nothing we know."""
    for name in ("update_file", "save_values", "update"):
        fn = getattr(mod, name, None)
        if callable(fn):
            fn(path, dict(changes))
            return True
    obj = None
    for name in ("load", "load_file", "read"):
        fn = getattr(mod, name, None)
        if callable(fn):
            obj = fn(path)
            break
    if isinstance(obj, tuple):
        obj = obj[0]
    if obj is None or not callable(getattr(obj, "set", None)):
        return False
    for k, v in changes.items():
        obj.set(k, v)
    if callable(getattr(obj, "save", None)):
        obj.save(path)
        return True
    ser = getattr(obj, "serialize", None)
    if callable(ser):
        data = ser()
        atomic_write(SETTINGS, data if isinstance(data, (bytes, str)) else str(data))
        return True
    return False


def update_settings(changes):
    """set some values in sticktime.txt, keeping everything else (lines, comments, CRLF, BOM).
    Uses settings.py (the host's twin) when it is there and does the job (checked by reading the
    file back); else our own writer, which follows the same format rules."""
    mod = _settings_module()
    if mod is not None and os.path.exists(p(SETTINGS)):
        try:
            if _update_with_module(mod, p(SETTINGS), changes):
                back = SettingsFile(read_bytes(SETTINGS) or b"").values
                if all(back.get(k.lower(), "") == v for k, v in changes.items()):
                    return True
                log("settings.py did not save the values: using the built-in writer")
        except Exception as e:                                       # noqa: BLE001  (fall back to our writer)
            log(f"settings.py: {type(e).__name__}: {e}: using the built-in writer")
    data = read_bytes(SETTINGS)
    sf = SettingsFile(data if data is not None else template_text().encode("utf-8"))
    for k, v in changes.items():
        sf.set(k, v)
    atomic_write(SETTINGS, sf.serialize())
    return True


# ---- NetworkManager keyfiles -------------------------------------------------------------------
# NM ignores keyfiles readable by anyone but root (nm-settings-keyfile(5)): 0600, owner root.

PSK_HEX = re.compile(r"^[0-9A-Fa-f]{64}$")


def psk_problem(psk):
    """None when psk is a valid WPA-PSK (8..63 printable ASCII, or 64 hex digits), else why not"""
    if PSK_HEX.match(psk):
        return None
    if any(not (32 <= ord(c) <= 126) for c in psk):
        return "the WiFi password may only use plain ASCII characters (letters, digits, punctuation, spaces)"
    if len(psk) < 8:
        return "the WiFi password must have at least 8 characters"
    if len(psk) > 63:
        return "the WiFi password must have at most 63 characters (or be 64 hex digits)"
    return None


def ssid_problem(ssid):
    b = ssid.encode("utf-8")
    if not b:
        return "no network name"
    if len(b) > 32:
        return f"the network name is {len(b)} bytes long; WiFi allows at most 32"
    if "\x00" in ssid:
        return "the network name contains a NUL character"
    return None


def kf_escape(s):
    """a GKeyFile string value: \\ \\n \\t \\r escaped, leading/trailing spaces as \\s"""
    s = s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t").replace("\r", "\\r")
    lead = len(s) - len(s.lstrip(" "))
    trail = len(s) - len(s.rstrip(" ")) if s.strip(" ") else 0
    if s.strip(" ") == "":
        return "\\s" * len(s)
    return "\\s" * lead + s[lead:len(s) - trail] + "\\s" * trail


def kf_ssid(ssid):
    """plain text when unambiguous for NM's reader, else NM's byte-list form (\"77;121;\").
    NM reads a value made only of digits, ';' and spaces as a byte list, so "1234" must be written
    as bytes too; UTF-8, ';', '\\' and leading/trailing spaces also take the byte form (as NM does)."""
    b = ssid.encode("utf-8")
    if all(0x21 <= c <= 0x7E for c in b[:1] + b[-1:]) and all(0x20 <= c <= 0x7E for c in b) \
            and not any(ch in ssid for ch in ";\\") and not re.fullmatch(r"[0-9 ]*", ssid):
        return ssid
    return "".join(f"{c};" for c in b)


def keyfile_client(ssid, psk, hidden=False, ifname="wlan0"):
    lines = ["[connection]", f"id={WIFI_ID}", f"uuid={uuid.uuid5(UUID_NS, WIFI_ID)}", "type=wifi",
             f"interface-name={ifname}", "autoconnect=true", "autoconnect-priority=10",
             "autoconnect-retries=0", "", "[wifi]", "band=bg", "mode=infrastructure",
             f"ssid={kf_ssid(ssid)}", "powersave=2"]
    if hidden:
        lines.append("hidden=true")
    lines.append("")
    if psk:
        lines += ["[wifi-security]", "auth-alg=open", "key-mgmt=wpa-psk", f"psk={kf_escape(psk)}", ""]
    lines += ["[ipv4]", "method=auto", "", "[ipv6]", "addr-gen-mode=default", "method=auto", "", "[proxy]", ""]
    return "\n".join(lines)


def keyfile_hotspot(ssid, psk, ifname="wlan0", channel=6):
    lines = ["[connection]", f"id={HOTSPOT_ID}", f"uuid={uuid.uuid5(UUID_NS, HOTSPOT_ID)}", "type=wifi",
             f"interface-name={ifname}", "autoconnect=false", "", "[wifi]", "band=bg", f"channel={channel}",
             "mode=ap", f"ssid={kf_ssid(ssid)}", "powersave=2", ""]
    if psk:
        lines += ["[wifi-security]", "group=ccmp;", "key-mgmt=wpa-psk", "pairwise=ccmp;", "proto=rsn;",
                  f"psk={kf_escape(psk)}", ""]
    lines += ["[ipv4]", "address1=10.42.0.1/24", "method=shared", "", "[ipv6]", "method=disabled", "",
              "[proxy]", ""]
    return "\n".join(lines)


def keyfile_module(ssid, psk, ifname="wlan0"):
    lines = ["[connection]", f"id={MODULE_ID}", f"uuid={uuid.uuid5(UUID_NS, MODULE_ID)}", "type=wifi",
             f"interface-name={ifname}", "autoconnect=false", "", "[wifi]", "band=bg", "mode=infrastructure",
             f"ssid={kf_ssid(ssid)}", "powersave=2", ""]
    if psk:
        lines += ["[wifi-security]", "auth-alg=open", "key-mgmt=wpa-psk", f"psk={kf_escape(psk)}", ""]
    lines += ["[ipv4]", "method=auto", "", "[ipv6]", "method=disabled", "", "[proxy]", ""]
    return "\n".join(lines)


def keyfile_path(con_id):
    return f"{NM_DIR}/{con_id}.nmconnection"


def write_keyfile(con_id, text):
    """True when written (content changed); always 0600"""
    return write_if_changed(keyfile_path(con_id), text, 0o600)


# ---- nmcli -------------------------------------------------------------------------------------

def nm_split(line):
    """split one line of 'nmcli -t' output: ':' separates fields, '\\:' and '\\\\' are escapes"""
    out, cur, i = [], [], 0
    while i < len(line):
        c = line[i]
        if c == "\\" and i + 1 < len(line):
            cur.append(line[i + 1])
            i += 2
            continue
        if c == ":":
            out.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    out.append("".join(cur))
    return out


def nm_running():
    r = run(["nmcli", "-t", "-f", "RUNNING", "general"], timeout=10, quiet=True)
    return r.ok and r.stdout.strip() == "running"


def wifi_devices():
    """[(device, state, connection)] of the WiFi devices NetworkManager knows"""
    r = run(["nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device"], timeout=10, quiet=True)
    devs = []
    if r.ok:
        for line in r.stdout.splitlines():
            f = nm_split(line)
            if len(f) >= 4 and f[1] == "wifi":
                devs.append((f[0], f[2], f[3]))
    return devs


def sys_wifi_devices():
    """WiFi interfaces from sysfs (works before NM runs)"""
    try:
        return sorted(n for n in os.listdir(p("/sys/class/net")) if os.path.exists(p(f"/sys/class/net/{n}/wireless"))
                      or os.path.exists(p(f"/sys/class/net/{n}/phy80211")))
    except OSError:
        return []


def hold(seconds):
    """ask the hotspot daemon to leave wlan0 alone for a while (a manual change is in progress)"""
    try:
        atomic_write(HOLD_FILE, f"{int(time.time() + seconds)}\n")
    except OSError as e:
        log(f"cannot write {HOLD_FILE}: {e}")


def held_until():
    try:
        return int((read_text(HOLD_FILE, "0") or "0").strip() or 0)
    except ValueError:
        return 0


def sha256_text(*parts):
    h = hashlib.sha256()
    for x in parts:
        h.update(x.encode("utf-8"))
        h.update(b"\0")
    return h.hexdigest()
