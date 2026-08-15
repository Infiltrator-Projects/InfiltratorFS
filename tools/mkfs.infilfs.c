// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/io.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INFS_MIN_SIZE_BYTES (16ull * 1024ull * 1024ull)

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--force] [-L label] <image-or-block-device>\n", prog);
}

static void bitmap_set(uint8_t *bitmap, uint64_t block)
{
    bitmap[block >> 3] |= (uint8_t)(1u << (block & 7u));
}

static int bitmap_get(const uint8_t *bitmap, uint64_t block)
{
    return (bitmap[block >> 3] >> (block & 7u)) & 1u;
}

static int64_t current_time_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

int main(int argc, char **argv)
{
    int force = 0;
    const char *label = "InfiltratorFS";
    const char *path = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--force") == 0) {
            force = 1;
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!path) {
        usage(argv[0]);
        return 2;
    }
    if (strlen(label) >= INFS_LABEL_MAX) {
        fprintf(stderr, "Label is too long (maximum %u bytes).\n", INFS_LABEL_MAX - 1u);
        return 2;
    }

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    uint64_t size_bytes = 0;
    int is_block = 0;
    if (infs_get_size_bytes(fd, &size_bytes, &is_block) != 0) {
        perror("size");
        close(fd);
        return 1;
    }

    if (is_block && !force) {
        fprintf(stderr, "Refusing to format a block device without --force.\n");
        close(fd);
        return 1;
    }
    if (size_bytes < INFS_MIN_SIZE_BYTES) {
        fprintf(stderr, "Target is too small; minimum is 16 MiB.\n");
        close(fd);
        return 1;
    }

    const uint64_t total_blocks = size_bytes / INFS_BLOCK_SIZE;
    const uint64_t bitmap_bytes = (total_blocks + 7u) / 8u;
    const uint64_t bitmap_blocks = (bitmap_bytes + INFS_BLOCK_SIZE - 1u) / INFS_BLOCK_SIZE;
    const uint64_t bitmap_start = 1u;
    const uint64_t index_block = bitmap_start + bitmap_blocks;
    const uint64_t root_block = index_block + 1u;
    const uint64_t checkpoints[INFS_CHECKPOINT_COUNT] = {
        0,
        total_blocks / 2u,
        total_blocks - 1u
    };

    if (root_block >= checkpoints[1]) {
        fprintf(stderr, "Target is too small for the format 0.3 metadata layout.\n");
        close(fd);
        return 1;
    }

    size_t bitmap_alloc = (size_t)(bitmap_blocks * INFS_BLOCK_SIZE);
    uint8_t *bitmap = calloc(1, bitmap_alloc);
    if (!bitmap) {
        perror("calloc");
        close(fd);
        return 1;
    }

    bitmap_set(bitmap, checkpoints[0]);
    bitmap_set(bitmap, checkpoints[1]);
    bitmap_set(bitmap, checkpoints[2]);
    for (uint64_t b = bitmap_start; b < bitmap_start + bitmap_blocks; ++b)
        bitmap_set(bitmap, b);
    bitmap_set(bitmap, index_block);
    bitmap_set(bitmap, root_block);

    for (uint64_t b = total_blocks; b < bitmap_blocks * INFS_BLOCK_SIZE * 8ull; ++b)
        bitmap_set(bitmap, b);

    uint64_t used_blocks = 0;
    for (uint64_t b = 0; b < total_blocks; ++b)
        if (bitmap_get(bitmap, b))
            ++used_blocks;

    uint8_t fs_uuid[16];
    uint8_t root_id[16];
    uint8_t index_id[16];
    if (infs_random_bytes(fs_uuid, sizeof(fs_uuid)) != 0 ||
        infs_random_bytes(root_id, sizeof(root_id)) != 0 ||
        infs_random_bytes(index_id, sizeof(index_id)) != 0) {
        perror("getrandom");
        free(bitmap);
        close(fd);
        return 1;
    }

    struct infs_superblock_disk sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, INFS_MAGIC, 8);
    sb.format_major = htole16(INFS_FORMAT_MAJOR);
    sb.format_minor = htole16(INFS_FORMAT_MINOR);
    sb.header_size = htole16(sizeof(sb));
    sb.block_shift = htole16(INFS_BLOCK_SHIFT);
    sb.checksum_type = htole32(INFS_CHECKSUM_CRC64_ECMA);
    sb.generation = htole64(1);
    sb.total_blocks = htole64(total_blocks);
    sb.free_blocks = htole64(total_blocks - used_blocks);
    sb.bitmap_start_block = htole64(bitmap_start);
    sb.bitmap_block_count = htole64(bitmap_blocks);
    sb.object_index_block = htole64(index_block);
    sb.root_object_block = htole64(root_block);
    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i)
        sb.checkpoint_block[i] = htole64(checkpoints[i]);
    memcpy(sb.filesystem_uuid, fs_uuid, sizeof(fs_uuid));
    memcpy(sb.root_object_id, root_id, sizeof(root_id));
    memcpy(sb.label, label, strlen(label));

    uint8_t block[INFS_BLOCK_SIZE];
    if (infs_encode_superblock(block, &sb) != 0) {
        perror("encode superblock");
        free(bitmap);
        close(fd);
        return 1;
    }

    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        off_t off = (off_t)(checkpoints[i] * INFS_BLOCK_SIZE);
        if (infs_pwrite_full(fd, block, sizeof(block), off) != 0) {
            perror("write checkpoint");
            free(bitmap);
            close(fd);
            return 1;
        }
    }

    if (infs_pwrite_full(fd, bitmap, bitmap_alloc,
                         (off_t)(bitmap_start * INFS_BLOCK_SIZE)) != 0) {
        perror("write bitmap");
        free(bitmap);
        close(fd);
        return 1;
    }

    if (infs_encode_object_index(block, index_id, 1) != 0) {
        perror("encode object index");
        free(bitmap);
        close(fd);
        return 1;
    }
    struct infs_object_header_disk *ih = (struct infs_object_header_disk *)block;
    struct infs_index_payload_disk *ip =
        (struct infs_index_payload_disk *)(block + sizeof(*ih));
    struct infs_index_entry_disk *ie = (struct infs_index_entry_disk *)(ip + 1);
    ip->entry_count = htole32(1);
    memcpy(ie->object_id, root_id, 16);
    ie->object_block = htole64(root_block);
    ie->object_type = htole16(INFS_OBJECT_DIRECTORY);
    ih->payload_size = htole32(sizeof(*ip) + sizeof(*ie));
    if (infs_object_finalize(block) != 0 ||
        infs_pwrite_full(fd, block, sizeof(block),
                         (off_t)(index_block * INFS_BLOCK_SIZE)) != 0) {
        perror("write object index");
        free(bitmap);
        close(fd);
        return 1;
    }

    if (infs_encode_root_directory(block, root_id, 1,
                                   (uint32_t)getuid(), (uint32_t)getgid(),
                                   current_time_ns()) != 0 ||
        infs_pwrite_full(fd, block, sizeof(block),
                         (off_t)(root_block * INFS_BLOCK_SIZE)) != 0) {
        perror("write root directory");
        free(bitmap);
        close(fd);
        return 1;
    }

    if (fsync(fd) != 0) {
        perror("fsync");
        free(bitmap);
        close(fd);
        return 1;
    }

    char uuid_text[37];
    infs_uuid_to_string(fs_uuid, uuid_text);
    printf("InfiltratorFS formatted successfully\n");
    printf("  Format:          %u.%u\n", INFS_FORMAT_MAJOR, INFS_FORMAT_MINOR);
    printf("  UUID:            %s\n", uuid_text);
    printf("  Label:           %s\n", (char *)sb.label);
    printf("  Block size:      %u bytes\n", INFS_BLOCK_SIZE);
    printf("  Total blocks:    %" PRIu64 "\n", total_blocks);
    printf("  Free blocks:     %" PRIu64 "\n", total_blocks - used_blocks);
    printf("  Bitmap blocks:   %" PRIu64 "\n", bitmap_blocks);
    printf("  Object index:    block %" PRIu64 "\n", index_block);
    printf("  Root block:      %" PRIu64 "\n", root_block);
    printf("  Checkpoints:     %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n",
           checkpoints[0], checkpoints[1], checkpoints[2]);

    free(bitmap);
    close(fd);
    return 0;
}
