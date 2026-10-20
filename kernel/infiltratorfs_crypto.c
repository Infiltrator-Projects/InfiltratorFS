// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"

#include <crypto/hash.h>

static DEFINE_MUTEX(infilfs_crypto_lock);
static struct crypto_shash *infilfs_sha256_tfm;

static struct crypto_shash *infilfs_crypto_get_sha256(void)
{
    struct crypto_shash *tfm = READ_ONCE(infilfs_sha256_tfm);

    if (likely(tfm))
        return tfm;
    mutex_lock(&infilfs_crypto_lock);
    tfm = infilfs_sha256_tfm;
    if (!tfm) {
        tfm = crypto_alloc_shash("sha256", 0, 0);
        if (!IS_ERR(tfm))
            WRITE_ONCE(infilfs_sha256_tfm, tfm);
    }
    mutex_unlock(&infilfs_crypto_lock);
    return tfm;
}

int infilfs_crypto_sha256(const u8 *data, size_t len, u8 out[32])
{
    struct crypto_shash *tfm = infilfs_crypto_get_sha256();
    int ret;

    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    {
        SHASH_DESC_ON_STACK(desc, tfm);

        desc->tfm = tfm;
        ret = crypto_shash_digest(desc, data, len, out);
        shash_desc_zero(desc);
    }
    return ret;
}

int infilfs_crypto_sha256_zeropad(const u8 *data, size_t len,
                                  size_t padded_len, u8 out[32])
{
    static const u8 zero[256];
    struct crypto_shash *tfm = infilfs_crypto_get_sha256();
    int ret;

    if (len > padded_len)
        return -EINVAL;
    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    {
        SHASH_DESC_ON_STACK(desc, tfm);

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
        shash_desc_zero(desc);
    }
    return ret;
}

/*
 * Native integrity authority: prefer the kernel SHA-256 provider and preserve
 * a deterministic, allocation-free software stream if that provider fails.
 * Digest bytes must not depend on runtime crypto-provider availability.
 */
static inline u32 infilfs_software_sha256_ror32(u32 value, unsigned int shift)
{
    return (value >> shift) | (value << (32u - shift));
}

static void infilfs_software_sha256_transform(struct infilfs_software_sha256_ctx *ctx,
                                        const u8 data[64])
{
    static const u32 k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
        0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
        0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
        0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
        0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
        0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
        0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
        0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
        0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    u32 w[64];
    u32 a,b,c,d,e,f,g,h,t1,t2;
    unsigned int i;

    for (i = 0; i < 16; ++i) {
        const u8 *p = data + i * 4;
        w[i] = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
               ((u32)p[2] << 8) | (u32)p[3];
    }
    for (i = 16; i < 64; ++i) {
        u32 s0 = infilfs_software_sha256_ror32(w[i-15],7) ^
                 infilfs_software_sha256_ror32(w[i-15],18) ^ (w[i-15] >> 3);
        u32 s1 = infilfs_software_sha256_ror32(w[i-2],17) ^
                 infilfs_software_sha256_ror32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (i = 0; i < 64; ++i) {
        u32 s1 = infilfs_software_sha256_ror32(e,6) ^ infilfs_software_sha256_ror32(e,11) ^
                 infilfs_software_sha256_ror32(e,25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 s0 = infilfs_software_sha256_ror32(a,2) ^ infilfs_software_sha256_ror32(a,13) ^
                 infilfs_software_sha256_ror32(a,22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        t1 = h + s1 + ch + k[i] + w[i];
        t2 = s0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

void infilfs_software_sha256_init(struct infilfs_software_sha256_ctx *ctx)
{
    static const u32 initial[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };

    memcpy(ctx->state, initial, sizeof(initial));
    ctx->total = 0;
    ctx->used = 0;
}

void infilfs_software_sha256_update(struct infilfs_software_sha256_ctx *ctx,
                                     const u8 *data, size_t length)
{
    size_t take;

    ctx->total += length;
    while (length) {
        take = min_t(size_t, length, 64u - ctx->used);
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take;
        data += take;
        length -= take;
        if (ctx->used == 64) {
            infilfs_software_sha256_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

void infilfs_software_sha256_final(struct infilfs_software_sha256_ctx *ctx,
                                    u8 out[32])
{
    u64 bits = ctx->total * 8u;
    unsigned int i;

    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        memset(ctx->block + ctx->used, 0, 64 - ctx->used);
        infilfs_software_sha256_transform(ctx, ctx->block);
        ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, 56 - ctx->used);
    for (i = 0; i < 8; ++i)
        ctx->block[56 + i] = (u8)(bits >> (56 - i * 8));
    infilfs_software_sha256_transform(ctx, ctx->block);

    for (i = 0; i < 8; ++i) {
        out[i*4]     = (u8)(ctx->state[i] >> 24);
        out[i*4 + 1] = (u8)(ctx->state[i] >> 16);
        out[i*4 + 2] = (u8)(ctx->state[i] >> 8);
        out[i*4 + 3] = (u8)ctx->state[i];
    }
}

int infilfs_native_inline_digest(const u8 *data, size_t size, u8 out[32])
{
    struct infilfs_software_sha256_ctx ctx;
    u8 zero[64] = {0};
    size_t left;

    if (size > INFILFS_DISK_BLOCK_SIZE)
        return -EINVAL;
    if (!infilfs_crypto_sha256_zeropad(
            data, size, INFILFS_DISK_BLOCK_SIZE, out))
        return 0;
    infilfs_software_sha256_init(&ctx);
    if (size)
        infilfs_software_sha256_update(&ctx, data, size);
    left = INFILFS_DISK_BLOCK_SIZE - size;
    while (left) {
        size_t chunk = min_t(size_t, left, sizeof(zero));
        infilfs_software_sha256_update(&ctx, zero, chunk);
        left -= chunk;
    }
    infilfs_software_sha256_final(&ctx, out);
    return 0;
}


void infilfs_native_block_digest(
    const u8 data[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_data_checksum_disk *digest)
{
    struct infilfs_software_sha256_ctx ctx;
    if (!infilfs_crypto_sha256(data, INFILFS_DISK_BLOCK_SIZE, digest->bytes))
        return;
    infilfs_software_sha256_init(&ctx);
    infilfs_software_sha256_update(&ctx, data, INFILFS_DISK_BLOCK_SIZE);
    infilfs_software_sha256_final(&ctx, digest->bytes);
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
