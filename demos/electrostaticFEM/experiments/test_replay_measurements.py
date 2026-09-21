"""Check MPI aggregation and rejection of invalid timing samples."""
import csv
from pathlib import Path
import tempfile
import unittest
from replay_measurements import read_replay


class ReplayMeasurements(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'configuration.txt').write_text('steps=2\nranks=2\ntolerance=1e-10\n')
        self.rows = []
        for rank, durations in enumerate([[3, 1, 1], [1, 4, 1]]):
            rows = []
            for method in ['plain', 'jacobi']:
                for solve, duration in enumerate(durations):
                    rows.append(dict(phase='replay', method=method, solve=solve, rank=rank,
                                     success=1, operator_setups=int(solve == 0),
                                     preconditioner_setups=int(method == 'jacobi' and solve == 0),
                                     rhs_norm=solve+1, iterations=10-solve, solve_seconds=duration,
                                     fresh_relative_residual=1e-11, operator_setup_seconds=.1 if solve == 0 else 0,
                                     preconditioner_setup_seconds=.2 if method == 'jacobi' and solve == 0 else 0,
                                     krylov_seconds=duration-.3, preconditioner_apply_seconds=0,
                                     operator_calls=11, preconditioner_operator_calls=0,
                                     triangular_calls=0, diagonal_calls=0, preconditioner_applications=0))
            self.rows.append(rows)
        self.write()

    def write(self):
        for rank, rows in enumerate(self.rows):
            with (self.root / f'solves-rank{rank}.csv').open('w') as f:
                writer = csv.DictWriter(f, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)

    def test_sum_before_maximum(self):
        _, summary, sequences, _ = read_replay(self.root)
        self.assertEqual(summary['plain']['total_seconds'], 6)
        self.assertEqual(summary['plain']['rank_total_min'], 5)
        self.assertEqual(summary['plain']['rank_total_mean'], 5.5)
        self.assertEqual(summary['plain']['recurring_seconds'], 2.5)
        self.assertEqual(sequences['plain']['cumulative_seconds'], [3, 5, 6])
        self.assertEqual(summary['plain']['total_iterations'], 27)

    def test_failed_solve_rejected(self):
        self.rows[1][2]['success'] = 0
        self.write()
        with self.assertRaisesRegex(ValueError, 'Failed/incomplete'):
            read_replay(self.root)

    def test_rebuild_rejected(self):
        self.rows[0][1]['operator_setups'] = 1
        self.write()
        with self.assertRaisesRegex(ValueError, 'rebuilt'):
            read_replay(self.root)

    def test_fresh_residual_rejected(self):
        self.rows[0][0]['fresh_relative_residual'] = 1e-4
        self.write()
        with self.assertRaisesRegex(ValueError, 'residual'):
            read_replay(self.root)


if __name__ == '__main__':
    unittest.main()
