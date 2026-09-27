// SPDX-License-Identifier: GPL-3.0-or-later
// Use the Linux POSIX ACL xattr ABI directly; no libacl link dependency.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#define ACCESS_NAME "system.posix_acl_access"
#define DEFAULT_NAME "system.posix_acl_default"
#define ACL_VERSION 2u
#define ACL_USER_OBJ 1u
#define ACL_USER 2u
#define ACL_GROUP_OBJ 4u
#define ACL_MASK 16u
#define ACL_OTHER 32u
#define ACL_READ 4u
#define ACL_WRITE 2u
#define ACL_EXECUTE 1u
#define ACL_UNDEFINED_ID 0xffffffffu

static void fail(const char *message)
{
    fprintf(stderr, "POSIX ACL qualification: %s: %s\n", message, strerror(errno));
    exit(1);
}

static void require(bool condition, const char *message)
{
    if (!condition) {
        errno = EIO;
        fail(message);
    }
}

static char *path2(const char *base, const char *name)
{
    char *path = NULL;
    if (asprintf(&path, "%s/%s", base, name) < 0)
        fail("allocate path");
    return path;
}

static void put16(unsigned char *out, unsigned value)
{
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *out, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        out[i] = (unsigned char)(value >> (8 * i));
}

static unsigned get16(const unsigned char *in)
{
    return (unsigned)in[0] | (unsigned)in[1] << 8;
}

static uint32_t get32(const unsigned char *in)
{
    return (uint32_t)in[0] | (uint32_t)in[1] << 8 |
           (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

static void build_acl(unsigned char out[44], uid_t uid, bool directory)
{
    static const unsigned tags[5] = {
        ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_MASK, ACL_OTHER
    };
    const unsigned permissions[5] = {
        ACL_READ | ACL_WRITE | (directory ? ACL_EXECUTE : 0),
        ACL_READ, 0, ACL_READ, 0
    };
    put32(out, ACL_VERSION);
    for (int i = 0; i < 5; i++) {
        unsigned char *entry = out + 4 + 8 * i;
        put16(entry, tags[i]);
        put16(entry + 2, permissions[i]);
        put32(entry + 4, i == 1 ? (uint32_t)uid : ACL_UNDEFINED_ID);
    }
}

static void acl_entries(const char *path, const char *name,
                        unsigned char *out, size_t *length)
{
    ssize_t result = getxattr(path, name, out, *length);
    if (result < 0)
        fail("read ACL xattr");
    require(result >= 4 && (result - 4) % 8 == 0 &&
            get32(out) == ACL_VERSION, "malformed ACL xattr");
    *length = (size_t)result;
}

static unsigned find_permission(const unsigned char *acl, size_t length,
                                unsigned tag, uint32_t id)
{
    unsigned matches = 0;
    unsigned permission = 0;
    for (size_t offset = 4; offset + 8 <= length; offset += 8) {
        if (get16(acl + offset) != tag)
            continue;
        if (tag == ACL_USER && get32(acl + offset + 4) != id)
            continue;
        matches++;
        permission = get16(acl + offset + 2);
    }
    require(matches == 1, "expected exactly one ACL entry");
    return permission;
}

static void check_named_read(const char *path, uid_t uid, unsigned mask)
{
    unsigned char acl[1024];
    size_t length = sizeof acl;
    acl_entries(path, ACCESS_NAME, acl, &length);
    require(find_permission(acl, length, ACL_USER, (uint32_t)uid) == ACL_READ,
            "named user ACL permission changed");
    require(find_permission(acl, length, ACL_MASK, 0) == mask,
            "ACL mask changed");
    require(find_permission(acl, length, ACL_OTHER, 0) == 0,
            "ACL other permission changed");
}

static bool readable_as(const char *path, uid_t uid, gid_t gid)
{
    pid_t pid = fork();
    if (pid < 0)
        fail("fork access probe");
    if (pid == 0) {
        if (setgroups(0, NULL) || setgid(gid) || setuid(uid))
            _exit(2);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            _exit(1);
        char byte;
        if (read(fd, &byte, 1) < 0)
            _exit(1);
        close(fd);
        _exit(0);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0)
        fail("wait access probe");
    require(WIFEXITED(status) && WEXITSTATUS(status) != 2,
            "unable to drop privileges for access probe");
    return WEXITSTATUS(status) == 0;
}

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0)
        fail("create ACL fixture");
    size_t length = strlen(text);
    while (length) {
        ssize_t written = write(fd, text, length);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            fail("write ACL fixture");
        text += written;
        length -= (size_t)written;
    }
    if (fsync(fd) || close(fd))
        fail("sync ACL fixture");
}

