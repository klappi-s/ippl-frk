"""Guard allocation coverage, staging, memory admission and placement analysis."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

import analyze_mesh_sweep as analyzer
import analyze_fd_pcg as pcg_analyzer
import rank_memory
import submit_campaigns as submitter
import submit_fd_pcg_campaign as pcg_submitter
import submit_fem_campaign as fem_submitter
from fd_pcg_variants import (
    RICHARDSON_ALT_ITERATIONS,
    RICHARDSON_GLOBAL_ITERATIONS,
    RICHARDSON_LOCAL_ITERATIONS,
    SMOKE_VARIANTS,
    TUNING_BUNDLES,
    VARIANTS,
)


class PhaseOne(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def submit(self, campaign, *extra):
        app = 'fem' if campaign in ('fem-small', 'fem-large') else 'alpine'
        args = ['submit_campaigns.py', '--application', app, '--campaign', campaign,
                '--output-root', str(self.root/campaign), '--dry-run'] + list(extra)
        output = io.StringIO()
        with patch.object(sys, 'argv', args), contextlib.redirect_stdout(output): submitter.main()
        plans = [json.loads(p.read_text()) for p in (self.root/campaign/'plans').glob('*.json')]
        return plans, output.getvalue()

    def test_full_matrices_populations_and_first_stage(self):
        for campaign, expected, placements, maximum in [('fem-small',243,9,2097152), ('fd-large',45,5,1073741824)]:
            plans, output = self.submit(campaign)
            self.assertEqual(len(plans), placements)
            tasks = [t for p in plans for t in p['tasks']]
            self.assertEqual(len(tasks), expected)
            self.assertEqual(max(t['particles'] for t in tasks), maximum)
            for t in tasks:
                self.assertEqual(t['particles'], 8*t['cells']**3)
                self.assertEqual(t['arguments'][t['arguments'].index('--variants')+1], 'cg')
                self.assertLess(t['memory']['screening_estimate_gib'], 70)
            self.assertEqual(sum(int(line.split(': ')[1].split()[0]) for line in output.splitlines()
                                 if 'pending allocations /' in line), expected//3)
        large, _ = self.submit('fd-large')
        self.assertNotIn('1x1', [p['placement'] for p in large])

    def test_select_cells_does_not_change_recipe(self):
        before, output = self.submit('fd-large', '--placements', '8x4', '--select-cells', '128')
        self.assertIn('1 pending allocations', output)
        after, output = self.submit('fd-large', '--placements', '8x4', '--select-cells', '256')
        self.assertEqual(before, after)
        self.assertIn('1 pending allocations', output)

    def test_large_fem_is_separate_and_keeps_screened_case_visible(self):
        plans, output = self.submit('fem-large', '--placements', '1x2',
                                    '--select-cells', '128')
        self.assertEqual(len(plans), 1)
        tasks = plans[0]['tasks']
        self.assertEqual(len(tasks), 18)
        self.assertEqual({t['cells'] for t in tasks}, {128, 256})
        self.assertEqual({t['order'] for t in tasks}, {1, 2, 3})
        excluded = [t for t in tasks if t['memory'].get('screening_excluded')]
        self.assertEqual({(t['order'], t['cells']) for t in excluded}, {(3, 256)})
        self.assertIn('3 pending allocations', output)
        after, output = self.submit('fem-large', '--placements', '1x2',
                                    '--select-cells', '256')
        self.assertEqual(plans, after)
        self.assertIn('2 pending allocations', output)
        self.assertIn('excluded by pinned memory screen', output)
        after, output = self.submit('fem-large', '--placements', '1x2',
                                    '--select-cells', '256', '--select-orders', '1')
        self.assertEqual(plans, after)
        self.assertIn('1 pending allocations', output)
        self.assertNotIn('excluded by pinned memory screen', output)
        _, output = self.submit('fem-large', '--placements', '8x4',
                                '--select-cells', '128')
        self.assertIn('3 pending allocations', output)
        self.assertIn('--time=08:00:00', output)
        self.assertIn('FEM_RUN_TIMEOUT=27000', output)
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.submit('fem-small', '--placements', '8x4', '--cells-3d', '128')

    def test_fem_campaign_separates_placement_and_tuning_outputs(self):
        calls = []
        placement = [
            'submit_fem_campaign.py', '--root', str(self.root/'study'),
            '--phase', 'placement', '--cells', '16', '--placements', '1x2',
            '--orders', '1', '2', '3', '--stage', 'first', '--dry-run',
        ]
        with patch.object(sys, 'argv', placement), \
             patch.object(fem_submitter.subprocess, 'run',
                          side_effect=lambda command, **kwargs: calls.append(command) or Mock(returncode=0)), \
             contextlib.redirect_stdout(io.StringIO()):
            fem_submitter.main()
        self.assertEqual(len(calls), 1)
        command = calls[0]
        self.assertIn('/placement/cg/n16-1x2', command[command.index('--output-root') + 1])
        self.assertEqual(command[command.index('--variants') + 1], 'cg')
        self.assertEqual(command[command.index('--campaign') + 1], 'fem-small')

        calls.clear()
        tuning = [
            'submit_fem_campaign.py', '--root', str(self.root/'study'),
            '--phase', 'tune', '--cells', '128', '--placements', '8x4',
            '--orders', '2', '3', '--variants', 'cg', 'jacobi', 'cheb4',
            '--stage', 'first', '--dry-run',
        ]
        with patch.object(sys, 'argv', tuning), \
             patch.object(fem_submitter.subprocess, 'run',
                          side_effect=lambda command, **kwargs: calls.append(command) or Mock(returncode=0)), \
             contextlib.redirect_stdout(io.StringIO()):
            fem_submitter.main()
        self.assertEqual(len(calls), 1)
        command = calls[0]
        self.assertIn('/tune/custom-', command[command.index('--output-root') + 1])
        self.assertEqual(command[command.index('--campaign') + 1], 'fem-large')
        self.assertEqual(command[command.index('--variants') + 1:command.index('--steps')],
                         ['cg', 'jacobi', 'cheb4'])

    def test_named_fem_tuning_bundles_have_stable_paths(self):
        calls = []
        args = [
            'submit_fem_campaign.py', '--root', str(self.root/'study'),
            '--phase', 'tune', '--cells', '64', '--placements', '1x1',
            '--orders', '1', '2', '3', '--bundle', 'chebyshev-low',
            '--stage', 'first', '--dry-run',
        ]
        with patch.object(sys, 'argv', args), \
             patch.object(fem_submitter.subprocess, 'run',
                          side_effect=lambda command, **kwargs: calls.append(command) or Mock(returncode=0)), \
             contextlib.redirect_stdout(io.StringIO()):
            fem_submitter.main()
        self.assertEqual(len(calls), 1)
        command = calls[0]
        self.assertIn('/tune/chebyshev-low/n64-1x1',
                      command[command.index('--output-root') + 1])
        self.assertEqual(command[command.index('--variants') + 1:command.index('--steps')],
                         ['cg', 'cheb2', 'cheb4'])

    def test_explicit_fem_tuning_label_is_validated(self):
        calls = []
        args = [
            'submit_fem_campaign.py', '--root', str(self.root/'study'),
            '--phase', 'tune', '--cells', '64', '--placements', '1x1',
            '--variants', 'cg', 'jacobi', '--label', 'jacobi-check', '--dry-run',
        ]
        with patch.object(sys, 'argv', args), \
             patch.object(fem_submitter.subprocess, 'run',
                          side_effect=lambda command, **kwargs: calls.append(command) or Mock(returncode=0)), \
             contextlib.redirect_stdout(io.StringIO()):
            fem_submitter.main()
        self.assertIn('/tune/jacobi-check/', calls[0][calls[0].index('--output-root') + 1])
        args[args.index('jacobi-check')] = 'Bad Label'
        with patch.object(sys, 'argv', args), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                fem_submitter.main()

    def test_fem_preset_accepts_only_fem_preconditioners(self):
        plans, _ = self.submit('fem-small', '--placements', '1x2',
                               '--cells-3d', '16', '--orders', '3',
                               '--variants', 'cg', 'jacobi', 'gs21', 'cheb4')
        self.assertEqual(list(plans[0]['variants']), ['cg', 'jacobi', 'gs21', 'cheb4'])
        args = ['submit_campaigns.py', '--application', 'fem', '--campaign', 'fem-small',
                '--output-root', str(self.root/'invalid-fem'), '--placements', '1x2',
                '--cells-3d', '16', '--variants', 'cg', 'newton1', '--dry-run']
        with patch.object(sys, 'argv', args), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                submitter.main()

    def test_campaign_wall_limits_follow_measured_scale(self):
        _, fem = self.submit('fem-small', '--placements', '4x2')
        self.assertIn('--time=01:00:00', fem)
        self.assertIn('FEM_RUN_TIMEOUT=3000', fem)
        _, fd = self.submit('fd-large', '--placements', '8x4')
        self.assertIn('--time=00:59:00', fd)
        self.assertIn('FEM_RUN_TIMEOUT=3000', fd)
        _, explicit = self.submit('fem-small', '--placements', '4x2',
                                  '--time', '02:00:00', '--timeout', '6000')
        self.assertIn('--time=02:00:00', explicit)
        self.assertIn('FEM_RUN_TIMEOUT=6000', explicit)

    def test_repeats_wait_for_successful_first_case(self):
        plans, _ = self.submit('fem-small', '--placements', '2x2')
        _, output = self.submit('fem-small', '--placements', '2x2', '--stage', 'repeats')
        self.assertNotIn('sbatch', output)
        task = plans[0]['tasks'][0]
        root = Path(task['output']); (root/'jobs').mkdir(parents=True)
        (root/'campaign.json').write_text(json.dumps(dict(schema=2, jobs=[dict(name='cg')])))
        state = root/'jobs/cg.json'
        state.write_text(json.dumps(dict(returncode=0, validation_error='')))
        _, output = self.submit('fem-small', '--placements', '2x2', '--stage', 'repeats')
        self.assertIn('2 pending allocations', output)
        state.write_text(json.dumps(dict(returncode=125, validation_error='memory budget')))
        _, output = self.submit('fem-small', '--placements', '2x2', '--stage', 'repeats')
        self.assertNotIn('sbatch', output)
        _, output = self.submit('fem-small', '--placements', '2x2', '--stage', 'first')
        self.assertIn('inspect before --retry-failed', output)
        self.assertIn('8 pending allocations', output)

    def test_all_stage_queues_independent_repeats_and_resumes_successes(self):
        plans, output = self.submit('fem-small', '--placements', '8x1',
                                    '--cells-3d', '64', '--stage', 'all',
                                    '--concurrent', '3')
        self.assertEqual(len(plans[0]['tasks']), 9)
        self.assertIn('--array=0,1,2,3,4,5,6,7,8%3', output)
        self.assertNotIn('waiting for successful first run', output)
        task = plans[0]['tasks'][0]
        run = Path(task['output'])
        (run/'jobs').mkdir(parents=True)
        (run/'campaign.json').write_text(json.dumps(dict(schema=2, jobs=[dict(name='cg')])))
        state = run/'jobs/cg.json'
        state.write_text(json.dumps(dict(returncode=0, validation_error='')))
        _, output = self.submit('fem-small', '--placements', '8x1',
                                '--cells-3d', '64', '--stage', 'all', '--concurrent', '3')
        self.assertIn('--array=1,2,3,4,5,6,7,8%3', output)
        state.write_text(json.dumps(dict(returncode=1, validation_error='residual')))
        _, output = self.submit('fem-small', '--placements', '8x1',
                                '--cells-3d', '64', '--stage', 'all', '--concurrent', '3')
        self.assertIn('inspect before --retry-failed', output)
        self.assertIn('--array=1,2,3,4,5,6,7,8%3', output)
        self.assertNotIn('waiting for successful first run', output)

    def test_extended_fem_placement_arrays_have_all_orders_and_repeats(self):
        for campaign, cells, placements in (
                ('fem-small', 64, ['8x1', '8x2', '8x4', '16x1', '16x2', '16x4']),
                ('fem-large', 128, ['2x1', '4x1', '8x1', '16x1',
                                     '2x2', '4x2', '8x2', '16x2', '16x4'])):
            plans, output = self.submit(campaign, '--placements', *placements,
                                        '--cells-3d', str(cells), '--stage', 'all',
                                        '--concurrent', '3')
            self.assertEqual({p['placement'] for p in plans}, set(placements))
            self.assertEqual(output.count('--array=0,1,2,3,4,5,6,7,8%3'), len(placements))
            for plan in plans:
                tasks = plan['tasks']
                self.assertEqual({(t['order'], t['repeat']) for t in tasks},
                                 {(p, r) for p in (1, 2, 3) for r in (0, 1, 2)})
                self.assertTrue(all(t['cells'] == cells and t['particles'] == 8*cells**3
                                    and t['memory']['screening_estimate_gib'] < 70
                                    for t in tasks))
                nodes, per_node = map(int, plan['placement'].split('x'))
                self.assertIn('--nodes=' + str(nodes), output)
                self.assertIn('--ntasks=' + str(nodes*per_node), output)
                self.assertIn('--ntasks-per-node=' + str(per_node), output)

    def test_fem_entry_point_forwards_extended_placements_and_all_stage(self):
        for cells, placements in ((64, ['8x1', '8x2', '8x4', '16x1', '16x2', '16x4']),
                                  (128, ['2x1', '4x1', '8x1', '16x1',
                                         '2x2', '4x2', '8x2', '16x2', '16x4'])):
            calls = []
            argv = ['submit_fem_campaign.py', '--root', str(self.root/'study'),
                    '--phase', 'placement', '--cells', str(cells),
                    '--placements', *placements, '--stage', 'all',
                    '--concurrent', '3', '--dry-run']
            with patch.object(sys, 'argv', argv), \
                 patch.object(fem_submitter.subprocess, 'run',
                              side_effect=lambda command, **kwargs: calls.append(command)), \
                 contextlib.redirect_stdout(io.StringIO()):
                fem_submitter.main()
            self.assertEqual(len(calls), len(placements))
            for command, placement in zip(calls, placements):
                self.assertEqual(command[command.index('--stage') + 1], 'all')
                self.assertEqual(command[command.index('--concurrent') + 1], '3')
                self.assertTrue(command[command.index('--output-root') + 1].endswith(
                    '/placement/cg/n%d-%s' % (cells, placement)))

    def test_fem_allocation_driver_accepts_every_new_topology(self):
        driver = Path(__file__).with_name('gh200_mesh.slurm')
        for nodes in (2, 4, 8, 16):
            for per_node in (1, 2, 4):
                env = dict(os.environ, FEM_ROOT=str(self.root), FEM_APPLICATION='fem',
                           SLURM_NTASKS=str(nodes*per_node), SLURM_JOB_NUM_NODES=str(nodes),
                           SLURM_JOB_ID='test', FEM_OUTPUT=str(self.root/'driver'),
                           FEM_EXECUTABLE=str(self.root/'missing-executable'))
                result = subprocess.run(['bash', str(driver)], env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        universal_newlines=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn('Missing CUDA executable', result.stdout)
                self.assertNotIn('Supported placements:', result.stdout)

    def test_changed_recipe_and_wrong_particle_policy_rejected(self):
        self.submit('fd-large', '--placements', '8x4')
        with self.assertRaisesRegex(SystemExit, 'recipe changed'):
            self.submit('fd-large', '--placements', '8x4', '--dt', '.1')
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit): self.submit('fd-large', '--particles', '32768')

    def test_submission_avoids_login_environment_retrieval(self):
        args = ['submit_campaigns.py', '--application', 'fem', '--campaign', 'fem-small',
                '--output-root', str(self.root/'fem-small'), '--placements', '1x4',
                '--timeout', '12345']
        inherited = dict(FEM_RUN_TIMEOUT='17', SBATCH_GET_USER_ENV='1',
                         SLURM_GET_USER_ENV='1', SLURM_EXPORT_ENV='NONE')
        with patch.object(sys, 'argv', args), patch.dict(os.environ, inherited), \
             patch.object(submitter.subprocess, 'run', return_value=Mock(stdout='123456\n')) as launch, \
             contextlib.redirect_stdout(io.StringIO()):
            submitter.main()
        launch.assert_called_once()
        command = launch.call_args[0][0]
        env = launch.call_args[1]['env']
        self.assertEqual([v for v in command if v.startswith('--export=')], ['--export=ALL'])
        self.assertEqual(env['FEM_RUN_TIMEOUT'], '12345.0')
        self.assertEqual(env['SLURM_EXPORT_ENV'], 'ALL')
        self.assertNotIn('SBATCH_GET_USER_ENV', env)
        self.assertNotIn('SLURM_GET_USER_ENV', env)
        state = json.loads((self.root/'fem-small/plans/1x4-submission.json').read_text())
        self.assertEqual(state['environment_overrides']['FEM_RUN_TIMEOUT'], '12345.0')

    def test_canceled_prior_array_can_be_replaced_but_other_queue_errors_stop(self):
        args = ['submit_campaigns.py', '--application', 'alpine', '--campaign', 'fd-large',
                '--output-root', str(self.root/'fd-large'), '--placements', '8x4',
                '--select-cells', '256']
        state_path = self.root/'fd-large/plans/8x4-submission.json'
        state_path.parent.mkdir(parents=True)
        state_path.write_text(json.dumps(dict(job_id='123456')))
        invalid = Mock(returncode=1, stdout='', stderr='slurm_load_jobs error: Invalid job id specified\n')
        submitted = Mock(returncode=0, stdout='234567\n')
        with patch.object(sys, 'argv', args), \
             patch.object(submitter.subprocess, 'run', side_effect=[invalid, submitted]) as launch, \
             contextlib.redirect_stdout(io.StringIO()):
            submitter.main()
        self.assertEqual(launch.call_count, 2)
        self.assertEqual(launch.call_args_list[0].args[0][:4], ['squeue', '-h', '-j', '123456'])
        self.assertEqual(launch.call_args_list[1].args[0][0], 'sbatch')
        self.assertEqual(json.loads(state_path.read_text())['job_id'], '234567')

        unavailable = Mock(returncode=1, stdout='', stderr='slurm_load_jobs error: Socket timed out\n')
        with patch.object(sys, 'argv', args), \
             patch.object(submitter.subprocess, 'run', return_value=unavailable) as launch, \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(SystemExit, 'Cannot check prior allocation'):
                submitter.main()
        launch.assert_called_once()
        self.assertEqual(json.loads(state_path.read_text())['job_id'], '234567')

    def test_equal_gpu_counts_keep_distinct_recorded_placements(self):
        for nodes, per_node in [(1,4),(2,2),(4,1)]:
            root = self.root/('arbitrary-%d' % nodes); (root/'jobs').mkdir(parents=True)
            config = dict(application='fem', nodes=nodes, gpus_per_node=per_node, ranks=4,
                executable_sha256='same', driver_sha256='same', reader_sha256='same', steps=10,
                tolerance=1e-10, dt=.05, max_iterations=10000, expected_backend='Cuda', variant_definitions={})
            job = dict(name='cg', dimension=3, order=1, cells=16, particles=32768, variant='cg', repeat=0)
            (root/'campaign.json').write_text(json.dumps(dict(schema=2, config=config, jobs=[job])))
            (root/'jobs/cg.json').write_text(json.dumps(dict(returncode=0, directory='data', wall_seconds=20)))
        measured = dict(recurring_solve_seconds=1., recurring_step_seconds=2., recurring_iterations=5., setup_seconds=.1)
        with patch.object(analyzer, 'inspect_run', return_value=measured):
            means = analyzer.analyze([self.root], self.root/'analysis')
        self.assertEqual({m['placement'] for m in means}, {'1x4','2x2','4x1'})
        self.assertEqual({m['reserved_node_seconds'] for m in means}, {20,40,80})

    def test_memory_guard_blocks_launch_above_free_budget(self):
        gpu = Mock(uuid='GPU-test', pci='test')
        gpu.read.return_value = dict(total=96*2**30, used=16*2**30, free=80*2**30)
        argv = ['rank_memory.py', '--output', str(self.root), '--estimate-gib', '65', '--', '/app']
        with patch.object(sys, 'argv', argv), patch.dict(os.environ, SLURM_PROCID='0'), \
             patch.object(rank_memory, 'DeviceMemory', return_value=gpu), \
             patch.object(rank_memory.subprocess, 'Popen') as launch:
            with self.assertRaises(SystemExit): rank_memory.main()
        launch.assert_not_called()
        record = json.loads((self.root/'memory-rank0.json').read_text())
        self.assertFalse(record['passed'])
        self.assertIn('not launched', record['error'])

    def test_memory_summary_has_no_sample_history(self):
        gpu = Mock(uuid='GPU-test', pci='test')
        gpu.read.side_effect = [dict(total=96*2**30, used=2**30, free=95*2**30),
                               dict(total=96*2**30, used=3*2**30, free=93*2**30)]
        child = Mock(); child.poll.return_value = 0
        argv = ['rank_memory.py', '--output', str(self.root), '--estimate-gib', '2', '--', '/app']
        with patch.object(sys, 'argv', argv), patch.dict(os.environ, SLURM_PROCID='0'), \
             patch.object(rank_memory, 'DeviceMemory', return_value=gpu), \
             patch.object(rank_memory.subprocess, 'Popen', return_value=child): rank_memory.main()
        record = json.loads((self.root/'memory-rank0.json').read_text())
        self.assertTrue(record['passed'])
        self.assertEqual(record['sampled_max_used_bytes'], 3*2**30)
        self.assertLess((self.root/'memory-rank0.json').stat().st_size, 2048)

    def test_fd_pcg_registry_has_exact_arguments_and_bounded_initial_bundles(self):
        self.assertEqual(VARIANTS['newton5']['args'], ['newton', '5'])
        self.assertEqual(VARIANTS['rich4c0']['args'], ['richardson', '4', '0'])
        self.assertEqual(VARIANTS['ssor42w157']['args'],
                         ['ssor', '4', '2', '1.57079632679'])
        self.assertEqual(VARIANTS['mg22w080m4c1']['args'],
                         ['multigrid', '2', '2', '0.8', '4', '1'])
        for variants in TUNING_BUNDLES.values():
            self.assertEqual(variants[0], 'cg')
            self.assertEqual(len(variants), len(set(variants)))

    def test_periodic_full_operator_richardson_uses_only_odd_sweep_counts(self):
        self.assertEqual(RICHARDSON_LOCAL_ITERATIONS, (1, 2, 4, 8))
        self.assertEqual(RICHARDSON_GLOBAL_ITERATIONS, (1, 3, 5, 7))
        self.assertEqual(RICHARDSON_ALT_ITERATIONS, (1, 3, 5, 7))
        self.assertIn('rich4c0', TUNING_BUNDLES['richardson'])
        self.assertNotIn('rich4c1', VARIANTS)
        self.assertNotIn('richa4', VARIANTS)
        self.assertIn('richa5', SMOKE_VARIANTS)
        for name in TUNING_BUNDLES['richardson']:
            if name != 'cg' and VARIANTS[name]['communication']:
                self.assertEqual(VARIANTS[name]['iterations'] % 2, 1)
        for name in TUNING_BUNDLES['richardson-alt']:
            if name != 'cg':
                self.assertEqual(VARIANTS[name]['iterations'] % 2, 1)

    def test_allocation_snapshot_includes_runner_registry_dependency(self):
        driver = Path(__file__).with_name('gh200_mesh.slurm').read_text()
        self.assertIn('"$experiments/fd_pcg_variants.py"', driver)

    def test_fd_pcg_1024_recipe_requires_explicit_large_count_acknowledgement(self):
        base = ['submit_campaigns.py', '--application', 'alpine', '--campaign', 'fd-pcg',
                '--output-root', str(self.root/'pcg'), '--placements', '8x4',
                '--cells-3d', '1024', '--variants', 'cg', '--dry-run']
        with patch.object(sys, 'argv', base), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, 'INT_MAX'):
                submitter.main()
        output = io.StringIO()
        with patch.object(sys, 'argv', base + ['--allow-large-particle-count']), \
             contextlib.redirect_stdout(output):
            submitter.main()
        plan = json.loads((self.root/'pcg/plans/8x4.json').read_text())
        task = plan['tasks'][0]
        self.assertEqual(task['particles'], 8 * 1024**3)
        self.assertTrue(task['memory']['large_particle_count_acknowledged'])
        self.assertLess(task['memory']['screening_estimate_gib'], 70)
        self.assertIn('--time=00:59:00', output.getvalue())

    def test_fd_pcg_tunes_both_anchors_and_allows_1024_after_preflight(self):
        calls = []
        def capture(command, **kwargs):
            calls.append(command)
            return Mock(returncode=0)
        argv = ['submit_fd_pcg_campaign.py', '--root', str(self.root/'study'),
                '--phase', 'tune', '--bundle', 'newton', '--dry-run']
        with patch.object(sys, 'argv', argv), patch.object(pcg_submitter.subprocess, 'run', side_effect=capture), \
             contextlib.redirect_stdout(io.StringIO()):
            pcg_submitter.main()
        self.assertEqual(len(calls), 2)
        self.assertTrue(any('256' in command for command in calls))
        self.assertTrue(any('512' in command for command in calls))
        for command in calls:
            self.assertEqual(command[command.index('--time') + 1], '00:59:00')
            self.assertEqual(command[command.index('--timeout') + 1], '3000')
        blocked = ['submit_fd_pcg_campaign.py', '--root', str(self.root/'study'),
                   '--phase', 'tune', '--bundle', 'newton', '--cells', '1024']
        with patch.object(sys, 'argv', blocked), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(SystemExit, 'preflight'):
                pcg_submitter.main()

    def test_fd_pcg_explicit_tuning_subset_accepts_stable_output_label(self):
        calls = []
        argv = [
            'submit_fd_pcg_campaign.py', '--root', str(self.root/'study'),
            '--phase', 'tune', '--variants', 'cg', 'ssor11w100',
            '--label', 'ssor-factorial', '--substudy', 'remaining-weights',
            '--cells', '256',
            '--placements', '1x2', '--stage', 'first', '--dry-run',
        ]
        with patch.object(sys, 'argv', argv), \
             patch.object(pcg_submitter.subprocess, 'run',
                          side_effect=lambda command, **kwargs: calls.append(command) or Mock(returncode=0)), \
             contextlib.redirect_stdout(io.StringIO()):
            pcg_submitter.main()
        self.assertEqual(len(calls), 1)
        output = calls[0][calls[0].index('--output-root') + 1]
        self.assertIn('/tune/ssor-factorial/n256-1x2/remaining-weights', output)

        argv[argv.index('ssor-factorial')] = 'Invalid_Label'
        with patch.object(sys, 'argv', argv), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                pcg_submitter.main()

    def test_fd_pcg_subset_repeats_reuse_successful_parent_first_run(self):
        calls = []
        argv = [
            'submit_fd_pcg_campaign.py', '--root', str(self.root/'study'),
            '--phase', 'tune', '--variants', 'cg', 'ssor11w100',
            '--label', 'ssor-factorial', '--substudy', 'converged',
            '--reuse-first-subset', '--cells', '256', '--placements', '1x2',
            '--stage', 'repeats', '--dry-run',
        ]
        with patch.object(sys, 'argv', argv), \
             patch.object(pcg_submitter.subprocess, 'run',
                          side_effect=lambda command, **kwargs: calls.append(command) or Mock(returncode=0)), \
             contextlib.redirect_stdout(io.StringIO()):
            pcg_submitter.main()
        self.assertEqual(len(calls), 1)
        command = calls[0]
        source = command[command.index('--first-run-output-root') + 1]
        self.assertTrue(source.endswith('/tune/ssor-factorial/n256-1x2'))
        output = command[command.index('--output-root') + 1]
        self.assertTrue(output.endswith('/tune/ssor-factorial/n256-1x2/converged'))

    def test_nested_subset_repeats_reuse_the_immediate_parent_first_run(self):
        calls = []
        argv = [
            'submit_fd_pcg_campaign.py', '--root', str(self.root/'study'),
            '--phase', 'tune', '--variants', 'cg', 'mg11w080m4c0',
            '--label', 'multigrid-factorial',
            '--substudy', 'smoothing-11/converged', '--reuse-first-subset',
            '--cells', '256', '--placements', '1x2', '--stage', 'repeats',
            '--dry-run',
        ]
        with patch.object(sys, 'argv', argv), \
             patch.object(pcg_submitter.subprocess, 'run',
                          side_effect=lambda command, **kwargs: calls.append(command) or Mock(returncode=0)), \
             contextlib.redirect_stdout(io.StringIO()):
            pcg_submitter.main()
        self.assertEqual(len(calls), 1)
        command = calls[0]
        source = command[command.index('--first-run-output-root') + 1]
        self.assertTrue(source.endswith(
            '/tune/multigrid-factorial/n256-1x2/smoothing-11'))
        output = command[command.index('--output-root') + 1]
        self.assertTrue(output.endswith(
            '/tune/multigrid-factorial/n256-1x2/smoothing-11/converged'))

    def test_repeat_gate_accepts_successful_subset_of_larger_first_bundle(self):
        source = self.root/'screen'
        first = source/'1x2/d3_p0_n256_r0'
        (first/'jobs').mkdir(parents=True)
        jobs = []
        for variant, code in [('cg', 0), ('ssor11w100', 0), ('ssor21w175', 1)]:
            name = f'd3_p0_n256_{variant}_r0'
            jobs.append(dict(name=name, variant=variant))
            (first/'jobs'/(name+'.json')).write_text(json.dumps(
                dict(returncode=code, validation_error='')))
        (first/'campaign.json').write_text(json.dumps(dict(schema=2, jobs=jobs)))

        output = io.StringIO()
        argv = [
            'submit_campaigns.py', '--application', 'alpine', '--campaign', 'fd-pcg',
            '--output-root', str(self.root/'followup'), '--placements', '1x2',
            '--cells-3d', '256', '--variants', 'cg', 'ssor11w100',
            '--stage', 'repeats', '--first-run-output-root', str(source), '--dry-run',
        ]
        # Reproduce the real failure: an earlier first-stage dry run already
        # persisted a plan before parent-subset admission was introduced.
        draft_argv = argv[:argv.index('--stage')] + ['--stage', 'first', '--dry-run']
        with patch.object(sys, 'argv', draft_argv), contextlib.redirect_stdout(io.StringIO()):
            submitter.main()
        path = self.root/'followup/plans/1x2.json'
        draft = path.read_bytes()
        with patch.object(sys, 'argv', argv), contextlib.redirect_stdout(output):
            submitter.main()
        self.assertIn('2 pending allocations', output.getvalue())
        self.assertIn('--array=1,2%1', output.getvalue())
        self.assertIn('Added parent first-run admission to unsubmitted plan', output.getvalue())
        self.assertEqual((path.parent/'history/1x2-before-subset-admission.json').read_bytes(), draft)
        plan = json.loads(path.read_text())
        self.assertEqual(plan['repeat_admission']['mode'], 'successful-variant-subset')
        self.assertEqual({key: value for key, value in plan.items() if key != 'repeat_admission'},
                         json.loads(draft))
        with patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()):
            submitter.main()
        self.assertEqual(json.loads(path.read_text()), plan)

    def test_subset_admission_cannot_rewrite_used_or_changed_plans(self):
        for evidence in ('submission', 'task', 'pin', 'log', 'recipe', 'admission'):
            with self.subTest(evidence=evidence):
                root = self.root/evidence
                path = root/'plans/1x2.json'
                path.parent.mkdir(parents=True)
                output = root/'1x2/d3_p0_n256_r0'
                previous = dict(tasks=[dict(output=str(output))], variants=['cg', 'ssor11w100'])
                if evidence == 'admission':
                    previous['repeat_admission'] = dict(mode='successful-variant-subset',
                                                      output_root='/other-source')
                path.write_text(json.dumps(previous))
                before = path.read_bytes()
                requested = dict(previous, repeat_admission=dict(
                    mode='successful-variant-subset', output_root='/screen'))
                if evidence == 'recipe':
                    requested['variants'] = ['cg', 'ssor11w125']
                elif evidence == 'submission':
                    (path.parent/'1x2-submission.json').write_text('{}')
                elif evidence == 'task':
                    output.mkdir(parents=True)
                elif evidence == 'pin':
                    (root/'study-pin.json').write_text('{}')
                elif evidence == 'log':
                    (root/'logs').mkdir()
                    (root/'logs/job.log').touch()
                with self.assertRaisesRegex(SystemExit, 'Study recipe changed'):
                    submitter.save_plan(path, requested)
                self.assertEqual(path.read_bytes(), before)
                self.assertFalse((path.parent/'history').exists())

    def test_subset_admission_requires_explicit_success(self):
        output = self.root/'r0'
        (output/'jobs').mkdir(parents=True)
        (output/'campaign.json').write_text(json.dumps(dict(
            schema=2, jobs=[dict(variant='cg', name='cg')])))
        (output/'jobs/cg.json').write_text('{}')
        self.assertFalse(submitter.task_subset_complete(dict(output=str(output)), ['cg']))

    def test_fd_pcg_preflight_accepts_a_recorded_newton_residual_rejection(self):
        root = self.root/'study'
        study = root/'preflight/capacity/n1024-8x4'
        output = study/'8x4/d3_p0_n1024_r0'
        (study/'plans').mkdir(parents=True)
        (output/'jobs').mkdir(parents=True)
        task = dict(repeat=0, output=str(output))
        (study/'plans/8x4.json').write_text(json.dumps(dict(tasks=[task])))
        jobs = []
        for index, variant in enumerate(pcg_submitter.LARGE_COUNT_PREFLIGHT_VARIANTS):
            name = f'd3_p0_n1024_{variant}_r0'
            jobs.append(dict(index=index, name=name, variant=variant))
            state = dict(returncode=0, validation_error='', directory=variant)
            if variant == 'newton5':
                state['returncode'] = 1
                data = output/variant
                data.mkdir()
                (data/'solves-rank0.csv').write_text(
                    'solve_seconds,fresh_relative_residual,success\n44.5,3.5e-9,0\n')
            (output/'jobs'/(name+'.json')).write_text(json.dumps(state))
        (output/'campaign.json').write_text(json.dumps(dict(schema=2, jobs=jobs)))
        self.assertTrue(pcg_submitter.preflight_complete(root))
        (output/'newton5/solves-rank0.csv').unlink()
        self.assertFalse(pcg_submitter.preflight_complete(root))

    def test_fd_pcg_1024_cg_sweep_accepts_explicit_feasible_placements(self):
        calls = []
        argv = [
            'submit_fd_pcg_campaign.py', '--root', str(self.root/'study'),
            '--phase', 'placement', '--cells', '1024',
            '--placements', '4x4', '8x4', '16x4', '32x4',
            '--stage', 'first', '--dry-run',
        ]
        with patch.object(sys, 'argv', argv), \
             patch.object(pcg_submitter.subprocess, 'run',
                          side_effect=lambda command, **kwargs: calls.append(command) or Mock(returncode=0)), \
             contextlib.redirect_stdout(io.StringIO()):
            pcg_submitter.main()
        self.assertEqual(len(calls), 4)
        self.assertEqual(
            {command[command.index('--placements') + 1] for command in calls},
            {'4x4', '8x4', '16x4', '32x4'})
        self.assertTrue(all(command[command.index('--cells-3d') + 1] == '1024'
                            for command in calls))
        self.assertTrue(all('/placement/cg/' in command[command.index('--output-root') + 1]
                            for command in calls))
        self.assertTrue(all(command[command.index('--slurm-script') + 1].endswith(
                            'gh200_campaign_extended.slurm') for command in calls))

    def test_fd_pcg_allocation_driver_accepts_extended_placements(self):
        legacy = Path(__file__).with_name('gh200_mesh.slurm').read_text()
        extended = Path(__file__).with_name('gh200_mesh_extended.slurm').read_text()
        self.assertNotIn('|16:4|32:4', legacy)
        self.assertIn('8:4|16:4|32:4', extended)

    def test_fd_pcg_transfer_uses_extended_driver_at_16x4(self):
        selection = self.root/'selection.json'
        selection.write_text(json.dumps({'variants': ['cg', 'ssor22w157']}))
        calls = []
        argv = [
            'submit_fd_pcg_campaign.py', '--root', str(self.root/'study'),
            '--phase', 'transfer', '--selection', str(selection), '--cells', '1024',
            '--placements', '8x4', '16x4', '--stage', 'first', '--dry-run',
        ]
        with patch.object(sys, 'argv', argv), \
             patch.object(pcg_submitter.subprocess, 'run',
                          side_effect=lambda command, **kwargs: calls.append(command) or Mock(returncode=0)), \
             contextlib.redirect_stdout(io.StringIO()):
            pcg_submitter.main()
        self.assertEqual(len(calls), 2)
        self.assertTrue(all(command[command.index('--slurm-script') + 1].endswith(
                            'gh200_campaign_extended.slurm') for command in calls))

    def test_fd_pcg_plain_cg_is_not_filed_as_tuning(self):
        argv = [
            'submit_fd_pcg_campaign.py', '--root', str(self.root/'study'),
            '--phase', 'tune', '--variants', 'cg', '--cells', '1024',
            '--placements', '8x4', '--stage', 'first', '--dry-run',
        ]
        with patch.object(sys, 'argv', argv), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                pcg_submitter.main()

    def test_fd_pcg_extended_placements_are_opt_in_and_memory_screened(self):
        output = self.root/'extended'
        argv = [
            'submit_campaigns.py', '--application', 'alpine', '--campaign', 'fd-pcg',
            '--output-root', str(output), '--placements', '16x4', '32x4',
            '--cells-3d', '1024', '--variants', 'cg', '--steps', '10',
            '--repeats', '3', '--stage', 'first', '--allow-large-particle-count',
            '--dry-run',
        ]
        with patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()):
            submitter.main()
        plans = {path.stem: json.loads(path.read_text())
                 for path in (output/'plans').glob('*.json')}
        self.assertEqual(set(plans), {'16x4', '32x4'})
        estimates = {
            placement: plan['tasks'][0]['memory']['screening_estimate_gib']
            for placement, plan in plans.items()
        }
        self.assertAlmostEqual(estimates['16x4'], 17.6875)
        self.assertAlmostEqual(estimates['32x4'], 9.34375)

    def test_fd_pcg_analysis_keeps_paired_bundle_control_and_selects_family_winner(self):
        run = self.root/'study/tune/newton/n256-1x2/1x2/d3_p0_n256_r0'
        (run/'jobs').mkdir(parents=True)
        config = dict(application='alpine', nodes=1, gpus_per_node=2, ranks=2)
        jobs = []
        for index, variant in enumerate(('cg', 'newton1', 'newton2')):
            job = dict(name=variant, dimension=3, order=0, cells=256,
                       particles=8*256**3, variant=variant, repeat=0)
            jobs.append(job)
            (run/'jobs'/(variant+'.json')).write_text(json.dumps(dict(
                returncode=0, validation_error='', directory=variant,
                allocation='123', wall_seconds=10)))
        (run/'campaign.json').write_text(json.dumps(dict(schema=2, config=config, jobs=jobs)))
        measurements = {
            'cg': dict(recurring_solve_seconds=2., recurring_step_seconds=3., recurring_iterations=100,
                       initial_solve_seconds=2.1, setup_seconds=.1, all_solve_seconds=22.,
                       max_fresh_residual=1e-11),
            'newton1': dict(recurring_solve_seconds=1.5, recurring_step_seconds=2.5, recurring_iterations=70,
                            initial_solve_seconds=1.8, setup_seconds=.2, all_solve_seconds=17.,
                            max_fresh_residual=1e-11),
            'newton2': dict(recurring_solve_seconds=1.2, recurring_step_seconds=2.2, recurring_iterations=50,
                            initial_solve_seconds=1.7, setup_seconds=.3, all_solve_seconds=14.,
                            max_fresh_residual=1e-11),
        }
        def measured(directory, job, settings):
            return measurements[job['variant']]
        output = self.root/'analysis'
        with patch.object(pcg_analyzer, 'inspect_run', side_effect=measured):
            records, means, selection = pcg_analyzer.analyze(self.root/'study', output)
        self.assertEqual(len(records), 3)
        self.assertEqual(selection['families']['newton'][0], 'newton2')
        self.assertIn('newton2', selection['variants'])
        self.assertTrue((output/'means.csv').is_file())

    def test_fd_pcg_analysis_ignores_allocation_preflight_campaign(self):
        study = self.root/'study'
        run = study/'smoke/families/n128-1x2/1x2/d3_p0_n128_r0'
        (run/'jobs').mkdir(parents=True)
        config = dict(application='alpine', nodes=1, gpus_per_node=2, ranks=2)
        job = dict(name='cg', dimension=3, order=0, cells=128,
                   particles=8*128**3, variant='cg', repeat=0)
        (run/'campaign.json').write_text(json.dumps(dict(schema=2, config=config, jobs=[job])))
        (run/'jobs/cg.json').write_text(json.dumps(dict(
            returncode=0, validation_error='', directory='data',
            allocation='123', wall_seconds=10)))
        preflight = run/'allocations/123-test/preflight'
        preflight.mkdir(parents=True)
        (preflight/'campaign.json').write_text(json.dumps(dict(
            schema=2,
            config=dict(application='alpine', nodes=None, gpus_per_node=None, ranks=2),
            jobs=[])))
        measured = dict(recurring_solve_seconds=1., recurring_step_seconds=2.,
                        recurring_iterations=5., initial_solve_seconds=1.1,
                        setup_seconds=.1, all_solve_seconds=11., max_fresh_residual=1e-11)
        with patch.object(pcg_analyzer, 'inspect_run', return_value=measured):
            records, means, _ = pcg_analyzer.analyze(study, self.root/'analysis')
        self.assertEqual(len(records), 1)
        self.assertEqual(len(means), 1)

    def test_fd_pcg_analysis_does_not_transfer_a_losing_family(self):
        run = self.root/'study/tune/richardson/n256-1x2/1x2/d3_p0_n256_r0'
        (run/'jobs').mkdir(parents=True)
        config = dict(application='alpine', nodes=1, gpus_per_node=2, ranks=2)
        jobs = []
        for variant in ('cg', 'rich1c0', 'rich2c0'):
            job = dict(name=variant, dimension=3, order=0, cells=256,
                       particles=8*256**3, variant=variant, repeat=0)
            jobs.append(job)
            (run/'jobs'/(variant+'.json')).write_text(json.dumps(dict(
                returncode=0, validation_error='', directory=variant,
                allocation='123', wall_seconds=10)))
        (run/'campaign.json').write_text(json.dumps(dict(schema=2, config=config, jobs=jobs)))
        measurements = {
            'cg': dict(recurring_solve_seconds=1., recurring_step_seconds=1.2,
                       recurring_iterations=100, initial_solve_seconds=1.1,
                       setup_seconds=.1, all_solve_seconds=11., max_fresh_residual=1e-11,
                       sampled_device_peak_gib=1., host_process_peak_gib=.1),
            'rich1c0': dict(recurring_solve_seconds=2., recurring_step_seconds=2.2,
                            recurring_iterations=80, initial_solve_seconds=2.1,
                            setup_seconds=.1, all_solve_seconds=22., max_fresh_residual=1e-11,
                            sampled_device_peak_gib=1., host_process_peak_gib=.1),
            'rich2c0': dict(recurring_solve_seconds=3., recurring_step_seconds=3.2,
                            recurring_iterations=60, initial_solve_seconds=3.1,
                            setup_seconds=.1, all_solve_seconds=33., max_fresh_residual=1e-11,
                            sampled_device_peak_gib=1., host_process_peak_gib=.1),
        }
        with patch.object(pcg_analyzer, 'inspect_run',
                          side_effect=lambda directory, job, settings: measurements[job['variant']]):
            _, _, selection = pcg_analyzer.analyze(self.root/'study', self.root/'analysis')
        self.assertNotIn('richardson', selection['families'])
        self.assertEqual(selection['variants'], ['cg'])
        self.assertTrue(selection['provisional'])

    def test_fd_pcg_analysis_requires_success_at_every_tuned_anchor(self):
        study = self.root/'study'
        measurements = {}
        for cells, placement, nodes, gpus in ((256, '1x2', 1, 2), (512, '8x4', 8, 4)):
            run = study/f'tune/newton/n{cells}-{placement}/{placement}/d3_p0_n{cells}_r0'
            (run/'jobs').mkdir(parents=True)
            config = dict(application='alpine', nodes=nodes, gpus_per_node=gpus,
                          ranks=nodes*gpus)
            jobs = []
            for variant in ('cg', 'newton1', 'newton2'):
                job = dict(name=f'{variant}-{cells}', dimension=3, order=0, cells=cells,
                           particles=8*cells**3, variant=variant, repeat=0)
                jobs.append(job)
                passed = variant != 'newton2' or cells == 256
                (run/'jobs'/(job['name']+'.json')).write_text(json.dumps(dict(
                    returncode=0 if passed else 1, validation_error='', directory=variant,
                    allocation=str(cells), wall_seconds=10)))
                measurements[(cells, variant)] = dict(
                    recurring_solve_seconds=(1. if variant == 'cg' else
                                             .5 if variant == 'newton2' else .8),
                    recurring_step_seconds=1.2, recurring_iterations=50,
                    initial_solve_seconds=1.1, setup_seconds=.1, all_solve_seconds=11.,
                    max_fresh_residual=1e-11, sampled_device_peak_gib=1.,
                    host_process_peak_gib=.1)
            (run/'campaign.json').write_text(json.dumps(dict(
                schema=2, config=config, jobs=jobs)))
        with patch.object(
                pcg_analyzer, 'inspect_run',
                side_effect=lambda directory, job, settings:
                measurements[(job['cells'], job['variant'])]):
            _, _, selection = pcg_analyzer.analyze(study, self.root/'analysis')
        self.assertEqual(selection['families']['newton'], ['newton1'])
        self.assertNotIn('newton2', selection['variants'])


if __name__ == '__main__': unittest.main()
