/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFS_CHECKPOINT_PROTOCOL_H
#define INFS_CHECKPOINT_PROTOCOL_H

/* Kernel-safe ordering contract shared by portable and native publication.
 * The caller has already made every CoW dependency durable. A locator is a
 * hint, never a commit record. No subsequent stage runs after an error.
 * Once publication starts, any error requires recovery before more writes.
 * Ordinary commits leave fixed locators untouched. */
struct infs_checkpoint_io {
    int (*locator)(void *context, unsigned copy);
    int (*checkpoint)(void *context, unsigned copy);
    int (*flush)(void *context);
    void (*committed)(void *context);
};

static inline int infs_checkpoint_publish(
    const struct infs_checkpoint_io *io, void *context,
    unsigned first, int relocate, int *committed)
{
    unsigned n;
    int status;
    *committed = 0;
    if (relocate) {
        status = io->locator(context, 0u);
        if (status)
            return status;
        status = io->flush(context);
        if (status)
            return status;
    }
    for (n = 0; n < 3u; ++n) {
        status = io->checkpoint(context, (first + n) % 3u);
        if (status)
            return status;
        status = io->flush(context);
        if (status)
            return status;
        if (!n) {
            *committed = 1;
            if (io->committed)
                io->committed(context);
        }
    }
    if (relocate) {
        status = io->locator(context, 1u);
        if (status)
            return status;
        return io->flush(context);
    }
    return 0;
}
#endif
