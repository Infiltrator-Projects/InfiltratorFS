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
