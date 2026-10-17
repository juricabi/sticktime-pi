"""What a StickTime release may contain and what the emulator page's Install may write: the zip
checks, sd_manifest.py's manifest, the editor's manProblem, the gallery and simulator.html checks."""
import io
import json
import re
import stat
import struct
import zipfile
import zlib

from .common import MIB, UpdateError, header_version, sha256, vkey

# sticktime/tools/sd_manifest.py (the page checks the same in editor.js)
EDITIONS = {
    "color": {"label": "Color", "loader": "StickTime.lua", "folder": None},
    "bw": {"label": "B&W", "loader": "StickTimeBW.lua", "folder": "StickTimeBW"},
    "lite": {"label": "Lite", "loader": "StickTimeLite.lua", "folder": "StickTimeLite"},
}
KEEP = ["StickTimeLite/data.txt"]
KEEP_NAME = re.compile(r"(^|/)(c\d+\.txt|[^/]*\.dat|data\.txt)$", re.I)
SHARED = ["StickTimeTracks/README.txt", "StickTimeGhosts/README.txt"]
GAME_NAME = re.compile(r"[A-Za-z0-9_-]+\.(lua|luac|txt)")
LOADERS = [e["loader"] for e in EDITIONS.values()]
ED_FOLDERS = [e["folder"] for e in EDITIONS.values() if e["folder"]]

PREFIX = "SCRIPTS/TOOLS/"
SEGMENT = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}")
MAX_ENTRIES = 256
MAX_FILE = 1048576            # the editor's cap per file
MAX_TOTAL = 8 * MIB
MIN_LUA = 10 * 1024
# everything a damaged zip can raise (a bad deflate stream is zlib.error, not BadZipFile)
ZIP_ERRORS = (zipfile.BadZipFile, zipfile.LargeZipFile, ValueError, EOFError, OSError, NotImplementedError,
              RuntimeError, zlib.error, struct.error, KeyError, IndexError)


def bad(why, asset_id=None):
    raise UpdateError("bad_zip", "The release file isn't a StickTime release the Pi can install (%s%s)."
                      % (why, ", asset %s" % asset_id if asset_id else ""))


def is_keep(rel):
    return bool(KEEP_NAME.search(rel)) or rel.lower() in (k.lower() for k in KEEP)


def edition_of(path):
    for name, e in EDITIONS.items():
        if path == e["loader"] or (e["folder"] and path.startswith(e["folder"] + "/")):
            return name
    return "shared"


def game_file(path, ed):
    """a file the page may write for edition ed (sd_manifest.check / editor manProblem)"""
    if ed == "shared":
        return path in SHARED
    e = EDITIONS[ed]
    return path == e["loader"] or bool(e["folder"] and path.startswith(e["folder"] + "/")
                                       and GAME_NAME.fullmatch(path[len(e["folder"]) + 1:]))


class GameZip:
    def __init__(self, files, version):
        self.files = files            # rel path under SCRIPTS/TOOLS -> bytes, as in the zip
        self.version = version        # StickTime.lua's header version string

    @property
    def lua(self):
        return self.files["StickTime.lua"]


