# Host (IDF-free) tests for fw/components/hb_core. Rig modelled on AI-Notes-recorder/firmware/test/host.
#   cmake -S fw/test/host -B fw/test/host/build -G Ninja && cmake --build fw/test/host/build
#   ctest --test-dir fw/test/host/build --output-on-failure
# Vendored, unmodified, copied from ESP-IDF v5.3.3: vendor/unity (Unity 2.6.0, MIT),
# vendor/cjson (cJSON 1.7.18, MIT) - the same cJSON the firmware links through the IDF `json` component.
cmake_minimum_required(VERSION 3.16)
project(hb_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()

set(HB_CORE "${CMAKE_CURRENT_SOURCE_DIR}/../../components/hb_core")
set(HB_VECTORS "${CMAKE_CURRENT_SOURCE_DIR}/../../../protocol/vectors.json")

add_library(unity STATIC vendor/unity/unity.c)
target_include_directories(unity PUBLIC vendor/unity)

add_library(cjson STATIC vendor/cjson/cJSON.c)
target_include_directories(cjson PUBLIC vendor/cjson)

add_library(hb_core STATIC "${HB_CORE}/hb_msg.c" "${HB_CORE}/hb_frame.c")
target_include_directories(hb_core PUBLIC "${HB_CORE}")
target_link_libraries(hb_core PUBLIC cjson)
target_compile_options(hb_core PRIVATE -Wall -Wextra -Werror)

add_executable(test_vectors test_vectors.c)
target_link_libraries(test_vectors PRIVATE hb_core unity)
target_compile_options(test_vectors PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(test_vectors PRIVATE "HB_VECTORS_PATH=\"${HB_VECTORS}\"")
add_test(NAME test_vectors COMMAND test_vectors)
