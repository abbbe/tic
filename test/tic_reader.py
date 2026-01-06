#!/usr/bin/env python3
"""
TIC serial reader - reads CSV output and validates results.

Usage:
    python tic_reader.py --port /dev/ttyUSB0 --duration 5 --expected-freq 1000 --expected-delay 100
"""

import argparse
import serial
import sys
import time
import re
from dataclasses import dataclass
from typing import Optional, List


@dataclass
class TicRow:
    """Parsed CSV row from TIC output."""
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


def parse_csv_line(line: str) -> Optional[TicRow]:
    """Parse a CSV line into a TicRow. Returns None if not a data line."""
    line = line.strip()
    if not line or line.startswith('A_N'):  # Skip header
        return None

    parts = line.split(',')
    if len(parts) != 19:
        return None

    try:
        return TicRow(
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
        return None


def reset_device(ser: serial.Serial) -> None:
    """Reset device using DTR/RTS sequence (ESP32 auto-reset)."""
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.1)
    ser.dtr = True
    time.sleep(0.5)  # Wait for boot


def read_tic_data(port: str, baudrate: int, duration: float) -> List[TicRow]:
    """Read TIC CSV data from serial port for specified duration."""
    rows = []

    with serial.Serial(port, baudrate, timeout=1) as ser:
        reset_device(ser)

        # Flush any boot messages
        time.sleep(1)
        ser.reset_input_buffer()

        start = time.time()
        while time.time() - start < duration:
            if ser.in_waiting:
                try:
                    line = ser.readline().decode('utf-8', errors='ignore')
                    row = parse_csv_line(line)
                    if row:
                        rows.append(row)
                        print(f"  Row: A={row.a_hz:.1f}Hz B={row.b_hz:.1f}Hz delay={row.d_avg_ns:.1f}ns")
                except Exception as e:
                    pass  # Ignore decode errors

    return rows


def validate_results(
    rows: List[TicRow],
    expected_freq_a: float,
    expected_freq_b: float,
    expected_delay_ns: float,
    freq_tolerance_pct: float = 1.0,
    delay_tolerance_ns: float = 50.0,
) -> bool:
    """Validate collected results against expected values."""

    if not rows:
        print("FAIL: No data rows received")
        return False

    # Use average of all rows for validation
    avg_a_hz = sum(r.a_hz for r in rows) / len(rows)
    avg_b_hz = sum(r.b_hz for r in rows) / len(rows)
    avg_delay = sum(r.d_avg_ns for r in rows) / len(rows)

    print(f"\nResults (averaged over {len(rows)} samples):")
    print(f"  Channel A: {avg_a_hz:.2f} Hz (expected {expected_freq_a:.2f} Hz)")
    print(f"  Channel B: {avg_b_hz:.2f} Hz (expected {expected_freq_b:.2f} Hz)")
    print(f"  Delay B-A: {avg_delay:.2f} ns (expected {expected_delay_ns:.2f} ns)")

    ok = True

    # Check frequency A
    freq_a_err = abs(avg_a_hz - expected_freq_a) / expected_freq_a * 100
    if freq_a_err > freq_tolerance_pct:
        print(f"FAIL: Channel A frequency error {freq_a_err:.2f}% > {freq_tolerance_pct}%")
        ok = False

    # Check frequency B
    freq_b_err = abs(avg_b_hz - expected_freq_b) / expected_freq_b * 100
    if freq_b_err > freq_tolerance_pct:
        print(f"FAIL: Channel B frequency error {freq_b_err:.2f}% > {freq_tolerance_pct}%")
        ok = False

    # Check delay
    delay_err = abs(avg_delay - expected_delay_ns)
    if delay_err > delay_tolerance_ns:
        print(f"FAIL: Delay error {delay_err:.2f} ns > {delay_tolerance_ns} ns")
        ok = False

    if ok:
        print("PASS: All values within tolerance")

    return ok


def main():
    parser = argparse.ArgumentParser(description='TIC serial reader and validator')
    parser.add_argument('--port', '-p', default='/dev/ttyUSB0',
                        help='Serial port (default: /dev/ttyUSB0)')
    parser.add_argument('--baudrate', '-b', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--duration', '-d', type=float, default=5.0,
                        help='Read duration in seconds (default: 5)')
    parser.add_argument('--expected-freq-a', type=float, default=1000.0,
                        help='Expected frequency A in Hz (default: 1000)')
    parser.add_argument('--expected-freq-b', type=float, default=1000.0,
                        help='Expected frequency B in Hz (default: 1000)')
    parser.add_argument('--expected-delay', type=float, default=0.0,
                        help='Expected delay B-A in ns (default: 0)')
    parser.add_argument('--freq-tolerance', type=float, default=1.0,
                        help='Frequency tolerance in percent (default: 1.0)')
    parser.add_argument('--delay-tolerance', type=float, default=50.0,
                        help='Delay tolerance in ns (default: 50)')

    args = parser.parse_args()

    print(f"Reading TIC data from {args.port} for {args.duration}s...")
    rows = read_tic_data(args.port, args.baudrate, args.duration)

    ok = validate_results(
        rows,
        args.expected_freq_a,
        args.expected_freq_b,
        args.expected_delay,
        args.freq_tolerance,
        args.delay_tolerance,
    )

    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
