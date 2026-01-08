#!/usr/bin/env python3
"""
TIC Binary Serial Reader

Reads binary matched pair timing data from the TIC (Time Interval Counter) ESP32 device
via USB CDC serial port.

Binary Frame Format:
    Header (74 bytes):
        - magic:           uint32  "TIC1" = 0x31434954
        - seq:             uint32  Frame sequence number
        - resolution_hz:   uint32  Timer resolution (80000000)
        - base_ts:         uint64  64-bit base timestamp
        - pair_count:      uint16  Number of matched pairs
        - edges_a:         uint16  Total channel A edges
        - edges_b:         uint16  Total channel B edges
        - miss_a:          uint16  Unmatched A edges
        - miss_b:          uint16  Unmatched B edges
        - flags:           uint16  Flags (bit 0 = overflow)
        - delay_mean_ns:   int32   Mean delay (B-A) in ns
        - delay_min_ns:    int32   Min delay in ns
        - delay_max_ns:    int32   Max delay in ns
        - delay_stddev_ns: int32   Stddev of delay in ns
        - cpu0_pct:        uint8   CPU0 utilization %
        - cpu1_pct:        uint8   CPU1 utilization %
        - period_a_mean_ns: uint32  Channel A mean period in ns
        - period_a_min_ns:  uint32  Channel A min period in ns
        - period_a_max_ns:  uint32  Channel A max period in ns
        - period_b_mean_ns: uint32  Channel B mean period in ns
        - period_b_min_ns:  uint32  Channel B min period in ns
        - period_b_max_ns:  uint32  Channel B max period in ns

    Matched Pairs (8 bytes each):
        - ts_a:            uint32  Channel A timestamp
        - ts_b:            uint32  Channel B timestamp

    CRC32 (4 bytes):
        - crc32:           uint32  CRC32-LE over header + pairs (zlib compatible)

Usage:
    # Read from TIC device
    reader = TICReader("/dev/tty.usbmodem3113101")
    for frame in reader.read_frames():
        print(f"Frame {frame.seq}: {frame.pair_count} pairs, delay={frame.delay_mean_ns}ns")

    # Test mode (displays like ESP32 console)
    python tic_binary_reader.py --port /dev/tty.usbmodem3113101 --test
"""

import struct
import time
import logging
import argparse
import zlib
from dataclasses import dataclass, field
from typing import Iterator, Optional

import serial

logger = logging.getLogger(__name__)

# Frame magic: "TIC1" in little-endian
TIC_FRAME_MAGIC = 0x31434954

# Frame header format (74 bytes):
# magic(4) + seq(4) + resolution(4) + base_ts(8) +
# pair_count(2) + edges_a(2) + edges_b(2) + miss_a(2) + miss_b(2) + flags(2) +
# delay_mean(4f) + delay_min(4f) + delay_max(4f) + delay_stddev(4f) +
# cpu0(1) + cpu1(1) +
# period_a_mean(4) + period_a_min(4) + period_a_max(4) +
# period_b_mean(4) + period_b_min(4) + period_b_max(4)
HEADER_FORMAT = '<IIIQHHHHHHffffbbIIIIII'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)  # 74 bytes

# Matched pair format: ts_a(4) + ts_b(4)
PAIR_FORMAT = '<II'
PAIR_SIZE = struct.calcsize(PAIR_FORMAT)  # 8 bytes

# CRC32 size
CRC_SIZE = 4

# Frame flags
TIC_FRAME_FLAG_OVERFLOW = 1 << 0


class ValidationError(Exception):
    """Raised when frame validation fails."""
    pass


@dataclass
class TICMatchedPair:
    """Single matched pair (A edge + B edge)."""
    ts_a: int
    ts_b: int

    def delay_ns(self, resolution_hz: int) -> float:
        """Calculate delay in nanoseconds (B - A)."""
        return (self.ts_b - self.ts_a) * 1e9 / resolution_hz


