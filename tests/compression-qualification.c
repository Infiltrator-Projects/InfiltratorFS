// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/iac1.h"

#include <lz4.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QUAL_BYTES (256u * 1024u)
#define FS_BLOCK 4096u
#define BENCH_LOOPS 8u

struct totals {
    uint64_t logical;
    uint64_t iac1_physical;
    uint64_t lz4_physical;
    unsigned attempted;
    unsigned selected;
    unsigned cases;
};

static void fail(const char *message)
{
    fprintf(stderr, "compression-qualification: %s\n", message);
    exit(1);
}

static void expect(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void put_u32le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void repeat_bytes(uint8_t *dst, size_t bytes,
                         const uint8_t *pattern, size_t pattern_bytes)
{
    size_t pos = 0;
    while (pos < bytes) {
        size_t chunk = pattern_bytes;
        if (chunk > bytes - pos)
            chunk = bytes - pos;
        memcpy(dst + pos, pattern, chunk);
        pos += chunk;
    }
}

static void fill_source(uint8_t *dst, size_t bytes)
{
    static const uint8_t source[] =
        "static int update_extent(struct volume *v, uint64_t logical) {\n"
        "    if (!v || logical >= v->blocks) return -1;\n"
        "    v->generation++;\n"
        "    return commit_transaction(v);\n"
        "}\n";
    repeat_bytes(dst, bytes, source, sizeof(source) - 1u);
    for (size_t i = 4096u; i + 4u <= bytes; i += 4096u)
        put_u32le(dst + i, (uint32_t)(i / 4096u));
}

static void fill_executable(uint8_t *dst, size_t bytes)
{
    static const uint8_t code[] = {
        0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x20,
        0x48, 0x8b, 0x45, 0xf8, 0x48, 0x85, 0xc0, 0x74,
        0x0a, 0x48, 0x8b, 0x00, 0x48, 0x83, 0xc0, 0x01,
        0x48, 0x89, 0x45, 0xf8, 0x31, 0xc0, 0xc9, 0xc3
    };
    repeat_bytes(dst, bytes, code, sizeof(code));
    for (size_t i = 2048u; i + 4u <= bytes; i += 2048u)
        put_u32le(dst + i, 0x00400000u + (uint32_t)i);
}

static void fill_office(uint8_t *dst, size_t bytes)
{
    static const uint8_t xml[] =
        "<w:p><w:r><w:t>InfiltratorFS compression qualification document "
        "content and repeated formatting metadata.</w:t></w:r></w:p>\n";
    repeat_bytes(dst, bytes, xml, sizeof(xml) - 1u);
}

static void fill_database(uint8_t *dst, size_t bytes)
{
    memset(dst, 0, bytes);
    for (size_t off = 0, row = 0; off + 64u <= bytes; off += 64u, ++row) {
        put_u32le(dst + off + 0u, (uint32_t)row);
        put_u32le(dst + off + 4u, 0x20260902u);
        put_u32le(dst + off + 8u, (uint32_t)(row % 97u));
        memcpy(dst + off + 16u, "ACTIVE  INFILTRATORFS ROW       ", 31u);
        put_u32le(dst + off + 48u, (uint32_t)(row * 4096u));
        put_u32le(dst + off + 52u, (uint32_t)(row * 17u));
    }
}

static void fill_vm(uint8_t *dst, size_t bytes)
{
    uint32_t state = UINT32_C(0x51f15e77);
    memset(dst, 0, bytes);
    for (size_t base = 0; base < bytes; base += 64u * 1024u) {
        size_t dirty = base + 12u * 1024u;
        size_t end = dirty + 4096u;
        if (end > bytes)
            end = bytes;
        for (size_t i = dirty; i < end; ++i)
            dst[i] = (uint8_t)xorshift32(&state);
    }
}

static void fill_structured(uint8_t *dst, size_t bytes)
{
    for (size_t i = 0; i + 4u <= bytes; i += 4u)
        put_u32le(dst + i, (uint32_t)(i / 4u));
}

static void fill_small_tree(uint8_t *dst, size_t bytes)
{
    static const uint8_t config[] =
        "[application]\nname=InfiltratorFS\nenabled=true\n"
        "cache=adaptive\nmode=portable\n"
        "{\"type\":\"file\",\"state\":\"clean\",\"generation\":1}\n";
    repeat_bytes(dst, bytes, config, sizeof(config) - 1u);
    for (size_t i = 1024u; i + 4u <= bytes; i += 1024u)
        put_u32le(dst + i, (uint32_t)i);
}

static void fill_entropy(uint8_t *dst, size_t bytes, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0; i < bytes; ++i)
        dst[i] = (uint8_t)xorshift32(&state);
}

