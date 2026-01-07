#pragma once

#include "tic_capture.h"
#include "tic_cpu.h"
#include <stdint.h>
#include <stddef.h>

// Statistics for a single channel
typedef struct {
    uint32_t count;           // Number of periods measured
    uint32_t edge_count;      // Number of edges captured
    uint32_t overflow_count;  // Number of timer overflows
    double min_ns;            // Minimum period in nanoseconds
    double max_ns;            // Maximum period in nanoseconds
    double mean_ns;           // Mean period in nanoseconds
    double stddev_ns;         // Standard deviation in nanoseconds
} tic_channel_stat_t;

// Statistics for relative delay between channels (B - A)
typedef struct {
    uint32_t count;           // Number of delay measurements
    uint32_t missed_a;        // Channel A pulses without matching B
    uint32_t missed_b;        // Channel B pulses without matching A
    double min_ns;            // Minimum delay (can be negative)
    double max_ns;            // Maximum delay
    double mean_ns;           // Mean delay
    double stddev_ns;         // Standard deviation of delay
} tic_delay_stat_t;

// Combined statistics for both channels and delay
typedef struct {
    tic_channel_stat_t ch_a;  // Channel A statistics
    tic_channel_stat_t ch_b;  // Channel B statistics
    tic_delay_stat_t delay;   // Relative delay B-A statistics
} tic_stats_t;

/**
 * @brief Process a buffer of events and calculate period statistics for both channels
 *
 * @param events Array of capture events (may contain interleaved A and B events)
 * @param event_count Number of events in the array
 * @param resolution_hz Capture timer resolution in Hz
 * @param stats Output: calculated statistics for both channels
 */
void tic_stats_process(const tic_event_t *events, size_t event_count,
                       uint32_t resolution_hz, tic_stats_t *stats);

/**
 * @brief Print statistics for a single channel to console
 *
 * @param stats Statistics to print
 * @param channel_name Name of the channel (e.g., "A" or "B")
 */
void tic_stats_print_channel(const tic_channel_stat_t *stats, const char *channel_name);

/**
 * @brief Print delay statistics to console
 *
 * @param stats Delay statistics to print
 */
void tic_stats_print_delay(const tic_delay_stat_t *stats);

/**
 * @brief Print statistics for both channels to console
 *
 * @param stats Statistics to print
 * @param cpu_stats CPU idle statistics (can be NULL)
 */
void tic_stats_print(const tic_stats_t *stats, const tic_cpu_stats_t *cpu_stats);
