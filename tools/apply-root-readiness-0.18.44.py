#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]

def replace(path, old, new, count=1):
    p = root / path
    text = p.read_text()
    if old not in text:
        raise SystemExit(f"anchor not found in {path}: {old[:100]!r}")
    if text.count(old) < count:
        raise SystemExit(f"anchor count too small in {path}")
    text = text.replace(old, new, count)
    p.write_text(text)

def write(path, content):
    p = root / path
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content)

# Development version. Format remains 0.18.
replace("CMakeLists.txt",
        "project(InfiltratorFS VERSION 0.18.43 LANGUAGES C)\n# Development qualification anchor: 0.18.43",
        "project(InfiltratorFS VERSION 0.18.44 LANGUAGES C)\n# Development qualification anchor: 0.18.44")

# Native O_TMPFILE needs an anonymous file object with zero persistent links.
replace("kernel/infiltratorfs_rw.inc",
'''static int infilfs_posix_create_object_native(
    struct infilfs_native_pending *pending, struct inode *parent,
    u16 type, umode_t mode, u64 portable_flags,
    const u8 object_id[16], u64 *block_out)''',
'''static int infilfs_posix_create_object_native(
    struct infilfs_native_pending *pending, struct inode *parent,
    u16 type, umode_t mode, u64 portable_flags, u64 initial_links,
    const u8 object_id[16], u64 *block_out)''')

replace("kernel/infiltratorfs_rw.inc",
'''        infilfs_posix_fill_common_attributes(&file->attributes,
                                              &file->posix, mode, 1);''',
'''        infilfs_posix_fill_common_attributes(&file->attributes,
                                              &file->posix, mode, initial_links);''')

replace("kernel/infiltratorfs_rw.inc",
'''        infilfs_posix_fill_common_attributes(&dir->attributes,
                                              &dir->posix, mode, 2);''',
'''        infilfs_posix_fill_common_attributes(&dir->attributes,
                                              &dir->posix, mode, initial_links);''')

replace("kernel/infiltratorfs_rw.inc",
'''        ret = infilfs_posix_create_object_native(
            pending, dir, type, mode, portable_flags,
            object_id, &object_block);''',
'''        ret = infilfs_posix_create_object_native(
            pending, dir, type, mode, portable_flags,
            type == INFILFS_OBJECT_DIRECTORY ? 2u : 1u,
            object_id, &object_block);''')

anchor = '''static int infilfs_posix_create_native_child(
    struct inode *dir, struct dentry *dentry, umode_t mode, u16 type,
    u8 object_id_out[16], u64 *block_out)
{'''
tmp_impl = r'''
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_tmpfile(struct mnt_idmap *idmap,
                                 struct inode *dir,
                                 struct file *file, umode_t mode)
#else
static int infilfs_posix_tmpfile(struct user_namespace *idmap,
                                 struct inode *dir,
                                 struct file *file, umode_t mode)
#endif
{
    struct infilfs_native_pending *pending = NULL;
    struct infilfs_ns_index_change change;
    struct infilfs_quota_reservation quota = {0};
    struct inode *inode;
    u8 object_id[16];
    u64 object_block = 0;
    int ret;

    (void)idmap;
    ret = infilfs_quota_reserve_create(dir, 0, 1, &quota);
    if (ret)
        return ret;
    ret = infilfs_ns_begin(dir->i_sb, &pending);
    if (ret)
        goto abort_quota;
    ret = infilfs_rw_generate_id(dir->i_sb, object_id);
    if (!ret)
        ret = infilfs_posix_create_object_native(
            pending, dir, INFILFS_OBJECT_FILE, mode, 0, 0,
            object_id, &object_block);
    if (!ret) {
        memset(&change, 0, sizeof(change));
        memcpy(change.object_id, object_id, sizeof(change.object_id));
        change.object_block = object_block;
        change.object_type = INFILFS_OBJECT_FILE;
        change.action = INFILFS_NS_ADD;
        ret = infilfs_ns_rebuild_index(pending, &change, 1);
    }
    ret = infilfs_ns_finish(pending, ret);
    pending = NULL;
    if (ret)
        goto abort_quota;

    inode = infilfs_get_inode(dir->i_sb, object_block,
                              INFILFS_OBJECT_FILE, object_id);
    if (IS_ERR(inode)) {
        ret = PTR_ERR(inode);
        goto abort_quota;
    }
    clear_nlink(inode);
    d_tmpfile(file, inode);
    infilfs_quota_reservation_finish(&quota, 0, 1);
    return finish_open_simple(file, 0);

abort_quota:
    infilfs_quota_reservation_abort(&quota);
    return ret;
}

'''
replace("kernel/infiltratorfs_rw.inc", anchor, tmp_impl + anchor)

acl_anchor = '''#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static struct dentry *infilfs_posix_acl_mkdir('''
acl_tmp = r'''
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_posix_acl_tmpfile(
    struct mnt_idmap *idmap, struct inode *dir,
    struct file *file, umode_t mode)
#else
static int infilfs_posix_acl_tmpfile(
    struct user_namespace *idmap, struct inode *dir,
    struct file *file, umode_t mode)
#endif
{
    struct posix_acl *default_acl = NULL;
    struct posix_acl *access_acl = NULL;
    struct inode *inode = NULL;
    int ret;

    ret = infilfs_posix_acl_prepare_create(
        dir, &mode, S_IFREG, &default_acl, &access_acl);
    if (!ret)
        ret = infilfs_posix_tmpfile(idmap, dir, file, mode);
    if (!ret) {
        inode = file_inode(file);
        if (!inode)
            ret = -EIO;
        else if (access_acl)
            ret = infilfs_posix_acl_store(
                idmap, NULL, inode, ACL_TYPE_ACCESS, access_acl);
        if (!ret && inode)
            set_cached_acl(inode, ACL_TYPE_ACCESS, access_acl);
    }
    posix_acl_release(access_acl);
    posix_acl_release(default_acl);
    return ret;
}

'''
replace("kernel/infiltratorfs_linux_meta.inc", acl_anchor, acl_tmp + acl_anchor)

replace("kernel/infiltratorfs_rw.inc",
'''#define infilfs_rw_create infilfs_posix_acl_create, \\
    .mknod = infilfs_posix_acl_mknod, \\''',
'''#define infilfs_rw_create infilfs_posix_acl_create, \\
    .mknod = infilfs_posix_acl_mknod, \\
    .tmpfile = infilfs_posix_acl_tmpfile, \\''')

