// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/schema.h"

#include <stdio.h>
#include <string.h>
#include "infilfs/checksum.h"
#include "infilfs/endian.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "format20-schema:%d: %s\n", __LINE__, #expr); \
    return 1; } } while (0)

/* Independent fixed vector: literal field bytes, SHA-256 preimage and
 * CRC64-ECMA over the 4096-byte zero-padded probe page. This is not generated
 * by the implementation under test and must not be regenerated to hide a
 * format change. */
static const uint8_t expected_digest[32] = {
    0x2d,0x18,0xbd,0x9a,0xb9,0xe0,0xf5,0x91,
    0xd0,0xa2,0x72,0xe7,0x88,0x93,0xc2,0x1a,
    0xbc,0x18,0x8e,0xfd,0x35,0x41,0x32,0xe7,
    0x96,0x63,0xca,0x3f,0xd1,0x42,0xb6,0xcc
};
static const uint8_t expected_block_bytes[8] =
    {0x88,0x77,0x66,0x55,0x44,0x33,0x02,0x00};

static void refresh_crc(uint8_t *page)
{
    memset(page + INFS20_OFF_CRC, 0, 8);
    infs_store_le64(page + INFS20_OFF_CRC,
                    infs_crc64_ecma(page, INFS20_PAGE_BYTES));
}

