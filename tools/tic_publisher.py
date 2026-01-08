#!/usr/bin/env python3
"""
TIC MQTT Publisher

Reads TIC statistics and publishes to MQTT for ingestion into the TIG stack.

Supports two modes:
1. Binary mode: Reads binary frames from USB CDC via TICReader (preferred)
2. CSV mode: Reads CSV output from UART serial (fallback)

MQTT Topics:
    tic/stats       - Per-buffer statistics (delay, jitter, edge counts)

Usage:
    # Binary mode (USB CDC) - default
    python tic_publisher.py --port /dev/tty.usbmodem3114401

    # CSV mode (UART serial)
    python tic_publisher.py --port /dev/tty.usbserial-xxx --mode csv

    # With MQTT broker
    python tic_publisher.py --port /dev/tty.usbmodem3114401 --mqtt-host 192.168.1.100

    # Dry run (no MQTT, just print stats)
    python tic_publisher.py --port /dev/tty.usbmodem3114401 --dry-run
"""

import argparse
import json
import re
import time
import logging
from typing import Optional, Iterator
from dataclasses import dataclass

import serial
import paho.mqtt.client as mqtt

from tic_binary_reader import TICReader, TICFrame

logger = logging.getLogger(__name__)


@dataclass
class TICStats:
    """TIC statistics from one measurement period."""
    seq: int
    # Channel A
    a_n: int
    a_hz: float
    a_min_us: float
    a_avg_us: float
    a_max_us: float
    a_std_us: float
    # Channel B
    b_n: int
    b_hz: float
    b_min_us: float
    b_avg_us: float
    b_max_us: float
    b_std_us: float
    # Delay B-A
    d_n: int
    d_min_ns: float
    d_avg_ns: float
    d_max_ns: float
    d_std_ns: float
    d_miss_a: int
    d_miss_b: int
    # CPU (optional)
    cpu0: Optional[float] = None
    cpu1: Optional[float] = None

    def to_mqtt_payload(self) -> dict:
        """Convert to MQTT payload format."""
        payload = {
            "ts": time.time(),
            "seq": self.seq,
            "a": {
                "n": self.a_n,
                "hz": self.a_hz,
                "min_us": self.a_min_us,
                "avg_us": self.a_avg_us,
                "max_us": self.a_max_us,
                "std_us": self.a_std_us,
            },
            "b": {
                "n": self.b_n,
                "hz": self.b_hz,
                "min_us": self.b_min_us,
                "avg_us": self.b_avg_us,
                "max_us": self.b_max_us,
                "std_us": self.b_std_us,
            },
            "d": {
                "n": self.d_n,
                "min_ns": self.d_min_ns,
                "avg_ns": self.d_avg_ns,
                "max_ns": self.d_max_ns,
                "std_ns": self.d_std_ns,
                "miss_a": self.d_miss_a,
                "miss_b": self.d_miss_b,
            },
        }
        if self.cpu0 is not None:
            payload["cpu"] = {"cpu0": self.cpu0, "cpu1": self.cpu1}
        return payload


