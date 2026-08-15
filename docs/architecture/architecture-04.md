<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# InfiltratorFS Architecture — Part 4

## 17. Non-goals

The project intentionally rejects several design directions:

- no FAT-style linked allocation chains;
- no fixed-size global inode table as a fundamental identity mechanism;
- no single irreplaceable superblock;
- no unchecksummed critical metadata;
- no assumption that successful I/O implies correct data;
- no global always-on deduplication in the synchronous write path;
- no dependence on FUSE in the on-disk format;
- no attempt to preserve compatibility with another filesystem's disk structures.
