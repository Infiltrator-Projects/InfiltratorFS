// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Model-level Format 0.19 crash protocol test. Does not simulate device
 * firmware or replace native mounted power-loss/fault qualification.
 *
 * Model each write followed by its own durable flush and interrupt at every
 * stage. Also corrupt block zero at each interruption and require at least
 * one surviving old/new checkpoint to be discoverable from a fixed locator.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct checkpoint_map {
    uint64_t block[3];
    int valid;
    int generation;
};
struct checkpoint_copy {
    uint64_t block;
    int valid;
    struct checkpoint_map identity;
};
struct simulation {
    struct checkpoint_map a, b, old_map, new_map;
    struct checkpoint_copy disk[6];
    unsigned disk_count;
};
static void ensure(int ok, const char *why)
{
    if (!ok) {
        fprintf(stderr, "Format 0.19 crash protocol: %s\n", why);
        exit(1);
    }
}
static int same_map(const struct checkpoint_map *a,
                    const struct checkpoint_map *b)
{
    return a->generation == b->generation &&
           memcmp(a->block, b->block, sizeof(a->block)) == 0;
}
static void put_checkpoint(struct simulation *s, uint64_t location,
                           const struct checkpoint_map *identity, int torn)
{
    unsigned i;
    for (i = 0; i < s->disk_count; ++i)
        if (s->disk[i].block == location)
            break;
    if (i == s->disk_count) {
        ensure(i < 6u, "too many physical checkpoint positions in model");
        ++s->disk_count;
    }
    s->disk[i].block = location;
    s->disk[i].valid = !torn;
    s->disk[i].identity = *identity;
}
static void setup(struct simulation *s, uint64_t old1, uint64_t old2,
                  uint64_t new1, uint64_t new2)
{
    memset(s, 0, sizeof(*s));
    s->old_map.block[1] = old1;
    s->old_map.block[2] = old2;
    s->old_map.generation = 19;
    s->old_map.valid = 1;
    s->new_map.block[1] = new1;
    s->new_map.block[2] = new2;
    s->new_map.generation = 20;
    s->new_map.valid = 1;
    s->a = s->b = s->old_map;
    for (unsigned i = 0; i < 3; ++i)
        put_checkpoint(s, s->old_map.block[i], &s->old_map, 0);
}
static int discover_without_block_zero(const struct simulation *s)
{
    const struct checkpoint_map *locators[2] = {&s->a, &s->b};
    for (unsigned i = 0; i < 2; ++i) {
        const struct checkpoint_map *hint = locators[i];
        if (!hint->valid)
            continue;
        for (unsigned j = 0; j < s->disk_count; ++j) {
            const struct checkpoint_copy *cp = &s->disk[j];
            if (cp->valid && cp->block &&
                (cp->block == hint->block[1] ||
                 cp->block == hint->block[2]) &&
                same_map(&cp->identity, hint))
                return 1;
        }
    }
    return 0;
}
/*
 * 0: publish A hint; 1: publish block-zero checkpoint; 2: flush backup 1;
 * 3: flush backup 2 (separately!); 4: publish B hint.
 */
static void do_step(struct simulation *s, unsigned step, int torn)
{
    switch (step) {
    case 0: s->a = s->new_map; s->a.valid = !torn; break;
    case 1: put_checkpoint(s, 0, &s->new_map, torn); break;
    case 2: put_checkpoint(s, s->new_map.block[1], &s->new_map, torn); break;
    case 3: put_checkpoint(s, s->new_map.block[2], &s->new_map, torn); break;
    case 4: s->b = s->new_map; s->b.valid = !torn; break;
    default: ensure(0, "unknown protocol step");
    }
}
static void scenario(uint64_t old1, uint64_t old2,
                     uint64_t new1, uint64_t new2)
{
    struct simulation s;
    setup(&s, old1, old2, new1, new2);
    ensure(discover_without_block_zero(&s),
           "old secondary checkpoints not recoverable");
    for (unsigned interrupted = 0; interrupted < 5; ++interrupted) {
        for (int torn = 0; torn <= 1; ++torn) {
            setup(&s, old1, old2, new1, new2);
            for (unsigned completed = 0; completed < interrupted; ++completed)
                do_step(&s, completed, 0);
            do_step(&s, interrupted, torn);
            /* Fail closed: never proceed to the next write after a failed
             * flush, torn write, or indeterminate publication. */
            ensure(discover_without_block_zero(&s),
                   "torn publication lost all checkpoint discoverability");
        }
    }
    setup(&s, old1, old2, new1, new2);
    for (unsigned step = 0; step < 5; ++step)
        do_step(&s, step, 0);
    ensure(discover_without_block_zero(&s),
           "fully published geometry not recoverable");
    if (old1 == new1 && old2 == new2) {
        /* Negative control: a parallel torn overwrite of both backups
         * destroys recovery if block zero is also damaged. */
        setup(&s, old1, old2, new1, new2);
        do_step(&s, 0, 0);
        do_step(&s, 1, 0);
        do_step(&s, 2, 1);
        do_step(&s, 3, 1);
        ensure(!discover_without_block_zero(&s),
               "concurrent secondary overwrite should be rejected");
    }
}
int main(void)
{
    scenario(8192, 16383, 8192, 16383);
    scenario(8192, 16383, 8192, 14000);
    scenario(8192, 16383, 7800, 14000);
    scenario(8192, 16383, 14000, 30000);
    puts("Format 0.19 model recovery under torn writes: PASS");
    return 0;
}
