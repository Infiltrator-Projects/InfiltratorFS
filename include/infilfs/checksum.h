// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_CHECKSUM_H
#define INFILFS_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

uint64_t infs_crc64_ecma(const void *data, size_t len);
void infs_sha256(const void *data, size_t len, uint8_t out[32]);

#endif
