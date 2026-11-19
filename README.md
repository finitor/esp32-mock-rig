# usb-mock-radio

ESP32-S3 firmware that plugs into a computer or Android phone over USB and looks like a ham radio with a built-in sound card and CAT port. Modem software such as Data2G, WSJT-X, VARA and fldigi can drive it through Hamlib as if it were a real rig, with no RF involved.

**Status:** early. The CAT protocol core, the IC-705 personality, the audio DSP and the channel simulation are written and tested on a desktop, and the CAT side passes real Hamlib. The firmware, including the USB sound card and serial port, builds but has not yet been run on a board.

## What it will do

- Appear as one USB device: a 48 kHz mono sound card plus a serial port.
- Emulate an Icom IC-705's CAT (CI-V protocol) on the serial port (Hamlib rig model 3085), with a Xiegu G90 personality (Hamlib 3088) planned as a later swap-in.
- Key from CAT, a serial control line, or VOX.
- Parrot mode: record transmit audio and play it back as receive audio.
- Simulate a channel: noise at a set SNR, attenuation, turnaround delay.
- Link two boards (ESP-NOW or wired UART) so two modems can hold an ARQ session over a fake radio path.

## Hardware

An ESP32-S3-DevKitC-1 style board with the ESP32-S3-WROOM-1 N16R8 module (16 MB flash, 8 MB octal PSRAM) and two USB jacks. The host connects to the jack labeled "USB"; the "COM"/"UART" jack is for flashing and logs. No other parts are needed.

## Documents

| File | Purpose |
| --- | --- |
| [DESIGN.md](DESIGN.md) | The living design: architecture, USB and CAT details, build plan, decision log, open questions |
| [AGENTS.md](AGENTS.md) | Instructions for coding agents working in this repository |
| [HANDOFF.md](HANDOFF.md) | The original project handoff, kept as a snapshot |

## Building

The protocol code builds and tests on a desktop with only a C compiler:

```bash
make -C host test
```

With Hamlib installed, this runs real `rigctl` against the parser through a pseudo-terminal, with no radio or board attached:

```bash
make -C host rigctl
```

The firmware builds with ESP-IDF v5.5.6. With ESP-IDF activated in the shell:

```bash
idf.py build
```

The firmware presents the sound card and CAT serial port, with audio looped back. Noise at a set SNR, attenuation and radio-style receive muting can be switched on at build time with `idf.py menuconfig`. None of it has been tested on hardware yet. To check the USB descriptors in a build against the ESP32-S3's limits:

```bash
python3 host/tools/check_usb_descriptors.py
```
