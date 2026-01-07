# TIC - Time Interval Counter for ESP32-S3

Dual-channel time interval counter using MCPWM capture. Measures signal period and inter-channel delay with 12.5ns resolution (80 MHz timer).

## Features

- **Dual channel capture** - Two independent input channels (A and B)
- **Period statistics** - min/max/mean/stddev for each channel
- **Relative delay** - B-A timing offset with matched-pair algorithm
- **High throughput** - Up to 8192 edges per buffer, ISR-driven double buffering
- **Dual signal generator** - Independent frequencies, configurable relative delay (ns)
- **Auto loopback** - Internal loopback enabled when generator GPIO matches capture GPIO

## Hardware

- ESP32-S3 (uses MCPWM peripheral)
- Input signals: 3.3V logic level, rising edge triggered

Default GPIOs (configurable via menuconfig):
- Capture: A=GPIO4, B=GPIO5
- Signal generator: A=GPIO6, B=GPIO7

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
| `TIC_INPUT_GPIO_A` | 4 | Channel A capture input |
| `TIC_INPUT_GPIO_B` | 5 | Channel B capture input |
| `TIC_SIGGEN_A_FREQ_HZ` | 1000 | Generator A frequency (0=disable) |
| `TIC_SIGGEN_A_GPIO` | 6 | Generator A output |
| `TIC_SIGGEN_B_FREQ_HZ` | 1000 | Generator B frequency (0=disable) |
| `TIC_SIGGEN_B_GPIO` | 7 | Generator B output |
| `TIC_SIGGEN_B_DELAY_NS` | 0 | Generator B relative delay (ns) |
| `TIC_EXPECTED_FREQ_HZ` | 0 | Expected frequency for external inputs |
| `TIC_BUFFER_SIZE` | 16384 | Events per buffer (swap when full) |
| `TIC_STATS_PERIOD_MS` | 1000 | Max time between stats (ms) |

## Output

Single row per buffer, header repeats every 24 rows:

| Prefix | Columns |
|--------|---------|
| `A_`, `B_` | N, Hz, min_us, avg_us, max_us, std_us |
| `D_` | N, min_ns, avg_ns, max_ns, std_ns, missA, missB |

## TODO

- [ ] Compact event struct (currently 8 bytes/event, could be 5 with packed channel+type)
- [ ] USB CDC streaming for raw edge data
- [ ] InfluxDB/MQTT integration

## License

MIT
