// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format.h"
#include "infilfs/posix_io.h"
#include "infilfs/storage.h"

#include <errno.h>
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
