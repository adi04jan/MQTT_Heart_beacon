#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "HttpsOTAUpdate.h"
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "esp_sleep.h"
#include "esp_sntp.h"
#include <time.h>
#include "credential.h"
#include "beacon_logic.h"

//#define TEST_ADC 1
#define DUMMY_VOLTAGE 5096
#define CURRENT_FIRMWARE_VERSION "0.1.2"  // Change this as needed
#define USER_BTN 0
#define LED_PIN 2
#define BATT_PIN A3
#define PIXEL_COUNT 9
#define MIN_MV 3300
#define LOW_BATT_MV 3450
#define MAX_MV 4150

const uint32_t WIFI_FAST_TIMEOUT_MS = 5000;               // cached AP + channel, no scan
const uint32_t WIFI_SCAN_TIMEOUT_MS = 10000;              // per known SSID found by a scan
const uint32_t MQTT_RETRY_MS = 5000;
const uint32_t OFFLINE_SLEEP_AFTER_MS = 5UL * 60 * 1000;  // offline this long -> deep sleep
RTC_DATA_ATTR uint8_t offline_fails = 0;                   // ... and retry after offline_retry_s(): 1, 3, then 15 min
const uint32_t BATT_CHECK_MS = 15000;
const uint32_t BATT_REPORT_MS = 30UL * 60 * 1000;
const uint32_t OTA_TIMEOUT_MS = 120000;
// ponytail: v1 payloads carry no timestamp, so a sleep command arriving this soon after (re)connect is assumed to
// be queued from while we were away and ignored. v2 adds timestamps.
const uint32_t SLEEP_GUARD_MS = 60000;
uint32_t mqtt_up_at = 0;

// Office hours, local time. A bench build may override these in its credential.h.
#ifndef OFFICE_DAYS
#define OFFICE_DAYS 0b01111110  // Mon-Sat (bit n = tm_wday n, 0 = Sunday)
#define OFFICE_START_MIN (9 * 60)
#define OFFICE_STOP_MIN (19 * 60)
#endif
#ifndef MAX_SLEEP_S
#define MAX_SLEEP_S (3UL * 3600)  // ponytail: the RC sleep timer drifts; wake and resync NTP at least this often
#endif
const Schedule OFFICE = { OFFICE_DAYS, OFFICE_START_MIN, OFFICE_STOP_MIN };
const char *TZ_INFO = "IST-5:30";
const uint32_t SCHED_GRACE_MS = 5UL * 60 * 1000;  // out of hours, stay up this long after power-on or a press
uint32_t hold_start = 0, hold_ms = 0;             // schedule sleep held off while millis() - hold_start < hold_ms
RTC_DATA_ATTR time_t wake_at = 0;                  // end of a timed sleep in progress (0 = none)

WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_NeoPixel strip(PIXEL_COUNT, LED_PIN, NEO_GRB + NEO_KHZ400);
Button button;

const uint8_t total_ssid_count = sizeof(ssids) / sizeof(ssids[0]);
bool STATUS_FLAG = true;
bool SLEEP_FLAG = false;
RTC_DATA_ATTR bool pending_heart = false;  // button pressed while offline: send on reconnect, survives sleep

// Last good AP, kept across deep sleep so a timer/button wake reconnects without a scan.
RTC_DATA_ATTR int8_t cached_idx = -1;
RTC_DATA_ATTR int32_t cached_channel = 0;
RTC_DATA_ATTR uint8_t cached_bssid[6];

