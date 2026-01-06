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
#   + Common GND
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/.env"

# Build and flash all devices
"$SCRIPT_DIR/bin/idf" -f 2000 -d 0 build flash tic1 tic2 tic3

# Validate
echo ""
echo "=== Running validation ==="
"$SCRIPT_DIR/.venv/bin/python3" "$SCRIPT_DIR/tic_multi_reader.py" \
    --ports "$TIC1_SERIAL_PORT,$TIC2_SERIAL_PORT,$TIC3_SERIAL_PORT" \
    --samples 15 \
    --skip-samples 3 \
    --expected-freq 2000 \
    --freq-tolerance-ppm 100

echo ""
echo "=== Test complete ==="
