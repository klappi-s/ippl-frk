"""Protect ordinary process isolation, failure collection and resumable campaigns."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

import run_mesh_sweep as runner
from ordinary_measurements import DEFAULT_VARIANTS, VARIANTS
from submit_campaigns import tasks_for

class OrdinaryCampaigns(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.app = self.root/'application'; self.app.write_text('binary pin')
        self.output = self.root/'campaign'
        self.args = ['--executable', str(self.app), '--output', str(self.output), '--dimensions', '3',
                     '--orders', '2', '--cells-3d', '4', '--repeats', '1', '--variants', 'cg', 'jacobi', 'cheb2']
        self.child = Mock(pid=123456)
        self.child.wait.return_value = 0
    def run_driver(self, extra=(), **patches):
        with patch.object(sys, 'argv', ['run_mesh_sweep.py'] + self.args + list(extra)), \
             patch.object(runner.subprocess, 'Popen', return_value=self.child) as launch, \
             patch.object(runner, 'inspect_run', **(patches or dict(return_value={'verified': True}))), \
             contextlib.redirect_stdout(io.StringIO()):
            runner.main()
        return launch
    def test_normal_launches_resume_and_pins(self):
        launched = self.run_driver()
        self.assertEqual(launched.call_count, 3)
        for call in launched.call_args_list:
            command = call.args[0]
            self.assertNotIn('--replay', command)
            self.assertEqual(command[command.index('--steps')+1], '10')
            self.assertEqual(command[command.index('--initial_condition')+1], 'alpine_product')
            self.assertTrue(call.kwargs['start_new_session'])
        before = {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        self.assertEqual(self.run_driver(['--resume', '--timeout', '18000']).call_count, 0)
        self.assertEqual(before, {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()})
        with self.assertRaisesRegex(SystemExit, 'differs'): self.run_driver(['--resume', '--steps', '11'])
        self.app.write_text('different executable')
        with self.assertRaisesRegex(SystemExit, 'differs'): self.run_driver(['--resume'])
    def test_validation_failure_does_not_abort_other_methods(self):
        with self.assertRaisesRegex(SystemExit, 'cg'):
            self.run_driver(side_effect=[AssertionError('bad residual'), {}, {}])
        states = [json.loads(p.read_text()) for p in (self.output/'jobs').glob('*.json')]
        self.assertEqual(len(states), 3)
        self.assertEqual(sum(bool(s['validation_error']) for s in states), 1)
    def test_timeout_terminates_process_group_and_records_failure(self):
        self.child.wait.side_effect = [subprocess.TimeoutExpired('application', 1), 0, 0, 0]
        with patch.object(runner.os, 'killpg') as kill:
            with self.assertRaisesRegex(SystemExit, 'cg'): self.run_driver()
        kill.assert_called_once_with(self.child.pid, signal.SIGTERM)
        state = json.loads((self.output/'jobs/d3_p2_n4_cg_r0.json').read_text())
        self.assertEqual(state['returncode'], 124)
        self.assertEqual(len(list((self.output/'jobs').glob('*.json'))), 3)
    def test_alpine_arguments_and_separate_method_variants(self):
        config = dict(application='alpine', launcher='mpiexec -n {ranks}', ranks=2,
                      executable='/path with spaces/LandauDamping', steps=10, tolerance=1e-10, max_iterations=10000, dt=.05)
        for variant in VARIANTS:
            command = runner.command_for(config, dict(cells=64, particles=32768, variant=variant), self.root/'data')
            self.assertNotIn('--replay', command)
            self.assertIn('--benchmark-output', command)
            self.assertEqual(command[10], '1.0')
            self.assertEqual(command[11], 'LeapFrog')
        command = runner.command_for(config, dict(cells=64, particles=32768, variant='gs21'), self.root/'data')
        self.assertEqual(command[12:16], ['gauss-seidel', '2', '1', '1'])
    def test_study_coverage_and_repeat_rotation(self):
        args = argparse.Namespace(application='fem', orders=[2,3], dimensions=[1,2,3],
            cells_3d=None, large=None, full_control=False, repeats=3, variants=DEFAULT_VARIANTS,
            output_root=self.root, particles=32768, tolerance=1e-10, dt=.05)
        tasks = tasks_for(args, '1x4')
        self.assertEqual(len(tasks), 63)
        self.assertEqual(len({t['output'] for t in tasks}), 63)
        self.assertEqual(len(tasks_for(args, '2x1')), 18)
        args.application='alpine'
        self.assertEqual(len(tasks_for(args, '1x4')), 12)
        self.assertEqual(len(tasks_for(args, '2x1')), 6)
        a = argparse.Namespace(application='fem', orders=[2], dimensions=[3], cells_1d=[], cells_2d=[],
            cells_3d=[64], repeat_offset=0, repeats=3, variants=DEFAULT_VARIANTS, particles_per_cell=None, particles=32768)
        jobs = runner.make_jobs(a)
        self.assertEqual(len(jobs), 24)
        self.assertEqual([j['variant'] for j in jobs[:3]], ['cg','jacobi','gs21'])
        self.assertEqual([j['variant'] for j in jobs[8:11]], ['jacobi','gs21','gs22'])
        self.assertEqual(len({j['name'] for j in jobs}), 24)

if __name__ == '__main__': unittest.main()
