"""Game updates (juricabi/sticktime releases).

Every installed release is a slot, game/<gen>-<version>-<sha8>/, written once and complete when its
SLOT.json exists: a power cut before that leaves an invisible folder that repair deletes. The
active slot is the highest gen with SLOT.json and no DISABLED marker. The files the host and the
web server use (sd/.../StickTime.lua, editor/index.html, editor/tracks/index.json) are copies,
and reconcile() makes them follow the active slot, at the end of every change and at every boot,
which also finishes an update the power cut interrupted. The player's saves, tracks and ghosts
are only ever read (for the backup before a version change)."""
import importlib.util
import io
import json
import os
import re
import time
import urllib.parse
import urllib.request
import zipfile

from .common import (LUA_MAX, MIB, TMP, UpdateError, VER_RE, create_marker, fail, free_bytes, fs_error,
                     header_version, json_bytes, log, make_dirs, need_space, now_iso, point, read_file,
                     read_json, remove, sha256, git_blob_sha1, vkey, write_file, write_state)
from .gamezip import (KEEP_NAME, MIN_LUA, SHARED, ZIP_ERRORS, gallery_ok, man_problem, manifest, manifest_bytes,
                      page_problem, validate_game_zip)
from .net import Release, iso_time, pick_release

SLOT_RE = re.compile(r"(\d{4,})-(\d+(?:\.\d+){1,3})-([0-9a-f]{8})")
LUA = "StickTime.lua"
ASSET = "StickTime-{ver}-sdcard.zip"
MAX_ZIP = 8 * MIB
MAX_PAGE = 8 * MIB
BACKUPS_KEPT = 5
STAGED_KEPT = 20
_SCRIPT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Slot:
    def __init__(self, root, name):
        m = SLOT_RE.fullmatch(name)
        self.name = name
        self.dir = os.path.join(root, name)
        self.gen = int(m.group(1))
        self.version = m.group(2)
        self.meta = self._load()

    def _load(self):
        p = os.path.join(self.dir, "SLOT.json")
        try:
            if os.path.getsize(p) == 0:
                return None
        except OSError:
            return None
        m = read_json(p, 1 * MIB)
        if (not isinstance(m, dict) or m.get("format") != 1 or m.get("kind") != "game"
                or m.get("version") != self.version or not isinstance(m.get("files"), dict)):
            return None
        for rel, e in m["files"].items():
            if (not isinstance(e, dict) or not isinstance(e.get("size"), int) or not isinstance(e.get("sha256"), str)
                    or not _safe_rel(rel)):
                return None
        return m if LUA in m["files"] else None

    @property
    def valid(self):
        return self.meta is not None

    @property
    def disabled(self):
        return os.path.lexists(os.path.join(self.dir, "DISABLED"))

    @property
    def lua_sha(self):
        return self.meta["files"][LUA]["sha256"]

    def has(self, rel):
        return rel in self.meta["files"]

    def path(self, rel):
        return os.path.join(self.dir, *rel.split("/"))

    def read(self, rel, verify=True):
        """the file's bytes, None when missing or (verify) not what SLOT.json says"""
        e = self.meta["files"].get(rel)
        if e is None:
            return None
        data = read_file(self.path(rel), max(e["size"], 0) + 1)
        if data is None:
            return None
        if verify and (len(data) != e["size"] or sha256(data) != e["sha256"]):
            return None
        return data

    def same_release(self, rel):
        """this slot holds release rel's exact asset"""
        m = self.meta
        if vkey(self.version) != vkey(rel.version) or m.get("source") != "github":
            return False
        if rel.digest:
            return m.get("asset_sha256") == rel.digest
        return m.get("asset_id") == rel.asset_id and m.get("asset_updated_at") == rel.updated_at


def _safe_rel(rel):
    return (isinstance(rel, str) and rel and not rel.startswith("/") and "\\" not in rel and ":" not in rel
            and all(s not in ("", ".", "..") for s in rel.split("/")))


class Installed:
    def __init__(self, slot, version, state, lua):
        self.slot = slot          # the active slot (or None)
        self.version = version    # what runs: the sd copy's header version, else the slot's
        self.state = state        # slot | staged | custom | missing | unmanaged
        self.lua = lua


