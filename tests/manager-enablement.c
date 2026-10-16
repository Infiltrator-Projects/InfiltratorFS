// SPDX-License-Identifier: GPL-3.0-or-later
#include "manager/infiltratorfs-manager-contract.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

// Checks remain executable under NDEBUG in optimized qualification builds.
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static struct infilfs_manager_enablement enabled_for(
    bool has_target, bool is_infiltrator, bool mounted, bool busy)
{
    const struct infilfs_manager_state state = {
        .has_target = has_target,
        .is_infiltrator = is_infiltrator,
        .mounted = mounted,
        .busy = busy,
        .volume_open = mounted
    };
    struct infilfs_manager_enablement enabled;
    memset(&enabled, 0xa5, sizeof(enabled));
    infilfs_manager_compute_enablement(&state, &enabled);
    return enabled;
}

int main(void)
{
    /*
     * Regression: filesystem discovery must not depend on filesystem discovery.
     * An unmounted target with no generic FSTYPE is exactly where Inspect is
     * needed to positively identify InfiltratorFS.
     */
    struct infilfs_manager_enablement unknown =
        enabled_for(true, false, false, false);
    CHECK(unknown.inspect);
    CHECK(!unknown.maintenance);
    CHECK(unknown.format);
    CHECK(!unknown.mount);
    CHECK(!unknown.unmount);
    CHECK(!unknown.files);

    struct infilfs_manager_enablement known =
        enabled_for(true, true, false, false);
    CHECK(known.inspect);
    CHECK(known.maintenance);
    CHECK(known.format);
    CHECK(known.mount);
    CHECK(!known.unmount);
    CHECK(known.files);

    struct infilfs_manager_enablement mounted =
        enabled_for(true, true, true, false);
    CHECK(!mounted.inspect);
    CHECK(!mounted.maintenance);
    CHECK(!mounted.format);
    CHECK(mounted.mount);
    CHECK(mounted.unmount);
    CHECK(mounted.files);

    struct infilfs_manager_enablement busy =
        enabled_for(true, false, false, true);
    CHECK(!busy.inspect);
    CHECK(!busy.maintenance);
    CHECK(!busy.format);
    CHECK(!busy.mount);

    struct infilfs_manager_enablement empty =
        enabled_for(false, false, false, false);
    CHECK(!empty.inspect);
    CHECK(!empty.maintenance);
    CHECK(!empty.format);
    CHECK(!empty.mount);

    return 0;
}
