"""System scripts: the helpers in /usr/lib/sticktime (this updater too), from the app set's lib/.

The SD image installs them on the root filesystem, which app updates (the FAT partition only) don't
reach. App packages carry the same files in lib/, which the updater swaps and rolls back with bin/
and web/, and sync() makes the folder match the installed set:

- the host runs the set's own copy at every start (`sticktime-update sync --to <libexec>`), so a set
  installed by an older updater (0.2.1) still brings its scripts; the updater syncs in-process after
  it installed, rolled back or repaired the app, before the restart (which may end its process);
- only files that match the set's SHA256SUMS and compile are written, each one atomically;
- before the first change the image's own files are kept in /var/lib/sticktime/system/image/: a set
  without lib/ (a rollback to 0.2.1 or older) brings them back, and the files an update added go;
- the updater must still start afterwards, and the boot setup's libraries import, else the previous
  files go back;
- sticktime-bt.service restarts when its files changed; the boot setup and the hotspot daemon (which
  may carry the web page's connection) pick up theirs at the next boot."""
import json
import os
import shlex
import shutil
import subprocess

from .app import parse_sums
from .common import MIB, fail, json_bytes, log, now_iso, read_file, read_json, sha256

SELF_TEST = "sticktime-update"
TMP = ".sync-tmp"
MAX_FILE = 8 * MIB
MAX_FILES = 2000
# a daemon and the files it loads: restarted when one of them changed
RESTART = {"sticktime-bt.service": ("sticktime-bt", "stlib.py", "settings.py")}
# read by the boot setup, or by the hotspot daemon (not restarted): they apply at the next boot
AT_BOOT = ("sticktime-setup", "stboot.py", "stlib.py", "settings.py", "sticktime-hotspot")
# the boot setup's libraries: they must import after a change
LIBS = ("stlib", "settings", "stboot")


class Result:
    def __init__(self, source=""):
        self.source = source
        self.changed = []
        self.removed = []
        self.restarted = []
        self.reboot = False

    def __bool__(self):
        return bool(self.changed or self.removed)

    def summary(self):
        if not self:
            return "The system scripts are up to date."
        parts = []
        if self.changed:
            parts.append("updated %s" % ", ".join(self.changed))
        if self.removed:
            parts.append("removed %s" % ", ".join(self.removed))
        msg = "System scripts from %s: %s." % (self.source, "; ".join(parts))
        if self.restarted:
            msg += " Restarted %s." % ", ".join(self.restarted)
        if self.reboot:
            msg += " Some changes apply after a reboot."
        return msg


def tree(root):
    """{relative path: bytes} of the regular files under root (no caches, temp files or links);
    None when root isn't a folder"""
    if not os.path.isdir(root):
        return None
    out = {}
    for d, dirs, names in os.walk(root):
        dirs[:] = sorted(x for x in dirs if x != "__pycache__" and not x.startswith(".") and not x.endswith(TMP))
        for n in sorted(names):
            if n.startswith(".") or n.endswith((TMP, ".updtmp")):
                continue
            p = os.path.join(d, n)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            if len(out) >= MAX_FILES:
                fail("bad_package", "%s holds more than %d files." % (root, MAX_FILES))
            data = read_file(p, MAX_FILE)
            if data is None:
                fail("bad_package", "%s is unreadable or bigger than 8 MB." % p)
            out[os.path.relpath(p, root).replace(os.sep, "/")] = data
    return out


