#define MQTT_USER "USER_NAME"        // Change this as needed
#define MQTT_PASS "P@SSW0RD"  // Change this as needed
#define MQTT_TOPIC "/topic/subtopic/"  // Change this as needed
#define MQTT_MSG_1 "Miss you"  // Change this as needed
#define MQTT_MSG_2 "Online"  // Change this as needed
#define MQTT_MSG_3 "BATT_STATUS"  // Change this as needed
#define MQTT_MSG_4 "DEEP SLEEP"  // Change this as needed
#define MQTT_GO_SLEEP "GO_DEEP_SLEEP"  // Change this as needed
#define MQTT_MSG_SORRY_1 "Sorry"  // Change this as needed
#define MQTT_MSG_SORRY_2 "sorry"  // Change this as needed
#define MQTT_MSG_ANNOY_1 "Annoy"  // Change this as needed
#define MQTT_MSG_ANNOY_2 "annoy"  // Change this as needed

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