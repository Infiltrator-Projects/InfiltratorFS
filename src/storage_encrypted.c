// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/storage_encrypted.h"
#include "infilfs/endian.h"
#include "storage_lock.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(INFS_HAVE_OPENSSL_AEAD)
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

#define ENC_HEADER_BYTES UINT64_C(4096)
#define ENC_LOGICAL_BLOCK UINT64_C(4096)
#define ENC_NONCE_BYTES 12u
#define ENC_TAG_BYTES 16u
#define ENC_KEY_BYTES 32u
#define ENC_SALT_BYTES 16u
#define ENC_RECORD_BYTES (ENC_LOGICAL_BLOCK + ENC_NONCE_BYTES + ENC_TAG_BYTES)
#define ENC_VERSION UINT32_C(1)
#define ENC_MAGIC "INFSEV01"
#define ENC_LOCK_STRIPES 64u

#define H_OFF_MAGIC 0u
#define H_OFF_VERSION 8u
#define H_OFF_HEADER_BYTES 12u
#define H_OFF_BLOCK_BYTES 16u
#define H_OFF_RECORD_BYTES 20u
#define H_OFF_LOGICAL_BLOCKS 24u
#define H_OFF_KDF_ITERATIONS 32u
#define H_OFF_RESERVED 36u
#define H_OFF_SALT 40u
#define H_OFF_WRAP_NONCE 56u
#define H_OFF_WRAPPED_KEY 68u
#define H_OFF_WRAP_TAG 100u
#define H_WRAP_AAD_BYTES H_OFF_WRAPPED_KEY

struct encrypted_context {
    struct infs_storage backing;
    uint64_t logical_blocks;
    uint64_t logical_size;
    uint8_t salt[ENC_SALT_BYTES];
    uint8_t volume_key[ENC_KEY_BYTES];
    struct infs_storage_lock block_locks[ENC_LOCK_STRIPES];
};

static void secure_zero(void *memory, size_t size)
{
#if defined(INFS_HAVE_OPENSSL_AEAD)
    OPENSSL_cleanse(memory, size);
#else
    volatile unsigned char *p = memory;
    while (size--)
        *p++ = 0;
#endif
}

static int range_valid(uint64_t logical_size, uint64_t offset, size_t size)
{
    return offset <= logical_size &&
        (uint64_t)size <= logical_size - offset;
}

static int physical_record_offset(uint64_t logical_block, uint64_t *offset)
{
    if (!offset || logical_block > (UINT64_MAX - ENC_HEADER_BYTES) /
                                      ENC_RECORD_BYTES)
        return 0;
    *offset = ENC_HEADER_BYTES + logical_block * ENC_RECORD_BYTES;
    return 1;
}

#if defined(INFS_HAVE_OPENSSL_AEAD)
static infs_status derive_kek(const void *passphrase, size_t passphrase_size,
                              const uint8_t salt[ENC_SALT_BYTES],
                              uint32_t iterations,
                              uint8_t kek[ENC_KEY_BYTES])
{
    if (!passphrase || passphrase_size == 0 ||
        passphrase_size > INT_MAX || iterations < 100000u)
        return INFS_STATUS_INVALID_ARGUMENT;
    return PKCS5_PBKDF2_HMAC(
        (const char *)passphrase, (int)passphrase_size,
        salt, ENC_SALT_BYTES, (int)iterations,
        EVP_sha256(), ENC_KEY_BYTES, kek) == 1 ?
        INFS_STATUS_OK : INFS_STATUS_ERROR;
}

static infs_status aead_encrypt(const uint8_t key[ENC_KEY_BYTES],
                                const uint8_t nonce[ENC_NONCE_BYTES],
                                const void *aad, size_t aad_size,
                                const uint8_t *plain, size_t plain_size,
                                uint8_t *cipher, uint8_t tag[ENC_TAG_BYTES])
{
    if (aad_size > INT_MAX || plain_size > INT_MAX)
        return INFS_STATUS_OVERFLOW;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return INFS_STATUS_NO_MEMORY;
    int n = 0, total = 0;
    infs_status status = INFS_STATUS_ERROR;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                            ENC_NONCE_BYTES, NULL) != 1 ||
        EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
        goto out;
    if (aad_size &&
        EVP_EncryptUpdate(ctx, NULL, &n, aad, (int)aad_size) != 1)
        goto out;
    if (plain_size &&
        EVP_EncryptUpdate(ctx, cipher, &n, plain, (int)plain_size) != 1)
        goto out;
    total = n;
    if (EVP_EncryptFinal_ex(ctx, cipher + total, &n) != 1)
        goto out;
    total += n;
    if ((size_t)total != plain_size ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                            ENC_TAG_BYTES, tag) != 1)
        goto out;
    status = INFS_STATUS_OK;
