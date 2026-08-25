// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Initial native Linux write path for Format 0.12.
 *
 * This deliberately starts with the smallest operation set that can be made
 * crash-safe without duplicating the whole portable core in kernel space:
 * regular-file creation, directory creation, truncate and writes while a file
 * remains inline (<= INFILFS_INLINE_DATA_MAX).  Every mutation is published as
 * a synchronous copy-on-write transaction with a new bitmap image and the
 * same three-checkpoint commit protocol as the portable core.  Volumes with
 * retained snapshots are rejected for writable native mounts until snapshot
 * ownership is taught to this adapter.
 */

#define INFILFS_RW_BITMAP_MAX_BYTES (64UL * 1024UL * 1024UL)
#define INFILFS_RW_FREE_RANGES_INITIAL 32u

struct infilfs_rw_free_range {
    u64 start;
    u64 count;
};

struct infilfs_rw_tx {
    struct super_block *sb;
    struct infilfs_sb_info *sbi;
    struct infilfs_superblock_disk next_sb;
    u8 *bitmap;
    size_t bitmap_bytes;
    u64 generation;
    u64 free_blocks;
    struct infilfs_rw_free_range *deferred;
    size_t deferred_count;
    size_t deferred_capacity;
};

struct infilfs_rw_sha256_ctx {
    u32 state[8];
    u64 total;
    u8 block[64];
    u32 used;
};

static bool infilfs_rw_bitmap_get(const u8 *bitmap, u64 block)
{
    return (bitmap[block >> 3] & (u8)(1u << (block & 7u))) != 0;
}

static void infilfs_rw_bitmap_set(u8 *bitmap, u64 block, bool used)
{
    u8 mask = (u8)(1u << (block & 7u));

    if (used)
        bitmap[block >> 3] |= mask;
    else
        bitmap[block >> 3] &= (u8)~mask;
}

static int infilfs_rw_write_block(struct super_block *sb, u64 block,
                                  const void *data)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct buffer_head *bh;
    int ret;

    if (!sbi || block >= sbi->device_blocks)
        return -EIO;

    bh = sb_getblk(sb, (sector_t)block);
    if (!bh)
        return -EIO;

    lock_buffer(bh);
    memcpy(bh->b_data, data, INFILFS_DISK_BLOCK_SIZE);
    set_buffer_uptodate(bh);
    mark_buffer_dirty(bh);
    unlock_buffer(bh);
    ret = sync_dirty_buffer(bh);
    brelse(bh);
    return ret;
}

static u64 infilfs_rw_crc64(const u8 *data, size_t length)
{
    const u64 poly = 0x42F0E1EBA9EA3693ULL;
    u64 crc = 0;
    size_t i;
    unsigned int bit;

    for (i = 0; i < length; ++i) {
        crc ^= (u64)data[i] << 56;
        for (bit = 0; bit < 8; ++bit)
            crc = (crc & (1ULL << 63)) ? (crc << 1) ^ poly : crc << 1;
    }
    return crc;
}

static void infilfs_rw_store_crc(u8 block[INFILFS_DISK_BLOCK_SIZE],
                                 size_t checksum_offset,
                                 size_t checksum_size)
{
    __le64 stored;
    u64 crc;

    memset(block + checksum_offset, 0, checksum_size);
    crc = infilfs_rw_crc64(block, INFILFS_DISK_BLOCK_SIZE);
    stored = cpu_to_le64(crc);
    memcpy(block + checksum_offset, &stored, sizeof(stored));
}

