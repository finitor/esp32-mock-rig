# usb-mock-radio

ESP32-S3 firmware that plugs into a computer or Android phone over USB and looks like a ham radio with a built-in sound card and CAT port. Modem software such as Data2G, WSJT-X, VARA and fldigi can drive it through Hamlib as if it were a real rig, with no RF involved.

**Status:** early. The CAT protocol core, the IC-705 and G90 personalities, the audio DSP, parrot mode, the channel simulation, PTT from CAT, a control line or VOX, their run-time controls, the web console's settings and telemetry and the two-board link are written and tested on a desktop, and the CAT side passes real Hamlib as both radios. The G90 personality is built from Xiegu's CAT manual and Hamlib's source, and has not been compared with a real G90. The firmware, including the USB sound card, the serial port, the web console over Wi-Fi and the link over ESP-NOW or a UART, builds but has not yet been run on a board.

## What it will do

- Appear as one USB device: a 48 kHz mono sound card plus a serial port.
- Emulate the CAT (CI-V protocol) of an Icom IC-705 (Hamlib rig model 3085) or a Xiegu G90 (Hamlib 3088) on the serial port, switchable while it runs.
- Key from CAT, a serial control line, or VOX.
- Parrot mode: record transmit audio and play it back as receive audio; keep recordings in flash as clips.
- Simulate a channel: noise at a set SNR, attenuation, turnaround delay, and HF fading by the same named conditions as Data2G's simulator.
- Link two boards (ESP-NOW or wired UART) so two modems can hold an ARQ session over a fake radio path.
- Offer a web console over Wi-Fi to change settings and watch telemetry while it runs.

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

With Hamlib installed, this runs real `rigctl` against the parser as each radio, through a pseudo-terminal, with no radio or board attached:

```bash
make -C host rigctl
```

The firmware builds with ESP-IDF v5.5.6. With ESP-IDF activated in the shell:

```bash
idf.py build
```

The firmware presents the sound card and CAT serial port, with audio looped back. Noise at a set SNR, attenuation and radio-style receive muting are off at power-up unless switched on at build time with `idf.py menuconfig`, and can be changed while it runs over the CAT serial port:

```bash
python3 host/tools/mockctl.py -p <CAT port> noise=on snr=-12.5
```

The board starts as an IC-705. To make it a G90, and keep it one through restarts (set the programs that use it to the same radio):

```bash
python3 host/tools/mockctl.py -p <CAT port> rig=g90
```

Fading makes the receive audio behave as on an HF path: two paths that fade independently. The conditions are named as in the Data2G project's simulator (`mpg` good, `mpp` poor, `mpd` disturbed, `mps` slow and selective, and four severe ITU-R F.1487 ones), and the SNR is then the average. A poor path at 5 dB:

```bash
python3 host/tools/mockctl.py -p <CAT port> fade=mpp noise=on snr=5
```

Parrot mode records each transmission and plays it back as receive audio; with a period it comes back in a later time slot, here one FT8 slot (15 s) on:

```bash
python3 host/tools/mockctl.py -p <CAT port> route=parrot period=15
```

A recording can be kept in flash as one of five clips and played back later, after a power cycle too. Keeping one takes seconds and disturbs audio while it lasts:

```bash
python3 host/tools/mockctl.py -p <CAT port> clip-save=1
```

```bash
python3 host/tools/mockctl.py -p <CAT port> route=parrot clip-play=1
```

Settings return to their built-in values at power-up unless they are kept. This makes the board start with the VOX on from now on, which Data2G's default PTT mode needs:

```bash
python3 host/tools/mockctl.py -p <CAT port> vox=on keep
```

While a modem application holds the CAT port, the same settings are in the web console, which also shows PTT, levels and the audio buffer live. Its "Board health" section shows what the board itself is doing: free memory, how busy each processor core is, how long the audio work takes, and each task's stack headroom when asked. The same goes to the log on the COM jack every 30 seconds. The board joins the Wi-Fi network set in `idf.py menuconfig` or in the console's own Wi-Fi form; with none, it hosts a network named `mockradio-xxxx` (password `mockradio`) and the console is at `http://192.168.4.1/`. On a shared network it is at `http://mockradio-xxxx.local/`. The console has no login, so use it on a network you trust.

Two boards hear each other in the link route: what one host transmits, the other receives, when both radios are on the same frequency. The audio travels over ESP-NOW, with both boards on the same Wi-Fi channel (joined to the same network, or one joined to the network the other hosts), or over three wires between their UARTs if the firmware is built for that. Two linked boards share one channel: noise, SNR and fading set on either apply to both, and the path fades alike both ways. Set the route on each board, here or in its web console:

```bash
python3 host/tools/mockctl.py -p <CAT port> route=link
```

To see the console without a board, with two simulated radios joined by the link, at `http://localhost:8080` and `http://localhost:8081`:

```bash
make -C host console-sim
```

None of the firmware has been tested on hardware yet.

To check the USB descriptors in a build against the ESP32-S3's limits:

```bash
python3 host/tools/check_usb_descriptors.py
```
