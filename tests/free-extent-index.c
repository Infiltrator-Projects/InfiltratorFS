// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/volume.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_BLOCKS UINT64_C(256)

int infs_internal_test_free_extent_rebuild(struct infs_volume *vol);
void infs_internal_test_free_extent_destroy(struct infs_volume *vol);
int infs_internal_test_free_extent_remove(struct infs_volume *vol,
                                          uint64_t start, uint64_t count);
int infs_internal_test_free_extent_add(struct infs_volume *vol,
                                       uint64_t start, uint64_t count);
int infs_internal_test_free_extent_choose_forward(
    const struct infs_volume *vol, uint64_t wanted, uint64_t cursor,
    uint64_t *start_out, uint64_t *count_out);
int infs_internal_test_free_extent_choose_reverse(
    const struct infs_volume *vol, uint64_t wanted, uint64_t cursor,
    int exact, uint64_t *start_out, uint64_t *count_out);

static void fail(const char *message)
{
    fprintf(stderr, "free-extent-index: %s\n", message);
    exit(1);
}

static void expect(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static int bitmap_get(const uint8_t *bitmap, uint64_t block)
{
    return (bitmap[block >> 3] >> (block & 7u)) & 1u;
}

static void bitmap_set(uint8_t *bitmap, uint64_t block, int allocated)
{
    uint8_t mask = (uint8_t)(1u << (block & 7u));
    if (allocated)
        bitmap[block >> 3] |= mask;
    else
        bitmap[block >> 3] &= (uint8_t)~mask;
}

static void volume_reset(struct infs_volume *vol)
{
    infs_internal_test_free_extent_destroy(vol);
    free(vol->bitmap);
    memset(vol, 0, sizeof(*vol));
    vol->sb.total_blocks = infs_cpu_to_le64(TEST_BLOCKS);
    vol->bitmap_bytes = (size_t)((TEST_BLOCKS + 7u) / 8u);
    vol->bitmap = malloc(vol->bitmap_bytes);
    if (!vol->bitmap)
        fail("allocate bitmap");
    memset(vol->bitmap, 0xff, vol->bitmap_bytes);
}

static void mark_free(struct infs_volume *vol, uint64_t start, uint64_t count)
{
    expect(start <= TEST_BLOCKS && count <= TEST_BLOCKS - start,
           "free range bounds");
    for (uint64_t block = start; block < start + count; ++block)
        bitmap_set(vol->bitmap, block, 0);
}

static uint64_t bitmap_largest_free_run(const struct infs_volume *vol)
{
    uint64_t best = 0;
    uint64_t run = 0;
    for (uint64_t block = 0; block < TEST_BLOCKS; ++block) {
        if (!bitmap_get(vol->bitmap, block)) {
            ++run;
            if (run > best)
                best = run;
        } else {
            run = 0;
        }
    }
    return best;
}

static void expect_selected_free(const struct infs_volume *vol,
                                 uint64_t start, uint64_t count)
{
    expect(count != 0 && start <= TEST_BLOCKS &&
           count <= TEST_BLOCKS - start, "selected range bounds");
    for (uint64_t block = start; block < start + count; ++block)
        expect(!bitmap_get(vol->bitmap, block), "selected range is free");
}

static uint64_t next_random(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static void check_cursor_straddle(struct infs_volume *vol)
{
    uint64_t start = 0;
    uint64_t count = 0;

    volume_reset(vol);
    mark_free(vol, 100, 100);
    expect(infs_internal_test_free_extent_rebuild(vol),
           "rebuild cursor-straddle index");

    expect(infs_internal_test_free_extent_choose_forward(
               vol, 80, 150, &start, &count) &&
           start == 100 && count == 80,
           "forward allocation reuses whole cursor-straddling extent");

    start = count = 0;
    expect(infs_internal_test_free_extent_choose_reverse(
               vol, 80, 150, 1, &start, &count) &&
           start == 120 && count == 80,
           "exact reverse allocation reuses whole cursor-straddling extent");
}

static void check_split_merge(struct infs_volume *vol)
{
    uint64_t start = 0;
    uint64_t count = 0;

    volume_reset(vol);
    mark_free(vol, 50, 50);
    expect(infs_internal_test_free_extent_rebuild(vol),
           "rebuild split/merge index");
    expect(infs_internal_test_free_extent_remove(vol, 65, 10),
           "remove middle allocation from free extent");
    expect(infs_internal_test_free_extent_choose_forward(
               vol, 20, 1, &start, &count) &&
           start == 75 && count == 20,
           "split retains larger trailing free extent");
    expect(infs_internal_test_free_extent_add(vol, 65, 10),
           "merge returned range into free index");
    start = count = 0;
    expect(infs_internal_test_free_extent_choose_forward(
               vol, 50, 1, &start, &count) &&
           start == 50 && count == 50,
           "adjacent free ranges merge back to maximal extent");
}

static void check_fragmented_partial(struct infs_volume *vol)
{
    uint64_t start = 0;
    uint64_t count = 0;

    volume_reset(vol);
    mark_free(vol, 10, 20);
    mark_free(vol, 100, 35);
    expect(infs_internal_test_free_extent_rebuild(vol),
           "rebuild fragmented index");

    expect(infs_internal_test_free_extent_choose_forward(
               vol, 50, 120, &start, &count) &&
           start == 100 && count == 35,
           "forward partial allocation uses largest physical extent");

    start = count = 0;
    expect(!infs_internal_test_free_extent_choose_reverse(
               vol, 50, 120, 1, &start, &count),
           "exact reverse allocation reports no full extent");

    start = count = 0;
    expect(infs_internal_test_free_extent_choose_reverse(
               vol, 50, 120, 0, &start, &count) &&
           start == 100 && count == 35,
           "reverse partial allocation uses largest physical extent");
}

static void check_bitmap_oracle(struct infs_volume *vol)
{
    uint64_t state = UINT64_C(0xd1b54a32d192ed03);

    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        volume_reset(vol);
        for (uint64_t block = 1; block < TEST_BLOCKS; ++block) {
            if ((next_random(&state) & 3u) == 0)
                bitmap_set(vol->bitmap, block, 0);
        }
        expect(infs_internal_test_free_extent_rebuild(vol),
               "rebuild randomized index");

        uint64_t largest = bitmap_largest_free_run(vol);
        for (unsigned probe = 0; probe < 16; ++probe) {
            uint64_t wanted = 1u + (next_random(&state) % 24u);
            uint64_t cursor = 1u + (next_random(&state) % (TEST_BLOCKS - 1u));
            uint64_t start = 0;
            uint64_t count = 0;
            int found = infs_internal_test_free_extent_choose_forward(
                vol, wanted, cursor, &start, &count);

            if (!largest) {
                expect(!found, "forward oracle reports full volume");
            } else {
                expect(found, "forward oracle finds free space");
                expect(count == (largest >= wanted ? wanted : largest),
                       "forward oracle preserves maximal-run semantics");
                expect_selected_free(vol, start, count);
            }

            start = count = 0;
            found = infs_internal_test_free_extent_choose_reverse(
                vol, wanted, cursor, 1, &start, &count);
            if (largest >= wanted) {
                expect(found && count == wanted,
                       "exact reverse oracle finds available full run");
                expect_selected_free(vol, start, count);
            } else {
                expect(!found,
                       "exact reverse oracle refuses insufficient fragments");
            }

            start = count = 0;
            found = infs_internal_test_free_extent_choose_reverse(
                vol, wanted, cursor, 0, &start, &count);
            if (!largest) {
                expect(!found, "reverse oracle reports full volume");
            } else {
                expect(found, "reverse oracle finds free space");
                expect(count == (largest >= wanted ? wanted : largest),
                       "reverse oracle preserves maximal-run semantics");
                expect_selected_free(vol, start, count);
            }
        }
    }
}

int main(void)
{
    struct infs_volume volume;
    memset(&volume, 0, sizeof(volume));

    check_cursor_straddle(&volume);
    check_split_merge(&volume);
    check_fragmented_partial(&volume);
    check_bitmap_oracle(&volume);

    infs_internal_test_free_extent_destroy(&volume);
    free(volume.bitmap);
    puts("free-extent-index: PASS");
    return 0;
}