// esp_https_ota needs *a* cert configured; with an http:// base_url it is ignored (plain TCP).
static const char *server_certificate = "-----BEGIN CERTIFICATE-----\n"
                                        "MIIEkjCCA3qgAwIBAgIQCgFBQgAAAVOFc2oLheynCDANBgkqhkiG9w0BAQsFADA/\n"
                                        "MSQwIgYDVQQKExtEaWdpdGFsIFNpZ25hdHVyZSBUcnVzdCBDby4xFzAVBgNVBAMT\n"
                                        "DkRTVCBSb290IENBIFgzMB4XDTE2MDMxNzE2NDA0NloXDTIxMDMxNzE2NDA0Nlow\n"
                                        "SjELMAkGA1UEBhMCVVMxFjAUBgNVBAoTDUxldCdzIEVuY3J5cHQxIzAhBgNVBAMT\n"
                                        "GkxldCdzIEVuY3J5cHQgQXV0aG9yaXR5IFgzMIIBIjANBgkqhkiG9w0BAQEFAAOC\n"
                                        "AQ8AMIIBCgKCAQEAnNMM8FrlLke3cl03g7NoYzDq1zUmGSXhvb418XCSL7e4S0EF\n"
                                        "q6meNQhY7LEqxGiHC6PjdeTm86dicbp5gWAf15Gan/PQeGdxyGkOlZHP/uaZ6WA8\n"
                                        "SMx+yk13EiSdRxta67nsHjcAHJyse6cF6s5K671B5TaYucv9bTyWaN8jKkKQDIZ0\n"
                                        "Z8h/pZq4UmEUEz9l6YKHy9v6Dlb2honzhT+Xhq+w3Brvaw2VFn3EK6BlspkENnWA\n"
                                        "a6xK8xuQSXgvopZPKiAlKQTGdMDQMc2PMTiVFrqoM7hD8bEfwzB/onkxEz0tNvjj\n"
                                        "/PIzark5McWvxI0NHWQWM6r6hCm21AvA2H3DkwIDAQABo4IBfTCCAXkwEgYDVR0T\n"
                                        "AQH/BAgwBgEB/wIBADAOBgNVHQ8BAf8EBAMCAYYwfwYIKwYBBQUHAQEEczBxMDIG\n"
                                        "CCsGAQUFBzABhiZodHRwOi8vaXNyZy50cnVzdGlkLm9jc3AuaWRlbnRydXN0LmNv\n"
                                        "bTA7BggrBgEFBQcwAoYvaHR0cDovL2FwcHMuaWRlbnRydXN0LmNvbS9yb290cy9k\n"
                                        "c3Ryb290Y2F4My5wN2MwHwYDVR0jBBgwFoAUxKexpHsscfrb4UuQdf/EFWCFiRAw\n"
                                        "VAYDVR0gBE0wSzAIBgZngQwBAgEwPwYLKwYBBAGC3xMBAQEwMDAuBggrBgEFBQcC\n"
                                        "ARYiaHR0cDovL2Nwcy5yb290LXgxLmxldHNlbmNyeXB0Lm9yZzA8BgNVHR8ENTAz\n"
                                        "MDGgL6AthitodHRwOi8vY3JsLmlkZW50cnVzdC5jb20vRFNUUk9PVENBWDNDUkwu\n"
                                        "Y3JsMB0GA1UdDgQWBBSoSmpjBH3duubRObemRWXv86jsoTANBgkqhkiG9w0BAQsF\n"
                                        "AAOCAQEA3TPXEfNjWDjdGBX7CVW+dla5cEilaUcne8IkCJLxWh9KEik3JHRRHGJo\n"
                                        "uM2VcGfl96S8TihRzZvoroed6ti6WqEBmtzw3Wodatg+VyOeph4EYpr/1wXKtx8/\n"
                                        "wApIvJSwtmVi4MFU5aMqrSDE6ea73Mj2tcMyo5jMd6jmeWUHK8so/joWUoHOUgwu\n"
                                        "X4Po1QYz+3dszkDqMp4fklxBwXRsW10KXzPMTZ+sOPAveyxindmjkW8lGy+QsRlG\n"
                                        "PfZ+G6Z6h7mjem0Y+iWlkYcV4PIWL1iwBi8saCbGS5jN2p8M+X+Q7UNKEkROb3N6\n"
                                        "KOqkqm57TH2H3eDJAkSnh6/DNFu0Qg==\n"
                                        "-----END CERTIFICATE-----";

