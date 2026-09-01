// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_IAC1_H
#define INFILFS_IAC1_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 infs_iac1_u8;
typedef u16 infs_iac1_u16;
typedef u32 infs_iac1_u32;
typedef u64 infs_iac1_u64;
typedef size_t infs_iac1_size;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t infs_iac1_u8;
typedef uint16_t infs_iac1_u16;
typedef uint32_t infs_iac1_u32;
typedef uint64_t infs_iac1_u64;
typedef size_t infs_iac1_size;
#endif

#define INFS_IAC1_VERSION 1u
#define INFS_IAC1_MODE_IDENTITY 0u
#define INFS_IAC1_MODE_DELTA8   1u
#define INFS_IAC1_MODE_XOR4     2u
#define INFS_IAC1_HEADER_SIZE 4u
#define INFS_IAC1_HASH_BITS 14u
#define INFS_IAC1_HASH_SIZE (1u << INFS_IAC1_HASH_BITS)
#define INFS_IAC1_MATCH_MIN 4u
#define INFS_IAC1_MATCH_MAX 131u
#define INFS_IAC1_LITERAL_MAX 127u
#define INFS_IAC1_FILL_TOKEN 0x7fu
#define INFS_IAC1_FILL_MIN 4u
#define INFS_IAC1_FILL_MAX 65535u
#define INFS_IAC1_WINDOW_MAX 65535u

struct infs_iac1_scratch {
    infs_iac1_u32 latest[INFS_IAC1_HASH_SIZE];
    infs_iac1_u32 previous[INFS_IAC1_HASH_SIZE];
};

static inline infs_iac1_size infs_iac1_bound(infs_iac1_size input_size)
{
    return INFS_IAC1_HEADER_SIZE + input_size +
           (input_size + INFS_IAC1_LITERAL_MAX - 1u) /
               INFS_IAC1_LITERAL_MAX;
}

static inline infs_iac1_u8 infs_iac1_transform_byte(
    const infs_iac1_u8 *src, infs_iac1_size pos, infs_iac1_u8 mode)
{
    if (mode == INFS_IAC1_MODE_DELTA8)
        return pos ? (infs_iac1_u8)(src[pos] - src[pos - 1u]) : src[pos];
    if (mode == INFS_IAC1_MODE_XOR4)
        return pos >= 4u ? (infs_iac1_u8)(src[pos] ^ src[pos - 4u]) : src[pos];
    return src[pos];
}

static inline infs_iac1_u32 infs_iac1_hash4(
    const infs_iac1_u8 *src, infs_iac1_size pos, infs_iac1_u8 mode)
{
    infs_iac1_u32 value =
        (infs_iac1_u32)infs_iac1_transform_byte(src, pos, mode) |
        ((infs_iac1_u32)infs_iac1_transform_byte(src, pos + 1u, mode) << 8) |
        ((infs_iac1_u32)infs_iac1_transform_byte(src, pos + 2u, mode) << 16) |
        ((infs_iac1_u32)infs_iac1_transform_byte(src, pos + 3u, mode) << 24);
    value ^= value >> 15;
    value *= 0x2c1b3c6du;
    value ^= value >> 12;
    return value >> (32u - INFS_IAC1_HASH_BITS);
}

static inline void infs_iac1_clear_scratch(struct infs_iac1_scratch *scratch)
{
    infs_iac1_size i;
    for (i = 0; i < INFS_IAC1_HASH_SIZE; ++i) {
        scratch->latest[i] = 0;
        scratch->previous[i] = 0;
    }
}

static inline void infs_iac1_insert_position(
    const infs_iac1_u8 *src, infs_iac1_size input_size,
    infs_iac1_size pos, infs_iac1_u8 mode,
    struct infs_iac1_scratch *scratch)
{
    infs_iac1_u32 hash;
    if (pos + INFS_IAC1_MATCH_MIN > input_size)
        return;
    hash = infs_iac1_hash4(src, pos, mode);
    scratch->previous[hash] = scratch->latest[hash];
    scratch->latest[hash] = (infs_iac1_u32)pos + 1u;
}