rename_anchor = '''#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_ns_rename(struct mnt_idmap *idmap,
                             struct inode *old_dir, struct dentry *old_dentry,
                             struct inode *new_dir, struct dentry *new_dentry,
                             unsigned int flags)
#else'''
exchange_helper = r'''
static int infilfs_ns_rename_exchange_same_dir(
    struct inode *dir, struct dentry *old_dentry,
    struct dentry *new_dentry)
{
    struct inode *source = d_inode(old_dentry);
    struct inode *destination = d_inode(new_dentry);
    struct infilfs_inode_info *dir_ii = INFILFS_I(dir);
    struct infilfs_inode_info *source_ii = source ? INFILFS_I(source) : NULL;
    struct infilfs_inode_info *destination_ii =
        destination ? INFILFS_I(destination) : NULL;
    struct infilfs_native_pending *pending;
    struct infilfs_ns_index_change change;
    u64 first_dir_block = 0;
    u64 final_dir_block = 0;
    u64 original_dir_block;
    int ret;

    if (!source || !destination || !source_ii || !destination_ii || !dir_ii)
        return -ENOENT;
    ret = infilfs_ns_validate_name(&old_dentry->d_name);
    if (!ret)
        ret = infilfs_ns_validate_name(&new_dentry->d_name);
    if (ret)
        return ret;
    if (source == destination)
        return 0;

    ret = infilfs_ns_begin(dir->i_sb, &pending);
    if (ret)
        return ret;
    original_dir_block = dir_ii->object_block;
    ret = infilfs_ns_rebuild_directory(
        pending, dir, &old_dentry->d_name, &new_dentry->d_name,
        &old_dentry->d_name, destination_ii->object_id,
        destination_ii->object_type, 0, &first_dir_block);
    if (!ret) {
        dir_ii->object_block = first_dir_block;
        ret = infilfs_ns_rebuild_directory(
            pending, dir, NULL, NULL,
            &new_dentry->d_name, source_ii->object_id,
            source_ii->object_type, 0, &final_dir_block);
    }
    dir_ii->object_block = original_dir_block;
    if (!ret) {
        memset(&change, 0, sizeof(change));
        memcpy(change.object_id, dir_ii->object_id, sizeof(change.object_id));
        change.object_block = final_dir_block;
        change.object_type = INFILFS_OBJECT_DIRECTORY;
        change.action = INFILFS_NS_REPOINT;
        ret = infilfs_ns_rebuild_index(pending, &change, 1);
    }
    ret = infilfs_ns_finish(pending, ret);
    if (!ret) {
        dir_ii->object_block = final_dir_block;
        infilfs_refresh_inode_blocks_after_commit(dir);
    }
    return ret;
}

'''
replace("kernel/infiltratorfs_rw_namespace.inc", rename_anchor,
        exchange_helper + rename_anchor)

replace("kernel/infiltratorfs_rw_namespace.inc",
'''    if (flags & ~(RENAME_NOREPLACE))
        return -EINVAL;
    if ((flags & RENAME_NOREPLACE) && destination)
        return -EEXIST;''',
'''    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT))
        return -EINVAL;
    if (flags & RENAME_WHITEOUT)
        return -EOPNOTSUPP;
    if (flags & RENAME_EXCHANGE) {
        if (flags != RENAME_EXCHANGE)
            return -EINVAL;
        if (!destination)
            return -ENOENT;
        if (old_dir != new_dir)
            return -EOPNOTSUPP;
        return infilfs_ns_rename_exchange_same_dir(
            old_dir, old_dentry, new_dentry);
    }
    if ((flags & RENAME_NOREPLACE) && destination)
        return -EEXIST;''')

replace("include/infilfs/volume.h",
'''struct infs_scrub_report {
    uint64_t files_checked;
    uint64_t data_blocks_checked;
    uint64_t checksum_errors;
    uint64_t metadata_errors;
    uint64_t scrub_generation;
    uint64_t snapshots_checked;
};
''',
'''struct infs_scrub_report {
    uint64_t files_checked;
    uint64_t data_blocks_checked;
    uint64_t checksum_errors;
    uint64_t metadata_errors;
    uint64_t scrub_generation;
    uint64_t snapshots_checked;
};

struct infs_compression_metrics {
    uint64_t generation;
    uint64_t files_scanned;
    uint64_t snapshots_scanned;
    uint64_t referenced_logical_bytes;
    uint64_t compressed_referenced_logical_bytes;
    uint64_t unique_compressed_logical_bytes;
    uint64_t unique_compressed_physical_bytes;
    uint64_t compression_saved_bytes;
    uint64_t unique_compressed_streams;
};
''')

replace("include/infilfs/volume.h",
'''infs_status infs_scrub(struct infs_volume *vol,
                       struct infs_scrub_report *report);''',
'''infs_status infs_scrub(struct infs_volume *vol,
                       struct infs_scrub_report *report);
/*
 * Report physical compression savings without conflating compression with
 * sparse holes or reflink/snapshot sharing. The live generation and all named
 * retained snapshots are scanned; an identical compressed physical stream is
 * counted once in the unique/saved fields even when referenced many times.
 */
infs_status infs_compression_metrics(
    struct infs_volume *vol, struct infs_compression_metrics *metrics);''')

