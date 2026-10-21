// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/storage_mirror.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <pthread.h>
#include <time.h>
#endif

#if !defined(_WIN32)
struct race_state {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int first_on_primary;
    int second_finished;
};

struct concurrent_write {
    struct infs_storage *storage;
    struct race_state *race;
    char value;
    infs_status status;
};

static void *concurrent_write_thread(void *opaque)
{
    struct concurrent_write *op = opaque;
    unsigned char bytes[128];
    memset(bytes, op->value, sizeof(bytes));
    op->status = infs_storage_write(op->storage, 6144, bytes, sizeof(bytes));
    if (op->value == 'B') {
        pthread_mutex_lock(&op->race->lock);
        op->race->second_finished = 1;
        pthread_cond_broadcast(&op->race->changed);
        pthread_mutex_unlock(&op->race->lock);
    }
    return NULL;
}
#endif

struct member {
    unsigned char bytes[8192];
    int fail_reads;
    infs_status forced_read_status;
    int fail_writes;
    infs_status forced_flush_status;
    infs_status forced_random_status;
    infs_status forced_time_status;
    unsigned flushes;
    int closed;
#if !defined(_WIN32)
    struct race_state *race;
#endif
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
    if (m->forced_read_status != INFS_STATUS_OK)
        return m->forced_read_status;
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
#if !defined(_WIN32)
    if (m->race && offset == 6144 && size == 128 &&
        ((const unsigned char *)buffer)[0] == 'A') {
        /*
         * The first writer has completed its first replica and pauses.
         * Before the correction, writer B could pass it and finish both
         * members; writer A then wrote the second member last, splitting the
         * replicas. A complete-operation lock makes B wait, so A reaches
         * the deadline before finishing and B overwrites both members last.
         */
        struct race_state *race = m->race;
        pthread_mutex_lock(&race->lock);
        race->first_on_primary = 1;
        pthread_cond_broadcast(&race->changed);
        struct timespec deadline;
        timespec_get(&deadline, TIME_UTC);
        deadline.tv_nsec += 300000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
        while (!race->second_finished)
            if (pthread_cond_timedwait(
                    &race->changed, &race->lock, &deadline) != 0)
                break;
        pthread_mutex_unlock(&race->lock);
    }
#endif
    return INFS_STATUS_OK;
}

static infs_status fl(void *opaque)
{
    struct member *m = opaque;
    ++m->flushes;
    if (m->forced_flush_status != INFS_STATUS_OK)
        return m->forced_flush_status;
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
    struct member *m = opaque;
    if (m->forced_random_status != INFS_STATUS_OK)
        return m->forced_random_status;
    memset(buffer, 0xa5, size);
    return INFS_STATUS_OK;
}

