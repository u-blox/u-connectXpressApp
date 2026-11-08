cmake_minimum_required(VERSION 3.20)
project(u-connectXpressApp-stm32 C ASM)

# Cross build of the command layer for STM32 boards. Configure with
#   -DCMAKE_TOOLCHAIN_FILE=<repo>/u-connectClient/cmake/arm-none-eabi-gcc.cmake -DSTM32_BOARD=nucleo_h753zi
# (ucxapp.py --stm32-freertos does this).

set(UCX_MODULE "MERGED" CACHE STRING "Generated API variant in u-connectClient/ucx_api/generated (MERGED: NORA-W36X + NORA-B26X)")
if(UCX_MODULE MATCHES "^NORA-(W36X|B26X)$")
  # The app uses both modules' commands (Channel Sounding, power), so the single-module APIs no longer build it.
  message(STATUS "UCX_MODULE ${UCX_MODULE} -> MERGED")
  set(UCX_MODULE "MERGED" CACHE STRING "Generated API variant in u-connectClient/ucx_api/generated (MERGED: NORA-W36X + NORA-B26X)" FORCE)
endif()
include(../../u-connectClient/u-connectClient.cmake)

if(NOT DEFINED STM32_BOARD)
    set(STM32_BOARD "nucleo_h753zi" CACHE STRING "STM32 board")
endif()

set(EXAMPLE_COMMON_SRC
    ${U_CONNECT_CLIENT_UCX_API_SRC}
    ${U_CONNECT_CLIENT_AT_API_SRC}
)

include(../../u-connectClient/cmake/stm32.cmake)
# The library's examples log debug lines; in the app shell they interleave with command output.
list(REMOVE_ITEM STM32_COMPILE_DEFINITIONS U_CX_LOG_DEBUG=1)

