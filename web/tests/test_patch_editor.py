#!/usr/bin/env python3
"""tools/patch_editor.py: StickTime's simulator.html -> the Pi's /editor/index.html.

The real page (../sticktime/web/simulator.html) and small made-up ones: the shim goes right before
the only </head>, the Google Fonts links and the apple-touch-icon go, every other byte stays (line
ends too); patching twice changes nothing; a page it doesn't know fails loudly (PatchError, a
ValueError, which the updater catches; exit 1 from the command line) and writes nothing.

    python web/tests/test_patch_editor.py
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
TOOL = REPO / "tools" / "patch_editor.py"
SIM = REPO.parent / "sticktime" / "web" / "simulator.html"
sys.dont_write_bytecode = True                  # (no __pycache__ in the repo)
sys.path.insert(0, str(REPO / "tools"))
import patch_editor  # noqa: E402
from patch_editor import SHIM, PatchError, patch  # noqa: E402

FONTS = ('<link rel="preconnect" href="https://fonts.googleapis.com">\n'
         '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>\n'
         '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Inter:wght@400;600&amp;display=swap">\n')
ICON = '<link rel="apple-touch-icon" href="apple-touch-icon.png">\n'
BODY = ('<button id="tracksBtn">Tracks</button><button id="edConnect"></button><button id="edMineBtn"></button>'
        '<button id="edGalBtn"></button><button id="radioBtn"></button>\n'
        "<script>const GAME_FILES = 'sdcard/'; if (window.showDirectoryPicker) {}</script>\n")


def page(head_extra=FONTS + ICON, body=BODY, nl="\n", head_end="</head>"):
    """a page shaped like bundle_web.py's output"""
    text = ('<!doctype html><html lang="en"><head>\n<meta charset="utf-8">\n'
            '<link rel="icon" href="data:image/png;base64,AAAA">\n' + head_extra + head_end + "<body>\n" + body + "</body></html>\n")
    return text.replace("\n", nl)


class PatchTest(unittest.TestCase):
    def test_small_page(self):
        out = patch(page())
        self.assertEqual(out, page(head_extra="").replace("</head>", SHIM + "\n</head>"))

    def test_crlf_page_keeps_its_line_ends(self):
        src = page(nl="\r\n")
        out = patch(src)
        self.assertEqual(out, page(head_extra="", nl="\r\n").replace("</head>", SHIM + "\r\n</head>"))
        self.assertNotIn("\n", out.replace("\r\n", ""), "no bare LF added")

    def test_idempotent(self):
        once = patch(page())
        self.assertEqual(patch(once), once)
        self.assertEqual(patch(patch(once)), once)
        self.assertEqual(once.count(SHIM), 1)

    def test_head_end_forms(self):
        for end in ("</HEAD>", "</head >", "</Head\t>"):
            out = patch(page(head_end=end))
            self.assertIn(SHIM + "\n" + end, out, end)

    def test_no_head_end(self):
        with self.assertRaisesRegex(PatchError, "found 0"):
            patch(page(head_end=""))

    def test_two_head_ends(self):
        with self.assertRaisesRegex(PatchError, "found 2"):
            patch(page(body=BODY + "<pre>&lt;/head&gt; is fine, but </head> is not</pre>\n"))

    def test_missing_hooks_are_named(self):
        for hook in ('id="tracksBtn"', 'id="edConnect"', 'id="edMineBtn"', 'id="edGalBtn"', 'id="radioBtn"', "showDirectoryPicker"):
            with self.assertRaises(PatchError) as cm:
                patch(page(body=BODY.replace(hook, "x")))
            self.assertIn(hook, str(cm.exception))

    def test_unknown_fonts_link_fails(self):
        for link in ('<link href="https://fonts.googleapis.com/css2?family=X" rel="stylesheet">\n',           # attributes the other way round
                     '<link rel="stylesheet" type="text/css" href="https://fonts.googleapis.com/css?family=X">\n',
                     "<link rel='stylesheet' href='https://fonts.googleapis.com/css?family=X'>\n"):
            with self.assertRaisesRegex(PatchError, "Google Fonts link"):
                patch(page(head_extra=link))

    def test_known_fonts_link_forms(self):
        forms = ('  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin="anonymous" />\n'
                 '\t<link rel="dns-prefetch" href="https://fonts.googleapis.com">\n'
                 '<link rel="preload" href="https://fonts.gstatic.com/s/x.woff2" crossorigin as="font">\n'
                 '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=A&amp;display=swap"/>\n')
        self.assertEqual(patch(page(head_extra=forms)), patch(page(head_extra="")))

    def test_touch_icon_forms(self):
        for icon in ('<link rel="apple-touch-icon" sizes="180x180" href="apple-touch-icon.png">\n',
                     '<link rel="apple-touch-icon-precomposed" href="/icon.png" />\n'):
            self.assertEqual(patch(page(head_extra=icon)), patch(page(head_extra="")))

    def test_other_links_and_text_stay(self):
        keep = ('<link rel="icon" href="favicon.png">\n<link rel="stylesheet" href="local.css">\n'
                '<!-- fonts.googleapis.com is mentioned here, not linked -->\n')
        out = patch(page(head_extra=keep))
        self.assertIn(keep + SHIM, out)

    def test_shim_misplaced_or_twice(self):
        with self.assertRaisesRegex(PatchError, "after </head>"):
            patch(page(body=SHIM + "\n" + BODY))
        with self.assertRaisesRegex(PatchError, "2 times"):
            patch(page(head_extra=SHIM + "\n" + SHIM + "\n"))

    def test_patch_error_is_a_value_error(self):
        self.assertTrue(issubclass(PatchError, ValueError), "the updater catches ValueError")

    def test_last_line_without_line_end(self):
        src = page(head_extra='<link rel="apple-touch-icon" href="a.png">').rstrip("\n")
        self.assertNotIn("apple-touch-icon", patch(src))