static infs_status now(void *opaque, struct infs_timestamp *time)
{
    struct member *m = opaque;
    if (m->forced_time_status != INFS_STATUS_OK)
        return m->forced_time_status;
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

    struct infs_storage aliased_members[2] = {
        { .ops = &ops, .context = &a },
        { .ops = &ops, .context = &b },
    };
    ok(infs_storage_mirror_create(aliased_members, 2, &aliased_members[1]) ==
           INFS_STATUS_INVALID_ARGUMENT,
       "mirror rejects an output aliased to an input member");
    ok(aliased_members[0].ops == &ops && aliased_members[1].ops == &ops &&
       aliased_members[1].context == &b,
       "rejected alias preserves ownership of every caller member");

    struct infs_storage duplicate_members[2] = {
        { .ops = &ops, .context = &a },
        { .ops = &ops, .context = &a },
    };
    ok(infs_storage_mirror_create(duplicate_members, 2, &mirror) ==
           INFS_STATUS_INVALID_ARGUMENT,
       "reject duplicate ownership of one backend as two replicas");
    ok(duplicate_members[0].ops == &ops &&
       duplicate_members[1].ops == &ops && !mirror.ops &&
       !a.closed,
       "duplicate backend refusal leaves both handles untouched");

    ok(infs_storage_mirror_create(members, 2, &mirror) == INFS_STATUS_OK,
       "create mirror");
    ok(!members[0].ops && !members[1].ops, "member ownership transferred");

    /*
     * A temporary service failure is not an absent service. Only device I/O
     * permits fallback, and an exhausted failover must retain its real error.
     */
    uint8_t entropy[16] = {0};
    a.forced_random_status = INFS_STATUS_IO_ERROR;
    b.forced_random_status = INFS_STATUS_IO_ERROR;
    ok(infs_storage_random(&mirror, entropy, sizeof(entropy)) ==
           INFS_STATUS_IO_ERROR, "entropy failure is not unsupported");
    b.forced_random_status = INFS_STATUS_OK;
    ok(infs_storage_random(&mirror, entropy, sizeof(entropy)) ==
           INFS_STATUS_OK && entropy[0] == 0xa5,
       "entropy device I/O can fail over");
    a.forced_random_status = INFS_STATUS_CORRUPT;
    ok(infs_storage_random(&mirror, entropy, sizeof(entropy)) ==
           INFS_STATUS_CORRUPT, "entropy semantic failure is not masked");
    a.forced_random_status = INFS_STATUS_OK;

    struct infs_timestamp timestamp = {0};
    a.forced_time_status = INFS_STATUS_IO_ERROR;
    b.forced_time_status = INFS_STATUS_NOT_SUPPORTED;
    ok(infs_storage_current_time(&mirror, &timestamp) ==
           INFS_STATUS_IO_ERROR, "clock I/O failure is not unsupported");
    b.forced_time_status = INFS_STATUS_OK;
    ok(infs_storage_current_time(&mirror, &timestamp) ==
           INFS_STATUS_OK && timestamp.nanoseconds == 2,
       "clock device I/O can fail over");
    a.forced_time_status = INFS_STATUS_CORRUPT;
    ok(infs_storage_current_time(&mirror, &timestamp) ==
           INFS_STATUS_CORRUPT, "clock semantic failure is not masked");
    a.forced_time_status = INFS_STATUS_OK;

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

    /*
     * A readable mirror is not necessarily a valid mirror: a semantic error
     * on either replica cannot be hidden by bytes read from the other one.
     */
    a.fail_reads = 0;
    a.forced_read_status = INFS_STATUS_CORRUPT;
    ok(infs_storage_read(&mirror, 123, readback, sizeof(readback)) ==
           INFS_STATUS_CORRUPT,
       "primary corruption is not masked by secondary success");
    a.forced_read_status = INFS_STATUS_OK;
    b.forced_read_status = INFS_STATUS_CORRUPT;
    ok(infs_storage_read(&mirror, 123, readback, sizeof(readback)) ==
           INFS_STATUS_CORRUPT,
       "secondary corruption is not masked by primary success");
    b.forced_read_status = INFS_STATUS_NOT_SUPPORTED;
    ok(infs_storage_read(&mirror, 123, readback, sizeof(readback)) ==
           INFS_STATUS_NOT_SUPPORTED,
       "secondary policy failure is not masked by primary success");
    b.forced_read_status = INFS_STATUS_OK;

    /*
     * A failed durability barrier is not a successful commit. However a
     * transient non-I/O failure must remain retryable without re-opening the
     * entire mirror or silently dropping replication protection.
     */
    b.forced_flush_status = INFS_STATUS_INTERRUPTED;
    ok(infs_storage_flush(&mirror) == INFS_STATUS_INTERRUPTED,
       "interrupted replica flush propagates failure");
    b.forced_flush_status = INFS_STATUS_OK;
    ok(infs_storage_flush(&mirror) == INFS_STATUS_OK,
       "interrupted flush does not permanently quarantine replica");

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

    policy.protection_copies = 3u;
    static const char impossible[] = "three-copy-policy";
    ok(infs_storage_write_policy(
           &mirror, 3072, impossible, sizeof(impossible), &policy) ==
           INFS_STATUS_NOT_SUPPORTED,
       "impossible copy count rejected before write");
    ok(c_member.bytes[3072] == 0 && d_member.bytes[3072] == 0,
       "impossible copy count does not write a partial replica");
    memset(policy_readback, 0, sizeof(policy_readback));
    ok(infs_storage_read_policy(
           &mirror, 3072, policy_readback, sizeof(policy_readback),
           &policy) == INFS_STATUS_NOT_SUPPORTED,
       "impossible copy count rejected before read");

    policy.protection_copies = 2u;
    static const char two_copy[] = "two-copy-policy";
    ok(infs_storage_write_policy(
           &mirror, 3072, two_copy, sizeof(two_copy), &policy) ==
           INFS_STATUS_OK,
       "two-copy policy write");
    ok(!memcmp(c_member.bytes + 3072, two_copy, sizeof(two_copy)) &&
       !memcmp(d_member.bytes + 3072, two_copy, sizeof(two_copy)),
       "two-copy policy reaches both members");

    policy.encryption_domain = 7u;
    static const char unsupported_domain[] = "needs-domain-backend";
    ok(infs_storage_write_policy(
           &mirror, 4096, unsupported_domain,
           sizeof(unsupported_domain), &policy) ==
           INFS_STATUS_NOT_SUPPORTED,
       "mirror fails closed when members cannot provide encryption domain");
    policy.encryption_domain = 0u;
    static const char after_policy_error[] = "members-still-healthy";
    ok(infs_storage_write_policy(
           &mirror, 4608, after_policy_error,
           sizeof(after_policy_error), &policy) == INFS_STATUS_OK,
       "policy error does not quarantine healthy members");
    ok(!memcmp(c_member.bytes + 4608, after_policy_error,
               sizeof(after_policy_error)) &&
       !memcmp(d_member.bytes + 4608, after_policy_error,
               sizeof(after_policy_error)),
       "healthy members remain available after semantic policy failure");

    infs_storage_close(&mirror);
    ok(c_member.closed && d_member.closed,
       "policy-aware mirror closes every member");

#if !defined(_WIN32)
    /*
     * Exercise two overlapping writes to the same logical range, with a
     * deterministic interruption after the first writer's primary replica.
     * Both writers must report success and every replica must converge on
     * the final (second) writer; no stale split-brain data may remain.
     */
    struct race_state race;
    ok(pthread_mutex_init(&race.lock, NULL) == 0 &&
       pthread_cond_init(&race.changed, NULL) == 0,
       "initialise mirror race coordination");
    race.first_on_primary = 0;
    race.second_finished = 0;
    struct member race_a = {0}, race_b = {0};
    race_a.race = &race;
    struct infs_storage race_members[2] = {
        { .ops = &ops, .context = &race_a },
        { .ops = &ops, .context = &race_b },
    };
    memset(&mirror, 0, sizeof(mirror));
    ok(infs_storage_mirror_create(race_members, 2, &mirror) ==
           INFS_STATUS_OK,
       "create concurrently written mirror");
    struct concurrent_write first = {
        .storage = &mirror, .race = &race, .value = 'A',
        .status = INFS_STATUS_ERROR,
    };
    struct concurrent_write second = {
        .storage = &mirror, .race = &race, .value = 'B',
        .status = INFS_STATUS_ERROR,
    };
    pthread_t first_thread, second_thread;
    ok(pthread_create(&first_thread, NULL,
                      concurrent_write_thread, &first) == 0,
       "start first concurrent mirror writer");
    pthread_mutex_lock(&race.lock);
    while (!race.first_on_primary)
        pthread_cond_wait(&race.changed, &race.lock);
    pthread_mutex_unlock(&race.lock);
    ok(pthread_create(&second_thread, NULL,
                      concurrent_write_thread, &second) == 0,
       "start competing mirror writer");
    ok(pthread_join(first_thread, NULL) == 0 &&
       pthread_join(second_thread, NULL) == 0,
       "join concurrent mirror writers");
    ok(first.status == INFS_STATUS_OK &&
       second.status == INFS_STATUS_OK,
       "both mirror writes succeeded");
    for (size_t i = 6144; i < 6272; ++i)
        ok(race_a.bytes[i] == 'B' && race_b.bytes[i] == 'B',
           "concurrent mirror writes converge on both replicas");
    infs_storage_close(&mirror);
    ok(race_a.closed && race_b.closed,
       "concurrent mirror closes owned members");
    pthread_cond_destroy(&race.changed);
    pthread_mutex_destroy(&race.lock);
#endif

    puts("replicated storage backend: PASS");
    return 0;
}
