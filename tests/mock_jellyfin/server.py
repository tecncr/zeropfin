#!/usr/bin/env python3
"""Fake Jellyfin server for Zeropfin tests and local development.

Python standard library only. Serves the synthetic library in
tests/fixtures/library.json with the subset of the Jellyfin 10.10+/12.x REST
API that Zeropfin uses, enforces the `Authorization: MediaBrowser ... Token=`
header, records every request for assertions, and serves deterministic,
position-encoded media bytes with real HTTP Range semantics.

Test hooks (never part of Jellyfin):
  GET  /__test/log           -> recorded requests (JSON)
  POST /__test/reset         -> clear the log and test config
  POST /__test/config {...}  -> {"drop_after": N}  close a stream after N body bytes (once)
                                {"redirect": "same"|"cross"} redirect stream requests
                                {"stream_delay_ms": N} delay before stream headers

Usage: server.py --library tests/fixtures/library.json --port 8096
"""
import argparse
import json
import secrets
import struct
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


def pattern(offset, length):
    """Bytes [offset, offset+length) of the virtual media file: each aligned
    8-byte word holds its own big-endian offset, so any byte reveals where it
    came from (catches off-by-N range bugs)."""
    start = offset - offset % 8
    end = offset + length
    words = range(start, end + 8, 8)
    blob = b"".join(struct.pack(">Q", w) for w in words)
    skip = offset - start
    return blob[skip:skip + length]


