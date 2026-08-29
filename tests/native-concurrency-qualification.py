#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mounted native locking/concurrency qualification for InfiltratorFS."""

import hashlib
import multiprocessing as mp
import os
import sys
import traceback

WORKERS = 6
FILES_PER_WORKER = 24
FILE_BYTES = 32 * 1024
REGION_BYTES = 1024 * 1024
CHUNK_BYTES = 64 * 1024


def fail(queue, phase, worker):
    queue.put((phase, worker, traceback.format_exc()))


def payload(worker, index):
    seed = (worker * 37 + index * 19 + 11) & 0xFF
    return bytes(((seed + offset * 13) & 0xFF) for offset in range(FILE_BYTES))


def write_all(fd, data, offset=None):
    done = 0
    while done < len(data):
        if offset is None:
            count = os.write(fd, data[done:])
        else:
            count = os.pwrite(fd, data[done:], offset + done)
        if count <= 0:
            raise OSError("short write")
        done += count


def namespace_worker(root, worker, start, errors):
    try:
        start.wait()
        for index in range(FILES_PER_WORKER):
            tmp = os.path.join(root, f"w{worker:02d}-{index:03d}.tmp")
            final = os.path.join(root, f"w{worker:02d}-{index:03d}.dat")
            link = final + ".link"
            data = payload(worker, index)
            fd = os.open(tmp, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
            try:
                write_all(fd, data)
                os.fsync(fd)
            finally:
                os.close(fd)
            os.rename(tmp, final)
            os.link(final, link)
            os.setxattr(final, b"user.infiltratorfs-concurrency",
                        f"{worker}:{index}".encode("ascii"))
            os.unlink(link)
            if index % 6 == 0:
                dfd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(dfd)
                finally:
                    os.close(dfd)
    except BaseException:
        fail(errors, "namespace", worker)


def shared_file_worker(path, worker, start, errors):
    try:
        start.wait()
        fd = os.open(path, os.O_RDWR)
        try:
            base = worker * REGION_BYTES
            chunks = REGION_BYTES // CHUNK_BYTES
            for chunk in range(chunks):
                value = (worker * 41 + chunk * 23 + 7) & 0xFF
                data = bytes([value]) * CHUNK_BYTES
                write_all(fd, data, base + chunk * CHUNK_BYTES)
                if chunk % 4 == 3:
                    os.fsync(fd)
            os.fsync(fd)
        finally:
            os.close(fd)
    except BaseException:
        fail(errors, "shared-file", worker)


def xattr_writer_worker(path, worker, start, errors):
    try:
        name = f"user.infiltratorfs-rw-{worker}".encode("ascii")
        start.wait()
        for iteration in range(128):
            value = f"writer-{worker}-iteration-{iteration:03d}".encode("ascii")
            os.setxattr(path, name, value)
            if os.getxattr(path, name) != value:
                raise AssertionError("xattr writer readback mismatch")
    except BaseException:
        fail(errors, "xattr-writer", worker)


def xattr_reader_worker(path, start, errors):
    try:
        names = [f"user.infiltratorfs-rw-{worker}" for worker in range(3)]
        start.wait()
        for _ in range(256):
            listed = os.listxattr(path)
            for worker, name in enumerate(names):
                if name not in listed:
                    raise AssertionError(f"missing xattr during read: {name}")
                value = os.getxattr(path, name.encode("ascii"))
                prefix = f"writer-{worker}-iteration-".encode("ascii")
                if not value.startswith(prefix):
                    raise AssertionError(f"invalid xattr value: {name}")
    except BaseException:
        fail(errors, "xattr-reader", os.getpid())


def open_unlink_worker(root, worker, start, errors):
    try:
        start.wait()
        path = os.path.join(root, f"open-unlink-{worker:02d}")
        first = f"before-{worker}\n".encode("ascii")
        second = f"after-{worker}\n".encode("ascii")
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        try:
            write_all(fd, first)
            os.fsync(fd)
            os.unlink(path)
            os.lseek(fd, 0, os.SEEK_END)
            write_all(fd, second)
            os.fsync(fd)
            os.lseek(fd, 0, os.SEEK_SET)
            if os.read(fd, len(first) + len(second)) != first + second:
                raise AssertionError("open-unlink descriptor data mismatch")
        finally:
            os.close(fd)
        if os.path.exists(path):
            raise AssertionError("unlinked path reappeared")
    except BaseException:
        fail(errors, "open-unlink", worker)


def stable_reader(path, digest, stop, errors):
    try:
        loops = 0
        while not stop.is_set() and loops < 256:
            h = hashlib.sha256()
            with open(path, "rb", buffering=0) as stream:
                while True:
                    chunk = stream.read(128 * 1024)
                    if not chunk:
                        break
                    h.update(chunk)
            if h.hexdigest() != digest:
                raise AssertionError("stable reader observed corruption")
            loops += 1
        if loops == 0:
            raise AssertionError("stable reader performed no reads")
    except BaseException:
        fail(errors, "reader", 0)


def run_group(ctx, phase, target, args, errors, timeout=120):
    start = ctx.Event()
    processes = [
        ctx.Process(target=target, args=(*item, start, errors),
                    name=f"infiltratorfs-{phase}-{index}")
        for index, item in enumerate(args)
    ]
    for process in processes:
        process.start()
    start.set()
    for process in processes:
        process.join(timeout)
        if process.is_alive():
            process.terminate()
            process.join(10)
            raise RuntimeError(f"{phase}: worker {process.name} timed out")
        if process.exitcode != 0:
            raise RuntimeError(
                f"{phase}: worker {process.name} exited {process.exitcode}")


def drain_errors(errors):
    found = []
    while not errors.empty():
        found.append(errors.get())
    if found:
        details = "\n".join(
            f"[{phase} worker {worker}]\n{trace}"
            for phase, worker, trace in found
        )
        raise RuntimeError("concurrency worker failure:\n" + details)


def verify_namespace(root):
    expected = WORKERS * FILES_PER_WORKER
    names = sorted(name for name in os.listdir(root) if name.endswith(".dat"))
    if len(names) != expected:
        raise AssertionError(
            f"namespace count {len(names)} != expected {expected}")
    for worker in range(WORKERS):
        for index in range(FILES_PER_WORKER):
            path = os.path.join(root, f"w{worker:02d}-{index:03d}.dat")
            with open(path, "rb") as stream:
                actual = stream.read()
            if actual != payload(worker, index):
                raise AssertionError(f"payload mismatch: {path}")
            expected_xattr = f"{worker}:{index}".encode("ascii")
            if os.getxattr(path, b"user.infiltratorfs-concurrency") != expected_xattr:
                raise AssertionError(f"xattr mismatch: {path}")


def verify_shared_file(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        chunks = REGION_BYTES // CHUNK_BYTES
        for worker in range(WORKERS):
            base = worker * REGION_BYTES
            for chunk in range(chunks):
                value = (worker * 41 + chunk * 23 + 7) & 0xFF
                expected = bytes([value]) * CHUNK_BYTES
                actual = os.pread(fd, CHUNK_BYTES,
                                  base + chunk * CHUNK_BYTES)
                if actual != expected:
                    raise AssertionError(
                        f"shared-file mismatch worker={worker} chunk={chunk}")
    finally:
        os.close(fd)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: native-concurrency-qualification.py MOUNTED_TEST_DIRECTORY")
    root = os.path.abspath(sys.argv[1])
    os.makedirs(root, mode=0o700, exist_ok=False)

    ctx = mp.get_context("fork")
    errors = ctx.Queue()

    stable = os.path.join(root, "stable-reader.bin")
    stable_pattern = bytes((i * 29 + 5) & 0xFF for i in range(1024 * 1024))
    with open(stable, "wb", buffering=0) as stream:
        stream.write(stable_pattern)
        stream.write(stable_pattern)
        os.fsync(stream.fileno())
    stable_digest = hashlib.sha256(stable_pattern * 2).hexdigest()

    stop_reader = ctx.Event()
    reader = ctx.Process(target=stable_reader,
                         args=(stable, stable_digest, stop_reader, errors),
                         name="infiltratorfs-stable-reader")
    reader.start()

    shared_dir = os.path.join(root, "shared-directory")
    os.mkdir(shared_dir)
    run_group(
        ctx, "namespace", namespace_worker,
        [(shared_dir, worker) for worker in range(WORKERS)], errors)
    drain_errors(errors)
    verify_namespace(shared_dir)

    xattr_target = os.path.join(shared_dir, "w00-000.dat")
    for worker in range(3):
        os.setxattr(
            xattr_target,
            f"user.infiltratorfs-rw-{worker}".encode("ascii"),
            f"writer-{worker}-iteration-000".encode("ascii"),
        )
    xattr_start = ctx.Event()
    xattr_processes = [
        ctx.Process(
            target=xattr_writer_worker,
            args=(xattr_target, worker, xattr_start, errors),
            name=f"infiltratorfs-xattr-writer-{worker}",
        )
        for worker in range(3)
    ] + [
        ctx.Process(
            target=xattr_reader_worker,
            args=(xattr_target, xattr_start, errors),
            name=f"infiltratorfs-xattr-reader-{reader}",
        )
        for reader in range(3)
    ]
    for process in xattr_processes:
        process.start()
    xattr_start.set()
    for process in xattr_processes:
        process.join(120)
        if process.is_alive():
            process.terminate()
            process.join(10)
            raise RuntimeError(f"xattr contention: {process.name} timed out")
        if process.exitcode != 0:
            raise RuntimeError(
                f"xattr contention: {process.name} exited {process.exitcode}")
    drain_errors(errors)
    for worker in range(3):
        expected = f"writer-{worker}-iteration-127".encode("ascii")
        actual = os.getxattr(
            xattr_target, f"user.infiltratorfs-rw-{worker}".encode("ascii"))
        if actual != expected:
            raise AssertionError(f"final xattr mismatch for writer {worker}")

    shared_file = os.path.join(root, "shared-file.bin")
    fd = os.open(shared_file, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    try:
        os.ftruncate(fd, WORKERS * REGION_BYTES)
        os.fsync(fd)
    finally:
        os.close(fd)
    run_group(
        ctx, "shared-file", shared_file_worker,
        [(shared_file, worker) for worker in range(WORKERS)], errors)
    drain_errors(errors)
    verify_shared_file(shared_file)

    run_group(
        ctx, "open-unlink", open_unlink_worker,
        [(root, worker) for worker in range(WORKERS)], errors)
    drain_errors(errors)

    stop_reader.set()
    reader.join(60)
    if reader.is_alive():
        reader.terminate()
        reader.join(10)
        raise RuntimeError("stable reader timed out")
    if reader.exitcode != 0:
        raise RuntimeError(f"stable reader exited {reader.exitcode}")
    drain_errors(errors)

    dfd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(dfd)
    finally:
        os.close(dfd)

    print(
        "Native concurrency qualification: PASS "
        f"({WORKERS} mutators, {WORKERS * FILES_PER_WORKER} files, "
        f"{WORKERS} shared-inode writers, 3 xattr writers/readers, "
        f"{WORKERS} open-unlink writers)")


if __name__ == "__main__":
    main()
