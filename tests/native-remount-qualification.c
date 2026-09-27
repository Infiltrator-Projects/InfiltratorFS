// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise VFS remount transitions against an already-mounted disposable image.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#define PAYLOAD_SIZE (4u * 1024u * 1024u)
#define ORPHAN_SIZE (9u * 8192u)

static const char *root;

static void fail(const char *operation)
{
    fprintf(stderr, "%s: %s (errno=%d)\n", operation, strerror(errno), errno);
    exit(1);
}

static void require(bool condition, const char *operation)
{
    if (!condition) {
        errno = EIO;
        fail(operation);
    }
}

static char *path(const char *relative)
{
    char *result = NULL;
    if (asprintf(&result, "%s/%s", root, relative) < 0)
        fail("allocate path");
    return result;
}

static void write_all(int fd, const unsigned char *data, size_t length)
{
    while (length) {
        ssize_t n = write(fd, data, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            fail("write payload");
        data += n;
        length -= (size_t)n;
    }
}

static void check_bytes(int fd, const unsigned char *expected, size_t length)
{
    unsigned char buffer[65536];
    while (length) {
        size_t want = length < sizeof buffer ? length : sizeof buffer;
        ssize_t n = read(fd, buffer, want);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || (size_t)n > want)
            fail("short payload readback");
        require(!memcmp(buffer, expected, (size_t)n), "payload readback");
        expected += n;
        length -= (size_t)n;
    }
    unsigned char extra;
    require(read(fd, &extra, 1) == 0, "payload has unexpected extra bytes");
}

static void read_file(const char *name, const unsigned char *expected, size_t length)
{
    char *p = path(name);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        fail("open readback");
    check_bytes(fd, expected, length);
    if (close(fd))
        fail("close readback");
    free(p);
}

static void write_file(const char *name, const unsigned char *data, size_t length)
{
    char *p = path(name);
    int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0)
        fail("open output");
    write_all(fd, data, length);
    if (close(fd))
        fail("close output");
    free(p);
}

static void remount(bool readonly, const char *options, int expected)
{
    errno = 0;
    int result = mount(NULL, root, NULL,
                       MS_REMOUNT | (readonly ? MS_RDONLY : 0), options);
    int observed = result == 0 ? 0 : errno;
    if (observed != expected) {
        fprintf(stderr, "remount(ro=%d, options=%s): errno %d, expected %d\n",
                readonly, options ? options : "(none)", observed, expected);
        exit(1);
    }
}

static void check_options(bool readonly)
{
    FILE *mounts = fopen("/proc/mounts", "r");
    if (!mounts)
        fail("open /proc/mounts");
    char *line = NULL;
    size_t capacity = 0;
    bool found = false;
    while (getline(&line, &capacity, mounts) >= 0) {
        char *save = NULL;
        (void)strtok_r(line, " \t", &save);
        char *target = strtok_r(NULL, " \t", &save);
        (void)strtok_r(NULL, " \t", &save);
        char *options = strtok_r(NULL, " \t\n", &save);
        if (!target || !options || strcmp(target, root))
            continue;
        bool mode = false, compress = false, media = false;
        char *option_save = NULL;
        for (char *opt = strtok_r(options, ",", &option_save); opt;
             opt = strtok_r(NULL, ",", &option_save)) {
            mode |= !strcmp(opt, readonly ? "ro" : "rw");
            compress |= !strcmp(opt, "compress=off");
            media |= !strcmp(opt, "media=balanced");
        }
        require(mode && compress && media, "remount options changed");
        found = true;
        break;
    }
    free(line);
    fclose(mounts);
    require(found, "mountpoint missing from /proc/mounts");
}

static void probe_readonly(void)
{
    char *p = path("ro-probe");
    errno = 0;
    int fd = open(p, O_WRONLY | O_CREAT, 0600);
    int observed = fd < 0 ? errno : 0;
    if (fd >= 0)
        close(fd);
    free(p);
    require(observed == EROFS, "read-only mount accepted a create");
}

