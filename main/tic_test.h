#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Initialize the test signal generator (MCPWM PWM)
 *
 * @param gpio_num GPIO pin for PWM output
 * @param freq_hz Desired frequency in Hz
 * @return ESP_OK on success
 */
esp_err_t tic_test_init(int gpio_num, uint32_t freq_hz);

/**
 * @brief Start the test signal generator
 * @return ESP_OK on success
 */
esp_err_t tic_test_start(void);

/**
 * @brief Stop the test signal generator
 * @return ESP_OK on success
 */
esp_err_t tic_test_stop(void);
