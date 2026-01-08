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

if(CONFIG_TIC_OUTPUT_BINARY)
    list(APPEND SRCS "tic_serial.c")
    list(APPEND REQUIRES esp_tinyusb)
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${REQUIRES}
)
