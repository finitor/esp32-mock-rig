# usb-mock-radio

ESP32-S3 firmware that enumerates over USB as a ham radio with a built-in sound card and CAT port, so modem software (Data2G, WSJT-X, VARA, fldigi) can drive it through Hamlib like a real rig. It imitates an Icom IC-705 or a Xiegu G90, chosen at run time. No real RF, no analog audio.

[DESIGN.md](DESIGN.md) is the living design: status, scope, hardware, architecture, USB device, CAT table, PTT, audio/parrot/channel simulation, two-board link, Data2G findings, build plan, decision log, open questions, references. Read it before designing or implementing anything.

Keep it current as part of the work: update the section a fact belongs to, add a dated line to the decision log for each design decision, tick or add open questions, and refresh the Status section. [HANDOFF.md](HANDOFF.md) is the original handoff, frozen; do not edit it.

## Status

The CI-V core, the IC-705 and G90 personalities, the audio DSP, the receive routes and channel (`audio_engine`: monitor, radio, parrot and link routes; the parrot's recording and replay; noise at a set SNR, attenuation, HF fading by Data2G's named conditions and a frequency offset, mute while keyed and turnaround; PTT from CAT, a control line or VOX), the mock's own controls (`mock_ctl`: CI-V frames to address `F0` that change the route, channel, parrot, link and PTT settings at run time), the two-board link's core (`link`: 10 ms audio packets, jitter buffer, loss and clock-drift handling, peer discovery, frequency matching), the shared channel between two linked boards (`coord`: path settings and a fading seed and clock kept in step), recordings kept in flash (`clips`: a store of five clips with checks, safe against a power cut), the LED policy (`ui`) and the web console's settings, telemetry, board-health readings, front panel and audio waterfall (`console_api` in `web_console`, with the spectrum itself in `audio_dsp`: the same settings by name as JSON, through `mock_ctl`'s own table; and the radio imitated as an operator at it has it, built from the personality's tables and changed through the parser itself) exist as portable C with host tests; the CAT side passes real Hamlib on the host as both radios (`rigctl -m 3085` and `-m 3088`, Hamlib 4.7.1). The G90 personality is built from documents and Hamlib's source, never compared with a G90. The firmware builds for the ESP32-S3 with the USB composite device, CAT over serial, audio loopback, the status LED, the web console (Wi-Fi, HTTP server, WebSocket telemetry, the page), the board's health readings (`main/health.c`) and the link's two transports (ESP-NOW, UART) written, and its descriptors pass a structural check. The console's page has been tried in a browser against a host simulator of two linked boards, its front panel with real Hamlib and with Data2G's rigctld facade driving the simulated radio through the simulator's CAT serial port. One board has been flashed (Nov 25, 2026): on macOS it enumerates as a sound card and a CAT port, passes `rigctl -m 3085` and `-m 3088`, and loops audio back bit-exact. Other hosts, the web console in a browser, the link, the parrot and flash storage are not yet verified on a board. The development Mac is usually heavily loaded, so check an audio glitch against the board's own readings before blaming the board (see "Bring-up on hardware" in DESIGN.md). Work proceeds in the milestones in DESIGN.md (host-testable groundwork, then USB composite + CAT + loopback, then parrot + channel simulation, then the web console, then two-board link, then the G90 personality), each testable on its own. Everything that can be written without hardware is written; what remains is hardware work: the acceptance tests of each milestone, and reconciling the G90 personality with the real radio.

## Stack

- ESP-IDF (Arduino only as a fallback if descriptor work is easier there).
- Target: ESP32-S3-DevKitC-1 clone, ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB octal PSRAM).
- Modules: `cat_civ` (shared CI-V core), `rig_ic705` and `rig_g90` (personalities), `audio_dsp`, `audio_engine` (routes, parrot and receive channel), `mock_ctl` (mock-only controls), `ui` (LED policy; the RMT driver is in `main/`), `web_console` (`console_api.c` is portable; the HTTP server and the page are ESP-IDF only), `coord` (the shared channel, portable), `clips` (the clip store, portable; the flash underneath is `main/storage.c`), `link` (`link.c` is portable; `link_espnow.c` and `link_uart.c` are the transports, one compiled in by a menuconfig choice), `wifi_net` (Wi-Fi bring-up, shared by the console and ESP-NOW) and `usb_composite` exist.
- Wi-Fi and HTTP: ESP-IDF's own `esp_wifi` and `esp_http_server`, plus Espressif's `mdns` component for the board's `.local` name.
- USB stack: Espressif's `usb_device_uac` component in composite mode for audio, TinyUSB's CDC class directly for serial. Do not add `esp_tinyusb`; it brings its own `tusb_config.h` and has no audio class.

## Hard constraints

