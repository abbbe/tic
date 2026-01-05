#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Initialize the test signal generator with loopback (MCPWM PWM)
 *
 * Uses io_loop_back for internal signal routing (same pin for output and capture).
 *
 * @param gpio_num GPIO pin for PWM output (with loopback)
 * @param freq_hz Desired frequency in Hz
 * @return ESP_OK on success
 */
esp_err_t tic_test_init(int gpio_num, uint32_t freq_hz);

/**
 * @brief Initialize PWM output without loopback (MCPWM PWM)
 *
 * Normal GPIO output, no internal loopback. Use for external PWM generation.
 *
 * @param gpio_num GPIO pin for PWM output
 * @param freq_hz Desired frequency in Hz
 * @return ESP_OK on success
 */
esp_err_t tic_test_init_no_loopback(int gpio_num, uint32_t freq_hz);

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
