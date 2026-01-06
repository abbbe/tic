---
layout: post
title: "TIC v1.0 - Feature Complete"
date: 2026-01-05
---

Initial feature-complete release of the Time Interval Counter for ESP32-S3.

## Capture Engine

- MCPWM capture peripheral at 80 MHz (12.5 ns resolution)
- Dual channel support (A and B) on configurable GPIOs
- ISR-driven double buffering with configurable threshold (up to 8192 edges)
- Overflow handling for extended timestamps

## Statistics

Per-channel period measurement:
- Edge count
- Min/max/mean period
- Standard deviation (Welford's online algorithm)
- Frequency calculation

Inter-channel delay (B - A):
- Window-based edge matching (quarter-period window)
- O(n) algorithm with limited lookahead
- Missed pulse tracking
- Signed delay support (B can lead or lag A)

## Test Infrastructure

- Loopback mode: internal PWM routed to capture input
- External PWM output mode: reference signal generation
- Configurable test frequencies (1 Hz to 1 MHz)

## Configuration

All options via `idf.py menuconfig`:

```
TIC Configuration
├── Loopback test mode [off]
├── Channel A GPIO [4]
├── Channel B GPIO [5]
├── Edges per buffer [8192]
├── Stats period [1000 ms]
└── External PWM Output
    ├── Enable [off]
    ├── GPIO [4]
    └── Frequency [1000 Hz]
```

## What's Next

- USB CDC data streaming
- InfluxDB/MQTT integration
- Histogram output
