// SPDX-License-Identifier: GPL-3.0-or-later
static int remove_object(struct infs_volume *vol, const char *path, int want_directory)
{
    struct infs_lookup parent;
    char name[INFS_NAME_MAX + 1u];
    if (split_parent(vol, path, &parent, name) != 0)
        return -1;
    uint8_t parent_block[INFS_BLOCK_SIZE];
    if (read_object(vol, parent.block, parent_block) != 0)
        return -1;
    struct infs_directory_payload_disk *pp;
    if (dir_validate(parent_block, &pp) != 0)
        return -1;
    struct infs_dirent_disk *de = dir_find(pp, name, strlen(name), NULL);
    if (!de) {
        errno = ENOENT;
        return -1;
    }
    struct infs_lookup child;
    if (index_find(vol, de->object_id, &child) != 0)
        return -1;
    if (want_directory && child.type != INFS_OBJECT_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }
    if (!want_directory && child.type == INFS_OBJECT_DIRECTORY) {
        errno = EISDIR;
        return -1;
    }

    uint8_t child_block[INFS_BLOCK_SIZE];
    if (read_object(vol, child.block, child_block) != 0)
        return -1;
    if (child.type == INFS_OBJECT_DIRECTORY) {
        struct infs_directory_payload_disk *cp;
        if (dir_validate(child_block, &cp) != 0)
            return -1;
        if (le32toh(cp->entry_count) != 0) {
            errno = ENOTEMPTY;
            return -1;
        }
    } else if (child.type == INFS_OBJECT_FILE) {
        struct infs_file_payload_disk *fp;
        struct infs_extent_disk *ext;
        if (file_validate_volume(vol, child_block, &fp, &ext) != 0)
            return -1;
        uint32_t count = le32toh(fp->extent_count);
        for (uint32_t i = 0; i < count; ++i)
            free_run(vol, le64toh(ext[i].physical_block), le32toh(ext[i].block_count));
    }

    if (dir_remove(parent_block, name, NULL) != 0)
        return -1;
    if (child.type == INFS_OBJECT_DIRECTORY) {
        uint32_t nlink = le32toh(pp->stat.nlink);
        if (nlink > 2)
            pp->stat.nlink = htole32(nlink - 1u);
    }
    touch_mctime(&pp->stat);
    if (write_object(vol, parent.block, parent_block) != 0)
        return -1;
    if (index_remove(vol, child.object_id) != 0)
        return -1;
    free_run(vol, child.block, 1);
    return infs_volume_sync(vol);
}

int infs_unlink(struct infs_volume *vol, const char *path)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    return remove_object(vol, path, 0);
}

int infs_rmdir(struct infs_volume *vol, const char *path)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    return remove_object(vol, path, 1);
}

static int id_is_ancestor(struct infs_volume *vol, const uint8_t ancestor[16],
                          const uint8_t start[16])
{
    uint8_t cur[16];
    memcpy(cur, start, 16);
    for (unsigned depth = 0; depth < 1024; ++depth) {
        if (memcmp(cur, ancestor, 16) == 0)
            return 1;
        if (memcmp(cur, vol->sb.root_object_id, 16) == 0)
            return 0;
        struct infs_lookup lk;
        if (index_find(vol, cur, &lk) != 0)
            return -1;
        uint8_t block[INFS_BLOCK_SIZE];
        if (read_object(vol, lk.block, block) != 0)
            return -1;
        struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)block;
        if (id_is_zero(hdr->parent_id))
            return 0;
        memcpy(cur, hdr->parent_id, 16);
    }
    errno = ELOOP;
    return -1;
}

