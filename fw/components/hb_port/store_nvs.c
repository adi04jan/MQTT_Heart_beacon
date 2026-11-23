// NVS namespace "hb". M1 only reads; claim (M4) and config (M3) add writers.
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
