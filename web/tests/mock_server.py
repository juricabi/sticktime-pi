#!/usr/bin/env python3
"""A stand-in for the StickTime Pi's web server (docs/WEB_API.md), for developing and testing the
web UI (web/) and the editor shim (web/pi-sd.js) on a PC. Python standard library only.

    python web/tests/mock_server.py [--port 8080] [--sd DIR] [--music DIR] [--simulator PATH]
                                    [--sdcard DIR] [--gallery FILE] [--hostname sticktime]

Without --sd/--music it makes a temporary SD folder (SCRIPTS/TOOLS/StickTime.lua, a track, a save)
and a music folder. --simulator patches StickTime's web/simulator.html with tools/patch_editor.py
and serves it at /editor/; --sdcard serves a staged release (tools/sd_manifest.py OUT) at
/editor/sdcard/ for the editor's Install; --gallery serves /editor/tracks/index.json.

Open http://localhost:8080/ (or http://sticktime.local:8080/ through a hosts entry: the Pi's page
is not a secure context, localhost is). The tests import MockPi and drive it directly; the same
state is reachable over HTTP under /__mock/ (requests log, settings of the mock itself).
"""
import argparse
import email.utils
import hashlib
import ipaddress
import json
import math
import os
import pathlib
import re
import shutil
import struct
import sys
import tempfile
import threading
import time
import urllib.parse
import zlib
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
WEB = REPO / "web"
KEYS_H = REPO / "host" / "src" / "settings_keys.h"

KIND = {"SK_STRING": "string", "SK_ENUM": "enum", "SK_INT": "int", "SK_NUM": "num", "SK_BOOL": "bool", "SK_SECRET": "secret"}
# as web_api.c: read by the host at start (the service restarts), and applied by sticktime-setup at boot
RESTART_KEYS = {"aspect", "av_mode", "hdmi_mode", "margin", "fonts", "audio_out"}
BOOT_KEYS = {"wifi_ssid", "wifi_password", "wifi_country", "hotspot", "hotspot_ssid", "hotspot_password", "bluetooth",
             "hostname", "ssh", "password"}
STATE_NAMES = ["unknown", "menu", "setup", "count", "fly", "crashed", "ready", "paused", "done"]
TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8",
         ".json": "application/json; charset=utf-8", ".png": "image/png", ".svg": "image/svg+xml", ".ico": "image/x-icon",
         ".txt": "text/plain; charset=utf-8", ".woff2": "font/woff2", ".wav": "audio/wav", ".mp3": "audio/mpeg",
         ".ogg": "audio/ogg", ".lua": "application/octet-stream", ".luac": "application/octet-stream"}
KEYS_ALLOWED = {"enter", "exit", "next", "prev", "inc", "dec", "nextpage", "prevpage", "menu", "up", "down", "enterlong", "exitlong"}
BOOL_WORDS = {"on", "off", "yes", "no", "true", "false", "1", "0"}
TMP_PREFIX = ".st-tmp-"
DEVICE = re.compile(r"(con|prn|aux|nul|com[0-9]|lpt[0-9])(\..*)?", re.I)
SLOT_RE = re.compile(r"scripts/tools/sticktimetracks/c[1-8]\.txt")
GAME_NAME = re.compile(r"[a-z0-9_-]+\.(lua|luac|txt|new)")


def load_keys(path=KEYS_H):
    """the known settings, from the C table (so the mock follows host/src/settings_keys.h)"""
    text = path.read_text(encoding="utf-8")
    s = r'"((?:[^"\\]|\\.)*)"'
    rx = re.compile(r'X\(\s*' + s + r',\s*(SK_\w+),\s*' + s + r',\s*' + s + r',\s*(true|false),\s*(true|false),\s*' + s + r'\)')
    keys = []
    for m in rx.finditer(text):
        key, kind, default, choices, live, reboot, help_ = m.groups()
        k = {"key": key, "kind": KIND[kind], "default": "" if kind == "SK_SECRET" else default, "live": live == "true",
             "reboot": reboot == "true" or key in BOOT_KEYS, "restart": key in RESTART_KEYS, "help": help_}
        if k["kind"] == "enum":
            k["choices"] = choices.split("|")
        elif k["kind"] in ("int", "num") and choices:
            lo, hi = choices.split("|")
            k["min"], k["max"] = (int(lo), int(hi)) if k["kind"] == "int" else (float(lo), float(hi))
            k["choices"] = []
        else:
            k["choices"] = []
        keys.append(k)
    if not keys:
        raise SystemExit(f"no settings in {path}")
    return keys


# ---------------------------------------------------------------- the settings format (settings.h)
def parse_settings(text):
    """{key: value} and warnings, as settings.c reads sticktime.txt"""
    out, warn = {}, []
    if text.startswith("﻿"):
        text = text[1:]
    for n, line in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        t = line.strip()
        if not t or t.startswith("#"):
            continue
        if "=" not in t:
            warn.append(f"line {n}: no '='")
            continue
        k, v = t.split("=", 1)
        k, v = k.strip().lower(), v.strip()
        if v.startswith('"'):
            val, i = [], 1
            while i < len(v) and v[i] != '"':
                if v[i] == "\\" and i + 1 < len(v):
                    i += 1
                val.append(v[i])
                i += 1
            v = "".join(val)
        else:
            m = re.search(r"[ \t]#", v)
            if m:
                v = v[:m.start()].rstrip()
        if k:
            out[k] = v
    return out, warn


def quote(v):
    if v != v.strip() or re.search(r"[ \t]#|^#|\"|\\", v):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return v


def validate(k, v):
    """None if fine, else the message (what settings_validate says, roughly)"""
    kind = k["kind"]
    if kind == "enum":
        return None if v.lower() in k["choices"] else "must be one of " + ", ".join(k["choices"])
    if kind == "bool":
        return None if v.lower() in BOOL_WORDS else "must be on or off"
    if kind in ("int", "num"):
        try:
            n = int(v, 10) if kind == "int" else float(v)
        except ValueError:
            return "must be a whole number" if kind == "int" else "must be a number"
        if "min" in k and not (k["min"] <= n <= k["max"]):
            return f"must be {k['min']} to {k['max']}"
        return None
    key = k["key"]
    if key == "hostname" and not re.fullmatch(r"[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?", v):
        return "letters, digits and dashes only (not at the start or end), 63 at most"
    if key == "wifi_country" and not re.fullmatch(r"[A-Za-z]{2}", v):
        return "a two-letter country code, like US or DE"
    if key == "channel_order" and sorted(v.upper()) != sorted("AETR"):
        return "the four letters A, E, T and R in any order, like AETR or TAER"
    if key == "stick_channels" and stick_channels(v) is None:
        return "four different channels 1 to 16 (aileron elevator throttle yaw), like 1 2 3 4, or empty"
    if key == "invert" and v and not all(p.strip().lower() in ("ail", "ele", "thr", "rud") for p in v.split(",")):
        return "a list of ail, ele, thr, rud"
    if key == "wifi_module_ip" and v:
        try:
            ipaddress.IPv4Address(v)
        except ValueError:
            return "an IP address like 10.0.0.1, or empty"
    if key == "hdmi_mode" and not re.fullmatch(r"auto|\d{3,4}x\d{3,4}(@\d{2,3})?", v):
        return "auto, or a size like 1280x720 or 1280x720@60"
    if key == "latency":
        try:
            if not 0 <= float(v) <= 1:
                return "0 to 1 seconds"
        except ValueError:
            return "a number of seconds, like 0 or 0.05"
    if key == "hotspot_password" and v and not 8 <= len(v.encode()) <= 63:
        return "8 to 63 characters"
    if key == "hotspot_ssid" and not 1 <= len(v.encode()) <= 32:
        return "1 to 32 characters"
    if key == "script" and not re.fullmatch(r"/SCRIPTS/[^\0]+\.lua", v, re.I):
        return "a .lua file in /SCRIPTS/"
    return None