static uint64_t physical_bytes(size_t stored, size_t logical)
{
    uint64_t logical_blocks = (logical + FS_BLOCK - 1u) / FS_BLOCK;
    uint64_t stored_blocks = (stored + FS_BLOCK - 1u) / FS_BLOCK;
    if (!stored || stored_blocks >= logical_blocks)
        return logical_blocks * FS_BLOCK;
    return stored_blocks * FS_BLOCK;
}

static double mib_per_second(size_t bytes, unsigned loops,
                             clock_t begin, clock_t end)
{
    double seconds = (double)(end - begin) / (double)CLOCKS_PER_SEC;
    double mib = ((double)bytes * (double)loops) / (1024.0 * 1024.0);
    return seconds > 0.0 ? mib / seconds : 0.0;
}

static void qualify_case(const char *name, uint8_t *plain, size_t bytes,
                         int expect_attempt, struct totals *total)
{
    size_t bound = infs_iac1_bound(bytes);
    int lz4_bound = LZ4_compressBound((int)bytes);
    uint8_t *iac1 = malloc(bound);
    uint8_t *iac1_again = malloc(bound);
    uint8_t *candidate = malloc(bound);
    uint8_t *decoded = malloc(bytes);
    char *lz4 = malloc((size_t)lz4_bound);
    char *lz4_decoded = malloc(bytes);
    struct infs_iac1_scratch *scratch = malloc(sizeof(*scratch));
    size_t iac1_bytes = 0;
    int lz4_bytes;
    int attempt;
    uint64_t iac1_physical;
    uint64_t lz4_physical;
    clock_t start;
    clock_t stop;
    double iac1_encode_mibs = 0.0;
    double iac1_decode_mibs = 0.0;
    double lz4_encode_mibs;
    double lz4_decode_mibs;
    unsigned loop;

    expect(iac1 && iac1_again && candidate && decoded && lz4 &&
           lz4_decoded && scratch, "allocate qualification buffers");

    attempt = infs_iac1_should_attempt(plain, bytes);
    if (expect_attempt >= 0)
        expect(attempt == expect_attempt,
               "adaptive attempt classifier disagrees with corpus expectation");

    if (attempt) {
        iac1_bytes = infs_iac1_compress(
            plain, bytes, iac1, bound, candidate, bound, scratch);
        expect(iac1_bytes != 0, "IAC1 failed to encode selected input");
        expect(infs_iac1_decompress(
                   iac1, iac1_bytes, decoded, bytes) == bytes,
               "IAC1 failed roundtrip");
        expect(memcmp(plain, decoded, bytes) == 0,
               "IAC1 roundtrip changed logical bytes");

        {
            size_t second = infs_iac1_compress(
                plain, bytes, iac1_again, bound, candidate, bound, scratch);
            expect(second == iac1_bytes &&
                   memcmp(iac1, iac1_again, iac1_bytes) == 0,
                   "IAC1 output is not deterministic");
        }

        start = clock();
        for (loop = 0; loop < BENCH_LOOPS; ++loop)
            expect(infs_iac1_compress(
                       plain, bytes, iac1_again, bound,
                       candidate, bound, scratch) != 0,
                   "IAC1 benchmark encode failed");
        stop = clock();
        iac1_encode_mibs = mib_per_second(bytes, BENCH_LOOPS, start, stop);

        start = clock();
        for (loop = 0; loop < BENCH_LOOPS; ++loop)
            expect(infs_iac1_decompress(
                       iac1, iac1_bytes, decoded, bytes) == bytes,
                   "IAC1 benchmark decode failed");
        stop = clock();
        iac1_decode_mibs = mib_per_second(bytes, BENCH_LOOPS, start, stop);
    }

    lz4_bytes = LZ4_compress_default(
        (const char *)plain, lz4, (int)bytes, lz4_bound);
    expect(lz4_bytes > 0, "LZ4 baseline encode failed");
    expect(LZ4_decompress_safe(
               lz4, lz4_decoded, lz4_bytes, (int)bytes) == (int)bytes,
           "LZ4 baseline decode failed");
    expect(memcmp(plain, lz4_decoded, bytes) == 0,
           "LZ4 baseline roundtrip changed logical bytes");

    start = clock();
    for (loop = 0; loop < BENCH_LOOPS; ++loop)
        expect(LZ4_compress_default(
                   (const char *)plain, lz4, (int)bytes, lz4_bound) > 0,
               "LZ4 benchmark encode failed");
    stop = clock();
    lz4_encode_mibs = mib_per_second(bytes, BENCH_LOOPS, start, stop);

    start = clock();
    for (loop = 0; loop < BENCH_LOOPS; ++loop)
        expect(LZ4_decompress_safe(
                   lz4, lz4_decoded, lz4_bytes, (int)bytes) == (int)bytes,
               "LZ4 benchmark decode failed");
    stop = clock();
    lz4_decode_mibs = mib_per_second(bytes, BENCH_LOOPS, start, stop);

    iac1_physical = attempt ? physical_bytes(iac1_bytes, bytes) :
                              physical_bytes(0, bytes);
    lz4_physical = physical_bytes((size_t)lz4_bytes, bytes);

    total->logical += physical_bytes(0, bytes);
    total->iac1_physical += iac1_physical;
    total->lz4_physical += lz4_physical;
    total->attempted += attempt ? 1u : 0u;
    total->selected += iac1_physical < physical_bytes(0, bytes) ? 1u : 0u;
    total->cases++;

    printf("[COMPRESSION-QUAL] case=%s attempt=%d "
           "iac1_bytes=%zu iac1_physical=%llu "
           "lz4_bytes=%d lz4_physical=%llu "
           "iac1_enc_mib_s=%.2f iac1_dec_mib_s=%.2f "
           "lz4_enc_mib_s=%.2f lz4_dec_mib_s=%.2f\n",
           name, attempt, iac1_bytes,
           (unsigned long long)iac1_physical,
           lz4_bytes, (unsigned long long)lz4_physical,
           iac1_encode_mibs, iac1_decode_mibs,
           lz4_encode_mibs, lz4_decode_mibs);

    free(scratch);
    free(lz4_decoded);
    free(lz4);
    free(decoded);
    free(candidate);
    free(iac1_again);
    free(iac1);
}

