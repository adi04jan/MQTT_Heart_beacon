// Battery on GPIO3 (ADC1) through a 1:2 divider, eFuse curve-fitting calibration.
#include "hb_port.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"

#define SAMPLES 8
#define DIVIDER 2

static const char *TAG = "adc";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;

esp_err_t hb_adc_init(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(HB_BATT_GPIO, &unit, &s_chan), TAG, "gpio");
    adc_oneshot_unit_init_cfg_t uc = {.unit_id = unit};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&uc, &s_adc), TAG, "unit");
    adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &cc), TAG, "channel");
    adc_cali_curve_fitting_config_t kc = {
        .unit_id = unit, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    return adc_cali_create_scheme_curve_fitting(&kc, &s_cali);
}

uint32_t hb_adc_batt_mv(void)
{
    if (!s_adc || !s_cali) {
        return 0;
    }
    uint32_t sum = 0, n = 0;
    for (int i = 0; i < SAMPLES; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            sum += (uint32_t)mv;
            n++;
        }
    }
    return n ? sum * DIVIDER / n : 0;
}
