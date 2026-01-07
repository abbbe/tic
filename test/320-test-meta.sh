#!/bin/bash
#
# Meta-test: Random frequency and delay combinations (dual device)
#
# Two interconnected devices (TIC2 and TIC3):
#   2.6 → 3.4, 2.7 → 3.5, 3.6 → 2.4, 3.7 → 2.5
# Runs 100 tests with random parameters, logs all results.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/.env"

NUM_TESTS=100

# Test parameter ranges
MIN_FREQ=1500
MAX_FREQ=24000

# Counters
PASS=0
FAIL=0

# Create timestamped log directory
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="$SCRIPT_DIR/logs/320_$TIMESTAMP"
mkdir -p "$LOG_DIR"

# Create results CSV (one per device)
RESULTS_CSV_D1="$LOG_DIR/results_d1.csv"
RESULTS_CSV_D2="$LOG_DIR/results_d2.csv"
echo "test_num,status,freq_configured_hz,delay_configured_ns,freq_a_measured_hz,freq_b_measured_hz,delay_measured_ns" > "$RESULTS_CSV_D1"
echo "test_num,status,freq_configured_hz,delay_configured_ns,freq_a_measured_hz,freq_b_measured_hz,delay_measured_ns" > "$RESULTS_CSV_D2"

# Function to generate random number in range [min, max] (uniform)
rand_range() {
    local min=$1
    local max=$2
    local range=$((max - min + 1))
    local rand=$(od -An -tu4 -N4 /dev/urandom | tr -d ' ')
    echo $((min + rand % range))
}

# Function to generate random number with logarithmic distribution
rand_log() {
    "$SCRIPT_DIR/.venv/bin/python3" "$SCRIPT_DIR/rand_log.py" "$1" "$2" --int
}

echo "=== TIC Meta-Test (Dual Device) ==="
echo "Log directory: $LOG_DIR"
echo "Running $NUM_TESTS random test combinations..."
echo ""

START_TIME=$(date +%s)

for i in $(seq 1 $NUM_TESTS); do
    # Generate random frequency (logarithmic distribution for even octave coverage)
    FREQ=$(rand_log $MIN_FREQ $MAX_FREQ)

    # Calculate max safe delay (period/4, capped at 10000ns)
    PERIOD_NS=$((1000000000 / FREQ))
    MAX_DELAY=$((PERIOD_NS / 4))
    [ $MAX_DELAY -gt 10000 ] && MAX_DELAY=10000
    [ $MAX_DELAY -lt 0 ] && MAX_DELAY=0

    # Generate random delay
    DELAY=$(rand_range 0 $MAX_DELAY)

    echo "=== Test $i/$NUM_TESTS: freq=${FREQ}Hz delay=${DELAY}ns ==="

    # Build and flash both devices
    echo "  Building & flashing..."
    if ! "$SCRIPT_DIR/bin/idf" -b "$LOG_DIR/build" -f $FREQ -d $DELAY build flash tic2 tic3 > "$LOG_DIR/build_$(printf '%03d' $i).log" 2>&1; then
        echo "  FAIL: Build/flash failed"
        echo "$i,FAIL,$FREQ,$DELAY,,," >> "$RESULTS_CSV_D1"
        echo "$i,FAIL,$FREQ,$DELAY,,," >> "$RESULTS_CSV_D2"
        ((FAIL++)) || true
        continue
    fi

    # Run validation
    echo "  Testing..."
    TEST_LOG="$LOG_DIR/test_$(printf '%03d' $i).log"
    RAW_LOG_PREFIX="$LOG_DIR/raw_$(printf '%03d' $i)"

    # Write test parameters to log
    echo "Test $i: freq=${FREQ}Hz delay=${DELAY}ns" > "$TEST_LOG"

    CSV_ROWS=$("$SCRIPT_DIR/.venv/bin/python3" "$SCRIPT_DIR/tic_multi_reader.py" \
        --ports "$TIC2_SERIAL_PORT,$TIC3_SERIAL_PORT" \
        --samples 100 \
        --skip-samples 1 \
        --expected-freq "$FREQ" \
        --freq-tolerance-ppm 1000 \
        --raw-log-prefix "$RAW_LOG_PREFIX" \
        --csv-rows \
        2>>"$TEST_LOG") && STATUS="PASS" || STATUS="FAIL"

    if [ "$STATUS" = "PASS" ]; then
        ((PASS++)) || true
    else
        ((FAIL++)) || true
    fi

    # Parse CSV rows (one per device): device,freq_a,freq_b,delay
    echo "$CSV_ROWS" | while IFS=',' read -r DEV FREQ_A FREQ_B DELAY_MEASURED; do
        [ -z "$DEV" ] && continue
        if [ "$DEV" = "1" ]; then
            echo "$i,$STATUS,$FREQ,$DELAY,$FREQ_A,$FREQ_B,$DELAY_MEASURED" >> "$RESULTS_CSV_D1"
        elif [ "$DEV" = "2" ]; then
            echo "$i,$STATUS,$FREQ,$DELAY,$FREQ_A,$FREQ_B,$DELAY_MEASURED" >> "$RESULTS_CSV_D2"
        fi
    done

    # Show summary for first device
    FIRST_LINE=$(echo "$CSV_ROWS" | head -1)
    if [ -n "$FIRST_LINE" ]; then
        FIRST_DELAY=$(echo "$FIRST_LINE" | cut -d',' -f4)
        DELAY_ERROR=$(awk "BEGIN {printf \"%.2f\", $FIRST_DELAY - $DELAY}")
        echo "  $STATUS: D1 delay=${FIRST_DELAY}ns (error=${DELAY_ERROR}ns)"
    else
        echo "  $STATUS (no CSV output from reader)"
    fi
done

END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))
DURATION_MIN=$((DURATION / 60))
DURATION_SEC=$((DURATION % 60))

echo ""
echo "=== Summary ==="
echo "Passed: $PASS/$NUM_TESTS"
echo "Failed: $FAIL/$NUM_TESTS"
echo "Duration: ${DURATION_MIN}m ${DURATION_SEC}s"
echo "Results: $RESULTS_CSV_D1"
echo "         $RESULTS_CSV_D2"
