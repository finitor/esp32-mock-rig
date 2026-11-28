#!/usr/bin/env python3
"""Serve the mock radio's web console with two simulated radios behind it.

    console_sim.py            board A on http://localhost:8080, board B on 8081
    console_sim.py --link     the same, with both boards in the link route
    console_sim.py --check    start, check the interface over HTTP, and stop

No ESP32 is involved. The two boards are joined by the two-board link, over a
path that delivers at once and, with --loss N, loses N percent of packets. The page is components/web_console/www/index.html, read
from disk on each request, so an edit shows on reload. The settings and the
telemetry come from host/build/console_sim, which is the firmware's own
console code and audio engine (build it with "make -C host"). This script is
only the HTTP and WebSocket layer, standing in for the firmware's
web_console.c with the same paths, so it does not test that file.

Each simulated host keys its radio by CAT and plays a tone, board A for 6 s
out of every 15 and board B for 4 s in the gap, unless --quiet is given.
/sim/play and /sim/key (with ?on=1 or ?on=0) do the same by hand, on the
board whose port they are sent to.

Each board also has a CAT serial port, a pseudo-terminal, so that Hamlib or
any other program can drive the radio the page shows:

    console_sim.py --quiet            and then, in another terminal:
    rigctl -m 3085 -r host/build/cat-a

Board B's is host/build/cat-b; --cat gives them another name. What arrives
there goes to the same parser as on a board's USB serial port.

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
import tty
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PAGE = os.path.join(ROOT, "components", "web_console", "www", "index.html")
BINARY = os.path.join(ROOT, "host", "build", "console_sim")
WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PUSH_S = 0.2   # telemetry interval, as in the firmware
SCOPE_S = 0.1  # waterfall rows, as in the firmware
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


class CatPort:
    """A board's CAT serial port: a pseudo-terminal, with a link to it at path."""

    def __init__(self, sim, board, path):
        self.sim, self.board, self.path = sim, board, path
        self.master, self.slave = os.openpty()
        # Raw: CI-V is binary. The slave stays open here, so that the master
        # never sees the end of the file when a program closes the port.
        tty.setraw(self.slave)
        if os.path.islink(path):
            os.unlink(path)
        os.symlink(os.ttyname(self.slave), path)
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                data = os.read(self.master, 64)
            except OSError:
                return
            if not data:
                continue
            reply = json.loads(self.sim.ask("%s civ %s" % (self.board, data.hex()))[1])
            if reply:
                try:
                    os.write(self.master, bytes.fromhex(reply))
                except OSError:
                    return

    def close(self):
        if os.path.islink(self.path):
            os.unlink(self.path)


def clock(sim, stop, demo):
    """Run the radios in real time, and the pretend hosts with them."""
    start = time.monotonic()
    done_ms = 0
    keyed = {"a": False, "b": False}
    slots = {"a": (2000, 8000), "b": (9500, 13500)}
    while not stop.is_set():
        now_ms = int((time.monotonic() - start) * 1000)
        for board in ("a", "b") if demo else ():
            want = slots[board][0] <= now_ms % 15000 < slots[board][1]
            if want != keyed[board]:
                keyed[board] = want
                sim.ask("%s civ %s" % (board, KEY_ON if want else KEY_OFF))
                sim.ask("%s play %s" % (board, "on" if want else "off"))
        step = min(now_ms - done_ms, 1000) // 5 * 5  # the simulator runs in 5 ms blocks
        if step:
            sim.ask("tick %d" % step)
            done_ms += step
        stop.wait(0.02)


