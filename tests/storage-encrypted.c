// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/storage_encrypted.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <pthread.h>
#include <time.h>
#endif

#define BACKING_BYTES (4096u + 32u * 4124u)

struct memory_backing {
    uint8_t *bytes;
    size_t size;
    uint64_t rng;
#if !defined(_WIN32)
    pthread_mutex_t io_lock;
    pthread_mutex_t race_lock;
    pthread_cond_t race_cond;
    int race_mode;
    unsigned race_reads;
#endif
};

static void fail(const char *message)
{
    fprintf(stderr, "storage-encrypted: %s\n", message);
    exit(1);
}

static void ok(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static infs_status rd(void *opaque, uint64_t offset, void *buffer, size_t size)
{
    struct memory_backing *m = opaque;
    if (offset > m->size || size > m->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
#if !defined(_WIN32)
    pthread_mutex_lock(&m->io_lock);
#endif
    memcpy(buffer, m->bytes + (size_t)offset, size);
#if !defined(_WIN32)
    pthread_mutex_unlock(&m->io_lock);

    /*
     * Deterministically expose the old partial-write race.  Two unsynchronised
     * encrypted writers both reach this point after decrypting the same old
     * record.  The repaired wrapper serialises by logical-block stripe, so the
     * first writer times out here and the second cannot read until that write
     * has completed.
     */
    pthread_mutex_lock(&m->race_lock);
    if (m->race_mode && offset >= 4096u) {
        ++m->race_reads;
        if (m->race_reads >= 2u) {
            m->race_mode = 0;
            pthread_cond_broadcast(&m->race_cond);
        } else {
            struct timespec deadline;
            timespec_get(&deadline, TIME_UTC);
            deadline.tv_nsec += 100000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                ++deadline.tv_sec;
                deadline.tv_nsec -= 1000000000L;
            }
            while (m->race_mode)
                if (pthread_cond_timedwait(
                        &m->race_cond, &m->race_lock, &deadline) != 0)
                    break;
        }
    }
    pthread_mutex_unlock(&m->race_lock);
#endif
    return INFS_STATUS_OK;
}

static infs_status wr(void *opaque, uint64_t offset,
                      const void *buffer, size_t size)
{
    struct memory_backing *m = opaque;
    if (offset > m->size || size > m->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
#if !defined(_WIN32)
    pthread_mutex_lock(&m->io_lock);
#endif
    memcpy(m->bytes + (size_t)offset, buffer, size);
#if !defined(_WIN32)
    pthread_mutex_unlock(&m->io_lock);
#endif
    return INFS_STATUS_OK;
}

static infs_status fl(void *opaque)
{
    (void)opaque;
    return INFS_STATUS_OK;
}

static infs_status sz(void *opaque, uint64_t *bytes, int *is_device)
{
    struct memory_backing *m = opaque;
    *bytes = m->size;
    *is_device = 0;
    return INFS_STATUS_OK;
}

static infs_status rnd(void *opaque, void *buffer, size_t size)
{
    struct memory_backing *m = opaque;
    uint8_t *out = buffer;
#if !defined(_WIN32)
    pthread_mutex_lock(&m->io_lock);
#endif
    for (size_t i = 0; i < size; ++i) {
        m->rng ^= m->rng << 13;
        m->rng ^= m->rng >> 7;
        m->rng ^= m->rng << 17;
        out[i] = (uint8_t)m->rng;
    }
#if !defined(_WIN32)
    pthread_mutex_unlock(&m->io_lock);
#endif
    return INFS_STATUS_OK;
}

static infs_status now(void *opaque, struct infs_timestamp *time)
{
    (void)opaque;
    time->seconds = 42;
    time->nanoseconds = 7;
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
    .get_size = sz,
    .random_bytes = rnd,
    .current_time = now,
    .close = cls,
};

static struct infs_storage storage(struct memory_backing *m)
{
    struct infs_storage s = { .ops = &ops, .context = m };
    return s;
}

#if !defined(_WIN32)
struct partial_write {
    struct infs_storage *storage;
    uint64_t offset;
    uint8_t value;
    infs_status status;
};

static void *partial_write_thread(void *opaque)
{
    struct partial_write *write = opaque;
    uint8_t bytes[2048];
    memset(bytes, write->value, sizeof(bytes));
    write->status = infs_storage_write(
        write->storage, write->offset, bytes, sizeof(bytes));
    return NULL;
}
#endif

int main(void)
{
    struct memory_backing memory = {
        .bytes = calloc(1, BACKING_BYTES),
        .size = BACKING_BYTES,
        .rng = UINT64_C(0x243f6a8885a308d3),
    };
    ok(memory.bytes != NULL, "allocate backing");
#if !defined(_WIN32)
    ok(pthread_mutex_init(&memory.io_lock, NULL) == 0,
       "initialise backing I/O lock");
    ok(pthread_mutex_init(&memory.race_lock, NULL) == 0,
       "initialise race lock");
    ok(pthread_cond_init(&memory.race_cond, NULL) == 0,
       "initialise race condition");
#endif

    static const char passphrase[] = "correct horse battery staple";

    /*
     * A backend that has no policy callback must not silently pretend it
     * honored requested replication/encryption semantics.
     */
    struct infs_storage plain_probe = storage(&memory);
    struct infs_storage_io_policy unsupported_policy = {0};
    unsupported_policy.object_id[0] = 1u;
    unsupported_policy.protection_copies = 1u;
    unsupported_policy.encryption_domain = 7u;
    uint8_t probe_byte = 0x5a;
    ok(infs_storage_write_policy(
           &plain_probe, 0, &probe_byte, 1u, &unsupported_policy) ==
           INFS_STATUS_NOT_SUPPORTED,
       "plain backend rejects unsupported write policy");
    ok(infs_storage_read_policy(
           &plain_probe, 0, &probe_byte, 1u, &unsupported_policy) ==
           INFS_STATUS_NOT_SUPPORTED,
       "plain backend rejects unsupported read policy");

    struct infs_storage backing = storage(&memory);
    struct infs_storage encrypted = {0};
    infs_status status = infs_storage_encrypted_format(
        &backing, passphrase, sizeof(passphrase) - 1u, 100000u, &encrypted);
    if (status == INFS_STATUS_NOT_SUPPORTED) {
#if !defined(_WIN32)
        pthread_cond_destroy(&memory.race_cond);
        pthread_mutex_destroy(&memory.race_lock);
        pthread_mutex_destroy(&memory.io_lock);
#endif
        free(memory.bytes);
        puts("authenticated encrypted storage: SKIP (crypto provider unavailable)");
        return 0;
    }
    ok(status == INFS_STATUS_OK, "format encrypted storage");
    ok(!backing.ops, "format transfers backing ownership");

    /*
     * OpenSSL's PBKDF2 API accepts a signed iteration count. The public
     * format API must reject an out-of-range count before modifying storage
     * or taking ownership of the caller's handle.
     */
    struct infs_storage invalid_iterations_backing = storage(&memory);
    struct infs_storage invalid_iterations_out = {0};
    ok(infs_storage_encrypted_format(
           &invalid_iterations_backing, passphrase,
           sizeof(passphrase) - 1u, UINT32_MAX,
           &invalid_iterations_out) == INFS_STATUS_INVALID_ARGUMENT,
       "reject KDF iterations outside provider range");
    ok(invalid_iterations_backing.ops == &ops &&
       invalid_iterations_backing.context == &memory &&
       !invalid_iterations_out.ops,
       "invalid iteration count preserves source ownership");

    /*
     * Construction must reject handle aliasing before clearing the output.
     * Both failure paths leave the caller's live backing handle intact.
     */
    struct infs_storage aliased_backing = storage(&memory);
    ok(infs_storage_encrypted_format(
           &aliased_backing, passphrase, sizeof(passphrase) - 1u,
           100000u, &aliased_backing) == INFS_STATUS_INVALID_ARGUMENT,
       "encrypted format rejects aliased output");
    ok(aliased_backing.ops == &ops && aliased_backing.context == &memory,
       "failed encrypted format preserves backing ownership");
    ok(infs_storage_encrypted_open(
           &aliased_backing, passphrase, sizeof(passphrase) - 1u,
           &aliased_backing) == INFS_STATUS_INVALID_ARGUMENT,
       "encrypted open rejects aliased output");
    ok(aliased_backing.ops == &ops && aliased_backing.context == &memory,
       "failed encrypted open preserves backing ownership");

    uint64_t logical_size = 0;
    int is_device = 1;
    ok(infs_storage_get_size(&encrypted, &logical_size, &is_device) ==
           INFS_STATUS_OK &&
       logical_size == 32u * 4096u && !is_device,
       "encrypted logical geometry");

    uint8_t payload[6000];
    for (size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = (uint8_t)(i * 37u + 11u);
    ok(infs_storage_write(&encrypted, 3000, payload, sizeof(payload)) ==
           INFS_STATUS_OK,
       "unaligned encrypted write");
    ok(infs_storage_flush(&encrypted) == INFS_STATUS_OK,
       "encrypted durability barrier");

    uint8_t readback[sizeof(payload)];
    memset(readback, 0, sizeof(readback));
    ok(infs_storage_read(&encrypted, 3000, readback, sizeof(readback)) ==
           INFS_STATUS_OK &&
       !memcmp(readback, payload, sizeof(payload)),
       "encrypted round trip");

    /*
     * A nonzero per-object encryption domain derives a distinct AEAD key and
     * authenticates the domain ID. The same physical record must therefore be
     * readable only through the domain that wrote it.
     */
    struct infs_storage_io_policy domain_policy = {0};
    for (size_t i = 0; i < sizeof(domain_policy.object_id); ++i)
        domain_policy.object_id[i] = (uint8_t)(0xa0u + i);
    domain_policy.encryption_domain = 7u;
    const uint64_t domain_offset = 20u * 4096u;
    uint8_t domain_payload[4096];
    for (size_t i = 0; i < sizeof(domain_payload); ++i)
        domain_payload[i] = (uint8_t)(i * 13u + 5u);
    ok(infs_storage_write_policy(
           &encrypted, domain_offset, domain_payload,
           sizeof(domain_payload), &domain_policy) == INFS_STATUS_OK,
       "write encryption domain 7");
    uint8_t domain_readback[4096] = {0};
    ok(infs_storage_read_policy(
           &encrypted, domain_offset, domain_readback,
           sizeof(domain_readback), &domain_policy) == INFS_STATUS_OK &&
       !memcmp(domain_readback, domain_payload, sizeof(domain_payload)),
       "read encryption domain 7");
    struct infs_storage_io_policy wrong_domain = domain_policy;
    wrong_domain.encryption_domain = 3u;
    ok(infs_storage_read_policy(
           &encrypted, domain_offset, domain_readback,
           sizeof(domain_readback), &wrong_domain) == INFS_STATUS_CORRUPT,
       "wrong encryption domain is authentication failure");

#if !defined(_WIN32)
    /*
     * Two disjoint partial updates to one authenticated logical block must be
     * atomic with respect to one another.  Without stripe locking, both
     * threads decrypt the same old block and one half-update is lost.
     */
    const uint64_t race_block = 12u * 4096u;
    uint8_t zero_block[4096] = {0};
    ok(infs_storage_write(&encrypted, race_block,
                          zero_block, sizeof(zero_block)) == INFS_STATUS_OK,
       "prepare concurrent partial-write block");
    pthread_mutex_lock(&memory.race_lock);
    memory.race_reads = 0;
    memory.race_mode = 1;
    pthread_mutex_unlock(&memory.race_lock);

    struct partial_write first = {
        .storage = &encrypted, .offset = race_block,
        .value = 0x3c, .status = INFS_STATUS_ERROR,
    };
    struct partial_write second = {
        .storage = &encrypted, .offset = race_block + 2048u,
        .value = 0xa7, .status = INFS_STATUS_ERROR,
    };
    pthread_t first_thread, second_thread;
    ok(pthread_create(&first_thread, NULL,
                      partial_write_thread, &first) == 0,
       "start first partial writer");
    ok(pthread_create(&second_thread, NULL,
                      partial_write_thread, &second) == 0,
       "start second partial writer");
    ok(pthread_join(first_thread, NULL) == 0 &&
       pthread_join(second_thread, NULL) == 0,
       "join partial writers");
    ok(first.status == INFS_STATUS_OK && second.status == INFS_STATUS_OK,
       "concurrent partial writes succeed");

    uint8_t concurrent_block[4096];
    ok(infs_storage_read(&encrypted, race_block,
                         concurrent_block, sizeof(concurrent_block)) ==
           INFS_STATUS_OK,
       "read concurrent partial-write block");
    for (size_t i = 0; i < 2048u; ++i)
        ok(concurrent_block[i] == 0x3c,
           "first concurrent partial write preserved");
    for (size_t i = 2048u; i < sizeof(concurrent_block); ++i)
        ok(concurrent_block[i] == 0xa7,
           "second concurrent partial write preserved");
#endif
    infs_storage_close(&encrypted);

    /*
     * The envelope is untrusted input. A CRC-independent, syntactically
     * recognisable header with an impossible PBKDF2 cost must be rejected
     * as corrupt without attempting a signed-overflow KDF call.
     */
    uint8_t saved_iterations[4];
    memcpy(saved_iterations, memory.bytes + 32u, sizeof(saved_iterations));
    memset(memory.bytes + 32u, 0xff, sizeof(saved_iterations));
    backing = storage(&memory);
    ok(infs_storage_encrypted_open(
           &backing, passphrase, sizeof(passphrase) - 1u, &encrypted) ==
           INFS_STATUS_CORRUPT,
       "reject impossible persisted KDF iteration count");
    ok(backing.ops == &ops && !encrypted.ops,
       "malformed encrypted header preserves backing ownership");
    memcpy(memory.bytes + 32u, saved_iterations, sizeof(saved_iterations));

    backing = storage(&memory);
    static const char wrong[] = "wrong password";
    ok(infs_storage_encrypted_open(
           &backing, wrong, sizeof(wrong) - 1u, &encrypted) ==
           INFS_STATUS_CORRUPT,
       "wrong passphrase rejected");
    ok(backing.ops != NULL, "failed open does not steal backing");

    backing = storage(&memory);
    ok(infs_storage_encrypted_open(
           &backing, passphrase, sizeof(passphrase) - 1u, &encrypted) ==
           INFS_STATUS_OK,
       "reopen encrypted storage");
    memset(readback, 0, sizeof(readback));
    ok(infs_storage_read(&encrypted, 3000, readback, sizeof(readback)) ==
           INFS_STATUS_OK &&
       !memcmp(readback, payload, sizeof(payload)),
       "encrypted data survives reopen");
    infs_storage_close(&encrypted);

    /*
     * Ciphertext starts after the 4 KiB envelope. Corrupt a non-zero record;
     * GCM must fail before unauthenticated bytes can reach the filesystem.
     */
    memory.bytes[4096u + 12u + 16u + 100u] ^= 0x80u;
    backing = storage(&memory);
    ok(infs_storage_encrypted_open(
           &backing, passphrase, sizeof(passphrase) - 1u, &encrypted) ==
           INFS_STATUS_OK,
       "reopen for tamper test");
    uint8_t block[4096];
    ok(infs_storage_read(&encrypted, 0, block, sizeof(block)) ==
           INFS_STATUS_CORRUPT,
       "ciphertext tamper rejected");
    infs_storage_close(&encrypted);

    /*
     * Erasing a ciphertext record must also fail authentication.  There is no
     * unauthenticated all-zero representation for sparse/free logical blocks.
     */
    const size_t erased_record =
        4096u + 5u * (4096u + 12u + 16u);
    memset(memory.bytes + erased_record, 0, 4096u + 12u + 16u);
    backing = storage(&memory);
    ok(infs_storage_encrypted_open(
           &backing, passphrase, sizeof(passphrase) - 1u, &encrypted) ==
           INFS_STATUS_OK,
       "reopen for erased-record tamper test");
    ok(infs_storage_read(&encrypted, 5u * 4096u, block, sizeof(block)) ==
           INFS_STATUS_CORRUPT,
       "erased ciphertext record rejected");
    infs_storage_close(&encrypted);

    /*
     * The smallest valid encrypted backing contains precisely one envelope
     * and one complete authenticated data record. An undersized backing must
     * fail without transferring ownership; equality must admit its one block.
     */
    memory.size = 4096u + 4124u - 1u;
    backing = storage(&memory);
    ok(infs_storage_encrypted_format(
           &backing, passphrase, sizeof(passphrase) - 1u,
           100000u, &encrypted) == INFS_STATUS_NO_SPACE &&
       backing.ops == &ops && !encrypted.ops,
       "reject a backing one byte short of an authenticated record");

    ++memory.size;
    ok(infs_storage_encrypted_format(
           &backing, passphrase, sizeof(passphrase) - 1u,
           100000u, &encrypted) == INFS_STATUS_OK &&
       !backing.ops,
       "accept an exact one-record encrypted backing");
    logical_size = 0;
    ok(infs_storage_get_size(&encrypted, &logical_size, &is_device) ==
           INFS_STATUS_OK && logical_size == 4096u,
       "exact one-record backing exposes one logical block");
    uint8_t minimum_payload[4096];
    uint8_t minimum_readback[4096] = {0};
    memset(minimum_payload, 0x7bu, sizeof(minimum_payload));
    ok(infs_storage_write(&encrypted, 0, minimum_payload,
                          sizeof(minimum_payload)) == INFS_STATUS_OK &&
       infs_storage_flush(&encrypted) == INFS_STATUS_OK,
       "write and durably flush the minimum encrypted volume");
    infs_storage_close(&encrypted);
    backing = storage(&memory);
    ok(infs_storage_encrypted_open(
           &backing, passphrase, sizeof(passphrase) - 1u,
           &encrypted) == INFS_STATUS_OK,
       "reopen exact one-record encrypted backing");
    ok(infs_storage_read(&encrypted, 0, minimum_readback,
                         sizeof(minimum_readback)) == INFS_STATUS_OK &&
       !memcmp(minimum_readback, minimum_payload, sizeof(minimum_payload)),
       "minimum encrypted volume survives reopen and authentication");
    infs_storage_close(&encrypted);

#if !defined(_WIN32)
    pthread_cond_destroy(&memory.race_cond);
    pthread_mutex_destroy(&memory.race_lock);
    pthread_mutex_destroy(&memory.io_lock);
#endif
    free(memory.bytes);
    puts("authenticated encrypted storage: PASS");
    return 0;
}
