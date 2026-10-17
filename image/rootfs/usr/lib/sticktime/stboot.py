"""config.txt and cmdline.txt from sticktime.txt: pure functions (no I/O).

Used at every boot by sticktime-setup and at build time by image/build.sh (sticktime-setup bake),
so a freshly flashed card already has the default video config and the first boot needs no
reboot (pios-image.md E4).
"""
import re

BEGIN = "# >>> sticktime: generated from sticktime.txt at every boot (edit sticktime.txt, not these lines) >>>"
END = "# <<< sticktime <<<"
_BEGIN_RE = re.compile(r"^\s*#\s*>>>\s*sticktime\b")
_END_RE = re.compile(r"^\s*#\s*<<<\s*sticktime\b")
_SECTION_RE = re.compile(r"^\s*\[([^\]]*)\]")
# stock lines we take over: two vc4 overlay lines would load the overlay twice (E17)
_VC4_RE = re.compile(r"^\s*dtoverlay\s*=\s*vc4-f?kms-v3d(-pi\d)?\b")
_OWNED_ALL_RE = re.compile(r"^\s*(dtparam\s*=\s*audio\s*=\s*\w+\s*$|camera_auto_detect\s*=|display_auto_detect\s*=|"
                           r"disable_overscan\s*=|enable_tvout\s*=|disable_splash\s*=|disable_poe_fan\s*=)")
NOTE = "  # sticktime: set in the block at the end"

# vc4.tv_norm names the kernel parses (drm_get_tv_mode_from_name): PAL60 is NOT one of them, it is
# norm PAL + the 480-line mode, which the host picks (drm-kms.md 4.8)
TV_NORMS = {"ntsc": "NTSC", "ntsc-j": "NTSC-J", "ntsc-443": "NTSC-443", "pal": "PAL", "pal60": "PAL",
            "pal-m": "PAL-M", "pal-n": "PAL-N", "secam": "SECAM"}
VIDEOS = ("av", "hdmi")
AUDIO_OUTS = ("auto", "jack", "hdmi", "off")
# VTX control (SmartAudio / Tramp) on a PL011 UART that leaves Bluetooth's UART0 alone; the overlays
# README (rpi-6.12.y): uart3 = GPIOs 4-7, uart4 = 8-11, uart5 = 12-15 (TX, RX, then CTS/RTS only with ctsrts)
VTX_MODES = ("off", "auto", "smartaudio", "tramp")
VTX_UARTS = ("uart3", "uart4", "uart5")
# vtx_port: one pin in software (no overlay: the host takes the GPIO through /dev/gpiochip*), or a UART
VTX_PORTS = ("gpio4", "gpio17", "gpio22", "gpio23", "gpio27") + VTX_UARTS
_UART_RE = re.compile(r"^dtoverlay\s*=\s*uart[2-5]\b")

# cmdline.txt tokens sticktime-setup owns; every other token is kept as it is, in order
# (root=PARTUUID changes on the first boot, 'resize' belongs to the initramfs, Imager adds systemd.run=)
OWNED_FLAGS = ("quiet", "logo.nologo")
OWNED_KEYS = ("loglevel", "vt.global_cursor_default", "consoleblank", "cpufreq.default_governor",
              "vc4.tv_norm", "cfg80211.ieee80211_regdom", "snd_bcm2835.enable_headphones")


def jack_audio(video, audio_out):
    """does sound go to the 3.5 mm jack (the same rule as block_lines)"""
    if audio_out == "hdmi" and video == "av":
        audio_out = "jack"
    if audio_out == "auto":
        audio_out = "jack" if video == "av" else "both"
    return audio_out in ("jack", "both")


def block_lines(video, tv, audio_out, vtx_uart=None):
    """our config.txt block (list of lines) and warnings; vtx_uart: "uart3".."uart5" when VTX control is on"""
    warn = []
    if audio_out == "hdmi" and video == "av":
        warn.append("audio_out = hdmi needs video = hdmi (the Pi 4 turns HDMI off for composite): using the jack")
        audio_out = "jack"
    if audio_out == "auto":
        audio_out = "jack" if video == "av" else "both"
    jack = audio_out in ("jack", "both")
    hdmi_audio = audio_out in ("hdmi", "both")
    overlay = "dtoverlay=vc4-kms-v3d" + (",composite" if video == "av" else "") + ("" if hdmi_audio else ",noaudio")
    lines = [BEGIN, "[all]", overlay,
             # Pi 4: composite only works with enable_tvout=1 (which disables HDMI); HDMI mode says 0
             # explicitly so composite never comes up when no HDMI screen is found (config_txt/video.adoc)
             "enable_tvout=1" if video == "av" else "enable_tvout=0",
             f"dtparam=audio={'on' if jack else 'off'}",
             "disable_overscan=1",
             # boot speed: no rainbow splash, no PoE HAT / camera / DSI display I2C probes
             "disable_splash=1", "disable_poe_fan=1", "camera_auto_detect=0", "display_auto_detect=0"]
    if vtx_uart in VTX_UARTS:
        lines.append("dtoverlay=" + vtx_uart)     # TX and RX only (no ctsrts): the VTX's one wire
    elif vtx_uart:
        warn.append(f"vtx_port = {vtx_uart!r} is not one of {', '.join(VTX_UARTS)}: no UART for VTX control")
    lines.append(END)
    return lines, warn