@dataclass
class TICFrame:
    """TIC frame containing matched pairs and embedded stats."""
    seq: int
    resolution_hz: int
    base_ts: int
    pair_count: int
    edges_a: int
    edges_b: int
    miss_a: int
    miss_b: int
    overflow: bool
    delay_mean_ns: float
    delay_min_ns: float
    delay_max_ns: float
    delay_stddev_ns: float
    cpu0_pct: int
    cpu1_pct: int
    period_a_mean_ns: int
    period_a_min_ns: int
    period_a_max_ns: int
    period_b_mean_ns: int
    period_b_min_ns: int
    period_b_max_ns: int
    pairs: list[TICMatchedPair] = field(default_factory=list)

    @property
    def period_a_hz(self) -> float:
        """Channel A frequency in Hz."""
        if self.period_a_mean_ns > 0:
            return 1e9 / self.period_a_mean_ns
        return 0.0

    @property
    def period_b_hz(self) -> float:
        """Channel B frequency in Hz."""
        if self.period_b_mean_ns > 0:
            return 1e9 / self.period_b_mean_ns
        return 0.0


@dataclass
class TICValidationStats:
    """Tracks validation statistics."""
    frames_received: int = 0
    frames_valid: int = 0
    seq_errors: int = 0
    crc_errors: int = 0
    last_seq: Optional[int] = None
    expected_seq: Optional[int] = None


