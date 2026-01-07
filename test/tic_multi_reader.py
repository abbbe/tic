#!/usr/bin/env python3
"""
TIC multi-device reader - reads CSV output from multiple TIC devices in parallel.

Usage:
    python tic_multi_reader.py --ports /dev/ttyUSB0,/dev/ttyUSB1,/dev/ttyUSB2 \
        --duration 30 --expected-freq 2000 --skip-samples 3
"""

import argparse
import serial
import sys
import time
import re
import threading
import queue
from dataclasses import dataclass, field
from typing import Optional, List, Dict, Tuple
from collections import defaultdict


@dataclass
class TicRow:
    """Parsed CSV row from TIC output."""
    device_id: int
    seq: int
    timestamp: float  # When we received this row
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


def parse_csv_line(line: str, device_id: int, timestamp: float) -> Tuple[Optional[int], Optional[TicRow]]:
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
            device_id=device_id,
            seq=seq,
            timestamp=timestamp,
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


def device_reader(
    device_id: int,
    port: str,
    baudrate: int,
    data_queue: queue.Queue,
    stop_event: threading.Event,
    boot_event: threading.Event,
    quiet: bool = False,
    raw_log_file: Optional[str] = None
):
    """Thread function to read from a single device."""
    raw_lines = []
    try:
        with serial.Serial(port, baudrate, timeout=1) as ser:
            reset_device(ser)

            # Wait for boot marker
            boot_found = False
            boot_timeout = time.time() + 10.0
            while time.time() < boot_timeout and not stop_event.is_set():
                if ser.in_waiting:
                    try:
                        line = ser.readline().decode('utf-8', errors='ignore')
                        raw_lines.append(line)
                        if 'ESP-IDF' in line:
                            boot_found = True
                            boot_event.set()
                            if not quiet:
                                print(f"  D{device_id}: Boot detected", file=sys.stderr)
                            break
                    except Exception:
                        pass

            if not boot_found:
                data_queue.put(('error', device_id, "Boot timeout"))
                if raw_log_file:
                    with open(raw_log_file, 'w') as f:
                        f.writelines(raw_lines)
                return

            # Read CSV data
            expected_seq = 0
            header_seen = False
            while not stop_event.is_set():
                if ser.in_waiting:
                    try:
                        line = ser.readline().decode('utf-8', errors='ignore')
                        raw_lines.append(line)
                        timestamp = time.time()
                        seq, row = parse_csv_line(line, device_id, timestamp)

                        if seq is not None:
                            if seq != expected_seq:
                                data_queue.put(('seq_error', device_id, f"Expected {expected_seq}, got {seq}"))
                            expected_seq = seq + 1

                            if row is None:
                                # This is the header line
                                header_seen = True
                            else:
                                data_queue.put(('row', device_id, row))
                                if not quiet:
                                    print(f"  D{device_id} CSV{seq}: A={row.a_hz:.1f}Hz B={row.b_hz:.1f}Hz "
                                          f"delay={row.d_avg_ns:.1f}ns miss={row.d_miss_a}/{row.d_miss_b}",
                                          file=sys.stderr)
                        elif header_seen:
                            # Non-CSV line after header - capture it
                            stripped = line.strip()
                            if stripped:
                                data_queue.put(('non_csv', device_id, stripped))
                    except Exception as e:
                        data_queue.put(('error', device_id, str(e)))

    except Exception as e:
        data_queue.put(('error', device_id, f"Serial error: {e}"))
    finally:
        if raw_log_file:
            with open(raw_log_file, 'w') as f:
                f.writelines(raw_lines)


