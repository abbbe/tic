#!/usr/bin/env python3
"""
TIC serial reader - reads CSV output and validates results.

Usage:
    python tic_reader.py --port /dev/ttyUSB0 --duration 5 \
        --expected-freq-a 2000 --expected-freq-b 2000 --expected-delay 100
"""

import argparse
import serial
import sys
import time
import re
from dataclasses import dataclass
from typing import Optional, List, Tuple


@dataclass
class TicRow:
    """Parsed CSV row from TIC output."""
    seq: int
    a_n: int
    a_hz: float
    a_min_us: float
    a_avg_us: float
    a_max_us: float
    a_std_us: float
    b_n: int
    b_hz: float
    b_min_us: float
    b_avg_us: float
    b_max_us: float
    b_std_us: float
    d_n: int
    d_min_ns: float
    d_avg_ns: float
    d_max_ns: float
    d_std_ns: float
    d_miss_a: int
    d_miss_b: int


def parse_csv_line(line: str) -> Tuple[Optional[int], Optional[TicRow]]:
    """Parse a CSV line. Returns (seq, row) or (seq, None) for header or (None, None) for non-CSV."""
    line = line.strip()
    if not line:
        return None, None

    # Must start with CSV<seq><tab>
    match = re.match(r'^CSV(\d+)\t(.*)$', line)
    if not match:
        return None, None

    seq = int(match.group(1))
    rest = match.group(2)

    # Check if header (starts with A_N)
    if rest.startswith('A_N'):
        return seq, None

    parts = rest.split(',')
    if len(parts) != 19:
        return None, None

    try:
        return seq, TicRow(
            seq=seq,
            a_n=int(parts[0]),
            a_hz=float(parts[1]),
            a_min_us=float(parts[2]),
            a_avg_us=float(parts[3]),
            a_max_us=float(parts[4]),
            a_std_us=float(parts[5]),
            b_n=int(parts[6]),
            b_hz=float(parts[7]),
            b_min_us=float(parts[8]),
            b_avg_us=float(parts[9]),
            b_max_us=float(parts[10]),
            b_std_us=float(parts[11]),
            d_n=int(parts[12]),
            d_min_ns=float(parts[13]),
            d_avg_ns=float(parts[14]),
            d_max_ns=float(parts[15]),
            d_std_ns=float(parts[16]),
            d_miss_a=int(parts[17]),
            d_miss_b=int(parts[18]),
        )
    except (ValueError, IndexError):
        return None, None


def reset_device(ser: serial.Serial) -> None:
    """Reset device via RTS (EN) while keeping DTR low (GPIO0 high for normal boot)."""
    ser.dtr = False  # GPIO0 high = normal boot (not download mode)
    ser.rts = True   # EN low = reset
    time.sleep(0.1)
    ser.rts = False  # EN high = run


def wait_for_boot(ser: serial.Serial, timeout: float = 5.0) -> bool:
    """Wait for ESP-IDF boot marker. Returns True if found."""
    start = time.time()
    while time.time() - start < timeout:
        if ser.in_waiting:
            try:
                line = ser.readline().decode('utf-8', errors='ignore')
                if 'ESP-IDF' in line:
                    return True
            except Exception:
                pass
    return False


def read_tic_data(port: str, baudrate: int, duration: float, max_lines: int,
                  quiet: bool = False, raw_log_file: Optional[str] = None) -> Tuple[List[TicRow], List[str], List[str]]:
    """Read TIC CSV data from serial port. Returns (rows, errors, non_csv_lines)."""
    rows = []
    errors = []
    non_csv_lines = []
    raw_lines = []
    expected_seq = 0
    header_seen = False

    with serial.Serial(port, baudrate, timeout=1) as ser:
        reset_device(ser)

        # Wait for boot marker
        boot_timeout = time.time() + 5.0
        boot_found = False
        while time.time() < boot_timeout:
            if ser.in_waiting:
                try:
                    line = ser.readline().decode('utf-8', errors='ignore')
                    raw_lines.append(line)
                    if 'ESP-IDF' in line:
                        boot_found = True
                        break
                except Exception:
                    pass

        if not boot_found:
            errors.append("Timeout waiting for ESP-IDF boot marker")
            if raw_log_file:
                with open(raw_log_file, 'w') as f:
                    f.writelines(raw_lines)
            return rows, errors, non_csv_lines

        start = time.time()
        while True:
            # Check termination conditions
            if duration > 0 and time.time() - start >= duration:
                break
            if max_lines > 0 and len(rows) >= max_lines:
                break

            if ser.in_waiting:
                try:
                    line = ser.readline().decode('utf-8', errors='ignore')
                    raw_lines.append(line)
                    seq, row = parse_csv_line(line)

                    if seq is not None:
                        # Check sequence
                        if seq != expected_seq:
                            errors.append(f"Sequence error: expected {expected_seq}, got {seq}")
                        expected_seq = seq + 1

                        if row is None:
                            # This is the header line
                            header_seen = True
                        else:
                            rows.append(row)
                            if not quiet:
                                print(f"  CSV{seq}: A={row.a_hz:.1f}Hz B={row.b_hz:.1f}Hz "
                                      f"delay={row.d_avg_ns:.1f}ns", file=sys.stderr)
                    elif header_seen:
                        # Non-CSV line after header - capture it
                        stripped = line.strip()
                        if stripped:
                            non_csv_lines.append(stripped)
                except Exception:
                    pass

    # Write raw log if requested
    if raw_log_file:
        with open(raw_log_file, 'w') as f:
            f.writelines(raw_lines)

    return rows, errors, non_csv_lines


