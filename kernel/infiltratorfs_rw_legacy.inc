// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Initial native Linux write path retained by Format 0.17.
 *
 * This file retains the original synchronous copy-on-write transaction engine
 * used by the native bring-up.  The composed native layers add extent and
 * namespace operations over it while preserving the same allocation-ownership and
 * three-checkpoint commit protocol as the portable core.  Snapshot ownership
 * is represented by a mount-time retained-ownership bitset so retained generations remain
 * immutable while the live generation advances.
 */

bool infilfs_rw_bitmap_get(const u8 *bitmap, u64 block)
{
    return (bitmap[block >> 3] & (u8)(1u << (block & 7u))) != 0;
}

void infilfs_rw_bitmap_set(u8 *bitmap, u64 block, bool used)
{
    u8 mask = (u8)(1u << (block & 7u));

    if (used)
        bitmap[block >> 3] |= mask;
    else
        bitmap[block >> 3] &= (u8)~mask;
}

void infilfs_rw_free_extent_index_invalidate(struct infilfs_rw_tx *tx)
{
    if (tx)
        tx->free_extent_index_valid = false;
}

static int infilfs_rw_free_extent_index_reserve(struct infilfs_rw_tx *tx,
                                                 size_t needed)
{
    struct infilfs_rw_free_range *grown;
    size_t next;

    if (needed <= tx->free_extent_capacity)
        return 0;
    next = tx->free_extent_capacity ? tx->free_extent_capacity : 64u;
    while (next < needed) {
        if (next > SIZE_MAX / 2u)
            return -EOVERFLOW;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(*grown))
        return -EOVERFLOW;
    /*
     * The free-extent index can legitimately grow into the hundreds of
     * thousands of entries on a heavily fragmented filesystem.  krealloc()
     * is a poor fit for that cache because sufficiently large kmalloc-backed
     * growth can fail solely from physical-memory fragmentation even when
     * virtual memory is plentiful.  The index is rebuildable and never used
     * for DMA, so allow kvmalloc() to fall back to vmalloc-backed storage.
     */
    grown = kvmalloc_array(next, sizeof(*grown), GFP_NOFS);
    if (!grown)
        return -ENOMEM;
    if (tx->free_extents && tx->free_extent_count)
        memcpy(grown, tx->free_extents,
               tx->free_extent_count * sizeof(*grown));
    kvfree(tx->free_extents);
    tx->free_extents = grown;
    tx->free_extent_capacity = next;
    return 0;
}

static int infilfs_rw_free_extent_index_rebuild(struct infilfs_rw_tx *tx)
{
    u64 total;
    u64 block = 0;
    size_t count = 0;
    int ret;

    if (!tx || !tx->bitmap || !tx->sbi)
        return -EINVAL;
    total = infilfs_volume_blocks(tx->sbi);

    while (block < total) {
        while (block < total && infilfs_rw_bitmap_get(tx->bitmap, block))
            block++;
        if (block == total)
            break;
        count++;
        while (block < total && !infilfs_rw_bitmap_get(tx->bitmap, block))
            block++;
    }

    ret = infilfs_rw_free_extent_index_reserve(tx, count ? count : 1u);
    if (ret) {
        infilfs_rw_free_extent_index_invalidate(tx);
        return ret;
    }

    block = 0;
    count = 0;
    while (block < total) {
        u64 start;

        while (block < total && infilfs_rw_bitmap_get(tx->bitmap, block))
            block++;
        if (block == total)
            break;
        start = block;
        while (block < total && !infilfs_rw_bitmap_get(tx->bitmap, block))
            block++;
        tx->free_extents[count].start = start;
        tx->free_extents[count].count = block - start;
        count++;
    }

    tx->free_extent_count = count;
    tx->free_extent_index_valid = true;
    return 0;
}

int infilfs_rw_free_extent_index_remove(struct infilfs_rw_tx *tx,
                                                u64 start, u64 count)
{
    u64 end;
    size_t i;
    int ret;

    if (!tx || !tx->free_extent_index_valid || !count)
        return -EINVAL;
    if (start > U64_MAX - count)
        return -EOVERFLOW;
    end = start + count;

    for (i = 0; i < tx->free_extent_count; ++i) {
        u64 extent_start = tx->free_extents[i].start;
        u64 extent_end;

        if (extent_start > U64_MAX - tx->free_extents[i].count)
            return -EFSCORRUPTED;
        extent_end = extent_start + tx->free_extents[i].count;
        if (start < extent_start || end > extent_end)
            continue;

        if (start == extent_start && end == extent_end) {
            memmove(&tx->free_extents[i], &tx->free_extents[i + 1u],
                    (tx->free_extent_count - i - 1u) *
                        sizeof(*tx->free_extents));
            tx->free_extent_count--;
            return 0;
        }
        if (start == extent_start) {
            tx->free_extents[i].start = end;
            tx->free_extents[i].count = extent_end - end;
            return 0;
        }
        if (end == extent_end) {
            tx->free_extents[i].count = start - extent_start;
            return 0;
        }

        ret = infilfs_rw_free_extent_index_reserve(
            tx, tx->free_extent_count + 1u);
        if (ret)
            return ret;
        memmove(&tx->free_extents[i + 2u], &tx->free_extents[i + 1u],
                (tx->free_extent_count - i - 1u) *
                    sizeof(*tx->free_extents));
        tx->free_extents[i].count = start - extent_start;
        tx->free_extents[i + 1u].start = end;
        tx->free_extents[i + 1u].count = extent_end - end;
        tx->free_extent_count++;
        return 0;
    }
    return -ENOENT;
}

