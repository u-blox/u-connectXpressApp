cmake_minimum_required(VERSION 3.10)
project(u-connectXpressApp)

# Include u-connectClient library definitions from submodule
set(UCX_MODULE "MERGED" CACHE STRING "Generated API variant in u-connectClient/ucx_api/generated (MERGED: NORA-W36X + NORA-B26X)")
if(UCX_MODULE MATCHES "^NORA-(W36X|B26X)$")
  # The app uses both modules' commands (Channel Sounding, power), so the single-module APIs no longer build it.
  message(STATUS "UCX_MODULE ${UCX_MODULE} -> MERGED")
  set(UCX_MODULE "MERGED" CACHE STRING "Generated API variant in u-connectClient/ucx_api/generated (MERGED: NORA-W36X + NORA-B26X)" FORCE)
endif()
include(../u-connectClient/u-connectClient.cmake)

# ============================================================================
# Auto-versioning: Use timestamp-based build number
# ============================================================================
file(GLOB _ucx_ver_hdr "${CMAKE_CURRENT_SOURCE_DIR}/../u-connectClient/ucx_api/generated/${UCX_MODULE}/u_cx_version.h")
file(READ "${_ucx_ver_hdr}" _ucx_ver_h)
foreach(_p MAJOR MINOR PATCH)
  string(REGEX MATCH "#define U_CX_VERSION_${_p} ([0-9]+)" _m "${_ucx_ver_h}")
  set(APP_VERSION_${_p} ${CMAKE_MATCH_1})
endforeach()
set(APP_VERSION_BUILD 0)

# Generate build number from current date: YYMMDD (e.g., 251204 = Dec 4, 2025)
# This is the most common CalVer format on GitHub
string(TIMESTAMP APP_VERSION_BUILD "%y%m%d")

message(STATUS "Build version: ${APP_VERSION_MAJOR}.${APP_VERSION_MINOR}.${APP_VERSION_PATCH}.${APP_VERSION_BUILD} (timestamp-based)")

# u-connectClient commit for app.sbom (read at configure time)
execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR}/../u-connectClient rev-parse --short HEAD
                OUTPUT_VARIABLE UCC_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT UCC_COMMIT)
  set(UCC_COMMIT "unknown")
endif()

# Generate version header
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/version.h
  @ONLY
)

# Web page served by web.start: app/web/index.html as a byte array (string literals hit MSVC length limits)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html _ucx_page HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _ucx_page "${_ucx_page}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" _ucx_page "${_ucx_page}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ucxapp_webpage.h.tmp
  "/* Generated from app/web/index.html by CMake; do not edit. */\nstatic const unsigned char kPageBytes[] = {\n${_ucx_page}0x00\n};\n#define kPage ((const char *)kPageBytes)\n")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/ucxapp_webpage.h.tmp ${CMAKE_CURRENT_BINARY_DIR}/ucxapp_webpage.h COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html)

# The page's /icon.svg (tab icon and header logo): app/web/icon.svg as a byte array
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/web/icon.svg _ucx_ico HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _ucx_ico "${_ucx_ico}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" _ucx_ico "${_ucx_ico}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ucxapp_favicon.h.tmp
  "/* Generated from app/web/icon.svg by CMake; do not edit. */\nstatic const unsigned char kFavicon[] = {\n${_ucx_ico}\n};\n")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/ucxapp_favicon.h.tmp ${CMAKE_CURRENT_BINARY_DIR}/ucxapp_favicon.h COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/icon.svg)

# Set binary output directory to bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# Release is built for speed: /O2 (MSVC) or -O3 (gcc) plus link-time optimisation.
include(CheckIPOSupported)
check_ipo_supported(RESULT _ucx_ipo OUTPUT _ucx_ipo_msg LANGUAGES C)
if(_ucx_ipo AND NOT EMSCRIPTEN)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# Browser build (emcmake): every object, mbedTLS included, must be built for shared memory.
if(EMSCRIPTEN)
  string(APPEND CMAKE_C_FLAGS " -pthread")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/mbedtls.cmake)

