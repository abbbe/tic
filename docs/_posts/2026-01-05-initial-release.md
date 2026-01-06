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

## Signal Generator

- Dual PWM outputs with independent frequencies
- Configurable relative delay (ns) for generator B
- Auto loopback: internal routing when gen GPIO == capture GPIO
- Frequency range: 1 Hz to 1 MHz

## Configuration

All options via `idf.py menuconfig`:

```
TIC Configuration
├── Channel A input GPIO [4]
├── Channel B input GPIO [5]
└── Signal Generator
    ├── Generator A freq (0=disable) [1000 Hz]
    ├── Generator A GPIO [6]
    ├── Generator B freq (0=disable) [1000 Hz]
    ├── Generator B GPIO [7]
    └── Generator B relative delay [0 ns]
├── Max buffer size [8192]
├── Edges per buffer [8192]
└── Stats period [1000 ms]
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
esp_err_t tic_capture_init(int gpio_a, int gpio_b, bool loopback_a, bool loopback_b, size_t edges_per_buffer);
esp_err_t tic_capture_start(void);
esp_err_t tic_capture_stop(void);
tic_event_t* tic_capture_get_ready_buffer(size_t *count);
uint32_t tic_capture_get_resolution(void);
void tic_capture_force_swap(void);
```

### tic_test.h

```c
esp_err_t tic_siggen_init_a(int gpio, uint32_t freq_hz, bool loopback);
esp_err_t tic_siggen_init_b(int gpio, uint32_t freq_hz, int32_t delay_ns, bool loopback);
esp_err_t tic_siggen_start(void);
esp_err_t tic_siggen_stop(void);
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
