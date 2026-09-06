// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void die(const char *what)
{
    perror(what);
    exit(1);
}

static int rename_exchange(const char *a, const char *b)
{
    return syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, RENAME_EXCHANGE);
}

static void read_exact(const char *path, const char *expected)
{
    char buffer[64] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die("open read");
    ssize_t got = read(fd, buffer, sizeof(buffer) - 1u);
    close(fd);
    if (got < 0)
        die("read");
    if ((size_t)got != strlen(expected) ||
        memcmp(buffer, expected, strlen(expected)) != 0) {
        fprintf(stderr, "%s contains unexpected data\n", path);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <mounted-directory>\n", argv[0]);
        return 2;
    }
    int dirfd = open(argv[1], O_RDONLY | O_DIRECTORY);
    if (dirfd < 0)
        die("open directory");

    int fd = openat(dirfd, ".", O_TMPFILE | O_RDWR, 0640);
    if (fd < 0)
        die("O_TMPFILE");
    struct stat tmp_st;
    if (fstat(fd, &tmp_st) != 0)
        die("fstat tmpfile");
    if (tmp_st.st_nlink != 0) {
        fprintf(stderr, "anonymous tmpfile has link count %lu, expected 0\n",
                (unsigned long)tmp_st.st_nlink);
        return 1;
    }
    if (write(fd, "anonymous-data", 14) != 14)
        die("write tmpfile");
    if (fsync(fd) != 0)
        die("fsync tmpfile");
    if (linkat(fd, "", dirfd, "linked-tmpfile", AT_EMPTY_PATH) != 0)
        die("linkat tmpfile");
    if (fstat(fd, &tmp_st) != 0)
        die("fstat linked tmpfile");
    if (tmp_st.st_nlink != 1) {
        fprintf(stderr, "linked tmpfile has link count %lu, expected 1\n",
                (unsigned long)tmp_st.st_nlink);
        return 1;
    }
    close(fd);
    close(dirfd);

    char a[4096], b[4096], linked[4096];
    if (snprintf(a, sizeof(a), "%s/exchange-a", argv[1]) >= (int)sizeof(a) ||
        snprintf(b, sizeof(b), "%s/exchange-b", argv[1]) >= (int)sizeof(b) ||
        snprintf(linked, sizeof(linked), "%s/linked-tmpfile", argv[1]) >=
            (int)sizeof(linked))
        return 2;
    int fa = open(a, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    int fb = open(b, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fa < 0 || fb < 0)
        die("create exchange");
    if (write(fa, "alpha", 5) != 5 || write(fb, "bravo", 5) != 5)
        die("write exchange");
    close(fa);
    close(fb);
    if (rename_exchange(a, b) != 0)
        die("RENAME_EXCHANGE");
    read_exact(a, "bravo");
    read_exact(b, "alpha");
    read_exact(linked, "anonymous-data");

    errno = 0;
    if (syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b,
                RENAME_NOREPLACE) == 0 || errno != EEXIST) {
        fprintf(stderr, "RENAME_NOREPLACE did not return EEXIST\n");
        return 1;
    }
    puts("Native Linux API compatibility: PASS");
    return 0;
}
