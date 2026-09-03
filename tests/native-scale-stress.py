#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mounted native scale stress for InfiltratorFS."""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import resource
import subprocess
import time


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", help="stress directory below an already-mounted filesystem")
    parser.add_argument("--files", type=int, default=1_000_000)
    parser.add_argument("--directories", type=int, default=1_000)
    parser.add_argument("--workers", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--churn-files", type=int, default=100_000)
    parser.add_argument("--batch-directories", type=int, default=64)
    parser.add_argument("--reclaim-vfs-cache", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    return parser.parse_args()


def directory_file_count(total_files: int, directories: int, index: int) -> int:
    base, remainder = divmod(total_files, directories)
    return base + (1 if index < remainder else 0)


def sentinel_payload(index: int) -> bytes:
    return f"infiltratorfs-scale:{index:06d}\n".encode("ascii")


def file_name(index: int) -> str:
    return f"f{index:06d}"


def directory_name(index: int) -> str:
    return f"d{index:06d}"


def create_one(path: str, payload: bytes | None = None) -> None:
    fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_EXCL | os.O_CLOEXEC, 0o600)
    try:
        if payload:
            view = memoryview(payload)
            while view:
                written = os.write(fd, view)
                if written <= 0:
                    raise OSError("short write while creating scale sentinel")
                view = view[written:]
    finally:
        os.close(fd)


def create_dirs(root: str, total_files: int, directories: int,
                indices: list[int]) -> tuple[int, float]:
    started = time.monotonic()
    created = 0
    for directory_index in indices:
        dpath = os.path.join(root, directory_name(directory_index))
        count = directory_file_count(total_files, directories, directory_index)
        for local_index in range(count):
            payload = sentinel_payload(directory_index) if local_index == 0 else None
            create_one(os.path.join(dpath, file_name(local_index)), payload)
            created += 1
    return created, time.monotonic() - started


def churn_dirs(root: str, total_files: int, directories: int, churn_files: int,
               recreate: bool, indices: list[int]) -> tuple[int, float]:
    started = time.monotonic()
    changed = 0
    base, remainder = divmod(churn_files, directories)
    for directory_index in indices:
        count = directory_file_count(total_files, directories, directory_index)
        quota = min(count, base + (1 if directory_index < remainder else 0))
        dpath = os.path.join(root, directory_name(directory_index))
        for local_index in range(quota):
            path = os.path.join(dpath, file_name(local_index))
            if recreate:
                payload = sentinel_payload(directory_index) if local_index == 0 else None
                create_one(path, payload)
            else:
                os.unlink(path)
            changed += 1
    return changed, time.monotonic() - started


def split_indices(directories: int, workers: int) -> list[list[int]]:
    workers = max(1, min(workers, directories))
    groups = [[] for _ in range(workers)]
    for index in range(directories):
        groups[index % workers].append(index)
    return groups


def run_parallel(fn, groups: list[list[int]], *prefix_args) -> tuple[int, float]:
    started = time.monotonic()
    total = 0
    with concurrent.futures.ProcessPoolExecutor(max_workers=len(groups)) as pool:
        futures = [pool.submit(fn, *prefix_args, group) for group in groups]
        for future in concurrent.futures.as_completed(futures):
            count, _elapsed = future.result()
            total += count
    return total, time.monotonic() - started


def split_values(indices: list[int], workers: int) -> list[list[int]]:
    workers = max(1, min(workers, len(indices)))
    groups = [[] for _ in range(workers)]
    for position, index in enumerate(indices):
        groups[position % workers].append(index)
    return [group for group in groups if group]


def read_meminfo_kib() -> dict[str, int]:
    wanted = {"MemAvailable", "Slab", "SReclaimable"}
    observed: dict[str, int] = {}
    try:
        with open("/proc/meminfo", encoding="ascii") as stream:
            for line in stream:
                key, _, value = line.partition(":")
                if key not in wanted:
                    continue
                observed[key] = int(value.strip().split()[0])
    except (OSError, ValueError):
        pass
    return observed


def reclaim_vfs_cache(label: str) -> float:
    """
    Keep the million-file qualification about persistent filesystem scale,
    rather than exhausting a small hosted runner with clean Linux dentry/inode
    cache.  The pool has no live file descriptors at batch boundaries.  Sync
    first, then reclaim only dentries/inodes (drop_caches=2), never dirty data.
    """
    started = time.monotonic()
    before = read_meminfo_kib()
    subprocess.run(
        ["sudo", "-n", "sh", "-c", "sync; echo 2 > /proc/sys/vm/drop_caches"],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    elapsed = time.monotonic() - started
    after = read_meminfo_kib()
    print(
        "[SCALE-MEM] "
        f"{label} reclaim_elapsed={elapsed:.3f}s "
        f"available_before_kib={before.get('MemAvailable', -1)} "
        f"available_after_kib={after.get('MemAvailable', -1)} "
        f"slab_after_kib={after.get('Slab', -1)} "
        f"sreclaimable_after_kib={after.get('SReclaimable', -1)}"
    )
    return elapsed


def run_parallel_batched(
    fn,
    directories: int,
    workers: int,
    batch_directories: int,
    reclaim_cache: bool,
    *prefix_args,
) -> tuple[int, float]:
    started = time.monotonic()
    total = 0
    batch_number = 0
    for first in range(0, directories, batch_directories):
        last = min(directories, first + batch_directories)
        groups = split_values(list(range(first, last)), workers)
        count, _elapsed = run_parallel(fn, groups, *prefix_args)
        total += count
        batch_number += 1
        if reclaim_cache:
            reclaim_vfs_cache(
                f"batch={batch_number} directories={first}-{last - 1}"
            )
    return total, time.monotonic() - started


def verify_population(root: str, total_files: int, directories: int) -> float:
    started = time.monotonic()
    observed = 0
    for directory_index in range(directories):
        dpath = os.path.join(root, directory_name(directory_index))
        expected = directory_file_count(total_files, directories, directory_index)
        with os.scandir(dpath) as entries:
            count = sum(1 for entry in entries if entry.is_file(follow_symlinks=False))
        if count != expected:
            raise AssertionError(f"{dpath}: expected {expected} files, found {count}")
        observed += count

        sentinel = os.path.join(dpath, file_name(0))
        with open(sentinel, "rb", buffering=0) as stream:
            data = stream.read()
        if data != sentinel_payload(directory_index):
            raise AssertionError(f"sentinel mismatch: {sentinel}")

        for local_index in {0, expected // 2, expected - 1}:
            path = os.path.join(dpath, file_name(local_index))
            st = os.stat(path, follow_symlinks=False)
            if not os.path.isfile(path) or st.st_nlink != 1:
                raise AssertionError(f"metadata mismatch: {path}")

    if observed != total_files:
        raise AssertionError(f"expected {total_files} files, found {observed}")
    return time.monotonic() - started


def sync_timed() -> float:
    started = time.monotonic()
    os.sync()
    return time.monotonic() - started


def main() -> int:
    args = parse_args()
    if args.files < 1:
        raise SystemExit("--files must be positive")
    if args.directories < 1 or args.directories > args.files:
        raise SystemExit("--directories must be between 1 and --files")
    if args.workers < 1:
        raise SystemExit("--workers must be positive")
    if args.churn_files < 0 or args.churn_files > args.files:
        raise SystemExit("--churn-files must be between 0 and --files")
    if args.batch_directories < 1:
        raise SystemExit("--batch-directories must be positive")

    root = os.path.abspath(args.root)
    worker_count = max(1, min(args.workers, args.directories))

    if args.verify_only:
        elapsed = verify_population(root, args.files, args.directories)
        print(f"[SCALE-PERF] verify files={args.files} directories={args.directories} "
              f"elapsed={elapsed:.3f}s rate={args.files / elapsed:.1f} files/s")
        print("Native million-file durable verification: PASS")
        return 0

    os.mkdir(root, 0o755)
    for directory_index in range(args.directories):
        os.mkdir(os.path.join(root, directory_name(directory_index)), 0o755)

    created, create_elapsed = run_parallel_batched(
        create_dirs, args.directories, worker_count,
        args.batch_directories, args.reclaim_vfs_cache,
        root, args.files, args.directories)
    if created != args.files:
        raise AssertionError(f"created {created}, expected {args.files}")
    print(f"[SCALE-PERF] create files={created} directories={args.directories} "
          f"workers={worker_count} batch_directories={args.batch_directories} "
          f"elapsed={create_elapsed:.3f}s "
          f"rate={created / create_elapsed:.1f} files/s")

    sync_elapsed = sync_timed()
    print(f"[SCALE-PERF] post-create sync elapsed={sync_elapsed:.3f}s")

    verify_elapsed = verify_population(root, args.files, args.directories)
    print(f"[SCALE-PERF] enumerate+verify files={args.files} elapsed={verify_elapsed:.3f}s "
          f"rate={args.files / verify_elapsed:.1f} files/s")

    if args.churn_files:
        removed, remove_elapsed = run_parallel_batched(
            churn_dirs, args.directories, worker_count,
            args.batch_directories, args.reclaim_vfs_cache,
            root, args.files, args.directories, args.churn_files, False)
        if removed != args.churn_files:
            raise AssertionError(f"removed {removed}, expected {args.churn_files}")
        print(f"[SCALE-PERF] unlink files={removed} elapsed={remove_elapsed:.3f}s "
              f"rate={removed / remove_elapsed:.1f} files/s")

        recreated, recreate_elapsed = run_parallel_batched(
            churn_dirs, args.directories, worker_count,
            args.batch_directories, args.reclaim_vfs_cache,
            root, args.files, args.directories, args.churn_files, True)
        if recreated != args.churn_files:
            raise AssertionError(f"recreated {recreated}, expected {args.churn_files}")
        print(f"[SCALE-PERF] recreate files={recreated} elapsed={recreate_elapsed:.3f}s "
              f"rate={recreated / recreate_elapsed:.1f} files/s")

        churn_sync = sync_timed()
        print(f"[SCALE-PERF] post-churn sync elapsed={churn_sync:.3f}s")
        verify_elapsed = verify_population(root, args.files, args.directories)
        print(f"[SCALE-PERF] post-churn verify files={args.files} elapsed={verify_elapsed:.3f}s "
              f"rate={args.files / verify_elapsed:.1f} files/s")

    usage = resource.getrusage(resource.RUSAGE_SELF)
    print(f"[SCALE-PERF] controller maxrss_kib={usage.ru_maxrss}")
    print(f"Native scale stress: PASS ({args.files} distinct files, "
          f"{args.directories} directories, {args.churn_files} churned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
