#!/bin/bash
#
# Test: 2 kHz with external wiring, zero delay
#
# Single device with external wires: GPIO 6→4, 7→5
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/.env"

# Build and flash (normal mode = external wiring)
"$SCRIPT_DIR/bin/idf" -f 2000 -d 0 build flash tic1

# Validate
echo ""
echo "=== Running validation ==="
"$SCRIPT_DIR/.venv/bin/python3" "$SCRIPT_DIR/tic_reader.py" \
    --port "$TIC1_SERIAL_PORT" \
    --duration 10 \
    --expected-freq-a 2000 \
    --expected-freq-b 2000 \
    --expected-delay 0 \
    --freq-tolerance 0.01 \
    --delay-tolerance 12.5

echo ""
echo "=== Test complete ==="
