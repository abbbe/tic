# TIC Test Suite

Automated tests for validating TIC functionality.

## Setup

1. Create Python virtual environment:
   ```bash
   python3 -m venv .venv
   source .venv/bin/activate
   pip install -r requirements.txt
   ```

2. Create `.env` from sample:
   ```bash
   cp .env.sample .env
   ```

3. Edit `.env` with your settings:
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

## Test Organization

| Series | Mode | Wiring | Description |
|--------|------|--------|-------------|
| 1xx | Loopback | None (internal) | Gen GPIO = Capture GPIO |
| 2xx | External | Single device | GPIO 6→4, 7→5 via wires |
| 3xx | Dual-device | 2 devices | Cross-device measurements |
| 4xx | Multi-device | 3 devices | Cross-device measurements |

## Build Helper

Use `bin/idf` for building and flashing:

```bash
./bin/idf --help                          # Show usage
./bin/idf build                           # Build normal mode (external wiring)
./bin/idf build flash tic1                # Build and flash TIC1
./bin/idf -m loopback build tic1          # Build loopback mode
./bin/idf -f 5000 -d 100 build            # Custom freq/delay
./bin/idf monitor tic1                    # Monitor single device
./bin/idf monitor all                     # Monitor all in tmux
```

| Option | Default | Description |
|--------|---------|-------------|
| `-f`, `--freq` | 2000 | Signal generator frequency (Hz) |
| `-d`, `--delay` | 0 | Channel B delay (ns) |
| `-m`, `--mode` | normal | Build mode: `normal` or `loopback` |

## 1xx - Loopback Tests

No external wiring needed. Signal generator output is internally routed to capture input.

### 100-test-f_2kHz.sh

2 kHz loopback, zero delay.

**Configuration:**
- Gen A GPIO = Capture A GPIO = 4 (loopback)
- Gen B GPIO = Capture B GPIO = 5 (loopback)
- Frequency: 2000 Hz
- Delay B-A: 0 ns

**Pass criteria:** Frequency exact, delay within 12.5 ns

### 110-test-f_2kHz-delay_100ns.sh

2 kHz loopback, 100ns delay.

**Configuration:**
- Gen A GPIO = Capture A GPIO = 4 (loopback)
- Gen B GPIO = Capture B GPIO = 5 (loopback)
- Frequency: 2000 Hz
- Delay B-A: 100 ns

**Pass criteria:** Frequency exact, delay within 12.5 ns of 100 ns

### 120-test-meta.sh

Random parameter stress test (100 iterations).

- Frequency range: 1.5-50 kHz
- Delay range: 0 to period/4
- Logs results to `logs/<timestamp>/`

## 2xx - External Wiring Tests (Single Device)

Requires external wires on one device:
```
GPIO 6 ──→ GPIO 4   (Gen A → Capture A)
GPIO 7 ──→ GPIO 5   (Gen B → Capture B)
```

### 200-test-f_2kHz.sh

2 kHz external wiring, zero delay.

**Configuration:**
- Capture A: GPIO 4, Capture B: GPIO 5
- Gen A: GPIO 6, Gen B: GPIO 7
- Frequency: 2000 Hz
- Delay B-A: 0 ns

**Pass criteria:** Frequency exact, delay within 12.5 ns

### 210-test-f_2kHz-delay_100ns.sh

2 kHz external wiring, 100ns delay.

**Configuration:**
- Capture A: GPIO 4, Capture B: GPIO 5
- Gen A: GPIO 6, Gen B: GPIO 7
- Frequency: 2000 Hz
- Delay B-A: 100 ns

**Pass criteria:** Frequency exact, delay within 12.5 ns of 100 ns

## 3xx - Dual-Device Tests

Requires 2 TIC devices with cross-wiring.

### Wiring Topology

```
Device.GPIO notation: D1.4 = Device 1, GPIO 4

  1.6 → 2.4    D1 Gen A → D2 Cap A
  1.7 → 2.5    D1 Gen B → D2 Cap B
  2.6 → 1.4    D2 Gen A → D1 Cap A
  2.7 → 1.5    D2 Gen B → D1 Cap B

+ Common GND between devices
```

