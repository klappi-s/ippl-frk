#!/usr/bin/env python3
"""Print the successful variants from one complete first-stage campaign."""

import argparse
import json
from pathlib import Path


def successful_variants(output):
    manifest_path = output / 'campaign.json'
    if not manifest_path.is_file():
        raise ValueError('Missing campaign manifest: ' + str(manifest_path))
    manifest = json.loads(manifest_path.read_text())
    jobs = manifest.get('jobs')
    if manifest.get('schema') != 2 or not isinstance(jobs, list) or not jobs:
        raise ValueError('Invalid campaign manifest: ' + str(manifest_path))

    names = set()
    variants = set()
    passed = []
    missing = []
    for job in jobs:
        name = job.get('name')
        variant = job.get('variant')
        if not name or not variant or name in names or variant in variants:
            raise ValueError('Campaign jobs must have unique names and variants')
        names.add(name)
        variants.add(variant)
        state_path = output / 'jobs' / (name + '.json')
        if not state_path.is_file():
            missing.append(variant)
            continue
        state = json.loads(state_path.read_text())
        if state.get('job') != job:
            raise ValueError('Job-state identity differs: ' + str(state_path))
        if state.get('returncode') == 0 and not state.get('validation_error'):
            passed.append(variant)

    if missing:
        raise ValueError('First stage is incomplete; missing: ' + ' '.join(missing))
    if 'cg' not in passed:
        raise ValueError('Paired CG did not pass')
    if len(passed) < 2:
        raise ValueError('No successful multigrid configuration is available')
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        variants = successful_variants(args.output.resolve())
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        parser.error(str(error))
    print('\n'.join(variants))


if __name__ == '__main__':
    main()
