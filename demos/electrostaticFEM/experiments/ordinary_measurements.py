"""Validate initialization + ordinary PIC steps for FEM and Alpine (stdlib only)."""
import csv
import json
import math
from pathlib import Path
import statistics

from fd_pcg_variants import DEFAULT_VARIANTS, VARIANTS

def read_rows(path):
    with Path(path).open() as stream:
        return list(csv.DictReader(stream))

def inspect_run(directory, job, settings):
    directory = Path(directory)
    config = dict(line.split('=', 1) for line in (directory/'configuration.txt').read_text().splitlines())
    app, count, ranks = settings['application'], settings['steps'] + 1, settings['ranks']
    variant = VARIANTS[job['variant']]
    assert 'replay' not in config, 'Obsolete replay-capable output: rebuild the application'
    for key, expected in [('dimension', job['dimension']), ('particles', job['particles']),
                          ('steps', settings['steps']), ('ranks', ranks),
                          ('max_iterations', settings['max_iterations'])]:
        assert int(config[key]) == expected, key
    assert config['backend'] == settings['expected_backend'], 'Wrong backend'
    assert config['pic_method'] == variant['method']
    for key, expected in dict(initial_guess='previous', initial_condition='alpine_product',
                              load_balancing='disabled', timer_recording='summary',
                              timer_sample_limit='0', seed='42').items():
        assert config[key] == expected, key
    assert float(config['tolerance']) == settings['tolerance']
    assert float(config['dt']) == settings['dt']
    assert float(config['alpha']) == .05
    assert all(int(config['cells_%d' % d]) == job['cells'] for d in range(job['dimension']))
    assert all(math.isclose(float(config['length_%d' % d]), 4*math.pi, rel_tol=1e-15)
               for d in range(job['dimension']))
    if app == 'fem':
        assert int(config['order']) == job['order']
        assert int(config['quadrature_points']) == job['order'] + 1
        for key, expected in dict(interpolation_nodes='gll', stiffness='constant_preserving',
                                  reuse='full', timer_level='coarse').items():
            assert config[key] == expected, key
        assert int(config['chebyshev_degree']) == variant.get('degree', 8)
        assert int(config['gauss_seidel_inner_iterations']) == variant.get('inner', 2)
        assert int(config['gauss_seidel_outer_iterations']) == variant.get('outer', 2)
    else:
        assert config['application'] == 'alpine' and config['warmup_solves'] == '0'
        args = variant['args']
        assert [config['preconditioner_arg_%d' % i] for i in range(len(args))] == args
    gate = max(20 * settings['tolerance'], 5e-11)
    rows_by_rank, steps_by_rank = [], []
    timers = read_rows(directory/'timer-summary.csv')
    assert {int(r['rank']) for r in timers} == set(range(ranks))
    assert len({(r['rank'], r['timer_name']) for r in timers}) == len(timers)
    assert all(r['stored_samples'] == '0' for r in timers)
    assert not (directory/'timers.csv').exists()
    for rank in range(ranks):
        rows = read_rows(directory/('solves-rank%d.csv' % rank))
        assert len(rows) == count and [int(r['solve']) for r in rows] == list(range(count)), 'Incomplete solves'
        assert [r['phase'] for r in rows] == ['initial'] + ['step'] * (count - 1)
        assert all(r['method'] == variant['method'] and int(r['rank']) == rank and r['success'] == '1' for r in rows)
        assert all(0 <= int(r['iterations']) < settings['max_iterations'] for r in rows)
        assert all(math.isfinite(float(r['fresh_relative_residual']))
                   and 0 <= float(r['fresh_relative_residual']) <= gate for r in rows)
        assert all(math.isfinite(float(v)) and float(v) >= 0
                   for r in rows for k, v in r.items() if k.endswith('_seconds') or k in ('rhs_norm', 'recursive_residual'))
        if rank:
            assert [r['iterations'] for r in rows] == [r['iterations'] for r in rows_by_rank[0]]
            assert all(math.isclose(float(a['rhs_norm']), float(b['rhs_norm']), rel_tol=2e-11, abs_tol=1e-13)
                       for a, b in zip(rows, rows_by_rank[0]))
        if app == 'fem':
            assert [int(r['operator_setups']) for r in rows] == [1] + [0]*(count-1)
            assert [int(r['preconditioner_setups']) for r in rows] == [int(variant['method'] != 'plain')] + [0]*(count-1)
            if variant['method'] == 'plain':
                assert all(r['preconditioner_applications'] == '0' for r in rows)
            counts = {r['timer_name']: int(r['count']) for r in timers if int(r['rank']) == rank}
            assert counts['evaluateAx'] == counts['evaluateAx: outer loop'] == sum(int(r['operator_calls']) for r in rows) + count
        else:
            assert all(float(r['solver_setup_seconds']) == 0 for r in rows[1:])
            if config.get('preconditioner_lifecycle_metrics') == '1':
                setup_counts = [int(r['preconditioner_setups']) for r in rows]
                levels = [int(r['preconditioner_levels']) for r in rows]
                coarsest = [int(r['preconditioner_coarsest_local_extent']) for r in rows]
                if variant['method'] == 'multigrid':
                    assert setup_counts == [1] * count, 'Multigrid hierarchy was rebuilt'
                    assert min(levels) > 0 and min(coarsest) > 0
                    assert len(set(levels)) == len(set(coarsest)) == 1
                else:
                    assert setup_counts == [0] * count
                    assert levels == [0] * count and coarsest == [0] * count
        steps = read_rows(directory/('steps-rank%d.csv' % rank))
        assert not any(r['stage'] == 'capture_load' for r in steps)
        totals = [r for r in steps if r['stage'] == 'step_total']
        assert [int(r['step']) for r in totals] == list(range(1, count)), 'Incomplete timesteps'
        assert all(int(r['rank']) == rank and math.isfinite(float(r['seconds'])) and float(r['seconds']) >= 0 for r in steps)
        steps_by_rank.append(sum(float(r['seconds']) for r in totals))
        rows_by_rank.append(rows)
    memory = []
    if settings.get('rank_monitor'):
        for rank in range(ranks):
            item = json.loads((directory/('memory-rank%d.json' % rank)).read_text())
            assert item['rank'] == rank and item['passed'] and item['samples'] > 0, 'Memory admission failed'
            memory.append(item)
        assert len({(m['host'], m['device_uuid']) for m in memory}) == ranks, 'GPU assignment overlaps'
    physics = read_rows(directory/'physics.csv')
    assert [int(r['step']) for r in physics] == list(range(count)), 'Incomplete physics history'
    volume = (4*math.pi)**job['dimension']
    for row in physics:
        assert all(math.isfinite(float(v)) for v in row.values())
        assert abs(float(row['mean_potential'])) <= 1e-11
        if app == 'fem':
            assert abs(float(row['net_charge']))/volume <= 1e-11
            energy = float(row['field_energy'])
            assert energy >= 0 and abs(energy-float(row['weak_field_energy']))/max(energy, 1e-12) <= max(100*settings['tolerance'], 1e-10)
        else:
            assert abs(float(row['mean_charge_density'])) <= 1e-11
            assert float(row['Ex_field_energy']) >= 0
    def recurring(key):
        return max(sum(float(r[key]) for r in rows[1:]) for rows in rows_by_rank)/(count-1)
    setup_keys = ['operator_setup_seconds', 'preconditioner_setup_seconds'] if app == 'fem' else ['solver_setup_seconds']
    result = dict(recurring_solve_seconds=recurring('solve_seconds'),
                recurring_step_seconds=max(steps_by_rank)/(count-1),
                recurring_iterations=statistics.mean(int(r['iterations']) for r in rows_by_rank[0][1:]),
                initial_solve_seconds=max(float(rows[0]['solve_seconds']) for rows in rows_by_rank),
                setup_seconds=max(sum(float(rows[0][k]) for k in setup_keys) for rows in rows_by_rank),
                all_solve_seconds=max(sum(float(r['solve_seconds']) for r in rows) for rows in rows_by_rank),
                max_fresh_residual=max(float(r['fresh_relative_residual']) for rows in rows_by_rank for r in rows),
                rank_solve_records=count*ranks)
    diagnostic, initialization = [], []
    for rank in range(ranks):
        steps = read_rows(directory/('steps-rank%d.csv' % rank))
        initialization.append(sum(float(r['seconds']) for r in steps
                                  if r['stage'] == 'initialization' and int(r['step']) == 0))
        diagnostic.append(sum(float(r['seconds']) for r in steps
                              if r['stage'] == 'diagnostics' and int(r['step']) > 0)/(count-1))
    result['initialization_seconds'] = max(initialization) if app == 'fem' else None
    result['recurring_diagnostic_seconds'] = max(diagnostic) if app == 'fem' else None
    if memory:
        result['sampled_device_peak_gib'] = max(m['sampled_max_used_bytes'] for m in memory)/2**30
        result['host_process_peak_gib'] = max(m['host_process_maxrss_bytes'] for m in memory)/2**30
    return result