Each device measures the other's signal generator output.

### 300-test-dual.sh

Cross-device frequency measurement at 2 kHz, zero delay.

**Configuration:**
- Capture A: GPIO 4, Capture B: GPIO 5
- Gen A: GPIO 6, Gen B: GPIO 7
- Frequency: 2000 Hz
- Delay B-A: 0 ns

**Pass criteria:**
- All frequencies within 100 ppm of expected
- Low delay jitter (stddev)

### 310-test-dual-delay_100ns.sh

Cross-device frequency measurement at 2 kHz, 100ns delay.

**Configuration:**
- Capture A: GPIO 4, Capture B: GPIO 5
- Gen A: GPIO 6, Gen B: GPIO 7
- Frequency: 2000 Hz
- Delay B-A: 100 ns

**Pass criteria:**
- All frequencies within 100 ppm of expected
- Low delay jitter (stddev)

## 4xx - Multi-Device Tests

Requires 3 TIC devices with cross-wiring.

### Wiring Topology

```
Device.GPIO notation: D1.4 = Device 1, GPIO 4

  1.4 ← 2.6    D2 Gen A → D1 Cap A     RED
  1.5 ← 3.6    D3 Gen A → D1 Cap B     GREY
  1.6 → 2.4    D1 Gen A → D2 Cap A     PURPLE
  1.7 → 3.4    D1 Gen B → D3 Cap A     BLUE
  2.7 → 3.5    D2 Gen B → D3 Cap B     GREEN
  3.7 → 2.5    D3 Gen B → D2 Cap B     YELLOW

+ Common GND between all devices
```

### 400-test-multi.sh

Cross-device frequency measurement at 2 kHz.

**Pass criteria:**
- All frequencies within 100 ppm of expected
- Low delay jitter (stddev)

## Python Tools

### tic_reader.py

Single-device serial reader and validator.

```bash
python tic_reader.py --port /dev/ttyUSB0 --duration 5 \
    --expected-freq-a 2000 --expected-freq-b 2000 --expected-delay 100
```

| Option | Default | Description |
|--------|---------|-------------|
| `--port`, `-p` | (required) | Serial port |
| `--baudrate`, `-b` | 115200 | Baud rate |
| `--duration`, `-d` | 0 | Read duration (seconds) |
| `--lines`, `-n` | 0 | Number of CSV lines to capture |
| `--expected-freq-a` | 1000 | Expected frequency A (Hz) |
| `--expected-freq-b` | 1000 | Expected frequency B (Hz) |
| `--expected-delay` | 0 | Expected delay B-A (ns) |
| `--freq-tolerance` | 0 | Frequency tolerance (%) |
| `--delay-tolerance` | 12.5 | Delay tolerance (ns) |
| `--quiet`, `-q` | false | Suppress per-row output |

**Output:**
```
Reading TIC data from /dev/ttyUSB0...
  CSV1: A=2000.0Hz B=2000.0Hz delay=0.0ns
  CSV2: A=2000.0Hz B=2000.0Hz delay=0.0ns

Results (averaged over 2 samples):
  Channel A: 2000.00 Hz (expected 2000.00 Hz)
  Channel B: 2000.00 Hz (expected 2000.00 Hz)
  Delay B-A: 0.00 ns (expected 0.00 ns)
PASS: All values within tolerance
```

### tic_multi_reader.py

Multi-device parallel reader for 3xx and 4xx tests.

```bash
python tic_multi_reader.py --ports /dev/ttyUSB0,/dev/ttyUSB1 \
    --samples 10 --expected-freq 2000
```

| Option | Default | Description |
|--------|---------|-------------|
| `--ports` | (required) | Comma-separated serial ports |
| `--samples` | 10 | Number of CSV samples to collect |
| `--skip-samples` | 3 | Skip initial samples (startup noise) |
| `--expected-freq` | 2000 | Expected frequency (Hz) |
| `--freq-tolerance-ppm` | 100 | Frequency tolerance (ppm) |
| `--output` | (none) | Save raw data to CSV file |

## Exit Codes

- 0: PASS - all values within tolerance
- 1: FAIL - one or more values out of tolerance
