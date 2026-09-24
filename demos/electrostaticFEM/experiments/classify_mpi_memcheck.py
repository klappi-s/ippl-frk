#!/usr/bin/env python3
"""Review complete, unsuppressed single-rank memcheck output (Python 3.6+).

Only paired Cray GTL 9.1.0 pointer-query diagnostics reviewed in jobs 4734849
and 4735622 are accepted. The caller must independently preserve application /
launcher failures. This script never edits or discards the original reports.
"""
import argparse
import json
import re
from pathlib import Path
import xml.etree.ElementTree as ET


def fields(node, names):
    if node is None or sorted(child.tag for child in node) != sorted(names):
        raise ValueError('Unexpected or incomplete report structure')
    return {child.tag: child.text or '' for child in node}


def pointer_query(record):
    """Return the reviewed MPI call site, rejecting any other report shape."""
    kind = record.findtext('kind')
    names = ['kind', 'what', 'hostStack']
    if kind == 'Sanitizer':
        names.append('level')
    fields(record, names)
    if record.tag != 'record' or kind not in ('Api', 'Sanitizer'):
        raise ValueError('Unreviewed report kind')
    stack = record.find('hostStack')
    if sorted(child.tag for child in stack) != ['frame', 'frame', 'saveLocation']:
        raise ValueError('Unreviewed host stack')
    if stack.findtext('saveLocation') != 'error':
        raise ValueError('Unreviewed stack location')
    cuda, mpi = stack.findall('frame')
    cuda = fields(cuda, ['func', 'pc', 'module'])
    mpi = fields(mpi, ['func', 'pc', 'module', 'path', 'line'])
    if cuda != {'func': 'cuPointerGetAttribute', 'pc': '0x307dbf',
                'module': '/usr/lib64/libcuda.so.1'}:
        raise ValueError('Unreviewed CUDA call site')
    if (mpi['func'] != 'gtlt_cuda_pointer_type'
            or mpi['path'] != '/workspace/src/gtlt/cuda/gtlt_cuda_query.c'
            or not re.fullmatch(r'/user-environment/linux-neoverse_v2/'
                                r'cray-gtl-9\.1\.0-[^/]+/lib/libmpi_gtl_cuda\.so',
                                mpi['module'])
            or (mpi['line'], mpi['pc']) not in (('372', '0xfcdf'), ('408', '0xfd43'))):
        raise ValueError('Unreviewed MPI library/call site')
    if kind == 'Api':
        what = fields(record.find('what'), ['text', 'api', 'error', 'message', 'result'])
        if what != {
                'text': 'Program hit CUDA_ERROR_INVALID_VALUE (error 1) due to '
                        '"invalid argument" on CUDA API call to cuPointerGetAttribute.',
                'api': 'cuPointerGetAttribute', 'error': 'CUDA_ERROR_INVALID_VALUE',
                'message': 'invalid argument', 'result': '1'}:
            raise ValueError('Unreviewed CUDA API error')
    else:
        what = fields(record.find('what'), ['text', 'error'])
        if (record.findtext('level') != 'Error'
                or not re.fullmatch(r'Could not find allocation for given ptr 0x[0-9a-f]+',
                                    what['error'])
                or what['text'] != 'CUDA API Error: ' + what['error']):
            raise ValueError('Unreviewed allocation-query diagnostic')
    return kind, tuple(sorted(mpi.items()))


def review(xml_path, log_path, expected_tests):
    result = {'policy': 'cray-gtl-9.1.0-pointer-query-v1', 'accepted': False,
              'reports': 0, 'accepted_mpi_reports': 0, 'problems': []}
    try:
        root = ET.parse(str(xml_path)).getroot()
        if root.tag != 'ComputeSanitizerOutput' or any(r.tag != 'record' for r in root):
            raise ValueError('Unexpected sanitizer XML document')
        records = list(root)
        result['reports'] = len(records)
        signatures = []
        for index, record in enumerate(records):
            try:
                signatures.append(pointer_query(record))
            except ValueError as error:
                signatures.append(None)
                result['problems'].append('Report {}: {}'.format(index + 1, error))
        # The reviewed tool emits an allocation diagnostic followed by its API
        # failure at the same call site. A changed/incomplete sequence needs review.
        for index in range(0, len(signatures), 2):
            pair = signatures[index:index + 2]
            if (len(pair) == 2 and all(pair) and pair[0][0] == 'Sanitizer'
                    and pair[1][0] == 'Api' and pair[0][1] == pair[1][1]):
                result['accepted_mpi_reports'] += 2
            else:
                result['problems'].append('Unreviewed report pair at {}'.format(index + 1))
        log = Path(log_path).read_text()
        summaries = re.findall(r'^========= ERROR SUMMARY: ([0-9]+) errors\s*$', log, re.M)
        if summaries != [str(len(records))]:
            result['problems'].append('Missing/inconsistent sanitizer summary')
        passed = re.findall(r'^\[  PASSED  \] ([0-9]+) tests?\.$', log, re.M)
        started = re.findall(r'^\[ RUN      \] (.+)$', log, re.M)
        completed = re.findall(r'^\[       OK \] (.+) \([0-9]+ ms\)$', log, re.M)
        if (passed != [str(expected_tests)] or len(started) != expected_tests
                or started != completed or '[  FAILED  ]' in log):
            result['problems'].append('Missing/failed/incomplete GoogleTest run')
        if re.search(r'^========= (?:Error:|Fatal:|Target application terminated)', log, re.M):
            result['problems'].append('Sanitizer tool/target execution error')
    except (OSError, ValueError, ET.ParseError) as error:
        result['problems'].append(str(error))
    result['accepted'] = not result['problems']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('xml', type=Path)
    parser.add_argument('log', type=Path)
    parser.add_argument('--expected-tests', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = review(args.xml, args.log, args.expected_tests)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    label = 'PASS with MPI exception' if result['accepted_mpi_reports'] else 'PASS'
    if not result['accepted']:
        label = 'FAIL'
    print('{}: {} reports; {} reviewed MPI reports'.format(
        label, result['reports'], result['accepted_mpi_reports']))
    for problem in result['problems']:
        print('  ' + problem)
    return 0 if result['accepted'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
