#!/usr/bin/env python3
"""Read and change the mock radio's own settings over its CAT serial port.

The settings are CI-V frames sent to address F0, which the mock keeps for
itself (see components/mock_ctl/include/mock_ctl.h). No radio is involved.

    mockctl.py -p PORT                        show every setting and the status
    mockctl.py -p PORT noise=on snr=-12.5     change settings, then show them
    mockctl.py -p PORT reset                  back to the power-up settings

Settings:
    route=monitor|radio   monitor: loopback, PTT ignored; radio: mute while keyed
    noise=on|off          add noise and hold the signal at the SNR
    snr=DB                -60 to 80, in steps of 0.1
    bw=HZ                 bandwidth the SNR is quoted in, 100 to 24000
    floor=DBFS            noise floor, -90 to -12
    atten=DB              extra attenuation, 0 to 120
    turnaround=MS         receive mute after unkeying, 0 to 5000 (radio route)

PORT can also come from the MOCK_PORT environment variable. Only one program
should use the port at a time: a reply read by another program is lost here
and confuses it there.

Needs Python 3 only. Uses pyserial if it is installed (required on Windows).
"""
import argparse
import os
import sys
import time

ADDR = 0xF0        # the mock's own address
CONTROLLER = 0xE0
OK, NG = 0xFB, 0xFA

CMD_ID, CMD_RESET, CMD_STATUS = 0x00, 0x01, 0x20
FLAG_RX_MUTED, FLAG_GAIN_LIMITED = 0x01, 0x02

# name: (command, BCD bytes, signed, wire units per unit)
NUMBERS = {
    "snr": (0x12, 2, True, 10),
    "bw": (0x13, 3, False, 1),
    "floor": (0x14, 2, False, -10),   # sent as dB below full scale
    "atten": (0x15, 2, False, 10),
    "turnaround": (0x16, 2, False, 1),
}
SWITCHES = {
    "route": (0x10, ("monitor", "radio")),
    "noise": (0x11, ("off", "on")),
}


class Refused(Exception):
    pass


def to_bcd(value, nbytes):
    digits = "%0*d" % (nbytes * 2, value)
    if len(digits) != nbytes * 2:
        raise ValueError("%d does not fit in %d BCD bytes" % (value, nbytes))
    return bytes.fromhex(digits)


def from_bcd(data):
    return int(data.hex())


def encode_number(name, text):
    cmd, nbytes, signed, scale = NUMBERS[name]
    wire = round(float(text) * scale)
    if wire < 0 and not signed:
        raise ValueError("%s=%s has the wrong sign" % (name, text))
    data = to_bcd(abs(wire), nbytes)
    if signed:
        data += b"\x01" if wire < 0 else b"\x00"
    return cmd, data


def decode_number(name, data):
    _, nbytes, signed, scale = NUMBERS[name]
    if len(data) != nbytes + (1 if signed else 0):
        raise ValueError("unexpected reply length for %s" % name)
    wire = from_bcd(data[:nbytes])
    if signed and data[nbytes]:
        wire = -wire
    return wire / scale


def frame(cmd, data=b""):
    return bytes([0xFE, 0xFE, ADDR, CONTROLLER, cmd]) + data + b"\xFD"


class Port:
    """A raw serial port: pyserial when there is one, termios otherwise."""

    def __init__(self, path):
        self.fd = None
        self.ser = None
        try:
            import serial
        except ImportError:
            serial = None
        if serial is not None:
            self.ser = serial.Serial(path, 115200, timeout=0.05)
            return
        import termios
        import tty
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        attrs[2] |= termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def write(self, data):
        if self.ser is not None:
            self.ser.write(data)
            return
        while data:
            try:
                data = data[os.write(self.fd, data):]
            except BlockingIOError:
                time.sleep(0.005)

    def read(self):
        """Whatever has arrived, waiting up to 50 ms for the first byte."""
        if self.ser is not None:
            return self.ser.read(256)
        import select
        ready, _, _ = select.select([self.fd], [], [], 0.05)
        if not ready:
            return b""
        try:
            return os.read(self.fd, 256)
        except BlockingIOError:
            return b""

    def close(self):
        if self.ser is not None:
            self.ser.close()
        else:
            os.close(self.fd)