static int infilfs_rw_finalize_object(u8 block[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_object_header_disk *header =
        (struct infilfs_object_header_disk *)block;
    u32 payload = le32_to_cpu(header->payload_size);

    if (payload > INFILFS_DISK_BLOCK_SIZE - sizeof(*header))
        return -EINVAL;
    memset(block + sizeof(*header) + payload, 0,
           INFILFS_DISK_BLOCK_SIZE - sizeof(*header) - payload);
    infilfs_rw_store_crc(block,
        offsetof(struct infilfs_object_header_disk, checksum),
        sizeof(header->checksum));
    return 0;
}

static int infilfs_rw_finalize_page(u8 block[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_metadata_page_disk *page =
        (struct infilfs_metadata_page_disk *)block;
    u32 bytes = le32_to_cpu(page->bytes_used);

    if (bytes > INFILFS_METADATA_PAGE_DATA_SIZE)
        return -EINVAL;
    memset(block + sizeof(*page) + bytes, 0,
           INFILFS_DISK_BLOCK_SIZE - sizeof(*page) - bytes);
    infilfs_rw_store_crc(block,
        offsetof(struct infilfs_metadata_page_disk, checksum),
        sizeof(page->checksum));
    return 0;
}

static int infilfs_rw_encode_superblock(
    const struct infilfs_superblock_disk *disk,
    u8 block[INFILFS_DISK_BLOCK_SIZE])
{
    struct infilfs_superblock_disk *out;

    memset(block, 0, INFILFS_DISK_BLOCK_SIZE);
    memcpy(block, disk, sizeof(*disk));
    out = (struct infilfs_superblock_disk *)block;
    infilfs_rw_store_crc(block,
        offsetof(struct infilfs_superblock_disk, checksum),
        sizeof(out->checksum));
    return 0;
}

static inline u32 infilfs_rw_ror32(u32 value, unsigned int shift)
{
    return (value >> shift) | (value << (32u - shift));
}

static void infilfs_rw_sha256_transform(struct infilfs_rw_sha256_ctx *ctx,
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
        u32 s0 = infilfs_rw_ror32(w[i-15],7) ^
                 infilfs_rw_ror32(w[i-15],18) ^ (w[i-15] >> 3);
        u32 s1 = infilfs_rw_ror32(w[i-2],17) ^
                 infilfs_rw_ror32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (i = 0; i < 64; ++i) {
        u32 s1 = infilfs_rw_ror32(e,6) ^ infilfs_rw_ror32(e,11) ^
                 infilfs_rw_ror32(e,25);
        u32 ch = (e & f) ^ ((~e) & g);
        u32 s0 = infilfs_rw_ror32(a,2) ^ infilfs_rw_ror32(a,13) ^
                 infilfs_rw_ror32(a,22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        t1 = h + s1 + ch + k[i] + w[i];
        t2 = s0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void infilfs_rw_sha256_init(struct infilfs_rw_sha256_ctx *ctx)
{
    static const u32 initial[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };

    memcpy(ctx->state, initial, sizeof(initial));
    ctx->total = 0;
    ctx->used = 0;
}

static void infilfs_rw_sha256_update(struct infilfs_rw_sha256_ctx *ctx,
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
            infilfs_rw_sha256_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void infilfs_rw_sha256_final(struct infilfs_rw_sha256_ctx *ctx,
                                    u8 out[32])
{
    u64 bits = ctx->total * 8u;
    unsigned int i;

    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        memset(ctx->block + ctx->used, 0, 64 - ctx->used);
        infilfs_rw_sha256_transform(ctx, ctx->block);
        ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, 56 - ctx->used);
    for (i = 0; i < 8; ++i)
        ctx->block[56 + i] = (u8)(bits >> (56 - i * 8));
    infilfs_rw_sha256_transform(ctx, ctx->block);

    for (i = 0; i < 8; ++i) {
        out[i*4]     = (u8)(ctx->state[i] >> 24);
        out[i*4 + 1] = (u8)(ctx->state[i] >> 16);
        out[i*4 + 2] = (u8)(ctx->state[i] >> 8);
        out[i*4 + 3] = (u8)ctx->state[i];
    }
}

static int infilfs_rw_inline_digest(const u8 *data, size_t size, u8 out[32])
{
    struct infilfs_rw_sha256_ctx ctx;
    u8 zero[64] = {0};
    size_t left;

    if (size > INFILFS_INLINE_DATA_MAX)
        return -EINVAL;
    infilfs_rw_sha256_init(&ctx);
    if (size)
        infilfs_rw_sha256_update(&ctx, data, size);
    left = INFILFS_DISK_BLOCK_SIZE - size;
    while (left) {
        size_t chunk = min_t(size_t, left, sizeof(zero));
        infilfs_rw_sha256_update(&ctx, zero, chunk);
        left -= chunk;
    }
    infilfs_rw_sha256_final(&ctx, out);
    return 0;
}

static bool infilfs_rw_utf8_valid(const u8 *s, size_t len)
{
    size_t i = 0;

    while (i < len) {
        u8 c = s[i++];
        u32 cp;
        unsigned int need;

        if (c < 0x80)
            continue;
        if (c >= 0xc2 && c <= 0xdf) {
            cp = c & 0x1f; need = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            cp = c & 0x0f; need = 2;
        } else if (c >= 0xf0 && c <= 0xf4) {
            cp = c & 0x07; need = 3;
        } else {
            return false;
        }
        if (i + need > len)
            return false;
        while (need--) {
            u8 d = s[i++];
            if ((d & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (d & 0x3f);
        }
        if (cp < 0x80 || (cp < 0x800 && c >= 0xe0) ||
            (cp < 0x10000 && c >= 0xf0) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
            return false;
    }
    return true;
}

static int infilfs_rw_snapshot_count(struct super_block *sb, u32 *count_out)
{
    static const u8 snapshot_id[16] = {
        'I','N','F','S','-','S','N','A','P','-','C','A','T','-','0','1'
    };
    struct infilfs_snapshot_catalog_payload_disk *payload;
    struct infilfs_object_header_disk *header;
    u64 block;
    u16 type;
    u8 *object;
    int ret;

    *count_out = 0;
    ret = infilfs_index_lookup(sb, snapshot_id, &block, &type);
    if (ret == -ENOENT)
        return 0;
    if (ret)
        return ret;
    if (type != INFILFS_OBJECT_SNAPSHOT_CATALOG)
        return -EFSCORRUPTED;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(sb, block, INFILFS_OBJECT_SNAPSHOT_CATALOG,
                              snapshot_id, object);
    if (!ret) {
        header = (struct infilfs_object_header_disk *)object;
        if (le32_to_cpu(header->payload_size) < sizeof(*payload)) {
            ret = -EFSCORRUPTED;
        } else {
            payload = (struct infilfs_snapshot_catalog_payload_disk *)(header + 1);
            if (le32_to_cpu(payload->reserved) != 0)
                ret = -EFSCORRUPTED;
            else
                *count_out = le32_to_cpu(payload->snapshot_count);
        }
    }
    kfree(object);
    return ret;
}

static int infilfs_rw_mount_init(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u64 bitmap_start = le64_to_cpu(sbi->disk.bitmap_start_block);
    u64 bitmap_blocks = le64_to_cpu(sbi->disk.bitmap_block_count);
    u64 total = sbi->device_blocks;
    u64 free_count = 0;
    u32 snapshots = 0;
    size_t bytes;
    u64 i;
    int ret;

    if (sb_rdonly(sb))
        return 0;
    if (!(le64_to_cpu(sbi->disk.incompat_flags) & INFILFS_INCOMPAT_INLINE_DATA))
        return -EOPNOTSUPP;

    ret = infilfs_rw_snapshot_count(sb, &snapshots);
    if (ret)
        return ret;
    if (snapshots) {
        pr_warn("InfiltratorFS: native writable mount rejected: %u retained snapshot(s) require snapshot-aware ownership\n",
                snapshots);
        return -EOPNOTSUPP;
    }

    if (!bitmap_blocks || bitmap_blocks > SIZE_MAX / INFILFS_DISK_BLOCK_SIZE)
        return -EFSCORRUPTED;
    bytes = (size_t)bitmap_blocks * INFILFS_DISK_BLOCK_SIZE;
    if (bytes > INFILFS_RW_BITMAP_MAX_BYTES) {
        pr_warn("InfiltratorFS: native writable bitmap %zu bytes exceeds initial RW limit\n",
                bytes);
        return -EFBIG;
    }
    if (bitmap_start >= total || bitmap_blocks > total - bitmap_start)
        return -EFSCORRUPTED;

    sbi->bitmap = kvmalloc(bytes, GFP_KERNEL);
    if (!sbi->bitmap)
        return -ENOMEM;
    sbi->bitmap_bytes = bytes;
    for (i = 0; i < bitmap_blocks; ++i) {
        ret = infilfs_read_block(sb, bitmap_start + i,
                                 sbi->bitmap + i * INFILFS_DISK_BLOCK_SIZE);
        if (ret)
            goto fail;
    }
    for (i = 0; i < total; ++i)
        if (!infilfs_rw_bitmap_get(sbi->bitmap, i))
            ++free_count;
    if (free_count != le64_to_cpu(sbi->disk.free_blocks)) {
        ret = -EFSCORRUPTED;
        goto fail;
    }
    for (i = total; i < (u64)bytes * 8u; ++i) {
        if (!infilfs_rw_bitmap_get(sbi->bitmap, i)) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
    }
    sbi->rw_enabled = true;
    return 0;
fail:
    kvfree(sbi->bitmap);
    sbi->bitmap = NULL;
    sbi->bitmap_bytes = 0;
    return ret;
}

static void infilfs_rw_mount_destroy(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);

    if (!sbi)
        return;
    kvfree(sbi->bitmap);
    sbi->bitmap = NULL;
    sbi->bitmap_bytes = 0;
    sbi->rw_enabled = false;
}

static int infilfs_rw_tx_begin(struct super_block *sb, struct infilfs_rw_tx *tx)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u64 generation;

    memset(tx, 0, sizeof(*tx));
    if (sb_rdonly(sb) || !sbi->rw_enabled)
        return -EROFS;
    if (sbi->write_poisoned)
        return -EIO;
    generation = le64_to_cpu(sbi->disk.generation);
    if (generation == U64_MAX)
        return -EOVERFLOW;

    tx->bitmap = kvmalloc(sbi->bitmap_bytes, GFP_NOFS);
    if (!tx->bitmap)
        return -ENOMEM;
    memcpy(tx->bitmap, sbi->bitmap, sbi->bitmap_bytes);
    tx->deferred_capacity = INFILFS_RW_FREE_RANGES_INITIAL;
    tx->deferred = kcalloc(tx->deferred_capacity, sizeof(*tx->deferred),
                           GFP_NOFS);
    if (!tx->deferred) {
        kvfree(tx->bitmap);
        tx->bitmap = NULL;
        return -ENOMEM;
    }
    tx->sb = sb;
    tx->sbi = sbi;
    tx->bitmap_bytes = sbi->bitmap_bytes;
    tx->next_sb = sbi->disk;
    tx->generation = generation + 1u;
    tx->free_blocks = le64_to_cpu(sbi->disk.free_blocks);
    return 0;
}

static void infilfs_rw_tx_abort(struct infilfs_rw_tx *tx)
{
    kvfree(tx->bitmap);
    kfree(tx->deferred);
    memset(tx, 0, sizeof(*tx));
}

static int infilfs_rw_tx_defer_free(struct infilfs_rw_tx *tx,
                                    u64 start, u64 count)
{
    struct infilfs_rw_free_range *grown;

    if (!count || start >= tx->sbi->device_blocks ||
        count > tx->sbi->device_blocks - start)
        return count ? -EINVAL : 0;
    if (tx->deferred_count) {
        struct infilfs_rw_free_range *last =
            &tx->deferred[tx->deferred_count - 1];
        if (last->start + last->count == start) {
            last->count += count;
            return 0;
        }
    }
    if (tx->deferred_count == tx->deferred_capacity) {
        size_t next = tx->deferred_capacity * 2u;
        if (next < tx->deferred_capacity)
            return -EOVERFLOW;
        grown = krealloc(tx->deferred, next * sizeof(*grown), GFP_NOFS);
        if (!grown)
            return -ENOMEM;
        tx->deferred = grown;
        tx->deferred_capacity = next;
    }
    tx->deferred[tx->deferred_count].start = start;
    tx->deferred[tx->deferred_count].count = count;
    tx->deferred_count++;
    return 0;
}

static int infilfs_rw_tx_alloc(struct infilfs_rw_tx *tx, u64 count,
                               u64 *start_out)
{
    u64 total = tx->sbi->device_blocks;
    u64 b = total;
    u64 run = 0;
    u64 start = 0;
    u64 i;

    if (!count || count > tx->free_blocks)
        return -ENOSPC;
    while (b > 1) {
        u64 candidate = --b;
        if (!infilfs_rw_bitmap_get(tx->bitmap, candidate)) {
            run++;
            if (run == count) {
                start = candidate;
                break;
            }
        } else {
            run = 0;
        }
    }
    if (run != count)
        return -ENOSPC;
    for (i = 0; i < count; ++i)
        infilfs_rw_bitmap_set(tx->bitmap, start + i, true);
    tx->free_blocks -= count;
    *start_out = start;
    return 0;
}

static int infilfs_rw_tx_apply_deferred(struct infilfs_rw_tx *tx)
{
    size_t r;
    u64 i;

    for (r = 0; r < tx->deferred_count; ++r) {
        u64 start = tx->deferred[r].start;
        u64 count = tx->deferred[r].count;
        for (i = 0; i < count; ++i) {
            u64 block = start + i;
            unsigned int c;
            bool checkpoint = false;

            for (c = 0; c < INFILFS_CHECKPOINT_COUNT; ++c)
                if (block == le64_to_cpu(tx->next_sb.checkpoint_block[c]))
                    checkpoint = true;
            if (checkpoint || !infilfs_rw_bitmap_get(tx->bitmap, block))
                continue;
            infilfs_rw_bitmap_set(tx->bitmap, block, false);
            tx->free_blocks++;
        }
    }
    return 0;
}

static int infilfs_rw_tx_commit(struct infilfs_rw_tx *tx)
{
    struct infilfs_sb_info *sbi = tx->sbi;
    u64 bitmap_blocks = le64_to_cpu(tx->next_sb.bitmap_block_count);
    u64 old_bitmap = le64_to_cpu(tx->next_sb.bitmap_start_block);
    u64 new_bitmap;
    u8 *sb_block;
    unsigned int first;
    unsigned int n;
    int ret;
    int replica_error = 0;

    ret = infilfs_rw_tx_alloc(tx, bitmap_blocks, &new_bitmap);
    if (ret)
        return ret;
    ret = infilfs_rw_tx_defer_free(tx, old_bitmap, bitmap_blocks);
    if (ret)
        return ret;
    ret = infilfs_rw_tx_apply_deferred(tx);
    if (ret)
        return ret;

    tx->next_sb.bitmap_start_block = cpu_to_le64(new_bitmap);
    tx->next_sb.generation = cpu_to_le64(tx->generation);
    tx->next_sb.free_blocks = cpu_to_le64(tx->free_blocks);

    ret = sync_blockdev(tx->sb->s_bdev);
    if (ret)
        return ret;
    for (n = 0; n < bitmap_blocks; ++n) {
        ret = infilfs_rw_write_block(tx->sb, new_bitmap + n,
            tx->bitmap + (size_t)n * INFILFS_DISK_BLOCK_SIZE);
        if (ret)
            return ret;
    }
    ret = sync_blockdev(tx->sb->s_bdev);
    if (ret)
        return ret;

    sb_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!sb_block)
        return -ENOMEM;
    infilfs_rw_encode_superblock(&tx->next_sb, sb_block);
    first = (unsigned int)(tx->generation % INFILFS_CHECKPOINT_COUNT);
    ret = infilfs_rw_write_block(tx->sb,
        le64_to_cpu(tx->next_sb.checkpoint_block[first]), sb_block);
    if (!ret)
        ret = sync_blockdev(tx->sb->s_bdev);
    if (ret) {
        kfree(sb_block);
        return ret;
    }

    /* Commit point reached.  Update the live mount before replica writes so
     * reads observe the generation that is now durable even if redundancy
     * repair later fails. */
    sbi->disk = tx->next_sb;
    kvfree(sbi->bitmap);
    sbi->bitmap = tx->bitmap;
    tx->bitmap = NULL;
    sbi->bitmap_bytes = tx->bitmap_bytes;

    for (n = 1; n < INFILFS_CHECKPOINT_COUNT; ++n) {
        unsigned int idx = (first + n) % INFILFS_CHECKPOINT_COUNT;
        ret = infilfs_rw_write_block(tx->sb,
            le64_to_cpu(sbi->disk.checkpoint_block[idx]), sb_block);
        if (ret && !replica_error)
            replica_error = ret;
    }
    ret = sync_blockdev(tx->sb->s_bdev);
    if (ret && !replica_error)
        replica_error = ret;
    kfree(sb_block);

    if (replica_error) {
        sbi->write_poisoned = true;
        pr_warn("InfiltratorFS: native RW checkpoint committed but replica refresh failed; further writes disabled until remount\n");
    }
    kfree(tx->deferred);
    tx->deferred = NULL;
    tx->deferred_count = 0;
    tx->deferred_capacity = 0;
    return 0;
}

static void infilfs_rw_init_page(u8 *block, const u8 magic[8],
                                 const u8 owner[16], u64 generation)
{
    struct infilfs_metadata_page_disk *page;

    memset(block, 0, INFILFS_DISK_BLOCK_SIZE);
    page = (struct infilfs_metadata_page_disk *)block;
    memcpy(page->magic, magic, 8);
    page->generation = cpu_to_le64(generation);
    memcpy(page->owner_object_id, owner, 16);
    page->checksum_type = cpu_to_le32(INFILFS_CHECKSUM_CRC64_ECMA);
}

static int infilfs_rw_collect_index(struct infilfs_rw_tx *tx,
                                    u8 old_head[INFILFS_DISK_BLOCK_SIZE],
                                    struct infilfs_index_entry_disk **entries_out,
                                    u32 *count_out)
{
    struct infilfs_object_header_disk *header;
    struct infilfs_index_payload_disk *payload;
    struct infilfs_index_entry_disk *entries;
    u32 count;
    u16 version;
    int ret;

    ret = infilfs_read_object(tx->sb,
        le64_to_cpu(tx->sbi->disk.object_index_block),
        INFILFS_OBJECT_INDEX, NULL, old_head);
    if (ret)
        return ret;
    header = (struct infilfs_object_header_disk *)old_head;
    payload = (struct infilfs_index_payload_disk *)(header + 1);
    count = le32_to_cpu(payload->entry_count);
    entries = kvmalloc_array((size_t)count + 1u, sizeof(*entries), GFP_NOFS);
    if (!entries)
        return -ENOMEM;
    version = le16_to_cpu(header->object_version);

    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        size_t needed = sizeof(*payload) + (size_t)count * sizeof(*entries);
        if (le32_to_cpu(payload->reserved) != 0 ||
            needed > le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
        memcpy(entries, payload + 1, (size_t)count * sizeof(*entries));
    } else if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const __le64 *pages = (const __le64 *)(payload + 1);
        u32 page_count = le32_to_cpu(payload->reserved);
        u32 copied = 0;
        u32 p;
        u8 *page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);

        if (!page_block) {
            ret = -ENOMEM;
            goto fail;
        }
        if ((count && !page_count) || page_count > INFILFS_INDEX_PAGE_POINTERS) {
            kfree(page_block);
            ret = -EFSCORRUPTED;
            goto fail;
        }
        for (p = 0; p < page_count; ++p) {
            struct infilfs_metadata_page_disk *page;
            u32 page_entries;
            u64 block = le64_to_cpu(pages[p]);

            ret = infilfs_read_block(tx->sb, block, page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(tx->sb, page_block,
                                             infilfs_index_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            page_entries = le32_to_cpu(page->entry_count);
            if (page_entries > INFILFS_INDEX_ENTRIES_PER_PAGE ||
                copied > count || page_entries > count - copied ||
                le32_to_cpu(page->bytes_used) !=
                    page_entries * sizeof(*entries)) {
                ret = -EFSCORRUPTED;
                break;
            }
            memcpy(entries + copied, page + 1,
                   (size_t)page_entries * sizeof(*entries));
            copied += page_entries;
            ret = infilfs_rw_tx_defer_free(tx, block, 1);
            if (ret)
                break;
        }
        kfree(page_block);
        if (ret)
            goto fail;
        if (copied != count) {
            ret = -EFSCORRUPTED;
            goto fail;
        }
    } else {
        ret = -EFSCORRUPTED;
        goto fail;
    }

    *entries_out = entries;
    *count_out = count;
    return 0;
fail:
    kvfree(entries);
    return ret;
}

static int infilfs_rw_rebuild_index(struct infilfs_rw_tx *tx,
                                    const u8 *repoint_id, u64 repoint_block,
                                    u16 repoint_type,
                                    const u8 *add_id, u64 add_block,
                                    u16 add_type)
{
    u8 *old_head;
    u8 *new_head;
    u8 *page_block;
    struct infilfs_object_header_disk *header;
    struct infilfs_index_payload_disk *payload;
    struct infilfs_index_entry_disk *entries = NULL;
    u32 count = 0;
    u32 new_count;
    u32 i;
    bool found = !repoint_id;
    int ret;
    u64 new_head_block;
    bool paged;

    old_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!old_head || !new_head || !page_block) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_rw_collect_index(tx, old_head, &entries, &count);
    if (ret)
        goto out;

    for (i = 0; i < count; ++i) {
        if (repoint_id && memcmp(entries[i].object_id, repoint_id, 16) == 0) {
            entries[i].object_block = cpu_to_le64(repoint_block);
            entries[i].object_type = cpu_to_le16(repoint_type);
            found = true;
        }
        if (add_id && memcmp(entries[i].object_id, add_id, 16) == 0) {
            ret = -EEXIST;
            goto out;
        }
    }
    if (!found) {
        ret = -ENOENT;
        goto out;
    }
    new_count = count;
    if (add_id) {
        memset(&entries[new_count], 0, sizeof(entries[new_count]));
        memcpy(entries[new_count].object_id, add_id, 16);
        entries[new_count].object_block = cpu_to_le64(add_block);
        entries[new_count].object_type = cpu_to_le16(add_type);
        new_count++;
    }

    header = (struct infilfs_object_header_disk *)old_head;
    paged = le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_PAGED ||
        (le64_to_cpu(tx->next_sb.incompat_flags) & INFILFS_INCOMPAT_PAGED_METADATA);
    memset(new_head, 0, INFILFS_DISK_BLOCK_SIZE);
    memcpy(new_head, old_head, sizeof(*header) + sizeof(*payload));
    header = (struct infilfs_object_header_disk *)new_head;
    payload = (struct infilfs_index_payload_disk *)(header + 1);
    header->generation = cpu_to_le64(tx->generation);
    payload->entry_count = cpu_to_le32(new_count);

    if (paged) {
        __le64 *pointers = (__le64 *)(payload + 1);
        u32 page_count = DIV_ROUND_UP(new_count, INFILFS_INDEX_ENTRIES_PER_PAGE);
        u32 copied = 0;
        u32 p;

        if (page_count > INFILFS_INDEX_PAGE_POINTERS) {
            ret = -ENOSPC;
            goto out;
        }
        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_PAGED);
        payload->reserved = cpu_to_le32(page_count);
        header->payload_size = cpu_to_le32(sizeof(*payload) +
            (size_t)page_count * sizeof(__le64));
        for (p = 0; p < page_count; ++p) {
            u32 n = min_t(u32, INFILFS_INDEX_ENTRIES_PER_PAGE,
                          new_count - copied);
            struct infilfs_metadata_page_disk *page;
            u64 page_block_no;

            ret = infilfs_rw_tx_alloc(tx, 1, &page_block_no);
            if (ret)
                goto out;
            infilfs_rw_init_page(page_block, infilfs_index_page_magic,
                                 header->object_id, tx->generation);
            page = (struct infilfs_metadata_page_disk *)page_block;
            page->entry_count = cpu_to_le32(n);
            page->bytes_used = cpu_to_le32(n * sizeof(*entries));
            memcpy(page + 1, entries + copied, (size_t)n * sizeof(*entries));
            ret = infilfs_rw_finalize_page(page_block);
            if (ret)
                goto out;
            ret = infilfs_rw_write_block(tx->sb, page_block_no, page_block);
            if (ret)
                goto out;
            pointers[p] = cpu_to_le64(page_block_no);
            copied += n;
        }
    } else {
        size_t payload_size = sizeof(*payload) +
            (size_t)new_count * sizeof(*entries);
        if (payload_size > INFILFS_DISK_BLOCK_SIZE - sizeof(*header)) {
            ret = -ENOSPC;
            goto out;
        }
        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
        payload->reserved = 0;
        header->payload_size = cpu_to_le32(payload_size);
        memcpy(payload + 1, entries, (size_t)new_count * sizeof(*entries));
    }

    ret = infilfs_rw_finalize_object(new_head);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_alloc(tx, 1, &new_head_block);
    if (ret)
        goto out;
    ret = infilfs_rw_write_block(tx->sb, new_head_block, new_head);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_defer_free(tx,
        le64_to_cpu(tx->sbi->disk.object_index_block), 1);
    if (ret)
        goto out;
    tx->next_sb.object_index_block = cpu_to_le64(new_head_block);
out:
    kvfree(entries);
    kfree(page_block);
    kfree(new_head);
    kfree(old_head);
    return ret;
}

static int infilfs_rw_collect_directory(struct infilfs_rw_tx *tx,
                                        struct inode *dir,
                                        u8 old_head[INFILFS_DISK_BLOCK_SIZE],
                                        u8 **bytes_out, size_t *bytes_out_len,
                                        u32 *entry_count_out)
{
    struct infilfs_inode_info *ii = INFILFS_I(dir);
    struct infilfs_object_header_disk *header;
    struct infilfs_directory_payload_disk *payload;
    u16 version;
    u32 count;
    size_t bytes = 0;
    u8 *all = NULL;
    int ret;

    ret = infilfs_read_object(tx->sb, ii->object_block,
                              INFILFS_OBJECT_DIRECTORY, ii->object_id, old_head);
    if (ret)
        return ret;
    header = (struct infilfs_object_header_disk *)old_head;
    payload = (struct infilfs_directory_payload_disk *)(header + 1);
    count = le32_to_cpu(payload->entry_count);
    version = le16_to_cpu(header->object_version);

    if (version == INFILFS_OBJECT_VERSION_CLASSIC) {
        bytes = le32_to_cpu(payload->bytes_used);
        if (sizeof(*payload) + bytes > le32_to_cpu(header->payload_size))
            return -EFSCORRUPTED;
        all = kvmalloc(bytes ? bytes : 1u, GFP_NOFS);
        if (!all)
            return -ENOMEM;
        if (bytes)
            memcpy(all, payload + 1, bytes);
    } else if (version == INFILFS_OBJECT_VERSION_PAGED) {
        const __le64 *pages = (const __le64 *)(payload + 1);
        u32 page_count = le32_to_cpu(payload->bytes_used);
        u32 p;
        u8 *page_block;

        if ((count && !page_count) || page_count > INFILFS_DIRECTORY_PAGE_POINTERS)
            return -EFSCORRUPTED;
        all = kvmalloc((size_t)page_count * INFILFS_METADATA_PAGE_DATA_SIZE + 1u,
                       GFP_NOFS);
        if (!all)
            return -ENOMEM;
        page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
        if (!page_block) {
            kvfree(all);
            return -ENOMEM;
        }
        for (p = 0; p < page_count; ++p) {
            struct infilfs_metadata_page_disk *page;
            u32 used;
            u64 block = le64_to_cpu(pages[p]);

            ret = infilfs_read_block(tx->sb, block, page_block);
            if (ret)
                break;
            if (!infilfs_metadata_page_valid(tx->sb, page_block,
                                             infilfs_directory_page_magic,
                                             header->object_id)) {
                ret = -EFSCORRUPTED;
                break;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            used = le32_to_cpu(page->bytes_used);
            memcpy(all + bytes, page + 1, used);
            bytes += used;
            ret = infilfs_rw_tx_defer_free(tx, block, 1);
            if (ret)
                break;
        }
        kfree(page_block);
        if (ret) {
            kvfree(all);
            return ret;
        }
    } else {
        return -EFSCORRUPTED;
    }

    *bytes_out = all;
    *bytes_out_len = bytes;
    *entry_count_out = count;
    return 0;
}

static int infilfs_rw_directory_contains(const u8 *bytes, size_t length,
                                         const char *name, size_t name_len)
{
    size_t offset = 0;

    while (offset < length) {
        const struct infilfs_dirent_disk *entry;
        u16 rec;
        u16 n;

        if (length - offset < sizeof(*entry))
            return -EFSCORRUPTED;
        entry = (const struct infilfs_dirent_disk *)(bytes + offset);
        rec = le16_to_cpu(entry->record_size);
        n = le16_to_cpu(entry->name_length);
        if (rec < sizeof(*entry) || rec > length - offset || (rec & 7u))
            return -EFSCORRUPTED;
        if (n == name_len && sizeof(*entry) + n <= rec &&
            memcmp(bytes + offset + sizeof(*entry), name, n) == 0)
            return 1;
        offset += rec;
    }
    return offset == length ? 0 : -EFSCORRUPTED;
}

static int infilfs_rw_rebuild_directory(struct infilfs_rw_tx *tx,
                                        struct inode *dir,
                                        const char *name,
                                        const u8 child_id[16], u16 child_type,
                                        u64 *new_dir_block_out)
{
    struct infilfs_inode_info *ii = INFILFS_I(dir);
    u8 *old_head = NULL;
    u8 *new_head = NULL;
    u8 *page_block = NULL;
    u8 *all = NULL;
    u8 *grown;
    size_t bytes = 0;
    size_t name_len = strlen(name);
    size_t rec = ALIGN(sizeof(struct infilfs_dirent_disk) + name_len, 8);
    u32 count = 0;
    struct infilfs_object_header_disk *header;
    struct infilfs_directory_payload_disk *payload;
    bool paged;
    u64 new_head_block;
    int ret;

    old_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_head = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    page_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!old_head || !new_head || !page_block) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_rw_collect_directory(tx, dir, old_head, &all, &bytes, &count);
    if (ret)
        goto out;
    ret = infilfs_rw_directory_contains(all, bytes, name, name_len);
    if (ret < 0)
        goto out;
    if (ret > 0) {
        ret = -EEXIST;
        goto out;
    }
    grown = kvmalloc(bytes + rec, GFP_NOFS);
    if (!grown) {
        ret = -ENOMEM;
        goto out;
    }
    if (bytes)
        memcpy(grown, all, bytes);
    kvfree(all);
    all = grown;
    memset(all + bytes, 0, rec);
    {
        struct infilfs_dirent_disk *entry =
            (struct infilfs_dirent_disk *)(all + bytes);
        entry->record_size = cpu_to_le16((u16)rec);
        entry->name_length = cpu_to_le16((u16)name_len);
        entry->object_type = cpu_to_le16(child_type);
        memcpy(entry->object_id, child_id, 16);
        memcpy(all + bytes + sizeof(*entry), name, name_len);
    }
    bytes += rec;

    header = (struct infilfs_object_header_disk *)old_head;
    paged = le16_to_cpu(header->object_version) == INFILFS_OBJECT_VERSION_PAGED ||
        (le64_to_cpu(tx->next_sb.incompat_flags) & INFILFS_INCOMPAT_PAGED_METADATA);
    memset(new_head, 0, INFILFS_DISK_BLOCK_SIZE);
    memcpy(new_head, old_head, sizeof(*header) + sizeof(*payload));
    header = (struct infilfs_object_header_disk *)new_head;
    payload = (struct infilfs_directory_payload_disk *)(header + 1);
    header->generation = cpu_to_le64(tx->generation);
    payload->entry_count = cpu_to_le32(count + 1u);
    payload->attributes.modification_time_ns = cpu_to_le64(ktime_get_real_ns());
    payload->attributes.change_time_ns = payload->attributes.modification_time_ns;
    if (child_type == INFILFS_OBJECT_DIRECTORY)
        payload->attributes.link_count = cpu_to_le64(
            le64_to_cpu(payload->attributes.link_count) + 1u);

    if (paged) {
        __le64 *pointers = (__le64 *)(payload + 1);
        size_t offset = 0;
        u32 page_count = 0;

        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_PAGED);
        while (offset < bytes) {
            struct infilfs_metadata_page_disk *page;
            size_t page_used = 0;
            u32 page_entries = 0;
            u64 page_no;

            if (page_count >= INFILFS_DIRECTORY_PAGE_POINTERS) {
                ret = -ENOSPC;
                goto out;
            }
            infilfs_rw_init_page(page_block, infilfs_directory_page_magic,
                                 header->object_id, tx->generation);
            while (offset < bytes) {
                struct infilfs_dirent_disk *entry =
                    (struct infilfs_dirent_disk *)(all + offset);
                u16 record_size = le16_to_cpu(entry->record_size);
                if (!record_size || record_size > bytes - offset) {
                    ret = -EFSCORRUPTED;
                    goto out;
                }
                if (page_used && page_used + record_size >
                    INFILFS_METADATA_PAGE_DATA_SIZE)
                    break;
                if (record_size > INFILFS_METADATA_PAGE_DATA_SIZE) {
                    ret = -ENAMETOOLONG;
                    goto out;
                }
                memcpy(page_block + sizeof(*page) + page_used,
                       all + offset, record_size);
                page_used += record_size;
                offset += record_size;
                page_entries++;
            }
            page = (struct infilfs_metadata_page_disk *)page_block;
            page->entry_count = cpu_to_le32(page_entries);
            page->bytes_used = cpu_to_le32(page_used);
            ret = infilfs_rw_finalize_page(page_block);
            if (ret)
                goto out;
            ret = infilfs_rw_tx_alloc(tx, 1, &page_no);
            if (ret)
                goto out;
            ret = infilfs_rw_write_block(tx->sb, page_no, page_block);
            if (ret)
                goto out;
            pointers[page_count++] = cpu_to_le64(page_no);
        }
        payload->bytes_used = cpu_to_le32(page_count);
        header->payload_size = cpu_to_le32(sizeof(*payload) +
            (size_t)page_count * sizeof(__le64));
    } else {
        if (sizeof(*payload) + bytes > INFILFS_DISK_BLOCK_SIZE - sizeof(*header)) {
            ret = -ENOSPC;
            goto out;
        }
        header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
        payload->bytes_used = cpu_to_le32(bytes);
        header->payload_size = cpu_to_le32(sizeof(*payload) + bytes);
        memcpy(payload + 1, all, bytes);
    }

    ret = infilfs_rw_finalize_object(new_head);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_alloc(tx, 1, &new_head_block);
    if (ret)
        goto out;
    ret = infilfs_rw_write_block(tx->sb, new_head_block, new_head);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_defer_free(tx, ii->object_block, 1);
    if (ret)
        goto out;
    if (memcmp(ii->object_id, tx->sbi->disk.root_object_id, 16) == 0)
        tx->next_sb.root_object_block = cpu_to_le64(new_head_block);
    *new_dir_block_out = new_head_block;
    ret = 0;
out:
    kvfree(all);
    kfree(page_block);
    kfree(new_head);
    kfree(old_head);
    return ret;
}

static int infilfs_rw_generate_id(struct super_block *sb, u8 id[16])
{
    unsigned int attempt;

    for (attempt = 0; attempt < 32; ++attempt) {
        u64 block;
        u16 type;
        u8 combined = 0;
        unsigned int i;
        int ret;

        get_random_bytes(id, 16);
        for (i = 0; i < 16; ++i)
            combined |= id[i];
        if (!combined)
            continue;
        ret = infilfs_index_lookup(sb, id, &block, &type);
        if (ret == -ENOENT)
            return 0;
        if (ret)
            return ret;
    }
    return -EAGAIN;
}

static void infilfs_rw_fill_common_attributes(
    struct infilfs_attributes_disk *attributes,
    struct infilfs_posix_compat_disk *posix,
    umode_t mode, u64 links)
{
    u64 now = ktime_get_real_ns();
    u32 uid = 0;
    u32 gid = 0;

    memset(attributes, 0, sizeof(*attributes));
    memset(posix, 0, sizeof(*posix));
    attributes->link_count = cpu_to_le64(links);
    attributes->birth_time_ns = cpu_to_le64(now);
    attributes->access_time_ns = cpu_to_le64(now);
    attributes->modification_time_ns = cpu_to_le64(now);
    attributes->change_time_ns = cpu_to_le64(now);
    posix->permissions = cpu_to_le32(mode & 07777);
    posix->uid = cpu_to_le32(uid);
    posix->gid = cpu_to_le32(gid);
}

static int infilfs_rw_create_object(struct infilfs_rw_tx *tx,
                                    struct inode *parent, u16 type,
                                    umode_t mode, const u8 object_id[16],
                                    u64 *block_out)
{
    struct infilfs_inode_info *parent_ii = INFILFS_I(parent);
    struct infilfs_object_header_disk *header;
    u8 *object;
    u64 block;
    int ret;

    object = kzalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    header = (struct infilfs_object_header_disk *)object;
    memcpy(header->magic, infilfs_object_magic, 8);
    header->object_type = cpu_to_le16(type);
    header->object_version = cpu_to_le16(
        type == INFILFS_OBJECT_DIRECTORY &&
        (le64_to_cpu(tx->next_sb.incompat_flags) & INFILFS_INCOMPAT_PAGED_METADATA) ?
        INFILFS_OBJECT_VERSION_PAGED : INFILFS_OBJECT_VERSION_CLASSIC);
    header->header_size = cpu_to_le32(sizeof(*header));
    header->generation = cpu_to_le64(tx->generation);
    memcpy(header->object_id, object_id, 16);
    memcpy(header->parent_id, parent_ii->object_id, 16);
    header->checksum_type = cpu_to_le32(INFILFS_CHECKSUM_CRC64_ECMA);

    if (type == INFILFS_OBJECT_FILE) {
        struct infilfs_file_payload_disk *file =
            (struct infilfs_file_payload_disk *)(header + 1);
        header->payload_size = cpu_to_le32(sizeof(*file));
        infilfs_rw_fill_common_attributes(&file->attributes, &file->posix,
                                          mode, 1);
        file->data_checksum_type = cpu_to_le32(INFILFS_CHECKSUM_SHA256);
    } else if (type == INFILFS_OBJECT_DIRECTORY) {
        struct infilfs_directory_payload_disk *dir =
            (struct infilfs_directory_payload_disk *)(header + 1);
        header->payload_size = cpu_to_le32(sizeof(*dir));
        infilfs_rw_fill_common_attributes(&dir->attributes, &dir->posix,
                                          mode, 2);
    } else {
        kfree(object);
        return -EINVAL;
    }

    ret = infilfs_rw_finalize_object(object);
    if (ret)
        goto out;
    ret = infilfs_rw_tx_alloc(tx, 1, &block);
    if (ret)
        goto out;
    ret = infilfs_rw_write_block(tx->sb, block, object);
    if (!ret)
        *block_out = block;
out:
    kfree(object);
    return ret;
}

static int infilfs_rw_create_child(struct inode *dir, struct dentry *dentry,
                                   umode_t mode, u16 type,
                                   u8 object_id_out[16], u64 *block_out)
{
    struct infilfs_inode_info *dir_ii = INFILFS_I(dir);
    struct infilfs_sb_info *sbi = INFILFS_SB(dir->i_sb);
    struct infilfs_rw_tx tx;
    u8 object_id[16];
    u64 object_block;
    u64 new_dir_block;
    bool root_parent;
    int ret;

    if (!dentry->d_name.len || dentry->d_name.len > INFILFS_NAME_MAX ||
        !infilfs_rw_utf8_valid(dentry->d_name.name, dentry->d_name.len) ||
        (dentry->d_name.len == 1 && dentry->d_name.name[0] == '.') ||
        (dentry->d_name.len == 2 && dentry->d_name.name[0] == '.' &&
         dentry->d_name.name[1] == '.'))
        return -EINVAL;

    mutex_lock(&sbi->write_lock);
    ret = infilfs_rw_generate_id(dir->i_sb, object_id);
    if (ret)
        goto unlock;
    ret = infilfs_rw_tx_begin(dir->i_sb, &tx);
    if (ret)
        goto unlock;
    ret = infilfs_rw_create_object(&tx, dir, type, mode, object_id, &object_block);
    if (ret)
        goto abort;
    ret = infilfs_rw_rebuild_directory(&tx, dir, dentry->d_name.name,
                                       object_id, type, &new_dir_block);
    if (ret)
        goto abort;
    root_parent = memcmp(dir_ii->object_id, sbi->disk.root_object_id, 16) == 0;
    ret = infilfs_rw_rebuild_index(&tx,
        root_parent ? NULL : dir_ii->object_id, new_dir_block,
        INFILFS_OBJECT_DIRECTORY, object_id, object_block, type);
    if (ret)
        goto abort;
    ret = infilfs_rw_tx_commit(&tx);
    if (ret)
        goto abort;
    dir_ii->object_block = new_dir_block;
    memcpy(object_id_out, object_id, 16);
    *block_out = object_block;
    mutex_unlock(&sbi->write_lock);
    return 0;
abort:
    infilfs_rw_tx_abort(&tx);
unlock:
    mutex_unlock(&sbi->write_lock);
    return ret;
}

static int infilfs_rw_load_inline(struct inode *inode, u8 *data, size_t *size_out)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    struct infilfs_data_checksum_disk *digest;
    u8 actual[32];
    u64 size;
    u8 *object;
    int ret;

    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!object)
        return -ENOMEM;
    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_FILE, ii->object_id, object);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    size = le64_to_cpu(file->attributes.logical_size);
    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_CLASSIC ||
        le32_to_cpu(file->extent_count) != 0 || size > INFILFS_INLINE_DATA_MAX) {
        ret = -EOPNOTSUPP;
        goto out;
    }
    memset(data, 0, INFILFS_INLINE_DATA_MAX);
    if (size) {
        size_t expected = sizeof(*file) + sizeof(*digest) + (size_t)size;
        if (expected != le32_to_cpu(header->payload_size)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        digest = (struct infilfs_data_checksum_disk *)(file + 1);
        memcpy(data, digest + 1, (size_t)size);
        infilfs_rw_inline_digest(data, (size_t)size, actual);
        if (memcmp(actual, digest->bytes, sizeof(actual)) != 0) {
            ret = -EIO;
            goto out;
        }
    } else if (le32_to_cpu(header->payload_size) != sizeof(*file)) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    *size_out = (size_t)size;
    ret = 0;
out:
    kfree(object);
    return ret;
}

