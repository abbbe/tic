#pragma once

#include "tic_stats.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Streaming edge matcher for delay calculation
 *
 * Matches edges between channels A and B to calculate delay statistics.
 * Unlike batch processing, this matcher maintains state across buffer
 * boundaries, ensuring edges at the end of one buffer can match with
 * edges at the start of the next buffer.
 *
 * Uses a simple two-slot state machine:
 * - One pending slot per channel
 * - When edge arrives, replaces any existing pending edge on same channel
 *   (since newer edge is always a better match candidate)
 * - When both slots are occupied, tries to match and clear
 */

typedef struct {
    // Pending edge timestamps (0 = no pending edge)
    // Using extended 64-bit timestamps for overflow handling
    uint64_t pending_a;
    uint64_t pending_b;

    // Overflow tracking for timestamp extension
    uint32_t last_value_a;
    uint32_t last_value_b;
    uint64_t overflow_count_a;
    uint64_t overflow_count_b;

    // Timer resolution for ns conversion
    double ns_per_tick;

    // Running delay statistics (Welford's algorithm)
    uint32_t delay_count;
    double delay_mean;
    double delay_m2;
    double delay_min;
    double delay_max;
    uint32_t missed_a;
    uint32_t missed_b;
} tic_matcher_t;

/**
 * @brief Initialize the matcher
 *
 * @param m Matcher instance
 * @param resolution_hz Capture timer resolution in Hz
 */
void tic_matcher_init(tic_matcher_t *m, uint32_t resolution_hz);

/**
 * @brief Feed an edge to the matcher
 *
 * @param m Matcher instance
 * @param channel TIC_CHANNEL_A or TIC_CHANNEL_B
 * @param value Raw 32-bit capture timer value
 * @param max_delay_ns Maximum delay to consider a valid match (e.g., period/2)
 * @return true if this edge resulted in a match
 */
bool tic_matcher_feed_edge(tic_matcher_t *m, uint8_t channel,
                           uint32_t value, double max_delay_ns);

/**
 * @brief Feed an overflow event
 *
 * Call this when a timer overflow event is encountered in the edge stream.
 *
 * @param m Matcher instance
 */
void tic_matcher_feed_overflow(tic_matcher_t *m);

/**
 * @brief Flush stale pending edges
 *
 * Call this after processing a buffer to clean up any pending edges
 * that are older than max_age_ns.
 *
 * @param m Matcher instance
 * @param current_time Extended timestamp of "now"
 * @param max_age_ns Maximum age before edge is considered missed
 */
void tic_matcher_timeout(tic_matcher_t *m, uint64_t current_time, double max_age_ns);

/**
 * @brief Get delay statistics and optionally reset
 *
 * @param m Matcher instance
 * @param stats Output: delay statistics
 * @param reset If true, reset internal counters after copying
 */
void tic_matcher_get_stats(tic_matcher_t *m, tic_delay_stat_t *stats, bool reset);