out:
    EVP_CIPHER_CTX_free(ctx);
    return status;
}

static infs_status aead_decrypt(const uint8_t key[ENC_KEY_BYTES],
                                const uint8_t nonce[ENC_NONCE_BYTES],
                                const void *aad, size_t aad_size,
                                const uint8_t *cipher, size_t cipher_size,
                                const uint8_t tag[ENC_TAG_BYTES],
                                uint8_t *plain)
{
    if (aad_size > INT_MAX || cipher_size > INT_MAX)
        return INFS_STATUS_OVERFLOW;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return INFS_STATUS_NO_MEMORY;
    int n = 0, total = 0;
    infs_status status = INFS_STATUS_CORRUPT;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                            ENC_NONCE_BYTES, NULL) != 1 ||
        EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
        goto out;
    if (aad_size &&
        EVP_DecryptUpdate(ctx, NULL, &n, aad, (int)aad_size) != 1)
        goto out;
    if (cipher_size &&
        EVP_DecryptUpdate(ctx, plain, &n, cipher, (int)cipher_size) != 1)
        goto out;
    total = n;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                            ENC_TAG_BYTES, (void *)tag) != 1)
        goto out;
    if (EVP_DecryptFinal_ex(ctx, plain + total, &n) != 1)
        goto out;
    total += n;
    status = (size_t)total == cipher_size ?
        INFS_STATUS_OK : INFS_STATUS_CORRUPT;
out:
    EVP_CIPHER_CTX_free(ctx);
    if (status != INFS_STATUS_OK)
        secure_zero(plain, cipher_size);
    return status;
}
#endif

static infs_status domain_key(
    const struct encrypted_context *ctx, uint8_t domain,
    uint8_t key[ENC_KEY_BYTES])
{
#if !defined(INFS_HAVE_OPENSSL_AEAD)
    (void)ctx;
    (void)domain;
    (void)key;
    return INFS_STATUS_NOT_SUPPORTED;
#else
    if (!ctx || !key)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (domain == 0u) {
        memcpy(key, ctx->volume_key, ENC_KEY_BYTES);
        return INFS_STATUS_OK;
    }
    static const uint8_t label[] = "InfiltratorFS encryption domain v1";
    uint8_t material[sizeof(label)];
    memcpy(material, label, sizeof(label) - 1u);
    material[sizeof(label) - 1u] = domain;
    unsigned int out_size = 0;
    if (!HMAC(EVP_sha256(), ctx->volume_key, ENC_KEY_BYTES,
              material, sizeof(material), key, &out_size) ||
        out_size != ENC_KEY_BYTES) {
        secure_zero(key, ENC_KEY_BYTES);
        return INFS_STATUS_ERROR;
    }
    return INFS_STATUS_OK;
#endif
}

static size_t block_aad(
    const struct encrypted_context *ctx, uint64_t logical_block,
    uint8_t domain, uint8_t aad[25])
{
    memcpy(aad, ctx->salt, ENC_SALT_BYTES);
    infs_store_le64(aad + ENC_SALT_BYTES, logical_block);
    if (domain == 0u)
        return 24u;
    aad[24] = domain;
    return 25u;
}

