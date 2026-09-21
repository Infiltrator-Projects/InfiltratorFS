<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS 0.18.69

This release addresses the real-machine I/O bottlenecks exposed by the native
performance qualification on a fast NVMe device.

The native verified read path now submits contiguous allocated runs in bounded
4 MiB buffer-head batches before waiting, allowing the Linux block layer and
NVMe device to carry useful queue depth without bypassing InfiltratorFS SHA-256
verification. The single-block path retains bounded speculative readahead.

Linux O_DIRECT is explicitly admitted through FMODE_CAN_ODIRECT and routed
through aligned native verified read/write callbacks. Ordinary read/write
continues to use the page cache.

Transaction publication no longer calls sync_blockdev() for every fsync. After
the replacement allocation tree is staged, publication drains only the
transaction-private CoW allocation set, writes checkpoint replicas
synchronously, and retains the final block-device cache flush as the
stable-media crash-consistency boundary.

The performance harness now avoids the earlier measurement errors: metadata
operations run in one process instead of thousands of helper processes,
throughput uses incompressible input, results are returned to the invoking
user, the raw-device control is included, and fio is an optional cross-check
rather than a requirement.

The on-disk format remains 0.18.
