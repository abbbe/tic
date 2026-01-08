set(SRCS
    "main.c"
    "tic_capture.c"
    "tic_cpu.c"
    "tic_matcher.c"
    "tic_stats.c"
    "tic_test.c"
)

set(REQUIRES
    driver
    esp_timer
)

set(PRIV_REQUIRES
    esp_wifi
    esp_netif
    esp_event
    nvs_flash
    mqtt
)

if(CONFIG_TIC_OUTPUT_BINARY)
    list(APPEND SRCS "tic_serial.c")
    list(APPEND REQUIRES esp_tinyusb)
endif()

if(CONFIG_TIC_WIFI_ENABLED)
    list(APPEND SRCS "tic_wifi.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${REQUIRES}
    PRIV_REQUIRES ${PRIV_REQUIRES}
)
