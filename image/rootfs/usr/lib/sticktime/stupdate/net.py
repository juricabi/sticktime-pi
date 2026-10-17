"""HTTP for the updater: capped reads with a total deadline, https-only redirects to GitHub's hosts,
the token never following a redirect, GitHub's rate limit remembered, and every failure turned
into an UpdateError with a message a user understands."""
import calendar
import email.utils
import http.client
import json
import socket
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request

from .common import MIB, UpdateError, fail, log, read_json, write_state, json_bytes

GITHUB_HOSTS = ("github.com", "api.github.com", "juricabi.github.io")


def host_ok(url, cfg):
    """https to GitHub's own hosts; plain http only to a configured base (the tests' fake server)"""
    try:
        u = urllib.parse.urlsplit(url)
    except ValueError:
        return False
    for b in cfg.bases():
        bu = urllib.parse.urlsplit(b)
        if (u.scheme, u.netloc.lower()) == (bu.scheme, bu.netloc.lower()):
            return True
    if u.scheme != "https":
        return False
    h = (u.hostname or "").lower()
    return h in GITHUB_HOSTS or h.endswith(".githubusercontent.com")


def _host(url):
    try:
        return urllib.parse.urlsplit(url).hostname or url
    except ValueError:
        return url


class _Redirects(urllib.request.HTTPRedirectHandler):
    max_redirections = 5

    def __init__(self, cfg):
        super().__init__()
        self.cfg = cfg

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        newurl = urllib.parse.urljoin(req.full_url, newurl)
        if not host_ok(newurl, self.cfg):
            fail("unexpected_response", "GitHub sent the download somewhere unexpected (%s), so it wasn't followed."
                 % _host(newurl))
        # add_unredirected_header() keeps the token off the new host; urllib copies only req.headers
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class Resp:
    def __init__(self, status, headers, body, url):
        self.status = status
        self.headers = headers
        self.body = body
        self.url = url

    def date(self):
        """the server's clock (the Pi may have none yet)"""
        try:
            return email.utils.parsedate_to_datetime(self.headers.get("date", "")).timestamp()
        except (TypeError, ValueError, IndexError, OverflowError):
            return None


