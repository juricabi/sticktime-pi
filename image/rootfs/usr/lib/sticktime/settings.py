"""sticktime.txt: the Python twin of host/src/settings.c (format in host/src/settings.h).

Same parse, same effective values, same validation messages, same warnings and the same bytes
on save: host/tests/settings_vectors.txt holds both to that (image/tests/test_settings_py.py runs
it here, host/tests/test_settings.c in C). KEYS and TEMPLATE are copies of
host/src/settings_keys.h; host/tests/check_settings_keys.py verifies they match.

Python 3.11+ standard library only. The file is handled as bytes, so a value that isn't UTF-8
survives an edit of another key. Values come back as str decoded with "surrogateescape" (stray
bytes become U+DC80..U+DCFF and encode back to the same bytes).

    s, exists = settings.load("/boot/firmware/sticktime.txt")
    s.get("video"); s.get_bool("ssh", False); s.warnings()
    s.set("wifi_ssid", " My Net "); s.save(path)          # atomic: temp file + fsync + rename
    settings.update_file(path, {"password": ""})          # load, set, save in one go
    settings.validate("hostname", "my-pi")                # None, or why not

Command line (for shell scripts and debugging):
    settings.py [-f FILE] get KEY... | raw KEY | set KEY=VALUE... | warnings | json
    settings.py validate KEY VALUE
"""
import errno
import json
import os
import re
import sys
from typing import NamedTuple

KEY_MAX = 64
FILE_MAX = 1 << 20
BOM = b"\xef\xbb\xbf"
DEFAULT_PATH = "/boot/firmware/sticktime.txt"

STRING, ENUM, INT, NUM, BOOL, SECRET = "string", "enum", "int", "num", "bool", "secret"


class Key(NamedTuple):
    key: str
    kind: str
    default: str
    choices: str        # ENUM: "av|hdmi"; INT/NUM: "min|max"
    live: bool          # applied without a restart of the host
    reboot: bool        # needs a reboot (config.txt / cmdline.txt)
    help: str


