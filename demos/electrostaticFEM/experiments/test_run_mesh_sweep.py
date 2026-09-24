"""Exercise repeated sweep planning, replay launches and resume without MPI/GPU."""
import contextlib
import io
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import run_mesh_sweep


class ForwardOnlyDict(dict):
    # Model Python 3.6/3.7 dictionaries even when testing on newer Python.
    __reversed__ = None


class MeshSweepRunner(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.app = self.root / 'mock application.py'
        self.app.write_text(
            'import json, sys\n'
            'from pathlib import Path\n'
            'args = sys.argv[1:]\n'
            'output = Path(args[args.index("--output") + 1])\n'
            'output.mkdir()\n'
            '(output / "arguments.json").write_text(json.dumps(args))\n')
        self.output = self.root / 'campaign'
        # A failed allocation can leave evidence before campaign.json exists.
        (self.output / 'allocations').mkdir(parents=True)
        (self.output / 'allocations/previous.log').write_text('preflight passed\n')
        self.args = ['--executable', str(self.app), '--output', str(self.output),
                     '--dimensions', '1', '2', '3', '--orders', '3',
                     '--cells-1d', '256', '512', '1024', '--cells-2d', '16', '32', '64',
                     '--cells-3d', '4', '8', '16', '--steps', '10', '--repeats', '3',
                     '--launcher', shlex.quote(sys.executable), '--resume']

    def check_plan(self):
        manifest = json.loads((self.output / 'campaign.json').read_text())
        jobs = manifest['jobs']
        self.assertEqual(len(jobs), 54)
        self.assertEqual([j['index'] for j in jobs], list(range(54)))
        self.assertEqual(len({j['name'] for j in jobs}), 54)
        meshes = {(1, 256), (1, 512), (1, 1024), (2, 16), (2, 32), (2, 64),
                  (3, 4), (3, 8), (3, 16)}
        self.assertEqual({(j['dimension'], j['cells']) for j in jobs}, meshes)
        for dim, cells in meshes:
            actual = [(j['repeat'], j['batch']) for j in jobs
                      if (j['dimension'], j['cells']) == (dim, cells)]
            self.assertEqual(actual, [(0, 'baseline'), (0, 'tuned'),
                                      (1, 'tuned'), (1, 'baseline'),
                                      (2, 'baseline'), (2, 'tuned')])
        self.assertEqual((self.output / 'allocations/previous.log').read_text(), 'preflight passed\n')
        return manifest

    def test_three_repeats_with_nonreversible_dictionary(self):
        with patch.object(run_mesh_sweep, 'BATCHES', ForwardOnlyDict(run_mesh_sweep.BATCHES)), \
                patch.object(sys, 'argv', ['run_mesh_sweep.py'] + self.args + ['--dry-run']), \
                contextlib.redirect_stdout(io.StringIO()):
            run_mesh_sweep.main()
        self.check_plan()

    def test_all_launches_method_rotation_and_resume(self):
        command = [sys.executable, str(Path(run_mesh_sweep.__file__).resolve())] + self.args
        def run(extra=()):
            return subprocess.run(command + list(extra), stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, universal_newlines=True)
        completed = run()
        self.assertEqual(completed.returncode, 0, completed.stdout)
        manifest = self.check_plan()
        expected_methods = {
            'baseline': ['plain,jacobi,gauss-seidel,chebyshev',
                         'jacobi,gauss-seidel,chebyshev,plain',
                         'gauss-seidel,chebyshev,plain,jacobi'],
            'tuned': ['plain,gauss-seidel,chebyshev',
                      'gauss-seidel,chebyshev,plain', 'chebyshev,plain,gauss-seidel']}
        for job in manifest['jobs']:
            state = json.loads((self.output / 'jobs' / (job['name'] + '.json')).read_text())
            self.assertEqual(state['returncode'], 0)
            batch = self.output / state['directory']
            replay = json.loads((batch / 'runs.json').read_text())[0]
            arguments = json.loads((batch / replay['name'] / 'arguments.json').read_text())
            self.assertEqual(arguments[arguments.index('--replay') + 1],
                             expected_methods[job['batch']][job['repeat']])
            self.assertEqual(arguments[arguments.index('--steps') + 1], '10')
            self.assertEqual(arguments[arguments.index('--timer_recording') + 1], 'summary')
            self.assertEqual(arguments[arguments.index('--timer_sample_limit') + 1], '1024')
        before = {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()}
        resumed = run()
        self.assertEqual(resumed.returncode, 0, resumed.stdout)
        self.assertEqual(resumed.stdout.count('completed; skipped'), 54)
        self.assertEqual(before, {p: p.read_bytes() for p in self.output.rglob('*') if p.is_file()})
        changed_recording = run(['--timer-recording', 'raw'])
        self.assertNotEqual(changed_recording.returncode, 0)
        self.assertIn('Resume configuration/binary/runner differs', changed_recording.stdout)
        changed = run(['--steps', '11'])
        self.assertNotEqual(changed.returncode, 0)
        self.assertIn('Resume configuration/binary/runner differs', changed.stdout)


if __name__ == '__main__':
    unittest.main()
