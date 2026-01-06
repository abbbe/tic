#!/bin/bash
#
# Test: Two interconnected devices at 2 kHz
#
# Topology (device.gpio):
#   1.6 → 2.4    D1 Gen A → D2 Cap A
#   1.7 → 2.5    D1 Gen B → D2 Cap B
#   2.6 → 1.4    D2 Gen A → D1 Cap A
#   2.7 → 1.5    D2 Gen B → D1 Cap B
#   + Common GND
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/.env"

# Test parameters
FREQ_HZ=2000
SAMPLES=15
SKIP_SAMPLES=3
FREQ_TOLERANCE_PPM=100

# Create timestamped log directory
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="$SCRIPT_DIR/logs/dual_$TIMESTAMP"
mkdir -p "$LOG_DIR"

echo "=== TIC Dual-Device Test ==="
echo "Log directory: $LOG_DIR"

# Build and flash both devices
"$SCRIPT_DIR/bin/idf" -f $FREQ_HZ -d 0 build flash tic1 tic2 2>&1 | tee "$LOG_DIR/build_flash.log"

# Small delay to let devices settle
sleep 1

# Run dual-device test
echo ""
echo "=== Running dual-device test ==="
PORTS="$TIC1_SERIAL_PORT,$TIC2_SERIAL_PORT"

"$SCRIPT_DIR/.venv/bin/python3" "$SCRIPT_DIR/tic_multi_reader.py" \
    --ports "$PORTS" \
    --samples $SAMPLES \
    --skip-samples $SKIP_SAMPLES \
    --expected-freq $FREQ_HZ \
    --freq-tolerance-ppm $FREQ_TOLERANCE_PPM \
    --output "$LOG_DIR/raw_data.csv" \
    2>&1 | tee "$LOG_DIR/test_output.log"

EXIT_CODE=${PIPESTATUS[0]}

echo ""
echo "=== Test complete ==="
echo "Logs: $LOG_DIR/"

exit $EXIT_CODE
