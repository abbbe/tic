#!/usr/bin/env python3
"""
TIC Binary Serial Reader

Reads binary edge timing data from the TIC (Time Interval Counter) ESP32 device
via USB CDC serial port.

Binary Frame Format:
    Header (28 bytes):
        - magic:        uint32  "TIC1" = 0x31434954
        - version:      uint32  Protocol version (1)
        - resolution_hz: uint32  Timer resolution (80000000)
        - seq:          uint32  Frame sequence number
        - first_edge_ts: uint64  64-bit timestamp of first edge
        - event_count:  uint16  Number of events
        - flags:        uint16  Flags (bit 0 = overflow)

    Events (8 bytes each):
        - type:         uint8   0=edge, 1=overflow
        - channel:      uint8   0=A, 1=B
        - reserved:     uint16
        - value:        uint32  Capture timestamp (32-bit)

    CRC32 (4 bytes):
        - crc32:        uint32  CRC32-LE over header + events (zlib compatible)

Usage:
    # Read from TIC device
    reader = TICReader("/dev/tty.usbmodem3113101")
    for frame, stats in reader.read_frames():
        print(f"Frame {frame.seq}: {len(frame.events)} events")
        if stats:
            print(f"  Delay: {stats['d']['avg_ns']:.1f}ns")

    # Test mode (displays like ESP32 console)
    python tic_reader.py --port /dev/tty.usbmodem3113101 --test
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

# Frame header format: magic(4) + version(4) + resolution(4) + seq(4) + first_edge_ts(8) + event_count(2) + flags(2)
HEADER_FORMAT = '<IIIIQHH'
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)  # 28 bytes

# Event format: type(1) + channel(1) + reserved(2) + value(4)
EVENT_FORMAT = '<BBHI'
EVENT_SIZE = struct.calcsize(EVENT_FORMAT)  # 8 bytes

# CRC32 size
CRC_SIZE = 4

# Event types
TIC_EVENT_EDGE = 0
TIC_EVENT_OVERFLOW = 1

# Channels
TIC_CHANNEL_A = 0
TIC_CHANNEL_B = 1

# Frame flags
TIC_FRAME_FLAG_OVERFLOW = 1 << 0


class ValidationError(Exception):
    """Raised when frame validation fails."""
    pass


@dataclass
class TICEvent:
    """Single TIC event (edge or overflow)."""
    type: int
    channel: int
    value: int

    @property
    def is_edge(self) -> bool:
        return self.type == TIC_EVENT_EDGE

    @property
    def is_overflow(self) -> bool:
        return self.type == TIC_EVENT_OVERFLOW

    @property
    def channel_name(self) -> str:
        return 'A' if self.channel == TIC_CHANNEL_A else 'B'


@dataclass
class TICFrame:
    """TIC frame containing events."""
    version: int
    resolution_hz: int
    seq: int
    first_edge_ts: int
    events: list[TICEvent]
    overflow: bool

    @property
    def edges_a(self) -> list[TICEvent]:
        return [e for e in self.events if e.is_edge and e.channel == TIC_CHANNEL_A]

    @property
    def edges_b(self) -> list[TICEvent]:
        return [e for e in self.events if e.is_edge and e.channel == TIC_CHANNEL_B]

    @property
    def overflow_count(self) -> int:
        return sum(1 for e in self.events if e.is_overflow)


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
    - Edge counts in frame must match stats when available
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
            validate: Enable validation (sequence continuity, edge counts)
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

    def read_frame(self) -> Optional[tuple[TICFrame, Optional[dict], list[str]]]:
        """
        Read a single frame from the serial port.

        Returns:
            Tuple of (TICFrame, stats_dict, validation_errors) or None if no frame available.
            stats_dict may be None if no stats line followed the frame.
            validation_errors is a list of error strings (empty if all valid).
        """
        if self._serial is None:
            raise RuntimeError("Serial port not open")

        try:
            # Read header - may raise SerialException/OSError on disconnect
            header_data = self._read_bytes(HEADER_SIZE)
        except TimeoutError:
            return None

        magic, version, resolution_hz, seq, first_edge_ts, event_count, flags = \
            struct.unpack(HEADER_FORMAT, header_data)

        logger.debug(f"Frame header: seq={seq} events={event_count}")

        # Validate header fields
        valid = (magic == TIC_FRAME_MAGIC and version == 1 and
                 resolution_hz == 80000000 and event_count <= 10000)
        if not valid:
            if magic != TIC_FRAME_MAGIC:
                logger.warning(f"Invalid magic: 0x{magic:08x}, resyncing...")
            else:
                logger.warning(f"Invalid header (ver={version}, res={resolution_hz}, events={event_count}), resyncing...")
            if not self._sync_to_magic():
                return None
            # Try again after resync
            return self.read_frame()

        # Read all events as raw bytes for CRC calculation
        events_data = self._read_bytes(event_count * EVENT_SIZE)

        # Read CRC32
        crc_data = self._read_bytes(CRC_SIZE)
        received_crc = struct.unpack('<I', crc_data)[0]

        # Calculate expected CRC over header + events
        expected_crc = zlib.crc32(header_data)
        expected_crc = zlib.crc32(events_data, expected_crc) & 0xffffffff

        if received_crc != expected_crc:
            logger.warning(f"CRC mismatch: got 0x{received_crc:08x}, expected 0x{expected_crc:08x}, resyncing...")
            self.validation_stats.crc_errors += 1
            if not self._sync_to_magic():
                return None
            return self.read_frame()

        # Parse events from raw data
        events = []
        for i in range(event_count):
            offset = i * EVENT_SIZE
            type_, channel, _, value = struct.unpack(EVENT_FORMAT, events_data[offset:offset+EVENT_SIZE])
            events.append(TICEvent(type=type_, channel=channel, value=value))

        frame = TICFrame(
            version=version,
            resolution_hz=resolution_hz,
            seq=seq,
            first_edge_ts=first_edge_ts,
            events=events,
            overflow=(flags & TIC_FRAME_FLAG_OVERFLOW) != 0,
        )

        # No more JSON stats in binary mode - CRC follows events directly
        stats = None

        # Validate if enabled
        validation_errors = []
        if self.validate:
            validation_errors.extend(self._validate_sequence(seq))

        self.validation_stats.frames_received += 1
        if not validation_errors:
            self.validation_stats.frames_valid += 1

        return frame, stats, validation_errors


    def read_frames(self, auto_reconnect: bool = True) -> Iterator[tuple[TICFrame, Optional[dict], list[str]]]:
        """
        Generator that yields frames continuously.

        Args:
            auto_reconnect: If True, automatically reconnect on device disconnect

        Yields:
            Tuple of (TICFrame, stats_dict, validation_errors) for each frame received.
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
    """Print table header like ESP32 console output."""
    print("=" * 140)
    print("  seq |  n_A | Hz_A  | min_A  | avg_A  | max_A  | std_A  |"
          "  n_B | Hz_B  | min_B  | avg_B  | max_B  | std_B  |"
          " d_n  |   d_min   |   d_avg   |   d_max   |   d_std   | CPU0| CPU1")
    print("-" * 140)