def set_files(cfg):
    """the installed set's lib/, checked against its SHA256SUMS; None when the set has no lib/"""
    files = tree(os.path.join(cfg.base, "lib"))
    if files is None:
        return None
    data = read_file(os.path.join(cfg.base, "SHA256SUMS"), 4 * MIB)
    if data is None:
        fail("bad_package", "The installed StickTime Pi has a lib/ folder but no SHA256SUMS.")
    try:
        sums = parse_sums(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        fail("bad_package", "The installed StickTime Pi's SHA256SUMS is broken (%s)." % e)
    want = {p[4:]: h for p, h in sums.items() if p.startswith("lib/")}
    for rel, b in files.items():
        if want.get(rel) != sha256(b):
            fail("bad_package", "lib/%s doesn't match the installed StickTime Pi's SHA256SUMS." % rel)
    missing = sorted(rel for rel in want if rel not in files)
    if missing:
        fail("bad_package", "lib/%s is missing from the installed StickTime Pi." % missing[0])
    if SELF_TEST not in files:
        fail("bad_package", "The installed StickTime Pi's lib/ has no %s." % SELF_TEST)
    return files


def mode_of(data):
    return 0o755 if data.startswith(b"#!") else 0o644


def _fsync_dir(d):
    if os.name == "nt":
        return
    fd = os.open(d, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _write(path, data):
    """temp file + fsync + rename (ext4: a crash leaves the old file or the new one)"""
    d = os.path.dirname(path)
    os.makedirs(d, exist_ok=True)
    tmp = path + TMP
    try:
        with open(tmp, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, mode_of(data))
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    _fsync_dir(d)


def _apply(dst, changes):
    """changes: {rel: bytes, or None to remove}"""
    for rel, b in sorted(changes.items()):
        p = os.path.join(dst, *rel.split("/"))
        if b is None:
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
        else:
            _write(p, b)
    # stale bytecode would only cost a recompile (the source's mtime and size changed), but a
    # removed module's .pyc must not stay importable
    for d, dirs, _names in os.walk(dst):
        if "__pycache__" in dirs:
            shutil.rmtree(os.path.join(d, "__pycache__"), ignore_errors=True)
            dirs.remove("__pycache__")


def _python(rel, b):
    return rel.endswith(".py") or (b.startswith(b"#!") and b"python" in b.split(b"\n", 1)[0])


def compile_problem(changes):
    for rel, b in sorted(changes.items()):
        if b is None or not _python(rel, b):
            continue
        try:
            compile(b, rel, "exec", dont_inherit=True)
        except (SyntaxError, ValueError) as e:
            return "%s: %s" % (rel, e)
    return ""


# ---------------------------------------------------------------- the image's own files

def _sysdir(cfg):
    return os.path.join(cfg.var, "system")


def load_image(cfg):
    """the image's scripts as they were before the first sync changed them (None: never changed)"""
    if not isinstance(read_json(os.path.join(_sysdir(cfg), "image.json")), dict):
        return None
    return tree(os.path.join(_sysdir(cfg), "image"))


def save_image(cfg, files):
    """complete once image.json exists; an unfinished copy is made again"""
    d = os.path.join(_sysdir(cfg), "image")
    tmp = d + TMP
    shutil.rmtree(tmp, ignore_errors=True)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(tmp)
    for rel, b in files.items():
        _write(os.path.join(tmp, *rel.split("/")), b)
    os.rename(tmp, d)
    _fsync_dir(_sysdir(cfg))
    _write(os.path.join(_sysdir(cfg), "image.json"),
           json_bytes({"files": len(files), "app": cfg.app_version(), "at": now_iso()}))


# ---------------------------------------------------------------- after a change

def self_test(cfg, dst):
    """'' when the updater starts and the boot setup's libraries import from dst"""
    exe = os.path.join(dst, SELF_TEST)
    if not os.path.isfile(exe):
        return "%s is missing" % SELF_TEST
    libs = [m for m in LIBS if os.path.isfile(os.path.join(dst, m + ".py"))]
    runs = [("%s --self-test" % SELF_TEST, [cfg.python, "-I", "-B", exe, "--self-test"], "ok")]
    if libs:
        runs.append(("import " + ", ".join(libs), [cfg.python, "-I", "-B", "-c", "import sys; sys.path.insert(0, "
                     "sys.argv[1]); import %s" % ", ".join(libs), dst], ""))
    for what, argv, want in runs:
        try:
            p = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               timeout=60)
        except (OSError, subprocess.SubprocessError) as e:
            return "%s: %s" % (what, e)
        out = p.stdout.decode("utf-8", "replace").strip()
        if p.returncode != 0 or not out.startswith(want):
            return "%s: %s" % (what, out.splitlines()[-1][:200] if out else "exit %d" % p.returncode)
    return ""


def _restart(cfg, r):
    hit = set(r.changed) | set(r.removed)
    r.reboot = bool(hit & set(AT_BOOT))
    cmd = (cfg.systemctl or "").strip()
    if not cmd:
        return
    for svc, deps in RESTART.items():
        if not hit & set(deps):
            continue
        try:
            argv = json.loads(cmd) if cmd.startswith("[") else shlex.split(cmd)
            p = subprocess.run(argv + ["try-restart", svc], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        except (OSError, subprocess.SubprocessError, ValueError, TypeError) as e:
            log("system scripts: can't restart %s: %s" % (svc, e))
            continue
        if p.returncode == 0:
            r.restarted.append(svc)
        else:
            log("system scripts: restarting %s: %s" % (svc, p.stdout.decode("utf-8", "replace").strip()[-200:]))


def sync(cfg, dst=None):
    """make dst (/usr/lib/sticktime) match the installed set's lib/, or the image's own files when the
    set has none; raises UpdateError (nothing changed, or the previous files are back)"""
    dst = dst or cfg.libdir
    files = set_files(cfg)
    image = load_image(cfg)
    if files is None:
        if image is None:
            return Result("the SD image")       # never changed: the image's files are there
        want, r = image, Result("the SD image")
    else:
        want, r = files, Result("StickTime Pi %s" % (cfg.app_version() or "?"))
    have = tree(dst) or {}
    keep = image if image is not None else have      # what the image had is never removed
    changes = {rel: b for rel, b in want.items() if have.get(rel) != b}
    for rel in have:
        if rel not in want and rel not in keep:
            changes[rel] = None
    if not changes:
        return r
    why = compile_problem(changes)
    if why:
        fail("bad_package", "The system scripts of %s don't compile (%s); nothing was changed." % (r.source, why))
    if image is None:
        save_image(cfg, have)
    old = {rel: have.get(rel) for rel in changes}
    try:
        _apply(dst, changes)
        problem = self_test(cfg, dst)
    except OSError as e:
        problem = str(e)
    if problem:
        try:
            _apply(dst, old)
        except OSError as e:
            log("system scripts: couldn't put the previous ones back: %s" % e)
        fail("bad_package", "The system scripts of %s failed their test (%s); the previous ones are back."
             % (r.source, problem))
    r.changed = sorted(k for k, v in changes.items() if v is not None)
    r.removed = sorted(k for k, v in changes.items() if v is None)
    _restart(cfg, r)
    try:
        _write(os.path.join(_sysdir(cfg), "state.json"),
               json_bytes({"source": r.source, "app": cfg.app_version(), "at": now_iso(), "changed": r.changed,
                           "removed": r.removed, "restarted": r.restarted, "reboot": r.reboot}))
    except OSError as e:
        log("system scripts: couldn't write state.json: %s" % e)
    log(r.summary())
    return r


def sync_after_app(cfg, st=None):
    """the updater, after the app set changed: its scripts, never in the way of the restart"""
    try:
        r = sync(cfg)
    except Exception as e:                       # noqa: BLE001  (an UpdateError, a full disk, a bug)
        msg = getattr(e, "message", None) or "%s" % e
        log("system scripts: %s" % msg)
        if st is not None:
            st.set(message=(st.d["message"] + " The system scripts weren't updated: " + msg).strip())
        return None
    if r.reboot and st is not None:
        st.set(message=(st.d["message"] + " Some system changes apply after a reboot.").strip())
    return r