static bool infilfs_rw_free_extent_choose_forward(
    const struct infilfs_rw_tx *tx, u64 wanted, u64 cursor, u64 *start_out)
{
    unsigned int pass;
    size_t i;

    if (!tx || !tx->free_extent_index_valid || !wanted ||
        !tx->free_extent_count)
        return false;

    for (pass = 0; pass < 2u; ++pass) {
        for (i = 0; i < tx->free_extent_count; ++i) {
            u64 start = tx->free_extents[i].start;
            u64 end = start + tx->free_extents[i].count;
            u64 candidate;

            if ((!pass && end <= cursor) || (pass && end > cursor))
                continue;
            candidate = !pass && start < cursor ? cursor : start;
            if (candidate < end && wanted <= end - candidate) {
                *start_out = candidate;
                return true;
            }
        }
    }

    /*
     * A locality cursor can bisect one maximal free run. Reconsider whole
     * extents before falling back to the authoritative bitmap scanner.
     */
    for (i = 0; i < tx->free_extent_count; ++i) {
        if (tx->free_extents[i].count >= wanted) {
            *start_out = tx->free_extents[i].start;
            return true;
        }
    }
    return false;
}

static bool infilfs_rw_free_extent_choose_reverse(
    const struct infilfs_rw_tx *tx, u64 wanted, u64 cursor, u64 *start_out)
{
    unsigned int pass;
    size_t n;

    if (!tx || !tx->free_extent_index_valid || !wanted ||
        !tx->free_extent_count)
        return false;

    for (pass = 0; pass < 2u; ++pass) {
        for (n = tx->free_extent_count; n > 0; --n) {
            const struct infilfs_rw_free_range *extent =
                &tx->free_extents[n - 1u];
            u64 start = extent->start;
            u64 end = start + extent->count;
            u64 candidate_end;

            if ((!pass && start > cursor) || (pass && start <= cursor))
                continue;
            candidate_end = !pass && end > cursor + 1u ?
                cursor + 1u : end;
            if (candidate_end > start &&
                wanted <= candidate_end - start) {
                *start_out = candidate_end - wanted;
                return true;
            }
        }
    }

    for (n = tx->free_extent_count; n > 0; --n) {
        const struct infilfs_rw_free_range *extent =
            &tx->free_extents[n - 1u];
        if (extent->count >= wanted) {
            *start_out = extent->start + extent->count - wanted;
            return true;
        }
    }
    return false;
}

int infilfs_rw_write_block(struct super_block *sb, u64 block,
                                  const void *data)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct buffer_head *bh;

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
    brelse(bh);
    return 0;
}

