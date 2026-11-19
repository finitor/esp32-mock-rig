# usb-mock-radio

ESP32-S3 firmware that enumerates over USB as a ham radio with a built-in sound card and CAT port, so modem software (Data2G, WSJT-X, VARA, fldigi) can drive it through Hamlib like a real rig. The first pass imitates an Icom IC-705; a Xiegu G90 personality is a later pass. No real RF, no analog audio.

[DESIGN.md](DESIGN.md) is the living design: status, scope, hardware, architecture, USB device, CAT table, PTT, audio/parrot/channel simulation, two-board link, Data2G findings, build plan, decision log, open questions, references. Read it before designing or implementing anything.

Keep it current as part of the work: update the section a fact belongs to, add a dated line to the decision log for each design decision, tick or add open questions, and refresh the Status section. [HANDOFF.md](HANDOFF.md) is the original handoff, frozen; do not edit it.

## Status

The CI-V core and IC-705 personality exist as portable C and pass real Hamlib on the host. No ESP-IDF project exists yet. Work proceeds in the milestones in DESIGN.md (host-testable groundwork, then USB composite + CAT + loopback, then parrot + channel simulation, then two-board link, then the G90 personality), each testable on its own.

## Stack

- ESP-IDF with `esp_tinyusb` (Arduino only as a fallback if descriptor work is easier there).
- Target: ESP32-S3-DevKitC-1 clone, ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB octal PSRAM).
- Planned modules: `usb_composite`, `cat_civ` (shared CI-V core), `rig_ic705` (personality; `rig_g90` later), `audio_engine`, `link`, `ui`.

## Hard constraints

- **Endpoint budget:** the S3 USB-OTG has 6 endpoints (5 IN/OUT + 1 IN). One UAC2 audio function (48 kHz 16-bit mono, playback + capture) plus one CDC-ACM port uses nearly all of them. Do not add HID, a second serial port, or a second audio function.
- **Native USB only:** the mock radio runs on the "USB" jack (GPIO19/20). The "COM"/"UART" jack is for flashing and logs.
- **Reserved pins:** GPIO35, 36, 37 belong to the octal PSRAM. PSRAM must be configured as Octal (OPI) in menuconfig.
- **CAT is Icom CI-V, first as an IC-705** (Hamlib rig model 3085, address `A4`): binary frames `FE FE <to> <from> <cmd> ... FD`, OK is `FB`, error is `FA`. Answer frames at any radio address and reply from that address. Take reply layouts from Icom's IC-705 CI-V Reference Guide and from what `rigctl -m 3085 -vvvv` sends, not from memory; Hamlib's simulators are incomplete and only a hint.
- **Keep the core radio-neutral.** `cat_civ` must not name a radio; everything radio-specific (address, ID, mode table, supported commands, ranges, defaults) lives in the personality module, so the G90 (Hamlib 3088) can be added later without editing the core. The G90 and DE-19 are not accessible during this project, so do not plan on captures from them.
- **PTT sources:** CAT (CI-V `1C 00`), a serial control line, and optional VOX. Control-line PTT is a selectable mode, off by default, because hosts hold RTS and DTR high on open. Receive audio is muted (or low noise floor) while keyed.
- **CDC descriptors:** keep the interrupt IN notification endpoint and use device class 0xEF/2/1 with a CDC IAD; usb-serial-for-android (used by Data2G) requires both.
- **Link audio** between boards is 12 kHz 16-bit mono, half duplex keyed by PTT, with tens of ms of buffering and sample drop/repeat for clock drift.

## Build and test

Host build of the portable modules (needs only a C compiler, plus Hamlib's `rigctl` for the second command):

```bash
make -C host test
```

```bash
make -C host rigctl
```

Run both after any change to `components/cat_civ` or a personality. The host build uses `-Werror` with address and undefined-behaviour sanitizers; keep it clean.

Once an ESP-IDF project exists:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <COM port> flash monitor
```

CAT acceptance test on a PC (must pass with no Hamlib errors):

```bash
rigctl -m 3085 -r /dev/ttyACM0 -vvvv
```

then `f`, `F 7074000`, `m`, `T 1`, `T 0`.

Hardware-dependent behaviour (USB enumeration, audio, Hamlib polling, Android phones) cannot be verified without a board attached. Say so plainly rather than claiming it works.