write("src/volume/compression-metrics.inc", r'''// SPDX-License-Identifier: GPL-3.0-or-later
struct infs_metric_stream {
    uint64_t physical;
    uint64_t physical_blocks;
    uint64_t logical_blocks;
    uint32_t codec;
};

struct infs_metric_scan {
    struct infs_metric_stream *streams;
    size_t count;
    size_t capacity;
    struct infs_compression_metrics *metrics;
};

static int infs_metric_stream_compare(const void *left, const void *right)
{
    const struct infs_metric_stream *a = left;
    const struct infs_metric_stream *b = right;
    if (a->physical != b->physical)
        return a->physical < b->physical ? -1 : 1;
    if (a->physical_blocks != b->physical_blocks)
        return a->physical_blocks < b->physical_blocks ? -1 : 1;
    if (a->logical_blocks != b->logical_blocks)
        return a->logical_blocks < b->logical_blocks ? -1 : 1;
    if (a->codec != b->codec)
        return a->codec < b->codec ? -1 : 1;
    return 0;
}

static infs_status infs_metric_append_stream(
    struct infs_metric_scan *scan, uint64_t physical,
    uint64_t physical_blocks, uint64_t logical_blocks, uint32_t codec)
{
    if (scan->count == scan->capacity) {
        size_t next = scan->capacity ? scan->capacity * 2u : 256u;
        if (next < scan->capacity ||
            next > SIZE_MAX / sizeof(*scan->streams))
            return INFS_STATUS_OVERFLOW;
        void *grown = realloc(scan->streams, next * sizeof(*scan->streams));
        if (!grown)
            return INFS_STATUS_NO_MEMORY;
        scan->streams = grown;
        scan->capacity = next;
    }
    scan->streams[scan->count++] = (struct infs_metric_stream) {
        .physical = physical,
        .physical_blocks = physical_blocks,
        .logical_blocks = logical_blocks,
        .codec = codec,
    };
    return INFS_STATUS_OK;
}

static infs_status infs_metric_add_checked(uint64_t *value, uint64_t add)
{
    if (*value > UINT64_MAX - add)
        return INFS_STATUS_OVERFLOW;
    *value += add;
    return INFS_STATUS_OK;
}

static infs_status infs_metric_scan_view(
    struct infs_volume *vol, struct infs_metric_scan *scan)
{
    struct infs_index_entry_disk *entries = NULL;
    uint32_t entry_count = 0;
    infs_status status = index_snapshot(vol, &entries, &entry_count);
    if (status != INFS_STATUS_OK)
        return status;

    for (uint32_t i = 0; i < entry_count; ++i) {
        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_FILE)
            continue;

        uint8_t object[INFS_BLOCK_SIZE];
        struct infs_object_header_disk *header;
        struct infs_file_payload_disk *file;
        struct infs_extent_disk *extents;
        struct infs_extent_disk *owned_extents;
        uint32_t extent_count;
        int owns_extents = 0;

        status = read_object(
            vol, infs_le64_to_cpu(entries[i].object_block), object);
        if (status != INFS_STATUS_OK)
            goto out;
        header = (struct infs_object_header_disk *)object;
        if (memcmp(header->object_id, entries[i].object_id, 16) != 0) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
        if (file_validate_volume(vol, object, &file, &extents) !=
            INFS_STATUS_OK) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
        status = infs_metric_add_checked(
            &scan->metrics->referenced_logical_bytes,
            infs_le64_to_cpu(file->attributes.logical_size));
        if (status != INFS_STATUS_OK)
            goto out;
        status = infs_metric_add_checked(&scan->metrics->files_scanned, 1);
        if (status != INFS_STATUS_OK)
            goto out;
        if (file_is_inline(object, file))
            continue;

        extent_count = infs_le32_to_cpu(file->extent_count);
        owned_extents = extents;
        status = file_extent_snapshot(
            vol, object, file, extents, &owned_extents,
            &extent_count, &owns_extents);
        if (status != INFS_STATUS_OK)
            goto out;

        for (uint32_t j = 0; j < extent_count; ++j) {
            uint32_t flags = infs_le32_to_cpu(owned_extents[j].flags);
            uint64_t logical_blocks;
            uint64_t physical_blocks;
            uint64_t logical_bytes;

            if (!extent_is_compressed(flags))
                continue;
            logical_blocks =
                infs_le32_to_cpu(owned_extents[j].block_count);
            physical_blocks =
                extent_physical_blocks((uint32_t)logical_blocks, flags);
            if (!physical_blocks ||
                logical_blocks > UINT64_MAX / INFS_BLOCK_SIZE) {
                status = INFS_STATUS_CORRUPT;
                break;
            }
            logical_bytes = logical_blocks * INFS_BLOCK_SIZE;
            status = infs_metric_add_checked(
                &scan->metrics->compressed_referenced_logical_bytes,
                logical_bytes);
            if (status != INFS_STATUS_OK)
                break;
            status = infs_metric_append_stream(
                scan,
                infs_le64_to_cpu(owned_extents[j].physical_block),
                physical_blocks, logical_blocks, extent_codec(flags));
            if (status != INFS_STATUS_OK)
                break;
        }
        if (owns_extents)
            free(owned_extents);
        if (status != INFS_STATUS_OK)
            goto out;
    }
out:
    free(entries);
    return status;
}

infs_status infs_compression_metrics(
    struct infs_volume *vol, struct infs_compression_metrics *metrics)
{
    struct infs_metric_scan scan = {0};
    struct infs_snapshot_info *snapshots = NULL;
    size_t snapshot_count = 0;
    infs_status status;

    if (!vol || !metrics)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(metrics, 0, sizeof(*metrics));
    metrics->generation = infs_le64_to_cpu(vol->sb.generation);
    scan.metrics = metrics;

    status = infs_metric_scan_view(vol, &scan);
    if (status != INFS_STATUS_OK)
        goto out;

    status = infs_snapshot_list(vol, &snapshots, &snapshot_count);
    if (status != INFS_STATUS_OK)
        goto out;
    for (size_t i = 0; i < snapshot_count; ++i) {
        struct infs_volume view;
        status = snapshot_open_view(vol, snapshots[i].name, &view);
        if (status != INFS_STATUS_OK)
            goto out;
        status = infs_metric_scan_view(&view, &scan);
        snapshot_close_view(&view);
        if (status != INFS_STATUS_OK)
            goto out;
        status = infs_metric_add_checked(&metrics->snapshots_scanned, 1);
        if (status != INFS_STATUS_OK)
            goto out;
    }

    if (scan.count) {
        qsort(scan.streams, scan.count, sizeof(*scan.streams),
              infs_metric_stream_compare);
        for (size_t i = 0; i < scan.count; ++i) {
            if (i &&
                scan.streams[i].physical == scan.streams[i - 1].physical &&
                scan.streams[i].physical_blocks ==
                    scan.streams[i - 1].physical_blocks) {
                if (scan.streams[i].logical_blocks !=
                        scan.streams[i - 1].logical_blocks ||
                    scan.streams[i].codec != scan.streams[i - 1].codec) {
                    status = INFS_STATUS_CORRUPT;
                    goto out;
                }
                continue;
            }
            if (scan.streams[i].logical_blocks >
                    UINT64_MAX / INFS_BLOCK_SIZE ||
                scan.streams[i].physical_blocks >
                    UINT64_MAX / INFS_BLOCK_SIZE) {
                status = INFS_STATUS_OVERFLOW;
                goto out;
            }
            uint64_t logical =
                scan.streams[i].logical_blocks * INFS_BLOCK_SIZE;
            uint64_t physical =
                scan.streams[i].physical_blocks * INFS_BLOCK_SIZE;
            if (physical >= logical) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            status = infs_metric_add_checked(
                &metrics->unique_compressed_logical_bytes, logical);
            if (status == INFS_STATUS_OK)
                status = infs_metric_add_checked(
                    &metrics->unique_compressed_physical_bytes, physical);
            if (status == INFS_STATUS_OK)
                status = infs_metric_add_checked(
                    &metrics->compression_saved_bytes, logical - physical);
            if (status == INFS_STATUS_OK)
                status = infs_metric_add_checked(
                    &metrics->unique_compressed_streams, 1);
            if (status != INFS_STATUS_OK)
                goto out;
        }
    }
out:
    infs_free_snapshot_infos(snapshots);
    free(scan.streams);
    return status;
}
''')

replace("src/volume.c",
'''#include "volume/scrub.inc"
#include "volume/metadata-hardening.inc"''',
'''#include "volume/scrub.inc"
#include "volume/compression-metrics.inc"
#include "volume/metadata-hardening.inc"''')

write("tools/infilfs-compression.c", r'''// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/posix_io.h"
#include "infilfs/volume.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static int block_device_is_mounted(const char *path)
{
    struct stat st;
    FILE *stream;
    char *line = NULL;
    size_t capacity = 0;
    int mounted = 0;

    if (stat(path, &st) != 0 || !S_ISBLK(st.st_mode))
        return 0;
    stream = fopen("/proc/self/mountinfo", "r");
    if (!stream)
        return -1;
    while (getline(&line, &capacity, stream) >= 0) {
        unsigned int major_no = 0;
        unsigned int minor_no = 0;
        if (sscanf(line, "%*u %*u %u:%u", &major_no, &minor_no) == 2 &&
            major_no == major(st.st_rdev) &&
            minor_no == minor(st.st_rdev)) {
            mounted = 1;
            break;
        }
    }
    free(line);
    fclose(stream);
    return mounted;
}

static void print_bytes(const char *label, uint64_t value)
{
    printf("  %-34s %" PRIu64 " bytes (%.2f GiB)\n",
           label, value, (double)value / 1073741824.0);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr,
            "Usage: %s <unmounted-infiltratorfs-image-or-device>\n",
            argv[0]);
        return 2;
    }

    int mounted = block_device_is_mounted(argv[1]);
    if (mounted < 0) {
        fprintf(stderr, "infilfs-compression: cannot inspect mount table\n");
        return 1;
    }
    if (mounted) {
        fprintf(stderr,
            "infilfs-compression: refusing direct-device scan of a mounted "
            "filesystem; unmount it first\n");
        return 1;
    }

    struct infs_volume vol;
    infs_status status = infs_posix_volume_open(&vol, argv[1], 0);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "infilfs-compression: open: %s\n",
                infs_status_string(status));
        return 1;
    }

    struct infs_compression_metrics metrics;
    status = infs_compression_metrics(&vol, &metrics);
    infs_volume_close(&vol);
    if (status != INFS_STATUS_OK) {
        fprintf(stderr, "infilfs-compression: metrics: %s\n",
                infs_status_string(status));
        return 1;
    }

    printf("InfiltratorFS compression metrics\n");
    printf("  Generation:                        %" PRIu64 "\n",
           metrics.generation);
    printf("  Files scanned:                     %" PRIu64 "\n",
           metrics.files_scanned);
    printf("  Retained snapshots scanned:        %" PRIu64 "\n",
           metrics.snapshots_scanned);
    print_bytes("Referenced logical file bytes:",
                metrics.referenced_logical_bytes);
    print_bytes("Compressed referenced logical:",
                metrics.compressed_referenced_logical_bytes);
    print_bytes("Unique compressed logical:",
                metrics.unique_compressed_logical_bytes);
    print_bytes("Unique compressed physical:",
                metrics.unique_compressed_physical_bytes);
    print_bytes("Physical space saved by compression:",
                metrics.compression_saved_bytes);
    printf("  Unique compressed streams:         %" PRIu64 "\n",
           metrics.unique_compressed_streams);
    if (metrics.unique_compressed_logical_bytes) {
        double pct = 100.0 * (double)metrics.compression_saved_bytes /
            (double)metrics.unique_compressed_logical_bytes;
        printf("  Compression saving on compressed data: %.2f%%\n", pct);
    } else {
        puts("  Compression saving on compressed data: 0.00%");
    }
    puts("  Note: savings exclude sparse holes and reflink/snapshot sharing.");
    return 0;
}
''')

