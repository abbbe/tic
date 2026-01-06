#!/bin/bash
#
# Meta-test: Random frequency and delay combinations
#
# Runs 100 tests with random parameters, logs all results for analysis.
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

# Expand variables in IDF_PATH
eval IDF_PATH="$IDF_PATH"

# Source ESP-IDF
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ERROR: $IDF_PATH/export.sh not found" >&2
    exit 1
fi
source "$IDF_PATH/export.sh"

PORT="$TIC1_SERIAL_PORT"
NUM_TESTS=100

# Create timestamped log directory
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="$SCRIPT_DIR/logs/$TIMESTAMP"
mkdir -p "$LOG_DIR"

echo "=== TIC Meta-Test ==="
echo "Log directory: $LOG_DIR"
echo "Running $NUM_TESTS random test combinations..."
echo ""

# Test parameter ranges
# Frequency: 1500 Hz to 50000 Hz (safe range for 80MHz timer with 16-bit counter)
MIN_FREQ=1500
MAX_FREQ=50000

# Counters
PASS=0
FAIL=0

cd "$PROJECT_DIR"

# Create results CSV with header
RESULTS_CSV="$LOG_DIR/results.csv"
echo "test_num,status,fail_stage,freq_configured_hz,delay_configured_ns,freq_a_measured_hz,freq_b_measured_hz,delay_measured_ns,delay_error_ns,samples" > "$RESULTS_CSV"

# Function to generate random number in range [min, max]
rand_range() {
    local min=$1
    local max=$2
    local range=$((max - min + 1))
    # Use /dev/urandom for better randomness and larger range
    local rand=$(od -An -tu4 -N4 /dev/urandom | tr -d ' ')
    echo $((min + rand % range))
}

# Start time
START_TIME=$(date +%s)

for i in $(seq 1 $NUM_TESTS); do
    # Generate random frequency
    FREQ=$(rand_range $MIN_FREQ $MAX_FREQ)

    # Calculate period in ns and max safe delay
    PERIOD_NS=$((1000000000 / FREQ))
    # Max delay is period/4 (quarter-period matching window in tic_stats.c)
    # Also cap at 10000ns to keep tests reasonable
    MAX_DELAY=$((PERIOD_NS / 4))
    if [ $MAX_DELAY -gt 10000 ]; then
        MAX_DELAY=10000
    fi
    if [ $MAX_DELAY -lt 0 ]; then
        MAX_DELAY=0
    fi

    # Generate random delay (including 0)
    DELAY=$(rand_range 0 $MAX_DELAY)

    echo "=== Test $i/$NUM_TESTS: freq=${FREQ}Hz delay=${DELAY}ns ==="

    # Create test config
    cat > "$PROJECT_DIR/sdkconfig.test" << EOF
