// SPDX-License-Identifier: GPL-3.0-or-later
/* Qualify the isolated Format 0.19 in-memory image writer. Not a mounted
 * filesystem qualification, and not permission to format a real device. */
#include "infilfs/format_volume.h"
#include "infilfs/locator.h"
#include "infilfs/format.h"
#include "infilfs/endian.h"
#include "infilfs/fs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_BYTES (16u * 1024u * 1024u)

struct memory_volume {
    uint8_t *contents;
    uint64_t bytes;
    unsigned writes;
    unsigned flushes;
    uint8_t random_counter;
    int device;
};

static void expect(int condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "Format 0.19 image fixture: %s\n", what);
        exit(1);
    }
}
static infs_status mem_read(void *context, uint64_t offset,
                             void *out, size_t length)
{
    struct memory_volume *m = context;
    if (offset > m->bytes || length > m->bytes - offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(out, m->contents + (size_t)offset, length);
    return INFS_STATUS_OK;
}
static infs_status mem_write(void *context, uint64_t offset,
                              const void *source, size_t length)
{
    struct memory_volume *m = context;
    if (offset > m->bytes || length > m->bytes - offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(m->contents + (size_t)offset, source, length);
    ++m->writes;
    return INFS_STATUS_OK;
}
static infs_status mem_flush(void *context)
{
    ++((struct memory_volume *)context)->flushes;
    return INFS_STATUS_OK;
}
static infs_status mem_size(void *context, uint64_t *bytes, int *device)
{
    struct memory_volume *m = context;
    *bytes = m->bytes;
    *device = m->device;
    return INFS_STATUS_OK;
}
static infs_status mem_random(void *context, void *out, size_t length)
{
    struct memory_volume *m = context;
    memset(out, ++m->random_counter, length);
    return INFS_STATUS_OK;
}
static infs_status mem_time(void *context, struct infs_timestamp *timestamp)
{
    (void)context;
    memset(timestamp, 0, sizeof(*timestamp));
    timestamp->seconds = 1234567;
    return INFS_STATUS_OK;
}
static const struct infs_storage_ops ops = {
    .read_at = mem_read, .write_at = mem_write,
    .flush = mem_flush, .get_size = mem_size,
    .random_bytes = mem_random, .current_time = mem_time
};

static void reset(struct memory_volume *m)
{
    memset(m->contents, 0, (size_t)m->bytes);
    m->writes = 0;
    m->flushes = 0;
    m->random_counter = 0;
    m->device = 0;
}

int main(void)
{
    struct memory_volume m = {0};
    struct infs_storage storage = { .ops = &ops, .context = &m };
    struct infs_locator a = {0}, b = {0};
    const uint8_t *root;
    const uint8_t *leaf;
    const uint8_t *bits;
    const struct infs_superblock_disk *sb;

    m.bytes = IMAGE_BYTES;
    m.contents = calloc(1u, IMAGE_BYTES);
    expect(m.contents != NULL, "allocate image backing");

    m.device = 1;
    expect(infs_format_storage_019_test_image(&storage, "Fixture19") ==
           INFS_STATUS_NOT_SUPPORTED,
           "experimental formatter must reject physical device targets");
    expect(m.writes == 0, "physical-device rejection before destructive write");

    reset(&m);
    expect(infs_format_storage_019_test_image(&storage, "Fixture19") ==
           INFS_STATUS_OK, "create in-memory 0.19 test image");

    root = m.contents;
    sb = (const struct infs_superblock_disk *)root;
    expect(infs_le16_to_cpu(sb->format_minor) == INFS_LOCATOR_FORMAT_MINOR,
           "test image checkpoint carries new 0.19 format identity");
    expect(!infs_validate_superblock_block(root),
           "0.18 reader rejects an unqualified 0.19 volume");
    expect(infs_validate_superblock_block_minor(
               root, INFS_LOCATOR_FORMAT_MINOR),
           "explicit test-only 0.19 checkpoint validator accepts fixture");
    expect(infs_le64_to_cpu(sb->allocation_root_block) == 2,
           "initial allocation starts after block-zero and locator A");

    expect(infs_locator_decode(
           m.contents + INFS_BLOCK_SIZE * INFS_LOCATOR_BLOCK_A,
           IMAGE_BYTES / INFS_BLOCK_SIZE, &a) == INFS_STATUS_OK,
           "decode first fixed recovery locator");
    expect(infs_locator_decode(
           m.contents + INFS_BLOCK_SIZE * INFS_LOCATOR_BLOCK_B,
           IMAGE_BYTES / INFS_BLOCK_SIZE, &b) == INFS_STATUS_OK,
           "decode second fixed recovery locator");
    expect(memcmp(&a, &b, sizeof(a)) == 0,
           "both bootstrap locator records agree");
    expect(a.generation == 1 && a.total_blocks == 4096 &&
           a.checkpoint[0] == 0 && a.checkpoint[1] == 2048 &&
           a.checkpoint[2] == 4095,
           "locator checkpoint geometry is exact");
    expect(memcmp(a.filesystem_uuid, sb->filesystem_uuid, 16) == 0,
           "locator identity matches checkpoint identity");

    /* 16 MiB: allocation root=2, level2=3, level1=4, leaf=5.
     * The authoritative bitmap MUST account for the new locator blocks. */
    leaf = m.contents + 5u * INFS_BLOCK_SIZE;
    expect(infs_validate_allocation_page(
               leaf, INFS_ALLOCATION_LEAF_PAGE_MAGIC, 1u, 0u, 0u),
           "verify initial allocation bitmap leaf CRC64");
    bits = leaf + sizeof(struct infs_allocation_page_disk);
    expect((bits[INFS_LOCATOR_BLOCK_A / 8] &
            (1u << (INFS_LOCATOR_BLOCK_A % 8))) != 0,
           "reserve fixed locator A in allocation map");
    expect((bits[INFS_LOCATOR_BLOCK_B / 8] &
            (1u << (INFS_LOCATOR_BLOCK_B % 8))) != 0,
           "reserve fixed locator B in allocation map");
    expect(m.flushes >= 6, "locators and each checkpoint durably flushed");

    {
        struct infs_superblock_disk found[INFS_LOCATOR_MAX_CANDIDATES];
        uint8_t valid[INFS_LOCATOR_MAX_CANDIDATES] = {0};
        unsigned count = 0;
        /* Simulate losing the primary checkpoint without changing the two
         * durable secondary checkpoints or either locator. */
        m.contents[0] ^= 0xffu;
        expect(infs_locator_discover_019_headers(
                   &storage, IMAGE_BYTES, found, valid, &count) ==
                   INFS_STATUS_OK && count >= 2,
               "surviving 0.19 checkpoints discoverable without block zero");
        expect(infs_le64_to_cpu(found[1].generation) == 1 ||
               infs_le64_to_cpu(found[2].generation) == 1,
               "recovered secondary checkpoint carries valid generation");

        /* One corrupt fixed locator must not hide the surviving backup. */
        m.contents[INFS_BLOCK_SIZE * INFS_LOCATOR_BLOCK_A] ^= 0xffu;
        expect(infs_locator_discover_019_headers(
                   &storage, IMAGE_BYTES, found, valid, &count) ==
                   INFS_STATUS_OK && count >= 2,
               "secondary locator independently discovers checkpoint copies");

        /* Both damaged locator copies give no authority to invent positions. */
        m.contents[INFS_BLOCK_SIZE * INFS_LOCATOR_BLOCK_B] ^= 0xffu;
        expect(infs_locator_discover_019_headers(
                   &storage, IMAGE_BYTES, found, valid, &count) ==
                   INFS_STATUS_CORRUPT,
               "two damaged locator records fail without speculative scan");
    }

    reset(&m);
    expect(infs_format_storage(&storage, "Legacy") == INFS_STATUS_OK,
           "old default format remains available during transition");
    sb = (const struct infs_superblock_disk *)m.contents;
    expect(infs_le16_to_cpu(sb->format_minor) == INFS_FORMAT_MINOR &&
           INFS_FORMAT_MINOR == 18u,
           "production formatter identity remains exactly 0.18");

    free(m.contents);
    puts("Format 0.19 experimental memory-image fixture: PASS");
    return 0;
}
