#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deterministic structural recovery migration for the recovered 0.18.45 tree."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def write(path, text):
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(text, old, new, label):
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected one occurrence, found {n}")
    return text.replace(old, new, 1)


def replace_function(text, signature, replacement, label):
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"{label}: signature not found")
    brace = text.find("{", start + len(signature))
    if brace < 0:
        raise SystemExit(f"{label}: opening brace not found")
    depth = 0
    i = brace
    in_str = in_char = in_line = in_block = False
    esc = False
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if in_line:
            if c == "\n":
                in_line = False
        elif in_block:
            if c == "*" and n == "/":
                in_block = False
                i += 1
        elif in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        elif in_char:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == "'":
                in_char = False
        else:
            if c == "/" and n == "/":
                in_line = True
                i += 1
            elif c == "/" and n == "*":
                in_block = True
                i += 1
            elif c == '"':
                in_str = True
            elif c == "'":
                in_char = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return text[:start] + replacement.rstrip() + "\n" + text[i + 1:]
        i += 1
    raise SystemExit(f"{label}: unterminated function")


def insert_after_open_brace(text, signature, line, label):
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"{label}: signature not found")
    brace = text.find("{", start + len(signature))
    if brace < 0:
        raise SystemExit(f"{label}: opening brace not found")
    if line.strip() in text[brace:brace + 1200]:
        return text
    return text[:brace + 1] + "\n    " + line.strip() + text[brace + 1:]


# 1. Portable SHA-256 acceleration with a tested scalar fallback.
cmake = read("CMakeLists.txt")
if "INFS_HAVE_OPENSSL_SHA256" not in cmake:
    anchor = "target_link_libraries(infilfs_core PUBLIC InfiltratrCommon::Portable lz4_static)\ninfilfs_enable_warnings(infilfs_core)\n"
    addition = """target_link_libraries(infilfs_core PUBLIC InfiltratrCommon::Portable lz4_static)

# Integrity hashing is on every data path. Prefer the host crypto provider so
# SHA-NI/ARMv8 SHA and provider acceleration are used automatically. The
# known-answer-tested scalar code remains a fail-safe fallback.
if(WIN32)
    target_link_libraries(infilfs_core PUBLIC bcrypt)
    target_compile_definitions(infilfs_core PRIVATE INFS_HAVE_BCRYPT_SHA256=1)
else()
    find_package(OpenSSL QUIET COMPONENTS Crypto)
    if(OpenSSL_FOUND)
        target_link_libraries(infilfs_core PUBLIC OpenSSL::Crypto)
        target_compile_definitions(infilfs_core PRIVATE INFS_HAVE_OPENSSL_SHA256=1)
    endif()
endif()
infilfs_enable_warnings(infilfs_core)
"""
    cmake = replace_once(cmake, anchor, addition, "CMake SHA acceleration")
    write("CMakeLists.txt", cmake)

checksum = read("src/checksum.c")
if "infs_sha256_scalar" not in checksum:
    checksum = replace_once(
        checksum,
        "#include <string.h>\n",
        """#include <string.h>
#include <limits.h>

#if defined(INFS_HAVE_BCRYPT_SHA256)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#elif defined(INFS_HAVE_OPENSSL_SHA256)
#include <openssl/evp.h>
#endif
""",
        "checksum accelerated includes")
    checksum = replace_once(
        checksum,
        "void infs_sha256(const void *data, size_t len, uint8_t out[32])",
        "static void infs_sha256_scalar(const void *data, size_t len, uint8_t out[32])",
        "rename scalar SHA")
    checksum += r'''

static int infs_sha256_accelerated(const void *data, size_t len, uint8_t out[32])
{
#if defined(INFS_HAVE_BCRYPT_SHA256)
    BCRYPT_ALG_HANDLE algorithm = NULL;
    NTSTATUS status;

    if (len > ULONG_MAX)
        return 0;
    status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (status < 0)
        return 0;
    status = BCryptHash(algorithm, NULL, 0,
                        (PUCHAR)(uintptr_t)data, (ULONG)len, out, 32);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status >= 0;
#elif defined(INFS_HAVE_OPENSSL_SHA256)
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int produced = 0;
    int ok;

    if (!ctx)
        return 0;
    ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
         EVP_DigestUpdate(ctx, data, len) == 1 &&
         EVP_DigestFinal_ex(ctx, out, &produced) == 1 &&
         produced == 32u;
    EVP_MD_CTX_free(ctx);
    return ok;
#else
    (void)data;
    (void)len;
    (void)out;
    return 0;
#endif
}

void infs_sha256(const void *data, size_t len, uint8_t out[32])
{
    if (!out)
        return;
    if (!data && len) {
        memset(out, 0, 32);
        return;
    }
    if (infs_sha256_accelerated(data, len, out))
        return;
    infs_sha256_scalar(data, len, out);
}
'''
    write("src/checksum.c", checksum)