def test_reader(port: str, duration: Optional[float] = None, verbose: bool = False):
    """Test TIC reader with live output matching ESP32 console format."""
    print(f"Reading TIC frames from {port}...")
    print(f"Press Ctrl+C to stop\n")

    start_time = time.time()
    frame_count = 0
    total_edges_a = 0
    total_edges_b = 0
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

                frame, stats, errors = result
                frame_count += 1
                edges_a = len(frame.edges_a)
                edges_b = len(frame.edges_b)
                total_edges_a += edges_a
                total_edges_b += edges_b

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

                # Print stats in table format (like ESP32 console)
                if stats:
                    ch_a = stats.get('a', {})
                    ch_b = stats.get('b', {})
                    delay = stats.get('d', {})
                    cpu = stats.get('cpu', {})

                    print(f"{frame.seq:5d} |"
                          f"{ch_a.get('n', 0):5d} |"
                          f"{ch_a.get('hz', 0):6.1f} |"
                          f"{ch_a.get('min_us', 0):7.2f} |"
                          f"{ch_a.get('avg_us', 0):7.2f} |"
                          f"{ch_a.get('max_us', 0):7.2f} |"
                          f"{ch_a.get('std_us', 0):7.3f} |"
                          f"{ch_b.get('n', 0):5d} |"
                          f"{ch_b.get('hz', 0):6.1f} |"
                          f"{ch_b.get('min_us', 0):7.2f} |"
                          f"{ch_b.get('avg_us', 0):7.2f} |"
                          f"{ch_b.get('max_us', 0):7.2f} |"
                          f"{ch_b.get('std_us', 0):7.3f} |"
                          f"{delay.get('n', 0):5d} |"
                          f"{delay.get('min_ns', 0):10.2f} |"
                          f"{delay.get('avg_ns', 0):10.2f} |"
                          f"{delay.get('max_ns', 0):10.2f} |"
                          f"{delay.get('std_ns', 0):10.2f} |"
                          f"{cpu.get('cpu0', 0):4.0f} |"
                          f"{cpu.get('cpu1', 0):4.0f}")
                else:
                    # No stats - just print frame info
                    print(f"{frame.seq:5d} | {edges_a:5d} A + {edges_b:5d} B edges (no stats)")

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
    print(f"  Duration:       {elapsed:.1f}s")
    print(f"  Frames:         {vs.frames_received}")
    print(f"  Valid frames:   {vs.frames_valid}")
    print(f"  Seq errors:     {vs.seq_errors}")
    print(f"  CRC errors:     {vs.crc_errors}")
    print(f"  Total edges A:  {total_edges_a}")
    print(f"  Total edges B:  {total_edges_b}")
    if elapsed > 0:
        print(f"  Rate A:         {total_edges_a/elapsed:.1f} edges/s")
        print(f"  Rate B:         {total_edges_b/elapsed:.1f} edges/s")

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
