// Balloon datalog — Pro Mini 3.3V/8MHz, single-shot HAB mission.
// Forked from rocket-flight-logger/drone_baro_gps_2sd. Balloon-specific:
//   - Arm jumper on D2 (INPUT_PULLUP, to GND). LOW = jumper in = paused (no CSV writes,
//     no photos, state reset). HIGH = jumper pulled = recording. Toggle any time.
//   - Green armed LED on D5, yellow GPS LED on D7, red system LED on D9 (all 220 ohm).
//   - Altitude-based state machine drives the photo cadence.
//   - Uses BMP180 altitude below 4000 m of climb, GPS altitude above (BMP180
//     pressure is unreliable above ~9 km, so 4000 m is a safe switchover).
// Logs one CSV row per second to FLIGHT.CSV on SD (SPI, CS=D10).
// Photo trigger on D6 -> ESP32-CAM GPIO 3, 500 us pulse.

#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <SoftwareSerial.h>
#include <TinyGPSPlus.h>
#include <SPI.h>
#include <SD.h>

#define SD_CS_PIN 10
#define GPS_RX_PIN 4
#define GPS_TX_PIN 3
#define ARM_LED 5           // green
#define PHOTO_TRIG_PIN 6
#define GPS_FIX_LED 7       // yellow
#define ARM_PIN 2           // jumper to GND when safe
#define STATUS_LED 9        // red

// LED patterns:
//   D7 GPS_FIX_LED (yellow) : off / 1 Hz blink / 5 Hz blink / solid = no NMEA / no fix / weak / good
//   D9 STATUS_LED  (red)    : heartbeat = OK. N blinks + pause = error code: 2=BMP180 fail, 3=SD mount fail, 4=SD open fail, 5=SD write fail (runtime).
//   D5 ARM_LED     (green)  : off = WAITING. 2 Hz pulse = armed and flying. solid = LANDED.

const float SEA_LEVEL_PA = 101300.0;
const uint8_t GPS_MIN_SATS = 6;
const unsigned long LOG_INTERVAL_MS = 1000;

enum State : uint8_t {
  WAITING       = 0,   // jumper in, no photos except a 1/min heartbeat shot
  ARMED_GROUND  = 1,   // jumper pulled, still on ground: 0.5 Hz
  ASCENT_1      = 2,   // <20 m above launch: 1 Hz
  ASCENT_2      = 3,   // 20-60 m: 0.5 Hz
  ASCENT_3      = 4,   // 60-150 m: 0.25 Hz
  ASCENT_4      = 5,   // 150-4000 m: 0.1 Hz
  ASCENT_5      = 6,   // 4000-15000 m: 1 / 60 s
  ASCENT_6      = 7,   // 15000 m to burst: 3 / 60 s
  BURST         = 8,   // burst detected: 1 Hz for 10 s
  DESCENT_1     = 9,   // burst window done, >150 m from ground: 0.1 Hz
  DESCENT_2     = 10,  // 150-60 m: 0.5 Hz
  DESCENT_3     = 11,  // <60 m: 1 Hz
  LANDED        = 12   // stationary and low: photos off, log stays running
};

// Photo interval per state (ms). Kept as a switch so the constants live in
// flash instead of a RAM-resident const array — saves ~52 bytes.
unsigned long photoIntervalMs(uint8_t s) {
  switch (s) {
    case WAITING:      return 60000UL;
    case ARMED_GROUND: return 2000UL;   // 0.5 Hz on the pad — capture release moment
    case ASCENT_1:     return 1000UL;
    case ASCENT_2:     return 2000UL;
    case ASCENT_3:     return 4000UL;
    case ASCENT_4:     return 10000UL;
    case ASCENT_5:     return 60000UL;
    case ASCENT_6:     return 20000UL;
    case BURST:        return 1000UL;
    case DESCENT_1:    return 10000UL;
    case DESCENT_2:    return 2000UL;
    case DESCENT_3:    return 1000UL;
    case LANDED:       return 0UL;
  }
  return 0UL;
}

// 20-sample ring buffer at 1 Hz = 20 s window. int16_t (metres) to save 80 bytes
// vs float — 1 m resolution is plenty for the 5 m stability and 30 m fall tests.
#define ALT_BUF_LEN 20
int16_t gps_alt_buf[ALT_BUF_LEN];
int16_t bmp_alt_buf[ALT_BUF_LEN];
uint8_t alt_buf_idx = 0;
uint8_t gps_valid_count = 0;
uint8_t bmp_valid_count = 0;

