#!/usr/bin/env python3
"""The WiFi joystick's numbers in the docs match the code they describe (stdlib only):

    python host/tests/test_wifi_docs.py

The request rules' restart tries and their spacing (R6: WIFI_RESTART_TRIES sends in all, WIFI_THROTTLE_US
apart; the docs must not count the sends after the first as the total), the lock timeout and what a pause in
the stream does (R11: the mixer counts the WiFi source as live for INPUT_STALE_US, 0.5 s, like every source,
then the game pauses in flight; the lock is released after WIFI_SOURCE_TIMEOUT_US, 3 s; holding the sticks
until then is the joystick apps' rule, not StickTime Pi's), the wait for a Crossfire beacon (WIFI_XF_BEACON_US),
the tables (WIFI_TABLE) and the release (R14: the exit waits WIFI_RELEASE_WAIT_US for the answers, counted from
the stop or Release that sent them; a release's POST gets RELEASE_POST_MS; modules activated or heard less than
WIFI_SOURCE_TIMEOUT_US ago; the web page's Release lasts until Connect or the program's next start, nothing is
saved), in docs/USER_GUIDE.md, docs/WEB_API.md, docs/RESEARCH.md and the web page. The constants come from the
vendored core's wifi_proto.h, host/src/input.h and host/src/input_wifi.c."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def defines(path):
    text = (ROOT / path).read_text(encoding="utf-8")
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"^#define\s+(\w+)\s+(\d+)\b", text, re.M)}


CORE = defines("host/third_party/elrs-wifi/wifi_proto.h")
INPUT = defines("host/src/input.h")


def doc(name):
    """the doc's text with line breaks and indents as single spaces (sentences wrap)"""
    return re.sub(r"\s+", " ", (ROOT / "docs" / name).read_text(encoding="utf-8"))


def seconds(us):
    s = us / 1e6
    return f"{s:g}"