static infs_status encrypted_read_block(struct encrypted_context *ctx,
                                        uint64_t logical_block,
                                        uint8_t domain,
                                        uint8_t plain[4096])
{
#if !defined(INFS_HAVE_OPENSSL_AEAD)
    (void)ctx;
    (void)logical_block;
    (void)domain;
    (void)plain;
    return INFS_STATUS_NOT_SUPPORTED;
#else
    uint64_t physical = 0;
    if (logical_block >= ctx->logical_blocks ||
        !physical_record_offset(logical_block, &physical))
        return INFS_STATUS_IO_ERROR;

    uint8_t *record = malloc((size_t)ENC_RECORD_BYTES);
    if (!record)
        return INFS_STATUS_NO_MEMORY;
    infs_status status = infs_storage_read(
        &ctx->backing, physical, record, (size_t)ENC_RECORD_BYTES);
    if (status == INFS_STATUS_OK) {
        uint8_t aad[25];
        uint8_t key[ENC_KEY_BYTES] = {0};
        size_t aad_size = block_aad(ctx, logical_block, domain, aad);
        status = domain_key(ctx, domain, key);
        const uint8_t *nonce = record;
        const uint8_t *tag = record + ENC_NONCE_BYTES;
        const uint8_t *cipher = tag + ENC_TAG_BYTES;
        /*
         * Every logical block, including logical zeroes, carries a valid GCM
         * tag.  An all-zero physical record is therefore corruption rather
         * than a sparse-zero escape hatch; otherwise an offline attacker could
         * erase ciphertext and bypass authentication by manufacturing zeroes.
         */
        if (status == INFS_STATUS_OK)
            status = aead_decrypt(
                key, nonce, aad, aad_size,
                cipher, 4096, tag, plain);
        secure_zero(key, sizeof(key));
    }
    secure_zero(record, (size_t)ENC_RECORD_BYTES);
    free(record);
    return status;
#endif
}

static infs_status encrypted_write_block(struct encrypted_context *ctx,
                                         uint64_t logical_block,
                                         uint8_t domain,
                                         const uint8_t plain[4096])
{
#if !defined(INFS_HAVE_OPENSSL_AEAD)
    (void)ctx;
    (void)logical_block;
    (void)domain;
    (void)plain;
    return INFS_STATUS_NOT_SUPPORTED;
#else
    uint64_t physical = 0;
    if (logical_block >= ctx->logical_blocks ||
        !physical_record_offset(logical_block, &physical))
        return INFS_STATUS_IO_ERROR;

    uint8_t *record = malloc((size_t)ENC_RECORD_BYTES);
    if (!record)
        return INFS_STATUS_NO_MEMORY;
    uint8_t *nonce = record;
    uint8_t *tag = record + ENC_NONCE_BYTES;
    uint8_t *cipher = tag + ENC_TAG_BYTES;
    infs_status status = infs_storage_random(
        &ctx->backing, nonce, ENC_NONCE_BYTES);
    if (status == INFS_STATUS_OK) {
        uint8_t aad[25];
        uint8_t key[ENC_KEY_BYTES] = {0};
        size_t aad_size = block_aad(ctx, logical_block, domain, aad);
        status = domain_key(ctx, domain, key);
        if (status == INFS_STATUS_OK)
            status = aead_encrypt(
                key, nonce, aad, aad_size,
                plain, 4096, cipher, tag);
        secure_zero(key, sizeof(key));
    }
    if (status == INFS_STATUS_OK)
        status = infs_storage_write(
            &ctx->backing, physical, record, (size_t)ENC_RECORD_BYTES);
    secure_zero(record, (size_t)ENC_RECORD_BYTES);
    free(record);
    return status;
#endif
}

static infs_status encrypted_read_policy(
    void *opaque, uint64_t offset, void *buffer, size_t size,
    const struct infs_storage_io_policy *policy)
{
    struct encrypted_context *ctx = opaque;
    uint8_t domain = policy ? policy->encryption_domain : 0u;
    if (!range_valid(ctx->logical_size, offset, size))
        return INFS_STATUS_IO_ERROR;
    size_t done = 0;
    uint8_t block[4096];
    while (done < size) {
        uint64_t position = offset + done;
        uint64_t logical = position / ENC_LOGICAL_BLOCK;
        size_t within = (size_t)(position % ENC_LOGICAL_BLOCK);
        size_t chunk = 4096u - within;
        if (chunk > size - done)
            chunk = size - done;

        struct infs_storage_lock *lock =
            &ctx->block_locks[logical % ENC_LOCK_STRIPES];
        infs_storage_lock_acquire(lock);
        infs_status status = encrypted_read_block(
            ctx, logical, domain, block);
        infs_storage_lock_release(lock);
        if (status != INFS_STATUS_OK) {
            secure_zero(block, sizeof(block));
            return status;
        }
        memcpy((uint8_t *)buffer + done, block + within, chunk);
        done += chunk;
    }
    secure_zero(block, sizeof(block));
    return INFS_STATUS_OK;
}

static infs_status encrypted_read(void *opaque, uint64_t offset,
                                  void *buffer, size_t size)
{
    return encrypted_read_policy(opaque, offset, buffer, size, NULL);
}

