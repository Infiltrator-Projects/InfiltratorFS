#!/usr/bin/env python3
from pathlib import Path

path = Path('kernel/infiltratorfs_rw.inc')
text = path.read_text()
old = '''        if (!S_ISREG(inode->i_mode))
            return -EISDIR;
        if (attr->ia_size < 0)
'''
new = '''        if (!S_ISREG(inode->i_mode)) {
            /*
             * Linux marks truncation requested by open(O_TRUNC) with
             * ATTR_OPEN.  O_TRUNC is deliberately ignored for character,
             * block, FIFO and socket inodes (for example /dev/null), while
             * an explicit truncate(2) of a non-regular inode remains an
             * error.  Treating every ATTR_SIZE on a special inode as EISDIR
             * made perfectly valid device opens fail during debootstrap.
             */
            if (attr->ia_valid & ATTR_OPEN) {
                metadata.ia_valid &= ~(ATTR_SIZE | ATTR_MTIME | ATTR_CTIME);
                goto size_change_done;
            }
            return S_ISDIR(inode->i_mode) ? -EISDIR : -EINVAL;
        }
        if (attr->ia_size < 0)
'''
if text.count(old) != 1:
    raise SystemExit(f'expected one non-regular truncate anchor, found {text.count(old)}')
text = text.replace(old, new, 1)
old2 = '''        truncate_pagecache(inode, attr->ia_size);
        metadata.ia_valid &= ~ATTR_SIZE;
    }

    disk_fields = metadata.ia_valid &
'''
new2 = '''        truncate_pagecache(inode, attr->ia_size);
        metadata.ia_valid &= ~ATTR_SIZE;
size_change_done:
        ;
    }

    disk_fields = metadata.ia_valid &
'''
if text.count(old2) != 1:
    raise SystemExit(f'expected one size-change tail anchor, found {text.count(old2)}')
path.write_text(text.replace(old2, new2, 1))
