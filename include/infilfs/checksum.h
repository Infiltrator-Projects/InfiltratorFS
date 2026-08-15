// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_CHECKSUM_H
#define INFILFS_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

uint64_t infs_crc64_ecma(const void *data, size_t len);

#endif
