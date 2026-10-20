// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Native mounted regression: a user-memory fault can short-copy a full-folio
 * overwrite. The untouched suffix must remain the original verified data,
 * including after fsync, cache eviction and a fresh read.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void fail(const char *what)
{
    perror(what);
    exit(1);
}

int main(int argc, char **argv)
{
    enum { BLOCK = 4096, AVAILABLE = 128 };
    char path[4096];
    unsigned char original[BLOCK], observed[BLOCK];
    long page_size = sysconf(_SC_PAGESIZE);
    unsigned char *mapping, *faulting;
    ssize_t written;
    int fd;

    if (argc != 2 || page_size < BLOCK || page_size > 1048576) {
        fprintf(stderr, "usage: %s <mounted-directory>\n", argv[0]);
        return 2;
    }
    if (snprintf(path, sizeof(path), "%s/short-write-regression-%ld",
                 argv[1], (long)getpid()) >= (int)sizeof(path)) {
        fputs("short-write path too long\n", stderr);
        return 2;
    }
    fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        fail("open short-write file");
    memset(original, 'A', sizeof(original));
    if (pwrite(fd, original, BLOCK, 0) != BLOCK)
        fail("initial short-write fixture");
    if (fsync(fd))
        fail("initial fsync");
    if (posix_fadvise(fd, 0, BLOCK, POSIX_FADV_DONTNEED))
        fail("evict original folio");

    mapping = mmap(NULL, (size_t)page_size * 2u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
        fail("mmap");
    memset(mapping, 'B', (size_t)page_size);
    if (mprotect(mapping + page_size, (size_t)page_size, PROT_NONE))
        fail("mprotect");
    faulting = mapping + page_size - AVAILABLE;
    errno = 0;
    written = pwrite(fd, faulting, BLOCK, 0);
    /*
     * A total EFAULT without an accepted prefix does not exercise the
     * interrupted-copy invariant. Require an actual partial overwrite so
     * this regression cannot pass without testing the vulnerable state.
     */
    if (written <= 0 || written > AVAILABLE) {
        fprintf(stderr, "expected a positive short write, got %zd errno=%d\n",
                written, errno);
        return 1;
    }
    if (fsync(fd))
        fail("post-fault fsync");
    if (posix_fadvise(fd, 0, BLOCK, POSIX_FADV_DONTNEED))
        fail("evict post-fault folio");
    if (pread(fd, observed, BLOCK, 0) != BLOCK)
        fail("verified post-fault read");
    for (size_t i = 0; i < BLOCK; ++i) {
        unsigned char expected = i < (size_t)written ? 'B' : 'A';
        if (observed[i] != expected) {
            fprintf(stderr,
                    "short-write corruption: byte=%zu wrote=%zd got=%u expected=%u\n",
                    i, written, observed[i], expected);
            return 1;
        }
    }
    if (munmap(mapping, (size_t)page_size * 2u))
        fail("munmap");
    if (close(fd))
        fail("close");
    if (unlink(path))
        fail("unlink");
    printf("short write preserved original suffix (accepted %zd bytes)\n",
           written);
    return 0;
}