def validate_game_zip(data, tag_version, asset_id=None):
    """the checks of releases.md 2.7; raises UpdateError('bad_zip')"""
    try:
        z = zipfile.ZipFile(io.BytesIO(data))
        infos = z.infolist()
    except ZIP_ERRORS as e:
        bad("not a zip: %s" % e, asset_id)
    if len(infos) > MAX_ENTRIES:
        bad("%d entries" % len(infos), asset_id)
    total, picked, seen = 0, {}, set()
    for i in infos:
        name = i.filename
        # ZipInfo cuts the name at a NUL and (on Windows) turns '\' into '/': judge the stored name
        raw = getattr(i, "orig_filename", name)
        if "\x00" in raw or "\\" in raw:
            bad("unsafe name %r" % raw[:80], asset_id)
        if i.flag_bits & 0x1:
            bad("%s is encrypted" % name, asset_id)
        if i.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            bad("%s uses compression %d" % (name, i.compress_type), asset_id)
        mode = i.external_attr >> 16
        if stat.S_ISLNK(mode):
            bad("%s is a link" % name, asset_id)
        if "\x00" in name or "\\" in name or name.startswith("/") or ":" in name:
            bad("unsafe name %r" % name[:80], asset_id)
        is_dir = name.endswith("/")
        parts = (name[:-1] if is_dir else name).split("/")
        for seg in parts:
            if seg in ("", ".", "..") or not SEGMENT.fullmatch(seg):
                bad("unsafe name %r" % name[:80], asset_id)
        if is_dir:
            if i.file_size:
                bad("%s: a folder with data" % name, asset_id)
            if name not in ("SCRIPTS/", PREFIX) and not name.startswith(PREFIX):
                bad("%s is outside SCRIPTS/TOOLS" % name, asset_id)
            continue
        if not name.startswith(PREFIX):
            bad("%s is outside SCRIPTS/TOOLS" % name, asset_id)
        rel = name[len(PREFIX):]
        if i.file_size > MAX_FILE:
            bad("%s is %d bytes" % (rel, i.file_size), asset_id)
        total += i.file_size
        if total > MAX_TOTAL:
            bad("more than 8 MB", asset_id)
        if rel.lower() in seen:
            bad("%s twice" % rel, asset_id)
        seen.add(rel.lower())
        if is_keep(rel):
            bad("%s is a player's file (saves, tracks and ghosts are never shipped)" % rel, asset_id)
        picked[rel] = i
    if "StickTime.lua" not in picked:
        bad("no SCRIPTS/TOOLS/StickTime.lua", asset_id)
    files = {}
    for rel, i in picked.items():
        try:
            b = z.read(i)            # checks the CRC
        except ZIP_ERRORS as e:
            bad("%s: %s" % (rel, e), asset_id)
        if len(b) != i.file_size:
            bad("%s: wrong size" % rel, asset_id)
        files[rel] = b
    lua = files["StickTime.lua"]
    first = lua.split(b"\n", 1)[0]
    if b"TNS|StickTime|TNE" not in first:
        bad("StickTime.lua isn't the StickTime tool", asset_id)
    if len(lua) < MIN_LUA:
        bad("StickTime.lua is only %d bytes" % len(lua), asset_id)
    v = header_version(lua)
    if v is None:
        bad("no version in StickTime.lua", asset_id)
    if vkey(v) != vkey(tag_version):
        bad("StickTime.lua says %s, the release %s" % (v, tag_version), asset_id)
    return GameZip(files, v)


def manifest(gz):
    """sd_manifest.py's manifest() for the editions this release fully has (loader present, same version),
    with only the files the page's manProblem accepts: (manifest, {rel: bytes})"""
    eds = {}
    for name, e in EDITIONS.items():
        b = gz.files.get(e["loader"])
        v = header_version(b) if b is not None else None
        if v is not None and vkey(v) == vkey(gz.version):
            eds[name] = dict(e)
    out = {}
    for rel in sorted(gz.files, key=lambda p: p.split("/")):
        ed = edition_of(rel)
        if ed != "shared" and ed not in eds:
            continue
        if is_keep(rel) or not game_file(rel, ed):
            continue
        out[rel] = gz.files[rel]
    m = {"version": gz.version, "base": "SCRIPTS/TOOLS", "editions": eds, "keep": list(KEEP),
         "files": [{"path": k, "size": len(v), "sha256": sha256(v), "edition": edition_of(k)} for k, v in out.items()]}
    return m, out


def manifest_bytes(m):
    return (json.dumps(m, indent=1) + "\n").encode("utf-8")


_HEX64 = re.compile(r"[0-9a-f]{64}")
_KINDS = ("color", "bw", "lite")


