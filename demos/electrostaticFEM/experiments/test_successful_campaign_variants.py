"""Tests for selecting repeatable variants from a complete first stage."""

import json
from pathlib import Path
import tempfile
import unittest

from successful_campaign_variants import successful_variants


class SuccessfulCampaignVariants(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.output = Path(temporary.name)
        (self.output / 'jobs').mkdir()
        self.jobs = []
        for variant, code in [('cg', 0), ('mg-pass', 0), ('mg-fail', 1)]:
            job = dict(name='case-' + variant, variant=variant)
            self.jobs.append(job)
            (self.output / 'jobs' / (job['name'] + '.json')).write_text(json.dumps(
                dict(job=job, returncode=code, validation_error='')))
        (self.output / 'campaign.json').write_text(json.dumps(
            dict(schema=2, jobs=self.jobs)))

    def test_selects_only_explicit_successes_in_manifest_order(self):
        self.assertEqual(successful_variants(self.output), ['cg', 'mg-pass'])

    def test_refuses_to_freeze_a_subset_while_first_stage_is_incomplete(self):
        (self.output / 'jobs' / 'case-mg-fail.json').unlink()
        with self.assertRaisesRegex(ValueError, 'First stage is incomplete'):
            successful_variants(self.output)

    def test_refuses_state_for_a_different_job(self):
        path = self.output / 'jobs' / 'case-mg-pass.json'
        state = json.loads(path.read_text())
        state['job']['variant'] = 'different'
        path.write_text(json.dumps(state))
        with self.assertRaisesRegex(ValueError, 'identity differs'):
            successful_variants(self.output)


if __name__ == '__main__':
    unittest.main()
