"""Validated per-rank replay measurements, shared by mesh-study analysis."""
import csv
from pathlib import Path
import numpy as np


def read_replay(directory):
    directory = Path(directory)
    config = dict(line.split('=', 1) for line in (directory / 'configuration.txt').read_text().splitlines())
    count = int(config['steps']) + 1
    files = [directory / f'solves-rank{rank}.csv' for rank in range(int(config['ranks']))]
    rank_rows = []
    for path in files:
        with path.open() as stream:
            rank_rows.append(list(csv.DictReader(stream)))
    methods = list(dict.fromkeys(r['method'] for r in rank_rows[0] if r['phase'] == 'replay'))
    summaries, sequences = {}, {}
    reference = None
    for method in methods:
        rows = [[r for r in rank if r['phase'] == 'replay' and r['method'] == method] for rank in rank_rows]
        for rank in rows:
            rank.sort(key=lambda r: int(r['solve']))
            if [int(r['solve']) for r in rank] != list(range(count)) or any(r['success'] != '1' for r in rank):
                raise ValueError(f'Failed/incomplete replay: {directory}, {method}')
            if sum(int(r['operator_setups']) for r in rank) != 1:
                raise ValueError('Fixed full-reuse operator was rebuilt')
            if sum(int(r['preconditioner_setups']) for r in rank) != (method != 'plain'):
                raise ValueError('Fixed full-reuse preconditioner was rebuilt')
        signatures = np.array([[float(r['rhs_norm']) for r in rank] for rank in rows])
        if reference is None:
            reference = signatures[0]
        np.testing.assert_allclose(signatures, np.broadcast_to(reference, signatures.shape), rtol=2e-11, atol=1e-13)
        times = np.array([[float(r['solve_seconds']) for r in rank] for rank in rows])
        iterations = np.array([[int(r['iterations']) for r in rank] for rank in rows])
        np.testing.assert_array_equal(iterations, np.broadcast_to(iterations[0], iterations.shape))
        if not np.isfinite(times).all() or np.any(times < 0):
            raise ValueError('Invalid timing samples')
        totals = times.sum(axis=1)
        critical = int(np.argmax(totals))
        selected = rows[critical]
        def total(key):
            return sum(float(r[key]) for r in selected)
        residuals = np.array([float(r['fresh_relative_residual']) for rank in rows for r in rank])
        residual = float(residuals.max())
        if not np.isfinite(residuals).all() or residual > max(20*float(config['tolerance']), 5e-11):
            raise ValueError('Fresh residual acceptance failed')
        summaries[method] = dict(
            solves=count, total_iterations=int(iterations[0].sum()), iterations_per_solve=float(iterations[0].mean()),
            recurring_iterations=float(iterations[0, 1:].mean()),
            total_seconds=float(totals.max()), rank_total_min=float(totals.min()), rank_total_mean=float(totals.mean()),
            recurring_seconds=float(times[:, 1:].sum(axis=1).max())/(count-1),
            first_seconds=float(times[:, 0].max()),
            first_setup_seconds=max(float(rank[0]['operator_setup_seconds']) + float(rank[0]['preconditioner_setup_seconds']) for rank in rows),
            operator_setup_seconds=total('operator_setup_seconds'), preconditioner_setup_seconds=total('preconditioner_setup_seconds'),
            krylov_seconds=total('krylov_seconds'), preconditioner_apply_seconds=total('preconditioner_apply_seconds'),
            operator_calls=total('operator_calls'), preconditioner_operator_calls=total('preconditioner_operator_calls'),
            triangular_calls=total('triangular_calls'), diagonal_calls=total('diagonal_calls'),
            preconditioner_applications=total('preconditioner_applications'), max_fresh_residual=residual)
        sequences[method] = dict(cumulative_seconds=times.cumsum(axis=1).max(axis=0).tolist(),
                                 iterations=iterations[0].tolist())
    return config, summaries, sequences, reference


def read_pic_steps(directory, ranks):
    """Direct canonical CG step totals; sum locally before rank aggregation."""
    totals = []
    for rank in range(ranks):
        with (Path(directory) / f'steps-rank{rank}.csv').open() as stream:
            rows = list(csv.DictReader(stream))
        steps = [r for r in rows if r['stage'] == 'step_total' and int(r['step']) > 0]
        totals.append(sum(float(r['seconds']) for r in steps))
    return max(totals)
