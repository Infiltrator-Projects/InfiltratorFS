// SPDX-License-Identifier: GPL-3.0-or-later
static int mutate_stat_tx(struct infs_volume *vol, const char *path,
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

static int mutate_stat_guarded(struct infs_volume *vol, const char *path,
                               int op, uint64_t a, uint64_t b,
                               const struct timespec tv[2])
{
    if (tx_begin(vol) != 0)
        return -1;
    int rc = mutate_stat_tx(vol, path, op, a, b, tv);
    if (rc != 0) {
        int saved = errno;
        tx_abort(vol);
        errno = saved;
    }
    return rc;
}

int infs_chmod(struct infs_volume *vol, const char *path, mode_t mode)
{
    return mutate_stat_guarded(vol, path, 1, mode, 0, NULL);
}

int infs_chown(struct infs_volume *vol, const char *path, uid_t uid, gid_t gid)
{
    return mutate_stat_guarded(vol, path, 2, uid, gid, NULL);
}

int infs_utimens(struct infs_volume *vol, const char *path,
                 const struct timespec tv[2])
{
    if (!tv) {
        struct timespec now[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
        return mutate_stat_guarded(vol, path, 3, 0, 0, now);
    }
    return mutate_stat_guarded(vol, path, 3, 0, 0, tv);
}
