# ESP32-S3 Mock Radio — Design

Living document. Edit it in place as the design changes: update the section a fact belongs to, add a dated line to the decision log, and tick or add open questions. [HANDOFF.md](HANDOFF.md) is the original Nov 19, 2026 handoff, kept as a frozen snapshot.

Operator: Tim, W0SIG (General).

## Status

Last updated: Nov 19, 2026.

- The CI-V core (`cat_civ`) and the IC-705 personality (`rig_ic705`) exist as portable C and pass real Hamlib on the host: `rigctl -m 3085` (Hamlib 4.7.1) over a pseudo-terminal, 16 command groups, no command rejected. The commands the Android FT8 apps FT8CN and FT8AF send an IC-705 were read from their source and all get correct replies on the host; neither app has been run.
- The audio DSP (`audio_dsp`) exists as portable C with host unit tests: resampler, channel noise and attenuation, level meter, VOX detector, and a FIFO with clock-drift steering.
- The receive channel of Milestone 2 (`audio_engine`) exists as portable C with host unit tests: a fixed noise floor with the signal held at a set SNR, attenuation, and receive mute while keyed plus a turnaround delay. It is wired into the firmware's audio path; the power-up settings are build-time and the default is still a plain loopback.
- The parrot (step 10) exists in `audio_engine` as a third receive route, with host unit tests: it records each transmission at 12 kHz and replays it after unkeying, after a delay or aligned to a slot period, to the sample. The firmware gives it a buffer in PSRAM. Not run on a board.
- PTT from a serial control line and from VOX (step 13) exists in `audio_engine` beside CAT keying, with host unit tests; both are off at power-up. A read of `1C 00` reports transmit whichever source keyed. The LED policy (`ui`) exists with host unit tests, and the firmware drives the board's RGB LED from it. Not run on a board.
- The mock's own controls (`mock_ctl`, step 12) exist as portable C with host unit tests: CI-V frames to the private address `F0` read and change the route, the channel settings, the parrot's settings and the PTT sources at run time, replay the parrot's recording, and read the engine's status. `host/tools/mockctl.py` sends them; it is checked against the parser over a pseudo-terminal, not against a board.
- The web console (Milestone 3, steps 16 to 19) is written. Its settings and telemetry (`console_api`) are portable C with host unit tests, sharing `mock_ctl`'s table of settings. The page has been tried in a desktop browser against a simulator that runs the same C code on the host. The Wi-Fi bring-up and the HTTP and WebSocket server have only been compiled: they have never run.
- The firmware builds for the ESP32-S3 (ESP-IDF v5.5.6, 966 KB image with the web console and the ESP-NOW link; 318 KB with the console off and the link on a UART, which leaves Wi-Fi out) with the whole of Milestone 1 written: the USB composite device (sound card plus CAT serial port), the serial port wired to the CAT parser, and audio loopback. Its descriptors pass a structural check against the S3's limits and Android's requirements. None of it has run on hardware: whether hosts enumerate it and stream audio is unknown until a board is flashed.
- Nothing has run on an ESP32 yet, and no modem application has been tried against any of it.
- The first pass emulates an Icom IC-705. A Xiegu G90 personality (the operator's own radio, used through a DE-19) is planned as a later swap-in module; the G90 and DE-19 are not accessible during this project.
- Next step: flash a board and work through Milestone 1, steps 6 and 9 (enumeration on each host, then the `rigctl` and audio acceptance tests). For that first bring-up the web console can be switched off in menuconfig, so that Wi-Fi is not a suspect. Milestone 2 has steps 10 to 13 written (parrot, channel, mock-only controls, VOX and line PTT with the LED states); the optional flash storage (step 14) and the FT8 acceptance test (step 15) remain. Milestone 3 has its acceptance test (step 20) left, which needs a board, and Milestone 4 its own (step 26), which needs two.
- The two-board link (Milestone 4, steps 21 to 25) is written. Its core (`link`: packets, jitter buffer, loss and clock-drift handling, peer discovery, frequency matching, framing for a wire) is portable C with host unit tests that join two boards by a simulated path with loss, jitter, reordering and clocks that differ. It is a fourth receive route in `audio_engine`, with two settings on both control paths and its statistics in the web console. Two simulated boards have been watched hearing each other in a browser. The ESP-NOW and UART transports have only been compiled: no packet has crossed between two boards.
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
- A two-board link (audio over ESP-NOW, wired UART as an option) so two phones running Data2G can hold an ARQ session over a fake radio path.
- A web console over Wi-Fi to change settings and watch telemetry while the boards run, without touching the USB connection the modem software is using.

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
| Status LED | Onboard RGB LED (WS2812 type), usually GPIO48; GPIO38 on the DevKitC-1 v1.1. On some clones the "RGB" solder pad must be bridged. The pin and brightness are menuconfig settings; see [PTT](#ptt) for what it shows. |
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
                                | Web console: settings and telemetry      | <---> Browser, over Wi-Fi
                                +------------------------------------------+
```

The host sees one USB device; inside, CAT state keys the audio engine, and the receive route decides what the receiver hears: the same host's transmit audio (monitor, or the parrot's replay) or, through the link, a second board's. The web console sits beside all of it: it reads and changes the same settings over Wi-Fi and never touches USB.

Firmware modules:

- `usb_composite`: descriptors and TinyUSB callbacks for UAC2 + CDC-ACM, including line-state changes
- `cat_civ`: CI-V framing, address handling, command dispatch and the radio state struct (VFO A/B, mode, split, PTT, levels)
- `rig_ic705`: the IC-705 personality (first pass); `rig_g90` joins it later behind the same interface
- `audio_dsp`: the signal processing itself, with no hardware dependencies: 48↔12 kHz resampling, noise and attenuation, level meter, VOX detector, FIFO with drift steering
- `audio_engine`: transmit audio in, receive audio out, using `audio_dsp`. The route picks what the receiver hears (the transmit audio, nothing, or the parrot's replay from PSRAM); the channel then applies mute while keyed, turnaround delay, noise at a set SNR and attenuation. In the link route the receiver hears a second board and the transmit audio goes to it; the engine only hands the audio over, through two functions the firmware supplies. It also combines the PTT sources: CAT, a selected control line, and the VOX it runs on the transmit audio
- `mock_ctl`: the mock's own controls, CI-V frames to address `F0`: route, channel settings, parrot settings, the link's settings, PTT sources and status now; personality later. It also offers the same settings by number to other control paths, which is how the web console shares its ranges
- `link`: the path between two boards: 12 kHz audio in 10 ms packets, a jitter buffer with loss and clock-drift handling, peer discovery and frequency matching, all portable; and one of two transports that carry the packets, ESP-NOW or a UART, chosen at build time. See [Two-board link](#two-board-link)
- `wifi_net`: Wi-Fi bring-up, shared by the web console and the ESP-NOW transport: joining or hosting a network, the board's name
- `web_console`: the settings and telemetry as JSON (`console_api`, portable), and around it Wi-Fi, a small HTTP server, the control page and a WebSocket for live telemetry; see [Web control and telemetry console](#web-control-and-telemetry-console)
- `ui`: which colour the RGB LED shows for the radio's state; the firmware drives the LED

`cat_civ`, the personalities, `audio_dsp`, `audio_engine`, `mock_ctl`, `ui`, the `console_api` part of `web_console` and the core of `link` are written as portable C with no ESP-IDF dependencies, so they can be tested on the host.

Runtime: FreeRTOS, as bundled with ESP-IDF (the S3 runs its dual-core SMP build). No separate RTOS choice is needed; the Arduino fallback runs on the same kernel. TinyUSB and the two directions of USB audio each get a task, and the audio engine runs in the capture one; `cat_civ` is passive and runs in whichever task delivers serial bytes. The web console's HTTP server has a task of its own at a lower priority than the USB and audio tasks. Because it changes the same settings as the CI-V controls, one mutex in `main/` is held around the CI-V parser and around each settings change from the console. The link is spread over three tasks without a lock: the audio task sends and plays, the transport's task (Wi-Fi's, or a UART reader) puts arriving packets into the jitter buffer, and the status task announces the board every 250 ms and copies the engine's link settings across.

Repository layout:

| Path | Contents |
| --- | --- |
| `components/cat_civ/` | CI-V core |
| `components/rig_ic705/` | IC-705 personality |
| `components/audio_dsp/` | Audio DSP |
| `components/audio_engine/` | Receive routes (monitor, radio, parrot), the parrot's recording and replay, and the receive channel: mute and turnaround, SNR, attenuation |
| `components/mock_ctl/` | The mock's own controls on CI-V address `F0` |
| `components/ui/` | LED policy: the state to show and its colour |
| `components/web_console/` | Web console: `console_api.c` (settings and telemetry as JSON, portable), `web_console.c` (HTTP and WebSocket server), `www/index.html` (the page, embedded in the firmware), `Kconfig` (the console switch) |
| `components/wifi_net/` | Wi-Fi: joining or hosting a network, the board's name, and its `Kconfig` settings |
| `components/link/` | Two-board link: `link.c` (portable core), `link_espnow.c` and `link_uart.c` (the transports; one is compiled), `Kconfig` (which one, and the UART's pins) |
| `components/usb_composite/` | USB device: descriptors, TinyUSB configuration, serial and audio callbacks |
| `main/` | Firmware entry point: boot self-check, then CAT and audio wiring, the status task and the web console's hookup; `status_led.c` drives the RGB LED through RMT; `Kconfig.projbuild` holds the power-up route, channel, parrot and PTT settings and the LED's pin |
| `CMakeLists.txt`, `sdkconfig.defaults` | ESP-IDF project file and board configuration (16 MB flash, octal PSRAM, UART console) |
| `host/` | Host-only build: `civ_pty` (parser behind a pseudo-terminal), `test_civ`, `test_dsp`, `test_engine`, `test_ctl`, `test_ui`, `test_console` and `test_link` (unit tests), `console_sim` with `tools/console_sim.py` (the web console in a browser with two simulated radios joined by the link, and a check of its interface), `rigctl_check.sh` (real Hamlib acceptance check), `tools/mockctl.py` (reads and changes the mock's settings over its CAT port) with `mockctl_check.sh` (its check over the pseudo-terminal), `tools/gen_resampler_fir.py` (filter generator), `tools/check_usb_descriptors.py` (descriptor check on the built firmware) |

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
| `10` | Receive route | `00` monitor, `01` radio, `02` parrot (refused if the parrot has no buffer), `03` link (refused if the mock was built without one) |
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
| `40` | Link buffer: audio held before it is played | ms, 2 bytes: 10 to 200 |
| `41` | Link frequency match | `00` off, `01` on: the other board is heard only when it transmits on this radio's receive frequency |
| `50` | Control-line PTT | `00` off, `01` RTS keys, `02` DTR keys |
| `51` | VOX | `00` off, `01` on |
| `52` | VOX threshold | Tenths of a dB below full scale, 2 bytes: 6.0 to 80.0 |
| `53` | VOX hang time | ms, 2 bytes: 0 to 5000 |
| `54` | PTT status, read only | The sources keying the radio now: `01` CAT, `02` control line, `04` VOX, added together; `00` is receiving |
| `31`, `60` | Reserved: play stored clip N, personality | Refused until those parts exist |

Example: `FE FE F0 E0 12 01 25 01 FD` sets the SNR to −12.5 dB and is answered `FE FE E0 F0 FB FD`.

`host/tools/mockctl.py` sends these from a PC (Python 3 only; it uses pyserial if installed, which Windows needs):

```
mockctl.py -p PORT                          show every setting and the status
mockctl.py -p PORT noise=on snr=-12.5       change settings, then show them
mockctl.py -p PORT reset                    back to the power-up settings
mockctl.py -p PORT route=parrot period=15   parrot route, replay one FT8 slot later
mockctl.py -p PORT replay                   play the parrot's recording again
mockctl.py -p PORT vox=on voxlevel=-40      key from transmit audio (Data2G's default PTT mode)
mockctl.py -p PORT line=rts                 key from the RTS line
mockctl.py -p PORT route=link linkbuffer=50 hear a second board, with 50 ms of buffer
```

Limits, as built:

- Settings live in RAM. A power cycle returns to the build-time values (menuconfig); `01` returns to them without one.
- The controls share the CAT port, so they can only be sent while no other program is using it, or by the program that is. A reply read by a second program on the same port is lost to the sender and may confuse the other. A phone app holding the port leaves no way in at all; the [web console](#web-control-and-telemetry-console) is the way in then.
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

As built (host-tested, not yet run on a board):

- **Any source keys.** `audio_engine` holds all three: the radio transmits while CAT, the selected control line or the VOX keys it, and unkeys when none does. CAT keying always works; the other two are settings, off at power-up, changed with the [mock-only controls](#mock-only-controls) or at build time.
- **Control line.** The setting names the line: off, RTS or DTR. The firmware passes every line-state change from the host to the engine, which ignores the lines unless one is selected.
- **VOX.** A transmit sample at or above the threshold (default −40 dBFS) opens it, and it closes the hang time (default 100 ms) after the last such sample. It is judged once per audio block: the block the audio starts in is already keyed, so the parrot records the start of the transmission. With no audio arriving at all the hang time still runs out. Changing an unrelated setting does not disturb an open VOX.
- **What CAT reports.** A read of `1C 00` says transmit whichever source keyed. The core keeps CAT's own PTT apart from the others', so neither clears the other; the firmware refreshes the others' state just before it feeds the parser each batch of serial bytes.
- **Data2G.** Its default PTT mode is VOX, so against the mock it needs `vox=on` (or its PTT mode set to CAT). VOX stays off at power-up because any sound the host plays would key the radio.
- **Monitor route.** PTT does not mute receive there, but the sources still report transmit to CAT, the status and the LED.

**Status LED** (colours chosen in `ui`, brightness 10 % by default):

| Colour | Meaning |
| --- | --- |
| Dim white | No USB host |
| Dim blue | Receiving |
| Red | Transmitting; in the parrot route this is the recording, including its 50 ms tail |
| Amber | The parrot has a recording and is waiting to replay it |
| Green | The parrot is replaying, or the other board is being heard over the link |

A task checks every 20 ms and writes the LED only when the state changes. The same task logs the control line and the VOX keying and unkeying on the UART jack.

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
| ESP-NOW | ~0.5–1 Mbps usable | No router, low latency, tens of meters; **chosen for link audio** |
| Wired UART | several Mbps | TX/RX/GND between boards; most deterministic; the fallback if sharing the radio hurts timing |
| Wi-Fi UDP | network-dependent | Boards anywhere; more latency and jitter; not planned |
| Bluetooth | a few hundred kbps | Considered and not used: the S3 has Bluetooth LE only (no Classic audio or serial profiles), LE delivers data in connection events at best every 7.5 ms, and it shares the one 2.4 GHz radio with Wi-Fi just as ESP-NOW does |

**One radio, two jobs.** Link audio goes over ESP-NOW and control goes over Wi-Fi (the [web console](#web-control-and-telemetry-console)), both on the S3's single 2.4 GHz radio at the same time. What that requires, from Espressif's documentation and not yet tried here:

- ESP-NOW sends on the channel the Wi-Fi interface is on, so both boards must be on the same channel. Joining the same access point puts them there; so does one board hosting an access point that the other joins.
- Wi-Fi power save must be off. A station that sleeps between beacons misses ESP-NOW packets. The boards are USB powered, so this costs nothing that matters.
- The two share airtime. Settings and status are light; anything heavy sent over Wi-Fi during a session (captured audio, say) competes with the link audio and may add jitter.

Audio over the link: 12 kHz 16-bit mono (~192 kbps), which covers Data2G's 2400 Hz bandwidth with room to spare.

Behavior:

- Half duplex keyed by PTT: board A's transmit audio becomes board B's receive audio.
- Pass audio only when both radios are on the same frequency (exchanged over the link).
- Keep buffering to tens of ms so ARQ turnaround timing looks like a real radio.
- Clock drift: each S3 has its own crystal, so the two 48 kHz rates differ by a few ppm. Handle it by occasionally dropping or repeating a sample (or resampling) to keep buffers from slowly filling or draining.

### As built

Steps 21 to 25. The core is host-tested; the two transports have only been compiled, and no packet has crossed between two boards.

**A fourth route.** `route=link` joins monitor, radio and parrot. In it the engine hands each block of transmit audio to the link, with whether the radio is keyed, and takes its receive source from the link; the receive channel (mute while keyed, turnaround, noise at the set SNR, attenuation) then applies as in any route. Each board's SNR setting is therefore the SNR of the path into that board, and the two directions can differ. Any PTT source keys the link, the VOX included. The monitor and parrot routes are unchanged; they were not moved behind the link, as step 21 once said, because a single selector already says what the receiver hears.

**Packets.** Little-endian, at most 249 bytes, so one fits an ESP-NOW frame of 250:

```
hello:  4D 11 <flags> <rx Hz, 4> <tx Hz, 4> <name, 16>          flags: 01 transmitting
audio:  4D 12 <flags> <seq, 2> <tx Hz, 4> <samples, 2 each>     flags: 01 first of a transmission, 02 last
```

`4D` marks a link packet, and the next byte is the format version (1) and the type. An audio packet holds up to 120 samples: 10 ms at 12 kHz.

**Sending.** While keyed, and for 50 ms after (transmit audio is still on its way through the host and USB when PTT drops), transmit audio is resampled to 12 kHz and sent 120 samples at a time. Each packet carries a sequence number and the frequency it was transmitted on. The last one is marked, and carries whatever part of 10 ms is left.

**Receiving.** Packets go into a jitter buffer, and playing starts when it holds the set time, 30 ms by default (`linkbuffer`, 10 to 200). A transmission shorter than that plays when its last packet arrives, or after 100 ms of nothing if that packet is lost. What goes wrong on the way is handled so that the audio after it is still on time, which OFDM modems care about more than a short gap:

- A lost packet is replaced by silence of the same length when the next one shows that it is missing, for up to 10 packets in a row.
- If the buffer runs empty before that, the silence played meanwhile counts towards the gap.
- A packet that arrives late or twice is dropped.
- A gap longer than 10 packets, or 100 ms of nothing, ends the reception; the next packet starts a new one.

**Clock drift.** The two boards' sample clocks differ by tens of parts per million. Over a transmission the buffer absorbs it: 40 ppm for two minutes moves the fill by 5 ms. If the fill, averaged over about a second, drifts further than 15 ms from the target, one sample per 5 ms block is dropped or repeated until it is back. The average keeps a lost packet, which moves the fill for a moment, from being taken for drift.

**Frequency.** A packet is only played if its transmit frequency equals the receiver's receive frequency, to the hertz, so split works and two radios on different bands do not hear each other. `match=off` removes the test. Mode is not compared.

**Peer.** A board in the link route announces itself four times a second with its name, its frequencies and whether it is transmitting. A board links to the first one it hears and stays with it until it has been silent for 3 s. Audio goes to the peer's own address and is taken only from the peer, so a third board in range is ignored by a linked pair. A board outside the link route announces nothing and ignores what arrives.

**Transports.** One is compiled in, chosen in menuconfig; both boards need the same.

| Transport | How |
| --- | --- |
| ESP-NOW (default) | Announcements are broadcast; audio is sent to the peer's address, which the radio acknowledges and retries. It sends on the channel Wi-Fi is on and through the interface Wi-Fi is using, so both boards must be on one channel: joined to the same network, or one joined to the network the other hosts. Wi-Fi is started for it even if the web console is off. Not encrypted. |
| UART | UART1, TX on GPIO17 and RX on GPIO18 by default, 921600 bit/s, crossed between the boards with a ground wire. Each packet is framed (SLIP) and checked (CRC-16), since a wire has no packets of its own. |

**Latency.** On a path with no delay the receiver starts playing 27 ms after the sender's audio begins (host test): the 30 ms buffer fills with the third 10 ms packet. On boards, the transport and the USB buffering on both sides come on top; that total has not been measured.

**Telemetry.** The web console shows the link's state, the other board's name, frequency and signal strength, the buffer's fill, and counts of packets received, lost, late and off frequency, packets sent and not acknowledged, times the buffer ran empty, and samples dropped or repeated for drift. The LED is green while the other board is being heard.

**On the host.** `make -C host console-sim` runs two simulated boards joined by the link, one page each (ports 8080 and 8081); `--link` starts both in the link route and `--loss N` loses packets on the way.

**Limits.**

- Two boards. A third in the link route pairs with whichever it hears first and is ignored by the other two; there is no way yet to choose a peer.
- The transport is a build-time choice, not a setting.
- Only frequency is matched: a USB signal is heard by a radio in LSB or FM as if the modes agreed.
- Each 10 ms audio packet is close to ESP-NOW's largest frame, and the rate ESP-NOW sends at has been left at its default; whether that leaves enough air time beside Wi-Fi is not known.
- Not verified on hardware, including whether ESP-NOW passes between a board hosting a network and a board joined to it, and whether it survives a board's change from joining to hosting.

## Web control and telemetry console

A browser on a laptop or phone reaches each board over Wi-Fi to change its settings and watch what it is doing while it runs. It exists because the other control path, the mock-only CI-V controls, shares the CAT serial port: a modem application holding that port (always on a phone, and WSJT-X on a PC) leaves no way to send them. The two-board acceptance test needs exactly that, stepping the SNR down while two phones hold a session.

As built (steps 16 to 19). The settings and telemetry code is host-tested and the page has been used in a desktop browser against a simulator; the Wi-Fi and server code has only been compiled, and nothing has run on a board.

**Network.** At start-up the board joins the Wi-Fi network it has been given. If it has none, or has not joined within 20 s, it hosts its own network until the next restart: WPA2 with the password `mockradio` (a menuconfig setting, as is the channel, 1), and the console at `http://192.168.4.1/`. It is a station or an access point, never both: a station that keeps scanning for a missing network drags an access point on the same radio from channel to channel. The network to join comes from the console's own Wi-Fi form, which stores it in flash (NVS) and restarts the board, or failing that from menuconfig. Each board names itself `mockradio-xxxx` from its MAC address. That is the name of its own network, its DHCP hostname and its mDNS name, so the console is also at `http://mockradio-xxxx.local/`. Power save is off (see [Two-board link](#two-board-link)).

**Interface.** JSON over HTTP, and one WebSocket:

| Request | Does |
| --- | --- |
| `GET /` | The page |
| `GET /api/info` | The board's name, the personality, and the list of settings: each one's name, label, group, and either its choices or its unit, range and step |
| `GET /api/settings` | Every setting: `{"route":"monitor","noise":"off","snr":10.0,"bw":2500, ...}` |
| `POST /api/settings` | Change any subset: `{"noise":"on","snr":-12.5}`. Answers with every setting, or with `{"error":"snr: must be between -60.0 and 80.0 dB"}` |
| `POST /api/reset` | Back to the power-up settings |
| `POST /api/replay` | Play the parrot's recording again |
| `GET /api/telemetry` | One telemetry snapshot |
| `GET /ws` | WebSocket: the same snapshot, pushed five times a second |
| `GET /api/wifi`, `POST /api/wifi` | How the board is connected; save a network to join (`{"ssid":...,"password":...}`) and restart |

**Settings.** The names and units are the ones `mockctl.py` uses (`snr=-12.5` there, `"snr":-12.5` here). `console_api` holds only the names: for each it names the `mock_ctl` command that carries the setting, and it reads, range-checks and writes the value through `mock_ctl`'s own functions. So there is one table of ranges and one set of power-up values, a change made on either path shows on the other, and values keep the CI-V controls' steps (tenths of a dB and of a second). A value out of range is refused, not clamped, and a request with one refused value changes nothing.

**Telemetry.** A snapshot holds: PTT and which sources key it; the CAT frequency, mode and split, and the CI-V frame counts; the level (RMS and peak over 0.1 s) of the audio the host plays and of the audio it records, each absent when the host is not streaming that way; the audio FIFO's fill, target and its overrun, underrun and drift-steering counts; the receive channel's measured source level, gain and mute; the parrot's state and recording length; the Wi-Fi signal strength; and a number that changes when the settings do, however they were changed, so the page knows to read them again. With the link built in, the link's state and statistics are in it too (see [Two-board link](#two-board-link)).

**Page.** One file of about 22 KB, embedded in the firmware, with nothing loaded from anywhere else, because the board may be on a network with no internet. It builds its settings form from `/api/info`, so a setting added in `console_api.c` appears without an edit to the page. A status line shows the same states as the LED; a strip chart shows the last 60 seconds of both levels and of PTT, kept in the browser only.

**On the host.** `make -C host console-sim` serves the page at `http://localhost:8080` with a simulated radio behind it: `console_api`, `mock_ctl` and the audio engine as compiled for the host, with a pretend host that keys and plays a tone for 6 s in every 15. `make -C host console` checks the interface through the same simulator. The simulator's HTTP layer is a Python stand-in for `web_console.c`, so neither tests that file.

**Limits.**

- No login and no encryption: it is a bench device on a private network. A Wi-Fi password typed into the form crosses that network in the clear. Requests that change anything must be sent as `application/json`, which a page on another site cannot make a browser do, so such a page cannot change settings behind the operator's back.
- Once the board has fallen back to hosting its own network it stays there until restarted, even if the configured network returns.
- Settings are still not stored (step 14); only the Wi-Fi network is.
- The page's history is 60 seconds and lives in the browser; nothing is recorded on the board.
- Telemetry is sampled five times a second, so a transmission shorter than 0.2 s can fall between two snapshots.
- Wi-Fi traffic shares the radio with ESP-NOW link audio, so the console keeps its traffic light: about 3 KB per second per open page.
- The console is on by default and can be switched off in menuconfig, which keeps Wi-Fi off altogether.

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
7. Written, not yet on hardware: CDC wired to the parser. Line-state changes go to the engine, which keys from them only when a PTT line is selected (step 13).
8. Written, not yet on hardware: audio loopback, USB OUT through a drift-steered FIFO (about 20 ms) to USB IN. It passes audio regardless of PTT, so a round trip can be checked with any recorder; muting receive while keyed belongs to parrot and link modes.
9. Acceptance: `rigctl -m 3085 -r /dev/ttyACM0 -vvvv`, then `f`, `F 7074000`, `m`, `T 1`, `T 0`, with no Hamlib errors; audio round-trips in Audacity or between two fldigi or WSJT-X instances.

**Milestone 2: parrot + channel simulation**

10. Written, not yet on hardware: PSRAM parrot buffer at 12 kHz in `audio_engine`, as a third route: record on PTT on, replay on PTT off after a configurable delay or aligned to a slot period, with host tests.
11. Written, not yet on hardware: channel on the receive path in `audio_engine`: noise at SNR, attenuation, receive mute while keyed and TX→RX turnaround delay, with host tests. Power-up settings are build-time; step 12 changes them at run time.
12. Written, not yet on hardware: mock-only controls in `mock_ctl` on CI-V address `F0`, which no radio in Hamlib 4.7.1 uses; a separate address keeps them clear of every radio's command set. Route, channel settings, parrot settings, replay and status so far, with host tests and `host/tools/mockctl.py`. Stored-clip, link, PTT-source and personality commands have numbers reserved and join with those parts.
13. Written, not yet on hardware: VOX PTT and control-line PTT in `audio_engine` beside CAT, both selectable and off at power-up, with host tests and mock-only controls; LED states (red recording, green playing, amber waiting, dim blue receiving, dim white no host) in `ui`, driven through RMT.
14. Optional flash persistence for the last transmission and canned clips.
15. Acceptance: WSJT-X FT8 self-decode with replay aligned to the 15 s slot.

**Milestone 3: web control and telemetry console**

16. Written, compiled only: Wi-Fi bring-up beside USB: join a configured network or host an access point, a name per board (`mockradio-xxxx`, also by mDNS), power save off.
17. Written: a JSON interface to the settings in `console_api`, through `mock_ctl`'s own settings functions, with host tests; the HTTP server around it is compiled only.
18. Written, tried in a desktop browser against the host simulator: control page embedded in the firmware: every setting, parrot replay, reset, and the Wi-Fi form.
19. Written: live telemetry over a WebSocket, five snapshots a second: PTT, frequency and mode, levels and gain, parrot state, audio FIFO and CI-V counters. The snapshot is host-tested; the push is compiled only.
20. Acceptance: change the SNR from a browser while WSJT-X or a phone app holds the CAT port, and watch levels and PTT live, with no added audio underruns.

**Milestone 4: two-board link**

21. Written: the link's core in `link` (packets, sending, receiving, announcing frequency and PTT), behind a send function and a receive call, with host tests; a fourth route in `audio_engine`. The monitor and parrot routes stayed where they were.
22. Written, compiled only: ESP-NOW transport on the channel Wi-Fi is using: 12 kHz 16-bit audio in 10 ms packets with sequence numbers; boards pair with the first other board they hear.
23. Written, host-tested: jitter buffer, silence in place of lost packets, and drift correction; link statistics in the console's telemetry and on its page.
24. Written, host-tested: frequency matching, with a setting to switch it off.
25. Written, compiled only: UART transport with framing and a check, as a build-time alternative.
26. Acceptance: two PCs running fldigi or VARA first, then two Android phones running Data2G in ARQ mode, stepping the SNR down from the web console to watch speed adaptation.

**Milestone 5: G90 personality (later pass)**

27. Run-time personality selection as a stored setting, switched by a mock-only control.
28. `rig_g90` from Hamlib's `xiegu.c` and the Radioddity CAT manual; test with `rigctl -m 3088 -s 19200 -vvvv` on the host, against more than one Hamlib version because the default address has changed.
29. When the real G90 and DE-19 are available: capture the DE-19's USB descriptors and a `rigctl` session against the radio, and reconcile the personality with them.

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
| Nov 19, 2026 | The PTT sources combine in `audio_engine`: CAT always, plus a selected control line and VOX as settings. Any one keys. | The VOX needs the transmit audio, which the engine already has, and one place decides transmit for the channel, the parrot, the status and the LED. Sources add rather than exclude, so a setting never locks CAT keying out. |
| Nov 19, 2026 | VOX and control-line PTT are off at power-up. | Hosts raise RTS and DTR on open, and any sound the host plays would open the VOX. Data2G's default mode is VOX and needs it switched on. |
| Nov 19, 2026 | `cat_civ` keeps CAT's PTT and other sources' PTT as two flags; `1C 00` reads true if either is set. | With one flag, a CAT unkey would clear a VOX transmit and the other way round. |
| Nov 19, 2026 | The LED is driven with ESP-IDF's RMT driver directly, and its colours are decided in a portable module. | One LED needs a few lines of RMT, not another managed component; the policy can be host-tested. |
| Nov 19, 2026 | Link audio goes over ESP-NOW and control over Wi-Fi, both on the S3's one radio. Bluetooth is not used. | Control has to reach a board while a modem application holds its CAT port, and Wi-Fi is the obvious way; ESP-NOW runs alongside Wi-Fi on the same channel, so the link need not move. The S3's Bluetooth is LE only, slower and less even in timing, and would share the same radio anyway. |
| Nov 19, 2026 | Add a web control and telemetry console over Wi-Fi, as its own milestone ahead of the two-board link. | The link's acceptance test steps the SNR while two phones hold the CAT ports, which the CI-V controls cannot do; building it first also settles the Wi-Fi channel before ESP-NOW has to share it. It works on one board. |
| Nov 19, 2026 | The web console reaches the settings through `mock_ctl`: it maps each setting's name to its command number and uses `mock_ctl`'s range table and its get and put functions. | One table of ranges and one set of power-up values, so the two control paths cannot drift apart, and a value keeps the same steps on both. |
| Nov 19, 2026 | The console's JSON is written and read by a small flat-object reader and writer in `console_api`, not a JSON library. | A setting is never nested, so little is needed; the code is the same on the host and the board and is tested there with sanitizers; numbers are printed from integers, so the text does not depend on the C library's floating-point printing. |
| Nov 19, 2026 | Wi-Fi: join the configured network, and after 20 s without it host one, as a station or an access point but never both. The network comes from the console's Wi-Fi form (stored in NVS, then a restart) or from menuconfig. | A board must be reachable with no router and no rebuild; a station that keeps scanning disturbs an access point on the same radio; a restart is the simplest change of network to get right without a board to test on. No password goes into a committed file. |
| Nov 19, 2026 | Each board is named `mockradio-xxxx` from its MAC address, announced by mDNS (Espressif's `mdns` component). | Two boards built from one image must be told apart, and a name is easier to reach than an address read from the log. |
| Nov 19, 2026 | The page is one self-contained file that builds its settings form from `/api/info`. | It must work on a network with no internet, and a new setting should need no edit to the page. |
| Nov 19, 2026 | Telemetry is a JSON snapshot pushed five times a second over a WebSocket; history is kept only in the browser. | Light on a radio that will also carry link audio, and fast enough to watch levels and PTT by eye. Recording on the board would need storage and a reason. |
| Nov 19, 2026 | The console has no login; requests that change anything must be `application/json`. | A bench device on a private network. The content type costs nothing and stops other sites' pages from posting to it through the operator's browser. |
| Nov 19, 2026 | One mutex in `main/` is held around the CI-V parser and around each settings change from the console. | Both paths read, change and write back the settings; without it two changes at the same moment could lose one. It also lets the console read the parser's frequency and mode whole. |
| Nov 19, 2026 | The firmware uses the partition table with a 1.5 MB application partition. | With Wi-Fi the image no longer fits the default 1 MB. The flash has 16 MB. |
| Nov 19, 2026 | The web console is on by default, with a menuconfig switch that keeps Wi-Fi off. | It is how the board is meant to be used, and its telemetry (FIFO counts, levels, whether the host is recording) helps the first bring-up; the switch is there for when Wi-Fi must be ruled out. |
| Nov 19, 2026 | The link is a fourth receive route in `audio_engine`, reached through two functions the firmware supplies; monitor and parrot were not moved behind a link interface. | One selector already says what the receiver hears, and the VOX and the channel work in the link route for free. Moving two tested routes behind an interface would have changed working code for no gain. |
| Nov 19, 2026 | Link audio is 10 ms packets of 120 samples, each carrying a sequence number and the frequency it was transmitted on. | 240 bytes of audio and 9 of header fit ESP-NOW's 250-byte frame. A packet that describes itself needs no second message to arrive first. |
| Nov 19, 2026 | A lost packet is replaced by silence of its own length, counting silence already played while the buffer was empty. | Modems resynchronise after a gap more easily than after a shift in time. |
| Nov 19, 2026 | Clock drift is absorbed by the jitter buffer and corrected, one sample at a time, only when the fill averaged over a second is more than 15 ms off. | Transmissions are short against the drift of two crystals, so correction is rarely needed; judging by the average keeps loss and jitter from being taken for drift. |
| Nov 19, 2026 | Frequency matching is exact, sender's transmit against receiver's receive, with a setting to switch it off. Mode is not compared. | Modem software sets the same dial frequency at both ends; an exact test is simple and handles split. The switch is for bring-up. |
| Nov 19, 2026 | A board pairs with the first other board it hears announcing itself, and audio goes to that board's address only. | Two boards need no set-up, and addressed ESP-NOW frames are acknowledged and retried by the radio where broadcast ones are not. |
| Nov 19, 2026 | The link's transport (ESP-NOW or UART) is chosen at build time; the link's buffer and frequency matching are run-time settings in the engine's settings. | The UART needs pins and wires, so it is a property of the build; the two settings get the CI-V control, the console form and reset for free by living with the others. |
| Nov 19, 2026 | Wi-Fi bring-up moved to its own component, `wifi_net`, used by the web console and by the ESP-NOW transport. | Both need the radio on one channel, and the link must work with the console switched off. |
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
- [x] How to change the mock's settings while a modem application holds the CAT port (always the case on a phone, and with WSJT-X on a PC). The web console over Wi-Fi (Milestone 3), decided Nov 19, 2026. Until it exists, set them before the application opens the port, or at build time. A line console on the UART jack remains a simpler fallback.
- [ ] Does ESP-NOW link audio keep its timing while the same radio serves the web console over Wi-Fi? If not, the wired UART link takes over. (step 26)
- [ ] Does ESP-NOW pass between a board hosting a network and a board joined to it, and does it come back after a board changes from joining to hosting? Compiled only. (step 26)
- [ ] Is ESP-NOW's default sending rate enough for 100 packets of 249 bytes a second in each direction of a session, or does the rate need raising? (step 26)
- [ ] What is the link's delay end to end on boards, host audio in to host audio out, and is 30 ms of buffer the right default for Data2G's ARQ turnaround? 27 ms on a path with no delay in the host test. (step 26)
- [ ] Should a board be able to choose its peer, for more than two boards in range? Today the first one heard is taken.
- [ ] Should the link compare mode as well as frequency?
- [ ] Are GPIO17 and GPIO18 free for the UART link on the boards that arrive? (step 26)
- [x] How does a board get its Wi-Fi settings, and which board hosts the access point when there is no router? Both ways: the console's Wi-Fi form (stored in flash) or menuconfig. A board that cannot join a network hosts its own, so with no router one board is left to host and the other is pointed at its network through the form. Decided Nov 19, 2026; not tried.
- [ ] Does the web console work on a board: joining a network, falling back to its own, the `.local` name, the WebSocket push, and the page on an Android phone's browser? Compiled only. (step 20)
- [ ] Is the default password of the board's own network (`mockradio`) acceptable, or should each board get its own?
- [ ] Are five telemetry snapshots a second enough to follow ARQ turnarounds, or does PTT need to be counted or timestamped on the board? (step 26)
- [ ] Does Wi-Fi running beside USB audio cause underruns or audible glitches on the S3? (step 20)
- [ ] Which telemetry is worth recording over time rather than only showing live, and where it is kept (the browser, or a file the page can download)? The page keeps 60 seconds of levels and PTT in the browser; nothing longer yet. (after step 20)
- [ ] Do the mock-only controls work on a board, and does a setting change mid-stream sound clean? Host-tested only. (after step 9)
- [x] Default reference bandwidth for the SNR setting: 2500 Hz. Data2G reports its SNR in 2500 Hz (`SNR_REF_BW_HZ` in `native/core/generated/config.hpp`), as does WSJT-X; 3000 Hz remains a setting for VARA.
- [ ] Are the channel defaults right for real modems: noise floor −40 dBFS, turnaround 30 ms, and silence (not a low noise floor) on receive while keyed? To be judged with WSJT-X, FT8AF and Data2G on a board. (after step 9)
- [ ] Should the S-meter (`15 02`) follow the receive level the engine produces? `civ_set_meter` exists for it. Not done with step 13; no client seen so far depends on it.
- [ ] Are the VOX defaults right for real modems (−40 dBFS, 100 ms hang), and does Data2G in its default VOX mode hold a clean transmission against the mock with `vox=on`? (after step 9)
- [ ] Is the RGB LED on GPIO48 on the boards that arrive, does it need the solder pad bridged, and is 10 % brightness right? (step 6)
- [ ] G90, later pass: exact replies (BCD byte order, mode bytes for U-D and L-D, ID bytes for `19 00` and `1D 19`, behaviour on unknown commands, echo), and whether to support `25`/`26` (steps 28–29)
- [ ] G90, later pass: what the real DE-19 presents over USB (VID/PID, one or two COM ports, audio format, device name), and whether it keys the G90 from RTS or DTR or only by CAT. One setup guide says to hold both lines low with PTT set to CAT. (step 29)
- [x] How control-line PTT mode is selected and stored. Selected with mock-only control `50` or at build time (Nov 19, 2026). It is not stored: like every setting it returns to its build-time value at power-up until step 14. The BOOT button is not used.
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