static int check_page(void)
{
    struct infs20_page_identity id = {0};
    for (unsigned i = 0; i < INFS20_ID_BYTES; ++i) {
        id.set_id[i] = (uint8_t)(i + 1);
        id.scope_id[i] = (uint8_t)(i + 17);
        id.page_id[i] = (uint8_t)(i + 33);
        id.member_id[i] = (uint8_t)(i + 49);
    }
    id.sequence = UINT64_C(0x0102030405060708);
    id.block = UINT64_C(0x0002334455667788);
    id.lane = 1;
    uint8_t page[INFS20_PAGE_BYTES], copy[INFS20_PAGE_BYTES];
    struct infs20_decoded_page decoded;
    const uint8_t abc[] = {'a','b','c'};
    CHECK(infs20_page_encode(page, sizeof(page), &id, abc, sizeof(abc)) == 0);
    CHECK(memcmp(page, "INF20S0!", 8) == 0);
    CHECK(page[10] == 20 && page[11] == 0 && page[16] == 192);
    CHECK(memcmp(page + INFS20_OFF_BLOCK, expected_block_bytes, 8) == 0);
    CHECK(memcmp(page + INFS20_OFF_DIGEST, expected_digest, 32) == 0);
    CHECK(infs_load_le64(page + INFS20_OFF_CRC) == UINT64_C(0x8ccb56083cf84aa1));
    memcpy(copy, page, sizeof(copy));
    CHECK(infs20_page_decode(page, sizeof(page), id.member_id, id.block,
                             id.block + 1, &decoded) == 0);
    CHECK(memcmp(copy, page, sizeof(page)) == 0);
    CHECK(decoded.payload_size == 3 && decoded.identity.sequence == id.sequence);
    CHECK(decoded.identity.block == id.block && decoded.identity.lane == id.lane);
    CHECK(memcmp(decoded.identity.set_id, id.set_id, 16) == 0);
    CHECK(memcmp(decoded.identity.scope_id, id.scope_id, 16) == 0);
    CHECK(memcmp(decoded.identity.page_id, id.page_id, 16) == 0);
    CHECK(memcmp(decoded.identity.member_id, id.member_id, 16) == 0);
    CHECK(memcmp(decoded.digest, expected_digest, 32) == 0);

    /* Replicas change physical placement but not immutable logical identity. */
    struct infs20_page_identity replica = id;
    replica.member_id[0] ^= 0x80;
    replica.block = 17;
    replica.lane = 2;
    CHECK(infs20_page_encode(copy, sizeof(copy), &replica, abc, sizeof(abc)) == 0);
    CHECK(memcmp(copy + INFS20_OFF_DIGEST, expected_digest, 32) == 0);
    CHECK(infs_load_le64(copy + INFS20_OFF_CRC) !=
          infs_load_le64(page + INFS20_OFF_CRC));
    CHECK(infs20_page_decode(copy, sizeof(copy), replica.member_id, 17, 18, &decoded) == 0);
    CHECK(infs20_page_decode(copy, sizeof(copy), id.member_id, 17, 18, &decoded) ==
          INFS_STATUS_CORRUPT);

    CHECK(infs20_page_decode(page, sizeof(page) - 1, id.member_id, id.block,
                             id.block + 1, &decoded) == INFS_STATUS_CORRUPT);
    CHECK(infs20_page_decode(page, sizeof(page), id.member_id, id.block,
                             id.block, &decoded) == INFS_STATUS_INVALID_ARGUMENT);
    CHECK(infs20_page_decode(page, sizeof(page), id.member_id, id.block - 1,
                             id.block + 1, &decoded) == INFS_STATUS_CORRUPT);
    CHECK(infs20_page_decode(page, sizeof(page), id.member_id, id.block,
                             UINT64_MAX, &decoded) == INFS_STATUS_INVALID_ARGUMENT);

    /* Every physical byte is CRC-covered. Recomputed CRC below tests the
     * deeper invariants separately instead of merely exercising a checksum. */
    for (size_t i = 8; i < sizeof(page); ++i) {
        memcpy(copy, page, sizeof(copy));
        copy[i] ^= 1;
        CHECK(infs20_page_decode(copy, sizeof(copy), id.member_id, id.block,
                                 id.block + 1, &decoded) == INFS_STATUS_CORRUPT);
    }
    struct mutation { size_t offset; uint8_t value; infs_status status; };
    const struct mutation mutations[] = {
        {INFS20_OFF_MINOR, 21, INFS_STATUS_NOT_SUPPORTED},
        {INFS20_OFF_KIND, 255, INFS_STATUS_NOT_SUPPORTED},
        {INFS20_OFF_SCHEMA, 2, INFS_STATUS_NOT_SUPPORTED},
        {INFS20_OFF_REQUIRED_FLAGS, 1, INFS_STATUS_NOT_SUPPORTED},
        {INFS20_OFF_PHYSICAL_FLAGS, 1, INFS_STATUS_NOT_SUPPORTED},
        {INFS20_OFF_HEADER, 191, INFS_STATUS_CORRUPT},
        {INFS20_OFF_PAYLOAD_SIZE + 3, 1, INFS_STATUS_CORRUPT},
        {INFS20_OFF_RESERVED_A, 1, INFS_STATUS_CORRUPT},
        {INFS20_OFF_RESERVED_B, 1, INFS_STATUS_CORRUPT},
        {INFS20_OFF_PAYLOAD + 3, 1, INFS_STATUS_CORRUPT},
        {INFS20_OFF_PAYLOAD, 'z', INFS_STATUS_CORRUPT},
        {INFS20_OFF_DIGEST, 0, INFS_STATUS_CORRUPT},
        {INFS20_OFF_LANE, 3, INFS_STATUS_CORRUPT},
        {INFS20_OFF_BLOCK, 0, INFS_STATUS_CORRUPT}
    };
    for (size_t i = 0; i < sizeof(mutations)/sizeof(mutations[0]); ++i) {
        memcpy(copy, page, sizeof(copy));
        copy[mutations[i].offset] = mutations[i].value;
        refresh_crc(copy);
        memset(&decoded, 0xa5, sizeof(decoded));
        uint8_t before[sizeof(decoded)];
        memcpy(before, &decoded, sizeof(before));
        CHECK(infs20_page_decode(copy, sizeof(copy), id.member_id, id.block,
                                 id.block + 1, &decoded) == mutations[i].status);
        CHECK(memcmp(before, &decoded, sizeof(before)) == 0);
    }
    memcpy(copy, page, sizeof(copy));
    memset(copy + INFS20_OFF_SET, 0, INFS20_ID_BYTES);
    refresh_crc(copy);
    CHECK(infs20_page_decode(copy, sizeof(copy), id.member_id, id.block,
                             id.block + 1, &decoded) == INFS_STATUS_CORRUPT);
    memcpy(copy, page, sizeof(copy));
    memset(copy + INFS20_OFF_SEQUENCE, 0, 8);
    refresh_crc(copy);
    CHECK(infs20_page_decode(copy, sizeof(copy), id.member_id, id.block,
                             id.block + 1, &decoded) == INFS_STATUS_CORRUPT);
    memcpy(copy, page, sizeof(copy));
    memcpy(copy, "INFS2026", 8); /* existing format magic is not a probe */
    CHECK(infs20_page_decode(copy, sizeof(copy), id.member_id, id.block,
                             id.block + 1, &decoded) == INFS_STATUS_NOT_SUPPORTED);

    /* Boundary lengths, input/output aliasing and zero-write refusal. */
    CHECK(infs20_page_encode(copy, sizeof(copy), &id, NULL, 0) == 0);
    CHECK(infs20_page_decode(copy, sizeof(copy), id.member_id, id.block,
                             id.block + 1, &decoded) == 0 && decoded.payload_size == 0);
    memset(copy + INFS20_OFF_PAYLOAD, 0x5a, INFS20_PAYLOAD_BYTES);
    CHECK(infs20_page_encode(copy, sizeof(copy), &id, copy + INFS20_OFF_PAYLOAD,
                             INFS20_PAYLOAD_BYTES) == 0);
    CHECK(infs20_page_decode(copy, sizeof(copy), id.member_id, id.block,
                             id.block + 1, &decoded) == 0 &&
          decoded.payload_size == INFS20_PAYLOAD_BYTES);
    CHECK(copy[INFS20_PAGE_BYTES - 1] == 0x5a);
    memcpy(page, copy, sizeof(page));
    CHECK(infs20_page_encode(copy, sizeof(copy), &id, abc,
                             INFS20_PAYLOAD_BYTES + 1) == INFS_STATUS_NO_SPACE);
    CHECK(memcmp(copy, page, sizeof(copy)) == 0);
    id.block = INFS20_MAX_BLOCK + 1;
    CHECK(infs20_page_encode(copy, sizeof(copy), &id, abc, sizeof(abc)) ==
          INFS_STATUS_INVALID_ARGUMENT);
    CHECK(memcmp(copy, page, sizeof(copy)) == 0);
    id.block = INFS20_MAX_BLOCK;
    CHECK(infs20_page_encode(copy, sizeof(copy), &id, abc, sizeof(abc)) == 0);
    CHECK(infs20_page_decode(copy, sizeof(copy), id.member_id, id.block,
                             INFS20_MAX_MEMBER_BLOCKS, &decoded) == 0);
    CHECK(infs20_page_decode(copy, sizeof(copy), id.member_id, id.block,
                             INFS20_MAX_MEMBER_BLOCKS + 1, &decoded) ==
          INFS_STATUS_INVALID_ARGUMENT);
    return 0;
}

