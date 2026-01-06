---
layout: home
title: TIC
---

# TIC - Time Interval Counter

Dual-channel time interval counter for ESP32-S3 using MCPWM capture hardware.

**Resolution:** 12.5 ns (80 MHz timer)

## Features

- Dual channel capture (A and B)
- Per-channel period statistics (min/max/mean/stddev)
- Inter-channel delay measurement (B - A)
- ISR-driven double buffering (up to 8192 edges)
- Self-test loopback mode

## Quick Start

```bash
git clone <repo>
cd tic
idf.py set-target esp32s3
idf.py build flash monitor
```

## Hardware

| Pin | Default GPIO | Function |
|-----|--------------|----------|
| Ch A | GPIO4 | Input capture |
| Ch B | GPIO5 | Input capture |

3.3V logic, rising edge triggered.

## Output Example

```
Ch A: 4096 edges, 1000.000 Hz, period [999.988..1000.012..1000.037] us, std=0.008 us
Ch B: 4096 edges, 1000.000 Hz, period [999.991..1000.011..1000.031] us, std=0.007 us
Delay B-A: 4095 pairs, mean=50.125 ns, std=2.341 ns, [45.000..55.250]
```
