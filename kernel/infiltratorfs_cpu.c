// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Filesystem-wide CPU execution policy.
 *
 * CPU-heavy native work shares one unbound, reclaim-safe pool across mounts.
 * The live atomic gate enforces max(1, online physical cores - 1). SMT
 * siblings remain scheduler capacity but do not inflate the filesystem's
 * CPU-heavy worker budget. This keeps one complete core worth of execution
 * headroom for interactive/system work on hybrid and SMT machines.
 *
 * The workqueue is still sized from possible logical CPUs so CPU hotplug
 * cannot freeze the module at its load-time topology. No persistent filesystem
 * semantics live here.
 */
#include <linux/topology.h>

#include "infiltratorfs_internal.h"

static struct workqueue_struct *infilfs_cpu_wq;
static atomic_t infilfs_cpu_active;
static DECLARE_WAIT_QUEUE_HEAD(infilfs_cpu_wait);

static unsigned int infilfs_online_physical_cores(void)
{
    unsigned int cpu;
    unsigned int cores = 0;

    /*
     * topology_sibling_cpumask() groups SMT siblings that share one physical
     * core. Count exactly one currently-online representative from each group.
     * On non-SMT architectures the sibling mask contains only the CPU itself.
     */
    for_each_online_cpu(cpu) {
        unsigned int first = cpumask_first_and(
            topology_sibling_cpumask(cpu), cpu_online_mask);

        if (first == cpu)
            cores++;
    }

    return cores ? cores : 1u;
}

unsigned int infilfs_cpu_budget(void)
{
    unsigned int online_physical_cores = infilfs_online_physical_cores();

    /*
     * Architectural invariant: reserve one complete physical core worth of
     * CPU-heavy filesystem concurrency for the rest of the OS whenever a
     * second core exists. SMT siblings are capacity, not additional cores.
     */
    return online_physical_cores > 1u ? online_physical_cores - 1u : 1u;
}

bool infilfs_queue_cpu_work(struct work_struct *work)
{
    return infilfs_cpu_wq && queue_work(infilfs_cpu_wq, work);
}

void infilfs_mod_delayed_cpu_work(struct delayed_work *work,
                                  unsigned long delay)
{
    if (WARN_ON_ONCE(!infilfs_cpu_wq))
        return;
    mod_delayed_work(infilfs_cpu_wq, work, delay);
}

static bool infilfs_cpu_try_enter(void)
{
    int active;
    unsigned int budget;

    for (;;) {
        active = atomic_read(&infilfs_cpu_active);
        budget = infilfs_cpu_budget();
        if ((unsigned int)active >= budget)
            return false;
        if (atomic_cmpxchg(&infilfs_cpu_active, active, active + 1) == active)
            return true;
        cpu_relax();
    }
}

void infilfs_cpu_work_enter(void)
{
    wait_event(infilfs_cpu_wait, infilfs_cpu_try_enter());
}

void infilfs_cpu_work_exit(void)
{
    atomic_dec(&infilfs_cpu_active);
    wake_up(&infilfs_cpu_wait);
}

int infilfs_cpu_pool_init(void)
{
    unsigned int online_logical = num_online_cpus();
    unsigned int online_cores = infilfs_online_physical_cores();
    unsigned int possible = num_possible_cpus();
    unsigned int max_active = possible > 1u ? possible - 1u : 1u;
    unsigned int budget = online_cores > 1u ? online_cores - 1u : 1u;

    /*
     * max_active follows possible CPUs so later CPU onlining is not trapped by
     * the module-load count. The live atomic gate below enforces online-1 at
     * execution time, including CPU hotplug in either direction.
     */
    infilfs_cpu_wq = alloc_workqueue(
        "infiltratorfs-cpu", WQ_UNBOUND | WQ_MEM_RECLAIM, max_active);
    if (!infilfs_cpu_wq)
        return -ENOMEM;
    atomic_set(&infilfs_cpu_active, 0);
    pr_info("InfiltratorFS: CPU pool online_logical_cpus=%u online_physical_cores=%u filesystem_budget=%u reserved_for_os_cores=%u possible_logical_cpus=%u\n",
            online_logical, online_cores, budget,
            online_cores > 1u ? 1u : 0u, possible);
    return 0;
}

void infilfs_cpu_pool_exit(void)
{
    struct workqueue_struct *wq = infilfs_cpu_wq;

    infilfs_cpu_wq = NULL;
    if (wq)
        destroy_workqueue(wq);
}