void colorWipe(uint32_t color, int wait) {
  for (uint16_t i = 0; i < strip.numPixels(); i++) {
    strip.setPixelColor(i, color);
    strip.show();
    delay(wait);
  }
}

void heartbeat_effect(uint32_t color, int times, int fadeDelay) {
  for (int t = 0; t < times; t++) {
    for (int b = 0; b <= 255; b += 5) {  // fade in
      strip.fill(strip.Color(((color >> 16) & 0xFF) * b / 255, ((color >> 8) & 0xFF) * b / 255, (color & 0xFF) * b / 255));
      strip.show();
      delay(fadeDelay);
    }
    for (int b = 255; b >= 0; b -= 5) {  // fade out
      strip.fill(strip.Color(((color >> 16) & 0xFF) * b / 255, ((color >> 8) & 0xFF) * b / 255, (color & 0xFF) * b / 255));
      strip.show();
      delay(fadeDelay);
    }
    delay(100);
  }
}

uint32_t check_batt_voltage() {
  static uint32_t samples[10] = { 0 };
  static bool initialized = false;
  for (int j = 9; j > 0; j--) samples[j] = samples[j - 1];
  samples[0] = analogReadMilliVolts(BATT_PIN);
  if (!initialized && samples[0] != 0) {
    for (int j = 1; j < 10; j++) samples[j] = samples[0];
    initialized = true;
  }
  uint32_t mv = 0;
  for (int j = 0; j < 10; j++) mv += samples[j];
  mv /= 5;  // mean of 10 samples x2 for the 1:2 divider
#ifdef TEST_ADC
  mv = DUMMY_VOLTAGE;
#endif
  Serial.printf("Battery voltage is %lu\n", (unsigned long)mv);
  return mv;
}

void battery_level_show() {
  int lit = battery_pixels(check_batt_voltage(), MIN_MV, MAX_MV, PIXEL_COUNT);
  for (int x = 0; x < PIXEL_COUNT; x++) {
    // red -> yellow over pixels 0..4, yellow -> green over 5..8
    uint8_t r = x <= 4 ? 255 : map(x, 5, 8, 255, 0);
    uint8_t g = x <= 4 ? map(x, 0, 4, 0, 255) : 255;
    strip.setPixelColor(x, x < lit ? strip.Color(r, g, 0) : 0);
    if (x < lit) {
      strip.show();
      delay(80);
    }
  }
  strip.show();
}

void publishMqttMessage(const char *topic, const char *message) {
  JsonDocument doc;
  doc["id"] = WiFi.macAddress();
  doc["message"] = message;
  doc["volt"] = check_batt_voltage() / 1000.0;
  char buffer[384];  // details reply carries wifi + last sleep + boot trail
  size_t n = serializeJson(doc, buffer);
  // uint8_t* overload: the old char* call matched publish(topic, payload, retained) and retained every message.
  bool ok = client.publish(topic, (const uint8_t *)buffer, n, false);
  Serial.printf("[MQTT] %s: %s\n", ok ? "Sent" : "SEND FAILED", buffer);
}

RTC_DATA_ATTR char last_sleep[64];  // why / when / how long the last deep sleep was; reported at boot and in details
// Boot breadcrumbs (B<cause> W/w wifi T/t clock O ota M/m<rc> mqtt): survive sleep, so a wake that dies before
// MQTT still shows where it stopped. prev_trail = the previous boot's trail.
RTC_DATA_ATTR char trail[48];
char prev_trail[48];
void crumb(const char *s) {
  strncat(trail, s, sizeof(trail) - strlen(trail) - 1);
}