static const u64 infilfs_rw_crc64_table[256] = {
    0x0000000000000000ULL, 0x42F0E1EBA9EA3693ULL, 0x85E1C3D753D46D26ULL, 0xC711223CFA3E5BB5ULL,
    0x493366450E42ECDFULL, 0x0BC387AEA7A8DA4CULL, 0xCCD2A5925D9681F9ULL, 0x8E224479F47CB76AULL,
    0x9266CC8A1C85D9BEULL, 0xD0962D61B56FEF2DULL, 0x17870F5D4F51B498ULL, 0x5577EEB6E6BB820BULL,
    0xDB55AACF12C73561ULL, 0x99A54B24BB2D03F2ULL, 0x5EB4691841135847ULL, 0x1C4488F3E8F96ED4ULL,
    0x663D78FF90E185EFULL, 0x24CD9914390BB37CULL, 0xE3DCBB28C335E8C9ULL, 0xA12C5AC36ADFDE5AULL,
    0x2F0E1EBA9EA36930ULL, 0x6DFEFF5137495FA3ULL, 0xAAEFDD6DCD770416ULL, 0xE81F3C86649D3285ULL,
    0xF45BB4758C645C51ULL, 0xB6AB559E258E6AC2ULL, 0x71BA77A2DFB03177ULL, 0x334A9649765A07E4ULL,
    0xBD68D2308226B08EULL, 0xFF9833DB2BCC861DULL, 0x388911E7D1F2DDA8ULL, 0x7A79F00C7818EB3BULL,
    0xCC7AF1FF21C30BDEULL, 0x8E8A101488293D4DULL, 0x499B3228721766F8ULL, 0x0B6BD3C3DBFD506BULL,
    0x854997BA2F81E701ULL, 0xC7B97651866BD192ULL, 0x00A8546D7C558A27ULL, 0x4258B586D5BFBCB4ULL,
    0x5E1C3D753D46D260ULL, 0x1CECDC9E94ACE4F3ULL, 0xDBFDFEA26E92BF46ULL, 0x990D1F49C77889D5ULL,
    0x172F5B3033043EBFULL, 0x55DFBADB9AEE082CULL, 0x92CE98E760D05399ULL, 0xD03E790CC93A650AULL,
    0xAA478900B1228E31ULL, 0xE8B768EB18C8B8A2ULL, 0x2FA64AD7E2F6E317ULL, 0x6D56AB3C4B1CD584ULL,
    0xE374EF45BF6062EEULL, 0xA1840EAE168A547DULL, 0x66952C92ECB40FC8ULL, 0x2465CD79455E395BULL,
    0x3821458AADA7578FULL, 0x7AD1A461044D611CULL, 0xBDC0865DFE733AA9ULL, 0xFF3067B657990C3AULL,
    0x711223CFA3E5BB50ULL, 0x33E2C2240A0F8DC3ULL, 0xF4F3E018F031D676ULL, 0xB60301F359DBE0E5ULL,
    0xDA050215EA6C212FULL, 0x98F5E3FE438617BCULL, 0x5FE4C1C2B9B84C09ULL, 0x1D14202910527A9AULL,
    0x93366450E42ECDF0ULL, 0xD1C685BB4DC4FB63ULL, 0x16D7A787B7FAA0D6ULL, 0x5427466C1E109645ULL,
    0x4863CE9FF6E9F891ULL, 0x0A932F745F03CE02ULL, 0xCD820D48A53D95B7ULL, 0x8F72ECA30CD7A324ULL,
    0x0150A8DAF8AB144EULL, 0x43A04931514122DDULL, 0x84B16B0DAB7F7968ULL, 0xC6418AE602954FFBULL,
    0xBC387AEA7A8DA4C0ULL, 0xFEC89B01D3679253ULL, 0x39D9B93D2959C9E6ULL, 0x7B2958D680B3FF75ULL,
    0xF50B1CAF74CF481FULL, 0xB7FBFD44DD257E8CULL, 0x70EADF78271B2539ULL, 0x321A3E938EF113AAULL,
    0x2E5EB66066087D7EULL, 0x6CAE578BCFE24BEDULL, 0xABBF75B735DC1058ULL, 0xE94F945C9C3626CBULL,
    0x676DD025684A91A1ULL, 0x259D31CEC1A0A732ULL, 0xE28C13F23B9EFC87ULL, 0xA07CF2199274CA14ULL,
    0x167FF3EACBAF2AF1ULL, 0x548F120162451C62ULL, 0x939E303D987B47D7ULL, 0xD16ED1D631917144ULL,
    0x5F4C95AFC5EDC62EULL, 0x1DBC74446C07F0BDULL, 0xDAAD56789639AB08ULL, 0x985DB7933FD39D9BULL,
    0x84193F60D72AF34FULL, 0xC6E9DE8B7EC0C5DCULL, 0x01F8FCB784FE9E69ULL, 0x43081D5C2D14A8FAULL,
    0xCD2A5925D9681F90ULL, 0x8FDAB8CE70822903ULL, 0x48CB9AF28ABC72B6ULL, 0x0A3B7B1923564425ULL,
    0x70428B155B4EAF1EULL, 0x32B26AFEF2A4998DULL, 0xF5A348C2089AC238ULL, 0xB753A929A170F4ABULL,
    0x3971ED50550C43C1ULL, 0x7B810CBBFCE67552ULL, 0xBC902E8706D82EE7ULL, 0xFE60CF6CAF321874ULL,
    0xE224479F47CB76A0ULL, 0xA0D4A674EE214033ULL, 0x67C58448141F1B86ULL, 0x253565A3BDF52D15ULL,
    0xAB1721DA49899A7FULL, 0xE9E7C031E063ACECULL, 0x2EF6E20D1A5DF759ULL, 0x6C0603E6B3B7C1CAULL,
    0xF6FAE5C07D3274CDULL, 0xB40A042BD4D8425EULL, 0x731B26172EE619EBULL, 0x31EBC7FC870C2F78ULL,
    0xBFC9838573709812ULL, 0xFD39626EDA9AAE81ULL, 0x3A28405220A4F534ULL, 0x78D8A1B9894EC3A7ULL,
    0x649C294A61B7AD73ULL, 0x266CC8A1C85D9BE0ULL, 0xE17DEA9D3263C055ULL, 0xA38D0B769B89F6C6ULL,
    0x2DAF4F0F6FF541ACULL, 0x6F5FAEE4C61F773FULL, 0xA84E8CD83C212C8AULL, 0xEABE6D3395CB1A19ULL,
    0x90C79D3FEDD3F122ULL, 0xD2377CD44439C7B1ULL, 0x15265EE8BE079C04ULL, 0x57D6BF0317EDAA97ULL,
    0xD9F4FB7AE3911DFDULL, 0x9B041A914A7B2B6EULL, 0x5C1538ADB04570DBULL, 0x1EE5D94619AF4648ULL,
    0x02A151B5F156289CULL, 0x4051B05E58BC1E0FULL, 0x87409262A28245BAULL, 0xC5B073890B687329ULL,
    0x4B9237F0FF14C443ULL, 0x0962D61B56FEF2D0ULL, 0xCE73F427ACC0A965ULL, 0x8C8315CC052A9FF6ULL,
    0x3A80143F5CF17F13ULL, 0x7870F5D4F51B4980ULL, 0xBF61D7E80F251235ULL, 0xFD913603A6CF24A6ULL,
    0x73B3727A52B393CCULL, 0x31439391FB59A55FULL, 0xF652B1AD0167FEEAULL, 0xB4A25046A88DC879ULL,
    0xA8E6D8B54074A6ADULL, 0xEA16395EE99E903EULL, 0x2D071B6213A0CB8BULL, 0x6FF7FA89BA4AFD18ULL,
    0xE1D5BEF04E364A72ULL, 0xA3255F1BE7DC7CE1ULL, 0x64347D271DE22754ULL, 0x26C49CCCB40811C7ULL,
    0x5CBD6CC0CC10FAFCULL, 0x1E4D8D2B65FACC6FULL, 0xD95CAF179FC497DAULL, 0x9BAC4EFC362EA149ULL,
    0x158E0A85C2521623ULL, 0x577EEB6E6BB820B0ULL, 0x906FC95291867B05ULL, 0xD29F28B9386C4D96ULL,
    0xCEDBA04AD0952342ULL, 0x8C2B41A1797F15D1ULL, 0x4B3A639D83414E64ULL, 0x09CA82762AAB78F7ULL,
    0x87E8C60FDED7CF9DULL, 0xC51827E4773DF90EULL, 0x020905D88D03A2BBULL, 0x40F9E43324E99428ULL,
    0x2CFFE7D5975E55E2ULL, 0x6E0F063E3EB46371ULL, 0xA91E2402C48A38C4ULL, 0xEBEEC5E96D600E57ULL,
    0x65CC8190991CB93DULL, 0x273C607B30F68FAEULL, 0xE02D4247CAC8D41BULL, 0xA2DDA3AC6322E288ULL,
    0xBE992B5F8BDB8C5CULL, 0xFC69CAB42231BACFULL, 0x3B78E888D80FE17AULL, 0x7988096371E5D7E9ULL,
    0xF7AA4D1A85996083ULL, 0xB55AACF12C735610ULL, 0x724B8ECDD64D0DA5ULL, 0x30BB6F267FA73B36ULL,
    0x4AC29F2A07BFD00DULL, 0x08327EC1AE55E69EULL, 0xCF235CFD546BBD2BULL, 0x8DD3BD16FD818BB8ULL,
    0x03F1F96F09FD3CD2ULL, 0x41011884A0170A41ULL, 0x86103AB85A2951F4ULL, 0xC4E0DB53F3C36767ULL,
    0xD8A453A01B3A09B3ULL, 0x9A54B24BB2D03F20ULL, 0x5D45907748EE6495ULL, 0x1FB5719CE1045206ULL,
    0x919735E51578E56CULL, 0xD367D40EBC92D3FFULL, 0x1476F63246AC884AULL, 0x568617D9EF46BED9ULL,
    0xE085162AB69D5E3CULL, 0xA275F7C11F7768AFULL, 0x6564D5FDE549331AULL, 0x279434164CA30589ULL,
    0xA9B6706FB8DFB2E3ULL, 0xEB46918411358470ULL, 0x2C57B3B8EB0BDFC5ULL, 0x6EA7525342E1E956ULL,
    0x72E3DAA0AA188782ULL, 0x30133B4B03F2B111ULL, 0xF7021977F9CCEAA4ULL, 0xB5F2F89C5026DC37ULL,
    0x3BD0BCE5A45A6B5DULL, 0x79205D0E0DB05DCEULL, 0xBE317F32F78E067BULL, 0xFCC19ED95E6430E8ULL,
    0x86B86ED5267CDBD3ULL, 0xC4488F3E8F96ED40ULL, 0x0359AD0275A8B6F5ULL, 0x41A94CE9DC428066ULL,
    0xCF8B0890283E370CULL, 0x8D7BE97B81D4019FULL, 0x4A6ACB477BEA5A2AULL, 0x089A2AACD2006CB9ULL,
    0x14DEA25F3AF9026DULL, 0x562E43B4931334FEULL, 0x913F6188692D6F4BULL, 0xD3CF8063C0C759D8ULL,
    0x5DEDC41A34BBEEB2ULL, 0x1F1D25F19D51D821ULL, 0xD80C07CD676F8394ULL, 0x9AFCE626CE85B507ULL,
};

