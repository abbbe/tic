#!/usr/bin/env python3
"""
TIC Binary Console

CLI tool for reading and displaying TIC binary frames.

Usage:
    # Table output matching ESP32 console format
    python tic_binary_console.py --port /dev/tty.usbmodem3113101 --test

    # CSV output
    python tic_binary_console.py --port /dev/tty.usbmodem3113101 --csv

    # Limited duration
    python tic_binary_console.py --port /dev/tty.usbmodem3113101 --test --duration 60
"""

import sys
import time
import logging
import argparse
from typing import Optional

import serial

from tic_binary_reader import TICReader, TICFrame


def print_table_header():
    """Print table header matching ESP32 console output."""
    print("A_N  |   A_Hz|A_min_us|A_avg_us|A_max_us|A_std_us|"
          "B_N  |   B_Hz|B_min_us|B_avg_us|B_max_us|B_std_us|"
          "D_N  |   D_min_ns|   D_avg_ns|   D_max_ns|   D_std_ns|D_missA|D_missB|CPU0|CPU1")
    print("-----|-------|--------|--------|--------|--------|"
          "-----|-------|--------|--------|--------|--------|"
          "-----|-----------|-----------|-----------|-----------|-------|-------|----|----|")


def print_csv_header(seq: int):
    """Print CSV header matching ESP32 CSV output."""
    print(f"CSV{seq}\tA_N,A_Hz,A_min_us,A_avg_us,A_max_us,A_std_us,"
          "B_N,B_Hz,B_min_us,B_avg_us,B_max_us,B_std_us,"
          "D_N,D_min_ns,D_avg_ns,D_max_ns,D_std_ns,D_missA,D_missB,"
          "CPU0,CPU1")


def frame_to_csv(frame: TICFrame, seq: int) -> str:
    """Convert TICFrame to CSV line matching ESP32 format."""
    a_min_us = frame.period_a_min_ns / 1000.0
    a_avg_us = frame.period_a_mean_ns / 1000.0
    a_max_us = frame.period_a_max_ns / 1000.0
    a_std_us = 0.0  # stddev not in binary header
    b_min_us = frame.period_b_min_ns / 1000.0
    b_avg_us = frame.period_b_mean_ns / 1000.0
    b_max_us = frame.period_b_max_ns / 1000.0
    b_std_us = 0.0  # stddev not in binary header

    return (f"CSV{seq}\t{frame.edges_a},{frame.period_a_hz:.2f},"
            f"{a_min_us:.3f},{a_avg_us:.3f},{a_max_us:.3f},{a_std_us:.3f},"
            f"{frame.edges_b},{frame.period_b_hz:.2f},"
            f"{b_min_us:.3f},{b_avg_us:.3f},{b_max_us:.3f},{b_std_us:.3f},"
            f"{frame.pair_count},{frame.delay_min_ns:.3f},{frame.delay_mean_ns:.3f},"
            f"{frame.delay_max_ns:.3f},{frame.delay_stddev_ns:.3f},"
            f"{frame.miss_a},{frame.miss_b},"
            f"{frame.cpu0_pct:.1f},{frame.cpu1_pct:.1f}")


