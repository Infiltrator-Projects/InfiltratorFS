// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_IOCTL_H
#define INFILTRATORFS_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define INFILFS_IOC_MAGIC 0xf5

struct infilfs_fragmentation_metrics {
    __u64 logical_bytes;
    __u64 allocated_blocks;
    __u64 data_extents;
    __u64 hole_extents;
    __u64 largest_data_extent_blocks;
    __u64 generation;
    __u64 fragmentation_milli;
    __u64 physical_runs;
};

struct infilfs_defrag_request {
    __u64 max_bytes;
    __u64 moved_bytes;
    __u64 before_data_extents;
    __u64 after_data_extents;
    __u64 before_fragmentation_milli;
    __u64 after_fragmentation_milli;
    __u64 reserved[2];
};

#define INFILFS_RESIZE_TO_DEVICE_MAX 0x00000001u

struct infilfs_resize_request {
    __u64 size_bytes;
    __u64 old_size_bytes;
    __u64 new_size_bytes;
    __u32 flags;
    __u32 reserved0;
    __u64 reserved[3];
};

#define INFILFS_IOC_GET_FRAGMENTATION \
    _IOR(INFILFS_IOC_MAGIC, 0x01, struct infilfs_fragmentation_metrics)
#define INFILFS_IOC_DEFRAG_FILE \
    _IOWR(INFILFS_IOC_MAGIC, 0x02, struct infilfs_defrag_request)

#define INFILFS_IOC_RESIZE_VOLUME _IOWR(INFILFS_IOC_MAGIC, 0x03, struct infilfs_resize_request)

#endif