class TICReader:
    """
    Reads binary frames from TIC device via USB CDC serial.

    Provides validation for:
    - Sequence numbers must be continuous and monotonous (no gaps)
    - CRC32 integrity check
    """

    def __init__(
        self,
        port: str,
        baudrate: int = 115200,
        timeout: float = 2.0,
        validate: bool = True,
    ):
        """
        Initialize TIC reader.

        Args:
            port: Serial port path (e.g., /dev/tty.usbmodem3113101)
            baudrate: Baud rate (not used for USB CDC, but set anyway)
            timeout: Read timeout in seconds
            validate: Enable validation (sequence continuity)
        """
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.validate = validate
        self._serial: Optional[serial.Serial] = None
        self._buffer = b''
        self.validation_stats = TICValidationStats()

    def open(self):
        """Open the serial port."""
        if self._serial is not None:
            return

        logger.info(f"Opening serial port {self.port}")
        self._serial = serial.Serial(
            port=self.port,
            baudrate=self.baudrate,
            timeout=0.1,  # 100ms read timeout
        )

        # Sync to frame boundary using CRC validation
        self._buffer = b''
        self.validation_stats = TICValidationStats()
        logger.info("Syncing to frame boundary...")
        if not self._sync_to_magic():
            logger.warning("Could not sync to frame boundary")
        else:
            logger.info("Synced to frame boundary")

    def close(self):
        """Close the serial port."""
        if self._serial is not None:
            try:
                self._serial.close()
            except Exception:
                pass
            self._serial = None
            logger.info("Serial port closed")

    def reconnect(self, poll_interval: float = 0.05, max_attempts: int = 0) -> bool:
        """
        Try to reconnect to the serial port with fast polling.

        Args:
            poll_interval: Time between reconnection attempts in seconds
            max_attempts: Maximum attempts (0 = infinite)

        Returns:
            True if reconnected, False if max_attempts reached
        """
        self.close()
        attempts = 0
        print(f"Device disconnected, polling for reconnection...")

        while max_attempts == 0 or attempts < max_attempts:
            attempts += 1
            try:
                self._serial = serial.Serial(
                    port=self.port,
                    baudrate=self.baudrate,
                    timeout=0.1,
                )
                self._buffer = b''
                # Reset sequence expectation on reconnect
                self.validation_stats.expected_seq = None
                print(f"Reconnected to {self.port} after {attempts} attempts")
                return True
            except (serial.SerialException, OSError):
                if attempts % 20 == 0:
                    print(f"  Still waiting... ({attempts} attempts)")
                time.sleep(poll_interval)

        return False

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def _read_bytes(self, n: int) -> bytes:
        """Read exactly n bytes from serial, using internal buffer."""
        deadline = time.time() + self.timeout
        last_log = 0

        while len(self._buffer) < n:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError(f"Timeout reading {n} bytes (got {len(self._buffer)})")

            # Use blocking read - serial timeout handles waiting
            needed = n - len(self._buffer)
            chunk = self._serial.read(needed)
            if chunk:
                self._buffer += chunk
            else:
                # Timeout with no data - log periodically
                now = time.time()
                if now - last_log > 1.0:
                    logger.debug(f"Waiting for data... buffer={len(self._buffer)}/{n}")
                    last_log = now

        result = self._buffer[:n]
        self._buffer = self._buffer[n:]
        return result

    def _sync_to_magic(self) -> bool:
        """Scan for frame magic, discarding bytes until found."""
        magic_bytes = struct.pack('<I', TIC_FRAME_MAGIC)
        deadline = time.time() + self.timeout * 5  # Allow more time for sync

        # Read bytes until we find magic
        search_buf = b''
        bytes_searched = 0

        while time.time() < deadline and bytes_searched < 100000:
            byte = self._serial.read(1)
            if byte:
                bytes_searched += 1
                search_buf += byte

                # Keep last 4 bytes for magic matching
                if len(search_buf) > 4:
                    search_buf = search_buf[-4:]

                if search_buf == magic_bytes:
                    logger.debug(f"Found magic after scanning {bytes_searched} bytes")
                    # Put magic bytes back in buffer so header read includes them
                    self._buffer = magic_bytes + self._buffer
                    return True

        logger.warning(f"Could not find frame magic after {bytes_searched} bytes")
        return False

    def _validate_sequence(self, seq: int) -> list[str]:
        """Validate sequence number continuity. Returns list of errors."""
        errors = []
        vs = self.validation_stats

        if vs.expected_seq is not None:
            if seq != vs.expected_seq:
                if seq < vs.last_seq:
                    errors.append(f"SEQ ERROR: got {seq}, expected {vs.expected_seq} (went backwards!)")
                else:
                    gap = seq - vs.expected_seq
                    errors.append(f"SEQ ERROR: got {seq}, expected {vs.expected_seq} (gap of {gap})")
                vs.seq_errors += 1

        vs.last_seq = seq
        vs.expected_seq = seq + 1
        return errors

    def read_frame(self) -> Optional[tuple[TICFrame, list[str]]]:
        """
        Read a single frame from the serial port.

        Returns:
            Tuple of (TICFrame, validation_errors) or None if no frame available.
            validation_errors is a list of error strings (empty if all valid).
        """
        if self._serial is None:
            raise RuntimeError("Serial port not open")

        try:
            # Read header - may raise SerialException/OSError on disconnect
            header_data = self._read_bytes(HEADER_SIZE)
        except TimeoutError:
            return None

        (magic, seq, resolution_hz, base_ts,
         pair_count, edges_a, edges_b, miss_a, miss_b, flags,
         delay_mean_ns, delay_min_ns, delay_max_ns, delay_stddev_ns,
         cpu0_pct, cpu1_pct,
         period_a_mean_ns, period_a_min_ns, period_a_max_ns,
         period_b_mean_ns, period_b_min_ns, period_b_max_ns) = \
            struct.unpack(HEADER_FORMAT, header_data)

        logger.debug(f"Frame header: seq={seq} pairs={pair_count}")

        # Validate header fields
        valid = (magic == TIC_FRAME_MAGIC and
                 resolution_hz == 80000000 and pair_count <= 10000)
        if not valid:
            if magic != TIC_FRAME_MAGIC:
                logger.warning(f"Invalid magic: 0x{magic:08x}, resyncing...")
            else:
                logger.warning(f"Invalid header (res={resolution_hz}, pairs={pair_count}), resyncing...")
            if not self._sync_to_magic():
                return None
            # Try again after resync
            return self.read_frame()

        # Read all pairs as raw bytes for CRC calculation
        pairs_data = self._read_bytes(pair_count * PAIR_SIZE) if pair_count > 0 else b''

        # Read CRC32
        crc_data = self._read_bytes(CRC_SIZE)
        received_crc = struct.unpack('<I', crc_data)[0]

        # Calculate expected CRC over header + pairs
        expected_crc = zlib.crc32(header_data)
        if pairs_data:
            expected_crc = zlib.crc32(pairs_data, expected_crc)
        expected_crc &= 0xffffffff

        if received_crc != expected_crc:
            logger.warning(f"CRC mismatch: got 0x{received_crc:08x}, expected 0x{expected_crc:08x}, resyncing...")
            self.validation_stats.crc_errors += 1
            if not self._sync_to_magic():
                return None
            return self.read_frame()

        # Parse pairs from raw data
        pairs = []
        for i in range(pair_count):
            offset = i * PAIR_SIZE
            ts_a, ts_b = struct.unpack(PAIR_FORMAT, pairs_data[offset:offset+PAIR_SIZE])
            pairs.append(TICMatchedPair(ts_a=ts_a, ts_b=ts_b))

        frame = TICFrame(
            seq=seq,
            resolution_hz=resolution_hz,
            base_ts=base_ts,
            pair_count=pair_count,
            edges_a=edges_a,
            edges_b=edges_b,
            miss_a=miss_a,
            miss_b=miss_b,
            overflow=(flags & TIC_FRAME_FLAG_OVERFLOW) != 0,
            delay_mean_ns=delay_mean_ns,
            delay_min_ns=delay_min_ns,
            delay_max_ns=delay_max_ns,
            delay_stddev_ns=delay_stddev_ns,
            cpu0_pct=cpu0_pct,
            cpu1_pct=cpu1_pct,
            period_a_mean_ns=period_a_mean_ns,
            period_a_min_ns=period_a_min_ns,
            period_a_max_ns=period_a_max_ns,
            period_b_mean_ns=period_b_mean_ns,
            period_b_min_ns=period_b_min_ns,
            period_b_max_ns=period_b_max_ns,
            pairs=pairs,
        )

        # Validate if enabled
        validation_errors = []
        if self.validate:
            validation_errors.extend(self._validate_sequence(seq))

        self.validation_stats.frames_received += 1
        if not validation_errors:
            self.validation_stats.frames_valid += 1

        return frame, validation_errors


    def read_frames(self, auto_reconnect: bool = True) -> Iterator[tuple[TICFrame, list[str]]]:
        """
        Generator that yields frames continuously.

        Args:
            auto_reconnect: If True, automatically reconnect on device disconnect

        Yields:
            Tuple of (TICFrame, validation_errors) for each frame received.
        """
        while True:
            try:
                result = self.read_frame()
                if result is not None:
                    yield result
            except (serial.SerialException, OSError) as e:
                logger.warning(f"Device error: {e}")
                if auto_reconnect:
                    if not self.reconnect():
                        raise
                else:
                    raise


