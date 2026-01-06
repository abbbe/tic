#!/bin/bash
#
# Multi-device test: 3 interconnected TIC devices
#
# Topology (device.gpio):
#   1.4 ← 2.6    D2 Gen A → D1 Cap A
#   1.5 ← 3.6    D3 Gen A → D1 Cap B
#   1.6 → 2.4    D1 Gen A → D2 Cap A
#   1.7 → 3.4    D1 Gen B → D3 Cap A
#   2.7 → 3.5    D2 Gen B → D3 Cap B
#   3.7 → 2.5    D3 Gen B → D2 Cap B
#
# All devices run at 2kHz, measuring cross-device frequency consistency.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Source .env (required)
if [ ! -f "$SCRIPT_DIR/.env" ]; then
    echo "ERROR: $SCRIPT_DIR/.env not found. Copy .env.sample and configure." >&2
    exit 1
fi
source "$SCRIPT_DIR/.env"

# Validate required variables
for var in TIC1_SERIAL_PORT TIC2_SERIAL_PORT TIC3_SERIAL_PORT IDF_PATH; do
    if [ -z "${!var}" ]; then
        echo "ERROR: $var not set in .env" >&2
        exit 1
    fi
done

# Expand variables in IDF_PATH
eval IDF_PATH="$IDF_PATH"

# Source ESP-IDF
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ERROR: $IDF_PATH/export.sh not found" >&2
    exit 1
fi
source "$IDF_PATH/export.sh"

# Test parameters
FREQ_HZ=2000
SAMPLES=15
SKIP_SAMPLES=3
FREQ_TOLERANCE_PPM=100

# Create timestamped log directory
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="$SCRIPT_DIR/logs/multi_$TIMESTAMP"
mkdir -p "$LOG_DIR"

echo "=== TIC Multi-Device Test ==="
echo "Log directory: $LOG_DIR"
echo "Devices:"
echo "  TIC1: $TIC1_SERIAL_PORT"
echo "  TIC2: $TIC2_SERIAL_PORT"
echo "  TIC3: $TIC3_SERIAL_PORT"
echo ""

cd "$PROJECT_DIR"

# Build directory for multi-device test
BUILD_DIR="build_multi"

# Create sdkconfig for multi-device (no loopback, external wiring)
cat > "$PROJECT_DIR/sdkconfig.multi" << EOF
CONFIG_TIC_INPUT_GPIO_A=4
CONFIG_TIC_INPUT_GPIO_B=5
CONFIG_TIC_SIGGEN_A_GPIO=6
CONFIG_TIC_SIGGEN_B_GPIO=7
CONFIG_TIC_SIGGEN_A_FREQ_HZ=$FREQ_HZ
CONFIG_TIC_SIGGEN_B_FREQ_HZ=$FREQ_HZ
CONFIG_TIC_SIGGEN_B_DELAY_NS=0
CONFIG_TIC_OUTPUT_CSV=y
EOF

echo "=== Building firmware ==="
idf.py -B "$BUILD_DIR" \
    -DIDF_TARGET=esp32s3 \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.multi" \
    build > "$LOG_DIR/build.log" 2>&1
echo "Build complete"

# Flash all devices (same firmware to all)
echo ""
echo "=== Flashing devices ==="
for i in 1 2 3; do
    port_var="TIC${i}_SERIAL_PORT"
    port="${!port_var}"
    echo "  Flashing TIC$i ($port)..."
    if ! idf.py -B "$BUILD_DIR" -p "$port" flash > "$LOG_DIR/flash_tic$i.log" 2>&1; then
        echo "ERROR: Failed to flash TIC$i" >&2
        cat "$LOG_DIR/flash_tic$i.log" >&2
        exit 1
    fi
done
echo "All devices flashed"

# Small delay to let devices settle
sleep 1

# Run multi-device test
echo ""
echo "=== Running multi-device test ==="
PORTS="$TIC1_SERIAL_PORT,$TIC2_SERIAL_PORT,$TIC3_SERIAL_PORT"

"$SCRIPT_DIR/.venv/bin/python3" "$SCRIPT_DIR/tic_multi_reader.py" \
    --ports "$PORTS" \
    --samples $SAMPLES \
    --skip-samples $SKIP_SAMPLES \
    --expected-freq $FREQ_HZ \
    --freq-tolerance-ppm $FREQ_TOLERANCE_PPM \
    --output "$LOG_DIR/raw_data.csv" \
    2>&1 | tee "$LOG_DIR/test_output.log"

EXIT_CODE=${PIPESTATUS[0]}

# Cleanup
rm -f "$PROJECT_DIR/sdkconfig.multi"

# Save test parameters
cat > "$LOG_DIR/params.txt" << EOF
Multi-Device Test Parameters
============================
Date: $(date)
Frequency: $FREQ_HZ Hz
Samples: $SAMPLES (skipping first $SKIP_SAMPLES)
Frequency tolerance: $FREQ_TOLERANCE_PPM ppm

Devices:
  TIC1: $TIC1_SERIAL_PORT
  TIC2: $TIC2_SERIAL_PORT
  TIC3: $TIC3_SERIAL_PORT

Topology:
  1.4 ← 2.6    D2 Gen A → D1 Cap A
  1.5 ← 3.6    D3 Gen A → D1 Cap B
  1.6 → 2.4    D1 Gen A → D2 Cap A
  1.7 → 3.4    D1 Gen B → D3 Cap A
  2.7 → 3.5    D2 Gen B → D3 Cap B
  3.7 → 2.5    D3 Gen B → D2 Cap B

Result: $([ $EXIT_CODE -eq 0 ] && echo "PASS" || echo "FAIL")
EOF

echo ""
echo "=== Test complete ==="
echo "Logs: $LOG_DIR/"

exit $EXIT_CODE
