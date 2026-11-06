"""Protect missing-only recovery of immutable factorial campaign outputs."""

import json
import contextlib
import io
from pathlib import Path
from unittest.mock import patch
import sys
import tempfile
import unittest

from resume_missing_campaign import missing_jobs
import submit_multigrid_missing as submitter


class MissingOnlyRecovery(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.output = Path(temporary.name)
        (self.output / 'jobs').mkdir()
        self.jobs = [
            dict(name='passed', variant='cg'),
            dict(name='failed', variant='multigrid'),
            dict(name='interrupted-without-state', variant='multigrid'),
            dict(name='never-started', variant='multigrid'),
        ]
        (self.output / 'jobs' / 'passed.json').write_text(json.dumps(dict(
            returncode=0, validation_error='')))
        (self.output / 'jobs' / 'failed.json').write_text(json.dumps(dict(
            returncode=1, wall_seconds=1644, validation_error='')))
        # An attempt directory is not a completed canonical record. This is
        # how an application interrupted by the Slurm wall limit is recovered.
        (self.output / 'interrupted-without-state_attempt0' / 'data').mkdir(parents=True)

    def test_only_absent_canonical_states_are_selected(self):
        self.assertEqual(
            [job['name'] for job in missing_jobs(self.output, self.jobs)],
            ['interrupted-without-state', 'never-started'])

    def test_recorded_failure_is_never_selected_even_when_very_slow(self):
        selected = {job['name'] for job in missing_jobs(self.output, self.jobs)}
        self.assertNotIn('failed', selected)


class SubmitMissingRecovery(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        study = self.root / 'tune' / 'multigrid-factorial'
        for cells, placement in submitter.ANCHORS:
            for pair in submitter.SMOOTHING_PAIRS:
                base = study / ('n%d-%s' % (cells, placement)) / ('smoothing-' + pair)
                output = base / placement / ('d3_p0_n%d_r0' % cells)
                (base / 'plans').mkdir(parents=True)
                (base / 'logs').mkdir()
                (output / 'jobs').mkdir(parents=True)
                jobs = [
                    dict(name='passed', variant='cg', dimension=3, order=0,
                         cells=cells, repeat=0),
                    dict(name='failed', variant='bad', dimension=3, order=0,
                         cells=cells, repeat=0),
                    dict(name='missing', variant='next', dimension=3, order=0,
                         cells=cells, repeat=0),
                ]
                task = dict(name='d3_p0_n%d_r0' % cells, output=str(output),
                            dimension=3, order=0, cells=cells, repeat=0)
                plan = dict(tasks=[task], variants={name: {} for name in ('cg', 'bad', 'next')})
                (base / 'plans' / (placement + '.json')).write_text(json.dumps(plan))
                (output / 'campaign.json').write_text(json.dumps(dict(schema=2, jobs=jobs)))
                (output / 'jobs' / 'passed.json').write_text(json.dumps(dict(returncode=0)))
                (output / 'jobs' / 'failed.json').write_text(json.dumps(
                    dict(returncode=1, wall_seconds=1500)))

    def test_dry_run_submits_only_one_missing_job_per_bundle(self):
        output = io.StringIO()
        argv = ['submit_multigrid_missing.py', '--root', str(self.root),
                '--account', 'c41', '--uenv', '/uenv.squashfs', '--view', 'default',
                '--dry-run']
        with patch.object(sys, 'argv', argv), contextlib.redirect_stdout(output):
            submitter.main()
        text = output.getvalue()
        self.assertEqual(text.count('1 missing applications'), 24)
        self.assertEqual(text.count('sbatch --parsable'), 24)
        self.assertIn('--partition=normal', text)
        self.assertIn('--time=00:40:00', text)
        self.assertIn('--account=c41', text)
        self.assertIn('--uenv=/uenv.squashfs', text)
        self.assertIn('--view=default', text)
        self.assertIn('--timeout 180', text)
        self.assertIn('--wrap=', text)
        self.assertNotIn('failed starting', text)


if __name__ == '__main__':
    unittest.main()
