// SPDX-License-Identifier: GPL-3.0-or-later
// Native mounted scale workload. Paths and payloads match the Python reference.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct options {
    const char *root;
    size_t files, directories, churn_files, batch_directories, workers;
    bool reclaim_cache, verify_only;
};

enum phase { CREATE, REMOVE, RECREATE };
struct job {
    const struct options *options;
    enum phase phase;
    size_t first, last, worker, workers, completed;
    int error;
};

static void die(const char *message)
{
    fprintf(stderr, "%s: %s\n", message, strerror(errno));
    exit(1);
}

static void require(bool ok, const char *message)
{
    if (!ok) {
        errno = EIO;
        die(message);
    }
}

static double now(void)
{
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp))
        die("clock_gettime");
    return (double)stamp.tv_sec + stamp.tv_nsec * 1e-9;
}

static size_t parse_number(const char *text, const char *name)
{
    if (!text || !*text || *text == '-') {
        fprintf(stderr, "%s requires a nonnegative integer\n", name);
        exit(2);
    }
    errno = 0;
    char *end;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || value > SIZE_MAX) {
        fprintf(stderr, "%s has an invalid value: %s\n", name, text);
        exit(2);
    }
    return (size_t)value;
}

static char *directory_path(const struct options *opt, size_t directory)
{
    char *path = NULL;
    if (asprintf(&path, "%s/d%06zu", opt->root, directory) < 0)
        die("allocate directory path");
    return path;
}

static char *file_path(const char *directory, size_t index)
{
    char *path = NULL;
    if (asprintf(&path, "%s/f%06zu", directory, index) < 0)
        die("allocate file path");
    return path;
}

static size_t directory_file_count(const struct options *opt, size_t directory)
{
    return opt->files / opt->directories +
        (directory < opt->files % opt->directories);
}

static size_t churn_count(const struct options *opt, size_t directory)
{
    size_t wanted = opt->churn_files / opt->directories +
        (directory < opt->churn_files % opt->directories);
    size_t available = directory_file_count(opt, directory);
    return wanted < available ? wanted : available;
}

static int create_file(const char *path, size_t directory, bool sentinel)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (sentinel) {
        char payload[96];
        int length = snprintf(payload, sizeof payload,
                              "infiltratorfs-scale:%06zu\n", directory);
        if (length < 0 || (size_t)length >= sizeof payload) {
            errno = EOVERFLOW;
            close(fd);
            return -1;
        }
        size_t written = 0;
        while (written < (size_t)length) {
            ssize_t n = write(fd, payload + written, (size_t)length - written);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                if (!n) errno = EIO;
                int error = errno;
                close(fd);
                errno = error;
                return -1;
            }
            written += (size_t)n;
        }
    }
    return close(fd);
}

static void *worker(void *context)
{
    struct job *job = context;
    for (size_t directory = job->first + job->worker;
         directory < job->last; directory += job->workers) {
        char *dir = directory_path(job->options, directory);
        size_t count = job->phase == CREATE ?
            directory_file_count(job->options, directory) :
            churn_count(job->options, directory);
        for (size_t index = 0; index < count; index++) {
            char *path = file_path(dir, index);
            int result = job->phase == REMOVE ? unlink(path) :
                create_file(path, directory, index == 0);
            int error = errno;
            if (result) {
                fprintf(stderr, "%s: %s\n", path, strerror(error));
                free(path);
                free(dir);
                job->error = error;
                return NULL;
            }
            job->completed++;
            free(path);
        }
        free(dir);
    }
    return NULL;
}

static long meminfo(const char *key)
{
    FILE *stream = fopen("/proc/meminfo", "r");
    if (!stream) return -1;
    char *line = NULL;
    size_t capacity = 0;
    long value = -1;
    while (getline(&line, &capacity, stream) >= 0) {
        char label[64];
        long amount;
        if (sscanf(line, "%63[^:]: %ld kB", label, &amount) == 2 &&
            !strcmp(label, key)) {
            value = amount;
            break;
        }
    }
    free(line);
    fclose(stream);
    return value;
}

static void reclaim_cache(size_t batch, size_t first, size_t last)
{
    double started = now();
    long before = meminfo("MemAvailable");
    sync();
    // sudo -n preserves the original unprivileged harness contract.
    int result = system("sudo -n sh -c 'echo 2 > /proc/sys/vm/drop_caches' >/dev/null");
    require(result == 0, "reclaim VFS cache");
    printf("[SCALE-MEM] batch=%zu directories=%zu-%zu "
           "reclaim_elapsed=%.3fs available_before_kib=%ld "
           "available_after_kib=%ld slab_after_kib=%ld "
           "sreclaimable_after_kib=%ld\n", batch, first, last - 1,
           now() - started, before, meminfo("MemAvailable"), meminfo("Slab"),
           meminfo("SReclaimable"));
}

