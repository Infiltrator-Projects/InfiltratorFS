// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int make_path(char *out, size_t capacity,
                     const char *root, const char *name)
{
    int length = snprintf(out, capacity, "%s/%s", root, name);
    if (length < 0 || (size_t)length >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *bytes = buffer;
    size_t done = 0;
    while (done < size) {
        ssize_t written = write(fd, bytes + done, size - done);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)written;
    }
    return 0;
}

static int create_content(const char *path, const char *content)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0640);
    if (fd < 0)
        return -1;
    int rc = write_all(fd, content, strlen(content));
    if (close(fd) != 0)
        rc = -1;
    return rc;
}

static int expect_fd(int fd, const char *expected)
{
    char buffer[128] = {0};
    size_t length = strlen(expected);
    ssize_t got = pread(fd, buffer, length, 0);
    return got == (ssize_t)length && memcmp(buffer, expected, length) == 0 ?
        0 : -1;
}

static int expect_path(const char *path, const char *expected)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int rc = expect_fd(fd, expected);
    if (close(fd) != 0)
        rc = -1;
    return rc;
}

static int fail(const char *operation)
{
    fprintf(stderr, "fuse-open-handles: %s: %s\n",
            operation, strerror(errno));
    return 1;
}

static int hold_unlinked(const char *root, const char *ready_path)
{
    char retained[4096];
    if (make_path(retained, sizeof(retained), root, "interrupted-handle") != 0)
        return fail("construct interrupted path");
    int fd = open(retained, O_CREAT | O_TRUNC | O_RDWR, 0640);
    if (fd < 0 || write_all(fd, "retained", 8) != 0 ||
        unlink(retained) != 0 || fsync(fd) != 0)
        return fail("prepare interrupted open handle");
    if (create_content(ready_path, "ready\n") != 0)
        return fail("signal interrupted handle readiness");
    for (;;)
        pause();
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--hold-unlinked") == 0)
        return hold_unlinked(argv[2], argv[3]);
    if (argc != 2) {
        fprintf(stderr,
                "Usage: %s <mounted-directory>\n"
                "       %s --hold-unlinked <mounted-directory> <ready-file>\n",
                argv[0], argv[0]);
        return 2;
    }

    char original[4096], renamed[4096], source[4096], destination[4096];
    char directory_a[4096], directory_b[4096], nested_a[4096], nested_b[4096];
    if (make_path(original, sizeof(original), argv[1], "handle-original") ||
        make_path(renamed, sizeof(renamed), argv[1], "handle-renamed") ||
        make_path(source, sizeof(source), argv[1], "replace-source") ||
        make_path(destination, sizeof(destination), argv[1], "replace-destination") ||
        make_path(directory_a, sizeof(directory_a), argv[1], "directory-a") ||
        make_path(directory_b, sizeof(directory_b), argv[1], "directory-b") ||
        make_path(nested_a, sizeof(nested_a), directory_a, "nested") ||
        make_path(nested_b, sizeof(nested_b), directory_b, "nested"))
        return fail("construct paths");

    int fd = open(original, O_CREAT | O_TRUNC | O_RDWR, 0640);
    if (fd < 0 || write_all(fd, "before", 6) != 0)
        return fail("create open file");
    if (rename(original, renamed) != 0)
        return fail("rename open file");
    if (pwrite(fd, "after!", 6, 0) != 6 || expect_path(renamed, "after!") != 0)
        return fail("access renamed descriptor");
    if (unlink(renamed) != 0 || access(renamed, F_OK) == 0 || errno != ENOENT)
        return fail("unlink open file");
    if (expect_fd(fd, "after!") != 0 || ftruncate(fd, 3) != 0 ||
        expect_fd(fd, "aft") != 0 || fsync(fd) != 0)
        return fail("access unlinked descriptor");
    if (close(fd) != 0)
        return fail("close unlinked descriptor");

    if (create_content(source, "source") != 0 ||
        create_content(destination, "destination") != 0)
        return fail("create replacement files");
    fd = open(destination, O_RDONLY);
    if (fd < 0 || rename(source, destination) != 0)
        return fail("replace open destination");
    if (expect_fd(fd, "destination") != 0 ||
        expect_path(destination, "source") != 0)
        return fail("verify replacement lifetimes");
    if (unlink(destination) != 0 || expect_fd(fd, "destination") != 0 ||
        close(fd) != 0)
        return fail("release replaced destination");

    if (mkdir(directory_a, 0750) != 0 ||
        create_content(nested_a, "nested") != 0)
        return fail("create nested file");
    fd = open(nested_a, O_RDWR);
    if (fd < 0 || rename(directory_a, directory_b) != 0 ||
        pwrite(fd, "moved!", 6, 0) != 6 ||
        expect_path(nested_b, "moved!") != 0)
        return fail("rename parent of open file");
    if (unlink(nested_b) != 0 || expect_fd(fd, "moved!") != 0 ||
        close(fd) != 0 || rmdir(directory_b) != 0)
        return fail("unlink nested open file");

    puts("fuse-open-handles: PASS");
    return 0;
}
