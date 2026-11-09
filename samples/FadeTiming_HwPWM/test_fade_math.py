import unittest
from fractions import Fraction

from fade_math import MAX_CYCLES, Segment, calculate, nearest_cycle, split_fade


class FadeMathTests(unittest.TestCase):
    def test_measured_fade_configuration(self) -> None:
        segments = split_fade(4095, 3000, 8000)
        self.assertEqual(segments, [Segment(819, 600)] * 5)
        floor = calculate(segments, 8000, "idf")
        nearest = calculate(segments, 8000, "nearest")
        carried = calculate(segments, 8000, "carried")
        self.assertEqual(floor.cycles, (5,) * 5)
        self.assertEqual(floor.duration_ms, Fraction(20475, 8))
        self.assertEqual(nearest.cycles, (6,) * 5)
        self.assertEqual(nearest.duration_ms, Fraction(24570, 8))
        self.assertEqual(sorted(carried.cycles), [5, 6, 6, 6, 6])
        self.assertLess(abs(carried.duration_ms - 3000), abs(nearest.duration_ms - 3000))

    def test_nearest_is_exhaustively_optimal(self) -> None:
        for ideal in (Fraction(1, 4), Fraction(11, 2), Fraction(4800, 819), Fraction(10239, 10), Fraction(4000)):
            chosen = nearest_cycle(ideal)
            self.assertEqual(abs(chosen - ideal), min(abs(cycle - ideal) for cycle in range(1, MAX_CYCLES + 1)))

    def test_exact_fade_is_unchanged(self) -> None:
        for method in ("idf", "nearest", "carried"):
            self.assertEqual(calculate([Segment(800, 600)], 8000, method).duration_ms, 600)

    def test_fast_fades_retain_idf_scale(self) -> None:
        segments = split_fade(4095, 20, 8000)
        self.assertEqual(len(segments), 1)
        for method in ("idf", "nearest", "carried"):
            result = calculate(segments, 8000, method)
            self.assertEqual(result.duration_ms, Fraction(163, 8))
            self.assertEqual(result.fast_segments, 1)

    def test_split_preserves_range_and_records_ms_truncation(self) -> None:
        segments = split_fade(3686, 3000, 8000)
        self.assertEqual(sum(segment.duty_range for segment in segments), 3686)
        self.assertTrue(all(segment.duty_range <= 1023 for segment in segments))
        self.assertLessEqual(sum(segment.duration_ms for segment in segments), 3000)

    def test_hardware_cycle_limit(self) -> None:
        self.assertEqual(calculate([Segment(1, 3000)], 8000, "nearest").cycles, (1023,))

    def test_reload_overhead(self) -> None:
        segments = [Segment(800, 600)] * 2
        self.assertEqual(calculate(segments, 8000, "nearest", 125).duration_ms, Fraction(9601, 8))

    def test_invalid_inputs(self) -> None:
        for duty, duration, freq in ((0, 3000, 8000), (100, 0, 8000), (100, 10, 0)):
            with self.assertRaises(ValueError):
                split_fade(duty, duration, freq)
        with self.assertRaises(ValueError):
            calculate([Segment(100, 1)], 1, "idf")
        with self.assertRaises(ValueError):
            calculate([Segment(100, 20)], 8000, "unknown")


if __name__ == "__main__":
    unittest.main()
