<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS 0.18.57

Fix Linux native read-only to read-write remount initialization, preserve mount policy across remounts, and synchronize writes before read-only transitions. Add native remount regression qualification.

Batch metadata reads during orphan discovery and remove redundant object-index lookups while retaining the full recovery scan and validation. Report orphan-scan duration in the kernel log.

Preserve the mounted filesystem root owner, permissions and ACLs in the desktop manager helper instead of changing them to the invoking user.

The on-disk format remains 0.18. Qualification includes native remount cycles, open-unlinked recovery and clean filesystem scrubs. These repairs do not resolve all large-volume performance problems: a million-file clone still takes about 23 seconds for orphan discovery, and boot and runtime stalls remain under investigation. This release does not claim those stalls are fixed.