class Http:
    def __init__(self, cfg, sleep=time.sleep):
        self.cfg = cfg
        self.sleep = sleep
        self.opener = urllib.request.build_opener(_Redirects(cfg))
        self.token_expiry = None      # from github-authentication-token-expiration

    # ---- GitHub's API limit (60 an hour per public IP without a token, shared by the whole home)
    def _rl_path(self):
        return self.cfg.s("ratelimit.json")

    def limited_until(self):
        d = read_json(self._rl_path())
        r = d.get("reset") if isinstance(d, dict) else None
        return r if isinstance(r, (int, float)) and r > time.time() else None

    def _rate_limited(self, reset):
        reset = int(reset)
        try:
            write_state(self._rl_path(), json_bytes({"reset": reset}))
        except OSError:
            pass
        self._rl_fail(reset)

    def _rl_fail(self, reset):
        at = time.strftime("%H:%M", time.localtime(reset))
        fail("rate_limited", "GitHub's limit for this network was reached (60 checks an hour, shared by every "
             "device on it). Try again after %s." % at, retry_at=int(reset))

    def get(self, url, cap, deadline, accept=None, token=None, api=False, progress=None, retries=2):
        if not host_ok(url, self.cfg):
            fail("unexpected_response", "Refused to fetch %s (not GitHub)." % _host(url))
        if api:
            r = self.limited_until()
            if r:
                self._rl_fail(r)
        headers = {"User-Agent": self.cfg.ua, "Accept": accept or ("application/vnd.github+json" if api else "*/*")}
        if api:
            headers["X-GitHub-Api-Version"] = "2022-11-28"
        t_end = time.monotonic() + deadline
        attempt = 0
        while True:
            req = urllib.request.Request(url, headers=headers)
            if token:
                req.add_unredirected_header("Authorization", "Bearer " + token)
            try:
                return self._once(req, url, cap, t_end, progress)
            except urllib.error.HTTPError as e:
                wait = self._http_error(e, url, api, token)
                attempt += 1
                if wait is None or attempt > retries or time.monotonic() + wait > t_end:
                    raise UpdateError("rate_limited", "%s is limiting downloads from this network right now; try "
                                      "again in a few minutes." % _host(url), retry_at=int(time.time() + (wait or 60)))
                log("%s: 429, retrying in %d s" % (_host(url), wait))
                self.sleep(wait)
            except UpdateError:
                raise
            except Exception as e:
                raise self._classify(e, url)

    def _once(self, req, url, cap, t_end, progress):
        with self.opener.open(req, timeout=self.cfg.timeout) as r:
            hdrs = {k.lower(): v for k, v in r.headers.items()}
            exp = hdrs.get("github-authentication-token-expiration")
            if exp:
                self.token_expiry = exp
            clen = hdrs.get("content-length")
            try:
                clen = int(clen) if clen is not None else None
            except ValueError:
                clen = None
            if clen is not None and clen > cap:
                fail("too_big", "%s sent %.1f MB, more than the %.1f MB expected; refused." % (_host(url), clen / MIB, cap / MIB))
            buf = bytearray()
            while True:
                if time.monotonic() > t_end:
                    fail("timeout", "The download from %s took too long; try again." % _host(url))
                chunk = r.read(65536)
                if not chunk:
                    break
                buf += chunk
                if len(buf) > cap:
                    fail("too_big", "%s sent more than the %.1f MB expected; refused." % (_host(url), cap / MIB))
                if progress:
                    progress(len(buf), clen)
            if clen is not None and len(buf) != clen:
                fail("download_incomplete", "The download from %s broke off (%d of %d bytes); try again."
                     % (_host(url), len(buf), clen))
            return Resp(r.status, hdrs, bytes(buf), r.geturl())

    def _http_error(self, e, url, api, token):
        """raises the UpdateError for an HTTP error status; returns seconds to wait for a retryable 429"""
        hdrs = {k.lower(): v for k, v in (e.headers.items() if e.headers else [])}
        try:
            body = e.read(65536)
        except Exception:
            body = b""
        finally:
            e.close()
        code = e.code
        if code in (403, 429):
            rem = hdrs.get("x-ratelimit-remaining")
            reset = hdrs.get("x-ratelimit-reset")
            ra = hdrs.get("retry-after")
            try:
                ra = max(0, int(ra)) if ra is not None else None
            except ValueError:
                ra = 60
            if rem == "0" or ra is not None or code == 429:
                if api:
                    try:
                        at = int(reset) if rem == "0" and reset else time.time() + (ra if ra is not None else 60)
                    except ValueError:
                        at = time.time() + 60
                    self._rate_limited(at)
                return ra if ra is not None else 60
            if token:
                fail("token_forbidden", "The GitHub token may not read %s (it needs Contents: read-only access to the "
                     "repository)." % urllib.parse.urlsplit(url).path)
            fail("http_error", "GitHub refused the request (403): %s" % _gh_message(body))
        if code == 401:
            fail("token_rejected", "GitHub rejected the token (expired or wrong): make a new one and put it in "
                 "github_token.")
        if code == 404:
            fail("not_found", "Not found on GitHub: %s" % urllib.parse.urlsplit(url).path)
        if code >= 500:
            fail("server_error", "GitHub had a problem (%d); try again later." % code)
        fail("http_error", "GitHub answered %d for %s." % (code, urllib.parse.urlsplit(url).path))

    def _classify(self, e, url):
        host = _host(url)
        r = e.reason if isinstance(e, urllib.error.URLError) else e
        if isinstance(r, ssl.SSLCertVerificationError) or isinstance(r, ssl.SSLError):
            if "not yet valid" in str(r):
                return UpdateError("clock_not_set", "The Pi's clock isn't set yet (it gets the time from the internet), "
                                   "so secure downloads fail. Try again in a minute.")
            return UpdateError("unexpected_response", "The secure connection to %s failed (a hotspot login page or a "
                               "proxy in the way?)." % host)
        if isinstance(r, socket.gaierror):
            return UpdateError("offline", "No internet: couldn't find %s. The installed versions stay; try again "
                               "when the Pi is online." % host)
        if isinstance(r, (TimeoutError, socket.timeout)):
            return UpdateError("timeout", "%s didn't answer in time; try again." % host)
        if isinstance(r, (http.client.IncompleteRead, http.client.RemoteDisconnected, ConnectionResetError,
                          ConnectionAbortedError, http.client.BadStatusLine)):
            return UpdateError("download_incomplete", "The connection to %s broke off; try again." % host)
        if isinstance(r, http.client.HTTPException):
            return UpdateError("unexpected_response", "%s sent something that isn't HTTP." % host)
        if isinstance(r, OSError):
            return UpdateError("offline", "Couldn't connect to %s (%s). The installed versions stay; try again when "
                               "the Pi is online." % (host, getattr(r, "strerror", None) or r))
        return UpdateError("unexpected_response", "Fetching from %s failed: %s" % (host, r))

    def get_json(self, url, cap=2 * MIB, deadline=30, token=None, api=True):
        resp = self.get(url, cap, deadline, token=token, api=api)
        ctype = resp.headers.get("content-type", "")
        if "json" not in ctype.lower():
            fail("unexpected_response", "Got a web page instead of GitHub's answer from %s (a hotspot login page "
                 "or a proxy?)." % _host(url))
        try:
            return json.loads(resp.body.decode("utf-8")), resp
        except (UnicodeDecodeError, ValueError):
            fail("unexpected_response", "%s sent broken JSON." % _host(url))


