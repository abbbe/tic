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
- Channel A: GPIO4
- Channel B: GPIO5

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
| `TIC_INPUT_GPIO_A` | 4 | Channel A input pin |
| `TIC_INPUT_GPIO_B` | 5 | Channel B input pin |
| `TIC_MAX_BUFFER_SIZE` | 8192 | Max events per buffer (RAM: 2 × N × 8 bytes) |
| `TIC_EDGES_PER_BUFFER` | 8192 | Buffer swap threshold |
| `TIC_STATS_PERIOD_MS` | 1000 | Max time between stats (ms) |
| `TIC_PWM_OUTPUT_ENABLE` | off | Generate PWM on separate pin |

## Output

Single row per buffer, header repeats every 24 rows:

| Prefix | Columns |
|--------|---------|
| `A_`, `B_` | N, Hz, min_us, avg_us, max_us, std_us |
| `D_` | N, min_ns, avg_ns, max_ns, std_ns, missA, missB |

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

## TODO

- [ ] Compact event struct (currently 8 bytes/event, could be 5 with packed channel+type)
- [ ] USB CDC streaming for raw edge data
- [ ] InfluxDB/MQTT integration

## License

MIT
