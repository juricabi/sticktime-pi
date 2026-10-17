#!/usr/bin/env python3
"""StickTime's web/simulator.html -> the Pi's /editor/index.html.

    python3 tools/patch_editor.py IN OUT        (IN may be OUT; exit 1 when the page doesn't fit)

patch(html) puts <script src="pi-sd.js"></script> right before the only </head>, so the shim
(web/pi-sd.js) runs before every page script and the track editor sees its showDirectoryPicker;
it removes the Google Fonts <link> tags (the page then needs no internet: on the Pi's hotspot a
stylesheet from fonts.googleapis.com would hold the page until it times out) and the
apple-touch-icon (a 404 on the Pi). Everything else stays byte for byte, line ends included.
Install stays on: the Pi serves the staged release at /editor/sdcard/.

A page that doesn't look like the one this was written for (a future StickTime) raises
PatchError instead of making a page that half works: no single </head>, a Google Fonts link left
that the patterns don't know, or none of the editor's hooks the shim needs. Patching a patched
page changes nothing. The updater imports patch() (stdlib only, Python 3.11+).
"""
import os
import re
import sys
import tempfile

SHIM = '<script src="pi-sd.js"></script>'
HEAD_END = re.compile(r"</head\s*>", re.I)
# the tags bundle_web.py writes, each on a line of its own (with its line end)
FONT_LINKS = re.compile(
    r'[ \t]*<link\s+rel="(?:preconnect|dns-prefetch|preload|stylesheet)"\s+href="https://fonts\.(?:googleapis|gstatic)\.com[^"]*"'
    r'(?:\s+crossorigin(?:="[^"]*")?)?(?:\s+as="[^"]*")?\s*/?>[ \t]*(?:\r\n|\n|\r)?')
TOUCH_ICON = re.compile(r'[ \t]*<link\s+rel="apple-touch-icon(?:-precomposed)?"\s+(?:sizes="[^"]*"\s+)?href="[^"]*"\s*/?>[ \t]*(?:\r\n|\n|\r)?')
# anything still pointing at Google Fonts after the patch: a form the patterns above don't know
FONT_LEFT = re.compile(r"<link\b[^>]*fonts\.(?:googleapis|gstatic)\.com", re.I)
# what pi-sd.js works with: the editor's folder picker and the buttons it clicks
HOOKS = ("showDirectoryPicker", 'id="tracksBtn"', 'id="edConnect"', 'id="edMineBtn"', 'id="edGalBtn"', 'id="radioBtn"')


class PatchError(ValueError):
    """the page isn't one this patch knows: nothing is written"""


def patch(html: str) -> str:
    heads = HEAD_END.findall(html)
    if len(heads) != 1:
        raise PatchError(f"expected exactly one </head>, found {len(heads)}")
    missing = [h for h in HOOKS if h not in html]
    if missing:
        raise PatchError("not StickTime's emulator page, or a version pi-sd.js doesn't know: no " + ", ".join(missing))
    shims = html.count(SHIM)
    if shims > 1:
        raise PatchError(f"{SHIM} is in the page {shims} times")
    head = HEAD_END.search(html)
    if shims == 0:
        # the line end the page uses (CR LF in a Windows checkout, LF from GitHub)
        nl = "\r\n" if "\r\n" in html[:head.start() + 2048] else "\n"
        html = html[:head.start()] + SHIM + nl + html[head.start():]
    elif html.find(SHIM) > head.start():
        raise PatchError(f"{SHIM} is after </head>: it must run before the page's scripts")
    html = FONT_LINKS.sub("", html)
    html = TOUCH_ICON.sub("", html)
    left = FONT_LEFT.search(html)
    if left:
        raise PatchError("a Google Fonts link this patch doesn't know: " + html[left.start():html.find(">", left.start()) + 1][:200])
    return html


def write_atomic(path, data: bytes):
    d = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(prefix=".patch-", dir=d)
    try:
        with os.fdopen(fd, "wb") as f:
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


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print("usage: patch_editor.py IN OUT", file=sys.stderr)
        return 2
    src, dst = argv
    try:
        with open(src, "rb") as f:
            html = f.read().decode("utf-8")
        out = patch(html)
        write_atomic(dst, out.encode("utf-8"))
    except (OSError, UnicodeDecodeError, PatchError) as e:
        print(f"patch_editor: {src}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