Adafruit_BMP085 bmp;
SoftwareSerial gpsSerial(GPS_RX_PIN, GPS_TX_PIN);
TinyGPSPlus gps;
File logFile;

bool sdWriteFailed = false;
uint32_t sampleId = 0;
uint32_t photoId = 0;
unsigned long lastPhotoMs = 0;
unsigned long burstEnteredMs = 0;

State state = WAITING;
bool armed = false;                    // tracks jumper live; true when D2 is HIGH (jumper pulled)
float ground_alt_bmp = 0.0;            // BMP altitude at first strong GPS fix
float ground_alt_gps = 0.0;            // GPS altitude at first strong GPS fix
bool ground_alt_valid = false;

// N blinks (250 ms on / 250 ms off), then a 1.5 s pause. Repeats forever.
// N is the error code (see D9 comment above).
void fatalBlink(uint8_t n) {
  pinMode(STATUS_LED, OUTPUT);
  while (1) {
    for (uint8_t i = 0; i < n; i++) {
      digitalWrite(STATUS_LED, HIGH); delay(250);
      digitalWrite(STATUS_LED, LOW);  delay(250);
    }
    delay(1500);
  }
}

// Toggle `pin` on/off when `halfPeriodMs` elapses. Caller keeps `lastToggle`
// and `ledOn` so multiple LEDs can blink independently from one helper.
void tickBlink(uint8_t pin, unsigned int halfPeriodMs,
               unsigned long &lastToggle, bool &ledOn) {
  unsigned long now = millis();
  if (now - lastToggle >= halfPeriodMs) {
    ledOn = !ledOn;
    digitalWrite(pin, ledOn ? HIGH : LOW);
    lastToggle = now;
  }
}

// Drives the yellow GPS LED from TinyGPS state: off when no NMEA bytes,
// 1 Hz blink when receiving but no fix, 5 Hz when weak fix (<6 sats),
// solid when fix has enough sats.
void updateGpsLed() {
  static unsigned long lastToggle = 0;
  static bool ledOn = false;
  if (gps.charsProcessed() == 0) {
    digitalWrite(GPS_FIX_LED, LOW); ledOn = false; return;
  }
  bool hasFix = gps.location.isValid();
  uint8_t sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  if (hasFix && sats >= GPS_MIN_SATS) {
    digitalWrite(GPS_FIX_LED, HIGH); ledOn = true; return;
  }
  if (hasFix) { tickBlink(GPS_FIX_LED, 100, lastToggle, ledOn); return; }
  tickBlink(GPS_FIX_LED, 500, lastToggle, ledOn);
}

// Non-blocking 5-blinks-then-pause for runtime SD write fail (error code 5).
// Phases 0..9 = 5 blinks (250 ms each edge). Phase 10 = 1500 ms off.
void updateStatusLed() {
  static unsigned long lastTransition = 0;
  static uint8_t phase = 0;
  if (!sdWriteFailed) return;
  unsigned long now = millis();
  uint16_t duration = (phase == 10) ? 1500 : 250;
  if (now - lastTransition >= duration) {
    phase = (phase + 1) % 11;
    lastTransition = now;
    bool on = (phase < 10 && (phase % 2 == 0));
    digitalWrite(STATUS_LED, on ? HIGH : LOW);
  }
}

// Drives the green armed LED from the state machine: off in WAITING,
// 2 Hz pulse from ARMED_GROUND through DESCENT_3, solid on LANDED.
void updateArmLed() {
  static unsigned long lastToggle = 0;
  static bool ledOn = false;
  if (state == WAITING) { digitalWrite(ARM_LED, LOW);  ledOn = false; return; }
  if (state == LANDED)  { digitalWrite(ARM_LED, HIGH); ledOn = true;  return; }
  tickBlink(ARM_LED, 250, lastToggle, ledOn);  // 2 Hz pulse
}

// Push one 1 Hz sample into the ring buffers used by falling20s / stable20sBmp.
// Any invalid GPS sample resets the GPS window (no false burst calls);
// BMP180 is assumed always valid at bench temperatures.
void pushAltSample(float gps_alt, bool gps_valid, float bmp_alt) {
  gps_alt_buf[alt_buf_idx] = (int16_t)gps_alt;
  bmp_alt_buf[alt_buf_idx] = (int16_t)bmp_alt;
  alt_buf_idx = (alt_buf_idx + 1) % ALT_BUF_LEN;
  if (gps_valid) {
    if (gps_valid_count < ALT_BUF_LEN) gps_valid_count++;
  } else {
    gps_valid_count = 0;  // any invalid sample voids the window
  }
  if (bmp_valid_count < ALT_BUF_LEN) bmp_valid_count++;
}