# Non-Windows hosts (Linux): command-line app built from the portable command layer only

if(NOT WIN32)

  find_package(Threads REQUIRED)



  add_executable(ucxapp

    src/ucxapp_main.c

    src/ucxapp_events.c
    src/ucxapp_thread.c
    src/ucxapp_ui.c
    src/ucxapp_tui.c
    src/ucxapp_cmds_web.c
    src/u_webserver.c

    src/ucxapp_io.c

    src/ucxapp_io_stdio.c
    src/ucxapp_store.c
    src/ucxapp_hash.c

    src/ucxapp_cmd.c

    src/ucxapp_urc.c

    src/ucxapp_cmds_device.c

    src/ucxapp_cmds_wifi.c
    src/ucxapp_cmds_wifiloc.c

    src/ucxapp_cmds_sock.c

    src/ucxapp_cmds_net.c

    src/ucxapp_cmds_sys.c
    src/ucxapp_cmds_app.c

    src/ucxapp_cmds_mqtt.c
    src/ucxapp_cmds_thread.c

    src/ucxapp_cmds_bt.c
    src/ucxapp_adv.c
    src/ucxapp_cmds_cert.c
    src/ucxapp_cmds_certfile.c
    src/ucxapp_crypt.c
    src/ucxapp_cmds_tls.c
    src/ucxapp_tls.c
    src/ucxapp_cmds_dtls.c
    src/ucxapp_dtls.c
    src/ucxapp_cmds_coap.c
    src/ucxapp_cmds_tftp.c
    src/ucxapp_cmds_ft.c
    src/ucxapp_cmds_ancs.c
    src/ucxapp_cmds_notify.c
    src/ucxapp_cmds_session.c
    src/ucxapp_cmds_cfg.c
    src/ucxapp_cmds_trace.c
    src/ucxapp_cmds_perf.c
    src/ucxapp_cmds_xplorer.c
    src/ucxapp_xplorer.c
    src/ucxapp_cmds_time.c
    src/ucxapp_cmds_can.c
    src/ucxapp_cmds_fw.c
    src/ucxapp_cmds_ymodem.c
    src/ucxapp_ymodem.c
    src/ucxapp_cmds_gatt.c
    src/ucxapp_cmds_qr.c
    src/ucxapp_cmds_utils.c
    src/ucxapp_stream.c
    src/ucxapp_corr.c
    src/ucxapp_cmds_gnss.c
    src/ucxapp_cmds_imu.c
    src/ucxapp_cmds_hw.c
    src/ucxapp_hwcfg.c
    src/ucxapp_cmds_cell.c
    src/ucxapp_cmds_dash.c
    src/ucxapp_cmds_hid.c
    src/ucxapp_cmds_examples.c
    src/ucxapp_cmds_usecase.c
    third-party/qrcodegen/qrcodegen.c

    src/ucxapp_platform_posix.c

    $<IF:$<BOOL:${EMSCRIPTEN}>,wasm/u_port_posix_wasm.c,../u-connectClient/ports/os/u_port_posix.c>

    $<IF:$<BOOL:${EMSCRIPTEN}>,wasm/u_port_uart_webserial.c,../u-connectClient/ports/uart/u_port_uart_linux.c>

    ${U_CONNECT_CLIENT_UCX_API_SRC}

    ${U_CONNECT_CLIENT_AT_API_SRC}

  )

  target_compile_options(ucxapp PRIVATE -Wall -Wextra)

  option(UCXAPP_COVERAGE "Instrument ucxapp with gcov" OFF)
  if(UCXAPP_COVERAGE)
    target_compile_options(ucxapp PRIVATE --coverage -O0 -g)
    target_link_options(ucxapp PRIVATE --coverage)
    # Keep the instrumented exe out of app/bin so it never replaces the normal build.
    set_target_properties(ucxapp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
                                            RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR})
  endif()

  target_compile_definitions(ucxapp PRIVATE U_PORT_POSIX
    UCXAPP_CRYPTO
    UCXAPP_UCC_COMMIT="${UCC_COMMIT}"
    UCXAPP_UCX_MODULE="${UCX_MODULE}"
    U_CX_AT_CONFIG_FILE="ucxapp_logcfg.h"
    U_CX_PORT_PRINTF=ucxappLibPrintf
    APP_VERSION_MAJOR=${APP_VERSION_MAJOR}
    APP_VERSION_MINOR=${APP_VERSION_MINOR}
    APP_VERSION_PATCH=${APP_VERSION_PATCH}
    APP_VERSION_BUILD=${APP_VERSION_BUILD})

  target_include_directories(ucxapp PRIVATE

    ${U_CONNECT_CLIENT_INC}

    ${U_CONNECT_CLIENT_PORT_DIR}

    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third-party

  )

  target_link_libraries(ucxapp PRIVATE Threads::Threads m ${UCXAPP_MBEDTLS_LIBS})

  # Browser build: build/wasm/site is a static web site (the page, the app as WebAssembly, Web Serial shim).
  if(EMSCRIPTEN)
    set(_ucx_site ${CMAKE_BINARY_DIR}/site)
    set_target_properties(ucxapp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${_ucx_site}
                                            RUNTIME_OUTPUT_DIRECTORY_RELEASE ${_ucx_site})
    target_link_options(ucxapp PRIVATE -pthread -sMODULARIZE=1 -sEXPORT_NAME=createUcxapp
      -sENVIRONMENT=web,worker -sEXIT_RUNTIME=0 -sPTHREAD_POOL_SIZE=8
      -sINITIAL_MEMORY=67108864 -sSTACK_SIZE=1048576 -sDEFAULT_PTHREAD_STACK_SIZE=262144
      "-sEXPORTED_FUNCTIONS=_main,_malloc,_free,_ucxapp_web_local_request,_ucxapp_web_local_body,_ucxapp_web_local_length,_ucxapp_web_local_type,_ucxapp_web_local_poll,_ucxapp_webserial_rx,_ucxapp_webserial_ready"
      -sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString,HEAPU8)
    add_custom_command(TARGET ucxapp POST_BUILD
      COMMAND ${CMAKE_COMMAND} -DPAGE=${CMAKE_CURRENT_SOURCE_DIR}/web/index.html -DOUT=${_ucx_site}/index.html
              -P ${CMAKE_CURRENT_SOURCE_DIR}/wasm/site.cmake
      COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/wasm/ucxapp-wasm.js
              ${CMAKE_CURRENT_SOURCE_DIR}/wasm/coi-serviceworker.js ${CMAKE_CURRENT_SOURCE_DIR}/wasm/vnora.json
              ${_ucx_site}
      COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/web/icon.svg ${_ucx_site}/icon.svg
      VERBATIM)
  endif()

  return()

