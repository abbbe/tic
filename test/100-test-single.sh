#!/bin/bash
#
# Test: 1 kHz loopback with 100ns delay
#
# Builds TIC with both signal generators at 1 kHz, 100ns relative delay,
# in loopback mode (gen GPIO = capture GPIO), then validates output.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Source .env if exists
if [ -f "$SCRIPT_DIR/.env" ]; then
    source "$SCRIPT_DIR/.env"
fi

# Default port if not set
PORT="${TIC_PORT:-/dev/ttyUSB0}"

echo "=== TIC Test: 1kHz loopback with 100ns delay ==="
echo "Project: $PROJECT_DIR"
echo "Port: $PORT"
echo ""

cd "$PROJECT_DIR"

# Build with test configuration
echo "=== Building with test configuration ==="
cat > sdkconfig.defaults.test << 'EOF'
# Loopback mode: gen GPIO = capture GPIO
CONFIG_TIC_INPUT_GPIO_A=4
CONFIG_TIC_INPUT_GPIO_B=5
CONFIG_TIC_SIGGEN_A_GPIO=4
CONFIG_TIC_SIGGEN_B_GPIO=5
CONFIG_TIC_SIGGEN_A_FREQ_HZ=1000
CONFIG_TIC_SIGGEN_B_FREQ_HZ=1000
CONFIG_TIC_SIGGEN_B_DELAY_NS=100
CONFIG_TIC_OUTPUT_CSV=y
CONFIG_TIC_STATS_PERIOD_MS=500
CONFIG_TIC_EDGES_PER_BUFFER=2048
EOF

# Clean and build with test config
rm -f sdkconfig
cp sdkconfig.defaults.test sdkconfig.defaults
idf.py fullclean > /dev/null 2>&1 || true
idf.py set-target esp32s3
idf.py build

echo ""
echo "=== Flashing ==="
idf.py -p "$PORT" flash

echo ""
echo "=== Running validation ==="
python3 "$SCRIPT_DIR/tic_reader.py" \
    --port "$PORT" \
    --duration 10 \
    --expected-freq-a 1000 \
    --expected-freq-b 1000 \
    --expected-delay 100 \
    --freq-tolerance 1.0 \
    --delay-tolerance 50

# Cleanup
rm -f sdkconfig.defaults.test

echo ""
echo "=== Test complete ==="
