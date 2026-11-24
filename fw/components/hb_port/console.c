// Temporary M3 provisioning over the USB console (plan M3; M4 replaces it with claim + wifi.set over BLE).
// Values are stored in NVS and used from the next boot. Secrets are never printed back.
//   prov                     show what is set
//   prov <key> <value...>    key: did user pass host ssid psk; the value is the rest of the line, empty clears
//   prov erase               forget everything in namespace "hb" (provisioning, claimed, config)
//   reboot
// Setting `pass` also marks the beacon claimed (M3 has no claim flow; M4 adds it).
#include "hb_port.h"

#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const struct {
    const char *key;
    size_t max;    // bytes
    bool secret;
} k_keys[] = {{"did", 9, false}, {"user", 32, false}, {"pass", 64, true},
              {"host", 64, false}, {"ssid", 32, false}, {"psk", 63, true}};

static bool did_ok(const char *v)
{
    if (!v[0]) {
        return true;  // empty: derive from the MAC
    }
    if (strlen(v) != 9 || strncmp(v, "HB-", 3) != 0) {
        return false;
    }
    return strspn(v + 3, "0123456789ABCDEF") == 6;
}

static void handle(char *line)
{
    if (!strcmp(line, "reboot")) {
        esp_restart();  // operator's request; the beacon itself never reboots to recover
    }
    if (!strcmp(line, "prov")) {
        for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++) {
            char v[72];
            hb_store_get_str(k_keys[i].key, v, sizeof v);
            printf("%-4s = %s\n", k_keys[i].key, k_keys[i].secret ? (v[0] ? "(set)" : "(unset)") : v);
        }
        printf("claimed = %u\n", hb_store_get_u8("claimed", 0));
        return;
    }
    if (!strcmp(line, "prov erase")) {
        puts(hb_store_erase_all() == ESP_OK ? "erased, reboot to apply" : "erase failed");
        return;
    }
    if (!strncmp(line, "prov ", 5)) {
        char *key = line + 5, *val = strchr(key, ' ');
        if (val) {
            *val++ = 0;
        } else {
            val = key + strlen(key);
        }
        for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++) {
            if (strcmp(key, k_keys[i].key) != 0) {
                continue;
            }
            if (strlen(val) > k_keys[i].max || (!strcmp(key, "did") && !did_ok(val))) {
                printf("bad value for %s (max %u bytes%s)\n", key, (unsigned)k_keys[i].max,
                       strcmp(key, "did") ? "" : ", HB- + 6 uppercase hex");
                return;
            }
            esp_err_t err = hb_store_set_str(key, val);
            if (err == ESP_OK && !strcmp(key, "ssid")) {
                hb_wifi_forget();
            }
            if (err == ESP_OK && !strcmp(key, "pass")) {
                err = hb_store_set_u8("claimed", val[0] != 0);
            }
            puts(err == ESP_OK ? "ok, reboot to apply" : "store failed");
            return;
        }
    }
    puts("commands: prov | prov <did|user|pass|host|ssid|psk> <value> | prov erase | reboot");
}

static void task(void *arg)
{
    (void)arg;
    char line[160];
    size_t n = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(10000)) != 1) {
            continue;  // bounded wait; nothing typed
        }
        if (ch == '\r' || ch == '\n') {
            line[n] = 0;
            if (n) {
                handle(line);
            }
            n = 0;
        } else if (n < sizeof line - 1) {
            line[n++] = (char)ch;
        }
    }
}

void hb_console_start(void)
{
    // Reads go through the driver; logs keep the default console path, which never blocks without a host.
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        xTaskCreate(task, "console", 3072, NULL, 1, NULL);
    }
}