def tiny_png(w=4, h=6, gray=96):
    raw = b"".join(b"\x00" + bytes([gray]) * w for _ in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class State:
    def __init__(self, library_path):
        with open(library_path) as f:
            self.lib = json.load(f)
        self.items = {i["Id"]: i for i in self.lib["items"]}
        self.sources = {}
        for it in self.lib["items"]:
            for ms in it.get("MediaSources", []):
                self.sources[ms["Id"]] = ms
        self.log = []
        self.config = {}
        self.lock = threading.Lock()

    def record(self, entry):
        with self.lock:
            self.log.append(entry)


STATE = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "FakeJellyfin/1.0"

    def log_message(self, fmt, *args):  # quiet
        pass

    # ---- helpers ---------------------------------------------------------
    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def send_empty(self, status):
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def authed(self):
        auth = self.headers.get("Authorization", "")
        return auth.startswith("MediaBrowser ") and f'Token="{STATE.lib["token"]}"' in auth

    def body_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            return json.loads(raw) if raw else None
        except ValueError:
            return "<invalid json>"

    def strip(self, item, fields):
        out = {k: v for k, v in item.items() if k not in ("MediaSources", "Overview", "Path")}
        if "Overview" in fields and "Overview" in item:
            out["Overview"] = item["Overview"]
        return out

    def page(self, items, q, fields):
        start = int(q.get("startIndex", ["0"])[0])
        limit = int(q.get("limit", ["100"])[0])
        sel = items[start:start + limit]
        return {"Items": [self.strip(i, fields) for i in sel], "TotalRecordCount": len(items), "StartIndex": start}

    # ---- dispatch ------------------------------------------------------------
    def handle_any(self):
        u = urlsplit(self.path)
        q = parse_qs(u.query)
        path = u.path
        body = self.body_json() if self.command == "POST" else None
        STATE.record({"method": self.command, "path": path, "query": u.query,
                      "auth": self.headers.get("Authorization", ""),
                      "range": self.headers.get("Range"), "body": body,
                      "headers": {k: v for k, v in self.headers.items() if k.lower().startswith("x-")}})

        if path.startswith("/__test/"):
            return self.test_hook(path, body)
        if path == "/System/Info/Public":
            return self.send_json(STATE.lib["server"])
        if path == "/Users/AuthenticateByName" and self.command == "POST":
            user = STATE.lib["user"]
            if isinstance(body, dict) and body.get("Username") == user["Name"] and body.get("Pw") == user["Password"]:
                return self.send_json({"User": {"Id": user["Id"], "Name": user["Name"]},
                                       "AccessToken": STATE.lib["token"], "ServerId": STATE.lib["server"]["Id"],
                                       "SessionInfo": {}})
            return self.send_empty(401)
        if not self.authed():
            return self.send_empty(401)
        if "api_key" in q or "ApiKey" in q:
            return self.send_empty(400)  # Zeropfin must never use query-string tokens

        items = STATE.lib["items"]
        fields = ",".join(q.get("fields", [""])).split(",")
        if path == "/UserViews":
            return self.send_json({"Items": STATE.lib["views"], "TotalRecordCount": len(STATE.lib["views"]),
                                   "StartIndex": 0})
        if path == "/Items" and self.command == "GET":
            sel = list(items)
            if "parentId" in q:
                sel = [i for i in sel if i.get("ParentId") == q["parentId"][0]]
            if "includeItemTypes" in q:
                types = q["includeItemTypes"][0].split(",")
                sel = [i for i in sel if i["Type"] in types]
            if "searchTerm" in q:
                term = q["searchTerm"][0].lower()
                sel = [i for i in sel if term in i["Name"].lower()]
            if q.get("filters", [""])[0] == "IsUnplayed":
                sel = [i for i in sel if not i.get("UserData", {}).get("Played")]
            sort = q.get("sortBy", ["SortName"])[0].split(",")[0]
            key = {"SortName": lambda i: i["Name"].lower(),
                   "DateCreated": lambda i: i.get("DateCreated", ""),
                   "PremiereDate": lambda i: i.get("ProductionYear") or 0,
                   "CommunityRating": lambda i: i.get("CommunityRating") or 0}.get(sort, lambda i: i["Name"])
            sel.sort(key=key, reverse=q.get("sortOrder", ["Ascending"])[0] == "Descending")
            return self.send_json(self.page(sel, q, fields))
        if path == "/UserItems/Resume":
            sel = [i for i in items if i.get("UserData", {}).get("PlaybackPositionTicks", 0) > 0]
            return self.send_json(self.page(sel, q, fields))
        if path == "/Shows/NextUp":
            sel = [i for i in items if i["Type"] == "Episode" and not i["UserData"].get("Played")][:1]
            return self.send_json(self.page(sel, q, fields))
        if path == "/Items/Latest":
            sel = [i for i in items if not q.get("parentId") or i.get("ParentId") == q["parentId"][0]]
            sel = [i for i in sel if i["Type"] in ("Movie", "Series")]
            sel.sort(key=lambda i: i.get("DateCreated", ""), reverse=True)
            limit = int(q.get("limit", ["16"])[0])
            return self.send_json([self.strip(i, fields) for i in sel[:limit]])
        parts = path.strip("/").split("/")
        if len(parts) == 2 and parts[0] == "Items" and self.command == "GET":
            it = STATE.items.get(parts[1])
            return self.send_json(it) if it else self.send_empty(404)
        if len(parts) == 3 and parts[0] == "Shows" and parts[2] == "Seasons":
            sel = [i for i in items if i["Type"] == "Season" and i.get("SeriesId") == parts[1]]
            return self.send_json(self.page(sel, q, fields))
        if len(parts) == 3 and parts[0] == "Shows" and parts[2] == "Episodes":
            sel = [i for i in items if i["Type"] == "Episode" and i.get("SeriesId") == parts[1]]
            if "seasonId" in q:
                sel = [i for i in sel if i.get("SeasonId") == q["seasonId"][0]]
            return self.send_json(self.page(sel, q, fields))
        if len(parts) == 4 and parts[0] == "Items" and parts[2] == "Images":
            if parts[1] not in STATE.items and not any(v["Id"] == parts[1] for v in STATE.lib["views"]):
                return self.send_empty(404)
            png = tiny_png(gray=64 + int(parts[1][-2:], 16) % 128)
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(png)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(png)
            return
        if len(parts) == 3 and parts[0] == "Items" and parts[2] == "PlaybackInfo" and self.command == "POST":
            it = STATE.items.get(parts[1])
            if not it:
                return self.send_empty(404)
            return self.send_json({"MediaSources": it.get("MediaSources", []),
                                   "PlaySessionId": secrets.token_hex(16)})
        if len(parts) == 3 and parts[0] == "Videos" and parts[2] == "stream":
            return self.stream(parts[1], q)
        if path in ("/Sessions/Playing", "/Sessions/Playing/Progress", "/Sessions/Playing/Stopped"):
            return self.send_empty(204)
        return self.send_empty(404)

    def test_hook(self, path, body):
        if path == "/__test/log":
            with STATE.lock:
                return self.send_json(STATE.log)
        if path == "/__test/reset":
            with STATE.lock:
                STATE.log.clear()
                STATE.config.clear()
            return self.send_empty(204)
        if path == "/__test/config":
            with STATE.lock:
                STATE.config.update(body or {})
            return self.send_empty(204)
        return self.send_empty(404)

    def stream(self, item_id, q):
        ms_id = q.get("mediaSourceId", [""])[0]
        ms = STATE.sources.get(ms_id)
        if not ms or q.get("static", [""])[0].lower() != "true":
            return self.send_empty(404)
        cfg = STATE.config
        if cfg.get("stream_delay_ms"):
            time.sleep(cfg["stream_delay_ms"] / 1000)
        redirect = cfg.get("redirect")
        if redirect and "hop" not in q:
            host = "127.0.0.1" if redirect == "same" else "localhost"
            self.send_response(302)
            self.send_header("Location", f"http://{host}:{self.server.server_port}{self.path}&hop=1")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        size = ms["Size"]
        rng = self.headers.get("Range")
        start, end, status = 0, size - 1, 200
        if rng:
            spec = rng.split("=", 1)[1]
            a, _, b = spec.partition("-")
            if a == "":
                start, end = max(0, size - int(b)), size - 1
            else:
                start = int(a)
                end = int(b) if b else size - 1
            if start >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            end = min(end, size - 1)
            status = 206
        length = end - start + 1
        self.send_response(status)
        self.send_header("Content-Type", "video/x-matroska" if ms["Container"] == "mkv" else "video/mp4")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if self.command == "HEAD":
            return
        drop = None
        with STATE.lock:
            if STATE.config.get("drop_after"):
                drop = STATE.config.pop("drop_after")
        sent = 0
        pos = start
        try:
            while sent < length:
                n = min(65536, length - sent)
                if drop is not None and sent + n > drop:
                    n = drop - sent
                    if n > 0:
                        self.wfile.write(pattern(pos, n))
                    self.wfile.flush()
                    self.close_connection = True
                    self.connection.shutdown(2)
                    return
                self.wfile.write(pattern(pos, n))
                sent += n
                pos += n
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    do_GET = handle_any
    do_POST = handle_any
    do_HEAD = handle_any


def serve(library, port, host="127.0.0.1"):
    global STATE
    STATE = State(library)
    srv = ThreadingHTTPServer((host, port), Handler)
    srv.daemon_threads = True
    return srv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--library", required=True)
    ap.add_argument("--port", type=int, default=8096)
    ap.add_argument("--host", default="127.0.0.1")
    a = ap.parse_args()
    srv = serve(a.library, a.port, a.host)
    print(f"fake Jellyfin on http://{a.host}:{srv.server_port} (user demo / demo-password)", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
