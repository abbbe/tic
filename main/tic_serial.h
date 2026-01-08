#pragma once

#include "tic_capture.h"
#include "tic_stats.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Binary frame magic number: "TIC1"
#define TIC_FRAME_MAGIC     0x31434954  // "TIC1" in little-endian

// Frame flags
#define TIC_FRAME_FLAG_OVERFLOW  (1 << 0)  // Buffer overflow occurred

// Matched pair structure (8 bytes)
typedef struct __attribute__((packed)) {
    uint32_t ts_a;    // Channel A timestamp (32-bit, relative to base_ts)
    uint32_t ts_b;    // Channel B timestamp (32-bit, relative to base_ts)
} tic_matched_pair_t;

// Binary frame header (74 bytes)
// Frame format: header (74) + pairs (N*8) + crc32 (4)
// CRC32 is calculated over header + pairs using CRC32-LE (zlib compatible)
typedef struct __attribute__((packed)) {
    // Identity (8 bytes)
    uint32_t magic;           // "TIC1" = 0x31434954
    uint32_t seq;             // Frame sequence number

    // Timing (12 bytes)
    uint32_t resolution_hz;   // Timer clock (80MHz)
    uint64_t base_ts;         // 64-bit base timestamp for this frame

    // Counts (12 bytes)
    uint16_t pair_count;      // Number of matched pairs in frame
    uint16_t edges_a;         // Total channel A edges seen
    uint16_t edges_b;         // Total channel B edges seen
    uint16_t miss_a;          // Unmatched channel A edges
    uint16_t miss_b;          // Unmatched channel B edges
    uint16_t flags;           // OVERFLOW, etc.

    // Delay stats (16 bytes) - in nanoseconds, signed for B-A
    float    delay_mean_ns;
    float    delay_min_ns;
    float    delay_max_ns;
    float    delay_stddev_ns;

    // CPU stats (2 bytes)
    uint8_t  cpu0_pct;        // CPU0 utilization 0-100
    uint8_t  cpu1_pct;        // CPU1 utilization 0-100

    // Period stats Channel A (12 bytes) - in nanoseconds
    uint32_t period_a_mean_ns;
    uint32_t period_a_min_ns;
    uint32_t period_a_max_ns;

    // Period stats Channel B (12 bytes) - in nanoseconds
    uint32_t period_b_mean_ns;
    uint32_t period_b_min_ns;
    uint32_t period_b_max_ns;
} tic_frame_header_t;

/**
 * @brief Initialize serial output module
 *
 * Sets up USB CDC for binary output.
 */
void tic_serial_init(void);

/**
 * @brief Send a binary frame containing matched pairs and stats
 *
 * @param pairs Array of matched pairs
 * @param pair_count Number of pairs
 * @param stats Statistics for this frame
 * @param cpu_stats CPU statistics (can be NULL)
 * @param resolution_hz Timer resolution
 * @param base_ts Base timestamp for the frame
 * @param overflow True if overflow occurred
 */
void tic_serial_send_frame(const tic_matched_pair_t *pairs, uint16_t pair_count,
                           const tic_stats_t *stats, const tic_cpu_stats_t *cpu_stats,
                           uint32_t resolution_hz, uint64_t base_ts, bool overflow);

/**
 * @brief Send statistics as JSON (for compatibility with existing tools)
 *
 * @param stats Statistics structure
 * @param cpu_stats CPU statistics (can be NULL)
 */
void tic_serial_send_stats(const tic_stats_t *stats, const tic_cpu_stats_t *cpu_stats);