static int infilfs_rw_replace_inline_locked(struct inode *inode,
                                            const u8 *data, size_t size)
{
    struct infilfs_inode_info *ii = INFILFS_I(inode);
    struct infilfs_rw_tx tx;
    struct infilfs_object_header_disk *header;
    struct infilfs_file_payload_disk *file;
    struct infilfs_data_checksum_disk *digest;
    u8 *old_object;
    u8 *new_object;
    u64 new_block;
    int ret;

    if (size > INFILFS_INLINE_DATA_MAX)
        return -EFBIG;
    ret = infilfs_rw_tx_begin(inode->i_sb, &tx);
    if (ret)
        return ret;
    old_object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    new_object = kzalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!old_object || !new_object) {
        ret = -ENOMEM;
        goto abort;
    }
    ret = infilfs_read_object(inode->i_sb, ii->object_block,
                              INFILFS_OBJECT_FILE, ii->object_id, old_object);
    if (ret)
        goto abort;
    header = (struct infilfs_object_header_disk *)old_object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    if (le16_to_cpu(header->object_version) != INFILFS_OBJECT_VERSION_CLASSIC ||
        le32_to_cpu(file->extent_count) != 0 ||
        le64_to_cpu(file->attributes.logical_size) > INFILFS_INLINE_DATA_MAX) {
        ret = -EOPNOTSUPP;
        goto abort;
    }

    memcpy(new_object, old_object, sizeof(*header) + sizeof(*file));
    header = (struct infilfs_object_header_disk *)new_object;
    file = (struct infilfs_file_payload_disk *)(header + 1);
    header->generation = cpu_to_le64(tx.generation);
    header->object_version = cpu_to_le16(INFILFS_OBJECT_VERSION_CLASSIC);
    file->attributes.logical_size = cpu_to_le64(size);
    file->attributes.modification_time_ns = cpu_to_le64(ktime_get_real_ns());
    file->attributes.change_time_ns = file->attributes.modification_time_ns;
    file->extent_count = 0;
    file->data_checksum_type = cpu_to_le32(INFILFS_CHECKSUM_SHA256);
    memset(file->checksum_head_id, 0, sizeof(file->checksum_head_id));
    if (size) {
        digest = (struct infilfs_data_checksum_disk *)(file + 1);
        ret = infilfs_rw_inline_digest(data, size, digest->bytes);
        if (ret)
            goto abort;
        memcpy(digest + 1, data, size);
        header->payload_size = cpu_to_le32(sizeof(*file) + sizeof(*digest) + size);
    } else {
        header->payload_size = cpu_to_le32(sizeof(*file));
    }
    ret = infilfs_rw_finalize_object(new_object);
    if (ret)
        goto abort;
    ret = infilfs_rw_tx_alloc(&tx, 1, &new_block);
    if (ret)
        goto abort;
    ret = infilfs_rw_write_block(inode->i_sb, new_block, new_object);
    if (ret)
        goto abort;
    ret = infilfs_rw_rebuild_index(&tx, ii->object_id, new_block,
                                   INFILFS_OBJECT_FILE, NULL, 0, 0);
    if (ret)
        goto abort;
    ret = infilfs_rw_tx_defer_free(&tx, ii->object_block, 1);
    if (ret)
        goto abort;
    ret = infilfs_rw_tx_commit(&tx);
    if (ret)
        goto abort;
    ii->object_block = new_block;
    i_size_write(inode, size);
    kfree(new_object);
    kfree(old_object);
    return 0;