def stick_channels(v):
    """[ail, ele, thr, rud] channels, [] for none, None when invalid (input.c mix_parse_channels)"""
    out = []
    for t in re.split(r"[ \t,]+", v):
        if not t:
            continue
        m = re.fullmatch(r"(?:ch)?([0-9]{1,2})", t, re.I)
        c = int(m.group(1)) if m else 0
        if not 1 <= c <= 16 or c in out or len(out) == 4:
            return None
        out.append(c)
    return out if len(out) in (0, 4) else None


def normalize(k, v):
    """the stored form of a valid value (settings.c normalize())"""
    if k["kind"] == "bool":
        return "on" if v.lower() in ("on", "yes", "true", "1") else "off"
    if k["kind"] == "int":
        return str(int(v, 10))
    if k["kind"] == "enum" or k["key"] in ("hdmi_mode", "latency"):
        return v.lower()
    if k["key"] == "stick_channels":
        return " ".join(str(c) for c in stick_channels(v) or [])
    if k["key"] in ("wifi_country", "channel_order"):
        return v.upper()
    return v


# ---------------------------------------------------------------- /api/sd paths
class ApiError(Exception):
    def __init__(self, status, name, message, **extra):
        super().__init__(message)
        self.status, self.name, self.message, self.extra = status, name, message, extra


BAD_CHARS = re.compile(r'[\x00-\x1f\x7f"*:<>?|\\/]')


def check_component(c, max_bytes=255):
    if c in (".", "..") or not c:
        raise ApiError(400, "TypeError", f"'{c}' is not a valid name.")
    if BAD_CHARS.search(c) or c[-1] in ". " or len(c.encode()) > max_bytes:
        raise ApiError(400, "TypeError", f"'{c}' is not a valid name on the SD card.")


def split_path(raw):
    """the raw (still encoded) path after /api/sd/ -> its decoded components"""
    if len(raw.encode()) > 1024 * 3:
        raise ApiError(400, "TypeError", "The path is too long.")
    parts = raw.split("/")
    if parts and parts[-1] == "":
        parts = parts[:-1]
    out = []
    for p in parts:
        try:
            c = urllib.parse.unquote(p, errors="strict")
        except UnicodeDecodeError:
            raise ApiError(400, "TypeError", "A name is not UTF-8.")
        check_component(c)
        if c.startswith(TMP_PREFIX) or c.endswith(".tmp~"):
            raise ApiError(400, "TypeError", "That name isn't allowed on the SD card.")
        out.append(c)
    if len("/".join(out).encode()) > 1024:
        raise ApiError(400, "TypeError", "The path is too long.")
    return out


def ms(t):
    return int(t * 1000)


def stat_json(p, name):
    st = os.lstat(p)
    d = os.path.isdir(p)
    j = {"name": name, "kind": "directory" if d else "file", "mtime": ms(st.st_mtime)}
    if not d:
        j["size"] = st.st_size
    return j


