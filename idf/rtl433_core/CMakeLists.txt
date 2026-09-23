# rtl_433 (merbanan/rtl_433, see UPSTREAM) built as an ESP-IDF component for the ESP32-P4.
# Radio input comes from esp_rtl_sdr (USB host) through the "esp" backend in src/sdr.c.
file(GLOB R433_SRCS "src/*.c" "src/devices/*.c")
# No terminal here: port/term_ctl_stub.c replaces the TTY colour code
list(FILTER R433_SRCS EXCLUDE REGEX ".*/src/term_ctl\\.c$")
list(APPEND R433_SRCS "port/rtl433_port.c" "port/term_ctl_stub.c" "port/rtl433_bands.c")

idf_component_register(
    SRCS ${R433_SRCS}
    INCLUDE_DIRS "include" "port"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_rtl_sdr pthread lwip esp_timer freertos log vfs
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    THREADS ESP_RTL_SDR _GNU_SOURCE
    MG_ENABLE_FILESYSTEM=1 MG_ENABLE_HTTP_CGI=0 MG_ENABLE_DIRECTORY_LISTING=0 MG_ENABLE_BROADCAST=1
    # mg_broadcast's message is one sdr_event_t (28 bytes); the default 8 KB lives on the stack of every caller
    MG_CTL_MSG_MESSAGE_SIZE=64)
# Every rtl_433 source sees the port shims (exit() -> task exit, missing POSIX bits)
# ESPHome builds for size (-Os); the demodulators are tight DSP loops that gain far more from -O2
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/port/rtl433_port.h"
    -Wno-unused-variable -Wno-unused-function -Wno-sign-compare -Wno-format
    -Wno-missing-field-initializers -Wno-type-limits -Wno-unused-but-set-variable
    -Wno-maybe-uninitialized -Wno-pointer-sign -Wno-char-subscripts -Wno-implicit-fallthrough
    -Wno-error -Wno-incompatible-pointer-types)
if(CONFIG_LWIP_IPV6)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MG_ENABLE_IPV6=1)
endif()
set_source_files_properties("src/rtl_433.c" PROPERTIES COMPILE_DEFINITIONS "main=rtl_433_main")
