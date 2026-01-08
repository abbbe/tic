#!/bin/bash
#
# Meta-test: Random frequency and delay combinations (external wiring)
#
# Single device with external wires: GPIO 6→4, 7→5
# Runs 100 tests with random parameters, logs all results.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
source "$PROJECT_DIR/.env"

PORT="$TIC1_SERIAL_PORT"
NUM_TESTS=100

# Test parameter ranges
MIN_FREQ=1500
MAX_FREQ=24000

# Counters
PASS=0
FAIL=0

# Create timestamped log directory
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="$SCRIPT_DIR/logs/220_$TIMESTAMP"
mkdir -p "$LOG_DIR"

# Create results CSV
RESULTS_CSV="$LOG_DIR/results.csv"
echo "test_num,status,freq_configured_hz,delay_configured_ns,freq_a_measured_hz,freq_b_measured_hz,delay_measured_ns" > "$RESULTS_CSV"

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
    "$PROJECT_DIR/.venv/bin/python3" "$SCRIPT_DIR/rand_log.py" "$1" "$2" --int
}

echo "=== TIC Meta-Test (External Wiring) ==="
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

    # Build and flash
    echo "  Building & flashing..."
    if ! "$PROJECT_DIR/bin/idf" -b "$LOG_DIR/build" -f $FREQ -d $DELAY --csv build flash tic1 > "$LOG_DIR/build_$(printf '%03d' $i).log" 2>&1; then
        echo "  FAIL: Build/flash failed"
        echo "$i,FAIL,$FREQ,$DELAY,,," >> "$RESULTS_CSV"
        ((FAIL++)) || true
        continue
    fi

    # Run validation
    echo "  Testing..."
    TEST_LOG="$LOG_DIR/test_$(printf '%03d' $i).log"
    RAW_LOG="$LOG_DIR/raw_$(printf '%03d' $i).log"

    # Write test parameters to log
    echo "Test $i: freq=${FREQ}Hz delay=${DELAY}ns" > "$TEST_LOG"

    CSV_ROW=$("$PROJECT_DIR/.venv/bin/python3" "$SCRIPT_DIR/tic_reader.py" \
        --port "$PORT" \
        --lines 100 \
        --expected-freq-a "$FREQ" \
        --expected-freq-b "$FREQ" \
        --expected-delay "$DELAY" \
        --freq-tolerance 0.1 \
        --delay-tolerance 12.5 \
        --raw-log "$RAW_LOG" \
        --csv-row \
        2>>"$TEST_LOG") && STATUS="PASS" || STATUS="FAIL"

    if [ "$STATUS" = "PASS" ]; then
        ((PASS++)) || true
    else
        ((FAIL++)) || true
    fi

    # Parse CSV row: freq_a,freq_b,delay
    IFS=',' read -r FREQ_A FREQ_B DELAY_MEASURED <<< "$CSV_ROW"

    if [ -n "$DELAY_MEASURED" ]; then
        DELAY_ERROR=$(awk "BEGIN {printf \"%.2f\", $DELAY_MEASURED - $DELAY}")
        echo "  $STATUS: measured delay=${DELAY_MEASURED}ns (error=${DELAY_ERROR}ns)"
    else
        echo "  $STATUS: no measurements"
    fi

    echo "$i,$STATUS,$FREQ,$DELAY,$FREQ_A,$FREQ_B,$DELAY_MEASURED" >> "$RESULTS_CSV"
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
echo "Results: $RESULTS_CSV"
