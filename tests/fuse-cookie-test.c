// SPDX-License-Identifier: GPL-3.0-or-later
#define main infilfs_fuse_program_main
#include "../fuse/infilfs_fuse.c"
#undef main

#include <stdio.h>
#include <stdlib.h>

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "fuse-cookie-test: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    size_t start = SIZE_MAX;
    expect(readdir_cookie_start(0, 3, &start) == 0 && start == 0,
           "zero cookie starts at dot");
    expect(readdir_cookie_start(1, 3, &start) == 0 && start == 1,
           "cookie one resumes at dot-dot");
    expect(readdir_cookie_start(2, 3, &start) == 0 && start == 2,
           "cookie two resumes at first real entry");
    expect(readdir_cookie_start(3, 3, &start) == 0 && start == 3,
           "cookie three resumes at second real entry");
    expect(readdir_cookie_start(5, 3, &start) == 0 && start == 5,
           "end cookie produces no more entries");
    expect(readdir_cookie_start(99, 3, &start) == 0 && start == 5,
           "stale oversized cookie clamps to end");
    expect(readdir_cookie_start((off_t)-1, 3, &start) == -EINVAL,
           "negative cookie is rejected");
    expect(readdir_cookie_start(0, 3, NULL) == -EINVAL,
           "null cookie output is rejected");

    puts("fuse-cookie-test: PASS");
    return 0;
}
