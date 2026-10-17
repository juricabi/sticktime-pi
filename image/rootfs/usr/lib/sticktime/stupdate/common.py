"""Shared pieces: configuration, errors and exit codes, versions, safe writes on FAT, the lock
and the status file the web UI reads."""
import errno
import hashlib
import json
import os
import re
import shutil
import sys
import time

# exit codes (the web server and the units look at these)
EXIT_OK = 0
EXIT_INTERNAL = 1
EXIT_USAGE = 2
EXIT_NETWORK = 3
EXIT_RATE = 4
EXIT_VERIFY = 5
EXIT_DISK = 6
EXIT_BUSY = 7
EXIT_REFUSED = 8
EXIT_AUTH = 9
EXIT_UPDATE = 10          # check: an update is available

CODE_EXIT = {
    "offline": EXIT_NETWORK, "timeout": EXIT_NETWORK, "server_error": EXIT_NETWORK,
    "download_incomplete": EXIT_NETWORK, "unexpected_response": EXIT_NETWORK, "clock_not_set": EXIT_NETWORK,
    "not_found": EXIT_NETWORK, "http_error": EXIT_NETWORK, "pending_asset": EXIT_NETWORK,
    "rate_limited": EXIT_RATE,
    "bad_digest": EXIT_VERIFY, "bad_zip": EXIT_VERIFY, "bad_package": EXIT_VERIFY, "too_big": EXIT_VERIFY,
    "disk_full": EXIT_DISK, "fs_readonly": EXIT_DISK,
    "busy": EXIT_BUSY,
    "downgrade_refused": EXIT_REFUSED, "nothing_to_rollback": EXIT_REFUSED, "needs_new_image": EXIT_REFUSED,
    "custom_game": EXIT_REFUSED,
    "token_missing": EXIT_AUTH, "token_rejected": EXIT_AUTH, "no_access": EXIT_AUTH, "token_forbidden": EXIT_AUTH,
    "usage": EXIT_USAGE,
}

MIB = 1024 * 1024
LUA_MAX = 2 * MIB         # bigger than any StickTime.lua
RESERVE = 8 * MIB         # left free on the FAT for the game's saves and ghosts
TMP = ".updtmp"           # our temp files; boot repair removes only these


