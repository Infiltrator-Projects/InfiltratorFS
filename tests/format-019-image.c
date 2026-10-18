// SPDX-License-Identifier: GPL-3.0-or-later
/* Qualify the isolated Format 0.19 in-memory image writer. Not a mounted
 * filesystem qualification, and not permission to format a real device. */
#include "infilfs/format_volume.h"
#include "infilfs/volume.h"
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
    uint64_t unreadable_blocks[3];
    unsigned unreadable_count;
    unsigned checkpoint_flushes;
    unsigned fail_checkpoint_flush;
    int checkpoint_pending_flush;
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
    for (unsigned i = 0; i < m->unreadable_count; ++i)
        if (length == INFS_BLOCK_SIZE &&
            offset == m->unreadable_blocks[i] * INFS_BLOCK_SIZE)
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
    if (length == INFS_BLOCK_SIZE &&
        (offset == 0 ||
         offset == (IMAGE_BYTES / 2u) ||
         offset == IMAGE_BYTES - INFS_BLOCK_SIZE))
        m->checkpoint_pending_flush = 1;
    return INFS_STATUS_OK;
}
static infs_status mem_flush(void *context)
{
    struct memory_volume *m = context;
    ++m->flushes;
    if (m->checkpoint_pending_flush) {
        m->checkpoint_pending_flush = 0;
        ++m->checkpoint_flushes;
        if (m->fail_checkpoint_flush == m->checkpoint_flushes)
            return INFS_STATUS_IO_ERROR;
    }
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
    m->unreadable_count = 0;
    m->checkpoint_flushes = 0;
    m->fail_checkpoint_flush = 0;
    m->checkpoint_pending_flush = 0;
}

