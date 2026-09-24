// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_STORAGE_LOCK_H
#define INFILFS_STORAGE_LOCK_H

/*
 * Small private blocking lock used by portable storage wrappers.
 *
 * Storage callbacks may block on real devices, so do not use a spin lock here.
 * SRWLOCK is available on every supported Windows target; POSIX builds use a
 * pthread mutex. The CMake Threads target carries the required Unix linkage.
 */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct infs_storage_lock {
    SRWLOCK native;
};

static inline int infs_storage_lock_init(struct infs_storage_lock *lock)
{
    InitializeSRWLock(&lock->native);
    return 1;
}

static inline void infs_storage_lock_destroy(struct infs_storage_lock *lock)
{
    (void)lock;
}

static inline void infs_storage_lock_acquire(struct infs_storage_lock *lock)
{
    AcquireSRWLockExclusive(&lock->native);
}

static inline void infs_storage_lock_release(struct infs_storage_lock *lock)
{
    ReleaseSRWLockExclusive(&lock->native);
}
#else
#include <pthread.h>

struct infs_storage_lock {
    pthread_mutex_t native;
};

static inline int infs_storage_lock_init(struct infs_storage_lock *lock)
{
    return pthread_mutex_init(&lock->native, NULL) == 0;
}

static inline void infs_storage_lock_destroy(struct infs_storage_lock *lock)
{
    (void)pthread_mutex_destroy(&lock->native);
}

static inline void infs_storage_lock_acquire(struct infs_storage_lock *lock)
{
    (void)pthread_mutex_lock(&lock->native);
}

static inline void infs_storage_lock_release(struct infs_storage_lock *lock)
{
    (void)pthread_mutex_unlock(&lock->native);
}
#endif

#endif