void deep_sleep(uint64_t timer_us, const char *why) {
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(last_sleep, sizeof(last_sleep), "%s at %02d:%02d:%02d for %llus", why, lt.tm_hour, lt.tm_min, lt.tm_sec,
           timer_us / 1000000ULL);
  if (client.connected()) client.disconnect();  // broker marks us offline now and starts queueing
  strip.clear();
  strip.show();
  esp_deep_sleep_enable_gpio_wakeup(1ULL << USER_BTN, ESP_GPIO_WAKEUP_GPIO_LOW);
  if (timer_us) esp_sleep_enable_timer_wakeup(timer_us);
  Serial.printf("Going for deep sleep: %s\n", last_sleep);
  Serial.flush();
  esp_deep_sleep_start();
}

static bool clock_valid() {
  return time(nullptr) > 1700000000;
}

// Office-schedule seconds from now: until hours start again (0 = inside hours), or until the strictly next start.
// 0 when the clock is unknown, so an unsynced beacon stays awake.
static uint32_t schedule_seconds(bool next_start) {
  if (!clock_valid()) return 0;
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  uint32_t sod = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
  return next_start ? seconds_to_next_start(OFFICE, lt.tm_wday, sod) : seconds_until_on(OFFICE, lt.tm_wday, sod);
}

// Deep sleep until epoch t, in chunks of at most MAX_SLEEP_S (each wake resyncs the clock), or until the button.
void sleep_until(time_t t, const char *why) {
  wake_at = t;
  time_t now = time(nullptr);
  uint64_t s = t > now ? (uint64_t)(t - now) : 1;
  if (s > MAX_SLEEP_S) s = MAX_SLEEP_S;
  Serial.printf("Sleeping until %s", ctime(&t));
  deep_sleep(s * 1000000ULL, why);
}

void sync_time() {
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  uint32_t t0 = millis();
  while (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - t0 < 3000) delay(50);
  time_t t = time(nullptr);
  Serial.printf("Clock %s: %s", clock_valid() ? "ok" : "UNKNOWN", ctime(&t));
}

// Offline: sleep until office hours if we are outside them, else retry after 1, 3, then 15 min (a weak-signal
// miss right at office start used to cost 15 min). Button wakes either way.
void sleep_offline() {
  uint32_t off = schedule_seconds(false);
  if (off) sleep_until(time(nullptr) + off, "offline, out of hours");
  uint32_t s = offline_retry_s(offline_fails);
  if (offline_fails < 255) offline_fails++;
  deep_sleep(s * 1000000ULL, "offline");
}

// Announced sleep. Commanded: until the next office start (or button). Low battery: button only.
void go_to_sleep(bool until_office) {
  if (client.connected()) {
    publishMqttMessage(MQTT_TOPIC, MQTT_MSG_4);
    delay(1000);  // let TCP flush
  }
  uint32_t s = until_office ? schedule_seconds(true) : 0;
  if (s) sleep_until(time(nullptr) + s, "sleep command");
  wake_at = 0;
  deep_sleep(0, until_office ? "sleep command, clock unknown" : "low battery");
}

void low_batt_notify() {
  uint32_t mv = check_batt_voltage();
  if (mv > LOW_BATT_MV) return;
  Serial.printf("Low Battery Detected: %lu mV\n", (unsigned long)mv);
  for (int i = 0; i < 3; i++) {
    strip.setPixelColor(0, strip.Color(200, 0, 0));
    strip.show();
    delay(500);
    strip.setPixelColor(0, 0);
    strip.show();
    delay(500);
  }
  if (mv <= MIN_MV) go_to_sleep(false);
}

static bool wait_connected(uint32_t timeout_ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(50);
  }
  return false;
}

static void remember_ap(int idx, uint32_t t0, const char *how) {
  cached_idx = idx;
  cached_channel = WiFi.channel();
  memcpy(cached_bssid, WiFi.BSSID(), sizeof(cached_bssid));
  Serial.printf("WiFi %s: %s ch%ld %lddBm in %lums, IP %s\n", how, ssids[idx], (long)cached_channel,
                (long)WiFi.RSSI(), (unsigned long)(millis() - t0), WiFi.localIP().toString().c_str());
}