replace("CMakeLists.txt",
'''    add_executable(infilfs-optimize tools/infilfs-optimize.c)
    add_executable(infiltratorfs-resize tools/infiltratorfs-resize.c)''',
'''    add_executable(infilfs-optimize tools/infilfs-optimize.c)
    add_executable(infilfs-compression tools/infilfs-compression.c)
    add_executable(infiltratorfs-resize tools/infiltratorfs-resize.c)''')

replace("CMakeLists.txt",
'''    foreach(tool_target IN ITEMS mkfs.infilfs infilfs-inspect infilfs-tool infilfs-scrub infilfs-forensic infilfs-optimize infiltratorfs-resize infiltratorfs-quota)''',
'''    foreach(tool_target IN ITEMS mkfs.infilfs infilfs-inspect infilfs-tool infilfs-scrub infilfs-forensic infilfs-optimize infilfs-compression infiltratorfs-resize infiltratorfs-quota)''')

replace("CMakeLists.txt",
'''    install(TARGETS mkfs.infilfs infilfs-inspect infilfs-tool infilfs-scrub infilfs-forensic infilfs-optimize infiltratorfs-resize infiltratorfs-quota''',
'''    install(TARGETS mkfs.infilfs infilfs-inspect infilfs-tool infilfs-scrub infilfs-forensic infilfs-optimize infilfs-compression infiltratorfs-resize infiltratorfs-quota''')

write("tests/native-linux-api.c", r'''// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void die(const char *what)
{
    perror(what);
    exit(1);
}

static int rename_exchange(const char *a, const char *b)
{
    return syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, RENAME_EXCHANGE);
}

static void read_exact(const char *path, const char *expected)
{
    char buffer[64] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die("open read");
    ssize_t got = read(fd, buffer, sizeof(buffer) - 1u);
    close(fd);
    if (got < 0)
        die("read");
    if ((size_t)got != strlen(expected) ||
        memcmp(buffer, expected, strlen(expected)) != 0) {
        fprintf(stderr, "%s contains unexpected data\n", path);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <mounted-directory>\n", argv[0]);
        return 2;
    }
    int dirfd = open(argv[1], O_RDONLY | O_DIRECTORY);
    if (dirfd < 0)
        die("open directory");

    int fd = openat(dirfd, ".", O_TMPFILE | O_RDWR, 0640);
    if (fd < 0)
        die("O_TMPFILE");
    if (write(fd, "anonymous-data", 14) != 14)
        die("write tmpfile");
    if (fsync(fd) != 0)
        die("fsync tmpfile");
    if (linkat(fd, "", dirfd, "linked-tmpfile", AT_EMPTY_PATH) != 0)
        die("linkat tmpfile");
    close(fd);
    close(dirfd);

    char a[4096], b[4096], linked[4096];
    if (snprintf(a, sizeof(a), "%s/exchange-a", argv[1]) >= (int)sizeof(a) ||
        snprintf(b, sizeof(b), "%s/exchange-b", argv[1]) >= (int)sizeof(b) ||
        snprintf(linked, sizeof(linked), "%s/linked-tmpfile", argv[1]) >=
            (int)sizeof(linked))
        return 2;
    int fa = open(a, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    int fb = open(b, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fa < 0 || fb < 0)
        die("create exchange");
    if (write(fa, "alpha", 5) != 5 || write(fb, "bravo", 5) != 5)
        die("write exchange");
    close(fa);
    close(fb);
    if (rename_exchange(a, b) != 0)
        die("RENAME_EXCHANGE");
    read_exact(a, "bravo");
    read_exact(b, "alpha");
    read_exact(linked, "anonymous-data");

    errno = 0;
    if (syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b,
                RENAME_NOREPLACE) == 0 || errno != EEXIST) {
        fprintf(stderr, "RENAME_NOREPLACE did not return EEXIST\n");
        return 1;
    }
    puts("Native Linux API compatibility: PASS");
    return 0;
}
''')

