// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATORFS_UEFI_LIBC_H
#define INFILTRATORFS_UEFI_LIBC_H

#include <stddef.h>

/* GNU-EFI supplies memcpy() and memset(). The remaining small libc surface is
 * provided by uefi_compat.c. This header is force-included for the vendored
 * freestanding sources so LZ4 and the InfiltratorFS core have declarations
 * without depending on a host C runtime at link time. */
void *malloc(size_t size);
void free(void *pointer);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void *memset(void *destination, int value, size_t size);
void *memcpy(void *destination, const void *source, size_t size);
void *memmove(void *destination, const void *source, size_t size);
int memcmp(const void *left, const void *right, size_t size);
void *memchr(const void *memory, int value, size_t size);
size_t strlen(const char *text);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t size);
char *strchr(const char *text, int value);
char *strrchr(const char *text, int value);
char *strcpy(char *destination, const char *source);
char *strncpy(char *destination, const char *source, size_t size);
void qsort(void *base_pointer, size_t count, size_t width,
           int (*compare)(const void *, const void *));

#endif