abort:
    kfree(new_object);
    kfree(old_object);
    infilfs_rw_tx_abort(&tx);
    return ret;
}

static ssize_t infilfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *filep = iocb->ki_filp;
    struct inode *inode = file_inode(filep);
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    loff_t pos = iocb->ki_pos;
    size_t requested = iov_iter_count(from);
    size_t old_size = 0;
    size_t copied;
    size_t new_size;
    u8 *data;
    int ret;

    if (pos < 0)
        return -EINVAL;
    if (!requested)
        return 0;
    if ((u64)pos > INFILFS_INLINE_DATA_MAX ||
        requested > INFILFS_INLINE_DATA_MAX - (size_t)pos)
        return -EFBIG;

    data = kzalloc(INFILFS_INLINE_DATA_MAX, GFP_NOFS);
    if (!data)
        return -ENOMEM;
    mutex_lock(&sbi->write_lock);
    ret = infilfs_rw_load_inline(inode, data, &old_size);
    if (ret)
        goto out;
    copied = copy_from_iter(data + pos, requested, from);
    if (!copied) {
        ret = -EFAULT;
        goto out;
    }
    new_size = max_t(size_t, old_size, (size_t)pos + copied);
    ret = infilfs_rw_replace_inline_locked(inode, data, new_size);
    if (ret)
        goto out;
    iocb->ki_pos += copied;
    ret = copied;