def parse_csv_line(line: str, seq_counter: int) -> Optional[TICStats]:
    """
    Parse CSV line from TIC UART output.

    Format: CSV<seq>\t<19 comma-separated values>
    Example: CSV42\t4096,8000.00,125.000,...
    """
    line = line.strip()
    if not line:
        return None

    # Must start with CSV<seq><tab>
    match = re.match(r'^CSV(\d+)\t(.*)$', line)
    if not match:
        return None

    seq = int(match.group(1))
    rest = match.group(2)

    # Skip header
    if rest.startswith('A_N'):
        return None

    parts = rest.split(',')
    if len(parts) < 19:
        return None

    try:
        return TICStats(
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
    except (ValueError, IndexError) as e:
        logger.debug(f"Failed to parse CSV: {e}")
        return None


def frame_to_stats(frame: TICFrame) -> TICStats:
    """Convert TICFrame (from binary reader) to TICStats."""
    return TICStats(
        seq=frame.seq,
        a_n=frame.edges_a,
        a_hz=frame.period_a_hz,
        a_min_us=frame.period_a_min_ns / 1000.0,
        a_avg_us=frame.period_a_mean_ns / 1000.0,
        a_max_us=frame.period_a_max_ns / 1000.0,
        a_std_us=0.0,  # Not in binary header
        b_n=frame.edges_b,
        b_hz=frame.period_b_hz,
        b_min_us=frame.period_b_min_ns / 1000.0,
        b_avg_us=frame.period_b_mean_ns / 1000.0,
        b_max_us=frame.period_b_max_ns / 1000.0,
        b_std_us=0.0,  # Not in binary header
        d_n=frame.pair_count,
        d_min_ns=frame.delay_min_ns,
        d_avg_ns=frame.delay_mean_ns,
        d_max_ns=frame.delay_max_ns,
        d_std_ns=frame.delay_stddev_ns,
        d_miss_a=frame.miss_a,
        d_miss_b=frame.miss_b,
        cpu0=float(frame.cpu0_pct),
        cpu1=float(frame.cpu1_pct),
    )


def read_binary_stats(port: str) -> Iterator[TICStats]:
    """Read binary frames from USB CDC port using TICReader."""
    with TICReader(port, validate=True) as reader:
        logger.info(f"Reading binary frames from {port}")
        for frame, errors in reader.read_frames():
            for err in errors:
                logger.warning(err)
            if frame.overflow:
                logger.warning(f"OVERFLOW in frame {frame.seq}")
            yield frame_to_stats(frame)


def read_csv_stats(port: str, baudrate: int = 115200) -> Iterator[TICStats]:
    """Read CSV stats from UART serial port."""
    seq_counter = 0

    with serial.Serial(port, baudrate, timeout=1) as ser:
        logger.info(f"Reading CSV stats from {port}")

        while True:
            try:
                line = ser.readline().decode('utf-8', errors='ignore')
                stats = parse_csv_line(line, seq_counter)
                if stats:
                    seq_counter += 1
                    yield stats
            except Exception as e:
                logger.error(f"Error reading serial: {e}")
                time.sleep(1)


class TICPublisher:
    """Publishes TIC stats to MQTT broker."""

    def __init__(
        self,
        mqtt_host: str = "localhost",
        mqtt_port: int = 1883,
        topic: str = "tic/stats",
        client_id: Optional[str] = None,
        username: Optional[str] = None,
        password: Optional[str] = None,
    ):
        self.topic = topic
        self.connected = False
        self.stats_count = 0

        # Create MQTT client
        self.mqtt = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=client_id or f"tic-publisher-{int(time.time())}",
        )
        self.mqtt.on_connect = self._on_connect
        self.mqtt.on_disconnect = self._on_disconnect

        # Set credentials if provided
        if username:
            self.mqtt.username_pw_set(username, password)

        # Connect
        logger.info(f"Connecting to MQTT broker at {mqtt_host}:{mqtt_port}")
        self.mqtt.connect(mqtt_host, mqtt_port, keepalive=60)
        self.mqtt.loop_start()

        # Wait for connection
        timeout = 5.0
        start = time.time()
        while not self.connected and (time.time() - start) < timeout:
            time.sleep(0.1)

        if not self.connected:
            raise ConnectionError(f"Failed to connect to MQTT broker at {mqtt_host}:{mqtt_port}")

        logger.info(f"Connected to MQTT broker, publishing to {topic}")

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            logger.info("MQTT connected")
            self.connected = True
        else:
            logger.error(f"MQTT connection failed: {reason_code}")

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        logger.warning(f"MQTT disconnected: {reason_code}")
        self.connected = False

    def publish_stats(self, stats: TICStats) -> bool:
        """Publish TIC stats to MQTT."""
        if not self.connected:
            logger.warning("Not connected to MQTT broker")
            return False

        payload = stats.to_mqtt_payload()
        result = self.mqtt.publish(self.topic, json.dumps(payload), qos=1)

        if result.rc == mqtt.MQTT_ERR_SUCCESS:
            self.stats_count += 1
            return True
        return False

    def close(self):
        """Close MQTT connection."""
        self.mqtt.loop_stop()
        self.mqtt.disconnect()
        logger.info(f"Disconnected from MQTT broker (published {self.stats_count} stats)")


