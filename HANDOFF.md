# ESP32-S3 Mock Radio — Project Handoff

> Original handoff, kept as a snapshot. The living design document is [DESIGN.md](DESIGN.md).

Nov 19, 2026 · Tim (W0SIG)

## Goal and scope

Build ESP32-S3 firmware that plugs into a computer or Android phone over USB and looks like a ham radio with a built-in sound card and CAT port, the way a Digirig-plus-radio or an IC-7300's USB port does. Modem and logging software (Data2G, WSJT-X, VARA, Winlink, fldigi) should drive it through Hamlib exactly as it would a real rig.

In scope:

- One USB connection that shows up as a USB sound card (UAC2, 48 kHz 16-bit mono, playback + capture) plus a USB serial port (CDC-ACM).
- A Kenwood TS-480 CAT emulation on the serial port that Hamlib (rig model 2028) accepts.
- PTT from CAT (`TX;`/`TX0;`/`TX1;`/`RX;`), from the serial RTS line (Digirig style), and optionally VOX.
- Parrot mode: record transmit audio into PSRAM and play it back as receive audio.
- Channel simulation: noise at a set SNR, attenuation, optional fading.
- A two-board link (ESP-NOW first, wired UART as an option) so two phones running Data2G can hold an ARQ session over a fake radio path.

Out of scope for now: real RF, analog audio (no codec), and Icom CI-V emulation.

Operator: Tim, W0SIG (General). The wireless link between boards is unlicensed 2.4 GHz (Part 15), not amateur RF.

## Hardware

Target board: ESP32-S3-DevKitC-1 clone with the ESP32-S3-WROOM-1 N16R8 module (16 MB flash, 8 MB octal PSRAM). Three boards ordered, at roughly $7–12 each.

| Item | Detail |
| --- | --- |
| USB jack for the mock radio | The one labeled "USB" (native USB, GPIO19/20). The "COM"/"UART" jack goes through a USB-serial bridge chip and can't do audio. |
| Second jack | "COM"/"UART" stays free for flashing and debug logs while the native port runs the mock radio. |
| PSRAM | Octal mode: ESP-IDF menuconfig → Octal, or Arduino "OPI PSRAM". Without it the extra 8 MB isn't available. |
| Reserved pins | GPIO35, 36, 37 are used by the octal PSRAM; don't use them. |
| Status LED | Onboard RGB LED, usually GPIO48; on some clones the "RGB" solder pad must be bridged. |
| Power | USB only; no breadboard or extra parts needed for the all-digital build. Phone OTG supplies it fine (roughly 100–300 mA with Wi-Fi on). |
| Flashing | If the native port doesn't enumerate: hold BOOT, tap RESET, release BOOT, or flash through COM. |
| Optional | "-1U" module variant with an external antenna connector, for longer ESP-NOW range. |

Avoid single-USB boards whose only jack goes through a CH343 USB-serial chip: they can't present the native USB device.

## Architecture

```
 Phone or PC                    ESP32-S3 mock radio
 (Data2G, WSJT-X, VARA;   USB   +------------------------------------------+
  Hamlib: Kenwood TS-480) <---> | USB composite device                     |
                                |   UAC2 audio, 48 kHz mono, both ways     |
                                |   CDC-ACM serial port (CAT + RTS)        |
                                +------------------------------------------+
                                   |                          ^ v
                                   v                          | |
                                +--------------+  PTT  +--------------------+
                                | CAT parser   | ----> | Audio engine       |
                                | TS-480 cmds  |       | parrot replay      |
                                | PTT: CAT/RTS/|       | noise at set SNR   |
                                | VOX          |       +--------------------+
                                +--------------+              ^ v
                                +------------------------------------------+
                                | Link layer: loopback, ESP-NOW or UART    | <---> Second board
                                +------------------------------------------+       (its own phone
                                                                                    running Data2G)
```

The host sees one USB device; inside, CAT state keys the audio engine, and the link layer decides where transmit audio goes: back to the same host (loopback/parrot) or to a second board.

Suggested firmware modules:

- `usb_composite`: descriptors and TinyUSB callbacks for UAC2 + CDC-ACM, including RTS line-state changes
- `cat_ts480`: command parser and the radio state struct (VFO A/B, mode, PTT, power, AI)
- `audio_engine`: loopback, PSRAM parrot buffer, 48↔12 kHz resampling, noise/attenuation, TX mute
- `link`: one interface with loopback, ESP-NOW and UART backends; jitter buffer and drift correction
- `ui`: RGB LED states; a Wi-Fi status page later if wanted