static inline infs_iac1_size infs_iac1_candidate_match(
    const infs_iac1_u8 *src, infs_iac1_size input_size,
    infs_iac1_size pos, infs_iac1_size candidate,
    infs_iac1_u8 mode)
{
    infs_iac1_size limit;
    infs_iac1_size length = 0;
    if (candidate >= pos || pos - candidate > INFS_IAC1_WINDOW_MAX)
        return 0;
    limit = input_size - pos;
    if (limit > INFS_IAC1_MATCH_MAX)
        limit = INFS_IAC1_MATCH_MAX;
    while (length < limit &&
           infs_iac1_transform_byte(src, pos + length, mode) ==
               infs_iac1_transform_byte(src, candidate + length, mode))
        ++length;
    return length >= INFS_IAC1_MATCH_MIN ? length : 0;
}

static inline infs_iac1_size infs_iac1_find_match(
    const infs_iac1_u8 *src, infs_iac1_size input_size,
    infs_iac1_size pos, infs_iac1_u8 mode,
    struct infs_iac1_scratch *scratch, infs_iac1_u16 *distance_out)
{
    infs_iac1_size best = 0;
    infs_iac1_size best_candidate = 0;
    if (pos + INFS_IAC1_MATCH_MIN <= input_size) {
        infs_iac1_u32 hash = infs_iac1_hash4(src, pos, mode);
        infs_iac1_u32 encoded[2] = {
            scratch->latest[hash], scratch->previous[hash]
        };
        unsigned i;
        for (i = 0; i < 2u; ++i) {
            infs_iac1_size candidate;
            infs_iac1_size length;
            if (!encoded[i])
                continue;
            candidate = (infs_iac1_size)encoded[i] - 1u;
            length = infs_iac1_candidate_match(
                src, input_size, pos, candidate, mode);
            if (length > best) {
                best = length;
                best_candidate = candidate;
            }
        }
    }
    if (best && distance_out)
        *distance_out = (infs_iac1_u16)(pos - best_candidate);
    return best;
}

static inline infs_iac1_size infs_iac1_fill_run(
    const infs_iac1_u8 *src, infs_iac1_size input_size,
    infs_iac1_size pos, infs_iac1_u8 mode)
{
    infs_iac1_u8 value = infs_iac1_transform_byte(src, pos, mode);
    infs_iac1_size length = 1u;
    infs_iac1_size limit = input_size - pos;
    if (limit > INFS_IAC1_FILL_MAX)
        limit = INFS_IAC1_FILL_MAX;
    while (length < limit &&
           infs_iac1_transform_byte(src, pos + length, mode) == value)
        ++length;
    return length >= INFS_IAC1_FILL_MIN ? length : 0;
}

static inline infs_iac1_size infs_iac1_compress_mode(
    const infs_iac1_u8 *src, infs_iac1_size input_size,
    infs_iac1_u8 *dst, infs_iac1_size capacity,
    struct infs_iac1_scratch *scratch, infs_iac1_u8 mode)
{
    infs_iac1_size pos = 0;
    infs_iac1_size out = 0;
    if (!src || !dst || !scratch || !input_size ||
        mode > INFS_IAC1_MODE_XOR4 || capacity < INFS_IAC1_HEADER_SIZE)
        return 0;

    dst[out++] = INFS_IAC1_VERSION;
    dst[out++] = mode;
    dst[out++] = 0;
    dst[out++] = 0;
    infs_iac1_clear_scratch(scratch);

