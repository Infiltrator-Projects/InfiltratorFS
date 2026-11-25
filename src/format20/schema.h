// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_EXPERIMENTAL_FORMAT20_SCHEMA_H
#define INFILFS_EXPERIMENTAL_FORMAT20_SCHEMA_H

#include <stddef.h>
#include <stdint.h>
#include "infilfs/status.h"

/*
 * S0 experimental byte contract, deliberately private to conformance tests.
 * Neither the probe magic nor its record/page kind is a production format
 * identifier. Do not include this header in a formatter or mounted reader.
 * Host structures below are decoded values, never serialized C layouts.
 */
#define INFS20_PAGE_BYTES 4096u
#define INFS20_HEADER_BYTES 192u
#define INFS20_PAYLOAD_BYTES (INFS20_PAGE_BYTES - INFS20_HEADER_BYTES)
#define INFS20_ID_BYTES 16u
#define INFS20_DIGEST_BYTES 32u
#define INFS20_PROBE_KIND 1u
#define INFS20_TREE_KIND 2u
#define INFS20_COMMIT_KIND 3u
#define INFS20_PROBE_SCHEMA 1u
#define INFS20_LANE_COUNT 3u
#define INFS20_RECORD_HEADER_BYTES 8u
/* A member's complete capacity, not merely its final byte address, must
 * be representable by the existing unsigned 64-bit storage byte-count API. */
#define INFS20_MAX_MEMBER_BLOCKS (UINT64_MAX / UINT64_C(4096))
#define INFS20_MAX_BLOCK (INFS20_MAX_MEMBER_BLOCKS - UINT64_C(1))

enum infs20_page_offset {
    INFS20_OFF_MAGIC = 0,
    INFS20_OFF_MAJOR = 8,
    INFS20_OFF_MINOR = 10,
    INFS20_OFF_KIND = 12,
    INFS20_OFF_SCHEMA = 14,
    INFS20_OFF_HEADER = 16,
    INFS20_OFF_REQUIRED_FLAGS = 18,
    INFS20_OFF_PAYLOAD_SIZE = 20,
    INFS20_OFF_SET = 24,
    INFS20_OFF_SCOPE = 40,
    INFS20_OFF_PAGE = 56,
    INFS20_OFF_SEQUENCE = 72,
    INFS20_OFF_DIGEST = 80,
    INFS20_OFF_MEMBER = 112,
    INFS20_OFF_BLOCK = 128,
    INFS20_OFF_LANE = 136,
    INFS20_OFF_PHYSICAL_FLAGS = 138,
    INFS20_OFF_RESERVED_A = 140,
    INFS20_OFF_CRC = 144,
    INFS20_OFF_RESERVED_B = 152,
    INFS20_OFF_PAYLOAD = 192
};

struct infs20_page_identity {
    uint8_t set_id[INFS20_ID_BYTES];
    uint8_t scope_id[INFS20_ID_BYTES];
    uint8_t page_id[INFS20_ID_BYTES];
    uint8_t member_id[INFS20_ID_BYTES];
    uint64_t sequence;
    uint64_t block;
    uint16_t lane;
};

struct infs20_decoded_page {
    struct infs20_page_identity identity;
    uint32_t payload_size;
    uint16_t kind;
    uint8_t digest[INFS20_DIGEST_BYTES];
};

struct infs20_record_view {
    uint16_t kind;
    const uint8_t *key;
    size_t key_size;
    const uint8_t *value;
    size_t value_size;
    size_t consumed;
};

/*
 * Encode one experimental, scoped probe page. The output is unchanged on
 * failure. A zero-length payload may have a null pointer. Input and output
 * byte buffers may overlap because publication uses a private staging page.
 * This userspace prototype intentionally does not perform storage I/O.
 */
infs_status infs20_page_encode(uint8_t *out, size_t out_size,
                              const struct infs20_page_identity *identity,
                              const uint8_t *payload, size_t payload_size);

/*
 * Verify physical placement against caller-supplied member and capacity, then
 * canonical encoding and logical digest. Only publish decoded output on
 * success. This proves record-level integrity, not membership of a committed
 * tree, or adversarial authentication. Do not pass untrusted on-page geometry
 * as the independently expected geometry.
 */
/* Typed experimental pages share the same envelope. No production kind is registered. */
infs_status infs20_page_encode_kind(uint8_t *out, size_t out_size,
                                   const struct infs20_page_identity *identity,
                                   uint16_t kind, const uint8_t *payload,
                                   size_t payload_size);

infs_status infs20_page_decode(const uint8_t *page, size_t page_size,
                              const uint8_t expected_member[INFS20_ID_BYTES],
                              uint64_t expected_block, uint64_t member_blocks,
                              struct infs20_decoded_page *out);

/*
 * Decode a single experimental typed record prefix within an already bounded
 * payload. Returned views borrow the input storage; no allocation occurs.
 * This checks framing only, not tree ordering, typed semantics or reachability.
 */
infs_status infs20_record_decode(const uint8_t *bytes, size_t size,
                                struct infs20_record_view *out);

#endif