class Scope:
    """A board's waterfall rows. One row serves every page showing it, as on a board."""

    def __init__(self, sim, board):
        self.sim, self.board = sim, board
        self.lock = threading.Lock()
        self.at, self.row = 0.0, b""

    def latest(self):
        with self.lock:
            now = time.monotonic()
            if now - self.at >= SCOPE_S * 0.9:
                self.at = now
                self.row = bytes.fromhex(json.loads(self.sim.ask(self.board + " scope")[1]))
            return self.row


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    sim = None
    scope = None
    board = "a"  # each board's server has its own subclass

    def ask(self, command):
        return self.sim.ask(self.board + " " + command)

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
        elif path in ("/api/info", "/api/settings", "/api/telemetry", "/api/health", "/api/radio"):
            self.reply(*self.ask(path[5:]))
        elif path == "/api/wifi":
            self.reply(200, json.dumps({"name": "mockradio-sim" + self.board, "mode": "simulator, no Wi-Fi",
                                        "ssid": "", "ip": "", "join": ""}))
        elif path in ("/sim/play", "/sim/key"):
            on = query == "on=1"
            if path == "/sim/key":
                self.ask("civ " + (KEY_ON if on else KEY_OFF))
            else:
                self.ask("play " + ("on" if on else "off"))
            self.reply(200, "{}")
        elif path == "/ws":
            self.websocket(PUSH_S, 0x81, lambda: self.ask("telemetry")[1].encode())
        elif path == "/ws/scope":
            self.websocket(SCOPE_S, 0x82, self.scope.latest)
        else:
            self.error(404, "no such page")

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        with_body = {"/api/settings": "apply ", "/api/rig": "rig ", "/api/clip": "clip ", "/api/tasks": "tasks",
                     "/api/radio": "panel "}
        if self.path in with_body:
            # As in the firmware: a cross-site form cannot send this type.
            if not (self.headers.get("Content-Type") or "").startswith("application/json"):
                self.error(415, "send the request as application/json")
                return
            command = with_body[self.path]
            self.reply(*self.ask(command + body if command.endswith(" ") else command))
        elif self.path in ("/api/reset", "/api/replay", "/api/keep", "/api/forget"):
            self.reply(*self.ask(self.path[5:]))
        elif self.path == "/api/wifi":
            self.error(400, "the simulator has no Wi-Fi to set up")
        else:
            self.error(404, "no such page")

    def websocket(self, period, opcode, message):
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
        self.connection.settimeout(period)
        try:
            while True:
                data = message()
                head = bytes([opcode, len(data)]) if len(data) < 126 else bytes([opcode, 0x7e]) + struct.pack(">H", len(data))
                self.connection.sendall(head + data)
                try:
                    # Anything the browser sends is a close or a pong; a close ends it.
                    got = self.connection.recv(64)
                    if not got or got[0] & 0x0F == 0x8:
                        return
                    time.sleep(period)
                except socket.timeout:
                    pass
        except OSError:
            pass


