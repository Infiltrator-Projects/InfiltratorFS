// SPDX-License-Identifier: GPL-3.0-or-later
#include "schema.h"

#include <string.h>
#include "infilfs/checksum.h"
#include "infilfs/endian.h"

static const uint8_t probe_magic[8] = {'I','N','F','2','0','S','0','!'};
static const uint8_t digest_domain[] = "INFS20/PAGE";

_Static_assert(INFS20_OFF_PAYLOAD == INFS20_HEADER_BYTES, "header boundary");
_Static_assert(INFS20_HEADER_BYTES + INFS20_PAYLOAD_BYTES == INFS20_PAGE_BYTES,
               "page boundary");
_Static_assert(INFS20_OFF_DIGEST + INFS20_DIGEST_BYTES == INFS20_OFF_MEMBER,
               "logical and physical envelope boundary");
_Static_assert(sizeof(digest_domain) - 1u == 11u, "domain length");

static int bytes_zero(const uint8_t *bytes, size_t length)
{
    uint8_t combined = 0;
    for (size_t i = 0; i < length; ++i)
        combined |= bytes[i];
    return combined == 0;
}

static int identity_valid(const struct infs20_page_identity *id)
{
    return !bytes_zero(id->set_id, INFS20_ID_BYTES) &&
           !bytes_zero(id->scope_id, INFS20_ID_BYTES) &&
           !bytes_zero(id->page_id, INFS20_ID_BYTES) &&
           !bytes_zero(id->member_id, INFS20_ID_BYTES) &&
           id->sequence != 0 && id->block <= INFS20_MAX_BLOCK &&
           id->lane < INFS20_LANE_COUNT;
}

/*
 * The candidate digest preimage is precisely:
 * LE16(11) || ASCII("INFS20/PAGE") || page[0..79] || meaningful payload.
 * All physical replica fields are outside the first 80 bytes. Replica copies
 * therefore have one logical digest even at different member addresses.
 * A typed location payload, when designed, will still hash its actual data
 * locations; excluding the envelope's placement does not exclude payload.
 *
 * Fixed bounded scratch is appropriate for this userspace-only prototype.
 * A future kernel/firmware implementation must use an explicit caller-owned
 * workspace or streaming hash contract rather than inherit large stack frames.
 */
static void logical_digest(const uint8_t *page, uint32_t payload_size,
                           uint8_t out[INFS20_DIGEST_BYTES])
{
    enum { prefix_size = 2 + sizeof(digest_domain) - 1 + INFS20_OFF_DIGEST };
    uint8_t preimage[prefix_size + INFS20_PAYLOAD_BYTES];
    infs_store_le16(preimage, sizeof(digest_domain) - 1u);
    memcpy(preimage + 2, digest_domain, sizeof(digest_domain) - 1u);
    memcpy(preimage + 2 + sizeof(digest_domain) - 1u, page, INFS20_OFF_DIGEST);
    memcpy(preimage + prefix_size, page + INFS20_OFF_PAYLOAD, payload_size);
    infs_sha256(preimage, prefix_size + payload_size, out);
}

/* Never modify the caller's input when verifying physical CRC. */
static uint64_t physical_crc(const uint8_t *page)
{
    uint8_t scratch[INFS20_PAGE_BYTES];
    memcpy(scratch, page, sizeof(scratch));
    memset(scratch + INFS20_OFF_CRC, 0, sizeof(uint64_t));
    return infs_crc64_ecma(scratch, sizeof(scratch));
}

infs_status infs20_page_encode(uint8_t *out, size_t out_size,
                              const struct infs20_page_identity *identity,
                              const uint8_t *payload, size_t payload_size)
{
    if (!out || !identity || out_size != INFS20_PAGE_BYTES ||
        (payload_size != 0 && !payload))
        return INFS_STATUS_INVALID_ARGUMENT;
    if (payload_size > INFS20_PAYLOAD_BYTES)
        return INFS_STATUS_NO_SPACE;
    if (!identity_valid(identity))
        return INFS_STATUS_INVALID_ARGUMENT;

    uint8_t page[INFS20_PAGE_BYTES] = {0};
    memcpy(page + INFS20_OFF_MAGIC, probe_magic, sizeof(probe_magic));
    infs_store_le16(page + INFS20_OFF_MAJOR, 0);
    infs_store_le16(page + INFS20_OFF_MINOR, 20);
    infs_store_le16(page + INFS20_OFF_KIND, INFS20_PROBE_KIND);
    infs_store_le16(page + INFS20_OFF_SCHEMA, INFS20_PROBE_SCHEMA);
    infs_store_le16(page + INFS20_OFF_HEADER, INFS20_HEADER_BYTES);
    infs_store_le32(page + INFS20_OFF_PAYLOAD_SIZE, (uint32_t)payload_size);
    memcpy(page + INFS20_OFF_SET, identity->set_id, INFS20_ID_BYTES);
    memcpy(page + INFS20_OFF_SCOPE, identity->scope_id, INFS20_ID_BYTES);
    memcpy(page + INFS20_OFF_PAGE, identity->page_id, INFS20_ID_BYTES);
    infs_store_le64(page + INFS20_OFF_SEQUENCE, identity->sequence);
    memcpy(page + INFS20_OFF_MEMBER, identity->member_id, INFS20_ID_BYTES);
    infs_store_le64(page + INFS20_OFF_BLOCK, identity->block);
    infs_store_le16(page + INFS20_OFF_LANE, identity->lane);
    if (payload_size)
        memcpy(page + INFS20_OFF_PAYLOAD, payload, payload_size);
    logical_digest(page, (uint32_t)payload_size, page + INFS20_OFF_DIGEST);
    infs_store_le64(page + INFS20_OFF_CRC, infs_crc64_ecma(page, sizeof(page)));
    memcpy(out, page, sizeof(page));
    return INFS_STATUS_OK;
}

