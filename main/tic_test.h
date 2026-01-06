#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize signal generator A
 *
 * @param gpio GPIO pin for output
 * @param freq_hz Frequency in Hz
 * @param loopback Enable internal loopback
 * @return ESP_OK on success
 */
esp_err_t tic_siggen_init_a(int gpio, uint32_t freq_hz, bool loopback);

/**
 * @brief Initialize signal generator B with relative delay to A
 *
 * Generator B is synchronized to A via software sync. The delay is applied
 * as a phase offset when sync triggers.
 *
 * @param gpio GPIO pin for output
 * @param freq_hz Frequency in Hz
 * @param delay_ns Delay relative to A in nanoseconds (positive = B lags A)
 * @param loopback Enable internal loopback
 * @return ESP_OK on success
 */
esp_err_t tic_siggen_init_b(int gpio, uint32_t freq_hz, int32_t delay_ns, bool loopback);

/**
 * @brief Start signal generators (synchronized)
 * @return ESP_OK on success
 */
esp_err_t tic_siggen_start(void);

/**
 * @brief Stop signal generators
 * @return ESP_OK on success
 */
esp_err_t tic_siggen_stop(void);
