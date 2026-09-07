#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:-.}"
grep -Fq 'INFS_HAVE_OPENSSL_SHA256' "$root/CMakeLists.txt"
grep -Fq 'INFS_HAVE_BCRYPT_SHA256' "$root/CMakeLists.txt"
grep -Fq 'EVP_DigestUpdate' "$root/src/checksum.c"
grep -Fq 'BCryptHash' "$root/src/checksum.c"
grep -Fq 'crypto_alloc_shash("sha256"' "$root/kernel/infiltratorfs_crypto.c"
! grep -Fq 'file_other_reference_cover' "$root/src/volume/reflink.inc"
grep -Fq 'shared_ref_index_lookup' "$root/src/volume/reflink.inc"
grep -Fq 'shared_ref_index_rebuild' "$root/src/volume/shared-ref-index.inc"

# Extent heads must contain one root pointer; no fixed page-pointer ceiling may
# remain in portable extent validation or rewriting.
grep -Fq 'extent_page_next_block' "$root/src/volume/paged-extents.inc"
grep -Fq 'sizeof(*file) + sizeof(*head) + sizeof(uint64_t)' "$root/src/volume/paged-extents.inc"
! grep -Fq 'new_pages > INFS_EXTENT_PAGE_POINTERS' "$root/src/volume/paged-extents.inc"
! grep -Fq 'pages > INFS_EXTENT_PAGE_POINTERS' "$root/src/volume/paged-extents.inc"

# The chain link is part of bytes_used, so metadata finalization checksums it
# instead of zeroing it as unused tail padding.
grep -Fq 'bytes != extent_bytes + sizeof(uint64_t)' "$root/src/volume/paged-extents.inc"
grep -Fq 'extent_bytes + sizeof(encoded)' "$root/src/volume/paged-extents.inc"

# File-head validation must scale with chain length and retain one root pointer.
grep -Fq 'pages > count' "$root/src/volume/file-layout.inc"
grep -Fq 'sizeof(*p) + sizeof(*head) + sizeof(uint64_t)' "$root/src/volume/file-layout.inc"
! grep -Fq 'pages > INFS_EXTENT_PAGE_POINTERS' "$root/src/volume/file-layout.inc"