    while (pos < input_size) {
        infs_iac1_size fill =
            infs_iac1_fill_run(src, input_size, pos, mode);
        if (fill) {
            infs_iac1_size k;
            if (out + 4u > capacity)
                return 0;
            dst[out++] = INFS_IAC1_FILL_TOKEN;
            dst[out++] = infs_iac1_transform_byte(src, pos, mode);
            dst[out++] = (infs_iac1_u8)(fill & 0xffu);
            dst[out++] = (infs_iac1_u8)(fill >> 8);
            for (k = 0; k < fill; ++k)
                infs_iac1_insert_position(
                    src, input_size, pos + k, mode, scratch);
            pos += fill;
            continue;
        }

        {
            infs_iac1_u16 distance = 0;
            infs_iac1_size match = infs_iac1_find_match(
                src, input_size, pos, mode, scratch, &distance);
            infs_iac1_insert_position(src, input_size, pos, mode, scratch);
            if (match) {
                infs_iac1_size k;
                if (out + 3u > capacity)
                    return 0;
                dst[out++] = (infs_iac1_u8)(
                    0x80u | (infs_iac1_u8)(match - 4u));
                dst[out++] = (infs_iac1_u8)(distance & 0xffu);
                dst[out++] = (infs_iac1_u8)(distance >> 8);
                for (k = 1u; k < match; ++k)
                    infs_iac1_insert_position(
                        src, input_size, pos + k, mode, scratch);
                pos += match;
                continue;
            }
        }

        {
            infs_iac1_size literal_start = pos;
            ++pos;
            while (pos < input_size &&
                   pos - literal_start < INFS_IAC1_LITERAL_MAX) {
                infs_iac1_u16 probe_distance = 0;
                infs_iac1_size probe_fill =
                    infs_iac1_fill_run(src, input_size, pos, mode);
                infs_iac1_size probe = infs_iac1_find_match(
                    src, input_size, pos, mode, scratch, &probe_distance);
                if (probe_fill || probe)
                    break;
                infs_iac1_insert_position(
                    src, input_size, pos, mode, scratch);
                ++pos;
            }
            {
                infs_iac1_size literal_length = pos - literal_start;
                if (out + 1u + literal_length > capacity)
                    return 0;
                dst[out++] = (infs_iac1_u8)(literal_length - 1u);
                while (literal_start < pos)
                    dst[out++] = infs_iac1_transform_byte(
                        src, literal_start++, mode);
            }
        }
    }
    return out;
}

static inline int infs_iac1_should_attempt(
    const infs_iac1_u8 *src, infs_iac1_size input_size)
{
    infs_iac1_u64 seen[4] = { 0, 0, 0, 0 };
    infs_iac1_size samples;
    infs_iac1_size stride;
    infs_iac1_size i;
    infs_iac1_size delta_hits = 0;
    infs_iac1_size xor_hits = 0;
    unsigned unique = 0;
    if (!src || input_size < 32u)
        return 0;

    samples = input_size < 512u ? input_size : 512u;
    stride = input_size / samples;
    if (!stride)
        stride = 1u;
    for (i = 0; i < samples; ++i) {
        infs_iac1_size pos = i * stride;
        infs_iac1_u8 value = src[pos];
        infs_iac1_u64 bit =
            ((infs_iac1_u64)1u) << (value & 63u);
        unsigned bucket = value >> 6;
        if ((seen[bucket] & bit) == 0) {
            seen[bucket] |= bit;
            ++unique;
        }
        if (pos >= 2u) {
            infs_iac1_u8 a =
                (infs_iac1_u8)(src[pos] - src[pos - 1u]);
            infs_iac1_u8 b =
                (infs_iac1_u8)(src[pos - 1u] - src[pos - 2u]);
            if (a == b)
                ++delta_hits;
        }
        if (pos >= 4u && src[pos] == src[pos - 4u])
            ++xor_hits;
    }
    return unique <= 200u || delta_hits > samples / 24u ||
           xor_hits > samples / 24u;
}

static inline infs_iac1_u8 infs_iac1_predictor_mode(
    const infs_iac1_u8 *src, infs_iac1_size input_size)
{
    infs_iac1_size delta_hits = 0;
    infs_iac1_size xor_hits = 0;
    infs_iac1_size i;
    if (input_size < 8u)
        return INFS_IAC1_MODE_IDENTITY;
    for (i = 2u; i < input_size; ++i) {
        infs_iac1_u8 a =
            (infs_iac1_u8)(src[i] - src[i - 1u]);
        infs_iac1_u8 b =
            (infs_iac1_u8)(src[i - 1u] - src[i - 2u]);
        if (a == b)
            ++delta_hits;
    }
    for (i = 4u; i < input_size; ++i)
        if (src[i] == src[i - 4u])
            ++xor_hits;

    if (xor_hits > delta_hits && xor_hits > input_size / 16u)
        return INFS_IAC1_MODE_XOR4;
    if (delta_hits > input_size / 16u)
        return INFS_IAC1_MODE_DELTA8;
    return INFS_IAC1_MODE_IDENTITY;
}

