// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/volume.h"

#include "infilfs/checksum.h"
#include "infilfs/fs.h"
#include "infilfs/io.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INFS_DIRENT_ALIGN 8u


#include "volume/part1.inc"
#include "volume/part2.inc"
#include "volume/part3.inc"
#include "volume/part4.inc"
#include "volume/part5.inc"