class Game:
    def __init__(self, cfg, http, status):
        self.cfg = cfg
        self.http = http
        self.st = status
        self.repo = cfg.game_repo
        self._patcher = None

    # ---------------------------------------------------------------- paths
    @property
    def lua_path(self):
        return os.path.join(self.cfg.sd_tools, LUA)

    @property
    def stage_dir(self):
        return os.path.join(self.cfg.editor, "sdcard")

    # ---------------------------------------------------------------- slots
    def slots(self):
        try:
            names = os.listdir(self.cfg.game_dir)
        except OSError:
            return []
        out = [Slot(self.cfg.game_dir, n) for n in names
               if SLOT_RE.fullmatch(n) and os.path.isdir(os.path.join(self.cfg.game_dir, n))]
        return sorted(out, key=lambda s: s.gen)

    @staticmethod
    def active(slots):
        valid = [s for s in slots if s.valid]
        live = [s for s in valid if not s.disabled]
        if live:
            return live[-1]
        return valid[-1] if valid else None

    @staticmethod
    def previous(slots, act):
        older = [s for s in slots if s.valid and not s.disabled and act is not None and s.gen < act.gen]
        return older[-1] if older else None

    def make_slot(self, version, sha8, files, meta):
        try:
            names = [n for n in os.listdir(self.cfg.game_dir) if SLOT_RE.fullmatch(n)]
        except OSError:
            names = []
        gen = max(int(SLOT_RE.fullmatch(n).group(1)) for n in names) + 1 if names else 0
        name = "%04d-%s-%s" % (gen, version, sha8)
        d = os.path.join(self.cfg.game_dir, name)
        make_dirs(d)
        try:
            for i, (rel, data) in enumerate(files.items()):
                p = os.path.join(d, *rel.split("/"))
                make_dirs(os.path.dirname(p))
                write_file(p, data)
                if i == 0:
                    point("slot_partial")
            meta["files"] = {rel: {"size": len(b), "sha256": sha256(b)} for rel, b in files.items()}
            point("before_slot_json")
            write_file(os.path.join(d, "SLOT.json"), json_bytes(meta))
        except BaseException:
            try:
                self._delete(d)
            except Exception as e:
                log("couldn't remove the unfinished %s: %s" % (name, e))
            raise
        point("after_slot_json")
        return Slot(self.cfg.game_dir, name)

    @staticmethod
    def _delete(d):
        # SLOT.json first: a half-deleted slot must be invalid
        remove(os.path.join(d, "SLOT.json"))
        remove(d)

    def delete_slot(self, slot):
        self._delete(slot.dir)

    def retention(self):
        """keep the factory slot, the active one, the one before it and the newest disabled one"""
        slots = self.slots()
        valid = [s for s in slots if s.valid]
        act = self.active(slots)
        keep = {s.name for s in valid if s.gen == 0}
        if act:
            keep.add(act.name)
            prev = self.previous(slots, act)
            if prev:
                keep.add(prev.name)
        dis = [s for s in valid if s.disabled]
        if dis:
            keep.add(dis[-1].name)
        for s in slots:
            if s.name not in keep:
                try:
                    self.delete_slot(s)
                except OSError as e:
                    log("couldn't remove %s: %s" % (s.name, e))

    # ---------------------------------------------------------------- what is installed
    def sd_lua(self):
        return read_file(self.lua_path, LUA_MAX)

    def staged(self):
        """{"version","digest","manifest","lua"} of the release staged for the page's Install, or None"""
        mb = read_file(os.path.join(self.stage_dir, "manifest.json"), 1 * MIB)
        if mb is None:
            return None
        try:
            m = json.loads(mb.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return None
        if man_problem(m):
            return None
        rec = read_json(os.path.join(self.cfg.upd, "staged.json"))
        digest = None
        if isinstance(rec, dict) and rec.get("manifest_sha256") == sha256(mb):
            digest = rec.get("digest")
        lua = next((f["sha256"] for f in m["files"] if f["path"] == LUA), None)
        return {"version": m["version"], "digest": digest, "manifest": m, "lua": lua}

    def installed(self, slots=None):
        slots = self.slots() if slots is None else slots
        act = self.active(slots)
        cur = self.sd_lua()
        if cur is None:
            return Installed(act, act.version if act else "", "missing", None)
        csha = sha256(cur)
        version = header_version(cur) or (act.version if act else "")
        if act and csha == act.lua_sha:
            state = "slot"
        else:
            stg = self.staged()
            if (stg and stg["lua"] == csha) or csha in self._staged_history():
                state = "staged"            # the page's Install put a staged release there
            else:
                state = "unmanaged" if act is None else "custom"
        return Installed(act, version, state, cur)

    @staticmethod
    def update_available(inst, best):
        if best is None:
            return False
        if not inst.version:
            return True
        a, b = vkey(best.version), vkey(inst.version)
        if a != b:
            return a > b
        # a re-release of the same version (moved tag + replaced zip)
        return bool(inst.state == "slot" and inst.slot and inst.slot.meta.get("source") == "github"
                    and not inst.slot.same_release(best))

    # ---------------------------------------------------------------- reconcile
    # update/sd.json says which StickTime.lua in sd/ is ours: {"sha256": what we wrote, "prev": what was
    # there when we began}. It is written before the file, so a cut between the two still knows both.
    def _sd_record(self):
        rec = read_json(os.path.join(self.cfg.upd, "sd.json"))
        if not isinstance(rec, dict) or not isinstance(rec.get("sha256"), str):
            return None
        return {"sha256": rec["sha256"], "prev": rec.get("prev") if isinstance(rec.get("prev"), str) else None}

    def _remember_sd(self, digest, prev=None):
        want = {"sha256": digest, "prev": prev}
        if self._sd_record() != want:
            make_dirs(self.cfg.upd)
            write_file(os.path.join(self.cfg.upd, "sd.json"), json_bytes(want))

    def _replaceable(self, cur, slots, force):
        """may reconcile overwrite the sd copy? Ours (what we wrote or were about to replace; without a
        record, any release we keep) or damaged: yes. A copy from a PC or from the page's Install: no
        (unless forced)."""
        if cur is None or force:
            return True
        csha = sha256(cur)
        rec = self._sd_record()
        if rec is not None:
            if csha in (rec["sha256"], rec["prev"]):
                return True
        elif any(s.valid and s.lua_sha == csha for s in slots):
            return True
        return self._damaged(cur, csha, slots)

    @staticmethod
    def _damaged(cur, csha, slots):
        """what a power cut and fsck leave of our own copy: too short, no header, or a release's file
        with the same size but other bytes, or cut short. An exact release file is never damaged."""
        if len(cur) < MIN_LUA or b"TNS|StickTime|TNE" not in cur.split(b"\n", 1)[0]:
            return True
        hv = header_version(cur)
        if hv is None:
            return True
        same = [s for s in slots if s.valid and vkey(s.version) == vkey(hv)]
        if any(s.lua_sha == csha for s in same):
            return False
        for s in same:
            e = s.meta["files"][LUA]
            if len(cur) == e["size"]:
                return True
            if len(cur) < e["size"]:
                full = s.read(LUA)
                if full is not None and full.startswith(cur):
                    return True
        return False

    def reconcile(self, force=False, slots=None):
        """make sd/StickTime.lua, editor/ and GAME_VERSION follow the active slot: none | copied | custom"""
        slots = self.slots() if slots is None else slots
        act = self.active(slots)
        res = "none"
        if act is not None:
            want = act.read(LUA)
            cur = self.sd_lua()
            if want is not None:
                if cur is None or sha256(cur) != act.lua_sha:
                    if self._replaceable(cur, slots, force):
                        self._remember_sd(act.lua_sha, sha256(cur) if cur is not None else None)
                        make_dirs(self.cfg.sd_tools)
                        write_file(self.lua_path, want)
                        point("reconcile_lua")
                        self._remember_sd(act.lua_sha)
                        res = "copied"
                    else:
                        res = "custom"
                else:
                    self._remember_sd(act.lua_sha)
            self._readmes(act)
            self._page(act)
            self._gallery_fallback(act)
        self._game_version_file()
        return res

    def _readmes(self, act):
        missing = [r for r in SHARED if not os.path.lexists(os.path.join(self.cfg.sd_tools, *r.split("/")))]
        if not missing or not act.has("release.zip"):
            return
        data = act.read("release.zip")
        if data is None:
            return
        try:
            z = zipfile.ZipFile(io.BytesIO(data))
            for r in missing:
                try:
                    b = z.read("SCRIPTS/TOOLS/" + r)
                except KeyError:
                    continue
                p = os.path.join(self.cfg.sd_tools, *r.split("/"))
                make_dirs(os.path.dirname(p))
                write_file(p, b)
        except ZIP_ERRORS as e:
            if isinstance(e, OSError) and fs_error(e):
                raise
            log("README from %s: %s" % (act.name, e))

    def patcher(self):
        """tools/patch_editor.py's patch(): the installed app's lib/ (an app release can bring a newer one;
        the same card holds the binary that runs as root, so this trusts nothing new), else beside this
        script in the image, else the repo's in a checkout"""
        if self._patcher is None:
            self._patcher = (None, "")
            cands = ([self.cfg.patch_editor] if self.cfg.patch_editor else
                     [self.cfg.p("lib", "patch_editor.py"), os.path.join(_SCRIPT_DIR, "patch_editor.py"),
                      os.path.normpath(os.path.join(_SCRIPT_DIR, "..", "..", "..", "..", "..", "tools", "patch_editor.py"))])
            for p in cands:
                src = read_file(p, 1 * MIB)
                if src is None:
                    continue
                try:
                    spec = importlib.util.spec_from_file_location("stupdate_patch_editor", p)
                    mod = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(mod)
                except Exception as e:
                    log("%s: %s" % (p, e))
                    continue
                fn = getattr(mod, "patch", None) or getattr(mod, "patch_editor_page", None)
                if callable(fn):
                    self._patcher = (fn, sha256(src)[:16])
                    break
        return self._patcher

    def patch_page(self, raw):
        """simulator.html -> editor/index.html bytes; raises ValueError when it can't be patched"""
        fn, _ = self.patcher()
        if fn is None:
            raise ValueError("patch_editor.py is missing")
        out = fn(raw.decode("utf-8"))
        return out.encode("utf-8") if isinstance(out, str) else bytes(out)

    def _page(self, act):
        src = "simulator.html" if act.has("simulator.html") else ("editor.html" if act.has("editor.html") else None)
        if src is None:
            return
        target = os.path.join(self.cfg.editor, "index.html")
        recp = os.path.join(self.cfg.upd, "page.json")
        rec = read_json(recp)
        pid = self.patcher()[1] if src == "simulator.html" else ""
        src_sha = act.meta["files"][src]["sha256"]
        if (isinstance(rec, dict) and rec.get("src") == src_sha and rec.get("patcher") == pid
                and rec.get("sha256") is not None):
            cur = read_file(target, 16 * MIB)
            if cur is not None and sha256(cur) == rec["sha256"]:
                return
        raw = act.read(src)
        if raw is None:
            return
        if src == "simulator.html":
            try:
                html = self.patch_page(raw)
            except (ValueError, UnicodeDecodeError) as e:
                log("the editor page of %s can't be patched: %s" % (act.name, e))
                return
        else:
            html = raw
        make_dirs(self.cfg.editor)
        write_file(target, html)
        point("reconcile_page")
        make_dirs(self.cfg.upd)
        write_file(recp, json_bytes({"slot": act.name, "src": src_sha, "patcher": pid, "sha256": sha256(html)}))

    def _gallery_fallback(self, act):
        target = os.path.join(self.cfg.editor, "tracks", "index.json")
        cur = read_file(target, 1 * MIB)
        if cur is not None and not gallery_ok(cur):
            return
        data = act.read("tracks/index.json")
        if data is None or gallery_ok(data):
            return
        make_dirs(os.path.dirname(target))
        write_file(target, data)

    def _game_version_file(self):
        cur = self.sd_lua()
        v = header_version(cur) if cur else None
        if not v:
            return
        p = os.path.join(self.cfg.sd_tools, "GAME_VERSION")
        want = (v + "\n").encode("ascii")
        if read_file(p, 256) != want:
            write_file(p, want)

    # ---------------------------------------------------------------- the staged release
    def stage(self, rel, zipdata=None, gz=None, progress=None):
        """the release into editor/sdcard/ for the page's Install; False when it is staged already"""
        cur = self.staged()
        if cur and cur["digest"] and rel.digest and cur["digest"] == rel.digest and vkey(cur["version"]) == vkey(rel.version):
            return False
        if gz is None:
            if not rel.digest:
                # only apply checks a zip without a digest (against the tag's files): the page's
                # Install trusts what is staged, so nothing unchecked goes there
                log("StickTime %s has no checksum on GitHub: not staged" % rel.version)
                return False
            zipdata = self.download_zip(rel, progress)
            try:
                gz = validate_game_zip(zipdata, rel.version, rel.asset_id)
            except UpdateError:
                self.drop_downloads()
                raise
        self.stage_files(gz, sha256(zipdata))
        return True

    def stage_files(self, gz, digest):
        m, files = manifest(gz)
        mb = manifest_bytes(m)
        need_space(self.cfg.base, sum(len(b) for b in files.values()) + 4096 * (len(files) + 2) + len(mb))
        man = os.path.join(self.stage_dir, "manifest.json")
        # no manifest while the files change: the page then offers nothing instead of a mix
        remove(man)
        point("stage_cleared")
        tools = os.path.join(self.stage_dir, "SCRIPTS", "TOOLS")
        try:
            for rel, data in files.items():
                p = os.path.join(tools, *rel.split("/"))
                if read_file(p, len(data) + 1) != data:
                    make_dirs(os.path.dirname(p))
                    write_file(p, data)
        except OSError:
            # a half-staged release is useless (no manifest) and only takes space
            try:
                remove(self.stage_dir)
            except OSError as e:
                log("couldn't remove the half-staged release: %s" % e)
            raise
        point("stage_files")
        want = {rel.lower() for rel in files}
        for root, dirs, names in os.walk(tools, topdown=False):
            for n in names:
                p = os.path.join(root, n)
                rel = os.path.relpath(p, tools).replace(os.sep, "/")
                if rel.lower() not in want:
                    remove(p)
            for n in dirs:
                p = os.path.join(root, n)
                try:
                    if not os.listdir(p):
                        remove(p)
                except OSError:
                    pass
        write_file(man, mb)
        lua = next((f["sha256"] for f in m["files"] if f["path"] == LUA), None)
        # every StickTime.lua staged so far: a copy the page installed stays "a release" after the next one
        hist = [h for h in self._staged_history() if h != lua][-(STAGED_KEPT - 1):] + ([lua] if lua else [])
        make_dirs(self.cfg.upd)
        write_file(os.path.join(self.cfg.upd, "staged.json"),
                   json_bytes({"version": m["version"], "digest": digest, "manifest_sha256": sha256(mb), "lua": hist}))

    def _staged_history(self):
        rec = read_json(os.path.join(self.cfg.upd, "staged.json"))
        h = rec.get("lua") if isinstance(rec, dict) else None
        return [x for x in h if isinstance(x, str)] if isinstance(h, list) else []

    def staged_ok(self):
        stg = self.staged()
        if stg is None:
            return False
        tools = os.path.join(self.stage_dir, "SCRIPTS", "TOOLS")
        for f in stg["manifest"]["files"]:
            data = read_file(os.path.join(tools, *f["path"].split("/")), f["size"] + 1)
            if data is None or len(data) != f["size"] or sha256(data) != f["sha256"]:
                return False
        return True

    # ---------------------------------------------------------------- GitHub
    def latest(self, use_cache=True, want=None):
        """(best, pending, server time): the release list costs 1 API call, cached for 10 minutes"""
        cache = self.cfg.s("check-game.json")
        if use_cache and want is None:
            c = read_json(cache)
            if (isinstance(c, dict) and c.get("repo") == self.repo and isinstance(c.get("at"), (int, float))
                    and 0 <= time.time() - c["at"] < self.cfg.cache_s):
                best = Release.from_dict(c["best"]) if isinstance(c.get("best"), dict) else None
                return best, c.get("pending"), c.get("date") or time.time()
        rels, resp = self.http.get_json("%s/repos/%s/releases?per_page=30" % (self.cfg.api, self.repo))
        if not isinstance(rels, list):
            fail("unexpected_response", "GitHub's list of StickTime releases looks wrong.")
        best, pending = pick_release(rels, ASSET, MAX_ZIP, want)
        date = resp.date() or time.time()
        if want is None:
            write_state(cache, json_bytes({"at": time.time(), "repo": self.repo, "date": date, "pending": pending,
                                           "best": best.to_dict() if best else None}))
        return best, pending, date

    def forget_cache(self):
        try:
            os.unlink(self.cfg.s("check-game.json"))
        except OSError:
            pass

    def download_zip(self, rel, progress=None):
        dl = self.cfg.s("dl")
        cache = os.path.join(dl, "%s.zip" % (rel.digest or "asset-%s" % rel.asset_id))
        if rel.digest:
            data = read_file(cache, MAX_ZIP)
            if data is not None and len(data) == rel.size and sha256(data) == rel.digest:
                return data
        if free_bytes(self.cfg.state) < 2 * rel.size:
            fail("disk_full", "Not enough memory for the download (%.1f MB free in %s)."
                 % (free_bytes(self.cfg.state) / MIB, self.cfg.state))
        for attempt in (1, 2):
            try:
                resp = self.http.get(rel.url, MAX_ZIP, 120, accept="application/octet-stream", progress=progress)
                data = resp.body
                if len(data) != rel.size:
                    fail("download_incomplete", "The download of %s broke off (%d of %d bytes); try again."
                         % (rel.asset_name, len(data), rel.size))
                if rel.digest and sha256(data) != rel.digest:
                    fail("bad_digest", "%s doesn't match GitHub's checksum (asset %s), so it wasn't installed."
                         % (rel.asset_name, rel.asset_id))
                break
            except UpdateError as e:
                # signed CDN links expire and a replaced asset can be cached for a moment: start over once
                if e.code not in ("download_incomplete", "bad_digest", "timeout") or attempt == 2:
                    raise
                log("%s; downloading again" % e.message)
        try:
            for n in os.listdir(dl) if os.path.isdir(dl) else []:
                if n.endswith(".zip"):
                    os.unlink(os.path.join(dl, n))
            write_state(cache, data)
        except OSError as e:
            log("download cache: %s" % e)
        return data

    def drop_downloads(self):
        dl = self.cfg.s("dl")
        try:
            for n in os.listdir(dl):
                os.unlink(os.path.join(dl, n))
        except OSError:
            pass

    def _resolve(self, tag):
        """the tag's commit now (target_commitish goes stale when a tag moves)"""
        obj, _ = self.http.get_json("%s/repos/%s/git/ref/tags/%s" % (self.cfg.api, self.repo, urllib.parse.quote(tag)))
        for _ in range(4):
            o = obj.get("object") if isinstance(obj, dict) else None
            if not isinstance(o, dict) or not isinstance(o.get("sha"), str) or not re.fullmatch(r"[0-9a-f]{40}", o["sha"]):
                break
            if o.get("type") == "commit":
                return o["sha"]
            if o.get("type") != "tag":
                break
            obj, _ = self.http.get_json("%s/repos/%s/git/tags/%s" % (self.cfg.api, self.repo, o["sha"]))
        fail("unexpected_response", "Couldn't tell which commit StickTime's tag %s is." % tag)

    def _tree(self, commit):
        obj, _ = self.http.get_json("%s/repos/%s/git/trees/%s?recursive=1" % (self.cfg.api, self.repo, commit), cap=8 * MIB)
        if not isinstance(obj, dict) or not isinstance(obj.get("tree"), list):
            fail("unexpected_response", "GitHub's file list of StickTime %s looks wrong." % commit[:8])
        if obj.get("truncated"):
            log("the tree of %s is truncated: no blob checks" % commit[:8])
            return {}
        out = {}
        for e in obj["tree"]:
            if isinstance(e, dict) and e.get("type") == "blob" and isinstance(e.get("path"), str) and isinstance(e.get("sha"), str):
                out[e["path"]] = (e["sha"], e.get("size"))
        return out

    def _page_fetch(self, commit, tree, version):
        """(simulator.html at the commit, note): blob-checked against the tree; None when the release has none"""
        url = "%s/%s/%s/web/simulator.html" % (self.cfg.raw, self.repo, commit)
        try:
            data = self.http.get(url, MAX_PAGE, 180).body
        except UpdateError as e:
            if e.code == "not_found":
                return None, " This release has no track editor page; the old one stays."
            raise
        entry = tree.get("web/simulator.html")
        if entry is None and tree:
            # raw by commit and the tree are the same snapshot: a page the tree lacks can't be checked
            log("simulator.html at %s: not in the tree" % commit[:8])
            return None, " The track editor page of this release couldn't be checked; the old one stays."
        if entry and (entry[0] != git_blob_sha1(data) or (entry[1] is not None and entry[1] != len(data))):
            fail("bad_digest", "The track editor page from GitHub doesn't match StickTime %s's files; try again." % version)
        why = page_problem(data, version)
        if why:
            log("simulator.html at %s: %s" % (commit[:8], why))
            return None, " The track editor page of this release isn't usable (%s); the old one stays." % why
        try:
            self.patch_page(data)
        except (ValueError, UnicodeDecodeError) as e:
            # kept in the slot anyway: a newer patch_editor.py (an app update) can still make the page
            log("simulator.html at %s: %s" % (commit[:8], e))
            return data, " The track editor page couldn't be prepared for the Pi (%s); the old one stays." % e
        return data, ""

    def fetch_gallery(self):
        """the site's tracks/index.json (no API quota), None when it isn't usable"""
        try:
            resp = self.http.get(self.cfg.pages + "/tracks/index.json", 1 * MIB, 30)
        except UpdateError as e:
            log("gallery: %s" % e.message)
            return None
        if "json" not in resp.headers.get("content-type", "json").lower():
            log("gallery: not JSON (%s)" % resp.headers.get("content-type"))
            return None
        why = gallery_ok(resp.body)
        if why:
            log("gallery: %s" % why)
            return None
        return resp.body

    def refresh_gallery(self, data=None):
        data = self.fetch_gallery() if data is None else data
        if data is None:
            return False
        target = os.path.join(self.cfg.editor, "tracks", "index.json")
        if read_file(target, 1 * MIB) == data:
            return False
        make_dirs(os.path.dirname(target))
        write_file(target, data)
        return True

    def request_reload(self):
        url = self.cfg.reload_url
        if not url:
            return
        try:
            req = urllib.request.Request(url, data=b"", method="POST", headers={"X-StickTime": "1"})
            urllib.request.build_opener(urllib.request.ProxyHandler({})).open(req, timeout=3).close()
        except Exception as e:
            log("couldn't ask the game to reload (%s); it loads the new one at its next start" % e)

    # ---------------------------------------------------------------- backup
    def backup(self, frm, to, extra=None):
        """copies of the saves, tracks and ghosts (and a replaced custom StickTime.lua) into backup/"""
        src = self.cfg.sd_tools
        items = {}
        try:
            for n in os.listdir(src):
                if KEEP_NAME.search(n) and os.path.isfile(os.path.join(src, n)):
                    items[n] = os.path.join(src, n)
        except OSError:
            pass
        for sub in ("StickTimeTracks", "StickTimeGhosts"):
            d = os.path.join(src, sub)
            try:
                for n in os.listdir(d):
                    if os.path.isfile(os.path.join(d, n)) and not n.endswith(TMP):
                        items[sub + "/" + n] = os.path.join(d, n)
            except OSError:
                pass
        p = os.path.join(src, "StickTimeLite", "data.txt")
        if os.path.isfile(p):
            items["StickTimeLite/data.txt"] = p
        data = {}
        for rel, p in items.items():
            b = read_file(p, 16 * MIB)
            if b is not None:
                data[rel] = b
        for rel, b in (extra or {}).items():
            data[rel] = b
        if not data:
            return None
        clean = lambda v: re.sub(r"[^0-9.]", "", v or "") or "none"
        root = self.cfg.p("backup")
        name = "%s-%s-to-%s" % (time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()), clean(frm), clean(to))
        d, n = os.path.join(root, name), 2
        while os.path.exists(d):
            d = os.path.join(root, "%s-%d" % (name, n))
            n += 1
        try:
            for rel, b in data.items():
                q = os.path.join(d, *rel.split("/"))
                make_dirs(os.path.dirname(q))
                write_file(q, b)
        except OSError:
            try:
                remove(d)
            except OSError as e:
                log("couldn't remove the unfinished backup: %s" % e)
            raise
        try:
            olds = sorted(x for x in os.listdir(root) if os.path.isdir(os.path.join(root, x)))
            for x in olds[:-BACKUPS_KEPT]:
                remove(os.path.join(root, x))
        except OSError as e:
            log("old backups: %s" % e)
        return d

    def _backup_size(self):
        total = 0
        for root, _, names in os.walk(self.cfg.sd_tools):
            rel = os.path.relpath(root, self.cfg.sd_tools).replace(os.sep, "/")
            for n in names:
                if rel == "." and KEEP_NAME.search(n) or rel in ("StickTimeTracks", "StickTimeGhosts") or (
                        rel == "StickTimeLite" and n == "data.txt"):
                    try:
                        total += os.path.getsize(os.path.join(root, n)) + 4096
                    except OSError:
                        pass
        return total

    # ---------------------------------------------------------------- commands
    def check(self, use_cache=True):
        """refresh latest/notes/staged in the status; True when an update is available"""
        best, pending, _ = self.latest(use_cache)
        slots = self.slots()
        inst = self.installed(slots)
        self.st.part("game", installed=inst.version)
        notes = []
        if best is None:
            self.st.part("game", latest="", notes="")
            return False, ("StickTime %s is being published; check again in a few minutes." % pending if pending
                           else "No StickTime release with an SD card zip was found.")
        self.st.part("game", latest=best.version, notes=best.notes)
        try:
            self.stage(best, progress=self._dl_progress("Downloading StickTime %s for the editor's Install…" % best.version, 0.3, 0.8))
        except UpdateError as e:
            notes.append("Couldn't prepare it for the editor's Install: " + e.message)
        except OSError as e:
            err = fs_error(e)
            if err is None:
                raise
            notes.append("Couldn't prepare it for the editor's Install: " + err.message)
        try:
            self.refresh_gallery()
        except OSError as e:
            if fs_error(e) is None:
                raise
        stg = self.staged()
        self.st.part("game", staged=stg["version"] if stg else "")
        avail = self.update_available(inst, best)
        if avail:
            msg = "StickTime %s is available (%s installed)." % (best.version, inst.version or "none")
        elif inst.state == "custom":
            msg = "Your own StickTime.lua (%s) is installed; the newest release is %s." % (inst.version or "no version", best.version)
        else:
            msg = "StickTime %s is up to date." % (inst.version or best.version)
        if pending:
            msg += " StickTime %s is being published." % pending
        return avail, " ".join([msg] + notes)

    def _dl_progress(self, msg, lo, hi):
        def cb(n, total):
            frac = (n / total) if total else 0
            self.st.progress(lo + (hi - lo) * min(1.0, frac), msg)
        return cb

    def _disabled_release(self, slots, rel):
        return any(s.valid and s.disabled and s.same_release(rel) for s in slots)

    def apply(self, version=None, force=False, auto=False, use_cache=True):
        """install the newest release (or version); returns installed | current | skipped | none"""
        st = self.st
        st.set("checking", "Looking for StickTime releases…", 0.02, error="")
        best, pending, date = self.latest(use_cache and version is None, want=version)
        if best is None:
            if version:
                fail("not_found", "StickTime %s isn't among the releases on GitHub (or its zip is missing)." % version)
            st.set("done", "StickTime %s is being published; try again in a few minutes." % pending if pending
                   else "No StickTime release with an SD card zip was found.", 1)
            return "none"
        if version is None:
            st.part("game", latest=best.version, notes=best.notes)
        slots = self.slots()
        inst = self.installed(slots)
        st.part("game", installed=inst.version)
        act = inst.slot
        cmpv = 1 if not inst.version else (vkey(best.version) > vkey(inst.version)) - (vkey(best.version) < vkey(inst.version))
        same = act is not None and act.same_release(best)
        # the same version from the image (or a snapshot of it): nothing to download
        unproven = cmpv == 0 and (inst.state == "unmanaged" or (
            inst.state == "slot" and act.meta.get("source") != "github"))
        if (same and inst.state in ("slot", "missing")) or (unproven and not force):
            self.reconcile(slots=slots)
            self._finish_extras(best)
            st.part("game", installed=self.installed().version)
            st.set("done", "StickTime %s is up to date." % (inst.version or best.version), 1)
            return "current"
        if same and not auto:
            # the release is kept already: put it back over a copy from a PC or the page
            try:
                if inst.state == "custom":
                    self.backup(inst.version, best.version, {"StickTime.lua": inst.lua})
                self.reconcile(force=True, slots=slots)
            except OSError as e:
                err = fs_error(e)
                if err:
                    raise err
                raise
            self._finish_extras(best)
            self.request_reload()
            st.part("game", installed=self.installed().version)
            st.set("done", "StickTime %s is installed." % best.version, 1)
            return "installed"
        gmin = self.game_min()
        if gmin and vkey(best.version) < vkey(gmin):
            # compat.json of the installed StickTime Pi: older games don't run on this host
            msg = "StickTime Pi %s needs StickTime %s or later, so %s isn't installed." % (
                self.cfg.app_version() or "", gmin, best.version)
            if auto:
                self.reconcile(slots=slots)
                st.set("done", msg, 1)
                return "skipped"
            fail("downgrade_refused", msg)
        if cmpv < 0 and not force and not auto:
            fail("downgrade_refused", "StickTime %s is installed, newer than %s; going back needs a confirmation "
                 "(--force), so nothing was changed." % (inst.version, best.version))
        if auto:
            skip = None
            if cmpv < 0:
                # a deleted release or a moved tag: never go back on our own
                skip = "StickTime %s is installed, newer than the newest release (%s)." % (inst.version, best.version)
            elif inst.state == "custom":
                skip = "Your own StickTime.lua (%s) stays: automatic updates never replace it." % (inst.version or "no version")
            elif self._disabled_release(slots, best):
                skip = "StickTime %s was rolled back on this Pi, so it isn't installed again automatically." % best.version
            elif cmpv == 0 and (inst.state == "unmanaged" or (inst.state == "slot" and act.meta.get("source") != "github")):
                skip = "StickTime %s is up to date." % inst.version
            else:
                t = iso_time(best.updated_at)
                if t is not None and date - t < self.cfg.settle_s:
                    skip = "StickTime %s was published less than an hour ago; it will be installed later." % best.version
            if skip:
                self.reconcile(slots=slots)
                st.set("done", skip, 1)
                return "skipped"

        # download and check everything in RAM before the first write to the SD card
        msg = "Downloading StickTime %s…" % best.version
        st.set("downloading", msg, 0.05)
        try:
            zipdata = self.download_zip(best, self._dl_progress(msg, 0.05, 0.45))
        except UpdateError as e:
            if e.code in ("bad_digest", "not_found", "download_incomplete"):
                # the asset may have been replaced or removed since the list was cached
                self.forget_cache()
            raise
        st.set("installing", "Checking StickTime %s…" % best.version, 0.5)
        try:
            gz = validate_game_zip(zipdata, best.version, best.asset_id)
        except UpdateError:
            self.drop_downloads()
            raise
        point("after_download")
        commit = self._resolve(best.tag)
        tree = self._tree(commit)
        if not best.digest:
            e = tree.get("sdcard/SCRIPTS/TOOLS/StickTime.lua")
            if not e or e[0] != git_blob_sha1(gz.lua):
                fail("bad_digest", "StickTime %s's zip has no checksum on GitHub and doesn't match the tag's files, "
                     "so it wasn't installed (asset %s)." % (best.version, best.asset_id))
        st.set("downloading", "Downloading the track editor page…", 0.6)
        page, note = self._page_fetch(commit, tree, gz.version)
        st.set("downloading", "Downloading the track gallery…", 0.75)
        gallery = self.fetch_gallery()
        point("after_verify")

        st.set("installing", "Installing StickTime %s…" % best.version, 0.8)
        files = {"release.zip": zipdata, LUA: gz.lua}
        if page is not None:
            files["simulator.html"] = page
        if gallery is not None:
            files["tracks/index.json"] = gallery
        custom = inst.lua if inst.state == "custom" else None
        need = (sum(len(b) for b in files.values()) + 4096 * (len(files) + 2) + len(gz.lua)
                + (len(page) + 65536 if page else 0) + self._backup_size() + len(custom or b""))
        need_space(self.cfg.base, need)
        slot = None
        try:
            if not any(s.valid for s in slots):
                self._snapshot()
                point("after_snapshot")
            meta = {"format": 1, "kind": "game", "version": best.version, "tag": best.tag, "commit": commit,
                    "release_id": best.release_id, "asset_id": best.asset_id, "asset_name": best.asset_name,
                    "asset_size": best.size, "asset_sha256": sha256(zipdata), "asset_digest": best.digest,
                    "asset_updated_at": best.updated_at, "published_at": best.published_at,
                    "installed_at": now_iso(), "source": "github", "lua": LUA, "notes": best.notes}
            slot = self.make_slot(best.version, sha256(zipdata)[:8], files, meta)
            if inst.version != gz.version or custom is not None:
                self.backup(inst.version, gz.version, {"StickTime.lua": custom} if custom is not None else None)
            point("after_backup")
            # only a custom copy is protected (auto mode skipped it above); a release the page's Install
            # put there is replaced like ours
            self.reconcile(force=not auto or inst.state != "custom")
            point("after_reconcile")
        except (UpdateError, OSError) as e:
            if slot is not None:
                # the previous slot stays active
                try:
                    self.delete_slot(slot)
                    self.reconcile()
                except Exception as e2:
                    log("cleanup after the failed install: %s" % e2)
            err = fs_error(e) if isinstance(e, OSError) else None
            if err:
                raise err
            raise
        if gallery is not None:
            try:
                self.refresh_gallery(gallery)
            except OSError as e:
                log("gallery: %s" % e)
        try:
            self.stage(best, zipdata, gz)
        except (UpdateError, OSError) as e:
            log("staging: %s" % e)
        point("after_stage")
        self.request_reload()
        self.retention()
        stg = self.staged()
        st.part("game", installed=self.installed().version, staged=stg["version"] if stg else "")
        st.set("done", "StickTime %s is installed.%s" % (gz.version, note), 1)
        return "installed"

    def game_min(self):
        c = read_json(self.cfg.p("compat.json"), 64 * 1024)
        v = c.get("game_min") if isinstance(c, dict) else None
        return v if isinstance(v, str) and VER_RE.fullmatch(v) else None

    def _finish_extras(self, best):
        """an up-to-date game still gets the newest gallery, and the release staged for the page"""
        try:
            self.refresh_gallery()
            self.stage(best)
        except (UpdateError, OSError) as e:
            log("gallery/staging: %s" % e)
        stg = self.staged()
        self.st.part("game", staged=stg["version"] if stg else "")

    def _snapshot(self):
        """the game the device came with becomes slot 0000, so a rollback can return to it"""
        cur = self.sd_lua()
        hv = header_version(cur) if cur else None
        if not hv or not VER_RE.fullmatch(hv):
            return None
        files = {LUA: cur}
        page = read_file(os.path.join(self.cfg.editor, "index.html"), 16 * MIB)
        if page and page[:64].lower().lstrip().startswith(b"<!doctype html"):
            files["editor.html"] = page
        g = read_file(os.path.join(self.cfg.editor, "tracks", "index.json"), 1 * MIB)
        if g is not None and not gallery_ok(g):
            files["tracks/index.json"] = g
        meta = {"format": 1, "kind": "game", "version": hv, "source": "snapshot", "installed_at": now_iso(), "lua": LUA}
        slot = self.make_slot(hv, sha256(cur)[:8], files, meta)
        self._remember_sd(sha256(cur))
        return slot

    def rollback(self):
        st = self.st
        st.set("installing", "Going back to the previous StickTime…", 0.1, error="")
        slots = self.slots()
        inst = self.installed(slots)
        act = inst.slot
        if act is None:
            fail("nothing_to_rollback", "No earlier StickTime is kept on this Pi, so there is nothing to go back to.")
        try:
            if inst.state in ("custom", "staged"):
                # back to the release the updater installed
                target = act
                self.backup(inst.version, act.version, {"StickTime.lua": inst.lua})
                self.reconcile(force=True, slots=slots)
            else:
                target = self.previous(slots, act)
                if target is None:
                    fail("nothing_to_rollback", "StickTime %s is the oldest version kept on this Pi, so there is "
                         "nothing to go back to." % act.version)
                create_marker(os.path.join(act.dir, "DISABLED"), json.dumps(
                    {"reason": "rolled back", "at": now_iso(), "digest": act.meta.get("asset_sha256")}) + "\n")
                point("rollback_marked")
                if inst.version != target.version:
                    self.backup(inst.version, target.version)
                self.reconcile(force=True)
        except OSError as e:
            err = fs_error(e)
            if err:
                raise err
            raise
        self.request_reload()
        st.part("game", installed=self.installed().version)
        st.set("done", "Went back to StickTime %s." % target.version, 1)
        return target

    def cleanup(self):
        """our temp files and the slots a cut-off run left without SLOT.json; nothing else"""
        notes = []
        for d in (self.cfg.game_dir, self.cfg.sd_tools, self.cfg.editor, self.cfg.upd, self.cfg.p("backup")):
            for root, _, names in os.walk(d):
                for n in names:
                    if n.endswith(TMP):
                        remove(os.path.join(root, n))
        for s in self.slots():
            if not s.valid:
                log("removing the unfinished slot %s" % s.name)
                self._delete(s.dir)
                notes.append("removed an unfinished game update")
        return notes

    def repair(self):
        """boot: remove leftovers, check the active slot, finish an interrupted change; no network"""
        notes = self.cleanup()
        for _ in range(64):
            slots = self.slots()
            act = self.active(slots)
            if act is None or act.disabled:
                break
            if act.read(LUA) is not None and (not act.has("release.zip") or act.read("release.zip") is not None):
                break
            zipdata = act.read("release.zip") if act.has("release.zip") else None
            fixed = False
            if zipdata is not None:
                try:
                    gz = validate_game_zip(zipdata, act.version)
                    if sha256(gz.lua) == act.lua_sha:
                        write_file(act.path(LUA), gz.lua)
                        fixed = True
                        notes.append("repaired StickTime %s from its release zip" % act.version)
                except UpdateError as e:
                    log("%s: %s" % (act.name, e.message))
            if not fixed:
                log("%s is damaged: disabled" % act.name)
                create_marker(os.path.join(act.dir, "DISABLED"),
                              json.dumps({"reason": "corrupt", "at": now_iso(), "digest": act.meta.get("asset_sha256")}) + "\n")
                notes.append("StickTime %s was damaged on the SD card and is not used" % act.version)
        res = self.reconcile()
        if res == "copied":
            notes.append("finished installing StickTime %s" % self.installed().version)
        if not self.staged_ok():
            act = self.active(self.slots())
            zipdata = act.read("release.zip") if act is not None and act.has("release.zip") else None
            staged = False
            if zipdata is not None:
                try:
                    self.stage_files(validate_game_zip(zipdata, act.version), sha256(zipdata))
                    staged = True
                except (UpdateError, OSError) as e:
                    if isinstance(e, OSError) and fs_error(e) is None:
                        raise
                    log("staging from %s: %s" % (act.name, e))
            if not staged:
                remove(os.path.join(self.stage_dir, "manifest.json"))
        return notes
