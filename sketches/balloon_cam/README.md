# balloon_cam

ESP32-CAM trigger sketch. Each rising edge on GPIO 3 captures one UXGA
JPEG to the onboard SD. Files are named `img_NNNNNN.jpg` with a counter
that survives reboots.

Flashed on an AI-Thinker ESP32-CAM for the SSP-001 flight — 693 photos
captured over 2 h 47 min, no missed triggers, no corrupted files.

## Wiring

| ESP32-CAM pin | To | Notes |
|---|---|---|
| GPIO 3 (U0RXD) | Pro Mini D6 (via the datalogger's trigger output) | `INPUT_PULLDOWN`, debounced 500 ms in software |
| GPIO 33 | onboard red status LED | active-LOW, blinks on each capture |
| 5 V | shared 5 V rail | 220 µF bulk cap next to the ESP's 5 V pin is required (lesson learned 2026-07-27 — see repo history) |
| GND | shared ground | |
| microSD slot | FAT32-formatted card | SD_MMC in 1-bit mode so GPIO 12/13 stay free |

**Do not use GPIO 12 or 13 as the trigger input.** Both are internally
connected to the SD card socket; even in 1-bit SD_MMC mode they pick up
phantom edges from SD write activity. GPIO 3 is the only safe trigger
pin on the AI-Thinker board.

## Build config

- Board: **ESP32 → AI Thinker ESP32-CAM**
- Partition scheme: default (Huge APP OK — code is small)
- Upload speed: 921600
- Flash mode / size: QIO / 4 MB
- Programmer: hooked up via the matching USB downloader board (CH340)

## What the sketch does at boot

1. Disables WiFi + Bluetooth entirely (critical — the 2.4 GHz radio
   desenses the GPS LNA otherwise).
2. Clocks the CPU down to 80 MHz and the camera XCLK to 10 MHz. Cuts
   both power draw and the EMI harmonics that would otherwise fall near
   the 1575 MHz GPS L1 band.
3. Initialises OV2640 at UXGA + JPEG quality 8 + 1 framebuffer in PSRAM.
   Falls back to SVGA in DRAM if PSRAM isn't present.
4. Mounts SD_MMC, scans the root for the highest-numbered existing
   `img_NNNNNN.jpg`, resumes the counter after it.
5. Attaches an ISR on GPIO 3 rising edge. The ISR debounces 500 ms and
   sets a flag; the main loop handles capture + file write.

## Status LED blink codes

Visible on the onboard red LED (GPIO 33, active-LOW):

- Startup: 3 blinks after camera init OK, 2 blinks after SD mount OK
- Running: 50 ms pulse on every successful capture
- Camera init failed → 1 fast blink forever (10 Hz)
- SD mount failed → 1 slow blink forever (5 Hz)
- Capture / file-open / short-write errors → 4 fast blinks + continue

## Capture triggering

The Pro Mini datalogger (see `sketches/balloon_datalog/`) pulses D6 HIGH
for 500 µs at an interval that depends on the current flight state:

| State | Interval |
|---|---|
| WAITING | 1 photo / min |
| ARMED_GROUND | 1 / 2 s |
| ASCENT_1 (0–20 m) | 1 Hz |
| ASCENT_2 (20–60 m) | 0.5 Hz |
| ASCENT_3 (60–150 m) | 0.25 Hz |
| ASCENT_4 (150–4 000 m) | 0.1 Hz |
| ASCENT_5 (4 – 15 km) | 1 / min |
| ASCENT_6 (15 km → burst) | 3 / min |
| BURST | 1 Hz for 10 s |
| DESCENT_1 (burst → 150 m) | 0.1 Hz |
| DESCENT_2 (150 – 60 m) | 0.5 Hz |
| DESCENT_3 (< 60 m) | 1 Hz |
| LANDED | off |

Each capture row in the Pro Mini CSV carries a `photo_id` field that
matches the ESP's `img_NNNNNN.jpg` filename, so the two SD cards can be
paired on the ground after recovery.
