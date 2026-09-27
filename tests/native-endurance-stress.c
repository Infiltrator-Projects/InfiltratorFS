// SPDX-License-Identifier: GPL-3.0-or-later
// Mounted near-full and mixed-workload qualification. The JSON proof format is
// intentionally compatible with native-endurance-stress.py for independent QA.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define MIB (1024u * 1024u)
#define BLOCK 4096u

struct options {
    const char *root, *manifest_out, *verify_manifest;
    unsigned seconds, workers, reserve_mib, coarse_mib;
    bool skip_fill;
};
struct fragment {
    size_t coarse, refill, tiny;
    uint64_t after_coarse, after_holes, after_refill, after_second_holes, final_free;
    double elapsed;
};
struct worker {
    const struct options *options;
    unsigned id;
    size_t counts[8], iterations, namespace_count;
    double fsync_sum_ms, fsync_max_ms;
    uint64_t log_size, sparse_size;
    char proof_sha[65], hot_sha[65], log_sha[65], tail_sha[65], namespace_sha[65];
    char final_xattr[64];
    int error;
};

static void die(const char *message)
{
    fprintf(stderr, "endurance: %s: %s\n", message, strerror(errno));
    exit(1);
}
static void check(bool condition, const char *message)
{
    if (!condition) { errno = EIO; die(message); }
}
static double elapsed_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) die("clock");
    return now.tv_sec + now.tv_nsec / 1e9;
}
static char *path2(const char *a, const char *b)
{
    char *result = NULL;
    if (asprintf(&result, "%s/%s", a, b) < 0) die("allocate path");
    return result;
}
static uint64_t free_bytes(const char *path)
{
    struct statvfs st;
    if (statvfs(path, &st)) die("statvfs");
    return (uint64_t)st.f_bavail * st.f_frsize;
}
static uint64_t total_bytes(const char *path)
{
    struct statvfs st;
    if (statvfs(path, &st)) die("statvfs total");
    return (uint64_t)st.f_blocks * st.f_frsize;
}
static void write_all(int fd, const unsigned char *buffer, size_t size)
{
    while (size) {
        ssize_t n = write(fd, buffer, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) die("write");
        buffer += n; size -= (size_t)n;
    }
}
static void hex_digest(const unsigned char *digest, size_t length, char out[65])
{
    for (size_t i = 0; i < length; i++)
        snprintf(out + 2 * i, 3, "%02x", digest[i]);
    out[2 * length] = 0;
}
static void hash_file(const char *path, char out[65])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) die("SHA-256 init");
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open hash file");
    unsigned char block[65536];
    ssize_t n;
    while ((n = read(fd, block, sizeof block)) != 0) {
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || EVP_DigestUpdate(ctx, block, (size_t)n) != 1) die("SHA-256 update");
    }
    if (close(fd)) die("close hash file");
    unsigned char digest[32]; unsigned length = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &length) != 1 || length != 32) die("SHA-256 final");
    hex_digest(digest, 32, out);
    EVP_MD_CTX_free(ctx);
}
static void tail_hash(const char *path, char out[65])
{
    struct stat st;
    if (stat(path, &st)) die("stat sparse file");
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open sparse tail");
    off_t at = st.st_size > BLOCK ? st.st_size - BLOCK : 0;
    unsigned char data[BLOCK];
    ssize_t n = pread(fd, data, sizeof data, at);
    if (n < 0 || close(fd)) die("read sparse tail");
    unsigned char digest[32]; unsigned length = 0;
    if (EVP_Digest(data, (size_t)n, digest, &length, EVP_sha256(), NULL) != 1)
        die("hash sparse tail");
    hex_digest(digest, length, out);
}
static int compare_names(const void *left, const void *right)
{
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}
static size_t namespace_hash(const char *path, char out[65])
{
    DIR *directory = opendir(path);
    if (!directory) die("open namespace");
    char **names = NULL;
    size_t count = 0, capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            names = realloc(names, capacity * sizeof *names);
            if (!names) die("grow namespace names");
        }
        names[count] = strdup(entry->d_name);
        if (!names[count]) die("copy namespace name");
        count++;
    }
    closedir(directory);
    qsort(names, count, sizeof *names, compare_names);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) die("namespace hash init");
    for (size_t i = 0; i < count; i++) {
        char *file = path2(path, names[i]);
        struct stat st;
        if (stat(file, &st)) die("stat namespace entry");
        check(S_ISREG(st.st_mode), "unexpected namespace entry");
        if (EVP_DigestUpdate(ctx, names[i], strlen(names[i]) + 1) != 1)
            die("namespace name hash");
        int fd = open(file, O_RDONLY);
        if (fd < 0) die("open namespace entry");
        unsigned char data[1024]; ssize_t n;
        while ((n = read(fd, data, sizeof data)) != 0) {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 || EVP_DigestUpdate(ctx, data, (size_t)n) != 1)
                die("namespace content hash");
        }
        close(fd);
        const unsigned char zero = 0;
        if (EVP_DigestUpdate(ctx, &zero, 1) != 1) die("namespace separator");
        free(file); free(names[i]);
    }
    free(names);
    unsigned char digest[32]; unsigned length = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &length) != 1) die("namespace hash final");
    hex_digest(digest, length, out);
    EVP_MD_CTX_free(ctx);
    return count;
}
static void write_extent(const char *path, size_t size, unsigned salt)
{
    char seed[80];
    snprintf(seed, sizeof seed, "infiltratorfs-endurance:%u", salt);
    unsigned char *pattern = malloc(MIB);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!pattern || !ctx || EVP_DigestInit_ex(ctx, EVP_shake256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, seed, strlen(seed)) != 1 ||
        EVP_DigestFinalXOF(ctx, pattern, MIB) != 1) die("SHAKE-256 extent");
    EVP_MD_CTX_free(ctx);
    int fd = open(path, O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) die("create extent");
    for (size_t left = size; left;) {
        size_t chunk = left > MIB ? MIB : left;
        write_all(fd, pattern, chunk);
        left -= chunk;
    }
    if (close(fd)) die("close extent");
    free(pattern);
}
static void fragment_volume(const struct options *opt, struct fragment *out)
{
    double started = elapsed_seconds();
    char *pool = path2(opt->root, "fragment-pool");
    if (mkdir(pool, 0700)) die("create fragment pool");
    uint64_t reserve = (uint64_t)opt->reserve_mib * MIB;
    uint64_t coarse = (uint64_t)opt->coarse_mib * MIB;
    check(coarse > 0, "coarse size must be positive");
    char name[64];
    while (free_bytes(opt->root) > reserve + coarse) {
        snprintf(name, sizeof name, "coarse-%05zu.bin", out->coarse);
        char *file = path2(pool, name);
        write_extent(file, (size_t)coarse, (unsigned)out->coarse);
        out->coarse++; free(file);
    }
    sync(); out->after_coarse = free_bytes(opt->root);
    for (size_t i = 0; i < out->coarse; i += 3) {
        snprintf(name, sizeof name, "coarse-%05zu.bin", i);
        char *file = path2(pool, name);
        if (unlink(file)) die("unlink coarse hole");
        free(file);
    }
    sync(); out->after_holes = free_bytes(opt->root);
    static const unsigned refill_sizes[] = {1, 2, 3, 5};
    while (free_bytes(opt->root) > reserve + 5u * MIB) {
        size_t size = refill_sizes[out->refill % 4] * MIB;
        if (free_bytes(opt->root) <= reserve + size) break;
        snprintf(name, sizeof name, "refill-%06zu.bin", out->refill);
        char *file = path2(pool, name);
        write_extent(file, size, 100000 + (unsigned)out->refill);
        out->refill++; free(file);
    }
    sync(); out->after_refill = free_bytes(opt->root);
    for (size_t i = 0; i < out->refill; i += 4) {
        snprintf(name, sizeof name, "refill-%06zu.bin", i);
        char *file = path2(pool, name);
        if (unlink(file)) die("unlink refill hole");
        free(file);
    }
    for (size_t i = 1; i < out->coarse; i += 7) {
        snprintf(name, sizeof name, "coarse-%05zu.bin", i);
        char *file = path2(pool, name);
        if (access(file, F_OK) == 0 && unlink(file)) die("unlink second coarse hole");
        free(file);
    }
    sync(); out->after_second_holes = free_bytes(opt->root);
    static const unsigned tiny_kib[] = {256, 512, 768, 1280};
    while (free_bytes(opt->root) > reserve + 1280u * 1024u) {
        size_t size = tiny_kib[out->tiny % 4] * 1024u;
        if (free_bytes(opt->root) <= reserve + size) break;
        snprintf(name, sizeof name, "tiny-%06zu.bin", out->tiny);
        char *file = path2(pool, name);
        write_extent(file, size, 200000 + (unsigned)out->tiny);
        out->tiny++; free(file);
    }
    out->final_free = free_bytes(opt->root);
    out->elapsed = elapsed_seconds() - started;
    printf("[ENDURANCE-PERF] fragmentation coarse=%zu refill=%zu tiny=%zu "
           "elapsed=%.3fs free_mib=%.1f free_pct=%.2f%%\n", out->coarse,
           out->refill, out->tiny, out->elapsed, out->final_free / (double)MIB,
           100.0 * out->final_free / total_bytes(opt->root));
    free(pool);
}
static uint64_t next_random(uint64_t *state)
{
    *state ^= *state >> 12;
    *state ^= *state << 25;
    *state ^= *state >> 27;
    return *state * UINT64_C(2685821657736338717);
}
static void make_block(unsigned char data[BLOCK], unsigned w, size_t iteration)
{
    for (size_t i = 0; i < BLOCK; i++)
        data[i] = (unsigned char)(w * 53 + iteration * 17 + (i % 256) * 29 + 7);
}
static void *worker_loop(void *context)
{
    struct worker *result = context;
    const struct options *opt = result->options;
    char name[64]; snprintf(name, sizeof name, "worker-%02u", result->id);
    char *dir = path2(opt->root, name);
    if (mkdir(dir, 0700)) die("create worker directory");
    char *ns = path2(dir, "namespace");
    if (mkdir(ns, 0700)) die("create namespace");
    char *hot = path2(dir, "hot.bin");
    char *log = path2(dir, "append.log");
    char *sparse = path2(dir, "sparse.bin");
    int fd_hot = open(hot, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    int fd_sparse = open(sparse, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    int fd_log = open(log, O_CREAT | O_EXCL | O_RDWR | O_APPEND | O_CLOEXEC, 0600);
    if (fd_hot < 0 || fd_sparse < 0 || fd_log < 0 ||
        ftruncate(fd_hot, 16 * MIB) || ftruncate(fd_sparse, 256 * MIB))
        die("initialize worker files");
    uint64_t random = UINT64_C(0x1f51a7e) + result->id * UINT64_C(0x10001);
    bool *visited = calloc(16 * MIB / BLOCK, sizeof *visited);
    unsigned char (*last)[BLOCK] = calloc(16 * MIB / BLOCK, sizeof *last);
    size_t *visited_indices = calloc(16 * MIB / BLOCK, sizeof *visited_indices);
    size_t visited_count = 0;
    if (!visited || !last || !visited_indices) die("allocate readback history");
    double deadline = elapsed_seconds() + opt->seconds;
    unsigned char block[BLOCK];
    while (elapsed_seconds() < deadline) {
        size_t iteration = result->iterations;
        int op = (int)(iteration % 8);
        if (op == 0) {
            size_t index = next_random(&random) % (16 * MIB / BLOCK);
            make_block(block, result->id, iteration);
            check(pwrite(fd_hot, block, sizeof block, (off_t)index * BLOCK) == BLOCK,
                  "hot pwrite");
            if (!visited[index]) {
                visited[index] = true;
                visited_indices[visited_count++] = index;
            }
            memcpy(last[index], block, BLOCK);
            result->counts[op]++;
        } else if (op == 1) {
            char record[80];
            int n = snprintf(record, sizeof record, "worker=%u iteration=%zu\n",
                             result->id, iteration);
            check(write(fd_log, record, (size_t)n) == n, "append record");
            result->counts[op]++;
        } else if (op == 2) {
            size_t slot = iteration % 128;
            snprintf(name, sizeof name, "tmp-%03zu", slot);
            char *tmp = path2(ns, name);
            snprintf(name, sizeof name, "live-%03zu", slot);
            char *final = path2(ns, name);
            if (unlink(tmp) && errno != ENOENT) die("remove old tmp");
            if (unlink(final) && errno != ENOENT) die("remove old live");
            int fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
            if (fd < 0) die("create namespace entry");
            char value[80]; int n = snprintf(value, sizeof value, "%u:%zu\n", result->id, iteration);
            write_all(fd, (const unsigned char *)value, (size_t)n);
            if (close(fd) || rename(tmp, final)) die("rename namespace entry");
            free(tmp); free(final); result->counts[op]++;
        } else if (op == 3) {
            char value[80], observed[80];
            int n = snprintf(value, sizeof value, "%u:%zu", result->id, iteration);
            if (setxattr(hot, "user.infiltratorfs-endurance", value, (size_t)n, 0))
                die("set hot xattr");
            check(getxattr(hot, "user.infiltratorfs-endurance", observed,
                           sizeof observed) == n && !memcmp(value, observed, (size_t)n),
                  "hot xattr readback");
            result->counts[op]++;
        } else if (op == 4) {
            off_t size = (off_t)(64 + iteration % 192) * MIB;
            if (ftruncate(fd_sparse, size)) die("truncate sparse");
            char tail[80]; int n = snprintf(tail, sizeof tail, "tail:%u:%zu",
                                            result->id, iteration);
            check(pwrite(fd_sparse, tail, (size_t)n, size - BLOCK) == n,
                  "sparse tail pwrite");
            result->counts[op]++;
        } else if (op == 5) {
            char *link = path2(dir, "hot.link"), *sym = path2(dir, "hot.sym");
            if (unlink(link) && errno != ENOENT) die("unlink old hard link");
            if (unlink(sym) && errno != ENOENT) die("unlink old symlink");
            if (linkat(AT_FDCWD, hot, AT_FDCWD, link, 0) ||
                symlink("hot.bin", sym)) die("create links");
            struct stat first, second;
            if (stat(hot, &first) || stat(link, &second)) die("stat links");
            check(first.st_ino == second.st_ino, "hard-link identity");
            char target[80]; ssize_t n = readlink(sym, target, sizeof target);
            check(n == 7 && !memcmp(target, "hot.bin", 7), "symlink target");
            if (unlink(link) || unlink(sym)) die("remove temporary links");
            free(link); free(sym); result->counts[op]++;
        } else if (op == 6) {
            if (visited_count) {
                size_t index = visited_indices[next_random(&random) % visited_count];
                check(pread(fd_hot, block, sizeof block, (off_t)index * BLOCK) == BLOCK &&
                      !memcmp(block, last[index], BLOCK), "random overwrite readback");
                result->counts[op]++;
            }
        } else {
            double started = elapsed_seconds();
            if (fsync(fd_hot) || fsync(fd_log) || fsync(fd_sparse)) die("worker fsync");
            double ms = (elapsed_seconds() - started) * 1000;
            result->fsync_sum_ms += ms;
            if (ms > result->fsync_max_ms) result->fsync_max_ms = ms;
            result->counts[op]++;
        }
        result->iterations++;
    }
    if (fsync(fd_hot) || fsync(fd_log) || fsync(fd_sparse) ||
        close(fd_hot) || close(fd_log) || close(fd_sparse)) die("final worker sync");
    char *proof = path2(dir, "proof.bin");
    int fd = open(proof, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) die("create proof");
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    if (!digest || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1)
        die("proof hash init");
    unsigned char *chunk = malloc(MIB);
    if (!chunk) die("allocate proof chunk");
    for (unsigned c = 0; c < 2; c++) {
        for (size_t i = 0; i < MIB; i++)
            chunk[i] = (unsigned char)(result->id * 71 + c * 31 + (i % 256) * 13 + 5);
        write_all(fd, chunk, MIB);
        if (EVP_DigestUpdate(digest, chunk, MIB) != 1) die("proof hash update");
    }
    free(chunk);
    if (fsync(fd) || close(fd)) die("proof sync");
    unsigned char proof_hash[32]; unsigned proof_length = 0;
    if (EVP_DigestFinal_ex(digest, proof_hash, &proof_length) != 1)
        die("proof hash final");
    hex_digest(proof_hash, proof_length, result->proof_sha);
    EVP_MD_CTX_free(digest);
    snprintf(result->final_xattr, sizeof result->final_xattr,
             "final:%u:%zu", result->id, result->iterations);
    if (setxattr(hot, "user.infiltratorfs-endurance", result->final_xattr,
                 strlen(result->final_xattr), 0)) die("final xattr");
    fd = open(hot, O_RDWR);
    if (fd < 0 || fsync(fd) || close(fd)) die("sync final hot file");
    char *final_link = path2(dir, "hot.final.link");
    char *final_sym = path2(dir, "hot.final.sym");
    if (unlink(final_link) && errno != ENOENT) die("unlink final link");
    if (unlink(final_sym) && errno != ENOENT) die("unlink final symlink");
    if (linkat(AT_FDCWD, hot, AT_FDCWD, final_link, 0) ||
        symlink("hot.bin", final_sym)) die("create final links");
    hash_file(hot, result->hot_sha);
    hash_file(log, result->log_sha);
    tail_hash(sparse, result->tail_sha);
    struct stat st;
    if (stat(log, &st)) die("stat log");
    result->log_size = (uint64_t)st.st_size;
    if (stat(sparse, &st)) die("stat sparse");
    result->sparse_size = (uint64_t)st.st_size;
    result->namespace_count = namespace_hash(ns, result->namespace_sha);
    free(dir); free(ns); free(hot); free(log); free(sparse);
    free(proof); free(final_link); free(final_sym);
    free(visited); free(last); free(visited_indices);
    return NULL;
}
static void write_manifest(const struct options *opt, const struct worker *workers,
                           const struct fragment *frag, uint64_t total,
                           uint64_t free_final)
{
    FILE *out = fopen(opt->manifest_out, "wx");
    if (!out) die("open manifest");
    fprintf(out, "{\n  \"version\": 1,\n  \"total_bytes\": %llu,\n"
            "  \"final_free_bytes\": %llu,\n  \"fragmentation\": {",
            (unsigned long long)total, (unsigned long long)free_final);
    if (!opt->skip_fill)
        fprintf(out, "\"coarse_files\": %zu, \"refill_files\": %zu, "
                "\"tiny_files\": %zu, \"free_after_coarse\": %llu, "
                "\"free_after_holes\": %llu, \"free_after_refill\": %llu, "
                "\"free_after_second_holes\": %llu, \"final_free\": %llu, "
                "\"elapsed\": %.6f", frag->coarse, frag->refill, frag->tiny,
                (unsigned long long)frag->after_coarse,
                (unsigned long long)frag->after_holes,
                (unsigned long long)frag->after_refill,
                (unsigned long long)frag->after_second_holes,
                (unsigned long long)frag->final_free, frag->elapsed);
    fputs("},\n  \"workers\": [\n", out);
    for (unsigned i = 0; i < opt->workers; i++) {
        const struct worker *w = &workers[i];
        double mean = w->counts[7] ? w->fsync_sum_ms / w->counts[7] : 0;
        fprintf(out, "    {\"worker\": %u, \"iterations\": %zu,\n"
                "     \"counts\": {\"pwrite\": %zu, \"append\": %zu, "
                "\"rename\": %zu, \"xattr\": %zu, \"truncate\": %zu, "
                "\"link\": %zu, \"readback\": %zu, \"fsync\": %zu},\n"
                "     \"proof\": \"worker-%02u/proof.bin\", \"proof_sha256\": \"%s\",\n"
                "     \"hot\": \"worker-%02u/hot.bin\", \"hot_sha256\": \"%s\",\n"
                "     \"final_xattr\": \"%s\", \"final_link\": \"worker-%02u/hot.final.link\",\n"
                "     \"final_sym\": \"worker-%02u/hot.final.sym\",\n"
                "     \"log\": \"worker-%02u/append.log\", \"log_size\": %llu, "
                "\"log_sha256\": \"%s\",\n"
                "     \"sparse\": \"worker-%02u/sparse.bin\", "
                "\"sparse_size\": %llu, \"sparse_tail_sha256\": \"%s\",\n"
                "     \"namespace_count\": %zu, \"namespace_sha256\": \"%s\",\n"
                "     \"fsync_mean_ms\": %.6f, \"fsync_max_ms\": %.6f}%s\n",
                i, w->iterations, w->counts[0], w->counts[1], w->counts[2],
                w->counts[3], w->counts[4], w->counts[5], w->counts[6],
                w->counts[7], i, w->proof_sha, i, w->hot_sha, w->final_xattr,
                i, i, i, (unsigned long long)w->log_size, w->log_sha,
                i, (unsigned long long)w->sparse_size, w->tail_sha,
                w->namespace_count, w->namespace_sha, mean, w->fsync_max_ms,
                i + 1 == opt->workers ? "" : ",");
    }
    fputs("  ]\n}\n", out);
    if (fflush(out) || fsync(fileno(out)) || fclose(out)) die("sync manifest");
}
static char *read_manifest(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) die("open manifest for verification");
    if (fseek(file, 0, SEEK_END)) die("seek manifest");
    long size = ftell(file);
    check(size >= 0 && size <= 16 * MIB, "manifest length");
    rewind(file);
    char *text = malloc((size_t)size + 1);
    if (!text) die("allocate manifest");
    check(fread(text, 1, (size_t)size, file) == (size_t)size, "read manifest");
    text[size] = 0; fclose(file); return text;
}
static void json_string(const char *object, const char *key,
                        char *out, size_t capacity)
{
    char quoted[128]; snprintf(quoted, sizeof quoted, "\"%s\"", key);
    const char *found = strstr(object, quoted);
    check(found != NULL, "manifest key missing");
    const char *start = strchr(found + strlen(quoted), ':');
    check(start != NULL, "manifest key separator missing");
    start = strchr(start, '"');
    check(start != NULL, "manifest value missing");
    start++;
    const char *end = strchr(start, '"');
    check(end != NULL && (size_t)(end - start) < capacity, "manifest value too long");
    memcpy(out, start, (size_t)(end - start)); out[end - start] = 0;
}
static uint64_t json_integer(const char *object, const char *key)
{
    char quoted[128]; snprintf(quoted, sizeof quoted, "\"%s\"", key);
    const char *found = strstr(object, quoted);
    check(found != NULL, "manifest numeric key missing");
    const char *start = strchr(found + strlen(quoted), ':');
    check(start != NULL, "manifest numeric separator missing");
    errno = 0; char *end;
    unsigned long long value = strtoull(start + 1, &end, 10);
    check(errno == 0 && end != start + 1, "manifest number invalid");
    return (uint64_t)value;
}
static void verify_manifest(const struct options *opt)
{
    char *text = read_manifest(opt->verify_manifest);
    check(json_integer(text, "version") == 1, "unknown manifest format");
    const char *cursor = strstr(text, "\"workers\"");
    check(cursor != NULL, "manifest workers missing");
    cursor = strchr(cursor, '[');
    check(cursor != NULL, "manifest workers array missing");
    cursor++;
    unsigned count = 0;
    for (;;) {
        while (*cursor == ' ' || *cursor == '\n' || *cursor == '\t' ||
               *cursor == ',') cursor++;
        if (*cursor == ']') break;
        check(*cursor == '{', "manifest worker object missing");
        const char *end = cursor;
        unsigned depth = 0;
        bool quoted = false, escaped = false;
        do {
            char ch = *end;
            check(ch != 0, "unterminated manifest worker");
            if (quoted) {
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') quoted = false;
            } else if (ch == '"') quoted = true;
            else if (ch == '{') depth++;
            else if (ch == '}') depth--;
            end++;
        } while (depth);
        char *object = strndup(cursor, (size_t)(end - cursor));
        if (!object) die("copy manifest worker");
        unsigned id = (unsigned)json_integer(object, "worker");
        check(id == count && id < 1024, "unexpected manifest worker identity");
        char directory_name[64]; snprintf(directory_name, sizeof directory_name,
                                          "worker-%02u", id);
        char *dir = path2(opt->root, directory_name);
        char *proof = path2(dir, "proof.bin");
        char *hot = path2(dir, "hot.bin");
        char *log = path2(dir, "append.log");
        char *sparse = path2(dir, "sparse.bin");
        char expected[128], actual[65];
        hash_file(proof, actual); json_string(object, "proof_sha256", expected, sizeof expected);
        check(!strcmp(actual, expected), "proof hash mismatch");
        hash_file(hot, actual); json_string(object, "hot_sha256", expected, sizeof expected);
        check(!strcmp(actual, expected), "hot hash mismatch");
        json_string(object, "final_xattr", expected, sizeof expected);
        char attr[128]; ssize_t length = getxattr(hot, "user.infiltratorfs-endurance",
                                                  attr, sizeof attr);
        check(length >= 0 && (size_t)length == strlen(expected) &&
              !memcmp(attr, expected, (size_t)length), "final xattr mismatch");
        char *link = path2(dir, "hot.final.link");
        char *sym = path2(dir, "hot.final.sym");
        struct stat a, b;
        if (stat(link, &a) || stat(hot, &b)) die("stat final link");
        check(a.st_ino == b.st_ino, "final hard-link mismatch");
        char target[16]; ssize_t n = readlink(sym, target, sizeof target);
        check(n == 7 && !memcmp(target, "hot.bin", 7), "final symlink mismatch");
        if (stat(log, &a)) die("stat log");
        check((uint64_t)a.st_size == json_integer(object, "log_size"), "log size mismatch");
        hash_file(log, actual); json_string(object, "log_sha256", expected, sizeof expected);
        check(!strcmp(actual, expected), "log hash mismatch");
        if (stat(sparse, &a)) die("stat sparse");
        check((uint64_t)a.st_size == json_integer(object, "sparse_size"), "sparse size mismatch");
        tail_hash(sparse, actual);
        json_string(object, "sparse_tail_sha256", expected, sizeof expected);
        check(!strcmp(actual, expected), "sparse tail mismatch");
        char *ns = path2(dir, "namespace");
        check(namespace_hash(ns, actual) == json_integer(object, "namespace_count"),
              "namespace count mismatch");
        json_string(object, "namespace_sha256", expected, sizeof expected);
        check(!strcmp(actual, expected), "namespace hash mismatch");
        free(dir); free(proof); free(hot); free(log); free(sparse);
        free(link); free(sym); free(ns); free(object);
        count++;
        cursor = end;
    }
    check(count > 0, "manifest has no workers");
    free(text);
    puts("Native near-full/mixed durable verification: PASS");
}
static unsigned parse_unsigned(const char *text, const char *name)
{
    if (*text == '-' || !*text) { fprintf(stderr, "%s invalid\n", name); exit(2); }
    errno = 0; char *end;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed > UINT32_MAX) {
        fprintf(stderr, "%s invalid\n", name); exit(2);
    }
    return (unsigned)parsed;
}
int main(int argc, char **argv)
{
    struct options opt = {NULL, NULL, NULL, 300, 4, 384, 16, false};
    static const struct option flags[] = {
        {"seconds", required_argument, NULL, 's'},
        {"workers", required_argument, NULL, 'w'},
        {"reserve-mib", required_argument, NULL, 'r'},
        {"coarse-mib", required_argument, NULL, 'c'},
        {"skip-fill", no_argument, NULL, 'k'},
        {"manifest-out", required_argument, NULL, 'm'},
        {"verify-manifest", required_argument, NULL, 'v'},
        {0, 0, 0, 0}
    };
    int choice;
    while ((choice = getopt_long(argc, argv, "", flags, NULL)) != -1) {
        switch (choice) {
        case 's': opt.seconds = parse_unsigned(optarg, "seconds"); break;
        case 'w': opt.workers = parse_unsigned(optarg, "workers"); break;
        case 'r': opt.reserve_mib = parse_unsigned(optarg, "reserve-mib"); break;
        case 'c': opt.coarse_mib = parse_unsigned(optarg, "coarse-mib"); break;
        case 'k': opt.skip_fill = true; break;
        case 'm': opt.manifest_out = optarg; break;
        case 'v': opt.verify_manifest = optarg; break;
        default: return 2;
        }
    }
    if (optind + 1 != argc) {
        fputs("usage: native-endurance-stress ROOT [--seconds N] [--workers N] "
              "[--reserve-mib N] [--coarse-mib N] [--skip-fill] "
              "[--manifest-out FILE] [--verify-manifest FILE]\n", stderr);
        return 2;
    }
    opt.root = argv[optind];
    if (opt.verify_manifest) { verify_manifest(&opt); return 0; }
    check(opt.seconds > 0 && opt.workers > 0 && opt.workers <= 64,
          "seconds and workers must be positive and workers <= 64");
    if (mkdir(opt.root, 0700)) die("create endurance root");
    uint64_t total = total_bytes(opt.root);
    check(opt.skip_fill || (uint64_t)opt.reserve_mib * MIB < total,
          "reserve must be smaller than volume");
    struct fragment frag = {0};
    if (!opt.skip_fill) fragment_volume(&opt, &frag);
    struct worker *workers = calloc(opt.workers, sizeof *workers);
    pthread_t *threads = calloc(opt.workers, sizeof *threads);
    if (!workers || !threads) die("allocate workers");
    double started = elapsed_seconds();
    for (unsigned i = 0; i < opt.workers; i++) {
        workers[i].id = i; workers[i].options = &opt;
        int rc = pthread_create(&threads[i], NULL, worker_loop, &workers[i]);
        if (rc) { errno = rc; die("start worker"); }
    }
    size_t operations = 0, fsync_count = 0;
    double fsync_sum = 0, fsync_max = 0;
    for (unsigned i = 0; i < opt.workers; i++) {
        int rc = pthread_join(threads[i], NULL);
        if (rc) { errno = rc; die("join worker"); }
        for (unsigned op = 0; op < 8; op++) operations += workers[i].counts[op];
        fsync_count += workers[i].counts[7];
        fsync_sum += workers[i].fsync_sum_ms;
        if (workers[i].fsync_max_ms > fsync_max) fsync_max = workers[i].fsync_max_ms;
    }
    double elapsed = elapsed_seconds() - started;
    sync();
    uint64_t free_final = free_bytes(opt.root);
    printf("[ENDURANCE-PERF] mixed seconds=%u workers=%u operations=%zu "
           "elapsed=%.3fs rate=%.1f ops/s\n", opt.seconds, opt.workers,
           operations, elapsed, operations / elapsed);
    printf("[ENDURANCE-PERF] fsync count=%zu mean_ms=%.3f max_ms=%.3f\n",
           fsync_count, fsync_count ? fsync_sum / fsync_count : 0, fsync_max);
    printf("[ENDURANCE-PERF] final free_mib=%.1f free_pct=%.2f%%\n",
           free_final / (double)MIB, 100.0 * free_final / total);
    if (opt.manifest_out)
        write_manifest(&opt, workers, &frag, total, free_final);
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage)) die("resource usage");
    printf("[ENDURANCE-PERF] controller maxrss_kib=%ld\n", usage.ru_maxrss);
    puts("Native near-full, fragmentation and mixed workload stress: PASS");
    free(workers); free(threads);
    return 0;
}
