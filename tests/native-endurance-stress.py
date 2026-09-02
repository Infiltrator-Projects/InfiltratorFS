#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Near-full, fragmentation and mixed mounted workload qualification."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import random
import resource
import time
from pathlib import Path


MIB = 1024 * 1024
BLOCK = 4096


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("root")
    p.add_argument("--seconds", type=int, default=300)
    p.add_argument("--workers", type=int, default=min(4, os.cpu_count() or 1))
    p.add_argument("--reserve-mib", type=int, default=384)
    p.add_argument("--coarse-mib", type=int, default=16)
    p.add_argument("--skip-fill", action="store_true")
    p.add_argument("--manifest-out")
    p.add_argument("--verify-manifest")
    return p.parse_args()


def free_bytes(path: str) -> int:
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize


def total_bytes(path: str) -> int:
    st = os.statvfs(path)
    return st.f_blocks * st.f_frsize


def write_extent(path: str, size: int, salt: int) -> None:
    # This phase measures physical near-full/fragmentation behaviour.  A
    # short repeated motif became highly compressible once native IAC1 was
    # enabled, so free space stopped falling and the qualification could spend
    # the whole job filling data that occupied very little physical storage.
    # SHAKE-256 gives us deterministic, compression-resistant bytes while
    # retaining a reproducible workload.
    seed = f"infiltratorfs-endurance:{salt}".encode("ascii")
    pattern = hashlib.shake_256(seed).digest(MIB)
    fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_EXCL | os.O_CLOEXEC, 0o600)
    try:
        left = size
        while left:
            chunk = pattern if left >= len(pattern) else pattern[:left]
            view = memoryview(chunk)
            while view:
                n = os.write(fd, view)
                if n <= 0:
                    raise OSError("short write")
                view = view[n:]
            left -= len(chunk)
    finally:
        os.close(fd)


def fill_fragmented(root: str, reserve: int, coarse_size: int) -> dict[str, int | float]:
    pool = os.path.join(root, "fragment-pool")
    os.mkdir(pool)
    coarse: list[str] = []
    started = time.monotonic()

    index = 0
    while free_bytes(root) > reserve + coarse_size:
        path = os.path.join(pool, f"coarse-{index:05d}.bin")
        write_extent(path, coarse_size, index)
        coarse.append(path)
        index += 1
    os.sync()
    after_coarse = free_bytes(root)

    for path in coarse[::3]:
        os.unlink(path)
    os.sync()
    after_holes = free_bytes(root)

    refill_sizes = [1 * MIB, 2 * MIB, 3 * MIB, 5 * MIB]
    refill: list[str] = []
    index = 0
    while free_bytes(root) > reserve + max(refill_sizes):
        size = refill_sizes[index % len(refill_sizes)]
        if free_bytes(root) <= reserve + size:
            break
        path = os.path.join(pool, f"refill-{index:06d}.bin")
        write_extent(path, size, 100000 + index)
        refill.append(path)
        index += 1
    os.sync()
    after_refill = free_bytes(root)

    # A second fragmentation pass creates a broader run-size distribution.
    for path in refill[::4]:
        os.unlink(path)
    for path in coarse[1::7]:
        if os.path.exists(path):
            os.unlink(path)
    os.sync()
    after_second_holes = free_bytes(root)

    tiny_sizes = [256 * 1024, 512 * 1024, 768 * 1024, 1280 * 1024]
    tiny: list[str] = []
    index = 0
    while free_bytes(root) > reserve + max(tiny_sizes):
        size = tiny_sizes[index % len(tiny_sizes)]
        if free_bytes(root) <= reserve + size:
            break
        path = os.path.join(pool, f"tiny-{index:06d}.bin")
        write_extent(path, size, 200000 + index)
        tiny.append(path)
        index += 1
    os.sync()

    elapsed = time.monotonic() - started
    final_free = free_bytes(root)
    total = total_bytes(root)
    print(
        f"[ENDURANCE-PERF] fragmentation coarse={len(coarse)} refill={len(refill)} "
        f"tiny={len(tiny)} elapsed={elapsed:.3f}s "
        f"free_mib={final_free / MIB:.1f} free_pct={100.0 * final_free / total:.2f}%"
    )
    return {
        "coarse_files": len(coarse),
        "refill_files": len(refill),
        "tiny_files": len(tiny),
        "free_after_coarse": after_coarse,
        "free_after_holes": after_holes,
        "free_after_refill": after_refill,
        "free_after_second_holes": after_second_holes,
        "final_free": final_free,
        "elapsed": elapsed,
    }


