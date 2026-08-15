// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/fs.h"

#include "infilfs/checksum.h"
#include "infilfs/io.h"

#include <endian.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static uint64_t block_crc_with_zeroed_checksum(const uint8_t block[INFS_BLOCK_SIZE],
                                                size_t checksum_offset,
                                                size_t checksum_size)
{
    uint8_t tmp[INFS_BLOCK_SIZE];
    memcpy(tmp, block, sizeof(tmp));
    memset(tmp + checksum_offset, 0, checksum_size);
    return infs_crc64_ecma(tmp, sizeof(tmp));
}

int infs_encode_superblock(uint8_t block[INFS_BLOCK_SIZE],
                           const struct infs_superblock_disk *sb)
{
    if (!block || !sb) {
        errno = EINVAL;
        return -1;
    }
    memset(block, 0, INFS_BLOCK_SIZE);
    memcpy(block, sb, sizeof(*sb));

    const size_t off = offsetof(struct infs_superblock_disk, checksum);
    memset(block + off, 0, sizeof(sb->checksum));
    uint64_t crc = htole64(block_crc_with_zeroed_checksum(
        block, off, sizeof(sb->checksum)));
    memcpy(block + off, &crc, sizeof(crc));
    return 0;
}

int infs_validate_superblock_block(const uint8_t block[INFS_BLOCK_SIZE])
{
    const struct infs_superblock_disk *sb =
        (const struct infs_superblock_disk *)block;

    if (memcmp(sb->magic, INFS_MAGIC, 8) != 0)
        return 0;
    if (le16toh(sb->format_major) != INFS_FORMAT_MAJOR)
        return 0;
    if (le16toh(sb->format_minor) > INFS_FORMAT_MINOR)
        return 0;
    if (le16toh(sb->header_size) != sizeof(*sb))
        return 0;
    if (le16toh(sb->block_shift) != INFS_BLOCK_SHIFT)
        return 0;
    if (le32toh(sb->checksum_type) != INFS_CHECKSUM_CRC64_ECMA)
        return 0;

    const size_t off = offsetof(struct infs_superblock_disk, checksum);
    uint64_t stored_le;
    memcpy(&stored_le, block + off, sizeof(stored_le));
    uint64_t stored = le64toh(stored_le);
    uint64_t actual = block_crc_with_zeroed_checksum(
        block, off, sizeof(sb->checksum));
    return stored == actual;
}

int infs_decode_superblock(const uint8_t block[INFS_BLOCK_SIZE],
                           struct infs_superblock_disk *sb)
{
    if (!sb || !infs_validate_superblock_block(block)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(sb, block, sizeof(*sb));
    return 0;
}

int infs_object_init(uint8_t block[INFS_BLOCK_SIZE], uint16_t object_type,
                     const uint8_t object_id[16], const uint8_t parent_id[16],
                     uint64_t generation, uint32_t payload_size)
{
    if (!block || !object_id ||
        payload_size > INFS_BLOCK_SIZE - sizeof(struct infs_object_header_disk)) {
        errno = EINVAL;
        return -1;
    }

    memset(block, 0, INFS_BLOCK_SIZE);
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)block;
    memcpy(hdr->magic, INFS_OBJECT_MAGIC, 8);
    hdr->object_type = htole16(object_type);
    hdr->object_version = htole16(1);
    hdr->header_size = htole32(sizeof(*hdr));
    hdr->generation = htole64(generation);
    memcpy(hdr->object_id, object_id, 16);
    if (parent_id)
        memcpy(hdr->parent_id, parent_id, 16);
    hdr->payload_size = htole32(payload_size);
    hdr->checksum_type = htole32(INFS_CHECKSUM_CRC64_ECMA);
    return 0;
}

int infs_object_finalize(uint8_t block[INFS_BLOCK_SIZE])
{
    if (!block) {
        errno = EINVAL;
        return -1;
    }
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)block;
    if (memcmp(hdr->magic, INFS_OBJECT_MAGIC, 8) != 0 ||
        le32toh(hdr->header_size) != sizeof(*hdr) ||
        le32toh(hdr->payload_size) > INFS_BLOCK_SIZE - sizeof(*hdr)) {
        errno = EINVAL;
        return -1;
    }

    const size_t off = offsetof(struct infs_object_header_disk, checksum);
    memset(block + off, 0, sizeof(hdr->checksum));
    uint64_t crc = htole64(block_crc_with_zeroed_checksum(
        block, off, sizeof(hdr->checksum)));
    memcpy(block + off, &crc, sizeof(crc));
    return 0;
}

