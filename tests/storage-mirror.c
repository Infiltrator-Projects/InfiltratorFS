// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/storage_mirror.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct member {
    unsigned char bytes[8192];
    int fail_reads;
    int fail_writes;
    unsigned flushes;
    int closed;
};

static void die(const char *message)
{
    fprintf(stderr, "storage-mirror: %s\n", message);
    exit(1);
}

static void ok(int condition, const char *message)
{
    if (!condition)
        die(message);
}

static infs_status rd(void *opaque, uint64_t offset, void *buffer, size_t size)
{
    struct member *m = opaque;
    if (m->fail_reads)
        return INFS_STATUS_IO_ERROR;
    if (offset > sizeof(m->bytes) || size > sizeof(m->bytes) - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(buffer, m->bytes + (size_t)offset, size);
    return INFS_STATUS_OK;
}

static infs_status wr(void *opaque, uint64_t offset,
                      const void *buffer, size_t size)
{
    struct member *m = opaque;
    if (m->fail_writes)
        return INFS_STATUS_IO_ERROR;
    if (offset > sizeof(m->bytes) || size > sizeof(m->bytes) - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(m->bytes + (size_t)offset, buffer, size);
    return INFS_STATUS_OK;
}

static infs_status fl(void *opaque)
{
    struct member *m = opaque;
    ++m->flushes;
    return m->fail_writes ? INFS_STATUS_IO_ERROR : INFS_STATUS_OK;
}

static infs_status sz(void *opaque, uint64_t *bytes, int *is_device)
{
    (void)opaque;
    *bytes = 8192;
    *is_device = 1;
    return INFS_STATUS_OK;
}

static infs_status rnd(void *opaque, void *buffer, size_t size)
{
    (void)opaque;
    memset(buffer, 0xa5, size);
    return INFS_STATUS_OK;
}

static infs_status now(void *opaque, struct infs_timestamp *time)
{
    (void)opaque;
    time->seconds = 1;
    time->nanoseconds = 2;
    return INFS_STATUS_OK;
}

static void cls(void *opaque)
{
    ((struct member *)opaque)->closed = 1;
}

static const struct infs_storage_ops ops = {
    .read_at = rd,
    .write_at = wr,
    .flush = fl,
    .get_size = sz,
    .random_bytes = rnd,
    .current_time = now,
    .close = cls,
};

int main(void)
{
    struct member a = {0}, b = {0};
    struct infs_storage members[2] = {
        { .ops = &ops, .context = &a },
        { .ops = &ops, .context = &b },
    };
    struct infs_storage mirror = {0};

    ok(infs_storage_mirror_create(members, 2, &mirror) == INFS_STATUS_OK,
       "create mirror");
    ok(!members[0].ops && !members[1].ops, "member ownership transferred");

    static const char payload[] = "replicated";
    ok(infs_storage_write(&mirror, 123, payload, sizeof(payload)) ==
           INFS_STATUS_OK,
       "replicated write");
    ok(!memcmp(a.bytes + 123, payload, sizeof(payload)) &&
       !memcmp(b.bytes + 123, payload, sizeof(payload)),
       "both members contain payload");
    ok(infs_storage_flush(&mirror) == INFS_STATUS_OK &&
       a.flushes == 1 && b.flushes == 1,
       "durability barrier reaches every member");

    char readback[sizeof(payload)] = {0};
    a.fail_reads = 1;
    ok(infs_storage_read(&mirror, 123, readback, sizeof(readback)) ==
           INFS_STATUS_OK &&
       !memcmp(readback, payload, sizeof(payload)),
       "read fails over to surviving replica");

    b.fail_writes = 1;
    static const char degraded[] = "degraded";
    ok(infs_storage_write(&mirror, 512, degraded, sizeof(degraded)) ==
           INFS_STATUS_IO_ERROR,
       "degraded write is never reported as healthy");
    ok(!memcmp(a.bytes + 512, degraded, sizeof(degraded)),
       "healthy replica still receives degraded write");

    /*
     * The member that missed a write is quarantined for the lifetime of this
     * mirror, so a later transient read recovery cannot serve stale bytes.
     */
    a.fail_reads = 0;
    b.fail_writes = 0;
    b.fail_reads = 0;
    memset(readback, 0, sizeof(readback));
    ok(infs_storage_read(&mirror, 512, readback, sizeof(degraded)) ==
           INFS_STATUS_OK &&
       !memcmp(readback, degraded, sizeof(degraded)),
       "runtime quarantine prevents stale-member reads");

    uint64_t bytes = 0;
    int is_device = 0;
    ok(infs_storage_get_size(&mirror, &bytes, &is_device) == INFS_STATUS_OK &&
       bytes == 8192 && is_device,
       "mirror geometry");

    infs_storage_close(&mirror);
    ok(a.closed && b.closed, "mirror closes every owned member");

    /*
     * Recreating the mirror loses the in-memory quarantine state. A stale but
     * readable replica must therefore be detected by cross-member comparison,
     * never silently selected as authoritative.
     */
    a.closed = 0;
    b.closed = 0;
    struct infs_storage reopened_members[2] = {
        { .ops = &ops, .context = &a },
        { .ops = &ops, .context = &b },
    };
    memset(&mirror, 0, sizeof(mirror));
    ok(infs_storage_mirror_create(reopened_members, 2, &mirror) ==
           INFS_STATUS_OK,
       "recreate mirror with divergent members");
    memset(readback, 0, sizeof(readback));
    ok(infs_storage_read(&mirror, 512, readback, sizeof(degraded)) ==
           INFS_STATUS_CORRUPT,
       "split-brain replicas are rejected instead of serving stale data");
    infs_storage_close(&mirror);
    ok(a.closed && b.closed, "reopened mirror closes every member");

    /*
     * Per-object protection policy must select an exact deterministic subset
     * of mirror members rather than silently falling back to complete-volume
     * mirroring. A one-copy class therefore writes exactly one member.
     */
    struct member c_member = {0}, d_member = {0};
    struct infs_storage policy_members[2] = {
        { .ops = &ops, .context = &c_member },
        { .ops = &ops, .context = &d_member },
    };
    memset(&mirror, 0, sizeof(mirror));
    ok(infs_storage_mirror_create(policy_members, 2, &mirror) ==
           INFS_STATUS_OK,
       "create policy-aware mirror");
    struct infs_storage_io_policy policy = {0};
    for (size_t i = 0; i < sizeof(policy.object_id); ++i)
        policy.object_id[i] = (uint8_t)(i * 17u + 3u);
    policy.protection_copies = 1u;
    static const char one_copy[] = "one-copy-policy";
    ok(infs_storage_write_policy(
           &mirror, 2048, one_copy, sizeof(one_copy), &policy) ==
           INFS_STATUS_OK,
       "one-copy policy write");
    int on_c = !memcmp(c_member.bytes + 2048, one_copy, sizeof(one_copy));
    int on_d = !memcmp(d_member.bytes + 2048, one_copy, sizeof(one_copy));
    ok(on_c != on_d, "one-copy policy lands on exactly one member");

    char policy_readback[sizeof(one_copy)] = {0};
    ok(infs_storage_read_policy(
           &mirror, 2048, policy_readback, sizeof(policy_readback), &policy) ==
           INFS_STATUS_OK &&
       !memcmp(policy_readback, one_copy, sizeof(one_copy)),
       "one-copy policy reads from selected member");

    policy.protection_copies = 2u;
    static const char two_copy[] = "two-copy-policy";
    ok(infs_storage_write_policy(
           &mirror, 3072, two_copy, sizeof(two_copy), &policy) ==
           INFS_STATUS_OK,
       "two-copy policy write");
    ok(!memcmp(c_member.bytes + 3072, two_copy, sizeof(two_copy)) &&
       !memcmp(d_member.bytes + 3072, two_copy, sizeof(two_copy)),
       "two-copy policy reaches both members");
    infs_storage_close(&mirror);
    ok(c_member.closed && d_member.closed,
       "policy-aware mirror closes every member");

    puts("replicated storage backend: PASS");
    return 0;
}
