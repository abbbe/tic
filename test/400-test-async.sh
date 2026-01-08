#!/bin/bash
#
# Test: Two devices with async wiring at 2 kHz
#
# Topology (device.gpio):
#  4.4 ← 4.6    D4 Gen A → D4 Cap A (loopback)
#  5.4 ← 5.6    D5 Gen A → D5 Cap B (loopback)
#  5.5 ← 4.7    D4 Gen B → D5 Cap B (cross)
#  4.5 ← 5.7    D5 Gen B → D4 Cap B (cross)
#   + Common GND
#
# Each device sees one channel from its own generator (loopback)
# and one channel from the peer's generator (cross).
# Delay is arbitrary and will drift (different XOs).
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$PROJECT_DIR/.env"

# Build and flash both devices
"$PROJECT_DIR/bin/idf" -f 2000 -d 0 --csv build flash tic4 tic5

# Validate
echo ""
echo "=== Running validation ==="
"$PROJECT_DIR/.venv/bin/python3" "$SCRIPT_DIR/tic_multi_reader.py" \
    --ports "$TIC4_SERIAL_PORT,$TIC5_SERIAL_PORT" \
    --samples 15 \
    --skip-samples 3 \
    --expected-freq 2000 \
    --freq-tolerance-ppm 100

echo ""
echo "=== Test complete ==="
