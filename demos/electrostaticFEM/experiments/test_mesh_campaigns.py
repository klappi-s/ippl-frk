"""Protect pooled mesh curves against incompatible or duplicate measurements."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from analyze_mesh_sweep import load_campaigns


class MeshCampaigns(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.manifest = dict(config=dict(
            orders=[3], particles=32768, particles_per_cell=None, steps=10,
            repeats=3, ranks=1, family='gll', quadrature='exact',
            initial_guess='previous', timer_level='coarse', tolerance=1e-10,
            max_iterations=10000, batches={'baseline': {'methods': ['plain']}},
            replay_runner_sha256='runner', executable_sha256='old-build'),
            jobs=[dict(name='small', dimension=3, order=3, cells=8, repeat=0,
                       batch='baseline')])

    def write(self, name, manifest):
        directory = self.root / name
        directory.mkdir()
        (directory / 'campaign.json').write_text(json.dumps(manifest))
        return directory

    def test_disjoint_extension_preserves_build_provenance(self):
        later = copy.deepcopy(self.manifest)
        later['jobs'][0].update(name='large', cells=16)
        later['config']['executable_sha256'] = 'new-build'
        a, b = self.write('a', self.manifest), self.write('b', later)
        campaigns = load_campaigns([a, b])
        self.assertEqual([m['config']['executable_sha256'] for _, m in campaigns],
                         ['old-build', 'new-build'])
        self.assertEqual([m['jobs'][0]['cells'] for _, m in campaigns], [8, 16])

    def test_changed_solver_tolerance_rejected(self):
        later = copy.deepcopy(self.manifest)
        later['config']['tolerance'] = 1e-8
        a, b = self.write('a', self.manifest), self.write('b', later)
        with self.assertRaisesRegex(ValueError, 'Incompatible campaign setting tolerance'):
            load_campaigns([a, b])

    def test_duplicate_measurements_rejected(self):
        a, b = self.write('a', self.manifest), self.write('b', self.manifest)
        with self.assertRaisesRegex(ValueError, 'Overlapping campaign job'):
            load_campaigns([a, b])


if __name__ == '__main__':
    unittest.main()