endif()

set(U_EXAMPLE_UART "COM3" CACHE STRING "The UART device to use")

# Port layer files for Windows (from u-connectClient submodule)
set(PORT_OS_SRC ../u-connectClient/ports/os/u_port_windows.c)
set(PORT_UART_SRC ../u-connectClient/ports/uart/u_port_uart_windows.c)

# FTDI D2XX is loaded at run time from the DLL embedded by the .rc file

# u-connectXpressApp Application (ucxapp.exe)
add_executable(ucxapp
  src/ucxapp_main.c
  src/ucxapp_urc.c
  src/ucxapp_events.c
  src/ucxapp_thread.c
  src/ucxapp_ui.c
  src/ucxapp_tui.c
  src/ucxapp_cmds_web.c
  src/u_webserver.c
  src/ucxapp_io.c
  src/ucxapp_io_stdio.c
  src/ucxapp_store.c
  src/ucxapp_hash.c
  src/ucxapp_platform_win.c
  src/ucxapp_cmd.c
  src/ucxapp_cmds_device.c
  src/ucxapp_cmds_wifi.c
  src/ucxapp_cmds_wifiloc.c
  src/ucxapp_cmds_sock.c
  src/ucxapp_cmds_net.c
  src/ucxapp_cmds_sys.c
  src/ucxapp_cmds_app.c
  src/ucxapp_cmds_mqtt.c
  src/ucxapp_cmds_thread.c
  src/ucxapp_cmds_bt.c
  src/ucxapp_adv.c
  src/ucxapp_cmds_cert.c
  src/ucxapp_cmds_certfile.c
  src/ucxapp_crypt.c
  src/ucxapp_cmds_tls.c
  src/ucxapp_tls.c
  src/ucxapp_cmds_dtls.c
  src/ucxapp_dtls.c
  src/ucxapp_cmds_coap.c
  src/ucxapp_cmds_tftp.c
  src/ucxapp_cmds_ft.c
  src/ucxapp_cmds_ancs.c
  src/ucxapp_cmds_notify.c
  src/ucxapp_cmds_session.c
  src/ucxapp_cmds_cfg.c
  src/ucxapp_cmds_trace.c
  src/ucxapp_cmds_perf.c
  src/ucxapp_cmds_xplorer.c
  src/ucxapp_xplorer.c
  src/ucxapp_cmds_time.c
  src/ucxapp_cmds_can.c
  src/ucxapp_cmds_fw.c
  src/ucxapp_cmds_ymodem.c
  src/ucxapp_ymodem.c
  src/ucxapp_cmds_gatt.c
  src/ucxapp_cmds_qr.c
  src/ucxapp_cmds_utils.c
  src/ucxapp_stream.c
  src/ucxapp_corr.c
  src/ucxapp_cmds_gnss.c
  src/ucxapp_cmds_imu.c
  src/ucxapp_cmds_hw.c
  src/ucxapp_hwcfg.c
  src/ucxapp_cmds_cell.c
  src/ucxapp_cmds_dash.c
  src/ucxapp_cmds_hid.c
  src/ucxapp_cmds_examples.c
  src/ucxapp_cmds_usecase.c
  ${PORT_OS_SRC}
  ${PORT_UART_SRC}
  ucxapp.rc
  third-party/qrcodegen/qrcodegen.c
  ${U_CONNECT_CLIENT_UCX_API_SRC}
  ${U_CONNECT_CLIENT_AT_API_SRC}
)