# 2. Rebuildable live shared-reference interval index.
volh = read("include/infilfs/volume.h")
if "struct infs_shared_ref_run;" not in volh:
    volh = replace_once(
        volh, "struct infs_free_extent;\n",
        "struct infs_free_extent;\nstruct infs_shared_ref_run;\n",
        "shared-ref forward declaration")
    anchor = "    struct infs_free_extent *free_extents;\n    size_t free_extent_count;\n    size_t free_extent_capacity;\n    int free_extent_index_valid;\n"
    repl = anchor + "    /* Rebuildable live physical-reference interval index. */\n    struct infs_shared_ref_run *shared_refs;\n    size_t shared_ref_count;\n    size_t shared_ref_capacity;\n    int shared_ref_index_valid;\n"
    volh = replace_once(volh, anchor, repl, "shared-ref volume fields")
    write("include/infilfs/volume.h", volh)

shared_inc = r'''// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Rebuildable live shared-extent reference index.
 *
 * The allocation bitmap stays authoritative. This interval map turns reflink
 * release from repeated whole-filesystem scans into one graph rebuild followed
 * by logarithmic interval lookups for the mutation.
 */
struct infs_shared_ref_run {
    uint64_t start;
    uint64_t end;
    uint32_t refs;
};

struct infs_shared_ref_event {
    uint64_t block;
    int64_t delta;
};

static void shared_ref_index_invalidate(struct infs_volume *vol)
{
    if (vol)
        vol->shared_ref_index_valid = 0;
}

static void shared_ref_index_destroy(struct infs_volume *vol)
{
    if (!vol)
        return;
    free(vol->shared_refs);
    vol->shared_refs = NULL;
    vol->shared_ref_count = 0;
    vol->shared_ref_capacity = 0;
    vol->shared_ref_index_valid = 0;
}

static int shared_ref_event_compare(const void *a, const void *b)
{
    const struct infs_shared_ref_event *ea = a;
    const struct infs_shared_ref_event *eb = b;
    return ea->block < eb->block ? -1 : ea->block > eb->block ? 1 : 0;
}

static infs_status shared_ref_reserve(struct infs_volume *vol, size_t needed)
{
    struct infs_shared_ref_run *grown;
    size_t next;

    if (needed <= vol->shared_ref_capacity)
        return INFS_STATUS_OK;
    next = vol->shared_ref_capacity ? vol->shared_ref_capacity : 64u;
    while (next < needed) {
        if (next > SIZE_MAX / 2u)
            return INFS_STATUS_OVERFLOW;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(*grown))
        return INFS_STATUS_OVERFLOW;
    grown = realloc(vol->shared_refs, next * sizeof(*grown));
    if (!grown)
        return INFS_STATUS_NO_MEMORY;
    vol->shared_refs = grown;
    vol->shared_ref_capacity = next;
    return INFS_STATUS_OK;
}

static infs_status shared_ref_event_push(
    struct infs_shared_ref_event **events, size_t *count, size_t *capacity,
    uint64_t block, int64_t delta)
{
    if (*count == *capacity) {
        struct infs_shared_ref_event *grown;
        size_t next = *capacity ? *capacity * 2u : 128u;
        if (next < *capacity || next > SIZE_MAX / sizeof(*grown))
            return INFS_STATUS_OVERFLOW;
        grown = realloc(*events, next * sizeof(*grown));
        if (!grown)
            return INFS_STATUS_NO_MEMORY;
        *events = grown;
        *capacity = next;
    }
    (*events)[*count].block = block;
    (*events)[*count].delta = delta;
    ++*count;
    return INFS_STATUS_OK;
}

static infs_status shared_ref_index_rebuild(struct infs_volume *vol)
{
    struct infs_index_entry_disk *entries = NULL;
    struct infs_shared_ref_event *events = NULL;
    uint32_t entry_count = 0;
    size_t event_count = 0, event_capacity = 0;
    infs_status status = index_snapshot(vol, &entries, &entry_count);

    if (status != INFS_STATUS_OK)
        return status;
    vol->shared_ref_count = 0;

    for (uint32_t i = 0; i < entry_count; ++i) {
        uint8_t object[INFS_BLOCK_SIZE];
        struct infs_file_payload_disk *file;
        struct infs_extent_disk *ext;
        struct infs_extent_disk *all = NULL;
        uint32_t count = 0;
        int owned = 0;

        if (infs_le16_to_cpu(entries[i].object_type) != INFS_OBJECT_FILE)
            continue;
        status = read_object(vol, infs_le64_to_cpu(entries[i].object_block), object);
        if (status != INFS_STATUS_OK)
            goto out;
        status = file_validate_volume(vol, object, &file, &ext);
        if (status != INFS_STATUS_OK)
            goto out;
        status = file_extent_snapshot(vol, object, file, ext, &all, &count, &owned);
        if (status != INFS_STATUS_OK)
            goto out;
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t flags = infs_le32_to_cpu(all[j].flags);
            uint32_t logical_blocks = infs_le32_to_cpu(all[j].block_count);
            uint64_t start, blocks;

            if (extent_kind(flags) != INFS_EXTENT_NORMAL)
                continue;
            start = infs_le64_to_cpu(all[j].physical_block);
            blocks = extent_physical_blocks(logical_blocks, flags);
            if (!blocks || start > UINT64_MAX - blocks) {
                status = INFS_STATUS_CORRUPT;
                break;
            }
            status = shared_ref_event_push(&events, &event_count, &event_capacity,
                                           start, 1);
            if (status == INFS_STATUS_OK)
                status = shared_ref_event_push(&events, &event_count, &event_capacity,
                                               start + blocks, -1);
            if (status != INFS_STATUS_OK)
                break;
        }
        if (owned)
            free(all);
        if (status != INFS_STATUS_OK)
            goto out;
    }

    qsort(events, event_count, sizeof(*events), shared_ref_event_compare);
    {
        uint64_t cursor = 0;
        int64_t refs = 0;
        size_t i = 0;
        while (i < event_count) {
            uint64_t at = events[i].block;
            int64_t delta = 0;

            if (refs > 0 && cursor < at) {
                struct infs_shared_ref_run *run;
                if ((uint64_t)refs > UINT32_MAX) {
                    status = INFS_STATUS_OVERFLOW;
                    goto out;
                }
                status = shared_ref_reserve(vol, vol->shared_ref_count + 1u);
                if (status != INFS_STATUS_OK)
                    goto out;
                run = &vol->shared_refs[vol->shared_ref_count++];
                run->start = cursor;
                run->end = at;
                run->refs = (uint32_t)refs;
            }
            while (i < event_count && events[i].block == at) {
                delta += events[i].delta;
                ++i;
            }
            if ((delta < 0 && refs < -delta) ||
                (delta > 0 && refs > INT64_MAX - delta)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            refs += delta;
            if (refs < 0) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            cursor = at;
        }
        if (refs != 0) {
            status = INFS_STATUS_CORRUPT;
            goto out;
        }
    }
    vol->shared_ref_index_valid = 1;
    status = INFS_STATUS_OK;
out:
    if (status != INFS_STATUS_OK)
        vol->shared_ref_index_valid = 0;
    free(events);
    free(entries);
    return status;
}

static infs_status shared_ref_index_lookup(
    struct infs_volume *vol, uint64_t block, uint64_t limit,
    uint64_t *interval_end, uint32_t *refs_out)
{
    size_t lo = 0, hi;
    infs_status status;

    if (!vol->shared_ref_index_valid) {
        status = shared_ref_index_rebuild(vol);
        if (status != INFS_STATUS_OK)
            return status;
    }
    hi = vol->shared_ref_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        if (vol->shared_refs[mid].end <= block)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo < vol->shared_ref_count && vol->shared_refs[lo].start <= block &&
        block < vol->shared_refs[lo].end) {
        *interval_end = vol->shared_refs[lo].end < limit ?
            vol->shared_refs[lo].end : limit;
        *refs_out = vol->shared_refs[lo].refs;
        return INFS_STATUS_OK;
    }
    *interval_end = limit;
    if (lo < vol->shared_ref_count && vol->shared_refs[lo].start < limit)
        *interval_end = vol->shared_refs[lo].start;
    *refs_out = 0;
    return INFS_STATUS_OK;
}
'''
write("src/volume/shared-ref-index.inc", shared_inc)

