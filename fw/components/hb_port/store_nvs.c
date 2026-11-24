// NVS namespace "hb": provisioning (console.c), claimed flag, the accepted config's bytes.
#include "hb_port.h"

#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "store";

esp_err_t hb_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // ponytail: wipes whatever v1 left (its WiFi driver data); M8 checks whether that matters
        ESP_LOGW(TAG, "nvs: %s, erasing", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase");
        err = nvs_flash_init();
    }
    return err;
}

uint8_t hb_store_get_u8(const char *key, uint8_t def)
{
    nvs_handle_t h;
    uint8_t v = def;
    if (nvs_open("hb", NVS_READONLY, &h) == ESP_OK) {  // fails until something is written: first boot
        if (nvs_get_u8(h, key, &v) != ESP_OK) {
            v = def;
        }
        nvs_close(h);
    }
    return v;
}

void hb_store_get_str(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    size_t len = cap;
    out[0] = 0;
    if (nvs_open("hb", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, key, out, &len) != ESP_OK) {
            out[0] = 0;
        }
        nvs_close(h);
    }
}

size_t hb_store_get_blob(const char *key, void *out, size_t cap)
{
    nvs_handle_t h;
    size_t len = cap;
    if (nvs_open("hb", NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    if (nvs_get_blob(h, key, out, &len) != ESP_OK) {
        len = 0;
    }
    nvs_close(h);
    return len;
}

// One write, committed: kind 'u' u8, 's' string ("" erases), 'b' blob, 'e' erase all.
static esp_err_t put(char kind, const char *key, const void *v, size_t len)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open("hb", NVS_READWRITE, &h), TAG, "open");
    esp_err_t err;
    switch (kind) {
    case 'u':
        err = nvs_set_u8(h, key, *(const uint8_t *)v);
        break;
    case 's':
        err = ((const char *)v)[0] ? nvs_set_str(h, key, v) : nvs_erase_key(h, key);
        err = err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
        break;
    case 'b':
        err = nvs_set_blob(h, key, v, len);
        break;
    default:
        err = nvs_erase_all(h);
        break;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t hb_store_set_u8(const char *key, uint8_t v)
{
    return put('u', key, &v, 1);
}

esp_err_t hb_store_set_str(const char *key, const char *v)
{
    return put('s', key, v, 0);
}

esp_err_t hb_store_set_blob(const char *key, const void *v, size_t len)
{
    return put('b', key, v, len);
}

esp_err_t hb_store_erase_all(void)
{
    return put('e', NULL, NULL, 0);
}