write("tests/native-root-metadata-qualification.sh", r'''#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build}"
module="${2:-kernel/infiltratorfs.ko}"
root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
image="$work/root-metadata.img"
mountpoint="$work/mnt"
src="$work/source"
loop=""

cleanup() {
    set +e
    sync
    mountpoint -q "$mountpoint" && umount "$mountpoint"
    [[ -n "$loop" ]] && losetup -d "$loop" 2>/dev/null
    rmmod infiltratorfs 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

for cmd in setfacl getfacl setfattr getfattr setcap getcap rsync losetup mount umount; do
    command -v "$cmd" >/dev/null
done
[[ $EUID -eq 0 ]] || { echo "must run as root" >&2; exit 2; }

mkdir -p "$mountpoint" "$src"
truncate -s 2G "$image"
"$build_dir/mkfs.infilfs" "$image" >/dev/null
loop="$(losetup --find --show "$image")"
insmod "$module"
mount -t infiltratorfs "$loop" "$mountpoint"

mkdir "$mountpoint/meta"
touch "$mountpoint/meta/owner"
chown 12345:23456 "$mountpoint/meta/owner"
chmod 0640 "$mountpoint/meta/owner"
[[ "$(stat -c '%u:%g:%a' "$mountpoint/meta/owner")" == "12345:23456:640" ]]

cp /bin/true "$mountpoint/meta/capability"
setcap cap_net_bind_service=ep "$mountpoint/meta/capability"
getcap "$mountpoint/meta/capability" | grep -Fq cap_net_bind_service

setfattr -n user.infiltratorfs -v root-metadata "$mountpoint/meta/owner"
[[ "$(getfattr --only-values -n user.infiltratorfs "$mountpoint/meta/owner")" == "root-metadata" ]]
setfattr -n trusted.infiltratorfs -v trusted-value "$mountpoint/meta/owner"
[[ "$(getfattr --only-values -n trusted.infiltratorfs "$mountpoint/meta/owner")" == "trusted-value" ]]

touch "$mountpoint/meta/setuid"
chmod 4755 "$mountpoint/meta/setuid"
[[ "$(stat -c '%a' "$mountpoint/meta/setuid")" == "4755" ]]
mkdir "$mountpoint/meta/setgid" "$mountpoint/meta/sticky"
chmod 2755 "$mountpoint/meta/setgid"
chmod 1777 "$mountpoint/meta/sticky"
[[ "$(stat -c '%a' "$mountpoint/meta/setgid")" == "2755" ]]
[[ "$(stat -c '%a' "$mountpoint/meta/sticky")" == "1777" ]]

mkfifo "$mountpoint/meta/fifo"
mknod "$mountpoint/meta/char-null" c 1 3
[[ -p "$mountpoint/meta/fifo" && -c "$mountpoint/meta/char-null" ]]
python3 - "$mountpoint/meta/socket" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX)
s.bind(sys.argv[1])
s.close()
PY
[[ -S "$mountpoint/meta/socket" ]]
rm "$mountpoint/meta/socket"

printf 'hardlink-data' > "$mountpoint/meta/hard-a"
ln "$mountpoint/meta/hard-a" "$mountpoint/meta/hard-b"
[[ "$(stat -c %i "$mountpoint/meta/hard-a")" == "$(stat -c %i "$mountpoint/meta/hard-b")" ]]
ln -s hard-a "$mountpoint/meta/symlink"
[[ "$(readlink "$mountpoint/meta/symlink")" == hard-a ]]

python3 - "$mountpoint/meta/owner" <<'PY'
import os, sys
at = 1700000000123456789
mt = 1700000000987654321
os.utime(sys.argv[1], ns=(at, mt))
st = os.stat(sys.argv[1])
assert st.st_atime_ns == at, (st.st_atime_ns, at)
assert st.st_mtime_ns == mt, (st.st_mtime_ns, mt)
PY

setfacl -m u:12345:r-- "$mountpoint/meta/owner"
getfacl -n "$mountpoint/meta/owner" | grep -Eq '^user:12345:r--$'

mkdir -p "$src/tree"
printf 'clone-data' > "$src/tree/file"
ln "$src/tree/file" "$src/tree/file-hard"
ln -s file "$src/tree/file-link"
truncate -s 16M "$src/tree/sparse"
printf x | dd of="$src/tree/sparse" bs=1 seek=$((8*1024*1024)) conv=notrunc status=none
chown 12345:23456 "$src/tree/file" "$src/tree/file-hard"
chmod 0640 "$src/tree/file"
setfacl -m u:12345:r-- "$src/tree/file"
setfattr -n user.clone -v yes "$src/tree/file"
rsync -aHAXS --numeric-ids "$src/tree/" "$mountpoint/clone/"
[[ "$(stat -c '%u:%g:%a' "$mountpoint/clone/file")" == "12345:23456:640" ]]
[[ "$(stat -c %i "$mountpoint/clone/file")" == "$(stat -c %i "$mountpoint/clone/file-hard")" ]]
[[ "$(readlink "$mountpoint/clone/file-link")" == file ]]
getfacl -n "$mountpoint/clone/file" | grep -Eq '^user:12345:r--$'
[[ "$(getfattr --only-values -n user.clone "$mountpoint/clone/file")" == yes ]]

cc -O2 -Wall -Wextra "$root/tests/native-linux-api.c" -o "$work/native-linux-api"
mkdir "$mountpoint/meta/api"
"$work/native-linux-api" "$mountpoint/meta/api"
sync
umount "$mountpoint"
mount -t infiltratorfs "$loop" "$mountpoint"

[[ "$(stat -c '%u:%g:%a' "$mountpoint/meta/owner")" == "12345:23456:640" ]]
getcap "$mountpoint/meta/capability" | grep -Fq cap_net_bind_service
[[ "$(getfattr --only-values -n trusted.infiltratorfs "$mountpoint/meta/owner")" == "trusted-value" ]]
[[ -p "$mountpoint/meta/fifo" && -c "$mountpoint/meta/char-null" ]]
[[ "$(stat -c %i "$mountpoint/meta/hard-a")" == "$(stat -c %i "$mountpoint/meta/hard-b")" ]]
[[ "$(readlink "$mountpoint/meta/symlink")" == hard-a ]]
getfacl -n "$mountpoint/meta/owner" | grep -Eq '^user:12345:r--$'
[[ "$(cat "$mountpoint/meta/api/linked-tmpfile")" == anonymous-data ]]
[[ "$(cat "$mountpoint/meta/api/exchange-a")" == bravo ]]
sync
umount "$mountpoint"

"$build_dir/infilfs-scrub" "$loop" | tee "$work/scrub.txt"
grep -Fq 'Result:              CLEAN' "$work/scrub.txt"
echo 'Native Linux full root metadata qualification: PASS'
''')

replace(".github/workflows/root-volume-qualification.yml",
'''            acl build-essential cmake dkms e2fsprogs fontconfig initramfs-tools \\
            kmod linux-headers-generic policykit-1 rsync util-linux xdg-utils \\
            python3 python3-gi gir1.2-gtk-3.0''',
'''            acl attr build-essential cmake dkms e2fsprogs fontconfig initramfs-tools \\
            kmod libcap2-bin linux-headers-generic policykit-1 rsync util-linux xdg-utils \\
            python3 python3-gi gir1.2-gtk-3.0''')

rootstep_anchor = '''      - name: Build Debian package with root-volume integration
        run: |'''
rootstep = r'''      - name: Require complete Linux root metadata and API semantics
        if: steps.headers.outputs.runtime_kdir != ''
        run: |
          set -euo pipefail
          sudo bash tests/native-root-metadata-qualification.sh \
            build kernel/infiltratorfs.ko

'''
replace(".github/workflows/root-volume-qualification.yml",
        rootstep_anchor, rootstep + rootstep_anchor)

write("tests/compression-metrics.c", r'''// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/fs.h"
#include "infilfs/volume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct image { unsigned char *bytes; size_t size; };
static infs_status rd(void *c, uint64_t o, void *b, size_t s) {
    struct image *i=c; if (o>i->size || s>i->size-(size_t)o) return INFS_STATUS_IO_ERROR;
    memcpy(b,i->bytes+(size_t)o,s); return INFS_STATUS_OK;
}
static infs_status wr(void *c, uint64_t o, const void *b, size_t s) {
    struct image *i=c; if (o>i->size || s>i->size-(size_t)o) return INFS_STATUS_IO_ERROR;
    memcpy(i->bytes+(size_t)o,b,s); return INFS_STATUS_OK;
}
static infs_status fl(void *c){(void)c; return INFS_STATUS_OK;}
static void expect(int ok, const char *m){if(!ok){fprintf(stderr,"FAIL: %s\n",m);exit(1);}}

int main(void) {
    struct image image = { calloc(1, 64u*1024u*1024u), 64u*1024u*1024u };
    expect(image.bytes != NULL, "allocate");
    struct infs_storage st = { .ctx=&image, .read=rd, .write=wr, .flush=fl,
                               .size_bytes=image.size };
    expect(infs_format_storage(&st, "compression-metrics") == INFS_STATUS_OK, "format");
    struct infs_volume vol;
    expect(infs_volume_open_storage(&vol,&st,1) == INFS_STATUS_OK, "open");
    expect(infs_create_file(&vol,"/data",NULL) == INFS_STATUS_OK, "create");
    size_t n=4u*1024u*1024u;
    unsigned char *buf=malloc(n); expect(buf!=NULL,"buffer");
    for(size_t i=0;i<n;++i) buf[i]=(unsigned char)("AAAAAAAABBBBBBBB"[i&15]);
    expect(infs_write_file(&vol,"/data",buf,n,0) == (int64_t)n, "write");
    struct infs_compression_metrics one;
    expect(infs_compression_metrics(&vol,&one) == INFS_STATUS_OK, "metrics one");
    expect(one.compression_saved_bytes > 0, "saved bytes");
    expect(one.unique_compressed_physical_bytes < one.unique_compressed_logical_bytes,
           "physical less logical");
    expect(infs_reflink_file(&vol,"/data","/clone") == INFS_STATUS_OK, "reflink");
    expect(infs_snapshot_create(&vol,"retained") == INFS_STATUS_OK, "snapshot");
    struct infs_compression_metrics shared;
    expect(infs_compression_metrics(&vol,&shared) == INFS_STATUS_OK, "shared metrics");
    expect(shared.snapshots_scanned == 1, "snapshot scanned");
    expect(shared.compressed_referenced_logical_bytes >
           one.compressed_referenced_logical_bytes, "references grow");
    expect(shared.compression_saved_bytes == one.compression_saved_bytes,
           "unique physical compression saving deduped");
    free(buf);
    infs_volume_close(&vol);
    free(image.bytes);
    puts("Compression metrics: PASS");
    return 0;
}
''')

