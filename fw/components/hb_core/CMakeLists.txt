# ESP-IDF component. Host builds compile these sources directly (fw/test/host/CMakeLists.txt).
idf_component_register(SRCS "hb_msg.c" "hb_frame.c" "hb_button.c" "hb_fx.c" "hb_batt.c" "hb_policy.c" "hb_core.c"
                            "hb_config.c" "hb_link.c" "hb_wire.c"
                       INCLUDE_DIRS "."
                       REQUIRES json mbedtls)
