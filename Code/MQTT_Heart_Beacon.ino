#include <Adafruit_NeoPixel.h>
#include "WiFi.h"
#include <HTTPClient.h>
#include "HttpsOTAUpdate.h"
#include <PubSubClient.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "credential.h"
#include <ArduinoJson.h>

//#define TEST_ADC 1
#define DUMMY_VOLTAGE 5096
#define CURRENT_FIRMWARE_VERSION "0.0.9"  // Change this as needed
#define USER_BTN 0
#define LED_PIN 2
#define OTA_PIN 10
#define BATT_PIN A3
#define PIXEL_COUNT 9
#define MIN_VOLTAGE 3.3
#define LOW_BATT_VOLTAGE 3.45
#define MAX_VOLTAGE 4.15
#define WAKEUP_THRESHOLD_VOLTAGE 3.5
#define WIFI_TIMEOUT 60


WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_NeoPixel strip(PIXEL_COUNT, LED_PIN, NEO_GRB + NEO_KHZ400);

const uint8_t total_ssid_count = sizeof(ssids) / sizeof(ssids[0]);
bool WIFI_STATUS = false;
bool STATUS_FLAG = true;
bool SLEEP_FLAG = false;
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
  for (int i = 0; i < strip.numPixels(); i++) {  // For each pixel in strip...
    strip.setPixelColor(i, color);               //  Set pixel's color (in RAM)
    strip.show();                                //  Update strip to match
    delay(wait);                                 //  Pause for a moment
  }
}

void heartbeat_effect(uint32_t color, int times, int fadeDelay) {
  for (int t = 0; t < times; t++) {
    // Fade in
    for (int b = 0; b <= 255; b += 5) {
      uint32_t faded = strip.Color(
        (uint8_t)((color >> 16) & 0xFF) * b / 255,
        (uint8_t)((color >> 8) & 0xFF) * b / 255,
        (uint8_t)(color & 0xFF) * b / 255);
      for (int i = 0; i < PIXEL_COUNT; i++) {
        strip.setPixelColor(i, faded);
      }
      strip.show();
      delay(fadeDelay);
    }

    // Fade out
    for (int b = 255; b >= 0; b -= 5) {
      uint32_t faded = strip.Color(
        (uint8_t)((color >> 16) & 0xFF) * b / 255,
        (uint8_t)((color >> 8) & 0xFF) * b / 255,
        (uint8_t)(color & 0xFF) * b / 255);
      for (int i = 0; i < PIXEL_COUNT; i++) {
        strip.setPixelColor(i, faded);
      }
      strip.show();
      delay(fadeDelay);
    }

    delay(100);  // Short pause between heartbeats
  }
}

void heartbeatcallback(double x) {
  Serial.println("Hey we're in a heartbeatc callback, ");
  uint32_t color = strip.Color(255, 0, 0);  // Gray-white
  heartbeat_effect(color, 2, 20);
}

void HttpEvent(HttpEvent_t *event) {
  switch (event->event_id) {
    case HTTP_EVENT_ERROR: Serial.println("Http Event Error"); break;
    case HTTP_EVENT_ON_CONNECTED: Serial.println("Http Event On Connected"); break;
    case HTTP_EVENT_HEADER_SENT: Serial.println("Http Event Header Sent"); break;
    case HTTP_EVENT_ON_HEADER: Serial.printf("Http Event On Header, key=%s, value=%s\n", event->header_key, event->header_value); break;
    case HTTP_EVENT_ON_DATA: break;
    case HTTP_EVENT_ON_FINISH: Serial.println("Http Event On Finish"); break;
    case HTTP_EVENT_DISCONNECTED: Serial.println("Http Event Disconnected"); break;
    case HTTP_EVENT_REDIRECT: Serial.println("Http Event Redirect"); break;
  }
}

uint8_t connect_wifi() {
  if (WIFI_STATUS != false || WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi already connected");
    return 0;
  }
  Serial.println("Scanning for available WiFi networks...");

  int networks = WiFi.scanNetworks();
  if (networks == 0) {
    Serial.println("No networks found.");
    return 1;
  }

  for (int i = 0; i < networks; i++) {
    String ssid_found = WiFi.SSID(i);
    Serial.print("Found SSID: ");
    Serial.println(ssid_found);

    for (uint8_t j = 0; j < total_ssid_count; ++j) {
      if (ssid_found == ssids[j]) {
        Serial.print("Known SSID matched: ");
        Serial.println(ssids[j]);

        WiFi.begin(ssids[j], passwords[j]);

        // Try connecting
        for (uint8_t k = 0; k < WIFI_TIMEOUT; ++k) {
          if (WiFi.status() == WL_CONNECTED) {
            WIFI_STATUS = true;
            Serial.print("Connected to ");
            Serial.println(ssids[j]);
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            return 0;
          }
          delay(500);
          Serial.print(".");
        }

        Serial.println("\nFailed to connect to matched SSID.");
      }
    }
  }
  Serial.println("No known SSIDs available.");
  return 1;
}

