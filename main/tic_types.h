#pragma once

#include <stdint.h>

// Matched pair structure (8 bytes)
typedef struct __attribute__((packed)) {
    uint32_t ts_a;    // Channel A timestamp (32-bit, relative to base_ts)
    uint32_t ts_b;    // Channel B timestamp (32-bit, relative to base_ts)
} tic_matched_pair_t;
