// SPDX-License-Identifier: GPL-3.0-or-later
/* Minimal freestanding C runtime required by the portable InfiltratorFS core. */

#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <stdint.h>

#include "uefi_libc.h"

struct infs_uefi_alloc_header {
    UINTN size;
    UINTN reserved;
} __attribute__((aligned(16)));

void *malloc(size_t size)
{
    struct infs_uefi_alloc_header *header;
    UINTN total;

    if (size == 0)
        size = 1;
    if (size > (size_t)((~(UINTN)0) - sizeof(*header)))
        return NULL;
    total = (UINTN)size + sizeof(*header);
    header = AllocatePool(total);
    if (!header)
        return NULL;
    header->size = (UINTN)size;
    header->reserved = 0;
    return (void *)(header + 1);
}

void free(void *pointer)
{
    struct infs_uefi_alloc_header *header;
    if (!pointer)
        return;
    header = ((struct infs_uefi_alloc_header *)pointer) - 1;
    FreePool(header);
}

void *calloc(size_t count, size_t size)
{
    size_t bytes;
    void *pointer;

    if (count && size > (size_t)-1 / count)
        return NULL;
    bytes = count * size;
    pointer = malloc(bytes);
    if (pointer)
        memset(pointer, 0, bytes);
    return pointer;
}

void *realloc(void *pointer, size_t size)
{
    struct infs_uefi_alloc_header *header;
    size_t old_size;
    size_t copy_size;
    void *replacement;

    if (!pointer)
        return malloc(size);
    if (size == 0) {
        free(pointer);
        return NULL;
    }

    header = ((struct infs_uefi_alloc_header *)pointer) - 1;
    old_size = (size_t)header->size;
    replacement = malloc(size);
    if (!replacement)
        return NULL;
    copy_size = old_size < size ? old_size : size;
    memcpy(replacement, pointer, copy_size);
    free(pointer);
    return replacement;
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