static infs_status encrypted_write_policy(
    void *opaque, uint64_t offset, const void *buffer, size_t size,
    const struct infs_storage_io_policy *policy)
{
    struct encrypted_context *ctx = opaque;
    uint8_t domain = policy ? policy->encryption_domain : 0u;
    if (!range_valid(ctx->logical_size, offset, size))
        return INFS_STATUS_IO_ERROR;
    size_t done = 0;
    uint8_t block[4096];
    while (done < size) {
        uint64_t position = offset + done;
        uint64_t logical = position / ENC_LOGICAL_BLOCK;
        size_t within = (size_t)(position % ENC_LOGICAL_BLOCK);
        size_t chunk = 4096u - within;
        if (chunk > size - done)
            chunk = size - done;

        struct infs_storage_lock *lock =
            &ctx->block_locks[logical % ENC_LOCK_STRIPES];
        infs_storage_lock_acquire(lock);
        infs_status status = INFS_STATUS_OK;
        if (within != 0 || chunk != 4096u)
            status = encrypted_read_block(ctx, logical, domain, block);
        else
            memset(block, 0, sizeof(block));
        if (status == INFS_STATUS_OK) {
            memcpy(block + within, (const uint8_t *)buffer + done, chunk);
            status = encrypted_write_block(ctx, logical, domain, block);
        }
        infs_storage_lock_release(lock);
        if (status != INFS_STATUS_OK) {
            secure_zero(block, sizeof(block));
            return status;
        }
        done += chunk;
    }
    secure_zero(block, sizeof(block));
    return INFS_STATUS_OK;
}

static infs_status encrypted_write(void *opaque, uint64_t offset,
                                   const void *buffer, size_t size)
{
    return encrypted_write_policy(opaque, offset, buffer, size, NULL);
}

static infs_status encrypted_flush(void *opaque)
{
    struct encrypted_context *ctx = opaque;

    /*
     * A durability barrier must not pass an in-flight encrypted block update.
     * Acquire every stripe in a fixed order, flush the backing store, then
     * release in reverse order.
     */
    for (size_t i = 0; i < ENC_LOCK_STRIPES; ++i)
        infs_storage_lock_acquire(&ctx->block_locks[i]);
    infs_status status = infs_storage_flush(&ctx->backing);
    for (size_t i = ENC_LOCK_STRIPES; i > 0; --i)
        infs_storage_lock_release(&ctx->block_locks[i - 1u]);
    return status;
}

static infs_status encrypted_size(void *opaque, uint64_t *size_bytes,
                                  int *is_device)
{
    struct encrypted_context *ctx = opaque;
    *size_bytes = ctx->logical_size;
    *is_device = 0;
    return INFS_STATUS_OK;
}

static infs_status encrypted_random(void *opaque, void *buffer, size_t size)
{
    return infs_storage_random(
        &((struct encrypted_context *)opaque)->backing, buffer, size);
}

static infs_status encrypted_time(void *opaque, struct infs_timestamp *time)
{
    return infs_storage_current_time(
        &((struct encrypted_context *)opaque)->backing, time);
}

static void encrypted_close(void *opaque)
{
    struct encrypted_context *ctx = opaque;
    if (!ctx)
        return;
    secure_zero(ctx->volume_key, sizeof(ctx->volume_key));
    secure_zero(ctx->salt, sizeof(ctx->salt));
    infs_storage_close(&ctx->backing);
    for (size_t i = 0; i < ENC_LOCK_STRIPES; ++i)
        infs_storage_lock_destroy(&ctx->block_locks[i]);
    free(ctx);
}

static const struct infs_storage_ops encrypted_ops = {
    .read_at = encrypted_read,
    .write_at = encrypted_write,
    .read_at_policy = encrypted_read_policy,
    .write_at_policy = encrypted_write_policy,
    .flush = encrypted_flush,
    .get_size = encrypted_size,
    .random_bytes = encrypted_random,
    .current_time = encrypted_time,
    .close = encrypted_close,
};

static infs_status encrypted_take_backing(
    struct infs_storage *backing, uint64_t logical_blocks,
    const uint8_t salt[ENC_SALT_BYTES],
    const uint8_t key[ENC_KEY_BYTES],
    struct infs_storage *out)
{
    struct encrypted_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return INFS_STATUS_NO_MEMORY;

