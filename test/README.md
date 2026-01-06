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

3. Edit `.env` with your settings (all fields required):
   ```bash
   # Find your port
   ls /dev/tty.usb*      # macOS
   ls /dev/ttyUSB*       # Linux

   # Edit .env
   TIC1_SERIAL_PORT=/dev/tty.usbserial-0001
   IDF_PATH="$HOME/esp/esp-idf"
   ```

## Running Tests

```bash
source .venv/bin/activate
./100-test-f_2kHz.sh
./110-test-f_2kHz-delay_100ns.sh
```

The scripts automatically source `.env` and ESP-IDF.

## Test Scripts

### 100-test-f_2kHz.sh

Loopback test at 2 kHz with zero delay.

**Configuration:**
- Gen A GPIO = Capture A GPIO (loopback)
- Gen B GPIO = Capture B GPIO (loopback)
- Frequency: 2000 Hz (both channels)
- Delay B-A: 0 ns

**Pass criteria:**
- Frequency exact match
- Delay within 12.5 ns of 0

### 110-test-f_2kHz-delay_100ns.sh

Loopback test at 2 kHz with 100ns delay.

**Configuration:**
- Gen A GPIO = Capture A GPIO (loopback)
- Gen B GPIO = Capture B GPIO (loopback)
- Frequency: 2000 Hz (both channels)
- Delay B-A: 100 ns

**Pass criteria:**
- Frequency exact match
- Delay within 12.5 ns of 100 ns

## tic_reader.py

Serial reader and validator for TIC CSV output.

**Usage:**
```bash
python tic_reader.py --port /dev/ttyUSB0 --duration 5 \
    --expected-freq-a 2000 --expected-freq-b 2000 --expected-delay 100
```

**Options:**
| Option | Default | Description |
|--------|---------|-------------|
| `--port`, `-p` | (required) | Serial port |
| `--baudrate`, `-b` | 115200 | Baud rate |
| `--duration`, `-d` | 5 | Read duration (seconds) |
| `--expected-freq-a` | 1000 | Expected frequency A (Hz) |
| `--expected-freq-b` | 1000 | Expected frequency B (Hz) |
| `--expected-delay` | 0 | Expected delay B-A (ns) |
| `--freq-tolerance` | 0 | Frequency tolerance (%) |
| `--delay-tolerance` | 12.5 | Delay tolerance (ns) |
| `--quiet`, `-q` | false | Suppress per-row output |

**Output:**

Data rows are printed to stderr as they arrive (with CSV sequence number):
```
Reading TIC data from /dev/ttyUSB0...
  CSV1: A=2000.0Hz B=2000.0Hz delay=112.5ns
  CSV2: A=2000.0Hz B=2000.0Hz delay=112.5ns
```

Final validation printed to stdout:
```
Results (averaged over 2 samples):
  Channel A: 2000.00 Hz (expected 2000.00 Hz)
  Channel B: 2000.00 Hz (expected 2000.00 Hz)
  Delay B-A: 112.50 ns (expected 100.00 ns)
PASS: All values within tolerance
```

**Exit codes:**
- 0: PASS - all values within tolerance
- 1: FAIL - one or more values out of tolerance
