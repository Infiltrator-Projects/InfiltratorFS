// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_CHECKSUM_H
#define INFILFS_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

/* Stateless integrity primitives used by the portable format implementation.
 * CRC64-ECMA protects metadata against corruption; SHA-256 protects logical
 * file-data content. Neither primitive is keyed authentication and neither
 * should be presented as proof of a trusted writer. infs_sha256() writes the
 * canonical 32-byte digest to caller-owned storage. */
uint64_t infs_crc64_ecma(const void *data, size_t len);
void infs_sha256(const void *data, size_t len, uint8_t out[32]);

#endif