static void check_quota(void)
{
    char *p = path("over-quota");
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        fail("open quota probe");
    unsigned char *block = malloc(1024 * 1024);
    if (!block)
        fail("allocate quota block");
    memset(block, 'q', 1024 * 1024);
    size_t written = 0;
    int observed = 0;
    while (written < 40u * 1024u * 1024u) {
        ssize_t n = write(fd, block, 1024 * 1024);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            observed = errno;
            break;
        }
        require(n > 0, "zero-length quota write");
        written += (size_t)n;
    }
    free(block);
    if (close(fd) && !observed)
        observed = errno;
    require(observed == EDQUOT, "quota was not enforced with EDQUOT");
    if (unlink(p))
        fail("remove quota probe");
    free(p);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "quota"))) {
        fprintf(stderr, "usage: %s MOUNTPOINT [quota]\n", argv[0]);
        return 2;
    }
    root = argv[1];
    check_options(true);
    probe_readonly();
    remount(false, "compress=auto", EINVAL);
    check_options(true);
    probe_readonly();
    remount(false, NULL, 0);
    check_options(false);
    if (argc == 3) {
        check_quota();
        puts("Persisted quota enforced after initial RO-to-RW promotion: PASS");
        return 0;
    }

    char *work = path("remount-work");
    if (mkdir(work, 0750) || chmod(work, 0750) || chown(work, 0, 0))
        fail("create remount work directory");
    const char metadata[] = "metadata";
    if (setxattr(work, "user.remount", metadata, sizeof metadata - 1, 0))
        fail("set work xattr");
    char value[sizeof metadata] = {0};
    require(getxattr(work, "user.remount", value, sizeof value) == sizeof metadata - 1 &&
            !memcmp(value, metadata, sizeof metadata - 1), "xattr readback");
    struct stat st;
    if (stat(work, &st))
        fail("stat work directory");
    require((st.st_mode & 0777) == 0750, "directory mode readback");
    unsigned char *payload = malloc(PAYLOAD_SIZE);
    if (!payload)
        fail("allocate payload");
    for (size_t i = 0; i < PAYLOAD_SIZE; i++)
        payload[i] = (unsigned char)i;
    // No fsync: a read-only remount must flush dirty page-cache writes.
    write_file("remount-work/buffered", payload, PAYLOAD_SIZE);
    char *before = path("remount-work/buffered");
    char *durable = path("remount-work/durable");
    if (rename(before, durable))
        fail("rename buffered to durable");
    free(before);
    write_file("snapshot-live.txt", (const unsigned char *)"after-remount\n", 14);

    char *busy = path("remount-work/busy");
    int fd = open(busy, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0)
        fail("open busy file");
    remount(true, NULL, EBUSY);
    check_options(false);
    if (close(fd) || unlink(busy))
        fail("close and unlink busy file");
    free(busy);

    unsigned char orphan_data[ORPHAN_SIZE];
    for (size_t i = 0; i < sizeof orphan_data; i++)
        orphan_data[i] = "keep-open"[i % 9];
    write_file("remount-work/open-unlinked", orphan_data, sizeof orphan_data);
    char *orphan = path("remount-work/open-unlinked");
    fd = open(orphan, O_RDONLY);
    if (fd < 0 || unlink(orphan))
        fail("open and unlink orphan");
    remount(true, NULL, EBUSY);
    check_bytes(fd, orphan_data, sizeof orphan_data);
    if (close(fd))
        fail("close orphan");
    free(orphan);

    fd = open(durable, O_RDONLY);
    if (fd < 0)
        fail("open linked reader");
    for (int i = 0; i < 3; i++) {
        remount(true, NULL, 0);
        check_options(true);
        probe_readonly();
        read_file("remount-work/durable", payload, PAYLOAD_SIZE);
        remount(false, "compress=off,media=balanced", 0);
        check_options(false);
        if (lseek(fd, 0, SEEK_SET) < 0)
            fail("seek linked reader");
        check_bytes(fd, payload, PAYLOAD_SIZE);
        write_file("remount-work/temporary", (const unsigned char *)"namespace write works", 21);
        char *temporary = path("remount-work/temporary");
        if (unlink(temporary))
            fail("unlink temporary");
        free(temporary);
    }
    if (close(fd))
        fail("close linked reader");
    remount(true, NULL, 0);
    probe_readonly();
    remount(false, NULL, 0);
    read_file("remount-work/durable", payload, PAYLOAD_SIZE);
    free(payload);
    free(durable);
    free(work);
    puts("Initial RO promotion, repeated live remounts, metadata, buffered writes, "
         "failed remounts and open-unlinked lifetime: PASS");
    return 0;
}