int main(void)
{
    uint8_t *plain = malloc(QUAL_BYTES);
    struct totals total = {0};

    expect(plain != NULL, "allocate corpus buffer");
    expect(sizeof(struct infs_iac1_scratch) <= 128u * 1024u,
           "IAC1 scratch bound exceeds kernel qualification budget");

    fill_source(plain, QUAL_BYTES);
    qualify_case("source-text", plain, QUAL_BYTES, 1, &total);

    fill_executable(plain, QUAL_BYTES);
    qualify_case("executable-library", plain, QUAL_BYTES, 1, &total);

    fill_office(plain, QUAL_BYTES);
    qualify_case("office-document", plain, QUAL_BYTES, 1, &total);

    fill_database(plain, QUAL_BYTES);
    qualify_case("database", plain, QUAL_BYTES, 1, &total);

    fill_vm(plain, QUAL_BYTES);
    qualify_case("vm-zero-heavy", plain, QUAL_BYTES, 1, &total);

    fill_structured(plain, QUAL_BYTES);
    qualify_case("structured-binary", plain, QUAL_BYTES, 1, &total);

    fill_small_tree(plain, QUAL_BYTES);
    qualify_case("mixed-small-files", plain, QUAL_BYTES, 1, &total);

    fill_entropy(plain, QUAL_BYTES, UINT32_C(0x12345678));
    qualify_case("already-compressed-encrypted", plain, QUAL_BYTES, 0, &total);

    expect(total.attempted >= 7u,
           "adaptive policy did not attempt enough compressible workload classes");
    expect(total.selected >= 6u,
           "native codec did not save filesystem blocks on enough workload classes");
    expect(total.iac1_physical * 100u <= total.logical * 70u,
           "native codec corpus savings are below the 30 percent qualification floor");

    printf("[COMPRESSION-QUAL] aggregate cases=%u attempted=%u selected=%u "
           "logical_physical=%llu iac1_physical=%llu lz4_physical=%llu "
           "iac1_saving_pct=%.2f lz4_saving_pct=%.2f scratch_bytes=%zu\n",
           total.cases, total.attempted, total.selected,
           (unsigned long long)total.logical,
           (unsigned long long)total.iac1_physical,
           (unsigned long long)total.lz4_physical,
           100.0 * (1.0 - (double)total.iac1_physical / (double)total.logical),
           100.0 * (1.0 - (double)total.lz4_physical / (double)total.logical),
           sizeof(struct infs_iac1_scratch));

    free(plain);
    return 0;
}