# ---------------------------------------------------------------- a small PNG (the "screenshot")
def png(w, h, phase):
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            sky = y < h * 0.45 + math.sin(phase + x / 40) * 8
            if sky:
                row += bytes((40 + y * 60 // h, 90 + y * 80 // h, 200))
            else:
                g = 90 + ((x // 16 + y // 16 + int(phase * 3)) % 2) * 25
                row += bytes((30, g, 40))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")


# ---------------------------------------------------------------- the mock's state
SCAN_NETWORKS = [
    {"ssid": "Home", "signal": 72, "security": "WPA2", "band": "2.4", "chan": 6, "known": True},
    {"ssid": "ExpressLRS TX", "signal": 88, "security": "WPA2", "band": "2.4", "chan": 1, "known": False},
    {"ssid": "Cafe <b>Free</b> & \"WiFi\"", "signal": 35, "security": "", "band": "2.4", "chan": 11, "known": False},
    {"ssid": "Neighbour 5G", "signal": 18, "security": "WPA3", "band": "5", "chan": 36, "known": False},
]


class MockPi:
    def __init__(self, sd=None, music=None, simulator_html=None, sdcard=None, gallery=None, hostname="sticktime",
                 keys_h=KEYS_H, scan_busy=1):
        self.lock = threading.RLock()
        self.tmp = tempfile.mkdtemp(prefix="sticktime-mock-")
        self.sd = pathlib.Path(sd) if sd else self._demo_sd()
        self.music = pathlib.Path(music) if music else self._demo_music()
        self.editor_html = simulator_html          # str or None
        self.sdcard = pathlib.Path(sdcard) if sdcard else None
        self.gallery = gallery                     # bytes, a path or None
        self.keys = load_keys(keys_h)
        self.keymap = {k["key"]: k for k in self.keys}
        self.file_values = {"wifi_ssid": "Home", "wifi_password": "hunter22", "hotspot_password": "sticktime", "hostname": hostname}
        self.hostnames = [hostname.lower(), ""]          # as web.c: the name at start and the latest one saved
        self.unknown = {"my_note": "kept"}
        self.warnings = "line 41: no '='\n"
        self.requests = []                         # (method, path, headers, body bytes, status)
        self.keys_sent = []
        self.reloads = 0
        self.t0 = time.time()
        self.boot = time.time()
        self.down_until = 0.0
        self.version, self.game_version = "0.1.0", "1.7.2"
        self.scan_busy = scan_busy                 # how many scans answer 503 Busy first
        self.last_scan = {"networks": SCAN_NETWORKS, "at": int(time.time() * 1000)}   # ?cached=1 (None: no scan yet)
        self.update = {"state": "idle", "message": "", "progress": 0,
                       "game": {"installed": "1.7.2", "latest": "1.7.2", "staged": "", "notes": ""},
                       "app": {"installed": "0.1.0", "latest": "0.1.0", "notes": ""}, "checked_at": 0, "error": ""}
        self._upd_job = None
        self.update_start_delay = 0.0
        self.vtx_hold = None                       # an Event: the next GET /api/vtx is built, then sent once it is set
        self.vtx_held = False
        self.song_now = 0
        self.wifi_applied = None
        self.log_lines = [f"[{i * 0.37:8.3f}] " + t for i, t in enumerate([
            "sticktime-pi 0.1.0 starting", "settings: /boot/firmware/sticktime.txt", "video: Composite-1 720x480i 59.94 Hz, NTSC 4:3",
            "lua: StickTime.lua loaded (812 KB)", "web: listening on :80", "wifi: ELRS 10.0.0.1 beacon", "wifi: streaming 250 Hz"])]
        self.shots = [png(320, 240, p) for p in (0.0, 1.5, 3.0)]
        self.shot_n = 0
        self.fail = {}                             # route prefix -> (status, name, message[, times]): forced errors for tests
        self.restart_s = 0.8
        self.game_error = ""
        self.free_mb = 312
        self.vtx_table = None                      # a loaded VTX table (the Betaflight JSON as a dict), None = built in
        self.vtx_force = None                      # tests: {"state": ..., "message": ...} instead of "found and set"
        self.stick_move = None                     # tests: 0 = the WiFi radio's sticks rest, 1-16 = that channel sweeps
        self.stick_amp = 1000                      # (how far it sweeps, -1024..1024 units)

    # demo folders when none are given
    def _demo_sd(self):
        sd = pathlib.Path(self.tmp) / "sd"
        tools = sd / "SCRIPTS" / "TOOLS"
        (tools / "StickTimeTracks").mkdir(parents=True)
        (tools / "StickTimeGhosts").mkdir(parents=True)
        (tools / "StickTime.lua").write_text('local toolName = "TNS|StickTime|TNE"\n-- StickTime v1.7.2\n'
                                             "local function run() return 0 end\nreturn { run = run }\n")
        # the B&W and Lite editions, as the image installs them (the Lite's save inside its folder)
        (tools / "StickTimeBW.lua").write_text('local toolName = "TNS|StickTime BW|TNE"\n-- StickTime BW v1.7.2\nreturn {}\n')
        (tools / "StickTimeLite.lua").write_text('local toolName = "TNS|StickTime Lite|TNE"\n-- StickTime Lite v1.7.2\nreturn {}\n')
        for ed in ("StickTimeBW", "StickTimeLite"):
            (tools / ed).mkdir()
            (tools / ed / "core.lua").write_text("-- %s core\n" % ed)
        (tools / "StickTimeLite" / "data.txt").write_bytes(b"lite save\n")
        (tools / "StickTime.dat").write_bytes(b"\x01\x02save")
        (tools / "StickTimeTracks" / "c1.txt").write_text(
            "MultiGP UTT 2017\n1 285132 17 0 0 0 1 0 0 0 95.34 1 0 0 -23.84 47.67 1 180 0 -47.67 95.34 1 0 0 -23.84 -23.84 1 90 0\n")
        return sd

    def _demo_music(self):
        m = pathlib.Path(self.tmp) / "music"
        m.mkdir()
        (m / "Intro.mp3").write_bytes(b"ID3" + b"\0" * 2048)
        (m / "Racing Theme.ogg").write_bytes(b"OggS" + b"\0" * 4096)
        return m

    def close(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ---- settings
    def value(self, key):
        if key in self.file_values:
            return self.file_values[key]
        k = self.keymap.get(key)
        return k["default"] if k else self.unknown.get(key, "")

    def settings_json(self):
        values = {}
        for k in self.keys:
            if k["kind"] == "secret":
                values[k["key"]] = ""
                values[k["key"] + "_set"] = bool(self.value(k["key"]))
            else:
                values[k["key"]] = self.value(k["key"])
        for k, v in self.unknown.items():
            values.setdefault(k, v)
        return {"values": values, "keys": self.keys, "warnings": self.warnings, "exists": True}

    def post_settings(self, body):
        new, _ = parse_settings(body)
        fields, todo = {}, {}
        for key, v in new.items():
            if key.endswith("_clear") and key[:-6] in self.keymap and self.keymap[key[:-6]]["kind"] == "secret":
                continue
            if not re.fullmatch(r"[a-z0-9_]{1,64}", key):
                fields[key] = "not a valid key name"
                continue
            k = self.keymap.get(key)
            if k is None:
                todo[key] = v
                continue
            if k["kind"] == "secret":
                if v == "" and new.get(key + "_clear") != "1":
                    continue                       # an empty secret leaves it as it is
                todo[key] = v
                continue
            if v == "":
                todo[key] = None                   # an empty value clears the key: back to the default
                continue
            err = validate(k, v)
            if err:
                fields[key] = err
            else:
                todo[key] = normalize(k, v)
        if fields:
            raise ApiError(400, "BadRequest", "Some values are not valid: nothing was saved.", fields=fields)
        saved, reboot, restart = [], False, False
        with self.lock:
            uart_before = self.vtx_uart()
            for key, v in todo.items():
                k = self.keymap.get(key)
                if k is None:
                    self.unknown[key] = v or ""
                elif v is None:
                    self.file_values.pop(key, None)
                else:
                    self.file_values[key] = v
                    if key == "hostname":
                        self.hostnames[1] = v.lower()
                saved.append(key)
                if k and k["reboot"]:
                    reboot = True
                if k and k["restart"]:
                    restart = True
            uart_after = self.vtx_uart()
            if uart_after and uart_after != uart_before:
                reboot = True                          # as web_api.c: the two-pin mode's UART overlay comes at the next boot
            if restart and self.restart_s:             # the service restarts: the server is gone a moment
                self.down_until = time.time() + self.restart_s
                self.boot = self.down_until
        return {"ok": True, "saved": saved, "reboot": reboot, "restart": restart}

    # ---- VTX control (docs/WEB_API.md /api/vtx): the VTX takes every setting at once
    VTX_BANDS = [("BOSCAM_A", "A", [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725]),
                 ("BOSCAM_B", "B", [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866]),
                 ("BOSCAM_E", "E", [5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945]),
                 ("FATSHARK", "F", [5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880]),
                 ("RACEBAND", "R", [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917])]

    def vtx_uart(self):
        port = self.value("vtx_port")
        return port if self.value("vtx") != "off" and port.startswith("uart") else ""

    def vtx_table_now(self):
        if self.vtx_table:
            t = self.vtx_table["vtx_table"]
            bands = [{"name": b.get("name", ""), "letter": b["letter"], "factory": bool(b.get("is_factory_band")),
                      "frequencies": b["frequencies"]} for b in t["bands_list"]]
            return {"source": "file", "error": "", "description": self.vtx_table.get("description", ""),
                    "channels": max(len(b["frequencies"]) for b in bands), "bands": bands,
                    "powers": [{"value": p["value"], "label": str(p.get("label", p["value"])).strip()} for p in t["powerlevels_list"]]}
        tramp = self.value("vtx") == "tramp"
        powers = [{"value": v, "label": str(v)} for v in (25, 100, 200, 400, 600)] if tramp else \
            [{"value": 14, "label": "25"}, {"value": 20, "label": "100"}, {"value": 26, "label": "400"}]
        return {"source": "default", "error": "", "description": "StickTime Pi default: " + ("Tramp" if tramp else "SmartAudio 2.1"),
                "channels": 8, "powers": powers,
                "bands": [{"name": n, "letter": l, "factory": not tramp, "frequencies": f} for n, l, f in self.VTX_BANDS]}

    def vtx_json(self):
        mode, tbl = self.value("vtx"), self.vtx_table_now()
        band, freq_set = self.value("vtx_band"), int(self.value("vtx_freq") or 0)
        chan, power, pit = int(self.value("vtx_channel") or 1), int(self.value("vtx_power") or 0), self.value("vtx_pit") == "on"
        send_only = self.value("vtx_send_only") == "on"
        v = {"enabled": mode != "off", "state": "off", "message": "VTX control is off.", "error": "", "mode": mode, "port": self.value("vtx_port"),
             "device": "", "single_pin": True, "send_only": send_only, "needs_reboot": False, "line_stats": "", "protocol": "", "version": "", "line": "", "baud": 0,
             "found": False, "in_sync": False, "freq": 0, "freq_mode": False, "band": "", "channel": 0, "power": None, "pit": False,
             "pit_at_boot": False, "locked": False, "temp_c": None, "limits": None,
             "settings": {"band": band, "channel": chan, "freq": freq_set, "power": power, "pit": pit}, "table": tbl,
             "stats": {"sent": 12, "replies": 11, "timeouts": 1, "bad_frames": 0, "echo_ok": 12, "echo_missing": 0, "echo_bad": 0}, "age_ms": 120}
        if mode == "off":
            return v
        tramp = mode == "tramp"
        port = self.value("vtx_port")
        pins = {"gpio4": 7, "gpio17": 11, "gpio22": 15, "gpio23": 16, "gpio27": 13}
        device = f"GPIO{port[4:]}, pin {pins.get(port, 0)} (single pin)" if port.startswith("gpio") else f"/dev/ttyAMA{port[4:]} ({port})"
        v["single_pin"] = port.startswith("gpio")
        if v["single_pin"]:
            v["line_stats"] = "40 frames sent; TX edges late by 6 us on average, 31 us at most (last frame 12 us); 0 sent again; RX 300 bytes, 0 framing errors, 0 glitches"
        v.update({"device": device, "protocol": "tramp" if tramp else "smartaudio", "version": "" if tramp else "2.1",
                  "line": "9600 baud 8N1" if tramp else "4800 baud 8N2, a 0x00 before each frame", "baud": 9600 if tramp else 4800})
        if send_only:
            v.update({"state": "send_only", "protocol": "tramp" if tramp else "smartaudio", "version": "",
                      "message": "Sent (send-only). The VTX's answers aren't read, so this page can't show what it is set to."})
            return v
        if self.vtx_force:
            v.update(self.vtx_force)
            return v
        b = next((x for x in tbl["bands"] if x["letter"] == band), None)
        f = freq_set or (b["frequencies"][chan - 1] if b and chan <= len(b["frequencies"]) else 0) or 5865
        bl, ch = (band, chan) if (b and not freq_set) else ("", 0)
        lvl = power if 0 < power <= len(tbl["powers"]) else 1
        pw = tbl["powers"][lvl - 1]
        label = pw["label"]
        text = f"{bl}{ch} {f} MHz" if bl else f"{f} MHz"
        v.update({"state": "ready", "found": True, "in_sync": True, "freq": f, "freq_mode": bool(freq_set) or tramp, "band": bl, "channel": ch,
                  "power": {"level": lvl, "label": label, "value": pw["value"], "mw": int(label) if label.isdigit() else 0}, "pit": pit,
                  "temp_c": 33 if tramp else None, "limits": {"min_freq": 5600, "max_freq": 5950, "max_power": 600} if tramp else None,
                  "message": f"{'Tramp' if tramp else 'SmartAudio 2.1'}: {text}, {label} mW, pit mode {'on' if pit else 'off'}."})
        return v

    def vtx_status_line(self):
        v = self.vtx_json()
        if v["state"] == "off":
            return {"state": "off", "text": "off"}
        if v["found"]:
            where = (f"{v['band']}{v['channel']} " if v["band"] else "") + f"{v['freq']} MHz"
            proto = "Tramp" if v["protocol"] == "tramp" else "SmartAudio " + v["version"]
            return {"state": v["state"], "text": f"{proto} · {where} · {v['power']['label']} mW" + (" · pit mode" if v["pit"] else "")}
        return {"state": v["state"], "text": v["state"].replace("_", " ")}

    def vtx_post(self, text):
        new, _ = parse_settings(text)
        tbl, fields, todo = self.vtx_table_now(), {}, {}
        get = lambda n: new.get(n, new.get("vtx_" + n))  # noqa: E731
        band = None
        if get("band") is not None:
            band = get("band").upper()
            letters = [b["letter"] for b in tbl["bands"]]
            if band and band not in [x.upper() for x in letters]:
                fields["band"] = f"isn't a band of the VTX table ({', '.join(letters)})"
            else:
                todo["vtx_band"] = band
                if band and get("freq") is None:
                    todo["vtx_freq"] = "0"
        if get("channel") is not None:
            c = get("channel")
            if not c.isdigit() or not 1 <= int(c) <= tbl["channels"]:
                fields["channel"] = f"must be a channel from 1 to {tbl['channels']}"
            else:
                todo["vtx_channel"] = str(int(c))
        if get("freq") is not None:
            f = get("freq")
            if not f.isdigit() or (int(f) and not 4900 <= int(f) <= 6200):
                fields["freq"] = "must be 0 (band and channel) or 4900 to 6200 MHz"
            else:
                todo["vtx_freq"] = str(int(f))
        if get("power") is not None:
            p = get("power")
            if not p.isdigit() or int(p) > len(tbl["powers"]):
                fields["power"] = f"must be a level from 1 to {len(tbl['powers'])} (0 = leave it)"
            else:
                todo["vtx_power"] = str(int(p))
        if get("pit") is not None:
            p = get("pit").lower()
            if p not in BOOL_WORDS:
                fields["pit"] = "must be on or off"
            else:
                todo["vtx_pit"] = "on" if p in ("on", "yes", "true", "1") else "off"
        if fields:
            raise ApiError(400, "BadRequest", "Some values are not valid: nothing was saved.", fields=fields)
        if not todo:
            raise ApiError(400, "BadRequest", "Nothing to set: send band, channel, freq, power or pit (key = value lines).")
        with self.lock:
            saved = [k for k, v in todo.items() if self.value(k) != v]
            self.file_values.update(todo)
        return {"ok": True, "saved": saved}

    def vtx_put_table(self, body):
        try:
            j = json.loads(body.decode("utf-8-sig"))
            t = j["vtx_table"]
            ok = isinstance(t["bands_list"], list) and t["bands_list"] and isinstance(t["powerlevels_list"], list) and t["powerlevels_list"]
            ok = ok and all(isinstance(b.get("letter"), str) and len(b["letter"]) == 1 and isinstance(b.get("frequencies"), list) and b["frequencies"]
                            for b in t["bands_list"]) and all(isinstance(p.get("value"), int) for p in t["powerlevels_list"])
        except (ValueError, KeyError, TypeError, AttributeError, UnicodeDecodeError):
            ok = False
        if not ok:
            raise ApiError(400, "BadRequest", "This isn't a VTX table: a Betaflight VTX table file is a JSON object with vtx_table.")
        self.vtx_table = j
        return {"ok": True, "description": j.get("description", ""), "bands": len(t["bands_list"]),
                "channels": max(len(b["frequencies"]) for b in t["bands_list"]), "powers": len(t["powerlevels_list"])}

    def vtx_table_file(self):
        t = self.vtx_table_now()
        return {"description": t["description"], "version": "1.0", "vtx_table": {
            "bands_list": [{"name": b["name"], "letter": b["letter"], "is_factory_band": b["factory"], "frequencies": b["frequencies"]} for b in t["bands"]],
            "powerlevels_list": t["powers"]}}

    # ---- live status
    def uptime_ms(self):
        return ms(time.time() - self.boot)

    def sticks(self):
        t = time.time() - self.t0
        sources = []
        if self.stick_move is None:                 # the demo: every channel sweeps
            wifi = [int(1024 * math.sin(t * (0.7 + i * 0.13) + i)) for i in range(16)]
            wifi[4] = 1024 if int(t / 4) % 2 else -1024
        else:                                       # tests: sticks centred, throttle and CH5 low, a little noise,
            wifi = [int(30 * math.sin(t * 7 + i)) for i in range(16)]     # and channel stick_move (1-16) sweeping
            wifi[2], wifi[4] = -1000, -1024
            if self.stick_move:
                wifi[self.stick_move - 1] = int(self.stick_amp * math.sin(t * 7))
        sources.append({"kind": "wifi", "name": "ELRS 10.0.0.1", "live": True, "rate_hz": round(249 + math.sin(t) * 1.5, 1), "nch": 16, "ch": wifi})
        usb = [int(600 * math.cos(t * 0.5 + i)) for i in range(8)] + [0] * 8
        sources.append({"kind": "usb", "name": "EdgeTX RM TX16S Joystick", "live": False, "rate_hz": 0.0, "nch": 8, "ch": usb})
        # as input.c: a fixed source kind ignores the others (the wanted one heard but silent: shown, not live)
        want = self.value("input").lower()
        active = 0 if want in ("auto", "wifi", "") else next((i for i, s in enumerate(sources) if s["kind"] == want), -1)
        live = active >= 0 and sources[active]["live"]
        ch = sources[active]["ch"] if live else None
        chans = stick_channels(self.value("stick_channels")) or []
        if chans:
            idx = {c: chans[i] - 1 for i, c in enumerate("AETR")}
        else:
            order = self.value("channel_order").upper()
            idx = {c: order.index(c) for c in "AETR"} if sorted(order) == sorted("AETR") else {"A": 0, "E": 1, "T": 2, "R": 3}
        st = ({"ail": ch[idx["A"]], "ele": ch[idx["E"]], "thr": ch[idx["T"]], "rud": ch[idx["R"]], "pause": ch[4]} if ch
              else {"ail": 0, "ele": 0, "thr": -1024, "rud": 0, "pause": None})
        kind = sources[active]["kind"] if active >= 0 else "none"
        return {"sources": sources, "active": active, "kind": kind, "live": live, "sticks": st}

    def status(self):
        t = time.time() - self.t0
        s = self.sticks()
        state = [1, 2, 3, 4, 4, 4, 7, 4, 8][int(t / 5) % 9]
        net_mode = "hotspot" if self.wifi_applied == "hotspot" else "client"
        return {
            "version": self.version, "game_version": self.game_version, "uptime_ms": self.uptime_ms(),
            "fps": round(59.94 - abs(math.sin(t)) * 0.3, 2), "frame_ms": {"avg": 16.68, "max": round(17.1 + abs(math.sin(t * 3)), 2)},
            "run_ms": {"avg": round(3.1 + math.sin(t) * 0.2, 2), "p95": 4.0}, "present_ms": 0.6, "latency_ms": round(9.8 + math.sin(t / 2), 1),
            "frames": int(t * 60), "missed": 3,
            "game": {"state": state, "state_name": STATE_NAMES[state], "loaded": True, "error": self.game_error, "lua_kb": 812,
                     "script": self.value("script")},
            "video": {"desc": "Composite-1 720x480i 59.94 Hz, NTSC 4:3, render 640x480", "render_w": 640, "render_h": 480},
            "input": {"active": s["active"], "kind": s["kind"], "live": s["live"], "sticks": s["sticks"],
                      "sources": [{k: v for k, v in src.items() if k != "ch"} for src in s["sources"]]},
            "wifi_joystick": {"module": "ELRS 10.0.0.1", "state": "streaming", "rate_hz": s["sources"][0]["rate_hz"], "dropped": 0, "gaps": 2, "gap_max_ms": 840},
            "audio": {"device": "jack", "ok": True, "music": self.songs_list()[self.song_now % max(1, len(self.songs_list()))]["name"] if self.songs_list() else "Song 3 (generated)"},
            "network": {"ip": "192.168.1.23", "iface": "wlan0", "ssid": self.value("wifi_ssid"), "mode": net_mode, "hostname": self.value("hostname")},
            "system": {"cpu_temp": round(51.2 + math.sin(t / 7) * 2, 1), "throttled": "0x0"},
            "vtx": self.vtx_status_line(),
        }

    # ---- songs
    def songs_list(self):
        out = []
        for p in sorted(self.music.iterdir(), key=lambda p: p.name.lower()):
            if p.is_file() and p.suffix.lower() in (".mp3", ".ogg", ".wav") and not p.name.startswith(TMP_PREFIX):
                st = p.stat()
                out.append({"name": p.name, "size": st.st_size, "mtime": ms(st.st_mtime)})
        return out

    # ---- updates: a little state machine on a thread
    def start_update(self, what, kind):
        with self.lock:
            if self.update["state"] in ("checking", "downloading", "installing"):
                raise ApiError(409, "Busy", "An update is running already.")
        th = threading.Thread(target=self._run_update, args=(what, kind), daemon=True)
        th.start()

    def _run_update(self, what, kind):
        # the real updater is a process started in the background: until it writes its state, the
        # file still holds the last run's (update_start_delay > 0 shows that)
        if self.update_start_delay:
            time.sleep(self.update_start_delay)
        with self.lock:
            self.update["state"] = "checking" if kind == "check" else "downloading"
            self.update["progress"] = 0
            self.update["error"] = ""
            self.update["message"] = "Checking GitHub for new releases…" if kind == "check" else f"Downloading the {what} update…"
        steps = 6
        for i in range(steps):
            time.sleep(0.25)
            with self.lock:
                self.update["progress"] = (i + 1) / steps
                if kind != "check" and i == steps // 2:
                    self.update["state"] = "installing"
                    self.update["message"] = f"Installing the {what} update…"
        with self.lock:
            u = self.update
            if kind == "check":
                u["game"].update(latest="1.7.3", staged="1.7.3", notes="### New in 1.7.3\n- Faster gate rush\n- A new track: Riverside\n")
                u["app"].update(latest="0.2.0", notes="### New in 0.2.0\n- Bluetooth gamepads\n- The web page has a Sound tab\n")
                u.update(state="idle", message="Updates available: the game 1.7.3 and StickTime Pi 0.2.0.", checked_at=ms(time.time()))
            elif kind == "apply":
                if what == "game":
                    u["game"]["prev"] = u["game"]["installed"]
                    u["game"]["installed"] = u["game"]["latest"]
                    self.game_version = u["game"]["installed"]
                else:
                    u["app"]["prev"] = u["app"]["installed"]
                    u["app"]["installed"] = u["app"]["latest"]
                    self.version = u["app"]["installed"]
                    self.down_until = time.time() + 1.5      # the service restarts
                    self.boot = self.down_until
                u.update(state="done", message=f"Installed the {what} update.")
            else:
                prev = u[what].pop("prev", None)
                if prev:
                    u[what]["installed"] = prev
                    if what == "game":
                        self.game_version = prev
                    else:
                        self.version = prev
                u.update(state="done", message=f"Rolled the {what} back.")

    # ---- the SD folder
    def resolve(self, comps, create_last=False):
        """-> (os path, canonical relative path, exists). Case-insensitive per component, no links."""
        cur, canon = str(self.sd), []
        for i, c in enumerate(comps):
            if not os.path.isdir(cur):
                raise ApiError(409, "TypeMismatchError", f"{'/'.join(canon) or 'The root'} is a file, not a folder.")
            nxt = os.path.join(cur, c)
            if not os.path.lexists(nxt):
                low = c.lower()
                match = [n for n in os.listdir(cur) if n.lower() == low and not n.startswith(TMP_PREFIX)]
                if match:
                    nxt, c = os.path.join(cur, match[0]), match[0]
                else:
                    if i == len(comps) - 1:
                        return nxt, "/".join(canon + [c]), False
                    raise ApiError(404, "NotFoundError", f"{'/'.join(comps[:i + 1])} was not found.")
            if os.path.islink(nxt):
                raise ApiError(403, "NoModificationAllowedError", "Links are not followed.")
            cur, canon = nxt, canon + [c]
        return cur, "/".join(canon), True

    def policy(self, canon, kind):
        mode = self.value("sd_write").lower()
        low = canon.lower()
        dirs_tracks = {"scripts", "scripts/tools", "scripts/tools/sticktimetracks"}
        if mode == "all":
            return
        ok = False
        if mode in ("tracks", "game"):
            ok = (kind == "directory" and low in dirs_tracks) or (kind == "file" and bool(SLOT_RE.fullmatch(low)))
        if mode == "game" and not ok:
            tools = "scripts/tools/"
            rest = low[len(tools):] if low.startswith(tools) else None
            if kind == "directory":
                ok = low in dirs_tracks or rest in ("sticktimeghosts", "sticktimebw", "sticktimelite")
            elif rest is not None:
                parts = rest.split("/")
                ok = (rest in ("sticktime.lua", "sticktimebw.lua", "sticktimelite.lua", "sticktimetracks/readme.txt", "sticktimeghosts/readme.txt")
                      or (len(parts) == 2 and parts[0] in ("sticktimebw", "sticktimelite") and bool(GAME_NAME.fullmatch(parts[1]))
                          and parts[1] != "data.txt"))
        if not ok:
            say = {"off": "This Pi doesn't let the web page change its SD card (sd_write = off in sticktime.txt).",
                   "tracks": "This Pi lets the track editor write only its track slots (c1.txt to c8.txt).",
                   "game": "This Pi lets the web page write only the track slots (c1.txt to c8.txt) and the game's "
                           "own files (sd_write = game in sticktime.txt)."}
            raise ApiError(403, "NoModificationAllowedError", say.get(mode, say["off"]))

    def touched(self, canon):
        low = canon.lower()
        if SLOT_RE.fullmatch(low) or low.startswith("scripts/tools/sticktime"):
            self.reloads += 1

    def sd_request(self, method, raw, query, body):
        comps = split_path(raw)
        q = urllib.parse.parse_qs(query, keep_blank_values=True)
        if method == "GET":
            p, canon, exists = self.resolve(comps)
            if not exists:
                raise ApiError(404, "NotFoundError", f"{canon} was not found.")
            name = os.path.basename(p) if comps else ""
            if "stat" in q:
                return 200, stat_json(p, name)
            if "list" in q:
                if not os.path.isdir(p):
                    raise ApiError(409, "TypeMismatchError", f"{canon} is a file, not a folder.")
                ents = [stat_json(os.path.join(p, n), n) for n in sorted(os.listdir(p)) if not n.startswith(TMP_PREFIX)
                        and not os.path.islink(os.path.join(p, n))]
                return 200, {"entries": ents}
            if os.path.isdir(p):
                raise ApiError(409, "TypeMismatchError", f"{canon or 'The root'} is a folder, not a file.")
            if os.path.getsize(p) > 16 << 20:
                raise ApiError(413, "NotReadableError", f"{canon} is too big to read here.")
            return 200, ("file", p)
        if not comps:
            raise ApiError(403, "NoModificationAllowedError", "The SD folder itself can't be changed.")
        if method == "PUT":
            p, canon, exists = self.resolve(comps)
            if exists and os.path.isdir(p):
                raise ApiError(409, "TypeMismatchError", f"{canon} is a folder.")
            parent = os.path.dirname(p)
            self.policy(canon, "file")
            limit = 64 << 10 if SLOT_RE.fullmatch(canon.lower()) else 2 << 20
            if len(body) > limit:
                raise ApiError(413, "QuotaExceededError", f"That file is too big for {canon} ({limit // 1024} KB at most).")
            tmp = os.path.join(parent, f"{TMP_PREFIX}{os.getpid()}-{threading.get_ident()}")
            with open(tmp, "wb") as f:
                f.write(body)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, p)
            self.touched(canon)
            return 200, {"size": len(body), "mtime": ms(os.path.getmtime(p))}
        if method == "POST":
            kind = (q.get("create") or [""])[0]
            if kind not in ("file", "directory"):
                raise ApiError(400, "BadRequest", "POST /api/sd/ needs ?create=file or ?create=directory.")
            p, canon, exists = self.resolve(comps)
            if exists:
                if os.path.isdir(p) != (kind == "directory"):
                    raise ApiError(409, "TypeMismatchError", f"{canon} is a {'folder' if os.path.isdir(p) else 'file'}.")
                return 200, stat_json(p, os.path.basename(p))
            self.policy(canon, kind)
            if kind == "directory":
                os.mkdir(p)
            else:
                open(p, "xb").close()
                self.touched(canon)
            return 201, stat_json(p, os.path.basename(p))
        if method == "DELETE":
            p, canon, exists = self.resolve(comps)
            if not exists:
                raise ApiError(404, "NotFoundError", f"{canon} was not found.")
            isdir = os.path.isdir(p)
            self.policy(canon, "directory" if isdir else "file")
            if isdir:
                if os.listdir(p):
                    rec = (q.get("recursive") or [None])[0]
                    if rec is None or rec.lower() not in ("", "1", "on", "true", "yes"):
                        raise ApiError(409, "InvalidModificationError", f"{canon}: the folder isn't empty.")
                    if self.value("sd_write").lower() != "all":
                        raise ApiError(403, "NoModificationAllowedError",
                                       "This Pi lets the web page delete only empty folders (sd_write = all allows more).")
                shutil.rmtree(p)
            else:
                os.unlink(p)
                self.touched(canon)
            return 204, None
        raise ApiError(405, "MethodNotAllowed", f"{method} is not allowed here.")


# ---------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    server_version = "sticktime-pi-mock"
    protocol_version = "HTTP/1.1"
    pi: MockPi = None

    def log_message(self, fmt, *args):
        if os.environ.get("MOCK_VERBOSE"):
            sys.stderr.write("mock: " + (fmt % args) + "\n")

    # helpers
    def send(self, status, body=b"", ctype="application/json; charset=utf-8", headers=None):
        self.send_response(status)
        if status != 204:
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", (headers or {}).pop("Cache-Control", "no-store"))
        self.send_header("X-Content-Type-Options", "nosniff")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD" and status != 204:
            self.wfile.write(body)
        self._status = status

    def json(self, status, obj):
        self.send(status, json.dumps(obj).encode())

    def error(self, e):
        j = {"error": e.name, "message": e.message}
        j.update(e.extra)
        self.json(e.status, j)

    def host_ok(self):
        host = (self.headers.get("Host") or "").strip().lower()
        if host.startswith("["):
            h = host[1:host.find("]")] if "]" in host else ""
        else:
            h = host.rsplit(":", 1)[0] if host.count(":") == 1 else host
        if h == "localhost" or any(n and h in (n, n + ".local") for n in self.pi.hostnames):
            return True
        try:
            ipaddress.ip_address(h)
            return True
        except ValueError:
            return False

    def do_GET(self):
        self.handle_any("GET")

    def do_HEAD(self):
        self.handle_any("GET")

    def do_POST(self):
        self.handle_any("POST")

    def do_PUT(self):
        self.handle_any("PUT")

    def do_DELETE(self):
        self.handle_any("DELETE")

    def handle_any(self, method):
        pi = self.pi
        self._status = 0
        if time.time() < pi.down_until:               # rebooting / restarting: no answer at all
            self.close_connection = True
            return
        url = urllib.parse.urlsplit(self.path)
        path, query = url.path, url.query
        body = b""
        if method in ("POST", "PUT", "DELETE"):
            if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
                self.json(411, {"error": "BadRequest", "message": "Send a Content-Length."})
                return
            n = int(self.headers.get("Content-Length") or 0)
            if n > (70 << 20):
                self.json(413, {"error": "PayloadTooLarge", "message": "That is too big."})
                self.close_connection = True
                return
            body = self.rfile.read(n) if n else b""
        try:
            if not path.startswith("/__mock/"):
                if not self.host_ok():
                    raise ApiError(421, "Forbidden", "This server answers only to its own name or IP address.")
                if method != "GET":
                    if self.headers.get("X-StickTime") != "1":
                        raise ApiError(403, "Forbidden", "Changes need the X-StickTime: 1 header.")
                    origin = self.headers.get("Origin")
                    if origin and urllib.parse.urlsplit(origin).netloc.lower() != (self.headers.get("Host") or "").lower():
                        raise ApiError(403, "Forbidden", "Changes from another site are refused.")
            self.route(method, path, query, body)
        except ApiError as e:
            self.error(e)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            return
        except Exception as e:  # a bug in the mock: say so
            import traceback
            traceback.print_exc()
            self.json(500, {"error": "Internal", "message": f"mock: {e!r}"})
        finally:
            if not path.startswith("/__mock/") and path != "/api/sticks/stream":
                with pi.lock:
                    pi.requests.append({"method": method, "path": path, "query": query, "body": body,
                                        "headers": {k.lower(): v for k, v in self.headers.items()}, "status": self._status})

    def static(self, file, ctype=None):
        data = file if isinstance(file, bytes) else pathlib.Path(file).read_bytes()
        etag = '"' + hashlib.sha1(data).hexdigest()[:16] + '"'
        if self.headers.get("If-None-Match") == etag:
            self.send(304, b"", headers={"ETag": etag, "Cache-Control": "no-cache"})
            return
        self.send(200, data, ctype or "application/octet-stream", {"ETag": etag, "Cache-Control": "no-cache"})

    def route(self, method, path, query, body):
        pi = self.pi
        text = body.decode("utf-8", "replace")
        for prefix, f in list(pi.fail.items()):
            if path.startswith(prefix):
                with pi.lock:
                    if len(f) > 3:                     # only that many times
                        if f[3] <= 1:
                            pi.fail.pop(prefix, None)
                        else:
                            pi.fail[prefix] = (*f[:3], f[3] - 1)
                raise ApiError(*f[:3])
        # ---- the mock's own controls
        if path == "/__mock/requests":
            with pi.lock:
                self.json(200, [{**r, "body": r["body"].decode("utf-8", "replace")} for r in pi.requests])
            return
        if path == "/__mock/reset" and method == "POST":
            with pi.lock:
                pi.requests.clear()
            self.json(200, {"ok": True})
            return
        # ---- pages
        if method == "GET" and (path == "/" or path == "/index.html"):
            return self.static(WEB / "index.html", TYPES[".html"])
        if path == "/favicon.ico":
            return self.send(204)
        if path == "/editor":
            return self.send(301, b"", headers={"Location": "/editor/"})
        if method == "GET" and path in ("/editor/", "/editor/index.html"):
            if pi.editor_html is None:
                raise ApiError(404, "NotFound", "No editor page (start the mock with --simulator).")
            return self.static(pi.editor_html.encode("utf-8"), TYPES[".html"])
        if method == "GET" and path == "/editor/pi-sd.js":
            return self.static(WEB / "pi-sd.js", TYPES[".js"])
        if method == "GET" and path == "/editor/tracks/index.json":
            if pi.gallery is None:
                raise ApiError(404, "NotFound", "No gallery.")
            data = pi.gallery if isinstance(pi.gallery, bytes) else pathlib.Path(pi.gallery).read_bytes()
            return self.static(data, TYPES[".json"])
        if method == "GET" and path.startswith("/editor/sdcard/"):
            rel = urllib.parse.unquote(path[len("/editor/sdcard/"):])
            if pi.sdcard is None or ".." in rel.split("/"):
                raise ApiError(404, "NotFound", "Nothing is staged.")
            f = pi.sdcard / rel
            if not f.is_file():
                raise ApiError(404, "NotFound", "Not staged.")
            return self.static(f, TYPES.get(f.suffix.lower()))
        if path.startswith("/api/"):
            return self.api(method, path[5:], query, body, text)
        # other static files from web/
        if method == "GET":
            rel = urllib.parse.unquote(path.lstrip("/"))
            f = (WEB / rel).resolve()
            if WEB in f.parents and f.is_file() and "tests" not in f.relative_to(WEB).parts:
                return self.static(f, TYPES.get(f.suffix.lower()))
        raise ApiError(404, "NotFound", f"{path} was not found.")

    def api(self, method, path, query, body, text):
        pi = self.pi
        get, post = method == "GET", method == "POST"
        if path.startswith("sd/") or path == "sd":
            st, out = pi.sd_request(method, path[3:], query, body)
            if isinstance(out, tuple):
                data = pathlib.Path(out[1]).read_bytes()
                mt = os.path.getmtime(out[1])
                self.send(200, data, "application/octet-stream", {"X-Mtime-Ms": str(ms(mt)), "Last-Modified": email.utils.formatdate(mt, usegmt=True)})
            elif out is None:
                self.send(st)
            else:
                self.json(st, out)
            return
        if get and path == "status":
            return self.json(200, pi.status())
        if get and path == "sticks":
            return self.json(200, pi.sticks())
        if get and path == "sticks/stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            try:
                for _ in range(30 * 120):
                    self.wfile.write(b"data: " + json.dumps(pi.sticks()).encode() + b"\n\n")
                    self.wfile.flush()
                    time.sleep(0.033)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
                pass
            return
        if get and path == "log":
            return self.send(200, ("\n".join(pi.log_lines) + "\n").encode(), "text/plain; charset=utf-8")
        if get and path == "screenshot.png":
            pi.shot_n += 1
            return self.send(200, pi.shots[pi.shot_n % len(pi.shots)], "image/png")
        if path == "settings":
            if get:
                return self.json(200, pi.settings_json())
            if post:
                return self.json(200, pi.post_settings(self.form_or_text(text)))
        if post and path == "game/reload":
            pi.reloads += 1
            return self.json(200, {"ok": True})
        if post and path == "key":
            k = text.strip()
            if k not in KEYS_ALLOWED:
                raise ApiError(400, "BadRequest", "The key must be enter, exit, next, prev, inc, dec, nextpage, prevpage, menu, up or down.")
            pi.keys_sent.append(k)
            return self.json(200, {"ok": True})
        if get and path == "tools":
            tools = []
            d = pi.sd / "SCRIPTS" / "TOOLS"
            for p in sorted(d.iterdir()) if d.is_dir() else []:
                if p.is_file() and p.suffix.lower() == ".lua":
                    # the name in its first KB's TNS|...|TNE, else the file's (as web_api.c)
                    m = re.search(r"TNS\|([^\n]+?)\|TNE", p.read_bytes()[:1024].decode("utf-8", "replace"))
                    tools.append({"path": "/SCRIPTS/TOOLS/" + p.name, "name": m.group(1) if m else p.stem})
            return self.json(200, {"current": pi.value("script"), "tools": tools})
        if get and path == "songs":
            return self.json(200, {"songs": pi.songs_list(), "free_mb": pi.free_mb})
        if path.startswith("songs/"):
            try:
                name = urllib.parse.unquote(path[6:], errors="strict")
                check_component(name, 120)
                if name.startswith(".") or DEVICE.fullmatch(name) or name.endswith(".tmp~"):
                    raise ApiError(400, "BadRequest", "")
            except (ApiError, UnicodeDecodeError):
                raise ApiError(400, "BadRequest", "That song name isn't allowed (120 bytes at most, no / \\ : * ? \" < > |).")
            if not re.search(r"\.(mp3|ogg|wav)$", name, re.I):
                raise ApiError(400, "BadRequest", "Songs must be .mp3, .ogg or .wav files.")
            match = [x for x in pi.music.iterdir() if x.name.lower() == name.lower()]
            f = match[0] if match else pi.music / name
            if get:
                if not f.is_file():
                    raise ApiError(404, "NotFound", "There is no such song.")
                return self.static(f, TYPES.get(f.suffix.lower()))
            if method == "PUT":
                if len(body) > 64 << 20:
                    raise ApiError(413, "PayloadTooLarge", "The song is too big (64 MB at most).")
                if f.is_dir():
                    raise ApiError(409, "TypeMismatchError", "A folder has that name.")
                tmp = pi.music / (TMP_PREFIX + name)
                tmp.write_bytes(body)
                os.replace(tmp, f)
                return self.json(200, {"size": len(body)})
            if method == "DELETE":
                if not f.is_file():
                    raise ApiError(404, "NotFound", "There is no such song.")
                f.unlink()
                return self.send(204)
        if post and path == "music/next":
            pi.song_now += 1
            return self.json(200, {"ok": True})
        if get and path == "wifi/scan":
            if "cached" in urllib.parse.parse_qs(query, keep_blank_values=True):
                with pi.lock:
                    last = pi.last_scan
                if last is None:
                    raise ApiError(404, "NotFound", "No scan yet.")
                return self.json(200, last)
            with pi.lock:
                if pi.scan_busy > 0:
                    pi.scan_busy -= 1
                    raise ApiError(503, "Busy", "Scanning, try again in a moment.")
                pi.last_scan = {"networks": SCAN_NETWORKS, "at": ms(time.time())}
                return self.json(200, pi.last_scan)
        if post and path == "wifi":
            v, _ = parse_settings(self.form_or_text(text))
            fields = {}
            ssid, pw = v.get("ssid", ""), v.get("password", "")
            if not ssid:
                fields["ssid"] = "the network name is needed"
            elif len(ssid.encode()) > 32:
                fields["ssid"] = "at most 32 bytes"
            if pw and not re.fullmatch(r"[0-9A-Fa-f]{64}", pw) and not 8 <= len(pw.encode()) <= 63:
                fields["password"] = "8 to 63 characters (empty for an open network)"
            if v.get("country") and not re.fullmatch(r"[A-Za-z]{2}", v["country"]):
                fields["country"] = "a two-letter country code (US, GB, DE, HR, ...)"
            if fields:
                raise ApiError(400, "BadRequest", "Some values are not valid: nothing was saved.", fields=fields)
            pi.file_values["wifi_ssid"] = v["ssid"]
            if "password" in v:
                pi.file_values["wifi_password"] = v["password"]
            if v.get("country"):
                pi.file_values["wifi_country"] = v["country"].upper()
            pi.wifi_applied = v["ssid"]
            return self.json(200, {"ok": True, "applied": True})
        if post and path == "wifi/module-ap":
            m = text.strip()
            if m not in ("elrs", "tbs"):
                raise ApiError(400, "BadRequest", "The module must be elrs or tbs.")
            pi.file_values["wifi_module_ip"] = "10.0.0.1" if m == "elrs" else "192.168.4.1"
            pi.wifi_applied = "module-" + m
            return self.json(200, {"ok": True, "applied": True, "module_ip": pi.file_values["wifi_module_ip"]})
        if get and path == "update":
            with pi.lock:
                return self.json(200, pi.update)
        if post and path == "update/check":
            pi.start_update("", "check")
            return self.json(202, {"ok": True})
        if post and path in ("update/apply", "update/rollback"):
            what = text.strip()
            if what not in ("game", "app"):
                raise ApiError(400, "BadRequest", "Say game or app.")
            pi.start_update(what, "apply" if path.endswith("apply") else "rollback")
            return self.json(202, {"ok": True})
        if post and path in ("reboot", "shutdown", "restart"):
            self.json(202, {"ok": True})
            pi.down_until = time.time() + (2.0 if path == "reboot" else 1.0 if path == "restart" else 3600)
            pi.boot = pi.down_until
            return
        if path == "vtx":
            if get:
                j = pi.vtx_json()
                with pi.lock:
                    hold, pi.vtx_hold = pi.vtx_hold, None
                if hold:                                   # a late answer: as the Pi had it when asked
                    pi.vtx_held = True
                    hold.wait(10)
                return self.json(200, j)
            if post:
                return self.json(200, pi.vtx_post(self.form_or_text(text)))
            raise ApiError(405, "MethodNotAllowed", "That method isn't allowed here.")
        if path == "vtx/table":
            if get:
                return self.send(200, json.dumps(pi.vtx_table_file(), indent=2).encode(), "application/json; charset=utf-8",
                                 {"Content-Disposition": 'attachment; filename="vtxtable.json"'})
            if method == "PUT":
                return self.json(200, pi.vtx_put_table(body))
            if method == "DELETE":
                pi.vtx_table = None
                return self.json(200, {"ok": True})
            raise ApiError(405, "MethodNotAllowed", "That method isn't allowed here.")
        if get and path == "system":
            return self.json(200, {"hostname": pi.value("hostname"), "ssh": pi.value("ssh") == "on", "password_set": bool(pi.value("password")),
                                   "disk": {"free_mb": 312, "total_mb": 510}, "os": "Raspberry Pi OS Lite (64-bit) Trixie",
                                   "kernel": "6.12.47+rpt-rpi-v8", "model": "Raspberry Pi 4 Model B Rev 1.5", "version": pi.version})
        raise ApiError(404, "NotFound", f"/api/{path} was not found.")

    def form_or_text(self, text):
        if (self.headers.get("Content-Type") or "").startswith("application/x-www-form-urlencoded"):
            return "\n".join(f"{k} = {quote(v)}" for k, v in urllib.parse.parse_qsl(text, keep_blank_values=True))
        return text


class QuietServer(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1], (ConnectionError, TimeoutError)):   # a browser that went away is no news
            super().handle_error(request, client_address)


def serve(pi, port=0, bind="127.0.0.1"):
    """start the mock on a thread: (server, port)"""
    handler = type("BoundHandler", (Handler,), {"pi": pi})
    srv = QuietServer((bind, port), handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--sd")
    ap.add_argument("--music")
    ap.add_argument("--simulator", help="StickTime's web/simulator.html (patched with tools/patch_editor.py)")
    ap.add_argument("--sdcard", help="a staged release: the OUT folder of sticktime/tools/sd_manifest.py")
    ap.add_argument("--gallery", help="tracks/index.json for the editor's gallery")
    ap.add_argument("--hostname", default="sticktime")
    a = ap.parse_args()
    html = None
    if a.simulator:
        sys.dont_write_bytecode = True
        sys.path.insert(0, str(REPO / "tools"))
        import patch_editor
        html = patch_editor.patch(pathlib.Path(a.simulator).read_bytes().decode("utf-8"))
    pi = MockPi(sd=a.sd, music=a.music, simulator_html=html, sdcard=a.sdcard, gallery=a.gallery, hostname=a.hostname)
    srv, port = serve(pi, a.port, a.bind)
    print(f"mock StickTime Pi on http://localhost:{port}/  (SD folder {pi.sd}, music {pi.music})")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        srv.shutdown()
        pi.close()


if __name__ == "__main__":
    main()
