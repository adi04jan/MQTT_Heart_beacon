# certs/: the CA and config key the firmware pins (test-only files are committed; see fw/sim/README.md).
idf_component_register(SRCS "main.c"
                       REQUIRES hb_core hb_port app_update esp_app_format esp_driver_gpio esp_timer esp_hw_support
                       EMBED_TXTFILES "certs/ca.crt" "certs/cfg-sign.pub")
