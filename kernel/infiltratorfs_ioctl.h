// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_IOCTL_H
#define INFILTRATORFS_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * Native-driver synchronization contract
 * --------------------------------------
 * This header is included by the kernel translation unit and is copied into
 * every DKMS source tree, so the deadlock-prevention rules travel with the
 * code rather than existing only in project documentation.
 *
 * Allowed nested acquisition directions are:
 *
 *     resize_lock -> write_lock
 *     quota_lock  -> write_lock
 *     write_lock  -> bitmap_lock
 *     allocation-reservation shard spinlock -> bitmap_lock (when needed)
 *
 * The reverse directions are forbidden. bitmap_lock and reservation shard
 * spinlocks are non-sleeping inner locks; do not acquire filesystem mutexes or
 * perform sleeping work while holding them. linux_meta_lock owns compound
 * Linux sidecar metadata operations and has no general nesting permission with
 * the mutexes above: any future nesting must define one direction explicitly
 * before it is introduced.
 *
 * write_lock is also the persistent transaction/checkpoint publication domain.
 * resize_active is raised before resize drains already-started transactions and
 * reservations. Quota reservations are preflighted outside write_lock and are
 * finished or aborted only after that persistent mutation lock is released.
 *
 * Keep the fuller composition rationale in kernel/Makefile synchronized with
 * these source-level rules. tests/native-kernel-maintainability-policy.sh makes
 * the contract and current include layering machine-checked.
 */

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

#define INFILFS_QUOTA_USER    1u
#define INFILFS_QUOTA_GROUP   2u
#define INFILFS_QUOTA_PROJECT 3u

struct infilfs_quota_request {
    __u32 type;
    __u32 id;
    __u64 hard_bytes;
    __u64 hard_objects;
    __u64 used_bytes;
    __u64 used_objects;
    __u32 flags;
    __u32 reserved0;
    __u64 reserved[2];
};

struct infilfs_project_request {
    __u32 project_id;
    __u32 effective_project_id;
    __u32 flags;
    __u32 reserved0;
    __u64 reserved[2];
};
#define INFILFS_IOC_GET_FRAGMENTATION \
    _IOR(INFILFS_IOC_MAGIC, 0x01, struct infilfs_fragmentation_metrics)
#define INFILFS_IOC_DEFRAG_FILE \
    _IOWR(INFILFS_IOC_MAGIC, 0x02, struct infilfs_defrag_request)

#define INFILFS_IOC_RESIZE_VOLUME _IOWR(INFILFS_IOC_MAGIC, 0x03, struct infilfs_resize_request)

#define INFILFS_IOC_SET_QUOTA _IOWR(INFILFS_IOC_MAGIC, 0x04, struct infilfs_quota_request)
#define INFILFS_IOC_GET_QUOTA _IOWR(INFILFS_IOC_MAGIC, 0x05, struct infilfs_quota_request)
#define INFILFS_IOC_SET_PROJECT _IOWR(INFILFS_IOC_MAGIC, 0x06, struct infilfs_project_request)
#define INFILFS_IOC_GET_PROJECT _IOWR(INFILFS_IOC_MAGIC, 0x07, struct infilfs_project_request)

#endif
