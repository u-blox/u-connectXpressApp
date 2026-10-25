cmake_minimum_required(VERSION 3.20)
project(u-connectXpressApp-stm32 C ASM)

# Cross build of the command layer for STM32 boards. Configure with
#   -DCMAKE_TOOLCHAIN_FILE=<repo>/u-connectClient/cmake/arm-none-eabi-gcc.cmake -DSTM32_BOARD=nucleo_h753zi
# (ucxapp.py --stm32 does this).

set(UCX_MODULE "NORA-W36X" CACHE STRING "Generated API variant in u-connectClient/ucx_api/generated")
include(../../u-connectClient/u-connectClient.cmake)

if(NOT DEFINED STM32_BOARD)
    set(STM32_BOARD "nucleo_h753zi" CACHE STRING "STM32 board")
endif()

set(EXAMPLE_COMMON_SRC
    ${U_CONNECT_CLIENT_UCX_API_SRC}
    ${U_CONNECT_CLIENT_AT_API_SRC}
)

include(../../u-connectClient/cmake/stm32.cmake)

set(APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(UCXAPP_STM32_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ucxapp_main_stm32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ucxapp_platform_stm32.c
    ${APP_SRC}/ucxapp_io.c
    ${APP_SRC}/ucxapp_events.c
    ${APP_SRC}/ucxapp_store.c
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
# Boards with two spare erase sectors at the top of flash keep their files in littlefs there.
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
        ${APP_SRC}/ucxapp_cmds_net.c
        ${APP_SRC}/ucxapp_cmds_time.c
        ${APP_SRC}/ucxapp_cmds_cert.c
        ${APP_SRC}/ucxapp_cmds_examples.c
    )
endif()

string(TOLOWER "${STM32_BOARD}" _board)
set(UCXAPP_STM32_NAME "ucxapp_${_board}")
add_stm32_example(NAME ${UCXAPP_STM32_NAME} SOURCES ${UCXAPP_STM32_SOURCES})
# Full boards have the flash for -O2 (the shared default is -Og); lite boards stay at -Os.
if(NOT UCXAPP_LITE)
    target_compile_options(${UCXAPP_STM32_NAME}.elf PRIVATE -O2 -ffunction-sections -fdata-sections)
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
target_compile_definitions(${UCXAPP_STM32_NAME}.elf PRIVATE configTOTAL_HEAP_SIZE=${UCXAPP_HEAP_SIZE}
    U_CX_AT_CONFIG_FILE="ucxapp_logcfg.h")
