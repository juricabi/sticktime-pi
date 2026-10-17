"""A stand-in for GitHub for the updater's tests: the REST API (releases, refs, tags, trees, asset
downloads), release downloads through a redirect to a "CDN", raw.githubusercontent.com and the
Pages site. Stdlib only; it runs in a thread of the test process, which changes its data and
injects failures between runs.

    gh = FakeGitHub(); gh.start()
    gh.add_release("juricabi/sticktime", "v1.7.3", "StickTime-1.7.3-sdcard.zip", zipbytes,
                   files={"web/simulator.html": page, ...})
    gh.repo("juricabi/sticktime-pi", private=True, token="...")      # 404 without it, 401 with another
    gh.fault(r"^/cdn/", truncate=1000, times=1)
    gh.api_quota = 2              # then 403 with X-RateLimit-Remaining: 0
    gh.host_state = "fly"         # GET /status answers like the host's /api/status
    env: STICKTIME_GITHUB_API=gh.api  STICKTIME_GITHUB_RAW=gh.raw  STICKTIME_PAGES=gh.pages
         STICKTIME_RELOAD_URL=gh.base + "/reload" (POSTs are recorded)  STICKTIME_STATUS_URL=gh.base + "/status"
"""
import hashlib
import http.server
import json
import re
import socket
import threading
import time
import urllib.parse


def blob_sha1(b):
    return hashlib.sha1(b"blob %d\0" % len(b) + b).hexdigest()


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


class Repo:
    def __init__(self, name, private=False, token=None):
        self.name = name
        self.private = private
        self.token = token
        self.releases = []        # GitHub's release objects
        self.assets = {}          # asset id -> bytes
        self.refs = {}            # tag -> (type, sha)
        self.tags = {}            # annotated tag sha -> commit sha
        self.trees = {}           # commit sha -> {path: bytes}
        self.truncated = set()    # commits whose tree listing is truncated