- **Endpoint budget:** the S3 allows four IN endpoints besides EP0, and the device uses all four (audio feedback, audio capture, CDC notification, CDC data). Do not add HID, a second serial port, a second audio function, or any other IN endpoint. After any change to the descriptors or `tusb_config.h`, build and run `python3 host/tools/check_usb_descriptors.py`; it must pass.
- **Native USB only:** the mock radio runs on the "USB" jack (GPIO19/20). The "COM"/"UART" jack is for flashing and logs.
- **Reserved pins:** GPIO35, 36, 37 belong to the octal PSRAM. PSRAM must be configured as Octal (OPI) in menuconfig.
- **CAT is Icom CI-V**, as an IC-705 (Hamlib rig model 3085, address `A4`) or a G90 (model 3088): binary frames `FE FE <to> <from> <cmd> ... FD`, OK is `FB`, error is `FA`. Answer frames at any radio address and reply from that address, except `F0`, which is the mock's own (`mock_ctl`). Take reply layouts from Icom's IC-705 CI-V Reference Guide, the Radioddity G90 CAT manual and what `rigctl -m 3085 -vvvv` or `-m 3088` sends, not from memory; Hamlib's simulators are incomplete and only a hint.
- **Say where each G90 fact comes from.** No G90 is at hand, so every reply in `rig_g90` is marked with its source (the manual, Hamlib's source, a logged session), and the known differences from the real radio are listed in DESIGN.md. Keep both current; do not copy a quirk of the radio into the personality on the strength of memory.
- **Keep the core radio-neutral.** `cat_civ` must not name a radio; everything radio-specific (address, ID, mode table, supported commands, ranges, defaults) lives in a personality module, as the G90's does. A new personality is a new module, a line in the lists of personalities in `main/main.c`, `host/civ_pty.c` and `host/console_sim.c`, and a name in `mockctl.py`. The G90 and DE-19 are not accessible during this project, so do not plan on captures from them.
- **Mock-only controls** go to CI-V address `F0` and live in `mock_ctl`, never in a personality or the core. New ones take a command number from the table in DESIGN.md (some are reserved there), refuse out-of-range values with `FA`, and get a matching setting in `host/tools/mockctl.py`. A new setting also gets a row in `mock_ctl`'s settings table and one in `console_api.c`'s table of names, which is all the web console needs: its page builds the form from that table.
- **PTT sources:** CAT (CI-V `1C 00`), a serial control line, optional VOX, and the key on the web console's front panel, combined in `audio_engine`: any one keys, and CAT keying always works. Control-line PTT and VOX are settings, off at power-up, because hosts hold RTS and DTR high on open and any host sound would open the VOX. The panel's key is not a setting and is never kept; it stays apart from CAT's PTT, so a controller sees the radio transmitting and cannot release it. Receive audio is muted (or low noise floor) while keyed.
- **CDC descriptors:** keep the interrupt IN notification endpoint and use device class 0xEF/2/1 with a CDC IAD; usb-serial-for-android (used by Data2G) requires both.
- **Flash is written only when asked.** Settings go to flash on `02` (keep), clips on `35`, the radio imitated and the Wi-Fi network when they are changed; nothing is written on its own, and nothing on every transmission. Whatever is read back from flash is checked before it is used: kept settings go through `mock_ctl`'s range checks, clips through their CRCs. Writing a clip takes seconds and belongs in the clip task, never in the task that reads CAT or serves HTTP. The clip format and the kept-settings format each carry a version: raise it if the layout changes. Changing `partitions.csv` moves data on boards already flashed; say so when it is done.
- **Fading** follows the Data2G project's simulator (`../Data2G/data2g/hfchannel.py`, read-only): the same model, condition names and numbers, so that results can be compared. Do not change a named condition's numbers here unless they change there. The fading gains must stay a function of seed and time alone, in integer arithmetic for the time: that is what lets two boards fade alike. Regenerate the Hilbert and gain filters with `host/tools/gen_hilbert_fir.py --table`.
- **A peer's packets are untrusted.** What arrives over the link is checked like any other input: settings go through `mock_ctl`'s range checks, only the path's settings are taken from a peer, and lengths are checked before anything is read.
- **Link audio** between boards is 12 kHz 16-bit mono, half duplex keyed by PTT, with tens of ms of buffering and sample drop/repeat for clock drift. It travels over ESP-NOW (wired UART as the fallback). A link packet must fit one ESP-NOW frame (250 bytes); the packet format is in DESIGN.md and has a version number, to be raised if it changes. Everything about timing, loss and drift belongs in the portable `link.c`, where the host tests can reach it; a transport only sends packets and hands over what arrives. `link_receive()` runs in the Wi-Fi task and must stay short.
- **One radio:** Wi-Fi carries control only (the web console) and shares the radio with ESP-NOW, so both boards stay on one channel with Wi-Fi power save off, and Wi-Fi traffic stays light. Do not use Bluetooth. The web console needs no USB endpoint and must not add one.
- **Health readings must not disturb what they measure.** Reading FreeRTOS's table of tasks (`uxTaskGetSystemState`) holds the whole board up for about a millisecond, enough to cost a USB audio packet. It happens only when asked for (the console's button, or `MOCK_HEALTH_LOG_TASKS`), never on a timer by default, and never in the pushed telemetry. Anything read regularly must be a few counters. Nothing goes in the audio path but the two clock readings around `audio_to_host`. The simulator's health numbers are made up and must keep saying so.
- **Front panel:** the page's picture of the radio is generic. It is built from `/api/radio`, which is built from the personality's tables, so neither the page nor `console_api` may name a radio or carry a table of its levels, functions or modes. Whatever CAT can set, the panel sets by running the CI-V command through the parser (`civ_local`), never with a setter or a range check of its own; only the meters (`civ_set_meter`) and its PTT key (`ae_set_panel_ptt`) are the panel's own. A name or a value's name shown on the panel lives in the personality beside its number and needs a source like any other fact about the radio (the guide, the manual, a logged Hamlib session): leave a function a plain switch and a meter a raw number sooner than name them from memory. The page reads `/api/radio` again only when the telemetry's `rgen` changes; anything added to the radio's state that CAT can read back goes into `civ_state_hash`.
- **Waterfall:** the audio tasks only copy samples into a ring (`console_scope_add`), and nothing more may be added there for a display. The transforms run in the console's task (`console_scope_row`), and only while a page has the waterfall open, which a connection to `/ws/scope` says; with none, no row is made. A row is bytes, one for each bin, ten times a second: about 1.3 KB a second for each direction of audio, on the radio the link shares. Count that traffic before raising the rate, the span or the number of lanes. The row's first byte is its format's version: raise it if the layout changes. The page takes the bins and their spacing from `/api/info`, not from constants of its own.
- **Web console:** it changes settings only through `mock_ctl`'s settings functions (`mock_ctl_setting_put` and friends), never with a range table of its own, and refuses out-of-range values as the CI-V controls do. The page (`components/web_console/www/index.html`) stays one self-contained file that loads nothing from elsewhere. Requests that change anything must be `application/json`. No Wi-Fi name or password goes into a committed file (`sdkconfig.defaults` included). The firmware side holds the lock in `main/` around settings changes.