def collect_data(
    ports: List[str],
    baudrate: int,
    duration: float,
    max_samples: int,
    quiet: bool = False,
    raw_log_prefix: Optional[str] = None
) -> Tuple[Dict[int, List[TicRow]], List[str], Dict[int, List[str]]]:
    """Collect data from multiple devices. Returns (device_rows, errors, non_csv_lines)."""

    device_rows: Dict[int, List[TicRow]] = defaultdict(list)
    errors: List[str] = []
    non_csv_lines: Dict[int, List[str]] = defaultdict(list)

    data_queue: queue.Queue = queue.Queue()
    stop_event = threading.Event()
    boot_events = [threading.Event() for _ in ports]

    # Start reader threads
    threads = []
    for i, port in enumerate(ports):
        raw_log_file = f"{raw_log_prefix}_d{i+1}.log" if raw_log_prefix else None
        t = threading.Thread(
            target=device_reader,
            args=(i + 1, port, baudrate, data_queue, stop_event, boot_events[i], quiet, raw_log_file)
        )
        t.daemon = True
        t.start()
        threads.append(t)

    # Wait for all devices to boot (with timeout)
    if not quiet:
        print("Waiting for all devices to boot...", file=sys.stderr)

    boot_timeout = time.time() + 15.0
    while time.time() < boot_timeout:
        if all(e.is_set() for e in boot_events):
            break
        time.sleep(0.1)

    if not all(e.is_set() for e in boot_events):
        missing = [i + 1 for i, e in enumerate(boot_events) if not e.is_set()]
        errors.append(f"Boot timeout for devices: {missing}")
        stop_event.set()
        for t in threads:
            t.join(timeout=2)
        return device_rows, errors, dict(non_csv_lines)

    if not quiet:
        print("All devices booted, collecting data...", file=sys.stderr)

    # Collect data
    start_time = time.time()
    min_samples = 0

    while True:
        # Check termination conditions
        elapsed = time.time() - start_time
        if duration > 0 and elapsed >= duration:
            break

        min_samples = min(len(rows) for rows in device_rows.values()) if device_rows else 0
        if max_samples > 0 and min_samples >= max_samples:
            break

        # Process queue
        try:
            msg_type, device_id, data = data_queue.get(timeout=0.5)

            if msg_type == 'row':
                device_rows[device_id].append(data)
            elif msg_type == 'error':
                errors.append(f"D{device_id}: {data}")
            elif msg_type == 'seq_error':
                errors.append(f"D{device_id} seq: {data}")
            elif msg_type == 'non_csv':
                non_csv_lines[device_id].append(data)

        except queue.Empty:
            pass

    # Stop threads
    stop_event.set()
    for t in threads:
        t.join(timeout=2)

    # Drain remaining queue
    while not data_queue.empty():
        try:
            msg_type, device_id, data = data_queue.get_nowait()
            if msg_type == 'row':
                device_rows[device_id].append(data)
            elif msg_type == 'non_csv':
                non_csv_lines[device_id].append(data)
        except queue.Empty:
            break

    return dict(device_rows), errors, dict(non_csv_lines)


