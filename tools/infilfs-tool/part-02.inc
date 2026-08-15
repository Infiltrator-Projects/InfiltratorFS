// SPDX-License-Identifier: GPL-3.0-or-later
static int command_write(struct infs_volume *vol, const char *host,
                         const char *dest, const char *offset_text)
{
    char *endp = NULL;
    errno = 0;
    unsigned long long off64 = strtoull(offset_text, &endp, 0);
    if (errno || !endp || *endp != '\0' || off64 > (unsigned long long)INT64_MAX) {
        errno = EINVAL;
        return die_errno("write offset");
    }
    int input = open(host, O_RDONLY | O_CLOEXEC);
    if (input < 0)
        return die_errno("open host file");
    struct stat st;
    if (fstat(input, &st) != 0 || st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX) {
        close(input);
        if (errno == 0) errno = EFBIG;
        return die_errno("host file size");
    }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = size ? malloc(size) : NULL;
    if (size && !buf) {
        close(input);
        return die_errno("allocate host buffer");
    }
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(input, buf + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            free(buf);
            close(input);
            if (n == 0) errno = EIO;
            return die_errno("read host file");
        }
        done += (size_t)n;
    }
    close(input);
    ssize_t written = infs_write_file(vol, dest, buf, size, (off_t)off64);
    free(buf);
    if (written < 0 || (size_t)written != size) {
        if (written >= 0) errno = EIO;
        return die_errno("write destination");
    }
    return 0;
}

static int command_map(struct infs_volume *vol, const char *path,
                       const char *logical_text)
{
    char *endp = NULL;
    errno = 0;
    unsigned long long logical = strtoull(logical_text, &endp, 0);
    if (errno || !endp || *endp != '\0') {
        errno = EINVAL;
        return die_errno("map logical block");
    }
    struct infs_lookup lk;
    if (infs_lookup_path(vol, path, &lk) != 0)
        return die_errno("map lookup");
    if (lk.type != INFS_OBJECT_FILE) {
        errno = EISDIR;
        return die_errno("map");
    }
    uint8_t object[INFS_BLOCK_SIZE];
    if (infs_pread_full(vol->fd, object, sizeof(object),
                        (off_t)(lk.block * INFS_BLOCK_SIZE)) != 0 ||
        !infs_validate_object_block(object))
        return die_errno("map metadata");
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)object;
    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(object + sizeof(*hdr));
    uint32_t count = le32toh(file->extent_count);
    struct infs_extent_disk *ext = (struct infs_extent_disk *)(file + 1);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t l = le64toh(ext[i].logical_block);
        uint32_t n = le32toh(ext[i].block_count);
        if ((uint64_t)logical >= l && (uint64_t)logical < l + n) {
            printf("%" PRIu64 "\n", le64toh(ext[i].physical_block) +
                   ((uint64_t)logical - l));
            return 0;
        }
    }
    errno = ENXIO;
    return die_errno("map");
}