static double run_phase(const struct options *opt, enum phase phase,
                        size_t *completed)
{
    double started = now();
    *completed = 0;
    size_t batch_number = 0;
    for (size_t first = 0; first < opt->directories; first += opt->batch_directories) {
        size_t left = opt->directories - first;
        size_t count = left < opt->batch_directories ? left : opt->batch_directories;
        size_t threads_count = opt->workers < count ? opt->workers : count;
        pthread_t *threads = calloc(threads_count, sizeof *threads);
        struct job *jobs = calloc(threads_count, sizeof *jobs);
        if (!threads || !jobs)
            die("allocate workers");
        size_t launched = 0;
        for (; launched < threads_count; launched++) {
            jobs[launched] = (struct job){opt, phase, first, first + count,
                                          launched, threads_count, 0, 0};
            int error = pthread_create(&threads[launched], NULL, worker, &jobs[launched]);
            if (error) {
                errno = error;
                die("create worker");
            }
        }
        bool failed = false;
        for (size_t index = 0; index < launched; index++) {
            int error = pthread_join(threads[index], NULL);
            if (error) {
                errno = error;
                die("join worker");
            }
            *completed += jobs[index].completed;
            failed |= jobs[index].error != 0;
        }
        free(threads);
        free(jobs);
        require(!failed, "scale workload failed");
        batch_number++;
        if (opt->reclaim_cache)
            reclaim_cache(batch_number, first, first + count);
    }
    return now() - started;
}

static double verify(const struct options *opt)
{
    double started = now();
    size_t observed = 0;
    for (size_t directory = 0; directory < opt->directories; directory++) {
        char *dir = directory_path(opt, directory);
        DIR *stream = opendir(dir);
        if (!stream) die("open scale directory");
        size_t count = 0;
        struct dirent *entry;
        for (;;) {
            errno = 0;
            entry = readdir(stream);
            if (!entry) {
                require(errno == 0, "enumerate directory");
                break;
            }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            if (entry->d_type == DT_REG) {
                count++;
            } else if (entry->d_type == DT_UNKNOWN) {
                char *path = NULL;
                if (asprintf(&path, "%s/%s", dir, entry->d_name) < 0)
                    die("allocate enumeration path");
                struct stat st;
                if (lstat(path, &st)) die("stat enumerated file");
                if (S_ISREG(st.st_mode)) count++;
                free(path);
            }
        }
        closedir(stream);
        require(count == directory_file_count(opt, directory),
                "unexpected file population");
        observed += count;
        char *sentinel = file_path(dir, 0);
        int fd = open(sentinel, O_RDONLY);
        if (fd < 0) die("open sentinel");
        char expected[96], found[96];
        int length = snprintf(expected, sizeof expected,
                              "infiltratorfs-scale:%06zu\n", directory);
        ssize_t received = read(fd, found, sizeof found);
        require(length > 0 && received == length &&
                !memcmp(found, expected, (size_t)length), "sentinel readback");
        close(fd);
        free(sentinel);
        size_t expected_count = directory_file_count(opt, directory);
        size_t indices[] = {0, expected_count / 2, expected_count - 1};
        for (size_t i = 0; i < 3; i++) {
            char *path = file_path(dir, indices[i]);
            struct stat st;
            if (lstat(path, &st)) die("stat sampled file");
            require(S_ISREG(st.st_mode) && st.st_nlink == 1,
                    "sampled file metadata");
            free(path);
        }
        free(dir);
    }
    require(observed == opt->files, "total file population");
    return now() - started;
}

static size_t default_workers(void)
{
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof allowed, &allowed))
        return 1;
    struct { int package, core; } seen[CPU_SETSIZE];
    size_t cores = 0;
    size_t logical = 0;
    bool topology_ok = true;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        logical++;
        char filename[192];
        snprintf(filename, sizeof filename,
                 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
        FILE *stream = fopen(filename, "r");
        int package, core;
        if (!stream || fscanf(stream, "%d", &package) != 1) {
            if (stream) fclose(stream);
            topology_ok = false;
            break;
        }
        fclose(stream);
        snprintf(filename, sizeof filename,
                 "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        stream = fopen(filename, "r");
        if (!stream || fscanf(stream, "%d", &core) != 1) {
            if (stream) fclose(stream);
            topology_ok = false;
            break;
        }
        fclose(stream);
        bool known = false;
        for (size_t i = 0; i < cores; i++)
            known |= seen[i].package == package && seen[i].core == core;
        if (!known) {
            seen[cores].package = package;
            seen[cores].core = core;
            cores++;
        }
    }
    size_t available = topology_ok ? cores : logical;
    return available > 1 ? available - 1 : 1;
}