def config_txt(old, video, tv, audio_out, vtx_uart=None):
    """(new config.txt text, warnings). Keeps every line we don't own; comments out the stock lines
    we take over; our block always goes last, after its own [all] (never inside [cm4]/[pi5])."""
    crlf = "\r\n" in old
    lines = old.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    out, in_block, section_all = [], False, True
    for ln in lines:
        if in_block:
            if _END_RE.match(ln):
                in_block = False
            continue
        if _BEGIN_RE.match(ln):
            in_block = True              # a block without its end marker runs to the end of the file
            continue
        if _END_RE.match(ln):
            continue                     # an orphan end marker
        m = _SECTION_RE.match(ln)
        if m:
            section_all = m.group(1).strip().lower() in ("all", "")
        if _VC4_RE.match(ln) or (section_all and _OWNED_ALL_RE.match(ln)):
            out.append("#" + ln.strip() + NOTE)
            continue
        out.append(ln)
    while out and out[-1].strip() == "":
        out.pop()
    blk, warn = block_lines(video, tv, audio_out, vtx_uart)
    out += [""] + blk
    nl = "\r\n" if crlf else "\n"
    return nl.join(out) + nl, warn


def cmdline_txt(old, tv, country, jack=False):
    """new cmdline.txt (one line, LF), or None when the old one is unusable (no root=): then we must
    not touch it, the Pi would not boot. jack: sound on the 3.5 mm jack. With composite on, the Pi 4
    firmware puts snd_bcm2835.enable_headphones=0 on the command line, so there is no Headphones sound
    card at all; our =1 comes after it in the line and wins (checked on a Pi 4 with Pi OS trixie)."""
    toks = old.split()
    if not any(t.startswith("root=") for t in toks):
        return None
    want = {"loglevel": "3", "vt.global_cursor_default": "0", "consoleblank": "0",
            "cpufreq.default_governor": "performance", "vc4.tv_norm": TV_NORMS.get(tv, "NTSC")}
    if country:
        want["cfg80211.ieee80211_regdom"] = country
    if jack:
        want["snd_bcm2835.enable_headphones"] = "1"
    out, seen = [], set()
    for t in toks:
        k = t.split("=", 1)[0]
        if t in OWNED_FLAGS:
            if t in seen:
                continue
            seen.add(t)
            out.append(t)
        elif "=" in t and k in OWNED_KEYS:
            if k in seen or k not in want:
                continue                  # a duplicate, or a key we now drop (no country)
            seen.add(k)
            out.append(f"{k}={want[k]}")
        else:
            out.append(t)
    for f in OWNED_FLAGS:
        if f not in seen:
            out.append(f)
    for k in ("loglevel", "logo.nologo", "vt.global_cursor_default", "consoleblank", "cpufreq.default_governor",
              "vc4.tv_norm", "cfg80211.ieee80211_regdom", "snd_bcm2835.enable_headphones"):
        if k in want and k not in seen:
            out.append(f"{k}={want[k]}")
    return " ".join(out) + "\n"


def video_signature(config, cmdline):
    """what needs a reboot to take effect: the active vc4 overlay / enable_tvout lines, the TV norm and
    the UART overlay of VTX control. Audio lines, the WiFi country and the quiet flags apply without one
    (or at the next boot)."""
    sig = []
    for ln in config.replace("\r\n", "\n").split("\n"):
        s = ln.strip()
        if _UART_RE.match(s):
            sig.append(re.sub(r"\s+", "", s.split("#", 1)[0]))
            continue
        if re.match(r"^(dtoverlay\s*=\s*vc4-|enable_tvout\s*=)", s):
            s = re.sub(r"\s+", "", s.split("#", 1)[0])
            parts = s.split(",")
            # HDMI audio on/off rides on the overlay line but is no reason to reboot
            sig.append(",".join([parts[0]] + sorted(x for x in parts[1:] if not x.startswith(("noaudio", "audio")))))
    norm = ""
    for t in (cmdline or "").split():
        if t.startswith("vc4.tv_norm="):
            norm = t
    return "\n".join(sig) + "\n" + norm
