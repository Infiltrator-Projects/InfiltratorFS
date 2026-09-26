// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format_volume.h"
#include "infilfs/storage_encrypted.h"
#include "infilfs/storage_mirror.h"
#include "infilfs/volume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMBER_BYTES (32u * 1024u * 1024u)

struct memory_member {
    uint8_t *bytes;
    size_t size;
    uint64_t rng;
    int fail_reads;
    int fail_writes;
};

static void fail(const char *message)
{
    fprintf(stderr, "protected-volume: %s\n", message);
    exit(1);
}

static void ok(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static infs_status rd(void *opaque, uint64_t offset, void *buffer, size_t size)
{
    struct memory_member *m = opaque;
    if (m->fail_reads)
        return INFS_STATUS_IO_ERROR;
    if (offset > m->size || size > m->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(buffer, m->bytes + (size_t)offset, size);
    return INFS_STATUS_OK;
}

static infs_status wr(void *opaque, uint64_t offset,
                      const void *buffer, size_t size)
{
    struct memory_member *m = opaque;
    if (m->fail_writes)
        return INFS_STATUS_IO_ERROR;
    if (offset > m->size || size > m->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(m->bytes + (size_t)offset, buffer, size);
    return INFS_STATUS_OK;
}

static infs_status fl(void *opaque)
{
    struct memory_member *m = opaque;
    return m->fail_writes ? INFS_STATUS_IO_ERROR : INFS_STATUS_OK;
}

static infs_status gs(void *opaque, uint64_t *bytes, int *is_device)
{
    struct memory_member *m = opaque;
    *bytes = m->size;
    *is_device = 1;
    return INFS_STATUS_OK;
}

static infs_status rnd(void *opaque, void *buffer, size_t size)
{
    struct memory_member *m = opaque;
    uint8_t *out = buffer;
    for (size_t i = 0; i < size; ++i) {
        m->rng ^= m->rng << 13;
        m->rng ^= m->rng >> 7;
        m->rng ^= m->rng << 17;
        out[i] = (uint8_t)m->rng;
    }
    return INFS_STATUS_OK;
}

static infs_status now(void *opaque, struct infs_timestamp *time)
{
    (void)opaque;
    time->seconds = 1786752000;
    time->nanoseconds = 0;
    return INFS_STATUS_OK;
}

static void cls(void *opaque)
{
    (void)opaque;
}

static const struct infs_storage_ops ops = {
    .read_at = rd,
    .write_at = wr,
    .flush = fl,
    .get_size = gs,
    .random_bytes = rnd,
    .current_time = now,
    .close = cls,
};

static struct infs_storage member_storage(struct memory_member *m)
{
    struct infs_storage storage = { .ops = &ops, .context = m };
    return storage;
}

static size_t policy_member_for_object(const uint8_t object_id[16])
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < 16u; ++i) {
        hash ^= object_id[i];
        hash *= UINT64_C(1099511628211);
    }
    return (size_t)(hash % 2u);
}

static infs_status build_authenticated_mirror(
    struct memory_member member[2], const char *passphrase, int format,
    struct infs_storage *out)
{
    struct infs_storage encrypted[2] = {{0}};
    for (size_t i = 0; i < 2u; ++i) {
        struct infs_storage backing = member_storage(&member[i]);
        infs_status status = format ?
            infs_storage_encrypted_format(
                &backing, passphrase, strlen(passphrase), 100000u,
                &encrypted[i]) :
            infs_storage_encrypted_open(
                &backing, passphrase, strlen(passphrase), &encrypted[i]);
        if (status != INFS_STATUS_OK) {
            for (size_t n = 0; n < i; ++n)
                infs_storage_close(&encrypted[n]);
            return status;
        }
    }
    infs_status status = infs_storage_mirror_create(encrypted, 2u, out);
    if (status != INFS_STATUS_OK) {
        for (size_t i = 0; i < 2u; ++i)
            infs_storage_close(&encrypted[i]);
    }
    return status;
}

int main(void)
{
    struct memory_member member[2] = {
        {calloc(1, MEMBER_BYTES), MEMBER_BYTES,
         UINT64_C(0x123456789abcdef0), 0, 0},
        {calloc(1, MEMBER_BYTES), MEMBER_BYTES,
         UINT64_C(0xfedcba9876543210), 0, 0},
    };
    ok(member[0].bytes && member[1].bytes, "allocate members");

    const char *passphrase = "authenticated mirrored InfiltratorFS";
    struct infs_storage protected_storage = {0};
    infs_status status = build_authenticated_mirror(
        member, passphrase, 1, &protected_storage);
    if (status == INFS_STATUS_NOT_SUPPORTED) {
        free(member[0].bytes);
        free(member[1].bytes);
        puts("protected-volume: SKIP (crypto provider unavailable)");
        return 0;
    }
    ok(status == INFS_STATUS_OK, "create encrypted members and mirror");

    ok(infs_format_storage(&protected_storage, "protected") ==
           INFS_STATUS_OK,
       "format filesystem through authenticated mirror");

    struct infs_volume volume;
    ok(infs_volume_open_storage(
           &volume, &protected_storage, 1) == INFS_STATUS_OK,
       "open protected filesystem");
    struct infs_create_options options = { .posix_permissions = 0644 };
    ok(infs_create_file(&volume, "/protected.txt", &options) ==
           INFS_STATUS_OK,
       "create protected file");

    uint8_t payload[16384];
    for (size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = (uint8_t)(i * 29u + 7u);
    ok(infs_write_file(
           &volume, "/protected.txt", payload, sizeof(payload), 0) ==
           (int64_t)sizeof(payload),
       "write protected file");

    struct infs_create_options policy_options = {
        .portable_flags = INFS_ATTR_WITH_STORAGE_POLICY(0, 1u, 7u),
        .posix_permissions = 0600u,
    };
    ok(infs_create_file(&volume, "/policy.txt", &policy_options) ==
           INFS_STATUS_OK,
       "create one-copy encryption-domain file");
    uint8_t policy_payload[12288];
    for (size_t i = 0; i < sizeof(policy_payload); ++i)
        policy_payload[i] = (uint8_t)(i * 43u + 19u);
    ok(infs_write_file(
           &volume, "/policy.txt", policy_payload,
           sizeof(policy_payload), 0) == (int64_t)sizeof(policy_payload),
       "write one-copy encryption-domain file");

    static const char stream_payload[] = "policy-named-stream";
    ok(infs_named_stream_set(
           &volume, "/policy.txt", "user.policy-proof",
           stream_payload, sizeof(stream_payload)) == INFS_STATUS_OK,
       "write named stream under owner storage policy");

    struct infs_attributes policy_attributes;
    ok(infs_get_attributes(
           &volume, "/policy.txt", &policy_attributes) == INFS_STATUS_OK &&
       INFS_ATTR_PROTECTION_COPIES(policy_attributes.portable_flags) == 1u &&
       INFS_ATTR_ENCRYPTION_DOMAIN(policy_attributes.portable_flags) == 7u,
       "persist per-object storage policy");
    size_t selected_policy_member =
        policy_member_for_object(policy_attributes.object_id);

    ok(infs_snapshot_create(&volume, "protected-baseline") ==
           INFS_STATUS_OK,
       "snapshot protected filesystem");
    ok(infs_volume_sync(&volume) == INFS_STATUS_OK,
       "sync protected filesystem");
    infs_volume_close(&volume);

    /*
     * Reopen both authenticated members, then fail all reads from the first
     * backing.  The mirror must receive an I/O/authentication failure from that
     * member and satisfy the filesystem read from the second complete replica.
     */
    struct infs_storage encrypted[2] = {{0}};
    for (size_t i = 0; i < 2u; ++i) {
        struct infs_storage backing = member_storage(&member[i]);
        ok(infs_storage_encrypted_open(
               &backing, passphrase, strlen(passphrase), &encrypted[i]) ==
               INFS_STATUS_OK,
           "reopen encrypted member");
    }
    /*
     * Named streams inherit the owner's copies/domain class but retain their
     * own object identity, so a one-copy stream may intentionally select a
     * different mirror member from its owner. Verify the inherited protected
     * stream while all members are healthy; member-failure placement is proven
     * independently below for the owner file.
     */
    char stream_readback[sizeof(stream_payload)] = {0};
    ok(infs_named_stream_read(
           &volume, "/policy.txt", "user.policy-proof",
           stream_readback, sizeof(stream_readback), 0) ==
           (int64_t)sizeof(stream_readback) &&
       !memcmp(stream_readback, stream_payload, sizeof(stream_payload)),
       "named stream roundtrip under inherited storage policy");

    size_t unselected_policy_member = selected_policy_member ^ 1u;
    member[unselected_policy_member].fail_reads = 1;
    ok(infs_storage_mirror_create(encrypted, 2u, &protected_storage) ==
           INFS_STATUS_OK,
       "rebuild authenticated mirror");
    ok(infs_volume_open_storage(
           &volume, &protected_storage, 0) == INFS_STATUS_OK,
       "open through surviving authenticated replica");

    uint8_t readback[sizeof(payload)];
    memset(readback, 0, sizeof(readback));
    ok(infs_read_file(
           &volume, "/protected.txt", readback, sizeof(readback), 0) ==
           (int64_t)sizeof(readback),
       "default all-copy file reads through surviving replica");
    ok(!memcmp(readback, payload, sizeof(payload)),
       "default replica data matches");

    uint8_t policy_readback[sizeof(policy_payload)];
    memset(policy_readback, 0, sizeof(policy_readback));
    ok(infs_read_file(
           &volume, "/policy.txt", policy_readback,
           sizeof(policy_readback), 0) ==
           (int64_t)sizeof(policy_readback) &&
       !memcmp(policy_readback, policy_payload, sizeof(policy_payload)),
       "one-copy file reads when unselected member is unavailable");

    struct infs_scrub_report report;
    ok(infs_scrub(&volume, &report) == INFS_STATUS_OK &&
           report.metadata_errors == 0 && report.checksum_errors == 0,
       "scrub protected filesystem");
    infs_volume_close(&volume);
    member[unselected_policy_member].fail_reads = 0;

    for (size_t i = 0; i < 2u; ++i) {
        struct infs_storage backing = member_storage(&member[i]);
        ok(infs_storage_encrypted_open(
               &backing, passphrase, strlen(passphrase), &encrypted[i]) ==
               INFS_STATUS_OK,
           "reopen encrypted member for selected-copy failure");
    }
    member[selected_policy_member].fail_reads = 1;
    ok(infs_storage_mirror_create(encrypted, 2u, &protected_storage) ==
           INFS_STATUS_OK,
       "rebuild mirror for selected-copy failure");
    ok(infs_volume_open_storage(
           &volume, &protected_storage, 0) == INFS_STATUS_OK,
       "metadata opens from alternate complete replica");
    memset(policy_readback, 0, sizeof(policy_readback));
    ok(infs_read_file(
           &volume, "/policy.txt", policy_readback,
           sizeof(policy_readback), 0) == INFS_STATUS_IO_ERROR,
       "one-copy file fails closed when selected member is unavailable");
    infs_volume_close(&volume);
    member[selected_policy_member].fail_reads = 0;

    free(member[0].bytes);
    free(member[1].bytes);
    puts("protected-volume: PASS");
    return 0;
}