volume_c = read("src/volume.c")
if '#include "volume/shared-ref-index.inc"' not in volume_c:
    volume_c = replace_once(
        volume_c,
        '#include "volume/ownership-validation.inc"\n#include "volume/scrub.inc"',
        '#include "volume/ownership-validation.inc"\n#include "volume/shared-ref-index.inc"\n#include "volume/scrub.inc"',
        "compose shared-ref index")
    write("src/volume.c", volume_c)

reflink = read("src/volume/reflink.inc")
scan_sig = "static infs_status file_other_reference_cover(\n    struct infs_volume *vol, const uint8_t owner_id[16],\n    uint64_t cursor, uint64_t end, uint64_t *cover_end,\n    uint64_t *next_start)"
if scan_sig in reflink:
    start = reflink.find(scan_sig)
    brace = reflink.find("{", start)
    depth = 0
    i = brace
    while i < len(reflink):
        if reflink[i] == "{":
            depth += 1
        elif reflink[i] == "}":
            depth -= 1
            if depth == 0:
                reflink = reflink[:start] + reflink[i + 1:]
                break
        i += 1

free_sig = "static infs_status file_free_unshared_run(\n    struct infs_volume *vol, const uint8_t owner_id[16],\n    uint64_t start, uint64_t count)"
if "shared_ref_index_lookup(" not in reflink:
    reflink = replace_function(reflink, free_sig, r'''static infs_status file_free_unshared_run(
    struct infs_volume *vol, const uint8_t owner_id[16],
    uint64_t start, uint64_t count)
{
    uint64_t cursor, end;

    (void)owner_id;
    if (!count)
        return INFS_STATUS_OK;
    if ((infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_SHARED_EXTENTS) == 0) {
        free_run(vol, start, count);
        return vol->tx_error;
    }
    if (start > UINT64_MAX - count)
        return INFS_STATUS_OVERFLOW;

    cursor = start;
    end = start + count;
    while (cursor < end) {
        uint64_t interval_end;
        uint32_t refs;
        infs_status status = shared_ref_index_lookup(
            vol, cursor, end, &interval_end, &refs);
        if (status != INFS_STATUS_OK)
            return status;
        if (interval_end <= cursor || refs == 0)
            return INFS_STATUS_CORRUPT;
        if (refs == 1) {
            free_run(vol, cursor, interval_end - cursor);
            if (vol->tx_error != INFS_STATUS_OK)
                return vol->tx_error;
        }
        cursor = interval_end;
    }
    return INFS_STATUS_OK;
}''', "replace portable reflink scan")
write("src/volume/reflink.inc", reflink)

