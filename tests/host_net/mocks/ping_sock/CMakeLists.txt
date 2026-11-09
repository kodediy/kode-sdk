# CMock over ping/ping_sock.h. It lives inside lwip, which is excluded on
# linux, so the header is reached by path rather than by component.
idf_component_mock(
    INCLUDE_DIRS "$ENV{IDF_PATH}/components/lwip/include/apps"
                 "$ENV{IDF_PATH}/components/lwip/include/apps/ping"
                 "${CMAKE_CURRENT_LIST_DIR}/../../main/shim"
    REQUIRES esp_common esp_system
    MOCK_HEADER_FILES
        "$ENV{IDF_PATH}/components/lwip/include/apps/ping/ping_sock.h")
