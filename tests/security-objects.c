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

static struct infs_security_binding uid_binding(
    const uint8_t authority[16], uint32_t uid)
{
    struct infs_security_binding binding = {0};
    if (infs_security_binding_init_posix(
            &binding, INFS_BINDING_POSIX_UID, authority, uid) !=
        INFS_STATUS_OK)
        memset(&binding, 0, sizeof(binding));
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
        INFS_INCOMPAT_PORTABLE_SECURITY) != 0,
       "security feature enabled");

    static const uint8_t authority_a[16] = {
        0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
        0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11
    };
    static const uint8_t authority_b[16] = {
        0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,
        0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22
    };
    static const uint8_t user_sid[] = {
        1,2,0,0,0,0,0,5,21,0,0,0,0xe8,0x03,0,0
    };
    static const uint8_t group_sid[] = {
        1,2,0,0,0,0,0,5,32,0,0,0,0x20,0x02,0,0
    };
    static const uint8_t text_sid[] = "SID-USER-001";

    struct infs_security_binding user_bindings[2] = {0};
    user_bindings[0] = uid_binding(authority_a, 1000);
    ok(infs_security_binding_init_windows_sid(
           &user_bindings[1], user_sid, sizeof(user_sid)) == INFS_STATUS_OK,
       "canonical Windows SID binding");
    struct infs_security_binding bad_sid = {0};
    ok(infs_security_binding_init_windows_sid(
           &bad_sid, text_sid, sizeof(text_sid) - 1u) ==
           INFS_STATUS_INVALID_ARGUMENT,
       "reject textual SID bytes");

    struct infs_security_principal user = {0};
    user.kind = INFS_PRINCIPAL_USER;
    user.bindings = user_bindings;
    user.binding_count = 2;
    ok(infs_put_security_principal(&volume, &user) == INFS_STATUS_OK,
       "create multi-binding user principal");
    ok(memcmp(user.principal_id, (uint8_t[16]){0}, 16) != 0,
       "user principal ID allocated");

    struct infs_security_binding group_binding = {0};
    ok(infs_security_binding_init_windows_sid(
           &group_binding, group_sid, sizeof(group_sid)) == INFS_STATUS_OK,
       "canonical group SID binding");
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

    struct infs_security_binding scoped_uid =
        uid_binding(authority_b, 1000);
    struct infs_security_principal scoped_user = {0};
    scoped_user.kind = INFS_PRINCIPAL_USER;
    scoped_user.bindings = &scoped_uid;
    scoped_user.binding_count = 1;
    ok(infs_put_security_principal(&volume, &scoped_user) == INFS_STATUS_OK,
       "same numeric UID in another authority remains distinct");

    struct infs_security_binding opaque = {0};
    opaque.type = INFS_BINDING_OPAQUE;
    opaque.size = 3;
    memcpy(opaque.value, "abc", 3);
    struct infs_security_principal opaque_a = {0};
    opaque_a.kind = INFS_PRINCIPAL_SERVICE;
    opaque_a.bindings = &opaque;
    opaque_a.binding_count = 1;
    struct infs_security_principal opaque_b = opaque_a;
    ok(infs_put_security_principal(&volume, &opaque_a) == INFS_STATUS_OK,
       "first opaque binding");
    memset(opaque_b.principal_id, 0, sizeof(opaque_b.principal_id));
    ok(infs_put_security_principal(&volume, &opaque_b) == INFS_STATUS_OK,
       "duplicate opaque preservation binding");
    uint8_t opaque_resolved[16];
    ok(infs_find_security_principal_by_binding(
           &volume, &opaque, opaque_resolved) == INFS_STATUS_NOT_SUPPORTED,
       "opaque binding is not credential-resolvable");

    struct infs_security_principal reserved = {0};
    reserved.kind = INFS_PRINCIPAL_USER;
    memcpy(reserved.principal_id, infs_principal_owner_id, 16);
    ok(infs_put_security_principal(&volume, &reserved) ==
           INFS_STATUS_INVALID_ARGUMENT,
       "reserved well-known ID cannot become an ordinary principal");

    struct infs_security_principal got_principal = {0};
    ok(infs_get_security_principal(
           &volume, user.principal_id, &got_principal) == INFS_STATUS_OK,
       "get principal");
    ok(got_principal.binding_count == 2,
       "multi-binding principal preserved");
    ok(got_principal.bindings[1].type == INFS_BINDING_WINDOWS_SID,
       "SID binding preserved");
    uint8_t decoded_authority[16];
    uint32_t decoded_uid = 0;
    ok(infs_security_binding_get_posix(
           &got_principal.bindings[0], decoded_authority, &decoded_uid) ==
           INFS_STATUS_OK &&
       memcmp(decoded_authority, authority_a, 16) == 0 &&
       decoded_uid == 1000,
       "scoped POSIX binding round-trips");
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

    struct infs_security_ace special_aces[3] = {0};
    memcpy(special_aces[0].principal_id, infs_principal_owner_id, 16);
    special_aces[0].rights = INFS_RIGHT_READ_ATTRIBUTES;
    special_aces[0].disposition = INFS_ACE_ALLOW;
    memcpy(special_aces[1].principal_id, infs_principal_group_id, 16);
    special_aces[1].rights = INFS_RIGHT_WRITE_ATTRIBUTES;
    special_aces[1].disposition = INFS_ACE_ALLOW;
    memcpy(special_aces[2].principal_id, infs_principal_everyone_id, 16);
    special_aces[2].rights = INFS_RIGHT_READ_PERMISSIONS;
    special_aces[2].disposition = INFS_ACE_ALLOW;
    struct infs_security_descriptor special_descriptor = descriptor;
    special_descriptor.aces = special_aces;
    special_descriptor.ace_count = 3;
    ok(infs_security_access_allowed(
           &special_descriptor, subjects, 2,
           INFS_RIGHT_READ_ATTRIBUTES | INFS_RIGHT_WRITE_ATTRIBUTES) == 1,
       "OWNER and GROUP resolve through descriptor identities");
    ok(infs_security_access_allowed(
           &special_descriptor, NULL, 0,
           INFS_RIGHT_READ_PERMISSIONS) == 1,
       "EVERYONE applies without a credential binding");
    ok(infs_set_security_descriptor(
           &volume, "/two", &special_descriptor) == INFS_STATUS_OK,
       "well-known principals persist without principal objects");

    struct infs_security_ace unknown_special = {0};
    unknown_special.principal_id[15] = 6;
    unknown_special.rights = INFS_RIGHT_READ_DATA;
    unknown_special.disposition = INFS_ACE_ALLOW;
    ok(!infs_security_ace_is_valid(&unknown_special),
       "unknown reserved principal fails closed");

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

    struct infs_security_ace creator = {0};
    memcpy(creator.principal_id, infs_principal_creator_owner_id, 16);
    creator.rights = INFS_RIGHT_READ_DATA;
    creator.disposition = INFS_ACE_ALLOW;
    creator.flags = INFS_ACE_INHERIT_FILE | INFS_ACE_INHERIT_ONLY;
    struct infs_security_descriptor creator_template = descriptor;
    creator_template.aces = &creator;
    creator_template.ace_count = 1;
    ok(infs_security_inherit_descriptor(
           &creator_template, 0, scoped_user.principal_id,
           group.principal_id, &inherited) == INFS_STATUS_OK,
       "inherit CREATOR_OWNER template");
    ok(inherited.ace_count == 1 &&
       memcmp(inherited.aces[0].principal_id,
              scoped_user.principal_id, 16) == 0 &&
       (inherited.aces[0].flags & INFS_ACE_INHERITED) != 0 &&
       (inherited.aces[0].flags & INFS_ACE_INHERIT_ONLY) == 0,
       "CREATOR_OWNER becomes actual child owner");
    infs_free_security_descriptor(&inherited);

    creator.flags = INFS_ACE_INHERIT_FILE;
    ok(!infs_security_ace_is_valid(&creator),
       "creator principal requires inherit-only template semantics");

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