core_inc = read("src/volume/core.inc")
if "shared_ref_index_invalidate(vol);" not in core_inc:
    core_inc = replace_function(
        core_inc, "static void mutation_scope_success(struct infs_volume *vol)",
        "static void mutation_scope_success(struct infs_volume *vol)\n{\n    tx_operation_commit(vol);\n    shared_ref_index_invalidate(vol);\n}",
        "shared-ref mutation invalidation")
    core_inc = insert_after_open_brace(
        core_inc,
        "static void mutation_scope_fail(struct infs_volume *vol, int started_transaction)",
        "shared_ref_index_invalidate(vol);",
        "shared-ref failure invalidation")
    write("src/volume/core.inc", core_inc)

# 3. Native kernel crypto API SHA-256 backend.
crypto_c = r'''// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratorfs_internal.h"
#include <crypto/hash.h>

static DEFINE_MUTEX(infilfs_crypto_lock);
static struct crypto_shash *infilfs_sha256_tfm;

static struct crypto_shash *infilfs_crypto_get_sha256(void)
{
    struct crypto_shash *tfm;

    mutex_lock(&infilfs_crypto_lock);
    tfm = infilfs_sha256_tfm;
    if (!tfm) {
        tfm = crypto_alloc_shash("sha256", 0, 0);
        if (!IS_ERR(tfm))
            infilfs_sha256_tfm = tfm;
    }
    mutex_unlock(&infilfs_crypto_lock);
    return tfm;
}

int infilfs_crypto_sha256(const u8 *data, size_t len, u8 out[32])
{
    struct crypto_shash *tfm = infilfs_crypto_get_sha256();
    struct shash_desc *desc;
    size_t bytes;
    int ret;

    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    bytes = sizeof(*desc) + crypto_shash_descsize(tfm);
    desc = kmalloc(bytes, GFP_NOFS);
    if (!desc)
        return -ENOMEM;
    desc->tfm = tfm;
    ret = crypto_shash_digest(desc, data, len, out);
    kfree_sensitive(desc);
    return ret;
}

int infilfs_crypto_sha256_zeropad(const u8 *data, size_t len,
                                  size_t padded_len, u8 out[32])
{
    static const u8 zero[256];
    struct crypto_shash *tfm = infilfs_crypto_get_sha256();
    struct shash_desc *desc;
    size_t bytes;
    int ret;

    if (len > padded_len)
        return -EINVAL;
    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    bytes = sizeof(*desc) + crypto_shash_descsize(tfm);
    desc = kmalloc(bytes, GFP_NOFS);
    if (!desc)
        return -ENOMEM;
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
    kfree_sensitive(desc);
    return ret;
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
'''
write("kernel/infiltratorfs_crypto.c", crypto_c)

