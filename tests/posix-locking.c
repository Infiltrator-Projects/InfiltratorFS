// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format.h"
#include "infilfs/posix_io.h"
#include "infilfs/storage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

static void fail(const char *message)
{
    fprintf(stderr, "posix-locking: %s\n", message);
    exit(1);
}

static void expect(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

int main(void)
{
    char path[] = "/tmp/infilfs-locking-XXXXXX";
    int fd = mkstemp(path);
    expect(fd >= 0, "create temporary storage target");
    expect(ftruncate(fd, (off_t)(4u * INFS_BLOCK_SIZE)) == 0,
           "size temporary storage target");
    uint64_t size_bytes = 0;
    int is_device = -1;
    errno = 0;
    expect(infs_get_size_bytes(fd, NULL, &is_device) == -1 && errno == EINVAL,
           "reject missing size output");
    errno = 0;
    expect(infs_get_size_bytes(fd, &size_bytes, NULL) == -1 && errno == EINVAL,
           "reject missing device-type output");
    expect(infs_get_size_bytes(fd, &size_bytes, &is_device) == 0 &&
               size_bytes == 4u * INFS_BLOCK_SIZE && !is_device,
           "read regular file geometry");
    errno = 0;
    expect(infs_random_bytes(NULL, 1) == -1 && errno == EINVAL,
           "reject missing entropy buffer");
    expect(infs_random_bytes(NULL, 0) == 0,
           "zero-length entropy request is a no-op");
    close(fd);

    expect(infs_status_from_errno(EWOULDBLOCK) == INFS_STATUS_BUSY,
           "map a contended POSIX lock to BUSY");
    expect(infs_status_to_errno(INFS_STATUS_BUSY) == EBUSY,
           "map BUSY back to a native busy error");

    struct infs_storage first = {0};
    struct infs_storage second = {0};
    struct infs_storage contender = {0};
    expect(infs_posix_storage_open(&first, path, 0) == INFS_STATUS_OK,
           "acquire first shared read lock");
    expect(infs_posix_storage_open(&second, path, 0) == INFS_STATUS_OK,
           "acquire second shared read lock");
    const struct infs_storage_ops *original_ops = first.ops;
    void *original_context = first.context;
    expect(infs_posix_storage_open(&first, path, 1) ==
               INFS_STATUS_INVALID_ARGUMENT &&
               first.ops == original_ops && first.context == original_context,
           "refuse to overwrite a live storage handle");
    fd = open(path, O_RDONLY | O_CLOEXEC);
    expect(fd >= 0, "open separate caller-owned descriptor");
    expect(infs_posix_storage_adopt_fd(&first, fd) ==
               INFS_STATUS_INVALID_ARGUMENT &&
               fcntl(fd, F_GETFD) >= 0 &&
               first.ops == original_ops && first.context == original_context,
           "rejected descriptor adoption preserves both owners");
    close(fd);
    expect(infs_posix_storage_open(&contender, path, 1) == INFS_STATUS_BUSY,
           "shared readers exclude a writer");
    infs_storage_close(&second);
    infs_storage_close(&first);

    expect(infs_posix_storage_open(&first, path, 1) == INFS_STATUS_OK,
           "acquire exclusive writer lock");
    expect(infs_posix_storage_open(&second, path, 0) == INFS_STATUS_BUSY,
           "writer excludes a reader");
    expect(infs_posix_storage_open(&contender, path, 1) == INFS_STATUS_BUSY,
           "writer excludes another writer");
    infs_storage_close(&first);

    expect(infs_posix_storage_open(&second, path, 0) == INFS_STATUS_OK,
           "closing writer releases exclusive lock");
    infs_storage_close(&second);
    expect(unlink(path) == 0, "remove temporary storage target");

    puts("posix-locking: PASS");
    return 0;
}
