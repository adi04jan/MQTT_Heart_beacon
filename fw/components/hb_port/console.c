// Bench provisioning over the USB console. The product way in is BLE (claim, wifi.set); this stays as the
// fallback when no phone is at hand. Values are stored in NVS and used from the next boot. Secrets are never
// printed back. ssid0/psk0 is the first of the HB_WIFI_NETS network slots.
//   prov                     show what is set
//   prov <key> <value...>    key: did user pass host ssid0 psk0; the value is the rest of the line, empty clears
//   prov erase               forget everything in namespace "hb" (provisioning, claimed, config)
//   reboot
// Setting `pass` also marks the beacon claimed (M3 has no claim flow; M4 adds it).
#include "hb_port.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "driver/usb_serial_jtag.h"
#include "esp_core_dump.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const struct {
    const char *key;
    size_t max;    // bytes
    bool secret;
} k_keys[] = {{"did", 9, false}, {"user", 32, false}, {"pass", 64, true},
              {"host", 64, false}, {"ssid0", 32, false}, {"psk0", 63, true}};

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
    if (!strcmp(line, "dump erase")) {  // after `idf.py coredump-info` has read it
        puts(esp_core_dump_image_erase() == ESP_OK ? "core dump erased" : "no core dump");
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
            if (err == ESP_OK && !strcmp(key, "ssid0")) {
                hb_wifi_forget();
            }
            if (err == ESP_OK && !strcmp(key, "pass")) {
                err = hb_store_set_u8("claimed", val[0] != 0);
            }
            puts(err == ESP_OK ? "ok, reboot to apply" : "store failed");
            return;
        }
    }
    puts("commands: prov | prov <did|user|pass|host|ssid0|psk0> <value> | prov erase | dump erase | reboot");
}

// The default console VFS, polled: never the usb_serial_jtag driver. Its ISR leaves the TX-empty interrupt
// armed when the FIFO is full, and the logs (which bypass the driver) fill it whenever no host reads them:
// an interrupt storm that starved every task (bench 2026-11-26), and on battery there is never a host.
static void task(void *arg)
{
    (void)arg;
    int fd = fileno(stdin);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    char line[160];
    size_t n = 0;
    for (;;) {
        char ch;
        if (!usb_serial_jtag_is_connected()) {  // on battery there is no host: look again in 2 s, not 10 times a second
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (read(fd, &ch, 1) != 1) {
            vTaskDelay(pdMS_TO_TICKS(100));  // nothing typed
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            line[n] = 0;
            if (n) {
                handle(line);
            }
            n = 0;
        } else if (n < sizeof line - 1) {
            line[n++] = ch;
        }
    }
}

void hb_console_start(void)
{
    xTaskCreate(task, "console", 3072, NULL, 1, NULL);
}
