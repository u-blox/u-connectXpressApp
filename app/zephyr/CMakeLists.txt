cmake_minimum_required(VERSION 3.20.0)

# Zephyr build of the command layer. Build with
#   west build -b nucleo_h753zi app/zephyr
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
    ${APP_SRC}/ucxapp_cmds_mqtt.c
    ${APP_SRC}/ucxapp_cmds_net.c
    ${APP_SRC}/ucxapp_cmds_time.c
    ${APP_SRC}/ucxapp_cmds_cert.c
    ${APP_SRC}/ucxapp_cmds_examples.c
    ${APP_SRC}/ucxapp_cmds_ymodem.c
    ${APP_SRC}/ucxapp_ymodem.c
)

target_include_directories(app PRIVATE ${APP_SRC} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(app PRIVATE -Wall -Wextra)

# The library includes ucxapp_logcfg.h too (runtime AT trace switch).
zephyr_include_directories(${APP_SRC})
zephyr_compile_definitions(U_CX_AT_CONFIG_FILE="ucxapp_logcfg.h")