infs_status infs20_page_decode(const uint8_t *page, size_t page_size,
                              const uint8_t expected_member[INFS20_ID_BYTES],
                              uint64_t expected_block, uint64_t member_blocks,
                              struct infs20_decoded_page *out)
{
    if (!page || !expected_member || !out ||
        bytes_zero(expected_member, INFS20_ID_BYTES) ||
        expected_block > INFS20_MAX_BLOCK || expected_block >= member_blocks ||
        member_blocks > INFS20_MAX_MEMBER_BLOCKS)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (page_size != INFS20_PAGE_BYTES)
        return INFS_STATUS_CORRUPT;
    if (memcmp(page + INFS20_OFF_MAGIC, probe_magic, sizeof(probe_magic)) != 0)
        return INFS_STATUS_NOT_SUPPORTED;
    if (physical_crc(page) != infs_load_le64(page + INFS20_OFF_CRC))
        return INFS_STATUS_CORRUPT;
    /* Unknown semantics are not corruption and must not be silently admitted. */
    if (infs_load_le16(page + INFS20_OFF_MAJOR) != 0 ||
        infs_load_le16(page + INFS20_OFF_MINOR) != 20 ||
        infs_load_le16(page + INFS20_OFF_KIND) != INFS20_PROBE_KIND ||
        infs_load_le16(page + INFS20_OFF_SCHEMA) != INFS20_PROBE_SCHEMA ||
        infs_load_le16(page + INFS20_OFF_REQUIRED_FLAGS) != 0 ||
        infs_load_le16(page + INFS20_OFF_PHYSICAL_FLAGS) != 0)
        return INFS_STATUS_NOT_SUPPORTED;
    if (infs_load_le16(page + INFS20_OFF_HEADER) != INFS20_HEADER_BYTES ||
        !bytes_zero(page + INFS20_OFF_RESERVED_A, 4) ||
        !bytes_zero(page + INFS20_OFF_RESERVED_B, 40))
        return INFS_STATUS_CORRUPT;

    struct infs20_decoded_page decoded = {0};
    decoded.payload_size = infs_load_le32(page + INFS20_OFF_PAYLOAD_SIZE);
    if (decoded.payload_size > INFS20_PAYLOAD_BYTES ||
        !bytes_zero(page + INFS20_OFF_PAYLOAD + decoded.payload_size,
                    INFS20_PAYLOAD_BYTES - decoded.payload_size))
        return INFS_STATUS_CORRUPT;
    memcpy(decoded.identity.set_id, page + INFS20_OFF_SET, INFS20_ID_BYTES);
    memcpy(decoded.identity.scope_id, page + INFS20_OFF_SCOPE, INFS20_ID_BYTES);
    memcpy(decoded.identity.page_id, page + INFS20_OFF_PAGE, INFS20_ID_BYTES);
    memcpy(decoded.identity.member_id, page + INFS20_OFF_MEMBER, INFS20_ID_BYTES);
    decoded.identity.sequence = infs_load_le64(page + INFS20_OFF_SEQUENCE);
    decoded.identity.block = infs_load_le64(page + INFS20_OFF_BLOCK);
    decoded.identity.lane = infs_load_le16(page + INFS20_OFF_LANE);
    if (!identity_valid(&decoded.identity) ||
        decoded.identity.block != expected_block ||
        memcmp(decoded.identity.member_id, expected_member, INFS20_ID_BYTES) != 0)
        return INFS_STATUS_CORRUPT;
    logical_digest(page, decoded.payload_size, decoded.digest);
    if (memcmp(decoded.digest, page + INFS20_OFF_DIGEST, INFS20_DIGEST_BYTES) != 0)
        return INFS_STATUS_CORRUPT;
    *out = decoded;
    return INFS_STATUS_OK;
}

infs_status infs20_record_decode(const uint8_t *bytes, size_t size,
                                struct infs20_record_view *out)
{
    if (!bytes || !out)
        return INFS_STATUS_INVALID_ARGUMENT;
    if (size < INFS20_RECORD_HEADER_BYTES || size > INFS20_PAYLOAD_BYTES)
        return INFS_STATUS_CORRUPT;
    uint16_t kind = infs_load_le16(bytes);
    uint16_t flags = infs_load_le16(bytes + 2);
    size_t key = infs_load_le16(bytes + 4);
    size_t value = infs_load_le16(bytes + 6);
    size_t remaining = size - INFS20_RECORD_HEADER_BYTES;
    /* Subtraction checks prevent a length sum from escaping the input span. */
    if (!key || key > remaining || value > remaining - key)
        return INFS_STATUS_CORRUPT;
    if (kind != INFS20_PROBE_KIND || flags != 0)
        return INFS_STATUS_NOT_SUPPORTED;
    struct infs20_record_view record = {
        kind, bytes + INFS20_RECORD_HEADER_BYTES, key,
        bytes + INFS20_RECORD_HEADER_BYTES + key, value,
        INFS20_RECORD_HEADER_BYTES + key + value
    };
    *out = record;
    return INFS_STATUS_OK;
}