## Build and test

Host build of the portable modules (needs only a C compiler, plus Hamlib's `rigctl` for the second command and `python3` for the third and fourth):

```bash
make -C host test
```

```bash
make -C host rigctl
```

```bash
make -C host mockctl
```

```bash
make -C host console
```

Run the first after any change under `components/`, the second after any change to `components/cat_civ` or a personality, the third (needs only `python3`) after any change to `components/mock_ctl` or `host/tools/mockctl.py`, and the fourth after any change to `components/mock_ctl`, `components/web_console`, `components/link`, `components/cat_civ` or a personality. The fourth checks the console's JSON interface, the front panel against CAT, the simulator's CAT serial port, the waterfall's rows, and two simulated boards hearing each other over the link, through a host simulator; it does not run `web_console.c`, `wifi_net.c`, `link_espnow.c` or `link_uart.c`, which only the firmware build compiles. After a change to a link transport or to a `Kconfig` choice, build the firmware for each choice of `MOCK_LINK` (ESP-NOW, UART, none), not only the default.

After a change to the console's page, look at it in a browser. This serves two simulated boards joined by the link, board A at `http://localhost:8080` and board B at `http://localhost:8081`:

```bash
make -C host console-sim
```

The same with no pretend transmissions, to drive the simulated radio from Hamlib while watching the front panel (each board's CAT serial port is a pseudo-terminal, `host/build/cat-a` and `cat-b`):

```bash
make -C host radio-sim
```

```bash
rigctl -m 3085 -r host/build/cat-a
```

Regenerate the resampler table with `host/tools/gen_resampler_fir.py --table` rather than editing it by hand. The host build uses `-Werror` with address and undefined-behaviour sanitizers; keep it clean. Board settings and the flash layout live in `sdkconfig.defaults` and `partitions.csv`.

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

then `f`, `F 7074000`, `m`, `T 1`, `T 0`. The same as a G90, after `mockctl.py rig=g90`:

```bash
rigctl -m 3088 -r /dev/ttyACM0 -vvvv
```

Hardware-dependent behaviour (USB enumeration, audio, Hamlib polling, Android phones, Wi-Fi, the web console's server and the link's transports) cannot be verified without a board attached. Say so plainly rather than claiming it works.