static u64 infilfs_rw_crc64(const u8 *data, size_t length)
{
    u64 crc = 0;
    size_t i;
    for (i = 0; i < length; ++i)
        crc = (crc << 8) ^ infilfs_rw_crc64_table[(u8)((crc >> 56) ^ data[i])];
    return crc;
}

u64 infilfs_rw_crc64_zeroed(const u8 *data, size_t length,
                                    size_t zero_offset, size_t zero_length)
{
    u64 crc = 0;
    size_t i;
    for (i = 0; i < length; ++i) {
        u8 byte = (i >= zero_offset && i - zero_offset < zero_length) ? 0 : data[i];
        crc = (crc << 8) ^ infilfs_rw_crc64_table[(u8)((crc >> 56) ^ byte)];
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

int infilfs_rw_finalize_object(u8 block[INFILFS_DISK_BLOCK_SIZE])
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

int infilfs_rw_finalize_page(u8 block[INFILFS_DISK_BLOCK_SIZE])
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

int infilfs_rw_encode_superblock(
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

static int infilfs_rw_heal_checkpoints(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    u8 *block;
    unsigned int i;
    int ret = 0;

    if (sb_rdonly(sb) || !sbi->checkpoint_repair_needed)
        return 0;
    block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    if (!block)
        return -ENOMEM;
    infilfs_rw_encode_superblock(&sbi->disk, block);
    for (i = 0; i < INFILFS_CHECKPOINT_COUNT; ++i) {
        ret = infilfs_rw_write_block(
            sb, le64_to_cpu(sbi->disk.checkpoint_block[i]), block);
        if (ret)
            break;
    }
    if (!ret)
        ret = sync_blockdev(sb->s_bdev);
    if (!ret)
        sbi->checkpoint_repair_needed = false;
    kfree(block);
    return ret;
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

void infilfs_rw_sha256_init(struct infilfs_rw_sha256_ctx *ctx)
{
    static const u32 initial[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };

    memcpy(ctx->state, initial, sizeof(initial));
    ctx->total = 0;
    ctx->used = 0;
}

void infilfs_rw_sha256_update(struct infilfs_rw_sha256_ctx *ctx,
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

void infilfs_rw_sha256_final(struct infilfs_rw_sha256_ctx *ctx,
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

int infilfs_rw_inline_digest(const u8 *data, size_t size, u8 out[32])
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

bool infilfs_rw_utf8_valid(const u8 *s, size_t len)
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

int infilfs_rw_snapshot_count(struct super_block *sb, u32 *count_out)
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

static int infilfs_rw_snapshot_union(struct super_block *sb, u32 snapshots)
{
    static const u8 snapshot_id[16] = {
        'I','N','F','S','-','S','N','A','P','-','C','A','T','-','0','1'
    };
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_object_header_disk *header;
    struct infilfs_snapshot_catalog_payload_disk *payload;
    struct infilfs_snapshot_record_disk *records;
    u8 *object = NULL;
    u8 *bitmap_block = NULL;
    u64 catalog_block;
    u16 type;
    u32 i;
    int ret;

    if (!snapshots)
        return 0;
    object = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    bitmap_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_KERNEL);
    sbi->snapshot_bitmap = kvzalloc(sbi->bitmap_bytes, GFP_KERNEL);
    if (!object || !bitmap_block || !sbi->snapshot_bitmap) {
        ret = -ENOMEM;
        goto out;
    }
    ret = infilfs_index_lookup(sb, snapshot_id, &catalog_block, &type);
    if (ret)
        goto out;
    if (type != INFILFS_OBJECT_SNAPSHOT_CATALOG) {
        ret = -EFSCORRUPTED;
        goto out;
    }
    ret = infilfs_read_object(sb, catalog_block,
                              INFILFS_OBJECT_SNAPSHOT_CATALOG,
                              snapshot_id, object);
    if (ret)
        goto out;
    header = (struct infilfs_object_header_disk *)object;
    payload = (struct infilfs_snapshot_catalog_payload_disk *)(header + 1);
    records = (struct infilfs_snapshot_record_disk *)(payload + 1);
    if (le32_to_cpu(payload->snapshot_count) != snapshots ||
        sizeof(*payload) + (size_t)snapshots * sizeof(*records) !=
            le32_to_cpu(header->payload_size)) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    for (i = 0; i < snapshots; ++i) {
        u64 start = le64_to_cpu(records[i].bitmap_start_block);
        u64 blocks = le64_to_cpu(records[i].bitmap_block_count);
        u64 b;

        if (blocks != sbi->bitmap_bytes / INFILFS_DISK_BLOCK_SIZE ||
            !start || start >= infilfs_volume_blocks(sbi) ||
            blocks > infilfs_volume_blocks(sbi) - start) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        for (b = 0; b < blocks; ++b) {
            size_t offset = (size_t)b * INFILFS_DISK_BLOCK_SIZE;
            size_t n;
            size_t j;

            ret = infilfs_read_allocated_block(sb, start + b, bitmap_block);
            if (ret)
                goto out;
            n = min_t(size_t, INFILFS_DISK_BLOCK_SIZE,
                      sbi->bitmap_bytes - offset);
            for (j = 0; j < n; ++j)
                sbi->snapshot_bitmap[offset + j] |= bitmap_block[j];
        }
    }
    ret = 0;
out:
    if (ret) {
        kvfree(sbi->snapshot_bitmap);
        sbi->snapshot_bitmap = NULL;
    }
    kfree(bitmap_block);
    kfree(object);
    return ret;
}

static int infilfs_rw_mount_init(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);
    struct infilfs_allocation_layout committed_layout = {0};
    u64 total = infilfs_volume_blocks(sbi);
    u32 snapshots = 0;
    int ret;

    if (!sb_rdonly(sb) &&
        !(le64_to_cpu(sbi->disk.incompat_flags) & INFILFS_INCOMPAT_INLINE_DATA))
        return -EOPNOTSUPP;

    ret = infilfs_allocation_map_load(
        sb, &sbi->disk, &sbi->bitmap, &sbi->bitmap_bytes,
        &committed_layout);
    if (ret)
        goto fail;
    infilfs_allocation_cache_replace(sbi, &committed_layout);

    ret = infilfs_parallel_allocator_mount_init(sb);
    if (ret)
        goto fail;

    write_lock(&sbi->bitmap_lock);
    sbi->visible_bitmap = sbi->bitmap;
    sbi->visible_bitmap_bytes = sbi->bitmap_bytes;
    write_unlock(&sbi->bitmap_lock);
    if (sb_rdonly(sb))
        return 0;

    ret = infilfs_rw_snapshot_count(sb, &snapshots);
    if (ret)
        goto fail;
    ret = infilfs_rw_snapshot_union(sb, snapshots);
    if (ret)
        goto fail;
    sbi->data_alloc_hint = 1;
    sbi->metadata_alloc_hint = total > 1 ? total - 1u : 1u;
    sbi->rw_enabled = true;
    return 0;

fail:
    infilfs_parallel_allocator_mount_destroy(sb);
    write_lock(&sbi->bitmap_lock);
    sbi->visible_bitmap = NULL;
    sbi->visible_bitmap_bytes = 0;
    write_unlock(&sbi->bitmap_lock);
    kvfree(sbi->bitmap);
    sbi->bitmap = NULL;
    sbi->bitmap_bytes = 0;
    kvfree(sbi->snapshot_bitmap);
    sbi->snapshot_bitmap = NULL;
    infilfs_allocation_layout_destroy(&committed_layout);
    infilfs_allocation_cache_destroy(sbi);
    return ret;
}

static void infilfs_rw_mount_destroy(struct super_block *sb)
{
    struct infilfs_sb_info *sbi = INFILFS_SB(sb);

    if (!sbi)
        return;
    infilfs_parallel_allocator_mount_destroy(sb);
    write_lock(&sbi->bitmap_lock);
    sbi->visible_bitmap = NULL;
    sbi->visible_bitmap_bytes = 0;
    sbi->validation_bitmap = NULL;
    sbi->validation_bitmap_bytes = 0;
    write_unlock(&sbi->bitmap_lock);
    kvfree(sbi->bitmap);
    sbi->bitmap = NULL;
    sbi->bitmap_bytes = 0;
    kvfree(sbi->snapshot_bitmap);
    sbi->snapshot_bitmap = NULL;
    infilfs_allocation_cache_destroy(sbi);
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

    /*
     * Transactions borrow the live in-memory allocation image. All native
     * mutation is serialized by write_lock, allocations are journalled below,
     * and frees remain deferred until publication. Rollback therefore clears
     * only journalled allocations instead of cloning the whole bitmap.
     */
    tx->bitmap = sbi->bitmap;
    tx->deferred_capacity = INFILFS_RW_FREE_RANGES_INITIAL;
    tx->deferred = kcalloc(tx->deferred_capacity, sizeof(*tx->deferred),
                           GFP_NOFS);
    if (!tx->deferred) {
        tx->bitmap = NULL;
        return -ENOMEM;
    }
    tx->sb = sb;
    tx->sbi = sbi;
    tx->bitmap_bytes = sbi->bitmap_bytes;
    tx->next_sb = sbi->disk;
    tx->generation = generation + 1u;
    tx->free_blocks = le64_to_cpu(sbi->disk.free_blocks);
    (void)infilfs_rw_free_extent_index_rebuild(tx);
    return 0;
}

static void infilfs_rw_tx_abort(struct infilfs_rw_tx *tx)
{
    if (tx->sbi && tx->generation) {
        /*
         * The final native wrapper temporarily exposes the working generation
         * through sbi->disk so transaction-private metadata can be read back.
         * No checkpoint means no generation advance: restore the committed
         * generation before exposing the live bitmap again.
         */
        tx->sbi->disk.generation = cpu_to_le64(tx->generation - 1u);
    }
    if (tx->sbi && tx->bitmap == tx->sbi->bitmap) {
        size_t r;

        write_lock(&tx->sbi->bitmap_lock);
        for (r = 0; r < tx->allocated_count; ++r) {
            u64 i;
            for (i = 0; i < tx->allocated[r].count; ++i)
                infilfs_rw_bitmap_set(tx->bitmap,
                    tx->allocated[r].start + i, false);
        }
        for (r = 0; r < tx->deferred_count; ++r) {
            u64 i;
            for (i = 0; i < tx->deferred[r].count; ++i) {
                u64 block = tx->deferred[r].start + i;
                size_t a;
                bool private = false;
                for (a = 0; a < tx->allocated_count; ++a) {
                    u64 start = tx->allocated[a].start;
                    u64 count = tx->allocated[a].count;
                    if (block >= start && block - start < count) {
                        private = true;
                        break;
                    }
                }
                if (!private)
                    infilfs_rw_bitmap_set(tx->bitmap, block, true);
            }
        }
        tx->sbi->visible_bitmap = tx->sbi->bitmap;
        tx->sbi->visible_bitmap_bytes = tx->sbi->bitmap_bytes;
        write_unlock(&tx->sbi->bitmap_lock);
    }
    tx->bitmap = NULL;
    kfree(tx->deferred);
    kfree(tx->allocated);
    kvfree(tx->free_extents);
    memset(tx, 0, sizeof(*tx));
}

int infilfs_rw_tx_defer_free(struct infilfs_rw_tx *tx,
                                    u64 start, u64 count)
{
    struct infilfs_rw_free_range *grown;

    if (!count || start >= infilfs_volume_blocks(tx->sbi) ||
        count > infilfs_volume_blocks(tx->sbi) - start)
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

static int infilfs_rw_tx_record_alloc(struct infilfs_rw_tx *tx,
                                      u64 start, u64 count)
{
    return infilfs_parallel_tx_claim(tx, start, count, false);
}

int infilfs_rw_tx_alloc(struct infilfs_rw_tx *tx, u64 count,
                               u64 *start_out)
{
    struct infilfs_sb_info *sbi = tx->sbi;
    u64 total = infilfs_volume_blocks(sbi);
    u64 hint;
    u64 candidate;
    u64 scanned;
    u64 run = 0;
    u64 start = 0;
    unsigned int attempts = 0;
    int ret;

retry:
    run = 0;
    start = 0;
    if (attempts++ >= INFILFS_ALLOCATION_RESERVATION_SHARDS)
        return -ENOSPC;
    if (!count || count > tx->free_blocks || total <= 1)
        return -ENOSPC;

    /*
     * Continue metadata allocation from the previous search position instead
     * of rescanning from the end of the whole device for every CoW block.
     * The cursor is volatile and may move across rollback without affecting
     * the allocation bitmap's authority.
     */
    hint = sbi->metadata_alloc_hint;
    if (hint < 1 || hint >= total)
        hint = total - 1u;
    candidate = hint;

    if (tx->free_extent_index_valid &&
        infilfs_rw_free_extent_choose_reverse(tx, count, hint, &start) &&
        !infilfs_parallel_range_reserved(sbi, start, count)) {
        run = count;
    } else {
        for (scanned = 0; scanned < total - 1u; ++scanned) {
            if (!infilfs_rw_bitmap_get(tx->bitmap, candidate) &&
                !infilfs_parallel_range_reserved(sbi, candidate, 1u)) {
                run++;
                if (run == count) {
                    start = candidate;
                    break;
                }
            } else {
                run = 0;
            }

            if (candidate == 1) {
                candidate = total - 1u;
                run = 0;
            } else {
                candidate--;
            }
        }
    }
    if (run != count)
        return -ENOSPC;

    ret = infilfs_rw_tx_record_alloc(tx, start, count);
    if (ret == -EAGAIN) {
        infilfs_rw_free_extent_index_invalidate(tx);
        goto retry;
    }
    if (ret)
        return ret;
    sbi->metadata_alloc_hint = start > 1 ? start - 1u : total - 1u;
    *start_out = start;
    return 0;
}

int infilfs_rw_tx_apply_deferred(struct infilfs_rw_tx *tx)
{
    size_t r;
    u64 i;

    write_lock(&tx->sbi->bitmap_lock);
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
            if (checkpoint || !infilfs_rw_bitmap_get(tx->bitmap, block) ||
                (tx->sbi->snapshot_bitmap &&
                 infilfs_rw_bitmap_get(tx->sbi->snapshot_bitmap, block)))
                continue;
            infilfs_rw_bitmap_set(tx->bitmap, block, false);
            tx->free_blocks++;
        }
    }
    write_unlock(&tx->sbi->bitmap_lock);
    return 0;
}


static int infilfs_rw_tx_commit(struct infilfs_rw_tx *tx)
{
    struct infilfs_sb_info *sbi = tx->sbi;
    struct infilfs_allocation_layout next_allocation = {0};
    u8 *sb_block;
    unsigned int first;
    unsigned int n;
    int ret;
    int replica_error = 0;

    /*
     * All transaction-private file data and metadata are still unreachable at
     * this point. Build/stage the replacement allocation tree first, then use
     * one durability barrier for the entire dependency graph. There is no
     * safety benefit in flushing file data before staging the allocation tree:
     * neither can become reachable until a checkpoint is durable.
     */
    ret = infilfs_rw_allocation_map_publish(tx, &next_allocation);
    if (ret) {
        infilfs_allocation_layout_destroy(&next_allocation);
        return ret;
    }
    ret = sync_blockdev(tx->sb->s_bdev);
    if (ret) {
        infilfs_allocation_layout_destroy(&next_allocation);
        return ret;
    }

    sb_block = kmalloc(INFILFS_DISK_BLOCK_SIZE, GFP_NOFS);
    if (!sb_block) {
        infilfs_allocation_layout_destroy(&next_allocation);
        return -ENOMEM;
    }
    infilfs_rw_encode_superblock(&tx->next_sb, sb_block);
    first = (unsigned int)(tx->generation % INFILFS_CHECKPOINT_COUNT);

    /*
     * The dependency graph above is already durable, so all checkpoint
     * replicas can be issued before the second and final durability barrier.
     * If power fails during this phase, recovery may see generation N or any
     * complete generation-N+1 checkpoint; either is safe because every N+1
     * dependency was flushed before checkpoint writes began.
     */
    ret = infilfs_rw_write_block(tx->sb,
        le64_to_cpu(tx->next_sb.checkpoint_block[first]), sb_block);
    if (ret) {
        sbi->write_poisoned = true;
        sbi->checkpoint_repair_needed = true;
        pr_warn("InfiltratorFS: primary checkpoint write failed; further writes disabled until remount\n");
        infilfs_allocation_layout_destroy(&next_allocation);
        kfree(sb_block);
        return ret;
    }

    for (n = 1; n < INFILFS_CHECKPOINT_COUNT; ++n) {
        unsigned int idx = (first + n) % INFILFS_CHECKPOINT_COUNT;
        ret = infilfs_rw_write_block(tx->sb,
            le64_to_cpu(tx->next_sb.checkpoint_block[idx]), sb_block);
        if (ret && !replica_error)
            replica_error = ret;
    }

    ret = sync_blockdev(tx->sb->s_bdev);
    if (ret) {
        /*
         * At least the primary checkpoint write was issued. A failed flush
         * cannot prove whether generation N+1 reached stable storage, so keep
         * the existing fail-closed remount/recovery rule.
         */
        sbi->write_poisoned = true;
        sbi->checkpoint_repair_needed = true;
        pr_warn("InfiltratorFS: checkpoint durability indeterminate; further writes disabled until remount\n");
        infilfs_allocation_layout_destroy(&next_allocation);
        kfree(sb_block);
        return ret;
    }
    kfree(sb_block);

    /*
     * The primary checkpoint is now durable, which is the atomic publication
     * point. Replace the volatile allocation layout only after that barrier.
     */
    infilfs_allocation_cache_replace(sbi, &next_allocation);
    sbi->disk = tx->next_sb;
    write_lock(&sbi->bitmap_lock);
    sbi->visible_bitmap = sbi->bitmap;
    sbi->visible_bitmap_bytes = sbi->bitmap_bytes;
    write_unlock(&sbi->bitmap_lock);

    if (replica_error) {
        sbi->write_poisoned = true;
        sbi->checkpoint_repair_needed = true;
        pr_warn("InfiltratorFS: native RW checkpoint committed but a replica write failed; further writes disabled until remount\n");
    } else {
        sbi->checkpoint_repair_needed = false;
    }
    kfree(tx->deferred);
    tx->deferred = NULL;
    tx->deferred_count = 0;
    tx->deferred_capacity = 0;
    kfree(tx->allocated);
    tx->allocated = NULL;
    tx->allocated_count = 0;
    tx->allocated_capacity = 0;
    kvfree(tx->free_extents);
    tx->free_extents = NULL;
    tx->free_extent_count = 0;
    tx->free_extent_capacity = 0;
    tx->free_extent_index_valid = false;
    tx->bitmap = NULL;
    return 0;
}

void infilfs_rw_init_page(u8 *block, const u8 magic[8],
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

            ret = infilfs_read_allocated_block(tx->sb, block, page_block);
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

            ret = infilfs_read_allocated_block(tx->sb, block, page_block);
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
                                    umode_t mode, u64 portable_flags,
                                    const u8 object_id[16],
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
        file->attributes.portable_flags = cpu_to_le64(portable_flags);
        file->data_checksum_type = cpu_to_le32(INFILFS_CHECKSUM_SHA256);
    } else if (type == INFILFS_OBJECT_DIRECTORY) {
        struct infilfs_directory_payload_disk *dir =
            (struct infilfs_directory_payload_disk *)(header + 1);
        header->payload_size = cpu_to_le32(sizeof(*dir));
        infilfs_rw_fill_common_attributes(&dir->attributes, &dir->posix,
                                          mode, 2);
        dir->attributes.portable_flags = cpu_to_le64(portable_flags);
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



static int infilfs_rw_create_named_child(
    struct inode *dir, const struct qstr *name, umode_t mode, u16 type,
    u64 portable_flags, u8 object_id_out[16], u64 *block_out)
{
    struct infilfs_inode_info *dir_ii = INFILFS_I(dir);
    struct infilfs_sb_info *sbi = INFILFS_SB(dir->i_sb);
    struct infilfs_rw_tx tx;
    u8 object_id[16];
    u64 object_block;
    u64 new_dir_block;
    int ret;

    if (!name || !name->len || name->len > INFILFS_NAME_MAX ||
        !infilfs_rw_utf8_valid(name->name, name->len) ||
        (name->len == 1 && name->name[0] == '.') ||
        (name->len == 2 && name->name[0] == '.' && name->name[1] == '.'))
        return -EINVAL;

    mutex_lock(&sbi->write_lock);
    ret = infilfs_rw_generate_id(dir->i_sb, object_id);
    if (ret)
        goto unlock;
    ret = infilfs_rw_tx_begin(dir->i_sb, &tx);
    if (ret)
        goto unlock;
    ret = infilfs_rw_create_object(&tx, dir, type, mode, portable_flags,
                                   object_id, &object_block);
    if (ret)
        goto abort;
    ret = infilfs_rw_rebuild_directory(&tx, dir, name->name,
                                       object_id, type, &new_dir_block);
    if (ret)
        goto abort;
    ret = infilfs_rw_rebuild_index(&tx,
        dir_ii->object_id, new_dir_block, INFILFS_OBJECT_DIRECTORY,
        object_id, object_block, type);
    if (ret)
        goto abort;
    ret = infilfs_rw_tx_commit(&tx);
    if (ret)
        goto abort;
    dir_ii->object_block = new_dir_block;
    infilfs_refresh_inode_blocks_after_commit(dir);
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

static int infilfs_rw_create_child(struct inode *dir, struct dentry *dentry,
                                   umode_t mode, u16 type,
                                   u8 object_id_out[16], u64 *block_out)
{
    return infilfs_rw_create_named_child(dir, &dentry->d_name, mode, type, 0,
                                         object_id_out, block_out);
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

static __maybe_unused int infilfs_file_fsync(struct file *file, loff_t start, loff_t end,
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static struct dentry *infilfs_rw_mkdir(struct mnt_idmap *idmap,
                                       struct inode *dir,
                                       struct dentry *dentry, umode_t mode)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
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
    if (ret) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
        return ERR_PTR(ret);
#else
        return ret;
#endif
    }
    inode = infilfs_get_inode(dir->i_sb, block, INFILFS_OBJECT_DIRECTORY, id);
    if (IS_ERR(inode)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
        return ERR_PTR(PTR_ERR(inode));
#else
        return PTR_ERR(inode);
#endif
    }
    d_instantiate(dentry, inode);
    inc_nlink(dir);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
    return NULL;
#else
    return 0;
#endif
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