set(APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(UCXAPP_STM32_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ucxapp_main_freertos.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ucxapp_platform_freertos.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/ucxapp_flashfs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ucxapp_sdcard.c
)
# Full boards can use a FAT SD card on SPI (FatFs from the STM32Cube middleware).
set(FATFS_DIR ${STM32_HAL_PATH}/Middlewares/Third_Party/FatFs/src)
if(STM32_BOARD MATCHES "^(nucleo_h753zi|nucleo_f767zi|nucleo_f439zi|f407_disc|nucleo_f401re|nucleo_f446re)$")
    list(APPEND UCXAPP_STM32_SOURCES ${FATFS_DIR}/ff.c ${FATFS_DIR}/option/unicode.c)
    set_source_files_properties(${FATFS_DIR}/ff.c ${FATFS_DIR}/option/unicode.c PROPERTIES COMPILE_OPTIONS "-w")
endif()
# Boards with spare erase sectors above the firmware keep their files in littlefs there.
set(LITTLEFS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third-party/littlefs)
if(STM32_BOARD MATCHES "^(nucleo_h753zi|nucleo_f767zi|nucleo_f439zi|f407_disc)$")
    string(TOLOWER "${STM32_FAMILY_SHORT}" _fam)
    list(APPEND UCXAPP_STM32_SOURCES
        ${LITTLEFS_DIR}/lfs.c
        ${LITTLEFS_DIR}/lfs_util.c
        ${STM32_HAL_PATH}/Drivers/STM32${STM32_FAMILY_SHORT}xx_HAL_Driver/Src/stm32${_fam}xx_hal_flash.c
        ${STM32_HAL_PATH}/Drivers/STM32${STM32_FAMILY_SHORT}xx_HAL_Driver/Src/stm32${_fam}xx_hal_flash_ex.c
    )
    set_source_files_properties(
        ${LITTLEFS_DIR}/lfs.c
        ${STM32_HAL_PATH}/Drivers/STM32${STM32_FAMILY_SHORT}xx_HAL_Driver/Src/stm32${_fam}xx_hal_flash_ex.c
        PROPERTIES COMPILE_OPTIONS "-w")
endif()
# The NUCLEO-F103RB and NUCLEO-G071RB (128 KB flash) get the lite command set: Wi-Fi and Bluetooth/GATT, no net, MQTT or cert.
set(UCXAPP_LITE OFF)
if(STM32_BOARD STREQUAL "nucleo_f103rb" OR STM32_BOARD STREQUAL "nucleo_g071rb")
    set(UCXAPP_LITE ON)
else()
    list(APPEND UCXAPP_STM32_SOURCES
        ${APP_SRC}/ucxapp_cmds_mqtt.c
        ${APP_SRC}/ucxapp_cmds_mdns.c
        ${APP_SRC}/ucxapp_cmds_thread.c
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
# Boards with a CAN controller get can.* on it (bxCAN CAN1, FDCAN1 on the H753); the F401 has none.
if(STM32_BOARD MATCHES "^(nucleo_h753zi|nucleo_f767zi|nucleo_f439zi|f407_disc|nucleo_f446re)$")
    list(APPEND UCXAPP_STM32_SOURCES ${APP_SRC}/ucxapp_cmds_can.c ${CMAKE_CURRENT_SOURCE_DIR}/ucxapp_can_stm32.c)
    set(UCXAPP_CAN_HW ON)
endif()
# Boards with a hardware TRNG and RAM to spare serve the web UI over HTTPS only (mbedTLS, keys from the RNG
# peripheral; see docs/SECURITY.md). The F439/F407 (128-192 KB RAM) cannot hold the page buffers and a TLS
# session next to the app, the F401 has no RNG, the F446 and the lite boards lack the RAM.
if(STM32_BOARD MATCHES "^(nucleo_h753zi|nucleo_f767zi)$")
    set(UCXAPP_WEB ON)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../mbedtls_embedded.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../embed.cmake)
    set(_webgen ${CMAKE_CURRENT_BINARY_DIR}/gen)
    file(MAKE_DIRECTORY ${_webgen})
    ucxapp_embed(${CMAKE_CURRENT_SOURCE_DIR}/../web/index.html ${_webgen}/ucxapp_webpage.h kPageBytes GZIP)
    ucxapp_embed(${CMAKE_CURRENT_SOURCE_DIR}/../web/icon.svg ${_webgen}/ucxapp_favicon.h kFavicon)
    ucxapp_embed(${CMAKE_CURRENT_SOURCE_DIR}/../../COMMANDS.md ${_webgen}/ucxapp_manual_text.h kManualBytes NUL)
    list(APPEND UCXAPP_STM32_SOURCES
        ${APP_SRC}/ucxapp_cmds_web.c
        ${APP_SRC}/ucxapp_ui.c
        ${APP_SRC}/u_webserver.c
        ${APP_SRC}/ucxapp_tls.c
        ${APP_SRC}/ucxapp_cmds_manual.c
        ${APP_SRC}/ucxapp_cmds_conn.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ucxapp_thread_freertos.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ucxapp_rng_stm32.c
        ${UCXAPP_MBEDTLS_SOURCES}
    )
endif()

string(TOLOWER "${STM32_BOARD}" _board)
set(UCXAPP_STM32_NAME "ucxapp_${_board}")
add_stm32_example(NAME ${UCXAPP_STM32_NAME} SOURCES ${UCXAPP_STM32_SOURCES})
# Full boards build for size (UCXAPP_OPT, default -Os: the F401/F446 flash is nearly full at -O2 and the app is
# UART bound); lite boards add LTO. The shared default is -Og.
set(UCXAPP_OPT "-Os" CACHE STRING "Optimisation level of the full STM32 builds")
if(NOT UCXAPP_LITE)
    target_compile_options(${UCXAPP_STM32_NAME}.elf PRIVATE ${UCXAPP_OPT} -ffunction-sections -fdata-sections)
    target_link_options(${UCXAPP_STM32_NAME}.elf PRIVATE -Wl,--gc-sections)
else()
    # 128 KB flash: LTO keeps the lite command set inside ROM.
    target_compile_options(${UCXAPP_STM32_NAME}.elf PRIVATE -flto)
    target_link_options(${UCXAPP_STM32_NAME}.elf PRIVATE -flto=auto -Os)
    # The port's inline asm calls vTaskSwitchContext, which LTO cannot see.
    set(_rtos ${STM32_HAL_PATH}/Middlewares/Third_Party/FreeRTOS/Source)
    set_property(SOURCE ${_rtos}/tasks.c ${_rtos}/portable/GCC/ARM_CM0/port.c ${_rtos}/portable/GCC/ARM_CM3/port.c
        APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
endif()

target_include_directories(${UCXAPP_STM32_NAME}.elf PRIVATE ${APP_SRC} ${LITTLEFS_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${FATFS_DIR})
target_compile_definitions(${UCXAPP_STM32_NAME}.elf PRIVATE LFS_THREADSAFE LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)
if(UCXAPP_CAN_HW)
    target_compile_definitions(${UCXAPP_STM32_NAME}.elf PRIVATE UCXAPP_CAN_HW)
endif()
if(UCXAPP_WEB)
    target_include_directories(${UCXAPP_STM32_NAME}.elf PRIVATE ${_webgen} ${UCXAPP_MBEDTLS_INCLUDES})
    target_compile_definitions(${UCXAPP_STM32_NAME}.elf PRIVATE UCXAPP_WEB UCXAPP_WEB_HTTPS_ONLY ${UCXAPP_MBEDTLS_DEFINES}
        "kManual=((const char *)kManualBytes)" API_BUF=32768 DL_CHUNK=4096)
endif()
# The command layer needs more heap than the examples (32 KB default); the F439ZI and F407VG have little RAM.
if(UCXAPP_LITE)
    if(STM32_BOARD STREQUAL "nucleo_g071rb")
        set(UCXAPP_HEAP_SIZE 16384)
    else()
        set(UCXAPP_HEAP_SIZE 6144)
    endif()
    target_compile_definitions(${UCXAPP_STM32_NAME}.elf PRIVATE UCXAPP_LITE
        UCXAPP_STACK_WORDS=768 UCXAPP_RX_BUFFER_SIZE=1024 UCXAPP_URC_BUFFER_SIZE=512
        LISTED_MAX=8 LISTED_NAME_LEN=32 SCAN_MAX=8 U_PORT_UART_RX_BUFFER_SIZE=512
        BT_SCAN_MAX_DEVICES=8 MAX_SVC=8 MAX_CHR=24 MAX_SRV_CHARS=8 WPS_QUEUE=2)
elseif(STM32_BOARD STREQUAL "nucleo_f401re")
    set(UCXAPP_HEAP_SIZE 32768)
elseif(STM32_BOARD STREQUAL "nucleo_f439zi" OR STM32_BOARD STREQUAL "f407_disc" OR STM32_BOARD STREQUAL "nucleo_f446re")
    set(UCXAPP_HEAP_SIZE 49152)
else()
    set(UCXAPP_HEAP_SIZE 163840)
endif()
# The F767's 368 KB main RAM also holds the web UI and the newlib heap that mbedTLS allocates from (one TLS
# session is about 35 KB); the FreeRTOS heap only holds task stacks (app 24 KB, two web threads 24 KB) and
# RTOS objects.
if(UCXAPP_WEB AND STM32_BOARD STREQUAL "nucleo_f767zi")
    set(UCXAPP_HEAP_SIZE 81920)
endif()
target_compile_definitions(${UCXAPP_STM32_NAME}.elf PRIVATE configTOTAL_HEAP_SIZE=${UCXAPP_HEAP_SIZE}
    U_CX_AT_CONFIG_FILE="ucxapp_logcfg.h")
# 96-128 KB RAM boards: shorter URLs and request bodies, smaller receive buffers (same as the Zephyr build).
if(STM32_BOARD MATCHES "^(nucleo_f401re|nucleo_f446re|f407_disc)$")
    target_compile_definitions(${UCXAPP_STM32_NAME}.elf PRIVATE HTTP_MAX_URL=512 REQ_BODY_SIZE=2048
        UCXAPP_RX_BUFFER_SIZE=4096 UCXAPP_CONSOLE_RX_SIZE=1024 LISTED_MAX=32)
endif()
