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

# Build and flash both devices
"$SCRIPT_DIR/bin/idf" -f 2000 -d 0 build flash tic1 tic2

# Validate
echo ""
echo "=== Running validation ==="
"$SCRIPT_DIR/.venv/bin/python3" "$SCRIPT_DIR/tic_multi_reader.py" \
    --ports "$TIC1_SERIAL_PORT,$TIC2_SERIAL_PORT" \
    --samples 15 \
    --skip-samples 3 \
    --expected-freq 2000 \
    --freq-tolerance-ppm 100

echo ""
echo "=== Test complete ==="
