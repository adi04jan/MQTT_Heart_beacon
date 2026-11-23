idf_component_register(SRCS "led_rmt.c" "adc.c" "sleep.c" "store_nvs.c"
                       INCLUDE_DIRS "."
                       REQUIRES hb_core esp_driver_gpio esp_driver_rmt esp_adc esp_pm nvs_flash)
