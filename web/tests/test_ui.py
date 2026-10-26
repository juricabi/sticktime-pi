#!/usr/bin/env python3
"""The settings app (web/index.html, app.js, style.css) in headless Chromium against the mock Pi
(web/tests/mock_server.py, a fresh one per test), at http://sticktime.local like on the Pi (not a
secure context).

Covered: every tab on a desktop (light) and a phone (390x844, dark) without console errors, without
sideways scrolling, with screenshots in build/agent-webui/shots/; forms built from the settings'
metadata (kinds, choices, help, live/reboot/restart flags, fields that depend on others, keys a newer
Pi adds); saving only the changed keys in settings format; per-field validation errors (in the page
and from the Pi); secrets as set/not set with change/clear; the sticks' bars over SSE, by polling and
by polling when the stream fails; songs (several at once, progress, odd names, replace, refusals,
delete, next); the keypad and the keyboard on the Game page, live view, the script picker; WiFi scan
(busy first, names as text), join (validation, hidden, open, the Pi's own field errors, the
connection afterwards), the module's WiFi; updates (check, notes, install, roll back, the app
update reloading the page, errors); the System page (log, settings file view/edit/apply/download,
restart, reboot, shut down); the offline banner; themes; the Tracks page; the Status page (with the WiFi
joystick's Release and Connect); labels for every control; garbage or failing API answers. Every mutating request carries X-StickTime: 1.

    python web/tests/test_ui.py              (or: python -m pytest web/tests/test_ui.py)
    STP_BROWSER=firefox|webkit python web/tests/test_ui.py         (see browsers.py)
"""
import json
import pathlib
import re
import sys
import threading
import time
import unittest

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
SHOTS = REPO / "build" / "agent-webui" / "shots"
sys.dont_write_bytecode = True                  # (no __pycache__ in the repo)
sys.path.insert(0, str(HERE))
import browsers  # noqa: E402
import mock_server  # noqa: E402
from playwright.sync_api import sync_playwright  # noqa: E402

HOST = browsers.HOST
# how each browser logs a request that failed or was refused (allowed only where a test makes that happen)
NET_ERROR = re.compile(r"^Failed to load resource|can.t establish a connection to the server|^NetworkError|"
                       r"^\[JavaScript Error: \"(The connection to .* was interrupted|Firefox can.t establish)")
SUFFIX = "" if browsers.NAME == "chromium" else "-" + browsers.NAME   # screenshots of other browsers beside
TABS = ["status", "sticks", "video", "wifi", "sound", "tracks", "game", "updates", "system"]
DESKTOP = {"width": 1280, "height": 900}
MOBILE = {"width": 390, "height": 844}
# what each tab shows once its data is in
LOADED = {
    "status": "document.querySelector('[data-f=fps]').textContent !== '–' && !document.getElementById('shotImg').hidden",
    "sticks": "document.querySelectorAll('#srcCards .src-card').length === 2",
    "video": "document.querySelectorAll('form[data-form=video] .field').length === 11",
    "wifi": "document.querySelectorAll('#netList li').length === 4",
    "sound": "document.querySelectorAll('#songList li .name').length >= 2",
    "tracks": "document.querySelectorAll('#slotList li').length === 8",
    "game": "document.querySelectorAll('#toolList label').length >= 2 && !document.getElementById('remoteImg').hidden",
    "updates": "document.getElementById('updMsg').textContent !== '–'",
    "system": "document.getElementById('logText').textContent.includes('sticktime-pi') && document.querySelector('[data-s=model]').textContent !== '–'",
}


class UiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pw = sync_playwright().start()
        cls.br = browsers.launch(cls.pw)
        SHOTS.mkdir(parents=True, exist_ok=True)

    @classmethod
    def tearDownClass(cls):
        cls.br.close()
        cls.pw.stop()

    def setUp(self):
        self.pi = mock_server.MockPi(scan_busy=0)
        self.srv, port = mock_server.serve(self.pi)
        self.base = f"http://{HOST}:{port}/"
        self.errors, self.requests = [], []
        self.net_errors_ok = False                 # tests that make the Pi refuse or vanish on purpose
        self.ctx = None

    def tearDown(self):
        try:
            if self.ctx:
                self.ctx.close()
        finally:
            self.srv.shutdown()
            self.srv.server_close()
            self.pi.close()
        bad = [e for e in self.errors if not (self.net_errors_ok and NET_ERROR.search(e))]
        self.assertEqual(bad, [], "no errors on the page")
        for r in self.pi.requests:
            if r["method"] != "GET":
                self.assertEqual(r["headers"].get("x-sticktime"), "1", f"{r['method']} {r['path']} without X-StickTime: 1")

    # ---------------------------------------------------------------- helpers
    def open(self, tab="status", mobile=False, dark=False, storage=None):
        opts = {"viewport": MOBILE if mobile else DESKTOP, "color_scheme": "dark" if dark else "light", "accept_downloads": True}
        if mobile:
            browsers.mobile(opts)
        self.ctx = self.br.new_context(**opts)
        if storage:
            self.ctx.add_init_script("try {" + "".join(f"localStorage.setItem({json.dumps(k)}, {json.dumps(v)});" for k, v in storage.items()) + "} catch (e) {}")
        p = self.page = self.ctx.new_page()
        p.on("console", lambda m: m.type == "error" and self.errors.append(m.text))
        p.on("pageerror", lambda e: self.errors.append("pageerror: " + str(e)))
        p.on("dialog", lambda d: (self.errors.append("a native dialog: " + d.message), d.dismiss()))   # the app has its own
        p.on("request", lambda r: self.requests.append(r.url))
        p.goto(self.base + "#" + tab)
        self.ready()
        return p

    def ready(self):
        self.page.wait_for_function("window.stp && stp.S.values && stp.status && stp.forms.every((f) => f.fields.size || f.spec === '*')", timeout=15000)

    def tab(self, name):
        self.page.click(f'#tabs a[data-page="{name}"]')
        self.page.wait_for_selector(f"#p-{name}", state="visible")

    def posts(self, path=None):
        with self.pi.lock:
            return [r for r in self.pi.requests if r["method"] != "GET" and (path is None or r["path"] == path)]

    def gets(self, path):
        with self.pi.lock:
            return [r for r in self.pi.requests if r["method"] == "GET" and r["path"] == path]

    def until(self, fn, timeout=8.0, what="the condition"):
        end = time.time() + timeout
        while True:
            v = fn()
            if v:
                return v
            if time.time() > end:
                self.fail("timed out waiting for " + what)
            self.page.wait_for_timeout(40)

    def confirm(self, yes=True, expect=None):
        p = self.page
        p.wait_for_selector("#confirmDlg[open]")
        if expect:
            self.assertIn(expect, p.inner_text("#confirmDlg"))
        p.click("#confirmYes" if yes else "#confirmNo")
        p.wait_for_selector("#confirmDlg", state="hidden")

    def toast(self, text):
        self.page.wait_for_function("(t) => [...document.querySelectorAll('#toasts .toast')].some((e) => e.textContent.includes(t))", arg=text)

    def shot(self, name):
        self.page.evaluate("window.scrollTo(0, 0)")                 # (sticky parts are drawn where they are now)
        self.page.screenshot(path=str(SHOTS / f"{name}{SUFFIX}.png"), full_page=True)

    def save(self, form):
        self.page.click(f'form[data-form="{form}"] .savebar button[type=submit]')

    def field(self, form, key):
        """the field's root (a div or a fieldset) by its help line's id"""
        return f'form[data-form="{form}"] .field:has(#f-{form}-{key}-help)'

    def no_sideways_scroll(self, what):
        w = self.page.evaluate("[document.documentElement.scrollWidth, window.innerWidth]")
        self.assertLessEqual(w[0], w[1], f"{what}: the page scrolls sideways ({w[0]} > {w[1]})")

    # ---------------------------------------------------------------- every tab
    def tabs(self, mobile, dark, suffix):
        # dark (the game's colors) is the default whatever the device prefers; light is the theme button's choice
        p = self.open("status", mobile=mobile, dark=dark, storage=None if dark else {"stp.theme": "light"})
        bg = p.evaluate("getComputedStyle(document.body).backgroundColor")
        lum = sum(int(x) for x in re.findall(r"\d+", bg)[:3]) / 765
        self.assertTrue(lum < 0.3 if dark else lum > 0.7, f"{'dark' if dark else 'light'} theme background: {bg}")
        for t in TABS:
            self.tab(t)
            p.wait_for_function(LOADED[t], timeout=10000)
            self.assertEqual(p.get_attribute(f'#tabs a[data-page="{t}"]', "aria-current"), "page")
            self.assertEqual(p.evaluate("[...document.querySelectorAll('h1')].filter((h) => h.offsetParent).length"), 1, t)
            self.assertTrue(p.title().startswith(p.inner_text(f"#h-{t}")), p.title())
            # its settings forms: a control for every key named in data-keys
            got = p.evaluate("(t) => stp.forms.filter((f) => f.spec !== '*' && f.el.closest('.page').dataset.page === t).map((f) => [f.spec, [...f.fields.keys()].join(',')])", t)
            for spec, keys in got:
                self.assertEqual(keys, spec, f"{t}: the form's fields")
            self.no_sideways_scroll(f"{t} ({suffix})")
            p.wait_for_timeout(150)                                     # (fonts and images settle)
            self.shot(f"{t}-{suffix}")

    def test_tabs_desktop_light(self):
        self.tabs(False, False, "desktop")

    def test_tabs_mobile_dark(self):
        self.tabs(True, True, "mobile")
        p = self.page
        # the tabs scroll sideways on a phone, the page doesn't; keys are big enough for a thumb
        self.assertGreater(p.evaluate("document.getElementById('tabs').scrollWidth"), MOBILE["width"])
        self.tab("game")
        sizes = p.eval_on_selector_all(".keypad [data-key]", "els => els.map((e) => [e.offsetWidth, e.offsetHeight])")
        self.assertTrue(all(w >= 44 and h >= 44 for w, h in sizes), sizes)

    # ---------------------------------------------------------------- settings forms
    def test_forms_follow_the_metadata(self):
        p = self.open("video")
        form = 'form[data-form="video"]'
        self.assertEqual(p.eval_on_selector_all(f'{form} input[name="f-video-video"]', "els => els.map((e) => e.value)"), ["av", "hdmi"])
        self.assertEqual(p.eval_on_selector("#f-video-tv", "e => [...e.options].map((o) => o.value)"), self.pi.keymap["tv"]["choices"])
        self.assertEqual(p.input_value("#f-video-tv"), "ntsc")
        self.assertEqual([p.get_attribute("#f-video-margin", a) for a in ("type", "min", "max", "value")], ["range", "0", "20", "0"])
        self.assertIn("TV standard of the composite output", p.inner_text("#f-video-tv-help"))
        flag = lambda key: p.text_content(f"{self.field('video', key)} .flag:not(.changed)")  # noqa: E731
        self.assertEqual([flag(k) for k in ("video", "aspect", "show_ip", "drawing")], ["after reboot", "restarts the game", "at once", "at once"])
        # Drawing: its three ways, Multi-core the default; it is there for HDMI too
        self.assertEqual(sorted(p.eval_on_selector_all(f'{form} input[name="f-video-drawing"]', "els => els.map((e) => e.value)")), ["gpu", "multi", "single"])
        self.assertTrue(p.is_checked(f'{form} input[name="f-video-drawing"][value="multi"]'))
        self.assertIn("Multi-core", p.inner_text(self.field("video", "drawing")))
        self.assertEqual(p.text_content(f"{self.field('video', 'fonts')} .flag:not(.changed)"), "restarts the game")
        # Picture detail: with composite, progressive (the default) or interlaced; the flicker filter
        # only with interlaced
        self.assertTrue(p.is_checked(f'{form} input[name="f-video-av_mode"][value="progressive"]'))
        self.assertTrue(p.is_checked(f'{form} input[name="f-video-av_detail"][value="high"]'))
        self.assertTrue(p.is_visible(self.field("video", "av_detail")) and p.is_hidden(self.field("video", "av_flicker")))
        p.check(f'{form} input[name="f-video-av_mode"][value="interlaced"]')
        self.assertTrue(p.is_visible(self.field("video", "av_detail")) and p.is_visible(self.field("video", "av_flicker")))
        self.assertTrue(p.is_checked(f'{form} input[name="f-video-av_flicker"][value="full"]'))
        p.click(f"{form} .savebar button:has-text('Undo')")
        self.assertTrue(p.is_visible(self.field("video", "av_detail")) and p.is_hidden(self.field("video", "av_flicker")))
        # HDMI: its mode shows, the composite fields go; the change is marked; Undo puts it back
        self.assertTrue(p.is_hidden(self.field("video", "hdmi_mode")) and p.is_visible(self.field("video", "tv")))
        p.check(f'{form} input[name="f-video-video"][value="hdmi"]')
        self.assertTrue(p.is_visible(self.field("video", "hdmi_mode")) and p.is_hidden(self.field("video", "tv")) and p.is_hidden(self.field("video", "av_mode"))
                        and p.is_hidden(self.field("video", "av_detail")) and p.is_visible(self.field("video", "drawing")))
        self.assertTrue(p.is_visible(f"{self.field('video', 'video')} .flag.changed"))
        self.assertEqual(p.inner_text(f"{form} .savebar span"), "1 change")
        p.click(f"{form} .savebar button:has-text('Undo')")
        self.assertTrue(p.is_hidden(f"{form} .savebar") and p.is_visible(self.field("video", "tv")))
        self.assertTrue(p.is_checked(f'{form} input[name="f-video-video"][value="av"]'))
        # other kinds: a channel list, a 4-way choice, a switch, check boxes, a password
        self.tab("sticks")
        self.assertEqual(p.eval_on_selector("#f-sticks-pause_channel", "e => [...e.options].map((o) => o.textContent)"), ["Off"] + [f"CH{i}" for i in range(1, 17)])
        self.assertEqual(p.input_value("#f-sticks-pause_channel"), "5")
        self.assertEqual(p.eval_on_selector_all('input[name="f-sticks-stick_mode"]', "els => els.map((e) => e.value + (e.checked ? '*' : ''))"), ["1", "2*", "3", "4"])
        self.assertEqual(p.get_attribute("#f-sticks-stick_menus", "role"), "switch")
        self.assertTrue(p.is_checked("#f-sticks-stick_menus"))
        self.assertEqual(p.eval_on_selector_all(f"{self.field('sticks', 'invert')} input[type=checkbox]", "els => els.map((e) => e.value)"), ["ail", "ele", "thr", "rud"])
        self.assertIn("= 250 Hz", p.inner_text(self.field("sticks", "wifi_interval_us")))
        p.fill("#f-sticks-wifi_interval_us", "2000")
        self.assertIn("= 500 Hz", p.inner_text(self.field("sticks", "wifi_interval_us")))
        p.fill("#f-sticks-wifi_interval_us", "6667")
        self.assertIn("= 150 Hz", p.inner_text(self.field("sticks", "wifi_interval_us")))
        # the WiFi module's rates as suggestions, with their Hz; the channel count right after it
        self.assertEqual(p.get_attribute("#f-sticks-wifi_interval_us", "list"), "f-sticks-wifi_interval_us-list")
        self.assertEqual(p.eval_on_selector_all("#f-sticks-wifi_interval_us-list option", "els => els.map((e) => e.value + ' ' + e.label)"),
                         ["1000 1000 Hz", "2000 500 Hz", "3000 333 Hz", "4000 250 Hz (default)", "5000 200 Hz", "6667 150 Hz",
                          "10000 100 Hz", "20000 50 Hz"])
        self.assertEqual(p.eval_on_selector("#f-sticks-wifi_channels", "e => [...e.options].map((o) => o.textContent)"),
                         [f"{i} (CH1-CH{i})" for i in range(4, 17)])
        self.assertEqual(p.input_value("#f-sticks-wifi_channels"), "16")
        keys = p.eval_on_selector_all('form[data-form="sticks"] .field', "els => els.map((e) => (e.querySelector('[id^=f-sticks-]') || {}).id)")
        self.assertEqual(keys[keys.index("f-sticks-wifi_interval_us") + 1], "f-sticks-wifi_channels")
        self.assertIn("Crossfire always sends 16", p.inner_text(self.field("sticks", "wifi_channels")))
        self.assertIn("restarts a streaming ELRS module", p.inner_text(self.field("sticks", "wifi_interval_us")))
        p.select_option("#f-sticks-wifi_channels", "8")
        self.assertTrue(p.is_visible(f"{self.field('sticks', 'wifi_channels')} .flag.changed"))
        self.save("sticks")
        self.until(lambda: self.posts("/api/settings"))
        self.assertEqual(self.posts("/api/settings")[-1]["body"], b"wifi_interval_us = 6667\nwifi_channels = 8\n")
        self.until(lambda: self.pi.value("wifi_channels") == "8")
        self.assertEqual(self.pi.value("wifi_interval_us"), "6667")

    def test_a_newer_pi_adds_keys(self):
        # keys this page doesn't know: under System, More settings, by their kind (an unknown kind: text)
        self.pi.keys += [{"key": "future_mode", "kind": "enum", "default": "a", "choices": ["a", "b", "c"], "live": True, "reboot": False,
                          "restart": False, "help": "A setting of a newer StickTime Pi"},
                         {"key": "future_color", "kind": "color", "default": "#ff8800", "choices": [], "live": False, "reboot": False,
                          "restart": False, "help": "A kind this page doesn't know"}]
        self.pi.keymap = {k["key"]: k for k in self.pi.keys}
        p = self.open("system")
        self.assertTrue(p.is_visible("#moreCard"))
        self.assertEqual(p.eval_on_selector_all("form[data-form=more] .field-title", "els => els.map((e) => e.firstChild.textContent)"), ["Future mode", "Future color"])
        self.assertEqual(p.input_value("#f-more-future_color"), "#ff8800")
        self.assertIn("A setting of a newer StickTime Pi", p.inner_text("#moreCard"))
        p.check('input[name="f-more-future_mode"][value="b"]')
        self.save("more")
        self.until(lambda: self.posts("/api/settings"))
        self.assertEqual(self.posts("/api/settings")[0]["body"], b"future_mode = b\n")
        self.assertEqual(self.pi.value("future_mode"), "b")

    def test_only_changed_keys_are_sent(self):
        p = self.open("video")
        self.net_errors_ok = True                                   # the game restarts: the Pi is away a moment
        p.select_option("#f-video-tv", "pal")
        p.check('input[name="f-video-show_ip"][value="on"]')
        self.save("video")
        self.until(lambda: self.posts("/api/settings"), what="the POST")
        r = self.posts("/api/settings")[0]
        self.assertEqual(r["body"], b"tv = pal\nshow_ip = on\n")
        self.assertTrue(r["headers"]["content-type"].startswith("text/plain"))
        self.assertEqual((self.pi.value("tv"), self.pi.value("show_ip")), ("pal", "on"))
        self.toast("Saved.")
        p.wait_for_selector("#rebootBanner", state="visible")
        self.assertIn("TV standard", p.inner_text("#rebootText"))
        # a key that restarts the game: the page waits for it to come back
        p.check('input[name="f-video-aspect"][value="16:9"]')
        self.save("video")
        p.wait_for_selector("#waitBox", state="visible")
        self.assertEqual(p.inner_text("#waitTitle"), "Restarting the game…")
        p.wait_for_selector("#waitBox", state="hidden", timeout=20000)
        self.assertEqual(self.posts("/api/settings")[1]["body"], b"aspect = 16:9\n")
        self.assertTrue(p.is_hidden('form[data-form="video"] .savebar'))
        self.assertTrue(p.is_checked('input[name="f-video-aspect"][value="16:9"]'))
        # the channel order is a list of all 24 orders; Enter in a text field submits the form
        self.tab("sticks")
        opts = p.eval_on_selector_all("#f-sticks-channel_order option", "els => els.map((e) => e.value)")
        self.assertEqual((len(opts), len(set(opts)), opts[:2]), (24, 24, ["AETR", "TAER"]))
        p.select_option("#f-sticks-channel_order", "TAER")
        p.press("#f-sticks-wifi_module_ip", "Enter")
        self.until(lambda: len(self.posts("/api/settings")) == 3)
        self.assertEqual(self.posts("/api/settings")[2]["body"], b"channel_order = TAER\n")
        self.until(lambda: p.input_value("#f-sticks-channel_order") == "TAER", what="the stored value")
        self.assertEqual(self.pi.value("channel_order"), "TAER")
        # the Sound form saves by itself, a slider once when it stops
        self.tab("sound")
        n = len(self.posts("/api/settings"))
        p.click("#f-sound-music")
        self.until(lambda: len(self.posts("/api/settings")) == n + 1)
        self.assertEqual(self.posts("/api/settings")[-1]["body"], b"music = on\n")
        p.eval_on_selector("#f-sound-volume", "e => { for (const v of [60, 50, 40]) { e.value = v; e.dispatchEvent(new Event('input', { bubbles: true })); } }")
        self.until(lambda: len(self.posts("/api/settings")) == n + 2)
        p.wait_for_timeout(600)
        self.assertEqual([r["body"] for r in self.posts("/api/settings")[n + 1:]], [b"volume = 40\n"], "one POST for the drag")
        self.assertEqual(p.inner_text(f"{self.field('sound', 'volume')} output"), "40%")

    def test_validation_errors(self):
        p = self.open("sticks")
        self.net_errors_ok = True                                   # the Pi's 400s
        # in the page: nothing is sent
        p.fill("#f-sticks-wifi_interval_us", "500")
        self.save("sticks")
        p.wait_for_selector("#f-sticks-wifi_interval_us-err:has-text('1000 to 100000')")
        p.fill("#f-sticks-wifi_interval_us", "4k")
        self.save("sticks")
        p.wait_for_selector("#f-sticks-wifi_interval_us-err:has-text('a whole number')")
        self.assertEqual(self.posts("/api/settings"), [])
        self.assertEqual(p.get_attribute("#f-sticks-wifi_interval_us", "aria-invalid"), "true")
        self.assertEqual(p.evaluate("document.activeElement.id"), "f-sticks-wifi_interval_us", "focus on the first bad field")
        # typing clears the error
        p.fill("#f-sticks-wifi_interval_us", "4000")
        self.assertEqual(p.inner_text("#f-sticks-wifi_interval_us-err"), "")
        # from the Pi: per field, nothing saved (not even the valid channel order sent with it)
        p.select_option("#f-sticks-channel_order", "TAER")
        p.fill("#f-sticks-wifi_module_ip", "10.0.0.300")
        self.save("sticks")
        p.wait_for_selector("#f-sticks-wifi_module_ip-err:has-text('an IP address like 10.0.0.1')")
        self.assertIn("invalid", p.get_attribute(self.field("sticks", "wifi_module_ip"), "class"))
        self.assertNotIn("invalid", p.get_attribute(self.field("sticks", "channel_order"), "class"))
        self.assertEqual(self.posts("/api/settings")[0]["body"], b"channel_order = TAER\nwifi_module_ip = 10.0.0.300\n")
        self.assertEqual((self.pi.value("channel_order"), self.pi.value("wifi_module_ip")), ("AETR", ""))
        self.assertTrue(p.is_visible('form[data-form="sticks"] .savebar'), "the edits stay")
        self.shot("sticks-errors-desktop")
        # a password the Pi can't use, in the page
        self.tab("wifi")
        p.click(f"{self.field('wifi', 'hotspot_password')} button:has-text('Change')")
        p.fill("#f-wifi-hotspot_password", "short")
        self.save("wifi")
        p.wait_for_selector("#f-wifi-hotspot_password-err:has-text('8 to 63 characters')")
        p.fill("#f-wifi-hotspot_password", "x" * 64)
        self.save("wifi")
        p.wait_for_selector("#f-wifi-hotspot_password-err:has-text('8 to 63 characters')")
        p.fill("#f-wifi-hotspot_password", "line\nbreak")                # an input drops it; a paste of control characters wouldn't
        self.assertEqual(len([r for r in self.posts("/api/settings") if b"hotspot" in r["body"]]), 0)
        # a host name the Pi refuses
        self.tab("system")
        p.fill("#f-system-hostname", "my_pi!")
        self.save("system")
        p.wait_for_selector("#f-system-hostname-err:has-text('letters, digits and dashes')")
        self.assertEqual(self.pi.value("hostname"), "sticktime")
        # the Pi down while saving: said in the form
        self.pi.fail["/api/settings"] = (500, "Internal", "Couldn't save the settings: Read-only file system")
        p.fill("#f-system-hostname", "racer")
        self.save("system")
        p.wait_for_selector("form[data-form=system] .form-err:has-text('Read-only file system')")

    def test_secrets(self):
        p = self.open("wifi")
        self.assertNotIn("hunter22", p.content(), "secrets never come to the page")
        self.assertNotIn("sticktime\"", p.evaluate("JSON.stringify(stp.S.values)").replace('"hostname":"sticktime"', ""))
        hp = self.field("wifi", "hotspot_password")
        self.assertEqual(p.inner_text(f"{hp} .state"), "Set")
        self.assertTrue(p.is_hidden("#f-wifi-hotspot_password"))
        # Clear: "<key> =" and "<key>_clear = 1"
        p.click(f"{hp} button:has-text('Clear')")
        self.assertEqual(p.inner_text(f"{hp} .state"), "Will be cleared")
        self.save("wifi")
        self.until(lambda: self.posts("/api/settings"))
        self.assertEqual(self.posts("/api/settings")[0]["body"], b"hotspot_password =\nhotspot_password_clear = 1\n")
        self.assertEqual(self.pi.value("hotspot_password"), "")
        p.wait_for_selector(f"{hp} .state:has-text('Not set')")
        # Set one (shown while typing on request), then it is just "Set" again
        self.tab("updates")
        gt = self.field("updates", "github_token")
        self.assertEqual(p.inner_text(f"{gt} .state"), "Not set")
        p.click(f"{gt} button:has-text('Set')")
        p.fill("#f-updates-github_token", "ghp_secret123")
        self.assertEqual(p.get_attribute("#f-updates-github_token", "type"), "password")
        p.click(f"{gt} button:has-text('Show')")
        self.assertEqual(p.get_attribute("#f-updates-github_token", "type"), "text")
        self.save("updates")
        self.until(lambda: len(self.posts("/api/settings")) == 2)
        self.assertEqual(self.posts("/api/settings")[1]["body"], b"github_token = ghp_secret123\n")
        p.wait_for_selector(f"{gt} .state:text-is('Set')")            # has-text would match "Not set" too
        self.assertTrue(p.is_hidden("#f-updates-github_token"))
        self.assertNotIn("ghp_secret123", p.content())
        # Change, then Cancel: nothing to save; an empty change isn't sent either
        p.click(f"{gt} button:has-text('Change')")
        # the settings read back after the save landing now (a slow Pi) leaves the field open
        p.evaluate("stp.forms.forEach((f) => { if (!f.autosave) f.render(); })")
        self.assertTrue(p.is_visible("#f-updates-github_token"), "a refresh closed the field opened with Change")
        p.fill("#f-updates-github_token", "other")
        p.click(f"{gt} button:has-text('Cancel')")
        self.assertTrue(p.is_hidden('form[data-form="updates"] .savebar'))
        p.click(f"{gt} button:has-text('Change')")
        self.assertTrue(p.is_hidden('form[data-form="updates"] .savebar'))
        # the settings file view leaves secrets out
        self.tab("system")
        txt = p.input_value("#fileText")
        self.assertIn("# github_token is set", txt)
        self.assertIn("# wifi_password is set", txt)
        self.assertNotIn("ghp_secret123", txt)

    # ---------------------------------------------------------------- sticks
    def bars(self, sel):
        return self.page.eval_on_selector_all(f"{sel} .bar output", "els => els.map((e) => +e.textContent)")

    def test_sticks_stream(self):
        p = self.open("sticks")
        p.wait_for_function(LOADED["sticks"])
        a = self.bars("#srcCards .src-card:nth-child(1)")
        p.wait_for_timeout(400)
        b = self.bars("#srcCards .src-card:nth-child(1)")
        self.assertEqual(len(a), 16)
        self.assertNotEqual(a, b, "the bars move")
        self.assertTrue(all(-1024 <= v <= 1024 for v in a + b))
        self.assertTrue(any("/api/sticks/stream" in u for u in self.requests), "SSE first")
        self.assertEqual(self.gets("/api/sticks"), [], "no polling while the stream runs")
        # the USB radio has 8 channels: the rest greyed out; the source in use is marked
        off = p.eval_on_selector_all("#srcCards .src-card:nth-child(2) .bar", "els => els.map((e) => e.classList.contains('off'))")
        self.assertEqual(off, [False] * 8 + [True] * 8)
        self.assertTrue(p.is_visible("#srcCards .src-card:nth-child(1) .pill.ok") and p.is_hidden("#srcCards .src-card:nth-child(2) .pill.ok"))
        self.assertIn("EdgeTX RM TX16S Joystick", p.inner_text("#srcCards .src-card:nth-child(2) h2"))
        # what the game gets: 4 sticks + pause, the mode-2 gimbals
        m = self.bars("#mixBars")
        self.assertEqual(len(m), 5)
        self.assertEqual(m[4] in (-1024, 1024), True)
        self.assertEqual(p.inner_text("#gimL .glabel"), "throttle · yaw")
        self.assertRegex(p.get_attribute("#gimR .gdot", "style"), r"translate\(-?[\d.]+px, ?-?[\d.]+px\)")
        self.assertEqual(p.get_attribute("#mixBars .bar", "role"), "meter")
        # leaving the page closes the stream
        self.tab("video")
        p.wait_for_timeout(300)
        self.assertFalse(p.evaluate("!!document.querySelector('#p-sticks:not([hidden])')"))

    def test_sticks_polling(self):
        p = self.open("video", storage={"stp.nosse": "1"})
        self.tab("sticks")
        p.wait_for_function(LOADED["sticks"])
        n0 = len(self.gets("/api/sticks"))
        p.wait_for_timeout(1000)
        n1 = len(self.gets("/api/sticks"))
        self.assertGreaterEqual(n1 - n0, 10, "about 20 a second")
        self.assertLessEqual(n1 - n0, 25)
        a = self.bars("#mixBars")
        p.wait_for_timeout(300)
        self.assertNotEqual(a, self.bars("#mixBars"))
        self.tab("status")
        p.wait_for_timeout(200)
        n2 = len(self.gets("/api/sticks"))
        p.wait_for_timeout(600)
        self.assertLessEqual(len(self.gets("/api/sticks")) - n2, 1, "stops off the page")

    def test_sticks_stream_fails_then_polling(self):
        self.net_errors_ok = True
        p = self.open("status")
        p.route("**/api/sticks/stream", lambda r: r.abort())
        self.tab("sticks")
        self.until(lambda: len(self.gets("/api/sticks")) >= 5, timeout=8, what="polling after the stream failed")
        p.wait_for_function(LOADED["sticks"])

    STICK_SELS = ["#f-sticks-stick_channels", "#f-sticks-stick_channels-ele", "#f-sticks-stick_channels-thr", "#f-sticks-stick_channels-rud"]

    def stick_map(self):
        return [self.page.input_value(s) for s in self.STICK_SELS]

    def test_stick_mapping_field(self):
        p = self.open("sticks")
        f = self.field("sticks", "stick_channels")
        self.assertEqual(p.eval_on_selector(self.STICK_SELS[0], "e => [...e.options].map((o) => o.textContent)"), ["By order"] + [f"CH{i}" for i in range(1, 17)])
        self.assertEqual(p.eval_on_selector_all(f"{f} .chmap label > span", "els => els.map((e) => e.textContent)"), ["Aileron", "Elevator", "Throttle", "Yaw"])
        self.assertEqual(p.inner_text(f"{f} .field-title .lbl"), "Stick mapping")
        self.assertIn("Channel of each stick", p.inner_text("#f-sticks-stick_channels-help"))
        self.assertEqual(self.stick_map(), ["", "", "", ""], "not set: the channel order decides")
        self.assertTrue(p.is_hidden(f"{f} button:has-text('Use the channel order')"))
        # the first pick fills the other sticks from the channel order (TAER: thr CH1, ail CH2, ele CH3, rud CH4)
        p.select_option("#f-sticks-channel_order", "TAER")
        p.select_option(self.STICK_SELS[0], "6")
        self.assertEqual(self.stick_map(), ["6", "3", "1", "4"])
        # a channel another stick has: they swap
        p.select_option(self.STICK_SELS[1], "6")
        self.assertEqual(self.stick_map(), ["3", "6", "1", "4"])
        # a stick on the pause switch's channel: said
        self.assertTrue(p.is_hidden("#f-sticks-stick_channels-note"))
        p.select_option(self.STICK_SELS[3], "5")
        p.wait_for_selector("#f-sticks-stick_channels-note:has-text('CH5 is also the pause switch: moving the yaw stick would pause the game.')")
        p.select_option("#f-sticks-pause_channel", "0")
        self.assertTrue(p.is_hidden("#f-sticks-stick_channels-note"))
        self.assertTrue(p.is_visible(f"{f} .flag.changed"))
        self.save("sticks")
        self.until(lambda: self.posts("/api/settings"))
        self.assertEqual(self.posts("/api/settings")[0]["body"], b"channel_order = TAER\nstick_channels = 3 6 1 5\npause_channel = 0\n")
        self.assertEqual(self.pi.value("stick_channels"), "3 6 1 5")
        self.until(lambda: p.is_hidden('form[data-form="sticks"] .savebar'), what="saved")
        self.assertEqual(self.stick_map(), ["3", "6", "1", "5"])
        # what the game gets follows the mapping (the mock's radio: its CH3 is the aileron now)
        self.pi.stick_move = 3
        self.until(lambda: abs(self.bars("#mixBars")[0]) > 300, what="the aileron bar on CH3")
        # a channel twice (not possible with the lists alone) is refused in the page
        p.evaluate("""() => { const f = stp.forms.find((x) => x.name === 'sticks');
            document.getElementById('f-sticks-stick_channels-ele').value = '3'; f.changed(f.fields.get('stick_channels')); }""")
        self.save("sticks")
        p.wait_for_selector("#f-sticks-stick_channels-err:has-text('each stick needs its own channel')")
        self.assertEqual(len(self.posts("/api/settings")), 1)
        p.select_option(self.STICK_SELS[1], "")                     # "By order" on any stick: all of them
        self.assertEqual(self.stick_map(), ["", "", "", ""])
        p.select_option(self.STICK_SELS[2], "2")                    # from TAER (ail 2, ele 3, thr 1, rud 4), thr takes CH2
        self.assertEqual(self.stick_map(), ["1", "3", "2", "4"])
        p.click(f"{f} button:has-text('Use the channel order')")
        self.assertEqual(self.stick_map(), ["", "", "", ""])
        self.save("sticks")
        self.until(lambda: len(self.posts("/api/settings")) == 2)
        self.assertEqual(self.posts("/api/settings")[1]["body"], b"stick_channels = \n")
        self.assertEqual(self.pi.value("stick_channels"), "")

    def test_detect_wizard(self):
        self.pi.stick_move = 0                                      # the radio's sticks rest (with a little noise)
        p = self.open("sticks", mobile=True, dark=True)
        f = self.field("sticks", "stick_channels")
        p.click(f"{f} button:has-text('Detect')")
        p.wait_for_selector("#detectDlg[open]")
        self.assertEqual(p.inner_text("#detectAsk"), "Move the AILERON stick left and right")
        self.assertIn("Step 1 of 4: the right stick in mode 2 (roll)", p.inner_text("#detectHint"))
        p.wait_for_timeout(700)
        self.assertIn("Waiting for a channel to move", p.inner_text("#detectMsg"), "noise doesn't count")
        self.assertTrue(p.is_disabled("#detectUse"))
        self.assertLessEqual(p.eval_on_selector("#detectDlg", "e => e.getBoundingClientRect().right"), MOBILE["width"])
        self.no_sideways_scroll("the Detect wizard")
        self.shot("sticks-detect-mobile")
        # aileron: CH6 sweeps
        self.pi.stick_move = 6
        p.wait_for_selector("#detectAsk:has-text('ELEVATOR')", timeout=8000)
        self.assertEqual(p.inner_text("#detectDlg .detect-steps li:nth-child(1) b"), "CH6")
        self.assertIn("up and down", p.inner_text("#detectAsk"))
        self.pi.stick_move = 0
        p.wait_for_timeout(300)
        self.pi.stick_move = 2
        p.wait_for_selector("#detectAsk:has-text('THROTTLE')", timeout=8000)
        self.assertIn("left stick", p.inner_text("#detectHint"))
        # the aileron's channel again: refused
        self.pi.stick_move = 6
        p.wait_for_selector("#detectMsg:has-text('CH6 is already the aileron: move the THROTTLE stick')", timeout=8000)
        self.assertIn("THROTTLE", p.inner_text("#detectAsk"))
        # a channel that moves only part of the way: no step by itself, Use takes it
        self.pi.stick_amp, self.pi.stick_move = 400, 7
        p.wait_for_selector("#detectUse:has-text('Use CH7'):not([disabled])", timeout=8000)
        p.wait_for_timeout(1000)
        self.assertIn("THROTTLE", p.inner_text("#detectAsk"), "no step by itself")
        p.click("#detectUse")
        p.wait_for_selector("#detectAsk:has-text('YAW')")
        self.pi.stick_amp, self.pi.stick_move = 1000, 4
        p.wait_for_selector("#detectDlg", state="hidden", timeout=8000)
        self.assertEqual(self.stick_map(), ["6", "2", "7", "4"])
        self.toast("Save to keep it")
        self.assertEqual(self.posts("/api/settings"), [], "nothing saved yet")
        self.save("sticks")
        self.until(lambda: self.posts("/api/settings"))
        self.assertEqual(self.posts("/api/settings")[0]["body"], b"stick_channels = 6 2 7 4\n")
        self.assertEqual(self.pi.value("stick_channels"), "6 2 7 4")
        # Cancel and Esc leave the field as it was
        self.until(lambda: p.is_hidden('form[data-form="sticks"] .savebar'), what="saved")
        self.pi.stick_move = 0
        p.click(f"{f} button:has-text('Detect')")
        p.wait_for_selector("#detectDlg[open]")
        self.pi.stick_move = 1
        p.wait_for_selector("#detectAsk:has-text('ELEVATOR')", timeout=8000)
        p.click("#detectCancel")
        p.wait_for_selector("#detectDlg", state="hidden")
        p.click(f"{f} button:has-text('Detect')")
        p.wait_for_selector("#detectDlg[open]")
        self.assertEqual(p.inner_text("#detectDlg .detect-steps li:nth-child(1) b"), "–", "a new run starts over")
        p.keyboard.press("Escape")
        p.wait_for_selector("#detectDlg", state="hidden")
        self.assertEqual(self.stick_map(), ["6", "2", "7", "4"])
        self.assertTrue(p.is_hidden('form[data-form="sticks"] .savebar'))
        # no radio sending: said
        kb = '{"sources":[{"kind":"keyboard","name":"Keyboard","live":true,"nch":4,"ch":[0,0,0,0]}],"active":0}'
        p.route("**/api/sticks/stream", lambda r: r.fulfill(status=200, content_type="text/event-stream", body="data: " + kb + "\n\n"))
        p.route("**/api/sticks", lambda r: r.fulfill(status=200, content_type="application/json", body=kb))
        self.tab("video")
        self.tab("sticks")
        p.click(f"{f} button:has-text('Detect')")
        p.wait_for_selector("#detectMsg:has-text('No radio sends sticks yet')", timeout=8000)
        p.evaluate("location.hash = '#status'")                     # the back button: leaving the page closes it
        p.wait_for_selector("#detectDlg", state="hidden")
        p.wait_for_selector("#p-status", state="visible")

    def test_bluetooth_off(self):
        # bluetooth = off: the Sticks page says no Bluetooth device can connect (not only controllers), and a
        # stick source set to Bluetooth gets nothing, whatever else sends
        self.pi.file_values["bluetooth"] = "off"
        self.pi.file_values["input"] = "bluetooth"
        p = self.open("sticks")
        p.wait_for_selector("#btOff", state="visible")
        self.assertIn("no Bluetooth device can connect (the ELRS BLE joystick, gamepads, keyboards)", p.inner_text("#btOff"))
        self.assertEqual(p.get_attribute("#btOff", "role"), "status")
        p.wait_for_selector("#inputWarnSticks", state="visible")
        self.assertEqual(p.inner_text("#inputWarnSticks .input-warn-text"),
                         "Stick source is set to Bluetooth, but Bluetooth is off: the game gets no sticks.")
        self.assertIn("shares the Pi's radio with WiFi", p.inner_text("#f-sticks-bluetooth-help"))
        self.no_sideways_scroll("the Bluetooth notice")
        p.click("#inputWarnSticks button:has-text('Use auto')")
        p.wait_for_selector("#inputWarnSticks", state="hidden")
        self.assertTrue(p.is_visible("#btOff"))                          # still off: still said
        self.pi.file_values["bluetooth"] = "on"
        p.reload()
        p.wait_for_function(LOADED["sticks"])
        self.until(lambda: p.is_hidden("#btOff"), what="no notice with Bluetooth on")
        # off is saved but the Pi not restarted: a Bluetooth joystick still sends, so it isn't "no sticks"
        self.pi.file_values["bluetooth"] = "off"
        self.pi.file_values["input"] = "bluetooth"
        bt = json.dumps({"sources": [{"kind": "bluetooth", "name": "ELRS Joystick", "live": True, "rate_hz": 100, "nch": 8,
                                      "ch": [0] * 16}], "active": 0, "sticks": {"ail": 0, "ele": 0, "thr": -1024, "rud": 0}})
        p.route("**/api/sticks/stream", lambda r: r.fulfill(status=200, content_type="text/event-stream", body="data: " + bt + "\n\n"))
        p.route("**/api/sticks", lambda r: r.fulfill(status=200, content_type="application/json", body=bt))
        p.reload()
        self.until(lambda: "after the next restart of the Pi" in p.inner_text("#btOff"), what="off from the next restart")
        self.assertTrue(p.is_hidden("#inputWarnSticks"))

    def test_fixed_source_ignores_the_others(self):
        # Stick source = Bluetooth while only the ELRS WiFi joystick sends: the game gets nothing, the page says why
        self.pi.file_values["input"] = "bluetooth"
        p = self.open("status")
        p.wait_for_selector("#inputWarnStatus", state="visible")
        self.assertEqual(p.inner_text("#inputWarnStatus .input-warn-text"), "Stick source is set to Bluetooth: ELRS 10.0.0.1 (WiFi) is ignored.")
        self.assertEqual(p.get_attribute("#inputWarnStatus", "role"), "status")
        self.until(lambda: "(ignored)" in p.inner_text("#srcList"), what="the list marks it")
        self.assertNotIn("(in use)", p.inner_text("#srcList"))
        self.assertEqual(p.inner_text("[data-f=inputName]"), "none: the source is set to Bluetooth")
        self.tab("sticks")
        p.wait_for_selector("#inputWarnSticks", state="visible")
        self.assertIn("ELRS 10.0.0.1 (WiFi) is ignored", p.inner_text("#inputWarnSticks"))
        p.wait_for_function(LOADED["sticks"])
        self.until(lambda: p.is_visible("#srcCards .src-card:nth-child(1) .pill.warn"), what="the ignored pill")
        self.assertTrue(p.is_hidden("#srcCards .src-card:nth-child(1) .pill.ok"))
        self.no_sideways_scroll("the notice")
        self.shot("sticks-ignored-desktop")
        p.click("#inputWarnSticks button:has-text('Use auto')")
        self.until(lambda: self.posts("/api/settings"))
        self.assertEqual(self.posts("/api/settings")[0]["body"], b"input = auto\n")
        p.wait_for_selector("#inputWarnSticks", state="hidden")
        self.toast("Stick source saved.")
        self.assertEqual(self.pi.value("input"), "auto")
        self.assertEqual(p.input_value("#f-sticks-input"), "auto")
        self.until(lambda: p.is_visible("#srcCards .src-card:nth-child(1) .pill.ok"), what="in use")
        self.assertTrue(p.is_hidden("#srcCards .src-card:nth-child(1) .pill.warn"))
        self.tab("status")
        self.until(lambda: "ELRS 10.0.0.1 (in use)" in p.inner_text("#srcList"), what="in use on Status")
        self.assertTrue(p.is_hidden("#inputWarnStatus"))
        # a fixed kind that does send: no notice, the other sources are marked as ignored
        self.tab("sticks")
        p.select_option("#f-sticks-input", "wifi")
        self.save("sticks")
        self.until(lambda: len(self.posts("/api/settings")) == 2)
        self.until(lambda: p.is_visible("#srcCards .src-card:nth-child(2) .pill.warn"), what="USB ignored")
        p.wait_for_timeout(300)
        self.assertTrue(p.is_hidden("#inputWarnSticks"))
        # the Pi refusing the save: said, nothing changes
        self.pi.file_values["input"] = "usb"
        self.net_errors_ok = True
        self.page.evaluate("stp.S.values.input = 'usb'")
        p.wait_for_selector("#inputWarnSticks", state="visible")
        self.pi.fail["/api/settings"] = (500, "Internal", "Couldn't save the settings: Read-only file system", 1)
        p.click("#inputWarnSticks button:has-text('Use auto')")
        self.toast("Read-only file system")
        self.assertTrue(p.is_visible("#inputWarnSticks"))

    def test_module_wifi(self):
        p = self.open("sticks")
        p.click('[data-modap="tbs"]')
        self.confirm(False, "Join the TBS module's WiFi?")
        self.assertEqual(self.posts("/api/wifi/module-ap"), [])
        p.click('[data-modap="elrs"]')
        self.confirm(True, "This page loses the Pi")
        self.until(lambda: self.posts("/api/wifi/module-ap"))
        self.assertEqual(self.posts("/api/wifi/module-ap")[0]["body"], b"elrs")
        self.assertEqual(self.pi.value("wifi_module_ip"), "10.0.0.1")
        self.toast("joining the ELRS module's WiFi")

    # ---------------------------------------------------------------- sound
    def song_names(self):
        return self.page.eval_on_selector_all("#songList li .name", "els => els.map((e) => e.textContent)")

    def test_songs(self):
        p = self.open("sound")
        p.wait_for_function(LOADED["sound"])
        self.assertEqual(self.song_names(), ["Intro.mp3", "Racing Theme.ogg"])
        self.assertEqual(p.inner_text("#songFree"), "312 MB free")
        files = [("Fast Lap.mp3", "audio/mpeg", b"ID3" + bytes(400000)), ("Chill.ogg", "audio/ogg", b"OggS" + bytes(999)),
                 ("notes.txt", "text/plain", b"hello"), ("con.wav", "audio/wav", b"RIFF" + bytes(10)),
                 ('What: "Now"?.MP3', "audio/mpeg", b"ID3 now"), ("chill.OGG", "audio/ogg", b"twice")]
        p.set_input_files("#songInput", [{"name": n, "mimeType": t, "buffer": b} for n, t, b in files])
        want = {"Fast Lap.mp3": files[0][2], "Chill.ogg": files[1][2], "con_.wav": files[3][2], "What_ _Now__.mp3": files[4][2]}
        self.until(lambda: all((self.pi.music / n).is_file() and (self.pi.music / n).read_bytes() == b for n, b in want.items()), timeout=10, what="the uploads")
        rows = p.eval_on_selector_all("#uploads li", "els => els.map((e) => e.textContent)")
        self.assertEqual(len(rows), 6)
        self.assertIn("not an MP3, OGG or WAV file", rows[2])
        self.assertIn("(was con.wav)", rows[3])
        self.assertIn("twice in this batch", rows[5])
        self.toast("4 songs added.")
        puts = self.posts()
        self.assertEqual(sorted(r["path"] for r in puts), sorted("/api/songs/" + mock_server.urllib.parse.quote(n) for n in want))
        self.assertTrue(all(r["headers"]["content-type"] == "application/octet-stream" for r in puts))
        p.wait_for_function("document.querySelectorAll('#songList li .name').length === 6")
        self.assertEqual(self.song_names(), sorted(["Intro.mp3", "Racing Theme.ogg", *want], key=str.lower))
        p.wait_for_function("document.querySelectorAll('#uploads li').length === 2", timeout=8000)   # done rows go, the refused stay
        self.shot("sound-uploads-desktop")
        # the same name again: asked first; No skips it, Replace replaces it
        p.set_input_files("#songInput", [{"name": "intro.MP3", "mimeType": "audio/mpeg", "buffer": b"new intro"}])
        self.confirm(False, "Replace intro.mp3?")
        p.wait_for_timeout(200)
        self.assertEqual(len(self.posts()), 4)
        p.set_input_files("#songInput", [{"name": "intro.MP3", "mimeType": "audio/mpeg", "buffer": b"new intro"}])
        self.confirm(True)
        self.until(lambda: (self.pi.music / "Intro.mp3").read_bytes() == b"new intro")
        self.assertFalse((self.pi.music / "intro.mp3").exists() and not (self.pi.music / "Intro.mp3").samefile(self.pi.music / "intro.mp3"))
        # the Pi refuses one: the row says why
        self.net_errors_ok = True
        self.pi.fail["/api/songs/Big.mp3"] = (507, "QuotaExceededError", "Not enough space on the SD card (3 MB free).")
        p.set_input_files("#songInput", [{"name": "Big.mp3", "mimeType": "audio/mpeg", "buffer": b"ID3 big"}])
        p.wait_for_selector("#uploads li .st.bad:has-text('Not enough space on the SD card (3 MB free).')")
        # delete: asked first
        p.click('#songList button[aria-label="Delete Racing Theme.ogg"]')
        self.confirm(False, "Delete Racing Theme.ogg?")
        self.assertTrue((self.pi.music / "Racing Theme.ogg").exists())
        p.click('#songList button[aria-label="Delete Racing Theme.ogg"]')
        self.confirm(True)
        self.until(lambda: not (self.pi.music / "Racing Theme.ogg").exists())
        self.assertIn(("DELETE", "/api/songs/Racing%20Theme.ogg"), [(r["method"], r["path"]) for r in self.posts()])
        p.wait_for_function("![...document.querySelectorAll('#songList .name')].some((e) => e.textContent === 'Racing Theme.ogg')")
        # next song; a Pi that can't tell its free space
        p.click("#nextSong")
        self.until(lambda: self.pi.song_now == 1)
        self.pi.free_mb = -1
        self.tab("video")
        self.tab("sound")
        p.wait_for_function("document.getElementById('songFree').textContent === ''")

    def test_song_names(self):
        p = self.open("sound")
        cases = {"a/b\\c.mp3": "a_b_c.mp3", "...hidden.ogg": "hidden.ogg", "song. .mp3": "song.mp3", "": "song", "COM1.mp3": "COM1_.mp3",
                 "lpt9.backup.wav": "lpt9_.backup.wav", "console.mp3": "console.mp3", "nul": "nul_", "tab\there.WAV": "tab_here.wav",
                 "é.mp3": "é.mp3"}
        got = p.evaluate("(c) => Object.fromEntries(Object.keys(c).map((k) => [k, stp.songName(k)]))", cases)
        self.assertEqual(got, cases)
        long = p.evaluate("[stp.songName('x'.repeat(300) + '.mp3'), stp.songName('é'.repeat(100) + '.ogg'), stp.songName('😀'.repeat(40) + '.mp3')]")
        for n, ext in zip(long, (".mp3", ".ogg", ".mp3")):
            self.assertLessEqual(len(n.encode("utf-8")), 120)
            self.assertTrue(n.endswith(ext))
            n.encode("utf-8")                                               # whole characters (a lone surrogate would raise)
        self.assertEqual(len(long[1].encode()), 120)

    # ---------------------------------------------------------------- game
    def test_keypad_and_keyboard(self):
        p = self.open("game")
        p.wait_for_function(LOADED["game"])
        order = ["exit", "up", "menu", "prev", "enter", "next", "dec", "down", "inc", "prevpage", "nextpage"]
        for i, k in enumerate(order):
            p.click(f'.keypad [data-key="{k}"]')
            self.until(lambda: len(self.pi.keys_sent) == i + 1, what=k)
        self.assertEqual(self.pi.keys_sent, order)
        self.assertTrue(all(r["body"].decode() in order for r in self.posts("/api/key")))
        shots = len(self.gets("/api/screenshot.png"))
        self.until(lambda: len(self.gets("/api/screenshot.png")) > shots, what="a fresh picture after a key")
        # the keyboard, away from fields and dialogs
        p.evaluate("document.activeElement.blur()")
        for key in ("ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Enter", "Escape", "+", "-", "PageDown", "m"):
            p.keyboard.press(key)
        want = order + ["up", "down", "prev", "next", "enter", "exit", "inc", "dec", "nextpage", "menu"]
        self.until(lambda: len(self.pi.keys_sent) == len(want))
        self.assertEqual(sorted(self.pi.keys_sent), sorted(want))
        p.focus("#f-game-latency")
        p.keyboard.press("ArrowUp")
        p.keyboard.press("Enter")
        self.tab("video")
        p.evaluate("document.activeElement.blur()")
        p.keyboard.press("ArrowDown")
        p.wait_for_timeout(300)
        self.assertEqual(len(self.pi.keys_sent), len(want), "no keys from a field or another page")
        # live view: pictures one after another, until it's off
        self.tab("game")
        p.check("#liveView")
        n = len(self.gets("/api/screenshot.png"))
        self.until(lambda: len(self.gets("/api/screenshot.png")) >= n + 3, timeout=6, what="live pictures")
        p.uncheck("#liveView")
        p.wait_for_timeout(500)
        n = len(self.gets("/api/screenshot.png"))
        p.wait_for_timeout(900)
        self.assertLessEqual(len(self.gets("/api/screenshot.png")) - n, 0)

    def test_scripts_and_reload(self):
        p = self.open("game")
        p.wait_for_function(LOADED["game"])
        # the three editions by their names, each with what it is
        self.assertEqual(p.eval_on_selector_all("#toolList label b", "els => els.map((e) => e.textContent)"),
                         ["StickTime", "StickTime BW", "StickTime Lite"])
        self.assertEqual([d.split(":")[0] for d in p.eval_on_selector_all("#toolList .tdesc", "els => els.map((e) => e.textContent)")],
                         ["Color edition", "B&W edition", "Lite edition"])
        self.assertTrue(p.is_checked('#toolList input[value="/SCRIPTS/TOOLS/StickTime.lua"]'))
        p.click("#toolList label:has-text('StickTime BW')")
        self.until(lambda: self.posts("/api/settings"))
        self.assertEqual(self.posts("/api/settings")[0]["body"], b"script = /SCRIPTS/TOOLS/StickTimeBW.lua\n")
        self.toast("Running StickTime BW.")
        p.click("#p-game [data-action=reload-game]")
        self.until(lambda: self.posts("/api/game/reload"))
        self.toast("The game is reloading.")
        self.assertEqual([p.is_visible(f"#f-game-{k}") or p.is_visible(f"{self.field('game', k)}") for k in ("latency", "stats", "sd_write")], [True] * 3)

    # ---------------------------------------------------------------- wifi
    def test_wifi_scan_and_join(self):
        self.net_errors_ok = True                                   # 503 while the first scan runs, a 400
        self.pi.scan_busy = 2
        p = self.open("wifi")
        p.wait_for_function(LOADED["wifi"], timeout=15000)
        self.assertIn("Last scan", p.inner_text("#scanNote"))
        scans = lambda: [r for r in self.gets("/api/wifi/scan") if "cached" not in r["query"]]
        self.assertEqual(scans(), [], "opening the tab doesn't scan: a scan interrupts the WiFi joystick")
        self.assertEqual(len(self.gets("/api/wifi/scan")), 1, "only the last results")
        p.click("#scanBtn")
        self.until(lambda: len(scans()) == 3, timeout=15, what="the scan")
        p.wait_for_function("!document.getElementById('scanNote').textContent.startsWith('Last scan')", timeout=10000)
        self.assertEqual(len(scans()), 3, "Busy twice, then the list")
        self.assertEqual(p.eval_on_selector_all("#netList .ssid", "els => els.map((e) => e.textContent)"),
                         ["Home", "ExpressLRS TX", 'Cafe <b>Free</b> & "WiFi"', "Neighbour 5G"], "saved first, then by signal; names as text")
        self.assertIsNone(p.query_selector("#netList b"))
        self.assertIn("saved", p.inner_text("#netList li:nth-child(1)"))
        self.assertEqual(p.get_attribute("#netList li:nth-child(2) .sig", "data-n"), "4")
        # a protected network: the password checked in the page, then the Pi's own field error
        p.click("#netList li:nth-child(2) button")
        p.wait_for_selector("#joinDlg[open]")
        self.assertEqual(p.inner_text("#joinTitle"), "Join ExpressLRS TX")
        self.assertEqual(p.evaluate("document.activeElement.id"), "joinPass")
        p.fill("#joinPass", "short")
        p.click("#joinGo")
        self.assertIn("8 to 63 characters", p.inner_text("#joinPassErr"))
        self.assertEqual(self.posts("/api/wifi"), [])
        p.route("**/api/wifi", lambda r: r.fulfill(status=400, content_type="application/json", body=json.dumps(
            {"error": "BadRequest", "message": "Some values are not valid: nothing was saved.", "fields": {"password": "the Pi says no"}})))
        p.fill("#joinPass", "expresslrs")
        p.click("#joinGo")
        p.wait_for_selector("#joinPassErr:has-text('the Pi says no')")
        p.unroute("**/api/wifi")
        self.shot("wifi-join-desktop")
        p.click("#joinGo")
        p.wait_for_selector("#joinDlg", state="hidden")
        self.assertEqual(self.posts("/api/wifi")[0]["body"], b"ssid = ExpressLRS TX\npassword = expresslrs\ncountry = US\n")
        self.assertIn('Joining "ExpressLRS TX"', p.inner_text("#joinState"))
        p.wait_for_selector("#joinState:has-text('Connected to \"ExpressLRS TX\" as 192.168.1.23.')", timeout=10000)
        # an open network: no password asked
        p.click("#netList li:nth-child(3) button")
        self.assertTrue(p.is_hidden("#joinPassBox"))
        p.click("#joinGo")
        p.wait_for_selector("#joinDlg", state="hidden")
        self.assertEqual(self.posts("/api/wifi")[1]["body"], b'ssid = Cafe <b>Free</b> & "WiFi"\npassword = \ncountry = US\n')
        # a hidden one: the name typed, a 64-hex-digit key, a bad country first
        p.click("#otherNet")
        self.assertTrue(p.is_checked("#joinHidden"))
        self.assertFalse(p.evaluate("document.getElementById('joinSsid').readOnly"))
        p.click("#joinGo")
        self.assertIn("name", p.inner_text("#joinSsidErr"))
        p.fill("#joinSsid", "Secret # Net ")
        p.fill("#joinPass", "a" * 64)
        p.fill("#joinCountry", "u1")
        p.click("#joinGo")
        self.assertIn("Two letters", p.inner_text("#joinCountryErr"))
        p.fill("#joinCountry", "de")
        p.click("#joinGo")
        p.wait_for_selector("#joinDlg", state="hidden")
        self.assertEqual(self.posts("/api/wifi")[2]["body"], b'ssid = "Secret # Net "\npassword = ' + b"a" * 64 + b"\ncountry = DE\nhidden = 1\n")
        self.assertEqual(self.pi.value("wifi_ssid"), "Secret # Net ")
        # Cancel closes without sending
        p.click("#netList li:nth-child(1) button")
        p.click("#joinCancel")
        p.wait_for_selector("#joinDlg", state="hidden")
        self.assertEqual(len(self.posts("/api/wifi")), 3)

    def test_wifi_join_loses_the_pi(self):
        self.net_errors_ok = True
        p = self.open("wifi")
        p.wait_for_function(LOADED["wifi"])
        p.click("#netList li:nth-child(4) button")
        p.fill("#joinPass", "neighbourpass")
        orig = self.pi.status
        p.click("#joinGo")
        p.wait_for_selector("#joinDlg", state="hidden")
        self.pi.down_until = time.time() + 4                          # the Pi left this network
        p.wait_for_selector("#joinState:has-text('The Pi left this network')", timeout=10000)
        self.assertIn("http://sticktime.local", p.inner_text("#joinState"))
        self.pi.wifi_applied = "hotspot"                              # and couldn't join: its hotspot again
        self.pi.status = lambda: {**orig(), "network": {**orig()["network"], "ssid": "StickTime", "mode": "hotspot"}}
        p.wait_for_selector("#joinState:has-text('its own hotspot')", timeout=12000)
        self.assertIn("Check the password", p.inner_text("#joinState"))

    # ---------------------------------------------------------------- updates
    def test_updates(self):
        p = self.open("updates")
        self.assertTrue(p.is_disabled('[data-apply="game"]') and p.is_hidden("#updBadge"))
        self.assertEqual(p.inner_text("#updChecked"), "Not checked yet.")
        p.click("#updCheck")
        p.wait_for_selector("#updBar", state="visible")
        self.assertTrue(p.is_disabled("#updCheck"))
        p.wait_for_selector("#updGame [data-u=latest]:has-text('1.7.3 (new)')", timeout=10000)
        self.assertTrue(p.is_hidden("#updBar") and p.is_visible("#updBadge"))
        self.assertEqual(p.inner_text('[data-apply="game"]'), "Install 1.7.3")
        self.assertEqual(p.inner_text("#updGame [data-u=staged]"), "1.7.3 (ready for Install)")
        self.assertEqual(p.eval_on_selector_all("#updGame .notes > *", "els => els.map((e) => e.tagName + ':' + e.textContent)"),
                         ["H3:New in 1.7.3", "UL:Faster gate rushA new track: Riverside"])
        self.assertIn("Last checked just now", p.inner_text("#updChecked"))
        self.shot("updates-available-desktop")
        # the game
        p.click('[data-apply="game"]')
        self.confirm(True, "Install StickTime 1.7.3?")
        self.until(lambda: self.posts("/api/update/apply"))
        self.assertEqual(self.posts("/api/update/apply")[0]["body"], b"game")
        p.wait_for_selector("#updGame [data-u=installed]:has-text('1.7.3')", timeout=10000)
        self.assertEqual(p.inner_text("#updMsg"), "Installed the game update.")
        p.click('[data-rollback="game"]')
        self.confirm(True, "Roll back the game?")
        p.wait_for_selector("#updGame [data-u=installed]:has-text('1.7.2')", timeout=10000)
        self.assertEqual(self.posts("/api/update/rollback")[0]["body"], b"game")
        # the app: the Pi restarts, the page reloads with the new version
        self.net_errors_ok = True
        p.click('[data-apply="app"]')
        self.confirm(True, "Install StickTime Pi 0.2.0?")
        p.wait_for_selector("#waitBox", state="visible", timeout=10000)
        p.wait_for_function("document.querySelector('[data-f=appVersion]') && document.querySelector('[data-f=appVersion]').textContent === '0.2.0'", timeout=30000)
        self.ready()
        self.assertTrue(p.is_hidden("#waitBox"))
        # an error from the updater
        self.pi.update.update(state="error", message="Download failed: no network", error="Download failed: no network")
        self.tab("status")
        self.tab("updates")
        p.wait_for_selector("#updErr:has-text('Download failed: no network')")
        # nothing to roll back: the updater's own error comes back the same way; Cancel sends nothing
        n = len(self.posts("/api/update/rollback"))
        p.click('[data-rollback="app"]')
        self.confirm(False)
        self.assertEqual(len(self.posts("/api/update/rollback")), n)

    def test_updates_old_state_file(self):
        # the updater starts in the background: right after the POST the state file still holds the last run
        self.pi.update.update(state="done", message="StickTime 1.7.2 is up to date.", progress=1)
        self.pi.update_start_delay = 1.6
        p = self.open("updates")
        self.assertEqual(p.inner_text("#updMsg"), "StickTime 1.7.2 is up to date.")
        p.click("#updCheck")
        p.wait_for_selector("#updMsg:has-text('Starting the updater…')")
        self.assertTrue(p.is_visible("#updBar") and p.is_disabled("#updCheck"))
        p.wait_for_selector("#updMsg:has-text('Checking GitHub')", timeout=6000)
        p.wait_for_selector("#updGame [data-u=latest]:has-text('1.7.3 (new)')", timeout=10000)
        self.assertFalse(p.is_disabled("#updCheck"))
        # the app: an old "done" isn't taken for this install; the page reloads only after the real restart
        self.net_errors_ok = True
        p.evaluate("window.notReloaded = true")
        with p.expect_navigation(timeout=30000):
            p.click('[data-apply="app"]')
            self.confirm(True)
            p.wait_for_timeout(1200)
            self.assertTrue(p.evaluate("window.notReloaded === true"), "not reloaded on the old state")
            self.assertIn("Starting the updater", p.inner_text("#updMsg"))
        self.ready()
        p.wait_for_function("document.querySelector('[data-f=appVersion]').textContent === '0.2.0'", timeout=10000)
        self.assertIsNone(p.evaluate("window.notReloaded"), "the new page")

    # ---------------------------------------------------------------- system
    def test_system(self):
        p = self.open("system")
        p.wait_for_function(LOADED["system"])
        self.assertEqual(p.inner_text("[data-s=model]"), "Raspberry Pi 4 Model B Rev 1.5")
        self.assertEqual(p.inner_text("[data-s=disk]"), "312 MB free of 510 MB")
        with p.expect_download() as dl:
            p.click("#logSave")
        self.assertEqual(dl.value.suggested_filename, "sticktime-log.txt")
        self.assertIn("web: listening on :80", pathlib.Path(dl.value.path()).read_text(encoding="utf-8"))
        # follow the log
        n = len(self.gets("/api/log"))
        p.check("#logAuto")
        self.until(lambda: len(self.gets("/api/log")) >= n + 2, timeout=6, what="the log followed")
        p.uncheck("#logAuto")
        # the settings file: view, warnings, edit, apply only what changed
        txt = p.input_value("#fileText")
        for line in ("tv = ntsc", "volume = 70", "hostname = sticktime", "my_note = kept", "# TV standard of the composite output"):
            self.assertIn(line, txt)
        self.assertIn("line 41: no '='", p.inner_text("#fileWarn"))
        p.fill("#fileText", txt.replace("tv = ntsc", "tv = pal").replace("volume = 70", "volume = 55  # quieter").replace("my_note = kept", 'my_note = "a # in it"'))
        p.click("#fileApply")
        self.until(lambda: self.posts("/api/settings"))
        self.assertEqual(self.posts("/api/settings")[0]["body"], b'tv = pal\nvolume = 55\nmy_note = "a # in it"\n')
        self.assertEqual((self.pi.value("tv"), self.pi.value("volume"), self.pi.unknown["my_note"]), ("pal", "55", "a # in it"))
        p.wait_for_function("document.getElementById('fileText').value.includes('tv = pal')")
        self.net_errors_ok = True
        p.fill("#fileText", p.input_value("#fileText").replace("margin = 0", "margin = 99"))
        self.assertIn("margin = 99", p.input_value("#fileText"), "the edit is in the box")
        p.click("#fileApply")
        p.wait_for_selector("#fileErr:has-text('margin: must be 0 to 20')", timeout=10000)
        self.assertEqual(self.pi.value("margin"), "0")
        p.click("#fileReset")
        self.assertIn("margin = 0", p.input_value("#fileText"))
        self.assertTrue(p.is_hidden("#fileErr"))
        p.click("#fileApply")
        self.toast("Nothing changed.")
        with p.expect_download() as dl:
            p.click("#fileSave")
        self.assertEqual(dl.value.suggested_filename, "sticktime.txt")
        self.assertEqual(pathlib.Path(dl.value.path()).read_text(encoding="utf-8"), p.input_value("#fileText"))
        self.assertTrue(p.is_hidden("#moreCard"), "every key has its place")

    def test_power(self):
        self.net_errors_ok = True
        p = self.open("system")
        # pulling the plug sends the WiFi module no stop (R14): the note says what does
        note = p.inner_text("section[aria-labelledby=h-power] .note")
        self.assertIn("it sends the WiFi module no stop", note)
        self.assertIn("A shutdown here, or Status → Inputs → Release module, stops its stream.", note)
        p.click("#restartBtn")
        self.confirm(False, "Restart the game?")
        self.assertEqual(self.posts("/api/restart"), [])
        p.click("#restartBtn")
        self.confirm(True)
        p.wait_for_selector("#waitBox", state="visible")
        self.assertEqual(p.inner_text("#waitTitle"), "Restarting the game…")
        p.wait_for_selector("#waitBox", state="hidden", timeout=20000)
        self.toast("The game is back.")
        p.click("#rebootBtn")
        self.confirm(True, "Reboot the Pi?")
        p.wait_for_selector("#waitBox", state="visible")
        self.assertEqual(p.inner_text("#waitTitle"), "Rebooting…")
        p.wait_for_selector("#waitBox", state="hidden", timeout=30000)
        self.toast("The Pi is back.")
        p.click("#shutdownBtn")
        self.confirm(True, "Shut down the Pi?")
        p.wait_for_selector("#waitBox", state="visible")
        self.assertEqual(p.inner_text("#waitTitle"), "The Pi is shutting down")
        self.shot("system-shutdown-desktop")
        p.click("#waitClose")
        self.assertTrue(p.is_hidden("#waitBox"))
        self.assertEqual([r["path"] for r in self.posts()], ["/api/restart", "/api/reboot", "/api/shutdown"])

    # ---------------------------------------------------------------- VTX control (Video page)
    def test_vtx_card(self):
        self.net_errors_ok = True                                   # (a refused table answers 400)
        p = self.open("video")
        # off: says so, the pickers are hidden, the setup form has its three keys
        p.wait_for_selector("#vtxPill:has-text('Off')")
        self.assertIn("VTX control is off", p.inner_text("#vtxMsg"))
        self.assertTrue(p.is_hidden("#vtxLive"))
        self.assertEqual(p.eval_on_selector_all('input[name="f-vtx-vtx"]', "els => els.map((e) => e.value)"), ["off", "auto", "smartaudio", "tramp"])
        self.assertEqual(p.eval_on_selector("#f-vtx-vtx_port", "e => [...e.options].map((o) => o.textContent)"),
                         ["One pin: GPIO4, pin 7", "One pin: GPIO17, pin 11", "One pin: GPIO22, pin 15", "One pin: GPIO23, pin 16",
                          "One pin: GPIO27, pin 13", "Two pins: UART3, TX pin 7 + RX pin 29", "Two pins: UART4, TX pin 24 + RX pin 21",
                          "Two pins: UART5, TX pin 32 + RX pin 33"])
        self.assertEqual(p.input_value("#f-vtx-vtx_port"), "gpio4")
        self.assertEqual(p.text_content(f"{self.field('vtx', 'vtx_port')} .flag:not(.changed)"), "at once")
        self.assertEqual(p.inner_text("[data-f=vtx]"), "Off")                    # the Status page's line (hidden page)
        # on with one pin: saved by the form, applies at once (no overlay, no reboot)
        p.check('input[name="f-vtx-vtx"][value="auto"]')
        self.save("vtx")
        self.until(lambda: self.posts("/api/settings"))
        self.assertEqual(self.posts("/api/settings")[-1]["body"], b"vtx = auto\n")
        p.wait_for_selector("#vtxPill:has-text('Ready')", timeout=8000)
        self.assertTrue(p.is_hidden("#rebootBanner"))
        self.assertTrue(p.is_visible("#vtxLive"))
        self.assertIn("SmartAudio 2.1", p.inner_text("#vtxDev"))
        self.assertIn("GPIO4, pin 7 (single pin)", p.inner_text("#vtxDev"))
        self.assertIn("TX edges late by", p.inner_text("#vtxStats"))           # the single pin's timing
        # the two-pin mode: its UART comes at the next boot
        p.select_option("#f-vtx-vtx_port", "uart3")
        self.save("vtx")
        p.wait_for_selector("#rebootBanner", state="visible")
        p.wait_for_selector("#vtxDev:has-text('/dev/ttyAMA3 (uart3)')", timeout=8000)
        self.assertTrue(p.is_hidden("#vtxStats"))
        p.click("#rebootLater")
        p.select_option("#f-vtx-vtx_port", "gpio4")
        self.save("vtx")
        p.wait_for_selector("#vtxDev:has-text('GPIO4')", timeout=8000)
        self.assertEqual(p.eval_on_selector("#vtxBand", "e => [...e.options].map((o) => o.value)"), ["", "A", "B", "E", "F", "R"])
        self.assertEqual(p.input_value("#vtxBand"), "")
        self.assertEqual(p.eval_on_selector_all("#vtxChan input", "els => els.filter((e) => !e.disabled).length"), 0)   # no band: no channel
        self.assertEqual(p.eval_on_selector_all("#vtxPower span", "els => els.map((e) => e.firstChild.textContent)"), ["25 mW", "100 mW", "400 mW"])
        # band R: posted with a channel; the channels show their MHz; the dot shows where the VTX is
        p.select_option("#vtxBand", "R")
        self.until(lambda: self.posts("/api/vtx"))
        self.assertEqual(self.posts("/api/vtx")[-1]["body"], b"band = R\nchannel = 1\n")
        p.wait_for_selector("#vtxChan input[value='1']:checked")
        self.assertEqual(p.eval_on_selector_all("#vtxChan small", "els => els.map((e) => e.textContent)"),
                         ["5658", "5695", "5732", "5769", "5806", "5843", "5880", "5917"])
        p.wait_for_selector("#vtxChan input[value='1'] + span.now")
        p.wait_for_selector("#vtxNow:has-text('R1 · 5658 MHz')")
        p.click("#vtxChan label:has(input[value='5'])")
        self.until(lambda: self.posts("/api/vtx")[-1]["body"] == b"band = R\nchannel = 5\n", what="channel 5 posted")
        p.wait_for_selector("#vtxNow:has-text('R5 · 5806 MHz')")
        self.assertEqual(self.pi.value("vtx_freq"), "0")
        p.click("#vtxPower label:has(input[value='2'])")
        self.until(lambda: self.posts("/api/vtx")[-1]["body"] == b"power = 2\n", what="power posted")
        p.wait_for_selector("#vtxPowNow:has-text('100 mW')")
        p.wait_for_selector("#vtxPower input[value='2'] + span.now")
        p.check("#vtxPit")
        self.until(lambda: self.posts("/api/vtx")[-1]["body"] == b"pit = on\n", what="pit posted")
        p.wait_for_selector("#vtxPowNow:has-text('pit mode')")
        # a frequency: checked here first, then posted; it clears the channel pick
        p.fill("#vtxFreq", "123")
        p.click("#vtxFreqSet")
        self.assertIn("4900 to 6200", p.inner_text("#vtxFreqErr"))
        p.fill("#vtxFreq", "5810")
        p.press("#vtxFreq", "Enter")
        self.until(lambda: self.posts("/api/vtx")[-1]["body"] == b"freq = 5810\n", what="freq posted")
        p.wait_for_selector("#vtxNow:has-text('5810 MHz')")
        p.wait_for_function("!document.querySelector('#vtxChan input:checked')")
        p.click("#vtxFreqClear")
        self.until(lambda: self.posts("/api/vtx")[-1]["body"] == b"freq = 0\n", what="freq cleared")
        # the Pi refuses a value: its message as a toast, the pickers as the Pi has them
        def refuse(text):
            raise mock_server.ApiError(400, "BadRequest", "Some values are not valid: nothing was saved.", fields={"power": "is too much here"})
        self.pi.vtx_post = refuse
        p.click("#vtxPower label:has(input[value='3'])")
        self.toast("Not set: power is too much here")
        del self.pi.vtx_post
        p.wait_for_selector("#vtxPower input[value='2']:checked")
        # the status page's line
        self.tab("status")
        p.wait_for_selector("[data-f=vtx]:has-text('SmartAudio 2.1')")
        self.tab("video")
        # the VTX table: load a maker's file, use it, download it, a broken one, back to the built-in one
        table = {"description": "Maker X VTX", "version": "1.0", "vtx_table": {
            "bands_list": [{"name": "RACEBAND", "letter": "R", "is_factory_band": True, "frequencies": [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]},
                           {"name": "LOWRACE", "letter": "L", "is_factory_band": False, "frequencies": [5362, 5399, 5436, 0, 5510, 5547, 5584, 5621]}],
            "powerlevels_list": [{"value": 14, "label": "25 "}, {"value": 23, "label": "200"}, {"value": 30, "label": "1W"}]}}
        p.set_input_files("#vtxFile", [{"name": "makerx.json", "mimeType": "application/json", "buffer": json.dumps(table).encode()}])
        self.toast("VTX table loaded: Maker X VTX (2 bands, 3 power levels).")
        p.wait_for_selector("#vtxTableLine:has-text('loaded (Maker X VTX)')")
        self.assertEqual(p.eval_on_selector("#vtxBand", "e => [...e.options].map((o) => o.textContent)"), ["– leave it –", "R · RACEBAND", "L · LOWRACE"])
        self.assertEqual(p.eval_on_selector_all("#vtxPower span", "els => els.map((e) => e.firstChild.textContent)"), ["25 mW", "200 mW", "1W"])
        p.select_option("#vtxBand", "L")
        self.until(lambda: self.posts("/api/vtx")[-1]["body"] == b"band = L\nchannel = 5\n", what="band L (channel 5: 4 is not in the table)")
        p.wait_for_selector("#vtxChan input[value='5']:checked")
        self.assertTrue(p.is_disabled("#vtxChan input[value='4']"))
        with p.expect_download() as dl:
            p.click("#vtxDownload")
        self.assertEqual(dl.value.suggested_filename, "vtxtable.json")
        got = json.loads(pathlib.Path(dl.value.path()).read_text(encoding="utf-8"))
        self.assertEqual([b["letter"] for b in got["vtx_table"]["bands_list"]], ["R", "L"])
        p.set_input_files("#vtxFile", [{"name": "bad.json", "mimeType": "application/json", "buffer": b"{nope"}])
        self.toast("Not loaded: This isn't a VTX table")
        p.click("#vtxDefault")
        self.confirm(True, "Use the built-in VTX table?")
        p.wait_for_selector("#vtxTableLine:has-text('built in')")
        self.assertTrue(p.is_disabled("#vtxDefault"))
        for r in self.pi.requests:
            if r["path"].startswith("/api/vtx") and r["method"] != "GET":
                self.assertEqual(r["headers"].get("x-sticktime"), "1")

    def test_vtx_states(self):
        self.pi.file_values.update({"vtx": "smartaudio"})
        p = self.open("video")
        cases = [({"state": "no_port", "message": "uart3 isn't turned on: reboot once.", "needs_reboot": True}, "UART off", "warn"),
                 ({"state": "searching", "message": "Looking for the VTX: SmartAudio, 4800 baud 8N2…"}, "Searching", "info"),
                 ({"state": "not_found", "message": "No answer from the VTX (tried SmartAudio)."}, "Not found", "warn"),
                 ({"state": "error", "message": "The VTX didn't take the power setting.", "error": "The VTX didn't take the power setting."}, "Problem", "bad")]
        for force, pill, kind in cases:
            self.pi.vtx_force = force
            p.wait_for_selector(f"#vtxPill:has-text('{pill}')", timeout=8000)
            self.assertIn(force["message"], p.inner_text("#vtxMsg"))
            self.assertIn(kind, p.get_attribute("#vtxMsg", "class"))
            self.assertEqual(p.is_visible("#vtxReboot"), bool(force.get("needs_reboot")))
            self.assertEqual(p.inner_text("#vtxNow"), "–")
            self.assertEqual(p.inner_text("#vtxMsg").count("didn't take"), 1 if kind == "bad" else 0)   # the error once
        self.pi.vtx_force = None
        self.pi.file_values.update({"vtx": "tramp", "vtx_send_only": "on"})
        p.wait_for_selector("#vtxPill:has-text('Send-only')", timeout=8000)
        self.assertIn("can't show what it is set to", p.inner_text("#vtxMsg"))
        self.assertEqual(p.inner_text("#vtxNow"), "not read back (send-only)")
        self.pi.file_values.update({"vtx_send_only": "off"})
        p.wait_for_selector("#vtxPill:has-text('Ready')", timeout=8000)
        p.wait_for_selector("#vtxTemp:has-text('33 °C')")
        self.assertIn("Tramp", p.inner_text("#vtxDev"))

    def test_vtx_late_poll(self):
        """a poll that left before a pick and comes back after it doesn't undo the pick on the page"""
        self.pi.file_values.update({"vtx": "auto", "vtx_band": "R", "vtx_channel": "1", "vtx_power": "1"})
        p = self.open("video")
        p.wait_for_selector("#vtxChan input[value='1']:checked", timeout=8000)
        hold = threading.Event()
        self.pi.vtx_hold = hold
        self.until(lambda: self.pi.vtx_held, what="a VTX poll on its way")
        p.check("#vtxPit")
        self.until(lambda: self.posts("/api/vtx") and self.posts("/api/vtx")[-1]["body"] == b"pit = on\n", what="pit posted")
        p.wait_for_selector("#vtxPowNow:has-text('pit mode')")
        n = len(self.gets("/api/vtx"))
        hold.set()                                 # the late answer: pit off
        self.until(lambda: len(self.gets("/api/vtx")) > n, what="the late answer")
        p.wait_for_timeout(300)
        self.assertTrue(p.is_checked("#vtxPit"), "a late poll unchecked Pit mode")
        self.assertIn("pit mode", p.inner_text("#vtxPowNow"))

    def test_vtx_phone(self):
        self.pi.file_values.update({"vtx": "auto", "vtx_band": "F", "vtx_channel": "8", "vtx_power": "3"})
        p = self.open("video", mobile=True, dark=True)
        p.wait_for_selector("#vtxChan input[value='8']:checked", timeout=8000)
        p.locator("#vtxCard").scroll_into_view_if_needed()
        self.no_sideways_scroll("video, VTX card (phone)")
        boxes = p.eval_on_selector_all("#vtxChan span, #vtxPower span", "els => els.map((e) => { const r = e.getBoundingClientRect(); return [r.width, r.height, r.right]; })")
        self.assertTrue(all(w >= 44 and h >= 44 and right <= 390 for w, h, right in boxes), boxes)
        self.assertEqual(p.eval_on_selector("#vtxChan", "e => getComputedStyle(e).gridTemplateColumns.split(' ').length"), 4)
        p.locator("#vtxCard").screenshot(path=str(SHOTS / f"vtx-mobile{SUFFIX}.png"))

    def test_reboot_banner(self):
        self.net_errors_ok = True
        p = self.open("system")
        p.fill("#f-system-hostname", "racer")
        self.save("system")
        p.wait_for_selector("#rebootBanner", state="visible")
        self.assertIn("Name: applies when the Pi has restarted", p.inner_text("#rebootText"))
        p.click("#rebootLater")
        self.assertTrue(p.is_hidden("#rebootBanner"))
        p.check("#f-system-ssh")
        self.save("system")
        p.wait_for_selector("#rebootBanner", state="visible")
        p.click("#rebootNow")
        self.confirm(True, "Reboot the Pi?")
        self.until(lambda: self.posts("/api/reboot"))
        p.wait_for_selector("#waitBox", state="hidden", timeout=30000)
        self.assertTrue(p.is_hidden("#rebootBanner"))

    # ---------------------------------------------------------------- the rest
    def test_offline_banner(self):
        self.net_errors_ok = True
        p = self.open("status")
        self.assertTrue(p.is_hidden("#offline"))
        self.assertEqual(p.inner_text("#connText"), "Online")
        p.route("**/api/**", lambda r: r.abort())
        p.wait_for_selector("#offline", state="visible", timeout=8000)
        self.assertEqual(p.inner_text("#connText"), "Offline")
        self.shot("status-offline-desktop")
        p.unroute("**/api/**")
        p.wait_for_selector("#offline", state="hidden", timeout=8000)
        self.assertEqual(p.inner_text("#connText"), "Online")

    def test_theme(self):
        p = self.open("status")                                    # (a device that prefers light)
        bg = lambda: p.evaluate("getComputedStyle(document.body).backgroundColor")  # noqa: E731
        lum = lambda c: sum(int(x) for x in re.findall(r"\d+", c)[:3]) / 765  # noqa: E731
        self.assertIsNone(p.get_attribute("html", "data-theme"))
        self.assertEqual(p.get_attribute("#themeBtn", "aria-label"), "Theme: dark (change)")
        dark = bg()
        self.assertLess(lum(dark), 0.3, "the game's dark colors by default, even on a light device")
        p.click("#themeBtn")
        self.assertEqual(p.get_attribute("html", "data-theme"), "light")
        self.assertEqual(p.get_attribute("#themeBtn", "aria-label"), "Theme: light (change)")
        light = bg()
        self.assertGreater(lum(light), 0.7)
        self.assertEqual(p.get_attribute('meta[name="theme-color"][media*="dark"]', "content"), "#e7eaee")
        p.click("#themeBtn")
        self.assertEqual(p.get_attribute("html", "data-theme"), "auto")
        self.assertEqual(p.get_attribute("#themeBtn", "aria-label"), "Theme: automatic (change)")
        self.assertEqual(bg(), light, "as the device: light here")
        self.assertEqual(p.get_attribute('meta[name="theme-color"][media*="dark"]', "content"), "#0e121c")
        self.assertEqual(p.get_attribute('meta[name="theme-color"][media*="light"]', "content"), "#e7eaee")
        p.reload()
        self.ready()
        self.assertEqual(p.get_attribute("html", "data-theme"), "auto", "kept in this browser")
        self.assertEqual(bg(), light)
        p.emulate_media(color_scheme="dark")
        self.assertEqual(bg(), dark, "as the device: dark there")
        p.click("#themeBtn")
        self.assertIsNone(p.get_attribute("html", "data-theme"))
        self.assertEqual(p.get_attribute("#themeBtn", "aria-label"), "Theme: dark (change)")
        p.emulate_media(color_scheme="light")
        self.assertEqual(bg(), dark)

    def test_tracks_page(self):
        (self.pi.sd / "SCRIPTS" / "TOOLS" / "StickTimeTracks" / "C3.TXT").write_text("Lowercase Slot\n1 123 4 0\n")
        (self.pi.sd / "SCRIPTS" / "TOOLS" / "StickTimeTracks" / "c4.txt").write_text("not a track")
        (self.pi.sd / "SCRIPTS" / "TOOLS" / "StickTimeTracks" / "c5.txt").write_text("Big One\n2 973733 7 3\n")
        (self.pi.sd / "SCRIPTS" / "TOOLS" / "StickTimeTracks" / "c6.txt").write_text("Future\n3 5 6 0\n")
        p = self.open("tracks")
        p.wait_for_function(LOADED["tracks"])
        self.assertEqual(p.eval_on_selector_all(".launch a", "els => els.map((e) => e.getAttribute('href'))"),
                         ["/editor/#tracks", "/editor/#mine", "/editor/#open", "/editor/#install", "/editor/#emulator"])
        rows = p.eval_on_selector_all("#slotList li", "els => els.map((e) => e.innerText.replace(/\\s+/g, ' ').trim())")
        self.assertEqual(rows[0], "c1 MultiGP UTT 2017 track 9 Download")
        self.assertEqual(rows[1], "c2 empty track 10")
        self.assertEqual(rows[2], "c3 Lowercase Slot track 11 Download")
        self.assertEqual(rows[3], "c4 the game can't read it track 12 Download")
        self.assertEqual(rows[4], "c5 Big One track 13 Download")            # format 2 (StickTime 1.8.0)
        self.assertEqual(rows[5], "c6 the game can't read it track 14 Download")
        self.assertEqual(p.get_attribute("#slotList li:nth-child(3) a", "href"), "/api/sd/SCRIPTS/TOOLS/StickTimeTracks/C3.TXT")
        (self.pi.sd / "SCRIPTS" / "TOOLS" / "StickTimeTracks" / "c2.txt").write_text("Fresh One\n1 5 6 0\n")
        p.click("#slotsBtn")
        p.wait_for_selector("#slotList li:nth-child(2):has-text('Fresh One')")
        with self.ctx.expect_page() as newp:
            p.click(".launch a[href='/editor/#mine']", modifiers=["Control"])
        newp.value.close()

    def test_tracks_page_scenery(self):
        # StickTime 1.10.0 lists formats 3 and 4 too (tracks with scenery); the mock's game is 1.7.2 elsewhere
        self.pi.game_version = "1.10.0"
        d = self.pi.sd / "SCRIPTS" / "TOOLS" / "StickTimeTracks"
        (d / "c2.txt").write_text("Farm Field\n3 572715 11 9  0 0 1 0 0 / / 20 20 4 0 / / 32 28 7 5 0 6\n")
        (d / "c3.txt").write_text("Container Yard\n4 913 0 0  0 0 1 0 0\n")
        (d / "c4.txt").write_text("Future\n5 5 6 0\n")
        p = self.open("tracks")
        p.wait_for_function(LOADED["tracks"])
        rows = p.eval_on_selector_all("#slotList li", "els => els.map((e) => e.innerText.replace(/\\s+/g, ' ').trim())")
        self.assertEqual(rows[1], "c2 Farm Field track 10 Download")
        self.assertEqual(rows[2], "c3 Container Yard track 11 Download")
        self.assertEqual(rows[3], "c4 the game can't read it track 12 Download")

    def test_tracks_page_bw(self):
        # StickTime BW lists formats 1 and, from 1.10.0 on, 3: not the color edition's bigger 2 and 4
        self.pi.game_version = "1.10.0"
        self.pi.file_values["script"] = "/SCRIPTS/TOOLS/StickTimeBW.lua"
        d = self.pi.sd / "SCRIPTS" / "TOOLS" / "StickTimeTracks"
        (d / "c2.txt").write_text("Farm Field\n3 572715 11 9  0 0 1 0 0 / / 20 20 4 0 / / 32 28 7 5 0 6\n")
        (d / "c3.txt").write_text("Container Yard\n4 913 0 0  0 0 1 0 0\n")
        (d / "c4.txt").write_text("Big One\n2 973733 7 3\n")
        p = self.open("tracks")
        p.wait_for_function(LOADED["tracks"])
        rows = p.eval_on_selector_all("#slotList li", "els => els.map((e) => e.innerText.replace(/\\s+/g, ' ').trim())")
        self.assertEqual(rows[1], "c2 Farm Field track 10 Download")
        self.assertEqual(rows[2], "c3 the game can't read it track 11 Download")
        self.assertEqual(rows[3], "c4 the game can't read it track 12 Download")

    def test_status_page(self):
        p = self.open("status")
        p.wait_for_function(LOADED["status"])
        self.assertRegex(p.inner_text("[data-f=fps]"), r"^\d+\.\d$")
        self.assertIn(p.inner_text("[data-f=state]"), ["In the menu", "Settings", "Countdown", "Flying", "Paused", "Finished"])
        self.assertEqual(p.inner_text("[data-f=inputKind]"), "WiFi")
        self.assertEqual(p.inner_text("[data-f=netName]"), "sticktime.local")
        self.assertEqual(p.inner_text("#hostLine"), "sticktime.local · 192.168.1.23")
        self.assertEqual(p.inner_text("[data-f=throttled]"), "OK")
        self.assertEqual(p.evaluate("document.getElementById('shotImg').naturalWidth"), 320)
        src = p.get_attribute("#shotImg", "src")
        p.click("#shotBtn")
        p.wait_for_function("(s) => document.getElementById('shotImg').getAttribute('src') !== s", arg=src)
        self.assertEqual(p.eval_on_selector_all("#srcList li", "els => els.length"), 2)
        self.assertIn("(in use)", p.inner_text("#srcList li:nth-child(1)"))
        # a game error, an under-powered Pi
        orig = self.pi.status
        self.pi.game_error = "[string \"StickTime.lua\"]:12: attempt to index a nil value"
        self.pi.status = lambda: {**orig(), "system": {"cpu_temp": 81.5, "throttled": "0x50005"}}
        p.wait_for_selector("#gameError", state="visible", timeout=5000)
        self.assertIn("attempt to index a nil value", p.inner_text("#gameErrorText"))
        p.wait_for_selector("[data-f=throttled]:has-text('Now: too low voltage, throttled (use a 3 A supply)')")
        self.assertEqual(p.inner_text("[data-f=cpuTemp]"), "81.5 °C")
        self.shot("status-error-desktop")
        p.click("#gameError [data-action=reload-game]")
        self.until(lambda: self.posts("/api/game/reload"))
        self.pi.status = lambda: {**orig(), "system": {"cpu_temp": 50.0, "throttled": "0x50000"}}
        p.wait_for_selector("[data-f=throttled]:has-text('OK now (earlier: low voltage, throttled)')")

    def test_wifi_release(self):
        """Status → Inputs: Release module (asked first) lets go of the WiFi module for the PC app; Connect listens
        again; with the receiver off, neither"""
        p = self.open("status")
        p.wait_for_function(LOADED["status"])
        p.wait_for_selector("#wjRelease", state="visible")
        self.assertTrue(p.is_hidden("#wjConnect") and p.is_hidden("#wjNote"))
        p.click("#wjRelease")
        self.assertIn("So does the next start of StickTime Pi (a restart of the game, a StickTime Pi update, a reboot): "
                      "with a Module address set, that takes the module back at once.",
                      p.wait_for_selector("#confirmDlg[open]").inner_text())
        self.confirm(False, "Release the module?")
        self.assertEqual(self.posts("/api/wifi_joystick/release"), [])
        p.click("#wjRelease")
        self.confirm(True, "the PC app (or another device) can connect to it")
        self.until(lambda: self.posts("/api/wifi_joystick/release"))
        self.toast("Asked the module to stop streaming: Inputs shows its answer.")
        p.wait_for_selector("#wjConnect", state="visible", timeout=5000)
        self.assertTrue(p.is_hidden("#wjRelease"))
        self.assertTrue(p.is_visible("#wjNote"))
        # the module's answer (wifi_joystick.release), and how long the release lasts
        p.wait_for_selector("#wjResult:has-text('broadcasts again: the PC app can connect to it')", timeout=5000)
        self.assertIn("until you press Connect or StickTime Pi starts again (a restart of the game, a StickTime Pi update, "
                      "a reboot)", p.inner_text("#wjNote"))
        self.assertFalse(p.eval_on_selector("#wjNote", "e => e.classList.contains('bad')"))
        self.assertEqual(p.inner_text("[data-f=wjState]"), "released, not listening")
        self.assertEqual(p.inner_text("[data-f=wjModule]"), "none (released)")
        p.wait_for_function("document.querySelectorAll('#srcList li').length === 1")     # no WiFi source
        self.shot("status-released-desktop")
        # the other answers: not confirmed (a warning: switch the module's WiFi off and on), nothing sent (no module
        # streamed, or only a Crossfire one), waiting, and no answer to show (the note alone)
        for r, text in (("failed", "didn't confirm the stop, so it may still stream to the Pi: switch its WiFi off and on"),
                        ("none", "Nothing was sent: no module was streaming to the Pi, or it is a Crossfire module, "
                                 "which broadcasts while it streams"),
                        ("sending", "Asking the module to stop streaming to the Pi")):
            self.pi.wifi_release = r
            p.wait_for_selector(f"#wjResult:has-text({json.dumps(text)})", timeout=5000)
            self.assertEqual(p.eval_on_selector("#wjNote", "e => e.classList.contains('bad')"), r == "failed", r)
        self.pi.wifi_release = ""
        p.wait_for_function("document.querySelector('#wjResult').textContent === ''", timeout=5000)
        self.assertIn("StickTime Pi starts again", p.inner_text("#wjNote"))
        self.pi.wifi_release = "ok"
        # released with no other source: the hints say so (here and on the Sticks page), not "start the module"
        self.pi.usb_plugged = False
        p.wait_for_selector("#srcList li.note:has-text('the WiFi joystick is released (Connect, above, listens again)')",
                            timeout=5000)
        self.tab("sticks")
        p.wait_for_selector("#srcCards .note:has-text('The WiFi joystick is released: Status → Inputs → Connect listens again')",
                            timeout=8000)
        self.assertNotIn("start the ELRS or Crossfire module", p.inner_text("#srcCards"))
        self.tab("status")
        self.pi.usb_plugged = True
        p.wait_for_function("document.querySelectorAll('#srcList li').length === 1 && !document.querySelector('#srcList li.note')",
                            timeout=5000)
        p.click("#wjConnect")
        self.until(lambda: self.posts("/api/wifi_joystick/connect"))
        self.toast("Listening for the WiFi joystick again.")
        p.wait_for_selector("#wjRelease", state="visible", timeout=5000)
        self.assertTrue(p.is_hidden("#wjConnect") and p.is_hidden("#wjNote"))
        p.wait_for_selector("[data-f=wjState]:has-text('streaming')")
        self.assertEqual([r["path"] for r in self.posts() if r["path"].startswith("/api/wifi_joystick/")],
                         ["/api/wifi_joystick/release", "/api/wifi_joystick/connect"])
        for r in self.posts():
            self.assertEqual(r["body"], b"", r["path"])
        # no receiver (state off): neither button
        orig = self.pi.status
        self.pi.status = lambda: {**orig(), "wifi_joystick": {"module": "", "state": "off", "rate_hz": 0.0, "dropped": 0,
                                                             "gaps": 0, "gap_max_ms": 0}}
        p.wait_for_selector("#wjRelease", state="hidden", timeout=5000)
        self.assertTrue(p.is_hidden("#wjConnect") and p.is_hidden("#wjNote"))
        self.assertTrue(p.is_hidden("#wjBtns"), "the empty button row takes no room")

    def test_wifi_release_ends_with_restart(self):
        """a Release is not saved: after a restart of the game the Pi listens again (Release module shows, not Connect)"""
        self.net_errors_ok = True                                   # the game restarts: the Pi is away a moment
        p = self.open("status")
        p.wait_for_function(LOADED["status"])
        p.click("#wjRelease")
        self.confirm(True)
        p.wait_for_selector("#wjConnect", state="visible", timeout=5000)
        self.tab("system")
        p.click("#restartBtn")
        self.confirm(True, "Restart the game?")
        p.wait_for_selector("#waitBox", state="visible")
        p.wait_for_selector("#waitBox", state="hidden", timeout=20000)
        self.tab("status")
        p.wait_for_selector("#wjRelease", state="visible", timeout=8000)
        self.assertTrue(p.is_hidden("#wjConnect") and p.is_hidden("#wjNote"))
        p.wait_for_selector("[data-f=wjState]:has-text('streaming')", timeout=5000)
        self.assertEqual(self.posts("/api/wifi_joystick/connect"), [])

    def test_wifi_release_phone(self):
        p = self.open("status", mobile=True, dark=True)
        p.wait_for_function(LOADED["status"])
        p.click("#wjRelease")
        self.confirm(True)
        p.wait_for_selector("#wjConnect", state="visible", timeout=5000)
        self.no_sideways_scroll("status, released (phone)")
        self.shot("status-released-mobile")

    def test_every_control_has_a_name(self):
        p = self.open("status")
        for t in TABS:
            self.tab(t)
            p.wait_for_function(LOADED[t], timeout=10000)
            nameless = p.evaluate("""(t) => {
              const sec = document.getElementById('p-' + t), out = [];
              const named = (e) => (e.labels && e.labels.length && [...e.labels].some((l) => l.textContent.trim())) || e.getAttribute('aria-label')
                || e.getAttribute('aria-labelledby') || (e.closest('label') && e.closest('label').textContent.trim()) || e.title;
              for (const e of sec.querySelectorAll('input, select, textarea, button, a[href]')) {
                if (!e.offsetParent && e.type !== 'radio' && e.type !== 'checkbox' && e.type !== 'file') continue;
                const text = (e.textContent || '').trim();
                if (e.matches('button, a') ? !(text || e.getAttribute('aria-label')) : !named(e)) out.push(e.outerHTML.slice(0, 120));
              }
              for (const img of sec.querySelectorAll('img')) if (!img.alt) out.push(img.outerHTML.slice(0, 120));
              return out;
            }""", t)
            self.assertEqual(nameless, [], t)
        for dlg in ("#confirmDlg", "#joinDlg"):
            self.assertTrue(p.get_attribute(dlg, "aria-labelledby"))

    def test_bad_answers(self):
        self.net_errors_ok = True
        self.pi.fail["/api/settings"] = (500, "Internal", "Can't read the settings.")
        p = self.open_raw("video")
        p.wait_for_selector("form[data-form=video] .alert.bad:has-text(\"Couldn't read the settings: Can't read the settings.\")")
        self.pi.fail.clear()
        p.click("form[data-form=video] button:has-text('Try again')")
        p.wait_for_function(LOADED["video"])
        # garbage where JSON objects should be: dashes, no errors
        p.route("**/api/status", lambda r: r.fulfill(status=200, content_type="application/json", body="{}"))
        p.route("**/api/update", lambda r: r.fulfill(status=200, content_type="application/json", body='{"state":"weird","game":null,"app":"x"}'))
        p.route("**/api/songs", lambda r: r.fulfill(status=200, content_type="application/json", body='{"songs":"nope"}'))
        p.route("**/api/tools", lambda r: r.fulfill(status=200, content_type="application/json", body='{}'))
        p.route("**/api/sticks/stream", lambda r: r.fulfill(status=200, content_type="text/event-stream", body='data: {"sources":7}\n\ndata: not json\n\n'))
        for t in TABS:
            self.tab(t)
            p.wait_for_timeout(400)
        self.tab("status")
        p.wait_for_function("document.querySelector('[data-f=fps]').textContent === '–'", timeout=8000)

    def open_raw(self, tab):
        """open without waiting for the settings (they may fail)"""
        self.ctx = self.br.new_context(viewport=DESKTOP)
        p = self.page = self.ctx.new_page()
        p.on("console", lambda m: m.type == "error" and self.errors.append(m.text))
        p.on("pageerror", lambda e: self.errors.append("pageerror: " + str(e)))
        p.goto(self.base + "#" + tab)
        return p


if __name__ == "__main__":
    unittest.main(verbosity=2)
