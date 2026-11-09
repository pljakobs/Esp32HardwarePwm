#!/usr/bin/env python3
"""Compare IDF fade truncation with nearest-error cycle_num selection.

Uses exact rational arithmetic. Models duty stepping, not ISR/edge alignment,
endpoint snaps, networking, spread spectrum, or application polling delays.
No third-party dependencies and no device access.
"""

import argparse
from dataclasses import dataclass
from fractions import Fraction


MAX_STEPS = 1023
MAX_CYCLES = 1023
MAX_SCALE = 1023


@dataclass(frozen=True)
class Segment:
    duty_range: int
    duration_ms: int


@dataclass(frozen=True)
class Result:
    duration_ms: Fraction
    cycles: tuple[int, ...]
    fast_segments: int


def split_fade(duty_range: int, duration_ms: int, frequency: int) -> list[Segment]:
    """Mirror queueDutyChan's proportional splitting, assuming enough capacity."""
    if duty_range <= 0 or duration_ms <= 0 or frequency <= 0:
        raise ValueError("range, duration and frequency must be positive")
    can_split = frequency * duration_ms >= 1000 * duty_range
    count = (duty_range + MAX_STEPS - 1) // MAX_STEPS if can_split else 1
    boundaries = [duty_range * index // count for index in range(count + 1)]
    return [
        Segment(end - start, max(1, duration_ms * (end - start) // duty_range))
        for start, end in zip(boundaries, boundaries[1:])
    ]


def nearest_cycle(ideal: Fraction) -> int:
    """Minimise absolute timing error; prefer the shorter duration on ties."""
    lower = max(1, min(MAX_CYCLES, ideal.numerator // ideal.denominator))
    upper = min(MAX_CYCLES, lower + 1)
    return min((lower, upper), key=lambda cycle: (abs(cycle - ideal), cycle))


def calculate(
    segments: list[Segment],
    frequency: int,
    method: str,
    reload_us: int = 0,
) -> Result:
    """Compare floor, nearest per segment, and nearest with error carried forward.

    The scale>1 fast-fade path deliberately retains IDF's scale selection:
    choosing a different cycle_num alone cannot fix that regime.
    reload_us is a hypothetical constant gap between library segments; it is
    included in the carried error, not deducted from each requested duration.
    """
    if frequency <= 0 or reload_us < 0:
        raise ValueError("frequency must be positive and reload overhead nonnegative")
    if method not in ("idf", "nearest", "carried"):
        raise ValueError(f"Unknown method: {method}")
    elapsed = Fraction(0)
    requested = Fraction(0)
    choices: list[int] = []
    fast_segments = 0
    for index, segment in enumerate(segments):
        duty_range, duration_ms = segment.duty_range, segment.duration_ms
        if duty_range <= 0 or duration_ms <= 0:
            raise ValueError("segment range and duration must be positive")
        if index:
            elapsed += Fraction(reload_us, 1000)
        total_cycles = frequency * duration_ms // 1000
        if total_cycles == 0:
            raise ValueError("sub-period fades use IDF's immediate-set path; not modelled")
        requested += duration_ms
        if total_cycles < duty_range:
            scale = min(MAX_SCALE, duty_range // total_cycles)
            cycle = 1
            fast_segments += 1
        else:
            scale = 1
            ideal = Fraction(frequency * duration_ms, 1000 * duty_range)
            if method == "idf":
                cycle = min(MAX_CYCLES, total_cycles // duty_range)
            else:
                if method == "carried":
                    ideal = (requested - elapsed) * frequency / (1000 * duty_range)
                cycle = nearest_cycle(ideal)
        elapsed += Fraction((duty_range // scale) * cycle * 1000, frequency)
        choices.append(cycle)
    return Result(elapsed, tuple(choices), fast_segments)


def cases(max_duty: int) -> list[tuple[str, list[Segment]]]:
    chunks = [
        Segment(max_duty * step // 6 - max_duty * (step - 1) // 6, 500)
        for step in range(1, 7)
    ]
    return [
        ("full-up-3000", [Segment(max_duty, 3000)]),
        ("full-down-3000", [Segment(max_duty, 3000)]),
        ("5-to-95pct-3000", [Segment(max_duty * 95 // 100 - max_duty * 5 // 100, 3000)]),
        ("six-500ms-chunks", chunks),
        ("150x20ms-full", [Segment(max_duty, 20)] * 150),
        ("short-full-200", [Segment(max_duty, 200)]),
        ("small-range-3000", [Segment(10, 3000)]),
    ]


def cycle_summary(cycles: tuple[int, ...]) -> str:
    return "/".join(f"{cycle}x{cycles.count(cycle)}" for cycle in sorted(set(cycles)))


def positive_int(value: str) -> int:
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def nonnegative_int(value: str) -> int:
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return number


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frequency", type=positive_int, help="override the default frequency/resolution matrix")
    parser.add_argument("--bits", type=positive_int, default=12, help="resolution with --frequency (default: 12)")
    parser.add_argument("--reload-us", type=nonnegative_int, default=0, help="assumed inter-segment gap (default: 0)")
    args = parser.parse_args()
    if args.bits > 20:
        parser.error("--bits must be between 1 and 20")
    configs = [(args.frequency, args.bits)] if args.frequency else [(1000, 10), (4000, 12), (8000, 12), (8000, 13)]
    print("Duty-step model only; excludes endpoint/edge/ISR costs. No existing library corrections.")
    print("Same split boundaries for all methods; sufficient queue capacity assumed.")
    print("Fast scale>1 fades retain IDF scale. Carried error is reset for each row.")
    print(f"Assumed gap between library segments: {args.reload_us} us")
    for frequency, bits in configs:
        print(f"\n{frequency} Hz / {bits} bits")
        print(f"{'fade':22} {'segs':>4} {'want ms':>9} {'IDF ms':>10} {'nearest ms':>11} {'error ms':>10} "
              f"{'carried ms':>11} {'error ms':>10}  nearest cycle_num x segments; carried")
        for name, logical_segments in cases((1 << bits) - 1):
            segments = [
                split
                for logical in logical_segments
                for split in split_fade(logical.duty_range, logical.duration_ms, frequency)
            ]
            expected = sum(segment.duration_ms for segment in logical_segments)
            baseline, nearest, carried = [
                calculate(segments, frequency, method, args.reload_us)
                for method in ("idf", "nearest", "carried")
            ]
            print(f"{name:22} {len(segments):4} {expected:9.3f} {float(baseline.duration_ms):10.3f} "
                  f"{float(nearest.duration_ms):11.3f} {float(nearest.duration_ms - expected):+10.3f} "
                  f"{float(carried.duration_ms):11.3f} {float(carried.duration_ms - expected):+10.3f}  "
                  f"{cycle_summary(nearest.cycles)}; {cycle_summary(carried.cycles)}"
                  f"{' [fast: unchanged]' if baseline.fast_segments else ''}")


if __name__ == "__main__":
    main()