# host/src/settings_keys.h, same order (check_settings_keys.py compares them)
KEYS = (
    Key("video", ENUM, "av", "av|hdmi", False, True, "Video output: av (composite on the 3.5 mm jack, to a VTX) or hdmi"),
    Key("tv", ENUM, "ntsc", "ntsc|pal|pal60|ntsc-j|pal-m|pal-n|ntsc-443|secam", False, True, "TV standard of the composite output"),
    Key("aspect", ENUM, "4:3", "4:3|16:9", False, False, "Shape of the picture in your goggles or screen"),
    Key("av_mode", ENUM, "interlaced", "interlaced|progressive", False, False, "Composite: interlaced (480i/576i) or progressive (240p/288p)"),
    Key("hdmi_mode", STRING, "auto", "", False, False, "HDMI mode: auto (1280x720 if offered) or WxH, WxH@Hz"),
    Key("margin", INT, "0", "0|20", False, False, "Black border in percent, for goggles that cut off the edges"),
    Key("fonts", ENUM, "auto", "auto|std|sml|lrg|pc|hd", False, False, "Text size: auto, or the fonts of a radio screen class"),
    Key("show_ip", ENUM, "auto", "auto|on|off", True, False, "Show the web address on the main menu"),
    Key("wifi_ssid", STRING, "", "", False, False, "Your WiFi network (2.4 GHz is best next to 5.8 GHz video)"),
    Key("wifi_password", SECRET, "", "", False, False, "WiFi password (empty for an open network)"),
    Key("wifi_country", STRING, "US", "", False, False, "Two-letter country code for WiFi (US, GB, DE, HR, ...)"),
    Key("hotspot", ENUM, "auto", "auto|on|off", False, False, "Own WiFi network: auto (when no known network is found), on, off"),
    Key("hotspot_ssid", STRING, "StickTime", "", False, False, "Name of the hotspot network"),
    Key("hotspot_password", SECRET, "sticktime", "", False, False, "Hotspot password (8-63 characters)"),
    Key("input", ENUM, "auto", "auto|wifi|usb|bluetooth|gamepad|joystick|keyboard", True, False, "Stick source: auto (the last one that moved) or a fixed one"),
    Key("channel_order", STRING, "AETR", "", True, False, "Which channels are aileron, elevator, throttle, rudder"),
    Key("stick_channels", STRING, "", "", True, False, "Channel of each stick: aileron, elevator, throttle, yaw, e.g. 1 2 3 4 (empty: the channel order)"),
    Key("invert", STRING, "", "", True, False, "Sticks to invert, e.g. ail, thr"),
    Key("pause_channel", INT, "5", "0|16", True, False, "Switch channel that pauses the game in flight (0 = off)"),
    Key("stick_menus", BOOL, "on", "", True, False, "Fly the menus with the sticks"),
    Key("wifi_module_ip", STRING, "", "", True, False, "ELRS/Crossfire WiFi module IP (empty: find it by its broadcast)"),
    Key("wifi_interval_us", INT, "4000", "1000|100000", True, False, "WiFi joystick packet interval in microseconds (4000 = 250 Hz)"),
    Key("bluetooth", BOOL, "on", "", False, False, "Pair ELRS BLE joysticks and gamepads automatically"),
    Key("latency", STRING, "0", "", True, False, "Camera prediction in seconds (STICKTIME_LAT): 0 = off"),
    Key("stick_mode", INT, "2", "1|4", True, False, "Your radio's stick mode (what getStickMode() tells the game)"),
    Key("script", STRING, "/SCRIPTS/TOOLS/StickTime.lua", "", True, False, "The tool script to run (SD path)"),
    Key("stats", BOOL, "off", "", True, False, "Show game and display times next to the fps (game setting Show FPS)"),
    Key("sd_write", ENUM, "game", "game|tracks|all|off", True, False, "What the web page may write in the game's SD folder"),
    Key("sound", BOOL, "on", "", True, False, "Motor sound and beeps"),
    Key("volume", INT, "70", "0|100", True, False, "Sound volume in percent"),
    Key("music", BOOL, "off", "", True, False, "Music"),
    Key("music_volume", INT, "60", "0|100", True, False, "Music volume in percent"),
    Key("music_source", ENUM, "generated", "generated|songs", True, False, "Music: generated live, or your songs (the music folder)"),
    Key("audio_out", ENUM, "auto", "auto|jack|hdmi|off", False, False, "Audio output: the 3.5 mm jack (VTX audio pad), HDMI, or off"),
    Key("audio_mono", BOOL, "off", "", True, False, "Mix the sound to mono (a VTX audio pad has one channel)"),
    Key("vtx", ENUM, "off", "off|auto|smartaudio|tramp", True, False, "VTX control: off, auto (finds SmartAudio or Tramp), smartaudio, tramp"),
    Key("vtx_port", ENUM, "gpio4", "gpio4|gpio17|gpio22|gpio23|gpio27|uart3|uart4|uart5", True, False, "VTX control pin: gpio4 (pin 7) or another single pin; uart3, uart4, uart5: two pins (needs a reboot)"),
    Key("vtx_band", STRING, "", "", True, False, "VTX band: its letter in the VTX table (A, B, E, F, R, ...); empty = leave it"),
    Key("vtx_channel", INT, "1", "1|8", True, False, "VTX channel in the band, 1-8"),
    Key("vtx_freq", INT, "0", "0|6200", True, False, "VTX frequency in MHz instead of band and channel (0 = band and channel)"),
    Key("vtx_power", INT, "0", "0|8", True, False, "VTX power level in the VTX table, 1 = the lowest (0 = leave it)"),
    Key("vtx_pit", BOOL, "off", "", True, False, "VTX pit mode: almost no power (bench, race events)"),
    Key("vtx_send_only", BOOL, "off", "", True, False, "Only send to the VTX (when its answers can't be read): nothing is read back"),
    Key("hostname", STRING, "sticktime", "", False, False, "Network name: http://<hostname>.local"),
    Key("ssh", BOOL, "off", "", False, False, "SSH login as user pi (set a password first)"),
    Key("password", SECRET, "", "", False, False, "Sets the password of user pi, then is cleared"),
    Key("auto_update", BOOL, "off", "", False, False, "Install updates of the game and StickTime Pi when online"),
    Key("keep_logs", BOOL, "off", "", False, False, "Keep the system log across restarts (on the SD card, at most 50 MB), to see why the Pi stopped"),
    Key("github_token", SECRET, "", "", False, False, "GitHub token (optional): only for app updates from a private fork"),
)

