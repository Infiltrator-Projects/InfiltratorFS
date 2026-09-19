<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS 0.18.59

**Published:** 2026-09-18.

Use Infiltratr Common 1.19.2 more deeply only where it improves InfiltratorFS
itself.

This release removes duplicate userspace numeric and binary-size parsing from
the resize, quota, optimize, mkfs and image tools, replaces the image tool's
hand-written exact-read loop with Common's EINTR-safe exact I/O helper, and uses
Common's overflow-safe dynamic-array reserve primitive for the portable
free-extent index, transaction allocation/deferred journals and snapshot
traversal state.

Filesystem-specific code remains local: the Linux kernel module, allocation
policy, copy-on-write machinery, persistent structures, integrity/checksum paths
and compression are unchanged. The on-disk Format 0.18 contract is unchanged.
