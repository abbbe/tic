---
layout: post
title: "TIC v0.1 - First Release"
date: 2026-01-05
---

Initial release of the Time Interval Counter for ESP32-S3.

## Capture Engine

- MCPWM capture peripheral at 80 MHz (12.5 ns resolution)
- Dual channel support (A and B) on configurable GPIOs
- ISR-driven double buffering with configurable threshold (up to 8192 edges)
- Overflow handling for extended timestamps

## Statistics

Per-channel period measurement:
- Edge count
- Min/max/mean period
- Standard deviation (Welford's online algorithm)
- Frequency calculation

Inter-channel delay (B - A):
- Window-based edge matching (quarter-period window)
- O(n) algorithm with limited lookahead
- Missed pulse tracking
- Signed delay support (B can lead or lag A)

## Test Infrastructure

- Loopback mode: internal PWM routed to capture input
- External PWM output mode: reference signal generation
- Configurable test frequencies (1 Hz to 1 MHz)

## Configuration

All options via `idf.py menuconfig`:

```
TIC Configuration
├── Loopback test mode [off]
│   └── Test frequency [1000 Hz]
├── Channel A GPIO [4]
├── Channel B GPIO [5]
├── Max buffer size [8192]
├── Edges per buffer [8192]
├── Stats period [1000 ms]
└── External PWM Output
    ├── Enable [off]
    ├── GPIO [4]
    └── Frequency [1000 Hz]
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      main.c                             │
│  - Periodic timer (force swap)                          │
│  - Event loop (wait for buffer ready)                   │
└─────────────────────────────────────────────────────────┘
         │                              │
         ▼                              ▼
┌─────────────────────┐    ┌─────────────────────────────┐
│   tic_capture.c     │    │       tic_stats.c           │
│  - MCPWM capture    │    │  - Welford's algorithm      │
│  - Double buffering │    │  - Period calculation       │
│  - ISR handling     │    │  - Delay matching           │
└─────────────────────┘    └─────────────────────────────┘
         │
         ▼
┌─────────────────────┐
│    tic_test.c       │
│  - MCPWM PWM gen    │
│  - Loopback mode    │
└─────────────────────┘
```

## API

### tic_capture.h

```c
esp_err_t tic_capture_init(int gpio_a, int gpio_b, bool loopback, size_t edges_per_buffer);
esp_err_t tic_capture_start(void);
esp_err_t tic_capture_stop(void);
tic_event_t* tic_capture_get_ready_buffer(size_t *count);
uint32_t tic_capture_get_resolution(void);
void tic_capture_force_swap(void);
```

### tic_stats.h

```c
void tic_stats_process(const tic_event_t *events, size_t count, uint32_t resolution_hz, tic_stats_t *stats);
void tic_stats_print(const tic_stats_t *stats);
```

### Data structures

```c
typedef struct {
    uint8_t type;      // TIC_EVENT_EDGE or TIC_EVENT_OVERFLOW
    uint8_t channel;   // TIC_CHANNEL_A or TIC_CHANNEL_B
    uint32_t value;    // 32-bit capture timestamp
} tic_event_t;

typedef struct {
    uint32_t count, edge_count, overflow_count;
    double min_ns, max_ns, mean_ns, stddev_ns;
} tic_channel_stat_t;

typedef struct {
    uint32_t count, missed_a, missed_b;
    double min_ns, max_ns, mean_ns, stddev_ns;
} tic_delay_stat_t;
```