## CAT emulation: Kenwood TS-480

The mock radio speaks Kenwood TS-480 CAT, because it's plain ASCII (two-letter command, optional digits, `;` terminator) and Hamlib, WSJT-X, fldigi and most homebuilt rigs support it. Software selects Hamlib rig model 2028 (Kenwood TS-480). The parser buffers bytes until `;`, switches on the first two letters, updates a state struct and replies like a real radio.

| Command | Meaning | Mock reply |
| --- | --- | --- |
| `ID;` | Radio identity | `ID020;` (020 = TS-480) |
| `PS;` | Power status | `PS1;` |
| `AI0;` / `AI;` | Auto-info off / query | none / `AI0;` |
| `FA;` / `FAnnnnnnnnnnn;` | VFO A query / set (11 digits, Hz) | `FA00014074000;` / none |
| `FB;` / `FBnnnnnnnnnnn;` | VFO B query / set | as FA |
| `MD;` / `MDn;` | Mode query / set (1 LSB, 2 USB, 3 CW, 4 FM, 5 AM, 6 FSK, 7 CW-R, 9 FSK-R) | `MD2;` / none |
| `TX;` `TX0;` `TX1;` | Key (any variant) | none |
| `RX;` | Unkey | none |
| `IF;` | Full status (frequency, RIT, mode, TX state, VFO, split) | Fixed-length composite reply |
| anything else | Unsupported query | `?;` |

Notes for implementation:

- `IF;` is the only fiddly reply. Hamlib polls it constantly and rejects a reply whose length or field positions are wrong. Get the exact format from Hamlib's Kenwood backend and its simulator programs (`simulators/` in the Hamlib source); verify with `rigctl -vvvv`.
- Hamlib's Kenwood driver opens with `ID;`, `AI0;`, `PS;`, then polls `IF;`, `FA;`, `MD;`.
- PTT also comes from the serial RTS line (TinyUSB line-state callback) and optionally from VOX (transmit audio level above a threshold).
- Reserve a few unused two-letter codes for mock-only controls: replay last TX, play canned clip N, set SNR, set link mode. Exact codes to be picked so they don't collide with real TS-480 commands.

## Audio, parrot mode and channel simulation

Audio stays digital end to end: USB audio in from the host is transmit, USB audio out to the host is receive. While keyed, the receive stream carries silence or a low noise floor, as a real radio mutes receive during TX.

**Parrot mode (record/replay)**

1. On PTT on, write incoming transmit audio into a PSRAM ring buffer.
2. On PTT off, stop recording; after an optional delay, play the buffer out as receive audio.
3. Optionally save the last transmission or canned clips to a flash partition so they survive a power cycle.

| Storage format | Rate | Capacity in 8 MB PSRAM |
| --- | --- | --- |
| As received | 48 kHz, 16-bit mono (~96 KB/s) | ~85 s |
| Decimated ÷4 (low-pass first) | 12 kHz, 16-bit mono (~24 KB/s) | ~5.5 min |

Use cases: FT8 self-decode (TX in one 15 s slot, replay aligned to the next slot boundary; WSJT-X should decode it with near-zero DT), checking TX drive and clipping, and confirming a modem detects its own frames. ARQ modems (VARA, Data2G connected mode) can't complete a session against a parrot; use the two-board link for that.

**Channel simulation (applied on the receive side)**

- Additive noise at a configurable SNR, so ARQ modems step down through their speed levels.
- Attenuation, and optionally fading or a small frequency offset (PathSim-style), later.
- TX→RX turnaround delay of a few tens of ms, to mimic a real radio switching.

## Two-board link and the Data2G target

