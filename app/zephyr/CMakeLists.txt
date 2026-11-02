cmake_minimum_required(VERSION 3.20.0)

# Zephyr build of the command layer. Build with
#   west build -b <board> app/zephyr      (boards: app/zephyr/boards/*.overlay)
# u-connectClient is added as a Zephyr module straight from the submodule.

get_filename_component(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
set(UCX_MODULE "NORA-W36X" CACHE STRING "Generated API variant in u-connectClient/ucx_api/generated")
list(APPEND ZEPHYR_EXTRA_MODULES ${REPO_ROOT}/u-connectClient)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ucxapp_zephyr LANGUAGES C)

set(APP_SRC ${REPO_ROOT}/app/src)

target_sources(app PRIVATE
    ucxapp_main_zephyr.c
    ucxapp_platform_zephyr.c
    ${APP_SRC}/ucxapp_io.c
    ${APP_SRC}/ucxapp_events.c
    ${APP_SRC}/ucxapp_store.c
    ${APP_SRC}/ucxapp_store_ram.c
    ${APP_SRC}/ucxapp_stubs_embedded.c
    ${APP_SRC}/ucx_obfuscate.c
    ${APP_SRC}/ucxapp_hash.c
    ${APP_SRC}/ucxapp_cmd.c
    ${APP_SRC}/ucxapp_urc.c
    ${APP_SRC}/ucxapp_cmds_device.c
    ${APP_SRC}/ucxapp_cmds_wifi.c
    ${APP_SRC}/ucxapp_cmds_sock.c
    ${APP_SRC}/ucxapp_cmds_sys.c
    ${APP_SRC}/ucxapp_cmds_app.c
    ${APP_SRC}/ucxapp_cmds_bt.c
    ${APP_SRC}/ucxapp_adv.c
    ${APP_SRC}/ucxapp_cmds_gatt.c
)
# 128 KB flash boards get the lite command set (Wi-Fi and Bluetooth/GATT), as in the FreeRTOS build.
if(BOARD MATCHES "^(nucleo_f103rb|nucleo_g071rb)$")
    target_compile_definitions(app PRIVATE UCXAPP_LITE
        UCXAPP_RX_BUFFER_SIZE=1024 UCXAPP_URC_BUFFER_SIZE=512 UCXAPP_CONSOLE_RX_SIZE=256
        LISTED_MAX=8 LISTED_NAME_LEN=32 SCAN_MAX=8
        BT_SCAN_MAX_DEVICES=8 MAX_SVC=8 MAX_CHR=24 MAX_SRV_CHARS=8 WPS_QUEUE=2)
    zephyr_compile_definitions(U_PORT_UART_RX_BUFFER_SIZE=512 U_PORT_UART_TX_BUFFER_SIZE=256)
else()
    target_sources(app PRIVATE
        ${APP_SRC}/ucxapp_cmds_mqtt.c
        ${APP_SRC}/ucxapp_cmds_net.c
        ${APP_SRC}/ucxapp_cmds_wifiloc.c
        ${APP_SRC}/ucxapp_cmds_cfg.c
        ${APP_SRC}/ucxapp_cmds_perf.c
        ${APP_SRC}/ucxapp_cmds_time.c
        ${APP_SRC}/ucxapp_cmds_cert.c
        ${APP_SRC}/ucxapp_cmds_examples.c
        ${APP_SRC}/ucxapp_cmds_usecase.c
        ${APP_SRC}/ucxapp_cmds_ymodem.c
        ${APP_SRC}/ucxapp_ymodem.c
    )
endif()
# 96-128 KB RAM boards: Zephyr's own RAM leaves no room for the full-size buffers next to a heap.
if(BOARD MATCHES "^(nucleo_f401re|nucleo_f446re|stm32f4_disco)$")
    target_compile_definitions(app PRIVATE HTTP_MAX_URL=512 REQ_BODY_SIZE=2048
        UCXAPP_RX_BUFFER_SIZE=4096 UCXAPP_CONSOLE_RX_SIZE=1024 LISTED_MAX=32)
endif()

target_include_directories(app PRIVATE ${APP_SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(app PRIVATE -Wall -Wextra)

# The library includes ucxapp_logcfg.h too (runtime AT trace switch).
zephyr_include_directories(${APP_SRC})
zephyr_compile_definitions(U_CX_AT_CONFIG_FILE="ucxapp_logcfg.h")