int main(int argc, char **argv)
{
    if (argc != 1 && (argc != 3 || strcmp(argv[1], "--emit-image") != 0)) {
        fprintf(stderr, "Usage: %s [--emit-image <regular-image-path>]\n",
                argv[0]);
        return 2;
    }
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
    {
        struct infs_locator foreign = b;
        struct infs_superblock_disk rejected = {0};
        uint8_t *page = m.contents +
            INFS_LOCATOR_BLOCK_B * INFS_BLOCK_SIZE;
        foreign.filesystem_uuid[0] ^= 0x5au;
        expect(infs_locator_encode(page, &foreign) == INFS_STATUS_OK,
               "construct individually CRC-valid foreign locator record");
        expect(infs_volume_verify_019_test_image(&storage, &rejected) ==
                   INFS_STATUS_CORRUPT,
               "portable recovery rejects two conflicting filesystem UUIDs");
        expect(infs_locator_encode(page, &b) == INFS_STATUS_OK,
               "restore original locator after foreign-UUID rejection");
        expect(infs_volume_verify_019_test_image(&storage, &rejected) ==
                   INFS_STATUS_OK,
               "normal recovery resumes with matching locator identities");
    }
    expect(a.generation == 1 && a.total_blocks == 4096 &&
           a.checkpoint[0] == 0 && a.checkpoint[1] == 2048 &&
           a.checkpoint[2] == 4095,
           "locator checkpoint geometry is exact");
    expect(memcmp(a.filesystem_uuid, sb->filesystem_uuid, 16) == 0,
           "locator identity matches checkpoint identity");

    {
        struct infs_superblock_disk committed = {0};
        expect(infs_volume_verify_019_test_image(&storage, &committed) ==
               INFS_STATUS_OK,
               "0.19 fresh image passes full committed-graph validation");
        expect(infs_le64_to_cpu(committed.generation) == 1u,
               "full graph verifier selects committed initial generation");
    }

    /* The experimental image can be opened as a real *read-only* portable
     * volume, not just scanned for valid checkpoint headers. */
    {
        struct infs_volume view = {0};
        struct infs_storage view_storage = { .ops = &ops, .context = &m };
        struct infs_lookup root_lookup = {0};
        unsigned before = m.writes;
        expect(infs_volume_open_019_test_image(
                   &view, &view_storage) == INFS_STATUS_OK,
               "0.19 image opens as read-only portable volume");
        expect(view.writable == 0 && !view_storage.ops,
               "read-only open transfers storage ownership");
        expect(infs_lookup_path(&view, "/", &root_lookup) == INFS_STATUS_OK &&
               root_lookup.type == INFS_OBJECT_DIRECTORY,
               "0.19 mounted image resolves its root directory");
        infs_volume_close(&view);
        expect(m.writes == before, "read-only open/close never writes media");
    }
    {
        struct infs_volume view = {0};
        struct infs_storage view_storage = { .ops = &ops, .context = &m };
        unsigned before = m.writes;
        m.device = 1;
        expect(infs_volume_open_019_test_image(
                   &view, &view_storage) == INFS_STATUS_NOT_SUPPORTED,
               "experimental 0.19 portable open refuses physical devices");
        expect(view_storage.ops == &ops && m.writes == before,
               "rejected physical open preserves ownership and media");
        m.device = 0;
    }

    /* Real copy-on-write transactions on a disposable 0.19 memory image. */
    {
        struct infs_volume writer = {0}, reader = {0};
        struct infs_storage ws = { .ops = &ops, .context = &m };
        struct infs_storage rs = { .ops = &ops, .context = &m };
        struct infs_superblock_disk committed = {0};
        struct infs_create_options options = {
            .posix_permissions = 0644u, .posix_uid = 1000u,
            .posix_gid = 1000u,
        };
        static const char payload[] = "Format 0.19 committed payload";
        char readback[sizeof(payload)] = {0};
        uint8_t locator_a[INFS_BLOCK_SIZE], locator_b[INFS_BLOCK_SIZE];
        memcpy(locator_a, m.contents + INFS_LOCATOR_BLOCK_A * INFS_BLOCK_SIZE,
               sizeof(locator_a));
        memcpy(locator_b, m.contents + INFS_LOCATOR_BLOCK_B * INFS_BLOCK_SIZE,
               sizeof(locator_b));
        expect(infs_volume_open_019_rw_test_image(&writer, &ws) ==
                   INFS_STATUS_OK && writer.writable && !ws.ops,
               "fully validated experimental writable 0.19 image open");
        expect(infs_volume_resize(&writer, IMAGE_BYTES) ==
                   INFS_STATUS_NOT_SUPPORTED,
               "unfinished 0.19 resize is blocked without media changes");
        unsigned checkpoint_before = m.checkpoint_flushes;
        expect(infs_create_file(&writer, "/written", &options) ==
                   INFS_STATUS_OK, "create file using 0.19 image transaction");
        expect(m.checkpoint_flushes == checkpoint_before + 3u,
               "0.19 committed all three checkpoint flushes separately");
        expect(infs_write_file(&writer, "/written", payload,
                   sizeof(payload)-1u, 0) == (int64_t)(sizeof(payload)-1u),
               "write and commit actual 0.19 file data");
        expect(infs_volume_sync(&writer) == INFS_STATUS_OK &&
               infs_le64_to_cpu(writer.sb.generation) > 1u,
               "durably publish multiple 0.19 generations");
        expect(memcmp(locator_a,
                      m.contents + INFS_LOCATOR_BLOCK_A * INFS_BLOCK_SIZE,
                      sizeof(locator_a)) == 0 &&
               memcmp(locator_b,
                      m.contents + INFS_LOCATOR_BLOCK_B * INFS_BLOCK_SIZE,
                      sizeof(locator_b)) == 0,
               "ordinary 0.19 writes do not rewrite discovery locators");
        infs_volume_close(&writer);
        expect(infs_volume_verify_019_test_image(&storage, &committed) ==
                   INFS_STATUS_OK &&
               infs_le64_to_cpu(committed.generation) > 1u,
               "committed 0.19 graph verifies after writer closes");
        expect(infs_volume_open_019_test_image(&reader, &rs) ==
                   INFS_STATUS_OK,
               "read-only 0.19 reopen sees newly committed generation");
        expect(infs_read_file(&reader, "/written", readback,
                   sizeof(payload)-1u, 0) == (int64_t)(sizeof(payload)-1u) &&
               memcmp(readback, payload, sizeof(payload)-1u) == 0,
               "0.19 read-only reopen preserves committed payload bytes");
        infs_volume_close(&reader);
        {
            struct infs_volume denied = {0};
            struct infs_storage probe = { .ops = &ops, .context = &m };
            unsigned before = m.writes;
            m.device = 1;
            expect(infs_volume_open_019_rw_test_image(&denied, &probe) ==
                       INFS_STATUS_NOT_SUPPORTED &&
                   m.writes == before && probe.ops == &ops,
                   "0.19 experimental writer refuses physical media without writes");
            m.device = 0;
            m.contents[INFS_LOCATOR_BLOCK_A * INFS_BLOCK_SIZE] ^= 0xffu;
            expect(infs_volume_open_019_rw_test_image(&denied, &probe) ==
                       INFS_STATUS_CORRUPT &&
                   m.writes == before && probe.ops == &ops,
                   "0.19 writable opener rejects damaged locator without healing");
            m.contents[INFS_LOCATOR_BLOCK_A * INFS_BLOCK_SIZE] ^= 0xffu;
            m.unreadable_blocks[0] = IMAGE_BYTES / INFS_BLOCK_SIZE - 1u;
            m.unreadable_count = 1;
            expect(infs_volume_open_019_rw_test_image(&denied, &probe) ==
                       INFS_STATUS_CORRUPT &&
                   m.writes == before && probe.ops == &ops,
                   "0.19 writable opener rejects unreadable checkpoint");
            m.unreadable_count = 0;
        }
        reset(&m);
        expect(infs_format_storage_019_test_image(&storage, "Fixture19") ==
                   INFS_STATUS_OK,
               "restore empty 0.19 image for independent recovery tests");
    }

    /* Exercise all three independent checkpoint flush failures in a
     * regular-image-only writer. An ambiguous publication MUST poison the
     * writer, preserve the fixed locators, and leave a graph which can be
     * independently validated after a fresh read-only reopen. This is an
     * I/O-error model, not physical power-loss durability qualification. */
    for (unsigned fault = 1u; fault <= INFS_CHECKPOINT_COUNT; ++fault) {
        struct infs_volume writer = {0}, reader = {0};
        struct infs_storage ws = { .ops = &ops, .context = &m };
        struct infs_storage rs = { .ops = &ops, .context = &m };
        struct infs_superblock_disk recovered = {0};
        struct infs_lookup root_lookup = {0};
        struct infs_create_options options = { .posix_permissions = 0644u };
        uint8_t locator_a[INFS_BLOCK_SIZE], locator_b[INFS_BLOCK_SIZE];
        unsigned before = m.checkpoint_flushes;
        unsigned poisoned_writes;
        memcpy(locator_a,
               m.contents + INFS_LOCATOR_BLOCK_A * INFS_BLOCK_SIZE,
               sizeof(locator_a));
        memcpy(locator_b,
               m.contents + INFS_LOCATOR_BLOCK_B * INFS_BLOCK_SIZE,
               sizeof(locator_b));
        expect(infs_volume_open_019_rw_test_image(&writer, &ws) ==
                   INFS_STATUS_OK,
               "open clean 0.19 image before flush-failure matrix");
        m.fail_checkpoint_flush = before + fault;
        expect(infs_create_file(&writer, "/ambiguous", &options) ==
                   INFS_STATUS_IO_ERROR,
               "fault at independently flushed 0.19 checkpoint is reported");
        expect(m.checkpoint_flushes == before + fault,
               "fault injection stopped at requested checkpoint flush");
        poisoned_writes = m.writes;
        expect(!writer.writable &&
                   writer.reopen_required_status == INFS_STATUS_IO_ERROR &&
                   infs_volume_sync(&writer) == INFS_STATUS_IO_ERROR &&
                   m.writes == poisoned_writes,
               "checkpoint failure poisons writer without further I/O writes");
        m.fail_checkpoint_flush = 0;
        infs_volume_close(&writer);
        expect(memcmp(locator_a,
                      m.contents + INFS_LOCATOR_BLOCK_A * INFS_BLOCK_SIZE,
                      sizeof(locator_a)) == 0 &&
               memcmp(locator_b,
                      m.contents + INFS_LOCATOR_BLOCK_B * INFS_BLOCK_SIZE,
                      sizeof(locator_b)) == 0,
               "same-geometry fault does not rewrite recovery locators");
        expect(infs_volume_verify_019_test_image(&storage, &recovered) ==
                   INFS_STATUS_OK,
               "full 0.19 graph remains recoverable after checkpoint I/O error");
        expect(infs_volume_open_019_test_image(&reader, &rs) ==
                   INFS_STATUS_OK && !reader.writable,
               "fresh read-only 0.19 reopen after interrupted publication");
        expect(infs_lookup_path(&reader, "/", &root_lookup) ==
                   INFS_STATUS_OK &&
                   root_lookup.type == INFS_OBJECT_DIRECTORY,
               "post-error recovery preserves validated root directory");
        infs_volume_close(&reader);
        reset(&m);
        expect(infs_format_storage_019_test_image(&storage, "Fixture19") ==
                   INFS_STATUS_OK,
               "restore fresh 0.19 image before next flush-failure case");
    }

    /* A failed read is not proof that every independent recovery copy is
     * unusable. Exercise unreadable locator and checkpoint pages, not just
     * validly read pages with corrupted checksums. */
    {
        struct infs_superblock_disk committed = {0};
        struct infs_superblock_disk found[INFS_LOCATOR_MAX_CANDIDATES];
        uint8_t valid[INFS_LOCATOR_MAX_CANDIDATES];
        unsigned count = 0;
        unsigned before = m.writes;

        m.unreadable_count = 1;
        m.unreadable_blocks[0] = INFS_LOCATOR_BLOCK_A;
        expect(infs_locator_discover_019_headers(
                   &storage, IMAGE_BYTES, found, valid, &count) ==
                   INFS_STATUS_OK && count >= 2,
               "unreadable locator A does not hide surviving locator B");
        expect(infs_volume_verify_019_test_image(&storage, &committed) ==
                   INFS_STATUS_OK,
               "complete graph recovers with unreadable locator A");

        m.unreadable_blocks[0] = 0;
        expect(infs_locator_discover_019_headers(
                   &storage, IMAGE_BYTES, found, valid, &count) ==
                   INFS_STATUS_OK && count >= 4,
               "unreadable primary checkpoint does not hide secondaries");
        expect(infs_volume_verify_019_test_image(&storage, &committed) ==
                   INFS_STATUS_OK,
               "complete graph recovers after primary checkpoint read error");

        m.unreadable_count = 2;
        m.unreadable_blocks[0] = 0;
        m.unreadable_blocks[1] = IMAGE_BYTES / INFS_BLOCK_SIZE - 1u;
        expect(infs_volume_verify_019_test_image(&storage, &committed) ==
                   INFS_STATUS_OK,
               "complete graph recovers with only middle checkpoint readable");

        m.unreadable_blocks[0] = INFS_LOCATOR_BLOCK_A;
        m.unreadable_blocks[1] = INFS_LOCATOR_BLOCK_B;
        expect(infs_locator_discover_019_headers(
                   &storage, IMAGE_BYTES, found, valid, &count) ==
                   INFS_STATUS_OK && count == 3,
               "valid primary checkpoint survives both locator read errors");
        expect(infs_volume_verify_019_test_image(&storage, &committed) ==
                   INFS_STATUS_OK,
               "full verifier recovers through primary with unreadable locators");

        m.unreadable_count = 3;
        m.unreadable_blocks[2] = 0;
        expect(infs_locator_discover_019_headers(
                   &storage, IMAGE_BYTES, found, valid, &count) ==
                   INFS_STATUS_IO_ERROR,
               "all three unreadable discovery pages preserve media I/O error");
        expect(infs_volume_verify_019_test_image(&storage, &committed) ==
                   INFS_STATUS_IO_ERROR,
               "full verifier cannot invent addresses when every source fails");
        m.unreadable_count = 0;
        expect(m.writes == before, "read-only I/O recovery never writes media");
    }

    /* Losing BOTH discovery locators must not hide an independently valid
     * primary checkpoint and its explicitly recorded backup locations. */
    {
        uint8_t saved_a[INFS_BLOCK_SIZE];
        uint8_t saved_b[INFS_BLOCK_SIZE];
        struct infs_superblock_disk committed = {0};
        struct infs_volume view = {0};
        struct infs_storage view_storage = { .ops = &ops, .context = &m };
        struct infs_lookup root_lookup = {0};
        unsigned before = m.writes;
        memcpy(saved_a, m.contents + INFS_LOCATOR_BLOCK_A * INFS_BLOCK_SIZE,
               sizeof(saved_a));
        memcpy(saved_b, m.contents + INFS_LOCATOR_BLOCK_B * INFS_BLOCK_SIZE,
               sizeof(saved_b));
        m.contents[INFS_LOCATOR_BLOCK_A * INFS_BLOCK_SIZE] ^= 0xffu;
        m.contents[INFS_LOCATOR_BLOCK_B * INFS_BLOCK_SIZE] ^= 0xffu;
        expect(infs_volume_verify_019_test_image(&storage, &committed) ==
                   INFS_STATUS_OK,
               "valid primary recovers committed graph without either locator");
        expect(infs_volume_open_019_test_image(&view, &view_storage) ==
                   INFS_STATUS_OK,
               "read-only image opens after losing both fixed locators");
        expect(infs_lookup_path(&view, "/", &root_lookup) == INFS_STATUS_OK &&
                   root_lookup.type == INFS_OBJECT_DIRECTORY,
               "recovered primary supplies trustworthy root namespace");
        infs_volume_close(&view);
        memcpy(m.contents + INFS_LOCATOR_BLOCK_A * INFS_BLOCK_SIZE,
               saved_a, sizeof(saved_a));
        memcpy(m.contents + INFS_LOCATOR_BLOCK_B * INFS_BLOCK_SIZE,
               saved_b, sizeof(saved_b));
        expect(m.writes == before,
               "primary-only read-only recovery never rewrites locators");
    }

    /* Before destructive crash-fixture mutations, optionally emit the exact
     * fully validated Format 0.19 image for native loop-mounted RO tests. */
    if (argc == 3) {
        FILE *out = fopen(argv[2], "wb");
        expect(out != NULL, "create disposable 0.19 image fixture");
        expect(fwrite(m.contents, 1u, IMAGE_BYTES, out) == IMAGE_BYTES,
               "write complete 0.19 image fixture");
        expect(fclose(out) == 0, "close 0.19 fixture image");
        free(m.contents);
        puts("Format 0.19 image emitted: PASS");
        return 0;
    }

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
        {
            struct infs_superblock_disk committed = {0};
            expect(infs_volume_verify_019_test_image(
                       &storage, &committed) == INFS_STATUS_OK,
                   "0.19 full graph recovered after primary checkpoint loss");
        }
        expect(infs_locator_discover_019_headers(
                   &storage, IMAGE_BYTES, found, valid, &count) ==
                   INFS_STATUS_OK && count >= 2,
               "surviving 0.19 checkpoints discoverable without block zero");
        {
            struct infs_volume view = {0};
            struct infs_storage view_storage = { .ops = &ops, .context = &m };
            struct infs_lookup root_lookup = {0};
            expect(infs_volume_open_019_test_image(
                       &view, &view_storage) == INFS_STATUS_OK,
                   "0.19 read-only volume recovers after block-zero loss");
            expect(infs_lookup_path(&view, "/", &root_lookup) ==
                       INFS_STATUS_OK,
                   "recovered 0.19 volume reads root through normal namespace");
            infs_volume_close(&view);
        }

        expect(infs_le64_to_cpu(found[1].generation) == 1 ||
               infs_le64_to_cpu(found[2].generation) == 1,
               "recovered secondary checkpoint carries valid generation");

        /* One corrupt fixed locator must not hide the surviving backup. */
        m.contents[INFS_BLOCK_SIZE * INFS_LOCATOR_BLOCK_A] ^= 0xffu;
        {
            struct infs_superblock_disk committed = {0};
            expect(infs_volume_verify_019_test_image(
                       &storage, &committed) == INFS_STATUS_OK,
                   "full graph still recoverable with damaged primary and locator A");
        }
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
        {
            struct infs_superblock_disk committed = {0};
            expect(infs_volume_verify_019_test_image(
                       &storage, &committed) == INFS_STATUS_CORRUPT,
                   "full graph verifier cannot invent missing locator addresses");
        }
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
