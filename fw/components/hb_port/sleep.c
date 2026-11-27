// Wake cause, automatic light sleep, deep sleep. The button's light-sleep wake lives in main.c with its ISR.
#include "hb_port.h"

#include "driver/gpio.h"
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
#if CONFIG_HB_HW_B || CONFIG_HB_HW_C
    // GPIO0-5 keep their pad setting through deep sleep, and a weak pull-up (~45k) would leak 25-75 uA here:
    // GPIO5 is tied to GND, GPIO1 sees the LIS3DH INT1 (idle low), GPIO3 the USB-sense divider. Float them and
    // hold that. A future LIS3DH wake on GPIO1 must gpio_hold_dis() it first.
    gpio_config_t q = {.pin_bit_mask = BIT64(1) | BIT64(3) | BIT64(5), .mode = GPIO_MODE_DISABLE};
    gpio_config(&q);
    gpio_hold_en(GPIO_NUM_1);
    gpio_hold_en(GPIO_NUM_3);
    gpio_hold_en(GPIO_NUM_5);
#endif
    // IDF enables the pin's pull-up for the low-level wake (ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS)
    esp_deep_sleep_enable_gpio_wakeup(BIT64(CONFIG_HB_BTN_GPIO), ESP_GPIO_WAKEUP_GPIO_LOW);
    if (secs) {
        esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000u);
    }
    esp_deep_sleep_start();
}