int infs_rename(struct infs_volume *vol, const char *oldpath, const char *newpath)
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    struct infs_lookup old_parent, new_parent;
    char old_name[INFS_NAME_MAX + 1u], new_name[INFS_NAME_MAX + 1u];
    if (split_parent(vol, oldpath, &old_parent, old_name) != 0 ||
        split_parent(vol, newpath, &new_parent, new_name) != 0)
        return -1;

    uint8_t old_block[INFS_BLOCK_SIZE], new_block[INFS_BLOCK_SIZE];
    if (read_object(vol, old_parent.block, old_block) != 0)
        return -1;
    struct infs_directory_payload_disk *op;
    if (dir_validate(old_block, &op) != 0)
        return -1;
    struct infs_dirent_disk *source = dir_find(op, old_name, strlen(old_name), NULL);
    if (!source) {
        errno = ENOENT;
        return -1;
    }
    uint8_t source_id[16];
    memcpy(source_id, source->object_id, 16);
    uint16_t source_type = le16toh(source->object_type);

    if (old_parent.block == new_parent.block) {
        memcpy(new_block, old_block, sizeof(new_block));
    } else if (read_object(vol, new_parent.block, new_block) != 0) {
        return -1;
    }
    struct infs_directory_payload_disk *np;
    if (dir_validate(new_block, &np) != 0)
        return -1;
    if (dir_find(np, new_name, strlen(new_name), NULL)) {
        errno = EEXIST;
        return -1;
    }

    if (source_type == INFS_OBJECT_DIRECTORY) {
        int a = id_is_ancestor(vol, source_id, new_parent.object_id);
        if (a < 0)
            return -1;
        if (a) {
            errno = EINVAL;
            return -1;
        }
    }

    if (old_parent.block == new_parent.block) {
        if (dir_remove(new_block, old_name, NULL) != 0 ||
            infs_object_finalize(new_block) != 0 ||
            dir_add(new_block, new_name, source_id, source_type) != 0)
            return -1;
        np = (struct infs_directory_payload_disk *)(new_block + sizeof(struct infs_object_header_disk));
        touch_mctime(&np->stat);
        if (write_object(vol, new_parent.block, new_block) != 0)
            return -1;
    } else {
        if (dir_add(new_block, new_name, source_id, source_type) != 0 ||
            dir_remove(old_block, old_name, NULL) != 0)
            return -1;
        op = (struct infs_directory_payload_disk *)(old_block + sizeof(struct infs_object_header_disk));
        np = (struct infs_directory_payload_disk *)(new_block + sizeof(struct infs_object_header_disk));
        if (source_type == INFS_OBJECT_DIRECTORY) {
            uint32_t on = le32toh(op->stat.nlink);
            if (on > 2)
                op->stat.nlink = htole32(on - 1u);
            np->stat.nlink = htole32(le32toh(np->stat.nlink) + 1u);
        }
        touch_mctime(&op->stat);
        touch_mctime(&np->stat);
        if (write_object(vol, old_parent.block, old_block) != 0 ||
            write_object(vol, new_parent.block, new_block) != 0)
            return -1;

        struct infs_lookup child;
        if (index_find(vol, source_id, &child) != 0)
            return -1;
        uint8_t child_block[INFS_BLOCK_SIZE];
        if (read_object(vol, child.block, child_block) != 0)
            return -1;
        struct infs_object_header_disk *ch = (struct infs_object_header_disk *)child_block;
        memcpy(ch->parent_id, new_parent.object_id, 16);
        struct infs_stat_disk *st = object_stat(child_block);
        if (st)
            st->ctime_ns = htole64(now_ns());
        if (write_object(vol, child.block, child_block) != 0)
            return -1;
    }
    return infs_volume_sync(vol);
}

static int mutate_stat(struct infs_volume *vol, const char *path,
                       int op, uint64_t a, uint64_t b,
                       const struct timespec tv[2])
{
    if (!vol->writable) {
        errno = EROFS;
        return -1;
    }
    struct infs_lookup lk;
    if (infs_lookup_path(vol, path, &lk) != 0)
        return -1;
    uint8_t block[INFS_BLOCK_SIZE];
    if (read_object(vol, lk.block, block) != 0)
        return -1;
    struct infs_stat_disk *st = object_stat(block);
    if (!st) {
        errno = EIO;
        return -1;
    }
    uint64_t n = now_ns();
    if (op == 1) {
        uint32_t old = le32toh(st->mode);
        st->mode = htole32((old & S_IFMT) | ((uint32_t)a & 07777u));
    } else if (op == 2) {
        if ((uid_t)a != (uid_t)-1)
            st->uid = htole32((uint32_t)a);
        if ((gid_t)b != (gid_t)-1)
            st->gid = htole32((uint32_t)b);
    } else if (op == 3) {
        uint64_t at = tv[0].tv_nsec == UTIME_NOW ? n :
            tv[0].tv_nsec == UTIME_OMIT ? le64toh(st->atime_ns) :
            (uint64_t)tv[0].tv_sec * 1000000000ull + (uint64_t)tv[0].tv_nsec;
        uint64_t mt = tv[1].tv_nsec == UTIME_NOW ? n :
            tv[1].tv_nsec == UTIME_OMIT ? le64toh(st->mtime_ns) :
            (uint64_t)tv[1].tv_sec * 1000000000ull + (uint64_t)tv[1].tv_nsec;
        st->atime_ns = htole64(at);
        st->mtime_ns = htole64(mt);
    }
    st->ctime_ns = htole64(n);
    if (write_object(vol, lk.block, block) != 0)
        return -1;
    return infs_volume_sync(vol);
}

int infs_chmod(struct infs_volume *vol, const char *path, mode_t mode)
{
    return mutate_stat(vol, path, 1, mode, 0, NULL);
}

int infs_chown(struct infs_volume *vol, const char *path, uid_t uid, gid_t gid)
{
    return mutate_stat(vol, path, 2, uid, gid, NULL);
}

int infs_utimens(struct infs_volume *vol, const char *path,
                 const struct timespec tv[2])
{
    if (!tv) {
        struct timespec now[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
        return mutate_stat(vol, path, 3, 0, 0, now);
    }
    return mutate_stat(vol, path, 3, 0, 0, tv);
}
