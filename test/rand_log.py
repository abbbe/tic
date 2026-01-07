#!/usr/bin/env python3
"""Generate random value with logarithmic distribution."""

import argparse
import random
import math


def rand_log(min_val: float, max_val: float) -> float:
    """Generate random value uniformly distributed in log space."""
    log_min = math.log(min_val)
    log_max = math.log(max_val)
    log_val = random.uniform(log_min, log_max)
    return math.exp(log_val)


def main():
    parser = argparse.ArgumentParser(description='Generate random value with logarithmic distribution')
    parser.add_argument('min', type=float, help='Minimum value')
    parser.add_argument('max', type=float, help='Maximum value')
    parser.add_argument('--int', action='store_true', help='Output as integer')
    args = parser.parse_args()

    val = rand_log(args.min, args.max)
    if args.int:
        print(int(round(val)))
    else:
        print(f"{val:.2f}")


if __name__ == '__main__':
    main()