kmake = read("kernel/Makefile")
if "infiltratorfs_crypto.o" not in kmake:
    kmake = replace_once(
        kmake,
        "infiltratorfs-y := infiltratorfs_core.o infiltratorfs_allocation_map.o",
        "infiltratorfs-y := infiltratorfs_core.o infiltratorfs_crypto.o infiltratorfs_allocation_map.o",
        "kernel crypto Kbuild object")
    write("kernel/Makefile", kmake)

internal = read("kernel/infiltratorfs_internal.h")
if "infilfs_crypto_sha256(" not in internal:
    anchor = "bool infilfs_crc64_block_valid(\n    const u8 block[INFILFS_DISK_BLOCK_SIZE],\n    size_t checksum_offset, size_t checksum_size);\nint infilfs_read_block(struct super_block *sb, u64 block, void *out);\n"
    internal = replace_once(
        internal, anchor,
        anchor + "int infilfs_crypto_sha256(const u8 *data, size_t len, u8 out[32]);\nint infilfs_crypto_sha256_zeropad(const u8 *data, size_t len,\n                                  size_t padded_len, u8 out[32]);\nvoid infilfs_crypto_exit(void);\n",
        "kernel crypto prototypes")
    write("kernel/infiltratorfs_internal.h", internal)

rw_data = read("kernel/infiltratorfs_rw_data.inc")
block_sig = "void infilfs_native_block_digest(\n    const u8 data[INFILFS_DISK_BLOCK_SIZE],\n    struct infilfs_data_checksum_disk *digest)"
if block_sig in rw_data and "infilfs_crypto_sha256(data" not in rw_data[rw_data.find(block_sig):rw_data.find(block_sig)+800]:
    rw_data = replace_function(rw_data, block_sig, r'''void infilfs_native_block_digest(
    const u8 data[INFILFS_DISK_BLOCK_SIZE],
    struct infilfs_data_checksum_disk *digest)
{
    struct infilfs_rw_sha256_ctx ctx;

    if (!infilfs_crypto_sha256(data, INFILFS_DISK_BLOCK_SIZE, digest->bytes))
        return;
    infilfs_rw_sha256_init(&ctx);
    infilfs_rw_sha256_update(&ctx, data, INFILFS_DISK_BLOCK_SIZE);
    infilfs_rw_sha256_final(&ctx, digest->bytes);
}''', "kernel block digest")
    write("kernel/infiltratorfs_rw_data.inc", rw_data)

