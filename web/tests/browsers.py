"""The browser the Playwright tests run in: STP_BROWSER = chromium (default), firefox or webkit.

The Pi's pages are http://sticktime.local, not a secure context; Chromium and Firefox get that name
mapped to 127.0.0.1. WebKit can't map names, so it opens http://127.0.0.1, which IS a secure context
(native crypto.subtle): SECURE tells the tests that.
"""
import os

NAME = os.environ.get("STP_BROWSER", "chromium").lower()
if NAME not in ("chromium", "firefox", "webkit"):
    raise SystemExit(f"STP_BROWSER={NAME}: chromium, firefox or webkit")
HOST = "127.0.0.1" if NAME == "webkit" else "sticktime.local"
SECURE = NAME == "webkit"


def launch(pw):
    if NAME == "chromium":
        return pw.chromium.launch(args=[f"--host-resolver-rules=MAP {HOST} 127.0.0.1"])
    if NAME == "firefox":
        return pw.firefox.launch(firefox_user_prefs={"network.dns.localDomains": HOST, "network.proxy.type": 0})
    return pw.webkit.launch()


def mobile(opts):
    """a phone's context options (Firefox has no mobile emulation: just the size)"""
    if NAME != "firefox":
        opts.update(is_mobile=True, has_touch=True)
    return opts