class UpdateError(Exception):
    """a failure the user is told about: code (machine), message (plain English)"""

    def __init__(self, code, message, retry_at=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.retry_at = retry_at

    @property
    def exit_code(self):
        return CODE_EXIT.get(self.code, EXIT_INTERNAL)


def fail(code, message, retry_at=None):
    raise UpdateError(code, message, retry_at)


def log(msg):
    # stderr ends up in the journal; never print the token
    print("sticktime-update: " + msg, file=sys.stderr, flush=True)


# ---------------------------------------------------------------- versions

TAG_RE = re.compile(r"v(\d+(?:\.\d+){1,3})")
VER_RE = re.compile(r"\d+(?:\.\d+){1,3}")
HEADER_RE = re.compile(rb"StickTime(?: BW| Lite)? v(\d+(?:\.\d+)+)")


def vkey(v):
    """'1.6' == '1.6.0' (as cmpVer in the editor): int tuple padded with zeros"""
    t = tuple(int(x) for x in v.split("."))
    while len(t) > 1 and t[-1] == 0:
        t = t[:-1]
    return t + (0,) * max(0, 4 - len(t))


def header_version(data):
    """the version in a StickTime loader's header (first 600 bytes), or None"""
    if not data:
        return None
    m = HEADER_RE.search(data[:600])
    return m.group(1).decode("ascii") if m else None


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def git_blob_sha1(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def now_iso():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


# ---------------------------------------------------------------- configuration

class Config:
    def __init__(self, base=None, settings=None, state=None, env=None):
        env = os.environ if env is None else env
        self.base = base or env.get("STICKTIME_BASE") or "/boot/firmware/sticktime"
        self.settings = settings or env.get("STICKTIME_SETTINGS") or "/boot/firmware/sticktime.txt"
        self.state = state or env.get("STICKTIME_STATE") or "/run/sticktime"
        # where programs may run (the preflight): Pi OS mounts /run noexec, the ext4 root isn't
        self.var = env.get("STICKTIME_VAR") or "/var/lib/sticktime"
        # the system scripts (stupdate/system.py): synced from the app set's lib/
        self.libdir = env.get("STICKTIME_LIBDIR") or "/usr/lib/sticktime"
        self.python = env.get("STICKTIME_PYTHON") or sys.executable or "/usr/bin/python3"
        self.systemctl = env.get("STICKTIME_SYSTEMCTL", "systemctl --no-block")     # a JSON list too; "" skips
        self.api = (env.get("STICKTIME_GITHUB_API") or "https://api.github.com").rstrip("/")
        self.raw = (env.get("STICKTIME_GITHUB_RAW") or "https://raw.githubusercontent.com").rstrip("/")
        self.pages = (env.get("STICKTIME_PAGES") or "https://juricabi.github.io/sticktime").rstrip("/")
        self.game_repo = env.get("STICKTIME_GAME_REPO") or "juricabi/sticktime"
        self.app_repo = env.get("STICKTIME_APP_REPO") or "juricabi/sticktime-pi"
        self.patch_editor = env.get("STICKTIME_PATCH_EDITOR") or ""
        # "" skips: tests, or a host that isn't a systemd service
        self.restart_cmd = env.get("STICKTIME_RESTART_CMD", "systemctl --no-block try-restart sticktime.service")
        self.reload_url = env.get("STICKTIME_RELOAD_URL", "http://127.0.0.1/api/game/reload")
        # automatic runs restart the host only while it shows the menu (or isn't running)
        self.status_url = env.get("STICKTIME_STATUS_URL", "http://127.0.0.1/api/status")
        self.image_version_file = env.get("STICKTIME_IMAGE_VERSION_FILE") or "/etc/sticktime-image-version"
        self.timeout = float(env.get("STICKTIME_HTTP_TIMEOUT") or 15)
        self.time_wait = float(env.get("STICKTIME_TIME_WAIT") or 60)
        self.preflight = env.get("STICKTIME_UPDATE_PREFLIGHT") or "auto"
        self.cache_s = float(env.get("STICKTIME_CHECK_CACHE_S") or 600)
        self.settle_s = float(env.get("STICKTIME_SETTLE_S") or 3600)
        self.ua = "sticktime-pi/%s (+https://github.com/juricabi/sticktime-pi)" % (self.app_version() or "0")

    # FAT
    def p(self, *parts):
        return os.path.join(self.base, *parts)

    @property
    def sd_tools(self):
        return self.p("sd", "SCRIPTS", "TOOLS")

    @property
    def game_dir(self):
        return self.p("game")

    @property
    def editor(self):
        return self.p("editor")

    @property
    def upd(self):
        return self.p("update")

    # tmpfs
    def s(self, *parts):
        return os.path.join(self.state, *parts)

    def bases(self):
        return (self.api, self.raw, self.pages)

    def app_version(self):
        try:
            with open(os.path.join(self.base, "VERSION"), "rb") as f:
                v = f.read(64).decode("ascii", "replace").strip()
        except OSError:
            return ""
        return v if VER_RE.fullmatch(v) else ""


# ---------------------------------------------------------------- test hooks
# A simulated power cut (os._exit at a named step), ENOSPC after n FAT writes, a read-only FAT
# and a fake free space. Read once; unset in production.

class _Faults:
    def __init__(self):
        e = os.environ
        self.crash_at = e.get("STICKTIME_UPDATE_CRASH_AT") or ""
        n = e.get("STICKTIME_UPDATE_ENOSPC_AFTER")
        self.enospc_after = int(n) if n else None
        # once: only that one write fails (space freed meanwhile); else every write after it
        self.enospc_once = e.get("STICKTIME_UPDATE_ENOSPC_ONCE") == "1"
        self.erofs = e.get("STICKTIME_UPDATE_EROFS") == "1"
        f = e.get("STICKTIME_UPDATE_FREE_MB")
        self.free = int(float(f) * MIB) if f else None
        self.writes = 0


FAULTS = _Faults()


def point(name):
    """a step boundary: the tests cut the power here"""
    if FAULTS.crash_at == name:
        sys.stderr.flush()
        os._exit(99)


def _fat_op(grow=True):
    if FAULTS.erofs:
        raise OSError(errno.EROFS, "Read-only file system")
    if not grow:
        return          # renames and deletes never need space
    FAULTS.writes += 1
    n = FAULTS.enospc_after
    if n is not None and (FAULTS.writes == n + 1 if FAULTS.enospc_once else FAULTS.writes > n):
        raise OSError(errno.ENOSPC, "No space left on device")


def free_bytes(path):
    if FAULTS.free is not None:
        return FAULTS.free
    while path and not os.path.exists(path):
        path = os.path.dirname(path)
    try:
        return shutil.disk_usage(path or ".").free
    except OSError:
        return 0


def need_space(path, need, what="the SD card"):
    free = free_bytes(path)
    if free < need + RESERVE:
        fail("disk_full", "Not enough free space on %s: %.1f MB free, %.1f MB needed (8 MB stays free for "
             "the game's saves and ghosts)." % (what, free / MIB, (need + RESERVE) / MIB))


def fs_error(e):
    """OSError from a FAT write -> UpdateError (disk full, read-only), else re-raise"""
    if isinstance(e, OSError) and e.errno == errno.ENOSPC:
        return UpdateError("disk_full", "The SD card is full: %.1f MB free. Nothing was changed; free some space "
                           "(old songs, backups) and try again." % (free_bytes(".") / MIB))
    if isinstance(e, OSError) and e.errno == errno.EROFS:
        return UpdateError("fs_readonly", "The SD card's boot partition is read-only (it had an error). "
                           "Reboot the Pi so it gets checked, then try again.")
    return None


# ---------------------------------------------------------------- safe writes on FAT
# FAT has no atomic directory swap, and a rename over a file is only safe once both the file and
# its directory are fsynced: tmp -> fsync -> rename -> fsync(file) -> fsync(dir).

def fsync_dir(d):
    if os.name == "nt":
        return
    try:
        fd = os.open(d or ".", os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def fsync_path(p):
    if os.name == "nt":
        return
    try:
        fd = os.open(p, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def make_dirs(d):
    if os.path.isdir(d):
        return
    parent = os.path.dirname(d)
    if parent and parent != d:
        make_dirs(parent)
    _fat_op()
    try:
        os.mkdir(d)
    except FileExistsError:
        if not os.path.isdir(d):
            raise
        return
    fsync_dir(parent)


def write_file(path, data):
    """atomic replace on FAT; never chmod (FAT refuses modes outside its masks)"""
    _fat_op()
    d = os.path.dirname(path)
    tmp = path + TMP
    try:
        with open(tmp, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    fsync_path(path)
    fsync_dir(d)


def create_marker(path, text=""):
    """create-only (never rewritten); an existing one is fine"""
    _fat_op()
    try:
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    except FileExistsError:
        return False
    try:
        os.write(fd, text.encode("utf-8"))
        os.fsync(fd)
    finally:
        os.close(fd)
    fsync_dir(os.path.dirname(path))
    return True


def rename(src, dst):
    _fat_op(False)
    os.rename(src, dst)
    fsync_dir(os.path.dirname(dst))
    if os.path.dirname(src) != os.path.dirname(dst):
        fsync_dir(os.path.dirname(src))


def remove(path):
    """a file or a whole folder; missing is fine"""
    if not os.path.lexists(path):
        return
    _fat_op(False)
    if os.path.isdir(path) and not os.path.islink(path):
        # onerror is deprecated from 3.12 (the Pi has 3.13), onexc doesn't exist in 3.11
        if sys.version_info >= (3, 12):
            shutil.rmtree(path, onexc=_rm_err)
        else:
            shutil.rmtree(path, onerror=_rm_err)
    else:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    fsync_dir(os.path.dirname(path))


def _rm_err(fn, path, exc):
    e = exc[1] if isinstance(exc, tuple) else exc
    if isinstance(e, FileNotFoundError):
        return
    raise e


def read_file(path, cap=64 * MIB):
    try:
        with open(path, "rb") as f:
            data = f.read(cap + 1)
    except (FileNotFoundError, NotADirectoryError, IsADirectoryError, PermissionError):
        return None
    return data if len(data) <= cap else None


def read_json(path, cap=4 * MIB):
    data = read_file(path, cap)
    if data is None:
        return None
    try:
        return json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return None


def json_bytes(obj):
    return (json.dumps(obj, indent=1, sort_keys=False) + "\n").encode("utf-8")


def write_state(path, data):
    """tmpfs (/run/sticktime): atomic for readers, no fsync needed"""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = "%s.%d.tmp" % (path, os.getpid())
    with open(tmp, "wb") as f:
        f.write(data)
    for i in range(50):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            # Windows: a reader has it open for a moment
            if os.name != "nt" or i == 49:
                raise
            time.sleep(0.02)


# ---------------------------------------------------------------- the lock

try:
    import fcntl
except ImportError:          # Windows (the tests)
    fcntl = None
    import msvcrt


class Lock:
    """one updater at a time: flock on /run/sticktime/update.lock (released when the process dies)"""

    def __init__(self, path):
        self.path = path
        self.fd = None

    def acquire(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o644)
        try:
            if fcntl:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            else:
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        except OSError:
            os.close(fd)
            return False
        if fcntl:
            try:
                os.ftruncate(fd, 0)
                os.write(fd, b"%d\n" % os.getpid())
            except OSError:
                pass
        self.fd = fd
        return True

    def release(self):
        if self.fd is None:
            return
        try:
            if fcntl:
                fcntl.flock(self.fd, fcntl.LOCK_UN)
            else:
                os.lseek(self.fd, 0, os.SEEK_SET)
                msvcrt.locking(self.fd, msvcrt.LK_UNLCK, 1)
        except OSError:
            pass
        os.close(self.fd)
        self.fd = None


# ---------------------------------------------------------------- status (update.json)

def _blank():
    return {"state": "idle", "message": "", "progress": 0,
            "game": {"installed": "", "latest": "", "staged": "", "notes": ""},
            "app": {"installed": "", "latest": "", "notes": ""},
            "checked_at": 0, "error": ""}


STATES = ("idle", "checking", "downloading", "installing", "done", "error")


class Status:
    """/run/sticktime/update.json, exactly the fields of docs/WEB_API.md 'Updates', replaced atomically"""

    def __init__(self, cfg, write=True):
        self.path = cfg.s("update.json")
        self.write = write
        self.d = _blank()
        old = read_json(self.path)
        if isinstance(old, dict):
            for k in ("message", "error"):
                if isinstance(old.get(k), str):
                    self.d[k] = old[k]
            if old.get("state") in STATES:
                self.d["state"] = old["state"]
            if isinstance(old.get("progress"), (int, float)):
                self.d["progress"] = old["progress"]
            if isinstance(old.get("checked_at"), int):
                self.d["checked_at"] = old["checked_at"]
            for part in ("game", "app"):
                if isinstance(old.get(part), dict):
                    for k in self.d[part]:
                        if isinstance(old[part].get(k), str):
                            self.d[part][k] = old[part][k]
        self._last = 0.0

    def save(self):
        if self.write:
            write_state(self.path, (json.dumps(self.d) + "\n").encode("utf-8"))
        self._last = time.monotonic()

    def set(self, state=None, message=None, progress=None, error=None, save=True):
        if state is not None:
            self.d["state"] = state
        if message is not None:
            self.d["message"] = message
        if progress is not None:
            self.d["progress"] = round(max(0.0, min(1.0, float(progress))), 3)
        if error is not None:
            self.d["error"] = error
        if save:
            self.save()

    def part(self, what, **kw):
        for k, v in kw.items():
            if k in self.d[what] and v is not None:
                self.d[what][k] = v

    def checked(self):
        self.d["checked_at"] = int(time.time() * 1000)

    def progress(self, frac, message=None):
        """throttled: a download calls this for every chunk"""
        self.set(message=message, progress=frac, save=False)
        if time.monotonic() - self._last >= 0.25:
            self.save()

    def failed(self, e):
        self.set(state="error", message=e.message, error=e.message, progress=0)


# ---------------------------------------------------------------- settings

def read_settings(path):
    """{key: value} from sticktime.txt via settings.py (the parser shared with the host); a minimal
    reader when it can't be imported"""
    try:
        import settings as sm
    except Exception:
        sm = None
    if sm is not None:
        try:
            d = _settings_from_module(sm, path)
            if d is not None:
                return d
        except FileNotFoundError:
            return {}
        except Exception as e:
            log("settings.py: %s; reading %s directly" % (e, path))
    data = read_file(path, 1 * MIB)
    return parse_settings(data.decode("utf-8", "replace")) if data is not None else {}


def _settings_from_module(sm, path):
    for name in ("load", "read", "load_file", "read_file"):
        fn = getattr(sm, name, None)
        if callable(fn):
            obj = fn(path)
            if isinstance(obj, tuple):
                obj = obj[0]
            return _as_dict(obj)
    fn = getattr(sm, "parse", None)
    if callable(fn):
        data = read_file(path, 1 * MIB)
        return _as_dict(fn(data.decode("utf-8", "replace"))) if data is not None else {}
    return None


def _as_dict(obj):
    if isinstance(obj, dict):
        return {str(k).lower(): ("" if v is None else str(v)) for k, v in obj.items()}
    # settings.py's Settings: as_dict() gives the effective values (defaults, normalized on/off)
    for attr in ("as_dict", "values", "items"):
        v = getattr(obj, attr, None)
        if callable(v):
            try:
                v = v()
            except TypeError:
                continue
        if isinstance(v, dict):
            return _as_dict(v)
        if v is not None and not isinstance(v, (str, bytes)):
            try:
                return _as_dict(dict(v))
            except (TypeError, ValueError):
                pass
    get = getattr(obj, "get", None)
    if callable(get):
        return {k: (get(k) or "") for k in ("auto_update", "github_token")}
    return None


def parse_settings(text):
    """the format of host/src/settings.h, enough for the keys the updater reads"""
    out = {}
    if text.startswith("﻿"):
        text = text[1:]
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#") or "=" not in s:
            continue
        k, v = s.split("=", 1)
        k, v = k.strip().lower(), v.strip()
        if v.startswith('"'):
            buf, i = [], 1
            while i < len(v) and v[i] != '"':
                if v[i] == "\\" and i + 1 < len(v) and v[i + 1] in '"\\':
                    i += 1
                buf.append(v[i])
                i += 1
            v = "".join(buf)
        else:
            m = re.search(r"[ \t]#", v)
            if m:
                v = v[:m.start()].rstrip()
        out[k] = v
    return out


def truthy(v):
    return str(v).strip().lower() in ("on", "yes", "true", "1")

