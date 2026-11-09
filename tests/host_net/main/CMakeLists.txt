set(SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../..")

file(GLOB CASE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../cases/test_*.c")
# Sorted: a case registers itself from a constructor, so link order is run
# order, and an unsorted glob returns whatever the filesystem hands it - a
# different order on the bench than in CI. See tests/host/main/CMakeLists.txt.
list(SORT CASE_SOURCES)

file(GLOB khal_includes LIST_DIRECTORIES true "${SDK_DIR}/components/khal/*/include")

idf_component_register(
    SRCS "runner.c"
         "log_stub.c"
         "identity_stub.c"
         "crt_bundle_stub.c"
         "time_stub.c"
         ${CASE_SOURCES}
         "${SDK_DIR}/components/kdrv/radio/src/kdrv_net.c"
    INCLUDE_DIRS "."
                 "shim"
                 "${SDK_DIR}/components/kdrv/audio/include"
                 "${SDK_DIR}/components/kdrv/buttons/include"
                 "${SDK_DIR}/components/kdrv/clocks/include"
                 "${SDK_DIR}/components/kdrv/cst820/include"
                 "${SDK_DIR}/components/kdrv/display/include"
                 "${SDK_DIR}/components/kdrv/led/include"
                 "${SDK_DIR}/components/kdrv/tca6408a/include"
                 "${SDK_DIR}/components/kdrv/radio/include"
                 ${khal_includes}
                 "${SDK_DIR}/components/kdefs/include"
                 "${SDK_DIR}/components/klog/include"
    REQUIRES unity freertos esp_netif esp_http_client ping_sock
    WHOLE_ARCHIVE)

# kdrv_net refuses to compile without the certificate bundle, because https://
# would silently stop verifying anything. Nothing here opens a TLS connection -
# the HTTP client is a mock - but the guard is right and the definition is how
# a host build satisfies it honestly.
# kdrv_net.c is compiled for real here, so every Kconfig symbol its headers
# reach for has to exist. esp_netif_sntp.h sizes its server list from
# CONFIG_LWIP_SNTP_MAX_SERVERS, which the linux target's sdkconfig does not
# define at all - the header does not fail soft, it fails to compile.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1"
    "CONFIG_LWIP_SNTP_MAX_SERVERS=1")

# Sanitizers, only when KODE_SANITIZE is set; see tests/sanitize.cmake.
include(${CMAKE_CURRENT_LIST_DIR}/../../sanitize.cmake)
