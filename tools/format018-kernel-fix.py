#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path('.')

def replace(path, old, new, count=None):
    p = ROOT / path
    text = p.read_text(encoding='utf-8')
    actual = text.count(old)
    if actual == 0:
        raise SystemExit(f'{path}: expected anchor not found: {old[:120]}')
    if count is not None and actual != count:
        raise SystemExit(f'{path}: expected {count} occurrences, found {actual}: {old[:120]}')
    p.write_text(text.replace(old, new), encoding='utf-8')

# Linux 6.8+ hides raw inode timestamps behind accessor helpers.
replace('kernel/infiltratorfs_core.c',
'''    inode->i_atime = infilfs_timestamp_decode(&attributes->access_time);
    inode->i_mtime = infilfs_timestamp_decode(&attributes->modification_time);
    inode->i_ctime = infilfs_timestamp_decode(&attributes->change_time);''',
'''    inode_set_atime_to_ts(
        inode, infilfs_timestamp_decode(&attributes->access_time));
    inode_set_mtime_to_ts(
        inode, infilfs_timestamp_decode(&attributes->modification_time));
    inode_set_ctime_to_ts(
        inode, infilfs_timestamp_decode(&attributes->change_time));''', 1)

replace('kernel/infiltratorfs_rw.inc',
        '    struct timespec64 before = inode->i_atime;',
        '    struct timespec64 before = inode_get_atime(inode);', 1)
replace('kernel/infiltratorfs_rw.inc',
        '        after = inode->i_atime;',
        '        after = inode_get_atime(inode);', 1)
replace('kernel/infiltratorfs_linux_meta.inc',
        '            inode->i_ctime = attr.ia_ctime;',
        '            inode_set_ctime_to_ts(inode, attr.ia_ctime);', 1)

# Format 0.18 timestamp fields are structures. Convert any remaining native
# metadata writes that still assign a scalar real-time nanosecond value to a
# nested common-attribute timestamp.
pattern = re.compile(
    r'(?P<lhs>[A-Za-z_][A-Za-z0-9_]*->attributes\.(?:birth|access|modification|change)_time)\s*=\s*'
    r'cpu_to_le64\(ktime_get_real_ns\(\)\);', re.M)
changed = 0
for path in (ROOT / 'kernel').rglob('*'):
    if path.suffix not in {'.c', '.h', '.inc'}:
        continue
    text = path.read_text(encoding='utf-8')
    def repl(match):
        return f'infilfs_timestamp_encode_ns(&{match.group("lhs")}, ktime_get_real_ns());'
    out, n = pattern.subn(repl, text)
    if n:
        path.write_text(out, encoding='utf-8')
        changed += n
if changed != 11:
    raise SystemExit(f'expected 11 remaining scalar nested timestamp writes, converted {changed}')

# The read-cache callback is intentionally wrapped by the Format 0.18 atime
# persistence layer. Keep the maintainability guard strict, but point it at the
# new final VFS alias instead of the pre-0.18 cached callback.
replace('tests/native-kernel-maintainability-policy.sh',
        "grep -Fq '#define infilfs_file_read_iter infilfs_file_read_iter_cached' \"$rw\" || \\\n    fail 'read-cache alias bridge changed'",
        "grep -Fq '#define infilfs_file_read_iter infilfs_file_read_iter_atime' \"$rw\" || \\\n    fail 'read-cache/atime alias bridge changed'", 1)

print('Native Format 0.18 kernel compatibility corrections applied.')