// Fast path: last AP's BSSID + channel (no scan). Slow path: scan, then each known SSID once
// (mesh duplicates skipped), using the scanned BSSID + channel.
// ponytail: relies on the IDF scan list being strongest-first; sort by RSSI if a weak mesh node gets picked.
bool connect_wifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  uint32_t t0 = millis();
  if (cached_idx >= 0 && cached_idx < total_ssid_count) {
    WiFi.begin(ssids[cached_idx], passwords[cached_idx], cached_channel, cached_bssid);
    if (wait_connected(WIFI_FAST_TIMEOUT_MS)) {
      remember_ap(cached_idx, t0, "fast");
      return true;
    }
    WiFi.disconnect();
    cached_idx = -1;
  }
  int networks = WiFi.scanNetworks();
  Serial.printf("WiFi scan: %d networks in %lums\n", networks, (unsigned long)(millis() - t0));
  uint32_t tried = 0;
  for (int i = 0; i < networks; i++) {
    for (uint8_t j = 0; j < total_ssid_count && j < 32; j++) {
      if ((tried >> j & 1) || WiFi.SSID(i) != ssids[j]) continue;
      tried |= 1UL << j;
      WiFi.begin(ssids[j], passwords[j], WiFi.channel(i), WiFi.BSSID(i));
      if (wait_connected(WIFI_SCAN_TIMEOUT_MS)) {
        WiFi.scanDelete();
        remember_ap(j, t0, "scan");
        return true;
      }
      WiFi.disconnect();
      Serial.printf("WiFi: failed to join %s\n", ssids[j]);
    }
  }
  WiFi.scanDelete();
  Serial.println("WiFi: no known network reachable");
  return false;
}

void perform_ota() {
  HTTPClient http_data;
  http_data.begin(String(base_url) + "version.txt");
  int httpCode = http_data.GET();
  if (httpCode != 200) {
    Serial.printf("Failed to fetch version.txt, HTTP code: %d\n", httpCode);
    http_data.end();
    return;
  }
  String new_version = http_data.getString();
  new_version.trim();
  http_data.end();
  Serial.printf("Current Version: %s | New Version: %s\n", CURRENT_FIRMWARE_VERSION, new_version.c_str());
  if (!version_is_newer(new_version.c_str(), CURRENT_FIRMWARE_VERSION)) return;  // upgrade only

  String firmware_url = String(base_url) + "firmware_" + new_version + ".bin";
  Serial.println("Starting OTA: " + firmware_url);
  HttpsOTA.onHttpEvent([](HttpEvent_t *) {});  // required: HttpsOTA calls the callback unconditionally
  HttpsOTA.begin(firmware_url.c_str(), server_certificate, false);
  uint32_t t0 = millis();
  bool on = false;
  while (millis() - t0 < OTA_TIMEOUT_MS) {
    HttpsOTAStatus_t st = HttpsOTA.status();
    if (st == HTTPS_OTA_SUCCESS) {
      colorWipe(strip.Color(0, 255, 0), 50);
      Serial.println("Firmware written, rebooting");
      ESP.restart();
    }
    if (st == HTTPS_OTA_FAIL) {
      colorWipe(strip.Color(255, 0, 0), 50);
      Serial.println("Firmware Upgrade Fail");
      return;
    }
    strip.fill((on = !on) ? strip.Color(255, 0, 255) : strip.Color(50, 0, 50));
    strip.show();
    delay(500);
  }
  Serial.println("OTA taking too long, continuing");  // download task keeps going; image applies on next reset
  colorWipe(strip.Color(255, 165, 0), 50);
}

String get_connected_wifi_info() {
  if (WiFi.status() != WL_CONNECTED) return "Not connected to any WiFi.";
  return "SSID: " + WiFi.SSID() + " | RSSI: " + String(WiFi.RSSI()) + " dBm";
}

