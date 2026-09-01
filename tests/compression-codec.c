// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/iac1.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_BYTES (256u * 1024u)

static void fail(const char *message)
{
    fprintf(stderr, "iac1: %s\n", message);
    exit(1);
}

static void expect(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static size_t roundtrip(const uint8_t *source, size_t size,
                        uint8_t *compressed, uint8_t *candidate,
                        uint8_t *decoded,
                        struct infs_iac1_scratch *scratch)
{
    size_t bound = infs_iac1_bound(size);
    size_t stored = infs_iac1_compress(
        source, size, compressed, bound, candidate, bound, scratch);
    expect(stored != 0, "encode stream");
    expect(infs_iac1_decompress(
               compressed, stored, decoded, size) == size,
           "decode stream");
    expect(memcmp(source, decoded, size) == 0, "round-trip bytes");
    return stored;
}

int main(void)
{
    size_t bound = infs_iac1_bound(TEST_BYTES);
    uint8_t *source = malloc(TEST_BYTES);
    uint8_t *compressed = malloc(bound);
    uint8_t *candidate = malloc(bound);
    uint8_t *decoded = malloc(TEST_BYTES);
    uint8_t *again = malloc(bound);
    struct infs_iac1_scratch *scratch = malloc(sizeof(*scratch));
    struct infs_iac1_scratch *scratch2 = malloc(sizeof(*scratch2));
    uint32_t random_state = UINT32_C(0x12345678);
    size_t stored;

    expect(source && compressed && candidate && decoded && again &&
               scratch && scratch2,
           "allocate codec buffers");

    memset(source, 0, TEST_BYTES);
    expect(infs_iac1_should_attempt(source, TEST_BYTES),
           "zero-heavy input selected");
    stored = roundtrip(source, TEST_BYTES, compressed, candidate,
                       decoded, scratch);
    expect(stored < 64u, "zero-heavy native compression ratio");

    for (size_t i = 0; i < TEST_BYTES; ++i)
        source[i] = (uint8_t)i;
    expect(infs_iac1_should_attempt(source, TEST_BYTES),
           "predictable ramp selected");
    stored = roundtrip(source, TEST_BYTES, compressed, candidate,
                       decoded, scratch);
    expect(stored < 64u, "delta predictor compression ratio");
    expect(compressed[1] == INFS_IAC1_MODE_DELTA8,
           "delta predictor selected");

    {
        static const char phrase[] =
            "InfiltratorFS generation checkpoint extent metadata portable core\n";
        for (size_t i = 0; i < TEST_BYTES; ++i)
            source[i] = (uint8_t)phrase[i % (sizeof(phrase) - 1u)];
    }
    stored = roundtrip(source, TEST_BYTES, compressed, candidate,
                       decoded, scratch);
    expect(stored < TEST_BYTES / 8u, "repeating text compression ratio");

    {
        size_t first = infs_iac1_compress(
            source, TEST_BYTES, compressed, bound,
            candidate, bound, scratch);
        size_t second = infs_iac1_compress(
            source, TEST_BYTES, again, bound,
            candidate, bound, scratch2);
        expect(first == second && memcmp(compressed, again, first) == 0,
               "codec output deterministic");
    }

    for (size_t i = 0; i < TEST_BYTES; ++i) {
        random_state = random_state * UINT32_C(1664525) +
                       UINT32_C(1013904223);
        source[i] = (uint8_t)(random_state >> 24);
    }
    expect(!infs_iac1_should_attempt(source, TEST_BYTES),
           "high-entropy input rejected cheaply");

    {
        const uint8_t bad_version[] = { 2, 0, 0, 0, 0, 0 };
        expect(infs_iac1_decompress(
                   bad_version, sizeof(bad_version), decoded, 1u) == 0,
               "reject unknown codec version");
    }
    {
        const uint8_t bad_match[] = {
            INFS_IAC1_VERSION, INFS_IAC1_MODE_IDENTITY, 0, 0,
            0x80, 0, 0
        };
        expect(infs_iac1_decompress(
                   bad_match, sizeof(bad_match), decoded, 4u) == 0,
               "reject zero-distance match");
    }
    {
        const uint8_t bad_fill[] = {
            INFS_IAC1_VERSION, INFS_IAC1_MODE_IDENTITY, 0, 0,
            INFS_IAC1_FILL_TOKEN, 0, 3, 0
        };
        expect(infs_iac1_decompress(
                   bad_fill, sizeof(bad_fill), decoded, 3u) == 0,
               "reject undersized fill token");
    }

    free(scratch2);
    free(scratch);
    free(again);
    free(decoded);
    free(candidate);
    free(compressed);
    free(source);
    return 0;
}