    size_t initialized = 0;
    for (; initialized < ENC_LOCK_STRIPES; ++initialized) {
        if (!infs_storage_lock_init(&ctx->block_locks[initialized])) {
            while (initialized > 0)
                infs_storage_lock_destroy(
                    &ctx->block_locks[--initialized]);
            free(ctx);
            return INFS_STATUS_ERROR;
        }
    }

    ctx->backing = *backing;
    backing->ops = NULL;
    backing->context = NULL;
    ctx->logical_blocks = logical_blocks;
    ctx->logical_size = logical_blocks * ENC_LOGICAL_BLOCK;
    memcpy(ctx->salt, salt, ENC_SALT_BYTES);
    memcpy(ctx->volume_key, key, ENC_KEY_BYTES);
    out->ops = &encrypted_ops;
    out->context = ctx;
    return INFS_STATUS_OK;
}

infs_status infs_storage_encrypted_format(
    struct infs_storage *backing,
    const void *passphrase, size_t passphrase_size,
    uint32_t kdf_iterations,
    struct infs_storage *out)
{
#if !defined(INFS_HAVE_OPENSSL_AEAD)
    (void)backing;
    (void)passphrase;
    (void)passphrase_size;
    (void)kdf_iterations;
    (void)out;
    return INFS_STATUS_NOT_SUPPORTED;
#else
    if (!infs_storage_valid(backing) || !passphrase || !passphrase_size ||
        !out)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!kdf_iterations)
        kdf_iterations = INFS_ENCRYPTED_STORAGE_DEFAULT_KDF_ITERATIONS;
    if (kdf_iterations < 100000u)
        return INFS_STATUS_INVALID_ARGUMENT;

    uint64_t physical_size = 0;
    int ignored_device = 0;
    infs_status status = infs_storage_get_size(
        backing, &physical_size, &ignored_device);
    if (status != INFS_STATUS_OK)
        return status;
    if (physical_size <= ENC_HEADER_BYTES + ENC_RECORD_BYTES)
        return INFS_STATUS_NO_SPACE;
    uint64_t logical_blocks =
        (physical_size - ENC_HEADER_BYTES) / ENC_RECORD_BYTES;
    if (!logical_blocks ||
        logical_blocks > UINT64_MAX / ENC_LOGICAL_BLOCK)
        return INFS_STATUS_OVERFLOW;

    uint8_t header[4096] = {0};
    uint8_t kek[ENC_KEY_BYTES] = {0};
    uint8_t key[ENC_KEY_BYTES] = {0};
    memcpy(header + H_OFF_MAGIC, ENC_MAGIC, 8);
    infs_store_le32(header + H_OFF_VERSION, ENC_VERSION);
    infs_store_le32(header + H_OFF_HEADER_BYTES, (uint32_t)ENC_HEADER_BYTES);
    infs_store_le32(header + H_OFF_BLOCK_BYTES, (uint32_t)ENC_LOGICAL_BLOCK);
    infs_store_le32(header + H_OFF_RECORD_BYTES, (uint32_t)ENC_RECORD_BYTES);
    infs_store_le64(header + H_OFF_LOGICAL_BLOCKS, logical_blocks);
    infs_store_le32(header + H_OFF_KDF_ITERATIONS, kdf_iterations);
    infs_store_le32(header + H_OFF_RESERVED, 0);

    status = infs_storage_random(backing, header + H_OFF_SALT, ENC_SALT_BYTES);
    if (status == INFS_STATUS_OK)
        status = infs_storage_random(
            backing, header + H_OFF_WRAP_NONCE, ENC_NONCE_BYTES);
    if (status == INFS_STATUS_OK)
        status = infs_storage_random(backing, key, sizeof(key));
    if (status == INFS_STATUS_OK)
        status = derive_kek(
            passphrase, passphrase_size, header + H_OFF_SALT,
            kdf_iterations, kek);
    if (status == INFS_STATUS_OK)
        status = aead_encrypt(
            kek, header + H_OFF_WRAP_NONCE,
            header, H_WRAP_AAD_BYTES,
            key, sizeof(key),
            header + H_OFF_WRAPPED_KEY,
            header + H_OFF_WRAP_TAG);
    if (status == INFS_STATUS_OK)
        status = infs_storage_write(backing, 0, header, sizeof(header));

    /*
     * There is deliberately no unauthenticated sparse-record representation.
     * Initialise every logical block as authenticated zeroes so erasing any
     * ciphertext record is detected by GCM on the next read.
     */
    if (status == INFS_STATUS_OK) {
        struct encrypted_context initializing;
        uint8_t zero[4096] = {0};
        memset(&initializing, 0, sizeof(initializing));
        initializing.backing = *backing;
        initializing.logical_blocks = logical_blocks;
        initializing.logical_size = logical_blocks * ENC_LOGICAL_BLOCK;
        memcpy(initializing.salt, header + H_OFF_SALT, ENC_SALT_BYTES);
        memcpy(initializing.volume_key, key, ENC_KEY_BYTES);
        for (uint64_t block = 0; block < logical_blocks; ++block) {
            status = encrypted_write_block(&initializing, block, 0u, zero);
            if (status != INFS_STATUS_OK)
                break;
        }
        secure_zero(initializing.volume_key,
                    sizeof(initializing.volume_key));
        secure_zero(initializing.salt, sizeof(initializing.salt));
        secure_zero(zero, sizeof(zero));
    }
    if (status == INFS_STATUS_OK)
        status = infs_storage_flush(backing);
    if (status == INFS_STATUS_OK)
        status = encrypted_take_backing(
            backing, logical_blocks, header + H_OFF_SALT, key, out);

    secure_zero(kek, sizeof(kek));
    secure_zero(key, sizeof(key));
    secure_zero(header, sizeof(header));
    return status;
