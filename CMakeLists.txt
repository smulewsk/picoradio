# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Pico SDK: PICO_SDK_PATH (-D or environment), else the VS Code extension's
# install, else PICO_SDK_FETCH_FROM_GIT=on downloads it (pico_sdk_import.cmake)
if (NOT PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH} AND EXISTS "$ENV{HOME}/.pico-sdk/sdk/2.3.0")
    set(PICO_SDK_PATH "$ENV{HOME}/.pico-sdk/sdk/2.3.0")
endif()
include(pico_sdk_import.cmake)

# Firmware version: bump it here. Shown on the web page, the info screen,
# the serial log and by `picotool info`.
project(pico_radio VERSION 1.0.1 LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/FreeRTOS-Kernel CACHE PATH "FreeRTOS kernel")
set(MINIMP3_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/minimp3)
set(HELIX_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/arduino-libhelix/src)
if (NOT EXISTS ${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake
    OR NOT EXISTS ${MINIMP3_PATH}/minimp3.h
    OR NOT EXISTS ${HELIX_PATH}/libhelix-aac/aacdec.h)
    message(FATAL_ERROR "Libraries missing - run ./setup.sh first")
endif()
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)

add_executable(pico_radio
    src/main.c
    src/radio.c
    src/audio_i2s.c
    src/decoder.c
    src/net_stream.c
    src/st7789.c
    src/gfx.c
    src/encoder.c
    src/ui.c
    src/minimp3_impl.c
    src/bt_sink.c
    src/mic.c
    src/tls_ws.c
    src/ai_gemini.c
    src/aac_dec.c
    src/mem.c
    src/tls_conn.c
    src/web.c
    src/stations.c
    src/ota.c
    src/version.c
    src/settings.c
    src/cfg.c
    src/rb.c
    src/ap.c
)

# version.c gets the version and the build time (regenerated on every build)
set_source_files_properties(src/version.c PROPERTIES
    COMPILE_DEFINITIONS "FW_VERSION=\"${PROJECT_VERSION}\"")
add_custom_target(pico_radio_build_time
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/build_time.h
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/build_time.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/build_time.h
    VERBATIM)
add_dependencies(pico_radio pico_radio_build_time)
pico_set_program_version(pico_radio "${PROJECT_VERSION}")

# Web remote page: web/index.html -> generated/web_page.h (re-run when edited)
set(WEB_PAGE_SRC ${CMAKE_CURRENT_LIST_DIR}/web/index.html)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WEB_PAGE_SRC})
# stored gzip-compressed (31 KB -> ~10 KB) and sent with Content-Encoding: gzip
set(WEB_PAGE_GZ ${CMAKE_CURRENT_BINARY_DIR}/generated/index.html.gz)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(ARCHIVE_CREATE OUTPUT ${WEB_PAGE_GZ} PATHS ${WEB_PAGE_SRC}
     FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL 9)
file(READ ${WEB_PAGE_GZ} WEB_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," WEB_HEX "${WEB_HEX}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/web_page.h
    "/* generated from web/index.html (gzip) */\nstatic const unsigned char WEB_PAGE_GZ[] = { ${WEB_HEX} };\n")

# Helix AAC decoder (AAC-LC + HE-AAC/SBR), RealNetworks RPSL/RCSL licence.
# Its portable C code path is selected with ARDUINO.
file(GLOB HELIX_AAC_SOURCES ${HELIX_PATH}/libhelix-aac/*.c)
target_sources(pico_radio PRIVATE ${HELIX_AAC_SOURCES})
set_source_files_properties(${HELIX_AAC_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "ARDUINO=1;USE_DEFAULT_STDLIB=1"
    COMPILE_OPTIONS "-w")
# its 51 KB SBR state is allocated with plain malloc: route it to xmem_alloc
set_property(SOURCE ${HELIX_PATH}/libhelix-aac/sbr.c APPEND PROPERTY
    COMPILE_DEFINITIONS "malloc=xmem_alloc;free=xmem_free")

pico_generate_pio_header(pico_radio ${CMAKE_CURRENT_LIST_DIR}/src/i2s.pio)
pico_generate_pio_header(pico_radio ${CMAKE_CURRENT_LIST_DIR}/src/mic_i2s.pio)
pico_generate_pio_header(pico_radio ${CMAKE_CURRENT_LIST_DIR}/src/tft_spi.pio)

target_include_directories(pico_radio PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${MINIMP3_PATH}
    ${HELIX_PATH}
    ${HELIX_PATH}/libhelix-aac
)

target_compile_options(pico_radio PRIVATE -O2)
# malloc may fail (TLS / AAC try the other heap or report it): don't panic
target_compile_definitions(pico_radio PRIVATE PICO_MALLOC_PANIC=0)

# Optional secrets from the command line instead of config.h
foreach(SECRET WIFI_SSID WIFI_PASSWORD GEMINI_API_KEY)
    if (DEFINED ${SECRET})
        target_compile_definitions(pico_radio PRIVATE ${SECRET}=\"${${SECRET}}\")
    endif()
endforeach()

target_link_libraries(pico_radio
    pico_stdlib
    pico_cyw43_arch_lwip_sys_freertos
    FreeRTOS-Kernel-Heap4
    pico_btstack_cyw43
    pico_btstack_classic
    pico_btstack_sbc_decoder
    pico_mbedtls
    pico_rand
    pico_lwip_mdns
    hardware_pio
    hardware_dma
    hardware_spi
    hardware_clocks
    hardware_pwm
    hardware_flash
    hardware_watchdog
    pico_bootrom
    pico_flash
)

pico_enable_stdio_usb(pico_radio 1)
pico_enable_stdio_uart(pico_radio 0)
pico_add_extra_outputs(pico_radio)

# Partition table for OTA updates (two 1984 KB firmware slots, see pt.json).
# Flash build/pt.uf2 once over USB (BOOTSEL), then build/pico_radio.uf2.
pico_init_picotool()
if (TARGET picotool)
    set(PT_JSON ${CMAKE_CURRENT_LIST_DIR}/pt.json)
    set(PT_UF2 ${CMAKE_CURRENT_BINARY_DIR}/pt.uf2)
    add_custom_command(OUTPUT ${PT_UF2}
        COMMAND picotool partition create --quiet ${PT_JSON} ${PT_UF2}
        DEPENDS ${PT_JSON}
        COMMENT "Partition table -> pt.uf2"
        VERBATIM)
    add_custom_target(partition_table ALL DEPENDS ${PT_UF2})
endif()
add_custom_command(TARGET pico_radio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DBIN=$<TARGET_FILE_DIR:pico_radio>/pico_radio.bin -DLIMIT=2031616
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/check_size.cmake
    VERBATIM)