void perform_ota() {
  static HttpsOTAStatus_t otastatus;
  static int total_ota_time = 0;
  Serial.println("OTA PIN set, Starting OTA...!!!!");
  colorWipe(strip.Color(180, 0, 155), 50);

  // Step 1: Download version.txt
  HTTPClient http_data;
  String version_url = String(base_url) + "version.txt";
  http_data.begin(version_url);
  int httpCode = http_data.GET();

  if (httpCode != 200) {
    Serial.printf("Failed to fetch version.txt, HTTP code: %d\n", httpCode);
    colorWipe(strip.Color(255, 0, 0), 50);  // Red
    http_data.end();
    return;
  }

  String new_version = http_data.getString();
  new_version.trim();  // Remove extra whitespace or newline
  http_data.end();

  Serial.printf("Current Version: %s | New Version: %s\n", CURRENT_FIRMWARE_VERSION, new_version.c_str());

  // Step 2: Compare with current version
  if (new_version.equals(CURRENT_FIRMWARE_VERSION)) {
    Serial.println("Already running the latest firmware.");
    return;
  }

  Serial.println("New firmware available. Starting OTA update...");

  // Step 3: Construct firmware URL
  String firmware_url = String(base_url) + "firmware_" + new_version + ".bin";
  Serial.println("Firmware URL: " + firmware_url);

  ////////////////////////
  HttpsOTA.onHttpEvent(HttpEvent);
  HttpsOTA.begin(firmware_url.c_str(), server_certificate, false);

  while (true) {
    otastatus = HttpsOTA.status();
    total_ota_time++;
    if (otastatus == HTTPS_OTA_SUCCESS) {
      colorWipe(strip.Color(0, 255, 0), 50);  // Green
      Serial.println("Firmware written successfully. To reboot device, call API ESP.restart() or PUSH restart button on device");
      ESP.restart();
    } else if (otastatus == HTTPS_OTA_FAIL) {
      colorWipe(strip.Color(255, 0, 0), 50);  // Red
      Serial.println("Firmware Upgrade Fail");
      break;
    } else {
      colorWipe(strip.Color(50, 0, 50), 100);
      Serial.println("OTA going on");
      delay(1000);
      colorWipe(strip.Color(255, 0, 255), 100);
    }
    if (total_ota_time > 120) {
      total_ota_time = 0;
      Serial.println("OTA taking too long, returning...");
      colorWipe(strip.Color(255, 165, 0), 50);  // Orange
      return;                                   // Exit OTA loop after 2 minutes
    }
    delay(1000);
  }
}

uint32_t check_batt_voltage() {
  uint32_t raw_voltage = 0;
  static uint32_t raw_voltage_arr[10] = { 0 };
  // If array was uninitialized (first run), fill all with first value
  static bool initialized = false;

  // Shift values right
  for (int j = 9; j > 0; j--) {
    raw_voltage_arr[j] = raw_voltage_arr[j - 1];
  }
  // Read new voltage
  raw_voltage_arr[0] = analogReadMilliVolts(BATT_PIN);
  if (!initialized && raw_voltage_arr[0] != 0) {
    for (int j = 1; j < 10; j++) {
      raw_voltage_arr[j] = raw_voltage_arr[0];
    }
    initialized = true;
  }
  // Average the samples
  for (int j = 0; j < 10; j++) {
    raw_voltage += raw_voltage_arr[j];
  }
  raw_voltage /= 5;  // Average of 10 samples but as we have a resitor divider, we take 5 samples to get the average

  // Convert ADC value to millivolts (assuming 3.3V ref and 12-bit ADC)
#ifdef TEST_ADC
  raw_voltage = DUMMY_VOLTAGE;
#endif

  // raw_voltage = (3050 * raw_voltage) / 4095;
  Serial.print("Battery voltage is ");
  Serial.println(raw_voltage);
  return raw_voltage;
}

