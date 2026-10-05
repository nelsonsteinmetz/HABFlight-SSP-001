// balloon_cam — ESP32-CAM trigger sketch for the weather balloon payload.
// Captures one JPEG to SD each time GPIO 3 sees a rising edge from the Pro Mini
// (balloon_datalog D6 → this GPIO 3). Files are named img_NNNNNN.jpg and paired
// on the ground with the Pro Mini CSV's photo_id column.
//
// Board: AI-Thinker ESP32-CAM.
// Trigger input: GPIO 3 (U0RXD), INPUT_PULLDOWN, safe from SD-slot coupling.
// Status LED: onboard red LED GPIO 33 (active-LOW), blinks on capture.
// SD_MMC in 1-bit mode so GPIO 12/13 stay free and quiet.
//
// WiFi + Bluetooth are explicitly disabled at boot to keep the ESP's 2.4 GHz
// radio from desensitising the ATGM336H GPS receiver sitting a few cm away.

#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include "esp_wifi.h"
#include "esp_bt.h"

// Uncomment for standalone bench test: fires an internal trigger every N ms.
// #define SELF_TEST_MS 5000

// AI-Thinker ESP32-CAM camera pinout
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define TRIGGER_PIN  3   // U0RXD; safe from SD-slot coupling that plagues GPIO 12/13
#define STATUS_LED   33

volatile bool capture_flag = false;
volatile uint32_t last_trigger_ms = 0;
uint32_t next_photo_id = 0;

// Debounce in the ISR: ignore rising edges within TRIGGER_LOCKOUT_MS of the last accepted one.
// Real triggers are seconds apart; EMI coupling from adjacent SPI SCK arrives in nanosecond bursts.
#define TRIGGER_LOCKOUT_MS 500
void IRAM_ATTR on_trigger() {
  uint32_t now = millis();
  if (now - last_trigger_ms >= TRIGGER_LOCKOUT_MS) {
    last_trigger_ms = now;
    capture_flag = true;
  }
}

static void led_on()  { digitalWrite(STATUS_LED, LOW); }
static void led_off() { digitalWrite(STATUS_LED, HIGH); }

static void blink(uint8_t n, uint16_t period_ms) {
  for (uint8_t i = 0; i < n; i++) {
    led_on();  delay(period_ms / 2);
    led_off(); delay(period_ms / 2);
  }
}

// Scan SD root and resume the counter past the highest existing img_NNNNNN.jpg
// so a mid-flight reboot doesn't overwrite earlier photos.
static void resume_photo_counter() {
  File root = SD_MMC.open("/");
  if (!root) return;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String name = String(f.name());
    if (name.startsWith("/")) name = name.substring(1);
    if (name.startsWith("img_") && name.endsWith(".jpg")) {
      uint32_t id = (uint32_t) name.substring(4, name.length() - 4).toInt();
      if (id + 1 > next_photo_id) next_photo_id = id + 1;
    }
    f.close();
  }
  root.close();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[balloon_cam] boot");

  // Kill WiFi + Bluetooth before anything else. The 2.4 GHz radio would
  // desense the GPS LNA sitting a few cm away in the payload. We don't need
  // either radio for the trigger-and-capture mission.
  esp_wifi_stop();
  esp_wifi_deinit();
  btStop();
  esp_bt_controller_deinit();
  Serial.println("[ok] WiFi + BT disabled");

  // Drop CPU from 240 MHz to 80 MHz. Still fast enough for JPEG encoding
  // + SD writes, and it cuts power draw and 240-MHz-family EMI harmonics
  // that fall near the GPS L1 band (1575 MHz).
  setCpuFrequencyMhz(80);
  Serial.printf("[ok] CPU @ %d MHz\n", getCpuFrequencyMhz());

  // Silence any core-library debug prints we didn't ask for.
  Serial.setDebugOutput(false);

  pinMode(STATUS_LED, OUTPUT);
  led_off();

  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0       = Y2_GPIO_NUM;
  cfg.pin_d1       = Y3_GPIO_NUM;
  cfg.pin_d2       = Y4_GPIO_NUM;
  cfg.pin_d3       = Y5_GPIO_NUM;
  cfg.pin_d4       = Y6_GPIO_NUM;
  cfg.pin_d5       = Y7_GPIO_NUM;
  cfg.pin_d6       = Y8_GPIO_NUM;
  cfg.pin_d7       = Y9_GPIO_NUM;
  cfg.pin_xclk     = XCLK_GPIO_NUM;
  cfg.pin_pclk     = PCLK_GPIO_NUM;
  cfg.pin_vsync    = VSYNC_GPIO_NUM;
  cfg.pin_href     = HREF_GPIO_NUM;
  cfg.pin_sccb_sda = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn     = PWDN_GPIO_NUM;
  cfg.pin_reset    = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 10000000;  // 10 MHz. Half the default 20 MHz — camera works fine, less HF emission.
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.grab_mode    = CAMERA_GRAB_LATEST;
  cfg.frame_size   = FRAMESIZE_UXGA;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  cfg.jpeg_quality = 8;   // 0=best/large, 63=worst/small. <8 can overflow the driver's JPEG buffer at UXGA → fb_get returns NULL.
  cfg.fb_count     = 1;   // Single-shot capture — no need for the second framebuffer (~200 KB PSRAM saved).

  if (!psramFound()) {
    Serial.println("[warn] no PSRAM — dropping to SVGA/DRAM");
    cfg.frame_size   = FRAMESIZE_SVGA;
    cfg.fb_location  = CAMERA_FB_IN_DRAM;
    cfg.jpeg_quality = 10;
    cfg.fb_count     = 1;
  }

  if (esp_camera_init(&cfg) != ESP_OK) {
    Serial.println("[fatal] camera init failed");
    while (true) blink(1, 100);
  }
  Serial.println("[ok] camera");
  blink(3, 400);

  // 1-bit SD mode: uses only GPIO 2/14/15 for the card, frees 4/12/13 for other uses.
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[fatal] SD_MMC init failed");
    while (true) blink(1, 200);
  }
  Serial.println("[ok] SD_MMC (1-bit)");

  resume_photo_counter();
  Serial.printf("[ok] next id = %lu\n", next_photo_id);
  blink(2, 200);

  pinMode(TRIGGER_PIN, INPUT_PULLDOWN);
  attachInterrupt(TRIGGER_PIN, on_trigger, RISING);
  Serial.printf("[ok] armed on GPIO %d\n", TRIGGER_PIN);
}

void loop() {
#ifdef SELF_TEST_MS
  static uint32_t last = 0;
  if (millis() - last >= SELF_TEST_MS) { last = millis(); capture_flag = true; }
#endif

  if (!capture_flag) return;
  capture_flag = false;

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[err] fb_get");
    blink(4, 60);
    return;
  }

  char path[32];
  snprintf(path, sizeof(path), "/img_%06lu.jpg", next_photo_id);

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[err] open %s\n", path);
    esp_camera_fb_return(fb);
    blink(4, 60);
    return;
  }

  size_t len = fb->len;
  size_t written = f.write(fb->buf, len);
  f.close();
  esp_camera_fb_return(fb);

  if (written != len) {
    Serial.printf("[err] short write %s (%u/%u)\n", path, (unsigned)written, (unsigned)len);
    blink(4, 60);
    return;
  }

  Serial.printf("[cap] %s (%u bytes)\n", path, (unsigned)len);
  next_photo_id++;
  led_on(); delay(50); led_off();
}