def man_problem(m):
    """editor.js manProblem: '' when the page would accept this manifest"""
    if (not isinstance(m, dict) or not isinstance(m.get("files"), list) or not isinstance(m.get("editions"), dict)
            or not isinstance(m.get("keep"), list)):
        return "not a list of files"
    v = m.get("version")
    if not isinstance(v, str) or not re.fullmatch(r"\d+(\.\d+)+", v) or m.get("base") != "SCRIPTS/TOOLS":
        return "its version or its folder"
    if not all(isinstance(k, str) for k in m["keep"]):
        return "its list of the player's files"
    for e, info in m["editions"].items():
        if (e not in _KINDS or not isinstance(info, dict) or not isinstance(info.get("label"), str)
                or info.get("loader") not in LOADERS
                or (info.get("folder") is not None and info.get("folder") not in ED_FOLDERS)):
            return "the %s edition" % e
    seen = set()
    keep = [k.lower() for k in m["keep"]]
    for f in m["files"]:
        p = f.get("path") if isinstance(f, dict) and isinstance(f.get("path"), str) else ""
        if not p or p.lower() in seen:
            return "a file without a name, or twice"
        seen.add(p.lower())
        if KEEP_NAME.search(p) or p.lower() in keep:
            return p + " is the player's own"
        ed = f.get("edition")
        info = m["editions"].get(ed) if isinstance(ed, str) else None
        if ed == "shared":
            ok = p in SHARED
        else:
            folder = info.get("folder") if info else None
            ok = bool(info) and (p == info["loader"] or (bool(folder) and p.startswith(folder + "/")
                                                       and bool(GAME_NAME.fullmatch(p[len(folder) + 1:]))))
        if not ok:
            return p + " isn't one of the game's files"
        size = f.get("size")
        if (not isinstance(size, int) or isinstance(size, bool) or size < 0 or size > MAX_FILE
                or not isinstance(f.get("sha256"), str) or not _HEX64.fullmatch(f["sha256"])):
            return p + "'s size"
    for info in m["editions"].values():
        if not any(isinstance(f, dict) and f.get("path") == info["loader"] for f in m["files"]):
            return info["loader"] + " is missing"
    return ""


# ---------------------------------------------------------------- the gallery and the page

MAX_GALLERY = 1 * MIB


def gallery_ok(data):
    """the site's tracks/index.json: {"tracks":[{text,...}]} as the editor's galleryOf reads it"""
    try:
        g = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return "not JSON"
    if not isinstance(g, dict) or not isinstance(g.get("tracks"), list):
        return "no list of tracks"
    t = g["tracks"]
    if not 1 <= len(t) <= 500:
        return "%d tracks" % len(t)
    good = 0
    for e in t:
        if not isinstance(e, dict):
            return "a track that isn't an object"
        text = e.get("text")
        if text is not None and not isinstance(text, str):
            return "a track's text"
        if isinstance(text, str) and len(text) > 1000:
            return "a track over 1000 characters"
        for k, cap in (("by", 60), ("about", 300), ("source", 300), ("name", 200), ("file", 200)):
            if k in e and e[k] is not None and not isinstance(e[k], str):
                return "a track's %s" % k
        was = e.get("was")
        if was is not None and (not isinstance(was, list) or len(was) > 100
                                or not all(isinstance(x, int) and not isinstance(x, bool) and x > 0 for x in was)):
            return "a track's earlier ids"
        if isinstance(text, str) and text.strip():
            good += 1
    return "" if good else "no track with a text"


def page_problem(html, version):
    """StickTime's web/simulator.html at the tag: '' when it looks right"""
    if not html[:64].lower().lstrip().startswith(b"<!doctype html"):
        return "not an HTML page"
    if ("StickTime v%s" % version).encode("ascii") not in html:
        return "not StickTime %s's page" % version
    try:
        html.decode("utf-8")
    except UnicodeDecodeError:
        return "not UTF-8"
    return ""