def validate_results(
    rows: List[TicRow],
    errors: List[str],
    non_csv_lines: List[str],
    expected_freq_a: float,
    expected_freq_b: float,
    expected_delay_ns: float,
    freq_tolerance_pct: float = 0.0,
    delay_tolerance_ns: float = 12.5,
) -> bool:
    """Validate collected results against expected values."""

    ok = True

    # Report non-CSV output (warnings, errors from device) - any non-CSV line is a failure
    if non_csv_lines:
        print("\nNon-CSV output from device (FAIL):", file=sys.stderr)
        for line in non_csv_lines:
            print(f"  {line}", file=sys.stderr)
        ok = False

    # Report sequence errors
    for err in errors:
        print(f"ERROR: {err}", file=sys.stderr)
        ok = False

    if not rows:
        print("FAIL: No data rows received", file=sys.stderr)
        return False

    # Use average of all rows for validation
    avg_a_hz = sum(r.a_hz for r in rows) / len(rows)
    avg_b_hz = sum(r.b_hz for r in rows) / len(rows)
    avg_delay = sum(r.d_avg_ns for r in rows) / len(rows)

    print(f"\nResults (averaged over {len(rows)} samples):", file=sys.stderr)
    print(f"  Channel A: {avg_a_hz:.2f} Hz (expected {expected_freq_a:.2f} Hz)", file=sys.stderr)
    print(f"  Channel B: {avg_b_hz:.2f} Hz (expected {expected_freq_b:.2f} Hz)", file=sys.stderr)
    print(f"  Delay B-A: {avg_delay:.2f} ns (expected {expected_delay_ns:.2f} ns)", file=sys.stderr)

    # Check frequency A
    if expected_freq_a > 0:
        freq_a_err = abs(avg_a_hz - expected_freq_a) / expected_freq_a * 100
        if freq_a_err > freq_tolerance_pct:
            print(f"FAIL: Channel A frequency error {freq_a_err:.2f}% > {freq_tolerance_pct}%", file=sys.stderr)
            ok = False

    # Check frequency B
    if expected_freq_b > 0:
        freq_b_err = abs(avg_b_hz - expected_freq_b) / expected_freq_b * 100
        if freq_b_err > freq_tolerance_pct:
            print(f"FAIL: Channel B frequency error {freq_b_err:.2f}% > {freq_tolerance_pct}%", file=sys.stderr)
            ok = False

    # Check delay
    delay_err = abs(avg_delay - expected_delay_ns)
    if delay_err > delay_tolerance_ns:
        print(f"FAIL: Delay error {delay_err:.2f} ns > {delay_tolerance_ns} ns", file=sys.stderr)
        ok = False

    if ok:
        print("PASS: All values within tolerance", file=sys.stderr)

    return ok


def main():
    parser = argparse.ArgumentParser(description='TIC serial reader and validator')
    parser.add_argument('--port', '-p', required=True,
                        help='Serial port')
    parser.add_argument('--baudrate', '-b', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--duration', '-d', type=float, default=0,
                        help='Read duration in seconds (default: 0 = unlimited)')
    parser.add_argument('--lines', '-n', type=int, default=0,
                        help='Number of CSV data lines to capture (default: 0 = unlimited)')
    parser.add_argument('--expected-freq-a', type=float, default=1000.0,
                        help='Expected frequency A in Hz (default: 1000)')
    parser.add_argument('--expected-freq-b', type=float, default=1000.0,
                        help='Expected frequency B in Hz (default: 1000)')
    parser.add_argument('--expected-delay', type=float, default=0.0,
                        help='Expected delay B-A in ns (default: 0)')
    parser.add_argument('--freq-tolerance', type=float, default=0.0,
                        help='Frequency tolerance in percent (default: 0)')
    parser.add_argument('--delay-tolerance', type=float, default=12.5,
                        help='Delay tolerance in ns (default: 12.5)')
    parser.add_argument('--quiet', '-q', action='store_true',
                        help='Suppress per-row output')
    parser.add_argument('--raw-log', type=str, default=None,
                        help='File to save raw serial output')
    parser.add_argument('--csv-row', action='store_true',
                        help='Output CSV row to stdout: freq_a,freq_b,delay')

    args = parser.parse_args()

    if args.duration <= 0 and args.lines <= 0:
        parser.error("Must specify --duration or --lines")

    print(f"Reading TIC data from {args.port}...", file=sys.stderr)
    rows, errors, non_csv_lines = read_tic_data(args.port, args.baudrate, args.duration, args.lines, args.quiet, args.raw_log)

    ok = validate_results(
        rows,
        errors,
        non_csv_lines,
        args.expected_freq_a,
        args.expected_freq_b,
        args.expected_delay,
        args.freq_tolerance,
        args.delay_tolerance,
    )

    # Output CSV row if requested
    if args.csv_row and rows:
        avg_a_hz = sum(r.a_hz for r in rows) / len(rows)
        avg_b_hz = sum(r.b_hz for r in rows) / len(rows)
        avg_delay = sum(r.d_avg_ns for r in rows) / len(rows)
        print(f"{avg_a_hz:.2f},{avg_b_hz:.2f},{avg_delay:.2f}")

    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
