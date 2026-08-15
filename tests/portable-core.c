// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/volume.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct memory_storage {
    uint8_t *bytes;
    size_t size;
    uint64_t random_state;
};

static int memory_read(void *context, uint64_t offset, void *buffer, size_t size)
{
    struct memory_storage *memory = context;
    if (offset > memory->size || size > memory->size - (size_t)offset) {
        errno = EIO;
        return -1;
    }
    memcpy(buffer, memory->bytes + (size_t)offset, size);
    return 0;
}

static int memory_write(void *context, uint64_t offset,
                        const void *buffer, size_t size)
{
    struct memory_storage *memory = context;
    if (offset > memory->size || size > memory->size - (size_t)offset) {
        errno = EIO;
        return -1;
    }
    memcpy(memory->bytes + (size_t)offset, buffer, size);
    return 0;
}

static int memory_flush(void *context)
{
    (void)context;
    return 0;
}

static int memory_size(void *context, uint64_t *size_bytes, int *is_device)
{
    struct memory_storage *memory = context;
    *size_bytes = memory->size;
    *is_device = 0;
    return 0;
}

static int memory_random(void *context, void *buffer, size_t size)
{
    struct memory_storage *memory = context;
    uint8_t *bytes = buffer;
    for (size_t i = 0; i < size; ++i) {
        memory->random_state ^= memory->random_state << 13;
        memory->random_state ^= memory->random_state >> 7;
        memory->random_state ^= memory->random_state << 17;
        bytes[i] = (uint8_t)memory->random_state;
    }
    return 0;
}

static int64_t memory_time(void *context)
{
    (void)context;
    return INT64_C(1786744800000000000);
}

static void memory_close(void *context)
{
    (void)context;
}

static const struct infs_storage_ops memory_ops = {
    .read_at = memory_read,
    .write_at = memory_write,
    .flush = memory_flush,
    .get_size = memory_size,
    .random_bytes = memory_random,
    .current_time_ns = memory_time,
    .close = memory_close,
};

static void fail(const char *message)
{
    fprintf(stderr, "portable-core: %s (errno=%d)\n", message, errno);
    exit(1);
}

static struct infs_storage make_storage(struct memory_storage *memory)
{
    struct infs_storage storage = {
        .ops = &memory_ops,
        .context = memory,
    };
    return storage;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <formatted-image>\n", argv[0]);
        return 2;
    }

    FILE *file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0)
        fail("open image");
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0)
        fail("size image");

    struct memory_storage memory = {
        .bytes = malloc((size_t)length),
        .size = (size_t)length,
        .random_state = UINT64_C(0x9e3779b97f4a7c15),
    };
    if (!memory.bytes || fread(memory.bytes, 1, memory.size, file) != memory.size)
        fail("read image");
    fclose(file);

    struct infs_storage storage = make_storage(&memory);
    struct infs_volume volume;
    if (infs_volume_open_storage(&volume, &storage, 1) != 0)
        fail("open through memory backend");

    const struct infs_create_options options = {
        .portable_flags = INFS_ATTR_ARCHIVE,
        .posix_permissions = 0640,
        .posix_uid = 1000,
        .posix_gid = 1000,
    };
    const char unicode_path[] = "/portable-\xc3\xa9";
    if (infs_create_file(&volume, unicode_path, &options) != 0)
        fail("create UTF-8 name");
    const char payload[] = "platform-neutral-storage";
    if (infs_write_file(&volume, unicode_path, payload,
                        sizeof(payload), 0) != (int64_t)sizeof(payload))
        fail("write through memory backend");

    const char invalid_path[] = "/invalid-\xc0\x80";
    errno = 0;
    if (infs_create_file(&volume, invalid_path, &options) == 0 || errno != EINVAL)
        fail("reject malformed UTF-8");
    infs_volume_close(&volume);

    storage = make_storage(&memory);
    if (infs_volume_open_storage(&volume, &storage, 0) != 0)
        fail("reopen through memory backend");
    char readback[sizeof(payload)] = {0};
    if (infs_read_file(&volume, unicode_path, readback,
                       sizeof(readback), 0) != (int64_t)sizeof(readback) ||
        memcmp(readback, payload, sizeof(payload)) != 0)
        fail("verify persisted bytes");
    struct infs_attributes attributes;
    if (infs_get_attributes(&volume, unicode_path, &attributes) != 0 ||
        attributes.birth_time_ns != memory_time(NULL) ||
        attributes.portable_flags != INFS_ATTR_ARCHIVE)
        fail("verify portable attributes");

    infs_volume_close(&volume);
    free(memory.bytes);
    puts("portable-core: PASS");
    return 0;
}