int battery_level_show() {
  float batt_voltage = check_batt_voltage() / 1000.0;  // assuming check_batt_voltage returns millivolts

  // Clamp voltage between MIN_VOLTAGE and MAX_VOLTAGE
  if (batt_voltage < MIN_VOLTAGE) batt_voltage = MIN_VOLTAGE;
  if (batt_voltage > MAX_VOLTAGE) batt_voltage = MAX_VOLTAGE;

  // Map voltage to LED count
  int num_leds_to_light = (int)(((batt_voltage - MIN_VOLTAGE) / (MAX_VOLTAGE - MIN_VOLTAGE)) * PIXEL_COUNT);

  for (int x = 0; x < PIXEL_COUNT; x++) {
    uint8_t r, g;

    if (x <= 4) {
      // Red to Yellow (0 to 4): Increase Green
      r = 255;
      g = map(x, 0, 4, 0, 255);
    } else {
      // Yellow to Green (5 to 8): Decrease Red
      r = map(x, 5, 8, 255, 0);
      g = 255;
    }

    if (x < num_leds_to_light) {
      strip.setPixelColor(x, strip.Color(r, g, 0));
      strip.show();
      delay(80);  // Color wipe effect
    } else {
      strip.setPixelColor(x, 0);  // Turn off
    }
  }

  strip.show();
  return num_leds_to_light;
}

void no_network() {
  static int ret = 0;
  static int count = 0;
  Serial.println("No WiFi network found, entering contionus scan");
  while (count++ < 10) {
    uint32_t color = strip.Color(0, 0, 100);  // Gray-white
    heartbeat_effect(color, 3, 10);
    ret = connect_wifi();
    if (ret == 0) {
      Serial.println("Connected to WiFi network");
      return;
    }
    Serial.println("Failed to connect to any wifi network");
  }
  ESP.restart();
}

void network_connected() {
  uint32_t color = strip.Color(120, 120, 120);  // Gray-white
  heartbeat_effect(color, 3, 10);               // 3 pulses, smooth 20ms fade step
}

void go_to_sleep() {
  publishMqttMessage(MQTT_TOPIC, MQTT_MSG_4);
  delay(1000);                                                                 // send notify
  esp_deep_sleep_enable_gpio_wakeup(1 << USER_BTN, ESP_GPIO_WAKEUP_GPIO_LOW);  // Wake on LOW (button press)
  // Go into deep sleep
  Serial.println("Going for deep sleep");
  esp_deep_sleep_start();
}

void low_batt_notify() {
  float batt_voltage = check_batt_voltage() / 1000.0;  // assuming check_batt_voltage returns millivolts

  // Clamp voltage between MIN_VOLTAGE and MAX_VOLTAGE
  if (batt_voltage < MIN_VOLTAGE) batt_voltage = MIN_VOLTAGE;
  if (batt_voltage > MAX_VOLTAGE) batt_voltage = MAX_VOLTAGE;

  if (batt_voltage <= LOW_BATT_VOLTAGE) {
    Serial.print("Low Battery Detected: ");
    Serial.println(batt_voltage);

    for (int i = 0; i < 3; i++) {
      strip.setPixelColor(0, strip.Color(200, 0, 0));
      strip.show();
      delay(500);
      strip.setPixelColor(0, strip.Color(0, 0, 0));
      strip.show();
      delay(500);
    }
  }

  if (batt_voltage <= MIN_VOLTAGE) {
    Serial.print("Low Battery Detected: ");
    Serial.println(batt_voltage);

    for (int i = 0; i < 3; i++) {
      strip.setPixelColor(0, strip.Color(200, 0, 0));
      strip.show();
      delay(500);
      strip.setPixelColor(0, strip.Color(0, 0, 0));
      strip.show();
      delay(500);
    }
    go_to_sleep();
  }
  return;
}

String get_connected_wifi_info() {
  if (WiFi.status() == WL_CONNECTED) {
    String ssid = WiFi.SSID();
    long rssi = WiFi.RSSI();
    String wifi_info = "SSID: " + ssid + " | RSSI: " + String(rssi) + " dBm";
    return wifi_info;
  } else {
    return "Not connected to any WiFi.";
  }
}

