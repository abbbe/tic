#pragma once

#include "tic_capture.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t count;           // Number of periods measured
    uint32_t overflow_count;  // Number of timer overflows
    double min_ns;            // Minimum period in nanoseconds
    double max_ns;            // Maximum period in nanoseconds
    double mean_ns;           // Mean period in nanoseconds
    double stddev_ns;         // Standard deviation in nanoseconds
} tic_stat_t;

/**
 * @brief Process a buffer of events and calculate period statistics
 *
 * @param events Array of capture events
 * @param event_count Number of events in the array
 * @param resolution_hz Capture timer resolution in Hz
 * @param stats Output: calculated statistics
 */
void tic_stats_process(const tic_event_t *events, size_t event_count,
                       uint32_t resolution_hz, tic_stat_t *stats);

/**
 * @brief Print statistics to console
 *
 * @param stats Statistics to print
 */
void tic_stats_print(const tic_stat_t *stats);