# SETTINGS_TEMPLATE_TEXT of host/src/settings_keys.h (check_settings_keys.py compares them)
TEMPLATE = (
    "# StickTime Pi settings\n"
    "#\n"
    "# Edit this file on a PC (it is on the SD card) or on the web page: http://sticktime.local\n"
    "# (or the address shown on the main menu). Lines starting with # are comments.\n"
    "# Put a value in \"double quotes\" when it contains a # after a space or starts with a space.\n"
    "# Video, WiFi and system changes apply at the next boot (video changes reboot once by themselves).\n"
    "\n"
    "# ---- Video ----------------------------------------------------------------------\n"
    "# av: composite video on the 3.5 mm jack, for a 5.8 GHz VTX (or a TV's yellow input)\n"
    "# hdmi: an HDMI screen or HDMI-in goggles. The Pi 4 can't do both at once.\n"
    "video = av\n"
    "# TV standard for av: ntsc, pal, pal60, ntsc-j, pal-m, pal-n, ntsc-443, secam\n"
    "tv = ntsc\n"
    "# shape of the picture in your goggles or screen: 4:3 or 16:9\n"
    "aspect = 4:3\n"
    "# av: interlaced (480i / 576i) or progressive (240p / 288p: half the lines, no flicker)\n"
    "av_mode = interlaced\n"
    "# hdmi: auto (1280x720 if the screen offers it, else its preferred mode), or e.g. 1920x1080, 1280x720@60\n"
    "hdmi_mode = auto\n"
    "# black border in percent, for goggles that cut off the edges of the picture (0-20)\n"
    "margin = 0\n"
    "# text size: auto, or the fonts of a radio screen: std (480x272), lrg (800x480), pc (1280x720), hd\n"
    "fonts = auto\n"
    "# show the web address on the main menu: auto (for a while after the network comes up), on, off\n"
    "show_ip = auto\n"
    "\n"
    "# ---- WiFi -----------------------------------------------------------------------\n"
    "# your network; 2.4 GHz is best next to 5.8 GHz video (and the ELRS module is 2.4 GHz only)\n"
    "wifi_ssid =\n"
    "wifi_password =\n"
    "# two-letter country code, needed for WiFi: US, GB, DE, FR, IT, ES, HR, AU, ...\n"
    "wifi_country = US\n"
    "# own WiFi network: auto = when no known network is found, on, off\n"
    "hotspot = auto\n"
    "hotspot_ssid = StickTime\n"
    "hotspot_password = sticktime\n"
    "\n"
    "# ---- Sticks ---------------------------------------------------------------------\n"
    "# source: auto (the last one that moved), wifi, usb, bluetooth, gamepad, joystick, keyboard\n"
    "input = auto\n"
    "# which channels are aileron, elevator, throttle, rudder: AETR, TAER, ...\n"
    "channel_order = AETR\n"
    "# or the channel of each stick (CH1-CH16): aileron elevator throttle yaw, e.g. 1 2 3 4\n"
    "# (empty: channel_order decides; gamepads and the keyboard keep their own layout)\n"
    "stick_channels =\n"
    "# sticks to invert, e.g.: ail, thr\n"
    "invert =\n"
    "# switch channel that pauses the game in flight (0 = off)\n"
    "pause_channel = 5\n"
    "# fly the menus with the sticks (pitch = up/down, roll right = select, roll left = back)\n"
    "stick_menus = on\n"
    "# ELRS / Crossfire WiFi joystick: the module's IP address (empty: find it by its broadcast)\n"
    "wifi_module_ip =\n"
    "# how often the module sends the sticks, in microseconds: 4000 = 250 Hz (ELRS allows 1000 and up)\n"
    "wifi_interval_us = 4000\n"
    "# pair ELRS BLE joysticks and Bluetooth gamepads automatically: on, off\n"
    "bluetooth = on\n"
    "\n"
    "# ---- Game -----------------------------------------------------------------------\n"
    "# camera prediction in seconds (the game's STICKTIME_LAT), 0 = off\n"
    "latency = 0\n"
    "# your radio's stick mode, 1-4 (the game shows the sticks this way)\n"
    "stick_mode = 2\n"
    "# the tool script to run, as a path in the game's SD folder (sticktime/sd)\n"
    "script = /SCRIPTS/TOOLS/StickTime.lua\n"
    "# game and display times next to the fps (turn on Show FPS in the game's settings): on, off\n"
    "stats = off\n"
    "# what the web page may change in the game's SD folder: game (tracks + installing the game), tracks, all, off\n"
    "sd_write = game\n"
    "\n"
    "# ---- Sound ----------------------------------------------------------------------\n"
    "# motor sound and beeps: on, off; volume 0-100\n"
    "sound = on\n"
    "volume = 70\n"
    "# music: on, off; volume 0-100; generated (made up live) or songs (your files in sticktime/music)\n"
    "music = off\n"
    "music_volume = 60\n"
    "music_source = generated\n"
    "# audio output: auto, jack (the 3.5 mm jack's audio, e.g. to a VTX audio pad), hdmi, off\n"
    "audio_out = auto\n"
    "# mono sound (a VTX has one audio channel): on, off\n"
    "audio_mono = off\n"
    "\n"
    "# ---- VTX control ----------------------------------------------------------------\n"
    "# change the VTX's channel, power and pit mode from the Pi over its SmartAudio or Tramp wire\n"
    "# (wiring and voltages: see the guide): off, auto (finds SmartAudio or Tramp), smartaudio, tramp\n"
    "vtx = off\n"
    "# the Pi's pin for it: gpio4 (pin 7, one wire through 1 kOhm; also gpio17, gpio22, gpio23, gpio27), or two wires\n"
    "# (the most reliable): uart3 (TX pin 7 + RX pin 29), uart4, uart5 (turning a UART on needs a reboot)\n"
    "vtx_port = gpio4\n"
    "# band: its letter in the VTX table (A, B, E, F, R, ...; empty = leave it), and the channel in it, 1-8\n"
    "vtx_band =\n"
    "vtx_channel = 1\n"
    "# frequency in MHz instead of band and channel (0 = use band and channel)\n"
    "vtx_freq = 0\n"
    "# power level in the VTX table, 1 = the lowest (0 = leave it); 25 mW is plenty next to the goggles\n"
    "vtx_power = 0\n"
    "# pit mode (almost no power): on, off\n"
    "vtx_pit = off\n"
    "# only send, read nothing back (when only the TX wire is connected): on, off\n"
    "vtx_send_only = off\n"
    "\n"
    "# ---- System ---------------------------------------------------------------------\n"
    "# network name: http://sticktime.local\n"
    "hostname = sticktime\n"
    "# SSH login as user pi: on, off (set a password first)\n"
    "ssh = off\n"
    "# sets the password of user pi at the next boot, then is cleared from this file\n"
    "password =\n"
    "# install updates of the game and StickTime Pi at boot when online: on, off\n"
    "auto_update = off\n"
    "# keep the system log across restarts (on the SD card, at most 50 MB), to see what happened\n"
    "# before the Pi stopped or restarted: on, off\n"
    "keep_logs = off\n"
    "# GitHub token (optional): only needed for StickTime Pi updates from a private fork\n"
    "github_token =\n"
)

