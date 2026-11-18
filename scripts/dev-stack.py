#!/usr/bin/env python3
"""Run a complete Zeropfin stack on a developer machine — no Pi, no Jellyfin.

Starts the fake Jellyfin (tests/mock_jellyfin), the native daemon with the mock
ZeroPlay player, and prints one JSON line with the URLs and a fresh pairing code.
Ctrl-C stops everything.

    cmake -S . -B build && cmake --build build
    (cd web && pnpm build)
    python3 scripts/dev-stack.py            # then open the printed URL
    (cd web && ZEROPFIN_DEV_DAEMON=http://127.0.0.1:8080 pnpm dev)   # optional hot reload
"""
import argparse
import importlib.util
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--listen", default="127.0.0.1")
    ap.add_argument("--web-root", default=os.path.join(ROOT, "web", "dist"))
    ap.add_argument("--mock-flags", default="fetch bytes-per-ms=10")
    a = ap.parse_args()

    spec = importlib.util.spec_from_file_location("fakejf", os.path.join(ROOT, "tests", "mock_jellyfin", "server.py"))
    fake = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fake)
    lib_path = os.path.join(ROOT, "tests", "fixtures", "library.json")
    srv = fake.serve(lib_path, 0)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    lib = json.load(open(lib_path))

    tmp = tempfile.mkdtemp(prefix="zeropfin-dev-")
    state, run = os.path.join(tmp, "state"), os.path.join(tmp, "run")
    os.makedirs(state, mode=0o700)
    os.makedirs(run, mode=0o700)
    cpath = os.path.join(state, "credentials.json")
    with open(cpath, "w") as f:
        json.dump({"serverUrl": f"http://127.0.0.1:{srv.server_port}", "serverId": lib["server"]["Id"],
                   "serverName": "Fixture Jellyfin", "serverVersion": lib["server"]["Version"],
                   "userId": lib["user"]["Id"], "userName": "demo", "accessToken": lib["token"],
                   "deviceId": "zeropfin-dev-device"}, f)
    os.chmod(cpath, 0o600)
    conf = os.path.join(tmp, "zeropfin.conf")
    with open(conf, "w") as f:
        f.write(f"listen_address = {a.listen}\nlisten_port = {a.port}\nweb_root = {os.path.abspath(a.web_root)}\n"
                f"state_dir = {state}\nruntime_dir = {run}\n"
                f"zeroplay_path = {os.path.join(a.build, 'tests', 'mock_zeroplay')}\nprogress_interval_s = 10\n")
    env = dict(os.environ, MOCK_ZEROPLAY_FLAGS=a.mock_flags)
    zeropfin = os.path.join(a.build, "zeropfin")
    proc = subprocess.Popen([zeropfin, "--config", conf, "serve"], env=env)

    def shutdown(*_):
        proc.terminate()
        proc.wait(timeout=10)
        srv.shutdown()
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)
    for _ in range(100):
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{a.port}/healthz", timeout=1).read()
            break
        except OSError:
            time.sleep(0.1)
    out = subprocess.run([zeropfin, "--config", conf, "pair"], capture_output=True, text=True).stdout
    code = re.search(r"[A-Z2-9]{4}-[A-Z2-9]{4}", out).group(0)
    print(json.dumps({"url": f"http://127.0.0.1:{a.port}/", "pairingCode": code, "config": conf,
                      "jellyfin": f"http://127.0.0.1:{srv.server_port}"}), flush=True)
    proc.wait()
    srv.shutdown()


if __name__ == "__main__":
    main()
