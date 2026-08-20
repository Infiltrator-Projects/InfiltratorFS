// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/checksum.h"
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/fs.h"
#include "infilfs/status.h"
#include "infilfs/utf8.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message)
{
    fprintf(stderr, "format-conformance: %s\n", message);
    exit(1);
}

static void expect(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static void put_le16(uint8_t *bytes, size_t offset, uint16_t value)
{
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1u] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *bytes, size_t offset, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = (uint8_t)(value >> (i * 8u));
}

static void put_le64(uint8_t *bytes, size_t offset, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i)
        bytes[offset + i] = (uint8_t)(value >> (i * 8u));
}

static void refresh_crc(uint8_t block[INFS_BLOCK_SIZE], size_t checksum_offset)
{
    memset(block + checksum_offset, 0, 32);
    put_le64(block, checksum_offset,
             infs_crc64_ecma(block, INFS_BLOCK_SIZE));
}

static void check_layout(void)
{
    expect(sizeof(struct infs_superblock_disk) == 252, "superblock size");
    expect(offsetof(struct infs_superblock_disk, magic) == 0, "magic offset");
    expect(offsetof(struct infs_superblock_disk, format_major) == 8,
           "major offset");
    expect(offsetof(struct infs_superblock_disk, checksum_type) == 16,
           "checkpoint checksum-type offset");
    expect(offsetof(struct infs_superblock_disk, generation) == 20,
           "generation offset");
    expect(offsetof(struct infs_superblock_disk, checkpoint_block) == 76,
           "checkpoint array offset");
    expect(offsetof(struct infs_superblock_disk, filesystem_uuid) == 100,
           "filesystem UUID offset");
    expect(offsetof(struct infs_superblock_disk, incompat_flags) == 148,
           "incompatible-flags offset");
    expect(offsetof(struct infs_superblock_disk, label) == 156,
           "label offset");
    expect(offsetof(struct infs_superblock_disk, checksum) == 220,
           "checkpoint checksum offset");

    expect(sizeof(struct infs_object_header_disk) == 96, "object header size");
    expect(offsetof(struct infs_object_header_disk, object_id) == 24,
           "object ID offset");
    expect(offsetof(struct infs_object_header_disk, payload_size) == 56,
           "payload-size offset");
    expect(offsetof(struct infs_object_header_disk, checksum) == 64,
           "object checksum offset");
    expect(sizeof(struct infs_attributes_disk) == 88, "attributes size");
    expect(offsetof(struct infs_attributes_disk, birth_time_ns) == 24,
           "birth-time offset");
    expect(offsetof(struct infs_attributes_disk, security_object_id) == 56,
           "security-object offset");
    expect(offsetof(struct infs_attributes_disk,
                    extended_attributes_object_id) == 72,
           "extended-attributes offset");
    expect(sizeof(struct infs_extent_disk) == 24, "extent size");
    expect(offsetof(struct infs_extent_disk, logical_block) == 0,
           "extent logical-block offset");
    expect(offsetof(struct infs_extent_disk, physical_block) == 8,
           "extent physical-block offset");
    expect(offsetof(struct infs_extent_disk, block_count) == 16,
           "extent block-count offset");
    expect(offsetof(struct infs_extent_disk, flags) == 20,
           "extent flags offset");
}

static void make_golden_superblock(struct infs_superblock_disk *sb)
{
    memset(sb, 0, sizeof(*sb));
    memcpy(sb->magic, INFS_MAGIC, 8);
    sb->format_major = infs_cpu_to_le16(INFS_FORMAT_MAJOR);
    sb->format_minor = infs_cpu_to_le16(INFS_FORMAT_MINOR);
    sb->header_size = infs_cpu_to_le16(sizeof(*sb));
    sb->block_shift = infs_cpu_to_le16(INFS_BLOCK_SHIFT);
    sb->checksum_type = infs_cpu_to_le32(INFS_CHECKSUM_CRC64_ECMA);
    sb->generation = infs_cpu_to_le64(UINT64_C(0x0102030405060708));
    sb->total_blocks = infs_cpu_to_le64(UINT64_C(0x1112131415161718));
    sb->free_blocks = infs_cpu_to_le64(UINT64_C(0x2122232425262728));
    sb->bitmap_start_block = infs_cpu_to_le64(UINT64_C(0x3132333435363738));
    sb->bitmap_block_count = infs_cpu_to_le64(UINT64_C(0x4142434445464748));
    sb->object_index_block = infs_cpu_to_le64(UINT64_C(0x5152535455565758));
    sb->root_object_block = infs_cpu_to_le64(UINT64_C(0x6162636465666768));
    sb->checkpoint_block[0] = infs_cpu_to_le64(UINT64_C(0x7172737475767778));
    sb->checkpoint_block[1] = infs_cpu_to_le64(UINT64_C(0x8182838485868788));
    sb->checkpoint_block[2] = infs_cpu_to_le64(UINT64_C(0x9192939495969798));
    for (unsigned i = 0; i < 16; ++i) {
        sb->filesystem_uuid[i] = (uint8_t)(0xa0u + i);
        sb->root_object_id[i] = (uint8_t)(0xb0u + i);
    }
    sb->compat_flags = infs_cpu_to_le64(UINT64_C(0xc1c2c3c4c5c6c7c8));
    sb->ro_compat_flags = infs_cpu_to_le64(UINT64_C(0xd1d2d3d4d5d6d7d8));
    sb->incompat_flags = infs_cpu_to_le64(
        INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS);
    memcpy(sb->label, "Golden-0.6", 10);
}

