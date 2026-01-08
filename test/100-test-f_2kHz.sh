#!/bin/bash
#
# Test: 2 kHz loopback with zero delay
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$PROJECT_DIR/.env"

# Build and flash
"$PROJECT_DIR/bin/idf" -m loopback -f 2000 -d 0 --csv build flash tic1

# Validate
echo ""
echo "=== Running validation ==="
"$PROJECT_DIR/.venv/bin/python3" "$SCRIPT_DIR/tic_reader.py" \
    --port "$TIC1_SERIAL_PORT" \
    --duration 10 \
    --expected-freq-a 2000 \
    --expected-freq-b 2000 \
    --expected-delay 0 \
    --freq-tolerance 0.01 \
    --delay-tolerance 12.5

echo ""
echo "=== Test complete ==="
