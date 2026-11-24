# hb_sim: the hb_core library target of fw/test/host (the same objects the host tests link) behind a TCP
# socket, plus its end-to-end test. Built from fw/test/host/CMakeLists.txt.
add_executable(hb_sim hb_sim.c)
target_link_libraries(hb_sim PRIVATE hb_core)
if(WIN32)
    target_link_libraries(hb_sim PRIVATE ws2_32)
endif()
target_compile_options(hb_sim PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(hb_sim PRIVATE "HB_SIM_KEY=\"${CMAKE_CURRENT_SOURCE_DIR}/../main/certs/cfg-sign.pub\"")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_sim COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/test_sim.py" $<TARGET_FILE:hb_sim>
                               "${HB_VECTORS}")
