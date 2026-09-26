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

    {
        void *direct_write = NULL;
        void *direct_read = NULL;
        int direct_fd;

        if (posix_memalign(&direct_write, 4096, 4096) != 0 ||
            posix_memalign(&direct_read, 4096, 4096) != 0)
            die("posix_memalign O_DIRECT");
        memset(direct_write, 0xa5, 4096);
        memset(direct_read, 0, 4096);
        direct_fd = openat(
            dirfd, "direct-io", O_CREAT | O_TRUNC | O_RDWR | O_DIRECT, 0600);
        if (direct_fd < 0)
            die("O_DIRECT open");
        if (write(direct_fd, direct_write, 4096) != 4096)
            die("O_DIRECT write");
        if (lseek(direct_fd, 0, SEEK_SET) != 0)
            die("O_DIRECT lseek");
        if (read(direct_fd, direct_read, 4096) != 4096)
            die("O_DIRECT read");
        if (memcmp(direct_write, direct_read, 4096) != 0) {
            fprintf(stderr, "O_DIRECT readback mismatch\n");
            return 1;
        }
        if (fsync(direct_fd) != 0)
            die("O_DIRECT fsync");
        close(direct_fd);
        if (unlinkat(dirfd, "direct-io", 0) != 0)
            die("O_DIRECT unlink");
        free(direct_read);
        free(direct_write);
    }

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

    {
        char dir_a[4096], dir_b[4096], cross_a[4096], cross_b[4096];
        if (snprintf(dir_a, sizeof(dir_a), "%s/exchange-dir-a", argv[1]) >=
                (int)sizeof(dir_a) ||
            snprintf(dir_b, sizeof(dir_b), "%s/exchange-dir-b", argv[1]) >=
                (int)sizeof(dir_b))
            return 2;
        if (mkdir(dir_a, 0700) != 0 || mkdir(dir_b, 0700) != 0)
            die("mkdir cross-directory exchange");
        if (snprintf(cross_a, sizeof(cross_a), "%s/left", dir_a) >=
                (int)sizeof(cross_a) ||
            snprintf(cross_b, sizeof(cross_b), "%s/right", dir_b) >=
                (int)sizeof(cross_b))
            return 2;

        int left = open(cross_a, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        int right = open(cross_b, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (left < 0 || right < 0)
            die("create cross-directory exchange");
        if (write(left, "left", 4) != 4 || write(right, "right", 5) != 5)
            die("write cross-directory exchange");
        close(left);
        close(right);

        if (rename_exchange(cross_a, cross_b) != 0)
            die("cross-directory RENAME_EXCHANGE");
        read_exact(cross_a, "right");
        read_exact(cross_b, "left");
    }

    puts("Native Linux API compatibility: PASS");
    return 0;
}