out:
    mutex_unlock(&sbi->write_lock);
    kfree(data);
    return ret;
}

static int infilfs_file_fsync(struct file *file, loff_t start, loff_t end,
                              int datasync)
{
    (void)start;
    (void)end;
    (void)datasync;
    return sync_blockdev(file_inode(file)->i_sb->s_bdev);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_rw_create(struct mnt_idmap *idmap, struct inode *dir,
                             struct dentry *dentry, umode_t mode, bool excl)
#else
static int infilfs_rw_create(struct user_namespace *idmap, struct inode *dir,
                             struct dentry *dentry, umode_t mode, bool excl)
#endif
{
    struct inode *inode;
    u8 id[16];
    u64 block;
    int ret;

    (void)idmap;
    (void)excl;
    ret = infilfs_rw_create_child(dir, dentry, mode, INFILFS_OBJECT_FILE,
                                  id, &block);
    if (ret)
        return ret;
    inode = infilfs_get_inode(dir->i_sb, block, INFILFS_OBJECT_FILE, id);
    if (IS_ERR(inode))
        return PTR_ERR(inode);
    d_instantiate(dentry, inode);
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_rw_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                            struct dentry *dentry, umode_t mode)
#else
static int infilfs_rw_mkdir(struct user_namespace *idmap, struct inode *dir,
                            struct dentry *dentry, umode_t mode)
#endif
{
    struct inode *inode;
    u8 id[16];
    u64 block;
    int ret;

    (void)idmap;
    ret = infilfs_rw_create_child(dir, dentry, mode, INFILFS_OBJECT_DIRECTORY,
                                  id, &block);
    if (ret)
        return ret;
    inode = infilfs_get_inode(dir->i_sb, block, INFILFS_OBJECT_DIRECTORY, id);
    if (IS_ERR(inode))
        return PTR_ERR(inode);
    d_instantiate(dentry, inode);
    inc_nlink(dir);
    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int infilfs_rw_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                              struct iattr *attr)
#else
static int infilfs_rw_setattr(struct user_namespace *idmap, struct dentry *dentry,
                              struct iattr *attr)
#endif
{
    struct inode *inode = d_inode(dentry);
    struct infilfs_sb_info *sbi = INFILFS_SB(inode->i_sb);
    u8 *data;
    size_t old_size = 0;
    size_t new_size;
    int ret;

    ret = setattr_prepare(idmap, dentry, attr);
    if (ret)
        return ret;
    if (!(attr->ia_valid & ATTR_SIZE))
        return -EOPNOTSUPP;
    if (attr->ia_size < 0 || attr->ia_size > INFILFS_INLINE_DATA_MAX)
        return -EFBIG;
    data = kzalloc(INFILFS_INLINE_DATA_MAX, GFP_NOFS);
    if (!data)
        return -ENOMEM;
    mutex_lock(&sbi->write_lock);
    ret = infilfs_rw_load_inline(inode, data, &old_size);
    if (ret)
        goto out;
    new_size = (size_t)attr->ia_size;
    if (new_size > old_size)
        memset(data + old_size, 0, new_size - old_size);
    ret = infilfs_rw_replace_inline_locked(inode, data, new_size);
out:
    mutex_unlock(&sbi->write_lock);
    kfree(data);
    return ret;
}