// alt_buf_idx points at the slot we just moved past (next to be overwritten
// on the following push) — so it IS the oldest of the 20 current samples.
int16_t oldestGpsAlt() { return gps_alt_buf[alt_buf_idx]; }
int16_t newestGpsAlt() {
  uint8_t i = (alt_buf_idx + ALT_BUF_LEN - 1) % ALT_BUF_LEN;
  return gps_alt_buf[i];
}

// True when the GPS altitude ring buffer shows a >30 m drop over the 20 s
// window (~1.5 m/s sustained descent). Used to detect balloon burst.
bool falling20s() {
  if (gps_valid_count < ALT_BUF_LEN) return false;
  return (newestGpsAlt() - oldestGpsAlt()) < -30;  // ~1.5 m/s descent
}

// True when the BMP180 altitude range over the last 20 s is under 5 m,
// signalling the payload has come to rest. Used for landing detection.
bool stable20sBmp() {
  if (bmp_valid_count < ALT_BUF_LEN) return false;
  int16_t lo = bmp_alt_buf[0], hi = bmp_alt_buf[0];
  for (uint8_t i = 1; i < ALT_BUF_LEN; i++) {
    if (bmp_alt_buf[i] < lo) lo = bmp_alt_buf[i];
    if (bmp_alt_buf[i] > hi) hi = bmp_alt_buf[i];
  }
  return (hi - lo) < 5;
}

// One tick of the 12-state photo-cadence machine. `armed` follows the jumper
// live (no latch): pull to start, reinstall to stop and reset. Also snapshots
// ground altitude at first strong GPS fix and advances state on thresholds.
void tickStateMachine(float bmp_alt, float gps_alt, bool gps_valid) {
  bool nowArmed = (digitalRead(ARM_PIN) == HIGH);
  if (armed && !nowArmed) {
    // Jumper reinstalled → reset the run so a fresh pull starts clean.
    state = WAITING;
    ground_alt_valid = false;
    burstEnteredMs = 0;
  }
  armed = nowArmed;

  if (!ground_alt_valid && gps_valid
      && gps.satellites.isValid()
      && gps.satellites.value() >= GPS_MIN_SATS) {
    ground_alt_gps = gps_alt;
    ground_alt_bmp = bmp_alt;
    ground_alt_valid = true;
  }

  float aag_bmp = ground_alt_valid ? (bmp_alt - ground_alt_bmp) : 0.0;

  switch (state) {
    case WAITING:
      if (armed) state = ARMED_GROUND;
      break;
    case ARMED_GROUND:
      if (ground_alt_valid && aag_bmp > 5.0) state = ASCENT_1;
      break;
    case ASCENT_1:
      if (aag_bmp > 20.0)   state = ASCENT_2;
      break;
    case ASCENT_2:
      if (aag_bmp > 60.0)   state = ASCENT_3;
      break;
    case ASCENT_3:
      if (aag_bmp > 150.0)  state = ASCENT_4;
      break;
    case ASCENT_4:
      if (aag_bmp > 4000.0) state = ASCENT_5;
      break;
    case ASCENT_5:
      if (gps_valid && gps_alt > 15000.0) state = ASCENT_6;
      break;
    case ASCENT_6:
      if (falling20s()) { state = BURST; burstEnteredMs = millis(); }
      break;
    case BURST:
      if (millis() - burstEnteredMs >= 10000UL) state = DESCENT_1;
      break;
    case DESCENT_1:
      if (ground_alt_valid && gps_valid
          && (gps_alt - ground_alt_gps) < 150.0) state = DESCENT_2;
      break;
    case DESCENT_2:
      if (aag_bmp < 60.0)   state = DESCENT_3;
      break;
    case DESCENT_3:
      if (stable20sBmp() && aag_bmp < 50.0) state = LANDED;
      break;
    case LANDED:
      break;
  }
}

// Which altitude source drove the state machine this tick — for the CSV.
char altSourceForState() {
  if (state <= ASCENT_4 || state >= DESCENT_2) return 'B';
  return 'G';
}

