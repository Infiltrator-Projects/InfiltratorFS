// SPDX-License-Identifier: GPL-3.0-or-later
/* Single-image, single-threaded firmware runtime. Each allocation remembers
 * its originating host so it is always released through that same host. */
#include <infiltratr/filesystem_module_v2.h>
#include <stddef.h>
#include <stdint.h>
#include "../uefi/filesystems/platform/uefi_libc.h"
static const IfsFsHostRuntimeV2 *current_host;
struct allocation { const IfsFsHostRuntimeV2 *host; size_t size; void *base; };
void infs_v2_set_host(const IfsFsHostRuntimeV2 *host) { current_host = host; }
void *malloc(size_t size)
{
    if (!current_host || size > SIZE_MAX - sizeof(struct allocation) - 15u) return NULL;
    if (!size) size = 1;
    void *base = current_host->allocate(current_host->context, size + sizeof(struct allocation) + 15u);
    if (!base) return NULL;
    uintptr_t address = ((uintptr_t)base + sizeof(struct allocation) + 15u) & ~(uintptr_t)15u;
    struct allocation *a = (struct allocation *)address - 1;
    a->host = current_host; a->size = size; a->base = base;
    return a + 1;
}
void free(void *p)
{
    if (!p) return;
    struct allocation *a = (struct allocation *)p - 1;
    a->host->release(a->host->context, a->base);
}
void *calloc(size_t count, size_t size)
{
    if (count && size > SIZE_MAX / count) return NULL;
    size *= count; void *p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}
void *realloc(void *p, size_t size)
{
    if (!p) return malloc(size);
    if (!size) { free(p); return NULL; }
    struct allocation *a = (struct allocation *)p - 1;
    void *replacement = malloc(size);
    if (!replacement) return NULL;
    memcpy(replacement, p, size < a->size ? size : a->size);
    free(p); return replacement;
}
void *memset(void *dst, int c, size_t size)
{
    unsigned char *p = dst;
    while (size--) *p++ = (unsigned char)c;
    return dst;
}
void *memcpy(void *dst, const void *src, size_t size)
{
    unsigned char *d = dst; const unsigned char *s = src;
    while (size--) *d++ = *s++;
    return dst;
}
void *memmove(void *destination, const void *source, size_t size)
{
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;

    if (out == in || size == 0)
        return destination;
    if (out < in) {
        while (size--)
            *out++ = *in++;
    } else {
        out += size;
        in += size;
        while (size--)
            *--out = *--in;
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t size)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    while (size--) {
        if (*a != *b)
            return *a < *b ? -1 : 1;
        ++a;
        ++b;
    }
    return 0;
}

void *memchr(const void *memory, int value, size_t size)
{
    const unsigned char *p = (const unsigned char *)memory;
    unsigned char needle = (unsigned char)value;
    while (size--) {
        if (*p == needle)
            return (void *)p;
        ++p;
    }
    return NULL;
}

size_t strlen(const char *text)
{
    const char *p = text;
    while (*p)
        ++p;
    return (size_t)(p - text);
}

int strcmp(const char *left, const char *right)
{
    while (*left && (unsigned char)*left == (unsigned char)*right) {
        ++left;
        ++right;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t size)
{
    while (size && *left && (unsigned char)*left == (unsigned char)*right) {
        ++left;
        ++right;
        --size;
    }
    if (!size)
        return 0;
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

char *strchr(const char *text, int value)
{
    char needle = (char)value;
    for (;;) {
        if (*text == needle)
            return (char *)text;
        if (*text == 0)
            return NULL;
        ++text;
    }
}

char *strrchr(const char *text, int value)
{
    const char *last = NULL;
    char needle = (char)value;
    do {
        if (*text == needle)
            last = text;
    } while (*text++);
    return (char *)last;
}

char *strcpy(char *destination, const char *source)
{
    char *out = destination;
    while ((*out++ = *source++) != 0)
        ;
    return destination;
}

char *strncpy(char *destination, const char *source, size_t size)
{
    char *out = destination;
    while (size && *source) {
        *out++ = *source++;
        --size;
    }
    while (size--)
        *out++ = 0;
    return destination;
}

static void infs_qsort_swap(unsigned char *a, unsigned char *b, size_t size)
{
    while (size--) {
        unsigned char tmp = *a;
        *a++ = *b;
        *b++ = tmp;
    }
}

static void infs_qsort_sift(unsigned char *base, size_t root, size_t count,
                            size_t width, int (*compare)(const void *, const void *))
{
    for (;;) {
        size_t child = root * 2u + 1u;
        size_t selected = root;
        if (child >= count)
            return;
        if (compare(base + selected * width, base + child * width) < 0)
            selected = child;
        if (child + 1u < count &&
            compare(base + selected * width, base + (child + 1u) * width) < 0)
            selected = child + 1u;
        if (selected == root)
            return;
        infs_qsort_swap(base + root * width, base + selected * width, width);
        root = selected;
    }
}

void qsort(void *base_pointer, size_t count, size_t width,
           int (*compare)(const void *, const void *))
{
    unsigned char *base = (unsigned char *)base_pointer;
    size_t start;
    size_t end;

    if (!base || !compare || width == 0 || count < 2)
        return;

    start = count / 2u;
    while (start > 0) {
        --start;
        infs_qsort_sift(base, start, count, width, compare);
    }

    end = count;
    while (end > 1u) {
        --end;
        infs_qsort_swap(base, base + end * width, width);
        infs_qsort_sift(base, 0, end, width, compare);
    }
}