replace("CMakeLists.txt",
'''add_executable(infilfs-compression-qualification tests/compression-qualification.c)
target_link_libraries(infilfs-compression-qualification PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-compression-qualification)
''',
'''add_executable(infilfs-compression-qualification tests/compression-qualification.c)
target_link_libraries(infilfs-compression-qualification PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-compression-qualification)

add_executable(infilfs-compression-metrics tests/compression-metrics.c)
target_link_libraries(infilfs-compression-metrics PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-compression-metrics)
''')

replace("CMakeLists.txt",
'''add_test(NAME infilfs-compression-qualification COMMAND infilfs-compression-qualification)
add_test(NAME infilfs-paged-metadata''',
'''add_test(NAME infilfs-compression-qualification COMMAND infilfs-compression-qualification)
add_test(NAME infilfs-compression-metrics COMMAND infilfs-compression-metrics)
add_test(NAME infilfs-paged-metadata''')

write("tests/root-boot-qemu.sh", r'''#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-build-root-boot}"
module="${2:-kernel/infiltratorfs.ko}"
work="${RUNNER_TEMP:-/tmp}/infiltratorfs-root-boot"
disk="$work/root-boot.raw"
mnt="$work/mnt"
loop=""
qemu_pid=""

cleanup() {
    set +e
    [[ -n "$qemu_pid" ]] && kill -9 "$qemu_pid" 2>/dev/null
    sync
    for p in "$mnt/run" "$mnt/sys" "$mnt/proc" "$mnt/dev/pts" "$mnt/dev" \
             "$mnt/boot/efi" "$mnt/boot" "$mnt"; do
        mountpoint -q "$p" && umount -l "$p"
    done
    [[ -n "$loop" ]] && losetup -d "$loop" 2>/dev/null
    rmmod infiltratorfs 2>/dev/null
}
trap cleanup EXIT

[[ $EUID -eq 0 ]] || { echo "root boot qualification requires root" >&2; exit 2; }
for cmd in qemu-system-x86_64 qemu-img debootstrap sfdisk losetup mkfs.vfat mkfs.ext4 \
           grub-install chroot timeout; do command -v "$cmd" >/dev/null; done

rm -rf "$work"
mkdir -p "$work" "$mnt"
cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --parallel
bash "$root/packaging/build-linux-packages.sh" "$build" "$work/base-dist"
INFILTRATORFS_PACKAGE_VERSION=0.18.44+rootci1 \
INFILTRATORFS_EMIT_RUN=0 \
bash "$root/packaging/build-linux-packages.sh" "$build" "$work/upgrade-dist"
base_deb="$(find "$work/base-dist" -name 'infiltratorfs_*.deb' -print -quit)"
upgrade_deb="$(find "$work/upgrade-dist" -name 'infiltratorfs_*.deb' -print -quit)"
[[ -s "$base_deb" && -s "$upgrade_deb" ]]

truncate -s 10G "$disk"
sfdisk "$disk" >/dev/null <<'EOF'
label: gpt
size=256M,type=U
size=768M,type=L
type=L
EOF
loop="$(losetup --find --show -P "$disk")"
mkfs.vfat -F32 "${loop}p1" >/dev/null
mkfs.ext4 -F "${loop}p2" >/dev/null
"$build/mkfs.infilfs" -L RootBoot "${loop}p3" >/dev/null

insmod "$module"
mount -t infiltratorfs "${loop}p3" "$mnt"
mkdir -p "$mnt/boot"
mount "${loop}p2" "$mnt/boot"
mkdir -p "$mnt/boot/efi"
mount "${loop}p1" "$mnt/boot/efi"

debootstrap --variant=minbase noble "$mnt" http://archive.ubuntu.com/ubuntu
cp "$base_deb" "$mnt/root/infiltratorfs-base.deb"
cp "$upgrade_deb" "$mnt/root/infiltratorfs-upgrade.deb"

mount --bind /dev "$mnt/dev"
mount --bind /dev/pts "$mnt/dev/pts"
mount -t proc proc "$mnt/proc"
mount -t sysfs sys "$mnt/sys"
mount --bind /run "$mnt/run"
cp /etc/resolv.conf "$mnt/etc/resolv.conf"

root_uuid="$(blkid -s UUID -o value "${loop}p3")"
boot_uuid="$(blkid -s UUID -o value "${loop}p2")"
efi_uuid="$(blkid -s UUID -o value "${loop}p1")"
cat >"$mnt/etc/fstab" <<EOF
UUID=$root_uuid / infiltratorfs defaults 0 0
UUID=$boot_uuid /boot ext4 defaults 0 2
UUID=$efi_uuid /boot/efi vfat umask=0077 0 1
EOF
echo infiltrator-root-ci > "$mnt/etc/hostname"
cat >"$mnt/etc/hosts" <<'EOF'
127.0.0.1 localhost
127.0.1.1 infiltrator-root-ci
EOF

chroot "$mnt" /bin/bash -eux <<'CHROOT'
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  linux-image-generic linux-headers-generic systemd-sysv initramfs-tools \
  grub-efi-amd64-bin grub2-common dkms kmod acl attr libcap2-bin rsync \
  ca-certificates passwd util-linux
dpkg -i /root/infiltratorfs-base.deb || apt-get -f install -y
dpkg -i /root/infiltratorfs-base.deb
cat >/etc/default/grub <<'EOF'
GRUB_DEFAULT=0
GRUB_TIMEOUT=1
GRUB_CMDLINE_LINUX_DEFAULT="console=ttyS0,115200n8"
GRUB_TERMINAL=console
EOF
grub-install --target=x86_64-efi --efi-directory=/boot/efi \
  --bootloader-id=InfiltratorRootCI --removable --no-nvram
update-initramfs -c -k all || update-initramfs -u -k all
update-grub
CHROOT

cat >"$mnt/usr/local/sbin/infiltrator-root-ci" <<'GUEST'
#!/bin/bash
set -euo pipefail
exec >/dev/ttyS0 2>&1
fstype="$(findmnt -n -o FSTYPE /)"
[[ "$fstype" == infiltratorfs ]] || { echo "ROOT_FSTYPE_FAIL:$fstype"; poweroff -f; }
phase="$(cat /var/lib/infiltrator-root-phase 2>/dev/null || echo 1)"
mkdir -p /var/lib/infiltrator-root-ci
if [[ "$phase" == 1 ]]; then
    echo "ROOT_BOOT_PASS"
    printf root-persistent >/var/lib/infiltrator-root-ci/persistent
    mkdir -p /home/root-ci /var/lib/infiltrator-root-ci/acl
    touch /var/lib/infiltrator-root-ci/acl/file
    chown 12345:23456 /var/lib/infiltrator-root-ci/acl/file
    chmod 0640 /var/lib/infiltrator-root-ci/acl/file
    setfacl -m u:12345:r-- /var/lib/infiltrator-root-ci/acl/file
    setfattr -n user.rootci -v yes /var/lib/infiltrator-root-ci/acl/file
    cp /bin/true /var/lib/infiltrator-root-ci/cap
    setcap cap_net_bind_service=ep /var/lib/infiltrator-root-ci/cap
    useradd -m rootcitest
    journalctl --sync || true

    mkdir -p /tmp/rootci-pkg/DEBIAN
    cat >/tmp/rootci-pkg/DEBIAN/control <<'EOF'
Package: rootci-workload
Version: 1.0
Architecture: all
Maintainer: CI <ci@example.invalid>
Description: InfiltratorFS root workload package
EOF
    mkdir -p /tmp/rootci-pkg/usr/share/rootci
    echo package-data >/tmp/rootci-pkg/usr/share/rootci/data
    dpkg-deb -b /tmp/rootci-pkg /tmp/rootci-workload.deb
    dpkg -i /tmp/rootci-workload.deb
    dpkg -r rootci-workload

    dpkg -i /root/infiltratorfs-upgrade.deb
    update-initramfs -u -k all
    dpkg --audit
    echo 2 >/var/lib/infiltrator-root-phase
    sync
    echo "ROOT_UPGRADE_STAGED_PASS"
    systemctl reboot
elif [[ "$phase" == 2 ]]; then
    [[ "$(dpkg-query -W -f='${Version}' infiltratorfs)" == "0.18.44+rootci1" ]]
    [[ "$(cat /var/lib/infiltrator-root-ci/persistent)" == root-persistent ]]
    getfacl -n /var/lib/infiltrator-root-ci/acl/file | grep -Eq '^user:12345:r--$'
    [[ "$(getfattr --only-values -n user.rootci /var/lib/infiltrator-root-ci/acl/file)" == yes ]]
    getcap /var/lib/infiltrator-root-ci/cap | grep -Fq cap_net_bind_service
    echo 3 >/var/lib/infiltrator-root-phase
    sync
    echo "ROOT_CRASH_READY"
    i=0
    while :; do
        printf '%08d\n' "$i" >>/var/lib/infiltrator-root-ci/crash-stream
        i=$((i+1))
        if (( i % 128 == 0 )); then sync; fi
    done
else
    [[ "$(findmnt -n -o FSTYPE /)" == infiltratorfs ]]
    [[ "$(dpkg-query -W -f='${Version}' infiltratorfs)" == "0.18.44+rootci1" ]]
    dpkg --audit
    [[ "$(cat /var/lib/infiltrator-root-ci/persistent)" == root-persistent ]]
    getfacl -n /var/lib/infiltrator-root-ci/acl/file | grep -Eq '^user:12345:r--$'
    getcap /var/lib/infiltrator-root-ci/cap | grep -Fq cap_net_bind_service
    echo "ROOT_RECOVERY_PASS"
    poweroff
fi
GUEST
chmod +x "$mnt/usr/local/sbin/infiltrator-root-ci"
cat >"$mnt/etc/systemd/system/infiltrator-root-ci.service" <<'EOF'
[Unit]
Description=InfiltratorFS root boot qualification
After=local-fs.target
Before=multi-user.target
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/infiltrator-root-ci
TimeoutStartSec=0
[Install]
WantedBy=multi-user.target
EOF
ln -sf ../infiltrator-root-ci.service \
  "$mnt/etc/systemd/system/multi-user.target.wants/infiltrator-root-ci.service"

sync
for p in "$mnt/run" "$mnt/sys" "$mnt/proc" "$mnt/dev/pts" "$mnt/dev" \
         "$mnt/boot/efi" "$mnt/boot" "$mnt"; do
    mountpoint -q "$p" && umount "$p"
done
losetup -d "$loop"
loop=""
rmmod infiltratorfs

ovmf_code="$(find /usr/share/OVMF -maxdepth 1 -type f -name 'OVMF_CODE*.fd' | head -n1)"
ovmf_vars_template="$(find /usr/share/OVMF -maxdepth 1 -type f -name 'OVMF_VARS*.fd' | head -n1)"
[[ -s "$ovmf_code" && -s "$ovmf_vars_template" ]]
cp "$ovmf_vars_template" "$work/OVMF_VARS.fd"

boot_once() {
    local log="$1"
    : >"$log"
    qemu-system-x86_64 -machine q35,accel=tcg -m 2048 -smp 2 \
      -nographic -serial mon:stdio \
      -drive if=pflash,format=raw,readonly=on,file="$ovmf_code" \
      -drive if=pflash,format=raw,file="$work/OVMF_VARS.fd" \
      -drive format=raw,file="$disk",if=virtio \
      -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
      >"$log" 2>&1 &
    qemu_pid=$!
}

boot_once "$work/boot12.log"
for _ in $(seq 1 360); do
    grep -Fq ROOT_CRASH_READY "$work/boot12.log" && break
    kill -0 "$qemu_pid" 2>/dev/null || { cat "$work/boot12.log"; exit 1; }
    sleep 2
done
grep -Fq ROOT_BOOT_PASS "$work/boot12.log"
grep -Fq ROOT_UPGRADE_STAGED_PASS "$work/boot12.log"
grep -Fq ROOT_CRASH_READY "$work/boot12.log"
kill -9 "$qemu_pid"
wait "$qemu_pid" 2>/dev/null || true
qemu_pid=""

loop="$(losetup --find --show -P "$disk")"
"$build/infilfs-scrub" "${loop}p3" | tee "$work/post-crash-scrub.txt"
grep -Fq 'Result:              CLEAN' "$work/post-crash-scrub.txt"
losetup -d "$loop"; loop=""

boot_once "$work/boot3.log"
for _ in $(seq 1 240); do
    grep -Fq ROOT_RECOVERY_PASS "$work/boot3.log" && break
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 2
done
grep -Fq ROOT_RECOVERY_PASS "$work/boot3.log" || {
    cat "$work/boot3.log"
    exit 1
}
wait "$qemu_pid" 2>/dev/null || true
qemu_pid=""

loop="$(losetup --find --show -P "$disk")"
"$build/infilfs-scrub" "${loop}p3" | tee "$work/final-scrub.txt"
grep -Fq 'Result:              CLEAN' "$work/final-scrub.txt"
echo 'Real UEFI InfiltratorFS root boot/upgrade/crash/recovery qualification: PASS'
''')

