// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_EXPERIMENTAL_FORMAT20_SPACE_WIRE_H
#define INFILFS_EXPERIMENTAL_FORMAT20_SPACE_WIRE_H
#include "engine.h"
#include "space.h"

/*
 * Bounded S1/S2 persistence bridge: six authenticated leaf payloads for the
 * allocation descriptor, location, owner, direct reference, arena and reclaim
 * roots. This model deliberately uses ordered fixed-size records and ordinal
 * keys. Final production keys and scalable multi-page mutation remain open.
 * The owner index is derived and must independently agree with the descriptors,
 * physical locations, reservations and retirement records on reopen.
 */
#define INFS20_SPACE_ROOTS 6u
struct infs20_space_wire {
    uint8_t payload[INFS20_SPACE_ROOTS][INFS20_PAYLOAD_BYTES];
    size_t size[INFS20_SPACE_ROOTS];
};
infs_status infs20_space_pack(const struct infs20_space *state,
                              struct infs20_space_wire *out);
infs_status infs20_space_unpack(const struct infs20_space_wire *wire,
                                uint64_t expected_capacity,
                                uint64_t expected_sequence,
                                struct infs20_space *out);
infs_status infs20_space_read(const struct infs20_io *io,
                              const struct infs20_authority *authority,
                              size_t budget, struct infs20_space *out);
#endif
