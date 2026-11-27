# cmake -DOUT=<header> -P build_time.cmake
# Writes the build time into a header; only touches the file when it changes,
# so an unchanged build within the same minute doesn't relink.
string(TIMESTAMP now "%Y-%m-%d %H:%M")
set(text "/* generated on every build */\n#define FW_BUILD_TIME \"${now}\"\n")
if (EXISTS ${OUT})
    file(READ ${OUT} old)
endif()
if (NOT "${old}" STREQUAL "${text}")
    file(WRITE ${OUT} "${text}")
endif()
