#!/usr/bin/env python3
"""Serve the mock radio's web console with a simulated radio behind it.

    console_sim.py            serve on http://localhost:8080 until stopped
    console_sim.py --check    start, check the interface over HTTP, and stop

No ESP32 is involved. The page is components/web_console/www/index.html, read
from disk on each request, so an edit shows on reload. The settings and the
telemetry come from host/build/console_sim, which is the firmware's own
console code and audio engine (build it with "make -C host"). This script is
only the HTTP and WebSocket layer, standing in for the firmware's
web_console.c with the same paths, so it does not test that file.

The simulated host keys the radio by CAT and plays a tone for 6 s out of
every 15, like an FT8 station, unless --quiet is given. /sim/play and
/sim/key (with ?on=1 or ?on=0) do the same by hand.

Needs Python 3 only.
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PAGE = os.path.join(ROOT, "components", "web_console", "www", "index.html")
BINARY = os.path.join(ROOT, "host", "build", "console_sim")
WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PUSH_S = 0.2   # telemetry interval, as in the firmware
KEY_ON = "fe fe a4 e0 1c 00 01 fd"
KEY_OFF = "fe fe a4 e0 1c 00 00 fd"


class Sim:
    """The simulator process: one command line in, one reply line out."""

    def __init__(self, path):
        self.proc = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.lock = threading.Lock()

    def ask(self, command):
        with self.lock:
            self.proc.stdin.write(command.replace("\n", " ").replace("\r", " ") + "\n")
            self.proc.stdin.flush()
            line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("the simulator stopped")
        status, _, body = line.rstrip("\n").partition(" ")
        return int(status), body

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=2)
        except Exception:
            self.proc.kill()


def clock(sim, stop, demo):
    """Run the radio in real time, and the pretend host with it."""
    start = time.monotonic()
    done_ms = 0
    keyed = False
    while not stop.is_set():
        now_ms = int((time.monotonic() - start) * 1000)
        if demo:
            want = 2000 <= now_ms % 15000 < 8000
            if want != keyed:
                keyed = want
                sim.ask("civ " + (KEY_ON if keyed else KEY_OFF))
                sim.ask("play " + ("on" if keyed else "off"))
        if now_ms > done_ms:
            sim.ask("tick %d" % min(now_ms - done_ms, 1000))
            done_ms = now_ms
        stop.wait(0.02)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    sim = None

    def log_message(self, fmt, *args):
        pass

    def reply(self, status, body, ctype="application/json"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def error(self, status, message):
        self.reply(status, json.dumps({"error": message}))

    def do_GET(self):
        path, _, query = self.path.partition("?")
        if path == "/":
            with open(PAGE, "rb") as f:
                self.reply(200, f.read(), "text/html; charset=utf-8")
        elif path in ("/api/info", "/api/settings", "/api/telemetry"):
            self.reply(*self.sim.ask(path[5:]))
        elif path == "/api/wifi":
            self.reply(200, json.dumps({"name": "mockradio-sim", "mode": "simulator, no Wi-Fi", "ssid": "", "ip": "",
                                        "join": ""}))
        elif path in ("/sim/play", "/sim/key"):
            on = query == "on=1"
            if path == "/sim/key":
                self.sim.ask("civ " + (KEY_ON if on else KEY_OFF))
            else:
                self.sim.ask("play " + ("on" if on else "off"))
            self.reply(200, "{}")
        elif path == "/ws":
            self.websocket()
        else:
            self.error(404, "no such page")

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        if self.path == "/api/settings":
            # As in the firmware: a cross-site form cannot send this type.
            if not (self.headers.get("Content-Type") or "").startswith("application/json"):
                self.error(415, "send the settings as application/json")
                return
            self.reply(*self.sim.ask("apply " + body))
        elif self.path in ("/api/reset", "/api/replay"):
            self.reply(*self.sim.ask(self.path[5:]))
        elif self.path == "/api/wifi":
            self.error(400, "the simulator has no Wi-Fi to set up")
        else:
            self.error(404, "no such page")

    def websocket(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if (self.headers.get("Upgrade") or "").lower() != "websocket" or not key:
            self.error(400, "this path is a WebSocket")
            return
        accept = base64.b64encode(hashlib.sha1((key + WS_MAGIC).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.close_connection = True
        self.connection.settimeout(PUSH_S)
        try:
            while True:
                data = self.sim.ask("telemetry")[1].encode()
                head = bytes([0x81, len(data)]) if len(data) < 126 else b"\x81\x7e" + struct.pack(">H", len(data))
                self.connection.sendall(head + data)
                try:
                    # Anything the browser sends is a close or a pong; a close ends it.
                    got = self.connection.recv(64)
                    if not got or got[0] & 0x0F == 0x8:
                        return
                    time.sleep(PUSH_S)
                except socket.timeout:
                    pass
        except OSError:
            pass


def check(port):
    """Exercise the interface the page uses. Returns the number of failures."""
    base = "http://127.0.0.1:%d" % port
    failures = 0

    def call(method, path, body=None, ctype="application/json"):
        req = urllib.request.Request(base + path, method=method, data=None if body is None else body.encode())
        if body is not None:
            req.add_header("Content-Type", ctype)
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, r.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def expect(what, ok):
        nonlocal failures
        print(("ok    " if ok else "FAIL  ") + what)
        failures += 0 if ok else 1

    status, page = call("GET", "/")
    expect("the page is served", status == 200 and "/api/info" in page and "<canvas" in page)

    status, body = call("GET", "/api/info")
    info = json.loads(body)
    names = [s["name"] for s in info["settings"]]
    expect("info lists the settings", status == 200 and info["rig"] and "snr" in names and "route" in names)

    status, body = call("GET", "/api/settings")
    settings = json.loads(body)
    expect("every listed setting has a value", status == 200 and sorted(settings) == sorted(names))

    for s in info["settings"]:
        # Each setting round-trips at both ends of its range, or for each choice.
        values = s.get("choices") or [s["min"], s["max"]]
        good = True
        for v in values:
            status, body = call("POST", "/api/settings", json.dumps({s["name"]: v}))
            good = good and status == 200 and json.loads(body)[s["name"]] == v
        expect("set %s to %s" % (s["name"], " and ".join(str(v) for v in values)), good)
        if "max" in s:
            status, body = call("POST", "/api/settings", json.dumps({s["name"]: s["max"] + s["step"]}))
            expect("refuse %s above its range" % s["name"], status == 400 and s["name"] in json.loads(body)["error"])

    status, body = call("POST", "/api/reset")
    expect("reset puts the settings back", status == 200 and json.loads(body) == settings)
    status, body = call("POST", "/api/settings", '{"snr":5}', "text/plain")
    expect("refuse settings not sent as JSON", status == 415)
    status, body = call("POST", "/api/replay")
    expect("refuse a replay with nothing recorded", status == 409 and "error" in json.loads(body))

    # A transmission in the parrot route comes back.
    call("POST", "/api/settings", '{"route":"parrot","delay":0.5}')
    call("GET", "/sim/key?on=1")
    call("GET", "/sim/play?on=1")
    time.sleep(1.0)
    status, body = call("GET", "/api/telemetry")
    t = json.loads(body)
    expect("telemetry shows the transmission", status == 200 and t["ptt"]["on"] and t["ptt"]["cat"]
           and t["parrot"]["state"] == "recording" and t["audio"]["tx"] and t["audio"]["tx"]["rms"] > -20)
    call("GET", "/sim/key?on=0")
    call("GET", "/sim/play?on=0")
    time.sleep(1.0)
    t = json.loads(call("GET", "/api/telemetry")[1])
    expect("the parrot replays it", not t["ptt"]["on"] and t["parrot"]["state"] == "playing"
           and t["audio"]["tx"] is None and t["audio"]["rx"]["rms"] > -20)
    time.sleep(1.2)
    status, body = call("POST", "/api/replay")
    expect("replay on request", status == 200)

    # The WebSocket pushes the same telemetry.
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(("GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    data = b""
    while b"\r\n\r\n" not in data:
        data += sock.recv(4096)
    head, _, data = data.partition(b"\r\n\r\n")
    while len(data) < 4 or len(data) < 4 + struct.unpack(">H", data[2:4])[0]:
        data += sock.recv(4096)
    pushed = json.loads(data[4:4 + struct.unpack(">H", data[2:4])[0]])
    sock.close()
    expect("the WebSocket pushes telemetry", b"101" in head and b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in head
           and data[0] == 0x81 and "ptt" in pushed and "parrot" in pushed)
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--quiet", action="store_true", help="no pretend transmissions")
    ap.add_argument("--check", action="store_true", help="check the interface and stop")
    args = ap.parse_args()

    if not os.path.exists(BINARY):
        sys.exit("build the simulator first: make -C host")
    sim = Sim(BINARY)
    Handler.sim = sim
    server = ThreadingHTTPServer(("127.0.0.1", 0 if args.check else args.port), Handler)
    server.daemon_threads = True
    stop = threading.Event()
    threading.Thread(target=clock, args=(sim, stop, not (args.quiet or args.check)), daemon=True).start()
    threading.Thread(target=server.serve_forever, daemon=True).start()

    status = 0
    try:
        if args.check:
            failures = check(server.server_address[1])
            print("console check: %s" % ("PASS" if not failures else "%d FAILED" % failures))
            status = 1 if failures else 0
        else:
            print("web console with a simulated radio: http://localhost:%d  (Ctrl-C stops it)" % args.port)
            while True:
                time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.shutdown()
        sim.close()
    sys.exit(status)


if __name__ == "__main__":
    main()
