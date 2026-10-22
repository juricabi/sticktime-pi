#!/usr/bin/env python3
"""tools/vendor_elrs_wifi.sh: copies the WiFi joystick core into host/third_party/elrs-wifi/ and checks it.

Made-up upstream and StickTime Pi repositories in a temp directory (git and sh needed, no network): a
clean vendoring records upstream's commit and passes --check, also in a fresh clone; --check finds an
edited, missing or extra file; the copy is the committed bytes even from a CRLF checkout (Git for Windows'
core.autocrlf), so a fresh clone still matches; a file committed upstream with CRLF (which this repo would
store as LF) is refused; a copy of uncommitted upstream changes says -dirty and
fails --check; a source that is no git checkout of its own (upstream's files copied into another work
tree, e.g. .local/ here) is refused instead of recording that repository's commit.

    python host/tests/test_vendor_elrs_wifi.py
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
SCRIPT = REPO / "tools" / "vendor_elrs_wifi.sh"
DIR = "host/third_party/elrs-wifi"
sys.dont_write_bytecode = True                  # (no __pycache__ in the repo)

# upstream path -> (path here, content)
FILES = {
    "core/wifi_proto.h": (f"{DIR}/wifi_proto.h", "/* proto */\n#define X 1\n"),
    "core/wifi_proto.c": (f"{DIR}/wifi_proto.c", "#include \"wifi_proto.h\"\nint x = X;\n"),
    "core/wifi_net.h": (f"{DIR}/wifi_net.h", "/* net */\n"),
    "core/wifi_net.c": (f"{DIR}/wifi_net.c", "#include \"wifi_net.h\"\n"),
    "core/tests/t.h": (f"{DIR}/tests/t.h", "/* t */\n"),
    "core/tests/test_wifi_proto.c": (f"{DIR}/tests/test_wifi_proto.c", "int main(void) { return 0; }\n"),
    "core/tests/test_wifi_net.c": (f"{DIR}/tests/test_wifi_net.c", "int main(void) { return 0; }\n"),
    "LICENSE": (f"{DIR}/LICENSE", "MIT License\n\nCopyright (c) juricabi\n"),
    "tests/fake_elrs.py": ("host/tests/fake_elrs.py", "#!/usr/bin/env python3\nprint('fake module')\n"),
}
# git without the user's or the system's configuration (signing, hooks, autocrlf...)
ENV = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_AUTHOR_NAME="test",
           GIT_AUTHOR_EMAIL="test@example.com", GIT_COMMITTER_NAME="test", GIT_COMMITTER_EMAIL="test@example.com")


def git(cwd, *args):
    return subprocess.run(["git", "-C", str(cwd), *args], check=True, env=ENV, capture_output=True,
                          text=True).stdout.strip()


def commit_all(repo, msg):
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", msg)
    return git(repo, "rev-parse", "HEAD")


class VendorTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = pathlib.Path(self._tmp.name)
        self.up = self.make_upstream(self.tmp / "elrs-crossfire-wifi-joystick-linux")
        self.up_head = git(self.up, "rev-parse", "HEAD")
        self.pi = self.make_pi(self.tmp / "sticktime-pi")

    def tearDown(self):
        self._tmp.cleanup()

    @staticmethod
    def write_files(root):
        for frm, (_, text) in FILES.items():
            p = root / frm
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(text.encode())
        (root / "tests/fake_elrs.py").chmod(0o755)

    def make_upstream(self, root):
        root.mkdir()
        git(root, "init", "-q")
        self.write_files(root)
        commit_all(root, "core")
        return root

    def make_pi(self, root):
        (root / "tools").mkdir(parents=True)
        (root / DIR).mkdir(parents=True)
        (root / "host/tests").mkdir(parents=True)
        shutil.copy(SCRIPT, root / "tools/vendor_elrs_wifi.sh")
        shutil.copy(REPO / DIR / "README.md", root / DIR / "README.md")
        shutil.copy(REPO / ".gitattributes", root / ".gitattributes")
        (root / ".gitignore").write_text(".local/\n")
        git(root, "init", "-q")
        commit_all(root, "pi")
        return root

    def run_script(self, *args, cwd=None):
        return subprocess.run(["sh", "tools/vendor_elrs_wifi.sh", *args], cwd=cwd or self.pi, env=ENV,
                              capture_output=True, text=True)

    def vendor_ok(self, src):
        r = self.run_script(str(src))
        self.assertEqual(r.returncode, 0, r.stderr)
        return r

    def vendor_line(self, root=None):
        text = ((root or self.pi) / DIR / "VENDOR").read_text()
        return [ln for ln in text.splitlines() if ln.startswith("commit ")]

    def check(self, cwd=None):
        return self.run_script("--check", cwd=cwd)

    def fresh_clone(self):
        commit_all(self.pi, "vendored")
        clone = self.tmp / "clone"
        git(self.tmp, "clone", "-q", str(self.pi), str(clone))
        return clone

    def assert_copied_from_commit(self):
        for _, (to, text) in FILES.items():
            self.assertEqual((self.pi / to).read_bytes(), text.encode(), to)

    # ---------------------------------------------------------------- tests
    def test_clean_vendoring_passes_check_also_in_a_fresh_clone(self):
        r = self.vendor_ok(self.up)
        self.assertIn(f"vendored commit {self.up_head}", r.stdout)
        self.assertNotIn("WARNING", r.stderr)
        self.assertEqual(self.vendor_line(), [f"commit {self.up_head}"])
        self.assertIn(f"| Commit | `{self.up_head}`", (self.pi / DIR / "README.md").read_text())
        self.assert_copied_from_commit()
        if os.name == "posix":
            self.assertTrue(os.access(self.pi / "host/tests/fake_elrs.py", os.X_OK), "the mode is kept")
        r = self.check()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f"matches VENDOR (9 files, commit {self.up_head})", r.stdout)
        r = self.check(cwd=self.fresh_clone())
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_check_finds_edited_missing_and_extra_files(self):
        self.vendor_ok(self.up)
        p = self.pi / DIR / "wifi_proto.c"
        p.write_bytes(p.read_bytes() + b"/* local fix */\n")
        r = self.check()
        self.assertEqual(r.returncode, 1)
        self.assertIn(f"changed: {DIR}/wifi_proto.c (edit it upstream", r.stderr)
        self.vendor_ok(self.up)
        (self.pi / DIR / "tests/t.h").unlink()
        (self.pi / DIR / "extra.c").write_text("int y;\n")
        r = self.check()
        self.assertEqual(r.returncode, 1)
        self.assertIn(f"missing: {DIR}/tests/t.h", r.stderr)
        self.assertIn(f"not in VENDOR: {DIR}/extra.c", r.stderr)
        self.vendor_ok(self.up)                          # vendoring again removes what upstream doesn't have
        self.assertFalse((self.pi / DIR / "extra.c").exists())
        self.assertEqual(self.check().returncode, 0)

    def test_crlf_checkout_vendors_the_committed_bytes(self):
        # Git for Windows: core.autocrlf=true, and upstream has no .gitattributes: CRLF in the work tree
        crlf = self.tmp / "crlf-checkout"
        git(self.tmp, "clone", "-q", "-c", "core.autocrlf=true", str(self.up), str(crlf))
        self.assertIn(b"\r\n", (crlf / "core/wifi_proto.c").read_bytes())
        self.assertEqual(git(crlf, "status", "--porcelain"), "")
        r = self.vendor_ok(crlf)
        self.assertNotIn("WARNING", r.stderr)
        self.assertEqual(self.vendor_line(), [f"commit {self.up_head}"])
        self.assert_copied_from_commit()
        self.assertEqual(self.check().returncode, 0)
        # this repo stores LF (.gitattributes): every other checkout, CI's too, still matches VENDOR
        r = self.check(cwd=self.fresh_clone())
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_crlf_committed_upstream_is_refused(self):
        # committed with CRLF (core.autocrlf off): this repo's .gitattributes stores it as LF, so a fresh clone
        # (CI) would not match the SHA-256 of the CRLF bytes although the local --check passes
        p = self.up / "core/wifi_proto.c"
        p.write_bytes(p.read_bytes().replace(b"\n", b"\r\n"))
        commit_all(self.up, "CRLF")
        before = (self.pi / DIR / "README.md").read_bytes()
        r = self.run_script(str(self.up))
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("core/wifi_proto.c is committed with CRLF line ends", r.stderr)
        self.assertFalse((self.pi / DIR / "VENDOR").exists())
        self.assertEqual((self.pi / DIR / "README.md").read_bytes(), before)
        self.assertFalse((self.pi / DIR / "wifi_proto.h").exists(), "nothing copied")
        # committed with LF again: fine
        p.write_bytes(p.read_bytes().replace(b"\r\n", b"\n"))
        commit_all(self.up, "LF")
        self.vendor_ok(self.up)
        r = self.check(cwd=self.fresh_clone())
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_dirty_source_is_marked_and_fails_check(self):
        p = self.up / "core/wifi_proto.c"
        p.write_bytes(p.read_bytes() + b"int not_committed;\n")
        r = self.vendor_ok(self.up)
        self.assertIn("WARNING", r.stderr)
        self.assertEqual(self.vendor_line(), [f"commit {self.up_head}-dirty"])
        self.assertIn(b"int not_committed;", (self.pi / DIR / "wifi_proto.c").read_bytes())
        r = self.check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("vendored from uncommitted upstream changes", r.stderr)
        # committed upstream and vendored again: fine
        head = commit_all(self.up, "the change")
        self.vendor_ok(self.up)
        self.assertEqual(self.vendor_line(), [f"commit {head}"])
        self.assertEqual(self.check().returncode, 0)

    def test_check_refuses_a_commit_that_is_no_commit_id(self):
        self.vendor_ok(self.up)
        v = self.pi / DIR / "VENDOR"
        v.write_text(v.read_text().replace(f"commit {self.up_head}", "commit main"))
        readme = self.pi / DIR / "README.md"
        readme.write_text(readme.read_text().replace(f"`{self.up_head}`", "`main`"))
        r = self.check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("'main' is not a commit id", r.stderr)

    def test_copy_inside_another_work_tree_is_refused(self):
        # upstream's files without .git, somewhere in this repo's work tree (an ignored .local/)
        nested = self.pi / ".local" / "elrs-crossfire-wifi-joystick-linux"
        self.write_files(nested)
        before = (self.pi / DIR / "README.md").read_bytes()
        r = self.run_script(".local/elrs-crossfire-wifi-joystick-linux")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("is not a git checkout of its own", r.stderr)
        self.assertFalse((self.pi / DIR / "VENDOR").exists())
        self.assertEqual((self.pi / DIR / "README.md").read_bytes(), before)
        self.assertFalse((self.pi / DIR / "wifi_proto.c").exists())

    def test_no_git_checkout_is_refused(self):
        plain = self.tmp / "plain"
        self.write_files(plain)
        r = self.run_script(str(plain))
        self.assertEqual(r.returncode, 1)
        self.assertIn("is not a git checkout", r.stderr)
        self.assertFalse((self.pi / DIR / "VENDOR").exists())


if __name__ == "__main__":
    if not shutil.which("git") or not shutil.which("sh"):
        print("SKIP: needs git and sh")
        sys.exit(0)
    unittest.main(verbosity=2)
