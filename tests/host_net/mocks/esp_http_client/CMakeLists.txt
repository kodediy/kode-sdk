# CMock over esp_http_client.h. The component returns early from its own
# CMakeLists for the linux target, so there is nothing real to link.
#
# esp_security is on the list because the public esp_http_client.h includes
# esp_key_config.h, which lives there. That component registers its include
# directory and returns for the linux target precisely so this works; the
# directories here are spelled out one by one, so a header that moves into a
# new component has to be spelled out too.
idf_component_mock(
    INCLUDE_DIRS "$ENV{IDF_PATH}/components/esp_http_client/include"
                 "$ENV{IDF_PATH}/components/tcp_transport/include"
                 "$ENV{IDF_PATH}/components/esp-tls"
                 "$ENV{IDF_PATH}/components/esp_security/include"
    REQUIRES esp_common esp_event
    MOCK_HEADER_FILES
        "$ENV{IDF_PATH}/components/esp_http_client/include/esp_http_client.h")
