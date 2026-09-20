# SPDX-License-Identifier: GPL-3.0-or-later

# Native driver synchronization and composition contract
# ------------------------------------------------------
# This is the implementation-level contract for the composite
# Linux driver. Keep it beside the Kbuild entry so repository and DKMS builds
# share the same rules instead of relying on tribal knowledge or README prose.
#
# Lock ordering / ownership:
#   resize_lock -> write_lock
#     resize_lock owns resize_active and geometry-change coordination. Resize
#     raises the gate and drains already-started work before taking write_lock.
#     A path holding write_lock must never acquire resize_lock.
#
#   quota_lock -> write_lock
#     quota_lock owns quota rules, project-root topology and live accounting.
#     Quota/project transitions may enter persistent mutation while quota_lock
#     is held. Ordinary data writers must not acquire quota_lock while holding
#     write_lock; reservation finish/abort occurs after write_lock is released.
#
#   write_lock -> bitmap_lock
#     write_lock owns persistent transaction/topology mutation. bitmap_lock is
#     a short non-sleeping inner lock protecting allocation visibility/publication.
#     Never sleep or acquire a filesystem mutex while bitmap_lock is held.
#
#   allocation-reservation shard spinlock -> bitmap_lock (when both are needed)
#     Reservation shard locks protect volatile reservation ranges only. They
#     must never be held across sleeping work or filesystem-mutex acquisition.
#
#   shared_range_build_lock -> write_lock (read side only)
#     Shared ownership-index construction may scan the live file population.
#     It is serialized by shared_range_build_lock while holding only the
#     topology read side, never write_lock writer-side, so discovery cannot
#     freeze unrelated stat/lookup/readdir readers behind an rm/eviction.
#
#   linux_meta_lock
#     Owns compound Linux sidecar metadata operations. It currently has no
#     general nesting permission with the mutexes above. Any future nested use
#     must document one direction here and at the call site before it lands.
#
#   pagecache_accounting_lock -> mapping page-cache/inode dirtying locks
#     Owns folio private accounting markers. It never nests filesystem mutexes
#     and is never acquired while holding mapping->i_pages or inode->i_lock.
#
# Transaction ownership:
#   write_lock is the authoritative pending-transaction merge/checkpoint
#   publication coordination domain. CPU-heavy data preparation, compression,
#   integrity hashing and lockless allocation reservation must happen before
#   entering this domain; it must not become a single-worker execution queue.
#   The native CPU execution budget is max(1, online logical CPUs - 1) across
#   all mounted InfiltratorFS volumes, leaving one logical CPU worth of
#   concurrency to the rest of the OS whenever more than one CPU is online.
#   resize_active gates new transactions/reservations before resize drains old
#   work. Buffered writes perform whole-request quota admission under inode_lock
#   before page-cache copying; per-folio reservations remain the authoritative
#   live accounting path and are reconciled without holding write_lock.
#   Cross-project preflight uses resident inode sizes when present so dirty
#   buffered data cannot evade destination project quota transfer accounting.
#
# Source composition:
#   infiltratorfs.ko is a composite Kbuild module.
#   infiltratorfs_core.c owns VFS/mount/checkpoint orchestration.
#   infiltratorfs_crypto.c owns accelerated integrity hashing.
#   infiltratorfs_allocation_map.c owns Format 0.18 allocation-map loading.
#   infiltratorfs_resize.c owns mounted geometry transitions.
#   infiltratorfs_index_tree.c owns Format 0.18 object-index reads/snapshots.
#   infiltratorfs_extent_tree.c owns scalable file extent-page indirection.
#   infiltratorfs_parallel_alloc.c owns volatile sharded data reservations.
#   infiltratorfs_allocation_publish.c owns CoW allocation-tree publication.
#   infiltratorfs_read_cache.c owns verified read cursor/page caching.
#   infiltratorfs_checksum_cache.c owns bounded checksum cursor/group caches.
#   infiltratorfs_checksum_store.c owns persistent checksum-chain object mutation.
#   infiltratorfs_locator_cache.c owns rebuildable native index/name/writer locators.
#   infiltratorfs_pagecache.c owns Linux folio/page-cache integration.
#   infiltratorfs_directory_tree.c owns scalable Format 0.18 directory trees.
#   infiltratorfs_linux_meta_codec.c owns Linux sidecar blob/naming codec rules.
#   infiltratorfs_shared_ownership.c owns incremental shared-extent reference accounting.
#   infiltratorfs_name_policy.c owns persistent cross-platform filename policy.
#   infiltratorfs_rw.inc is the only remaining nested implementation compositor;
#   leaf .inc files must not start including each other or creating hidden cycles.
#   Existing macro-renamed legacy/data entry points are migration debt, not a
#   pattern for new layers. New functionality must use explicit helper names.
#   The maintainability policy test enforces include ownership, key ordering,
#   alias-growth limits and byte ceilings so this structure cannot silently
#   become more tangled while qualified layers are gradually retired/split.

# Keep the Kbuild declaration unconditional. DKMS exports KERNELRELEASE before
# invoking this Makefile; if all normal targets are hidden behind a
# KERNELRELEASE conditional, the initial DKMS make terminates with
# "make: *** No targets. Stop." before Kbuild is ever entered.
obj-m += infiltratorfs.o
infiltratorfs-y := infiltratorfs_core.o infiltratorfs_crypto.o infiltratorfs_allocation_map.o infiltratorfs_resize.o infiltratorfs_index_tree.o infiltratorfs_extent_tree.o infiltratorfs_parallel_alloc.o infiltratorfs_allocation_publish.o infiltratorfs_read_cache.o infiltratorfs_pagecache.o infiltratorfs_directory_tree.o infiltratorfs_checksum_cache.o infiltratorfs_checksum_store.o infiltratorfs_locator_cache.o infiltratorfs_linux_meta_codec.o infiltratorfs_shared_ownership.o infiltratorfs_name_policy.o
ccflags-y += -I$(src)/../include/infilfs

KDIR ?= /lib/modules/$(shell uname -r)/build

# Module BTF can only be generated when the matching kernel's base BTF image is
# available. Distribution header packages commonly omit vmlinux, in which case
# Kbuild otherwise compiles the module successfully and then emits a misleading
# "Skipping BTF generation" message. Disable that optional stage only for such
# header trees; when vmlinux (or the newer vmlinux.unstripped) is present, keep
# the kernel's normal CONFIG_DEBUG_INFO_BTF_MODULES behaviour intact.
ifeq ($(strip $(wildcard $(KDIR)/vmlinux $(KDIR)/vmlinux.unstripped)),)
INFILTRATORFS_BTF_OVERRIDE := CONFIG_DEBUG_INFO_BTF_MODULES=
endif

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(INFILTRATORFS_BTF_OVERRIDE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
