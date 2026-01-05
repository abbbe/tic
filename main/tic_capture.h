#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TIC_EVENT_EDGE     0
#define TIC_EVENT_OVERFLOW 1

// Event group bit signaled when buffer is ready
#define TIC_BUFFER_READY_BIT  BIT0

typedef struct {
    uint8_t  type;       // TIC_EVENT_EDGE or TIC_EVENT_OVERFLOW
    uint8_t  reserved;
    uint16_t reserved2;
    uint32_t value;      // For edge: 32-bit capture value
                         // For overflow: not used
} tic_event_t;

/**
 * @brief Initialize the capture module
 *
 * @param gpio_num GPIO pin for capture input
 * @param loopback Enable internal loopback (for self-test mode)
 * @param edges_per_buffer Number of edges to capture before swapping buffers
 * @return ESP_OK on success
 */
esp_err_t tic_capture_init(int gpio_num, bool loopback, size_t edges_per_buffer);

/**
 * @brief Start capturing
 * @return ESP_OK on success
 */
esp_err_t tic_capture_start(void);

/**
 * @brief Stop capturing
 * @return ESP_OK on success
 */
esp_err_t tic_capture_stop(void);

/**
 * @brief Get the event group handle for buffer-ready notifications
 * @return Event group handle
 */
EventGroupHandle_t tic_capture_get_event_group(void);

/**
 * @brief Get the ready buffer for processing (call after TIC_BUFFER_READY_BIT is set)
 *
 * @param count Output: number of events in the returned buffer
 * @return Pointer to the buffer of events (valid until next buffer ready)
 */
tic_event_t* tic_capture_get_ready_buffer(size_t *count);

/**
 * @brief Get the capture timer resolution in Hz
 * @return Resolution in Hz (e.g., 160000000 for 160 MHz)
 */
uint32_t tic_capture_get_resolution(void);

/**
 * @brief Force a buffer swap (for periodic reporting even with 0 edges)
 *
 * Call this from a timer to ensure stats are reported periodically
 * even if edge count threshold is not reached.
 */
void tic_capture_force_swap(void);
