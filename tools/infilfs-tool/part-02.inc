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
    int64_t written = infs_write_file(vol, dest, buf, size, (uint64_t)off64);
    free(buf);
    if (written < 0 || (size_t)written != size) {
        return die_status("write destination",
                          written < 0 ? (infs_status)written :
                                        INFS_STATUS_IO_ERROR);
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
    infs_status status = infs_lookup_path(vol, path, &lk);
    if (status != INFS_STATUS_OK)
        return die_status("map lookup", status);
    if (lk.type != INFS_OBJECT_FILE)
        return die_status("map", INFS_STATUS_IS_DIRECTORY);
    uint8_t object[INFS_BLOCK_SIZE];
    status = infs_storage_read(&vol->storage, lk.block * INFS_BLOCK_SIZE,
                               object, sizeof(object));
    if (status != INFS_STATUS_OK)
        return die_status("map metadata", status);
    if (!infs_validate_object_block(object))
        return die_status("map metadata", INFS_STATUS_CORRUPT);
    struct infs_object_header_disk *hdr = (struct infs_object_header_disk *)object;
    struct infs_file_payload_disk *file =
        (struct infs_file_payload_disk *)(object + sizeof(*hdr));
    uint32_t count = infs_le32_to_cpu(file->extent_count);
    struct infs_extent_disk *ext = (struct infs_extent_disk *)(file + 1);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t l = infs_le64_to_cpu(ext[i].logical_block);
        uint32_t n = infs_le32_to_cpu(ext[i].block_count);
        if ((uint64_t)logical >= l && (uint64_t)logical < l + n) {
            if (infs_le32_to_cpu(ext[i].flags) == INFS_EXTENT_HOLE) {
                puts("hole");
                return 0;
            }
            printf("%" PRIu64 "\n", infs_le64_to_cpu(ext[i].physical_block) +
                   ((uint64_t)logical - l));
            return 0;
        }
    }
    return die_status("map", INFS_STATUS_NOT_FOUND);
}

static int command_punch(struct infs_volume *vol, const char *path,
                         const char *offset_text, const char *length_text)
{
    char *offset_end = NULL;
    char *length_end = NULL;
    errno = 0;
    unsigned long long offset = strtoull(offset_text, &offset_end, 0);
    if (errno || !offset_end || *offset_end != '\0') {
        errno = EINVAL;
        return die_errno("punch offset");
    }
    errno = 0;
    unsigned long long length = strtoull(length_text, &length_end, 0);
    if (errno || !length_end || *length_end != '\0') {
        errno = EINVAL;
        return die_errno("punch length");
    }
    infs_status status = infs_punch_hole(vol, path, (uint64_t)offset,
                                         (uint64_t)length);
    return status == INFS_STATUS_OK ? 0 : die_status("punch", status);
}