// Configure IO pins, init BMP180 / SD / open FLIGHT.CSV / write header,
// then start the GPS SoftwareSerial. Halts with an error blink code
// (see D9 comment) if any of the three init steps fail.
void setup() {
  pinMode(GPS_FIX_LED, OUTPUT);    digitalWrite(GPS_FIX_LED, LOW);
  pinMode(STATUS_LED, OUTPUT);     digitalWrite(STATUS_LED, LOW);
  pinMode(ARM_LED, OUTPUT);        digitalWrite(ARM_LED, LOW);
  pinMode(PHOTO_TRIG_PIN, OUTPUT); digitalWrite(PHOTO_TRIG_PIN, LOW);
  pinMode(ARM_PIN, INPUT_PULLUP);

  if (!bmp.begin())         fatalBlink(2);  // BMP180 begin failed
  if (!SD.begin(SD_CS_PIN)) fatalBlink(3);  // SD card mount failed

  logFile = SD.open("FLIGHT.CSV", FILE_WRITE);
  if (!logFile)             fatalBlink(4);  // SD open FLIGHT.CSV failed

  logFile.println(F("sample_id,utc_date,utc_time,millis,temp_c,pressure_pa,bmp_alt_m,gps_alt_m,lat,lon,sats,gps_age_ms,state,alt_source,photo_id"));
  logFile.flush();

  gpsSerial.begin(9600);
}

// One 1 Hz cycle: pump GPS bytes, read sensors, run state machine, write a
// CSV row, fire the photo trigger if the current state's interval elapsed,
// then spin the remainder of the second pumping GPS + updating the LEDs.
void loop() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  unsigned long t = millis();
  float temp = bmp.readTemperature();
  int32_t pressure = bmp.readPressure();
  float bmp_alt = bmp.readAltitude(SEA_LEVEL_PA);

  bool gps_alt_valid = gps.altitude.isValid();
  float gps_alt = gps_alt_valid ? gps.altitude.meters() : 0.0;

  pushAltSample(gps_alt, gps_alt_valid, bmp_alt);
  tickStateMachine(bmp_alt, gps_alt, gps_alt_valid);

  // Only write to SD + fire photos while the jumper is pulled (armed).
  // Jumper in = paused: no CSV growth, no photo triggers, state stays WAITING.
  if (armed) {

  if (!sdWriteFailed) digitalWrite(STATUS_LED, HIGH);

  sampleId++;
  logFile.print(sampleId); logFile.print(',');

  char buf[12];
  if (gps.date.isValid()) {
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u",
             gps.date.year(), gps.date.month(), gps.date.day());
    logFile.print(buf);
  }
  logFile.print(',');
  if (gps.time.isValid()) {
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
             gps.time.hour(), gps.time.minute(), gps.time.second());
    logFile.print(buf);
  }
  logFile.print(',');

  logFile.print(t);           logFile.print(',');
  logFile.print(temp, 1);     logFile.print(',');
  logFile.print(pressure);    logFile.print(',');
  logFile.print(bmp_alt, 1);  logFile.print(',');
  if (gps_alt_valid) logFile.print(gps_alt, 1);
  logFile.print(',');

  bool hasFix = gps.location.isValid();
  if (hasFix) {
    logFile.print(gps.location.lat(), 6); logFile.print(',');
    logFile.print(gps.location.lng(), 6); logFile.print(',');
    logFile.print(gps.satellites.value()); logFile.print(',');
    logFile.print(gps.location.age());
  } else {
    logFile.print(",,");
    logFile.print(gps.satellites.isValid() ? gps.satellites.value() : 0);
    logFile.print(",");
  }
  logFile.print(',');
  logFile.print((int)state);             logFile.print(',');
  logFile.print(altSourceForState());    logFile.print(',');

  unsigned long interval = photoIntervalMs(state);
  if (interval > 0 && millis() - lastPhotoMs >= interval) {
    digitalWrite(PHOTO_TRIG_PIN, HIGH);
    delayMicroseconds(500);
    digitalWrite(PHOTO_TRIG_PIN, LOW);
    logFile.print(photoId);
    photoId++;
    lastPhotoMs = millis();
  }
  logFile.println();
  logFile.flush();

  if (logFile.getWriteError()) {
    sdWriteFailed = true;
    logFile.clearWriteError();
  } else if (!sdWriteFailed) {
    digitalWrite(STATUS_LED, LOW);
  }

  } // end if (armed)

  unsigned long nextLog = t + LOG_INTERVAL_MS;
  while (millis() < nextLog) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());
    updateGpsLed();
    updateStatusLed();
    updateArmLed();
  }
}