int infs_validate_object_block(const uint8_t block[INFS_BLOCK_SIZE])
{
    const struct infs_object_header_disk *hdr =
        (const struct infs_object_header_disk *)block;
    if (memcmp(hdr->magic, INFS_OBJECT_MAGIC, 8) != 0)
        return 0;
    if (le32toh(hdr->header_size) != sizeof(*hdr))
        return 0;
    if (le32toh(hdr->payload_size) > INFS_BLOCK_SIZE - sizeof(*hdr))
        return 0;
    if (le32toh(hdr->checksum_type) != INFS_CHECKSUM_CRC64_ECMA)
        return 0;

    const size_t off = offsetof(struct infs_object_header_disk, checksum);
    uint64_t stored_le;
    memcpy(&stored_le, block + off, sizeof(stored_le));
    uint64_t stored = le64toh(stored_le);
    uint64_t actual = block_crc_with_zeroed_checksum(
        block, off, sizeof(hdr->checksum));
    return stored == actual;
}

static void fill_stat(struct infs_stat_disk *st, uint32_t mode,
                      uint32_t uid, uint32_t gid, uint32_t nlink,
                      int64_t now_ns)
{
    memset(st, 0, sizeof(*st));
    st->mode = htole32(mode);
    st->uid = htole32(uid);
    st->gid = htole32(gid);
    st->nlink = htole32(nlink);
    st->size = htole64(0);
    st->atime_ns = htole64((uint64_t)now_ns);
    st->mtime_ns = htole64((uint64_t)now_ns);
    st->ctime_ns = htole64((uint64_t)now_ns);
}

int infs_encode_root_directory(uint8_t block[INFS_BLOCK_SIZE],
                               const uint8_t object_id[16],
                               uint64_t generation,
                               uint32_t uid, uint32_t gid,
                               int64_t now_ns)
{
    if (infs_object_init(block, INFS_OBJECT_DIRECTORY, object_id, NULL,
                         generation, sizeof(struct infs_directory_payload_disk)) != 0)
        return -1;

    struct infs_directory_payload_disk *payload =
        (struct infs_directory_payload_disk *)(block + sizeof(struct infs_object_header_disk));
    fill_stat(&payload->stat, S_IFDIR | 0755u, uid, gid, 2u, now_ns);
    payload->entry_count = htole32(0);
    payload->bytes_used = htole32(0);
    return infs_object_finalize(block);
}

int infs_encode_object_index(uint8_t block[INFS_BLOCK_SIZE],
                             const uint8_t object_id[16],
                             uint64_t generation)
{
    if (infs_object_init(block, INFS_OBJECT_INDEX, object_id, NULL,
                         generation, sizeof(struct infs_index_payload_disk)) != 0)
        return -1;
    struct infs_index_payload_disk *payload =
        (struct infs_index_payload_disk *)(block + sizeof(struct infs_object_header_disk));
    payload->entry_count = htole32(0);
    payload->reserved = 0;
    return infs_object_finalize(block);
}

int infs_read_best_superblock(int fd, uint64_t size_bytes,
                              struct infs_superblock_disk *out,
                              unsigned *valid_copies)
{
    if (size_bytes < INFS_BLOCK_SIZE * 3u || !out) {
        errno = EINVAL;
        return -1;
    }

    const uint64_t total_blocks = size_bytes / INFS_BLOCK_SIZE;
    const uint64_t candidates[INFS_CHECKPOINT_COUNT] = {
        0,
        total_blocks / 2u,
        total_blocks - 1u
    };

    uint64_t best_generation = 0;
    int found = 0;
    unsigned valid = 0;
    uint8_t block[INFS_BLOCK_SIZE];

    for (unsigned i = 0; i < INFS_CHECKPOINT_COUNT; ++i) {
        off_t off = (off_t)(candidates[i] * INFS_BLOCK_SIZE);
        if (infs_pread_full(fd, block, sizeof(block), off) != 0)
            continue;
        if (!infs_validate_superblock_block(block))
            continue;

        struct infs_superblock_disk sb;
        memcpy(&sb, block, sizeof(sb));
        if (le64toh(sb.total_blocks) != total_blocks)
            continue;
        if (le64toh(sb.checkpoint_block[0]) != candidates[0] ||
            le64toh(sb.checkpoint_block[1]) != candidates[1] ||
            le64toh(sb.checkpoint_block[2]) != candidates[2])
            continue;

        ++valid;
        uint64_t gen = le64toh(sb.generation);
        if (!found || gen > best_generation) {
            memcpy(out, &sb, sizeof(*out));
            best_generation = gen;
            found = 1;
        }
    }

    if (valid_copies)
        *valid_copies = valid;
    if (!found) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void infs_uuid_to_string(const uint8_t id[16], char out[37])
{
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
             id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
}