def analyze_results(
    device_rows: Dict[int, List[TicRow]],
    errors: List[str],
    non_csv_lines: Dict[int, List[str]],
    expected_freq: float,
    skip_samples: int,
    freq_tolerance_ppm: float
) -> bool:
    """Analyze and validate collected results."""

    ok = True

    # Report non-CSV output (warnings, errors from devices) - any non-CSV line is a failure
    if non_csv_lines:
        print("\nNon-CSV output from devices (FAIL):", file=sys.stderr)
        for device_id in sorted(non_csv_lines.keys()):
            for line in non_csv_lines[device_id]:
                print(f"  D{device_id}: {line}", file=sys.stderr)
        ok = False

    # Report errors
    for err in errors:
        print(f"ERROR: {err}", file=sys.stderr)
        if "Boot timeout" in err:
            ok = False

    if not device_rows:
        print("FAIL: No data collected", file=sys.stderr)
        return False

    print(f"\n{'='*60}", file=sys.stderr)
    print("MULTI-DEVICE TEST RESULTS", file=sys.stderr)
    print(f"{'='*60}", file=sys.stderr)

    # Per-device analysis
    all_freqs_a = []
    all_freqs_b = []
    all_delays = []
    all_delay_stds = []

    for device_id in sorted(device_rows.keys()):
        rows = device_rows[device_id]

        # Skip initial samples (may have missed edges due to startup timing)
        if len(rows) <= skip_samples:
            print(f"\nDevice {device_id}: Only {len(rows)} samples, need > {skip_samples}", file=sys.stderr)
            ok = False
            continue

        valid_rows = rows[skip_samples:]

        # Calculate statistics
        freqs_a = [r.a_hz for r in valid_rows]
        freqs_b = [r.b_hz for r in valid_rows]
        delays = [r.d_avg_ns for r in valid_rows]
        delay_stds = [r.d_std_ns for r in valid_rows]
        miss_a = [r.d_miss_a for r in valid_rows]
        miss_b = [r.d_miss_b for r in valid_rows]

        avg_freq_a = sum(freqs_a) / len(freqs_a)
        avg_freq_b = sum(freqs_b) / len(freqs_b)
        avg_delay = sum(delays) / len(delays)
        avg_delay_std = sum(delay_stds) / len(delay_stds)
        total_miss_a = sum(miss_a)
        total_miss_b = sum(miss_b)

        all_freqs_a.append(avg_freq_a)
        all_freqs_b.append(avg_freq_b)
        all_delays.append(avg_delay)
        all_delay_stds.append(avg_delay_std)

        freq_a_err_ppm = abs(avg_freq_a - expected_freq) / expected_freq * 1e6
        freq_b_err_ppm = abs(avg_freq_b - expected_freq) / expected_freq * 1e6

        print(f"\nDevice {device_id} ({len(valid_rows)} samples after skipping {skip_samples}):", file=sys.stderr)
        print(f"  Channel A: {avg_freq_a:.4f} Hz (error: {freq_a_err_ppm:.1f} ppm)", file=sys.stderr)
        print(f"  Channel B: {avg_freq_b:.4f} Hz (error: {freq_b_err_ppm:.1f} ppm)", file=sys.stderr)
        print(f"  Delay Mean: {avg_delay:.2f} ns", file=sys.stderr)
        print(f"  Delay StdDev: {avg_delay_std:.2f} ns (avg)", file=sys.stderr)
        print(f"  Missed edges: A={total_miss_a} B={total_miss_b}", file=sys.stderr)

        if freq_a_err_ppm > freq_tolerance_ppm:
            print(f"  FAIL: Channel A frequency error {freq_a_err_ppm:.1f} ppm > {freq_tolerance_ppm} ppm", file=sys.stderr)
            ok = False
        if freq_b_err_ppm > freq_tolerance_ppm:
            print(f"  FAIL: Channel B frequency error {freq_b_err_ppm:.1f} ppm > {freq_tolerance_ppm} ppm", file=sys.stderr)
            ok = False

    # Cross-device analysis
    if len(all_freqs_a) >= 2:
        print(f"\n{'='*60}", file=sys.stderr)
        print("CROSS-DEVICE COMPARISON", file=sys.stderr)
        print(f"{'='*60}", file=sys.stderr)

        # Frequency spread across devices
        freq_spread_a = max(all_freqs_a) - min(all_freqs_a)
        freq_spread_b = max(all_freqs_b) - min(all_freqs_b)
        freq_spread_a_ppm = freq_spread_a / expected_freq * 1e6
        freq_spread_b_ppm = freq_spread_b / expected_freq * 1e6

        print(f"\nFrequency spread across devices:", file=sys.stderr)
        print(f"  Channel A: {freq_spread_a:.4f} Hz ({freq_spread_a_ppm:.1f} ppm)", file=sys.stderr)
        print(f"  Channel B: {freq_spread_b:.4f} Hz ({freq_spread_b_ppm:.1f} ppm)", file=sys.stderr)

        print(f"\nDelay StdDev (jitter) per device:", file=sys.stderr)
        for i, std in enumerate(all_delay_stds):
            print(f"  Device {i+1}: {std:.2f} ns", file=sys.stderr)

    print(f"\n{'='*60}", file=sys.stderr)
    if ok:
        print("PASS: All values within tolerance", file=sys.stderr)
    else:
        print("FAIL: One or more values out of tolerance", file=sys.stderr)

    return ok


