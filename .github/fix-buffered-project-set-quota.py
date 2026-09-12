from pathlib import Path

p = Path('kernel/infiltratorfs_quota.inc')
text = p.read_text(encoding='utf-8')
old = '        logical = le64_to_cpu(attributes->logical_size);\n'


def patch_section(text, function_name, sb_expr):
    start = text.index('static int ' + function_name + '(')
    end = text.index('\nstatic int ', start + 1)
    section = text[start:end]
    if section.count(old) != 1:
        raise SystemExit(f'{function_name}: expected one logical-size site, got {section.count(old)}')
    live = f'''        logical = le64_to_cpu(attributes->logical_size);\n        if (type == INFILFS_OBJECT_FILE) {{\n            struct infilfs_iget_args live_args = {{\n                .ino = infilfs_object_ino(entries[i].object_id),\n                .object_id = entries[i].object_id,\n            }};\n            struct inode *live_inode;\n\n            /*\n             * Buffered writes can advance i_size before the persistent object\n             * carries the new logical_size. Quota admission and project-domain\n             * transfer must therefore charge the VFS-visible size. Dirty page\n             * cache keeps the inode resident; otherwise the persisted size is\n             * authoritative.\n             */\n            live_inode = ilookup5(\n                {sb_expr}, (unsigned long)live_args.ino,\n                infilfs_inode_matches_id, &live_args);\n            if (live_inode) {{\n                loff_t live_size = i_size_read(live_inode);\n\n                if (live_size < 0) {{\n                    iput(live_inode);\n                    kfree(object);\n                    ret = -EFSCORRUPTED;\n                    break;\n                }}\n                logical = (u64)live_size;\n                iput(live_inode);\n            }}\n        }}\n'''
    section = section.replace(old, live, 1)
    return text[:start] + section + text[end:]


text = patch_section(text, 'infilfs_quota_rule_usage_locked', 'sb')
text = patch_section(text, 'infilfs_quota_domain_usage_locked', 'root->i_sb')
p.write_text(text, encoding='utf-8')
