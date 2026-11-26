# Heart Beacon v2 firmware: ESP-IDF v5.3.3, target esp32c3.
#   . C:\esp\esp-idf\export.ps1 ; idf.py -C fw set-target esp32c3 ; idf.py -C fw build
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "2.0.0-dev")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(heart_beacon)

# Size gate: the app slot is 0x1F0000 (2,031,616 B, partitions.csv); fail at 1.9 MB so an OTA image always
# leaves room to grow. Runs after every build of the app image.
add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DBIN=${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin -DMAX=1900000
            -P ${CMAKE_CURRENT_SOURCE_DIR}/size_gate.cmake
    VERBATIM)
