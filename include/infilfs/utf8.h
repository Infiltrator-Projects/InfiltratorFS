// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_UTF8_H
#define INFILFS_UTF8_H

#include <stddef.h>
#include "infiltratr/utf8.h"

static inline int infs_utf8_validate(const void *bytes, size_t length)
{
    return infiltratr_utf8_validate(bytes, length) ? 1 : 0;
}

#endif
