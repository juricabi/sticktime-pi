"""App updates (juricabi/sticktime-pi releases, sticktime-pi-app-<ver>.tar.gz).

bin/, web/ (and lib/, share/, fonts/) plus VERSION, SHA256SUMS and compat.json in the base folder are
swapped as a set: the new set is written to update/staging/ first, then a journal lists every
remove and rename (each one idempotent: it is done when its source is gone and its target is
there), and repair() at boot finishes a journal the power cut interrupted. FAT can't rename a
folder over a folder, so the old set always moves to <name>.prev first, which is also what a
rollback swaps back."""
import io
import json
import os
import platform
import re
import shlex
import subprocess
import sys
import tarfile
import time
import urllib.request
import zlib

from .common import (MIB, UpdateError, VER_RE, fail, free_bytes, fs_error, json_bytes, log, make_dirs, need_space,
                     now_iso, point, read_file, read_json, remove, rename, sha256, vkey, write_file, write_state)
from .net import Release, iso_time, pick_release

ASSET = "sticktime-pi-app-{ver}.tar.gz"
MAX_TGZ = 64 * MIB
MAX_FILE = 32 * MIB
MAX_ENTRIES = 4000
TOP_DIRS = ("bin", "web", "lib", "share", "fonts")
TOP_FILES = ("VERSION", "SHA256SUMS", "compat.json")
REC = "APP_RELEASE.json"          # which release the set came from (written by the updater, swapped with it)
# swap order: the web files before the binary that serves them, VERSION last
MANAGED = ("web", "lib", "share", "fonts", "compat.json", "SHA256SUMS", REC, "bin", "VERSION")
KEEP = ("fonts",)                 # a set without it keeps the card's (the host can't draw without fonts)
ESSENTIAL = ("bin", "web", "VERSION")
SEGMENT = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,99}")
SUM_LINE = re.compile(r"([0-9a-fA-F]{64}) [ *](.+)")
BIN = "bin/sticktime-pi"


def bad(why, asset_id=None):
    raise UpdateError("bad_package", "The StickTime Pi release file can't be installed (%s%s)."
                      % (why, ", asset %s" % asset_id if asset_id else ""))


class AppPackage:
    def __init__(self, version, files, compat):
        self.version = version
        self.files = files            # path -> bytes
        self.compat = compat


def parse_sums(text):
    """sha256sum format -> {path: hex}; raises ValueError"""
    out = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        m = SUM_LINE.fullmatch(line.rstrip("\r"))
        if not m:
            raise ValueError("bad line %r" % line[:80])
        p = m.group(2)
        if p.startswith("./"):
            p = p[2:]
        if p in out:
            raise ValueError("%s twice" % p)
        out[p] = m.group(1).lower()
    return out


def elf_problem(b):
    if len(b) < 64 or b[:4] != b"\x7fELF":
        return "bin/sticktime-pi isn't a program"
    if b[4] != 2 or b[5] != 1:
        return "bin/sticktime-pi isn't 64-bit little-endian"
    if int.from_bytes(b[16:18], "little") not in (2, 3):
        return "bin/sticktime-pi isn't an executable"
    if int.from_bytes(b[18:20], "little") != 183:
        return "bin/sticktime-pi isn't built for the Pi (aarch64)"
    return ""


def _name_ok(name):
    if "\x00" in name or "\\" in name or name.startswith("/") or ":" in name:
        return False
    return all(s not in (".", "..") and SEGMENT.fullmatch(s) for s in name.split("/"))