def main():
    parser = argparse.ArgumentParser(description='TIC multi-device reader and validator')
    parser.add_argument('--ports', '-p', required=True,
                        help='Comma-separated list of serial ports')
    parser.add_argument('--baudrate', '-b', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--duration', '-d', type=float, default=0,
                        help='Read duration in seconds (default: 0 = use --samples)')
    parser.add_argument('--samples', '-n', type=int, default=10,
                        help='Number of CSV samples per device (default: 10)')
    parser.add_argument('--expected-freq', type=float, default=2000.0,
                        help='Expected frequency in Hz (default: 2000)')
    parser.add_argument('--freq-tolerance-ppm', type=float, default=100.0,
                        help='Frequency tolerance in ppm (default: 100)')
    parser.add_argument('--skip-samples', type=int, default=3,
                        help='Skip first N samples (startup transients, default: 3)')
    parser.add_argument('--quiet', '-q', action='store_true',
                        help='Suppress per-row output')
    parser.add_argument('--output', '-o', type=str, default=None,
                        help='Output CSV file for raw data')
    parser.add_argument('--raw-log-prefix', type=str, default=None,
                        help='Prefix for raw serial log files (creates <prefix>_d1.log, <prefix>_d2.log, ...)')
    parser.add_argument('--csv-rows', action='store_true',
                        help='Output CSV rows to stdout (one per device): device,freq_a,freq_b,delay')

    args = parser.parse_args()

    ports = [p.strip() for p in args.ports.split(',')]
    if len(ports) < 2:
        parser.error("Need at least 2 ports for multi-device test")

    print(f"Reading from {len(ports)} devices: {ports}", file=sys.stderr)

    device_rows, errors, non_csv_lines = collect_data(
        ports,
        args.baudrate,
        args.duration,
        args.samples + args.skip_samples,  # Collect extra to account for skipping
        args.quiet,
        args.raw_log_prefix
    )

    # Write raw data if requested
    if args.output:
        with open(args.output, 'w') as f:
            f.write("device,seq,timestamp,a_n,a_hz,b_n,b_hz,d_avg_ns,d_std_ns,d_miss_a,d_miss_b\n")
            for device_id in sorted(device_rows.keys()):
                for row in device_rows[device_id]:
                    f.write(f"{row.device_id},{row.seq},{row.timestamp:.3f},"
                            f"{row.a_n},{row.a_hz:.4f},{row.b_n},{row.b_hz:.4f},"
                            f"{row.d_avg_ns:.2f},{row.d_std_ns:.2f},"
                            f"{row.d_miss_a},{row.d_miss_b}\n")
        print(f"Raw data written to {args.output}", file=sys.stderr)

    ok = analyze_results(
        device_rows,
        errors,
        non_csv_lines,
        args.expected_freq,
        args.skip_samples,
        args.freq_tolerance_ppm
    )

    # Output CSV rows if requested (one per device)
    if args.csv_rows and device_rows:
        for device_id in sorted(device_rows.keys()):
            rows = device_rows[device_id]
            if len(rows) > args.skip_samples:
                valid_rows = rows[args.skip_samples:]
                avg_freq_a = sum(r.a_hz for r in valid_rows) / len(valid_rows)
                avg_freq_b = sum(r.b_hz for r in valid_rows) / len(valid_rows)
                avg_delay = sum(r.d_avg_ns for r in valid_rows) / len(valid_rows)
                print(f"{device_id},{avg_freq_a:.2f},{avg_freq_b:.2f},{avg_delay:.2f}")

    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
