#!/usr/bin/env python3
"""End-to-end tests: real `zeropfin` daemon + mock ZeroPlay + fake Jellyfin.

Covers acceptance tests 1-5 of the project brief on an x86 host: pairing and
CSRF/Origin/Host defenses, browsing, eligibility, playback control and state
transitions, Jellyfin session reports (including crash != completion), the
loopback media relay (Range/206/416/HEAD, redirects, resume after upstream
drop, aborts, bounded memory, revocation), static asset serving and secret
hygiene in logs. Standard library only.
"""
import argparse
import http.client
import importlib.util
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))

passed = 0
failed = []


def check(cond, what):
    global passed
    if cond:
        passed += 1
    else:
        failed.append(what)
        print(f"  FAIL: {what}", flush=True)
    return cond


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def load_fake_jellyfin():
    spec = importlib.util.spec_from_file_location("fakejf", os.path.join(HERE, "..", "mock_jellyfin", "server.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Client:
    """Minimal browser-like client: cookie jar, CSRF header, explicit Host/Origin."""

    def __init__(self, port):
        self.port = port
        self.cookie = None
        self.csrf = None

    def req(self, method, path, body=None, headers=None, raw=False, host=None, origin="auto", timeout=30):
        h = {"Host": host or f"127.0.0.1:{self.port}"}
        if self.cookie:
            h["Cookie"] = self.cookie
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            h["Content-Type"] = "application/json"
        if method == "POST":
            if origin == "auto":
                h["Origin"] = f"http://{h['Host']}"
            elif origin:
                h["Origin"] = origin
            if self.csrf:
                h["X-CSRF-Token"] = self.csrf
        h.update(headers or {})
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        c.request(method, path, body=data, headers=h)
        r = c.getresponse()
        payload = r.read()
        sc = r.getheader("Set-Cookie")
        if sc and sc.startswith("zf_session="):
            val = sc.split(";", 1)[0]
            self.cookie = None if val == "zf_session=" else val
        c.close()
        if raw:
            return r, payload
        try:
            return r.status, json.loads(payload) if payload else None
        except ValueError:
            return r.status, payload


class Daemon:
    def __init__(self, args, jf_port, tmp, flags, name):
        self.args, self.tmp, self.name = args, tmp, name
        self.port = free_port()
        self.state = os.path.join(tmp, f"state-{name}")
        self.run = os.path.join(tmp, f"run-{name}")
        os.makedirs(self.state, mode=0o700)
        os.makedirs(self.run, mode=0o700)
        lib = json.load(open(os.path.join(args.fixtures, "library.json")))
        self.token = lib["token"]
        creds = {"serverUrl": f"http://127.0.0.1:{jf_port}", "serverId": lib["server"]["Id"],
                 "serverName": "Fixture Jellyfin", "serverVersion": "10.10.7", "userId": lib["user"]["Id"],
                 "userName": "demo", "accessToken": self.token, "deviceId": "zeropfin-test-device"}
        cpath = os.path.join(self.state, "credentials.json")
        with open(cpath, "w") as f:
            json.dump(creds, f)
        os.chmod(cpath, 0o600)
        self.mocklog = os.path.join(tmp, f"mock-{name}.log")
        self.conf = os.path.join(tmp, f"zeropfin-{name}.conf")
        with open(self.conf, "w") as f:
            f.write(f"listen_address = 127.0.0.1\nlisten_port = {self.port}\nrelay_port = 0\n"
                    f"web_root = {os.path.abspath(args.web_root)}\nstate_dir = {self.state}\n"
                    f"runtime_dir = {self.run}\nzeroplay_path = {os.path.abspath(args.mock_zeroplay)}\n"
                    "player_ready_timeout_s = 5\nplayer_load_timeout_s = 10\nprogress_interval_s = 5\n"
                    "device_name = Test Receiver\nverbose = true\n")
        env = dict(os.environ)
        env.pop("STATE_DIRECTORY", None)
        env.pop("RUNTIME_DIRECTORY", None)
        env["MOCK_ZEROPLAY_FLAGS"] = f"{flags} log={self.mocklog}"
        self.logpath = os.path.join(tmp, f"daemon-{name}.log")
        self.logf = open(self.logpath, "w")
        self.proc = subprocess.Popen([args.zeropfin, "--config", self.conf, "serve"], stdout=self.logf,
                                     stderr=subprocess.STDOUT, env=env)
        self.client = Client(self.port)
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                st, _ = self.client.req("GET", "/healthz")
                if st == 200:
                    break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError(f"daemon {name} did not start; see {self.logpath}")

    def ctl(self, *cmd):
        r = subprocess.run([self.args.zeropfin, "--config", self.conf, *cmd], capture_output=True, text=True,
                           timeout=10)
        return r.returncode, r.stdout + r.stderr

    def pair(self):
        rc, out = self.ctl("pair")
        m = re.search(r"([A-Z2-9]{4}-[A-Z2-9]{4})", out)
        assert rc == 0 and m, out
        st, body = self.client.req("POST", "/api/v1/pairing/complete", {"code": m.group(1), "deviceName": "pytest"})
        assert st == 200, (st, body)
        self.client.csrf = body["csrfToken"]

    def wait_player(self, pred, timeout=10):
        deadline = time.time() + timeout
        st = None
        while time.time() < deadline:
            _, st = self.client.req("GET", "/api/v1/player")
            if pred(st):
                return st
            time.sleep(0.1)
        print(f"  (timeout waiting; last player state: {json.dumps(st)})")
        return None

    def rss_kib(self):
        with open(f"/proc/{self.proc.pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
        return -1

    def stop(self):
        self.proc.terminate()
        try:
            rc = self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            rc = -9
        self.logf.close()
        return rc

    def log_text(self):
        with open(self.logpath, errors="replace") as f:
            return f.read()


def jf_log(jf_port, reset=False):
    c = http.client.HTTPConnection("127.0.0.1", jf_port, timeout=10)
    c.request("GET", "/__test/log")
    data = json.loads(c.getresponse().read())
    c.close()
    if reset:
        jf_post(jf_port, "/__test/reset", {})
    return data


def jf_post(jf_port, path, body):
    c = http.client.HTTPConnection("127.0.0.1", jf_port, timeout=10)
    c.request("POST", path, body=json.dumps(body), headers={"Content-Type": "application/json"})
    c.getresponse().read()
    c.close()


def ids(lib, name):
    for it in lib["items"]:
        if it["Name"] == name:
            return it["Id"]
    raise KeyError(name)


def relay_url(d):
    """The loopback URL ZeroPlay was told to play (from the mock's command log)."""
    with open(d.mocklog) as f:
        urls = re.findall(r"^load(?:at \d+)? (http://127\.0\.0\.1:\d+/media/[0-9a-f]{32})$", f.read(), re.M)
    return urls[-1] if urls else None


def http_raw(url, method="GET", headers=None, read_limit=None):
    m = re.match(r"http://127\.0\.0\.1:(\d+)(/.*)", url)
    c = http.client.HTTPConnection("127.0.0.1", int(m.group(1)), timeout=60)
    c.request(method, m.group(2), headers=headers or {})
    r = c.getresponse()
    body = r.read(read_limit) if read_limit else r.read()
    hdrs = dict(r.getheaders())
    status = r.status
    c.close()
    return status, hdrs, body


# ---------------------------------------------------------------------------------------------

def test_static_and_security(d, args):
    print("- static assets and request filters")
    have_web = os.path.exists(os.path.join(args.web_root, "index.html"))
    if have_web:
        r, body = d.client.req("GET", "/", raw=True)
        check(r.status == 200 and r.getheader("Content-Type").startswith("text/html"), "index served as HTML")
        csp = r.getheader("Content-Security-Policy") or ""
        check("script-src 'self'" in csp and "unsafe-eval" not in csp, "strict CSP on HTML")
        check(r.getheader("Cache-Control") == "no-cache", "index.html not cached long-term")
        assets = re.findall(r'(?:src|href)="(/assets/[^"]+)"', body.decode())
        check(len(assets) >= 2, "index references hashed /assets files")
        for a in assets:
            ra, _ = d.client.req("GET", a, raw=True)
            check(ra.status == 200 and "immutable" in (ra.getheader("Cache-Control") or ""), f"asset {a} cacheable")
            ct = ra.getheader("Content-Type") or ""
            if a.endswith(".js"):
                check(ct.startswith("text/javascript"), f"{a} MIME {ct}")
            if a.endswith(".css"):
                check(ct.startswith("text/css"), f"{a} MIME {ct}")
        rg, _ = d.client.req("GET", assets[0], raw=True, headers={"Accept-Encoding": "gzip"})
        check(rg.getheader("Content-Encoding") in ("gzip", None), "gzip negotiation sane")
    else:
        print("  (web/dist not built: static checks skipped)")
    for p in ["/../etc/passwd", "/assets/../../etc/passwd", "/.env", "/%2e%2e/etc/passwd", "/assets//x"]:
        st, _ = d.client.req("GET", p)
        check(st == 404, f"traversal {p} -> 404 (got {st})")
    st, body = d.client.req("GET", "/api/v1/views", host="evil.example.com")
    check(st == 421, "foreign Host header rejected (DNS rebinding)")
    st, body = d.client.req("GET", "/api/v1/views")
    check(st == 401 and body["error"]["code"] == "unauthenticated", "API requires pairing")
    st, body = d.client.req("GET", "/healthz")
    check(st == 200 and body == {"ok": True}, "healthz minimal")


def test_pairing(d):
    print("- pairing, CSRF and origin checks")
    st, body = d.client.req("POST", "/api/v1/pairing/complete", {"code": "AAAA-AAAA"})
    check(st == 401 and body["error"]["code"] == "invalid_code", "wrong code rejected")
    st, body = d.client.req("POST", "/api/v1/pairing/request", {}, origin="http://evil.example.com")
    check(st == 403, "cross-origin pairing request refused")
    st, body = d.client.req("POST", "/api/v1/pairing/request", {})
    check(st == 200 and body["expiresInSec"] == 600, "pairing request accepted")
    st, body = d.client.req("POST", "/api/v1/pairing/request", {})
    check(st == 429, "pairing request rate-limited")
    # The OSD command is written asynchronously by the player thread (and deferred
    # until ZeroPlay is ready), so poll the mock's command log.
    shown = None
    deadline = time.time() + 10
    while time.time() < deadline and not shown:
        with open(d.mocklog) as f:
            shown = re.search(r"^osd \d+ Zeropfin pairing code: [A-Z2-9]{4}-[A-Z2-9]{4}$", f.read(), re.M)
        if not shown:
            time.sleep(0.1)
    check(shown, "pairing code shown on the TV via ZeroPlay osd")
    d.pair()
    st, body = d.client.req("GET", "/api/v1/session")
    check(st == 200 and body["authenticated"] and len(body["csrfToken"]) == 32, "session established")
    csrf = d.client.csrf
    d.client.csrf = None
    st, body = d.client.req("POST", "/api/v1/player/stop", {})
    check(st == 403 and body["error"]["code"] == "csrf", "POST without CSRF token refused")
    d.client.csrf = "0" * 32
    st, _ = d.client.req("POST", "/api/v1/player/stop", {})
    check(st == 403, "wrong CSRF token refused")
    d.client.csrf = csrf
    st, _ = d.client.req("POST", "/api/v1/player/stop", {}, origin="http://evil.example.com")
    check(st == 403, "cross-origin POST refused even with token")
    st, _ = d.client.req("POST", "/api/v1/player/stop", None, headers={"Content-Type": "text/plain"})
    check(st == 415, "non-JSON POST refused")
    st, _ = d.client.req("POST", "/api/v1/player/seek", {"positionMs": "x" * 20000})
    check(st == 413, "oversized body refused")
    rc, out = d.ctl("sessions")
    check(rc == 0 and "pytest" in out, "sessions listed over control socket")


def test_browse(d, lib):
    print("- browsing")
    st, body = d.client.req("GET", "/api/v1/views")
    names = [v["name"] for v in body["items"]]
    check(st == 200 and names == ["Movies", "Shows"], f"video views only ({names})")
    movies = body["items"][0]["id"]
    st, p1 = d.client.req("GET", f"/api/v1/items?parentId={movies}&types=Movie&limit=20&start=0")
    st2, p2 = d.client.req("GET", f"/api/v1/items?parentId={movies}&types=Movie&limit=20&start=20")
    check(st == 200 and len(p1["items"]) == 20 and p1["total"] == 41, "page 1 of movies")
    check(st2 == 200 and len(p2["items"]) == 20 and p2["start"] == 20, "page 2 of movies")
    check(not set(i["id"] for i in p1["items"]) & set(i["id"] for i in p2["items"]), "pages disjoint")
    check(all("MediaSources" not in i and "Path" not in i for i in p1["items"]), "list items trimmed")
    st, body = d.client.req("GET", "/api/v1/items?limit=61")
    check(st == 400, "limit above maximum rejected")
    st, body = d.client.req("GET", "/api/v1/items?types=Movie;DROP")
    check(st == 400, "unknown item type rejected")
    st, body = d.client.req("GET", "/api/v1/items?search=harbor")
    check(st == 200 and [i["name"] for i in body["items"]] == ["Harbor Lights"], "search")
    st, body = d.client.req("GET", "/api/v1/resume")
    check(st == 200 and body["items"][0]["name"] == "Northern Line" and body["items"][0]["resumeMs"] == 600000,
          "continue watching with resume position")
    st, body = d.client.req("GET", "/api/v1/nextup")
    check(st == 200 and body["items"][0]["type"] == "Episode", "next up")
    st, body = d.client.req("GET", f"/api/v1/latest?parentId={movies}")
    check(st == 200 and len(body["items"]) == 16, "latest additions")
    series = ids(lib, "Lantern Bay")
    st, body = d.client.req("GET", f"/api/v1/items/{series}/seasons")
    check(st == 200 and [s["name"] for s in body["items"]] == ["Season 1", "Season 2"], "seasons")
    s1 = body["items"][0]["id"]
    st, body = d.client.req("GET", f"/api/v1/items/{series}/episodes?seasonId={s1}")
    check(st == 200 and len(body["items"]) == 3 and body["items"][1]["overview"], "episodes with overview")
    st, body = d.client.req("GET", f"/api/v1/items/{ids(lib, 'Harbor Lights')}")
    check(st == 200 and body["playback"]["playable"] and body["genres"] == ["Drama"], "details: playable movie")
    check(any("subtitles" in w.lower() for w in body["playback"]["warnings"]), "details: subtitle warning")
    expectations = {"Quiet Engine": "HEVC", "Paper Moons": "10-bit", "Copper Field": "DTS", "Fast Water": "60 fps",
                    "Heavy Rain": "Mbps", "Glass Garden": "AVI"}
    for name, needle in expectations.items():
        st, body = d.client.req("GET", f"/api/v1/items/{ids(lib, name)}")
        check(st == 200 and not body["playback"]["playable"] and any(needle in r for r in body["playback"]["reasons"]),
              f"details: {name} rejected mentioning {needle}")
    st, body = d.client.req("GET", f"/api/v1/items/{ids(lib, 'Two Sources')}")
    check(body["playback"]["playable"] and body["playback"]["media"]["width"] == 1280, "second media source chosen")
    st, body = d.client.req("GET", "/api/v1/items/not-an-id")
    check(st == 404, "malformed id rejected")
    st, body = d.client.req("GET", "/api/v1/items/" + "0" * 32)
    check(st == 404 and body["error"]["code"] == "not_found", "unknown item -> 404")
    r, img = d.client.req("GET", f"/api/v1/images/{ids(lib, 'Harbor Lights')}/Primary?tag=p0001&w=300", raw=True)
    check(r.status == 200 and r.getheader("Content-Type") == "image/png" and img.startswith(b"\x89PNG"), "image relay")
    check("immutable" in (r.getheader("Cache-Control") or ""), "tagged images cacheable")
    st, _ = d.client.req("GET", f"/api/v1/images/{ids(lib, 'Harbor Lights')}/Logo?tag=x")
    check(st == 404, "unknown image type refused")
    st, _ = d.client.req("GET", f"/api/v1/images/{ids(lib, 'Harbor Lights')}/Primary?w=99999")
    check(st == 400, "image width bounded")


def test_playback(d, lib, jf_port):
    print("- playback control and Jellyfin reports")
    jf_log(jf_port, reset=True)
    st, body = d.client.req("POST", "/api/v1/player/pause", {})
    check(st == 409, "pause when idle -> 409")
    st, body = d.client.req("POST", "/api/v1/player/play", {"itemId": ids(lib, "Quiet Engine")})
    check(st == 422 and body["error"]["code"] == "incompatible" and "HEVC" in body["error"]["message"],
          "HEVC refused at play time")
    st, body = d.client.req("POST", "/api/v1/player/play", {"itemId": ids(lib, "Northern Line"), "resume": True})
    check(st == 202 and body["startMs"] == 600000, "play with resume accepted")
    s = d.wait_player(lambda p: p["state"] == "playing")
    check(s is not None, "state -> playing")
    s = d.wait_player(lambda p: p["positionSource"] == "verified", timeout=5)
    check(s is not None and s["positionMs"] >= 600000, "verified position from status events, resumed at 10:00")
    check(s and s["item"]["title"] == "Northern Line" and s["item"]["subtitle"] == "2019", "now playing metadata")
    st, body = d.client.req("POST", "/api/v1/player/pause", {})
    check(st == 202, "pause accepted")
    s = d.wait_player(lambda p: p["state"] == "paused")
    check(s is not None, "state -> paused")
    st, body = d.client.req("POST", "/api/v1/player/seek", {"positionMs": 1200000})
    check(st == 202, "seek accepted")
    s = d.wait_player(lambda p: abs(p["positionMs"] - 1200000) < 2000 and p["state"] == "paused")
    check(s is not None, "seek reflected while paused")
    st, body = d.client.req("POST", "/api/v1/player/seek", {"deltaMs": -30000})
    s = d.wait_player(lambda p: abs(p["positionMs"] - 1170000) < 2000)
    check(st == 202 and s is not None, "relative seek")
    st, body = d.client.req("POST", "/api/v1/player/seek", {"positionMs": 1, "deltaMs": 1})
    check(st == 400, "seek with both fields rejected")
    st, body = d.client.req("POST", "/api/v1/player/seek", {"positionMs": 1.5})
    check(st == 400, "non-integer seek rejected")
    st, _ = d.client.req("POST", "/api/v1/player/resume", {})
    s = d.wait_player(lambda p: p["state"] == "playing")
    check(st == 202 and s is not None, "resume -> playing")

    url = relay_url(d)
    check(url is not None and d.token not in url, "player got a loopback relay URL without the token")
    st, _ = d.client.req("POST", "/api/v1/player/stop", {})
    s = d.wait_player(lambda p: p["state"] == "stopped")
    check(st == 202 and s is not None, "stop -> stopped")
    status, _, _ = http_raw(url, headers={"Range": "bytes=0-9"})
    check(status == 404, "relay grant revoked after stop")

    time.sleep(1.0)
    log = jf_log(jf_port)
    paths = [e["path"] for e in log]
    check("/Sessions/Playing" in paths, "playback start reported")
    prog = [e for e in log if e["path"] == "/Sessions/Playing/Progress"]
    check(any(e["body"].get("EventName") == "Pause" and e["body"]["IsPaused"] for e in prog), "pause reported")
    stops = [e for e in log if e["path"] == "/Sessions/Playing/Stopped"]
    check(len(stops) == 1 and stops[0]["body"]["Failed"] is False, "stop reported once, not failed")
    check(stops and 11000000000 < stops[0]["body"]["PositionTicks"] < 13000000000, "stop position in ticks")
    pinfo = [e for e in log if e["path"].endswith("/PlaybackInfo")]
    check(pinfo and pinfo[0]["body"]["EnableTranscoding"] is False and pinfo[0]["body"]["DeviceProfile"]
          ["DirectPlayProfiles"][0]["VideoCodec"] == "h264", "PlaybackInfo requests direct play only")
    streams = [e for e in log if e["path"].endswith("/stream")]
    check(streams and all(e["range"] for e in streams), "media fetched through relay with Range")
    check(all(e["auth"].startswith("MediaBrowser ") and 'Token="' in e["auth"] for e in log
              if not e["path"].startswith("/__test")), "every Jellyfin call uses the MediaBrowser header")
    check(not any("api_key" in e["query"].lower() for e in log), "no api_key query parameters")

    print("- natural end of file")
    jf_log(jf_port, reset=True)
    st, _ = d.client.req("POST", "/api/v1/player/play", {"itemId": ids(lib, "Short Clip"), "resume": False})
    s = d.wait_player(lambda p: p["state"] == "ended", timeout=15)
    check(st == 202 and s is not None and s["positionSource"] == "verified", "short clip ends -> ended")
    time.sleep(0.8)
    stops = [e for e in jf_log(jf_port) if e["path"] == "/Sessions/Playing/Stopped"]
    check(len(stops) == 1 and stops[0]["body"]["PositionTicks"] == 4 * 10_000_000 and not stops[0]["body"]["Failed"],
          "EOF reported with full runtime (marks played)")

    print("- replacement while playing")
    jf_log(jf_port, reset=True)
    d.client.req("POST", "/api/v1/player/play", {"itemId": ids(lib, "Harbor Lights")})
    d.wait_player(lambda p: p["state"] == "playing")
    d.client.req("POST", "/api/v1/player/play", {"itemId": ids(lib, "Filler Film 12")})
    s = d.wait_player(lambda p: p["state"] == "playing" and p["item"]["title"] == "Filler Film 12")
    check(s is not None, "second item playing")
    time.sleep(0.8)
    log = jf_log(jf_port)
    starts = [e["body"]["ItemId"] for e in log if e["path"] == "/Sessions/Playing"]
    stops = [e["body"]["ItemId"] for e in log if e["path"] == "/Sessions/Playing/Stopped"]
    check(starts == [ids(lib, "Harbor Lights"), ids(lib, "Filler Film 12")], "two starts in order")
    check(stops == [ids(lib, "Harbor Lights")], "first item stopped exactly once")


def test_relay(d, lib, jf_port, args):
    print("- loopback media relay")
    d.client.req("POST", "/api/v1/player/play", {"itemId": ids(lib, "Harbor Lights"), "resume": False})
    d.wait_player(lambda p: p["state"] == "playing")
    d.client.req("POST", "/api/v1/player/pause", {})
    url = relay_url(d)
    fake = load_fake_jellyfin()
    size = 5400 * 1000 * 10  # Harbor Lights: 90 min at the fixture's 10 bytes/ms
    st, h, body = http_raw(url, headers={"Range": "bytes=100-299"})
    check(st == 206 and h.get("Content-Range") == f"bytes 100-299/{size}" and body == fake.pattern(100, 200),
          "Range 206 with exact bytes")
    st, h, body = http_raw(url, headers={"Range": "bytes=-16"})
    check(st == 206 and body == fake.pattern(size - 16, 16), "suffix range")
    st, h, _ = http_raw(url, headers={"Range": f"bytes={size}-"})
    check(st == 416 and h.get("Content-Range") == f"bytes */{size}", "416 passthrough")
    st, h, body = http_raw(url, method="HEAD")
    check(st == 200 and h.get("Content-Length") == str(size) and h.get("Accept-Ranges") == "bytes" and body == b"",
          "HEAD")
    st, h, body = http_raw(url, headers={"Range": "bytes=0-1,5-6"})
    check(st == 200 and len(body) == size, "multi-range ignored -> full 200")
    st, _, _ = http_raw(url.replace(url[-6:], "000000"))
    check(st == 404, "unknown relay id -> 404")
    st, _, _ = http_raw(url, method="DELETE")
    check(st == 405, "relay only allows GET/HEAD")

    rss_before = d.rss_kib()
    peak = [rss_before]
    stop = threading.Event()

    def sample():
        while not stop.is_set():
            peak[0] = max(peak[0], d.rss_kib())
            time.sleep(0.05)

    t = threading.Thread(target=sample)
    t.start()
    t0 = time.time()
    st, h, body = http_raw(url)
    elapsed = time.time() - t0
    stop.set()
    t.join()
    check(st == 200 and len(body) == size and body[:64] == fake.pattern(0, 64) and
          body[-64:] == fake.pattern(size - 64, 64), f"full {size >> 20} MiB GET intact")
    growth = peak[0] - rss_before
    print(f"  relay throughput on this host: {size / elapsed / 1e6:.1f} MB/s; daemon RSS growth {growth} KiB "
          f"(before {rss_before} KiB, peak {peak[0]} KiB)")
    with open(args.zeropfin, "rb") as f:
        sanitized = b"__asan_init" in f.read()
    if sanitized:
        print("  (ASan build: RSS growth is not meaningful, bound checked on the release build)")
    else:
        check(growth < 8 * 1024, f"relay memory stays bounded while streaming {size >> 20} MiB")

    jf_post(jf_port, "/__test/config", {"drop_after": 5 * 1024 * 1024})
    st, h, body = http_raw(url, headers={"Range": "bytes=1000-20999999"})
    check(st == 206 and len(body) == 20999000 and body[-32:] == fake.pattern(20999999 - 31, 32) and
          body[5 * 1024 * 1024 - 8:5 * 1024 * 1024 + 8] == fake.pattern(1000 + 5 * 1024 * 1024 - 8, 16),
          "upstream drop resumed transparently with correct bytes")
    rc, out = d.ctl("status")
    check(rc == 0 and json.loads(out)["relay"]["resumes"] >= 1, "resume counted in status")

    # aborted client: read a little then hang up; relay must release the reader
    m = re.match(r"http://127\.0\.0\.1:(\d+)(/.*)", url)
    s = socket.create_connection(("127.0.0.1", int(m.group(1))))
    s.sendall(f"GET {m.group(2)} HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n".encode())
    s.recv(65536)
    s.close()
    deadline = time.time() + 10
    readers = None
    while time.time() < deadline:
        readers = json.loads(d.ctl("status")[1])["relay"]["activeReaders"]
        if readers == 0:
            break
        time.sleep(0.2)
    check(readers == 0, "aborted client released")

    jf_post(jf_port, "/__test/config", {"redirect": "same"})
    st, _, body = http_raw(url, headers={"Range": "bytes=0-15"})
    check(st == 206 and body == fake.pattern(0, 16), "same-origin redirect followed")
    jf_post(jf_port, "/__test/config", {"redirect": "cross"})
    st, _, _ = http_raw(url, headers={"Range": "bytes=0-15"})
    check(st == 502, "cross-origin redirect refused")
    jf_post(jf_port, "/__test/reset", {})
    d.client.req("POST", "/api/v1/player/stop", {})


def test_crash(args, jf_port, tmp, lib):
    print("- player crash is never completion")
    d = Daemon(args, jf_port, tmp, "duration-ms=600000 crash-after-ms=1500", "crash")
    try:
        d.pair()
        d.wait_player(lambda p: p["player"]["status"] == "ready")
        jf_log(jf_port, reset=True)
        d.client.req("POST", "/api/v1/player/play", {"itemId": ids(lib, "Filler Film 20"), "resume": False})
        s = d.wait_player(lambda p: p["state"] == "error", timeout=10)
        check(s is not None and "unexpectedly" in s["error"], "crash -> error state with message")
        s = d.wait_player(lambda p: p["player"]["status"] == "ready" and p["player"]["restarts"] >= 1, timeout=10)
        check(s is not None, "player restarted")
        time.sleep(0.8)
        stops = [e for e in jf_log(jf_port) if e["path"] == "/Sessions/Playing/Stopped"]
        check(len(stops) == 1 and stops[0]["body"]["Failed"] is True, "crash reported as failed stop")
        check(stops and stops[0]["body"].get("PositionTicks", 0) < 600 * 10_000_000, "no completion position")
    finally:
        d.stop()
    return d


def test_stock(args, jf_port, tmp, lib):
    print("- stock ZeroPlay (no status extensions)")
    d = Daemon(args, jf_port, tmp, "stock duration-ms=600000", "stock")
    try:
        d.pair()
        s = d.wait_player(lambda p: p["player"]["status"] == "ready", timeout=8)
        check(s is not None and s["player"]["statusEvents"] is False, "stock player detected")
        d.client.req("POST", "/api/v1/player/play", {"itemId": ids(lib, "Northern Line"), "resume": True})
        s = d.wait_player(lambda p: p["state"] == "playing", timeout=8)
        check(s is not None and s["positionSource"] == "estimated", "position labelled estimated")
        with open(d.mocklog) as f:
            check(re.search(r"^seek 600000$", f.read(), re.M), "resume via load+seek on stock player")
    finally:
        d.stop()
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zeropfin", required=True)
    ap.add_argument("--mock-zeroplay", required=True)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--web-root", required=True)
    args = ap.parse_args()
    lib = json.load(open(os.path.join(args.fixtures, "library.json")))
    fake = load_fake_jellyfin()
    srv = fake.serve(os.path.join(args.fixtures, "library.json"), 0)
    jf_port = srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    tmp = tempfile.mkdtemp(prefix="zeropfin-it-")
    daemons = []
    try:
        d = Daemon(args, jf_port, tmp, "fetch bytes-per-ms=10", "main")
        daemons.append(d)
        test_static_and_security(d, args)
        test_pairing(d)
        d.wait_player(lambda p: p["player"]["status"] == "ready")
        test_browse(d, lib)
        test_playback(d, lib, jf_port)
        test_relay(d, lib, jf_port, args)
        st, _ = d.client.req("POST", "/api/v1/session/logout", {})
        check(st == 200, "logout")
        st, _ = d.client.req("GET", "/api/v1/views")
        check(st == 401, "session invalid after logout")
        rc = d.stop()
        check(rc == 0, f"clean shutdown on SIGTERM (exit {rc})")
        daemons.append(test_crash(args, jf_port, tmp, lib))
        daemons.append(test_stock(args, jf_port, tmp, lib))
        print("- secret hygiene")
        for dd in daemons:
            text = dd.log_text()
            check(dd.token not in text, f"access token absent from {dd.name} daemon log")
            check(not re.search(r"/media/[0-9a-f]{32}", text), f"relay ids scrubbed from {dd.name} log")
            for fn in os.listdir(dd.state):
                with open(os.path.join(dd.state, fn)) as f:
                    content = f.read()
                if fn == "sessions.json":
                    check(dd.client.cookie is None or dd.client.cookie.split("=", 1)[1] not in content,
                          "session cookie stored hashed")
    finally:
        for dd in daemons:
            if dd.proc.poll() is None:
                dd.stop()
        srv.shutdown()
        if not failed:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"logs kept in {tmp}")
    print(f"\n{passed} checks passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