static int check_record(void)
{
    uint8_t bytes[] = {1,0,0,0,2,0,3,0,'k','1','a','b','c',0};
    struct infs20_record_view r;
    CHECK(infs20_record_decode(bytes, sizeof(bytes), &r) == 0);
    CHECK(r.kind == 1 && r.key_size == 2 && r.value_size == 3 && r.consumed == 13);
    CHECK(memcmp(r.key, "k1", 2) == 0 && memcmp(r.value, "abc", 3) == 0);
    for (size_t n = 0; n < 13; ++n)
        CHECK(infs20_record_decode(bytes, n, &r) == INFS_STATUS_CORRUPT);
    bytes[4] = 0;
    CHECK(infs20_record_decode(bytes, sizeof(bytes), &r) == INFS_STATUS_CORRUPT);
    bytes[4] = 2;
    bytes[7] = 0xff;
    CHECK(infs20_record_decode(bytes, sizeof(bytes), &r) == INFS_STATUS_CORRUPT);
    bytes[7] = 0;
    bytes[2] = 1;
    CHECK(infs20_record_decode(bytes, sizeof(bytes), &r) == INFS_STATUS_NOT_SUPPORTED);
    bytes[2] = 0;
    bytes[0] = 2;
    CHECK(infs20_record_decode(bytes, sizeof(bytes), &r) == INFS_STATUS_NOT_SUPPORTED);
    bytes[0] = 1;
    bytes[6] = 0;
    CHECK(infs20_record_decode(bytes, 10, &r) == 0 && r.value_size == 0);
    CHECK(infs20_record_decode(bytes, INFS20_PAYLOAD_BYTES + 1, &r) ==
          INFS_STATUS_CORRUPT);
    return 0;
}

int main(void)
{
    if (check_page() || check_record())
        return 1;
    puts("format20-schema: PASS (experimental S0 page and record framing)");
    return 0;
}