def deterministic_block(worker: int, iteration: int) -> bytes:
    motif = bytes(((worker * 53 + iteration * 17 + i * 29 + 7) & 0xFF)
                  for i in range(256))
    return motif * (BLOCK // len(motif))


def worker_loop(root: str, worker: int, seconds: int) -> dict[str, object]:
    wr = os.path.join(root, f"worker-{worker:02d}")
    os.mkdir(wr)
    ns = os.path.join(wr, "namespace")
    os.mkdir(ns)

    hot = os.path.join(wr, "hot.bin")
    hot_size = 16 * MIB
    with open(hot, "wb", buffering=0) as stream:
        stream.truncate(hot_size)

    log_path = os.path.join(wr, "append.log")
    sparse = os.path.join(wr, "sparse.bin")
    fd_sparse = os.open(sparse, os.O_CREAT | os.O_RDWR | os.O_EXCL | os.O_CLOEXEC, 0o600)
    os.ftruncate(fd_sparse, 256 * MIB)

    fd_hot = os.open(hot, os.O_RDWR | os.O_CLOEXEC)
    fd_log = os.open(log_path, os.O_CREAT | os.O_RDWR | os.O_APPEND | os.O_CLOEXEC, 0o600)

    rng = random.Random(0x1F51A7E + worker * 0x10001)
    deadline = time.monotonic() + seconds
    iteration = 0
    counts = {
        "pwrite": 0,
        "append": 0,
        "rename": 0,
        "xattr": 0,
        "truncate": 0,
        "link": 0,
        "fsync": 0,
        "readback": 0,
    }
    last_blocks: dict[int, bytes] = {}
    fsync_ms: list[float] = []

    try:
        while time.monotonic() < deadline:
            op = iteration % 8
            if op == 0:
                block_index = rng.randrange(hot_size // BLOCK)
                payload = deterministic_block(worker, iteration)
                offset = block_index * BLOCK
                n = os.pwrite(fd_hot, payload, offset)
                if n != len(payload):
                    raise OSError("short pwrite")
                last_blocks[block_index] = payload
                counts["pwrite"] += 1
            elif op == 1:
                record = f"worker={worker} iteration={iteration}\n".encode("ascii")
                if os.write(fd_log, record) != len(record):
                    raise OSError("short append")
                counts["append"] += 1
            elif op == 2:
                slot = iteration % 128
                src = os.path.join(ns, f"tmp-{slot:03d}")
                dst = os.path.join(ns, f"live-{slot:03d}")
                for path in (src, dst):
                    try:
                        os.unlink(path)
                    except FileNotFoundError:
                        pass
                fd = os.open(src, os.O_CREAT | os.O_WRONLY | os.O_EXCL | os.O_CLOEXEC, 0o600)
                os.write(fd, f"{worker}:{iteration}\n".encode("ascii"))
                os.close(fd)
                os.rename(src, dst)
                counts["rename"] += 1
            elif op == 3:
                value = f"{worker}:{iteration}".encode("ascii")
                os.setxattr(hot, b"user.infiltratorfs-endurance", value)
                if os.getxattr(hot, b"user.infiltratorfs-endurance") != value:
                    raise AssertionError("xattr readback mismatch")
                counts["xattr"] += 1
            elif op == 4:
                size = (64 + (iteration % 192)) * MIB
                os.ftruncate(fd_sparse, size)
                tail = f"tail:{worker}:{iteration}".encode("ascii")
                offset = max(0, size - BLOCK)
                if os.pwrite(fd_sparse, tail, offset) != len(tail):
                    raise OSError("sparse tail write failed")
                counts["truncate"] += 1
            elif op == 5:
                link = os.path.join(wr, "hot.link")
                sym = os.path.join(wr, "hot.sym")
                for path in (link, sym):
                    try:
                        os.unlink(path)
                    except FileNotFoundError:
                        pass
                os.link(hot, link)
                os.symlink("hot.bin", sym)
                if os.stat(hot).st_ino != os.stat(link).st_ino:
                    raise AssertionError("hard-link inode mismatch")
                if os.readlink(sym) != "hot.bin":
                    raise AssertionError("symlink target mismatch")
                os.unlink(link)
                os.unlink(sym)
                counts["link"] += 1
            elif op == 6:
                if last_blocks:
                    block_index = rng.choice(tuple(last_blocks.keys()))
                    expected = last_blocks[block_index]
                    got = os.pread(fd_hot, len(expected), block_index * BLOCK)
                    if got != expected:
                        raise AssertionError("random overwrite readback mismatch")
                    counts["readback"] += 1
            else:
                started = time.monotonic_ns()
                os.fsync(fd_hot)
                os.fsync(fd_log)
                os.fsync(fd_sparse)
                elapsed = (time.monotonic_ns() - started) / 1_000_000
                fsync_ms.append(elapsed)
                counts["fsync"] += 1
            iteration += 1
    finally:
        os.fsync(fd_hot)
        os.fsync(fd_log)
        os.fsync(fd_sparse)
        os.close(fd_hot)
        os.close(fd_log)
        os.close(fd_sparse)

    proof = os.path.join(wr, "proof.bin")
    proof_hash = hashlib.sha256()
    with open(proof, "xb", buffering=0) as stream:
        for chunk_index in range(2):
            motif = bytes(
                ((worker * 71 + chunk_index * 31 + i * 13 + 5) & 0xFF)
                for i in range(256)
            )
            chunk = motif * (MIB // len(motif))
            stream.write(chunk)
            proof_hash.update(chunk)
        os.fsync(stream.fileno())

    final_xattr = f"final:{worker}:{iteration}".encode("ascii")
    os.setxattr(hot, b"user.infiltratorfs-endurance", final_xattr)
    fd_hot = os.open(hot, os.O_RDWR | os.O_CLOEXEC)
    try:
        os.fsync(fd_hot)
    finally:
        os.close(fd_hot)

    final_link = os.path.join(wr, "hot.final.link")
    final_sym = os.path.join(wr, "hot.final.sym")
    for path in (final_link, final_sym):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    os.link(hot, final_link)
    os.symlink("hot.bin", final_sym)

    hot_hash = hashlib.sha256()
    with open(hot, "rb", buffering=0) as stream:
        while True:
            chunk = stream.read(MIB)
            if not chunk:
                break
            hot_hash.update(chunk)

    result: dict[str, object] = {
        "worker": worker,
        "counts": counts,
        "iterations": iteration,
        "proof": os.path.relpath(proof, root),
        "proof_sha256": proof_hash.hexdigest(),
        "hot": os.path.relpath(hot, root),
        "hot_sha256": hot_hash.hexdigest(),
        "final_xattr": final_xattr.decode("ascii"),
        "final_link": os.path.relpath(final_link, root),
        "final_sym": os.path.relpath(final_sym, root),
        "log": os.path.relpath(log_path, root),
        "log_size": os.stat(log_path).st_size,
        "log_sha256": sha256_file(log_path),
        "sparse": os.path.relpath(sparse, root),
        "sparse_size": os.stat(sparse).st_size,
        "sparse_tail_sha256": tail_block_digest(sparse),
        "namespace_count": len(os.listdir(ns)),
        "namespace_sha256": namespace_digest(ns),
        "fsync_mean_ms": (sum(fsync_ms) / len(fsync_ms)) if fsync_ms else 0.0,
        "fsync_max_ms": max(fsync_ms) if fsync_ms else 0.0,
    }
    return result


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb", buffering=0) as stream:
        while True:
            chunk = stream.read(MIB)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def namespace_digest(path: str) -> str:
    digest = hashlib.sha256()
    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if not os.path.isfile(full):
            raise AssertionError(f"unexpected namespace entry: {full}")
        digest.update(name.encode("utf-8"))
        digest.update(b"\0")
        with open(full, "rb", buffering=0) as stream:
            digest.update(stream.read())
        digest.update(b"\0")
    return digest.hexdigest()


def tail_block_digest(path: str) -> str:
    size = os.stat(path).st_size
    offset = max(0, size - BLOCK)
    with open(path, "rb", buffering=0) as stream:
        stream.seek(offset)
        data = stream.read(BLOCK)
    return hashlib.sha256(data).hexdigest()


def verify_manifest(root: str, manifest_path: str) -> None:
    with open(manifest_path, encoding="utf-8") as stream:
        manifest = json.load(stream)
    for worker in manifest["workers"]:
        proof = os.path.join(root, worker["proof"])
        hot = os.path.join(root, worker["hot"])
        log = os.path.join(root, worker["log"])
        sparse = os.path.join(root, worker["sparse"])
        if sha256_file(proof) != worker["proof_sha256"]:
            raise AssertionError(f"proof hash mismatch: {proof}")
        if sha256_file(hot) != worker["hot_sha256"]:
            raise AssertionError(f"hot hash mismatch: {hot}")
        if os.getxattr(hot, b"user.infiltratorfs-endurance").decode("ascii") != worker["final_xattr"]:
            raise AssertionError(f"xattr mismatch: {hot}")
        link = os.path.join(root, worker["final_link"])
        sym = os.path.join(root, worker["final_sym"])
        if os.stat(link).st_ino != os.stat(hot).st_ino:
            raise AssertionError(f"hard-link inode mismatch: {link}")
        if os.readlink(sym) != "hot.bin":
            raise AssertionError(f"symlink target mismatch: {sym}")
        if os.stat(log).st_size != worker["log_size"]:
            raise AssertionError(f"log size mismatch: {log}")
        if sha256_file(log) != worker["log_sha256"]:
            raise AssertionError(f"log hash mismatch: {log}")
        if os.stat(sparse).st_size != worker["sparse_size"]:
            raise AssertionError(f"sparse size mismatch: {sparse}")
        if tail_block_digest(sparse) != worker["sparse_tail_sha256"]:
            raise AssertionError(f"sparse tail mismatch: {sparse}")
        ns = os.path.join(os.path.dirname(hot), "namespace")
        if len(os.listdir(ns)) != worker["namespace_count"]:
            raise AssertionError(f"namespace count mismatch: {ns}")
        if namespace_digest(ns) != worker["namespace_sha256"]:
            raise AssertionError(f"namespace digest mismatch: {ns}")
    print("Native near-full/mixed durable verification: PASS")


def main() -> int:
    args = parse_args()
    root = os.path.abspath(args.root)

    if args.verify_manifest:
        verify_manifest(root, args.verify_manifest)
        return 0

    if args.seconds < 1 or args.workers < 1:
        raise SystemExit("--seconds and --workers must be positive")
    os.mkdir(root)

    total = total_bytes(root)
    reserve = args.reserve_mib * MIB
    if reserve >= total and not args.skip_fill:
        raise SystemExit("reserve must be smaller than the mounted filesystem")

    fragment = {}
    if not args.skip_fill:
        fragment = fill_fragmented(root, reserve, args.coarse_mib * MIB)

    started = time.monotonic()
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(worker_loop, root, worker, args.seconds)
                   for worker in range(args.workers)]
        workers = [future.result() for future in concurrent.futures.as_completed(futures)]
    elapsed = time.monotonic() - started
    workers.sort(key=lambda item: int(item["worker"]))

    os.sync()
    final_free = free_bytes(root)
    operation_count = sum(
        sum(int(value) for value in worker["counts"].values())
        for worker in workers
    )
    fsync_count = sum(int(worker["counts"]["fsync"]) for worker in workers)
    mean_fsync = (
        sum(float(worker["fsync_mean_ms"]) * int(worker["counts"]["fsync"]) for worker in workers)
        / fsync_count if fsync_count else 0.0
    )
    max_fsync = max(float(worker["fsync_max_ms"]) for worker in workers)

    print(
        f"[ENDURANCE-PERF] mixed seconds={args.seconds} workers={args.workers} "
        f"operations={operation_count} elapsed={elapsed:.3f}s "
        f"rate={operation_count / elapsed:.1f} ops/s"
    )
    print(
        f"[ENDURANCE-PERF] fsync count={fsync_count} mean_ms={mean_fsync:.3f} "
        f"max_ms={max_fsync:.3f}"
    )
    print(
        f"[ENDURANCE-PERF] final free_mib={final_free / MIB:.1f} "
        f"free_pct={100.0 * final_free / total:.2f}%"
    )

    manifest = {
        "version": 1,
        "total_bytes": total,
        "final_free_bytes": final_free,
        "fragmentation": fragment,
        "workers": workers,
    }
    if args.manifest_out:
        with open(args.manifest_out, "w", encoding="utf-8") as stream:
            json.dump(manifest, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())

    usage = resource.getrusage(resource.RUSAGE_SELF)
    print(f"[ENDURANCE-PERF] controller maxrss_kib={usage.ru_maxrss}")
    print("Native near-full, fragmentation and mixed workload stress: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
