# ESP-IDF component. Host builds compile these sources directly (fw/test/host/CMakeLists.txt).
idf_component_register(SRCS "hb_msg.c" "hb_frame.c"
                       INCLUDE_DIRS "."
                       REQUIRES json)
