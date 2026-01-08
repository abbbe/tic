#!/bin/bash
#
# Test: Binary USB CDC output mode
#
# Two interconnected devices at 2 kHz, 100ns delay with binary output enabled.
# Binary data streams over USB CDC (OTG port), serial console remains on UART.
#
# Topology (device.gpio):
#   2.6 → 3.4    D2 Gen A → D3 Cap A
#   2.7 → 3.5    D2 Gen B → D3 Cap B
#   3.6 → 2.4    D3 Gen A → D2 Cap A
#   3.7 → 2.5    D3 Gen B → D2 Cap B
#   + Common GND
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$PROJECT_DIR/.env"

echo "=== Binary Mode Test ==="
echo "Building with --binary flag for USB CDC output"
echo ""

# Build and flash both devices with binary mode
"$PROJECT_DIR/bin/idf" -f 2000 -d 100 --binary build flash tic2 tic3

echo ""
echo "=== Firmware flashed ==="
echo ""
echo "Binary output is on USB CDC (OTG port)."
echo "Serial console (stats) remains on UART."
echo ""
echo "To monitor serial console:"
echo "  bin/idf monitor tic2"
echo "  bin/idf monitor tic3"
echo ""
echo "To read binary CDC output:"
echo "  python3 tools/tic_binary_reader.py -p <CDC_PORT> --test"
echo ""
echo "CDC ports from .env:"
echo "  TIC2_CDC_PORT: ${TIC2_CDC_PORT:-not set}"
echo "  TIC3_CDC_PORT: ${TIC3_CDC_PORT:-not set}"
echo ""
echo "To find CDC ports (after device boots):"
echo "  ls /dev/tty.usbmodem*   # macOS"
echo "  ls /dev/ttyACM*         # Linux"
echo ""
