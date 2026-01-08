#!/bin/bash
#
# Run all non-meta test cases
#
# Runs: 1xx, 2xx, 3xx, 4xx series (excludes *-meta.sh and 9xx)
# Exits with non-zero if any test fails.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "========================================"
echo "TIC Test Suite - All Non-Meta Tests"
echo "========================================"
echo ""

TESTS=(
    "100-test-f_2kHz.sh"
    "110-test-f_2kHz-delay_100ns.sh"
    "200-test-f_2kHz.sh"
    "210-test-f_2kHz-delay_100ns.sh"
    "300-test-dual.sh"
    "310-test-dual-delay_100ns.sh"
    "400-test-async.sh"
)

for test in "${TESTS[@]}"; do
    echo "========================================"
    echo "Running: $test"
    echo "========================================"

    "$SCRIPT_DIR/$test"

    echo ""
done

echo "========================================"
echo "All tests passed!"
echo "========================================"