void callback(char *topic, byte *payload, unsigned int length) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.printf("[MQTT] bad JSON on %s: %s\n", topic, error.c_str());
    return;
  }
  const char *msg = doc["message"];
  if (!msg) return;
  Serial.printf("[MQTT] rx: %s\n", msg);

  if (strcmp(msg, MQTT_MSG_1) == 0) {
    heartbeat_effect(strip.Color(250, 0, 0), 3, 10);  // red
  } else if (strcmp(msg, MQTT_MSG_2) == 0) {
    if (WiFi.macAddress() != (doc["id"] | "")) heartbeat_effect(strip.Color(0, 250, 0), 3, 10);  // green, not our own echo
  } else if (strcmp(msg, MQTT_MSG_SORRY_1) == 0 || strcmp(msg, MQTT_MSG_SORRY_2) == 0) {
    heartbeat_effect(strip.Color(235, 161, 23), 3, 10);  // orange
  } else if (strcmp(msg, MQTT_MSG_ANNOY_1) == 0 || strcmp(msg, MQTT_MSG_ANNOY_2) == 0) {
    heartbeat_effect(strip.Color(23, 231, 235), 3, 10);  // cyan
  } else if (strcmp(msg, MQTT_GO_SLEEP) == 0) {
    if (millis() - mqtt_up_at < SLEEP_GUARD_MS) {
      Serial.println("[MQTT] sleep command right after connect: stale, ignored");
      return;
    }
    heartbeat_effect(strip.Color(128, 37, 247), 1, 5);  // purple
    SLEEP_FLAG = true;
  } else if (strcmp(msg, MQTT_MSG_ONLINE_1) == 0 || strcmp(msg, MQTT_MSG_ONLINE_2) == 0) {
    STATUS_FLAG = true;
  }
}

bool mqtt_connect() {
  if (!connect_wifi()) return false;
  String id = "heart-" + WiFi.macAddress();
  id.replace(":", "");
  // Persistent session (cleanSession=false) + QoS 1 subscription: the broker queues messages while we sleep or are
  // offline and delivers them on reconnect. QoS 0 senders (beacons, the bridge) also need
  // `queue_qos0_messages true` on the broker.
  if (!client.connect(id.c_str(), MQTT_USER, MQTT_PASS, nullptr, 0, false, nullptr, false)) {
    Serial.printf("MQTT connect failed, rc=%d\n", client.state());
    char c[8];
    snprintf(c, sizeof(c), "m%d", client.state());
    crumb(c);
    return false;
  }
  mqtt_up_at = millis();
  offline_fails = 0;
  Serial.println("MQTT connected as " + id);
  crumb("M");
  client.subscribe(MQTT_TOPIC, 1);
  publishMqttMessage(MQTT_TOPIC, MQTT_MSG_2);  // Online
  if (pending_heart) {
    pending_heart = false;
    publishMqttMessage(MQTT_TOPIC, MQTT_MSG_1);
  }
  return true;
}

void send_heart() {
  if (client.connected()) publishMqttMessage(MQTT_TOPIC, MQTT_MSG_1);
  else pending_heart = true;
}

