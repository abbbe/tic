#!/bin/bash
#
# Test: 2 kHz loopback with 100ns delay
#
# Builds TIC with both signal generators at 2 kHz, 100ns relative delay,
# in loopback mode (gen GPIO = capture GPIO), then validates output.
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
if [ -z "$TIC1_SERIAL_PORT" ]; then
    echo "ERROR: TIC1_SERIAL_PORT not set in .env" >&2
    exit 1
fi
if [ -z "$IDF_PATH" ]; then
    echo "ERROR: IDF_PATH not set in .env" >&2
    exit 1
fi

# Expand variables in IDF_PATH (e.g., $HOME)
eval IDF_PATH="$IDF_PATH"

# Source ESP-IDF
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ERROR: $IDF_PATH/export.sh not found" >&2
    exit 1
fi
source "$IDF_PATH/export.sh"

PORT="$TIC1_SERIAL_PORT"

echo "=== TIC Test: 2kHz loopback with 100ns delay ==="
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
CONFIG_TIC_SIGGEN_A_FREQ_HZ=2000
CONFIG_TIC_SIGGEN_B_FREQ_HZ=2000
CONFIG_TIC_SIGGEN_B_DELAY_NS=100
CONFIG_TIC_OUTPUT_CSV=y
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
    --expected-freq-a 2000 \
    --expected-freq-b 2000 \
    --expected-delay 100 \
    --freq-tolerance 0 \
    --delay-tolerance 12.5

# Cleanup
rm -f sdkconfig.defaults.test

echo ""
echo "=== Test complete ==="