def _gh_message(body):
    try:
        m = json.loads(body.decode("utf-8")).get("message")
        return str(m)[:200] if m else "no reason given"
    except Exception:
        return "no reason given"


# ---------------------------------------------------------------- releases

class Release:
    """the parts of a GitHub release (and its one asset) the updater uses"""
    FIELDS = ("version", "tag", "release_id", "asset_id", "asset_name", "size", "digest", "url", "api_url",
              "updated_at", "published_at", "notes", "sums_url", "sums_api_url")

    def __init__(self, **kw):
        for f in self.FIELDS:
            setattr(self, f, kw.get(f))

    def to_dict(self):
        return {f: getattr(self, f) for f in self.FIELDS}

    @classmethod
    def from_dict(cls, d):
        return cls(**{f: d.get(f) for f in cls.FIELDS})


def notes_of(body):
    """the release body's '### New in X' section (up to 'Earlier changes'), else the body"""
    if not isinstance(body, str):
        return ""
    body = body.replace("\r\n", "\n")
    i = body.find("### New in")
    if i >= 0:
        j = body.find("Earlier changes", i)
        part = body[i:j if j >= 0 else len(body)]
    else:
        part = body
    return part.strip()[:4000]


def _digest(a):
    d = a.get("digest")
    if d is None:
        return None, True
    if isinstance(d, str) and len(d) == 71 and d.startswith("sha256:"):
        h = d[7:].lower()
        if all(c in "0123456789abcdef" for c in h):
            return h, True
    return None, False


def pick_release(releases, asset_fmt, max_size, want=None):
    """(best, pending): the highest version with its asset uploaded (never /releases/latest: it sorts by
    commit date, and tags move), and a higher version still waiting for its asset (or None)"""
    from .common import TAG_RE, vkey
    best, pending = None, None
    for r in releases if isinstance(releases, list) else []:
        if not isinstance(r, dict) or r.get("draft") or r.get("prerelease"):
            continue
        tag = r.get("tag_name")
        if not isinstance(tag, str) or len(tag) > 32:
            continue
        m = TAG_RE.fullmatch(tag)
        if not m:
            continue
        ver = m.group(1)
        if want is not None and vkey(ver) != vkey(want):
            continue
        name = asset_fmt.format(ver=ver)
        assets = [a for a in (r.get("assets") or []) if isinstance(a, dict)]
        a = next((a for a in assets if a.get("name") == name), None)
        digest, dig_ok = _digest(a) if a else (None, False)
        ok = (a is not None and a.get("state") == "uploaded" and isinstance(a.get("size"), int)
              and 0 < a["size"] <= max_size and isinstance(a.get("browser_download_url"), str)
              and isinstance(a.get("id"), int) and dig_ok)
        if not ok:
            if pending is None or vkey(ver) > vkey(pending):
                pending = ver
            continue
        sums = next((s for s in assets if s.get("name") == "SHA256SUMS" and s.get("state") == "uploaded"
                     and isinstance(s.get("size"), int) and 0 < s["size"] <= 65536), None)
        rel = Release(version=ver, tag=tag, release_id=r.get("id"), asset_id=a["id"], asset_name=name,
                      size=a["size"], digest=digest, url=a["browser_download_url"], api_url=a.get("url"),
                      updated_at=a.get("updated_at") or "", published_at=r.get("published_at") or "",
                      notes=notes_of(r.get("body")),
                      sums_url=sums.get("browser_download_url") if sums else None,
                      sums_api_url=sums.get("url") if sums else None)
        if best is None or vkey(ver) > vkey(best.version) or (
                vkey(ver) == vkey(best.version) and rel.published_at > best.published_at):
            best = rel
    if pending is not None and best is not None and vkey(pending) <= vkey(best.version):
        pending = None
    return best, pending


def iso_time(s):
    """GitHub's '2026-10-14T09:23:30Z' -> epoch seconds, None when unparsable"""
    try:
        return calendar.timegm(time.strptime(s, "%Y-%m-%dT%H:%M:%SZ"))
    except (TypeError, ValueError, OverflowError):
        return None