_INDEX = {k.key: i for i, k in enumerate(KEYS)}
_WS = b" \t"
_L_BLANK, _L_COMMENT, _L_KV, _L_BAD = range(4)
_W_NOEQ, _W_BADKEY, _W_UNTERM, _W_AFTERQ, _W_UTF8 = 1, 2, 4, 8, 16

_EOL_RE = re.compile(rb"\r\n|\r|\n")
_HASH_RE = re.compile(rb"[ \t]#")
_KEY_RE = re.compile(rb"[a-z0-9_][a-z0-9_.\-]*")
_INT_RE = re.compile(rb"[+-]?[0-9]+")
_NUM_RE = re.compile(rb"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
_IP_RE = re.compile(rb"(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})")
_HDMI_RE = re.compile(rb"[1-9][0-9]{0,3}[xX][1-9][0-9]{0,3}(?:@([0-9]{1,3}(?:\.[0-9]{1,3})?))?")
_WORD_RE = re.compile(rb"[A-Za-z]+")
_CH_SEP_RE = re.compile(rb"[ \t,]+")
_CH_RE = re.compile(rb"(?:[cC][hH])?([0-9]{1,2})")
_INVERT_NAMES = (b"ail", b"aileron", b"ailerons", b"roll", b"ele", b"elevator", b"pitch",
                 b"thr", b"throttle", b"rud", b"rudder", b"yaw")


# ---------------------------------------------------------------- bytes <-> str
def _b(v):
    """str (or bytes) -> bytes; None when it can't be stored (a lone surrogate that isn't an escape)"""
    if isinstance(v, (bytes, bytearray)):
        return bytes(v)
    try:
        return str(v).encode("utf-8", "surrogateescape")
    except UnicodeEncodeError:
        return None


def _s(b):
    return b.decode("utf-8", "surrogateescape")


def _utf8_ok(b):
    try:
        b.decode("utf-8")       # strict, like settings.c: no overlongs, surrogates or > U+10FFFF
        return True
    except UnicodeDecodeError:
        return False


def _make_key(b):
    """the lower-cased key, or None unless it is [a-z0-9_][a-z0-9_.-]*, 1..64 chars (ASCII case only)"""
    if not b or len(b) > KEY_MAX:
        return None
    k = b.lower()
    return k if _KEY_RE.fullmatch(k) else None


def _lookup_key(key):
    """lookups take any case and don't check the characters; empty or over-long keys never match"""
    b = _b(key) if key is not None else None
    if not b or len(b) > KEY_MAX:
        return None
    return b.lower()


# ---------------------------------------------------------------- values
def _parse_int(v):
    """[+-]digits with |v| <= INT32_MAX, else None"""
    if not _INT_RE.fullmatch(v):
        return None
    neg = v[:1] == b"-"
    x = int(v.lstrip(b"+-"))
    if x > 0x7FFFFFFF:
        return None
    return -x if neg else x


def _parse_num(v):
    """[+-](digits[.[digits]] | .digits)[(e|E)[+-]digits], else None (no inf, nan, hex or spaces)"""
    return float(v) if _NUM_RE.fullmatch(v) else None


def _parse_bool(v):
    v = v.lower()
    if v in (b"on", b"yes", b"true", b"1"):
        return True
    if v in (b"off", b"no", b"false", b"0"):
        return False
    return None


def _in_choices(choices, v):
    return bool(v) and v.lower() in choices.encode().lower().split(b"|")


def _range_text(choices):
    lo, _, hi = choices.partition("|")
    return lo, hi


def _invert_ok(v):
    for w in _WORD_RE.findall(v):
        if len(w) >= 16:
            return False
        w = w.lower()
        if w == b"none" or w in _INVERT_NAMES:
            continue
        if len(w) > 4 or any(c not in b"aetr" for c in w):
            return False
    return True


def _order_ok(v):
    return len(v) == 4 and sorted(v.upper()) == sorted(b"AETR")


def _stick_channels(v):
    """stick_channels as input.c mix_parse_channels: [ail, ele, thr, rud], [] for none, None if invalid"""
    out = []
    for t in _CH_SEP_RE.split(v):
        if not t:
            continue
        m = _CH_RE.fullmatch(t)
        c = int(m.group(1)) if m else 0
        if not 1 <= c <= 16 or c in out or len(out) == 4:
            return None
        out.append(c)
    return out if len(out) in (0, 4) else None


def _hostname_ok(v):
    if not 1 <= len(v) <= 63:
        return False
    for i, c in enumerate(v):
        if not (0x30 <= c <= 0x39 or 0x41 <= c <= 0x5A or 0x61 <= c <= 0x7A or (c == 0x2D and 0 < i < len(v) - 1)):
            return False
    return True


def _ipv4_ok(v):
    m = _IP_RE.fullmatch(v)
    return bool(m) and all(int(p) <= 255 for p in m.groups())


def _hdmi_mode_ok(v):
    m = _HDMI_RE.fullmatch(v)
    return bool(m) and (m.group(1) is None or float(m.group(1)) > 0)


def _script_ok(v):
    n = len(v)
    if n < 6 or n > 255 or v[:1] != b"/" or b".." in v or b"\\" in v:
        return False
    if any(c < 0x20 or c == 0x7F for c in v):
        return False
    return v[-4:].lower() == b".lua"


def _is_hex(c):
    return 0x30 <= c <= 0x39 or 0x41 <= c <= 0x46 or 0x61 <= c <= 0x66


def _check(ki, v):
    """None when the bytes v are fine for KEYS[ki], else why (the messages of settings.c)"""
    k = KEYS[ki]
    name, n = k.key, len(v)
    if b"\r" in v or b"\n" in v:
        return "must not contain line breaks"
    if name == "channel_order":
        return None if _order_ok(v) else "must be the letters A, E, T and R in any order"
    if name == "stick_channels":
        if _stick_channels(v) is not None:
            return None
        return "must be four different channels from 1 to 16 for aileron, elevator, throttle, yaw (like 1 2 3 4), or empty"
    if name == "invert":
        return None if _invert_ok(v) else "must be a list of ail, ele, thr, rud"
    if name == "hostname":
        return None if _hostname_ok(v) else "must be 1-63 letters, digits or hyphens, not starting or ending with a hyphen"
    if name == "wifi_country":
        return None if n == 2 and v.isalpha() else "must be a two-letter country code"
    if name == "hotspot_password":
        return None if n == 0 or 8 <= n <= 63 else "must be 8-63 characters"
    if name == "wifi_password":
        ok = n == 0 or 8 <= n <= 63 or (n == 64 and all(_is_hex(c) for c in v))
        return None if ok else "must be empty, 8-63 characters or 64 hex digits"
    if name == "wifi_module_ip":
        return None if n == 0 or _ipv4_ok(v) else "must be an IPv4 address (like 192.168.1.50) or empty"
    if name == "hdmi_mode":
        return None if v.lower() == b"auto" or _hdmi_mode_ok(v) else "must be auto, WxH or WxH@Hz (like 1280x720@60)"
    if name == "latency":
        if v.lower() in (b"auto", b"game"):
            return None
        d = _parse_num(v)
        return None if d is not None and 0 <= d <= 0.2 else "must be auto, game or a number from 0 to 0.2"
    if name == "script":
        return None if _script_ok(v) else "must be an SD path ending in .lua (like /SCRIPTS/TOOLS/StickTime.lua)"
    if name == "wifi_ssid":
        return None if n <= 32 else "must be at most 32 bytes"
    if name == "hotspot_ssid":
        return None if 1 <= n <= 32 else "must be 1-32 bytes"
    if name == "vtx_band":
        ok = n == 0 or (n == 1 and (v.isalpha() or v.isdigit()))
        return None if ok else "must be one letter or digit (a band of the VTX table), or empty"
    if k.kind == ENUM:
        return None if _in_choices(k.choices, v) else "must be one of " + k.choices.replace("|", ", ")
    if k.kind == INT:
        lo, hi = _range_text(k.choices)
        x, mn, mx = _parse_int(v), _parse_int(lo.encode()), _parse_int(hi.encode())
        if x is not None and mn is not None and mx is not None and mn <= x <= mx:
            return None
        return f"must be a whole number from {lo} to {hi}"
    if k.kind == NUM:
        lo, hi = _range_text(k.choices)
        x, mn, mx = _parse_num(v), _parse_num(lo.encode()), _parse_num(hi.encode())
        if x is not None and mn is not None and mx is not None and mn <= x <= mx:
            return None
        return f"must be a number from {lo} to {hi}"
    if k.kind == BOOL:
        return None if _parse_bool(v) is not None else "must be on or off"
    return None


def _normalize(ki, v):
    """the stored form of a valid value of a known key"""
    k = KEYS[ki]
    if k.kind == BOOL:
        return b"on" if _parse_bool(v) else b"off"
    if k.kind == INT:
        return str(_parse_int(v) or 0).encode()
    if k.key == "stick_channels":
        return b" ".join(str(c).encode() for c in _stick_channels(v) or [])
    if k.kind == ENUM or k.key in ("hdmi_mode", "latency"):
        return v.lower()
    if k.key in ("wifi_country", "channel_order", "vtx_band"):
        return v.upper()
    return v


def key(name):
    """the Key of a known setting (any ASCII case), or None"""
    k = _lookup_key(name)
    i = _INDEX.get(k.decode("latin-1")) if k is not None else None
    return KEYS[i] if i is not None else None


def validate(name, value):
    """None if value is fine for the key, else a message. Unknown keys accept anything without a
    line break. (A NUL can't be stored either: C strings end there.)"""
    if value is None:
        return None
    v = _b(value)
    if v is None:
        return "must be valid UTF-8"
    if b"\r" in v or b"\n" in v:
        return "must not contain line breaks"
    if b"\0" in v:
        return "must not contain NUL characters"
    k = key(name)
    return _check(_INDEX[k.key], v) if k else None


# ---------------------------------------------------------------- lines
class _Line:
    __slots__ = ("text", "term", "kind", "flags", "key", "value", "eff", "ki",
                 "pre_len", "val_off", "val_len", "post_off", "comment")

    def __init__(self, text, term):
        self.text, self.term = text, term
        self.eff, self.ki = None, -1
        self.parse()

    def parse(self):
        t, n = self.text, len(self.text)
        self.key = self.value = None
        self.comment = False
        self.kind = _L_BAD
        self.flags = 0 if _utf8_ok(t) else _W_UTF8
        i = 0
        while i < n and t[i] in _WS:
            i += 1
        if i == n:
            self.kind = _L_BLANK
            return
        if t[i] == 0x23:
            self.kind = _L_COMMENT
            return
        e = t.find(b"=")
        if e < 0:
            self.flags |= _W_NOEQ
            return
        ke = e
        while ke > i and t[ke - 1] in _WS:
            ke -= 1
        k = _make_key(t[i:ke])
        if k is None:
            self.flags |= _W_BADKEY
            return
        self.kind = _L_KV
        self.key = k.decode("ascii")
        self.pre_len = e + 1
        vs = e + 1
        while vs < n and t[vs] in _WS:
            vs += 1
        if vs < n and t[vs] == 0x22:
            buf = bytearray()
            j, closed = vs + 1, False
            while j < n:
                c = t[j]
                if c == 0x5C and j + 1 < n and t[j + 1] in b'"\\':
                    buf.append(t[j + 1])
                    j += 2
                elif c == 0x22:
                    closed = True
                    break
                else:
                    buf.append(c)
                    j += 1
            if closed:
                k = j + 1
                while k < n and t[k] in _WS:
                    k += 1
                if k == n or t[k] == 0x23:
                    self.value = bytes(buf)
                    self.val_off, self.val_len, self.post_off = vs, j + 1 - vs, j + 1
                    self.comment = k < n
                    return
                self.flags |= _W_AFTERQ
            else:
                self.flags |= _W_UNTERM
        # unquoted: a '#' after a space or tab starts the comment
        m = _HASH_RE.search(t, e + 1)
        c = m.start() + 1 if m else n
        ve = c
        while ve > vs and t[ve - 1] in _WS:
            ve -= 1
        if ve <= vs:            # empty: everything after the '=' is the tail
            vs = ve = e + 1
        self.val_off, self.val_len, self.post_off = vs, ve - vs, ve
        self.comment = c < n
        self.value = t[vs:ve]

    def rewrite(self, v):
        """a new value, keeping the key as written, the spacing and the comment"""
        enc = _encode(v)
        t = self.text
        ws1 = self.val_off - self.pre_len if self.val_len and enc else 0
        add_sp = bool(enc) and not self.val_len
        post = t[self.post_off:] if self.comment else b""
        post_sp = bool(post) and post[0] not in _WS
        self.text = (t[:self.pre_len] + (b" " if add_sp else t[self.pre_len:self.pre_len + ws1]) + enc
                     + (b" " if post_sp else b"") + post)
        self.parse()


def _needs_quotes(v):
    if not v:
        return False
    if v[0] in _WS or v[0] in b'"#' or v[-1] in _WS:
        return True
    return any(v[i] == 0x23 and v[i - 1] in _WS for i in range(1, len(v)))


def _encode(v):
    """the value as it is written in the file"""
    if not _needs_quotes(v):
        return v
    return b'"' + v.replace(b"\\", b"\\\\").replace(b'"', b'\\"') + b'"'


# ---------------------------------------------------------------- the file
class Settings:
    """a parsed sticktime.txt that keeps every line"""

    def __init__(self):
        self._lines = []
        self._bom = False
        self._had_nul = False
        self._nl = b"\n"
        self._note = None
        self.load_error = None          # set by load() when the file exists but can't be read
        self._refresh()

    @classmethod
    def parse(cls, data):
        s = cls()
        if isinstance(data, str):
            b = _b(data)
            data = b if b is not None else data.encode("utf-8", "surrogatepass")   # kept, flagged "not UTF-8"
        data = bytes(data or b"")
        if b"\0" in data:
            s._had_nul = True
            data = data.replace(b"\0", b"")
        p = 0
        if data[:3] == BOM:
            s._bom, p = True, 3
        for m in _EOL_RE.finditer(data, p):          # a lone CR ends a line too (old Mac files)
            if not s._lines:
                s._nl = m.group()                     # new lines get the file's first line end
            s._lines.append(_Line(data[p:m.start()], m.group()))
            p = m.end()
        if p < len(data):
            s._lines.append(_Line(data[p:], b""))
        s._refresh()
        return s

    # -- effective values and warnings
    def _refresh(self):
        self._last = [-1] * len(KEYS)
        for i, ln in enumerate(self._lines):
            ln.eff, ln.ki = None, -1
            if ln.kind != _L_KV:
                continue
            ki = _INDEX.get(ln.key, -1)
            ln.ki = ki
            if ki < 0:
                continue
            self._last[ki] = i
            if _check(ki, ln.value) is None:
                ln.eff = _normalize(ki, ln.value)
        hp, hs = _INDEX["hotspot_password"], _INDEX["hotspot"]
        self._hp_default = False
        self._hp_default = self._last[hp] >= 0 and not self._lines[self._last[hp]].value and self._known(hs) != b"off"
        w = []
        if self._note:
            w.append(self._note)
        if self._had_nul:
            w.append("NUL bytes removed")
        for i, ln in enumerate(self._lines, 1):
            if ln.flags & _W_NOEQ:
                w.append(f"line {i}: no '='")
            if ln.flags & _W_BADKEY:
                w.append(f"line {i}: bad key")
            if ln.flags & _W_UNTERM:
                w.append(f"line {i}: unterminated quote")
            if ln.flags & _W_AFTERQ:
                w.append(f"line {i}: text after the closing quote")
            if ln.flags & _W_UTF8:
                w.append(f"line {i}: not UTF-8")
            if ln.kind != _L_KV or ln.ki < 0 or self._last[ln.ki] != i - 1:
                continue
            msg = _check(ln.ki, ln.value) if ln.value else None
            if msg:
                w.append(f"line {i}: {ln.key} {msg} (using the default)")
            elif ln.ki == hp and self._hp_default:
                w.append(f"line {i}: hotspot_password is empty but hotspot is not off (using the default)")
        self._warn = w

    def _known(self, ki):
        li = self._last[ki]
        default = KEYS[ki].default.encode()
        if li < 0 or (self._hp_default and KEYS[ki].key == "hotspot_password"):
            return default
        eff = self._lines[li].eff
        return eff if eff is not None else default

    def _find_last(self, k):
        for i in range(len(self._lines) - 1, -1, -1):
            if self._lines[i].kind == _L_KV and self._lines[i].key == k:
                return i
        return -1

    # -- reading
    def raw(self, name):
        """the value as parsed (unquoted, unescaped), or None when the key is not set"""
        k = _lookup_key(name)
        if k is None:
            return None
        i = self._find_last(k.decode("latin-1"))
        return _s(self._lines[i].value) if i >= 0 else None

    def get(self, name):
        """the effective value of a known key (normalized; the default when empty or invalid), the raw
        value of an unknown one, or "" """
        k = _lookup_key(name)
        if k is None:
            return ""
        ki = _INDEX.get(k.decode("latin-1"), -1)
        if ki >= 0:
            return _s(self._known(ki))
        v = self.raw(name)
        return v if v is not None else ""

    def get_int(self, name, default, lo, hi):
        x = _parse_int(_b(self.get(name)))
        return default if x is None else max(lo, min(hi, x))

    def get_num(self, name, default, lo, hi):
        x = _parse_num(_b(self.get(name)))
        if x is None or x != x:
            return default
        return max(lo, min(hi, x))

    def get_bool(self, name, default):
        b = _parse_bool(_b(self.get(name)))
        return default if b is None else b

    def each(self):
        """[(key, value)] in file order: each key once, at its first line, with its last value"""
        out = {}
        for ln in self._lines:
            if ln.kind == _L_KV:
                out[ln.key] = _s(ln.value)
        return list(out.items())

    items = each

    def as_dict(self, effective=True):
        """{key: value}: every known key's effective value plus the unknown keys' raw values
        (effective=False: only what the file sets, raw)"""
        d = dict(self.each())
        if effective:
            for k in KEYS:
                d[k.key] = self.get(k.key)
        return d

    def warnings(self):
        """one warning per line, each ending in "\\n"; "" when there are none (as settings_warnings)"""
        return "".join(m + "\n" for m in self._warn)

    def warning_list(self):
        return list(self._warn)

    # -- editing
    def set(self, name, value):
        """Set a value (None removes the key's lines). False for an invalid key name, a value with a
        line break or a NUL, or one that can't be encoded. Valid values of known keys are stored
        normalized (enums lower case, bools on/off, ...); others are stored as given."""
        kb = _b(name) if name is not None else None
        k = _make_key(kb.strip(_WS)) if kb is not None else None
        if k is None:
            return False
        k = k.decode("ascii")
        if value is None:
            self._lines = [ln for ln in self._lines if not (ln.kind == _L_KV and ln.key == k)]
            self._refresh()
            return True
        v = _b(value)
        if v is None or b"\r" in v or b"\n" in v or b"\0" in v:
            return False
        ki = _INDEX.get(k, -1)
        if ki >= 0 and _check(ki, v) is None:
            v = _normalize(ki, v)
        i = self._find_last(k)
        if i >= 0:
            if self._lines[i].value != v:
                self._lines[i].rewrite(v)
        else:
            enc = _encode(v)
            if self._lines and not self._lines[-1].term:
                self._lines[-1].term = self._nl
            self._lines.append(_Line(k.encode() + (b" = " + enc if enc else b" ="), self._nl))
        self._refresh()
        return True

    def serialize(self):
        """the file's bytes"""
        return (BOM if self._bom else b"") + b"".join(ln.text + ln.term for ln in self._lines)

    def save(self, path):
        """atomic: a temp file next to it, fsync, rename, fsync of the folder. Raises OSError."""
        write_atomic(path, self.serialize())


def parse(data):
    """Settings from the file's bytes (or text)"""
    return Settings.parse(data)


def new():
    return Settings()


def load(path):
    """(Settings, exists). A missing file gives the template's settings (exists=False). A file that
    exists but can't be read also gives the template, with a warning and .load_error set."""
    try:
        os.stat(path)
        exists = True
    except (FileNotFoundError, NotADirectoryError):
        exists = False
    except OSError:
        exists = True                    # something is there
    data, err = None, None
    if exists:
        try:
            with open(path, "rb") as f:
                data = f.read(FILE_MAX)
            if len(data) >= FILE_MAX:    # as plat_read_file: the limit itself is too big
                data, err = None, OSError(errno.EFBIG, os.strerror(errno.EFBIG), path)
        except FileNotFoundError:
            exists = False               # removed in between
        except OSError as e:
            err = e
    if data is not None:
        return Settings.parse(data), exists
    s = Settings.parse(TEMPLATE)
    if exists:
        why = (err.strerror if err is not None and err.strerror else str(err)) if err else "unknown error"
        s._note = f"cannot read {path}: {why} (using the defaults)"
        s.load_error = err
        s._refresh()
    return s, exists


def write_atomic(path, data):
    """temp file + fsync + rename + fsync of the folder (safe against power loss on FAT)"""
    tmp = f"{path}.{os.getpid()}.tmp~"
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
    if hasattr(os, "O_DIRECTORY"):
        try:
            fd = os.open(os.path.dirname(os.path.abspath(path)), os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        except OSError:
            pass


def update_file(path, changes):
    """Set some keys in the file (a missing file starts from the template) and save it atomically.
    changes: {key: value or None (remove)}. Nothing is written unless every change is possible
    (ValueError), and an existing file that can't be read is never replaced (OSError)."""
    s, _exists = load(path)
    if s.load_error is not None:
        raise s.load_error
    for k, v in dict(changes).items():
        if not s.set(k, v):
            raise ValueError(f"can't set {k!r}")
    s.save(path)
    return s


save_values = update_file


# ---------------------------------------------------------------- command line
def _main(argv):
    path = DEFAULT_PATH
    if len(argv) >= 2 and argv[0] in ("-f", "--file"):
        path, argv = argv[1], argv[2:]
    if not argv:
        print(__doc__.split("Command line")[1].strip(), file=sys.stderr)
        return 2
    cmd, args = argv[0], argv[1:]
    if cmd == "validate" and len(args) == 2:
        msg = validate(args[0], args[1])
        if msg:
            print(f"{args[0]} {msg}", file=sys.stderr)
        return 1 if msg else 0
    s, _ = load(path)
    if cmd == "get" and args:
        for k in args:
            print(s.get(k))
    elif cmd == "raw" and len(args) == 1:
        v = s.raw(args[0])
        if v is None:
            return 1
        print(v)
    elif cmd == "set" and args:
        changes = {}
        for a in args:
            k, eq, v = a.partition("=")
            if not eq:
                print(f"set: expected KEY=VALUE, got {a!r}", file=sys.stderr)
                return 2
            msg = validate(k, v)
            if msg:
                print(f"{k} {msg}", file=sys.stderr)
                return 1
            changes[k] = v
        update_file(path, changes)
    elif cmd == "warnings" and not args:
        sys.stdout.write(s.warnings())
    elif cmd == "json" and not args:
        print(json.dumps(s.as_dict(), ensure_ascii=False))
    else:
        print(f"unknown command or arguments: {' '.join(argv)}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
