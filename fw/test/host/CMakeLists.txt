# Host (IDF-free) tests for fw/components/hb_core, and hb_sim (fw/sim). Rig modelled on AI-Notes-recorder/firmware/test/host.
#   cmake -S fw/test/host -B fw/test/host/build -G Ninja && cmake --build fw/test/host/build
#   ctest --test-dir fw/test/host/build --output-on-failure
# Vendored, unmodified, copied from ESP-IDF v5.3.3: vendor/unity (Unity 2.6.0, MIT),
# vendor/cjson (cJSON 1.7.18, MIT) - the same cJSON the firmware links through the IDF `json` component.
# mbedTLS (hb_config's signature check) is compiled from the installed ESP-IDF's copy, the library the firmware
# links; -DHB_MBEDTLS_DIR=<esp-idf>/components/mbedtls/mbedtls if it is somewhere else.
cmake_minimum_required(VERSION 3.16)
project(hb_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()

set(HB_CORE "${CMAKE_CURRENT_SOURCE_DIR}/../../components/hb_core")
set(HB_VECTORS "${CMAKE_CURRENT_SOURCE_DIR}/../../../protocol/vectors.json")
set(HB_CERTS "${CMAKE_CURRENT_SOURCE_DIR}/../../main/certs")

file(TO_CMAKE_PATH "$ENV{IDF_PATH}" idf_env)
find_path(HB_MBEDTLS_DIR include/mbedtls/pk.h NO_DEFAULT_PATH
          PATHS "${idf_env}/components/mbedtls/mbedtls" "C:/esp/esp-idf/components/mbedtls/mbedtls"
                "/mnt/c/esp/esp-idf/components/mbedtls/mbedtls" "/c/esp/esp-idf/components/mbedtls/mbedtls")
if(NOT HB_MBEDTLS_DIR)
    message(FATAL_ERROR "mbedTLS source not found: install ESP-IDF v5.3.3 or pass -DHB_MBEDTLS_DIR=<dir>")
endif()
file(GLOB mbedtls_src "${HB_MBEDTLS_DIR}/library/*.c")
add_library(mbedcrypto_host STATIC ${mbedtls_src})
target_include_directories(mbedcrypto_host PUBLIC "${HB_MBEDTLS_DIR}/include" PRIVATE "${HB_MBEDTLS_DIR}/library")
target_compile_definitions(mbedcrypto_host PUBLIC
                           "MBEDTLS_CONFIG_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_host_config.h\"")
if(WIN32)
    target_link_libraries(mbedcrypto_host PUBLIC ws2_32)  # x509_crt.c: inet_pton for IP SANs
endif()

add_library(unity STATIC vendor/unity/unity.c)
target_include_directories(unity PUBLIC vendor/unity)

add_library(cjson STATIC vendor/cjson/cJSON.c)
target_include_directories(cjson PUBLIC vendor/cjson)

add_library(hb_core STATIC "${HB_CORE}/hb_msg.c" "${HB_CORE}/hb_frame.c" "${HB_CORE}/hb_button.c"
            "${HB_CORE}/hb_fx.c" "${HB_CORE}/hb_batt.c" "${HB_CORE}/hb_policy.c" "${HB_CORE}/hb_core.c"
            "${HB_CORE}/hb_config.c")
target_include_directories(hb_core PUBLIC "${HB_CORE}")
target_link_libraries(hb_core PUBLIC cjson mbedcrypto_host)
target_compile_options(hb_core PRIVATE -Wall -Wextra -Werror)

add_executable(test_vectors test_vectors.c)
target_link_libraries(test_vectors PRIVATE hb_core unity)
target_compile_options(test_vectors PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(test_vectors PRIVATE "HB_VECTORS_PATH=\"${HB_VECTORS}\"" "HB_CERTS_PATH=\"${HB_CERTS}\"")
add_test(NAME test_vectors COMMAND test_vectors)

add_executable(test_core test_core.c)
target_link_libraries(test_core PRIVATE hb_core unity)
target_compile_options(test_core PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_core COMMAND test_core)
