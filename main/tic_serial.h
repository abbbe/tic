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

// Binary frame header (28 bytes)
// Frame format: header (28) + events (N*8) + crc32 (4)
// CRC32 is calculated over header + events using CRC32-LE (zlib compatible)
typedef struct __attribute__((packed)) {
    uint32_t magic;           // "TIC1" = 0x31434954
    uint32_t version;         // Protocol version (1) // FIXME drop this, might include into a meta-header
    uint32_t resolution_hz;   // Timer resolution (80000000) // FIXME this belongs to meta-header
    uint32_t seq;             // Frame sequence number // FIXME could use 16 bits, it rolls over anyways
    uint64_t first_edge_ts;   // 64-bit timestamp of first edge in frame // FIXME could be 32 bits with proper extension
    uint16_t event_count;     // Number of events following header // FIXME this is not enough for high frequency
    uint16_t flags;           // Flags (overflow, etc.) // FIXME too many flags, we only use one bit now...
} tic_frame_header_t;

/**
 * @brief Initialize serial output module
 *
 * Sets up USB CDC for binary output.
 */
void tic_serial_init(void);

/**
 * @brief Send a binary frame containing edge events
 *
 * @param events Array of edge events
 * @param event_count Number of events
 * @param resolution_hz Timer resolution
 * @param overflow True if overflow occurred
 */
void tic_serial_send_frame(const tic_event_t *events, size_t event_count,
                           uint32_t resolution_hz, bool overflow);

/**
 * @brief Send statistics as JSON (for compatibility with existing tools)
 *
 * @param stats Statistics structure
 * @param cpu_stats CPU statistics (can be NULL)
 */
void tic_serial_send_stats(const tic_stats_t *stats, const tic_cpu_stats_t *cpu_stats);