CONFIG_TIC_INPUT_GPIO_A=4
CONFIG_TIC_INPUT_GPIO_B=5
CONFIG_TIC_SIGGEN_A_GPIO=4
CONFIG_TIC_SIGGEN_B_GPIO=5
CONFIG_TIC_SIGGEN_A_FREQ_HZ=$FREQ
CONFIG_TIC_SIGGEN_B_FREQ_HZ=$FREQ
CONFIG_TIC_SIGGEN_B_DELAY_NS=$DELAY
CONFIG_TIC_OUTPUT_CSV=y
EOF

    # Build
    echo "  Building..."
    if ! idf.py -B build_test \
        -DIDF_TARGET=esp32s3 \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.test" \
        build > "$LOG_DIR/build_$(printf '%03d' $i).log" 2>&1; then
        echo "  FAIL: Build failed"
        echo "$i,FAIL,BUILD,$FREQ,$DELAY,,,," >> "$RESULTS_CSV"
        ((FAIL++)) || true
        continue
    fi

    # Flash
    echo "  Flashing..."
    if ! idf.py -B build_test -p "$PORT" flash > "$LOG_DIR/flash_$(printf '%03d' $i).log" 2>&1; then
        echo "  FAIL: Flash failed"
        echo "$i,FAIL,FLASH,$FREQ,$DELAY,,,," >> "$RESULTS_CSV"
        ((FAIL++)) || true
        continue
    fi

    # Run test and capture output
    echo "  Testing..."
    TEST_LOG="$LOG_DIR/test_$(printf '%03d' $i).log"

    # Run tic_reader.py with venv python
    if "$SCRIPT_DIR/.venv/bin/python3" "$SCRIPT_DIR/tic_reader.py" \
        --port "$PORT" \
        --lines 5 \
        --expected-freq-a "$FREQ" \
        --expected-freq-b "$FREQ" \
        --expected-delay "$DELAY" \
        --freq-tolerance 0.1 \
        --delay-tolerance 12.5 \
        > "$TEST_LOG" 2>&1; then
        STATUS="PASS"
        ((PASS++)) || true
    else
        STATUS="FAIL"
        ((FAIL++)) || true
    fi

    # Parse the test output to extract measured values
    # Looking for: "Channel A: 2000.00 Hz", "Channel B: 2000.00 Hz", "Delay B-A: 100.00 ns"
    FREQ_A=$(grep "Channel A:" "$TEST_LOG" | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "")
    FREQ_B=$(grep "Channel B:" "$TEST_LOG" | grep -oE '[0-9]+\.[0-9]+' | head -1 || echo "")
    DELAY_MEASURED=$(grep "Delay B-A:" "$TEST_LOG" | grep -oE '[-]?[0-9]+\.[0-9]+' | head -1 || echo "")
    SAMPLES=$(grep "averaged over" "$TEST_LOG" | grep -oE '[0-9]+' | head -1 || echo "0")

    # Calculate delay error
    if [ -n "$DELAY_MEASURED" ]; then
        # Use awk for floating point math
        DELAY_ERROR=$(awk "BEGIN {printf \"%.2f\", $DELAY_MEASURED - $DELAY}")
    else
        DELAY_ERROR=""
    fi

    echo "  $STATUS: measured delay=${DELAY_MEASURED}ns (error=${DELAY_ERROR}ns)"

    # Write to CSV
    echo "$i,$STATUS,TEST,$FREQ,$DELAY,$FREQ_A,$FREQ_B,$DELAY_MEASURED,$DELAY_ERROR,$SAMPLES" >> "$RESULTS_CSV"
done

# Cleanup
rm -f "$PROJECT_DIR/sdkconfig.test"

# End time
END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))
DURATION_MIN=$((DURATION / 60))
DURATION_SEC=$((DURATION % 60))

# Summary
echo ""
echo "=== Summary ==="
echo "Passed: $PASS/$NUM_TESTS"
echo "Failed: $FAIL/$NUM_TESTS"
echo "Duration: ${DURATION_MIN}m ${DURATION_SEC}s"
echo "Results: $RESULTS_CSV"
echo "Logs: $LOG_DIR/"

# Write summary file
cat > "$LOG_DIR/summary.txt" << EOF
TIC Meta-Test Summary
=====================
Date: $(date)
Duration: ${DURATION_MIN}m ${DURATION_SEC}s

Results: $PASS passed, $FAIL failed out of $NUM_TESTS tests

Parameters:
- Frequency range: ${MIN_FREQ}-${MAX_FREQ} Hz
- Max delay: period/4 (capped at 10000 ns) - limited by quarter-period matching window
- Frequency tolerance: 0.1%
- Delay tolerance: 12.5 ns
- Samples per test: 5

Files:
- results.csv: Full results for analysis/plotting
- test_NNN.log: Individual test outputs
- build_NNN.log: Build logs
- flash_NNN.log: Flash logs
EOF

# Also create a quick stats summary
echo "" >> "$LOG_DIR/summary.txt"
echo "Delay Error Statistics:" >> "$LOG_DIR/summary.txt"
awk -F',' 'NR>1 && $9!="" {
    sum+=$9; sumsq+=$9*$9; n++;
    if(NR==2 || $9<min) min=$9;
    if(NR==2 || $9>max) max=$9
} END {
    if(n>0) {
        mean=sum/n;
        stddev=sqrt(sumsq/n - mean*mean);
        printf "  Count: %d\n  Min: %.2f ns\n  Max: %.2f ns\n  Mean: %.2f ns\n  StdDev: %.2f ns\n", n, min, max, mean, stddev
    }
}' "$RESULTS_CSV" >> "$LOG_DIR/summary.txt"

cat "$LOG_DIR/summary.txt"

echo ""
echo "=== Test complete ==="