static void make_expected_superblock(uint8_t block[INFS_BLOCK_SIZE])
{
    memset(block, 0, INFS_BLOCK_SIZE);
    memcpy(block, INFS_MAGIC, 8);
    put_le16(block, 8, INFS_FORMAT_MAJOR);
    put_le16(block, 10, INFS_FORMAT_MINOR);
    put_le16(block, 12, 252);
    put_le16(block, 14, INFS_BLOCK_SHIFT);
    put_le32(block, 16, INFS_CHECKSUM_CRC64_ECMA);
    put_le64(block, 20, UINT64_C(0x0102030405060708));
    put_le64(block, 28, UINT64_C(0x1112131415161718));
    put_le64(block, 36, UINT64_C(0x2122232425262728));
    put_le64(block, 44, UINT64_C(0x3132333435363738));
    put_le64(block, 52, UINT64_C(0x4142434445464748));
    put_le64(block, 60, UINT64_C(0x5152535455565758));
    put_le64(block, 68, UINT64_C(0x6162636465666768));
    put_le64(block, 76, UINT64_C(0x7172737475767778));
    put_le64(block, 84, UINT64_C(0x8182838485868788));
    put_le64(block, 92, UINT64_C(0x9192939495969798));
    for (unsigned i = 0; i < 16; ++i) {
        block[100 + i] = (uint8_t)(0xa0u + i);
        block[116 + i] = (uint8_t)(0xb0u + i);
    }
    put_le64(block, 132, UINT64_C(0xc1c2c3c4c5c6c7c8));
    put_le64(block, 140, UINT64_C(0xd1d2d3d4d5d6d7d8));
    put_le64(block, 148,
             INFS_INCOMPAT_UTF8_NAMES | INFS_INCOMPAT_SPARSE_EXTENTS);
    memcpy(block + 156, "Golden-0.6", 10);
    refresh_crc(block, 220);
}

static void expect_superblock_rejected(const uint8_t golden[INFS_BLOCK_SIZE],
                                       size_t offset, uint64_t value,
                                       unsigned width, const char *message)
{
    uint8_t changed[INFS_BLOCK_SIZE];
    memcpy(changed, golden, sizeof(changed));
    if (width == 2)
        put_le16(changed, offset, (uint16_t)value);
    else if (width == 4)
        put_le32(changed, offset, (uint32_t)value);
    else
        put_le64(changed, offset, value);
    refresh_crc(changed, 220);
    expect(!infs_validate_superblock_block(changed), message);
}

static void check_superblock_encoding(void)
{
    struct infs_superblock_disk sb;
    uint8_t encoded[INFS_BLOCK_SIZE];
    uint8_t expected[INFS_BLOCK_SIZE];
    make_golden_superblock(&sb);
    make_expected_superblock(expected);
    expect(infs_encode_superblock(encoded, &sb) == INFS_STATUS_OK,
           "encode golden checkpoint");
    expect(memcmp(encoded, expected, sizeof(encoded)) == 0,
           "golden checkpoint bytes changed");
    expect(infs_validate_superblock_block(encoded), "validate golden checkpoint");

    expect_superblock_rejected(encoded, 8, 1, 2, "reject foreign major version");
    expect_superblock_rejected(encoded, 10, INFS_FORMAT_MINOR + 1u, 2,
                               "reject future minor version");
    expect_superblock_rejected(encoded, 10, INFS_FORMAT_MINOR - 1u, 2,
                               "reject sparse feature under older minor");
    expect_superblock_rejected(encoded, 12, 251, 2, "reject header-size drift");
    expect_superblock_rejected(encoded, 14, 11, 2, "reject block-size drift");
    expect_superblock_rejected(encoded, 16, INFS_CHECKSUM_SHA256, 4,
                               "reject checkpoint checksum drift");
    expect_superblock_rejected(encoded, 148, INFS_INCOMPAT_SPARSE_EXTENTS, 8,
                               "reject missing UTF-8 feature");
    expect_superblock_rejected(encoded, 148, INFS_INCOMPAT_UTF8_NAMES, 8,
                               "reject missing sparse-extents feature");
    expect_superblock_rejected(encoded, 148,
                               INFS_INCOMPAT_UTF8_NAMES |
                                   INFS_INCOMPAT_SPARSE_EXTENTS |
                                   (UINT64_C(1) << 63),
                               8, "reject unknown incompatible feature");

    encoded[4000] ^= 0x80u;
    expect(!infs_validate_superblock_block(encoded), "reject checksum mismatch");
}