def validate_app_tar(data, tag_version, asset_id=None):
    try:
        tf = tarfile.open(fileobj=io.BytesIO(data), mode="r:gz")
    except (tarfile.TarError, OSError, EOFError, zlib.error) as e:
        bad("not a .tar.gz: %s" % e, asset_id)
    files, seen, total, n = {}, set(), 0, 0
    try:
        for m in tf:
            n += 1
            if n > MAX_ENTRIES:
                bad("more than %d entries" % MAX_ENTRIES, asset_id)
            name = m.name
            while name.startswith("./"):
                name = name[2:]
            if name in ("", "."):
                continue
            if m.isdir():
                name = name.rstrip("/")
                if not _name_ok(name) or name.split("/")[0] not in TOP_DIRS:
                    bad("unexpected folder %r" % name[:80], asset_id)
                continue
            if not m.isreg():
                bad("%r is a link or a device" % name[:80], asset_id)
            if not _name_ok(name):
                bad("unsafe name %r" % name[:80], asset_id)
            top = name.split("/")[0]
            if ("/" in name and top not in TOP_DIRS) or ("/" not in name and name not in TOP_FILES):
                bad("unexpected file %r" % name[:80], asset_id)
            if name.lower() in seen:
                bad("%s twice" % name, asset_id)
            seen.add(name.lower())
            if m.size > MAX_FILE:
                bad("%s is %.1f MB" % (name, m.size / MIB), asset_id)
            total += m.size
            if total > MAX_TGZ:
                bad("more than 64 MB", asset_id)
            f = tf.extractfile(m)
            b = f.read(m.size + 1) if f else b""
            if len(b) != m.size:
                bad("%s is cut short" % name, asset_id)
            files[name] = b
    except (tarfile.TarError, OSError, EOFError, zlib.error) as e:
        bad("damaged: %s" % e, asset_id)
    for need in ("VERSION", "SHA256SUMS", BIN):
        if need not in files:
            bad("no %s" % need, asset_id)
    v = files["VERSION"].decode("ascii", "replace").strip()
    if not VER_RE.fullmatch(v) or vkey(v) != vkey(tag_version):
        bad("VERSION says %r, the release %s" % (v[:20], tag_version), asset_id)
    try:
        sums = parse_sums(files["SHA256SUMS"].decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        bad("SHA256SUMS: %s" % e, asset_id)
    for p, b in files.items():
        if p == "SHA256SUMS":
            continue
        if p not in sums:
            bad("%s isn't in SHA256SUMS" % p, asset_id)
        if sums[p] != sha256(b):
            bad("%s doesn't match SHA256SUMS" % p, asset_id)
    extra = [p for p in sums if p not in files and p != "SHA256SUMS"]
    if extra:
        bad("SHA256SUMS lists %s, which isn't in the file" % extra[0], asset_id)
    why = elf_problem(files[BIN])
    if why:
        bad(why, asset_id)
    compat = {}
    if "compat.json" in files:
        try:
            compat = json.loads(files["compat.json"].decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            bad("compat.json isn't JSON", asset_id)
        if not isinstance(compat, dict):
            bad("compat.json isn't an object", asset_id)
    return AppPackage(v, files, compat)


class App:
    def __init__(self, cfg, http, status, settings):
        self.cfg = cfg
        self.http = http
        self.st = status
        self.settings = settings
        self.repo = cfg.app_repo
        self.need_restart = False

    def token(self):
        return (self.settings.get("github_token") or "").strip()

    @property
    def journal_path(self):
        return os.path.join(self.cfg.upd, "journal.json")

    def b(self, rel):
        return os.path.join(self.cfg.base, *rel.split("/"))

    def installed(self):
        return self.cfg.app_version()

    # ---------------------------------------------------------------- GitHub
    def latest(self, use_cache=True, want=None):
        token = self.token()
        cache = self.cfg.s("check-app.json")
        # which token the answer was for (a hash: /run is readable by more than the updater)
        tkey = sha256(token.encode("utf-8"))[:16] if token else ""
        if use_cache and want is None:
            c = read_json(cache)
            if (isinstance(c, dict) and c.get("repo") == self.repo and c.get("token") == tkey
                    and isinstance(c.get("at"), (int, float)) and 0 <= time.time() - c["at"] < self.cfg.cache_s):
                if isinstance(c.get("error"), dict):
                    # "no access" costs an API call too: remembered as long as an answer
                    fail(str(c["error"].get("code")), str(c["error"].get("message")))
                best = Release.from_dict(c["best"]) if isinstance(c.get("best"), dict) else None
                return best, c.get("pending"), c.get("date") or time.time(), c.get("warning") or ""
        try:
            try:
                rels, resp = self.http.get_json("%s/repos/%s/releases?per_page=30" % (self.cfg.api, self.repo),
                                                token=token or None)
            except UpdateError as e:
                if e.code == "not_found":
                    if not token:
                        fail("token_missing", "%s can't be seen without a token (a private repository): put a GitHub "
                             "token with read access to it in github_token to get its updates." % self.repo)
                    fail("no_access", "The GitHub token can't see StickTime Pi's releases (no access to %s, or no "
                         "release yet)." % self.repo)
                raise
        except UpdateError as e:
            if e.code in ("token_missing", "no_access", "token_rejected", "token_forbidden") and want is None:
                write_state(cache, json_bytes({"at": time.time(), "repo": self.repo, "token": tkey,
                                               "error": {"code": e.code, "message": e.message}}))
            raise
        if not isinstance(rels, list):
            fail("unexpected_response", "GitHub's list of StickTime Pi releases looks wrong.")
        best, pending = pick_release(rels, ASSET, MAX_TGZ, want)
        date = resp.date() or time.time()
        warning = self._expiry_warning(date)
        if want is None:
            write_state(cache, json_bytes({"at": time.time(), "repo": self.repo, "token": tkey, "date": date,
                                           "pending": pending, "warning": warning,
                                           "best": best.to_dict() if best else None}))
        return best, pending, date, warning

    def _expiry_warning(self, now):
        exp = self.http.token_expiry
        m = re.match(r"(\d{4})-(\d\d)-(\d\d)", exp or "")
        if not m:
            return ""
        t = iso_time("%s-%s-%sT00:00:00Z" % m.groups())
        if t is None:
            return ""
        days = (t - now) / 86400
        if days < 14:
            return " The GitHub token expires on %s-%s-%s: make a new one." % m.groups()
        return ""

    def _download(self, rel, progress=None):
        token = self.token()
        for attempt in (1, 2):
            try:
                if token and rel.api_url:
                    # a private repo's asset only comes through the API; the redirect drops the token
                    resp = self.http.get(rel.api_url, MAX_TGZ, 300, accept="application/octet-stream", token=token,
                                         api=True, progress=progress)
                else:
                    resp = self.http.get(rel.url, MAX_TGZ, 300, accept="application/octet-stream", progress=progress)
                data = resp.body
                if len(data) != rel.size:
                    fail("download_incomplete", "The download of %s broke off (%d of %d bytes); try again."
                         % (rel.asset_name, len(data), rel.size))
                if rel.digest and sha256(data) != rel.digest:
                    fail("bad_digest", "%s doesn't match GitHub's checksum (asset %s), so it wasn't installed."
                         % (rel.asset_name, rel.asset_id))
                return data
            except UpdateError as e:
                if e.code not in ("download_incomplete", "bad_digest", "timeout") or attempt == 2:
                    raise
                log("%s; downloading again" % e.message)
        return None

    def _check_sums_asset(self, rel, data):
        """the release's own SHA256SUMS asset, when there is one, must agree"""
        if not rel.sums_url:
            if not rel.digest:
                log("%s has no checksum on GitHub; relying on TLS and the archive's SHA256SUMS" % rel.asset_name)
            return
        token = self.token()
        if token and rel.sums_api_url:
            body = self.http.get(rel.sums_api_url, 65536, 60, accept="application/octet-stream", token=token, api=True).body
        else:
            body = self.http.get(rel.sums_url, 65536, 60).body
        try:
            sums = parse_sums(body.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as e:
            fail("bad_digest", "The release's SHA256SUMS is broken (%s)." % e)
        want = sums.get(rel.asset_name)
        if want is None:
            if not rel.digest:
                fail("bad_digest", "The release's SHA256SUMS doesn't list %s." % rel.asset_name)
            return
        if want != sha256(data):
            fail("bad_digest", "%s doesn't match the release's SHA256SUMS, so it wasn't installed." % rel.asset_name)

    def _compat(self, pkg):
        mi = pkg.compat.get("min_image") if pkg.compat else None
        if not isinstance(mi, str) or not VER_RE.fullmatch(mi):
            return
        img = read_file(self.cfg.image_version_file, 256)
        img = img.decode("ascii", "replace").strip() if img else ""
        if VER_RE.fullmatch(img) and vkey(img) < vkey(mi):
            fail("needs_new_image", "StickTime Pi %s needs a newer SD card image (%s or later; this card has %s): "
                 "flash the new image from the release page." % (pkg.version, mi, img))

    def _preflight(self, pkg):
        mode = self.cfg.preflight
        if mode in ("0", "off", "no"):
            return
        if mode == "auto" and not (sys.platform.startswith("linux") and platform.machine() in ("aarch64", "arm64")):
            return
        b = pkg.files[BIN]
        if b"--version" not in b:
            log("the new binary has no --version: preflight skipped")
            return
        d = self.cfg.s("preflight")
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, "sticktime-pi")
        with open(p, "wb") as f:
            f.write(b)
        os.chmod(p, 0o755)
        try:
            r = subprocess.run([p, "--version"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=5)
        except subprocess.TimeoutExpired:
            fail("bad_package", "The new StickTime Pi didn't answer --version within 5 s, so it wasn't installed.")
        except OSError as e:
            fail("bad_package", "The new StickTime Pi can't run on this Pi (%s), so it wasn't installed." % e)
        finally:
            try:
                os.unlink(p)
            except OSError:
                pass
        out = r.stdout.decode("utf-8", "replace")
        if r.returncode < 0 or pkg.version not in out:
            fail("bad_package", "The new StickTime Pi failed its start test (%s), so it wasn't installed."
                 % (("signal %d" % -r.returncode) if r.returncode < 0 else repr(out.strip()[:80])))

    # ---------------------------------------------------------------- the journal
    def _write_journal(self, j):
        make_dirs(self.cfg.upd)
        write_file(self.journal_path, json_bytes(j))

    def _read_journal(self):
        if not os.path.lexists(self.journal_path):
            return None, False
        j = read_json(self.journal_path)
        ok = (isinstance(j, dict) and j.get("format") == 1 and j.get("op") in ("apply", "rollback")
              and isinstance(j.get("steps"), list) and isinstance(j.get("next"), int)
              and all(self._step_ok(s) for s in j["steps"]))
        return (j if ok else None), True

    @staticmethod
    def _step_ok(s):
        if not isinstance(s, list) or not s or s[0] not in ("remove", "rename"):
            return False
        args = s[1:]
        if len(args) != (1 if s[0] == "remove" else 2):
            return False
        return all(isinstance(a, str) and a and not a.startswith("/") and ".." not in a.split("/")
                   and "\\" not in a and ":" not in a for a in args)

    def _step(self, s):
        if s[0] == "remove":
            remove(self.b(s[1]))
            return
        a, b = self.b(s[1]), self.b(s[2])
        ea, eb = os.path.lexists(a), os.path.lexists(b)
        if ea and eb:
            # the journal is written after every step, so a replay redoes at most the last one, whose
            # source is then gone; both here means something else moved files: deleting b could take
            # the only good copy, so leave both (the verify after the journal tells)
            log("both %s and %s exist; left as they are" % (s[1], s[2]))
        elif ea:
            rename(a, b)
        # else: done before the power cut (or lost; the verify after the journal tells)

    def _run(self, j):
        while j["next"] < len(j["steps"]):
            i = j["next"]
            self._step(j["steps"][i])
            point("app_step_%d" % i)
            j["next"] = i + 1
            if j["next"] < len(j["steps"]):
                self._write_journal(j)

    def _journaled(self, j, at, staging=False):
        """the journal, then its steps. A full card before the first step changed nothing (the staging
        goes); during the steps they are tried once more (renames need no space, and the write that
        failed may fit now); after that the journal stays for the next run or the boot to finish"""
        try:
            self._write_journal(j)
        except OSError:
            if staging:
                try:
                    remove(self.b("update/staging"))
                except OSError as e:
                    log("couldn't remove the staging: %s" % e)
            raise
        point(at)
        try:
            self._run(j)
        except OSError as e:
            err = fs_error(e)
            if err is None or err.code != "disk_full":
                raise
            log("%s; trying the remaining steps again" % e)
            self._run(j)

    def _plan_apply(self, stage_rel, names):
        steps = []
        for n in MANAGED:
            here = os.path.lexists(self.b(n))
            staged = n in names
            if (not here or n in KEEP) and not staged:
                if here and os.path.lexists(self.b(n + ".prev")):
                    steps.append(["remove", n + ".prev"])     # a rollback keeps the current one too
                continue
            if os.path.lexists(self.b(n + ".prev")):
                steps.append(["remove", n + ".prev"])
            if here:
                steps.append(["rename", n, n + ".prev"])
            if staged:
                steps.append(["rename", stage_rel + "/" + n, n])
        return steps

    def _plan_swap(self):
        """the current set <-> the .prev set"""
        steps = []
        for n in MANAGED:
            here, prev = os.path.lexists(self.b(n)), os.path.lexists(self.b(n + ".prev"))
            if (not here or n in KEEP) and not prev:
                continue
            if os.path.lexists(self.b(n + ".swap")):
                steps.append(["remove", n + ".swap"])
            if here:
                steps.append(["rename", n, n + ".swap"])
            if prev:
                steps.append(["rename", n + ".prev", n])
            if here:
                steps.append(["rename", n + ".swap", n + ".prev"])
        return steps

    def verify_installed(self):
        """problems with the installed set against its SHA256SUMS ([] when fine or when there is none)"""
        data = read_file(self.b("SHA256SUMS"), 4 * MIB)
        if data is None:
            return [] if not os.path.lexists(self.b(REC)) else ["SHA256SUMS is missing"]
        try:
            sums = parse_sums(data.decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as e:
            return ["SHA256SUMS: %s" % e]
        out = []
        for p, h in sums.items():
            if p == "SHA256SUMS":
                continue
            b = read_file(self.b(p), MAX_FILE)
            if b is None or sha256(b) != h:
                out.append(p)
        return out

    def _disabled(self):
        d = read_json(os.path.join(self.cfg.upd, "disabled.json"))
        return d.get("app") if isinstance(d, dict) and isinstance(d.get("app"), list) else []

    def _record_disabled(self, version, digest, reason):
        """only keeps auto-update from reinstalling it: losing it (a full card) is no reason to fail"""
        lst = [x for x in self._disabled() if isinstance(x, dict)][-9:]
        lst.append({"version": version, "digest": digest, "reason": reason, "at": now_iso()})
        try:
            make_dirs(self.cfg.upd)
            write_file(os.path.join(self.cfg.upd, "disabled.json"), json_bytes({"app": lst}))
        except OSError as e:
            log("couldn't note that %s was rolled back: %s" % (version, e))

    def host_busy(self):
        """True while a flight runs (the host's /api/status says a state other than the menu)"""
        url = self.cfg.status_url
        if not url:
            return False
        try:
            req = urllib.request.Request(url, headers={"User-Agent": self.cfg.ua})
            with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(req, timeout=3) as r:
                d = json.loads(r.read(1 << 20).decode("utf-8"))
        except Exception:
            return False            # not running (or not answering): a restart can't interrupt anything
        g = d.get("game") if isinstance(d, dict) else None
        name = g.get("state_name") if isinstance(g, dict) else None
        return isinstance(name, str) and name not in ("menu", "unknown", "")

    def restart(self, auto=False):
        """the new set runs once sticktime.service restarts; an automatic run never cuts a flight short"""
        self.need_restart = False
        cmd = self.cfg.restart_cmd
        if not cmd or not cmd.strip():
            return False
        if auto and self.host_busy():
            log("a flight is running: the new StickTime Pi starts at the next restart")
            self.st.set(message=(self.st.d["message"] + " It starts the next time StickTime Pi restarts.").strip())
            return False
        try:
            argv = json.loads(cmd) if cmd.lstrip().startswith("[") else shlex.split(cmd)
            subprocess.run(argv, stdin=subprocess.DEVNULL, timeout=30, check=False)
        except (OSError, ValueError, subprocess.SubprocessError) as e:
            log("restart: %s" % e)
            return False
        return True

    # ---------------------------------------------------------------- commands
    def check(self, use_cache=True):
        best, pending, _, warning = self.latest(use_cache)
        cur = self.installed()
        self.st.part("app", installed=cur)
        if best is None:
            self.st.part("app", latest="", notes="")
            return False, ("StickTime Pi %s is being published." % pending if pending else
                           "No StickTime Pi release was found.") + warning
        self.st.part("app", latest=best.version, notes=best.notes)
        rec = read_json(self.b(REC))
        avail = not cur or vkey(best.version) > vkey(cur) or (
            vkey(best.version) == vkey(cur) and isinstance(rec, dict) and bool(best.digest)
            and rec.get("digest") not in (None, best.digest))
        msg = ("StickTime Pi %s is available (%s installed)." % (best.version, cur or "none") if avail
               else "StickTime Pi %s is up to date." % (cur or best.version))
        return avail, msg + warning

    def apply(self, version=None, force=False, auto=False, use_cache=True):
        st = self.st
        st.set("checking", "Looking for StickTime Pi releases…", 0.02, error="")
        best, pending, date, warning = self.latest(use_cache and version is None, want=version)
        if best is None:
            if version:
                fail("not_found", "StickTime Pi %s isn't among the releases on GitHub." % version)
            st.set("done", ("StickTime Pi %s is being published; try again in a few minutes." % pending if pending
                            else "No StickTime Pi release was found.") + warning, 1)
            return "none"
        if version is None:
            st.part("app", latest=best.version, notes=best.notes)
        cur = self.installed()
        st.part("app", installed=cur)
        rec = read_json(self.b(REC))
        same = bool(cur) and vkey(cur) == vkey(best.version) and (
            not isinstance(rec, dict) or not best.digest or rec.get("digest") in (None, best.digest))
        if same and not force:
            st.set("done", "StickTime Pi %s is up to date.%s" % (cur, warning), 1)
            return "current"
        older = bool(cur) and vkey(best.version) < vkey(cur)
        if older and not force and not auto:
            fail("downgrade_refused", "StickTime Pi %s is installed, newer than %s; going back needs a confirmation "
                 "(--force), so nothing was changed." % (cur, best.version))
        if auto:
            skip = None
            if older:
                # a deleted release or a moved tag: never go back on our own
                skip = "StickTime Pi %s is installed, newer than the newest release (%s)." % (cur, best.version)
            elif any(isinstance(x, dict) and x.get("version") == best.version and x.get("digest") == best.digest
                     for x in self._disabled()):
                skip = "StickTime Pi %s was rolled back on this Pi, so it isn't installed again automatically." % best.version
            else:
                t = iso_time(best.updated_at)
                if t is not None and date - t < self.cfg.settle_s:
                    skip = "StickTime Pi %s was published less than an hour ago; it will be installed later." % best.version
            if skip:
                st.set("done", skip, 1)
                return "skipped"

        msg = "Downloading StickTime Pi %s…" % best.version
        st.set("downloading", msg, 0.05)
        if free_bytes(self.cfg.state) < 2 * best.size:
            fail("disk_full", "Not enough memory for the download (%.1f MB free in %s)."
                 % (free_bytes(self.cfg.state) / MIB, self.cfg.state))

        def prog(n, total):
            st.progress(0.05 + 0.45 * (min(1.0, n / total) if total else 0), msg)
        try:
            data = self._download(best, prog)
            self._check_sums_asset(best, data)
        except UpdateError as e:
            if e.code in ("bad_digest", "not_found", "download_incomplete"):
                # the asset may have been replaced or removed since the list was cached
                try:
                    os.unlink(self.cfg.s("check-app.json"))
                except OSError:
                    pass
            raise
        st.set("installing", "Checking StickTime Pi %s…" % best.version, 0.55)
        pkg = validate_app_tar(data, best.version, best.asset_id)
        self._compat(pkg)
        self._preflight(pkg)
        point("app_verified")

        st.set("installing", "Installing StickTime Pi %s…" % best.version, 0.6)
        stage_rel = "update/staging/app-%s-%s" % (pkg.version, sha256(data)[:8])
        stage = self.b(stage_rel)
        rec_new = json_bytes({"version": pkg.version, "tag": best.tag, "digest": sha256(data), "asset_id": best.asset_id,
                              "asset_name": best.asset_name, "published_at": best.published_at, "installed_at": now_iso()})
        need = sum(len(b) for b in pkg.files.values()) + 4096 * (len(pkg.files) + 8) + len(rec_new)
        try:
            need_space(self.cfg.base, need)
            remove(self.b("update/staging"))
            for p, b in sorted(pkg.files.items()):
                q = os.path.join(stage, *p.split("/"))
                make_dirs(os.path.dirname(q))
                write_file(q, b)
            write_file(os.path.join(stage, REC), rec_new)
            # read back: a card that drops writes shows here, before anything is swapped
            for p, b in pkg.files.items():
                if read_file(os.path.join(stage, *p.split("/")), len(b) + 1) != b:
                    fail("bad_package", "Writing %s to the SD card failed (it reads back different); the card may be "
                         "failing." % p)
        except (UpdateError, OSError) as e:
            try:
                remove(self.b("update/staging"))
            except OSError:
                pass
            err = fs_error(e) if isinstance(e, OSError) else None
            if err:
                raise err
            raise
        point("app_staged")
        names = {p.split("/")[0] for p in pkg.files} | {REC}
        j = {"format": 1, "op": "apply", "version": pkg.version, "from": cur, "digest": sha256(data),
             "stage": stage_rel, "steps": self._plan_apply(stage_rel, names), "next": 0}
        try:
            self._journaled(j, "app_journal", staging=True)
            point("app_swapped")
            problems = self.verify_installed()
            if problems:
                self._swap_back(j, problems)
                fail("bad_package", "StickTime Pi %s didn't read back right after installing (%s); the previous "
                     "version is back." % (pkg.version, ", ".join(problems[:3])))
            remove(stage)
            remove(self.b("update/staging"))
            remove(self.journal_path)
        except OSError as e:
            err = fs_error(e)
            if err:
                # the journal stays: the next boot finishes or undoes the swap
                raise err
            raise
        point("app_done")
        st.part("app", installed=pkg.version)
        st.set("done", "StickTime Pi %s is installed; restarting…%s" % (pkg.version, warning), 1)
        self.need_restart = True
        return "installed"

    def _swap_back(self, j, problems):
        log("installed files don't verify (%s): going back" % ", ".join(problems[:5]))
        self._record_disabled(j.get("version"), j.get("digest"), "verify failed: " + ", ".join(problems[:3]))
        # the damaged set goes to .prev and then away: a later rollback must not bring it back
        steps = self._plan_swap() + [["remove", n + ".prev"] for n in MANAGED]
        back = {"format": 1, "op": "rollback", "version": j.get("from"), "steps": steps, "next": 0}
        self._journaled(back, "app_swap_back")
        remove(self.b("update/staging"))
        remove(self.journal_path)

    def rollback(self):
        st = self.st
        st.set("installing", "Going back to the previous StickTime Pi…", 0.1, error="")
        if not os.path.lexists(self.b("bin.prev")):
            fail("nothing_to_rollback", "No earlier StickTime Pi is kept on this Pi, so there is nothing to go back to.")
        cur = self.installed()
        prevb = read_file(self.b("VERSION.prev"), 64)
        prev = prevb.decode("ascii", "replace").strip() if prevb else ""
        rec = read_json(self.b(REC))
        try:
            j = {"format": 1, "op": "rollback", "version": prev, "from": cur, "steps": self._plan_swap(), "next": 0}
            self._journaled(j, "app_rollback_journal")
            remove(self.journal_path)
            self._record_disabled(cur, rec.get("digest") if isinstance(rec, dict) else None, "rolled back")
        except OSError as e:
            err = fs_error(e)
            if err:
                raise err
            raise
        st.part("app", installed=self.installed())
        st.set("done", "Went back to StickTime Pi %s; restarting…" % (prev or "the previous version"), 1)
        self.need_restart = True
        return prev

    def repair(self):
        notes = []
        j, present = self._read_journal()
        if j is not None:
            self._run(j)
            if j["op"] == "apply":
                problems = self.verify_installed()
                if problems:
                    self._swap_back(j, problems)
                    notes.append("StickTime Pi %s was damaged while installing; the previous version is back" % j.get("version"))
                else:
                    notes.append("finished installing StickTime Pi %s" % j.get("version"))
            else:
                notes.append("finished going back to StickTime Pi %s" % (j.get("version") or "the previous version"))
            remove(self.b("update/staging"))
            remove(self.journal_path)
            # the host that runs now may be the old one (a run killed without a reboot)
            self.need_restart = True
            return notes
        if present:
            log("the update journal is unreadable: repairing by what is on the card")
            remove(self.journal_path)
        for n in ESSENTIAL:
            if not os.path.lexists(self.b(n)):
                for alt in (n + ".prev", n + ".swap"):
                    if os.path.lexists(self.b(alt)):
                        rename(self.b(alt), self.b(n))
                        notes.append("restored %s" % n)
                        break
        for n in MANAGED:
            if os.path.lexists(self.b(n + ".swap")):
                remove(self.b(n + ".swap"))
        remove(self.b("update/staging"))
        return notes
