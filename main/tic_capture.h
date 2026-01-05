#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Event types
#define TIC_EVENT_EDGE     0
#define TIC_EVENT_OVERFLOW 1

// Channel identifiers
#define TIC_CHANNEL_A      0
#define TIC_CHANNEL_B      1

// Event group bit signaled when buffer is ready
#define TIC_BUFFER_READY_BIT  BIT0

typedef struct {
    uint8_t  type;       // TIC_EVENT_EDGE or TIC_EVENT_OVERFLOW
    uint8_t  channel;    // TIC_CHANNEL_A or TIC_CHANNEL_B
    uint16_t reserved;
    uint32_t value;      // For edge: 32-bit capture value
                         // For overflow: not used
} tic_event_t;

/**
 * @brief Initialize the capture module with two channels
 *
 * @param gpio_a GPIO pin for Channel A capture input
 * @param gpio_b GPIO pin for Channel B capture input (-1 to disable)
 * @param loopback Enable internal loopback (for self-test mode, applies to Channel A only)
 * @param edges_per_buffer Number of edges to capture before swapping buffers
 * @return ESP_OK on success
 */
esp_err_t tic_capture_init(int gpio_a, int gpio_b, bool loopback, size_t edges_per_buffer);

/**
 * @brief Start capturing on all enabled channels
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
 * @return Resolution in Hz (e.g., 80000000 for 80 MHz)
 */
uint32_t tic_capture_get_resolution(void);

/**
 * @brief Force a buffer swap (for periodic reporting even with 0 edges)
 *
 * Call this from a timer to ensure stats are reported periodically
 * even if edge count threshold is not reached.
 */
void tic_capture_force_swap(void);
