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
- Dual signal generator with relative delay (ns)
- Auto loopback when gen GPIO == capture GPIO

## Quick Start

```bash
git clone <repo>
cd tic
idf.py set-target esp32s3
idf.py build flash monitor
```

## Hardware

| Function | Default GPIO |
|----------|--------------|
| Capture A | GPIO4 |
| Capture B | GPIO5 |
| SigGen A | GPIO6 |
| SigGen B | GPIO7 |

3.3V logic, rising edge triggered.

## Output

Single row per buffer, header repeats every 24 rows:

| Prefix | Columns |
|--------|---------|
| `A_`, `B_` | N, Hz, min_us, avg_us, max_us, std_us |
| `D_` | N, min_ns, avg_ns, max_ns, std_ns, missA, missB |
