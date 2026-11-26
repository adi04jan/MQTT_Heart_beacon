idf_component_register(SRCS "led_rmt.c" "adc.c" "sleep.c" "store_nvs.c" "wifi.c" "mqtt.c" "console.c" "ble.c"
                       INCLUDE_DIRS "."
                       REQUIRES hb_core esp_driver_gpio esp_driver_rmt esp_adc esp_pm nvs_flash esp_wifi esp_netif
                                esp_event mqtt bt esp_timer esp_driver_usb_serial_jtag espcoredump)
