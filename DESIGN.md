# ESP32-S3 Mock Radio — Design

Living document. Edit it in place as the design changes: update the section a fact belongs to, add a dated line to the decision log, and tick or add open questions. [HANDOFF.md](HANDOFF.md) is the original Nov 19, 2026 handoff, kept as a frozen snapshot.

Operator: Tim, W0SIG (General).

## Status

Last updated: Nov 19, 2026.

- The CI-V core (`cat_civ`) and the IC-705 personality (`rig_ic705`) exist as portable C and pass real Hamlib on the host: `rigctl -m 3085` (Hamlib 4.7.1) over a pseudo-terminal, 16 command groups, no command rejected. The commands the Android FT8 apps FT8CN and FT8AF send an IC-705 were read from their source and all get correct replies on the host; neither app has been run.
- The audio DSP (`audio_dsp`) exists as portable C with host unit tests: resampler, channel noise and attenuation, level meter, VOX detector, and a FIFO with clock-drift steering.
- The receive channel of Milestone 2 (`audio_engine`) exists as portable C with host unit tests: a fixed noise floor with the signal held at a set SNR, attenuation, and receive mute while keyed plus a turnaround delay. It is wired into the firmware's audio path; the power-up settings are build-time and the default is still a plain loopback.
- The parrot (step 10) exists in `audio_engine` as a third receive route, with host unit tests: it records each transmission at 12 kHz and replays it after unkeying, after a delay or aligned to a slot period, to the sample. The firmware gives it a buffer in PSRAM. Not run on a board.
- The mock's own controls (`mock_ctl`, step 12) exist as portable C with host unit tests: CI-V frames to the private address `F0` read and change the route, the channel settings and the parrot's settings at run time, replay the parrot's recording, and read the engine's status. `host/tools/mockctl.py` sends them; it is checked against the parser over a pseudo-terminal, not against a board.
- The firmware builds for the ESP32-S3 (ESP-IDF v5.5.6, 251 KB image) with the whole of Milestone 1 written: the USB composite device (sound card plus CAT serial port), the serial port wired to the CAT parser, and audio loopback. Its descriptors pass a structural check against the S3's limits and Android's requirements. None of it has run on hardware: whether hosts enumerate it and stream audio is unknown until a board is flashed.
- Nothing has run on an ESP32 yet, and no modem application has been tried against any of it.
- The first pass emulates an Icom IC-705. A Xiegu G90 personality (the operator's own radio, used through a DE-19) is planned as a later swap-in module; the G90 and DE-19 are not accessible during this project.
- Next step: flash a board and work through Milestone 1, steps 6 and 9 (enumeration on each host, then the `rigctl` and audio acceptance tests). Milestone 2 is under way meanwhile: the parrot (step 10), the channel (step 11) and the mock-only controls (step 12) are written; VOX and the LED states (step 13) remain.
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
- `audio_engine`: transmit audio in, receive audio out, using `audio_dsp`. The route picks what the receiver hears (the transmit audio, nothing, or the parrot's replay from PSRAM); the channel then applies mute while keyed, turnaround delay, noise at a set SNR and attenuation. The link will join as a further route
- `mock_ctl`: the mock's own controls, CI-V frames to address `F0`: route, channel settings, parrot settings and status now; link mode, PTT source and personality later
- `link`: one interface with loopback, ESP-NOW and UART backends; jitter buffer and drift correction
- `ui`: RGB LED states; a Wi-Fi status page later if wanted

`cat_civ`, the personalities, `audio_dsp`, `audio_engine` and `mock_ctl` are written as portable C with no ESP-IDF dependencies, so they can be tested on the host.

Runtime: FreeRTOS, as bundled with ESP-IDF (the S3 runs its dual-core SMP build). No separate RTOS choice is needed; the Arduino fallback runs on the same kernel. TinyUSB, the audio engine and the link layer each get a task; `cat_civ` is passive and runs in whichever task delivers serial bytes.

Repository layout:

| Path | Contents |
| --- | --- |
| `components/cat_civ/` | CI-V core |
| `components/rig_ic705/` | IC-705 personality |
| `components/audio_dsp/` | Audio DSP |
| `components/audio_engine/` | Receive routes (monitor, radio, parrot), the parrot's recording and replay, and the receive channel: mute and turnaround, SNR, attenuation |
| `components/mock_ctl/` | The mock's own controls on CI-V address `F0` |
| `components/usb_composite/` | USB device: descriptors, TinyUSB configuration, serial and audio callbacks |
| `main/` | Firmware entry point: boot self-check, then CAT and audio wiring; `Kconfig.projbuild` holds the power-up route, channel and parrot settings |
| `CMakeLists.txt`, `sdkconfig.defaults` | ESP-IDF project file and board configuration (16 MB flash, octal PSRAM, UART console) |
| `host/` | Host-only build: `civ_pty` (parser behind a pseudo-terminal), `test_civ`, `test_dsp`, `test_engine` and `test_ctl` (unit tests), `rigctl_check.sh` (real Hamlib acceptance check), `tools/mockctl.py` (reads and changes the mock's settings over its CAT port) with `mockctl_check.sh` (its check over the pseudo-terminal), `tools/gen_resampler_fir.py` (filter generator), `tools/check_usb_descriptors.py` (descriptor check on the built firmware) |

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
- **Personality (`rig_ic705`, later `rig_g90`):** a table of what makes one radio different: default address, transceiver ID reply, mode-byte table, which commands and sub-commands exist, frequency coverage and transmit bands, power limit, level and meter defaults, the `1A 05` menu settings it has, power-up state, and handlers for radio-specific commands.

The core never names a radio. Anything radio-specific goes through the personality interface, so adding the G90 means adding one module rather than editing the core. The first pass selects the personality at build time; run-time selection (a stored setting) arrives with the G90. The CI-V address cannot be used to pick the personality, because Hamlib 4.7.1 addresses both radios at `A4`.

### Framing

Binary frames, not ASCII:

```
command:  FE FE <radio addr> <controller addr> <cmd> [<sub>] [<data>...] FD
data:     FE FE <controller addr> <radio addr> <cmd> [<sub>] <data>...   FD
OK:       FE FE <controller addr> <radio addr> FB FD
error:    FE FE <controller addr> <radio addr> FA FD
```

**Address rule: answer whatever radio address the frame is sent to, and reply from that same address.** The controller address is normally `E0`. Measured with Hamlib 4.7.1 on the development Mac: the IC-705 backend opens with `FE FE A4 E0 03 FD`, the IC-7300 backend with `FE FE 94 E0 03 FD` followed by `25 00`, and the G90 backend with `FE FE A4 E0 03 FD`. Accepting any address keeps the mock independent of the host's Hamlib version and lets closely related models be tried. The one exception is `F0`, which the mock keeps for its own controls (see [Mock-only controls](#mock-only-controls)).

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
| `1A 05 <item>` | Menu settings | Stored value for the items in the personality's table; error otherwise. Listed so far: `01 19` (DATA MOD: `00` MIC, `01` USB, `02` MIC,USB, `03` WLAN), starting on USB |
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

Settings that belong to the mock, not to the radio it imitates, travel as CI-V frames on the same serial port, sent to an address the mock keeps for itself: **`F0`**. Frames to `F0` go to `mock_ctl` and never reach the personality; every other address is still the radio. Because the split is by address, the command numbers below are a space of their own and cannot collide with any radio's commands, whichever personality is loaded.

Why `F0`: in Hamlib 4.7.1's Icom backends the default radio addresses run from `01` to `B6`, one receiver (the Perseus) sits at `E1`, and the controller is `E0`. `FA` to `FF` are protocol bytes. Nothing uses `F0`. The core itself does not know the address; `cat_civ` only offers a hook that takes one address away from the radio (`civ_set_private`), and the firmware gives it `F0`.

```
command:  FE FE F0 <controller> <cmd> [<data>...] FD
read:     FE FE <controller> F0 <cmd> <data>... FD
set OK:   FE FE <controller> F0 FB FD
refused:  FE FE <controller> F0 FA FD
```

A command with no data reads; with data it sets. Numbers are BCD, most significant byte first, like the radio's `14` levels: 12.5 dB is `01 25`, in tenths. A signed number has one more byte, `00` for plus and `01` for minus. A value outside its range is refused with `FA`, not clamped.

| Command | Meaning | Data |
| --- | --- | --- |
| `00` | Identity, read only | Protocol version (`01`), then the personality's Hamlib model in 2 bytes (`30 85`) |
| `01` | Put every setting back to its power-up value | None |
| `10` | Receive route | `00` monitor, `01` radio, `02` parrot (refused if the parrot has no buffer) |
| `11` | Noise | `00` off, `01` on |
| `12` | SNR | Tenths of a dB, 2 bytes plus sign: −60.0 to +80.0 |
| `13` | Bandwidth the SNR is quoted in | Hz, 3 bytes: 100 to 24000 |
| `14` | Noise floor | Tenths of a dB below full scale, 2 bytes: 12.0 to 90.0 |
| `15` | Attenuation | Tenths of a dB, 2 bytes: 0 to 120.0 |
| `16` | Turnaround time | ms, 2 bytes: 0 to 5000 |
| `20` | Status, read only | Flags (`01` receive muted, `02` gain limited); level of the receive source in tenths of a dB below full scale, 2 bytes (`99 99` if nothing has been measured); gain the channel applies, tenths of a dB, 2 bytes plus sign |
| `30` | Play the parrot's recording again, now | None. Refused unless the route is parrot, there is a recording and PTT is off |
| `32` | Parrot delay, from unkeying to the replay | Tenths of a second, 2 bytes: 0 to 60.0 |
| `33` | Parrot period: the replay starts a whole number of periods after key-on | Tenths of a second, 2 bytes: 0 (off) to 300.0 |
| `34` | Parrot status, read only | State (`00` idle, `01` recording, `02` waiting, `03` playing); flags (`01` the buffer filled and the recording is cut short); length recorded and buffer capacity, each in tenths of a second, 2 bytes |
| `31`, `40`, `50`, `60` | Reserved: play stored clip N, link mode, PTT source, personality | Refused until those parts exist |

Example: `FE FE F0 E0 12 01 25 01 FD` sets the SNR to −12.5 dB and is answered `FE FE E0 F0 FB FD`.

`host/tools/mockctl.py` sends these from a PC (Python 3 only; it uses pyserial if installed, which Windows needs):

```
mockctl.py -p PORT                          show every setting and the status
mockctl.py -p PORT noise=on snr=-12.5       change settings, then show them
mockctl.py -p PORT reset                    back to the power-up settings
mockctl.py -p PORT route=parrot period=15   parrot route, replay one FT8 slot later
mockctl.py -p PORT replay                   play the parrot's recording again
```

Limits, as built:

- Settings live in RAM. A power cycle returns to the build-time values (menuconfig); `01` returns to them without one.
- The controls share the CAT port, so they can only be sent while no other program is using it, or by the program that is. A reply read by a second program on the same port is lost to the sender and may confuse the other. A phone app holding the port leaves no way in at all; see the open questions.
- `rigctl`'s `w` command sends a frame to `F0` (seen in the pseudo-terminal log) but printed no reply with Hamlib 4.7.1, so it can set but not usefully read.
- Verified on the host only: unit tests through the real parser and engine, and `mockctl.py` against the parser over a pseudo-terminal. Not run on a board.

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

As built in `audio_engine` (host-tested, not yet run on a board):

- **A third route.** *Parrot* is the radio route with a receive source: receive is silent while keyed and for the turnaround time, and the replay is what the receiver then hears. Select it with the route control or at build time.
- **Recording.** From key-on until 50 ms after unkeying, to catch audio still in the USB buffers when PTT drops. Audio is stored at 12 kHz in a PSRAM buffer allocated at start-up: 120 s by default (2.9 MB), 5 to 300 s in menuconfig. Silence is recorded as silence, so the recording is a timeline from key-on. Keying again replaces the recording, whether the last one was waiting, playing or finished.
- **Not a ring.** When the buffer fills, recording stops and the start is kept; the parrot's status flags the recording as cut short. Keeping the start keeps the replay's timing, which counts from key-on.
- **Replay.** Once, starting the set delay after unkeying (default 1 s, never less than the 50 ms tail). It passes through the channel like any receive source, so noise, SNR and attenuation apply; the level measurement starts afresh with each replay. The replay control plays the recording again at once.
- **Slot alignment.** With a period set, the replay waits for a whole number of periods after key-on. Key-on is the one moment the board shares with the host's clock, so this puts the audio at the same place in a later slot without the board knowing the time: 15 s for FT8, 7.5 s for FT4. The resampler's delay (92 samples) is taken out by skipping the first 23 stored samples, and the host test confirms a sample comes back exactly one period after it went in. What remains is the loopback FIFO (about 20 ms) and the host's own audio latency.
- **Capture must be running.** Recording and replay both run on the capture side, on the board's sample clock. The parrot records only while the host is also recording from the device; a host that stops capturing while it transmits would record nothing. See the open questions.
- **PTT timing.** PTT is sampled once per audio block (10 ms at most), so key-on and unkeying are seen up to a block late. Alignment does not suffer: the recording and the replay count from the same block edge.

**Resampling.** One 96-tap low-pass serves both directions: flat within 0.01 dB to 4 kHz, 3 dB down at 5 kHz, and at least 69 dB down from 6.5 kHz, so the 12 kHz path carries roughly 0–4.5 kHz. A 48 → 12 → 48 kHz round trip delays audio by 92 samples at 48 kHz, about 1.9 ms. Coefficients come from `host/tools/gen_resampler_fir.py`.

**SNR convention.** Noise is white across the whole sampled band, and SNR is quoted in a reference bandwidth, as HF software does: 2500 Hz for WSJT-X, 3000 Hz for VARA and most channel simulators. The mock's SNR setting defaults to 2500 Hz, which is also what Data2G reports its SNR in (`SNR_REF_BW_HZ` in its source); the bandwidth is a setting.

**Channel simulation (applied on the receive side)**

- Additive noise at a configurable SNR, so ARQ modems step down through their speed levels.
- Attenuation, and optionally fading or a small frequency offset (PathSim-style), later.
- TX→RX turnaround delay of a few tens of ms, to mimic a real radio switching.

As built in `audio_engine` (host-tested, not yet run on a board):

- **The noise floor is fixed and the signal is moved.** Noise sits at a set level (default −40 dBFS RMS across the whole 0–24 kHz band) whether or not a signal is present, as band noise does. The engine measures the level of the arriving signal and scales it to sit at the configured SNR, so the SNR comes out as set whatever level the sending modem drives at. Deriving the noise from the signal instead would clip at negative SNRs: a signal at −10 dBFS and −20 dB SNR would need noise far above full scale.
- **Limits.** The signal is never raised above −12 dBFS RMS; an SNR too high for the floor (above about +38 dB with the default floor) lowers the floor instead. A quiet signal is amplified by at most 30 dB; beyond that the SNR comes out lower and the engine's status says so. Measured on the host, the SNR lands within 0.1 dB of the setting from −30 to +25 dB.
- **Level measurement.** The signal level is the mean square of its loud parts, measured in 1 ms pieces (anything under −60 dBFS is ignored), as a plain average for the first second and a running average over about a second after that. 200 ms of silence ends a transmission and the next one is measured afresh. The gain therefore settles within the first few milliseconds of a transmission and moves little after.
- **Attenuation** applies on top: with noise off it is a plain loss; with noise on it lowers the signal, and so the SNR, by that many dB.
- **Routes.** *Monitor*: receive is always open and PTT is ignored; with noise off and no attenuation this is the Milestone 1 loopback, bit for bit. *Radio*: receive is silent while keyed and for the turnaround time after unkeying (default 30 ms), then carries the receive source through the channel. PTT is sampled once per audio block, so the turnaround can run up to 10 ms long. The radio route has no receive source, so the host hears the noise floor only. *Parrot* behaves like radio and adds the replay as its source.
- **Settings:** route, noise on/off, SNR, reference bandwidth, noise floor, attenuation, turnaround time, and the parrot's delay and period. Their power-up values are build-time (`idf.py menuconfig`, "Mock radio: receive route and channel at power-up"); the [mock-only controls](#mock-only-controls) change them at run time, and the engine takes a change at the next audio block.
- **Precision.** The channel's gain has 24 fractional bits and signal and noise are summed before rounding, so a signal of a few counts at −30 dB SNR keeps its level and is dithered by the noise.

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
| Every PTT on | `1A 05 01 19 01` | Set the data modulation input to USB (`02` over Bluetooth, `03` over WLAN) | `FB` |
| PTT on, CAT mode only | `1C 00 01` | Transmit | `FB` |
| Every 500 ms while keyed | `15 12` | Read SWR meter | `15 12 00 00` |
| Every 500 ms while keyed | `15 13` | Read ALC meter | `15 13 00 00` |
| PTT off, CAT mode only | `1C 00 00` | Receive | `FB` |

- It never checks `FB` or `FA`; "connected" means the serial port opened. The mock first answered `1A 05 01 19` with `FA`, which did no harm for that reason; it now stores the value and answers `FB`, as the IC-705's CI-V reference describes (read from a ManualsLib mirror, not Icom's own PDF). The value changes nothing in the mock, whose only audio input is USB.
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

10. Written, not yet on hardware: PSRAM parrot buffer at 12 kHz in `audio_engine`, as a third route: record on PTT on, replay on PTT off after a configurable delay or aligned to a slot period, with host tests.
11. Written, not yet on hardware: channel on the receive path in `audio_engine`: noise at SNR, attenuation, receive mute while keyed and TX→RX turnaround delay, with host tests. Power-up settings are build-time; step 12 changes them at run time.
12. Written, not yet on hardware: mock-only controls in `mock_ctl` on CI-V address `F0`, which no radio in Hamlib 4.7.1 uses; a separate address keeps them clear of every radio's command set. Route, channel settings, parrot settings, replay and status so far, with host tests and `host/tools/mockctl.py`. Stored-clip, link, PTT-source and personality commands have numbers reserved and join with those parts.
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
| Nov 19, 2026 | `1A 05` menu settings are a table in the personality (item number, highest value, power-up value) handled by the core; unlisted items answer `FA`. Items are added when a client is seen to send them. | Item numbers differ between Icom models, so they belong to the personality; a table keeps the personality constant data. First item: `01 19` for FT8CN and FT8AF. |
| Nov 19, 2026 | Channel SNR: keep the noise floor fixed and scale the signal to the set SNR, measuring the signal's level. | The SNR comes out as set whatever the modem's drive level, and negative SNRs do not clip. |
| Nov 19, 2026 | SNR is quoted in 2500 Hz by default. | Data2G and WSJT-X both use 2500 Hz. |
| Nov 19, 2026 | Two receive routes: monitor (always open, PTT ignored) and radio (mute while keyed plus turnaround). Monitor with noise off is the power-up default. | Loopback testing needs receive open during transmit; parrot and link need radio behaviour. The default keeps Milestone 1's acceptance test unchanged. |
| Nov 19, 2026 | Channel settings are build-time (menuconfig) until the mock-only CAT controls exist. | Lets the channel be tried on a board before step 12 without inventing a temporary control path. |
| Nov 19, 2026 | Mock-only controls are CI-V frames to a private address, `F0`, handled by a separate module (`mock_ctl`) through a one-address hook in the core. | No extra USB endpoint is needed; an address of its own means no command number can collide with a radio's, for any personality; `F0` is used by no radio or controller in Hamlib 4.7.1. The core stays radio-neutral and does not know what the controls are. |
| Nov 19, 2026 | Control values are BCD in tenths of a dB (Hz and ms as whole numbers), most significant byte first, with a sign byte where needed; out-of-range values are refused with `FA`. | Reads like the radio's own frames in a log; a refusal tells the sender its value did not take, where clamping would hide it. |
| Nov 19, 2026 | Run-time settings are not stored; the build-time values are the power-up values and control `01` restores them. | Persistence is step 14; a bench device should start in a known state. |
| Nov 19, 2026 | The parrot is a third receive route in `audio_engine`, behind one entry point that takes transmit audio and returns receive audio. | One selector says what the receiver hears, so there is no "parrot on in the monitor route" to define, and the firmware glue no longer picks the source. The link can join as a fourth route. |
| Nov 19, 2026 | The parrot records on the capture side, in the same task that replays, not in the USB playback callback. | One task and one sample clock: no locking, replay timing counted in samples, and host tests that are exact. The cost is that the host must be capturing while it transmits. |
| Nov 19, 2026 | The parrot's buffer keeps the start of a transmission when it fills (not a ring), at 12 kHz, 120 s by default. | The replay's timing counts from key-on, which a ring would lose. 120 s holds a WSPR transmission; 12 kHz quarters the memory and keeps 0–4.5 kHz. |
| Nov 19, 2026 | Replay timing is a delay after unkeying plus an optional period counted from key-on, with the resampler's delay taken out. | FT8 self-decode needs the audio back at the same place in a later slot. The board has no clock to know slot boundaries, but key-on comes from the host's clock. |
| Nov 19, 2026 | The parrot's delay and period travel in tenths of a second. | Two BCD bytes reach 300 s, within the core's 16-bit BCD helpers, and 7.5 s (FT4) is still exact. |

## Open questions

- [x] Which commands Hamlib's IC-705 backend sends. With Hamlib 4.7.1: `03`, `25 00`, `14 0C`, `26 00`, `1A 03`, `0F` and `1C 00` on open and for reads; `25`/`26` for frequency and mode sets, including the unselected VFO for split; `07` for VFO select; `14`/`15`/`16` for levels, meters and functions. It sent no `1A 05` items and no `19 00`.
- [x] Does selecting IC-7300 (model 3073) also work against the IC-705 personality? Yes for frequency, mode and PTT with Hamlib 4.7.1; the G90 model (3088) worked for the same commands too. Checked once by hand, not part of the automated check.
- [ ] Do WSJT-X, fldigi/flrig and VARA accept the personality? They send commands `rigctl` was not asked to send here. (after step 9, or earlier through the pty)
- [x] Which CI-V commands FT8CN sends an IC-705. `26 00`, `05`, `1A 05 01 19`, `1C 00`, `15 12` and `15 13`; see [Other Android clients: FT8CN](#other-android-clients-ft8cn). Read from source and replayed on the host, not run on a phone.
- [x] Should the IC-705 personality accept `1A 05 01 19` (data modulation input) and answer `FB`? Yes, added Nov 19, 2026. Its power-up value is USB because that is the mock's only input; the real radio's factory default was not checked.
- [ ] Does FT8AF (or FT8CN) work end to end against the mock on a phone, and does it show a decode of its own replayed transmission in parrot mode? (after step 9 and step 10)
- [x] Which CI-V address and command numbers to use for mock-only controls. Address `F0`; numbers in [Mock-only controls](#mock-only-controls). Decided Nov 19, 2026.
- [ ] Do hosts keep capturing while they transmit (WSJT-X, fldigi, VARA, FT8CN and FT8AF, Data2G on Android)? The parrot records on the capture side and gets nothing from one that does not. If one is found, recording moves to the USB playback side. (after step 9)
- [ ] FT8 self-decode with the parrot's period at 15 s: when does WSJT-X key relative to the slot start, and how far from zero is the DT it reports for its own replay? (step 15)
- [ ] How to change the mock's settings while a modem application holds the CAT port (always the case on a phone, and with WSJT-X on a PC). A line console on the UART jack driving the same settings is the likely answer; it needs no USB endpoint. (before step 15)
- [ ] Do the mock-only controls work on a board, and does a setting change mid-stream sound clean? Host-tested only. (after step 9)
- [x] Default reference bandwidth for the SNR setting: 2500 Hz. Data2G reports its SNR in 2500 Hz (`SNR_REF_BW_HZ` in `native/core/generated/config.hpp`), as does WSJT-X; 3000 Hz remains a setting for VARA.
- [ ] Are the channel defaults right for real modems: noise floor −40 dBFS, turnaround 30 ms, and silence (not a low noise floor) on receive while keyed? To be judged with WSJT-X, FT8AF and Data2G on a board. (after step 9)
- [ ] Should the S-meter (`15 02`) follow the receive level the engine produces? `civ_set_meter` exists for it. (step 13)
- [ ] G90, later pass: exact replies (BCD byte order, mode bytes for U-D and L-D, ID bytes for `19 00` and `1D 19`, behaviour on unknown commands, echo), and whether to support `25`/`26` (steps 23–24)
- [ ] G90, later pass: what the real DE-19 presents over USB (VID/PID, one or two COM ports, audio format, device name), and whether it keys the G90 from RTS or DTR or only by CAT. One setup guide says to hold both lines low with PTT set to CAT. (step 24)
- [ ] How control-line PTT mode is selected and stored (mock-only CAT command saved to flash, the BOOT button, or both). Control number `50` is reserved for it.
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
