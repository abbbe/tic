#!/usr/bin/env python3
"""
Raw CDC reader - just counts bytes and looks for magic sync.
No frame processing, minimal overhead.
"""

import argparse
import struct
import sys
import time
import serial

TIC_FRAME_MAGIC = 0x31434954  # "TIC1" little-endian
MAGIC_BYTES = struct.pack('<I', TIC_FRAME_MAGIC)
HEADER_SIZE = 28
EVENT_SIZE = 8


def hexdump(data: bytes, max_len: int = 64) -> str:
    """Return hex dump of data, truncated if needed."""
    if len(data) > max_len:
        return data[:max_len].hex() + f"... (+{len(data)-max_len} bytes)"
    return data.hex()


def main():
    parser = argparse.ArgumentParser(description='Raw TIC CDC reader')
    parser.add_argument('--port', '-p', required=True, help='CDC serial port')
    parser.add_argument('--duration', '-d', type=float, default=10, help='Duration in seconds')
    parser.add_argument('--buffer', '-b', type=int, default=4096, help='Read buffer size')
    parser.add_argument('--debug', '-D', action='store_true', help='Debug: show each chunk')
    parser.add_argument('--sync', '-s', action='store_true', help='Sync mode: try to parse frames')
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, 115200, timeout=0.1)
    except serial.SerialException as e:
        print(f"Error opening {args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"Reading raw bytes from {args.port} for {args.duration}s...")
    print(f"Looking for magic: {MAGIC_BYTES.hex()} ('TIC1')")
    if args.debug:
        print("Debug mode: showing each chunk")
    if args.sync:
        print("Sync mode: attempting frame parsing")
    print()

    total_bytes = 0
    magic_count = 0
    start_time = time.time()
    last_report = start_time
    interval_bytes = 0
    chunk_count = 0
    skipped_bytes = 0
    valid_frames = 0

    # Buffer for sync mode
    buf = b''

    try:
        while True:
            elapsed = time.time() - start_time
            if elapsed >= args.duration:
                break

            # Read chunk
            chunk = ser.read(args.buffer)
            if not chunk:
                continue

            chunk_len = len(chunk)
            total_bytes += chunk_len
            interval_bytes += chunk_len
            chunk_count += 1

            if args.debug:
                print(f"  CHUNK {chunk_count:4d}: {chunk_len:5d} bytes: {hexdump(chunk, 32)}")

            if args.sync:
                # Sync mode: accumulate and try to parse frames
                buf += chunk

                while len(buf) >= 4:
                    # Look for magic at start
                    if buf[:4] != MAGIC_BYTES:
                        # Not at frame start - find next magic
                        idx = buf.find(MAGIC_BYTES)
                        if idx == -1:
                            # No magic found - keep last 3 bytes, discard rest
                            skip = len(buf) - 3
                            if skip > 0:
                                skipped_bytes += skip
                                if args.debug:
                                    print(f"    SKIP: {skip} bytes (no magic), keeping: {buf[-3:].hex()}")
                                buf = buf[-3:]
                            break
                        else:
                            # Found magic - skip to it
                            skipped_bytes += idx
                            if args.debug:
                                print(f"    SKIP: {idx} bytes to magic")
                            buf = buf[idx:]
                            continue

                    # Have magic at start - check if we have full header
                    if len(buf) < HEADER_SIZE:
                        if args.debug:
                            print(f"    WAIT: need {HEADER_SIZE - len(buf)} more bytes for header")
                        break

                    # Parse header
                    magic, version, resolution, seq, first_ts, event_count, flags = \
                        struct.unpack('<IIIIQHH', buf[:HEADER_SIZE])

                    frame_size = HEADER_SIZE + event_count * EVENT_SIZE

                    if args.debug:
                        print(f"    HEADER: seq={seq} events={event_count} frame_size={frame_size}")

                    # Sanity check
                    if event_count > 20000:
                        print(f"    ERROR: event_count={event_count} too large, resyncing")
                        buf = buf[1:]  # Skip one byte and retry
                        skipped_bytes += 1
                        continue

                    # Check if we have full frame
                    if len(buf) < frame_size:
                        if args.debug:
                            print(f"    WAIT: need {frame_size - len(buf)} more bytes for frame")
                        break

                    # Got full frame
                    magic_count += 1
                    valid_frames += 1
                    buf = buf[frame_size:]

                    if args.debug:
                        print(f"    FRAME: seq={seq} events={event_count} OK")
            else:
                # Simple mode: just search for magic occurrences
                search_start = max(0, len(buf) - 3) if buf else 0
                buf += chunk

                pos = search_start
                while True:
                    idx = buf.find(MAGIC_BYTES, pos)
                    if idx == -1:
                        break
                    magic_count += 1
                    pos = idx + 1

                # Keep only last 3 bytes for cross-chunk detection
                if len(buf) > 3:
                    buf = buf[-3:]

            # Report every second
            now = time.time()
            if now - last_report >= 1.0:
                rate = interval_bytes / (now - last_report)
                extra = ""
                if args.sync:
                    extra = f" skip={skipped_bytes}"
                print(f"  {elapsed:5.1f}s: {total_bytes:10d} bytes, {magic_count:6d} frames, {rate/1024:7.1f} KB/s{extra}")
                interval_bytes = 0
                last_report = now

    except KeyboardInterrupt:
        print("\nStopped")

    elapsed = time.time() - start_time
    print()
    print(f"=== Summary ===")
    print(f"Duration:     {elapsed:.1f}s")
    print(f"Total bytes:  {total_bytes}")
    print(f"Chunks:       {chunk_count}")
    print(f"Magic found:  {magic_count}")
    if args.sync:
        print(f"Valid frames: {valid_frames}")
        print(f"Skipped:      {skipped_bytes} bytes")
    if elapsed > 0:
        print(f"Avg rate:     {total_bytes/elapsed/1024:.1f} KB/s")
    if magic_count > 0:
        print(f"Avg frame:    {total_bytes/magic_count:.0f} bytes")
        print(f"Frame rate:   {magic_count/elapsed:.1f} frames/s")


if __name__ == '__main__':
    main()
