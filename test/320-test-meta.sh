#!/bin/bash
#
# Meta-test: Random frequency and delay combinations (dual device)
#
# Two interconnected devices:
#   1.6 → 2.4, 1.7 → 2.5, 2.6 → 1.4, 2.7 → 1.5
# Runs 100 tests with random parameters.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/.env"

NUM_TESTS=100

# Test parameter ranges
MIN_FREQ=1500
MAX_FREQ=50000

# Counters
PASS=0
FAIL=0

# Function to generate random number in range [min, max]
rand_range() {
    local min=$1
    local max=$2
    local range=$((max - min + 1))
    local rand=$(od -An -tu4 -N4 /dev/urandom | tr -d ' ')
    echo $((min + rand % range))
}

echo "=== TIC Meta-Test (Dual Device) ==="
echo "Running $NUM_TESTS random test combinations..."
echo ""

for i in $(seq 1 $NUM_TESTS); do
    # Generate random frequency
    FREQ=$(rand_range $MIN_FREQ $MAX_FREQ)

    # Calculate max safe delay (period/4, capped at 10000ns)
    PERIOD_NS=$((1000000000 / FREQ))
    MAX_DELAY=$((PERIOD_NS / 4))
    [ $MAX_DELAY -gt 10000 ] && MAX_DELAY=10000
    [ $MAX_DELAY -lt 0 ] && MAX_DELAY=0

    # Generate random delay
    DELAY=$(rand_range 0 $MAX_DELAY)

    echo "=== Test $i/$NUM_TESTS: freq=${FREQ}Hz delay=${DELAY}ns ==="

    # Build and flash both devices
    "$SCRIPT_DIR/bin/idf" -f $FREQ -d $DELAY build flash tic1 tic2 > /dev/null 2>&1

    # Run validation
    if "$SCRIPT_DIR/.venv/bin/python3" "$SCRIPT_DIR/tic_multi_reader.py" \
        --ports "$TIC1_SERIAL_PORT,$TIC2_SERIAL_PORT" \
        --samples 5 \
        --skip-samples 1 \
        --expected-freq "$FREQ" \
        --freq-tolerance-ppm 1000 \
        > /dev/null 2>&1; then
        echo "  PASS"
        ((PASS++)) || true
    else
        echo "  FAIL"
        ((FAIL++)) || true
    fi
done

echo ""
echo "=== Summary ==="
echo "Passed: $PASS/$NUM_TESTS"
echo "Failed: $FAIL/$NUM_TESTS"
