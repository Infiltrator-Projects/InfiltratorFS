#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Apply the documented native unlink shared-range ownership accelerator.

This migration is intentionally deterministic and anchor-checked. It exists so
an interrupted qualification can be resumed from Git without reconstructing the
implementation from chat history. Delete it after the qualified source change
has landed and the normal CI path owns the resulting implementation.
"""
from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    s = p.read_text()
    count = s.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one anchor, found {count}")
    p.write_text(s.replace(old, new, 1))


# Mount-private state. The retained index stores only ranges observed with two
# or more live file owners; authoritative filesystem metadata remains canonical.
replace_once(
    "kernel/infiltratorfs_internal.h",
    """struct infilfs_native_directory_locator {
    u32 hash;
    u16 name_len;
    bool valid;
    u8 name[INFILFS_NAME_MAX];
};

struct infilfs_native_pending {
""",
    """struct infilfs_native_directory_locator {
    u32 hash;
    u16 name_len;
    bool valid;
    u8 name[INFILFS_NAME_MAX];
};

struct infilfs_native_shared_range {
    u64 start;
    u64 end;
};

struct infilfs_native_pending {
""",
    "shared range type",
)
replace_once(
    "kernel/infiltratorfs_internal.h",
    """    u32 directory_locator_capacity;
    u32 directory_locator_count;
    bool directory_locator_valid;
    u64 pending_bytes;
""",
    """    u32 directory_locator_capacity;
    u32 directory_locator_count;
    bool directory_locator_valid;
    struct infilfs_native_shared_range *shared_ranges;
    size_t shared_range_count;
    bool shared_range_index_valid;
    u64 pending_bytes;
""",
    "shared range state",
)

# Failed operations conservatively invalidate acceleration state; unmount frees
# the retained array with the other mount-private pending-writer caches.
replace_once(
    "kernel/infiltratorfs_rw_data.inc",
    """    infilfs_native_index_locator_invalidate(pending);
    infilfs_native_directory_locator_invalidate(pending);
    if (first_error) {
""",
    """    infilfs_native_index_locator_invalidate(pending);
    infilfs_native_directory_locator_invalidate(pending);
    pending->shared_range_index_valid = false;
    pending->shared_range_count = 0;
    if (first_error) {
""",
    "rollback invalidation",
)
replace_once(
    "kernel/infiltratorfs_rw_data.inc",
    """    kvfree(pending->undo);
    kvfree(pending->index_locators);
    kvfree(pending->directory_locators);
    kfree(pending);
""",
    """    kvfree(pending->undo);
    kvfree(pending->index_locators);
    kvfree(pending->directory_locators);
    kvfree(pending->shared_ranges);
    kfree(pending);
""",
    "ownership index destroy",
)

ns = Path("kernel/infiltratorfs_rw_namespace.inc")
s = ns.read_text()
anchor = """static int infilfs_ns_other_reference_cover(
    struct super_block *sb, const u8 owner_id[16], u64 cursor, u64 end,
    u64 *cover_end, u64 *next_start)
"""
if s.count(anchor) != 1:
    raise SystemExit(f"ownership scan anchor: expected one, found {s.count(anchor)}")

accelerator = r'''/*
 * Volatile shared-range accelerator.
 *
 * The exact ownership scan below is authoritative but historically made every
 * unshared unlink walk every other live file. Build a compact sweep-line index
 * once from the same authoritative live-file extents. Only physical ranges
 * observed with at least two live file owners are retained. Absence from this
 * index therefore proves a range was unshared when the index was built.
 *
 * Ordinary CoW writes, truncates, deletes and defrag only retire references and
 * allocate fresh unique ranges. They can make this index over-conservative
 * (a formerly shared range may become unique) but cannot create a false
 * negative. A possibly shared range still uses the exact scan. The current
 * native VFS exposes no reflink/clone creation operation; any future operation
 * that introduces a second live owner must update or invalidate this index.
 * Snapshots are protected independently by the retained-snapshot allocation
 * union used when deferred frees are applied.
 */
struct infilfs_ns_shared_event {
    u64 position;
    s8 delta;
};

static int infilfs_ns_shared_event_compare(const void *a, const void *b)
{
    const struct infilfs_ns_shared_event *left = a;
    const struct infilfs_ns_shared_event *right = b;

    if (left->position < right->position)
        return -1;
    if (left->position > right->position)
        return 1;
    return 0;
}

static int infilfs_ns_shared_event_append(
    struct infilfs_ns_shared_event **events, size_t *count, size_t *capacity,
    u64 position, s8 delta)
{
    struct infilfs_ns_shared_event *grown;
    size_t next;

    if (*count == *capacity) {
        next = *capacity ? *capacity * 2u : 256u;
        if (next < *capacity || next > SIZE_MAX / sizeof(*grown))
            return -EOVERFLOW;
        grown = kvmalloc_array(next, sizeof(*grown), GFP_NOFS);
        if (!grown)
            return -ENOMEM;
        if (*count)
            memcpy(grown, *events, *count * sizeof(*grown));
        kvfree(*events);
        *events = grown;
        *capacity = next;
    }
    (*events)[*count].position = position;
    (*events)[*count].delta = delta;
    (*count)++;
    return 0;
}

static int infilfs_ns_shared_range_append(
    struct infilfs_native_shared_range **ranges, size_t *count,
    size_t *capacity, u64 start, u64 end)
{
    struct infilfs_native_shared_range *grown;
    size_t next;

    if (start >= end)
        return -EFSCORRUPTED;
    if (*count && (*ranges)[*count - 1u].end >= start) {
        if (end > (*ranges)[*count - 1u].end)
            (*ranges)[*count - 1u].end = end;
        return 0;
    }
    if (*count == *capacity) {
        next = *capacity ? *capacity * 2u : 64u;
        if (next < *capacity || next > SIZE_MAX / sizeof(*grown))
            return -EOVERFLOW;
        grown = kvmalloc_array(next, sizeof(*grown), GFP_NOFS);
        if (!grown)
            return -ENOMEM;
        if (*count)
            memcpy(grown, *ranges, *count * sizeof(*grown));
        kvfree(*ranges);
        *ranges = grown;
        *capacity = next;
    }
    (*ranges)[*count].start = start;
    (*ranges)[*count].end = end;
    (*count)++;
    return 0;
}

static int infilfs_ns_shared_range_index_build(
    struct infilfs_native_pending *pending)
{
    struct infilfs_index_entry_disk *entries = NULL;
    struct infilfs_ns_shared_event *events = NULL;
    struct infilfs_native_shared_range *ranges = NULL;
    size_t event_count = 0, event_capacity = 0;
    size_t range_count = 0, range_capacity = 0;
    s64 coverage = 0;
    u32 count = 0, i;
    size_t e = 0;
    u64 previous = 0;
    int ret;

    if (pending->shared_range_index_valid)
        return 0;

    ret = infilfs_ns_index_snapshot(pending->sb, &entries, &count);
    if (ret)
        goto out;

    for (i = 0; i < count; ++i) {
        struct infilfs_extent_disk *extents = NULL;
        u32 extent_count = 0, j;

        if (le16_to_cpu(entries[i].object_type) != INFILFS_OBJECT_FILE)
            continue;
        ret = infilfs_ns_read_file_extents(
            pending->sb, le64_to_cpu(entries[i].object_block),
            entries[i].object_id, &extents, &extent_count);
        if (ret) {
            kvfree(extents);
            goto out;
        }
        for (j = 0; j < extent_count; ++j) {
            u32 flags = le32_to_cpu(extents[j].flags);
            u32 logical_blocks = le32_to_cpu(extents[j].block_count);
            u64 physical;
            u64 blocks;
            u64 end;

            if (infilfs_extent_kind(flags) != INFILFS_EXTENT_NORMAL)
                continue;
            physical = le64_to_cpu(extents[j].physical_block);
            blocks = infilfs_extent_physical_blocks(logical_blocks, flags);
            if (!physical || !blocks || physical > U64_MAX - blocks) {
                ret = -EFSCORRUPTED;
                break;
            }
            end = physical + blocks;
            ret = infilfs_ns_shared_event_append(
                &events, &event_count, &event_capacity, physical, 1);
            if (!ret)
                ret = infilfs_ns_shared_event_append(
                    &events, &event_count, &event_capacity, end, -1);
            if (ret)
                break;
        }
        kvfree(extents);
        if (ret)
            goto out;
    }

    if (!event_count) {
        kvfree(pending->shared_ranges);
        pending->shared_ranges = NULL;
        pending->shared_range_count = 0;
        pending->shared_range_index_valid = true;
        ret = 0;
        goto out;
    }

    sort(events, event_count, sizeof(*events),
         infilfs_ns_shared_event_compare, NULL);
    previous = events[0].position;
    while (e < event_count) {
        u64 position = events[e].position;
        s64 delta = 0;

        if (position < previous) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        if (position > previous && coverage >= 2) {
            ret = infilfs_ns_shared_range_append(
                &ranges, &range_count, &range_capacity,
                previous, position);
            if (ret)
                goto out;
        }
        while (e < event_count && events[e].position == position) {
            delta += events[e].delta;
            e++;
        }
        if ((delta < 0 && coverage < -delta) ||
            (delta > 0 && coverage > S64_MAX - delta)) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        coverage += delta;
        if (coverage < 0) {
            ret = -EFSCORRUPTED;
            goto out;
        }
        previous = position;
    }
    if (coverage != 0) {
        ret = -EFSCORRUPTED;
        goto out;
    }

    kvfree(pending->shared_ranges);
    pending->shared_ranges = ranges;
    pending->shared_range_count = range_count;
    pending->shared_range_index_valid = true;
    ranges = NULL;
    ret = 0;
out:
    kvfree(ranges);
    kvfree(events);
    kvfree(entries);
    if (ret) {
        pending->shared_range_index_valid = false;
        pending->shared_range_count = 0;
    }
    return ret;
}

static bool infilfs_ns_shared_range_maybe_shared(
    const struct infilfs_native_pending *pending, u64 start, u64 end)
{
    size_t low = 0;
    size_t high = pending->shared_range_count;

    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const struct infilfs_native_shared_range *range =
            &pending->shared_ranges[middle];

        if (range->end <= start)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < pending->shared_range_count &&
        pending->shared_ranges[low].start < end;
}

'''
ns.write_text(s.replace(anchor, accelerator + anchor, 1))

# Put the accelerator before the exact scan. Memory pressure or impossible
# sizing falls back safely; metadata/I/O corruption still fails closed.
s = ns.read_text()
old = """    cursor = start;
    end = start + count;
    while (cursor < end) {
"""
new = """    cursor = start;
    end = start + count;

    ret = infilfs_ns_shared_range_index_build(pending);
    if (!ret &&
        !infilfs_ns_shared_range_maybe_shared(pending, start, end))
        return infilfs_rw_tx_defer_free(&pending->tx, start, count);
    if (ret && ret != -ENOMEM && ret != -EOVERFLOW)
        return ret;

    while (cursor < end) {
"""
count_anchor = s.count(old)
if count_anchor != 1:
    raise SystemExit(
        f"free-unshared fast-path anchor: expected one, found {count_anchor}"
    )
ns.write_text(s.replace(old, new, 1))

policy = Path("tests/native-unlink-ownership-index-policy.sh")
policy.write_text(
    r'''#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root="${1:-.}"
ns="$root/kernel/infiltratorfs_rw_namespace.inc"
state="$root/kernel/infiltratorfs_internal.h"
data="$root/kernel/infiltratorfs_rw_data.inc"

fail() { echo "native unlink ownership index policy: $*" >&2; exit 1; }
for file in "$ns" "$state" "$data"; do test -f "$file" || fail "missing $file"; done

grep -Fq 'struct infilfs_native_shared_range' "$state" || fail 'shared-range state missing'
grep -Fq 'shared_range_index_valid' "$state" || fail 'shared-range validity state missing'
grep -Fq 'infilfs_ns_shared_range_index_build' "$ns" || fail 'shared-range builder missing'
grep -Fq 'infilfs_ns_shared_range_maybe_shared' "$ns" || fail 'shared-range query missing'
grep -Fq 'infilfs_ns_other_reference_cover' "$ns" || fail 'exact ownership fallback missing'
grep -Fq 'kvfree(pending->shared_ranges);' "$data" || fail 'ownership index unmount cleanup missing'

python3 - "$ns" <<'PY'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
start = s.index('static int infilfs_ns_free_unshared_run(')
end = s.index('\nstatic int ', start + 1)
body = s[start:end]
build = body.find('infilfs_ns_shared_range_index_build(pending)')
query = body.find('infilfs_ns_shared_range_maybe_shared')
fastfree = body.find('return infilfs_rw_tx_defer_free(&pending->tx, start, count)')
scan = body.find('infilfs_ns_other_reference_cover(')
if min(build, query, fastfree, scan) < 0:
    raise SystemExit('unlink ownership fast/fallback sequence incomplete')
if not (build < query < fastfree < scan):
    raise SystemExit('whole-filesystem ownership scan is not behind the fast shared-range gate')
PY

echo 'Native unlink ownership index policy guard passed.'
'''
)
policy.chmod(0o755)

# Keep standard CI and the complete destructive qualification aware of the
# algorithmic guard after this one-off migration tool is removed.
replace_once(
    ".github/workflows/ci.yml",
    """      - name: Guard native random-write optimizations
        run: bash tests/native-random-write-optimization-policy.sh .
      - name: Guard native sequential-write scaling
""",
    """      - name: Guard native random-write optimizations
        run: bash tests/native-random-write-optimization-policy.sh .
      - name: Guard native unlink ownership scaling
        run: bash tests/native-unlink-ownership-index-policy.sh .
      - name: Guard native sequential-write scaling
""",
    "CI unlink guard",
)
replace_once(
    "tests/native-complete-qualification.sh",
    """bash tests/native-random-write-optimization-policy.sh .
bash tests/native-sequential-write-scaling-policy.sh .
""",
    """bash tests/native-random-write-optimization-policy.sh .
bash tests/native-unlink-ownership-index-policy.sh .
bash tests/native-sequential-write-scaling-policy.sh .
""",
    "complete qualification unlink guard",
)

print("unlink ownership index transformation applied")