void callback(char *topic, byte *payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  char jsonBuffer[256];
  if (length >= sizeof(jsonBuffer)) length = sizeof(jsonBuffer) - 1;  // prevent overflow
  memcpy(jsonBuffer, payload, length);
  jsonBuffer[length] = '\0';

  Serial.println(jsonBuffer);

  // Parse JSON
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, jsonBuffer);

  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return;
  }

  const char *msg = doc["message"];
  if (!msg) {
    Serial.println("No 'message' field found in JSON.");
    return;
  }

  Serial.print("Parsed message: ");
  Serial.println(msg);

  if (strcmp(msg, MQTT_MSG_1) == 0) {
    uint32_t color = strip.Color(250, 0, 0);  // Red
    heartbeat_effect(color, 3, 10);
  } else if (strcmp(msg, MQTT_MSG_2) == 0) {
    uint32_t color = strip.Color(0, 250, 0);  // Green
    heartbeat_effect(color, 3, 10);
  } else if (strcmp(msg, MQTT_MSG_SORRY_1) == 0 || strcmp(msg, MQTT_MSG_SORRY_2) == 0) {
    uint32_t color = strip.Color(235, 161, 23);  // Orange
    heartbeat_effect(color, 3, 10);
  } else if (strcmp(msg, MQTT_MSG_ANNOY_1) == 0 || strcmp(msg, MQTT_MSG_ANNOY_2) == 0) {
    uint32_t color = strip.Color(23, 231, 235);  // Blue
    heartbeat_effect(color, 3, 10);
  } else if (strcmp(msg, MQTT_GO_SLEEP) == 0) {
    uint32_t color = strip.Color(128, 37, 247);  // Green
    heartbeat_effect(color, 1, 5);
    SLEEP_FLAG = true;
  } else if (strcmp(msg, MQTT_MSG_ONLINE_1) == 0 || strcmp(msg, MQTT_MSG_ONLINE_2) == 0) {
    STATUS_FLAG = true;
  } else {
    Serial.println("Unknown message received, no LED action.");
  }
  return;
}

void publishMqttMessage(const char *topic, const char *message) {
  float batt_voltage = check_batt_voltage() / 1000.0;  // assuming check_batt_voltage returns millivolts
  StaticJsonDocument<128> doc;
  char buffer[128];

  String chipmac = WiFi.macAddress();  // Get ESP MAC address
  doc["id"] = chipmac;
  doc["message"] = message;
  doc["volt"] = batt_voltage;

  size_t n = serializeJson(doc, buffer);

  // Publish JSON to MQTT
  client.publish(topic, buffer, n);

  Serial.print("[MQTT] Sent: ");
  Serial.println(buffer);
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    connect_wifi();
    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP8266Client-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      publishMqttMessage(MQTT_TOPIC, MQTT_MSG_2);  // ONLINE
      // ... and resubscribe
      client.subscribe(MQTT_TOPIC);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

int check_user_button() {
  if (digitalRead(USER_BTN) == LOW) {
    delay(200);
    if (digitalRead(USER_BTN) == LOW) {
      battery_level_show();
      delay(500);
      colorWipe(strip.Color(0, 0, 0), 100);
      return 0;
    }

    publishMqttMessage(MQTT_TOPIC, MQTT_MSG_1);  // send notify
  }
  return 0;
}

void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();

  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Wakeup caused by external signal using RTC_IO");
      break;
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("Wakeup caused by external signal using RTC_CNTL");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wakeup caused by timer");
      break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      Serial.println("Wakeup caused by touchpad");
      break;
    case ESP_SLEEP_WAKEUP_ULP:
      Serial.println("Wakeup caused by ULP program");
      break;
    case ESP_SLEEP_WAKEUP_GPIO:
      Serial.println("Wakeup caused by GPIO");
      break;
    default:
      Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason);
      break;
  }
}

void setup() {
  static int ret = 0;
  Serial.begin(115200);
  analogReadResolution(12);
  pinMode(USER_BTN, INPUT_PULLUP);
  pinMode(OTA_PIN, INPUT_PULLUP);
  delay(50);
  print_wakeup_reason();
  strip.begin();
  strip.show();
  delay(50);
  Serial.flush();
  low_batt_notify();
  battery_level_show();
  ret = connect_wifi();
  if (ret != 0) {
    Serial.println("Failed to connect to any wifi network");
    no_network();
  }
  network_connected();

  perform_ota();
  strip.show();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  colorWipe(strip.Color(0, 0, 0), 50);  // Off
  Serial.println("setup Complete");
}

void loop() {
  static int batt_time = 0;
  static int batt_push_time = 0;
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
  check_user_button();
  if (batt_time == 1500) {
    low_batt_notify();
    batt_time = 0;
    batt_push_time++;
  }
  if (batt_push_time == 120) {
    publishMqttMessage(MQTT_TOPIC, MQTT_MSG_3);
    batt_push_time = 0;
  }
  if (STATUS_FLAG == true) {
    STATUS_FLAG=false;
    String wifi_info = get_connected_wifi_info();
    String final_msg = String(CURRENT_FIRMWARE_VERSION) + " | " + wifi_info;
    Serial.print("msg : ");
    Serial.println(final_msg);
    publishMqttMessage(MQTT_TOPIC, final_msg.c_str());
  }
  if (SLEEP_FLAG == true) {
    SLEEP_FLAG=false;
    go_to_sleep();
  }
  batt_time++;
  delay(10);
}