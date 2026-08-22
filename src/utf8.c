// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/utf8.h"
#include "infiltratr/utf8.h"

int infs_utf8_validate(const void *bytes, size_t length)
{
    return infiltratr_utf8_validate(bytes, length) ? 1 : 0;
}
