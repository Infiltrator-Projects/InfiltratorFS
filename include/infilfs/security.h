// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_SECURITY_H
#define INFILFS_SECURITY_H

#include <stddef.h>
#include <stdint.h>

#include "infilfs/status.h"

typedef uint64_t infs_rights_mask;

#define INFS_RIGHT_READ_DATA            (UINT64_C(1) << 0)
#define INFS_RIGHT_WRITE_DATA           (UINT64_C(1) << 1)
#define INFS_RIGHT_APPEND_DATA          (UINT64_C(1) << 2)
#define INFS_RIGHT_EXECUTE              (UINT64_C(1) << 3)
#define INFS_RIGHT_LIST_DIRECTORY       (UINT64_C(1) << 4)
#define INFS_RIGHT_TRAVERSE_DIRECTORY   (UINT64_C(1) << 5)
#define INFS_RIGHT_CREATE_FILE          (UINT64_C(1) << 6)
#define INFS_RIGHT_CREATE_DIRECTORY     (UINT64_C(1) << 7)
#define INFS_RIGHT_DELETE               (UINT64_C(1) << 8)
#define INFS_RIGHT_DELETE_CHILD         (UINT64_C(1) << 9)
#define INFS_RIGHT_READ_ATTRIBUTES      (UINT64_C(1) << 10)
#define INFS_RIGHT_WRITE_ATTRIBUTES     (UINT64_C(1) << 11)
#define INFS_RIGHT_READ_NAMED_METADATA  (UINT64_C(1) << 12)
#define INFS_RIGHT_WRITE_NAMED_METADATA (UINT64_C(1) << 13)
#define INFS_RIGHT_READ_PERMISSIONS     (UINT64_C(1) << 14)
#define INFS_RIGHT_CHANGE_PERMISSIONS   (UINT64_C(1) << 15)
#define INFS_RIGHT_TAKE_OWNERSHIP       (UINT64_C(1) << 16)

#define INFS_RIGHT_ALL ( \
    INFS_RIGHT_READ_DATA | INFS_RIGHT_WRITE_DATA | INFS_RIGHT_APPEND_DATA | \
    INFS_RIGHT_EXECUTE | INFS_RIGHT_LIST_DIRECTORY | \
    INFS_RIGHT_TRAVERSE_DIRECTORY | INFS_RIGHT_CREATE_FILE | \
    INFS_RIGHT_CREATE_DIRECTORY | INFS_RIGHT_DELETE | \
    INFS_RIGHT_DELETE_CHILD | INFS_RIGHT_READ_ATTRIBUTES | \
    INFS_RIGHT_WRITE_ATTRIBUTES | INFS_RIGHT_READ_NAMED_METADATA | \
    INFS_RIGHT_WRITE_NAMED_METADATA | INFS_RIGHT_READ_PERMISSIONS | \
    INFS_RIGHT_CHANGE_PERMISSIONS | INFS_RIGHT_TAKE_OWNERSHIP)

#define INFS_PRINCIPAL_USER    UINT16_C(1)
#define INFS_PRINCIPAL_GROUP   UINT16_C(2)
#define INFS_PRINCIPAL_SERVICE UINT16_C(3)

#define INFS_BINDING_POSIX_UID   UINT16_C(1)
#define INFS_BINDING_POSIX_GID   UINT16_C(2)
#define INFS_BINDING_WINDOWS_SID UINT16_C(3)
#define INFS_BINDING_OPAQUE      UINT16_C(0xffff)

#ifndef INFS_SECURITY_BINDING_MAX
#define INFS_SECURITY_BINDING_MAX 68u
#endif

#ifndef INFS_SECURITY_POSIX_AUTHORITY_SIZE
#define INFS_SECURITY_POSIX_AUTHORITY_SIZE 16u
#endif
#ifndef INFS_SECURITY_POSIX_BINDING_SIZE
#define INFS_SECURITY_POSIX_BINDING_SIZE 20u
#endif
#ifndef INFS_SECURITY_WINDOWS_SID_MIN
#define INFS_SECURITY_WINDOWS_SID_MIN 8u
#endif
#ifndef INFS_SECURITY_WINDOWS_SID_MAX
#define INFS_SECURITY_WINDOWS_SID_MAX 68u
#endif
#ifndef INFS_SECURITY_WINDOWS_SID_REVISION
#define INFS_SECURITY_WINDOWS_SID_REVISION 1u
#endif
#ifndef INFS_SECURITY_WINDOWS_SID_MAX_SUB_AUTHORITIES
#define INFS_SECURITY_WINDOWS_SID_MAX_SUB_AUTHORITIES 15u
#endif