static inline infs_iac1_size infs_iac1_compress(
    const infs_iac1_u8 *src, infs_iac1_size input_size,
    infs_iac1_u8 *dst, infs_iac1_size capacity,
    infs_iac1_u8 *candidate, infs_iac1_size candidate_capacity,
    struct infs_iac1_scratch *scratch)
{
    infs_iac1_size best = infs_iac1_compress_mode(
        src, input_size, dst, capacity, scratch, INFS_IAC1_MODE_IDENTITY);
    infs_iac1_u8 mode = infs_iac1_predictor_mode(src, input_size);
    if (mode != INFS_IAC1_MODE_IDENTITY && candidate &&
        candidate_capacity >= capacity) {
        infs_iac1_size alternate = infs_iac1_compress_mode(
            src, input_size, candidate, candidate_capacity, scratch, mode);
        if (alternate && (!best || alternate < best)) {
            infs_iac1_size i;
            for (i = 0; i < alternate; ++i)
                dst[i] = candidate[i];
            best = alternate;
        }
    }
    return best;
}

static inline infs_iac1_size infs_iac1_decompress(
    const infs_iac1_u8 *src, infs_iac1_size input_size,
    infs_iac1_u8 *dst, infs_iac1_size output_size)
{
    infs_iac1_size in = INFS_IAC1_HEADER_SIZE;
    infs_iac1_size out = 0;
    infs_iac1_size i;
    infs_iac1_u8 mode;
    if (!src || !dst || input_size < INFS_IAC1_HEADER_SIZE ||
        src[0] != INFS_IAC1_VERSION || src[2] != 0 || src[3] != 0)
        return 0;
    mode = src[1];
    if (mode > INFS_IAC1_MODE_XOR4)
        return 0;

    while (in < input_size && out < output_size) {
        infs_iac1_u8 control = src[in++];
        if (control == INFS_IAC1_FILL_TOKEN) {
            infs_iac1_size length;
            infs_iac1_u8 value;
            if (input_size - in < 3u)
                return 0;
            value = src[in++];
            length = (infs_iac1_size)src[in] |
                     ((infs_iac1_size)src[in + 1u] << 8);
            in += 2u;
            if (length < INFS_IAC1_FILL_MIN ||
                length > output_size - out)
                return 0;
            for (i = 0; i < length; ++i)
                dst[out++] = value;
        } else if ((control & 0x80u) == 0) {
            infs_iac1_size length = (infs_iac1_size)control + 1u;
            if (length > input_size - in ||
                length > output_size - out)
                return 0;
            for (i = 0; i < length; ++i)
                dst[out++] = src[in++];
        } else {
            infs_iac1_size length =
                (infs_iac1_size)(control & 0x7fu) + 4u;
            infs_iac1_size distance;
            if (input_size - in < 2u ||
                length > output_size - out)
                return 0;
            distance = (infs_iac1_size)src[in] |
                       ((infs_iac1_size)src[in + 1u] << 8);
            in += 2u;
            if (!distance || distance > out)
                return 0;
            for (i = 0; i < length; ++i) {
                dst[out] = dst[out - distance];
                ++out;
            }
        }
    }
    if (in != input_size || out != output_size)
        return 0;

    if (mode == INFS_IAC1_MODE_DELTA8) {
        for (i = 1u; i < output_size; ++i)
            dst[i] = (infs_iac1_u8)(dst[i] + dst[i - 1u]);
    } else if (mode == INFS_IAC1_MODE_XOR4) {
        for (i = 4u; i < output_size; ++i)
            dst[i] = (infs_iac1_u8)(dst[i] ^ dst[i - 4u]);
    }
    return out;
}

#endif
