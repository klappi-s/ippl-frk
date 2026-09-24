"""Recording parity checks allow GPU iteration jitter, but retain numerical gates."""
import csv
from pathlib import Path
import tempfile
import unittest

from check_compact_timers import check


def write_rows(path, rows):
    with path.open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def alter(path, change):
    with path.open() as stream:
        rows = list(csv.DictReader(stream))
    change(rows)
    write_rows(path, rows)


class CompactTimers(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.summary, self.raw = [self.root / name for name in ('summary', 'raw')]
        for directory, iterations in ((self.summary, 50), (self.raw, 51)):
            directory.mkdir()
            raw = directory == self.raw
            (directory / 'configuration.txt').write_text(
                'ranks=1\ntolerance=1e-10\ntimer_recording=%s\ntimer_sample_limit=%d\n' %
                (directory.name, 4 if raw else 0))
            write_rows(directory / 'solves-rank0.csv', [dict(phase='step', method='plain',
                       solve=1, iterations=iterations, operator_setups=0, preconditioner_setups=0,
                       operator_calls=iterations+1, success=1, fresh_relative_residual=9e-11, rhs_norm=1)])
            write_rows(directory / 'physics.csv', [dict(step=1, time=0.05, field_energy=0.03,
                       weak_field_energy=0.03, mean_potential=0, net_charge=0)])
            write_rows(directory / 'timer-summary.csv', [dict(timer_name=timer, rank=0,
                       count=iterations+2, total_seconds=(iterations+2)*0.01, min_seconds=0.01,
                       max_seconds=0.01, mean_seconds=0.01, stddev_seconds=0,
                       stored_samples=4 if raw else 0)
                       for timer in ('evaluateAx', 'evaluateAx: outer loop')])
            if raw:
                write_rows(directory / 'timers.csv', [dict(timer_name=timer, rank=0,
                           measurement_id=i, duration_seconds=0.01)
                           for timer in ('evaluateAx', 'evaluateAx: outer loop') for i in range(4)])

    def test_iteration_jitter_is_reported(self):
        result = check(self.summary, self.raw, 1, 4)
        self.assertEqual(len(result['iteration_differences']), 1)
        self.assertEqual(result['iteration_differences'][0]['summary'], 50)
        self.assertEqual(result['iteration_differences'][0]['raw'], 51)
        self.assertEqual(len(result['timer_count_differences']), 2)

    def test_missing_operator_event_fails(self):
        alter(self.raw / 'timer-summary.csv', lambda rows: rows[0].update(count=52))
        with self.assertRaisesRegex(AssertionError, 'Operator timer count'):
            check(self.summary, self.raw, 1, 4)

    def test_bad_residual_still_fails(self):
        alter(self.raw / 'solves-rank0.csv', lambda rows: rows[0].update(fresh_relative_residual=3e-9))
        with self.assertRaises(AssertionError):
            check(self.summary, self.raw, 1, 4)

    def test_changed_load_still_fails(self):
        alter(self.raw / 'solves-rank0.csv', lambda rows: rows[0].update(rhs_norm=1.001))
        with self.assertRaises(AssertionError):
            check(self.summary, self.raw, 1, 4)

    def test_changed_physics_fails(self):
        alter(self.raw / 'physics.csv', lambda rows: rows[0].update(field_energy=0.031))
        with self.assertRaisesRegex(AssertionError, 'Physics histories'):
            check(self.summary, self.raw, 1, 4)

    def test_missing_raw_sample_fails(self):
        alter(self.raw / 'timers.csv', lambda rows: rows.pop())
        with self.assertRaises(AssertionError):
            check(self.summary, self.raw, 1, 4)


if __name__ == '__main__':
    unittest.main()