def frames_in(buf):
    """Split complete CI-V frames off the front of buf: (frames, the rest)."""
    out = []
    while True:
        start = buf.find(b"\xFE\xFE")
        if start < 0:
            return out, buf[-1:]
        end = buf.find(b"\xFD", start)
        if end < 0:
            return out, buf[start:]
        out.append(buf[start + 2:end].lstrip(b"\xFE"))
        buf = buf[end + 1:]


def ask(port, cmd, data=b"", timeout=1.0):
    """Send one command. Returns the reply's data bytes, b"" for a plain OK."""
    port.write(frame(cmd, data))
    buf = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        buf += port.read()
        found, buf = frames_in(buf)
        for body in found:
            # body: <to> <from> <cmd or FB/FA> <data...>; skip anyone else's traffic
            if len(body) < 3 or body[0] != CONTROLLER or body[1] != ADDR:
                continue
            if body[2] == OK:
                return b""
            if body[2] == NG:
                raise Refused()
            if body[2] == cmd:
                return body[3:]
    raise TimeoutError("no reply: is this the mock radio's CAT port?")


def show(port):
    ident = ask(port, CMD_ID)
    if len(ident) != 3:
        raise ValueError("unexpected identity reply")
    print("mock radio, control protocol %d, personality: Hamlib model %d" % (ident[0], from_bcd(ident[1:])))

    values = {}
    for name, (cmd, words) in SWITCHES.items():
        values[name] = words[ask(port, cmd)[0] & 1]
    for name, (cmd, _, _, _) in NUMBERS.items():
        values[name] = decode_number(name, ask(port, cmd))
    print("  route       %s" % values["route"])
    print("  noise       %s" % values["noise"])
    print("  snr         %+.1f dB in %d Hz" % (values["snr"], values["bw"]))
    print("  floor       %.1f dBFS" % values["floor"])
    print("  atten       %.1f dB" % values["atten"])
    print("  turnaround  %d ms" % values["turnaround"])

    st = ask(port, CMD_STATUS)
    if len(st) != 6:
        raise ValueError("unexpected status reply")
    level = from_bcd(st[1:3])
    gain = from_bcd(st[3:5]) / 10 * (-1 if st[5] else 1)
    notes = []
    if st[0] & FLAG_RX_MUTED:
        notes.append("receive muted")
    if st[0] & FLAG_GAIN_LIMITED:
        notes.append("gain limited: the source is too quiet to reach this SNR")
    source = "nothing measured yet" if level == 9999 else "%.1f dBFS" % (-level / 10)
    print("  status      source %s, gain %+.1f dB%s" % (source, gain, "".join("; " + n for n in notes)))


def apply(port, word):
    if word == "reset":
        ask(port, CMD_RESET)
        return
    name, sep, value = word.partition("=")
    if not sep:
        raise ValueError("%r is not name=value, 'show' or 'reset'" % word)
    if name in SWITCHES:
        cmd, words = SWITCHES[name]
        if value not in words:
            raise ValueError("%s is %s" % (name, " or ".join(words)))
        ask(port, cmd, bytes([words.index(value)]))
    elif name in NUMBERS:
        cmd, data = encode_number(name, value)
        ask(port, cmd, data)
    else:
        raise ValueError("no setting called %r" % name)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--port", default=os.environ.get("MOCK_PORT"), help="the mock radio's CAT serial port")
    parser.add_argument("words", nargs="*", metavar="name=value | show | reset")
    args = parser.parse_args()
    if not args.port:
        parser.error("no port: give -p PORT or set MOCK_PORT")

    try:
        port = Port(args.port)
    except Exception as e:  # pyserial and os raise different types
        sys.exit("cannot open %s: %s" % (args.port, e))
    try:
        for word in args.words:
            if word == "show":
                continue
            try:
                apply(port, word)
            except Refused:
                sys.exit("%s: refused by the mock (out of range?)" % word)
            except ValueError as e:
                sys.exit(str(e))
        show(port)
    except Refused:
        sys.exit("the device refused a read: is its firmware older than this tool?")
    except (TimeoutError, ValueError) as e:
        sys.exit(str(e))
    finally:
        port.close()


if __name__ == "__main__":
    main()
