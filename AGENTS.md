# usb-mock-radio

ESP32-S3 firmware that enumerates over USB as a ham radio with a built-in sound card and CAT port, so modem software (Data2G, WSJT-X, VARA, fldigi) can drive it through Hamlib like a real rig. The first pass imitates an Icom IC-705; a Xiegu G90 personality is a later pass. No real RF, no analog audio.

[DESIGN.md](DESIGN.md) is the living design: status, scope, hardware, architecture, USB device, CAT table, PTT, audio/parrot/channel simulation, two-board link, Data2G findings, build plan, decision log, open questions, references. Read it before designing or implementing anything.

Keep it current as part of the work: update the section a fact belongs to, add a dated line to the decision log for each design decision, tick or add open questions, and refresh the Status section. [HANDOFF.md](HANDOFF.md) is the original handoff, frozen; do not edit it.

## Status

The CI-V core, the IC-705 personality, the audio DSP and the receive channel (`audio_engine`: noise at a set SNR, attenuation, mute while keyed and turnaround) and the mock's own controls (`mock_ctl`: CI-V frames to address `F0` that change the channel settings at run time) exist as portable C with host tests; the CAT side passes real Hamlib on the host. The firmware builds for the ESP32-S3 with the USB composite device, CAT over serial and audio loopback written, and its descriptors pass a structural check. Nothing has been flashed, so no USB behaviour is verified. Work proceeds in the milestones in DESIGN.md (host-testable groundwork, then USB composite + CAT + loopback, then parrot + channel simulation, then two-board link, then the G90 personality), each testable on its own.

## Stack

- ESP-IDF (Arduino only as a fallback if descriptor work is easier there).
- Target: ESP32-S3-DevKitC-1 clone, ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB octal PSRAM).
- Modules: `cat_civ` (shared CI-V core), `rig_ic705` (personality; `rig_g90` later), `audio_dsp`, `audio_engine` (receive channel so far; parrot and routing to come), `mock_ctl` (mock-only controls) and `usb_composite` exist. Planned: `link`, `ui`.
- USB stack: Espressif's `usb_device_uac` component in composite mode for audio, TinyUSB's CDC class directly for serial. Do not add `esp_tinyusb`; it brings its own `tusb_config.h` and has no audio class.

## Hard constraints

- **Endpoint budget:** the S3 allows four IN endpoints besides EP0, and the device uses all four (audio feedback, audio capture, CDC notification, CDC data). Do not add HID, a second serial port, a second audio function, or any other IN endpoint. After any change to the descriptors or `tusb_config.h`, build and run `python3 host/tools/check_usb_descriptors.py`; it must pass.
- **Native USB only:** the mock radio runs on the "USB" jack (GPIO19/20). The "COM"/"UART" jack is for flashing and logs.
- **Reserved pins:** GPIO35, 36, 37 belong to the octal PSRAM. PSRAM must be configured as Octal (OPI) in menuconfig.
- **CAT is Icom CI-V, first as an IC-705** (Hamlib rig model 3085, address `A4`): binary frames `FE FE <to> <from> <cmd> ... FD`, OK is `FB`, error is `FA`. Answer frames at any radio address and reply from that address, except `F0`, which is the mock's own (`mock_ctl`). Take reply layouts from Icom's IC-705 CI-V Reference Guide and from what `rigctl -m 3085 -vvvv` sends, not from memory; Hamlib's simulators are incomplete and only a hint.
- **Keep the core radio-neutral.** `cat_civ` must not name a radio; everything radio-specific (address, ID, mode table, supported commands, ranges, defaults) lives in the personality module, so the G90 (Hamlib 3088) can be added later without editing the core. The G90 and DE-19 are not accessible during this project, so do not plan on captures from them.
- **Mock-only controls** go to CI-V address `F0` and live in `mock_ctl`, never in a personality or the core. New ones take a command number from the table in DESIGN.md (some are reserved there), refuse out-of-range values with `FA`, and get a matching setting in `host/tools/mockctl.py`.
- **PTT sources:** CAT (CI-V `1C 00`), a serial control line, and optional VOX. Control-line PTT is a selectable mode, off by default, because hosts hold RTS and DTR high on open. Receive audio is muted (or low noise floor) while keyed.
- **CDC descriptors:** keep the interrupt IN notification endpoint and use device class 0xEF/2/1 with a CDC IAD; usb-serial-for-android (used by Data2G) requires both.
- **Link audio** between boards is 12 kHz 16-bit mono, half duplex keyed by PTT, with tens of ms of buffering and sample drop/repeat for clock drift.

## Build and test

Host build of the portable modules (needs only a C compiler, plus Hamlib's `rigctl` for the second command and `python3` for the third):

```bash
make -C host test
```

```bash
make -C host rigctl
```

```bash
make -C host mockctl
```

Run the first after any change under `components/`, the second after any change to `components/cat_civ` or a personality, and the third (needs only `python3`) after any change to `components/mock_ctl` or `host/tools/mockctl.py`. Regenerate the resampler table with `host/tools/gen_resampler_fir.py --table` rather than editing it by hand. The host build uses `-Werror` with address and undefined-behaviour sanitizers; keep it clean.

Firmware build, with ESP-IDF v5.5.6 (the repository root is the ESP-IDF project). Activate ESP-IDF in the shell first, from wherever it is installed:

```bash
. ~/esp/esp-idf/export.sh
```

```bash
idf.py build
```

```bash
idf.py -p <COM port> flash monitor
```

Board settings live in `sdkconfig.defaults`; `sdkconfig` is generated and not committed. After changing the defaults, delete `sdkconfig` and rebuild. The portable components must keep building in both the host Makefile and ESP-IDF.

CAT acceptance test on a PC (must pass with no Hamlib errors):

```bash
rigctl -m 3085 -r /dev/ttyACM0 -vvvv
```

then `f`, `F 7074000`, `m`, `T 1`, `T 0`.

Hardware-dependent behaviour (USB enumeration, audio, Hamlib polling, Android phones) cannot be verified without a board attached. Say so plainly rather than claiming it works.
