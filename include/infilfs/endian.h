// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_ENDIAN_H
#define INFILFS_ENDIAN_H

#include "infiltratr/endian.h"

/* Compatibility names retained for the InfiltratorFS on-disk format code.
 * The implementation lives in Infiltratr Common 1.11+ so byte-order handling
 * is identical across projects. */
#define infs_bswap16(value) infiltratr_bswap16((uint16_t)(value))
#define infs_bswap32(value) infiltratr_bswap32((uint32_t)(value))
#define infs_bswap64(value) infiltratr_bswap64((uint64_t)(value))
#define infs_cpu_to_le16(value) infiltratr_cpu_to_le16(value)
#define infs_cpu_to_le32(value) infiltratr_cpu_to_le32(value)
#define infs_cpu_to_le64(value) infiltratr_cpu_to_le64(value)
#define infs_le16_to_cpu(value) infiltratr_le16_to_cpu(value)
#define infs_le32_to_cpu(value) infiltratr_le32_to_cpu(value)
#define infs_le64_to_cpu(value) infiltratr_le64_to_cpu(value)

#endif
