# ESP32-S3 Mock Radio — Design

Living document. Edit it in place as the design changes: update the section a fact belongs to, add a dated line to the decision log, and tick or add open questions. [HANDOFF.md](HANDOFF.md) is the original Nov 19, 2026 handoff, kept as a frozen snapshot.

Operator: Tim, W0SIG (General).

## Status

Last updated: Nov 25, 2026.

- First board brought up on Nov 25, 2026 (an ESP32-S3-DevKitC-1 clone, N16R8, CH343 on the COM jack), on macOS 15 through a USB hub. It boots cleanly with the web console and the ESP-NOW link built in: the octal PSRAM is found and tests good, the self-check passes, the board hosts its own network. macOS enumerates the composite device as "Mock Radio" with a sound card ("Mock Radio Audio", 48 kHz mono each way) and a CAT serial port. `rigctl -m 3085` and, after `mockctl.py rig=g90`, `rigctl -m 3088` pass the acceptance commands with no Hamlib errors. Audio loops back bit-exact at unity gain; see [Bring-up on hardware](#bring-up-on-hardware) for how it was measured and the one fix it needed. Linux, Windows, Android, the web console in a browser, the link, the parrot and flash storage are not yet tried on the board.

- The CI-V core (`cat_civ`) and the IC-705 personality (`rig_ic705`) exist as portable C and pass real Hamlib on the host: `rigctl -m 3085` (Hamlib 4.7.1) over a pseudo-terminal, 16 command groups, no command rejected. The commands the Android FT8 apps FT8CN and FT8AF send an IC-705 were read from their source and all get correct replies on the host; neither app has been run.
- The audio DSP (`audio_dsp`) exists as portable C with host unit tests: resampler, channel noise and attenuation, level meter, VOX detector, and a FIFO with clock-drift steering.
- The receive channel of Milestone 2 (`audio_engine`) exists as portable C with host unit tests: a fixed noise floor with the signal held at a set SNR, attenuation, and receive mute while keyed plus a turnaround delay. It is wired into the firmware's audio path; the power-up settings are build-time and the default is still a plain loopback.
- The parrot (step 10) exists in `audio_engine` as a third receive route, with host unit tests: it records each transmission at 12 kHz and replays it after unkeying, after a delay or aligned to a slot period, to the sample. The firmware gives it a buffer in PSRAM. Not run on a board.
- PTT from a serial control line and from VOX (step 13) exists in `audio_engine` beside CAT keying, with host unit tests; both are off at power-up. A read of `1C 00` reports transmit whichever source keyed. The LED policy (`ui`) exists with host unit tests, and the firmware drives the board's RGB LED from it. Not run on a board.
- The mock's own controls (`mock_ctl`, step 12) exist as portable C with host unit tests: CI-V frames to the private address `F0` read and change the route, the channel settings, the parrot's settings and the PTT sources at run time, replay the parrot's recording, and read the engine's status. `host/tools/mockctl.py` sends them; it is checked against the parser over a pseudo-terminal, not against a board.
- The web console (Milestone 3, steps 16 to 19) is written. Its settings and telemetry (`console_api`) are portable C with host unit tests, sharing `mock_ctl`'s table of settings. The page has been tried in a desktop browser against a simulator that runs the same C code on the host, and on the first board (Nov 25, 2026): hosting its own network and served to an Android phone, given a home network through the Wi-Fi form, then joined to it and reached by its `.local` name from a desktop browser, with the telemetry, settings, front panel, waterfall and health sections all working. The waterfall needed a fix on the board; see [Bring-up on hardware](#bring-up-on-hardware).
- The firmware builds for the ESP32-S3 (ESP-IDF v5.5.6, 1037 KB image with the web console and the ESP-NOW link; 319 KB with the console and the link both off, which leaves Wi-Fi out) with the whole of Milestone 1 written: the USB composite device (sound card plus CAT serial port), the serial port wired to the CAT parser, and audio loopback. Its descriptors pass a structural check against the S3's limits and Android's requirements. None of it has run on hardware: whether hosts enumerate it and stream audio is unknown until a board is flashed.
- No modem application has been tried against the board yet.
- The G90 personality (Milestone 5, steps 27 and 28) is written. `rig_g90` is built from the Radioddity CI-V table, Hamlib's Xiegu backend and one log of a real G90, not from a radio: the G90 and DE-19 are not accessible during this project. It passes real Hamlib on the host: `rigctl -m 3088` (Hamlib 4.7.1 only) over a pseudo-terminal, 32 command groups, no command rejected. The radio imitated is chosen at run time, over CI-V (`60`), with `mockctl.py` or in the web console, and the firmware keeps the choice in flash. Reconciling it with the real radio (step 29) waits for the station.
- Flash persistence (step 14) is written. Settings can be kept as the power-up settings, and the parrot's recording can be kept as one of five clips in a flash partition of its own and played back later. The packing of settings (`mock_ctl`) and the clip store (`clips`) are portable C with host unit tests, the latter on a pretend flash chip that can lose power mid-save. The firmware side (NVS, the partition, the task that writes clips) has only been compiled: no byte has been written to real flash.
- Path simulation (Milestone 6, steps 30 and 31) is written. The receive channel can fade the signal as an HF path does, by the eight conditions the Data2G project's simulator names (its `mpg`, `mpp`, `mpd` and `mps`, and four ITU-R F.1487 ones) or by custom numbers, and can shift it in frequency. Two linked boards share one channel: a protocol on the link keeps their path settings, fading seed and clock in step, so the path fades alike both ways. All of it is portable C with host unit tests, which check the fading's statistics and that two boards with clocks 100 ppm apart fade together. None of it has run on a board, and its cost in processor time there is an estimate.
- Board health readings are written, for the first bring-up: free memory, how busy each processor core is, the time the audio work takes per block, how often Wi-Fi was lost, why the board last restarted and, on request, every task's stack headroom and share of a core. They go to the log on the COM jack every 30 s and to a section of the web console. The layout of a reading and the timing of the audio work are portable C with host unit tests; the collecting (`main/health.c`) has only been compiled, so no real number has been read yet. The simulator's health numbers are made up and say so.
- The front panel (Milestone 7, steps 34 and 35) is written: a section of the web console that shows the radio imitated as an operator at it would have it (both VFOs, mode, split, RIT, every level, function and meter the personality lists) and lets them be changed there, with a PTT key of its own. It is generic: built from the personality's tables, with no radio drawn. `console_api` and `cat_civ` carry it as portable C with host unit tests. The simulator now gives each board a CAT serial port, so the page has been watched in a browser while real Hamlib (`rigctl -m 3085`) drove the radio, and while Data2G's rigctld facade (its `feature/rigctld` branch, model 3085 upstream, `rigctl -m 2` as the client) did. The firmware's side is compiled only.
- The waterfall (step 37) is written: a section of the web console that shows the spectrum of the audio each way, up to 3 kHz, scrolling. The spectrum (`audio_dsp`) and the row format (`console_api`) are portable C with host unit tests, and it has been watched in a desktop browser against the simulator, with a tone, noise and the receive mute all showing as they should. On a board the audio task would only copy samples; the transforms run in the console's task while a page has the waterfall open. That side is compiled only, and the time a row takes on the S3 is an estimate.
- Next step: the rest of Milestone 1 on the board: enumeration on Linux, Windows and the Android phones (step 6), and audio round trips through real applications (step 9), then the web console from a browser (step 20). Milestone 2 has steps 10 to 13 written (parrot, channel, mock-only controls, VOX and line PTT with the LED states); the optional flash storage (step 14) and the FT8 acceptance test (step 15) remain. Milestone 3 has its acceptance test (step 20) left, which needs a board, and Milestone 4 its own (step 26), which needs two. Everything that can be written without hardware now is.
- The two-board link (Milestone 4, steps 21 to 25) is written. Its core (`link`: packets, jitter buffer, loss and clock-drift handling, peer discovery, frequency matching, framing for a wire) is portable C with host unit tests that join two boards by a simulated path with loss, jitter, reordering and clocks that differ. It is a fourth receive route in `audio_engine`, with two settings on both control paths and its statistics in the web console. Two simulated boards have been watched hearing each other in a browser. The ESP-NOW and UART transports have only been compiled: no packet has crossed between two boards.
- Three ESP32-S3 boards are at hand; one has been flashed.
- Data2G's Android USB code has been read (fork at v0.5.2+8) and fits this design; see [Data2G target](#data2g-target).

## Goal and scope

Build ESP32-S3 firmware that plugs into a computer or Android phone over USB and looks like a ham radio with a built-in sound card and CAT port on one USB cable, the way an Icom IC-705's USB port does. Modem and logging software (Data2G, WSJT-X, VARA, Winlink, fldigi) should drive it through Hamlib exactly as it would a real rig.

The radio being imitated is a swappable personality: an Icom IC-705, which came first, or a Xiegu G90, so software can be set up on the bench and moved to the operator's real G90 without changing its rig settings.

In scope:

- One USB connection that shows up as a USB sound card (UAC2, 48 kHz 16-bit mono, playback + capture) plus a USB serial port (CDC-ACM).
- An Icom CI-V CAT emulation on the serial port: an IC-705 that Hamlib (rig model 3085) accepts, or a G90 (rig model 3088), switchable at run time.
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
- `cat_civ`: CI-V framing, address handling, command dispatch and the radio state struct (VFO A/B, mode, split, PTT, levels). It also runs a command for the front panel as if CAT had sent it (`civ_local`), and gives a number that changes when the radio's state does (`civ_state_hash`)
- `rig_ic705` and `rig_g90`: the two personalities, behind one interface; the parser becomes one or the other at run time
- `coord`: two linked boards agreeing on one channel: the path's settings, and the seed and clock their fading runs by
- `audio_dsp`: the signal processing itself, with no hardware dependencies: fading and frequency offset, 48↔12 kHz resampling, noise and attenuation, level meter, VOX detector, FIFO with drift steering, and the spectrum the console's waterfall shows
- `audio_engine`: transmit audio in, receive audio out, using `audio_dsp`. The route picks what the receiver hears (the transmit audio, nothing, or the parrot's replay from PSRAM); the channel then applies mute while keyed, turnaround delay, noise at a set SNR and attenuation. In the link route the receiver hears a second board and the transmit audio goes to it; the engine only hands the audio over, through two functions the firmware supplies. It also combines the PTT sources: CAT, a selected control line, the VOX it runs on the transmit audio, and the key on the front panel
- `mock_ctl`: the mock's own controls, CI-V frames to address `F0`: route, channel settings, parrot settings, the link's settings, PTT sources and status now; personality later. It also offers the same settings by number to other control paths, which is how the web console shares its ranges
- `link`: the path between two boards: 12 kHz audio in 10 ms packets, a jitter buffer with loss and clock-drift handling, peer discovery and frequency matching, all portable; and one of two transports that carry the packets, ESP-NOW or a UART, chosen at build time. See [Two-board link](#two-board-link)
- `wifi_net`: Wi-Fi bring-up, shared by the web console and the ESP-NOW transport: joining or hosting a network, the board's name
- `web_console`: the settings, the telemetry and the radio's front panel as JSON (`console_api`, portable), and around it Wi-Fi, a small HTTP server, the control page and a WebSocket for live telemetry; see [Web control and telemetry console](#web-control-and-telemetry-console)
- `ui`: which colour the RGB LED shows for the radio's state; the firmware drives the LED

`cat_civ`, the personalities, `audio_dsp`, `audio_engine`, `mock_ctl`, `ui`, the `console_api` part of `web_console` and the core of `link` are written as portable C with no ESP-IDF dependencies, so they can be tested on the host.

Runtime: FreeRTOS, as bundled with ESP-IDF (the S3 runs its dual-core SMP build). No separate RTOS choice is needed; the Arduino fallback runs on the same kernel. TinyUSB and the two directions of USB audio each get a task, and the audio engine runs in the capture one; `cat_civ` is passive and runs in whichever task delivers serial bytes. The web console's HTTP server has a task of its own at a lower priority than the USB and audio tasks. Because it changes the same settings as the CI-V controls, one mutex in `main/` is held around the CI-V parser and around each settings change from the console. The link is spread over three tasks without a lock: the audio task sends and plays, the transport's task (Wi-Fi's, or a UART reader) puts arriving packets into the jitter buffer, and the status task announces the board every 250 ms and copies the engine's link settings across.

Repository layout:

| Path | Contents |
| --- | --- |
| `components/cat_civ/` | CI-V core |
| `components/rig_ic705/` | IC-705 personality |
| `components/rig_g90/` | G90 personality |
| `components/audio_dsp/` | Audio DSP |
| `components/audio_engine/` | Receive routes (monitor, radio, parrot), the parrot's recording and replay, and the receive channel: mute and turnaround, SNR, attenuation |
| `components/mock_ctl/` | The mock's own controls on CI-V address `F0` |
| `components/ui/` | LED policy: the state to show and its colour |
| `components/coord/` | Shared channel: keeps two linked boards' path settings and fading in step |
| `components/clips/` | Clip store: recordings kept in a flash partition, and saving and playing them through the parrot |
| `components/web_console/` | Web console: `console_api.c` (settings and telemetry as JSON, portable), `web_console.c` (HTTP and WebSocket server), `www/index.html` (the page, embedded in the firmware), `Kconfig` (the console switch) |
| `components/wifi_net/` | Wi-Fi: joining or hosting a network, the board's name, and its `Kconfig` settings |
| `components/link/` | Two-board link: `link.c` (portable core), `link_espnow.c` and `link_uart.c` (the transports; one is compiled), `Kconfig` (which one, and the UART's pins) |
| `components/usb_composite/` | USB device: descriptors, TinyUSB configuration, serial and audio callbacks |
| `main/` | Firmware entry point: boot self-check, then CAT and audio wiring, the status task and the web console's hookup; `status_led.c` drives the RGB LED through RMT; `storage.c` keeps things in flash (NVS, the clips partition and the task that writes it); `Kconfig.projbuild` holds the power-up route, channel, parrot and PTT settings and the LED's pin |
| `partitions.csv` | Flash layout: NVS, 2 MB for the firmware, the rest of the 16 MB for clips |
| `CMakeLists.txt`, `sdkconfig.defaults` | ESP-IDF project file and board configuration (16 MB flash, octal PSRAM, UART console) |
| `host/` | Host-only build: `civ_pty` (parser behind a pseudo-terminal), `test_civ`, `test_dsp`, `test_engine`, `test_ctl`, `test_ui`, `test_console`, `test_link`, `test_clips` and `test_coord` (unit tests), `console_sim` with `tools/console_sim.py` (the web console in a browser with two simulated radios joined by the link, each with a CAT serial port for Hamlib, and a check of its interface), `rigctl_check.sh` (real Hamlib acceptance check), `tools/mockctl.py` (reads and changes the mock's settings over its CAT port) with `mockctl_check.sh` (its check over the pseudo-terminal), `tools/gen_resampler_fir.py` and `tools/gen_hilbert_fir.py` (filter generators), `tools/check_usb_descriptors.py` (descriptor check on the built firmware) |

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
| Xiegu G90 | Second | 3088 | `70` in the manual; Hamlib sends to `A4` (4.7.1) or `88` (master) | Radioddity CAT manual; Hamlib's `xiegu.c`; the real radio when available |

### Core and personalities

- **`cat_civ` (core):** buffers bytes until `FD`, validates the frame, dispatches on command and sub-command, keeps the radio state, and builds replies. It implements the commands every CI-V radio shares.
- **Personality (`rig_ic705`, `rig_g90`):** a table of what makes one radio different: default address, transceiver ID reply, mode-byte table, which commands and sub-commands exist, frequency coverage and transmit bands, power limit, level and meter defaults, the `1A 05` menu settings it has, power-up state, and handlers for radio-specific commands.

The core never names a radio. Anything radio-specific goes through the personality interface, so adding the G90 meant adding one module: the core gained only `civ_set_rig()`, which makes the parser another radio as if one had been switched off and the other on (power-up state, PTT released). Which radio is a run-time choice: the mock-only control `60`, `mockctl.py rig=g90` or the web console's selector, kept in flash by the firmware, with the build's choice (menuconfig) used until a board has been told. It is not one of the settings that `01` (reset) puts back. The CI-V address cannot be used to pick the personality, because Hamlib 4.7.1 addresses both radios at `A4`.

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

Names for the [front panel](#front-panel) sit beside each level, meter and function in `rig_ic705`. The functions' values are named only where Hamlib 4.7.1 was logged sending them (`rigctl -m 3085`, Nov 21, 2026): `16 12` AGC `01` fast, `02` medium, `03` slow, `00` off; `16 02` preamplifier `01` and `02` (asked for as `L PREAMP 1` and `2`; `10` and `20` are refused by Hamlib itself); `16 47` break-in `01` semi, `02` full; `11 20` for the attenuator at 20 dB. Icom's guide was not reachable to name the rest.

Emulated radio facts: receive 0.030–199.999 MHz and 400–470 MHz; transmit on the amateur HF, 50, 144 and 430 MHz bands; 10 W maximum. Power-up state: 14.074 MHz, USB with data mode on, VFO A, split off.

How it is verified without a radio:

- **Hamlib is the acceptance test.** `rigctl -m 3085 -vvvv` pointed at the parser on the host shows every frame Hamlib sends and whether it accepts each reply. This needs no radio and no ESP32.
- **Icom's CI-V Reference Guide** gives the official data layout for every command.
- Hamlib's `simulators/simic705.c` is a hint only. It sends nothing for unknown commands, does not handle `1A 05`, and rewrites the received buffer rather than building replies.

### G90 personality

Built from documents, not from a radio. Each fact below is marked with where it comes from:

- **[M]** Radioddity, "Xiegu G90 CAT control and digital modes" v1.0 (2021-06-23), section 6: the official command table.
- **[H]** Hamlib `rigs/icom/xiegu.c`, read at 4.5.5, 4.6.5, 4.7.1 and master. Master's G90 code cites [M], the specification sheet, and observations on stock firmware v1.81.
- **[I]** Hamlib issue 1547: a log of a G90 with firmware 20240504.8.
- **[R]** What `rigctl -m 3088` (Hamlib 4.7.1) sent this personality on the host, and accepted back.

| Command | In the personality | Source |
| --- | --- | --- |
| `00`, `01` | Set frequency and mode with no reply (transceive) | M |
| `02` | Frequency edges: lower, `2D` ('-'), higher; 0.5 and 30 MHz | M, range from H |
| `03`, `05` | Read and set the frequency, 5 BCD bytes, least significant first | M, R |
| `04`, `06` | Read and set mode and filter. Modes `00` LSB, `01` USB, `02` AM, `03` CW, `05` FM, `07` CW-R; filters 1 to 3 | M; FM and the filter byte from H |
| `07` | `00` VFO A, `01` VFO B, `B0` swap. No `A0` | M |
| `0F` | `00`/`01` split off and on. No repeater duplex | M |
| `11` | Attenuator: stores the byte given, reads it back | Differs from the radio, see below |
| `14` | `01` AF, `03` squelch, `09` CW sidetone, `0A` power, `0C` key speed, as 0 to 255; `02` RF gain. Any other sub-command reads as 255 and cannot be set | M; RF gain from H |
| `15` | `01` squelch status, `02` S-meter, `11` power, `12` SWR | M |
| `16` | `02` preamplifier, `12` AGC (off, fast, middle, slow), `22` NB, `44` compressor, `50` dial lock | M |
| `19 00` | Identity: `70` | H: the value Hamlib takes for a G90. Not in M |
| `1A 06` | Data mode switch: U-D and L-D are USB and LSB with it on. Reads back as two bytes, the second always `00` | H (Hamlib selects data modes this way); the second byte from a note in Xiegu's X6100 CI-V table seen only as a search-result quotation |
| `1C 00`, `1C 01` | PTT; tuner off, on, tune | M, R |
| `1D 19` | Xiegu's identity command on newer firmware: `00 90` | H (master tries it first) |
| `21` | RIT | H (master sets it on a tested G90). Not in M |
| `25`, `26` | Frequency and mode of either VFO | I, R: see below |
| `10`, `18`, `1A 03`, `1A 05` | Refused | Not in M, and Hamlib does not send them to a G90 |

**Commands 25 and 26.** They are not in [M], and [I] shows firmware 20240504.8 answering `25` wrongly. But Hamlib tries them on a G90, and if they are refused its first mode command of every session fails: `rigctl -m 3088` returned "Command rejected by the rig" for the first `m` when this personality refused them. So the personality answers them, correctly. A program that works here may meet the radio's faults there.

**Known differences from the radio.** None stops Hamlib; all are recorded so that a surprise on the real station has a first suspect.

- The radio echoes each command before answering it [I]. The mock does not, like an Icom with echo off; Hamlib copes with either.
- The radio answers PTT (older firmware) and the attenuator with the command itself, not `FB` [H]. The mock answers `FB`, which Hamlib accepts in both versions read.
- The attenuator command toggles whatever its data [M] and reads back `0C` for on [H]. The mock stores the byte it is given, which is what Hamlib 4.7.1 (`12`) and master (`01`) each expect to read back.
- Some of the radio's replies hold bytes that are not BCD: full power as `5F`, some AF and SWR values [H]. The mock's replies are always BCD.
- The radio's address is `70` by default and it answers `88` and others [M]; [I] shows one answering `A4`. The mock answers any address.
- Hamlib 4.7.1 sets the G90's AGC with numbers [M] does not give: `16 12 02` for fast, `05` for medium, `03` for slow, `00` for off (logged Nov 21, 2026), where [M] has `01` fast, `02` middle, `03` slow. The mock stores whichever byte arrives and reads it back, so Hamlib is content. The [front panel](#front-panel) names the values by [M], and shows `05` as a number. What the radio does with `05` is not known.

**Not checked.** Only Hamlib 4.7.1 has been run. Hamlib master differs for the G90 in ways read from its source and covered by unit tests, not by running it: it addresses `88`, tries `1D 19` first, sends `11 00`/`11 01` for the attenuator and `21 00` for RIT. No Android or desktop program that speaks to a G90 directly has been tried, and nothing has been compared with a real G90.

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
| `02` | Keep the settings as they are now as the power-up values, in flash | None. Refused if the mock has nowhere to keep them or the write fails |
| `03` | Forget the kept settings: the power-up values are the build's again | None. The settings in use do not change |
| `04` | Which power-up values are in force, read only | `00` the build's, `01` kept ones |
| `10` | Receive route | `00` monitor, `01` radio, `02` parrot (refused if the parrot has no buffer), `03` link (refused if the mock was built without one) |
| `11` | Noise | `00` off, `01` on |
| `12` | SNR | Tenths of a dB, 2 bytes plus sign: −60.0 to +80.0 |
| `13` | Bandwidth the SNR is quoted in | Hz, 3 bytes: 100 to 24000 |
| `14` | Noise floor | Tenths of a dB below full scale, 2 bytes: 12.0 to 90.0 |
| `15` | Attenuation | Tenths of a dB, 2 bytes: 0 to 120.0 |
| `16` | Turnaround time | ms, 2 bytes: 0 to 5000 |
| `17` | Fading | `00` off, `01` mpg, `02` mpp, `03` mpd, `04` mps, `05` nvis-d, `06` lowlat-d, `07` hilat-m, `08` hilat-d, `09` custom (`18` and `19`) |
| `18` | Custom fading: Doppler spread | Hundredths of a hertz, 2 bytes: 0.01 to 30.00 |
| `19` | Custom fading: delay of the second path | Tenths of a millisecond, 2 bytes: 0 to 10.0 |
| `1A` | Frequency offset | Tenths of a hertz, 2 bytes plus sign: −200.0 to +200.0 |
| `20` | Status, read only | Flags (`01` receive muted, `02` gain limited); level of the receive source in tenths of a dB below full scale, 2 bytes (`99 99` if nothing has been measured); gain the channel applies, tenths of a dB, 2 bytes plus sign |
| `30` | Play the parrot's recording again, now | None. Refused unless the route is parrot, there is a recording and PTT is off |
| `31` | Play clip N: a recording kept in flash replaces the parrot's and is replayed | N, 1 BCD byte from `01`. Refused unless the route is parrot, PTT is off, clip N exists and no clip operation is under way |
| `32` | Parrot delay, from unkeying to the replay | Tenths of a second, 2 bytes: 0 to 60.0 |
| `33` | Parrot period: the replay starts a whole number of periods after key-on | Tenths of a second, 2 bytes: 0 (off) to 300.0 |
| `34` | Parrot status, read only | State (`00` idle, `01` recording, `02` waiting, `03` playing); flags (`01` the buffer filled and the recording is cut short); length recorded and buffer capacity, each in tenths of a second, 2 bytes |
| `35` | Keep the parrot's recording as clip N, replacing what was there | N. Refused if there is no recording or one is being made. Answered when it has begun |
| `36` | Erase clip N | N |
| `37` | Clip status, read only | Flags (`01` an operation is under way, `02` the last one failed); the number of clips; then each clip's length in tenths of a second, 2 bytes, 0 for none |
| `40` | Link buffer: audio held before it is played | ms, 2 bytes: 10 to 200 |
| `41` | Link frequency match | `00` off, `01` on: the other board is heard only when it transmits on this radio's receive frequency |
| `42` | Link shared channel | `00` off, `01` on: two linked boards keep one set of path settings (`11` to `15`, `17` to `19`) and fade alike |
| `50` | Control-line PTT | `00` off, `01` RTS keys, `02` DTR keys |
| `51` | VOX | `00` off, `01` on |
| `52` | VOX threshold | Tenths of a dB below full scale, 2 bytes: 6.0 to 80.0 |
| `53` | VOX hang time | ms, 2 bytes: 0 to 5000 |
| `54` | PTT status, read only | The sources keying the radio now: `01` CAT, `02` control line, `04` VOX, `08` the key on the web console's front panel, added together; `00` is receiving |
| `60` | The radio imitated | Its Hamlib model number in BCD, 2 bytes: `30 85` the IC-705, `30 88` the G90. Setting it switches radios; the new one starts from its power-up state. Refused for a model the mock does not have |

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
mockctl.py -p PORT fade=mpp noise=on snr=5  a poor HF path, 5 dB SNR on average
mockctl.py -p PORT fade=custom doppler=0.5 fadedelay=1.5
mockctl.py -p PORT rig=g90                  become a Xiegu G90 (rig=ic705 to go back)
mockctl.py -p PORT vox=on keep              power up with the VOX on from now on
mockctl.py -p PORT forget                   power up with the build's settings again
mockctl.py -p PORT clip-save=1              keep the parrot's recording as clip 1
mockctl.py -p PORT route=parrot clip-play=1 replay clip 1 as receive audio
```

Limits, as built:

- Settings live in RAM and are written to flash only when asked (`02`, or `keep`). A power cycle returns to the power-up values, which are the kept ones if there are any and the build's (menuconfig) if not; `01` returns to them without a power cycle. The radio imitated is kept in flash whenever it is changed, and `01` leaves it alone.
- The controls share the CAT port, so they can only be sent while no other program is using it, or by the program that is. A reply read by a second program on the same port is lost to the sender and may confuse the other. A phone app holding the port leaves no way in at all; the [web console](#web-control-and-telemetry-console) is the way in then.
- `rigctl`'s `w` command sends a frame to `F0` (seen in the pseudo-terminal log) but printed no reply with Hamlib 4.7.1, so it can set but not usefully read.
- Verified on the host only: unit tests through the real parser and engine, and `mockctl.py` against the parser over a pseudo-terminal. Not run on a board.

## PTT

Four sources:

| Source | Trigger |
| --- | --- |
| CAT | CI-V `1C 00 01` keys, `1C 00 00` unkeys |
| Control line | RTS (or DTR) asserted, Digirig style |
| VOX | Transmit audio level above a threshold |
| Front panel | The PTT key on the web console's [front panel](#front-panel): an operator keying at the radio itself |

**Control-line PTT is a selectable mode, off by default.** The IC-705 works the same way: keying from DTR or RTS is a menu setting (USB SEND) rather than always on. Hosts raise DTR and RTS when they open a serial port and leave them high unless that line is the configured PTT. Data2G does this explicitly, and desktop operating systems do it for Hamlib. If the firmware always treated RTS as PTT, a host using CAT or VOX keying would hold the mock radio in transmit for the whole session.

While keyed, the receive stream carries silence or a low noise floor, as a real radio mutes receive during TX.

As built (host-tested, not yet run on a board):

- **Any source keys.** `audio_engine` holds all four: the radio transmits while CAT, the selected control line, the VOX or the front panel's key keys it, and unkeys when none does. CAT keying always works; the control line and the VOX are settings, off at power-up, changed with the [mock-only controls](#mock-only-controls) or at build time.
- **Front panel's key.** It is not a setting and is not kept: it is released at power-up and when the radio imitated is changed, and it stays down until the page releases it, so a browser that goes away leaves the radio transmitting, as a stuck microphone would. It exists to test what a controller does when the radio is keyed from somewhere else: CAT reads transmit, and a CAT unkey is answered `FB` and changes nothing.
- **Control line.** The setting names the line: off, RTS or DTR. The firmware passes every line-state change from the host to the engine, which ignores the lines unless one is selected.
- **VOX.** A transmit sample at or above the threshold (default −40 dBFS) opens it, and it closes the hang time (default 100 ms) after the last such sample. It is judged once per audio block: the block the audio starts in is already keyed, so the parrot records the start of the transmission. With no audio arriving at all the hang time still runs out. Changing an unrelated setting does not disturb an open VOX.
- **What CAT reports.** A read of `1C 00` says transmit whichever source keyed, the front panel's key included. The core keeps CAT's own PTT apart from the others', so neither clears the other; the firmware refreshes the others' state just before it feeds the parser each batch of serial bytes.
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

**Clips: recordings kept in flash (step 14).** Built as below; host-tested, and not yet run on a board.

- **What a clip is.** The parrot's recording, 12 kHz 16-bit mono, kept in flash so that it survives a power cycle: a transmission worth keeping, or a signal to test a decoder with. A canned clip gets in the same way as any other: play it into the mock from the host with the radio keyed in the parrot route, then keep the recording.
- **Three operations.** Keep the recording as clip N; play clip N, which puts it in the parrot's buffer in place of the recording and replays it at once through the channel like any replay; erase clip N. Playing needs the parrot route and PTT off.
- **Storage.** A flash partition of its own (`clips`, 13.9 MB of the 16 MB), cut into equal slots with no file system: five clips of up to 120 s. Each slot is one 4 KB sector for a header (a mark, the format version, the sample rate, the length, and CRC-32s of the samples and of the header) and then the samples.
- **Safe against a power cut.** Saving erases what the clip needs, writes the samples and writes the header last, so a save cut short leaves an empty slot. Loading checks both CRCs; a damaged clip is not played.
- **Slow, and not for a session.** Keeping a clip erases and writes up to 2.9 MB of flash, which takes seconds to tens of seconds, and every flash operation holds up everything else on the chip for milliseconds at a time. It runs in a task of its own at the lowest priority, the command is answered when the work has begun, and the status says when it is done. Audio is likely to be disturbed meanwhile: do it between sessions.
- **Borrowing the buffer.** The clip task and the audio task share the parrot's buffer by a hand-over in the engine: the clip task asks, the audio task lets go at its next block and neither records nor replays until the buffer is given back. A transmission made during the hand-over is not recorded.
- **Not automatic.** The last transmission is kept only when asked. Writing megabytes of flash after every transmission would wear the flash and break up the audio of the next one.
- **Limits.** Clips can only come from the parrot and go to it: there is no way yet to upload or download one as a file. A clip longer than the parrot's buffer plays as far as it fits. No wear levelling: a slot is rewritten only when something is kept in it.

**Resampling.** One 96-tap low-pass serves both directions: flat within 0.01 dB to 4 kHz, 3 dB down at 5 kHz, and at least 69 dB down from 6.5 kHz, so the 12 kHz path carries roughly 0–4.5 kHz. A 48 → 12 → 48 kHz round trip delays audio by 92 samples at 48 kHz, about 1.9 ms. Coefficients come from `host/tools/gen_resampler_fir.py`.

**SNR convention.** Noise is white across the whole sampled band, and SNR is quoted in a reference bandwidth, as HF software does: 2500 Hz for WSJT-X, 3000 Hz for VARA and most channel simulators. The mock's SNR setting defaults to 2500 Hz, which is also what Data2G reports its SNR in (`SNR_REF_BW_HZ` in its source); the bandwidth is a setting.

**Channel simulation (applied on the receive side)**

- Additive noise at a configurable SNR, so ARQ modems step down through their speed levels.
- Attenuation, fading and a small frequency offset.
- TX→RX turnaround delay of a few tens of ms, to mimic a real radio switching.

As built in `audio_engine` (host-tested, not yet run on a board):

- **The noise floor is fixed and the signal is moved.** Noise sits at a set level (default −40 dBFS RMS across the whole 0–24 kHz band) whether or not a signal is present, as band noise does. The engine measures the level of the arriving signal and scales it to sit at the configured SNR, so the SNR comes out as set whatever level the sending modem drives at. Deriving the noise from the signal instead would clip at negative SNRs: a signal at −10 dBFS and −20 dB SNR would need noise far above full scale.
- **Limits.** The signal is never raised above −12 dBFS RMS; an SNR too high for the floor (above about +38 dB with the default floor) lowers the floor instead. A quiet signal is amplified by at most 30 dB; beyond that the SNR comes out lower and the engine's status says so. Measured on the host, the SNR lands within 0.1 dB of the setting from −30 to +25 dB.
- **Level measurement.** The signal level is the mean square of its loud parts, measured in 1 ms pieces (anything under −60 dBFS is ignored), as a plain average for the first second and a running average over about a second after that. 200 ms of silence ends a transmission and the next one is measured afresh. The gain therefore settles within the first few milliseconds of a transmission and moves little after.
- **Attenuation** applies on top: with noise off it is a plain loss; with noise on it lowers the signal, and so the SNR, by that many dB.
- **Routes.** *Monitor*: receive is always open and PTT is ignored; with noise off and no attenuation this is the Milestone 1 loopback, bit for bit. *Radio*: receive is silent while keyed and for the turnaround time after unkeying (default 30 ms), then carries the receive source through the channel. PTT is sampled once per audio block, so the turnaround can run up to 10 ms long. The radio route has no receive source, so the host hears the noise floor only. *Parrot* behaves like radio and adds the replay as its source.
- **Settings:** route, noise on/off, SNR, reference bandwidth, noise floor, attenuation, turnaround time, and the parrot's delay and period. Their power-up values are build-time (`idf.py menuconfig`, "Mock radio: receive route and channel at power-up"); the [mock-only controls](#mock-only-controls) change them at run time, and the engine takes a change at the next audio block.
- **Precision.** The channel's gain has 24 fractional bits and signal and noise are summed before rounding, so a signal of a few counts at −30 dB SNR keeps its level and is dithered by the noise.

**Fading and frequency offset (step 30).** As built in `audio_dsp` and `audio_engine`; host-tested, and not yet run on a board.

- **The model** is the one Data2G's simulator uses (`data2g/hfchannel.py`), which is the Watterson model as ITU-R F.1487 has it: two paths of equal mean power, the second delayed, each multiplied by a complex gain that wanders as a Rayleigh process with a Gaussian Doppler spectrum. The Doppler spread is two standard deviations of that spectrum.
- **Conditions.** Named as Data2G names them, with its numbers, so a figure quoted there means the same here:

  | Name | Doppler spread | Delay | What Data2G calls it |
  | --- | --- | --- | --- |
  | `mpg` | 0.1 Hz | 0.5 ms | good |
  | `mpp` | 1 Hz | 2 ms | poor |
  | `mpd` | 2 Hz | 4 ms | disturbed |
  | `mps` | 0.15 Hz | 2 ms | slow and selective, measured on a 4000 km path |
  | `nvis-d` | 1 Hz | 7 ms | mid-latitude disturbed NVIS (F.1487) |
  | `lowlat-d` | 10 Hz | 6 ms | low latitude disturbed (F.1487) |
  | `hilat-m` | 10 Hz | 3 ms | high latitude moderate (F.1487) |
  | `hilat-d` | 30 Hz | 7 ms | high latitude disturbed (F.1487) |
  | `custom` | 0.01 to 30 Hz | 0 to 10 ms | the `doppler` and `fadedelay` settings |

- **Where.** In the receive channel, on whatever the route hears (the parrot's replay, the other board, or the loopback), before noise is added. It works at 12 kHz: the audio is brought down, made complex by a Hilbert transformer (127 taps, the image 60 dB down from 200 Hz to 5.8 kHz), faded, and brought back up. With fading or an offset on, receive audio is limited to 0–4.5 kHz and comes 7.25 ms later; the parrot starts its replay that much sooner, so slot alignment is kept.
- **SNR is the average.** The signal's level is measured before the fading and the gain set from that, so the SNR setting is the SNR at the path's mean power and a fade is a real drop below it, as in Data2G's simulator. The level measurement does not chase the fades.
- **Headroom.** With fading on, the signal's average is kept at −18 dBFS or below (−12 without), because the two paths sometimes add up; an SNR too high for the floor lowers the floor. With noise off there is no such scaling, and a loud signal's fading peaks clip.
- **The gains are a function of seed and time,** not of what went before: noise from a hash of (seed, path, sample number), shaped by one 59-tap filter at 32 samples per hertz of spread and interpolated. A stage can be put anywhere in its fading, and two stages with the same seed, settings and time fade alike to the sample. That is what lets two boards share a path. Data2G's generator shapes the whole session at once with an FFT; this one is a running equivalent, checked by its statistics, not against Data2G's output.
- **Frequency offset** moves the receive audio up or down by up to 200 Hz, with or without fading. It is each board's own setting.
- **Checked on the host:** the Rayleigh distribution of the envelope (the fraction of time 10 and 20 dB down and 3 dB up), unit mean power, the correlation over time that a Gaussian spectrum of the set spread gives, the correlation across frequency that the set delay gives, and that the result does not depend on block size.
- **Cost.** Estimated at a few million multiply-adds per second, some percent of one core. Not measured on the chip.
- **Not included** from Data2G's simulator: impulsive clicks, a wandering noise floor, interference, and sample-clock error (two real boards have that already).

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

**Shared channel (step 31).** Each board applies the channel to what it receives. For the two directions to be one path, the boards agree on it (`coord`), over a third kind of packet sent to the peer with each announcement:

```
state:  4D 13 <boot id, 4> <version, 4> <nonce, 4> <clock ms, 4> <seed, 4> <n> then n times <id> <value, 4>
```

- **Settings.** Noise on or off, SNR, its bandwidth, the noise floor, attenuation and the fading are the pair's. Each board's set has a version that goes up with every change made on that board; the higher version wins, so a change on either board reaches the other within half a second. When two boards meet with the same version (two that have not been touched since power-up, say), one set is picked by a number each board draws at start-up. Everything else stays each board's own: route, turnaround, the parrot, PTT, the link's buffer, the frequency offset.
- **Fading.** Both boards fade from one seed and one clock. The board with the higher start-up number leads; the other takes its seed and steers its own clock to the leader's, a millisecond per 20 ms tick, or at once if it is more than 200 ms out. If the leader goes away the follower carries on from where its clock stands.
- **Noise stays local.** Each board makes its own noise floor, because a receiver hears noise whether or not anyone is transmitting and the link carries nothing between transmissions. Only its settings are shared.
- **Nothing from a peer is trusted.** Values go through the same range checks as a command over CAT, only the path's settings are accepted, and what a board cannot use it leaves as it was.
- **Switch.** `share` (control `42`), on by default. Off on either board, each has its own channel, and the two directions can be set differently on purpose.
- **Checked on the host:** two boards whose clocks start a quarter of an hour apart and run 100 ppm apart agree on the time within 3 ms for five minutes; settings follow in both directions; and the two receivers' fading (2 Hz spread) correlates above 0.98 when shared and not at all when not.
- **Limits.** The clocks agree to a few milliseconds at best, less well over a real transport than in the test, so at the fastest fading (10 to 30 Hz, where the path changes in tens of milliseconds) the two directions fade alike only roughly. The versions count changes, not time: "newer" means more changes since power-up.

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
- A frequency offset set on both boards moves both directions the same way. Between two real radios the offsets are opposite; set them with opposite signs to imitate that.
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
| `POST /api/rig` | Change the radio imitated: `{"hamlib":3088}` |
| `GET /api/radio` | The radio as its front panel shows it: both VFOs, mode, split and the rest, and every level, function and meter with its name |
| `POST /api/radio` | A change made at the front panel: `{"freq":7074000,"mode":"LSB"}`. Answers with the radio as it is now, or with an error and nothing changed |
| `POST /api/reset` | Back to the power-up settings |
| `POST /api/keep`, `POST /api/forget` | Keep the settings as they are for power-up, in flash; forget them again |
| `POST /api/clip` | A clip operation: `{"op":"save","clip":1}`, with `op` one of `save`, `play`, `erase`. Answered when it has begun; the telemetry says when it is done |
| `POST /api/replay` | Play the parrot's recording again |
| `GET /api/telemetry` | One telemetry snapshot |
| `GET /ws` | WebSocket: the same snapshot, pushed five times a second |
| `GET /ws/scope` | WebSocket: the [waterfall](#waterfall), a row of spectrum ten times a second, as bytes, for as long as the connection is open |
| `GET /api/health` | The board's health: memory, processor load, the time the audio work takes, Wi-Fi drops, the last table of tasks |
| `POST /api/tasks` | The same, with the table of tasks read afresh. The body is `{}` |
| `GET /api/wifi`, `POST /api/wifi` | How the board is connected; save a network to join (`{"ssid":...,"password":...}`) and restart |

**Settings.** The names and units are the ones `mockctl.py` uses (`snr=-12.5` there, `"snr":-12.5` here). `console_api` holds only the names: for each it names the `mock_ctl` command that carries the setting, and it reads, range-checks and writes the value through `mock_ctl`'s own functions. So there is one table of ranges and one set of power-up values, a change made on either path shows on the other, and values keep the CI-V controls' steps (tenths of a dB and of a second). A value out of range is refused, not clamped, and a request with one refused value changes nothing.

**Telemetry.** A snapshot holds: PTT and which sources key it; a number that changes when the radio's own state does (`rgen`, see [Front panel](#front-panel)); the CAT frequency, mode and split, and the CI-V frame counts; the level (RMS and peak over 0.1 s) of the audio the host plays and of the audio it records, each absent when the host is not streaming that way; the audio FIFO's fill, target and its overrun, underrun and drift-steering counts; the receive channel's measured source level, gain and mute; the parrot's state and recording length; the Wi-Fi signal strength; and a number that changes when the settings do, however they were changed, so the page knows to read them again. With the link built in, the link's state and statistics are in it too (see [Two-board link](#two-board-link)).

**Board health.** What a bring-up asks about the board and not about the radio. It is asked for on its own and is not in the telemetry, so the five-times-a-second stream stays as small as it was; the page asks every 2 s, and only while its health section is open. A reading holds:

- why the board last restarted (power on, reset pin, crash, watchdog, brownout), which the page shows even with the section closed;
- internal memory free now, the least there has been since start, and the largest single free piece; PSRAM free;
- how busy each processor core was since the last reading, from the run time FreeRTOS keeps for each core's idle task;
- the audio work: the time `audio_to_host` takes for one block (the FIFO, the engine with its channel and fading, and in the link route the packets sent and taken), averaged over each second, the longest block in that second and the longest since start, and that time as a share of the time the block's samples last. This is the number that answers what fading costs;
- how many times a joined Wi-Fi network was lost;
- the number of tasks, and the table of tasks as last read: each one's stack never used, in bytes, and its share of a core between the last two readings of the table.

The table of tasks is the one part that is not free. FreeRTOS reads it with everything else on the board held up while it looks through every stack for the high-water mark: estimated at a millisecond or so, not measured, and long enough to cost a USB audio packet. So the table is read only when the page's "Read every task" button asks (`POST /api/tasks`), each reading says how long it took and how old it is, and the regular reading touches only counters. The same readings go to the log on the COM jack every 30 s (`MOCK_HEALTH_LOG_S` in menuconfig, 0 for never), without the table unless `MOCK_HEALTH_LOG_TASKS` asks for it, so a build with the console off can be judged too. The per-core and per-task figures need FreeRTOS's run-time statistics (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`, on in `sdkconfig.defaults`), which cost one clock reading at each task switch; a build without them reports the rest. The collecting is `main/health.c`; the layout of a reading and the timing arithmetic are in `console_api`, host-tested.

**Page.** One file of about 59 KB, embedded in the firmware, with nothing loaded from anywhere else, because the board may be on a network with no internet. It builds its settings form from `/api/info`, so a setting added in `console_api.c` appears without an edit to the page. A choice of up to four values is a row of buttons and a longer one a list; where the values' own words say too little, as with the fading conditions, `/api/info` gives a label for each ("ITU high latitude disturbed: hilat-d (30 Hz, 7 ms)") and the page shows that, while the value sent stays the word. A setting that is in force only while another has one value says so in `/api/info` (`"when":{"fade":"custom"}` on the custom fading's Doppler spread and path delay), and the page greys it out and will not take input for it the rest of the time; it keeps its value, and `mockctl.py` and the CI-V controls can still set it. A status line shows the same states as the LED; a strip chart shows the last 60 seconds of both levels and of PTT, kept in the browser only.

**On the host.** `make -C host console-sim` serves the page at `http://localhost:8080` with a simulated radio behind it: `console_api`, `mock_ctl` and the audio engine as compiled for the host, with a pretend host that keys and plays a tone for 6 s in every 15. `make -C host radio-sim` is the same without the pretend host. Each simulated board also has a CAT serial port, a pseudo-terminal linked at `host/build/cat-a` (and `cat-b`), which reaches the same parser as a board's USB serial port, so Hamlib or any CAT program can drive the radio the page shows. `make -C host console` checks the interface through the same simulator, that port included. The simulator's HTTP layer is a Python stand-in for `web_console.c`, so neither tests that file.

**Limits.**

- No login and no encryption: it is a bench device on a private network. A Wi-Fi password typed into the form crosses that network in the clear. Requests that change anything must be sent as `application/json`, which a page on another site cannot make a browser do, so such a page cannot change settings behind the operator's back.
- Once the board has fallen back to hosting its own network it stays there until restarted, even if the configured network returns.
- Settings are stored only when asked ("Keep these for power-up"); the Wi-Fi network and the radio imitated are stored when changed.
- The page's history is 60 seconds and lives in the browser; nothing is recorded on the board.
- Telemetry is sampled five times a second, so a transmission shorter than 0.2 s can fall between two snapshots.
- Wi-Fi traffic shares the radio with ESP-NOW link audio, so the console keeps its traffic light: about 3 KB per second per open page, up to 1 KB more while its health section is open, and 1.3 KB more for each direction of audio streaming while its waterfall is open.
- The health readings measure the board, not the host: a late or lost USB packet shows as an underrun in the telemetry, not here. Each reader (the page, the log) gets rates since it last asked, so two browsers with the health section open each see shorter intervals.
- The console is on by default and can be switched off in menuconfig, which keeps Wi-Fi off altogether.

### Front panel

A section of the page that shows the radio imitated as an operator standing at it would have it, and lets them change it there. The settings above belong to the mock; these belong to the radio. It exists to exercise rig-control software with no radio on the bench: Hamlib, and through it a rigctld facade such as Data2G's, can be watched setting the radio, and the other half of their job, noticing what changed at the radio, can be tested by changing it.

**Generic, not a drawing of one radio.** The panel is built from what the personality lists, as the settings form is built from `/api/info`: the page knows no radio. An IC-705's face would promise a scope and menus the mock does not have, a G90's could not be drawn from a radio, and one page serves both and any personality added later. The idea is grig's, the Hamlib front panel that looks the same for every radio, turned round: there the program is the controller, here the page is the radio.

**What it shows and sets.**

| On the panel | Sets it with | Notes |
| --- | --- | --- |
| Frequency of the selected VFO: a digit steps by its place (click its upper or lower half, the wheel, the arrow keys), or a number typed in kHz | `05` | The digits are as many as the radio's highest frequency has. The other VFO's frequency and mode are shown beside it |
| Selected VFO, A = B, A ⇄ B | `07 00`/`01`, `07 A0`, `07 B0` | A = B only where the radio has it |
| Mode, data mode, filter | `06`, `1A 06`, `06 <mode> <filter>` | The personality's own modes |
| Split, repeater shift | `0F 00`/`01`, `0F 10` to `12` | |
| RIT and its frequency, tuner, attenuator | `21 01`, `21 00`, `1C 01`, `11` | The attenuator only where the personality says which byte is "on" (`att_on`); otherwise the byte CAT left is shown |
| Every level | `14 <sub>` | A slider, 0 to 255, as CAT carries it |
| Every function | `16 <sub>` | A switch, or a list where the personality names the values |
| Every meter | none: CAT only reads them | A slider, 0 to 255, or a switch for the one-byte statuses. There is no RF, so the operator sets what the radio would measure |
| PTT key | none | A PTT source of its own; see [PTT](#ptt) |
| Tuning step, filter width, power | not set from the panel | Shown as CAT left them |

**One set of rules.** Everything CAT can set is set from the panel by running the CI-V command itself through the parser (`civ_local`), with nothing sent on the serial port and nothing counted as a CAT frame. So the panel cannot set what CAT would refuse, the G90's own handlers apply to it as they do to CAT, and a controller reads back exactly what the panel set. `console_api` only turns names into commands. A request is applied whole or not at all.

**Names come with their source.** The names of levels, meters and functions are in the personality's tables (`civ_item_t.name`), beside the numbers. A function's values are named only where there is a source: for the IC-705, AGC (`00` to `03`: off, fast, mid, slow), the preamplifier (`00` to `02`) and break-in (`00` to `02`: off, semi, full) are what Hamlib 4.7.1 was logged sending; for the G90, AGC is the manual's (`00` to `03`: off, fast, middle, slow). Every other function is a plain switch on the panel. CAT can still set any byte, and a value the panel has no name for is shown as its number. The IC-705's meter scales (which reading is S9, or an SWR of 2) are not on the panel for the same reason: Icom's CI-V guide could not be fetched when this was written, so the meters are in the raw 0 to 255 that CAT carries.

**Knowing when to read again.** The page reads `/api/radio` (about 3.2 KB for the IC-705) only when the radio's state has changed. The telemetry carries a hash of that state (`civ_state_hash`: frequencies, modes, levels, functions, meters and the rest of what CAT can read back, and which radio it is), computed five times a second. A hash, not a counter of commands, because WSJT-X and others set the same frequency and mode over and over: a counter would have the page reading again several times a second, and Wi-Fi traffic is to stay light beside ESP-NOW. PTT is not in the hash; the telemetry already carries it.

**Limits.**

- The panel shows what the mock implements, which is less than Hamlib's IC-705 backend can ask for: no memories, scanning, CTCSS tones, repeater offset, Morse sending or parameters (beep, backlight, time). Those are refused over CAT as before.
- A change at the panel is not announced on the CAT port. A real IC-705 with CI-V transceive on sends a frame when its dial is turned; the mock sends nothing unasked, so a controller learns of a change when it next reads.
- No meter follows the audio. The S-meter is whatever the panel last set.
- The panel's key stays down until the page releases it.
- The meters and function values carry no units or names beyond those sourced above.
- Tried in a desktop browser against the simulator only, with Hamlib 4.7.1 and with Data2G's facade driving it. Not on a board, not on a phone's browser.

### Waterfall

A section of the page that shows the spectrum of the audio each way, scrolling downwards: what the host records (receive, after the channel) and what it plays (transmit), side by side, 0 to 3 kHz, the last 30 seconds. It is an audio display, not a band scope: there is no RF. What it is for is seeing what the channel does to a signal (noise at the set SNR, the mute while keyed, fading across the passband, a frequency offset) and what a modem actually sends, without a second program on the host.

**Where the work is done.** On the board, and not in the audio path.

| Step | Task | Cost |
| --- | --- | --- |
| Keep the newest samples of each direction in a ring (4096 samples) | The two audio tasks | A copy of each block, whether or not anyone is watching |
| Take the newest 2144 samples, bring them to 12 kHz with the resampler's filter, window (Hann), transform (512 points), and turn 128 bins into bytes | The console's HTTP task, below the audio tasks in priority | Estimated well under a millisecond a direction on the S3 (10 µs on the development Mac), ten times a second, and only while a page has the waterfall open |
| Send the row | The same task | 4 bytes and 128 for each direction streaming |

Sending the audio for the browser to transform would be 24 KB a second beside the link's audio on the same radio; rows are 1.3 KB a second for each direction. The page opens a WebSocket of its own (`/ws/scope`) when its waterfall section is opened and closes it when the section is closed, so a console left open costs nothing more than before. The server marks such a connection in its session, sends rows to those and telemetry to the rest, and asks for no rows at all while there are none.

**A row.** Bytes, not JSON:

```
byte 0   format, 01
byte 1   flags: 01 a receive lane follows, 02 a transmit lane follows, 04 the radio is transmitting
byte 2   bins in a lane, 128
byte 3   00
then each lane that is there, receive first: one byte for each bin, the level
in half decibels below full scale (0 is 0 dBFS, 240 is -120 dBFS, silence)
```

Bin k is at k × 12000 / 512 Hz, 23.4 Hz apart; `/api/info` gives those numbers and the rows a second, so the page hard-codes none of them. A lane is left out while the host is not streaming that way, and the page shows the gap in grey. Levels are on the level meters' scale: a tone reads in its bin what the RX or TX meter reads for it as RMS, up to 1.4 dB less between two bins. Noise reads lower than the meter by the ratio of a bin's width to the whole band: the default floor of −40 dBFS in 2500 Hz is about −58 dBFS in a bin.

**The page.** Each lane has one hue, the same as its level in the chart above it (blue for receive, orange for transmit), from the page's surface for −90 dBFS and below to the hue's deep end at −10 dBFS, so level reads as lightness in the light and the dark theme. Rows during which the radio was transmitting have a red tick beside them. Pointing at a cell gives its time, frequency and level.

**Limits.**

- 3 kHz and 23 Hz, fixed. Each row covers 43 ms of audio, and rows come every 100 ms, so more than half the audio between rows is never looked at: a burst shorter than 60 ms can be missed.
- The two lanes are transformed one after the other from rings read at slightly different moments, so they are not aligned to better than a row.
- 32 KB of internal memory, always, for the two rings and the transform's tables.
- Every page showing the waterfall holds a second connection, of the seven the server allows.
- Watched in a desktop browser against the simulator only. The simulator's host plays a single tone, so fading has been seen as a tone that comes and goes, not yet as notches across a wide signal.

## Bring-up on hardware

**First board, Nov 25, 2026.** An ESP32-S3-DevKitC-1 clone with the WROOM-1 N16R8 (chip revision v0.2, Boya flash, AP Memory octal PSRAM), its COM jack behind a WCH CH343 (`1a86:55d3`). Flashed through the COM jack at 460800 baud with the default build (web console and ESP-NOW link on). The RGB LED is a WS2812 on GPIO48 and works without bridging a pad.

- **Boot.** PSRAM found and tested, self-check passes, the parrot's 120 s buffer and the five clip slots fit, the USB device starts, Wi-Fi hosts `mockradio-xxxx` on channel 1, ESP-NOW starts. The health log reads about 112 KB of internal memory free (76 KB at its lowest), 5.3 MB of PSRAM free, core 0 under 3 % busy and core 1 under 10 % while audio streams both ways, and 0.26 ms for each 240-sample block of audio work (at most 0.86 ms).
- **macOS 15 (through a hub).** Enumerates as "Mock Radio" (VID `1209`): a serial port `/dev/cu.usbmodem<MAC>` and a sound card "Mock Radio Audio", 1 channel each way at 48 kHz.
- **CAT.** `rigctl -m 3085 -vvvv` with `f`, `F 7074000`, `m`, `M USB 2400`, `T 1`, `t`, `T 0` gives every answer with no Hamlib error, timeout or retry; the same with `-m 3088` after `mockctl.py rig=g90`. `mockctl.py` reads and changes the mock's settings over the same port.
- **Audio.** Measured with a sample-counting ramp played into the mock and recorded back (monitor route), so that any lost, repeated or reordered sample shows exactly: `host/tools/audio_loopcheck.swift` plays and records through CoreAudio, `host/tools/ramp_check.py` reports what changed. The loop is bit-exact at unity gain. In steady state the only change is the FIFO's drift steering, a single repeated or dropped sample every few hundred milliseconds. The first second after a stream starts has a few gaps while the FIFO fills, as designed.

**Fix: the audio component's tasks moved off Wi-Fi's core.** At first the playback direction lost one whole 1 ms packet every 20 to 30 s. Espressif's `usb_device_uac` passes each received packet from the USB interrupt to its speaker task through a single buffer, so a packet is overwritten whenever that task has not run within 1 ms, and at its default (priority 5, either core) it shared core 0 with Wi-Fi and lwIP. `sdkconfig.defaults` now puts the TinyUSB, speaker and microphone tasks on core 1 at priorities 12 to 14 and pins lwIP to core 0. Before: 5 packets lost in 80 s. After: none in more than 10 minutes of streaming.

**Web console on the board.** The board, hosting its own network, served the page to an Android phone; the page's Wi-Fi form gave it the home network, and after its restart it joined (−60 dBm) and answered at `mockradio-67f8.local` (the name took a few seconds to resolve the first time; the IP address answered at once). From a desktop browser:

- The page loads in about 0.2 s, the API answers in about 40 ms, the telemetry pushes, and the page reports no errors.
- A setting changed on the page (noise on) takes effect on the board: read back over CAT with `mockctl.py`, and seen in the waterfall as a noise floor in the receive lane only.
- The front panel follows `rigctl`'s frequency and mode, and `rigctl` reads what the panel tunes. The panel's key reads as transmit over CAT, and CAT's `T 0` does not release it (a `rigctl` process that has just sent `T 0` answers `t 0` from Hamlib's cache; a fresh one reads `t 1`).
- Costs, measured with the ramp loop test running: the page with telemetry and health open cost no audio in 150 s. Opening the waterfall raised the time per 240-sample audio block from about 280 to 340 µs and core 1 from about 11 to 25 % busy, and cost no audio. Noise on raised the time per block to about 680 µs (14 % of the block's time). Each read of the table of tasks (the health section's button) held the board up 1.1 ms and cost exactly one 1 ms playback packet, as expected; it is never on a timer.

**Fix: the waterfall's socket is marked after its handshake.** On the board the waterfall stayed at "waiting for the board". ESP-IDF v5.5.6's server answers a WebSocket handshake itself and does not call the URI's handler for it (the handler hears only later frames, with `req->method` left over from an earlier request), so the code that marked a connection as a waterfall at the handshake never ran: no rows were made, and the telemetry went down the waterfall's socket too. It now marks the connection in the server's post-handshake callback (`CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT`, in `sdkconfig.defaults`; the build stops without it), and the handlers no longer take a GET to mean the handshake. The host simulator does not run this server, which is why it passed there.

**Measuring audio on this Mac.** The Mac used for development is usually heavily loaded by other work (Data2G profiling: load average around 9). Two recorders there lose audio that the board delivered, and their losses look like the board's:

- `ffmpeg -f avfoundation` keeps one pending buffer and replaces it if it is not read in time: it lost whole 512-frame buffers, about 12 % of the audio.
- An AudioQueue recorder with 4 × 10 ms buffers lost 20 to 30 ms at a time every 10 to 30 s, filled with zeros in real time. A temporary probe on the board showed its USB IN buffer steady (690 to 706 of 2156 bytes, never full or empty) and capture blocks every 10.3 ms at most right through those gaps; with 16 buffers the gaps went away.

So, on a loaded host, check a glitch against the board before blaming the board, and give recorders generous buffering. One run also showed a 30 s stretch where samples shifted by ±2 positions every few milliseconds with none lost, which looks like CoreAudio's clock correction; it is not understood and is inaudible.

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

**Its rigctld facade** (branch `feature/rigctld` of the fork, read Nov 21, 2026) serves Hamlib's NET rigctl protocol over the radio Data2G controls, so that other programs can share it. On that date it passed frequency, mode and PTT for the current VFO, with VFO, split and common levels being added. It can be exercised against the mock with no radio and no board, using the simulator's CAT port and the [front panel](#front-panel):

```
make -C host radio-sim                                     the page at http://localhost:8080
data2g-rigctld --rig-model 3085 --rig-device host/build/cat-a --port 14532
rigctl -m 2 -r 127.0.0.1:14532                             f, F 7074000, m, M PKTUSB 0, t, T 1
```

Tried once that way on Nov 21, 2026 (Hamlib 4.7.1 as the client): frequency and mode set through the facade showed on the panel, and those set at the panel were read back through it. With the panel's key down, `rigctl` straight to the radio read PTT as 1 and the facade read 0, which is what its document says: it reports the PTT it owns, not the radio's.

To check on a phone:

- Each phone's USB audio driver accepts a mono-only playback device.
- Android grants serial access on a device that also has audio capture. Android is believed to require microphone permission first; Data2G requests it, so grant that before the USB prompt.
- Opening the serial port succeeds on a composite audio+serial device. Data2G's own comments note it can fail if another driver holds the device.

**Half an end-to-end path on a Mac, Nov 25, 2026.** (Data2G and Ragtime here, and in the two-station tests below, are the software that was at hand for testing end to end; the mock is not tied to them, and any modem and client that work with the real radio sit in the same places.) One station, through the first board: Ragtime (baitisj/ragtime main at `ddd9d60`) → `data2g-gui` → Data2G's rigctld facade → the board, with the board's sound card as Data2G's audio both ways and the board in the parrot route, so the station hears its own transmission. Data2G was built from a local branch, upstream `master` (`ba2b8b0b`) with only the facade's three commits added. The pieces:

- `data2g-rigctld --rig-model 3085 --rig-device <the board's CAT port> --port 4532` owns the CAT port.
- `data2g-gui` (or `data2g-host`) with `--rig-model 2 --rig-device 127.0.0.1:4532 --ptt-method cat --rig-mode pkt_usb`, and the board's sound card for input and output.
- Ragtime, with its usual settings: data2g-host at 127.0.0.1 (8300, 8100), and its dial read through rigctld at 127.0.0.1:4532, the facade.

A broadcast sent from Ragtime keyed the board through the facade, set USB-D, and went out as a 24.3 s burst in `fsk16r25-r1/2` (three codewords). The parrot replayed it, and Data2G decoded all three (score 0.98, SNR estimate 26.6 dB). Ragtime's dial menu tuned the board through the facade, and Ragtime's dial followed changes made on the console's front panel. The board's CAT counters showed no refused frame. With noise off, the parrot route gives Data2G exact digital silence between replays (it measured −300 dB), so its waterfall stays black, and the replay comes back at full scale and lights its CLIP warning, because Data2G puts a burst's peak at full scale and the mock passes it at unity. Turning the mock's noise on (−40 dBFS floor, signal held 10 dB above it) gives the waterfall a noise floor and keeps the signal below clipping; for work with Data2G, noise on is the realistic setting, and it has been the power-up default since. A second broadcast with noise on at 10 dB SNR (in 2500 Hz) went out as a 14.7 s burst and decoded in full (score 0.92), and Data2G estimated its SNR at 9.9 dB: the mock's SNR and Data2G's agree, as both quote it in 2500 Hz. Not yet tried then: a session rather than a broadcast, and the channel's noise and fading under a real exchange.

**Two stations over the link, Nov 25, 2026.** The second board, flashed from the Mac and plugged into an Android phone (Android 16) with its native USB jack, enumerated there as the mock radio (a C-to-C cable that worked; the first one gave the phone no device). Data2G's Android app, built from the same local branch as package `org.cleverdomain.data2g.rigctldtest` and installed over wireless ADB, ran it with CAT keying (Icom IC-705, USB serial) and the board's sound card. Both boards joined the home network (channel 1) and found each other at once (−50 dBm), both in the link route on 5.4035 MHz with the shared channel at 10 dB SNR. A broadcast from Ragtime on the Mac went out through board one, across ESP-NOW (2457 and then 3952 packets, none lost or late) and into the phone, whose Data2G decoded it (`fsk16r25-r1/2`, two codewords, SNR 9.8 dB). The other way, a KISS frame handed to the phone's Data2G through an ADB port forward (`adb forward tcp:18100 tcp:8100`) keyed board two by CAT, crossed the link (345 packets, none lost) and was decoded by Data2G on the Mac (`qpsk-r1/5`, two codewords, SNR 11.2 dB). On Android the rig settings appear only once Transmit keying is set to CAT. Then Ragtime on both ends: Ragtime for Android (main, 0.2.0 build 3) connected to the phone's Data2G on 127.0.0.1 as W0SIG-1, and a broadcast typed there reached Ragtime on the Mac (807 link packets, none lost; three codewords decoded at 11.4 dB), which listed W0SIG-1 as heard with its locator. A reply addressed `@W0SIG-1` from the Mac's Ragtime showed on the phone. Chat client to chat client, both ways, through two mock radios. Then a ragchew: Ragtime on the phone called W0SIG and Data2G connected on the first try (3 s); messages went both ways inside the session in seconds, with no resends, lost control frames or timeouts, and a 1,007-byte file offered from the Mac was accepted on the phone and saved there byte for byte (same SHA-256) about 14 s after the answer. The first try at the file failed: board one's CAT port wedged (below).

**Fix: the CAT port wedged; the TinyUSB task now runs above the audio tasks.** Twice, board one went on receiving CAT frames (its counter rose) while no reply reached the Mac, even with the port opened afresh, so Hamlib timed out on every read and Data2G could not key; audio kept flowing, and only a restart cleared it. The first time it coincided with cables moving on the hub, the second time with nothing but a ragchew, just after the board recorded a 12.3 ms audio block. TinyUSB's interrupt hands each finished transfer to its task through a queue of 16 events and drops what does not fit (`osal_queue_send` from the interrupt fails silently); the three audio endpoints finish a transfer every millisecond, so about 5 ms without the task running overflows it, and a dropped completion on the CDC IN endpoint leaves that endpoint busy for good. The earlier fix (above) had put the TinyUSB task at priority 12, below the speaker (14) and microphone (13) tasks on the same core, so a long audio block kept it from running. It is now at 15, above both, and the queue holds 64 events (`CFG_TUD_TASK_QUEUE_SZ` in `tusb_config.h`, about 20 ms) for what nothing can preempt, such as a flash write. With that, a ragchew with a file ran with no CAT timeout and a worst audio block of 3.7 ms.

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
6. Enumeration check on macOS, Linux and Windows. macOS 15 done on Nov 25, 2026 (see [Bring-up on hardware](#bring-up-on-hardware)); Linux and Windows remain.
7. Written, not yet on hardware: CDC wired to the parser. Line-state changes go to the engine, which keys from them only when a PTT line is selected (step 13).
8. Written, not yet on hardware: audio loopback, USB OUT through a drift-steered FIFO (about 20 ms) to USB IN. It passes audio regardless of PTT, so a round trip can be checked with any recorder; muting receive while keyed belongs to parrot and link modes.
9. Acceptance: `rigctl -m 3085 -r /dev/ttyACM0 -vvvv`, then `f`, `F 7074000`, `m`, `T 1`, `T 0`, with no Hamlib errors; audio round-trips in Audacity or between two fldigi or WSJT-X instances. The `rigctl` part passed on macOS on Nov 25, 2026, as an IC-705 and as a G90, and audio round-trips bit-exact through CoreAudio; a round trip through a modem application remains.

**Milestone 2: parrot + channel simulation**

10. Written, not yet on hardware: PSRAM parrot buffer at 12 kHz in `audio_engine`, as a third route: record on PTT on, replay on PTT off after a configurable delay or aligned to a slot period, with host tests.
11. Written, not yet on hardware: channel on the receive path in `audio_engine`: noise at SNR, attenuation, receive mute while keyed and TX→RX turnaround delay, with host tests. Power-up settings are build-time; step 12 changes them at run time.
12. Written, not yet on hardware: mock-only controls in `mock_ctl` on CI-V address `F0`, which no radio in Hamlib 4.7.1 uses; a separate address keeps them clear of every radio's command set. Route, channel settings, parrot settings, replay and status so far, with host tests and `host/tools/mockctl.py`. Stored-clip, link, PTT-source and personality commands have numbers reserved and join with those parts.
13. Written, not yet on hardware: VOX PTT and control-line PTT in `audio_engine` beside CAT, both selectable and off at power-up, with host tests and mock-only controls; LED states (red recording, green playing, amber waiting, dim blue receiving, dim white no host) in `ui`, driven through RMT.
14. Written, not yet on hardware: flash persistence. Settings are kept as the power-up settings on request (`02`), and the parrot's recording is kept as one of five clips in a partition of its own and played back on request (`31`, `35`, `36`), with host tests of the packing and of the clip store. The firmware's NVS and partition code is compiled only.
15. Acceptance: WSJT-X FT8 self-decode with replay aligned to the 15 s slot.

**Milestone 3: web control and telemetry console**

16. Written, compiled only: Wi-Fi bring-up beside USB: join a configured network or host an access point, a name per board (`mockradio-xxxx`, also by mDNS), power save off.
17. Written: a JSON interface to the settings in `console_api`, through `mock_ctl`'s own settings functions, with host tests; the HTTP server around it is compiled only.
18. Written, tried in a desktop browser against the host simulator: control page embedded in the firmware: every setting, parrot replay, reset, and the Wi-Fi form.
19. Written: live telemetry over a WebSocket, five snapshots a second: PTT, frequency and mode, levels and gain, parrot state, audio FIFO and CI-V counters. The snapshot is host-tested; the push is compiled only.
20. Acceptance: change the SNR from a browser while WSJT-X or a phone app holds the CAT port, and watch levels and PTT live, with no added audio underruns. On Nov 25, 2026, with `rigctl` and a CoreAudio loop test in place of a modem application: noise switched on from the page took effect (read back over CAT, seen in the waterfall), the panel and CAT followed each other both ways, and the audio lost nothing. With a modem application: not yet.

**Milestone 4: two-board link**

21. Written: the link's core in `link` (packets, sending, receiving, announcing frequency and PTT), behind a send function and a receive call, with host tests; a fourth route in `audio_engine`. The monitor and parrot routes stayed where they were.
22. Written, compiled only: ESP-NOW transport on the channel Wi-Fi is using: 12 kHz 16-bit audio in 10 ms packets with sequence numbers; boards pair with the first other board they hear.
23. Written, host-tested: jitter buffer, silence in place of lost packets, and drift correction; link statistics in the console's telemetry and on its page.
24. Written, host-tested: frequency matching, with a setting to switch it off.
25. Written, compiled only: UART transport with framing and a check, as a build-time alternative.
26. Acceptance: two PCs running fldigi or VARA first, then two Android phones running Data2G in ARQ mode, stepping the SNR down from the web console to watch speed adaptation.

**Milestone 5: G90 personality**

27. Written, host-tested: run-time personality selection, switched by mock-only control `60`, by `mockctl.py` and in the web console. The firmware stores the choice in flash; that part is compiled only.
28. Written: `rig_g90` from the Radioddity CAT manual and Hamlib's `xiegu.c`, with host unit tests. `rigctl -m 3088` (Hamlib 4.7.1) runs against it on the host with no command rejected. Other Hamlib versions were read, not run.
29. When the real G90 and DE-19 are available: capture the DE-19's USB descriptors and a `rigctl` session against the radio, and reconcile the personality with them.

**Milestone 6: path simulation**

30. Written, host-tested: fading and frequency offset in the receive channel: two-path Rayleigh fading with a Gaussian Doppler spectrum, by Data2G's named conditions or custom numbers, with the SNR setting as the average.
31. Written, host-tested; over a real transport compiled only: the shared channel. Two linked boards keep one set of path settings and fade from one seed and clock.
32. Acceptance: two phones running Data2G in ARQ over `mpp` and `mpd` between two boards, with throughput and speed levels compared against Data2G's own simulator at the same condition and SNR; and the processor time fading takes, measured on a board.

**Milestone 7: front panel**

34. Written, host-tested, and tried in a desktop browser against the simulator with Hamlib and with Data2G's rigctld facade driving it: the radio's front panel in the web console (`/api/radio`), `civ_local` and `civ_state_hash` in the core, names for the panel in both personalities, and the panel's key as a fourth PTT source. The firmware's two routes are compiled only.
35. Written: a CAT serial port on each simulated board (`host/build/cat-a`, `cat-b`), so the panel can be used with rig-control software before any board exists.
36. Acceptance: on a board, `rigctl -m 3085` on the USB serial port while the page is open: what it sets shows on the panel within a second, what the panel sets it reads back, and the audio buffer's underruns do not rise while a slider is dragged.

37. Written, host-tested and watched in a desktop browser against the simulator: the audio waterfall. The spectrum in `audio_dsp`, the rings and the row format in `console_api`, `/ws/scope` in the simulator. The firmware's side (the rings fed from the audio tasks, the rows pushed from the server's task) is compiled only.
38. Acceptance: on a board, with a modem transmitting through the link or the parrot: the waterfall shows its signal each way, the audio buffer's underruns and the time per audio block are the same with the waterfall open as closed, and the HTTP task's share of a core with it open is read from the table of tasks.

**Bring-up aids**

33. Written; the layout and the timing arithmetic host-tested, the collecting compiled only: the board's health in the log and in the web console: memory, processor load per core, time per audio block, Wi-Fi drops, the reason for the last restart, and the table of tasks on request.

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
| Nov 19, 2026 | The G90 personality answers commands `25` and `26`, though the manual does not list them and one firmware answers them wrongly. | Hamlib tries them on a G90, and its first mode command of each session fails if they are refused (seen with rigctl 4.7.1). A bench stand-in that Hamlib rejects is no use; the difference from the radio is written down. |
| Nov 19, 2026 | The G90 personality does not copy the radio's known departures from CI-V: the echo, the non-`FB` acknowledgements, the toggling attenuator, the non-BCD bytes. | Hamlib accepts the plain forms in every version read, the departures differ between firmware versions, and none could be checked against a radio. They are listed in the design instead. |
| Nov 19, 2026 | The G90 identifies itself as `70` to `19 00` and `00 90` to `1D 19`. | Those are the values Hamlib's backend recognises as a G90, in 4.7.1 and in master. |
| Nov 19, 2026 | The radio imitated is chosen at run time by its Hamlib model number, through mock-only control `60` or the web console, and switching is power-off and power-on: `civ_set_rig()` in the core. | One firmware image serves both radios; the model number is what the user types into their software anyway; a clean power-up state leaves nothing of one radio in the other. The core still names no radio. |
| Nov 19, 2026 | The firmware stores the choice of radio in flash. Nothing else is stored, and reset (`01`) does not touch it. | A board set up as the station's radio should still be that radio after a power cycle; settings should still start from a known state. |
| Nov 19, 2026 | Settings are written to flash only when asked, as the power-up settings; reset returns to them, and they can be forgotten. This replaces "run-time settings are not stored" above. | A board for Data2G needs the VOX on at every power-up, and a phone holding the CAT port leaves no way to set it. Keeping them only on request still starts every session from a state someone chose. |
| Nov 19, 2026 | Kept settings are a list of command numbers and values with a check, read back through the same range checks as any setting. | Firmware with more settings, fewer, or other ranges still reads what an older one kept: what it cannot use stays at the build's value. Nothing unchecked reaches the engine from flash. |
| Nov 19, 2026 | Clips live in a raw flash partition cut into equal slots, with a header and CRCs, not in a file system. | A handful of large recordings needs no directory. Fixed slots are simple to make safe against a power cut (header last), need no extra component, and are testable on the host against a pretend flash chip. |
| Nov 19, 2026 | A recording is kept as a clip only when asked, by a task of its own; the command is answered when the work begins. | Megabytes of flash take seconds to write and disturb audio meanwhile, so it cannot follow every transmission or run in the task that reads CAT. |
| Nov 19, 2026 | A clip plays through the parrot: it replaces the recording in the parrot's buffer and is replayed. The engine lends the buffer to the clip task and takes it back. | The replay path, with the channel applied, exists already; one buffer and one hand-over is less than a second playback path. |
| Nov 19, 2026 | The firmware uses its own partition table (`partitions.csv`): 2 MB for the application and the rest for clips. This replaces the 1.5 MB table above. | Clips need a partition, and the image is near 1 MB. |
| Nov 19, 2026 | Fading uses Data2G's model, its condition names and its numbers. | The mock exists to exercise Data2G; a condition should mean the same in its simulator and on the bench, so that results can be compared. |
| Nov 19, 2026 | Fading runs at 12 kHz inside the receive channel, after the level measurement and before the noise. | The parrot's and the link's audio is 12 kHz already, so little is lost; a Hilbert transformer at 12 kHz is a quarter the work of one at 48 kHz. Measuring first makes the SNR setting the average, as Data2G defines it, and keeps the gain from cancelling slow fades. |
| Nov 19, 2026 | The fading gains are computed from a seed and a time, by a hash and a fixed filter, not by a running random generator. | Two boards can then produce the same fading without exchanging it, and a board can join part-way. Integer arithmetic for the time keeps the two exactly in step. |
| Nov 19, 2026 | Each board still applies the channel to what it receives, and two linked boards coordinate. One board does not process both directions. | Noise must be made where it is heard, since nothing crosses the link between transmissions. With noise local anyway, sharing settings and a fading clock is less than moving the fading to one board, and neither board is special. |
| Nov 19, 2026 | Shared settings are reconciled by version: the set changed more often wins, and a tie goes to the higher start-up number. The same number picks which board's clock leads. | No board needs configuring as the master, either may be changed from its console, and two boards always converge. Counting changes needs no shared time. |
| Nov 19, 2026 | The shared channel is on by default. | Two linked boards are one path. Wanting the two directions to differ is the exception, and has a switch. |
| Nov 19, 2026 | The frequency offset is not shared. | Between two radios it has opposite signs in the two directions, so one shared value would be wrong for one of them. |
| Nov 19, 2026 | The parrot's delay and period travel in tenths of a second. | Two BCD bytes reach 300 s, within the core's 16-bit BCD helpers, and 7.5 s (FT4) is still exact. |
| Nov 20, 2026 | The board reports its own health (memory, processor load, time per audio block, Wi-Fi drops, restart reason, tasks) in the log and in the web console. | The first flash puts six milestones on a board at once. Without these numbers "does fading cost too much" and "is a stack about to overflow" cannot be answered on the bench. |
| Nov 20, 2026 | Health is its own request, asked for by the page while its section is open, and not part of the pushed telemetry. | It is wanted during bring-up and rarely after. Keeping it out of the stream leaves the Wi-Fi traffic beside ESP-NOW as light as it was. |
| Nov 20, 2026 | The table of tasks is read only on request, never on a timer by default, and each reading reports how long it took. | FreeRTOS reads it inside a critical section while it scans every stack, about a millisecond by estimate. An instrument should not put clicks into the audio it is there to judge; where it must disturb, it says by how much. |
| Nov 20, 2026 | Processor load per core comes from the idle tasks' run-time counters, and the audio cost from timing `audio_to_host` itself. | Both are a few counter reads with no critical section worth the name, so they can be read every 2 s. The block timing counts exactly the work that must finish before the next block. |
| Nov 20, 2026 | The simulator's health numbers are made up, and the text where the restart reason goes says so. | There is no ESP32 to measure, but the page needs something to lay out. Invented numbers that looked measured would mislead about the very thing the section exists to answer. |
| Nov 21, 2026 | The web console gets a front panel for the radio imitated, generic and built from the personality's tables, not a picture of an IC-705 or a G90. | To exercise rig-control software (Hamlib, Data2G's rigctld facade) with no radio at hand: see what the controller set, and change things at the radio to see whether it notices. Generic because the page then needs no knowledge of any radio, serves both personalities, and promises nothing the mock lacks. |
| Nov 21, 2026 | The panel sets what CAT can set by running the CI-V command through the parser (`civ_local`), not through setters of its own. | One set of rules: the panel cannot set what CAT would refuse, a personality's own handlers apply to both, and there is no second table of ranges to drift. |
| Nov 21, 2026 | The page learns that the radio changed from a hash of its state in the telemetry, not from a count of commands. | Controllers repeat sets that change nothing. A counter would have the page fetching 3 KB several times a second beside ESP-NOW; the hash moves only when something did. |
| Nov 21, 2026 | The panel's PTT key is a fourth PTT source in `audio_engine`, apart from CAT's. | It is the case a controller must cope with: a radio keyed by its own operator. Sent as CAT's `1C 00` it would be indistinguishable from the controller's keying, and the controller could release it. |
| Nov 21, 2026 | Meters are set from the panel and nowhere else; a function's values are named only with a source, and meter scales are left out. | There is no RF to measure, and a controller's meter readout needs something to read. The names and scales that could not be sourced (Icom's guide refused the fetch) are left as numbers and not filled in from memory. |
| Nov 21, 2026 | The simulator gives each board a CAT serial port. | No board has been flashed, and the panel is for use with Hamlib now. The page and the port then show one radio, as a board's would. |
| Nov 21, 2026 | The console gets an audio waterfall, transformed on the board and sent as one byte for each bin. | To see what the channel does to a signal and what a modem sends. Transformed on the board because the audio itself would be 24 KB a second on the radio the link shares, against 1.3 KB for a direction's rows. |
| Nov 21, 2026 | The audio tasks only copy samples into a ring; the transforms run in the console's task, and only while a page has the waterfall open. | The audio path must not pay for a display. A slow row then delays the page, never a USB audio packet, and a console nobody is watching costs two copies a block. |
| Nov 21, 2026 | The waterfall has a WebSocket of its own, opened and closed with its section of the page. | The telemetry's socket only pushes and every page gets it; a row is wanted by some pages and not others. A second connection says which without the page having to send anything. |
| Nov 21, 2026 | 512 points at 12 kHz, the lowest 128 bins, ten rows a second, levels on the meters' scale. | 23 Hz across 3 kHz is the passband every modem here uses, at a width a 400-pixel lane can show. The resampler's filter already exists and keeps what is above 6 kHz from folding in. One scale for the meters and the waterfall means a tone reads the same on both. |
| Nov 21, 2026 | The custom fading's two numbers are greyed out on the page unless Fading is "Custom"; a named condition does not write its own numbers into them. | With `hilat-d` running at 30 Hz the field still read 1 Hz, which looked wrong. The fields are the custom condition's, kept so that they survive a visit to a named one, so the fix is to show that they are not in force, not to overwrite them. The page learns which settings these are from `/api/info`, not from their names. |
| Nov 25, 2026 | Run the audio component's TinyUSB, speaker and microphone tasks on core 1 at priorities 12 to 14, and pin lwIP to core 0 (`sdkconfig.defaults`). | The component hands each playback packet from the interrupt to its task through a single buffer; at its default priority 5 on either core, Wi-Fi and lwIP delayed the task past 1 ms and a packet was lost every 20 to 30 s on the first board. Core 1 has nothing else that is busy. |
| Nov 25, 2026 | Mark the waterfall's WebSocket in the server's post-handshake callback, and turn that callback on in `sdkconfig.defaults`. | ESP-IDF v5.5.6 does not pass the handshake to the handler, so marking it there never happened on the board. The page keeps only listening; making it send a first message instead would change the protocol the simulator checks. |
| Nov 25, 2026 | Noise on at power-up (`MOCK_NOISE_ON` defaults to y): a −40 dBFS floor with the signal held 10 dB above it. This replaces the Nov 19 choice of noise off. | With exact silence and full-scale bursts, Data2G's waterfall was black and it warned of clipping; a modem should meet the mock as it meets a radio. Exactness is a setting away (`noise=off`), and the bit-exact loop test sets it first. The host simulator keeps `ae_config_defaults`, noise off. |
| Nov 25, 2026 | The TinyUSB task runs above the audio tasks (priority 15 against 14 and 13, all on core 1), and its event queue holds 64 events instead of 16. | It empties the interrupt's event queue; below the audio tasks a 12 ms audio block overflowed the queue and a lost completion wedged the CAT port. The queue size covers stalls nothing can preempt. |

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
- [ ] How much processor time does fading take on the S3, and does it disturb USB audio? Estimated at some percent of one core. The health section's time per audio block, with fading off and on, is the measurement. (step 32)
- [x] How long does reading the table of tasks hold the board up, and does it cost a USB audio packet? 1.1 ms with 18 tasks, and yes: each reading cost exactly one 1 ms playback packet on the first board (Nov 25, 2026). It stays a button, never on a timer.
- [ ] Are the task stacks sized right? The sizes in the code are guesses; the table of tasks shows what each has never used. (step 6)
- [ ] Do FreeRTOS's run-time statistics, on by default now, cost anything that shows in the audio? (step 6)
- [ ] Does the mock's fading give Data2G the same results as Data2G's own simulator at the same condition and SNR? The gain generator is a running equivalent of its FFT one, checked by its statistics only. (step 32)
- [ ] How closely do two boards' clocks agree over ESP-NOW and over the UART, and is that close enough for the fast conditions (10 to 30 Hz) to fade alike both ways? Within 3 ms in the host test, where packets arrive at once. (step 32)
- [ ] Which more of Data2G's simulator is worth having: impulsive clicks, a wandering noise floor, interference?
- [ ] Should a board be able to choose its peer, for more than two boards in range? Today the first one heard is taken.
- [ ] Should the link compare mode as well as frequency?
- [ ] Are GPIO17 and GPIO18 free for the UART link on the boards that arrive? (step 26)
- [x] How does a board get its Wi-Fi settings, and which board hosts the access point when there is no router? Both ways: the console's Wi-Fi form (stored in flash) or menuconfig. A board that cannot join a network hosts its own, so with no router one board is left to host and the other is pointed at its network through the form. Decided Nov 19, 2026; not tried.
- [x] Does the web console work on a board: joining a network, falling back to its own, the `.local` name, the WebSocket push, and the page on an Android phone's browser? Yes on the first board (Nov 25, 2026): its own network served the page to an Android phone, whose Wi-Fi form gave it the home network; joined, it answered at `mockradio-xxxx.local` and pushed telemetry to a desktop browser. The waterfall's socket needed a fix.
- [ ] Is the default password of the board's own network (`mockradio`) acceptable, or should each board get its own?
- [ ] Are five telemetry snapshots a second enough to follow ARQ turnarounds, or does PTT need to be counted or timestamped on the board? (step 26)
- [ ] Does Wi-Fi running beside USB audio cause underruns or audible glitches on the S3? With the board hosting its own network and nobody connected: none, once the audio component's tasks were moved to core 1 (before that, a lost playback packet every 20 to 30 s). Joined to a home network with a page open, the telemetry pushed and health polled: none in 150 s of streaming; with the waterfall open as well: none in 40 s. (step 20)
- [ ] Which telemetry is worth recording over time rather than only showing live, and where it is kept (the browser, or a file the page can download)? The page keeps 60 seconds of levels and PTT in the browser; nothing longer yet. (after step 20)
- [ ] Do kept settings and clips work on a board: how long does keeping a 15 s and a 120 s clip take, and how badly does it disturb USB audio and the link while it lasts? Compiled only. (after step 9)
- [ ] Should clips be uploaded and downloaded as WAV files through the web console, so that a canned signal need not be played in through the sound card?
- [ ] Do the mock-only controls work on a board, and does a setting change mid-stream sound clean? Host-tested only. (after step 9)
- [x] Default reference bandwidth for the SNR setting: 2500 Hz. Data2G reports its SNR in 2500 Hz (`SNR_REF_BW_HZ` in `native/core/generated/config.hpp`), as does WSJT-X; 3000 Hz remains a setting for VARA.
- [ ] Are the channel defaults right for real modems: noise floor −40 dBFS, turnaround 30 ms, and silence (not a low noise floor) on receive while keyed? To be judged with WSJT-X, FT8AF and Data2G on a board. (after step 9) First evidence, Data2G on Nov 25, 2026: with noise off it gets exact silence and full-scale bursts, so its waterfall is black and it warns of clipping; with noise on it looks like a radio. Noise is now on at power-up (decided Nov 25, 2026); the floor, SNR and turnaround values remain to be judged.
- [ ] Should the S-meter (`15 02`) follow the receive level the engine produces? `civ_set_meter` exists for it. Not done with step 13; no client seen so far depends on it.
- [ ] Are the VOX defaults right for real modems (−40 dBFS, 100 ms hang), and does Data2G in its default VOX mode hold a clean transmission against the mock with `vox=on`? (after step 9)
- [x] Is the RGB LED on GPIO48 on the boards that arrive, does it need the solder pad bridged, and is 10 % brightness right? GPIO48, no pad to bridge, on the first board (Nov 25, 2026). 10 % is kept; it is a menuconfig setting.
- [x] G90: which replies to give, and whether to support `25`/`26`. Decided from the manual, Hamlib's source and a run of `rigctl -m 3088` (Nov 19, 2026): see [G90 personality](#g90-personality). `25`/`26` are supported, because Hamlib's first mode command fails without them.
- [ ] G90: how does the real radio differ from the personality? Known departures are listed in its section; what a real G90 answers to `26`, to a set of a level it lacks, to `1A 06`, and whether its data modes read back as assumed, are not known. (step 29)
- [ ] Does Hamlib master (default address `88`, `1D 19` first) accept the G90 personality when run, not only when read? And do WSJT-X, fldigi and Data2G's bundled Hamlib 4.7.2 with model 3088? (after step 9)
- [ ] Does writing the choice of radio to flash from the USB task disturb audio for the few milliseconds it takes? (after step 9)
- [ ] G90, later pass: what the real DE-19 presents over USB (VID/PID, one or two COM ports, audio format, device name), and whether it keys the G90 from RTS or DTR or only by CAT. One setup guide says to hold both lines low with PTT set to CAT. (step 29)
- [x] How control-line PTT mode is selected and stored. Selected with mock-only control `50` or at build time (Nov 19, 2026). It was not stored at first; since step 14 it is kept with the other settings when they are kept (`02`). The BOOT button is not used.
- [x] Exact S3 IN-endpoint limit, and whether duplex UAC2 with a feedback endpoint plus CDC fits. Four IN endpoints besides EP0; it fits exactly, with FIFO space to spare.
- [x] Why did the first board's CAT replies stop reaching the Mac while its CAT input kept working? TinyUSB's event queue overflowed while its task sat below the audio tasks, and a lost completion left the CDC IN endpoint busy. Fixed Nov 25, 2026 (priority and queue size); see [Bring-up on hardware](#bring-up-on-hardware).
- [ ] What made one audio block take 12.3 ms on the first board during a ragchew in the link route? The queue now rides it out, but the stall itself is not explained. The health readings' worst block time is the place to watch. (step 26)
- [ ] Does each host (macOS, Windows, Linux, Android) enumerate the device and stream audio both ways? macOS 15: yes, both ways, bit-exact (Nov 25, 2026). Android 16 (an Android phone): yes, enumerated and streamed into Data2G (Nov 25, 2026). Linux and Windows: not yet tried. (step 6)
- [ ] Audio latency: the audio component moves capture audio in 10 ms blocks and the loopback FIFO holds about 20 ms. Is that acceptable for ARQ turnaround, or do the intervals need tightening? (after step 9)
- [ ] USB VID/PID if a board ever leaves the bench
- [ ] Does the S3 composite device enumerate cleanly on both test phones? (after step 9)
- [x] How does Data2G on Android key PTT? VOX, CAT, DTR or RTS, chosen in its rig pane.
- [x] Does Data2G on Android support network rig control? Yes: "Network (rigctld or ser2net)".
- [ ] Front panel: the IC-705's function values beyond AGC, preamplifier and break-in (filter shape, notch width, SSB transmit bandwidth, DSQL/CSQL, tone squelch type), and its meter scales (S9, SWR 2.0, full power), from Icom's CI-V Reference Guide. Unnamed until someone reads the guide; the fetch was refused on Nov 21, 2026.
- [ ] Front panel: should the mock send CI-V transceive frames when the panel changes frequency or mode, as a mock-only setting off at power-up? Hamlib lists async data for model 3085, and it would test how a facade copes with data it did not ask for. The address to send from, and whether the HTTP task may write to the serial port, need deciding.
- [ ] Front panel: which of what Hamlib's IC-705 backend claims and the mock refuses is worth adding: memories and VFO/memory operations, scanning, CTCSS tones and repeater offset, Morse sending, parameters (beep, backlight, time), XIT. A `rigctl` sweep through every claimed capability would list them exactly. Driven by what Data2G's facade comes to pass through.
- [ ] Front panel: should the panel's key release itself when no page has been heard from for a while?
- [ ] Front panel: does dragging a slider (a request every few tens of milliseconds, each holding the CAT lock briefly) disturb USB audio or the link on a board? (step 36)
- [ ] G90: what does the real radio do with `16 12 05`, which Hamlib 4.7.1 sends for medium AGC? (step 29)
- [ ] Waterfall: how long does a row take on the S3, and does the waterfall open change the audio's underruns or the time per audio block? On the first board, opening it raised the time per 240-sample audio block from about 280 to 340 µs and core 1 from about 11 to 25 % busy, and cost no audio in 40 s. The time a row itself takes is not measured. (step 38)
- [ ] Waterfall: is 32 KB of internal memory for it acceptable beside Wi-Fi and the link, or should the rings be allocated only while a page is watching? The board's health section has the figures. (step 38)
- [ ] Waterfall: is 0 to 3 kHz the right span, or should it reach 4 kHz for WSJT-X's upper tones? 171 bins would be 1.7 KB a second a direction.
- [ ] Waterfall: should the simulator's pretend host play something wider than a tone (a comb, or a recorded modem burst), so that fading can be judged by eye on the host?

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
| [Radioddity "Xiegu G90 CAT control and digital modes" manual v1.0 (2021-06-23)](https://radioddity.com/blogs/all/xiegu-g90-cat-and-digital-modes) | The official CI-V command table (section 6); the source of the G90 personality and the one Hamlib's backend cites |
| [Hamlib issue 1547](https://github.com/Hamlib/Hamlib/issues/1547) | A log of a real G90 (firmware 20240504.8): it echoes commands, answers at `A4`, and answers `25` wrongly |
| [G90 digital modes with the DE-19 (user write-up)](https://hmdsamarasekara.github.io/posts/xiegu-g90-digital-modes/) | DE-19 cabling, serial settings and G90 menu settings; reports a CH342 chip |
| [Data2G](https://github.com/arodland/Data2G/) | Target modem; Android USB code under `native/core/audio/android`, `native/core/rig/android`, `native/android-app` |
| [FT8CN](https://github.com/N0BOY/FT8CN) and its maintained fork [FT8AF](https://github.com/patrickrb/FT8AF) | Android FT8 apps with their own CI-V code; Icom commands in `rigs/IcomRig.java` and `rigs/IcomRigConstant.java` |

Side project for the spare boards: ESP-SDR (ESPARGOS) and esp32-sdr-trx (DA2JH) turn an S3's Wi-Fi radio into a 2.4 GHz receiver / 13 cm transceiver.