def test_reader(port: str, duration: Optional[float] = None, verbose: bool = False):
    """Test TIC reader with live output matching ESP32 console format."""
    print(f"Reading TIC frames from {port}...")
    print(f"Press Ctrl+C to stop\n")

    start_time = time.time()
    frame_count = 0
    total_edges_a = 0
    total_edges_b = 0
    total_pairs = 0
    total_errors = 0

    with TICReader(port, validate=True, timeout=5.0) as reader:
        try:
            # Use a polling approach that respects duration
            while True:
                # Check duration first
                elapsed = time.time() - start_time
                if duration is not None and elapsed >= duration:
                    break

                result = reader.read_frame()
                if result is None:
                    if verbose:
                        print(f"  (no frame, elapsed={elapsed:.1f}s, in_waiting={reader._serial.in_waiting})")
                    continue

                frame, errors = result
                frame_count += 1
                total_edges_a += frame.edges_a
                total_edges_b += frame.edges_b
                total_pairs += frame.pair_count

                # Print header periodically
                if frame_count == 1 or frame_count % 24 == 0:
                    print_table_header()

                # Print validation errors
                for error in errors:
                    print(f"*** {error}")
                    total_errors += 1

                # Print overflow warning
                if frame.overflow:
                    print(f"*** OVERFLOW in frame {frame.seq}")

                # Print stats from frame header matching ESP32 format exactly
                # Convert period from ns to us for display
                a_min_us = frame.period_a_min_ns / 1000.0
                a_avg_us = frame.period_a_mean_ns / 1000.0
                a_max_us = frame.period_a_max_ns / 1000.0
                a_std_us = 0.0  # stddev not in binary header
                b_min_us = frame.period_b_min_ns / 1000.0
                b_avg_us = frame.period_b_mean_ns / 1000.0
                b_max_us = frame.period_b_max_ns / 1000.0
                b_std_us = 0.0  # stddev not in binary header

                # Format: %5lu|%7.2f|%8.3f|%8.3f|%8.3f|%8.3f| for each channel
                # Then:   %5lu|%11.3f|%11.3f|%11.3f|%11.3f|%7lu|%7lu|%4.0f|%4.0f
                print(f"{frame.edges_a:5d}|{frame.period_a_hz:7.2f}|"
                      f"{a_min_us:8.3f}|{a_avg_us:8.3f}|{a_max_us:8.3f}|{a_std_us:8.3f}|"
                      f"{frame.edges_b:5d}|{frame.period_b_hz:7.2f}|"
                      f"{b_min_us:8.3f}|{b_avg_us:8.3f}|{b_max_us:8.3f}|{b_std_us:8.3f}|"
                      f"{frame.pair_count:5d}|{frame.delay_min_ns:11.3f}|{frame.delay_mean_ns:11.3f}|"
                      f"{frame.delay_max_ns:11.3f}|{frame.delay_stddev_ns:11.3f}|"
                      f"{frame.miss_a:7d}|{frame.miss_b:7d}|{frame.cpu0_pct:4d}|{frame.cpu1_pct:4d}")

        except KeyboardInterrupt:
            print("\nStopped by user")
        except (serial.SerialException, OSError) as e:
            print(f"\nDevice error: {e}")
        except TimeoutError as e:
            print(f"\nTimeout: {e}")
        except Exception as e:
            print(f"\nUnexpected error: {type(e).__name__}: {e}")
            import traceback
            traceback.print_exc()

    elapsed = time.time() - start_time
    vs = reader.validation_stats

    print("\n" + "=" * 140)
    print("Summary:")
    print(f"  Duration: {elapsed:.1f}s, Frames: {vs.frames_received} (valid: {vs.frames_valid})")
    print(f"  Errors: seq={vs.seq_errors}, crc={vs.crc_errors}")
    print(f"  Edges: A={total_edges_a}, B={total_edges_b}, Pairs={total_pairs}")
    if elapsed > 0:
        print(f"  Rates: A={total_edges_a/elapsed:.1f}/s, B={total_edges_b/elapsed:.1f}/s, pairs={total_pairs/elapsed:.1f}/s")

    all_errors = total_errors + vs.crc_errors
    if all_errors > 0:
        print(f"\n*** VALIDATION FAILED: {all_errors} errors detected ***")
        return 1
    else:
        print("\n*** VALIDATION PASSED ***")
        return 0


def csv_reader(port: str, duration: Optional[float] = None, verbose: bool = False):
    """Read TIC frames and output as CSV matching ESP32 format."""
    csv_seq = 0

    with TICReader(port, validate=True, timeout=5.0) as reader:
        start_time = time.time()
        header_printed = False

        try:
            while True:
                elapsed = time.time() - start_time
                if duration is not None and elapsed >= duration:
                    break

                result = reader.read_frame()
                if result is None:
                    continue

                frame, errors = result
                csv_seq += 1

                # Print header on first row
                if not header_printed:
                    print_csv_header(csv_seq)
                    header_printed = True

                # Print CSV row
                print(frame_to_csv(frame, csv_seq))

                # Log errors to stderr if verbose
                if verbose:
                    for error in errors:
                        print(f"*** {error}", file=sys.stderr)
                    if frame.overflow:
                        print(f"*** OVERFLOW in frame {frame.seq}", file=sys.stderr)

        except KeyboardInterrupt:
            pass
        except (serial.SerialException, OSError) as e:
            print(f"Device error: {e}", file=sys.stderr)
            return 1

    return 0


def main():
    parser = argparse.ArgumentParser(description="TIC Binary Console")
    parser.add_argument(
        '--port', '-p',
        default='/dev/tty.usbmodem3113101',
        help='Serial port (default: /dev/tty.usbmodem3113101)'
    )
    parser.add_argument(
        '--test', '-t',
        action='store_true',
        help='Run test mode with table output'
    )
    parser.add_argument(
        '--csv',
        action='store_true',
        help='Output CSV format matching ESP32 console'
    )
    parser.add_argument(
        '--duration', '-d',
        type=float,
        default=None,
        help='Duration in seconds (default: indefinite)'
    )
    parser.add_argument(
        '--no-validate',
        action='store_true',
        help='Disable validation checks'
    )
    parser.add_argument(
        '--verbose', '-v',
        action='store_true',
        help='Verbose output'
    )

    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
    )

    if args.csv:
        return csv_reader(args.port, args.duration, args.verbose)
    elif args.test:
        return test_reader(args.port, args.duration, args.verbose)
    else:
        print("Use --test for table output, --csv for CSV output, or import TICReader in your code")
        return 0


if __name__ == '__main__':
    sys.exit(main())
