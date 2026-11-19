#!/usr/bin/env python3
"""Check the mock radio's USB descriptors without a board.

Reads desc_device and desc_configuration straight out of the built firmware
(build/usb_mock_radio.elf), prints what a host would see, and checks the
constraints the design depends on:

  - the ESP32-S3's endpoint and FIFO limits, as TinyUSB's driver applies them
  - what usb-serial-for-android (used by Data2G) needs to find the CDC port
  - a 48 kHz-capable, 16-bit mono UAC2 function in each direction

    python3 host/tools/check_usb_descriptors.py [path/to/firmware.elf]

Pure standard library. This checks structure, not behaviour: only a real host
can say whether the device enumerates and streams.
"""
import struct
import sys

# ESP32-S3 limits, from TinyUSB's dwc2_esp32.h and dcd_dwc2.c.
EP_COUNT = 7           # endpoint numbers 0..6
EP_IN_COUNT = 5        # IN endpoints including EP0
FIFO_WORDS = 1024 // 4
EP0_SIZE = 64

DT_DEVICE, DT_CONFIG, DT_INTERFACE, DT_ENDPOINT = 1, 2, 4, 5
DT_IAD, DT_CS_INTERFACE, DT_CS_ENDPOINT = 0x0B, 0x24, 0x25
CLASS_AUDIO, CLASS_CDC, CLASS_CDC_DATA, CLASS_MISC = 0x01, 0x02, 0x0A, 0xEF
XFER = {0: "control", 1: "isochronous", 2: "bulk", 3: "interrupt"}


