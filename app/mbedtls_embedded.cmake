# mbedTLS 4 compiled straight into a board image (STM32 FreeRTOS and Zephyr): server-only TLS with the configs
# mbedtls_embedded_crypto.h / mbedtls_embedded_tls.h. Sets UCXAPP_MBEDTLS_SOURCES, UCXAPP_MBEDTLS_INCLUDES and
# UCXAPP_MBEDTLS_DEFINES; the sources build without warnings flags (vendor code), gc-sections drops what is unused.

set(_mt ${CMAKE_CURRENT_LIST_DIR}/../third-party/mbedtls)
set(_gen ${CMAKE_CURRENT_LIST_DIR}/third-party/mbedtls-gen)
set(_psa ${_mt}/tf-psa-crypto)

file(GLOB UCXAPP_MBEDTLS_SOURCES
  ${_mt}/library/*.c
  ${_psa}/core/*.c
  ${_psa}/drivers/builtin/src/*.c
  ${_psa}/extras/*.c
  ${_psa}/platform/*.c
  ${_psa}/utilities/*.c)
# The generated sources come from the vendored copies.
list(FILTER UCXAPP_MBEDTLS_SOURCES EXCLUDE REGEX "/(error|ssl_debug_helpers_generated|version_features|psa_crypto_driver_wrappers_no_static)\\.c$")
list(APPEND UCXAPP_MBEDTLS_SOURCES
  ${_gen}/library/error.c
  ${_gen}/library/ssl_debug_helpers_generated.c
  ${_gen}/library/version_features.c
  ${_gen}/tf-psa-crypto/core/psa_crypto_driver_wrappers_no_static.c)
set_source_files_properties(${UCXAPP_MBEDTLS_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

set(UCXAPP_MBEDTLS_INCLUDES
  ${_gen}/library
  ${_gen}/tf-psa-crypto/core
  ${_mt}/include
  ${_mt}/library
  ${_psa}/include
  ${_psa}/core
  ${_psa}/dispatch
  ${_psa}/drivers/builtin/include
  ${_psa}/drivers/builtin/src
  ${_psa}/extras
  ${_psa}/platform
  ${_psa}/utilities)

set(UCXAPP_MBEDTLS_DEFINES
  "MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_LIST_DIR}/mbedtls_embedded_tls.h\""
  "TF_PSA_CRYPTO_USER_CONFIG_FILE=\"${CMAKE_CURRENT_LIST_DIR}/mbedtls_embedded_crypto.h\"")
