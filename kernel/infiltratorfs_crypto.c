// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"
#include <crypto/hash.h>

static DEFINE_MUTEX(infilfs_crypto_lock);
static struct crypto_shash *infilfs_sha256_tfm;

static struct crypto_shash *infilfs_crypto_get_sha256(void)
{
    struct crypto_shash *tfm;

    mutex_lock(&infilfs_crypto_lock);
    tfm = infilfs_sha256_tfm;
    if (!tfm) {
        tfm = crypto_alloc_shash("sha256", 0, 0);
        if (!IS_ERR(tfm))
            infilfs_sha256_tfm = tfm;
    }
    mutex_unlock(&infilfs_crypto_lock);
    return tfm;
}

int infilfs_crypto_sha256(const u8 *data, size_t len, u8 out[32])
{
    struct crypto_shash *tfm = infilfs_crypto_get_sha256();
    struct shash_desc *desc;
    size_t bytes;
    int ret;

    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    bytes = sizeof(*desc) + crypto_shash_descsize(tfm);
    desc = kmalloc(bytes, GFP_NOFS);
    if (!desc)
        return -ENOMEM;
    desc->tfm = tfm;
    ret = crypto_shash_digest(desc, data, len, out);
    kfree_sensitive(desc);
    return ret;
}

int infilfs_crypto_sha256_zeropad(const u8 *data, size_t len,
                                  size_t padded_len, u8 out[32])
{
    static const u8 zero[256];
    struct crypto_shash *tfm = infilfs_crypto_get_sha256();
    struct shash_desc *desc;
    size_t bytes;
    int ret;

    if (len > padded_len)
        return -EINVAL;
    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    bytes = sizeof(*desc) + crypto_shash_descsize(tfm);
    desc = kmalloc(bytes, GFP_NOFS);
    if (!desc)
        return -ENOMEM;
    desc->tfm = tfm;
    ret = crypto_shash_init(desc);
    if (!ret && len)
        ret = crypto_shash_update(desc, data, len);
    while (!ret && len < padded_len) {
        size_t chunk = min_t(size_t, sizeof(zero), padded_len - len);
        ret = crypto_shash_update(desc, zero, chunk);
        len += chunk;
    }
    if (!ret)
        ret = crypto_shash_final(desc, out);
    kfree_sensitive(desc);
    return ret;
}

void infilfs_crypto_exit(void)
{
    struct crypto_shash *tfm;

    mutex_lock(&infilfs_crypto_lock);
    tfm = infilfs_sha256_tfm;
    infilfs_sha256_tfm = NULL;
    mutex_unlock(&infilfs_crypto_lock);
    if (tfm)
        crypto_free_shash(tfm);
}
