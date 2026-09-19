// SPDX-License-Identifier: GPL-3.0-or-later
static int command_write(struct infs_volume *vol, const char *host,
                         const char *dest, const char *offset_text)
{
    uint64_t off64 = 0;
    if (!infiltratr_parse_u64_range(
            offset_text, 0, 0, (uint64_t)INT64_MAX, &off64)) {
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
    if (size && infiltratr_read_full(input, buf, size) != 0) {
        free(buf);
        close(input);
        return die_errno("read host file");
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

static int parse_resize_size(struct infs_volume *vol, const char *text,
                             uint64_t *bytes_out)
{
    uint64_t storage_bytes = 0;
    int is_device = 0;
    if (!vol || !text || !bytes_out)
        return 0;
    if (strcmp(text, "max") == 0) {
        if (infs_storage_get_size(&vol->storage, &storage_bytes,
                                  &is_device) != INFS_STATUS_OK)
            return 0;
        (void)is_device;
        if ((storage_bytes % INFS_BLOCK_SIZE) != 0)
            return 0;
        *bytes_out = storage_bytes;
        return 1;
    }

    uint64_t bytes = 0;
    if (!infiltratr_parse_binary_quantity_u64(text, &bytes))
        return 0;
    if ((bytes % INFS_BLOCK_SIZE) != 0)
        return 0;
    *bytes_out = bytes;
    return 1;
}

static int command_resize(struct infs_volume *vol, const char *size_text)
{
    uint64_t bytes = 0;
    if (!parse_resize_size(vol, size_text, &bytes)) {
        errno = EINVAL;
        return die_errno("resize size");
    }
    uint64_t before =
        infs_le64_to_cpu(vol->sb.total_blocks) * (uint64_t)INFS_BLOCK_SIZE;
    infs_status status = infs_volume_resize(vol, bytes);
    if (status != INFS_STATUS_OK)
        return die_status("resize", status);
    printf("resized=%" PRIu64 " previous=%" PRIu64 "\n", bytes, before);
    return 0;
}

static int command_map(struct infs_volume *vol, const char *path,
                       const char *logical_text)
{
    uint64_t logical = 0;
    if (!infiltratr_parse_u64(logical_text, 0, &logical)) {
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
        uint64_t logical_end = 0;
        if (!infiltratr_u64_add_checked(
                l, (uint64_t)n, &logical_end))
            return die_status("map metadata", INFS_STATUS_CORRUPT);
        if (logical >= l && logical < logical_end) {
            if (infs_le32_to_cpu(ext[i].flags) == INFS_EXTENT_HOLE) {
                puts("hole");
                return 0;
            }
            uint64_t physical = 0;
            if (!infiltratr_u64_add_checked(
                    infs_le64_to_cpu(ext[i].physical_block),
                    logical - l, &physical))
                return die_status("map metadata", INFS_STATUS_CORRUPT);
            printf("%" PRIu64 "\n", physical);
            return 0;
        }
    }
    return die_status("map", INFS_STATUS_NOT_FOUND);
}

static int command_punch(struct infs_volume *vol, const char *path,
                         const char *offset_text, const char *length_text)
{
    uint64_t offset = 0;
    uint64_t length = 0;
    if (!infiltratr_parse_u64(offset_text, 0, &offset)) {
        errno = EINVAL;
        return die_errno("punch offset");
    }
    if (!infiltratr_parse_u64(length_text, 0, &length)) {
        errno = EINVAL;
        return die_errno("punch length");
    }
    infs_status status = infs_punch_hole(vol, path, offset, length);
    return status == INFS_STATUS_OK ? 0 : die_status("punch", status);
}
