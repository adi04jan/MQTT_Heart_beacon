#define MQTT_USER "test"        // Change this as needed
#define MQTT_PASS "12345678"  // Change this as needed

const char *mqtt_server = "192.168.29.250";
const int mqtt_port = 1883;

static const char *url = "http://test.com/mystream/OTA/heart/firmware.bin";  //state url of your firmware image
static const char *base_url = "http://test.com/mystream/OTA/heart/";
static const char *ssids[] = {
  "wifi_network",
};

static const char *passwords[] = {
  "12345678",
};