write(".github/workflows/root-boot-qualification.yml", r'''# SPDX-License-Identifier: GPL-3.0-or-later
name: Linux root boot qualification

on:
  push:
    branches: [main]
    paths:
      - 'kernel/**'
      - 'src/**'
      - 'include/**'
      - 'tools/**'
      - 'tests/**'
      - 'packaging/**'
      - 'CMakeLists.txt'
      - '.github/release-trigger'
      - '.github/workflows/root-boot-qualification.yml'
  workflow_dispatch:

permissions:
  contents: read

jobs:
  root-boot:
    name: UEFI boot, live upgrade, crash and recovery
    runs-on: ubuntu-24.04
    timeout-minutes: 55
    steps:
      - uses: actions/checkout@v6
        with:
          submodules: recursive
      - name: Install root boot dependencies
        env:
          DEBIAN_FRONTEND: noninteractive
        run: |
          set -euo pipefail
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends \
            acl attr build-essential cmake debootstrap dkms dosfstools \
            e2fsprogs fontconfig grub-efi-amd64-bin initramfs-tools kmod \
            libcap2-bin linux-headers-generic ovmf policykit-1 \
            qemu-system-x86 qemu-utils rsync systemd-container util-linux \
            xdg-utils python3 python3-gi gir1.2-gtk-3.0
          sudo apt-get install -y --no-install-recommends \
            "linux-headers-$(uname -r)"
      - name: Build running-kernel module
        run: |
          set -euo pipefail
          make -C kernel KDIR="/lib/modules/$(uname -r)/build"
          test "$(modinfo -F vermagic kernel/infiltratorfs.ko | awk '{print $1}')" = "$(uname -r)"
      - name: Boot real InfiltratorFS root and recover forced power loss
        run: |
          set -euo pipefail
          sudo -E bash tests/root-boot-qemu.sh build-root-boot kernel/infiltratorfs.ko
''')

