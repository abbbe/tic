# TIC - Time Interval Counter for ESP32-S3

Dual-channel time interval counter using MCPWM capture. Measures signal period and inter-channel delay with 12.5ns resolution (80 MHz timer).

## Features

- **Dual channel capture** - Two independent input channels (A and B)
- **Period statistics** - min/max/mean/stddev for each channel
- **Relative delay** - B-A timing offset with matched-pair algorithm
- **High throughput** - Up to 8192 edges per buffer, ISR-driven double buffering
- **Self-test mode** - Internal loopback with configurable test frequency

## Hardware

- ESP32-S3 (uses MCPWM peripheral)
- Input signals: 3.3V logic level, rising edge triggered

Default GPIOs (configurable via menuconfig):
- Channel A: GPIO1
- Channel B: GPIO2

## Build

```bash
idf.py set-target esp32s3
idf.py menuconfig  # Optional: configure GPIOs and modes
idf.py build flash monitor
```

## Configuration

`idf.py menuconfig` → TIC Configuration:

| Option | Default | Description |
|--------|---------|-------------|
| `TIC_LOOPBACK_TEST_MODE` | off | Internal test signal on Channel A |
| `TIC_INPUT_GPIO_A` | 1 | Channel A input pin |
| `TIC_INPUT_GPIO_B` | 2 | Channel B input pin |
| `TIC_EDGES_PER_BUFFER` | 8192 | Buffer swap threshold |
| `TIC_STATS_PERIOD_MS` | 1000 | Max time between stats (ms) |
| `TIC_PWM_OUTPUT_ENABLE` | off | Generate PWM on separate pin |

## Output

```
=== TIC Statistics ===
Ch A: 4096 edges, 1000.000 Hz, period [999.988..1000.012..1000.037] us, std=0.008 us
Ch B: 4096 edges, 1000.000 Hz, period [999.991..1000.011..1000.031] us, std=0.007 us
Delay B-A: 4095 pairs, mean=50.125 ns, std=2.341 ns, [45.000..55.250]
```

Statistics per channel:
- Edge count, frequency (Hz)
- Period: [min..mean..max] in microseconds
- Standard deviation

Delay statistics (B - A):
- Matched pairs count
- Mean delay with stddev
- Min/max range
- Missed pulses (unmatched edges)

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

## License

MIT