def read_symbols(path, names):
    """Return {name: bytes} for data symbols in a 32-bit little-endian ELF."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit(f"{path}: not a 32-bit little-endian ELF file")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for sec in sections:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[sec[6]]
        for off in range(sec[4], sec[4] + sec[5], 16):
            st_name, st_value, st_size, _info, _other, st_shndx = struct.unpack_from("<IIIBBH", data, off)
            end = data.index(b"\0", strtab[4] + st_name)
            name = data[strtab[4] + st_name:end].decode()
            if name in names and 0 < st_shndx < shnum:
                home = sections[st_shndx]
                start = home[4] + (st_value - home[3])
                out[name] = data[start:start + st_size]
    missing = [n for n in names if n not in out]
    if missing:
        sys.exit(f"{path}: symbol(s) not found: {', '.join(missing)}")
    return out


class Checker:
    def __init__(self):
        self.failures = 0

    def check(self, ok, what):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}")
        if not ok:
            self.failures += 1


def u16(b, i):
    return b[i] | (b[i + 1] << 8)


def walk(config):
    """Split a configuration descriptor into (type, bytes) items."""
    items, i = [], 0
    while i < len(config):
        length = config[i]
        if length < 2 or i + length > len(config):
            sys.exit(f"configuration descriptor is malformed at offset {i}")
        items.append((config[i + 1], config[i:i + length]))
        i += length
    return items


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build/usb_mock_radio.elf"
    syms = read_symbols(path, ["desc_device", "desc_configuration"])
    dev, cfg = syms["desc_device"], syms["desc_configuration"]
    c = Checker()

    # ---- device ----------------------------------------------------------
    print(f"Device: VID {u16(dev, 8):04x} PID {u16(dev, 10):04x}, "
          f"class {dev[4]:02x}/{dev[5]:02x}/{dev[6]:02x}, EP0 {dev[7]} bytes")
    c.check(len(dev) == 18 and dev[0] == 18 and dev[1] == DT_DEVICE, "device descriptor is 18 bytes")
    c.check((dev[4], dev[5], dev[6]) == (CLASS_MISC, 2, 1),
            "device class EF/02/01, so hosts use the interface association descriptors")
    c.check(dev[7] == EP0_SIZE, "EP0 is 64 bytes")
    c.check(u16(dev, 8) != 0x0C26, "does not use Icom's vendor ID")
    c.check(dev[17] == 1, "one configuration")

    # ---- configuration ---------------------------------------------------
    items = walk(cfg)
    c.check(items[0][0] == DT_CONFIG and u16(cfg, 2) == len(cfg),
            f"wTotalLength ({u16(cfg, 2)}) matches the descriptor's size ({len(cfg)})")

    interfaces = {}     # (number, alt) -> dict
    iads, endpoints = [], []
    current = None
    for dtype, d in items[1:]:
        if dtype == DT_IAD:
            iads.append(dict(first=d[2], count=d[3], cls=d[4], sub=d[5]))
        elif dtype == DT_INTERFACE:
            current = dict(num=d[2], alt=d[3], neps=d[4], cls=d[5], sub=d[6], eps=[], cs=[])
            interfaces[(d[2], d[3])] = current
        elif dtype == DT_ENDPOINT:
            ep = dict(addr=d[2], attr=d[3], size=u16(d, 4) & 0x7FF, interval=d[6], itf=current)
            current["eps"].append(ep)
            endpoints.append(ep)
        elif dtype == DT_CS_INTERFACE and current is not None:
            current["cs"].append(d)

    numbers = sorted({n for n, _ in interfaces})
    c.check(cfg[4] == len(numbers) and numbers == list(range(len(numbers))),
            f"bNumInterfaces ({cfg[4]}) matches the interfaces present {numbers}")
    for key, itf in interfaces.items():
        if len(itf["eps"]) != itf["neps"]:
            c.check(False, f"interface {key} declares {itf['neps']} endpoints but has {len(itf['eps'])}")

    print("\nInterfaces:")
    for (num, alt), itf in sorted(interfaces.items()):
        eps = ", ".join(f"{e['addr']:02x} {XFER[e['attr'] & 3]} {e['size']}" for e in itf["eps"]) or "no endpoints"
        print(f"  {num}.{alt}  class {itf['cls']:02x}/{itf['sub']:02x}  {eps}")

    # ---- endpoint and FIFO budget ---------------------------------------
    print("\nESP32-S3 limits:")
    addrs = sorted({e["addr"] for e in endpoints})
    ins = [a for a in addrs if a & 0x80]
    outs = [a for a in addrs if not a & 0x80]
    c.check(all(1 <= (a & 0x0F) < EP_COUNT for a in addrs), f"endpoint numbers are within 1..{EP_COUNT - 1}")
    c.check(len(ins) <= EP_IN_COUNT - 1,
            f"{len(ins)} IN endpoints besides EP0 (limit {EP_IN_COUNT - 1})")
    # The same address may appear in several alternate settings, but must
    # always be the same kind of endpoint.
    kinds = {}
    for e in endpoints:
        kinds.setdefault(e["addr"], set()).add((e["attr"] & 3, e["size"]))
    c.check(all(len(k) == 1 for k in kinds.values()), "each endpoint address has one type and size")

    size_of = {a: max(e["size"] for e in endpoints if e["addr"] == a) for a in addrs}
    largest_out_words = max([-(-EP0_SIZE // 4)] + [-(-size_of[a] // 4) for a in outs])
    rx_words = 13 + 1 + 2 * (largest_out_words + 1) + 2 * EP_COUNT
    in_words = -(-EP0_SIZE // 4) + sum(-(-size_of[a] // 4) for a in ins)
    # Worst case in TinyUSB: IN FIFOs doubled, and DMA bookkeeping reserved.
    worst = rx_words + 2 * in_words + 2 * EP_COUNT
    c.check(worst <= FIFO_WORDS,
            f"FIFO use, worst case {worst} of {FIFO_WORDS} words (receive {rx_words}, transmit {in_words} x2)")

    # ---- CDC-ACM, as usb-serial-for-android needs it --------------------
    print("\nCDC-ACM serial port:")
    cdc_iads = [i for i in iads if i["cls"] == CLASS_CDC and i["sub"] == 2]
    c.check(len(cdc_iads) == 1 and cdc_iads[0]["count"] == 2,
            "one CDC/ACM interface association descriptor covering 2 interfaces")
    comm = [i for i in interfaces.values() if i["cls"] == CLASS_CDC and i["sub"] == 2]
    data = [i for i in interfaces.values() if i["cls"] == CLASS_CDC_DATA]
    c.check(len(comm) == 1 and len(data) == 1, "one control interface and one data interface")
    if comm and data and cdc_iads:
        comm, data = comm[0], data[0]
        c.check(cdc_iads[0]["first"] == comm["num"] and data["num"] == comm["num"] + 1,
                "the association starts at the control interface and the data interface follows it")
        first = comm["eps"][0] if comm["eps"] else None
        c.check(first is not None and first["addr"] & 0x80 and first["attr"] & 3 == 3,
                "the control interface's first endpoint is the interrupt IN notification endpoint")
        union = [d for d in comm["cs"] if d[2] == 0x06]
        c.check(len(union) == 1 and union[0][3] == comm["num"] and union[0][4] == data["num"],
                "union descriptor ties the control and data interfaces together")
        bulk = [e for e in data["eps"] if e["attr"] & 3 == 2]
        c.check(len(bulk) == 2 and {bool(e["addr"] & 0x80) for e in bulk} == {True, False},
                "the data interface has one bulk IN and one bulk OUT endpoint")
        c.check(all(e["size"] == 64 for e in bulk), "bulk endpoints are 64 bytes (full speed)")

    # ---- audio ------------------------------------------------------------
    print("\nUAC2 audio:")
    audio_iads = [i for i in iads if i["cls"] == CLASS_AUDIO]
    c.check(len(audio_iads) == 1 and audio_iads[0]["count"] == 3,
            "one audio interface association descriptor covering 3 interfaces")
    control = [i for i in interfaces.values() if i["cls"] == CLASS_AUDIO and i["sub"] == 1]
    c.check(len(control) == 1, "one audio control interface")
    if control:
        header = [d for d in control[0]["cs"] if d[2] == 0x01]
        c.check(len(header) == 1 and u16(header[0], 3) == 0x0200, "audio class version 2.0")
        c.check(sum(1 for d in control[0]["cs"] if d[2] == 0x0A) == 1, "one clock source")

    streams = sorted({n for (n, _a), i in interfaces.items() if i["cls"] == CLASS_AUDIO and i["sub"] == 2})
    c.check(len(streams) == 2, "two streaming interfaces (playback and capture)")
    seen = set()
    for num in streams:
        idle, active = interfaces.get((num, 0)), interfaces.get((num, 1))
        c.check(idle is not None and idle["neps"] == 0, f"interface {num}: alternate 0 has no endpoints (stream off)")
        if active is None:
            c.check(False, f"interface {num}: no alternate 1")
            continue
        general = [d for d in active["cs"] if d[2] == 0x01]
        fmt = [d for d in active["cs"] if d[2] == 0x02]
        channels = general[0][10] if general else None
        subslot, bits = (fmt[0][4], fmt[0][5]) if fmt else (None, None)
        iso = [e for e in active["eps"] if e["attr"] & 3 == 1]
        data_eps = [e for e in iso if (e["attr"] >> 4) & 3 == 0]
        feedback = [e for e in iso if (e["attr"] >> 4) & 3 == 1]
        if not data_eps:
            c.check(False, f"interface {num}: no isochronous data endpoint")
            continue
        ep = data_eps[0]
        direction = "capture (to host)" if ep["addr"] & 0x80 else "playback (from host)"
        seen.add(bool(ep["addr"] & 0x80))
        c.check((channels, subslot, bits) == (1, 2, 16), f"interface {num}, {direction}: mono, 16-bit samples")
        # 48 samples a millisecond, plus one spare for rate adjustment.
        c.check(ep["size"] == 49 * 2 and ep["interval"] == 1,
                f"interface {num}: {ep['size']}-byte packets every millisecond (room for 49 samples)")
        sync = (ep["attr"] >> 2) & 3
        if ep["addr"] & 0x80:
            c.check(not feedback, f"interface {num}: capture needs no feedback endpoint")
        else:
            c.check(sync == 1 and len(feedback) == 1 and feedback[0]["addr"] & 0x80,
                    f"interface {num}: asynchronous playback with an IN feedback endpoint")
    c.check(seen == {True, False}, "one stream in each direction")

    print()
    if c.failures:
        print(f"FAILED: {c.failures} check(s)")
        return 1
    print("PASS (structure only: enumeration and streaming still need a real host)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