def print_header():
    """Print table header matching ESP32 console output."""
    print("A_N  |   A_Hz|A_min_us|A_avg_us|A_max_us|A_std_us|"
          "B_N  |   B_Hz|B_min_us|B_avg_us|B_max_us|B_std_us|"
          "D_N  |   D_min_ns|   D_avg_ns|   D_max_ns|   D_std_ns|D_missA|D_missB|CPU0|CPU1")
    print("-----|-------|--------|--------|--------|--------|"
          "-----|-------|--------|--------|--------|--------|"
          "-----|-----------|-----------|-----------|-----------|-------|-------|----|----|")


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
                    print_header()

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


def main():
    parser = argparse.ArgumentParser(description="TIC Binary Serial Reader")
    parser.add_argument(
        '--port', '-p',
        default='/dev/tty.usbmodem3113101',
        help='Serial port (default: /dev/tty.usbmodem3113101)'
    )
    parser.add_argument(
        '--test', '-t',
        action='store_true',
        help='Run test mode with live output'
    )
    parser.add_argument(
        '--duration', '-d',
        type=float,
        default=None,
        help='Test duration in seconds (default: indefinite)'
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

    if args.test:
        return test_reader(args.port, args.duration, args.verbose)
    else:
        print("Use --test to run test mode, or import TICReader in your code")
        return 0


if __name__ == '__main__':
    import sys
    sys.exit(main())