release = root / ".github/workflows/release-packages.yml"
text = release.read_text()
needle = '''      - name: Record heavy qualification policy
        shell: bash
        run: |'''
wait_steps = r'''      - name: Require root-volume qualification for the same commit
        timeout-minutes: 35
        env:
          EXPECTED_SHA: ${{ github.event.workflow_run.head_sha }}
          GH_TOKEN: ${{ github.token }}
        shell: bash
        run: |
          set -euo pipefail
          api="repos/${GITHUB_REPOSITORY}/actions/runs?head_sha=${EXPECTED_SHA}&event=push&per_page=30"
          for attempt in $(seq 1 180); do
            mapfile -t gate < <(
              gh api "$api" --jq '
                first(.workflow_runs[] |
                  select(.name == "Linux root-volume qualification")) |
                .status, (.conclusion // ""), .html_url'
            )
            status="${gate[0]:-}"
            conclusion="${gate[1]:-}"
            run_url="${gate[2]:-}"
            if [[ "$status" == completed ]]; then
              [[ "$conclusion" == success ]] || {
                echo "Root-volume qualification failed: $conclusion ($run_url)" >&2
                exit 1
              }
              echo "Root-volume qualification passed: $run_url"
              exit 0
            fi
            sleep 10
          done
          echo "Timed out waiting for root-volume qualification." >&2
          exit 1

      - name: Require real root-boot qualification for the same commit
        timeout-minutes: 55
        env:
          EXPECTED_SHA: ${{ github.event.workflow_run.head_sha }}
          GH_TOKEN: ${{ github.token }}
        shell: bash
        run: |
          set -euo pipefail
          api="repos/${GITHUB_REPOSITORY}/actions/runs?head_sha=${EXPECTED_SHA}&event=push&per_page=30"
          for attempt in $(seq 1 300); do
            mapfile -t gate < <(
              gh api "$api" --jq '
                first(.workflow_runs[] |
                  select(.name == "Linux root boot qualification")) |
                .status, (.conclusion // ""), .html_url'
            )
            status="${gate[0]:-}"
            conclusion="${gate[1]:-}"
            run_url="${gate[2]:-}"
            if [[ "$status" == completed ]]; then
              [[ "$conclusion" == success ]] || {
                echo "Root-boot qualification failed: $conclusion ($run_url)" >&2
                exit 1
              }
              echo "Root-boot qualification passed: $run_url"
              exit 0
            fi
            sleep 10
          done
          echo "Timed out waiting for real root-boot qualification." >&2
          exit 1

'''
if needle not in text:
    raise SystemExit("release gate anchor not found")
release.write_text(text.replace(needle, wait_steps + needle, 1))

roadmap = root / "docs/ROADMAP.md"
rtext = roadmap.read_text()
marker = "- [x] POSIX access/default ACLs with VFS enforcement, inheritance and chmod-mask semantics."
if marker in rtext:
    rtext = rtext.replace(marker, marker + "\n"
        "- [x] Linux O_TMPFILE creation/linking and same-directory RENAME_EXCHANGE.\n"
        "- [x] Full Linux root metadata preservation qualification (ACL/xattr/capability/special nodes/timestamps/rsync).\n"
        "- [x] Real UEFI root boot, live package upgrade, forced-power-loss recovery and post-recovery scrub qualification.\n"
        "- [x] Exact compression-savings metrics with reflink/snapshot stream deduplication.", 1)
roadmap.write_text(rtext)

comp = root / "docs/COMPRESSION.md"
ctext = comp.read_text()
ctext += r'''

## Physical savings reporting

`infilfs-compression <unmounted-image-or-device>` reports compression separately
from sparse-hole and reflink/snapshot savings. It scans the live generation and
all named retained snapshots, deduplicates identical compressed physical codec
streams, and reports unique compressed logical bytes, unique compressed
physical bytes, and the resulting physical bytes saved. Direct-device scans
refuse mounted block devices so the portable reader never races the native
kernel writer.
'''
comp.write_text(ctext)

qual = root / "docs/QUALIFICATION.md"
qtext = qual.read_text()
qtext += r'''

### Linux system-root release gate

Every release-source commit must now pass the dedicated root-volume gate and the
real UEFI root-boot gate in addition to ordinary build/conformance and native
kernel qualification. The boot gate constructs an EFI + ext4 `/boot` +
InfiltratorFS `/` VM, reaches systemd with InfiltratorFS as `/`, exercises
metadata and dpkg workloads, installs a newer InfiltratorFS package while the
root is live, rebuilds initramfs, reboots, forces power loss during writes,
requires an offline CLEAN scrub, boots the same root again, verifies dpkg and
metadata state, and requires a final CLEAN scrub.
'''
qual.write_text(qtext)

write("tests/root-readiness-policy.sh", r'''#!/usr/bin/env bash
set -euo pipefail
root="${1:-.}"
grep -Fq '.tmpfile = infilfs_posix_acl_tmpfile' "$root/kernel/infiltratorfs_rw.inc"
grep -Fq 'RENAME_EXCHANGE' "$root/kernel/infiltratorfs_rw_namespace.inc"
grep -Fq 'infs_compression_metrics' "$root/include/infilfs/volume.h"
grep -Fq 'infilfs-compression' "$root/CMakeLists.txt"
grep -Fq 'Linux root boot qualification' "$root/.github/workflows/release-packages.yml"
grep -Fq 'Linux root-volume qualification' "$root/.github/workflows/release-packages.yml"
grep -Fq 'ROOT_RECOVERY_PASS' "$root/tests/root-boot-qemu.sh"
! grep -R -Fq 'INFS_IAC1_MIN_SAVINGS_DIVISOR' \
    "$root/kernel" "$root/src" "$root/include" "$root/tests" || exit 1
echo 'Root readiness policy: PASS'
''')

replace("CMakeLists.txt",
'''    add_test(NAME infilfs-root-volume-integration-policy
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/root-volume-integration-policy.sh
                     ${CMAKE_SOURCE_DIR})''',
'''    add_test(NAME infilfs-root-volume-integration-policy
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/root-volume-integration-policy.sh
                     ${CMAKE_SOURCE_DIR})
    add_test(NAME infilfs-root-readiness-policy
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/root-readiness-policy.sh
                     ${CMAKE_SOURCE_DIR})''')

trigger = root / ".github/release-trigger"
if trigger.exists():
    trigger.write_text("0.18.44-dev-1\n")

print("root-readiness 0.18.44 patch applied")
