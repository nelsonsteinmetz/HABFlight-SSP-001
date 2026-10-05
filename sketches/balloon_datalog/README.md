# balloon_datalog

Pro Mini 3.3V/8MHz sketch for the single-shot HAB mission. Forked from
`rocket-flight-logger/drone_baro_gps_2sd` and adapted with:

- Arm jumper (remove-before-flight)
- Altitude-based photo cadence (12-state state machine)
- GPS altitude used above ~4000 m where BMP180 pressure gets unreliable
- Extra CSV columns (`bmp_alt_m`, `gps_alt_m`, `state`, `alt_source`)

## Wiring

| Pin | Direction | Wired to | Notes |
|---|---|---|---|
| D2  | in  | Arm jumper → GND | INPUT_PULLUP. Jumper in = safe. Pull to arm. |
| D3  | out | GPS RX | SoftwareSerial TX (rarely used at runtime) |
| D4  | in  | GPS TX | SoftwareSerial RX (NMEA in) |
| D5  | out | **Green** armed LED | 220 Ω |
| D6  | out | ESP32-CAM GPIO 3 | 500 µs rising-edge photo trigger |
| D7  | out | **Yellow** GPS status LED | 220 Ω |
| D9  | out | **Red** system LED | 220 Ω |
| D10 | out | SD CS | SPI |
| D11/12/13 | SPI | SD | MOSI/MISO/SCK |
| A4/A5 | I²C | BMP180 | SDA/SCL |

## LED cheat sheet

| LED | Off | Blinking | Solid |
|---|---|---|---|
| D7 GPS (yellow) | No NMEA at all — module dead | 1 Hz = no fix. 5 Hz = weak fix (<6 sats). | Good fix (≥6 sats) |
| D9 system (red) | — | Heartbeat = OK. N blinks + long pause = error code: **2** = BMP180 fail, **3** = SD mount fail, **4** = SD open fail, **5** = SD write fail (runtime). | — |
| D5 arm (green) | WAITING (jumper in) | 2 Hz = armed and flying | LANDED |

## State machine

12 states, driven off altitude:

| # | Name | Photo interval | Transition out |
|---|---|---|---|
| 0 | WAITING | 1 / 60 s | Jumper pulled → 1 |
| 1 | ARMED_GROUND | **0.5 Hz** | BMP altitude >5 m above launch → 2 |
| 2 | ASCENT_1 | **1 Hz** | >20 m → 3 |
| 3 | ASCENT_2 | 0.5 Hz | >60 m → 4 |
| 4 | ASCENT_3 | 0.25 Hz | >150 m → 5 |
| 5 | ASCENT_4 | 0.1 Hz | >4000 m → 6 |
| 6 | ASCENT_5 | 1 / 60 s | GPS alt >15000 m → 7 |
| 7 | ASCENT_6 | 1 / 20 s | GPS falling >30 m over 20 s → 8 |
| 8 | BURST | **1 Hz** for 10 s | 10 s elapsed → 9 |
| 9 | DESCENT_1 | 0.1 Hz | GPS <150 m above launch → 10 |
| 10 | DESCENT_2 | 0.5 Hz | BMP <60 m → 11 |
| 11 | DESCENT_3 | **1 Hz** | BMP wobble <5 m for 20 s AND <50 m → 12 |
| 12 | LANDED | (no photos) | Terminal — logging continues |

## Arm behaviour

- Boot with the D7 → GND jumper installed. State = WAITING. Yellow LED off.
- Pull the jumper (remove-before-flight ribbon). D8 starts pulsing at 2 Hz.
- `armed` is latched in software — putting the jumper back in does nothing.

## Ground calibration

`SEA_LEVEL_PA` is a hardcoded constant (101300 Pa). BMP180 altitude is offset
from that. The state machine doesn't care about that offset — it uses a
delta from `ground_alt_bmp`, which is snapshotted the first time GPS gets a
6-sat fix. So SEA_LEVEL_PA only affects the logged `bmp_alt_m` value, not the
state transitions.

## CSV format

```
sample_id, utc_date, utc_time, millis, temp_c, pressure_pa, bmp_alt_m,
gps_alt_m, lat, lon, sats, gps_age_ms, state, alt_source, photo_id
```

- `state` — 0..12 per table above
- `alt_source` — `B` when the state machine used BMP180 delta, `G` when GPS

## Bench checks before flight

1. **Jumper**: power on with wire between D7 and any GND pin. D8 stays off, D9 heartbeats. Pull the wire, D8 starts fast-pulsing.
2. **State machine**: hold the payload high (elevator or stairwell) and watch photos speed up as you cross 20 m. Only meaningful once GPS has locked ground_alt.
3. **Landing**: leave it flat on a table for >20 s below 50 m — D8 should go solid, photos stop, logging continues.