void setup() {
  setCpuFrequencyMhz(80);  // lowest clock WiFi allows
  Serial.begin(115200);
  setenv("TZ", TZ_INFO, 1);  // local time from the RTC works before WiFi/NTP
  tzset();
  analogReadResolution(12);
  pinMode(USER_BTN, INPUT_PULLUP);
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool quiet = cause == ESP_SLEEP_WAKEUP_TIMER;  // offline retry or night check-in: no animations
  if (!quiet) wake_at = 0;                       // power-on or button: the user wants it up now
  Serial.printf("Heart beacon %s, wakeup cause %d, last sleep: %s\n", CURRENT_FIRMWARE_VERSION, (int)cause,
                last_sleep[0] ? last_sleep : "-");
  memcpy(prev_trail, trail, sizeof(trail));
  Serial.printf("Previous boot trail: %s\n", prev_trail[0] ? prev_trail : "-");
  trail[0] = 0;
  char c[8];
  snprintf(c, sizeof(c), "B%d", (int)cause);
  crumb(c);
  strip.begin();
  strip.show();
  button.begin(digitalRead(USER_BTN) == LOW, millis());
  low_batt_notify();
  if (!quiet) battery_level_show();

  WiFi.persistent(false);  // keep STA config in RAM: no NVS write on every begin()
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_MAX_MODEM);  // listen interval defaults to 3 beacons
  bool online = false;
  for (int round = 0; round < (quiet ? 1 : 3) && !online; round++) {
    if (round > 0) heartbeat_effect(strip.Color(0, 0, 100), 1, 10);  // blue: still searching
    online = connect_wifi();
  }
  crumb(online ? "W" : "w");
  if (online) {
    sync_time();
    crumb(clock_valid() ? "T" : "t");
  }
  // A timed sleep is still running (night check-in): straight back to sleep, no MQTT, so nothing plays at night.
  if (wake_at && clock_valid() && time(nullptr) < wake_at - 30) sleep_until(wake_at, "check-in, still out of hours");
  wake_at = 0;
  if (!online) sleep_offline();
  if (!quiet) {
    heartbeat_effect(strip.Color(120, 120, 120), 1, 10);  // white: connected
    hold_start = millis();
    hold_ms = SCHED_GRACE_MS;
  }
  STATUS_FLAG = !quiet;  // details reply only on user-started boots (the bridge relays it to Telegram)

  perform_ota();
  crumb("O");
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setKeepAlive(60);      // fewer pings than the 15 s default
  client.setBufferSize(1024);   // bridge messages carry the raw Telegram text
  strip.clear();
  strip.show();
  Serial.println("setup Complete");
}

void loop() {
  static uint32_t last_mqtt_try = 0, offline_since = 0, last_batt = 0, last_report = 0, last_sched = 0;
  uint32_t now = millis();

  if (!client.connected()) {
    if (!offline_since) offline_since = now | 1;
    if (every(now, last_mqtt_try, MQTT_RETRY_MS) && mqtt_connect()) offline_since = 0;
    else if (now - offline_since > OFFLINE_SLEEP_AFTER_MS) sleep_offline();
  }
  client.loop();

  if (every(now, last_sched, 60000) && now - hold_start >= hold_ms) {
    uint32_t off = schedule_seconds(false);
    if (off) {
      Serial.println("Out of office hours");
      sleep_until(time(nullptr) + off, "out of office hours");
    }
  }

  BtnEvent ev = button.update(digitalRead(USER_BTN) == LOW, millis());
  if (ev != BtnEvent::None) {
    hold_start = millis();  // a press out of hours keeps it up a while
    hold_ms = SCHED_GRACE_MS;
  }
  switch (ev) {
    case BtnEvent::Short: send_heart(); break;
    case BtnEvent::Long:
      battery_level_show();
      delay(500);
      colorWipe(0, 100);
      break;
    default: break;
  }

  if (every(now, last_batt, BATT_CHECK_MS)) low_batt_notify();
  if (every(now, last_report, BATT_REPORT_MS) && client.connected()) publishMqttMessage(MQTT_TOPIC, MQTT_MSG_3);

  if (STATUS_FLAG && client.connected()) {
    STATUS_FLAG = false;
    String final_msg = String(CURRENT_FIRMWARE_VERSION) + " | " + get_connected_wifi_info() + " | last sleep: " +
                       (last_sleep[0] ? last_sleep : "-") + " | prev boot: " + (prev_trail[0] ? prev_trail : "-");
    publishMqttMessage(MQTT_TOPIC, final_msg.c_str());
  }
  if (SLEEP_FLAG) {
    SLEEP_FLAG = false;
    go_to_sleep(true);
  }
  delay(10);
}
