#!/usr/bin/env python3
"""The WiFi joystick's numbers in the docs match the code they describe (stdlib only):

    python host/tests/test_wifi_docs.py

The request rules' restart tries and their spacing (R6: WIFI_RESTART_TRIES sends in all, WIFI_THROTTLE_US
apart; the docs must not count the sends after the first as the total), the lock timeout and what a pause in
the stream does (R11: the mixer counts the WiFi source as live for INPUT_STALE_US, 0.5 s, like every source,
then the game pauses in flight; the lock is released after WIFI_SOURCE_TIMEOUT_US, 3 s; holding the sticks
until then is the joystick apps' rule, not StickTime Pi's), the wait for a Crossfire beacon (WIFI_XF_BEACON_US)
and the tables (WIFI_TABLE), in docs/USER_GUIDE.md, docs/WEB_API.md and docs/RESEARCH.md. The constants come
from the vendored core's wifi_proto.h and host/src/input.h."""
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
                  "WIFI_TABLE"):
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


if __name__ == "__main__":
    unittest.main()