static void check_object_encoding(void)
{
    uint8_t block[INFS_BLOCK_SIZE];
    uint8_t object_id[16];
    uint8_t parent_id[16];
    for (unsigned i = 0; i < 16; ++i) {
        object_id[i] = (uint8_t)i;
        parent_id[i] = (uint8_t)(0xf0u + i);
    }
    expect(infs_object_init(block, INFS_OBJECT_FILE, object_id, parent_id,
                            UINT64_C(0x0102030405060708), 128) ==
               INFS_STATUS_OK,
           "initialize golden object");
    expect(infs_object_finalize(block) == INFS_STATUS_OK,
           "finalize golden object");
    expect(infs_validate_object_block(block), "validate golden object");
    expect(memcmp(block, INFS_OBJECT_MAGIC, 8) == 0, "object magic bytes");
    expect(block[8] == INFS_OBJECT_FILE && block[9] == 0, "object type bytes");
    expect(block[12] == 96 && block[13] == 0, "object header-size bytes");
    expect(memcmp(block + 24, object_id, 16) == 0, "object ID bytes");
    expect(memcmp(block + 40, parent_id, 16) == 0, "parent ID bytes");
    expect(block[56] == 128 && block[57] == 0, "payload-size bytes");

    block[4095] ^= 1u;
    expect(!infs_validate_object_block(block), "reject object checksum mismatch");
}

static void check_utf8(void)
{
    static const uint8_t valid_ascii[] = "ordinary-name";
    static const uint8_t valid_unicode[] = {0xc3, 0xa9, 0xe6, 0x96, 0x87,
                                             0xf0, 0x9f, 0x92, 0xbe};
    static const uint8_t overlong[] = {0xc0, 0x80};
    static const uint8_t surrogate[] = {0xed, 0xa0, 0x80};
    static const uint8_t too_high[] = {0xf4, 0x90, 0x80, 0x80};
    static const uint8_t truncated[] = {0xe2, 0x82};
    static const uint8_t bad_continuation[] = {0xe2, 0x28, 0xa1};

    expect(infs_utf8_validate(valid_ascii, sizeof(valid_ascii) - 1u),
           "accept ASCII UTF-8");
    expect(infs_utf8_validate(valid_unicode, sizeof(valid_unicode)),
           "accept multibyte UTF-8");
    expect(!infs_utf8_validate(overlong, sizeof(overlong)), "reject overlong UTF-8");
    expect(!infs_utf8_validate(surrogate, sizeof(surrogate)), "reject surrogate UTF-8");
    expect(!infs_utf8_validate(too_high, sizeof(too_high)), "reject high UTF-8 scalar");
    expect(!infs_utf8_validate(truncated, sizeof(truncated)), "reject truncated UTF-8");
    expect(!infs_utf8_validate(bad_continuation, sizeof(bad_continuation)),
           "reject bad UTF-8 continuation");
}

static void check_status_mapping(void)
{
    expect(INFS_STATUS_NOT_FOUND < INFS_STATUS_OK,
           "filesystem failures are negative");
    expect(INFS_STATUS_NOT_FOUND != INFS_STATUS_NO_SPACE,
           "filesystem statuses are distinct");
    expect(strcmp(infs_status_string(INFS_STATUS_CORRUPT),
                  "filesystem corruption detected") == 0,
           "stable status text");
    expect(strcmp(infs_status_string(INFS_STATUS_BUSY),
                  "storage target is busy") == 0,
           "busy status text");
}

int main(void)
{
    check_layout();
    check_superblock_encoding();
    check_object_encoding();
    check_utf8();
    check_status_mapping();
    puts("format-conformance: PASS");
    return 0;
}
