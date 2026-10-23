#!/usr/bin/env python3
"""Bounded, opt-in PERF-019 evidence collector for already-mounted native NVMe.

Never formats, partitions, mounts, unmounts, switches drivers or runs fsck.
Retains checksummed data for independent post-remount verification and scrub.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import time
import uuid

CHUNK = 1024 * 1024
SEED = 18110


def command(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()


def admission(mount):
    mount = mount.resolve(strict=True)
    if not mount.is_dir() or not os.path.ismount(mount) or str(mount) == '/':
        raise ValueError('target must be an already-mounted non-root volume')
    target = command('findmnt', '-n', '-T', str(mount), '-o', 'TARGET')
    source = command('findmnt', '-n', '-T', str(mount), '-o', 'SOURCE')
    fstype = command('findmnt', '-n', '-T', str(mount), '-o', 'FSTYPE')
    opts = command('findmnt', '-n', '-T', str(mount), '-o', 'OPTIONS')
    if Path(target).resolve() != mount or fstype != 'infiltratorfs' or 'rw' not in opts.split(','):
        raise ValueError('target must be a read-write native InfiltratorFS mount root')
    if not source.startswith('/dev/nvme') or not Path(source).is_block_device():
        raise ValueError('target must be mounted directly from a physical NVMe block device')
    if command('lsblk', '-dn', '-o', 'TYPE', source) not in ('part', 'disk'):
        raise ValueError('NVMe source must be a disk or partition')
    return {'mount': str(mount), 'source': str(Path(source).resolve()),
            'fstype': fstype, 'options': opts,
            'mount_id': command('findmnt', '-n', '-T', str(mount), '-o', 'ID'),
            'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip()}


def report_location(root, report):
    report = report.expanduser().resolve()
    if os.path.commonpath((str(root), str(report))) == str(root):
        raise ValueError('report must be outside the tested volume')
    return report


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(CHUNK), b''):
            digest.update(chunk)
    return digest.hexdigest()


def syncfs(path):
    # GNU sync -f invokes syncfs(2) on the filesystem containing this path.
    command('sync', '-f', str(path))


def elapsed_ns(start):
    return (time.perf_counter_ns() - start) / 1000000.0


def percentiles(values):
    data = sorted(values)
    if not data:
        raise ValueError('empty latency distribution')
    p = lambda q: data[max(0, min(len(data) - 1, math.ceil(q * len(data)) - 1))]
    return {'count': len(data), 'mean_ms': statistics.mean(data),
            'p50_ms': p(.5), 'p95_ms': p(.95), 'p99_ms': p(.99),
            'max_ms': data[-1]}


def record_file(path, root):
    return {'path': str(path.relative_to(root)),
            'size': path.stat().st_size, 'sha256': sha(path)}


def sequential(rounddir, size_mib):
    target = rounddir / 'sequential.bin'
    # Generate 16 MiB of deterministic high-entropy input before timing. The
    # bounded rotation exceeds the native compression window and avoids charging
    # SHAKE-256 generation time to the filesystem's buffered-write duration.
    blocks = [hashlib.shake_256(f'{SEED}:seq:{i}'.encode()).digest(CHUNK)
              for i in range(16)]
    start = time.perf_counter_ns()
    with target.open('xb', buffering=CHUNK) as stream:
        for i in range(size_mib):
            stream.write(blocks[i % len(blocks)])
    buffered = elapsed_ns(start)
    start = time.perf_counter_ns()
    syncfs(target)
    sync = elapsed_ns(start)
    return {'buffered_ms': buffered, 'final_sync_ms': sync}, [record_file(target, rounddir)]


def durable_small(rounddir, samples):
    target = rounddir / 'durable-4k.bin'
    fsync_times, total_times = [], []
    with target.open('xb', buffering=0) as stream:
        for i in range(samples):
            payload = hashlib.shake_256(f'{SEED}:4k:{i}'.encode()).digest(4096)
            start = time.perf_counter_ns()
            stream.write(payload)
            after_write = time.perf_counter_ns()
            os.fsync(stream.fileno())
            now = time.perf_counter_ns()
            fsync_times.append((now - after_write) / 1000000.0)
            total_times.append((now - start) / 1000000.0)
    return {'fsync': percentiles(fsync_times), 'write_plus_fsync': percentiles(total_times)}, [
        record_file(target, rounddir)]


def metadata(rounddir, count):
    base = rounddir / 'metadata'
    base.mkdir(mode=0o700)
    values = {}
    start = time.perf_counter_ns()
    for i in range(count):
        with (base / f'f-{i:05d}').open('xb'):
            pass
    values['create_ms'] = elapsed_ns(start)
    start = time.perf_counter_ns()
    for i in range(count):
        (base / f'f-{i:05d}').stat()
    values['stat_ms'] = elapsed_ns(start)
    start = time.perf_counter_ns()
    for i in range(count):
        (base / f'f-{i:05d}').rename(base / f'r-{i:05d}')
    values['rename_ms'] = elapsed_ns(start)
    start = time.perf_counter_ns()
    for i in range(count):
        (base / f'r-{i:05d}').unlink()
    values['unlink_ms'] = elapsed_ns(start)
    base.rmdir()
    start = time.perf_counter_ns()
    syncfs(rounddir)
    values['final_sync_ms'] = elapsed_ns(start)
    values['files'] = count
    return values


def inventory(tree, mount):
    if tree is None:
        return []
    tree = tree.expanduser().resolve(strict=True)
    overlap = os.path.commonpath((str(tree), str(mount)))
    if not tree.is_dir() or overlap == str(mount) or overlap == str(tree):
        raise ValueError('input tree must neither contain nor be contained in the test volume')
    found = []
    for directory, dirs, files in os.walk(tree, followlinks=False):
        dirs[:] = sorted(x for x in dirs if not (Path(directory) / x).is_symlink())
        for name in sorted(files):
            source = Path(directory) / name
            if source.is_file() and not source.is_symlink():
                found.append((source, source.relative_to(tree)))
    return found


def copy_tree(rounddir, files):
    if not files:
        return None, []
    root = rounddir / 'tree'
    root.mkdir()
    manifest = []
    start = time.perf_counter_ns()
    for src, rel in files:
        target = root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, target)
        manifest.append({'path': str(target.relative_to(rounddir)),
                         'size': target.stat().st_size})
    copied = elapsed_ns(start)
    start = time.perf_counter_ns()
    syncfs(root)
    sync = elapsed_ns(start)
    # Integrity reads and source comparisons must not pollute buffered-copy timing.
    for (src, _), item in zip(files, manifest):
        item['sha256'] = sha(rounddir / item['path'])
        if item['sha256'] != sha(src):
            raise ValueError(f'source changed while copying {src}')
    return {'files': len(manifest), 'bytes': sum(x['size'] for x in manifest),
            'buffered_ms': copied, 'final_sync_ms': sync}, manifest


def provenance(identity):
    values = {'kernel': command('uname', '-r'), 'source_commit': None,
              'debian_version': None, 'loaded_module_version': None, 'device_model': None}
    probes = (
        ('source_commit', ('git', '-C', str(Path(__file__).resolve().parents[1]),
                           'rev-parse', 'HEAD')),
        ('debian_version', ('dpkg-query', '-W', '-f=' + chr(36) + '{Version}', 'infiltratorfs')),
        ('loaded_module_version', ('modinfo', '-F', 'version', 'infiltratorfs')),
        ('device_model', ('lsblk', '-dn', '-o', 'MODEL', identity['source'])),
    )
    for key, args in probes:
        try:
            values[key] = command(*args)
        except (OSError, subprocess.CalledProcessError):
            pass
    return values


def collect(args):
    identity = admission(args.mount)
    root = Path(identity['mount'])
    report = report_location(root, args.report)
    if report.exists():
        raise ValueError('existing evidence report cannot be overwritten')
    if not args.execute:
        print('SAFE: no writes. Explicit --execute is needed for this physical NVMe benchmark.')
        print(json.dumps(identity, indent=2))
        return
    files = inventory(args.tree, root)
    total_bytes = sum(path.stat().st_size for path, _ in files)
    required = args.rounds * (args.seq_mib * CHUNK + total_bytes + args.fsync_samples * 4096)
    if shutil.disk_usage(root).free < int(required * 1.2) + 64 * CHUNK:
        raise ValueError('insufficient free space for retained samples')
    testdir = root / ('.infiltratorfs-perf019-' + uuid.uuid4().hex)
    testdir.mkdir(mode=0o700)
    result = {'identity': identity, 'environment': provenance(identity),
              'format': 'Operator must independently confirm on-disk Format 0.19',
              'tree_files': len(files), 'tree_bytes': total_bytes,
              'tree_matches_1460_case': len(files) == 1460,
              'requested_rounds': args.rounds, 'test_directory': str(testdir),
              'rounds': [], 'qualification': 'INCOMPLETE: needs remount, scrub and attribution'}
    try:
        for i in range(args.rounds):
            rounddir = testdir / f'round-{i + 1:02d}'
            rounddir.mkdir()
            sequential_times, seq_files = sequential(rounddir, args.seq_mib)
            tree_times, tree_files = copy_tree(rounddir, files)
            small_times, small_files = durable_small(rounddir, args.fsync_samples)
            meta = metadata(rounddir, args.metadata_files)
            manifest = seq_files + tree_files + small_files
            for item in manifest:
                if sha(rounddir / item['path']) != item['sha256']:
                    raise ValueError('immediate integrity mismatch')
            result['rounds'].append({'directory': rounddir.name,
                                      'sequential': sequential_times, 'tree': tree_times,
                                      '4k_durable': small_times, 'metadata': meta,
                                      'manifest': manifest})
            print(f'round {i + 1}/{args.rounds}: write={sequential_times["buffered_ms"]:.2f}ms '
                  f'syncfs={sequential_times["final_sync_ms"]:.2f}ms', flush=True)
    finally:
        # Retain a partial manifest and all data on failure: never guess at cleanup.
        report.parent.mkdir(parents=True, exist_ok=True)
        with report.open('x', encoding='utf-8') as stream:
            json.dump(result, stream, indent=2)
    print(f'Report: {report}\nRetained data: {testdir}')
    print('Unmount/remount this selected volume, verify, then offline-scrub separately.')


def verify(args):
    identity = admission(args.mount)
    root = Path(identity['mount'])
    report = report_location(root, args.report)
    result = json.loads(report.read_text(encoding='utf-8'))
    before = result['identity']
    if identity['mount'] != before['mount'] or identity['source'] != before['source']:
        raise ValueError('verification device differs from the original')
    if identity['mount_id'] == before['mount_id'] and identity['boot_id'] == before['boot_id']:
        raise ValueError('same mount instance: no real unmount/remount or reboot demonstrated')
    testdir = Path(result['test_directory'])
    if testdir.parent != root or not testdir.name.startswith('.infiltratorfs-perf019-'):
        raise ValueError('report identifies an unsafe path')
    if len(result['rounds']) != result['requested_rounds']:
        raise ValueError('incomplete run cannot be qualified')
    checked = 0
    for round_result in result['rounds']:
        base = testdir / round_result['directory']
        if base.parent != testdir:
            raise ValueError('round directory escaped test root')
        for item in round_result['manifest']:
            path = base / item['path']
            if not path.resolve().is_relative_to(base.resolve()):
                raise ValueError('manifest path escaped test root')
            if path.stat().st_size != item['size'] or sha(path) != item['sha256']:
                raise ValueError(f'post-remount integrity failure at {path}')
            checked += 1
    print(f'PASS: {checked} retained files match their pre-remount SHA-256.')
    print('This is not power-loss durability evidence. Run offline deep scrub separately.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest='mode', required=True)
    for mode in ('run', 'verify'):
        cmd = modes.add_parser(mode)
        cmd.add_argument('--mount', required=True, type=Path)
        cmd.add_argument('--report', required=True, type=Path)
        if mode == 'run':
            cmd.add_argument('--execute', action='store_true')
            cmd.add_argument('--tree', type=Path)
            cmd.add_argument('--rounds', type=int, default=3)
            cmd.add_argument('--seq-mib', type=int, default=256)
            cmd.add_argument('--fsync-samples', type=int, default=200)
            cmd.add_argument('--metadata-files', type=int, default=2000)
    args = parser.parse_args()
    if args.mode == 'run' and not (2 <= args.rounds <= 10 and
            1 <= args.seq_mib <= 1024 and 10 <= args.fsync_samples <= 10000 and
            10 <= args.metadata_files <= 100000):
        parser.error('benchmark parameters exceed bounded limits')
    try:
        (collect if args.mode == 'run' else verify)(args)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError, json.JSONDecodeError) as err:
        print(f'PERF-019: FAIL: {err}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