@unittest.skipUnless(SIM.is_file(), "../sticktime is not checked out next to this repo")
class RealPageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = SIM.read_bytes().decode("utf-8")
        cls.out = patch(cls.src)

    def test_shim_right_before_head_end(self):
        out = self.out
        self.assertEqual(out.count(SHIM), 1)
        i = out.index(SHIM)
        self.assertRegex(out[i + len(SHIM):i + len(SHIM) + 10], r"^\r?\n</head>")
        self.assertEqual(out.index("<script"), i, "before every page script")

    def test_only_the_tags_go(self):
        nl = "\r\n" if "\r\n" in self.src[:4096] else "\n"
        gone = [ln for ln in self.src.splitlines(True) if "fonts.googleapis.com" in ln or "fonts.gstatic.com" in ln or "apple-touch-icon" in ln]
        self.assertGreaterEqual(len(gone), 3, "the page has its Google Fonts links and the touch icon")
        for ln in gone:
            self.assertRegex(ln.strip(), r"^<link [^>]*>$", "each one a line of its own")
        expect = "".join(ln for ln in self.src.splitlines(True) if ln not in gone)
        self.assertEqual(self.out.replace(SHIM + nl, "", 1), expect, "every other byte as it was")

    def test_install_stays_on(self):
        self.assertIn("const GAME_FILES = 'sdcard/';", self.out)
        self.assertIn("const GALLERY_FILES = 'tracks/';", self.out)

    def test_idempotent(self):
        self.assertEqual(patch(self.out), self.out)

    def test_no_internet_left(self):
        for host in ("fonts.googleapis.com", "fonts.gstatic.com"):
            self.assertNotIn(host, self.out)
        self.assertNotIn('href="apple-touch-icon.png"', self.out)


class CliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="patch-editor-test-"))

    def tearDown(self):
        for p in sorted(self.tmp.rglob("*"), reverse=True):
            p.unlink() if p.is_file() else p.rmdir()
        self.tmp.rmdir()

    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, text=True)

    def test_in_to_out(self):
        src, dst = self.tmp / "simulator.html", self.tmp / "index.html"
        src.write_bytes(page(nl="\r\n").encode())
        r = self.run_tool(src, dst)
        self.assertEqual((r.returncode, r.stderr), (0, ""))
        self.assertEqual(dst.read_bytes(), patch(page(nl="\r\n")).encode(), "bytes as patch() gives them (no newline translation)")
        self.assertEqual(src.read_bytes(), page(nl="\r\n").encode(), "IN untouched")

    def test_in_place_and_again(self):
        f = self.tmp / "index.html"
        f.write_bytes(page().encode())
        self.assertEqual(self.run_tool(f, f).returncode, 0)
        once = f.read_bytes()
        self.assertEqual(self.run_tool(f, f).returncode, 0)
        self.assertEqual(f.read_bytes(), once)
        self.assertEqual(sorted(p.name for p in self.tmp.iterdir()), ["index.html"], "no temp file left")

    def test_bad_page_exit_1_writes_nothing(self):
        src, dst = self.tmp / "new.html", self.tmp / "index.html"
        src.write_bytes(page(head_end="").encode())
        dst.write_bytes(b"the old page")
        r = self.run_tool(src, dst)
        self.assertEqual(r.returncode, 1)
        self.assertIn("</head>", r.stderr)
        self.assertEqual(dst.read_bytes(), b"the old page")
        self.assertEqual(sorted(p.name for p in self.tmp.iterdir()), ["index.html", "new.html"])

    def test_missing_in(self):
        r = self.run_tool(self.tmp / "nope.html", self.tmp / "out.html")
        self.assertEqual(r.returncode, 1)
        self.assertIn("nope.html", r.stderr)
        self.assertFalse((self.tmp / "out.html").exists())

    def test_not_utf8(self):
        src = self.tmp / "bad.html"
        src.write_bytes(page().encode() + b"\xff\xfe")
        r = self.run_tool(src, self.tmp / "out.html")
        self.assertEqual(r.returncode, 1)
        self.assertFalse((self.tmp / "out.html").exists())

    def test_out_dir_missing(self):
        src = self.tmp / "s.html"
        src.write_bytes(page().encode())
        r = self.run_tool(src, self.tmp / "no" / "such" / "out.html")
        self.assertEqual(r.returncode, 1)

    def test_usage(self):
        for args in ((), ("one",), ("a", "b", "c")):
            r = self.run_tool(*args)
            self.assertEqual(r.returncode, 2, args)
            self.assertIn("usage", r.stderr)

    def test_main_in_process(self):
        src, dst = self.tmp / "s.html", self.tmp / "d.html"
        src.write_bytes(page().encode())
        self.assertEqual(patch_editor.main([str(src), str(dst)]), 0)
        self.assertEqual(dst.read_text(encoding="utf-8"), patch(page()))


if __name__ == "__main__":
    os.chdir(REPO)
    unittest.main(verbosity=2)
