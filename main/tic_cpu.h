#pragma once

#include <stdbool.h>

typedef struct {
    float util_pct_cpu0;  // CPU utilization (100 - idle)
    float util_pct_cpu1;
} tic_cpu_stats_t;

/**
 * @brief Initialize CPU idle time measurement
 *
 * Registers tick and idle hooks for both cores.
 */
void tic_cpu_init(void);

/**
 * @brief Get CPU idle statistics
 *
 * @param stats Output: idle percentages for each core
 * @param reset If true, reset counters after reading
 */
void tic_cpu_get_stats(tic_cpu_stats_t *stats, bool reset);
