// SPDX-License-Identifier: GPL-3.0-or-later
#include "manager/infiltratorfs-manager-contract.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

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
    assert(unknown.inspect);
    assert(!unknown.maintenance);
    assert(unknown.format);
    assert(!unknown.mount);
    assert(!unknown.unmount);
    assert(!unknown.files);

    struct infilfs_manager_enablement known =
        enabled_for(true, true, false, false);
    assert(known.inspect);
    assert(known.maintenance);
    assert(known.format);
    assert(known.mount);
    assert(!known.unmount);
    assert(known.files);

    struct infilfs_manager_enablement mounted =
        enabled_for(true, true, true, false);
    assert(!mounted.inspect);
    assert(!mounted.maintenance);
    assert(!mounted.format);
    assert(mounted.mount);
    assert(mounted.unmount);
    assert(mounted.files);

    struct infilfs_manager_enablement busy =
        enabled_for(true, false, false, true);
    assert(!busy.inspect);
    assert(!busy.maintenance);
    assert(!busy.format);
    assert(!busy.mount);

    struct infilfs_manager_enablement empty =
        enabled_for(false, false, false, false);
    assert(!empty.inspect);
    assert(!empty.maintenance);
    assert(!empty.format);
    assert(!empty.mount);

    return 0;
}