The end goal: two Android phones running [Data2G](https://github.com/arodland/Data2G/), each plugged into an S3 mock radio, holding a connected (ARQ) session over a link between the two boards. Data2G is an OFDM HF data modem aimed at VARA HF/Pactor-class performance, with ARQ and FEC modes, a VARA-compatible TNC (Winlink, BBS), a KISS TNC, and a 2400 Hz full-speed or 500 Hz narrow bandwidth setting. Tim reports it works on Android and believes it has Hamlib support.

**Link options (behind one swappable interface)**

| Link | Throughput | Notes |
| --- | --- | --- |
| Loopback (single board) | n/a | Parrot and self-tests |
| ESP-NOW | ~0.5–1 Mbps usable | No router, low latency, tens of meters; first choice |
| Wired UART | several Mbps | TX/RX/GND between boards; most deterministic |
| Wi-Fi UDP | network-dependent | Boards anywhere; more latency and jitter |

Audio over the link: 12 kHz 16-bit mono (~192 kbps), which covers Data2G's 2400 Hz bandwidth with room to spare.

**Link behavior**

- Half duplex keyed by PTT: board A's transmit audio becomes board B's receive audio.
- Pass audio only when both radios are on the same frequency (exchanged over the link).
- Keep buffering to tens of ms so ARQ turnaround timing looks like a real radio.
- Clock drift: each S3 has its own crystal, so the two 48 kHz rates differ by a few ppm. Handle it by occasionally dropping or repeating a sample (or resampling) to keep buffers from slowly filling or draining.

**Android notes**

- Android supports class-compliant USB audio; modem apps usually use the usb-serial-for-android library, which handles CDC-ACM. A composite audio+CDC device usually enumerates fine, but test with each app.
- Unconfirmed: how Data2G keys PTT on Android (Hamlib CAT, RTS or VOX). The firmware supports all three, so check the app's radio settings.

## Build plan and testing

Framework: ESP-IDF with esp_tinyusb (Arduino is the fallback if descriptor work gets easier there). Build in three milestones, each testable on its own.

1. **USB composite + CAT + loopback.** Custom descriptors for UAC2 (playback + capture, 48 kHz mono) plus one CDC-ACM port, within the S3's 6-endpoint budget. TS-480 parser with the state struct. Receive = transmit audio looped back.
   - Test on a PC: the board shows up as a sound card + serial port; `rigctl -m 2028 -r /dev/ttyACM0 -vvvv`, then `f`, `F 7074000`, `m`, `T 1`, `T 0`. The `IF;` reply must pass without Hamlib errors.
   - Test audio with two fldigi or WSJT-X instances, or record/playback in Audacity.
2. **Parrot + channel simulation.** PSRAM ring buffer, 12 kHz decimate/interpolate, replay on unkey, noise at set SNR, mock-only CAT commands, LED states (red recording, green playing).
   - Test: WSJT-X FT8 self-decode with replay aligned to the 15 s slot.
3. **Two-board link.** Link-layer interface (loopback / ESP-NOW / UART), frequency matching, jitter buffer, drift handling.
   - Test on PCs first (two fldigi or VARA instances), then two Android phones running Data2G in ARQ mode, stepping the SNR down to watch speed adaptation.

Endpoint budget (the main technical risk): the S3 USB-OTG supports 6 endpoints (5 IN/OUT + 1 IN). Duplex audio + CDC uses nearly all of them, so keep to one audio function and one CDC port; no HID or second serial port.

## References and open questions

No existing project does exactly this (checked Nov 2026); these are the closest starting points.

| Reference | Why it matters |
| --- | --- |
| [reSpeaker ESP32-S3 USB bridge (Seeed)](https://wiki.seeedstudio.com/respeaker_xvf3800_i2s_usb_bridge/) | Custom TinyUSB descriptors fitting duplex audio + CDC into the S3's endpoint/FIFO budget; best descriptor template |
| [Espressif usb_device_uac component](https://components.espressif.com/components/espressif/usb_device_uac/versions/1.3.2/examples/usb_uac) | Official UAC device building block for ESP32-Sx |
| [finger563/esp-usb-audio-example](https://github.com/finger563/esp-usb-audio-example) | S3 as a duplex USB headset (speaker + mic) |
| [Elektor: Xiegu X6200 USB controller](https://www.elektormagazine.com/labs/digital-radio-controller-via-usb-c-otg-audio-data-and-power) | Ham-specific UAC2 + CDC CAT work on an S3, host and device modes |
| [ESP-IDF USB Device Stack docs](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/api-reference/peripherals/usb_device.html) | Composite devices, 6-endpoint limit, "USB" jack wiring |
| Hamlib source: Kenwood backend + `simulators/` | Exact TS-480 replies, especially `IF;` |
| [Data2G](https://github.com/arodland/Data2G/) | Target modem |

Side project for the spare boards: ESP-SDR (ESPARGOS) and esp32-sdr-trx (DA2JH) turn an S3's Wi-Fi radio into a 2.4 GHz receiver / 13 cm transceiver.

**Open questions**

- [ ] How does Data2G on Android key PTT: Hamlib CAT, RTS or VOX?
- [ ] Does Data2G's Android Hamlib support network rig control (rigctld over Wi-Fi) as an alternative to USB serial?
- [ ] Exact `IF;` reply layout for TS-480 in current Hamlib
- [ ] Which unused Kenwood two-letter codes to claim for mock-only controls
- [ ] Does the S3 composite device enumerate cleanly on both test phones?