target_compile_options(ucxapp PRIVATE /W4 /FS /wd4200)

target_compile_definitions(ucxapp PRIVATE
  U_PORT_WINDOWS
  UCXAPP_CRYPTO
  UCXAPP_UCC_COMMIT="${UCC_COMMIT}"
  UCXAPP_UCX_MODULE="${UCX_MODULE}"
  U_CX_PORT_HEADER_FILE="os/u_port_windows.h"
  U_CX_XMODEM_FILE_SUPPORT=1
  U_CX_AT_CONFIG_FILE="ucxapp_logcfg.h"
  U_CX_PORT_PRINTF=ucxappLibPrintf
  _CRT_SECURE_NO_WARNINGS
  U_EXAMPLE_UART="${U_EXAMPLE_UART}"
  APP_VERSION_MAJOR=${APP_VERSION_MAJOR}
  APP_VERSION_MINOR=${APP_VERSION_MINOR}
  APP_VERSION_PATCH=${APP_VERSION_PATCH}
  APP_VERSION_BUILD=${APP_VERSION_BUILD}
)

target_include_directories(ucxapp PUBLIC 
  ${U_CONNECT_CLIENT_INC} 
  ${U_CONNECT_CLIENT_PORT_DIR} 
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third-party
  ${CMAKE_CURRENT_SOURCE_DIR} 
  ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(ucxapp PRIVATE setupapi psapi ${UCXAPP_MBEDTLS_LIBS})

# Set different output names for Debug and Release builds
set_target_properties(ucxapp PROPERTIES
  OUTPUT_NAME_DEBUG "ucxapp-debug"
  OUTPUT_NAME_RELEASE "ucxapp"
)
