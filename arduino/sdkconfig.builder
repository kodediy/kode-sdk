# What only the library builder's project needs, read after
# arduino/sdkconfig.sdk: picolibc, and an SDK app's value for every option the
# builder's defaults change that Arduino's core does not need its own of
# (arduino/sdkconfig.keep lists those).

# picolibc, ESP-IDF's default C library: khal's environment lock is its own. (ESP-IDF)
CONFIG_LIBC_PICOLIBC=y

# The PSRAM is up and in the heap before app_main. (ESP-IDF)
CONFIG_SPIRAM_BOOT_HW_INIT=y
CONFIG_SPIRAM_BOOT_INIT=y
# 64-byte L2 cache lines: kdrv_display aligns its frame buffers to them. (ESP-IDF)
CONFIG_CACHE_L2_CACHE_LINE_64B=y
# 32 KB of internal RAM stay out of malloc()'s reach, for DMA buffers. (ESP-IDF)
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
# The interrupt watchdog's own 800 ms. (ESP-IDF)
CONFIG_ESP_INT_WDT_TIMEOUT_MS=800
# No stack canaries and no heap poisoning: the SDK's budgets are measured without them. (ESP-IDF)
CONFIG_COMPILER_STACK_CHECK_MODE_NONE=y
CONFIG_HEAP_POISONING_DISABLED=y
# The stacks ESP-IDF gives its own tasks. (ESP-IDF)
CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=1536
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304
CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=3072
# File names as kodeOS writes them, and long names built on the heap. (ESP-IDF)
CONFIG_FATFS_CODEPAGE_437=y
CONFIG_FATFS_API_ENCODING_ANSI_OEM=y
CONFIG_FATFS_LFN_HEAP=y
# The SPI interrupt handlers in IRAM. (ESP-IDF)
CONFIG_SPI_MASTER_ISR_IN_IRAM=y
CONFIG_SPI_SLAVE_ISR_IN_IRAM=y
# Flash writes and erases in ESP-IDF's own steps. (ESP-IDF)
CONFIG_SPI_FLASH_WRITE_CHUNK_SIZE=8192
CONFIG_SPI_FLASH_ERASE_YIELD_DURATION_MS=20
CONFIG_SPI_FLASH_ERASE_YIELD_TICKS=1
# A sketch is never a slot to roll back: kodeOS owns otadata. (ESP-IDF)
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n
# kodeOS's bootloader starts a sketch, so nothing is reserved for this one's deep-sleep wake. (ESP-IDF)
CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=n
# No -Wwrite-strings for C. (ESP-IDF)
CONFIG_COMPILER_WARN_WRITE_STRINGS=n
# A sketch's panic leaves no core dump, as an SDK app's does not: the coredump
# partition is kodeOS's, and a sketch's dump would replace kodeOS's own. (ESP-IDF)
CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y
# A task watchdog that prints and does not panic. (ESP-IDF)
CONFIG_ESP_TASK_WDT_PANIC=n
# No run-time statistics, and no formatted task table: no counter read at every context switch. (ESP-IDF)
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=n
CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS=n
# No volume labels. (ESP-IDF)
CONFIG_FATFS_USE_LABEL=n
# No HTTPS server, no WebSocket server, no basic auth in the HTTP client. (ESP-IDF)
CONFIG_ESP_HTTPS_SERVER_ENABLE=n
CONFIG_HTTPD_WS_SUPPORT=n
CONFIG_ESP_HTTP_CLIENT_ENABLE_BASIC_AUTH=n
# The I2S and RGB panel interrupt handlers in flash. (ESP-IDF)
CONFIG_I2S_ISR_IRAM_SAFE=n
CONFIG_LCD_RGB_ISR_IRAM_SAFE=n
# Wi-Fi's block-ack windows. (ESP-IDF)
CONFIG_WIFI_RMT_RX_BA_WIN=6
CONFIG_WIFI_RMT_TX_BA_WIN=6
# lwIP's buffers, queues, timers and task. (ESP-IDF)
CONFIG_LWIP_MAX_SOCKETS=10
CONFIG_LWIP_TCP_MSS=1440
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=5760
CONFIG_LWIP_TCP_WND_DEFAULT=5760
CONFIG_LWIP_TCP_RECVMBOX_SIZE=6
CONFIG_LWIP_UDP_RECVMBOX_SIZE=6
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=32
CONFIG_LWIP_TCP_RTO_TIME=1500
CONFIG_LWIP_TCP_SYNMAXRTX=12
CONFIG_LWIP_TCP_SACK_OUT=n
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=3072
CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY=y
# lwIP's IPv6 with three addresses and none of ESP-IDF's routing hooks. (ESP-IDF)
CONFIG_LWIP_IPV6_NUM_ADDRESSES=3
CONFIG_LWIP_HOOK_IP6_ROUTE_NONE=y
CONFIG_LWIP_HOOK_ND6_GET_GW_NONE=y
CONFIG_LWIP_HOOK_IP6_SELECT_SRC_ADDR_NONE=y
CONFIG_LWIP_HOOK_NETCONN_EXT_RESOLVE_NONE=y
# No pings to broadcast or multicast addresses, no NTP servers from DHCP, SNTP every hour. (ESP-IDF)
CONFIG_LWIP_BROADCAST_PING=n
CONFIG_LWIP_MULTICAST_PING=n
CONFIG_LWIP_DHCP_GET_NTP_SRV=n
CONFIG_LWIP_DHCP_OPTIONS_LEN=69
CONFIG_LWIP_SNTP_UPDATE_DELAY=3600000
# No DTLS, Camellia or EC J-PAKE; TLS buffers kept for the session and its fragments sized each way. (ESP-IDF)
CONFIG_MBEDTLS_SSL_PROTO_DTLS=n
CONFIG_MBEDTLS_CAMELLIA_C=n
CONFIG_MBEDTLS_ECJPAKE_C=n
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
CONFIG_MBEDTLS_DYNAMIC_BUFFER=n
