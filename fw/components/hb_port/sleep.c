// Wake cause, automatic light sleep, deep sleep. The button's light-sleep wake lives in main.c with its ISR.
#include "hb_port.h"

#include "esp_pm.h"
#include "esp_sleep.h"
#include "sdkconfig.h"

hb_wake_t hb_sleep_wake_cause(void)
{
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER:
        return HB_WAKE_TIMER;
    case ESP_SLEEP_WAKEUP_GPIO:
        return HB_WAKE_BUTTON;
    default:
        return HB_WAKE_POWERON;  // reset, brownout, panic, OTA restart: start clean
    }
}

esp_err_t hb_sleep_auto_light(void)
{
    esp_pm_config_t pm = {.max_freq_mhz = 80, .min_freq_mhz = 40, .light_sleep_enable = true};
    return esp_pm_configure(&pm);
}

void hb_sleep_deep(uint32_t secs)
{
    hb_led_park();
    // IDF enables the pin's pull-up for the low-level wake (ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS)
    esp_deep_sleep_enable_gpio_wakeup(BIT64(CONFIG_HB_BTN_GPIO), ESP_GPIO_WAKEUP_GPIO_LOW);
    if (secs) {
        esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000u);
    }
    esp_deep_sleep_start();
}
