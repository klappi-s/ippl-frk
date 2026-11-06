#!/usr/bin/env python3
"""Run one GPU rank with bounded, sampled memory evidence (no sample history)."""
import argparse
import ctypes as ct
import json
import os
from pathlib import Path
import resource
import signal
import socket
import subprocess

GIB = 2**30


class DeviceMemory:
    class Info(ct.Structure):
        _fields_ = [('total', ct.c_ulonglong), ('free', ct.c_ulonglong), ('used', ct.c_ulonglong)]

    def __init__(self):
        # Resolve CUDA-visible ordinal zero to PCI ID without creating a context.
        # NVML indices alone need not follow CUDA_VISIBLE_DEVICES ordering.
        driver = ct.CDLL('libcuda.so.1')
        device, pci = ct.c_int(), ct.create_string_buffer(32)
        self.check(driver.cuInit(0), 'cuInit')
        self.check(driver.cuDeviceGet(ct.byref(device), 0), 'cuDeviceGet')
        self.check(driver.cuDeviceGetPCIBusId(pci, len(pci), device), 'cuDeviceGetPCIBusId')
        self.nvml = ct.CDLL('libnvidia-ml.so.1')
        self.check(self.nvml.nvmlInit_v2(), 'nvmlInit')
        self.handle = ct.c_void_p()
        self.check(self.nvml.nvmlDeviceGetHandleByPciBusId_v2(pci, ct.byref(self.handle)), 'nvmlDeviceGetHandle')
        uuid = ct.create_string_buffer(96)
        self.check(self.nvml.nvmlDeviceGetUUID(self.handle, uuid, len(uuid)), 'nvmlDeviceGetUUID')
        self.uuid, self.pci = uuid.value.decode(), pci.value.decode()

    @staticmethod
    def check(code, operation):
        if code: raise RuntimeError('%s failed with code %d' % (operation, code))

    def read(self):
        info = self.Info()
        self.check(self.nvml.nvmlDeviceGetMemoryInfo(self.handle, ct.byref(info)), 'nvmlDeviceGetMemoryInfo')
        return dict(total=info.total, free=info.free, used=info.used)


def host_available():
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'): return int(line.split()[1])*1024
    raise RuntimeError('MemAvailable is unavailable')


def cgroup_memory():
    # v2 only; retain node availability and per-process RSS on v1 systems.
    for line in Path('/proc/self/cgroup').read_text().splitlines():
        if line.startswith('0::'):
            path = Path('/sys/fs/cgroup')/line[3:].lstrip('/')
            current, limit = path/'memory.current', path/'memory.max'
            if current.exists() and limit.exists():
                value = limit.read_text().strip()
                return int(current.read_text()), int(value) if value != 'max' else None
    return None, None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--estimate-gib', type=float, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    a = parser.parse_args()
    command = a.command[1:] if a.command[:1] == ['--'] else a.command
    if not command: parser.error('Missing application command')
    rank = int(os.environ['SLURM_PROCID'])
    record = dict(rank=rank, host=socket.gethostname(), sampling_interval_seconds=1,
                  estimate_gib=a.estimate_gib, samples=0, error='', returncode=None,
                  note='Sampled device/node/cgroup usage can miss transient peaks. RSS is child process high-water mark.')
    child = None
    interrupted = [False]
    def stop(signum, frame):
        interrupted[0] = True
        if child is not None and child.poll() is None: child.terminate()
    previous = {s: signal.signal(s, stop) for s in (signal.SIGTERM, signal.SIGINT)}
    try:
        gpu = DeviceMemory()
        initial = gpu.read()
        budget = min(70*GIB, .8*initial['free'])
        host_initial = host_available()
        record.update(device_uuid=gpu.uuid, pci_bus_id=gpu.pci, initial=initial,
                      budget_bytes=budget, sampled_max_used_bytes=initial['used'],
                      sampled_min_host_available_bytes=host_initial,
                      sampled_max_cgroup_bytes=0, cgroup_limit_bytes=None)
        if a.estimate_gib*GIB > budget:
            raise RuntimeError('Screening estimate exceeds available HBM budget; application not launched')
        child = subprocess.Popen(command)
        while True:
            current = gpu.read()
            host = host_available()
            group, limit = cgroup_memory()
            record['samples'] += 1
            record['sampled_max_used_bytes'] = max(record['sampled_max_used_bytes'], current['used'])
            record['sampled_min_host_available_bytes'] = min(record['sampled_min_host_available_bytes'], host)
            record['sampled_max_cgroup_bytes'] = max(record['sampled_max_cgroup_bytes'], group or 0)
            record['cgroup_limit_bytes'] = limit
            if current['used'] - initial['used'] > budget:
                raise RuntimeError('Sampled device allocation exceeded campaign HBM budget')
            if host < .1*host_initial or (limit and group > .9*limit):
                raise RuntimeError('Sampled host memory headroom below campaign margin')
            code = child.poll()
            if code is not None:
                record['returncode'] = code
                break
            if interrupted[0]: raise RuntimeError('Rank monitor interrupted')
            try: child.wait(timeout=1)
            except subprocess.TimeoutExpired: pass
    except Exception as error:
        record['error'] = str(error)
        record['returncode'] = 125
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try: child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        record['host_process_maxrss_bytes'] = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss*1024
        record['passed'] = record['returncode'] == 0 and not record['error'] and record['samples'] > 0
        a.output.mkdir(parents=True, exist_ok=True)
        (a.output/('memory-rank%d.json' % rank)).write_text(json.dumps(record, indent=2)+'\n')
        for signum, handler in previous.items(): signal.signal(signum, handler)
    if not record['passed']: raise SystemExit(record['error'] or record['returncode'] or 125)


if __name__ == '__main__': main()