class Numbers(unittest.TestCase):
    def find(self, name, pattern):
        m = re.search(pattern, doc(name))
        self.assertIsNotNone(m, f"{name}: no match for {pattern!r}")
        return m.groups()

    def test_constants(self):
        for k in ("WIFI_RESTART_TRIES", "WIFI_THROTTLE_US", "WIFI_SOURCE_TIMEOUT_US", "WIFI_XF_BEACON_US",
                  "WIFI_TABLE", "WIFI_RELEASE_WAIT_US"):
            self.assertIn(k, CORE)
        self.assertIn("INPUT_STALE_US", INPUT)
        # every source, the WiFi joystick too, goes stale after INPUT_STALE_US: no separate WiFi hold time
        self.assertNotIn("INPUT_STALE_WIFI_US", INPUT)
        self.assertLess(INPUT["INPUT_STALE_US"], CORE["WIFI_SOURCE_TIMEOUT_US"])

    def test_restart_tries(self):
        """R6: 3 sends in all, 5 s apart"""
        tries, gap = str(CORE["WIFI_RESTART_TRIES"]), seconds(CORE["WIFI_THROTTLE_US"])
        self.assertEqual(self.find("USER_GUIDE.md", r"tries the stop and start (\d+) times in all, (\d+) s apart"),
                         (tries, gap))
        self.assertEqual(self.find("WEB_API.md", r"A failed one is sent again every (\d+) s while that module stays "
                                                 r"locked and is not known as Crossfire, (\d+) sends per change at most"),
                         (gap, tries))
        self.assertEqual(self.find("RESEARCH.md", r"A failed one is sent again every (\d+) s \(the throttle\) while "
                                                  r"that module is still locked and not known as Crossfire, at most "
                                                  r"(\d+) sends per request change"),
                         (gap, tries))
        # "sends them again ..., three times in all" reads as three more after the first
        for name in ("USER_GUIDE.md", "WEB_API.md", "RESEARCH.md"):
            for sentence in re.split(r"(?<=[.:;])\s", doc(name)):
                if re.search(r"\b(sends?|sent|tries|retried)\b[^.]*\bagain\b", sentence):
                    self.assertNotRegex(sentence, r"\b(three|\d+) times in all\b", f"{name}: {sentence}")

    def test_lock_and_pause(self):
        """R11 here: not live after 0.5 s (centered, throttle low: the game pauses in flight); the lock goes after
        3 s. Holding the sticks until the lock loss is the joystick apps' rule only."""
        lock, stale = seconds(CORE["WIFI_SOURCE_TIMEOUT_US"]), seconds(INPUT["INPUT_STALE_US"])
        self.assertEqual(self.find("USER_GUIDE.md", r"the game pauses \(the WiFi joystick: after ([\d.]+) s without "
                                                    r"stick data\)"), (stale,))
        self.assertEqual(self.find("USER_GUIDE.md", r"Once the stream has stopped for ([\d.]+) s, the game gets no "
                                                    r"sticks from it"), (stale,))
        self.assertTrue("the sticks center with the throttle at the bottom, and in flight the game pauses"
                        in doc("USER_GUIDE.md"), "USER_GUIDE.md: what a stopped stream does to the sticks")
        self.assertEqual(self.find("USER_GUIDE.md", r"After (\d+) s the Pi lets go of the module"), (lock,))
        self.assertEqual(self.find("WEB_API.md", r"it was updated within ([\d.]+) s, the WiFi joystick too"),
                         (stale,))
        self.assertEqual(self.find("WEB_API.md", r"its lock is released after (\d+) s"), (lock,))
        self.assertEqual(self.find("WEB_API.md", r"Gaps hold the last values while the WiFi source still counts as "
                                                 r"`live` \(([\d.]+) s, like every source\)"), (stale,))
        self.assertEqual(self.find("WEB_API.md", r"Lock loss \((\d+) s without data\)"), (lock,))
        self.assertEqual(self.find("RESEARCH.md", r"Lock loss \((\d+) s without data\), stop, exit"), (lock,))
        self.assertEqual(self.find("RESEARCH.md", r"Holding the sticks through a gap of up to (\d+) s \(until the "
                                                  r"lock loss\) is the joystick apps' rule"), (lock,))
        self.assertEqual(self.find("RESEARCH.md", r"StickTime Pi pauses the game after ([\d.]+) s instead"), (stale,))
        self.assertEqual(self.find("RESEARCH.md", r"as live only for ([\d.]+) s after its last update "
                                                  r"\(`INPUT_STALE_US`\)"), (stale,))
        self.assertEqual(self.find("RESEARCH.md", r"the lock is kept until the lock loss after (\d+) s"), (lock,))
        self.assertTrue("short gaps hold the last values" in doc("RESEARCH.md"), "RESEARCH.md: short gaps")
        # nothing may say StickTime Pi holds the sticks until the lock loss
        for name in ("USER_GUIDE.md", "WEB_API.md", "RESEARCH.md", "PLAN.md"):
            text = doc(name)
            self.assertNotIn("INPUT_STALE_WIFI_US", text, name)
            self.assertNotRegex(text, r"(?i)\bhold(s|ing)? the sticks\b(?! through a gap of up to \d+ s \(until the lock "
                                      r"loss\) is the joystick apps' rule)", name)
            self.assertNotRegex(text, r"the WiFi joystick within \d+ s", name)
            self.assertNotRegex(text, r"stays `live` until the lock loss", name)

    def test_crossfire_wait_and_tables(self):
        xf, table = seconds(CORE["WIFI_XF_BEACON_US"]), str(CORE["WIFI_TABLE"])
        self.assertEqual(self.find("USER_GUIDE.md", r"up to (\d+) s after the stream started coming in"), (xf,))
        self.assertEqual(self.find("WEB_API.md", r"held until the lock is (\d+) s old"), (xf,))
        self.assertEqual(self.find("WEB_API.md", r"remembers what it last asked each module \((\d+) at most"), (table,))
        self.assertEqual(self.find("RESEARCH.md", r"Tables are bounded \((\d+) modules\)"), (table,))

    def test_release(self):
        """R14: the exit waits at most 1.5 s for the answers; the 3 s windows; the rules are R1-R14 now, and no doc
        says StickTime Pi never sends joystick_end when it stops listening"""
        wait, window = seconds(CORE["WIFI_RELEASE_WAIT_US"]), seconds(CORE["WIFI_SOURCE_TIMEOUT_US"])
        self.assertEqual(self.find("RESEARCH.md", r"an exit waits for their answers at most ([\d.]+) s "
                                                  r"\(`WIFI_RELEASE_WAIT_US`\)"), (wait,))
        self.assertEqual(self.find("RESEARCH.md", r"we drop its frames \(one came less than (\d+) s ago"), (window,))
        self.assertEqual(self.find("RESEARCH.md", r"then to every module activated less than (\d+) s ago that has not "
                                                  r"locked since"), (window,))
        self.assertEqual(self.find("WEB_API.md", r"The exit waits for the answers at most ([\d.]+) s"), (wait,))
        self.assertEqual(self.find("WEB_API.md", r"to modules activated less than (\d+) s ago that have not locked "
                                                 r"since"), (window,))
        self.assertEqual(self.find("USER_GUIDE.md", r"sends the module the stop the same way and waits at most ([\d.]+) s "
                                                    r"for its answer"), (wait,))
        for name in ("USER_GUIDE.md", "WEB_API.md", "RESEARCH.md", "PLAN.md"):
            text = doc(name)
            self.assertNotIn("R1-R13", text, name)
            self.assertNotRegex(text, r"never when it stops listening", name)
            self.assertNotRegex(text, r"keeps broadcasting until powered off", name)
        self.assertIn("R1-R14", doc("RESEARCH.md"))
        self.assertIn("R1-R14", doc("WEB_API.md"))

    def test_release_post_and_wait(self):
        """R14 here: a release's POST gets RELEASE_POST_MS (input_wifi.c: the worker sends one request at a time),
        and the exit's 1.5 s count from the stop or Release that handed the releases over, as in the Linux app"""
        post = defines("host/src/input_wifi.c")["RELEASE_POST_MS"]
        self.assertEqual(post % 1000, 0, "whole seconds: the core's text says them")
        post, wait = seconds(post * 1000), seconds(CORE["WIFI_RELEASE_WAIT_US"])
        self.assertGreater(float(post), float(wait), "the exit's wait ends first")
        self.assertEqual(self.find("WEB_API.md", r"A module gets (\d+) s to answer: the requests go out one at a time"),
                         (post,))
        self.assertEqual(self.find("WEB_API.md", r"\(`timed out after (\d+) s`"), (post,))
        self.assertEqual(self.find("RESEARCH.md", r"a module gets (\d+) s to answer its release \(`timed out after (\d+) s`\)"),
                         (post, post))
        self.assertEqual(self.find("WEB_API.md", r"The exit waits for the answers at most ([\d.]+) s after the Release "
                                                 r"or stop that sent them"), (wait,))
        self.assertEqual(self.find("RESEARCH.md", r"The apps count the ([\d.]+) s from the last stop that handed "
                                                  r"releases over"), (wait,))

    def test_release_ends_at_next_start(self):
        """StickTime Pi's Release lasts until Connect or the program's next start: nothing is saved (no release file
        in the code), the docs and the page say so, and none says a restart keeps it"""
        for f in ("host/src/input_wifi.h", "host/src/input_wifi.c", "host/src/host.c", "host/src/web_api.c"):
            self.assertNotRegex((ROOT / f).read_text(encoding="utf-8"), r"wifi-released|WIFI_RELEASED_FILE|kept_released", f)
        self.assertIn("It lasts until Connect or the program's next start (nothing is saved)", doc("WEB_API.md"))
        self.assertIn("stops listening until **Connect** or the program's next start", doc("RESEARCH.md"))
        self.assertIn("The release lasts until you press **Connect** there or StickTime Pi starts again", doc("USER_GUIDE.md"))
        page = re.sub(r"\s+", " ", (ROOT / "web/index.html").read_text(encoding="utf-8") +
                      (ROOT / "web/app.js").read_text(encoding="utf-8"))
        self.assertIn("until you press Connect or StickTime Pi starts again", page)
        for text, where in [(doc(n), n) for n in ("USER_GUIDE.md", "WEB_API.md", "RESEARCH.md", "PLAN.md")] + [(page, "web/")]:
            self.assertNotRegex(text, r"wifi-released|lasts the boot|keeps the release|keeps it released|"
                                      r"still released|boots again: a restart", where)


if __name__ == "__main__":
    unittest.main()
