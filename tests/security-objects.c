// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format_volume.h"
#include "infilfs/security.h"
#include "infilfs/volume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCKS UINT64_C(8192)
#define BYTES ((size_t)(BLOCKS * INFS_BLOCK_SIZE))

struct image {
    uint8_t *p;
    size_t n;
    uint64_t rnd;
};

static void ok(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "security-objects: %s\n", message);
        exit(1);
    }
}

static infs_status rd(void *context, uint64_t offset, void *buffer, size_t size)
{
    struct image *image = context;
    if (offset > image->n || size > image->n - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(buffer, image->p + (size_t)offset, size);
    return INFS_STATUS_OK;
}

static infs_status wr(void *context, uint64_t offset,
                      const void *buffer, size_t size)
{
    struct image *image = context;
    if (offset > image->n || size > image->n - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(image->p + (size_t)offset, buffer, size);
    return INFS_STATUS_OK;
}

static infs_status fl(void *context)
{
    (void)context;
    return INFS_STATUS_OK;
}

static infs_status sz(void *context, uint64_t *size, int *is_device)
{
    struct image *image = context;
    *size = image->n;
    *is_device = 0;
    return INFS_STATUS_OK;
}

static infs_status rn(void *context, void *buffer, size_t size)
{
    struct image *image = context;
    uint8_t *out = buffer;
    for (size_t i = 0; i < size; ++i) {
        image->rnd ^= image->rnd << 13;
        image->rnd ^= image->rnd >> 7;
        image->rnd ^= image->rnd << 17;
        out[i] = (uint8_t)image->rnd;
    }
    return INFS_STATUS_OK;
}

static infs_status tm(void *context, struct infs_timestamp *time)
{
    (void)context;
    time->seconds = 1787288400;
    time->nanoseconds = 0;
    return INFS_STATUS_OK;
}

static void cl(void *context)
{
    (void)context;
}

static const struct infs_storage_ops ops = {
    .read_at = rd,
    .write_at = wr,
    .flush = fl,
    .get_size = sz,
    .random_bytes = rn,
    .current_time = tm,
    .close = cl
};

static struct infs_storage storage_for(struct image *image)
{
    struct infs_storage storage = {&ops, image};
    return storage;
}

static struct infs_security_binding uid_binding(uint32_t uid)
{
    struct infs_security_binding binding = {0};
    binding.type = INFS_BINDING_POSIX_UID;
    binding.size = 4;
    binding.value[0] = (uint8_t)uid;
    binding.value[1] = (uint8_t)(uid >> 8);
    binding.value[2] = (uint8_t)(uid >> 16);
    binding.value[3] = (uint8_t)(uid >> 24);
    return binding;
}

int main(void)
{
    struct image image = {
        calloc(1, BYTES), BYTES, UINT64_C(0x123456789abcdef)
    };
    ok(image.p != NULL, "allocate image");

    struct infs_storage storage = storage_for(&image);
    ok(infs_format_storage(&storage, "security-object-test") == INFS_STATUS_OK,
       "format");

    struct infs_volume volume;
    storage = storage_for(&image);
    ok(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
       "open");
    ok((infs_le64_to_cpu(volume.sb.incompat_flags) &
        INFS_INCOMPAT_SECURITY_OBJECTS_V1) != 0,
       "security feature enabled");

    struct infs_security_binding user_bindings[2] = {0};
    user_bindings[0] = uid_binding(1000);
    user_bindings[1].type = INFS_BINDING_WINDOWS_SID;
    user_bindings[1].size = 12;
    memcpy(user_bindings[1].value, "SID-USER-001", 12);

    struct infs_security_principal user = {0};
    user.kind = INFS_PRINCIPAL_USER;
    user.bindings = user_bindings;
    user.binding_count = 2;
    ok(infs_put_security_principal(&volume, &user) == INFS_STATUS_OK,
       "create multi-binding user principal");
    ok(memcmp(user.principal_id, (uint8_t[16]){0}, 16) != 0,
       "user principal ID allocated");

    struct infs_security_binding group_binding = {0};
    group_binding.type = INFS_BINDING_WINDOWS_SID;
    group_binding.size = 12;
    memcpy(group_binding.value, "SID-GROUP001", 12);
    struct infs_security_principal group = {0};
    group.kind = INFS_PRINCIPAL_GROUP;
    group.bindings = &group_binding;
    group.binding_count = 1;
    ok(infs_put_security_principal(&volume, &group) == INFS_STATUS_OK,
       "create group principal");

    struct infs_security_principal duplicate = {0};
    duplicate.kind = INFS_PRINCIPAL_USER;
    duplicate.bindings = &user_bindings[0];
    duplicate.binding_count = 1;
    ok(infs_put_security_principal(&volume, &duplicate) ==
           INFS_STATUS_ALREADY_EXISTS,
       "reject duplicate unique platform binding");

    struct infs_security_principal got_principal = {0};
    ok(infs_get_security_principal(
           &volume, user.principal_id, &got_principal) == INFS_STATUS_OK,
       "get principal");
    ok(got_principal.binding_count == 2,
       "multi-binding principal preserved");
    ok(got_principal.bindings[1].type == INFS_BINDING_WINDOWS_SID,
       "SID binding preserved");
    infs_free_security_principal(&got_principal);

    uint8_t resolved[16];
    ok(infs_find_security_principal_by_binding(
           &volume, &user_bindings[0], resolved) == INFS_STATUS_OK &&
       memcmp(resolved, user.principal_id, 16) == 0,
       "reverse binding lookup");

    ok(infs_create_file(&volume, "/one", NULL) == INFS_STATUS_OK,
       "create first file");
    ok(infs_create_file(&volume, "/two", NULL) == INFS_STATUS_OK,
       "create second file");

    struct infs_security_ace aces[3] = {0};
    memcpy(aces[0].principal_id, user.principal_id, 16);
    aces[0].rights = INFS_RIGHT_READ_DATA | INFS_RIGHT_WRITE_DATA;
    aces[0].disposition = INFS_ACE_ALLOW;

    memcpy(aces[1].principal_id, group.principal_id, 16);
    aces[1].rights = INFS_RIGHT_DELETE;
    aces[1].disposition = INFS_ACE_DENY;

    memcpy(aces[2].principal_id, group.principal_id, 16);
    aces[2].rights = INFS_RIGHT_READ_ATTRIBUTES;
    aces[2].disposition = INFS_ACE_ALLOW;
    aces[2].flags = INFS_ACE_INHERIT_FILE | INFS_ACE_INHERIT_DIRECTORY;

    struct infs_security_descriptor descriptor = {0};
    memcpy(descriptor.owner_principal_id, user.principal_id, 16);
    memcpy(descriptor.primary_group_principal_id, group.principal_id, 16);
    descriptor.flags =
        INFS_SECURITY_DACL_PRESENT | INFS_SECURITY_AUTO_INHERIT;
    descriptor.aces = aces;
    descriptor.ace_count = 3;

    ok(infs_set_security_descriptor(
           &volume, "/one", &descriptor) == INFS_STATUS_OK,
       "set first descriptor");
    ok(infs_set_security_descriptor(
           &volume, "/two", &descriptor) == INFS_STATUS_OK,
       "reuse descriptor");

    struct infs_attributes one = {0}, two = {0};
    ok(infs_get_attributes(&volume, "/one", &one) == INFS_STATUS_OK &&
       infs_get_attributes(&volume, "/two", &two) == INFS_STATUS_OK,
       "get descriptor attributes");
    ok(memcmp(one.security_object_id, two.security_object_id, 16) == 0,
       "identical descriptors are single-instanced");

    uint8_t subjects[32];
    memcpy(subjects, user.principal_id, 16);
    memcpy(subjects + 16, group.principal_id, 16);
    ok(infs_security_access_allowed(
           &descriptor, subjects, 2, INFS_RIGHT_READ_DATA) == 1,
       "ordered allow evaluation");
    ok(infs_security_access_allowed(
           &descriptor, subjects, 2, INFS_RIGHT_DELETE) == 0,
       "ordered deny evaluation");

    struct infs_security_ace ordered[2] = {0};
    memcpy(ordered[0].principal_id, user.principal_id, 16);
    ordered[0].rights = INFS_RIGHT_READ_DATA;
    ordered[0].disposition = INFS_ACE_ALLOW;
    memcpy(ordered[1].principal_id, user.principal_id, 16);
    ordered[1].rights = INFS_RIGHT_READ_DATA;
    ordered[1].disposition = INFS_ACE_DENY;
    struct infs_security_descriptor ordered_descriptor = descriptor;
    ordered_descriptor.aces = ordered;
    ordered_descriptor.ace_count = 2;
    ok(infs_security_access_allowed(
           &ordered_descriptor, subjects, 2, INFS_RIGHT_READ_DATA) == 1,
       "later deny cannot revoke an already decided right");

    ordered[0].disposition = UINT16_C(99);
    ok(infs_security_access_allowed(
           &ordered_descriptor, subjects, 2, INFS_RIGHT_READ_DATA) == 0,
       "malformed in-memory ACE fails closed");
    ordered[0].disposition = INFS_ACE_ALLOW;

    struct infs_security_descriptor missing = descriptor;
    uint8_t missing_id[16];
    memset(missing_id, 0xa5, sizeof(missing_id));
    struct infs_security_ace missing_ace = aces[0];
    memcpy(missing_ace.principal_id, missing_id, 16);
    missing.aces = &missing_ace;
    missing.ace_count = 1;
    ok(infs_set_security_descriptor(&volume, "/one", &missing) ==
           INFS_STATUS_INVALID_ARGUMENT,
       "reject descriptor with missing principal");

    struct infs_security_descriptor inherited = {0};
    ok(infs_security_inherit_descriptor(
           &descriptor, 0, user.principal_id, group.principal_id,
           &inherited) == INFS_STATUS_OK,
       "inherit descriptor");
    ok(inherited.ace_count == 1 &&
       (inherited.aces[0].flags & INFS_ACE_INHERITED) != 0,
       "file inheritance filters and marks ACE");
    infs_free_security_descriptor(&inherited);

    struct infs_security_ace no_propagate_ace = aces[2];
    no_propagate_ace.flags =
        INFS_ACE_INHERIT_FILE | INFS_ACE_NO_PROPAGATE;
    struct infs_security_descriptor no_propagate = descriptor;
    no_propagate.aces = &no_propagate_ace;
    no_propagate.ace_count = 1;
    ok(infs_security_inherit_descriptor(
           &no_propagate, 1, user.principal_id, group.principal_id,
           &inherited) == INFS_STATUS_OK,
       "inherit no-propagate descriptor");
    ok(inherited.ace_count == 0,
       "file-only no-propagate ACE does not create useless directory ACE");
    infs_free_security_descriptor(&inherited);

    const size_t large_count = (size_t)INFS_SECURITY_INLINE_ACES + 5u;
    struct infs_security_ace *many = calloc(large_count, sizeof(*many));
    ok(many != NULL, "allocate large ACL");
    for (size_t i = 0; i < large_count; ++i) {
        memcpy(many[i].principal_id, user.principal_id, 16);
        many[i].rights = (i & 1u) ?
            INFS_RIGHT_READ_DATA : INFS_RIGHT_READ_ATTRIBUTES;
        many[i].disposition = INFS_ACE_ALLOW;
    }

    struct infs_security_descriptor large = {0};
    memcpy(large.owner_principal_id, user.principal_id, 16);
    memcpy(large.primary_group_principal_id, group.principal_id, 16);
    large.flags = INFS_SECURITY_DACL_PRESENT;
    large.aces = many;
    large.ace_count = large_count;
    ok(infs_set_security_descriptor(&volume, "/one", &large) == INFS_STATUS_OK,
       "paged descriptor");
    free(many);

    struct infs_security_descriptor got = {0};
    ok(infs_get_security_descriptor(&volume, "/one", &got) == INFS_STATUS_OK,
       "get paged descriptor");
    ok(got.ace_count == large_count, "paged ACE count");
    infs_free_security_descriptor(&got);

    struct infs_scrub_report report;
    ok(infs_scrub(&volume, &report) == INFS_STATUS_OK &&
       report.metadata_errors == 0,
       "scrub security graph");

    infs_volume_close(&volume);
    storage = storage_for(&image);
    ok(infs_volume_open_storage(&volume, &storage, 1) == INFS_STATUS_OK,
       "remount");
    ok(infs_get_security_descriptor(&volume, "/one", &got) == INFS_STATUS_OK,
       "descriptor survives remount");
    infs_free_security_descriptor(&got);

    ok(infs_set_security_descriptor(&volume, "/one", NULL) == INFS_STATUS_OK,
       "detach first descriptor");
    ok(infs_get_security_descriptor(&volume, "/two", &got) == INFS_STATUS_OK,
       "shared descriptor survives other detach");
    infs_free_security_descriptor(&got);
    ok(infs_set_security_descriptor(&volume, "/two", NULL) == INFS_STATUS_OK,
       "detach final shared descriptor");
    ok(infs_scrub(&volume, &report) == INFS_STATUS_OK &&
       report.metadata_errors == 0,
       "scrub after descriptor reclamation");

    infs_volume_close(&volume);
    free(image.p);
    puts("portable security objects: PASS");
    return 0;
}