class FakeGitHub:
    def __init__(self):
        self.repos = {}
        self.gallery = None           # bytes of tracks/index.json on the Pages site
        self.gallery_type = "application/json; charset=utf-8"
        self.faults = []
        self.requests = []            # (method, path, headers)
        self.posts = []               # POSTs (the game reload)
        self.api_quota = None         # API calls left before 403 rate limit (None: unlimited)
        self.token_expiry = "2099-01-01 00:00:00 UTC"     # github-authentication-token-expiration
        self.host_state = None        # the host's /api/status game.state_name at /status (None: 404)
        self._ids = 1000
        self.lock = threading.Lock()
        self.httpd = None

    # ---------------------------------------------------------------- setup
    def start(self):
        gh = self

        class H(Handler):
            fake = gh
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        return self

    def stop(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()

    @property
    def base(self):
        return "http://127.0.0.1:%d" % self.port

    @property
    def api(self):
        return self.base + "/api"

    @property
    def raw(self):
        return self.base + "/raw"

    @property
    def pages(self):
        return self.base + "/pages"

    def repo(self, name, private=None, token=None):
        r = self.repos.get(name)
        if r is None:
            r = self.repos[name] = Repo(name)
        if private is not None:
            r.private = private
        if token is not None:
            r.token = token
        return r

    def new_id(self):
        self._ids += 1
        return self._ids

    def add_release(self, repo, tag, asset_name=None, data=None, files=None, digest=True, draft=False,
                    prerelease=False, published=None, updated=None, created=None, body="", state="uploaded",
                    annotated=False, extra_assets=None, size=None, truncated_tree=False):
        """a release with one asset (or none: asset_name None); files = the repo at the tag's commit"""
        r = self.repo(repo)
        now = time.time()
        published = now - 7200 if published is None else published
        updated = published if updated is None else updated
        created = published if created is None else created
        commit = hashlib.sha1(("%s %s %s %d" % (repo, tag, time.time(), self.new_id())).encode()).hexdigest()
        r.trees[commit] = dict(files or {})
        if truncated_tree:
            r.truncated.add(commit)
        if annotated:
            tsha = hashlib.sha1(("tag " + commit).encode()).hexdigest()
            r.tags[tsha] = commit
            r.refs[tag] = ("tag", tsha)
        else:
            r.refs[tag] = ("commit", commit)
        assets = []
        for name, b in ([(asset_name, data)] if asset_name else []) + list((extra_assets or {}).items()):
            aid = self.new_id()
            r.assets[aid] = b
            assets.append({
                "url": "%s/repos/%s/releases/assets/%d" % (self.api, repo, aid), "id": aid, "name": name, "label": "",
                "content_type": "application/octet-stream", "state": state if name == asset_name else "uploaded",
                "size": len(b) if size is None or name != asset_name else size,
                "digest": ("sha256:" + hashlib.sha256(b).hexdigest()) if (digest is True or name != asset_name)
                else digest,
                "download_count": 1, "created_at": iso(updated), "updated_at": iso(updated),
                "browser_download_url": "%s/dl/%s/releases/download/%s/%s" % (self.base, repo, tag, name)})
        rel = {"url": "%s/repos/%s/releases/%d" % (self.api, repo, self.new_id()), "id": self.new_id(),
               "tag_name": tag, "target_commitish": "0" * 40, "name": tag, "draft": draft, "prerelease": prerelease,
               "immutable": False, "created_at": iso(created), "published_at": iso(published), "updated_at": iso(updated),
               "assets": assets, "body": body}
        r.releases = [x for x in r.releases if x["tag_name"] != tag] + [rel]
        return rel, commit

    def remove_release(self, repo, tag):
        r = self.repo(repo)
        r.releases = [x for x in r.releases if x["tag_name"] != tag]

    def fault(self, pattern, times=None, **action):
        """for requests whose path matches pattern (re.search): status=, headers=, body=, ratelimit=True,
        truncate=n, corrupt=True, delay=s, drop=True, html=True, redirect=url, slow=s_per_chunk"""
        with self.lock:
            self.faults.append({"re": re.compile(pattern), "times": times, "action": action})

    def clear(self):
        with self.lock:
            self.faults = []
            self.requests = []
            self.posts = []
            self.api_quota = None

    def reset(self):
        """a fresh GitHub for the next test"""
        self.clear()
        self.repos = {}
        self.gallery = None
        self.gallery_type = "application/json; charset=utf-8"
        self.token_expiry = "2099-01-01 00:00:00 UTC"
        self.host_state = None

    def count(self, pattern, method="GET"):
        rx = re.compile(pattern)
        return sum(1 for m, p, _ in list(self.requests) if m == method and rx.search(p))

    def headers_of(self, pattern):
        rx = re.compile(pattern)
        return [h for _, p, h in list(self.requests) if rx.search(p)]

    def _take_fault(self, path):
        with self.lock:
            for f in self.faults:
                if f["re"].search(path) and (f["times"] is None or f["times"] > 0):
                    if f["times"] is not None:
                        f["times"] -= 1
                    return f["action"]
        return None


class Handler(http.server.BaseHTTPRequestHandler):
    fake = None
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        self.fake.posts.append((self.path, {k.lower(): v for k, v in self.headers.items()}, body))
        self.fake.requests.append(("POST", self.path, {k.lower(): v for k, v in self.headers.items()}))
        self._send(200, {"Content-Type": "application/json"}, b'{"ok":true}')

    def do_GET(self):
        hdrs = {k.lower(): v for k, v in self.headers.items()}
        self.fake.requests.append(("GET", self.path, hdrs))
        act = self.fake._take_fault(self.path) or {}
        if act.get("delay"):
            time.sleep(act["delay"])
        if act.get("drop"):
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            return
        if act.get("ratelimit"):
            return self._send(403, {"Content-Type": "application/json; charset=utf-8", "X-RateLimit-Limit": "60",
                                    "X-RateLimit-Remaining": "0", "X-RateLimit-Reset": str(int(time.time()) + 900),
                                    "X-RateLimit-Resource": "core"},
                              b'{"message":"API rate limit exceeded for 127.0.0.1."}')
        if "status" in act:
            h = {"Content-Type": "application/json; charset=utf-8"}
            h.update(act.get("headers") or {})
            return self._send(act["status"], h, act.get("body", b'{"message":"injected"}'))
        if act.get("html"):
            return self._send(200, {"Content-Type": "text/html"}, b"<!doctype html><title>Log in to the hotspot</title>")
        if act.get("redirect"):
            return self._send(302, {"Location": act["redirect"]}, b"")
        status, h, body = self.route(hdrs)
        if act.get("corrupt") and body:
            body = bytes([body[0] ^ 0xFF]) + body[1:]
        if act.get("truncate") is not None and status == 200:
            h["Content-Length"] = str(len(body))
            return self._send(status, h, body[:act["truncate"]], length_set=True)
        if act.get("slow") and status == 200:
            h["Content-Length"] = str(len(body))
            self.send_response(status)
            for k, v in h.items():
                self.send_header(k, v)
            self.end_headers()
            try:
                for i in range(0, len(body), 1024):
                    self.wfile.write(body[i:i + 1024])
                    self.wfile.flush()
                    time.sleep(act["slow"])
            except OSError:
                pass
            return
        self._send(status, h, body)

    def _send(self, status, headers, body, length_set=False):
        try:
            self.send_response(status)
            for k, v in headers.items():
                self.send_header(k, v)
            if not length_set:
                self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except OSError:
            pass

    # ---------------------------------------------------------------- routes
    def route(self, hdrs):
        u = urllib.parse.urlsplit(self.path)
        path, q = u.path, urllib.parse.parse_qs(u.query)
        f = self.fake
        jtype = {"Content-Type": "application/json; charset=utf-8"}
        notfound = (404, dict(jtype), b'{"message":"Not Found","documentation_url":"https://docs.github.com/rest"}')
        m = re.match(r"^/api/repos/([^/]+/[^/]+)/(.*)$", path)
        if m:
            repo = f.repos.get(m.group(1))
            rest = m.group(2)
            if f.api_quota is not None:
                if f.api_quota <= 0:
                    return 403, dict(jtype, **{"X-RateLimit-Remaining": "0", "X-RateLimit-Reset": str(int(time.time()) + 900)}), \
                        b'{"message":"API rate limit exceeded"}'
                f.api_quota -= 1
            if repo is None:
                return notfound
            auth = hdrs.get("authorization", "")
            if repo.private or (auth and repo.token):
                if not auth:
                    return notfound
                if auth != "Bearer " + (repo.token or ""):
                    return 401, dict(jtype), b'{"message":"Bad credentials"}'
            h = dict(jtype, **{"X-RateLimit-Remaining": "59" if f.api_quota is None else str(f.api_quota)})
            if auth:
                h["github-authentication-token-expiration"] = f.token_expiry
            if rest == "releases":
                n = int((q.get("per_page") or ["30"])[0])
                rels = sorted(repo.releases, key=lambda r: r["created_at"], reverse=True)[:n]
                return 200, h, json.dumps(rels).encode()
            mm = re.match(r"^git/ref/tags/(.+)$", rest)
            if mm:
                tag = urllib.parse.unquote(mm.group(1))
                if tag not in repo.refs:
                    return notfound
                typ, sha = repo.refs[tag]
                return 200, h, json.dumps({"ref": "refs/tags/" + tag, "object": {"sha": sha, "type": typ}}).encode()
            mm = re.match(r"^git/tags/([0-9a-f]{40})$", rest)
            if mm:
                c = repo.tags.get(mm.group(1))
                if c is None:
                    return notfound
                return 200, h, json.dumps({"sha": mm.group(1), "object": {"sha": c, "type": "commit"}}).encode()
            mm = re.match(r"^git/trees/([0-9a-f]{40})$", rest)
            if mm:
                files = repo.trees.get(mm.group(1))
                if files is None:
                    return notfound
                tree = [{"path": p, "mode": "100644", "type": "blob", "sha": blob_sha1(b), "size": len(b)}
                        for p, b in sorted(files.items())]
                return 200, h, json.dumps({"sha": "1" * 40, "tree": tree,
                                           "truncated": mm.group(1) in repo.truncated}).encode()
            mm = re.match(r"^releases/assets/(\d+)$", rest)
            if mm:
                aid = int(mm.group(1))
                if aid not in repo.assets:
                    return notfound
                if "application/octet-stream" in hdrs.get("accept", ""):
                    return 302, {"Location": "%s/cdn/%d" % (f.base, aid)}, b""
                return 200, h, b'{"id":%d}' % aid
            return notfound
        m = re.match(r"^/dl/([^/]+/[^/]+)/releases/download/([^/]+)/([^/]+)$", path)
        if m:
            repo = f.repos.get(m.group(1))
            if repo is None or repo.private:
                return 404, {"Content-Type": "text/plain"}, b"Not Found"
            for rel in repo.releases:
                if rel["tag_name"] == m.group(2):
                    for a in rel["assets"]:
                        if a["name"] == m.group(3):
                            return 302, {"Location": "%s/cdn/%d?sig=abc" % (f.base, a["id"])}, b""
            return 404, {"Content-Type": "text/plain"}, b"Not Found"
        m = re.match(r"^/cdn/(\d+)$", path)
        if m:
            for repo in f.repos.values():
                b = repo.assets.get(int(m.group(1)))
                if b is not None:
                    return 200, {"Content-Type": "application/octet-stream", "Accept-Ranges": "bytes"}, b
            return 404, {"Content-Type": "text/plain"}, b"Not Found"
        m = re.match(r"^/raw/([^/]+/[^/]+)/([^/]+)/(.+)$", path)
        if m:
            repo = f.repos.get(m.group(1))
            ref = m.group(2)
            if repo is not None and ref in repo.refs:
                typ, sha = repo.refs[ref]
                ref = repo.tags.get(sha, sha) if typ == "tag" else sha
            files = repo.trees.get(ref) if repo is not None else None
            if files is None or m.group(3) not in files:
                return 404, {"Content-Type": "text/plain; charset=utf-8"}, b"404: Not Found"
            return 200, {"Content-Type": "text/plain; charset=utf-8", "Cache-Control": "max-age=300"}, files[m.group(3)]
        if path == "/status":
            if f.host_state is None:
                return 404, {"Content-Type": "text/plain"}, b"Not Found"
            return 200, dict(jtype), json.dumps({"version": "0.1.0", "game": {"state_name": f.host_state}}).encode()
        if path == "/pages/tracks/index.json":
            if f.gallery is None:
                return 404, {"Content-Type": "text/html"}, b"<!doctype html><title>404</title>"
            return 200, {"Content-Type": f.gallery_type}, f.gallery
        return 404, {"Content-Type": "text/plain"}, b"Not Found"


if __name__ == "__main__":
    gh = FakeGitHub().start()
    print("fake GitHub at", gh.base, flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        gh.stop()
