# cmake -DBIN=<app.bin> -DMAX=<bytes> -P size_gate.cmake : fails when the app image is larger than MAX.
file(SIZE "${BIN}" size)
math(EXPR pct "${size} * 100 / ${MAX}")
if(size GREATER MAX)
    message(FATAL_ERROR "size gate: ${BIN} is ${size} bytes, over the ${MAX} byte limit (${pct} %)")
endif()
message(STATUS "size gate: ${size} of ${MAX} bytes (${pct} %)")
