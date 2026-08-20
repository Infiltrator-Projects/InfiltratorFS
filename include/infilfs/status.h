// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_STATUS_H
#define INFILFS_STATUS_H

#include <stdint.h>

/* Stable, operating-system-neutral result values. Success is zero; failures
 * are negative so byte-count APIs can return either a count or a status. */
typedef int32_t infs_status;

#define INFS_STATUS_OK                INT32_C(0)
#define INFS_STATUS_ERROR            -INT32_C(1)
#define INFS_STATUS_INVALID_ARGUMENT -INT32_C(2)
#define INFS_STATUS_IO_ERROR         -INT32_C(3)
#define INFS_STATUS_CORRUPT          -INT32_C(4)
#define INFS_STATUS_READ_ONLY        -INT32_C(5)
#define INFS_STATUS_NO_SPACE         -INT32_C(6)
#define INFS_STATUS_NOT_FOUND        -INT32_C(7)
#define INFS_STATUS_ALREADY_EXISTS   -INT32_C(8)
#define INFS_STATUS_NOT_DIRECTORY    -INT32_C(9)
#define INFS_STATUS_IS_DIRECTORY     -INT32_C(10)
#define INFS_STATUS_NOT_EMPTY        -INT32_C(11)
#define INFS_STATUS_NAME_TOO_LONG    -INT32_C(12)
#define INFS_STATUS_FILE_TOO_LARGE   -INT32_C(13)
#define INFS_STATUS_OVERFLOW         -INT32_C(14)
#define INFS_STATUS_LOOP_DETECTED    -INT32_C(15)
#define INFS_STATUS_NOT_SUPPORTED    -INT32_C(16)
#define INFS_STATUS_NO_MEMORY        -INT32_C(17)
#define INFS_STATUS_INTERRUPTED      -INT32_C(18)
#define INFS_STATUS_BUSY             -INT32_C(19)

const char *infs_status_string(infs_status status);

#endif
