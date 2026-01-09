# TIC - Time Interval Counter for ESP32

Dual-channel time interval counter and generator based on ESP32 S3/C6 MCPWM.
Measures signal period and inter-channel delay with 12.5/6.25ns resolution.

## Features

- **Dual channel capture** - Two independent input channels (A and B)
- **Period statistics** - min/max/mean/stddev for each channel
- **Relative delay** - B-A timing offset with matched-pair algorithm
- **Dual signal generator** - Independent frequencies, configurable relative delay (ns)
- **Auto loopback** - Internal loopback enabled when generator GPIO matches capture GPIO

## Hardware

- ESP32 S3 or C6 (uses MCPWM peripheral)
- Input signals: 3.3V logic level, rising edge triggered

Default GPIOs (configurable via menuconfig):
- Capture: A=GPIO4, B=GPIO5
- Signal generator: A=GPIO6, B=GPIO7

## Setup

1. Create `.env` from sample:
   ```bash
   cp .env.sample .env
   ```

2. Edit `.env` with your settings:
   ```bash
   # Find your ports
   ls /dev/tty.usb*      # macOS
   ls /dev/ttyUSB*       # Linux

   # Edit .env
   IDF_PATH="$HOME/esp/esp-idf"
   TIC1_SERIAL_PORT=/dev/tty.usbserial-0001
   TIC2_SERIAL_PORT=/dev/tty.usbserial-0002  # for multi-device tests
   TIC3_SERIAL_PORT=/dev/tty.usbserial-0003  # for multi-device tests
   ```

## Build Helper

Use `bin/idf` for building and flashing with test configurations:

```bash
bin/idf --help                          # Show usage
bin/idf build                           # Build normal mode (external wiring)
bin/idf build flash tic1                # Build and flash TIC1
bin/idf -m loopback build tic1          # Build loopback mode
bin/idf -f 5000 -d 100 build            # Custom freq/delay
bin/idf monitor tic1                    # Monitor single device
bin/idf monitor all                     # Monitor all in tmux
```

| Option | Default | Description |
|--------|---------|-------------|
| `-f`, `--freq` | 2000 | Signal generator frequency (Hz) |
| `--freq-b` | same as --freq | Generator B frequency (Hz) |
| `-d`, `--delay` | 0 | Channel B delay (ns) |
| `-m`, `--mode` | normal | Build mode: `normal` or `loopback` |
| `-b`, `--build-dir` | build/<mode> | Custom build directory |
| `--csv` | off | Enable CSV output format |
| `--binary` | off | Enable binary USB CDC output |
| `--no-reset` | off | Don't reset on monitor |

## Configuration

The build system uses a chain of sdkconfig files that are merged in order (later files override earlier):

```
sdkconfig.defaults              # Base config (IDF target)
    ↓
sdkconfig.defaults.<mode>       # GPIO mappings (loopback or normal)
    ↓
sdkconfig.defaults.binary       # Binary mode settings (when --binary)
    ↓
build/<mode>/sdkconfig.<mode>   # Runtime values (freq, delay, CSV flag)
```

### Configuration Files

| File | Contents |
|------|----------|
| `sdkconfig.defaults` | Base: IDF target (esp32s3) |
| `sdkconfig.defaults.loopback` | GPIO 4,5 for both gen and capture |
| `sdkconfig.defaults.normal` | GPIO 4,5 capture, 6,7 generator |
| `sdkconfig.defaults.binary` | TinyUSB CDC, buffer sizes |

### Mode GPIO Mappings

| Mode | Capture A | Capture B | Gen A | Gen B | Wiring |
|------|-----------|-----------|-------|-------|--------|
| `loopback` | GPIO 4 | GPIO 5 | GPIO 4 | GPIO 5 | None (internal) |
| `normal` | GPIO 4 | GPIO 5 | GPIO 6 | GPIO 7 | External wires |

## Custom builds

```bash
idf.py set-target esp32s3
idf.py menuconfig  # Optional: configure GPIOs and modes
idf.py build flash monitor
```

In `idf.py menuconfig` → TIC Configuration:

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

TIC_EXPECTED_FREQ_HZ have to be set, it is used as a hint during inter-channel pulse matching.
In loopback mode (when output matches input) half the generator period is used.

## Output

Single row per buffer, header repeats every 24 rows:

| Prefix | Columns |
|--------|---------|
| `A_`, `B_` | N, Hz, min_us, avg_us, max_us, std_us |
| `D_` | N, min_ns, avg_ns, max_ns, std_ns, missA, missB |

## License

GPLv3