/* The all-zero ID remains invalid. IDs 00..00:01 through 00..00:1f are
 * permanently reserved for implicit portable security semantics. */
#define INFS_PRINCIPAL_RESERVED_MAX_CODE UINT8_C(31)

extern const uint8_t infs_principal_owner_id[16];
extern const uint8_t infs_principal_group_id[16];
extern const uint8_t infs_principal_everyone_id[16];
extern const uint8_t infs_principal_creator_owner_id[16];
extern const uint8_t infs_principal_creator_group_id[16];

#define INFS_ACE_ALLOW UINT16_C(1)
#define INFS_ACE_DENY  UINT16_C(2)

#define INFS_ACE_INHERIT_FILE      UINT16_C(0x0001)
#define INFS_ACE_INHERIT_DIRECTORY UINT16_C(0x0002)
#define INFS_ACE_INHERIT_ONLY      UINT16_C(0x0004)
#define INFS_ACE_NO_PROPAGATE      UINT16_C(0x0008)
#define INFS_ACE_INHERITED         UINT16_C(0x0010)
#define INFS_ACE_KNOWN_FLAGS       UINT16_C(0x001f)

#define INFS_SECURITY_DACL_PRESENT UINT16_C(0x0001)
#define INFS_SECURITY_PROTECTED    UINT16_C(0x0002)
#define INFS_SECURITY_AUTO_INHERIT UINT16_C(0x0004)
#define INFS_SECURITY_KNOWN_FLAGS  UINT16_C(0x0007)

struct infs_security_binding {
    uint16_t type;
    uint16_t flags;
    uint16_t size;
    uint8_t value[INFS_SECURITY_BINDING_MAX];
};

struct infs_security_principal {
    uint8_t principal_id[16];
    uint16_t kind;
    uint16_t flags;
    struct infs_security_binding *bindings;
    size_t binding_count;
};

struct infs_security_ace {
    uint8_t principal_id[16];
    infs_rights_mask rights;
    uint16_t disposition;
    uint16_t flags;
};

struct infs_security_descriptor {
    uint8_t owner_principal_id[16];
    uint8_t primary_group_principal_id[16];
    uint16_t flags;
    struct infs_security_ace *aces;
    size_t ace_count;
};

int infs_security_principal_id_is_reserved(const uint8_t principal_id[16]);
int infs_security_principal_id_is_well_known(const uint8_t principal_id[16]);
int infs_security_principal_id_is_creator(const uint8_t principal_id[16]);

int infs_security_windows_sid_valid(const uint8_t *sid, size_t size);
int infs_security_binding_is_valid(const struct infs_security_binding *binding);
infs_status infs_security_binding_init_posix(
    struct infs_security_binding *binding, uint16_t type,
    const uint8_t authority_id[16], uint32_t numeric_id);
infs_status infs_security_binding_get_posix(
    const struct infs_security_binding *binding, uint8_t authority_id[16],
    uint32_t *numeric_id);
infs_status infs_security_binding_init_windows_sid(
    struct infs_security_binding *binding, const uint8_t *sid, size_t size);
infs_status infs_security_binding_digest(
    const struct infs_security_binding *binding, uint8_t digest[32]);
void infs_security_binding_index_object_id(
    const uint8_t digest[32], uint16_t slot, uint8_t object_id[16]);
void infs_security_descriptor_object_id(
    const uint8_t digest[32], uint16_t slot, uint8_t object_id[16]);

int infs_security_ace_is_valid(const struct infs_security_ace *ace);
int infs_security_descriptor_is_valid(
    const struct infs_security_descriptor *descriptor);

int infs_security_access_allowed(
    const struct infs_security_descriptor *descriptor,
    const uint8_t *principal_ids, size_t principal_count,
    infs_rights_mask requested);

infs_status infs_security_inherit_descriptor(
    const struct infs_security_descriptor *parent, int child_is_directory,
    const uint8_t owner_principal_id[16],
    const uint8_t primary_group_principal_id[16],
    struct infs_security_descriptor *child);

void infs_free_security_descriptor(struct infs_security_descriptor *descriptor);
void infs_free_security_principal(struct infs_security_principal *principal);

#endif
