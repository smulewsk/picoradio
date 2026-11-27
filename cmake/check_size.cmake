# cmake -DBIN=<file> -DLIMIT=<bytes> -P check_size.cmake
# Fails the build when the firmware no longer fits one OTA slot (pt.json).
file(SIZE ${BIN} size)
math(EXPR kb "${size} / 1024")
math(EXPR limit_kb "${LIMIT} / 1024")
if (size GREATER LIMIT)
    message(FATAL_ERROR "Firmware is ${kb} KB, an OTA slot holds ${limit_kb} KB: grow the slots in pt.json")
endif()
message(STATUS "Firmware ${kb} KB of ${limit_kb} KB OTA slot")