#endif
}

infs_status infs_storage_encrypted_open(
    struct infs_storage *backing,
    const void *passphrase, size_t passphrase_size,
    struct infs_storage *out)
{
#if !defined(INFS_HAVE_OPENSSL_AEAD)
    (void)backing;
    (void)passphrase;
    (void)passphrase_size;
    (void)out;
    return INFS_STATUS_NOT_SUPPORTED;
#else
    if (!infs_storage_valid(backing) || !passphrase || !passphrase_size ||
        !out)
        return INFS_STATUS_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    uint8_t header[4096];
    infs_status status = infs_storage_read(
        backing, 0, header, sizeof(header));
    if (status != INFS_STATUS_OK)
        return status;
    if (memcmp(header + H_OFF_MAGIC, ENC_MAGIC, 8) != 0 ||
        infs_load_le32(header + H_OFF_VERSION) != ENC_VERSION ||
        infs_load_le32(header + H_OFF_HEADER_BYTES) != ENC_HEADER_BYTES ||
        infs_load_le32(header + H_OFF_BLOCK_BYTES) != ENC_LOGICAL_BLOCK ||
        infs_load_le32(header + H_OFF_RECORD_BYTES) != ENC_RECORD_BYTES ||
        infs_load_le32(header + H_OFF_RESERVED) != 0) {
        secure_zero(header, sizeof(header));
        return INFS_STATUS_CORRUPT;
    }

    uint64_t logical_blocks =
        infs_load_le64(header + H_OFF_LOGICAL_BLOCKS);
    uint32_t iterations =
        infs_load_le32(header + H_OFF_KDF_ITERATIONS);
    uint64_t physical_size = 0;
    int ignored_device = 0;
    status = infs_storage_get_size(
        backing, &physical_size, &ignored_device);
    if (status != INFS_STATUS_OK)
        goto out;
    if (!logical_blocks || iterations < 100000u ||
        logical_blocks > (UINT64_MAX - ENC_HEADER_BYTES) / ENC_RECORD_BYTES ||
        ENC_HEADER_BYTES + logical_blocks * ENC_RECORD_BYTES > physical_size) {
        status = INFS_STATUS_CORRUPT;
        goto out;
    }

    uint8_t kek[ENC_KEY_BYTES] = {0};
    uint8_t key[ENC_KEY_BYTES] = {0};
    status = derive_kek(
        passphrase, passphrase_size, header + H_OFF_SALT,
        iterations, kek);
    if (status == INFS_STATUS_OK)
        status = aead_decrypt(
            kek, header + H_OFF_WRAP_NONCE,
            header, H_WRAP_AAD_BYTES,
            header + H_OFF_WRAPPED_KEY, ENC_KEY_BYTES,
            header + H_OFF_WRAP_TAG, key);
    if (status == INFS_STATUS_OK)
        status = encrypted_take_backing(
            backing, logical_blocks, header + H_OFF_SALT, key, out);
    secure_zero(kek, sizeof(kek));
    secure_zero(key, sizeof(key));
out:
    secure_zero(header, sizeof(header));
    return status;
#endif
}
