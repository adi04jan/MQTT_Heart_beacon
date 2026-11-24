// MQTT 5 over TLS for hb_link (esp-mqtt, PROTOCOL.md 5): one client per session, made on HB_LINK_MQTT and
// destroyed on HB_LINK_DOWN. hb_link drives every retry (auto reconnect off). Publishes are enqueued, so the
// main loop never blocks on the network; the network timeout bounds every socket wait in the mqtt task, and
// with it stop and destroy.
#include "hb_port.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_cli;
static char s_down[32], s_cfg[32];

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = data;
    hb_ev_t ev = {.type = HB_EV_MQTT};
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:  // subscribe first, so nothing the router sends on `state` is missed
        esp_mqtt_client_subscribe_single(e->client, s_down, 1);
        esp_mqtt_client_subscribe_single(e->client, s_cfg, 1);
        ev.up = true;
        hb_port_post(&ev);
        break;
    case MQTT_EVENT_DISCONNECTED:
        hb_port_post(&ev);
        break;
    case MQTT_EVENT_PUBLISHED:
        hb_port_post_puback(e->msg_id);
        break;
    case MQTT_EVENT_DATA:
        if (e->current_data_offset == 0 && e->data_len == e->total_data_len) {
            hb_port_post_rx(e->topic, e->topic_len, e->data, e->data_len);
        } else {  // over the 2048 B buffer, so over PROTOCOL.md's 1024 B anyway
            ESP_LOGW(TAG, "dropped a %d byte message", e->total_data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "transport: esp-tls 0x%x, tls 0x%x, errno %d", e->error_handle->esp_tls_last_esp_err,
                     e->error_handle->esp_tls_stack_err, e->error_handle->esp_transport_sock_errno);
        } else {
            ESP_LOGW(TAG, "error type %d, connack %d", e->error_handle->error_type,
                     e->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

esp_err_t hb_mqtt_start(const hb_mqtt_cfg_t *m)
{
    char id[16], will_topic[32];
    hb_mqtt_stop();
    snprintf(id, sizeof id, "d-%s", m->did);
    snprintf(will_topic, sizeof will_topic, "hb/v1/d/%s/state", m->did);
    snprintf(s_down, sizeof s_down, "hb/v1/d/%s/down", m->did);
    snprintf(s_cfg, sizeof s_cfg, "hb/v1/d/%s/cfg", m->did);
    const esp_mqtt_client_config_t c = {  // esp-mqtt copies the strings, not the CA
        .broker = {.address = {.hostname = m->host, .port = m->port, .transport = MQTT_TRANSPORT_OVER_SSL},
                   .verification = {.certificate = m->ca}},
        .credentials = {.username = m->user, .client_id = id, .authentication = {.password = m->pass}},
        .session = {.keepalive = 240,
                    .protocol_ver = MQTT_PROTOCOL_V_5,
                    .disable_clean_session = false,  // clean start; the router queues (PROTOCOL.md 5)
                    .last_will = {.topic = will_topic, .msg = m->will, .qos = 1, .retain = 1}},
        .network = {.timeout_ms = 5000, .disable_auto_reconnect = true},
        .buffer = {.size = 2048},
    };
    s_cli = esp_mqtt_client_init(&c);
    if (!s_cli) {
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(s_cli, ESP_EVENT_ANY_ID, on_event, NULL);
    ESP_LOGI(TAG, "connect %s:%u as %s", m->host, m->port, id);
    esp_err_t err = esp_mqtt_client_start(s_cli);
    if (err != ESP_OK) {
        hb_mqtt_stop();
    }
    return err;
}

void hb_mqtt_stop(void)
{
    if (s_cli) {
        esp_mqtt_client_stop(s_cli);
        esp_mqtt_client_destroy(s_cli);
        s_cli = NULL;
    }
}

int hb_mqtt_publish(const char *topic, const char *json, int len, bool retain)
{
    return s_cli ? esp_mqtt_client_enqueue(s_cli, topic, json, len, 1, retain, true) : -1;
}

bool hb_mqtt_flush(uint32_t ms)
{
    for (uint32_t t = 0; s_cli && esp_mqtt_client_get_outbox_size(s_cli) > 0; t += 50) {
        if (t >= ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}
