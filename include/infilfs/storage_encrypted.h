// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_STORAGE_ENCRYPTED_H
#define INFILFS_STORAGE_ENCRYPTED_H

#include <stddef.h>
#include <stdint.h>
#include "infilfs/storage.h"

#define INFS_ENCRYPTED_STORAGE_DEFAULT_KDF_ITERATIONS UINT32_C(600000)

/*
 * Authenticated whole-volume storage wrapper.
 *
 * The passphrase is never stored. A PBKDF2-HMAC-SHA256 derived key unwraps a
 * random 256-bit volume key, and each logical 4096-byte block is independently
 * protected with AES-256-GCM using a fresh random nonce. The block number and
 * volume salt are authenticated as associated data so ciphertext cannot be
 * relocated between blocks.
 *
 * format/open take ownership of backing only on success and clear the caller's
 * handle. The returned storage exposes a normal byte-addressed logical device
 * to the filesystem.
 */
infs_status infs_storage_encrypted_format(
    struct infs_storage *backing,
    const void *passphrase, size_t passphrase_size,
    uint32_t kdf_iterations,
    struct infs_storage *out);

infs_status infs_storage_encrypted_open(
    struct infs_storage *backing,
    const void *passphrase, size_t passphrase_size,
    struct infs_storage *out);

#endif