def check(port, port_b, sim, cat):
    """Exercise the interface the page uses. Returns the number of failures."""
    base = "http://127.0.0.1:%d" % port
    failures = 0

    def call(method, path, body=None, ctype="application/json", board_base=None):
        req = urllib.request.Request((board_base or base) + path, method=method,
                                     data=None if body is None else body.encode())
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

    def soon(what, test, within=3.0):
        """For what happens in the radio's own time: look until it is so."""
        end = time.monotonic() + within
        ok = test()
        while not ok and time.monotonic() < end:
            time.sleep(0.1)
            ok = test()
        expect(what, ok)

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

    def replaying():
        t = json.loads(call("GET", "/api/telemetry")[1])
        return (not t["ptt"]["on"] and t["parrot"]["state"] == "playing" and t["audio"]["tx"] is None
                and t["audio"]["rx"]["rms"] > -20)

    soon("the parrot replays it", replaying)
    soon("replay on request", lambda: call("POST", "/api/replay")[0] == 200)

    # The WebSocket pushes the same telemetry.
    def ws_frames(path, count):
        """The handshake's reply, and the first frames pushed: (opcode byte, payload) each."""
        sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        sock.sendall(("GET %s HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" % path).encode())
        data = b""
        while b"\r\n\r\n" not in data:
            data += sock.recv(4096)
        head, _, data = data.partition(b"\r\n\r\n")
        frames = []
        while len(frames) < count:
            while len(data) < 4:
                data += sock.recv(4096)
            start, size = (4, struct.unpack(">H", data[2:4])[0]) if data[1] == 0x7e else (2, data[1])
            while len(data) < start + size:
                data += sock.recv(4096)
            frames.append((data[0], data[start:start + size]))
            data = data[start + size:]
        sock.close()
        return head, frames

    head, frames = ws_frames("/ws", 1)
    pushed = json.loads(frames[0][1])
    expect("the WebSocket pushes telemetry", b"101" in head and b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in head
           and frames[0][0] == 0x81 and "ptt" in pushed and "parrot" in pushed)

    # The waterfall: rows of spectrum on a WebSocket of their own, as bytes.
    scope = json.loads(call("GET", "/api/info")[1])["scope"]
    bins = scope["bins"]
    expect("info gives the layout of the waterfall", scope == {"fs": 12000, "n": 512, "bins": 128, "rows": 10})
    call("POST", "/api/settings", '{"route":"monitor"}')
    soon("the parrot's replay has ended", lambda: json.loads(call("GET", "/api/telemetry")[1])["audio"]["tx"] is None
         and json.loads(call("GET", "/api/telemetry")[1])["parrot"]["state"] != "playing")
    time.sleep(0.3)
    head, frames = ws_frames("/ws/scope", 3)
    row = frames[-1][1]
    expect("a row with the host recording silence and playing nothing", b"101" in head and frames[-1][0] == 0x82
           and len(row) == 4 + bins and row[0] == 1 and row[1] == 0x01 and row[2] == bins and row[4 + 64] == 240)
    call("GET", "/sim/play?on=1")
    call("GET", "/sim/key?on=1")
    time.sleep(0.5)
    row = ws_frames("/ws/scope", 3)[1][-1][1]
    # The simulated host plays 1500 Hz, bin 64, at -12 dBFS peak: -15 dBFS RMS, 30 half decibels.
    expect("the tone the host plays is in both lanes, at its frequency and level", len(row) == 4 + 2 * bins
           and row[1] == 0x07 and row[4 + 64] == 30 and row[4 + bins + 64] == 30
           and row[4 + 30] > 150 and row[4 + bins + 100] > 150)
    call("GET", "/sim/play?on=0")
    call("GET", "/sim/key?on=0")
    time.sleep(0.4)
    row = ws_frames("/ws/scope", 2)[1][-1][1]
    expect("the transmit lane goes when the host stops playing", len(row) == 4 + bins and row[1] == 0x01)

    # Settings kept for power-up, and the recording kept as a clip and played back.
    call("POST", "/api/settings", '{"vox":"on","atten":6}')
    status, body = call("POST", "/api/keep")
    expect("keep the settings for power-up", status == 200 and json.loads(call("GET", "/api/telemetry")[1])["kept"])
    call("POST", "/api/settings", '{"vox":"off","atten":0}')
    reset = json.loads(call("POST", "/api/reset")[1])
    expect("reset goes back to the kept ones", reset["vox"] == "on" and reset["atten"] == 6.0)
    status, body = call("POST", "/api/forget")
    reset = json.loads(call("POST", "/api/reset")[1])
    expect("forget them again", status == 200 and reset["vox"] == "off" and reset["atten"] == 0.0
           and not json.loads(call("GET", "/api/telemetry")[1])["kept"])

    call("POST", "/api/settings", '{"route":"parrot","delay":60}')
    soon("the parrot is idle again", lambda: json.loads(call("GET", "/api/telemetry")[1])["parrot"]["state"] == "idle")
    recorded = json.loads(call("GET", "/api/telemetry")[1])["parrot"]["recorded"]
    status, body = call("POST", "/api/clip", '{"op":"save","clip":1}')
    t = json.loads(call("GET", "/api/telemetry")[1])
    expect("keep the recording as clip 1", status == 200 and recorded > 0.5
           and t["clips"]["seconds"] == [recorded, 0.0] and not t["clips"]["failed"])
    status, body = call("POST", "/api/clip", '{"op":"play","clip":2}')
    expect("refuse to play an empty clip", status == 409 and "empty" in json.loads(body)["error"])
    call("GET", "/sim/key?on=1")
    time.sleep(0.3)
    call("GET", "/sim/key?on=0")       # a short transmission replaces the recording
    soon("another recording replaces the first",
         lambda: 0 < json.loads(call("GET", "/api/telemetry")[1])["parrot"]["recorded"] < recorded)
    # Not while the new recording is still being finished: ask until it is taken.
    soon("clip 1 is played on request", lambda: call("POST", "/api/clip", '{"op":"play","clip":1}')[0] == 200)

    def clip_playing():
        t = json.loads(call("GET", "/api/telemetry")[1])
        return (t["parrot"]["state"] == "playing" and t["parrot"]["recorded"] == recorded and t["audio"]["rx"]
                and t["audio"]["rx"]["rms"] > -20)

    soon("and brings the first recording back", clip_playing)
    status, body = call("POST", "/api/clip", '{"op":"erase","clip":1}')
    expect("erase clip 1", status == 200
           and json.loads(call("GET", "/api/telemetry")[1])["clips"]["seconds"] == [0.0, 0.0])
    call("POST", "/api/reset")

    # The front panel: the radio as CAT left it, and changes made at the radio.
    def radio(board_base=None):
        return json.loads(call("GET", "/api/radio", board_base=board_base)[1])

    def item(r, table, name):
        return [x for x in r[table] if x["name"] == name][0]

    def civ(frame):
        return json.loads(sim.ask("a civ " + frame)[1])

    def rgen():
        return json.loads(call("GET", "/api/telemetry")[1])["rgen"]

    r = radio()
    expect("the panel shows the radio", r["hamlib"] == 3085 and r["vfo"] == "A" and r["a"]["freq"] == 14074000
           and r["a"]["mode"] == "USB" and r["a"]["data"] and "CW" in r["modes"]
           and item(r, "levels", "AF gain")["id"] == "14.01" and item(r, "funcs", "AGC")["choices"][2] == "mid"
           and item(r, "meters", "S-meter")["max"] == 255 and r["gen"] == rgen())
    before = rgen()
    status, body = call("POST", "/api/radio", '{"freq":7074000,"mode":"LSB","data":false}')
    r = json.loads(body)
    expect("tune and change mode at the panel", status == 200 and r["a"]["freq"] == 7074000 and r["a"]["mode"] == "LSB"
           and not r["a"]["data"] and rgen() != before and rgen() == r["gen"])
    expect("CAT reads back what the panel set", civ("fe fe a4 e0 03 fd") == "fe fe e0 a4 03 00 40 07 07 00 fd"
           and civ("fe fe a4 e0 04 fd") == "fe fe e0 a4 04 00 01 fd")
    before = rgen()
    civ("fe fe a4 e0 14 01 02 00 fd")       # AF gain 200, over CAT
    civ("fe fe a4 e0 0f 01 fd")             # split on
    civ("fe fe a4 e0 07 01 fd")             # VFO B
    r = radio()
    expect("the panel shows what CAT set", item(r, "levels", "AF gain")["value"] == 200 and r["split"]
           and r["vfo"] == "B" and rgen() != before)
    before = rgen()
    civ("fe fe a4 e0 03 fd")
    civ("fe fe a4 e0 14 01 02 00 fd")
    expect("reading, or setting what is already so, does not ask the page to read again", rgen() == before)
    status, body = call("POST", "/api/radio", '{"vfo":"A","split":false,"16.12":3,"14.0A":128,"15.02":120}')
    r = json.loads(body)
    expect("switches, levels and meters at the panel", status == 200 and r["vfo"] == "A" and not r["split"]
           and item(r, "funcs", "AGC")["value"] == 3 and item(r, "levels", "RF power")["value"] == 128
           and item(r, "meters", "S-meter")["value"] == 120)
    expect("and CAT reads them", civ("fe fe a4 e0 16 12 fd") == "fe fe e0 a4 16 12 03 fd"
           and civ("fe fe a4 e0 14 0a fd") == "fe fe e0 a4 14 0a 01 28 fd"
           and civ("fe fe a4 e0 15 02 fd") == "fe fe e0 a4 15 02 01 20 fd")
    status, body = call("POST", "/api/radio", '{"mode":"CW","freq":250000000}')
    expect("refuse a frequency the radio does not tune to, and change nothing",
           status == 400 and "freq" in json.loads(body)["error"] and radio()["a"]["mode"] == "LSB")
    status, body = call("POST", "/api/radio", '{"16.12":4}')
    expect("refuse a value the panel does not offer", status == 400 and "AGC" in json.loads(body)["error"])
    status, body = call("POST", "/api/radio", '{"freq":7074000}', "text/plain")
    expect("refuse a panel change not sent as JSON", status == 415)

    status, body = call("POST", "/api/radio", '{"ptt":true}')
    t = json.loads(call("GET", "/api/telemetry")[1])
    expect("key the radio at the panel", status == 200 and t["ptt"]["on"] and t["ptt"]["panel"] and not t["ptt"]["cat"])
    expect("CAT sees it transmitting and cannot unkey it", civ("fe fe a4 e0 1c 00 fd") == "fe fe e0 a4 1c 00 01 fd"
           and civ("fe fe a4 e0 1c 00 00 fd") == "fe fe e0 a4 fb fd"
           and json.loads(call("GET", "/api/telemetry")[1])["ptt"]["on"])
    call("POST", "/api/radio", '{"ptt":false}')
    expect("released at the panel", not json.loads(call("GET", "/api/telemetry")[1])["ptt"]["on"]
           and civ("fe fe a4 e0 1c 00 fd") == "fe fe e0 a4 1c 00 00 fd")

    # The CAT serial port: a program that opens it reaches the same radio.
    fd = os.open(cat, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)
    os.write(fd, bytes.fromhex("fefea4e003fd"))
    got = b""
    end = time.monotonic() + 3
    while not got.endswith(b"\xfd") and time.monotonic() < end:
        got += os.read(fd, 64)
    os.close(fd)
    expect("the CAT serial port answers", got.hex() == "fefee0a4030040070700fd")

    # The radio imitated can be changed, and the page is told.
    status, body = call("POST", "/api/rig", '{"hamlib":3088}')
    info = json.loads(call("GET", "/api/info")[1])
    t = json.loads(call("GET", "/api/telemetry")[1])
    expect("become a G90", status == 200 and info["rig"] == "Xiegu G90" and info["hamlib"] == 3088
           and t["hamlib"] == 3088 and len(info["rigs"]) == 2)
    reply = json.loads(sim.ask("a civ fe fe 70 e0 19 00 fd")[1])
    expect("the G90 answers CAT", reply == "fe fe e0 70 19 00 70 fd")
    r = radio()
    expect("the panel is the G90's", r["hamlib"] == 3088 and r["a"]["freq"] == 14074000 and r["duplex"] is None
           and r["step"] is None and not r["can"]["equal"] and not r["att"]["set"] and "RTTY" not in r["modes"]
           and item(r, "funcs", "AGC")["choices"] == ["off", "fast", "middle", "slow"] and len(r["levels"]) == 6)
    status, body = call("POST", "/api/radio", '{"op":"equal"}')
    expect("refuse what the G90 cannot do", status == 409 and "G90" in json.loads(body)["error"])
    status, body = call("POST", "/api/rig", '{"hamlib":2028}')
    expect("refuse a radio the mock is not", status == 409 and "2028" in json.loads(body)["error"])
    status, body = call("POST", "/api/rig", '{"hamlib":3085}')
    expect("and an IC-705 again", status == 200 and json.loads(call("GET", "/api/info")[1])["hamlib"] == 3085)

    # The board's health. The simulator's numbers are made up; this checks the reading's layout.
    status, body = call("GET", "/api/health")
    h = json.loads(body)
    expect("health is reported, without the tasks until they are asked for",
           status == 200 and h["heap"]["free"] > 0 and len(h["cpu"]) == 2 and h["tasks"] is None)
    status, body = call("POST", "/api/tasks", "{}", "text/plain")
    expect("refuse a reading of the tasks not sent as JSON", status == 415)
    status, body = call("POST", "/api/tasks", "{}")
    h = json.loads(body)
    expect("the tasks are read on request", status == 200 and h["tasks"]["list"] and "stack" in h["tasks"]["list"][0])
    soon("and the reading ages", lambda: json.loads(call("GET", "/api/health")[1])["tasks"]["age"] > 0)

    # Two boards in the link route: what one transmits, the other hears.
    base_b = "http://127.0.0.1:%d" % port_b

    def both(method, path, body=None):
        return [call(method, path, body, board_base=b) for b in (base, base_b)]

    def telemetry_b():
        return json.loads(call("GET", "/api/telemetry", board_base=base_b)[1])

    status = [r[0] for r in both("POST", "/api/settings", '{"route":"link"}')]
    expect("both boards take the link route", status == [200, 200])
    soon("the boards find each other",
         lambda: telemetry_b()["link"]["state"] == "idle" and telemetry_b()["link"]["peer"] == "mockradio-sima")
    call("GET", "/sim/key?on=1")
    call("GET", "/sim/play?on=1")
    time.sleep(1.0)
    t = telemetry_b()
    expect("board B hears board A", t["link"]["state"] == "receiving" and t["link"]["peer_keyed"]
           and t["link"]["received"] > 50 and t["link"]["lost"] == 0 and t["audio"]["rx"]["rms"] > -20)
    t = json.loads(call("GET", "/api/telemetry")[1])
    expect("board A is sending and hears nothing", t["link"]["state"] == "sending" and t["link"]["sent"] > 50
           and t["channel"]["muted"])
    # Board B tunes away (CI-V 05, 7.074 MHz): on another frequency it hears nothing.
    sim.ask("b civ fe fe a4 e0 05 00 40 07 07 00 fd")

    def off_frequency():
        t = telemetry_b()
        return (t["link"]["state"] == "off frequency" and t["link"]["off_frequency"] > 20
                and t["audio"]["rx"]["rms"] < -60)

    soon("not heard on another frequency", off_frequency)
    status, body = call("POST", "/api/settings", '{"match":"off"}', board_base=base_b)
    soon("heard again with matching off", lambda: status == 200 and telemetry_b()["link"]["state"] == "receiving")
    soon("strict frequencies are the pair's: board A follows board B",
         lambda: json.loads(call("GET", "/api/settings")[1])["match"] == "off")
    sim.ask("loss 20")
    soon("lost packets are counted", lambda: telemetry_b()["link"]["lost"] > 5)
    sim.ask("loss 0")
    call("GET", "/sim/key?on=0")
    call("GET", "/sim/play?on=0")
    soon("idle again after the transmission", lambda: telemetry_b()["link"]["state"] == "idle")

    # The two boards share one channel: a setting of the path's made on one
    # reaches the other, and a setting of the board's own does not.
    soon("the boards share their channel", lambda: telemetry_b()["link"]["shared"]
         and json.loads(call("GET", "/api/telemetry")[1])["link"]["shared"])
    call("POST", "/api/settings", '{"fade":"mpp","snr":-3.5,"noise":"on","turnaround":250}')

    def followed():
        b = json.loads(call("GET", "/api/settings", board_base=base_b)[1])
        return b["fade"] == "mpp" and b["snr"] == -3.5 and b["noise"] == "on" and b["turnaround"] == 30

    soon("board B takes board A's path settings, and keeps its own turnaround", followed)
    call("POST", "/api/settings", '{"fade":"hilat-d"}', board_base=base_b)
    soon("and board A takes board B's", lambda: json.loads(call("GET", "/api/settings")[1])["fade"] == "hilat-d")
    t_a = json.loads(call("GET", "/api/telemetry")[1])
    expect("one of the two keeps time", t_a["link"]["leads"] != telemetry_b()["link"]["leads"])
    call("POST", "/api/settings", '{"share":"off"}', board_base=base_b)
    call("POST", "/api/settings", '{"fade":"off"}')
    time.sleep(1.0)
    expect("not shared, each keeps its own", json.loads(call("GET", "/api/settings", board_base=base_b)[1])["fade"] == "hilat-d")
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8080, help="board A's port; board B takes the next one")
    ap.add_argument("--link", action="store_true", help="start with both boards in the link route")
    ap.add_argument("--loss", type=int, default=0, metavar="N", help="percent of link packets the path loses")
    ap.add_argument("--quiet", action="store_true", help="no pretend transmissions")
    ap.add_argument("--check", action="store_true", help="check the interface and stop")
    ap.add_argument("--cat", default=os.path.join(ROOT, "host", "build", "cat"), metavar="PATH",
                    help="where the boards' CAT serial ports appear: PATH-a and PATH-b")
    args = ap.parse_args()

    if not os.path.exists(BINARY):
        sys.exit("build the simulator first: make -C host")
    sim = Sim(BINARY)
    Handler.sim = sim
    servers = []
    for i, board in enumerate("ab"):
        handler = type("Handler" + board.upper(), (Handler,), {"board": board, "scope": Scope(sim, board)})
        server = ThreadingHTTPServer(("127.0.0.1", 0 if args.check else args.port + i), handler)
        server.daemon_threads = True
        servers.append(server)
    if args.link:
        for board in "ab":
            sim.ask(board + ' apply {"route":"link","noise":"on","snr":10}')
    sim.ask("loss %d" % args.loss)
    cats = [CatPort(sim, board, "%s-%s" % (args.cat + ("-check" if args.check else ""), board)) for board in "ab"]
    stop = threading.Event()
    threading.Thread(target=clock, args=(sim, stop, not (args.quiet or args.check)), daemon=True).start()
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()

    status = 0
    try:
        if args.check:
            failures = check(servers[0].server_address[1], servers[1].server_address[1], sim, cats[0].path)
            print("console check: %s" % ("PASS" if not failures else "%d FAILED" % failures))
            status = 1 if failures else 0
        else:
            print("web console with two simulated radios: board A http://localhost:%d, board B http://localhost:%d"
                  "  (Ctrl-C stops it)" % (args.port, args.port + 1))
            print("their CAT serial ports: %s and %s, for example  rigctl -m 3085 -r %s"
                  % (os.path.relpath(cats[0].path), os.path.relpath(cats[1].path), os.path.relpath(cats[0].path)))
            while True:
                time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        for server in servers:
            server.shutdown()
        for port in cats:
            port.close()
        sim.close()
    sys.exit(status)


if __name__ == "__main__":
    main()
