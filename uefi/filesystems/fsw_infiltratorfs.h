// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _FSW_INFILTRATORFS_H_
#define _FSW_INFILTRATORFS_H_

#define VOLSTRUCTNAME fsw_infiltratorfs_volume
#define DNODESTRUCTNAME fsw_infiltratorfs_dnode
#include "fsw_core.h"

#include "infilfs/endian.h"
#include "infilfs/volume.h"

struct fsw_infiltratorfs_volume {
    struct fsw_volume g;
    struct infs_volume infs;
    int infs_open;
};

struct fsw_infiltratorfs_dnode {
    struct fsw_dnode g;
    char *path;
    struct infs_attributes attributes;
    int attributes_valid;
};

#endif