rw_legacy = read("kernel/infiltratorfs_rw_legacy.inc")
inline_sig = "int infilfs_rw_inline_digest(const u8 *data, size_t size, u8 out[32])"
if inline_sig in rw_legacy and "infilfs_crypto_sha256_zeropad" not in rw_legacy[rw_legacy.find(inline_sig):rw_legacy.find(inline_sig)+1000]:
    rw_legacy = replace_function(rw_legacy, inline_sig, r'''int infilfs_rw_inline_digest(const u8 *data, size_t size, u8 out[32])
{
    struct infilfs_rw_sha256_ctx ctx;
    u8 zero[64] = {0};
    size_t left;

    if (size > INFILFS_DISK_BLOCK_SIZE)
        return -EINVAL;
    if (!infilfs_crypto_sha256_zeropad(data, size, INFILFS_DISK_BLOCK_SIZE, out))
        return 0;
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
}''', "kernel inline digest")
    write("kernel/infiltratorfs_rw_legacy.inc", rw_legacy)

core = read("kernel/infiltratorfs_core.c")
exit_sig = "static void __exit infilfs_exit(void)"
if "infilfs_crypto_exit();" not in core[core.find(exit_sig):]:
    core = insert_after_open_brace(core, exit_sig, "infilfs_crypto_exit();", "kernel crypto teardown")
    write("kernel/infiltratorfs_core.c", core)

policy = r'''#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:-.}"
grep -Fq 'INFS_HAVE_OPENSSL_SHA256' "$root/CMakeLists.txt"
grep -Fq 'INFS_HAVE_BCRYPT_SHA256' "$root/CMakeLists.txt"
grep -Fq 'EVP_DigestUpdate' "$root/src/checksum.c"
grep -Fq 'BCryptHash' "$root/src/checksum.c"
grep -Fq 'crypto_alloc_shash("sha256"' "$root/kernel/infiltratorfs_crypto.c"
! grep -Fq 'file_other_reference_cover' "$root/src/volume/reflink.inc"
grep -Fq 'shared_ref_index_lookup' "$root/src/volume/reflink.inc"
grep -Fq 'shared_ref_index_rebuild' "$root/src/volume/shared-ref-index.inc"
'''
write("tests/structural-overhaul-policy.sh", policy)
print("Structural recovery phase 1 applied.")
