#pragma once

#include "tic_capture.h"
#include "tic_stats.h"
#include "tic_types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Initialize WiFi and MQTT transport
 *
 * Sets up WiFi connection and MQTT client.
 * Uses CONFIG_TIC_WIFI_* settings from sdkconfig.
 *
 * @return ESP_OK on success
 */
esp_err_t tic_wifi_init(void);

/**
 * @brief Check if WiFi and MQTT are connected
 *
 * @return true if both WiFi and MQTT are connected
 */
bool tic_wifi_is_connected(void);

/**
 * @brief Send a binary frame via MQTT
 *
 * Uses same frame format as USB CDC (tic_serial).
 * If MQTT is disconnected, buffers frame in ring buffer.
 *
 * @param pairs Array of matched pairs
 * @param pair_count Number of pairs
 * @param stats Statistics for this frame
 * @param cpu_stats CPU statistics (can be NULL)
 * @param resolution_hz Timer resolution
 * @param base_ts Base timestamp for the frame
 * @param overflow True if overflow occurred
 */
void tic_wifi_send_frame(const tic_matched_pair_t *pairs, uint16_t pair_count,
                         const tic_stats_t *stats, const tic_cpu_stats_t *cpu_stats,
                         uint32_t resolution_hz, uint64_t base_ts, bool overflow);

/**
 * @brief Get ring buffer statistics
 *
 * @param buffered Output: number of frames currently buffered
 * @param dropped Output: number of frames dropped (buffer full)
 */
void tic_wifi_get_buffer_stats(uint32_t *buffered, uint32_t *dropped);