def run_publisher(
    port: str,
    mode: str,
    mqtt_host: str,
    mqtt_port: int,
    mqtt_user: Optional[str] = None,
    mqtt_pass: Optional[str] = None,
    duration: Optional[float] = None,
    dry_run: bool = False,
):
    """Run the TIC publisher."""

    # Select reader based on mode
    if mode == "csv":
        reader = read_csv_stats(port)
    else:
        reader = read_binary_stats(port)

    # Create publisher (or None for dry run)
    publisher = None
    if not dry_run:
        publisher = TICPublisher(mqtt_host, mqtt_port, username=mqtt_user, password=mqtt_pass)

    start_time = time.time()
    stats_count = 0

    try:
        for stats in reader:
            stats_count += 1

            # Print stats
            print(f"[{stats.seq:5d}] A={stats.a_hz:7.1f}Hz B={stats.b_hz:7.1f}Hz "
                  f"delay={stats.d_avg_ns:10.2f}±{stats.d_std_ns:.2f}ns "
                  f"(n={stats.d_n})")

            # Publish to MQTT
            if publisher:
                if publisher.publish_stats(stats):
                    logger.debug(f"Published stats #{stats.seq}")
                else:
                    logger.warning(f"Failed to publish stats #{stats.seq}")

            # Check duration
            if duration and (time.time() - start_time) >= duration:
                break

    except KeyboardInterrupt:
        print("\nStopped by user")
    finally:
        if publisher:
            publisher.close()

    elapsed = time.time() - start_time
    print(f"\nSummary: {stats_count} stats in {elapsed:.1f}s ({stats_count/elapsed:.1f}/s)")


def main():
    import os

    parser = argparse.ArgumentParser(description="TIC MQTT Publisher")
    parser.add_argument(
        "--port", "-p",
        required=True,
        help="Serial port (e.g., /dev/tty.usbmodem5AF71074821)"
    )
    parser.add_argument(
        "--mode", "-m",
        choices=["binary", "csv"],
        default="binary",
        help="Input mode: binary (USB CDC, default), csv (UART serial)"
    )
    parser.add_argument(
        "--mqtt-host",
        default="localhost",
        help="MQTT broker host (default: localhost)"
    )
    parser.add_argument(
        "--mqtt-port",
        type=int,
        default=1883,
        help="MQTT broker port (default: 1883)"
    )
    parser.add_argument(
        "--mqtt-user",
        default=os.environ.get("MQTT_USERNAME"),
        help="MQTT username (default: $MQTT_USERNAME)"
    )
    parser.add_argument(
        "--mqtt-pass",
        default=os.environ.get("MQTT_PASSWORD"),
        help="MQTT password (default: $MQTT_PASSWORD)"
    )
    parser.add_argument(
        "--duration", "-d",
        type=float,
        default=None,
        help="Run duration in seconds (default: indefinite)"
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Don't publish to MQTT, just print stats"
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Verbose output"
    )

    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
    )

    run_publisher(
        port=args.port,
        mode=args.mode,
        mqtt_host=args.mqtt_host,
        mqtt_port=args.mqtt_port,
        mqtt_user=args.mqtt_user,
        mqtt_pass=args.mqtt_pass,
        duration=args.duration,
        dry_run=args.dry_run,
    )


if __name__ == "__main__":
    main()
