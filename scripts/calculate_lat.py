#!/usr/bin/env python3

import argparse
import math
import struct
from typing import Iterable, List

def parse_binary_slowdowns(path: str, sample_rate: float = 1.0) -> List[float]:
    """Read binary slowdown samples: contiguous little-endian doubles."""
    slowdowns: List[float] = []

    if sample_rate <= 0.0 or sample_rate > 1.0:
        raise ValueError("sample_rate must be in (0, 1]")

    step = 1
    if sample_rate < 1.0:
        step = int(1.0 / sample_rate)
        if step < 1:
            step = 1

    index = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(8 * 1024)
            if not chunk:
                break

            chunk_len = len(chunk) - (len(chunk) % 8)
            offset = 0
            while offset < chunk_len:
                if index % step == 0:
                    (slowdown,) = struct.unpack_from("<d", chunk, offset)
                    slowdowns.append(slowdown)
                index += 1
                offset += 8

    return slowdowns


def drop_prefix_percent(values: Iterable[float], percent: float) -> List[float]:
    """Drop the first `percent`% of samples in original order."""
    data = list(values)
    if not data:
        return data

    discard_count = math.floor(len(data) * percent / 100.0)
    if discard_count <= 0:
        return data

    return data[discard_count:]


def percentile(values: List[float], p: float) -> float:
    """Return the p-th percentile with linear interpolation."""
    if not values:
        raise ValueError("no data for percentile computation")
    if len(values) == 1:
        return values[0]

    if p <= 0:
        return values[0]
    if p >= 100:
        return values[-1]

    pos = p / 100.0 * (len(values) - 1)
    lower = math.floor(pos)
    upper = math.ceil(pos)
    if lower == upper:
        return values[int(pos)]

    weight_upper = pos - lower
    weight_lower = 1.0 - weight_upper
    return values[lower] * weight_lower + values[upper] * weight_upper


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Read a binary slowdown file (little-endian doubles) and print "
            "P10/P50/P90/P99/P99.9."
        )
    )
    parser.add_argument("path", help="path to the binary result file")
    parser.add_argument(
        "--binary-sample-rate",
        type=float,
        default=1.0,
        metavar="R",
        help="sampling rate in (0, 1]; e.g. 0.01 keeps roughly 1 in 100 samples",
    )
    args = parser.parse_args()

    data = parse_binary_slowdowns(args.path, sample_rate=args.binary_sample_rate)
    if not data:
        raise SystemExit("no slowdown samples read from binary file.")

    total_packets = len(data)
    remaining = drop_prefix_percent(data, 1.0)
    remaining_sorted = sorted(remaining)

    percentiles = [10.0, 50.0, 90.0, 99.0, 99.9]
    percentile_results = {p: percentile(remaining_sorted, p) for p in percentiles}

    print(f"Total samples: {total_packets}")
    print(f"Dropped: {total_packets - len(remaining)} (first 1%)")
    print(f"Remaining samples: {len(remaining)}")
    print("Slowdown results: ")
    for p in percentiles:
        value = percentile_results[p]
        if p.is_integer():
            p_display = f"P{int(p)}"
        else:
            p_display = f"P{p}"
        print(f"  {p_display}: {value:.6f}")


if __name__ == "__main__":
    main()
