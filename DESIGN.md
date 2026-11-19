# ESP32-S3 Mock Radio — Design

Living document. Edit it in place as the design changes: update the section a fact belongs to, add a dated line to the decision log, and tick or add open questions. [HANDOFF.md](HANDOFF.md) is the original Nov 19, 2026 handoff, kept as a frozen snapshot.

Operator: Tim, W0SIG (General).

## Status

Last updated: Nov 19, 2026.

- The CI-V core (`cat_civ`) and the IC-705 personality (`rig_ic705`) exist as portable C and pass real Hamlib on the host: `rigctl -m 3085` (Hamlib 4.7.1) over a pseudo-terminal, 16 command groups, no command rejected.
- The audio DSP (`audio_dsp`) exists as portable C with host unit tests: resampler, channel noise and attenuation, level meter, VOX detector, and a FIFO with clock-drift steering.
- The firmware builds for the ESP32-S3 (ESP-IDF v5.5.6, 241 KB image) with the whole of Milestone 1 written: the USB composite device (sound card plus CAT serial port), the serial port wired to the CAT parser, and audio loopback. Its descriptors pass a structural check against the S3's limits and Android's requirements. None of it has run on hardware: whether hosts enumerate it and stream audio is unknown until a board is flashed.
- Nothing has run on an ESP32 yet, and no modem application has been tried against any of it.
- The first pass emulates an Icom IC-705. A Xiegu G90 personality (the operator's own radio, used through a DE-19) is planned as a later swap-in module; the G90 and DE-19 are not accessible during this project.
- Next step: flash a board and work through Milestone 1, steps 6 and 9 (enumeration on each host, then the `rigctl` and audio acceptance tests). Milestone 2 code can be written meanwhile.
- Three ESP32-S3 boards are ordered and will not arrive before Sunday, Nov 22, 2026; nothing has been tested on hardware. Until then, target work is compile-only.
- Data2G's Android USB code has been read (fork at v0.5.2+8) and fits this design; see [Data2G target](#data2g-target).

## Goal and scope

Build ESP32-S3 firmware that plugs into a computer or Android phone over USB and looks like a ham radio with a built-in sound card and CAT port on one USB cable, the way an Icom IC-705's USB port does. Modem and logging software (Data2G, WSJT-X, VARA, Winlink, fldigi) should drive it through Hamlib exactly as it would a real rig.

The radio being imitated is a swappable personality. The first pass is an Icom IC-705. A later pass adds a Xiegu G90, so software can be set up on the bench and moved to the operator's real G90 without changing its rig settings.

In scope:

- One USB connection that shows up as a USB sound card (UAC2, 48 kHz 16-bit mono, playback + capture) plus a USB serial port (CDC-ACM).
- An Icom CI-V CAT emulation on the serial port: first an IC-705 that Hamlib (rig model 3085) accepts, later a G90 (rig model 3088).
- PTT from CAT, from a serial control line (Digirig style), and optionally VOX.
- Parrot mode: record transmit audio into PSRAM and play it back as receive audio.
- Channel simulation: noise at a set SNR, attenuation, optional fading.
- A two-board link (ESP-NOW first, wired UART as an option) so two phones running Data2G can hold an ARQ session over a fake radio path.

Out of scope for now: real RF, analog audio (no codec), non-CI-V radios, and descriptor-identical copies of the real radios' USB hardware (see [USB device](#usb-device)).

The wireless link between boards is unlicensed 2.4 GHz (Part 15), not amateur RF.

## Hardware

Target board: ESP32-S3-DevKitC-1 clone with the ESP32-S3-WROOM-1 N16R8 module (16 MB flash, 8 MB octal PSRAM).

| Item | Detail |
| --- | --- |
| USB jack for the mock radio | The one labeled "USB" (native USB, GPIO19/20). The "COM"/"UART" jack goes through a USB-serial bridge chip and can't do audio. |
| Second jack | "COM"/"UART" stays free for flashing and debug logs while the native port runs the mock radio. |
| PSRAM | Octal mode: ESP-IDF menuconfig → Octal, or Arduino "OPI PSRAM". Without it the extra 8 MB isn't available. |
| Reserved pins | GPIO35, 36, 37 are used by the octal PSRAM; don't use them. |
| Status LED | Onboard RGB LED, usually GPIO48; on some clones the "RGB" solder pad must be bridged. |
| Power | USB only. Phone OTG supplies it fine (roughly 100–300 mA with Wi-Fi on). Either jack, or both at once, can power the board, so the "COM"/"UART" jack can take a charger or power bank while the "USB" jack goes to the phone. On clones, check that diodes isolate the two jacks' 5 V lines before connecting two sources; without them the charger back-feeds the phone's port. This does not charge the phone. |
| Flashing | If the native port doesn't enumerate: hold BOOT, tap RESET, release BOOT, or flash through COM. |
| Optional | "-1U" module variant with an external antenna connector, for longer ESP-NOW range. |

Avoid single-USB boards whose only jack goes through a CH343 USB-serial chip: they can't present the native USB device.

## Architecture

```
 Phone or PC                    ESP32-S3 mock radio
 (Data2G, WSJT-X, VARA;   USB   +------------------------------------------+
  Hamlib: Icom IC-705)    <---> | USB composite device                     |
                                |   UAC2 audio, 48 kHz mono, both ways     |
                                |   CDC-ACM serial port (CAT + lines)      |
                                +------------------------------------------+
                                   |                          ^ v
                                   v                          | |
                                +--------------+  PTT  +--------------------+
                                | CI-V core +  | ----> | Audio engine       |
                                | personality  |       | parrot replay      |
                                | PTT: CAT/    |       | noise at set SNR   |
                                | line/VOX     |       +--------------------+
                                +--------------+              ^ v
                                +------------------------------------------+
                                | Link layer: loopback, ESP-NOW or UART    | <---> Second board
                                +------------------------------------------+
```

The host sees one USB device; inside, CAT state keys the audio engine, and the link layer decides where transmit audio goes: back to the same host (loopback/parrot) or to a second board.

Firmware modules:

- `usb_composite`: descriptors and TinyUSB callbacks for UAC2 + CDC-ACM, including line-state changes
- `cat_civ`: CI-V framing, address handling, command dispatch and the radio state struct (VFO A/B, mode, split, PTT, levels)
- `rig_ic705`: the IC-705 personality (first pass); `rig_g90` joins it later behind the same interface
- `audio_dsp`: the signal processing itself, with no hardware dependencies: 48↔12 kHz resampling, noise and attenuation, level meter, VOX detector, FIFO with drift steering
- `audio_engine`: routes audio between USB, the parrot buffer in PSRAM and the link, using `audio_dsp`; applies TX mute and turnaround delay
- `link`: one interface with loopback, ESP-NOW and UART backends; jitter buffer and drift correction
- `ui`: RGB LED states; a Wi-Fi status page later if wanted

`cat_civ`, the personalities and `audio_dsp` are written as portable C with no ESP-IDF dependencies, so they can be tested on the host.

Runtime: FreeRTOS, as bundled with ESP-IDF (the S3 runs its dual-core SMP build). No separate RTOS choice is needed; the Arduino fallback runs on the same kernel. TinyUSB, the audio engine and the link layer each get a task; `cat_civ` is passive and runs in whichever task delivers serial bytes.

Repository layout:

| Path | Contents |
| --- | --- |
| `components/cat_civ/` | CI-V core |
| `components/rig_ic705/` | IC-705 personality |
| `components/audio_dsp/` | Audio DSP |
| `components/usb_composite/` | USB device: descriptors, TinyUSB configuration, serial and audio callbacks |
| `main/` | Firmware entry point; for now a boot self-check |
| `CMakeLists.txt`, `sdkconfig.defaults` | ESP-IDF project file and board configuration (16 MB flash, octal PSRAM, UART console) |
| `host/` | Host-only build: `civ_pty` (parser behind a pseudo-terminal), `test_civ` and `test_dsp` (unit tests), `rigctl_check.sh` (real Hamlib acceptance check), `tools/gen_resampler_fir.py` (filter generator), `tools/check_usb_descriptors.py` (descriptor check on the built firmware) |

## USB device

One composite device: a single UAC2 audio function (48 kHz 16-bit mono, playback + capture) and a single CDC-ACM port.

Requirements:

- **Device class 0xEF / subclass 2 / protocol 1** (TinyUSB's usual composite setup), with an interface association descriptor for a 2-interface CDC function. usb-serial-for-android locates the port through that IAD.
- **CDC control interface with an interrupt IN notification endpoint as its first endpoint**, plus a CDC data interface with bulk IN and bulk OUT. usb-serial-for-android refuses the port without the notification endpoint, so it cannot be dropped to save an endpoint.
- **No extra functions.** No HID, second serial port or second audio function.

**Stack.** The sound card is Espressif's `usb_device_uac` component (v1.3.2) in its "part of a composite device" mode, which supplies the UAC2 class handling including playback rate feedback. The serial port is TinyUSB's CDC class used directly (TinyUSB 0.19). Espressif's `esp_tinyusb` wrapper is not used, because it has no audio support and insists on its own configuration. Our `usb_composite` component owns `tusb_config.h` and the descriptors that join the two.

**Layout as built:**

| Interface | Function | Endpoints |
| --- | --- | --- |
| 0 | Audio control | none |
| 1 | Audio streaming, playback from host (transmit audio) | `01` isochronous OUT, 98 bytes; `81` isochronous IN feedback, 4 bytes |
| 2 | Audio streaming, capture to host (receive audio) | `82` isochronous IN, 98 bytes |
| 3 | CDC control | `83` interrupt IN notification, 8 bytes |
| 4 | CDC data | `04` bulk OUT and `84` bulk IN, 64 bytes |

Audio packets are 98 bytes: 48 samples per millisecond plus one spare for rate adjustment. Each streaming interface has an alternate setting 0 with no endpoints, which is how a host turns a stream off.

**Endpoint budget (the main technical risk).** The S3 has endpoint numbers 1 to 6, and TinyUSB's driver allows four IN endpoints besides EP0 (`ep_in_count = 5` including EP0, in its `dwc2_esp32.h`). The layout uses all four: audio feedback, audio capture, CDC notification and CDC data. All endpoints share 1024 bytes of FIFO; the layout needs 214 of 256 words in TinyUSB's worst case. There is no room for another IN endpoint. If the feedback endpoint ever has to go, playback would become adaptive, which means replacing the audio component's handling.

`host/tools/check_usb_descriptors.py` reads the descriptors out of the built firmware and checks all of the above, plus the CDC and audio structure. It checks structure only; it cannot tell whether a host accepts the device.

**Identity.** The device uses the pid.codes test IDs (VID `1209`, PID `0001`), which are for private bench use. It must never use a radio manufacturer's IDs. The serial number string is the chip's MAC address, so two boards on one host are distinguishable.

Line-keyed PTT arrives as CDC `SET_CONTROL_LINE_STATE`. Baud rate and framing are accepted and ignored, so whatever serial settings the host uses for the selected radio work unchanged.

**Relation to the real radios.** The mock is functionally equivalent to each radio's USB port, not descriptor-identical: one sound card and one serial port in a single composite device. Software that selects devices by name or VID/PID will see different ones than on the real hardware; software that selects by port and audio device works the same.

- **IC-705:** the closest match. The real radio is itself a composite USB device with a sound card and two CDC-ACM serial ports, "Serial Port A (CI-V)" and "Serial Port B". The mock presents only the first; a second CDC port would not fit the endpoint budget.
- **G90 with DE-19 (later pass):** the DE-19 connects over USB-C and presents a USB audio device plus a virtual COM port; its ACC cable carries audio to the G90 and its 3.5 mm CIV cable carries CAT. One user write-up reports a WCH CH342 serial chip, a dual-UART part, so the real unit may show two COM ports. The chip, the number of ports, the audio chip and the VID/PID are not confirmed.

## CAT emulation: Icom CI-V

The mock radio speaks Icom's CI-V protocol. Which radio it claims to be is a personality module; the framing and most commands are shared.

| Personality | Pass | Hamlib model | Default CI-V address | Reference |
| --- | --- | --- | --- | --- |
| Icom IC-705 | First | 3085 | `A4` | Icom's IC-705 CI-V Reference Guide; Hamlib's Icom backend |
| Xiegu G90 | Later | 3088 | `70`, `88` or `A4` depending on source | Hamlib's `xiegu.c`; Radioddity CAT manual; the real radio when available |

### Core and personalities

- **`cat_civ` (core):** buffers bytes until `FD`, validates the frame, dispatches on command and sub-command, keeps the radio state, and builds replies. It implements the commands every CI-V radio shares.
- **Personality (`rig_ic705`, later `rig_g90`):** a table of what makes one radio different: default address, transceiver ID reply, mode-byte table, which commands and sub-commands exist, frequency coverage and transmit bands, power limit, level and meter defaults, power-up state, and handlers for radio-specific commands.

The core never names a radio. Anything radio-specific goes through the personality interface, so adding the G90 means adding one module rather than editing the core. The first pass selects the personality at build time; run-time selection (a stored setting) arrives with the G90. The CI-V address cannot be used to pick the personality, because Hamlib 4.7.1 addresses both radios at `A4`.

### Framing

Binary frames, not ASCII:

```
command:  FE FE <radio addr> <controller addr> <cmd> [<sub>] [<data>...] FD
data:     FE FE <controller addr> <radio addr> <cmd> [<sub>] <data>...   FD
OK:       FE FE <controller addr> <radio addr> FB FD
error:    FE FE <controller addr> <radio addr> FA FD
```

**Address rule: answer whatever radio address the frame is sent to, and reply from that same address.** The controller address is normally `E0`. Measured with Hamlib 4.7.1 on the development Mac: the IC-705 backend opens with `FE FE A4 E0 03 FD`, the IC-7300 backend with `FE FE 94 E0 03 FD` followed by `25 00`, and the G90 backend with `FE FE A4 E0 03 FD`. Accepting any address keeps the mock independent of the host's Hamlib version and lets closely related models be tried.

**No bus echo.** The IC-705 has a "CI-V USB Echo Back" setting; the mock behaves as with it off.

### IC-705 personality (first pass)

Minimum command set. Exact data layouts come from Icom's CI-V Reference Guide and from what Hamlib sends, not from memory.

| Command | Meaning | Mock reply |
| --- | --- | --- |
| `03` | Read operating frequency | `03` + 5 BCD bytes (10 digits, Hz) |
| `05` + 5 BCD bytes | Set operating frequency | OK |
| `04` | Read mode | `04` + mode byte + filter byte |
| `06` + mode [+ filter] | Set mode | OK |
| `07 00` / `07 01` / `07 A0` / `07 B0` | Select VFO A / VFO B / copy A to B / exchange | OK |
| `0F` / `0F 00` / `0F 01` | Split read / off / on | value / OK |
| `14 <sub>` | Levels (AF, RF gain, RF power, ...) | Stored value, 2 BCD bytes |
| `15 <sub>` | Meters (S-meter, power, SWR, ...) | Simulated value, 2 BCD bytes |
| `16 <sub>` | Functions (NB, NR, ...) | Stored value |
| `19 00` | Transceiver ID | `19 00 A4` |
| `1A 03` / `1A 06` | Filter width / data mode | Stored value |
| `1A 05 <item>` | Menu settings | The items Hamlib asks for; error otherwise |
| `1C 00` / `1C 00 <0 or 1>` | PTT read / set | value / OK |
| `25 <vfo>` / `26 <vfo>` | Selected or unselected VFO frequency / mode with data flag and filter | value / OK |
| anything else | Unsupported | error (`FA`) |

Mode bytes are the usual Icom codes (`00` LSB, `01` USB, `02` AM, `03` CW, `04` RTTY, `05` FM, `06` WFM, `07` CW-R, `08` RTTY-R, `17` DV); data mode is a separate flag. Confirm against the guide.

Emulated radio facts: receive 0.030–199.999 MHz and 400–470 MHz; transmit on the amateur HF, 50, 144 and 430 MHz bands; 10 W maximum. Power-up state: 14.074 MHz, USB with data mode on, VFO A, split off.

How it is verified without a radio:

- **Hamlib is the acceptance test.** `rigctl -m 3085 -vvvv` pointed at the parser on the host shows every frame Hamlib sends and whether it accepts each reply. This needs no radio and no ESP32.
- **Icom's CI-V Reference Guide** gives the official data layout for every command.
- Hamlib's `simulators/simic705.c` is a hint only. It sends nothing for unknown commands, does not handle `1A 05`, and rewrites the received buffer rather than building replies.

### G90 personality (later pass)

What is known so far, kept here so the later pass starts from it:

- The G90 speaks a subset of CI-V at 19200 baud, 8N1, no handshake. Hamlib keys it with `1C 00`.
- Addresses: Hamlib 4.7.1 sends to `A4`, current Hamlib master defaults to `88`, and `70` is reported for some units. The any-address rule covers all three.
- Identity: newer Hamlib probes `1D 19` first and accepts `00 90` as a G90, then falls back to `19 00`.
- Hamlib notes that firmware v20240504.8 handles `25`/`26` badly (Hamlib issue 1547), and that undocumented `14` sub-commands return a fixed 255. Whether to support `25`/`26` or reply error so Hamlib falls back is undecided.
- Radio facts: receive 0.5–30 MHz, transmit on the amateur HF bands, 20 W maximum. Digital modes are U-D and L-D; how they are reported over CI-V is not confirmed.
- Hamlib's `simulators/simxiegug90.c` is only a partial reference: it answers the ID probe with a value Hamlib does not recognise, never answers `1D 19`, stores Hamlib enum values rather than CI-V mode codes, and sends nothing for unknown commands.
- No reference traffic from a real G90 exists for this project. Until the station is available, the G90 personality is built from Hamlib's `rigs/icom/xiegu.c` and the Radioddity CAT manual, and is unverified against hardware.

### Mock-only controls

Replay last TX, play canned clip N, set SNR, set link mode, set PTT source and select personality need a channel that no real radio command uses. Proposal: CI-V frames sent to a private radio address reserved for the mock. Still to be decided.

## PTT

Three sources:

| Source | Trigger |
| --- | --- |
| CAT | CI-V `1C 00 01` keys, `1C 00 00` unkeys |
| Control line | RTS (or DTR) asserted, Digirig style |
| VOX | Transmit audio level above a threshold |

**Control-line PTT is a selectable mode, off by default.** The IC-705 works the same way: keying from DTR or RTS is a menu setting (USB SEND) rather than always on. Hosts raise DTR and RTS when they open a serial port and leave them high unless that line is the configured PTT. Data2G does this explicitly, and desktop operating systems do it for Hamlib. If the firmware always treated RTS as PTT, a host using CAT or VOX keying would hold the mock radio in transmit for the whole session.

While keyed, the receive stream carries silence or a low noise floor, as a real radio mutes receive during TX.

## Audio, parrot mode and channel simulation

Audio stays digital end to end: USB audio in from the host is transmit, USB audio out to the host is receive.

**Parrot mode (record/replay)**

1. On PTT on, write incoming transmit audio into a PSRAM ring buffer.
2. On PTT off, stop recording; after an optional delay, play the buffer out as receive audio.
3. Optionally save the last transmission or canned clips to a flash partition so they survive a power cycle.

| Storage format | Rate | Capacity in 8 MB PSRAM |
| --- | --- | --- |
| As received | 48 kHz, 16-bit mono (~96 KB/s) | ~85 s |
| Decimated ÷4 (low-pass first) | 12 kHz, 16-bit mono (~24 KB/s) | ~5.5 min |

Use cases: FT8 self-decode (TX in one 15 s slot, replay aligned to the next slot boundary; WSJT-X should decode it with near-zero DT), checking TX drive and clipping, and confirming a modem detects its own frames. ARQ modems (VARA, Data2G connected mode) can't complete a session against a parrot; use the two-board link for that.

**Resampling.** One 96-tap low-pass serves both directions: flat within 0.01 dB to 4 kHz, 3 dB down at 5 kHz, and at least 69 dB down from 6.5 kHz, so the 12 kHz path carries roughly 0–4.5 kHz. A 48 → 12 → 48 kHz round trip delays audio by 92 samples at 48 kHz, about 1.9 ms. Coefficients come from `host/tools/gen_resampler_fir.py`.

**SNR convention.** Noise is white across the whole sampled band, and SNR is quoted in a reference bandwidth, as HF software does: 2500 Hz for WSJT-X, 3000 Hz for VARA and most channel simulators. `dsp_noise_rms_for_snr` takes the reference bandwidth as a parameter; which one the mock's SNR setting uses by default is an open question.

**Channel simulation (applied on the receive side)**

- Additive noise at a configurable SNR, so ARQ modems step down through their speed levels.
- Attenuation, and optionally fading or a small frequency offset (PathSim-style), later.
- TX→RX turnaround delay of a few tens of ms, to mimic a real radio switching.

## Two-board link

The end goal: two Android phones running Data2G, each plugged into an S3 mock radio, holding a connected (ARQ) session over a link between the two boards.

| Link | Throughput | Notes |
| --- | --- | --- |
| Loopback (single board) | n/a | Parrot and self-tests |
| ESP-NOW | ~0.5–1 Mbps usable | No router, low latency, tens of meters; first choice |
| Wired UART | several Mbps | TX/RX/GND between boards; most deterministic |
| Wi-Fi UDP | network-dependent | Boards anywhere; more latency and jitter |

Audio over the link: 12 kHz 16-bit mono (~192 kbps), which covers Data2G's 2400 Hz bandwidth with room to spare.

Behavior:

- Half duplex keyed by PTT: board A's transmit audio becomes board B's receive audio.
- Pass audio only when both radios are on the same frequency (exchanged over the link).
- Keep buffering to tens of ms so ARQ turnaround timing looks like a real radio.
- Clock drift: each S3 has its own crystal, so the two 48 kHz rates differ by a few ppm. Handle it by occasionally dropping or repeating a sample (or resampling) to keep buffers from slowly filling or draining.

## Data2G target

[Data2G](https://github.com/arodland/Data2G/) is an OFDM HF data modem aimed at VARA HF/Pactor-class performance, with ARQ and FEC modes, a VARA-compatible TNC (Winlink, BBS), a KISS TNC, and a 2400 Hz full-speed or 500 Hz narrow bandwidth setting. A fork is checked out at `../Data2G`.

What its Android source does (read at v0.5.2+8 on Nov 19, 2026; not yet tested on hardware):

| Area | Behavior |
| --- | --- |
| Audio | Android's own USB audio driver, through `AudioRecord` / `AudioTrack` with a preferred device, mono 16-bit. Capture tries the app rate, then 48000, 16000, 8000. The app needs no USB access for audio. |
| Serial | usb-serial-for-android 3.11.0, vendored. CDC-ACM is matched by interface class, so no VID/PID registration is needed. Its USB filter comment names the IC-705 as an example of a radio with built-in CDC-ACM. |
| CAT | Bundled Hamlib 4.7.2 with all serial backends, reached through a loopback socket bridge to the USB port. Icom IC-705 (model 3085) and Xiegu G90 (model 3088) are selectable, so desktop `rigctl` is a fair stand-in for its CAT traffic, allowing for Hamlib version differences. |
| PTT | Rig pane offers VOX, CAT, DTR line and RTS line. |
| Line defaults | DTR and RTS are asserted on open; only the line chosen for PTT is held low. |
| Network rig | CAT connection can be USB serial, Bluetooth, or "Network (rigctld or ser2net)". |
| USB permission | The attach filter matches CDC by interface class, so "always open with Data2G" can make the permission stick across replugs. |

To check on a phone:

- Each phone's USB audio driver accepts a mono-only playback device.
- Android grants serial access on a device that also has audio capture. Android is believed to require microphone permission first; Data2G requests it, so grant that before the USB prompt.
- Opening the serial port succeeds on a composite audio+serial device. Data2G's own comments note it can fail if another driver holds the device.

## Other Android clients: FT8CN

[FT8CN](https://github.com/N0BOY/FT8CN) is an open-source Android FT8 app with USB audio and USB serial CAT. FT8 is one-way, so it is the phone-side counterpart of the WSJT-X self-decode test for parrot mode. It also speaks CI-V itself instead of going through Hamlib, which makes it a second, independent client for the IC-705 personality.

What its source sends an IC-705 over a USB cable (read on Nov 19, 2026 from the `release` branch, last pushed Jan 2025: `rigs/IcomRig.java`, `rigs/IcomRigConstant.java`, `MainViewModel.java`). The frames were replayed through `cat_civ` + `rig_ic705` on the host; the app itself has not been run.

| When | Command | Meaning | Mock's reply |
| --- | --- | --- | --- |
| On connect and on band change | `26 00 01 01 01` | Selected VFO to USB, data on, filter 1 | `FB` |
| 800 ms later | `05 <5 BCD bytes>` | Set frequency | `FB` |
| Every PTT on | `1A 05 01 19 01` | Set the data modulation input to USB (`02` over Bluetooth, `03` over WLAN) | `FA` (not implemented) |
| PTT on, CAT mode only | `1C 00 01` | Transmit | `FB` |
| Every 500 ms while keyed | `15 12` | Read SWR meter | `15 12 00 00` |
| Every 500 ms while keyed | `15 13` | Read ALC meter | `15 13 00 00` |
| PTT off, CAT mode only | `1C 00 00` | Receive | `FB` |

- It never checks `FB` or `FA`; "connected" means the serial port opened. The `FA` on `1A 05 01 19` therefore does no harm, though a real IC-705 accepts that command.
- It never asks for the frequency. A `03` read exists in the code but nothing calls it; it only follows unsolicited `00` frames, which the mock does not send.
- It accepts replies addressed to `E0` or `00` from the configured radio address, so the mock's replies parse.
- PTT modes are VOX (the default), CAT, RTS and DTR. In RTS or DTR mode it toggles the line and sends no `1C 00`, so the mock keys only with control-line PTT mode enabled. The serial open code does not set DTR or RTS itself.
- It warns when the SWR reading is 120 or more, or the ALC reading is above 120. The mock returns 0 for both. Driving the ALC reading from the transmit audio level would make the app's overdrive warning usable against the mock.
- Its rig list has the IC-705 at address `A4`, 115200 baud.

Not checked: its audio side (sample rate, how it picks the USB sound card), whether it shows a decode of its own transmission, and the FT8TW fork.

**FT8AF** ([patrickrb/FT8AF](https://github.com/patrickrb/FT8AF)) is a maintained fork of FT8CN (last pushed Nov 14, 2026; source read on Nov 19, 2026, app not run). For an IC-705 it uses the same CI-V code and the same commands as the table above, plus:

- A `03` frequency read every 2 s while not transmitting, starting a few seconds after connect. The mock answers with the frequency last set.
- A liveness watchdog: once the radio has answered at least once, 8 s without a reply turns the app's connection status to error. So unlike FT8CN it depends on the mock's replies.
- `1C 01 02` (start antenna tuner) when the user asks for a tune. The mock answers `FB` and does not key.

It bundles Hamlib 4.7.2, but only its "(Hamlib)" rig entries (FT-891, QMX) use it; the IC-705 entry goes through the app's own CI-V code. FT8AF is the preferred Android FT8 client for testing the mock, because it reads replies and is maintained. qFT8 and Decodium Mobile also claim USB CAT; no source was found for them and neither has been looked at.

## Build plan

Framework: ESP-IDF v5.5.6 with Espressif's `usb_device_uac` component for audio and TinyUSB's CDC class used directly for serial; `esp_tinyusb` is not used (see [USB device](#usb-device)). Arduino is the fallback if descriptor work gets easier there. Each milestone is testable on its own.

**Milestone 0: groundwork, no ESP32 board needed**

1. Done: `cat_civ` and `rig_ic705` as portable C: bytes in, reply bytes out, PTT change callback, with the personality interface in place from the start.
2. Done: host test harness. `make -C host test` runs the unit tests; `make -C host rigctl` runs real `rigctl -m 3085` against the parser over a pseudo-terminal.
3. Done: `audio_dsp` as portable C with host unit tests: 48↔12 kHz decimator and interpolator, noise at SNR, level meter, VOX detector, FIFO with drift steering. `make -C host test` runs them.
4. Done: ESP-IDF v5.5.6 installed and the project scaffolded: target `esp32s3`, 16 MB flash, octal PSRAM, console on the UART jack, the three portable modules as components, and a boot self-check in `main/`. It builds; it has not been flashed.

**Milestone 1: USB composite + CAT + loopback**

5. Written, not yet on hardware: descriptors for UAC2 + CDC-ACM within the endpoint budget, built on Espressif's `usb_device_uac` component. They pass `host/tools/check_usb_descriptors.py`.
6. Enumeration check on macOS, Linux and Windows.
7. Written, not yet on hardware: CDC wired to the parser. Line-state changes are logged only; line keying waits for the selectable PTT mode.
8. Written, not yet on hardware: audio loopback, USB OUT through a drift-steered FIFO (about 20 ms) to USB IN. It passes audio regardless of PTT, so a round trip can be checked with any recorder; muting receive while keyed belongs to parrot and link modes.
9. Acceptance: `rigctl -m 3085 -r /dev/ttyACM0 -vvvv`, then `f`, `F 7074000`, `m`, `T 1`, `T 0`, with no Hamlib errors; audio round-trips in Audacity or between two fldigi or WSJT-X instances.

**Milestone 2: parrot + channel simulation**

10. PSRAM parrot buffer at 12 kHz: record on PTT on, replay on PTT off after a configurable delay.
11. Channel on the receive path: noise at SNR, attenuation, TX→RX turnaround delay.
12. Mock-only controls on their own CI-V address, checked against the IC-705 and G90 command sets and Hamlib for collisions.
13. VOX PTT and LED states (red recording, green playing).
14. Optional flash persistence for the last transmission and canned clips.
15. Acceptance: WSJT-X FT8 self-decode with replay aligned to the 15 s slot.

**Milestone 3: two-board link**

16. Link interface (send frame, receive frame, exchange frequency and PTT); loopback moved behind it.
17. ESP-NOW backend: 12 kHz 16-bit frames with sequence numbers, board pairing.
18. Jitter buffer and drift correction.
19. Frequency matching.
20. UART backend.
21. Acceptance: two PCs running fldigi or VARA first, then two Android phones running Data2G in ARQ mode, stepping the SNR down to watch speed adaptation.

**Milestone 4: G90 personality (later pass)**

22. Run-time personality selection as a stored setting, switched by a mock-only control.
23. `rig_g90` from Hamlib's `xiegu.c` and the Radioddity CAT manual; test with `rigctl -m 3088 -s 19200 -vvvv` on the host, against more than one Hamlib version because the default address has changed.
24. When the real G90 and DE-19 are available: capture the DE-19's USB descriptors and a `rigctl` session against the radio, and reconcile the personality with them.

## Decision log

| Date | Decision | Why |
| --- | --- | --- |
| Nov 19, 2026 | ~~Emulate Kenwood TS-480 CAT (Hamlib 2028).~~ Superseded the same day. | Was chosen for its plain ASCII protocol and wide support. |
| Nov 19, 2026 | ~~Emulate a Xiegu G90 behind a DE-19 (CI-V, Hamlib 3088) as the only target.~~ Moved to a later pass the same day. | It is the operator's real station, but it is not accessible during the project, so there is no reference traffic. |
| Nov 19, 2026 | First pass targets the Icom IC-705 (Hamlib 3085); the G90 follows as a swap-in personality. | Official CI-V reference, stable Hamlib backend, the same USB shape as the mock (sound card plus CDC-ACM), and a radio other products already imitate. |
| Nov 19, 2026 | Split CAT into a shared `cat_civ` core and per-radio personality modules. | Both radios speak CI-V; keeps the G90 an addition rather than a rewrite. |
| Nov 19, 2026 | Answer CI-V frames at any radio address. | Hamlib's default G90 address differs by version (`A4` in 4.7.1, `88` in master, `70` reported), and 4.7.1 uses `A4` for the IC-705 too. |
| Nov 19, 2026 | All-digital audio; no codec, no RF. | No extra parts; USB-only build. |
| Nov 19, 2026 | ESP-IDF with `esp_tinyusb`. (The `esp_tinyusb` part is superseded by the next entry; ESP-IDF stands.) | Custom composite descriptors; Arduino kept as fallback. |
| Nov 19, 2026 | Build the sound card on Espressif's `usb_device_uac` component and the serial port on TinyUSB's CDC class, without `esp_tinyusb`. | The audio component is vendor-tested UAC2 handling for the S3; `esp_tinyusb` has no audio class. |
| Nov 19, 2026 | Keep asynchronous playback with a feedback endpoint. | It is what the tested component does, and it fits: four IN endpoints of four. |
| Nov 19, 2026 | Use the pid.codes test VID/PID for development. | Private bench device; no allocated ID, and a real maker's ID would be impersonation. |
| Nov 19, 2026 | Pin ESP-IDF to v5.5.6 rather than the newer 6.x line. | The USB references this design leans on are written against 5.5; revisit once the USB device works. |
| Nov 19, 2026 | One audio function and one CDC port only. | S3 endpoint budget. |
| Nov 19, 2026 | Control-line PTT is a selectable mode, off by default. | Hosts hold RTS and DTR high on open; found in Data2G's source. |
| Nov 19, 2026 | Keep the CDC notification endpoint; device class 0xEF/2/1 with a CDC IAD. | Required by usb-serial-for-android's CDC-ACM driver. |

## Open questions

- [x] Which commands Hamlib's IC-705 backend sends. With Hamlib 4.7.1: `03`, `25 00`, `14 0C`, `26 00`, `1A 03`, `0F` and `1C 00` on open and for reads; `25`/`26` for frequency and mode sets, including the unselected VFO for split; `07` for VFO select; `14`/`15`/`16` for levels, meters and functions. It sent no `1A 05` items and no `19 00`.
- [x] Does selecting IC-7300 (model 3073) also work against the IC-705 personality? Yes for frequency, mode and PTT with Hamlib 4.7.1; the G90 model (3088) worked for the same commands too. Checked once by hand, not part of the automated check.
- [ ] Do WSJT-X, fldigi/flrig and VARA accept the personality? They send commands `rigctl` was not asked to send here. (after step 9, or earlier through the pty)
- [x] Which CI-V commands FT8CN sends an IC-705. `26 00`, `05`, `1A 05 01 19`, `1C 00`, `15 12` and `15 13`; see [Other Android clients: FT8CN](#other-android-clients-ft8cn). Read from source and replayed on the host, not run on a phone.
- [ ] Should the IC-705 personality accept `1A 05 01 19` (data modulation input) and answer `FB`? FT8CN sends it on every PTT on and ignores the `FA` it gets now; a client that checks replies would not. Left as is on Nov 19, 2026.
- [ ] Does FT8AF (or FT8CN) work end to end against the mock on a phone, and does it show a decode of its own replayed transmission in parrot mode? (after step 9 and step 10)
- [ ] Which CI-V address and command numbers to use for mock-only controls (step 12)
- [ ] Default reference bandwidth for the SNR setting: 2500 Hz (WSJT-X) or 3000 Hz (VARA, channel simulators)? Whatever Data2G reports its SNR in would be the natural choice. (step 11)
- [ ] G90, later pass: exact replies (BCD byte order, mode bytes for U-D and L-D, ID bytes for `19 00` and `1D 19`, behaviour on unknown commands, echo), and whether to support `25`/`26` (steps 23–24)
- [ ] G90, later pass: what the real DE-19 presents over USB (VID/PID, one or two COM ports, audio format, device name), and whether it keys the G90 from RTS or DTR or only by CAT. One setup guide says to hold both lines low with PTT set to CAT. (step 24)
- [ ] How control-line PTT mode is selected and stored (mock-only CAT command saved to flash, the BOOT button, or both)
- [x] Exact S3 IN-endpoint limit, and whether duplex UAC2 with a feedback endpoint plus CDC fits. Four IN endpoints besides EP0; it fits exactly, with FIFO space to spare.
- [ ] Does each host (macOS, Windows, Linux, Android) enumerate the device and stream audio both ways? Structure is checked; behaviour is not. (step 6)
- [ ] Audio latency: the audio component moves capture audio in 10 ms blocks and the loopback FIFO holds about 20 ms. Is that acceptable for ARQ turnaround, or do the intervals need tightening? (after step 9)
- [ ] USB VID/PID if a board ever leaves the bench
- [ ] Does the S3 composite device enumerate cleanly on both test phones? (after step 9)
- [x] How does Data2G on Android key PTT? VOX, CAT, DTR or RTS, chosen in its rig pane.
- [x] Does Data2G on Android support network rig control? Yes: "Network (rigctld or ser2net)".

## References

No existing project does exactly this (checked Nov 2026); these are the closest starting points.

| Reference | Why it matters |
| --- | --- |
| [reSpeaker ESP32-S3 USB bridge (Seeed)](https://wiki.seeedstudio.com/respeaker_xvf3800_i2s_usb_bridge/) | Custom TinyUSB descriptors fitting duplex audio + CDC into the S3's endpoint/FIFO budget; best descriptor template |
| [Espressif usb_device_uac component](https://components.espressif.com/components/espressif/usb_device_uac/versions/1.3.2/examples/usb_uac) | Official UAC device building block for ESP32-Sx |
| [finger563/esp-usb-audio-example](https://github.com/finger563/esp-usb-audio-example) | S3 as a duplex USB headset (speaker + mic) |
| [Elektor: Xiegu X6200 USB controller](https://www.elektormagazine.com/labs/digital-radio-controller-via-usb-c-otg-audio-data-and-power) | Ham-specific UAC2 + CDC CAT work on an S3, host and device modes |
| [ESP-IDF USB Device Stack docs](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/peripherals/usb_device.html) | Composite devices, endpoint limits, "USB" jack wiring |
| Icom IC-705 CI-V Reference Guide (PDF from Icom's download site; mirrored on [ManualsLib](https://www.manualslib.com/manual/2012725/Icom-Ic-705.html)) | Official command table and data formats for the first-pass personality |
| Hamlib source: `rigs/icom/` (IC-705 backend) and [`simulators/simic705.c`](https://github.com/Hamlib/Hamlib/blob/master/simulators/simic705.c) | What Hamlib sends an IC-705 and expects back; the simulator is a hint only |
| Hamlib source: [`rigs/icom/xiegu.c`](https://github.com/Hamlib/Hamlib/blob/master/rigs/icom/xiegu.c) and [`simulators/simxiegug90.c`](https://github.com/Hamlib/Hamlib/blob/master/simulators/simxiegug90.c) | What Hamlib sends a G90 and expects back; the simulator is only a partial reference |
| Radioddity "Xiegu G90 CAT control and digital modes" manual v1.0 (2021-06-23) | The CAT command source Hamlib's backend cites |
| [G90 digital modes with the DE-19 (user write-up)](https://hmdsamarasekara.github.io/posts/xiegu-g90-digital-modes/) | DE-19 cabling, serial settings and G90 menu settings; reports a CH342 chip |
| [Data2G](https://github.com/arodland/Data2G/) | Target modem; Android USB code under `native/core/audio/android`, `native/core/rig/android`, `native/android-app` |
| [FT8CN](https://github.com/N0BOY/FT8CN) and its maintained fork [FT8AF](https://github.com/patrickrb/FT8AF) | Android FT8 apps with their own CI-V code; Icom commands in `rigs/IcomRig.java` and `rigs/IcomRigConstant.java` |

Side project for the spare boards: ESP-SDR (ESPARGOS) and esp32-sdr-trx (DA2JH) turn an S3's Wi-Fi radio into a 2.4 GHz receiver / 13 cm transceiver.