static void command(char *const argv[], char *output, size_t capacity)
{
    int pipefd[2] = {-1, -1};
    if (output && pipe(pipefd))
        fail("command output pipe");
    pid_t pid = fork();
    if (pid < 0)
        fail("fork command");
    if (pid == 0) {
        if (output) {
            close(pipefd[0]);
            if (dup2(pipefd[1], STDOUT_FILENO) < 0)
                _exit(127);
            close(pipefd[1]);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (output) {
        close(pipefd[1]);
        size_t used = 0;
        while (used < capacity - 1) {
            ssize_t n = read(pipefd[0], output + used, capacity - 1 - used);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                fail("read command output");
            if (!n)
                break;
            used += (size_t)n;
        }
        output[used] = 0;
        close(pipefd[0]);
        require(used < capacity - 1, "command output truncated");
    }
    int status;
    if (waitpid(pid, &status, 0) < 0)
        fail("wait command");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "setfacl/getfacl/rsync command failed");
}

static bool installed(const char *name)
{
    char *search = strdup(getenv("PATH") ? getenv("PATH") : "");
    if (!search)
        fail("copy PATH");
    bool found = false;
    char *save = NULL;
    for (char *item = strtok_r(search, ":", &save); item;
         item = strtok_r(NULL, ":", &save)) {
        char *candidate = path2(item, name);
        if (access(candidate, X_OK) == 0)
            found = true;
        free(candidate);
    }
    free(search);
    return found;
}

static void check_setfacl(const char *path, uid_t uid)
{
    char output[8192];
    char *args[] = {"getfacl", "-cpn", (char *)path, NULL};
    command(args, output, sizeof output);
    char expected[64];
    snprintf(expected, sizeof expected, "user:%u:r--", (unsigned)uid);
    require(strstr(output, expected) && strstr(output, "mask::r--"),
            "getfacl did not report named user and mask");
    struct stat st;
    if (stat(path, &st))
        fail("stat setfacl file");
    require((st.st_mode & 070) == 040, "ACL mask not reflected in group mode");
}

static void make_inherited(const char *directory, const char *name,
                           uid_t uid, gid_t gid)
{
    char *path = path2(directory, name);
    mode_t previous = umask(0077);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    umask(previous);
    if (fd < 0)
        fail("create inherited ACL file");
    const char text[] = "inherited-acl\n";
    require(write(fd, text, sizeof text - 1) == sizeof text - 1,
            "write inherited ACL file");
    if (fsync(fd) || close(fd))
        fail("sync inherited ACL file");
    check_named_read(path, uid, ACL_READ);
    require(readable_as(path, uid, gid), "default ACL did not grant access");
    free(path);
}

static void nobody(uid_t *uid, gid_t *gid)
{
    struct passwd *entry = getpwnam("nobody");
    *uid = entry ? entry->pw_uid : 65534;
    *gid = entry ? entry->pw_gid : 65534;
}

static void prepare(const char *root)
{
    uid_t uid; gid_t gid;
    nobody(&uid, &gid);
    char *base = path2(root, "posix-acl-qualification");
    if (mkdir(base, 0755) || chmod(base, 0755))
        fail("create ACL qualification directory");
    unsigned char access_acl[44], default_acl[44];
    build_acl(access_acl, uid, false);
    build_acl(default_acl, uid, true);
    char *access_file = path2(base, "access.txt");
    write_file(access_file, "named-user-access\n");
    require(!readable_as(access_file, uid, gid),
            "mode 0600 unexpectedly allowed nobody");
    if (setxattr(access_file, ACCESS_NAME, access_acl, sizeof access_acl, 0))
        fail("set access ACL");
    check_named_read(access_file, uid, ACL_READ);
    require(readable_as(access_file, uid, gid), "named ACL not enforced");

    bool has_tools = installed("setfacl") && installed("getfacl");
    bool require_tools = getenv("INFILFS_REQUIRE_SETFACL") &&
                         !strcmp(getenv("INFILFS_REQUIRE_SETFACL"), "1");
    if (has_tools) {
        char *real = path2(base, "setfacl.txt");
        write_file(real, "real-setfacl\n");
        char rule[64];
        snprintf(rule, sizeof rule, "u:%u:r--", (unsigned)uid);
        char *args[] = {"setfacl", "-m", rule, real, NULL};
        command(args, NULL, 0);
        check_setfacl(real, uid);
        require(readable_as(real, uid, gid), "setfacl ACL not enforced");
        free(real);
    } else if (require_tools) {
        require(false, "setfacl and getfacl required");
    } else {
        puts("native POSIX ACL qualification: setfacl/getfacl userspace check SKIP");
    }
    if (chmod(access_file, 0600))
        fail("chmod ACL mask 0600");
    check_named_read(access_file, uid, 0);
    require(!readable_as(access_file, uid, gid), "chmod 0600 did not restrict ACL");
    if (chmod(access_file, 0640))
        fail("chmod ACL mask 0640");
    check_named_read(access_file, uid, ACL_READ);
    require(readable_as(access_file, uid, gid), "chmod 0640 did not restore ACL");
    char *inherit = path2(base, "inherit");
    if (mkdir(inherit, 0755) || chmod(inherit, 0755) ||
        setxattr(inherit, DEFAULT_NAME, default_acl, sizeof default_acl, 0))
        fail("set default ACL");
    unsigned char observed[1024];
    size_t length = sizeof observed;
    acl_entries(inherit, DEFAULT_NAME, observed, &length);
    require(length == sizeof default_acl &&
            !memcmp(observed, default_acl, length), "default ACL readback");
    make_inherited(inherit, "before-remount.txt", uid, gid);
    require(installed("rsync"), "rsync required for root migration ACL check");
    char *src = path2(base, "rsync-src");
    char *dst = path2(base, "rsync-dst");
    if (mkdir(src, 0755) || mkdir(dst, 0755))
        fail("create rsync directories");
    char *source = path2(src, "file.txt");
    char *copied = path2(dst, "file.txt");
    write_file(source, "rsync-acl\n");
    if (setxattr(source, ACCESS_NAME, access_acl, sizeof access_acl, 0))
        fail("set source ACL");
    char *src_slash = path2(src, "");
    char *dst_slash = path2(dst, "");
    char *args[] = {"rsync", "-aA", src_slash, dst_slash, NULL};
    command(args, NULL, 0);
    int fd = open(copied, O_RDONLY);
    if (fd < 0)
        fail("open rsync copy");
    char data[32];
    ssize_t n = read(fd, data, sizeof data);
    close(fd);
    require(n == 10 && !memcmp(data, "rsync-acl\n", 10), "rsync data mismatch");
    size_t left = sizeof observed, right = sizeof default_acl;
    unsigned char other[44];
    acl_entries(source, ACCESS_NAME, observed, &left);
    acl_entries(copied, ACCESS_NAME, other, &right);
    require(left == right && !memcmp(observed, other, left),
            "rsync ACL copy mismatch");
    require(readable_as(copied, uid, gid), "rsync ACL not enforced");
    int dfd = open(base, O_RDONLY | O_DIRECTORY);
    if (dfd < 0 || fsync(dfd) || close(dfd))
        fail("sync ACL fixture directory");
    free(base); free(access_file); free(inherit);
    free(src); free(dst); free(source); free(copied);
    free(src_slash); free(dst_slash);
    puts("native POSIX ACL qualification: PREPARE PASS");
}

static void verify(const char *root)
{
    uid_t uid; gid_t gid;
    nobody(&uid, &gid);
    char *base = path2(root, "posix-acl-qualification");
    char *access_file = path2(base, "access.txt");
    check_named_read(access_file, uid, ACL_READ);
    require(readable_as(access_file, uid, gid), "access ACL lost on remount");
    char *real = path2(base, "setfacl.txt");
    if (access(real, F_OK) == 0) {
        check_setfacl(real, uid);
        require(readable_as(real, uid, gid), "setfacl ACL lost on remount");
    } else if (getenv("INFILFS_REQUIRE_SETFACL") &&
               !strcmp(getenv("INFILFS_REQUIRE_SETFACL"), "1")) {
        require(false, "required setfacl fixture missing after remount");
    }
    char *inherit = path2(base, "inherit");
    unsigned char expected[44], observed[1024];
    build_acl(expected, uid, true);
    size_t length = sizeof observed;
    acl_entries(inherit, DEFAULT_NAME, observed, &length);
    require(length == sizeof expected && !memcmp(expected, observed, length),
            "default ACL did not persist");
    char *before = path2(inherit, "before-remount.txt");
    check_named_read(before, uid, ACL_READ);
    make_inherited(inherit, "after-remount.txt", uid, gid);
    char *source = path2(base, "rsync-src/file.txt");
    char *copied = path2(base, "rsync-dst/file.txt");
    unsigned char left[1024], right[1024];
    size_t a = sizeof left, b = sizeof right;
    acl_entries(source, ACCESS_NAME, left, &a);
    acl_entries(copied, ACCESS_NAME, right, &b);
    require(a == b && !memcmp(left, right, a), "rsync ACL divergence");
    require(readable_as(copied, uid, gid), "rsync ACL lost after remount");
    free(base); free(access_file); free(real); free(inherit);
    free(before); free(source); free(copied);
    puts("native POSIX ACL qualification: REMOUNT PASS");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--encode-smoke")) {
        unsigned char acl[44];
        build_acl(acl, 65534, false);
        require(get32(acl) == ACL_VERSION &&
                find_permission(acl, sizeof acl, ACL_USER, 65534) == ACL_READ &&
                find_permission(acl, sizeof acl, ACL_MASK, 0) == ACL_READ,
                "ACL encoding smoke");
        puts("POSIX ACL encoding smoke: PASS");
        return 0;
    }
    if (argc != 3 || (strcmp(argv[1], "prepare") && strcmp(argv[1], "verify"))) {
        fprintf(stderr, "usage: %s prepare|verify MOUNTPOINT\n", argv[0]);
        return 2;
    }
    if (geteuid() != 0) {
        fputs("native POSIX ACL qualification must run as root\n", stderr);
        return 2;
    }
    char *root = realpath(argv[2], NULL);
    if (!root)
        fail("resolve mountpoint");
    if (!strcmp(argv[1], "prepare"))
        prepare(root);
    else
        verify(root);
    free(root);
    return 0;
}