int main(int argc, char **argv)
{
    struct options opt = {NULL, 1000000, 1000, 100000, 64,
                          default_workers(), false, false};
    static const struct option flags[] = {
        {"files", required_argument, NULL, 'f'},
        {"directories", required_argument, NULL, 'd'},
        {"workers", required_argument, NULL, 'w'},
        {"churn-files", required_argument, NULL, 'c'},
        {"batch-directories", required_argument, NULL, 'b'},
        {"reclaim-vfs-cache", no_argument, NULL, 'r'},
        {"verify-only", no_argument, NULL, 'v'},
        {0, 0, 0, 0}
    };
    int choice;
    while ((choice = getopt_long(argc, argv, "", flags, NULL)) != -1) {
        switch (choice) {
        case 'f': opt.files = parse_number(optarg, "--files"); break;
        case 'd': opt.directories = parse_number(optarg, "--directories"); break;
        case 'w': opt.workers = parse_number(optarg, "--workers"); break;
        case 'c': opt.churn_files = parse_number(optarg, "--churn-files"); break;
        case 'b': opt.batch_directories = parse_number(optarg, "--batch-directories"); break;
        case 'r': opt.reclaim_cache = true; break;
        case 'v': opt.verify_only = true; break;
        default: return 2;
        }
    }
    if (optind + 1 != argc) {
        fprintf(stderr, "usage: %s ROOT [--files N] [--directories N] "
                "[--workers N] [--churn-files N] [--batch-directories N] "
                "[--reclaim-vfs-cache] [--verify-only]\n", argv[0]);
        return 2;
    }
    opt.root = argv[optind];
    if (!opt.files || !opt.directories || opt.directories > opt.files ||
        !opt.workers || !opt.batch_directories || opt.churn_files > opt.files) {
        fprintf(stderr, "invalid scale workload count\n");
        return 2;
    }
    if (opt.verify_only) {
        double elapsed = verify(&opt);
        printf("[SCALE-PERF] verify files=%zu directories=%zu elapsed=%.3fs "
               "rate=%.1f files/s\n", opt.files, opt.directories,
               elapsed, opt.files / elapsed);
        puts("Native million-file durable verification: PASS");
        return 0;
    }
    if (mkdir(opt.root, 0755)) die("create workload directory");
    for (size_t i = 0; i < opt.directories; i++) {
        char *path = directory_path(&opt, i);
        if (mkdir(path, 0755)) die("create scale directory");
        free(path);
    }
    size_t completed;
    size_t workers = opt.workers < opt.directories ? opt.workers : opt.directories;
    double elapsed = run_phase(&opt, CREATE, &completed);
    require(completed == opt.files, "created count");
    printf("[SCALE-PERF] create files=%zu directories=%zu workers=%zu "
           "batch_directories=%zu elapsed=%.3fs rate=%.1f files/s\n",
           completed, opt.directories, workers, opt.batch_directories,
           elapsed, completed / elapsed);
    double start = now();
    sync();
    printf("[SCALE-PERF] post-create sync elapsed=%.3fs\n", now() - start);
    elapsed = verify(&opt);
    printf("[SCALE-PERF] enumerate+verify files=%zu elapsed=%.3fs rate=%.1f files/s\n",
           opt.files, elapsed, opt.files / elapsed);
    if (opt.churn_files) {
        elapsed = run_phase(&opt, REMOVE, &completed);
        require(completed == opt.churn_files, "removed count");
        printf("[SCALE-PERF] unlink files=%zu elapsed=%.3fs rate=%.1f files/s\n",
               completed, elapsed, completed / elapsed);
        elapsed = run_phase(&opt, RECREATE, &completed);
        require(completed == opt.churn_files, "recreated count");
        printf("[SCALE-PERF] recreate files=%zu elapsed=%.3fs rate=%.1f files/s\n",
               completed, elapsed, completed / elapsed);
        start = now();
        sync();
        printf("[SCALE-PERF] post-churn sync elapsed=%.3fs\n", now() - start);
        elapsed = verify(&opt);
        printf("[SCALE-PERF] post-churn verify files=%zu elapsed=%.3fs rate=%.1f files/s\n",
               opt.files, elapsed, opt.files / elapsed);
    }
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage)) die("getrusage");
    printf("[SCALE-PERF] controller maxrss_kib=%ld\n", usage.ru_maxrss);
    printf("Native scale stress: PASS (%zu distinct files, %zu directories, "
           "%zu churned)\n", opt.files, opt.directories, opt.churn_files);
    return 0;
}
